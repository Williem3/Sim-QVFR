// Part of mvr_pass.cpp's translation unit (included once, in order): post-role learning by bindings (scene copies,
// stages, the final blit, the frame's shape). Included only by mvr_pass.cpp, inside its anonymous namespace
// (lmu::<anonymous>), after mvr_pass_scene_learn.h; never include it anywhere else: the per-draw path stays in one
// translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// The texture pixel shader slot 0 reads, and render target 0's (null: none).
ComPtr<ID3D11Resource> ps_input0(ID3D11DeviceContext* context) {
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> r;
    if (srv)
        srv->GetResource(&r);
    return r;
}
ComPtr<ID3D11Resource> target0(ID3D11DeviceContext* context) {
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> r;
    if (rtv)
        rtv->GetResource(&r);
    return r;
}

// Is `target` a single-sample texture of the resolve's size, other than the resolve's target (what the scene copy, or
// a stage's image of the scene, is drawn into)?
bool scene_sized_copy(ID3D11Resource* target) {
    ComPtr<ID3D11Texture2D> t, r;
    if (!target || !g.resolved[g.pass] || target == g.resolved[g.pass].Get() || FAILED(target->QueryInterface(IID_PPV_ARGS(&t))) ||
        FAILED(g.resolved[g.pass].As(&r)))
        return false;
    D3D11_TEXTURE2D_DESC td, rd;
    t->GetDesc(&td);
    r->GetDesc(&rd);
    return td.SampleDesc.Count == 1 && td.Width == rd.Width && td.Height == rd.Height;
}

// The pass's final blit by what it binds, where the probe knows no blit shader by its checksum (a game update compiled
// it anew): a Draw reading the resolve's target into another texture, the eye's image where that is known.
bool blit_by_bindings(ID3D11DeviceContext* context) {
    if (!g.blitUnknown || g.pass < 0 || g.pass > 1 || !g.resolved[g.pass])
        return false;
    const ComPtr<ID3D11Resource> source = ps_input0(context), target = target0(context);
    if (!source || source.Get() != g.resolved[g.pass].Get() || !target || target.Get() == source.Get())
        return false;
    ID3D11Resource* const eye = g.fsrEye[g.pass & 1];
    return !eye || target.Get() == eye;
}

// The frame's shape (mvr_shape_ready): the steps the focus views are made at, seen on the eye's own post-processing
// whether or not a focus view is drawn this pass, by their bindings alone. Before the composite a draw reading the
// resolve's target into a texture of its size is the scene copy (or a stage's image of the scene, which YEBIS then
// starts from): the composite reads the latest. After the composite a Draw reading the resolve's target into another
// texture is the final blit (FSR on: the eye's image bound, fsr_final's rule).
void observe_post_draw(ID3D11DeviceContext* context, bool plainDraw) {
    if (!g.observeShape || g.obsFinal || !g.resolved[g.pass])
        return;
    const ComPtr<ID3D11Resource> source = ps_input0(context), target = target0(context);
    if (!target)
        return;
    ID3D11Resource* const resolved = g.resolved[g.pass].Get();
    if (!g.obsComposited) {
        // A stage needn't read the scene from slot 0 (its depth may be there): any of the first eight.
        bool readsResolve = source.Get() == resolved;
        if (!readsResolve && target.Get() != resolved) {
            ID3D11ShaderResourceView* srvs[8] = {};
            context->PSGetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
            for (ID3D11ShaderResourceView* srv : srvs) {
                if (!srv)
                    continue;
                ComPtr<ID3D11Resource> r;
                srv->GetResource(&r);
                readsResolve = readsResolve || r.Get() == resolved;
                srv->Release();
            }
        }
        if (readsResolve) {
            if (scene_sized_copy(target.Get())) {
                g.obsCopied = true;
                bool have = false;
                for (unsigned i = 0; i < g.obsCopyCount && !have; ++i)
                    have = g.obsCopyTargets[i] == target.Get();
                if (!have)
                    g.obsCopyTargets[g.obsCopyCount++ % std::size(g.obsCopyTargets)] = target.Get();
            }
        } else if (g.obsCopied && target.Get() == resolved &&
                   std::find(std::begin(g.obsCopyTargets), std::end(g.obsCopyTargets), source.Get()) !=
                       std::end(g.obsCopyTargets)) {
            ComPtr<ID3D11ShaderResourceView> exposure;
            context->PSGetShaderResources(2, 1, &exposure);
            ComPtr<ID3D11Resource> r;
            ComPtr<ID3D11Texture2D> t;
            D3D11_TEXTURE2D_DESC d{};
            if (exposure)
                exposure->GetResource(&r);
            if (r && SUCCEEDED(r.As(&t)))
                t->GetDesc(&d);
            g.obsComposited = (d.Width == 1 && d.Height == 1) || marked_role(context) == static_cast<UINT>(YebisRole::composite);
        }
    } else if (plainDraw && source.Get() == resolved && target.Get() != resolved) {
        g.obsFinal = true;
    }
}

// After the open tracked pass's resolve, before its post-processing: a draw with a YEBIS pixel shader (any role, as the
// probe marked it) begins it (track_bind: a rebind of the pass's scene before it stays in the pass).
void note_post_seen(ID3D11DeviceContext* context) {
    tr.postSeen = marked_role(context) != static_cast<UINT>(YebisRole::none);
    // No copy shader known by its checksum: the first draw reading the resolve's target begins it.
    if (!tr.postSeen && g.copyUnknown && g.pass >= 0 && g.pass < 4 && g.resolved[g.pass])
        tr.postSeen = ps_input0(context).Get() == g.resolved[g.pass].Get();
}

// In the post window before the focus copy: a draw into a single-sample texture of the resolve's size, other than the
// resolve's target, noted as a stage's image (the last one wins), with whether its viewports cover the focus view
// beside the eye (as MVR's rewrite leaves them) and its pixel shader's checksum.
void note_post_stage(ID3D11DeviceContext* context) {
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    ComPtr<ID3D11Texture2D> t, r;
    if (!target || target.Get() == g.resolved[g.pass].Get() || !g.resolved[g.pass] || FAILED(target.As(&t)) ||
        FAILED(g.resolved[g.pass].As(&r)))
        return;
    D3D11_TEXTURE2D_DESC td, rd;
    t->GetDesc(&td);
    r->GetDesc(&rd);
    if (td.SampleDesc.Count != 1 || td.Width != rd.Width || td.Height != rd.Height)
        return;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT n = static_cast<UINT>(std::size(vp));
    context->RSGetViewports(&n, vp);
    const MvrPassView& v = g.view;
    bool covers = false;
    for (UINT i = 0; i < n && !covers; ++i)
        covers = vp[i].TopLeftX <= v.eyeW && vp[i].TopLeftY <= 0 && vp[i].TopLeftX + vp[i].Width >= v.eyeW + v.focusW &&
                 vp[i].TopLeftY + vp[i].Height >= v.focusH;
    g.stageImage = target.Get();  // raw, compared only
    g.stageCovers = covers;
    std::memset(g.stagePs, 0, sizeof(g.stagePs));
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    UINT size = sizeof(g.stagePs);
    if (ps && (FAILED(ps->GetPrivateData(kShaderChecksumGuid, &size, g.stagePs)) || size != sizeof(g.stagePs)))
        std::memset(g.stagePs, 0, sizeof(g.stagePs));
}

// MSAA off, no YEBIS copy shader known by its checksum (a game update), outside the tracked passes: the scene's target
// by YEBIS's composite's bindings alone. A draw into a single-sample target that was drawn with a depth target, reading
// a texture of its size that wasn't (t0: the scene copy, or a stage's image) and a 1x1 texture (t2: the exposure).
void learn_by_signature(ID3D11DeviceContext* context) {
    const ComPtr<ID3D11Resource> target = target0(context), source = ps_input0(context);
    ComPtr<ID3D11Texture2D> t, x;
    if (!target || !source || target.Get() == source.Get() || !drawn_with_depth(target.Get()) ||
        drawn_with_depth(source.Get()) || FAILED(target.As(&t)) || FAILED(source.As(&x)))
        return;
    D3D11_TEXTURE2D_DESC d, xd;
    t->GetDesc(&d);
    x->GetDesc(&xd);
    if (d.SampleDesc.Count != 1 || xd.SampleDesc.Count != 1 || xd.Width != d.Width || xd.Height != d.Height ||
        (target.Get() == tr.scene && std::memcmp(&d, &tr.sceneDesc, sizeof(d)) == 0))
        return;
    ComPtr<ID3D11ShaderResourceView> exposure;
    context->PSGetShaderResources(2, 1, &exposure);
    ComPtr<ID3D11Resource> r;
    ComPtr<ID3D11Texture2D> e;
    D3D11_TEXTURE2D_DESC ed{};
    if (exposure)
        exposure->GetResource(&r);
    if (r && SUCCEEDED(r.As(&e)))
        e->GetDesc(&ed);
    if (ed.Width != 1 || ed.Height != 1 || scene_too_small(d, true))
        return;
    learn_scene(target.Get(), d, true);
}

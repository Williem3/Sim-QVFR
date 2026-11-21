// Part of mvr_pass.cpp's translation unit (included once, in order): draw doubling (instancing, layouts, overlays
// outside the passes), the overlay trace, the lens mask and panel draws, the copy hook. Included only by mvr_pass.cpp,
// inside its anonymous namespace (lmu::<anonymous>), after mvr_pass_post.h; never include it anywhere else: the
// per-draw path stays in one translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// Instancing: draw H made with twice its instances (the instanced twin bound: each instance's even copy into the eye's
// viewport, its odd one into the focus view's); with input layouts that read per-instance data, the bound one's twin
// (step rates doubled, mvr_probe.cpp) around it, so a pair reads its instance's data.
// May the draw with the bound input layout be doubled? None or per-vertex data: yes. Per-instance data: only with its
// twin (`twin`, raw: the layout holds it while bound), else a pair would read two instances' data at the game's step
// rate. A layout the probe never saw (made before it hooked the device; a layout's elements can't be read back): not
// known, so no. `layout`: the bound one, referenced for the caller to release. Looked at only once the probe has seen
// a per-instance layout or missed some (mvr_probe_layouts_to_check); one cached lookup per draw then.
bool layout_doubles(ID3D11DeviceContext* context, ID3D11InputLayout*& layout, ID3D11InputLayout*& twin) {
    layout = twin = nullptr;
    if (!mvr_probe_layouts_to_check())
        return true;
    context->IAGetInputLayout(&layout);
    if (!layout)
        return true;
    // (a new layout may take a released one's address: the probe's generation changes with each made)
    if (const uint64_t generation = mvr_probe_layout_generation(); layout != g.layoutChecked || generation != g.layoutGeneration) {
        g.layoutChecked = layout;
        g.layoutGeneration = generation;
        g.layoutTwin = nullptr;
        UINT kind = 0, size = sizeof(kind);
        if (FAILED(layout->GetPrivateData(kMvrLayoutKindGuid, &size, &kind)) || size != sizeof(kind))
            kind = 0;
        if (kind == kMvrLayoutPerInstance) {
            IUnknown* unknown = nullptr;
            size = sizeof(unknown);
            if (SUCCEEDED(layout->GetPrivateData(kMvrInstancedLayoutGuid, &size, &unknown)) && unknown) {
                ComPtr<ID3D11InputLayout> t;
                unknown->QueryInterface(IID_PPV_ARGS(&t));
                unknown->Release();
                g.layoutTwin = t.Get();  // raw: the game's layout holds it
            }
        }
        g.layoutKind = kind;
    }
    twin = g.layoutTwin;
    if (g.layoutKind == kMvrLayoutPerVertex || (g.layoutKind == kMvrLayoutPerInstance && twin))
        return true;
    ++g.stats.layoutEyeOnly;
    if (!g.layoutEyeOnlyLogged) {
        g.layoutEyeOnlyLogged = true;
        simqvfr::logf("lmu mvr: pass %d: a draw with an input layout %s: drawn into the eye alone (counted from now on)",
                      g.pass, g.layoutKind == kMvrLayoutPerInstance ? "of per-instance data without its twin"
                                                                    : "the probe didn't see made");
    }
    return false;
}

// Instancing: draw H made with twice its instances (the instanced twin bound: each instance's even copy into the eye's
// viewport, its odd one into the focus view's); with an input layout that reads per-instance data, its twin `twin`
// (step rates doubled, mvr_probe.cpp) around it in place of `layout`, so a pair reads its instance's data
// (layout_doubles decides).
template <int H, typename... A>
void doubled_draw(ID3D11DeviceContext* context, ID3D11InputLayout* layout, ID3D11InputLayout* twin, A... a) {
    const std::tuple<A...> t(a...);
    if (twin) {
        context->IASetInputLayout(twin);
        ++g.stats.layoutSwaps;
    }
    ++g.stats.doubledDraws;
    if constexpr (H == kHookDrawIndexed)
        call_real<kHookDrawIndexedInstanced>(context, std::get<0>(t), UINT{2}, std::get<1>(t), std::get<2>(t), UINT{0});
    else if constexpr (H == kHookDraw)
        call_real<kHookDrawInstanced>(context, std::get<0>(t), UINT{2}, std::get<1>(t), UINT{0});
    else if constexpr (H == kHookDrawIndexedInstanced)
        call_real<H>(context, std::get<0>(t), std::get<1>(t) * 2, std::get<2>(t), std::get<3>(t), std::get<4>(t));
    else if constexpr (H == kHookDrawInstanced)
        call_real<H>(context, std::get<0>(t), std::get<1>(t) * 2, std::get<2>(t), std::get<3>(t));
    else
        static_assert(H == kHookDraw, "a draw with an instance count");
    if (twin)
        context->IASetInputLayout(layout);
}

// The game's draw H in a pass as it reaches d3d11: doubled with instancing while two views are on (its layout
// allowing), else as it was; a draw that can't be doubled with the game's own shader, into the eye's viewport alone.
template <int H, typename... A>
void game_draw(ID3D11DeviceContext* context, A... a) {
    // rFactor 2: its eye passes' draws by its own rules (the atlas, NVIDIA's single-pass stereo or instancing). Its input layouts
    // are not looked at (the layout twins are LMU's), as before; a draw it doesn't handle goes on as every game's below.
    if (g.rf2Policy &&
        rf2.draw_eye(
            context, g.context, g.replaying, kMvrInstancedTwinGuid, kMvrTwinGuid,
            [&](ID3D11VertexShader* shader, ID3D11ClassInstance* const* instances, UINT count) {
                realVSSetShader(context, shader, instances, count);
            },
            [&](UINT count, const D3D11_VIEWPORT* viewports) { realRSSetViewports(context, count, viewports); },
            [&](UINT count, const D3D11_RECT* scissors) { realRSSetScissorRects(context, count, scissors); },
            [&] { doubled_draw<H>(context, nullptr, nullptr, a...); },  // instancing: twice the instances
            [&] { call_real<H>(context, a...); },                       // single-pass stereo: once, the driver makes both views
            [] { return take_over_with_instancing(); }))  // the driver refused the two views: instancing, if it can
        return;
    if (!(g.instanced && g.on && ours(context))) {
        call_real<H>(context, a...);
        return;
    }
    ID3D11InputLayout* layout = nullptr;
    ID3D11InputLayout* twin = nullptr;
    if (layout_doubles(context, layout, twin)) {
        doubled_draw<H>(context, layout, twin, a...);
    } else {
        realVSSetShader(context, g.gameVs, g.instanceCount ? g.instances : nullptr, g.instanceCount);
        call_real<H>(context, a...);
        realVSSetShader(context, g.boundVs, g.instanceCount ? g.instances : nullptr, g.instanceCount);
    }
    if (layout)
        layout->Release();
}

// mvr_ui_mips: for the panel's canvas `canvas` (the draw's pixel shader input 0), a copy of it with its mips, made once a
// frame (the canvas is drawn before the panels); null when it has its own mips, is small, or the copy can't be made.
ID3D11ShaderResourceView* ui_canvas_with_mips(ID3D11DeviceContext* context, ID3D11ShaderResourceView* canvas) {
    State::UiMips& m = g.uiMips;
    if (!m.on || m.failed || !canvas)
        return nullptr;
    ComPtr<ID3D11Resource> resource;
    canvas->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    canvas->GetDesc(&view);
    if (!resource || FAILED(resource.As(&texture)) || view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return nullptr;
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.Width < 512 || d.Height < 256)
        return nullptr;
    if (m.source == resource.Get() && m.frame == g.framesSinceInstall && m.srv)
        return m.srv.Get();
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC have{};
    if (m.texture)
        m.texture->GetDesc(&have);
    if (!m.texture || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format ||
        m.viewFormat != view.Format) {
        m.texture.Reset();
        m.srv.Reset();
        UINT support = 0;
        D3D11_TEXTURE2D_DESC td = d;
        td.MipLevels = 0;  // the whole chain
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.CPUAccessFlags = 0;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = view.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = static_cast<UINT>(-1);
        const bool ok = SUCCEEDED(device->CheckFormatSupport(view.Format, &support)) &&
                        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) &&
                        SUCCEEDED(device->CreateTexture2D(&td, nullptr, &m.texture)) &&
                        SUCCEEDED(device->CreateShaderResourceView(m.texture.Get(), &sd, &m.srv));
        if (!ok) {
            m.failed = true;
            m.texture.Reset();
            m.srv.Reset();
            simqvfr::logf("lmu mvr: the VR UI canvas (%ux%u format %d) can't have a copy with mips made: the panel "
                          "samples it as it is",
                          d.Width, d.Height, static_cast<int>(view.Format));
            return nullptr;
        }
        m.viewFormat = view.Format;
        m.texture->GetDesc(&have);
        if (!m.logged) {
            m.logged = true;
            simqvfr::logf("lmu mvr: the VR UI canvas is %ux%u with one mip level: the panel's doubled draws sample a copy "
                          "with %u (lmu_ui_mips=0: as it is)",
                          d.Width, d.Height, have.MipLevels);
        }
    }
    const bool was = g.replaying;  // our copy, not one of the game's the copy hook records
    g.replaying = true;
    context->CopySubresourceRegion(m.texture.Get(), 0, 0, 0, 0, resource.Get(), 0, nullptr);
    g.replaying = was;
    context->GenerateMips(m.srv.Get());
    m.source = resource.Get();
    m.frame = g.framesSinceInstall;
    ++g.stats.uiMipFrames;
    return m.srv.Get();
}

// R3c: after the passes, a draw into an eye's final whose focus final was made this frame, with a vertex shader that has
// a twin (the VR UI's panel): drawn with two views, the eye's remap and the focus viewport beside its own (cut to the
// part the eye rendered when it is the whole target), its canvas with mips (mvr_ui_mips), the state put back. False:
// not doubled (the caller draws it).
template <int H, typename... A>
bool double_outside(ID3D11DeviceContext* context, A... a) {
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    int eye = -1;
    for (int e = 0; e < 2 && target; ++e)
        if (g.finalThisFrame[e] && target.Get() == g.eyeFinal[e].Get())
            eye = e;
    if (eye < 0)
        return false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11ClassInstance* instances[256] = {};
    UINT instanceCount = 256;
    context->VSGetShader(&vs, instances, &instanceCount);
    const auto release = [&] {
        if (vs)
            vs->Release();
        for (UINT i = 0; i < instanceCount && i < 256; ++i)
            if (instances[i])
                instances[i]->Release();
    };
    ID3D11VertexShader* twin = twin_of(vs);
    D3D11_VIEWPORT vps[kMaxRects];
    UINT n = kMaxRects;
    context->RSGetViewports(&n, vps);
    ComPtr<ID3D11GeometryShader> gs;
    context->GSGetShader(&gs, nullptr, nullptr);
    ID3D11InputLayout* layout = nullptr;
    ID3D11InputLayout* twinLayout = nullptr;
    // (instancing: a layout it can't double leaves the draw to the caller, as the game drew it)
    if (!twin || gs || n != 1 || g.broken || (g.instanced && !layout_doubles(context, layout, twinLayout))) {
        if (layout)
            layout->Release();
        release();
        return false;
    }
    const MvrPassView& v = g.viewOf[eye];
    ID3D11Buffer* kept = nullptr;
    context->VSGetConstantBuffers(13, 1, &kept);
    context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, v.remap, 0, 0);
    realVSSetConstantBuffers(context, 13, 1, g.remapCb.GetAddressOf());  // (not the game's: past the hook)
    bool doubled = false;
    if (g.instanced || g.twoViews(context, true) == 0) {
        const D3D11_VIEWPORT first = overlay_viewport(eye, v, vps[0]);
        const D3D11_VIEWPORT two[2] = {unshifted(first, v.overlayShift[0]),
                                       unshifted(focus_viewport_of(v, first, true), v.overlayShift[1])};
        ID3D11ShaderResourceView* canvas = nullptr;
        context->PSGetShaderResources(0, 1, &canvas);
        ID3D11ShaderResourceView* mipped = ui_canvas_with_mips(context, canvas);
        if (mipped)
            context->PSSetShaderResources(0, 1, &mipped);
        realVSSetShader(context, twin, instanceCount ? instances : nullptr, instanceCount);
        realRSSetViewports(context, 2, two);
        if (g.instanced) {
            doubled_draw<H>(context, layout, twinLayout, a...);
        } else {
            call_real<H>(context, a...);
            if (g.twoViews(context, false) != 0)
                stereo_off_failed("after an overlay's doubled draw");
        }
        realVSSetShader(context, vs, instanceCount ? instances : nullptr, instanceCount);
        realRSSetViewports(context, n, vps);
        if (mipped)
            context->PSSetShaderResources(0, 1, &canvas);
        if (canvas)
            canvas->Release();
        doubled = true;
        ++g.stats.overlaysOutside;
    }
    realVSSetConstantBuffers(context, 13, 1, &kept);
    if (kept)
        kept->Release();
    if (layout)
        layout->Release();
    release();
    return doubled;
}

// A shader's name for the trace: its checksum's first word (the probe's private data), "*" for a vertex shader with a
// twin, "-" for none, "?" for one not named.
template <class Shader>
std::string shader_name(Shader* shader) {
    if (!shader)
        return "-";
    uint32_t c[4] = {};
    UINT size = sizeof(c);
    char s[16];
    if (FAILED(shader->GetPrivateData(kShaderChecksumGuid, &size, c)) || size != sizeof(c))
        std::snprintf(s, sizeof(s), "?%04x", static_cast<unsigned>(reinterpret_cast<uintptr_t>(shader) & 0xffff));
    else
        std::snprintf(s, sizeof(s), "%08x", c[0]);
    std::string name = s;
    if constexpr (std::is_same_v<Shader, ID3D11VertexShader>) {
        UINT twinSize = 0;
        if (shader->GetPrivateData(g.instanced ? kMvrInstancedTwinGuid : kMvrTwinGuid, &twinSize, nullptr) == S_OK &&
            twinSize)
            name += "*";
    }
    return name;
}

// The overlay trace's line for one draw (the state it ran with).
void trace_draw(ID3D11DeviceContext* context, const char* call, long long a, long long b) {
    const bool inPass = g.pass >= 0;
    const char* phase = !inPass ? "outside" : g.vsRole == VsRole::lensMask ? "mask" : g.afterResolve ? "post" : nullptr;
    if (!phase)
        return;  // a pass's scene: not traced
    if (g.traceDraws >= 800) {
        ++g.traceDropped;
        return;
    }
    ++g.traceDraws;
    ComPtr<ID3D11VertexShader> boundVs;
    context->VSGetShader(&boundVs, nullptr, nullptr);
    ID3D11VertexShader* vs = inPass ? g.gameVs : boundVs.Get();  // in a pass the twin may be bound: the game's named
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    D3D11_VIEWPORT vps[kMaxRects];
    UINT n = kMaxRects;
    context->RSGetViewports(&n, vps);
    const bool resolvedTarget = inPass && target && target.Get() == g.resolved[g.pass].Get();
    simqvfr::logf("lmu mvr trace: f%llu p%d %s %s(%lld,%lld) vs %s ps %s > %s #%06x%s depth %s vp %u: %.0f,%.0f %.0fx%.0f%s",
                  g.gpuFrameNo, g.pass, phase, call, a, b, shader_name(vs).c_str(), shader_name(ps.Get()).c_str(),
                  describe(target.Get()).c_str(),
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(target.Get()) & 0xffffff),
                  resolvedTarget ? " (resolved)" : "", dsv ? "yes" : "no", n, n ? vps[0].TopLeftX : 0.0f,
                  n ? vps[0].TopLeftY : 0.0f, n ? vps[0].Width : 0.0f, n ? vps[0].Height : 0.0f,
                  inPass && g.on ? " two views" : "");
}

// R6c: a draw with the lens mask's vertex shader (its role): counted, drawn like the scene's (with two views in a pass
// that draws its focus view: the twin puts the hidden area where the focus view sees it, R6a), or dropped while the mask
// is stale (mvr_mask_stale). The first two logged with the state they ran with. True: dropped (the caller skips it).
bool lens_mask_draw(ID3D11DeviceContext* context, const char* call, long long a, long long b) {
    if (g.maskAbDrop) {  // lmu_mask_ab's side without the mask: not counted as stale
        ++g.stats.maskAbDropped;
        return true;
    }
    const bool drop = g.maskStale && g.maskStale->load(std::memory_order_relaxed);
    if (drop) {
        ++g.stats.maskDropped;
        ++g.maskDroppedSince;
    } else {
        ++g.stats.maskDraws;
        if (g.on)
            ++g.stats.maskDrawsTwo;
    }
    if (g.maskDrawsLogged >= 2)
        return drop;
    ++g.maskDrawsLogged;
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    ComPtr<ID3D11Resource> target, depthTarget;
    if (rtv)
        rtv->GetResource(&target);
    if (dsv)
        dsv->GetResource(&depthTarget);
    D3D11_VIEWPORT vps[kMaxRects];
    UINT n = kMaxRects;
    context->RSGetViewports(&n, vps);
    simqvfr::logf("lmu mvr: lens mask draw: f%llu p%d %s(%lld,%lld) ps %s > %s depth %s vp %u: %.0f,%.0f %.0fx%.0f; %s",
                  g.gpuFrameNo, g.pass, call, a, b, shader_name(ps.Get()).c_str(),
                  target ? describe(target.Get()).c_str() : "none",
                  depthTarget ? describe(depthTarget.Get()).c_str() : "none", n, n ? vps[0].TopLeftX : 0.0f,
                  n ? vps[0].TopLeftY : 0.0f, n ? vps[0].Width : 0.0f, n ? vps[0].Height : 0.0f,
                  drop ? "dropped (the mask is stale: a live FOV crop change)"
                  : g.on ? "two views (into the focus view too)"
                         : "the eye only");
    return drop;
}

// R6b: the VR UI panel's draw outside the passes in a frame without focus finals (the menus): the PanelFn around it.
bool panel_draw_here(ID3D11DeviceContext* context) {
    return g.panelFn && context == g.context && !g.replaying && g.pass < 0 && g.vsRole == VsRole::vruiPanel &&
           !g.finalThisFrame[0] && !g.finalThisFrame[1];
}
void call_panel_fn(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start) {
    g.replaying = true;  // its work (a resample executed on this context, the redraw) passes the hooks
    g.panelFn(context, after, count, start);
    g.replaying = false;
}

// The game's CopySubresourceRegion: passed on, remembered (mvr_copy_source).
void STDMETHODCALLTYPE copy_subresource_region(ID3D11DeviceContext* context, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
                                              ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box) {
    call_real<kHookCopy>(context, dst, dstSub, x, y, z, src, srcSub, box);
    if (context != g.context || g.replaying)
        return;
    if (pt.recording) {
        const std::string from = pt_tex(src), to = pt_tex(dst);
        pt_event("C %s > %s at %u,%u%s", from.c_str(), to.c_str(), x, y, box ? "" : " (whole)");
    }
    State::CopySeen& c = g.copies[g.copyNext++ % State::kCopiesKept];
    c.dst = dst;
    hold(c.src, src);  // per copy, not per draw
    c.frame = g.framesSinceInstall;
}

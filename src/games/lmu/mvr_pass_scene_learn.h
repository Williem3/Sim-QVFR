// Part of mvr_pass.cpp's translation unit (included once, in order): the scene's single-sample target learned with MSAA
// off (at YEBIS's copy, its composite, a stage). Included only by mvr_pass.cpp, inside its anonymous namespace
// (lmu::<anonymous>), after mvr_pass_doubling.h; never include it anywhere else: the per-draw path stays in one
// translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// Not every texture YEBIS post-processes is the scene's: LMU does smaller views of the world the same way (a 2560x160
// one, its mirror's by its shape, was learned in place of the 3120x2505 scene when a race session began on
// 2026-11-11 22:28; no eye pass was found from then on, so no focus views, no DLSS). The scene's target is of the
// eye targets' order: nothing under a quarter of an eye's final a side, nor of the scene learned before, takes
// its place. `count`: a refused one counted and said once.
bool scene_too_small(const D3D11_TEXTURE2D_DESC& d, bool count) {
    UINT fw = 0, fh = 0;
    ComPtr<ID3D11Texture2D> final0;
    if (g.eyeFinal[0] && SUCCEEDED(g.eyeFinal[0].As(&final0))) {
        D3D11_TEXTURE2D_DESC fd;
        final0->GetDesc(&fd);
        fw = fd.Width;
        fh = fd.Height;
    }
    const bool tooSmall = (fw && (d.Width * 4 < fw || d.Height * 4 < fh)) ||
                          (tr.sceneDesc.Width && (d.Width * 4 < tr.sceneDesc.Width || d.Height * 4 < tr.sceneDesc.Height));
    if (tooSmall && count) {
        ++tr.stats.sceneCandidatesRefused;
        if (!tr.smallLogged) {
            tr.smallLogged = true;
            simqvfr::logf("lmu mvr: pass tracker: a %ux%u target YEBIS's copy reads (drawn with a depth target) isn't taken "
                          "for the scene's: too small beside the %ux%u scene learned before and the eyes' %ux%u finals",
                          d.Width, d.Height, tr.sceneDesc.Width, tr.sceneDesc.Height, fw, fh);
        }
    }
    return tooSmall;
}

// The scene's single-sample target learned: `resource` (raw from here on, compared only), from YEBIS's copy reading
// it, or (`atComposite`) from its composite drawing into it past a stage.
void learn_scene(ID3D11Resource* resource, const D3D11_TEXTURE2D_DESC& d, bool atComposite) {
    const bool again = tr.sceneLogged;
    tr.scene = resource;
    tr.sceneDesc = d;
    tr.learnedThisFrame = true;
    tr.msaaSinceScene = false;
    tr.sceneLogged = true;
    tr.stats.sceneRelearned += again ? 1 : 0;
    tr.stats.sceneAtComposite += atComposite ? 1 : 0;
    if (!again || (atComposite && tr.stats.sceneAtComposite == 1))
        simqvfr::logf("lmu mvr: pass tracker: no multisample scene (MSAA off): the scene's target is %ux%u format %d (%s): "
                      "the eye passes begin at its binds with depth, their post-processing at %s",
                      d.Width, d.Height, static_cast<int>(d.Format),
                      atComposite ? "what YEBIS's composite draws into, drawn with a depth target; its copy reads a "
                                    "stage's image of it"
                                  : "what YEBIS's copy reads, drawn with a depth target",
                      atComposite ? "the stage's draw reading it" : "the copy");
}

// MSAA off (Tracker::scene): YEBIS's copy, before it draws. Outside the tracked passes, in a frame without a
// multisample scene: the single-sample texture it reads, if it was drawn with a depth target, is the scene's target,
// learned. In a pass begun on that target: the pass's post-processing begins here, before the copy reads the scene
// (post_begins: DLAA in place, the focus post's window), as it does at the resolve with MSAA on.
void copy_before(ID3D11DeviceContext* context) {
    if (tr.msaaFrame || g.afterResolve)
        return;
    if (marked_role(context) != static_cast<UINT>(YebisRole::copy))
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> resource;
    if (srv)
        srv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)))
        return;
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.SampleDesc.Count != 1)
        return;
    if (tr.open && tr.singleSample && ours(context) && g.pass < 2) {
        if (resource.Get() != tr.scene)
            return;
        stamp(g.pass, 1);  // the scene ends, the post-processing starts
        g.afterResolve = true;
        rects_uncut();
        tr.resolved = true;
        ++g.stats.singleSamplePosts;
        post_begins(context, resource.Get(), resource.Get());
        return;
    }
    if (tr.open || (resource.Get() == tr.scene && std::memcmp(&d, &tr.sceneDesc, sizeof(d)) == 0))
        return;
    if (!drawn_with_depth(resource.Get())) {
        // Not the scene's target: perhaps a stage's image of it (learn_at_composite), if of the scene's order.
        if (!tr.stageCandidate && !scene_too_small(d, false)) {
            tr.stageCandidate = resource.Get();  // raw, compared only
            tr.candidateW = d.Width;
            tr.candidateH = d.Height;
        }
        return;
    }
    if (scene_too_small(d, true))
        return;
    learn_scene(resource.Get(), d, false);
}

// MSAA off past a stage, outside the tracked passes: a draw reading the copy's source of this frame (the stage's image
// of the scene) into a single-sample target of its size that was drawn with a depth target is YEBIS's composite, by
// its bindings, and that target the scene's.
void learn_at_composite(ID3D11DeviceContext* context) {
    if (tr.msaaFrame || tr.open) {
        tr.stageCandidate = nullptr;
        return;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> source;
    if (srv)
        srv->GetResource(&source);
    if (!source || source.Get() != tr.stageCandidate)
        return;
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    ComPtr<ID3D11Texture2D> texture;
    if (!target || target.Get() == source.Get() || !drawn_with_depth(target.Get()) || FAILED(target.As(&texture)))
        return;
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.Width != tr.candidateW || d.Height != tr.candidateH)
        return;
    tr.stageCandidate = nullptr;
    if ((target.Get() == tr.scene && std::memcmp(&d, &tr.sceneDesc, sizeof(d)) == 0) || scene_too_small(d, true))
        return;
    learn_scene(target.Get(), d, true);
}

// MSAA off past a stage, in a pass begun on the scene's target, its post-processing not begun: before a draw into
// another single-sample texture of the scene's size, bound without depth, that reads the scene's target (the stage's
// image of the scene, which YEBIS's copy then starts from). The pass's post-processing begins here, as at the copy
// without a stage: DLAA in place before anything reads the scene, the focus post's window (which then notes the
// stage's image: note_post_stage). Not YEBIS's blit (a blit reading the scene mid-pass isn't the post's start).
void stage_before(ID3D11DeviceContext* context) {
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC d{};
    if (target && SUCCEEDED(target.As(&texture)))
        texture->GetDesc(&d);
    if (!texture || d.SampleDesc.Count != 1 || d.Width != tr.sceneDesc.Width || d.Height != tr.sceneDesc.Height) {
        tr.offScene = false;  // not a target a stage's image could be: nothing more to look at until the next bind
        return;
    }
    if (marked_role(context) == static_cast<UINT>(YebisRole::blit))
        return;
    ID3D11ShaderResourceView* srvs[8] = {};
    context->PSGetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
    ComPtr<ID3D11Resource> scene;
    for (ID3D11ShaderResourceView* srv : srvs) {
        if (!srv)
            continue;
        ComPtr<ID3D11Resource> r;
        srv->GetResource(&r);
        if (r.Get() == tr.scene)
            scene = r;
        srv->Release();
    }
    if (!scene)
        return;
    tr.offScene = false;
    stamp(g.pass, 1);  // the scene ends, the post-processing starts
    g.afterResolve = true;
    rects_uncut();
    tr.resolved = true;
    ++g.stats.singleSamplePosts;
    ++g.stats.singleSampleStagePosts;
    if (!tr.stagePostLogged) {
        tr.stagePostLogged = true;
        simqvfr::logf("lmu mvr: pass tracker, MSAA off: frame %llu pass %d: a draw into %s reads the scene's target before "
                      "YEBIS's copy (a stage between them): the pass's post-processing begins there",
                      static_cast<unsigned long long>(g.framesSinceInstall), g.pass, describe(target.Get()).c_str());
    }
    post_begins(context, scene.Get(), scene.Get());
}

// A pixel shader's YEBIS role as the probe marked it by its checksum (0: none, or not one the probe knows). Inside a
// pass with the PSSetShader hook on (texture sharpening or car slots), the game's own shader as its last bind set it
// (read_bindings at the pass's start, the hook after: what update_ps trusts too), with no runtime call per draw;
// elsewhere asked of the context.
UINT marked_role(ID3D11DeviceContext* context) {
    if (context == g.context && g.pass >= 0 && !g.replaying && (g.biasHooks || g.slotsOn)) {
        ++g.stats.rolesByBind;
        return role_of_ps(g.gamePs);
    }
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    return role_of_ps(ps.Get());
}

// Part of mvr_pass.cpp's translation unit (included once, in order): target binds and rects: the OM and RS hooks,
// DLAA's depth target, FSR, the resolve and the post-processing's beginning. Included only by mvr_pass.cpp, inside its
// anonymous namespace (lmu::<anonymous>), after mvr_pass_two_views.h; never include it anywhere else: the per-draw path
// stays in one translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// After this pass's focus final blit (R3c): whether the first render target is the eye's final (an overlay's draw).
bool overlay_target(UINT n, ID3D11RenderTargetView* const* rtvs) {
    if (!g.focusBlitted || !n || !rtvs || !rtvs[0])
        return false;
    ComPtr<ID3D11Resource> r;
    rtvs[0]->GetResource(&r);
    return r && r.Get() == g.eyeFinal[g.pass & 1].Get();
}

// DLAA: which depth target a pass's render targets are drawn with (inside the tracked passes only: the views are held).
void note_scene_bind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!dsv || !n || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL || !rtvs || !rtvs[0] || !simqvfr::dlss_wanted())
        return;
    for (State::SceneBind& b : g.sceneBinds)
        if (b.rtv.Get() == rtvs[0]) {
            if (b.dsv.Get() != dsv)
                b.dsv = dsv;
            return;
        }
    State::SceneBind& b = g.sceneBinds[g.sceneBindNext++ % std::size(g.sceneBinds)];
    b.rtv = rtvs[0];
    b.dsv = dsv;
}

// The depth target last bound with a render target on `colour` (the scene's multisample target at its resolve).
ID3D11DepthStencilView* scene_depth(ID3D11Resource* colour) {
    for (State::SceneBind& b : g.sceneBinds) {
        if (!b.rtv || !b.dsv)
            continue;
        ComPtr<ID3D11Resource> r;
        b.rtv->GetResource(&r);
        if (r.Get() == colour)
            return b.dsv.Get();
    }
    return nullptr;
}

// FSR (mvr_fsr): this pass's focus composite into the scene's target beside the eye, right after YEBIS's composite
// drew the eye's there: FSR upscales the whole target into the eye's image, the focus view with it. False: not placed.
bool fsr_place(int eye) {
    ID3D11Texture2D* image = g.focusPost.image(eye);
    if (!image || !g.resolved[g.pass])
        return false;
    const MvrPassView& v = g.view;
    // Room beside the eye in the scene's target, and the whole region in the image (else no copy: no focus final).
    ComPtr<ID3D11Texture2D> scene;
    D3D11_TEXTURE2D_DESC sd{}, id{};
    image->GetDesc(&id);
    if (SUCCEEDED(g.resolved[g.pass].As(&scene)))
        scene->GetDesc(&sd);
    if (!scene || v.eyeW + v.focusW > sd.Width || v.focusH > sd.Height || v.focusW > id.Width || v.focusH > id.Height) {
        ++g.stats.focusFailures;
        return false;
    }
    const D3D11_BOX box{0, 0, 0, v.focusW, v.focusH, 1};
    const bool was = g.replaying;
    g.replaying = true;
    g.context->CopySubresourceRegion(g.resolved[g.pass].Get(), 0, v.eyeW, 0, 0, image, 0, &box);
    g.replaying = was;
    ++g.stats.fsrPlaced;
    return true;
}

// FSR: a render target bind in a pass whose focus composite was placed (fsr_place). The eye's image bound: FSR has
// written it, the focus view beside the eye's corner, stretched like it. The focus final is there; from here on the
// pass's draws into the eye's image are overlays, as after the final blit (focus_blit).
void fsr_final(UINT n, ID3D11RenderTargetView* const* rtvs) {
    if (!n || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL || !rtvs || !rtvs[0])
        return;
    const int eye = g.pass & 1;
    ComPtr<ID3D11Resource> target;
    rtvs[0]->GetResource(&target);
    ComPtr<ID3D11Texture2D> texture;
    if (!target || target.Get() != g.fsrEye[eye] || FAILED(target.As(&texture)))
        return;
    g.focusBlitted = true;  // once a pass
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    MvrPassView& v = g.view;
    const unsigned ew = final_eye_w(v), eh = final_eye_h(v);
    const unsigned fw = v.eyeW ? static_cast<unsigned>(std::lround(static_cast<double>(v.focusW) * ew / v.eyeW)) : v.focusW;
    const unsigned fh = v.eyeH ? static_cast<unsigned>(std::lround(static_cast<double>(v.focusH) * eh / v.eyeH)) : v.focusH;
    if (ew + fw > d.Width || fh > d.Height) {
        ++g.stats.focusFailures;
        return;
    }
    v.finalFocusW = g.viewOf[eye].finalFocusW = fw == v.focusW ? 0 : fw;
    v.finalFocusH = g.viewOf[eye].finalFocusH = fh == v.focusH ? 0 : fh;
    ++g.stats.focusBlits;
    ++g.stats.fsrFinals;
    if (tr.open) {
        tr.blitSeen = true;
        tr.finalTarget = target.Get();  // raw, compared only (a single-sample pass ends at another target after it)
    }
    g.eyeFinal[eye] = target;
    g.finalW[eye] = d.Width;
    g.finalH[eye] = d.Height;
    g.finalThisFrame[eye] = true;
    if (!g.fsrLogged) {
        g.fsrLogged = true;
        simqvfr::logf("lmu mvr: focus post: pass %d: FSR on: the focus composite beside the eye in the scene's target, "
                      "upscaled with it; the focus final %ux%u at %u,0 of the eye's %ux%u image",
                      g.pass, fw, fh, ew, d.Width, d.Height);
    }
}

// Car slots: the slot target given back among the game's render targets (an OMGetRenderTargets/OMSet round trip)
// dropped: `rtvs` then points at `own`, a copy without it, `n` its count up to the last view (the others unbound either
// way). At most eight compares, and only while the slot target exists.
void without_slot(UINT& n, ID3D11RenderTargetView* const*& rtvs, ID3D11RenderTargetView* (&own)[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]) {
    ID3D11RenderTargetView* const slot = g.slotRtv.Get();
    if (!slot || !rtvs || !n || n > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return;
    bool found = false;
    for (UINT i = 0; i < n; ++i)
        found = found || rtvs[i] == slot;
    if (!found)
        return;
    for (UINT i = 0; i < n; ++i)
        own[i] = rtvs[i] == slot ? nullptr : rtvs[i];
    while (n > 1 && !own[n - 1])
        --n;
    rtvs = own;
}

void STDMETHODCALLTYPE om_set_render_targets(ID3D11DeviceContext* context, UINT n, ID3D11RenderTargetView* const* rtvs,
                                             ID3D11DepthStencilView* dsv) {
    ID3D11RenderTargetView* own[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    if (context == g.context && !g.replaying && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        without_slot(n, rtvs, own);
    if (!ours(context)) {
        realOMSetRenderTargets(context, n, rtvs, dsv);
        if (tr.on && context == g.context && !g.replaying) {
            track_bind(n, rtvs, dsv);
            track_fsr_final(n, rtvs);
        }
        if (ours(context)) {  // the bind that began a tracked pass: its scene's targets
            note_scene_bind(n, rtvs, dsv);
            slot_bind(n, rtvs, dsv);
            if (g.glass.mode)
                glass_note_bind(dsv);
        } else if (tr.on && context == g.context && !g.replaying) {  // MSAA off: what the scene's target may be
            note_drawn_with_depth(n, rtvs, dsv);
        }
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realOMSetRenderTargets(context, n, rtvs, dsv);
    if (tr.on)
        track_bind(n, rtvs, dsv);
    note_scene_bind(n, rtvs, dsv);
    if (ours(context))  // (a bind the tracker ended the pass at isn't the pass's)
        slot_bind(n, rtvs, dsv);
    if (g.glass.mode && ours(context))
        glass_note_bind(dsv);
    if (g.fsr && g.compositeOk && !g.focusBlitted && ours(context) && g.pass < 2 && g.drawFocus)
        fsr_final(n, rtvs);
    track_fsr_final(n, rtvs);
    if (g.fsr && g.observeShape && g.obsComposited && !g.obsFinal && ours(context) && g.pass < 2 && n && rtvs && rtvs[0]) {
        ComPtr<ID3D11Resource> bound;  // the shape's final under FSR: the eye's image bound after the composite
        rtvs[0]->GetResource(&bound);
        g.obsFinal = bound && bound.Get() == g.fsrEye[g.pass & 1];
    }
    g.depthBound = dsv != nullptr;
    g.overlayTarget = overlay_target(n, rtvs);
    const bool recut = cut_rects();  // live resolution: the cut decided again for the target now bound
    update();
    if (recut) {
        apply_viewports();
        if (g.scCount)
            apply_scissors();
    }
}

void STDMETHODCALLTYPE om_set_render_targets_and_uavs(ID3D11DeviceContext* context, UINT n,
                                                      ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                                      UINT uavStart, UINT uavCount,
                                                      ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    ID3D11RenderTargetView* own[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    if (context == g.context && !g.replaying && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        without_slot(n, rtvs, own);
    if (!ours(context)) {
        realOMSetRenderTargetsAndUavs(context, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
        if (tr.on && context == g.context && !g.replaying && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
            track_bind(n, rtvs, dsv);
            track_fsr_final(n, rtvs);
        }
        if (ours(context)) {
            note_scene_bind(n, rtvs, dsv);
            if (g.glass.mode && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
                glass_note_bind(dsv);
        } else if (tr.on && context == g.context && !g.replaying) {
            note_drawn_with_depth(n, rtvs, dsv);
        }
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    // The game's render targets kept, its views from slot 1 on: the slot target holds RT1 (the runtime would refuse
    // the call), so it goes first.
    if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && g.slotsBound && uavCount != D3D11_KEEP_UNORDERED_ACCESS_VIEWS &&
        uavCount && uavStart < 2)
        slot_unbind();
    realOMSetRenderTargetsAndUavs(context, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
    if (tr.on && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        track_bind(n, rtvs, dsv);
    note_scene_bind(n, rtvs, dsv);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && ours(context))
        slot_bind(0, nullptr, nullptr);  // car slots: not beside unordered access views
    if (g.fsr && g.compositeOk && !g.focusBlitted && ours(context) && g.pass < 2 && g.drawFocus)
        fsr_final(n, rtvs);
    track_fsr_final(n, rtvs);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        g.depthBound = dsv != nullptr;
        g.overlayTarget = overlay_target(n, rtvs);
        if (g.glass.mode && ours(context))
            glass_note_bind(dsv);
        update();
    }
}

void STDMETHODCALLTYPE rs_set_viewports(ID3D11DeviceContext* context, UINT n, const D3D11_VIEWPORT* vps) {
    // Not ours, or a call the runtime drops (no rects, too many): passed on untouched, nothing recorded.
    if (!ours(context) || (n && !vps) || n > kMaxRects) {
        realRSSetViewports(context, n, vps);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    D3D11_VIEWPORT own;
    if (g.twoVpsApplied && n == 2 && sizeCut.vpCount == 1 && std::memcmp(vps, g.appliedVps, sizeof(g.appliedVps)) == 0) {
        own = sizeCut.vps[0];  // our two given back (a Get/Set round trip): the game's own one
        n = 1;
        vps = &own;
    }
    g.vpCount = n;
    if (n)
        std::memcpy(g.vps, vps, n * sizeof(D3D11_VIEWPORT));
    keep_game_vps();
    cut_rects();  // live resolution: the game's render size cut to the eye's
    const bool was = g.on;
    update();  // two views only with one viewport
    if (g.on == was)  // (update applied them if they turned on or off)
        apply_viewports();
}

// The game reading its viewports back (RSGetViewports). YEBIS's start reads the bound one to size the region it
// processes (mvr_pass_internal.h SizeCut): while ours are bound (the live resolution's cut, or the focus view's beside
// the eye's) the game is given its own, as it set them, so it sees what it would see without us. With Post Effects Low
// (and MSAA off) nothing ends the cut before YEBIS starts (no depth copy), and YEBIS stretched the eye's corner over its
// whole target: the periphery zoomed (2026-11-17, its copy's triangle scaled by full / eye). Our own reads (from this
// image: the hooks, the focus post's state guards, the tests) see what is bound.
bool from_this_image(const void* at) {
    static const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    static const uintptr_t size =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(&__ImageBase) + __ImageBase.e_lfanew)
            ->OptionalHeader.SizeOfImage;
    return reinterpret_cast<uintptr_t>(at) - base < size;
}

// The bound viewports aren't the game's own (cut, or two for the focus view).
bool viewports_ours() {
    return g.twoVpsApplied || g.vpCount != sizeCut.vpCount ||
           std::memcmp(g.vps, sizeCut.vps, g.vpCount * sizeof(D3D11_VIEWPORT)) != 0;
}

// The game's exe (YEBIS is linked into it): its reads, as against another DLL's (NGX's, a tool's).
bool from_game_exe(const void* at) {
    static const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const uintptr_t size = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        return static_cast<uintptr_t>(
            reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew)
                ->OptionalHeader.SizeOfImage);
    }();
    return reinterpret_cast<uintptr_t>(at) - base < size;
}

using RSGetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT*, D3D11_VIEWPORT*);
void rs_get_viewports(RSGetViewportsFn real, const void* caller, ID3D11DeviceContext* context, UINT* n,
                      D3D11_VIEWPORT* vps) {
    if (!n || !ours(context) || (from_this_image(caller) && !g.readsAsGameForTests) || !viewports_ours()) {
        real(context, n, vps);
        return;
    }
    // Another DLL than the game's exe reading the viewports while ours are bound (the log: which, and how often).
    if (!g.readsAsGameForTests && !from_game_exe(caller)) {
        ++g.stats.otherViewportReads;
        if (!g.otherReadLogged) {
            g.otherReadLogged = true;
            HMODULE module = nullptr;
            wchar_t name[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(caller), &module))
                GetModuleFileNameW(module, name, MAX_PATH);
            simqvfr::logf("lmu mvr: pass %d: %ls read the viewports while ours were bound: %s (the first; counted from "
                          "now on, lmu_viewport_reads=%d)",
                          g.pass, name, g.viewportReads == 2 ? "given the bound ones" : "given the game's own",
                          g.viewportReads);
        }
        if (g.viewportReads == 2) {  // only the game's exe is answered with its own
            real(context, n, vps);
            return;
        }
    }
    const UINT count = sizeCut.vpCount;
    if (vps) {
        for (UINT i = 0; i < *n; ++i)
            vps[i] = i < count ? sizeCut.vps[i] : D3D11_VIEWPORT{};
    }
    if (!vps || *n >= count)
        *n = count;
    ++g.stats.gameViewportReads;
    const int why = cut_active() ? 0 : 1;
    if (!g.gameReadLogged[why] && count) {
        g.gameReadLogged[why] = true;
        const D3D11_VIEWPORT& v = sizeCut.vps[0];
        simqvfr::logf("lmu mvr: pass %d: the game read its viewport back while ours were bound (%s): given its own "
                      "%.0f,%.0f %.0fx%.0f (the first; counted from now on)",
                      g.pass, why == 0 ? "the live resolution's cut" : "the focus view's beside the eye's", v.TopLeftX,
                      v.TopLeftY, v.Width, v.Height);
    }
}

// Compute writes bound (mvr_fsr_watch, a build whose FSR setting can't be read): FSR's sharpening writes the eye's
// image from a compute shader, so its view of the image bound here says FSR is on. Passed on first, untouched.
void cs_set_uavs(ID3D11DeviceContext* context, UINT start, UINT n, ID3D11UnorderedAccessView* const* uavs,
                 const UINT* counts) {
    call_real<kHookCSUav>(context, start, n, uavs, counts);
    if (!g.fsrWatch || g.fsrUavSeen || !uavs || !ours(context) || (!g.fsrEye[0] && !g.fsrEye[1]))
        return;
    for (UINT i = 0; i < n && i < D3D11_1_UAV_SLOT_COUNT; ++i) {
        if (!uavs[i])
            continue;
        ComPtr<ID3D11Resource> r;
        uavs[i]->GetResource(&r);
        if (r && (r.Get() == g.fsrEye[0] || r.Get() == g.fsrEye[1])) {
            g.fsrUavSeen = true;
            return;
        }
    }
}

void STDMETHODCALLTYPE rs_set_scissor_rects(ID3D11DeviceContext* context, UINT n, const D3D11_RECT* rects) {
    if (!ours(context) || (n && !rects) || n > kMaxRects) {
        realRSSetScissorRects(context, n, rects);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    D3D11_RECT own;
    if (g.twoScsApplied && n == 2 && sizeCut.scCount == 1 && std::memcmp(rects, g.appliedScs, sizeof(g.appliedScs)) == 0) {
        own = sizeCut.scs[0];  // our two given back: the game's own one
        n = 1;
        rects = &own;
    }
    g.scCount = n;
    if (n)
        std::memcpy(g.scs, rects, n * sizeof(D3D11_RECT));
    keep_game_scs();
    cut_rects();
    if (!g.scCount) {
        realRSSetScissorRects(context, 0, nullptr);
        g.twoScsApplied = false;
    } else {
        apply_scissors();
    }
}

// The resolve target cleared (its view made once: it holds the resource, so the pointer stays its own).
ComPtr<ID3D11RenderTargetView> clearRtv;
ID3D11Resource* clearFor = nullptr;

void clear_target(ID3D11Resource* target) {
    if (clearFor != target) {
        clearRtv.Reset();
        clearFor = target;
        ComPtr<ID3D11Device> device;
        g.context->GetDevice(&device);
        if (FAILED(device->CreateRenderTargetView(target, nullptr, &clearRtv)))
            clearRtv.Reset();
    }
    if (clearRtv) {
        const float black[4] = {0, 0, 0, 0};
        g.context->ClearRenderTargetView(clearRtv.Get(), black);
    }
}

void post_begins(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* drawn);

void STDMETHODCALLTYPE resolve_subresource(ID3D11DeviceContext* context, ID3D11Resource* dst, UINT dstSub,
                                           ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format) {
    if (pt.recording && context == g.context && !g.replaying) {
        const std::string from = pt_tex(src), to = pt_tex(dst);
        pt_event("R %s > %s, %s", from.c_str(), to.c_str(),
                 tr.open ? ("in tracked pass " + std::to_string(tr.pass)).c_str() : "outside the tracked passes");
    }
    if (tr.on && tr.open && context == g.context && !g.replaying && src) {
        ComPtr<ID3D11Texture2D> source;
        D3D11_TEXTURE2D_DESC sd{};
        if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&source)))) {
            source->GetDesc(&sd);
            tr.resolved = tr.resolved || sd.SampleDesc.Count > 1;
        }
    }
    bool multisample = false;
    if (ours(context) && g.pass <= 3 && src && dst) {
        ComPtr<ID3D11Texture2D> source;
        if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&source)))) {
            D3D11_TEXTURE2D_DESC d;
            source->GetDesc(&d);
            multisample = d.SampleDesc.Count > 1;
        }
    }
    const int eye = g.pass & 1;
    // A second multisample resolve in the same pass (its post began at the first): passed on as the game's, nothing
    // begun again. Else DLAA ran a second time, the post's scene (g.resolved) moved to the second's target and the
    // focus window was reset. LMU resolving twice in a pass isn't seen yet (review 2026-11-17): logged once if it does.
    if (multisample && g.afterResolve) {
        realResolveSubresource(context, dst, dstSub, src, srcSub, format);
        ++g.stats.extraResolves;
        if (!g.extraResolveLogged) {
            g.extraResolveLogged = true;
            simqvfr::logf("lmu mvr: pass %d: a second multisample resolve after its post began (into %s): passed on, the "
                          "post not begun again (the first; counted from now on)",
                          g.pass, describe(dst).c_str());
        }
        return;
    }
    if (multisample) {
        stamp(g.pass, 1);  // the scene ends, the post-processing starts
        g.afterResolve = true;
        rects_uncut();
    }
    if (multisample && g.replaceResolve && g.keptOk[eye]) {
        // Gate D: the scene this pass skipped is the focus view its eye's pass drew: into the top-left instead. Cleared
        // first: the post-processing covers the top-left at one scale, beyond the focus view where pass 1's images still
        // are (they would bloom into it).
        const MvrPassView& v = g.viewOf[eye];
        clear_target(dst);
        const D3D11_BOX box{0, 0, 0, v.focusW, v.focusH, 1};
        g.context->CopySubresourceRegion(dst, dstSub, 0, 0, 0, g.kept[eye].Get(), 0, &box);
        g.resolved[g.pass] = dst;
        ++g.stats.replacedResolves;
        return;
    }
    realResolveSubresource(context, dst, dstSub, src, srcSub, format);
    if (!multisample)
        return;
    ++g.stats.resolves;
    post_begins(context, dst, src);
}

// The pass's post-processing begins on `scene` (the resolve's target; with MSAA off the scene's own target, at YEBIS's
// copy, before it reads it), which was drawn into `drawn` (the multisample target, or the same): DLAA on the eye and its
// focus view, gate D's kept region, the focus post's window.
// Night car lighting: the eye's camera. The world's first (dlss_inputs.h lmu_pass_camera: the renderer's or the drawn
// pair with the static world's draws, as the race of 07:34 was right with; the same choice every pass and in both
// eyes); only without one, the pair the pass drew the most recorded car parts with, when it has the eye's FOV. That
// pair came first for a morning (2026-11-19) and wasn't one camera: its counts restart as the pass's 16 kept pairs turn
// over, and one that isn't the eye's (a mirror's) fell back to lmu_pass_camera, whose depth was then the other way
// round (view_pair.h view_proj_of_pair): doubled panels in one eye, flicker. False: none.
bool car_light_camera(int eye, float (&view)[16], float (&viewProj)[16]) {
    if (lmu_pass_camera(eye, view, viewProj))
        return true;
    if (mvr_car_view_pair(eye, view, viewProj)) {
        float p[16], expected[16];
        // The eye's FOV (a mirror's camera, with more car parts drawn, wouldn't be the eye's).
        if (projection_of_pair(view, viewProj, p) && (!qvfr_frame_eye_projection(eye, expected) || same_eye(p, expected)))
            return true;
    }
    return false;
}

void car_light_run(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* drawn, int eye) {
    CarLightView v;
    // Without a camera or the pass's depth the relight can't run; night vision's colour still can (CarLightPass::run).
    v.camera = car_light_camera(eye, v.view, v.viewProj);
    if (!v.camera)
        ++g.carLightMisses;
    ID3D11DepthStencilView* dsv = scene_depth(drawn);
    ComPtr<ID3D11Resource> depthTexture;
    if (dsv)
        dsv->GetResource(&depthTexture);
    if ((!v.camera || !depthTexture) && !g.carLightSettings.nightVision)
        return;
    v.eyeW = g.view.eyeW;
    v.eyeH = g.view.eyeH;
    if (g.drawFocus && g.passTwinBinds > 0) {
        v.focusW = g.view.focusRenderW ? g.view.focusRenderW : g.view.focusW;
        v.focusH = g.view.focusRenderH ? g.view.focusRenderH : g.view.focusH;
        std::memcpy(v.remap, g.view.remap, sizeof(v.remap));
    }
    simqvfr::d3d11_state::Spec spec;
    spec.parts = simqvfr::d3d11_state::kInputLayout | simqvfr::d3d11_state::kVS | simqvfr::d3d11_state::kPS |
                 simqvfr::d3d11_state::kGsHsDs | simqvfr::d3d11_state::kOMTargets | simqvfr::d3d11_state::kBlend |
                 simqvfr::d3d11_state::kDepth | simqvfr::d3d11_state::kRSState | simqvfr::d3d11_state::kViewports |
                 simqvfr::d3d11_state::kScissors;
    spec.psCb = {0, 3};
    spec.psSrv = {0, 7};
    bool stereoOff = false;
    if (g.stereo && !g.instanced) {
        if (g.twoViews(g.context, false) == 0)
            stereoOff = true;
        else
            stereo_off_failed("before the night car lighting");
    }
    const bool was = g.replaying;
    g.replaying = true;
    {
        // Put back through the context as the game's calls would be (as DLSS's own guard does, its draws inside
        // g.replaying too): the hooks see the game's shader and targets bound again.
        const simqvfr::d3d11_state::Guard<> keep(context, spec);
        if (g.carLightPass.run(context, scene, depthTexture.Get(), g.slotSrv.Get(), g.slotSamples > 1, g.carLights, v,
                               g.carLightSettings, eye, g.normalsOn ? g.normalSrv.Get() : nullptr,
                               g.detailOn ? g.detailSrv.Get() : nullptr, g.spotValid ? g.spotShadow.data() : nullptr,
                               g.spotValid ? g.spotShadow.size() : 0))
            ++g.carLightRuns;
    }
    g.replaying = was;
    if (stereoOff && g.twoViews(g.context, true) != 0) {
        g.stereo = false;
        update();
    }
}

void post_begins(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* drawn) {
    Timed postTimed(g.time.post, true);
    ID3D11Resource* const dst = scene;
    const int eye = g.pass & 1;
    g.resolved[g.pass] = dst;
    if (g.glass.mode && g.pass < 2) {  // DLSS's depth before the transparents: this pass's counts
        g.stats.glassPasses += g.glass.copied ? 1 : 0;
        g.stats.glassOpaqueAfter += g.glass.copied ? g.glass.opaqueAfterCopy : 0;
        g.stats.glassPassDraws += g.glass.copied ? tr.draws - tr.actualBegin[g.pass] : 0;
        g.glass.sceneBound = false;  // nothing after the scene is copied for
        g.glass.dsv = nullptr;
    }
    // Car slots: the slot target off the pipeline (the post doesn't draw into it; DLSS reads it), each part's motion
    // uploaded for this pass's vectors, now and then the gate image (level 1 and up).
    bool slotsDrawn = false;
    if (g.slotsOn) {
        slot_unbind();
        slotsDrawn = g.slotsCleared && g.pass < 2;
        if (slotsDrawn && g.motionBuffer) {
            {
                // Only the parts first drawn since the last upload (pass 0: the frame's; pass 1: the few its eye alone
                // drew), only their range of slots: run 25's A/B found the cars' motion dearer than its tracking alone.
                Timed timed(g.time.motion);
                size_t first = 0, last = 0;
                const size_t moving = g.cars.update_motions(g.motion, first, last);
                if (moving && first <= last && g.motion.size() * sizeof(float) == size_t{kCarSlots} * 48) {
                    const D3D11_BOX box = {static_cast<UINT>(first * 48), 0, 0, static_cast<UINT>((last + 1) * 48), 1,
                                           1};
                    g.context->UpdateSubresource(g.motionBuffer.Get(), 0, &box, g.motion.data() + first * 12, 0, 0);
                    ++g.stats.motionUploads;
                    g.stats.motionSlots += moving;
                }
            }
            // The gate image (diagnostics runs only: run 26b's two froze the game ~250 ms each, max frame 508 ms).
            const bool armed = g.slotImages && g.pass == 0 && g.stats.slotImages < 4 && g.slotViewsThisPass >= 50 &&
                               g.framesSinceInstall >= g.slotImageAfter;
            if (armed) {
                g.slotImageFrame = g.framesSinceInstall;
                g.slotImageAfter = g.framesSinceInstall + 2700;  // the next a while later
            }
            if (g.slotImageFrame == g.framesSinceInstall && g.slotImageFrame)
                save_slot_image(eye, dst);  // both eyes of the frame
        }
    }
    // Night car lighting (car_light.h): the cars relit in the resolved scene before DLSS and the post read it.
    if (slotsDrawn && g.carLightOn && g.pass != g.eyeOff)
        car_light_run(context, dst, drawn, eye);
    // A focus view drawn smaller (DLSS super resolution) goes through the module even if DLSS went off since the pass
    // was set up: it stretches what DLSS can't upscale.
    const bool smaller = (g.view.focusRenderW && g.view.focusRenderW < g.view.focusW) ||
                         (g.view.focusRenderH && g.view.focusRenderH < g.view.focusH);
    if (g.pass < 2 && g.pass != g.eyeOff && (simqvfr::dlss_wanted() || smaller) && g.view.eyeW && g.view.eyeH) {
        // DLAA on the eye (and its focus view beside it) before anything reads the resolved scene. Its draws and copies
        // pass these hooks untouched; single-pass stereo is off meanwhile (the scene's last shader may have left it on).
        simqvfr::DlssPass dp;
        dp.eye = eye;
        dp.eyeWidth = g.view.eyeW;
        dp.eyeHeight = g.view.eyeH;
        dp.focus = g.drawFocus && g.passTwinBinds > 0;
        dp.focusWidth = g.view.focusW;
        dp.focusHeight = g.view.focusH;
        dp.focusRenderWidth = g.view.focusRenderW;
        dp.focusRenderHeight = g.view.focusRenderH;
        std::memcpy(dp.remap, g.view.remap, sizeof(dp.remap));
        dp.depthCopied = g.glass.copied;  // DLSS's depth before the transparents (mvr_pass_glass.h)
        dp.depthCopyRead = g.glass.read;
        if (slotsDrawn && g.slotsLevel >= 2 && g.slotSrv && g.motionSrv) {  // DLSS's vectors take the parts' motion
            dp.objectSlots = g.slotSrv.Get();
            dp.objectMotion = g.motionSrv.Get();
            dp.objectCount = kCarSlots;
        }
        bool stereoOff = false;
        if (g.stereo && !g.instanced) {
            if (g.twoViews(g.context, false) == 0)
                stereoOff = true;
            else
                stereo_off_failed("before DLSS");
        }
        const bool was = g.replaying;
        g.replaying = true;
        simqvfr::dlss_after_resolve(context, dst, scene_depth(drawn), dp);
        g.replaying = was;
        if (stereoOff && g.twoViews(g.context, true) != 0) {
            // Refused back on: off as far as we know; update() tries once more and, refused, hands over as at any
            // refused turn-on (instancing, or the game's shader and one viewport with MVR off for the session).
            g.stereo = false;
            update();
        }
    }
    if (g.keepFocus && g.pass < 2 && g.drawFocus && g.passTwinBinds > 0) {
        g.keptOk[eye] = copy_region(g.kept[eye], dst, g.view.eyeW, 0, g.view.focusW, g.view.focusH);
        g.stats.keptRegions += g.keptOk[eye] ? 1 : 0;
    }
    if (g.focusReplay && g.pass < 2 && g.drawFocus && g.passTwinBinds > 0 && g.view.box.valid && !g.focusPostFailed) {
        if (!g.focusPost.ready()) {
            ComPtr<ID3D11Device> device;
            g.context->GetDevice(&device);
            if (!g.focusPost.initialize(device.Get())) {
                g.focusPostFailed = true;
                simqvfr::log_line("lmu mvr: the focus post couldn't be made (shaders, states, deferred context); off");
            }
        }
        g.postWindow = g.focusPost.ready();
        g.stageImage = nullptr;
        g.stageCovers = false;
    }
    if (pt.recording)
        pt_event("W pass %d: the focus post's window %s (focus view drawn %d, twin binds %llu, box %d)", g.pass,
                 g.postWindow ? "open" : "not opened", g.drawFocus ? 1 : 0,
                 static_cast<unsigned long long>(g.passTwinBinds), g.view.box.valid ? 1 : 0);
}

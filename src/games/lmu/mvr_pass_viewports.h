// Part of mvr_pass.cpp's translation unit (included once, in order): viewport mapping (the focus viewport beside the
// eye's, overlays, scissors, the mip bias constants) and the live resolution's size cut. Included only by mvr_pass.cpp,
// inside its anonymous namespace (lmu::<anonymous>), after mvr_pass_tracker.h; never include it anywhere else: the
// per-draw path stays in one translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// The eye's corner of its final (the overlays' and the focus final's space): the rendered corner stretched by the game's
// final blit when its render scale isn't 1 (MvrPassView::finalEyeW).
unsigned final_eye_w(const MvrPassView& view) { return view.finalEyeW ? view.finalEyeW : view.eyeW; }
unsigned final_eye_h(const MvrPassView& view) { return view.finalEyeH ? view.finalEyeH : view.eyeH; }

// The focus view's viewport beside the eye's `v`: in the scene the eye's rendered size scaled to the focus view's (to
// what it's drawn at before the post, `scene`: smaller under DLSS super resolution); for an overlay into the eye's final
// (`overlay`), the eye's corner of the final to the focus final's (the focus size, 1:1).
D3D11_VIEWPORT focus_viewport_of(const MvrPassView& view, const D3D11_VIEWPORT& v, bool overlay = false, bool scene = false) {
    const unsigned ew = overlay ? final_eye_w(view) : view.eyeW, eh = overlay ? final_eye_h(view) : view.eyeH;
    const unsigned fw = overlay ? (view.finalFocusW ? view.finalFocusW : view.focusW)
                                : scene && view.focusRenderW ? view.focusRenderW : view.focusW;
    const unsigned fh = overlay ? (view.finalFocusH ? view.finalFocusH : view.focusH)
                                : scene && view.focusRenderH ? view.focusRenderH : view.focusH;
    const float sx = ew ? static_cast<float>(fw) / static_cast<float>(ew) : 1.0f;
    const float sy = eh ? static_cast<float>(fh) / static_cast<float>(eh) : 1.0f;
    return {v.TopLeftX + v.Width, v.TopLeftY, v.Width * sx, v.Height * sy, v.MinDepth, v.MaxDepth};
}

// The pass's focus viewport now: the scene's draws until its post begins (the resolve, or YEBIS's copy with MSAA off),
// the post's region after it.
D3D11_VIEWPORT focus_viewport(const D3D11_VIEWPORT& v) {
    return focus_viewport_of(g.view, v, g.overlayTarget, !g.afterResolve);
}

// An overlay's viewport into eye `eye`'s final: the whole target cut to the eye's corner of it (as the driver tags'
// under live resolution: tag_viewport.h), so its second viewport lands on the focus view beside it.
D3D11_VIEWPORT overlay_viewport(int eye, const MvrPassView& view, const D3D11_VIEWPORT& v) {
    TagViewport cut{};
    if (!tag_viewport_cut({v.TopLeftX, v.TopLeftY, v.Width, v.Height}, g.finalW[eye], g.finalH[eye], final_eye_w(view),
                          final_eye_h(view), cut))
        return v;
    return {cut.x, cut.y, cut.width, cut.height, v.MinDepth, v.MaxDepth};
}

// The eye's viewport as applied with two views: the game's, or its cut for an overlay (R3c).
D3D11_VIEWPORT first_viewport() {
    return g.overlayTarget ? overlay_viewport(g.pass & 1, g.view, g.vps[0]) : g.vps[0];
}

// DLAA (engine/dlss_views.h): an overlay's viewport moved back by the clip shift its view's draws carry (a shift of s moves
// the drawing s[0] x width / 2 pixels right and s[1] x height / 2 up), so the overlay lands where the post's image,
// which DLAA left without the jitter, has the world.
D3D11_VIEWPORT unshifted(D3D11_VIEWPORT v, const double (&shift)[2]) {
    v.TopLeftX -= static_cast<float>(shift[0] * v.Width / 2);
    v.TopLeftY += static_cast<float>(shift[1] * v.Height / 2);
    return v;
}

// The game's scissor mapped like its viewport onto the focus viewport.
D3D11_RECT focus_scissor(const D3D11_RECT& r) {
    const D3D11_VIEWPORT v = first_viewport();
    const D3D11_VIEWPORT f = focus_viewport(v);
    const float sx = v.Width > 0 ? f.Width / v.Width : 1.0f, sy = v.Height > 0 ? f.Height / v.Height : 1.0f;
    const auto x = [&](LONG at) { return static_cast<LONG>(std::lround(f.TopLeftX + (static_cast<float>(at) - v.TopLeftX) * sx)); };
    const auto y = [&](LONG at) { return static_cast<LONG>(std::lround(f.TopLeftY + (static_cast<float>(at) - v.TopLeftY) * sy)); };
    return {x(r.left), y(r.top), x(r.right), y(r.bottom)};
}

// Texture sharpening's constants for the pixel shader twins (pixel shader buffer 13): the focus view's viewport as
// apply_viewports gives it (the focus bias inside it, the eye's outside: dxbc::add_focus_mip_bias), uploaded when they
// change. Without two viewports, no rectangle: the eye's bias everywhere.
void update_bias_constants(const D3D11_VIEWPORT* focus) {
    if (!g.bias || !g.biasCb)
        return;
    float c[8] = {0, 0, 0, 0, g.view.mipBias[1], g.view.mipBias[0], 0, 0};
    if (focus) {
        c[0] = focus->TopLeftX;
        c[1] = focus->TopLeftY;
        c[2] = focus->TopLeftX + focus->Width;
        c[3] = focus->TopLeftY + focus->Height;
    }
    if (std::memcmp(c, g.biasConstants, sizeof(c)) != 0) {
        std::memcpy(g.biasConstants, c, sizeof(c));
        g.context->UpdateSubresource(g.biasCb.Get(), 0, nullptr, c, 0, 0);
    }
}

// ---- Live resolution (mvr_size_cut; docs/recon-lmu.md "Census run 3"): the live resolution without writing into the game.
// The game renders at its own size; in a tracked eye pass before its post every viewport and scissor that is the game's
// render size (0,0 full W x H) is cut to the live eye's (0,0 eye W x H), as the game's own render size would have made
// them. Not cut: a draw into a full-size 32-bit single-channel target (the post's depth copy, which today covers the whole
// target: the focus view beside the eye needs its depth too).
// (SizeCut and sizeCut: mvr_pass_internal.h.)

// The game's rects stored as it set them: its viewports (its RSSetViewports), its scissors (RSSetScissorRects), both as
// the pass begins (read from the context, which holds them as the game set them outside the passes).
void keep_game_vps() {
    sizeCut.vpCount = g.vpCount;
    std::memcpy(sizeCut.vps, g.vps, g.vpCount * sizeof(D3D11_VIEWPORT));
}
void keep_game_scs() {
    sizeCut.scCount = g.scCount;
    std::memcpy(sizeCut.scs, g.scs, g.scCount * sizeof(D3D11_RECT));
}

bool cut_active() { return sizeCut.on && g.pass >= 0 && g.pass < 2 && !g.afterResolve && !sizeCut.endedThisPass; }

bool is_full(float x, float y, float w, float h) {
    return std::fabs(x) < 0.5f && std::fabs(y) < 0.5f && std::fabs(w - static_cast<float>(sizeCut.fullW)) < 0.5f &&
           std::fabs(h - static_cast<float>(sizeCut.fullH)) < 0.5f;
}

// The bound render target 0 is one the cut leaves alone (a full-size R32 target: the post's depth copy).
bool cut_excluded() {
    ComPtr<ID3D11RenderTargetView> rtv;
    g.context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> r;
    ComPtr<ID3D11Texture2D> t;
    if (!rtv)
        return false;
    rtv->GetResource(&r);
    if (!r || FAILED(r.As(&t)))
        return false;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    const bool r32 = d.Format >= DXGI_FORMAT_R32_TYPELESS && d.Format <= DXGI_FORMAT_R32_SINT;
    const bool excluded = r32 && d.Width == sizeCut.fullW && d.Height == sizeCut.fullH;
    if (excluded)
        sizeCut.endedThisPass = true;  // the post's depth copy: the cut ends here for this pass
    if (excluded && !sizeCut.leftLogged) {
        sizeCut.leftLogged = true;
        simqvfr::logf("lmu mvr: live resolution: pass %d: a full-size viewport into %ux%u f%d (the post's depth copy) left "
                      "as the game set it (counted from now on)",
                      g.pass, d.Width, d.Height, static_cast<int>(d.Format));
    }
    return excluded;
}

// g.vps / g.scs = the game's own rects, cut where they are its render size and the bound target isn't excluded. True:
// they differ from what they were (to be applied again).
bool cut_rects() {
    if (!cut_active())
        return false;
    // From the game's own rects (as it set them), so that a bind of an excluded target gives its full rects back.
    D3D11_VIEWPORT before[kMaxRects];
    D3D11_RECT beforeSc[kMaxRects];
    const UINT vpBefore = g.vpCount, scBefore = g.scCount;
    std::memcpy(before, g.vps, sizeof(before));
    std::memcpy(beforeSc, g.scs, sizeof(beforeSc));
    g.vpCount = sizeCut.vpCount;
    std::memcpy(g.vps, sizeCut.vps, g.vpCount * sizeof(D3D11_VIEWPORT));
    g.scCount = sizeCut.scCount;
    std::memcpy(g.scs, sizeCut.scs, g.scCount * sizeof(D3D11_RECT));
    bool any = false, checked = false, excluded = false;
    const auto skip = [&] {
        if (!checked) {
            checked = true;
            excluded = cut_excluded();
        }
        return excluded;
    };
    for (UINT i = 0; i < g.vpCount; ++i) {
        D3D11_VIEWPORT& v = g.vps[i];
        if (!is_full(v.TopLeftX, v.TopLeftY, v.Width, v.Height)) {
            if (std::fabs(v.TopLeftX) < 0.5f && std::fabs(v.TopLeftY) < 0.5f && v.Width > sizeCut.eyeW + 1.0f &&
                v.Height > sizeCut.eyeH + 1.0f && sizeCut.otherCount < static_cast<int>(std::size(sizeCut.otherSizes))) {
                bool seen = false;
                for (int k = 0; k < sizeCut.otherCount; ++k)
                    seen = seen || (sizeCut.otherSizes[k][0] == v.Width && sizeCut.otherSizes[k][1] == v.Height);
                if (!seen) {
                    sizeCut.otherSizes[sizeCut.otherCount][0] = v.Width;
                    sizeCut.otherSizes[sizeCut.otherCount][1] = v.Height;
                    ++sizeCut.otherCount;
                    ComPtr<ID3D11RenderTargetView> rtv;
                    g.context->OMGetRenderTargets(1, &rtv, nullptr);
                    ComPtr<ID3D11Resource> r;
                    if (rtv)
                        rtv->GetResource(&r);
                    simqvfr::logf("lmu mvr: live resolution: pass %d: a %.0fx%.0f viewport, neither the game's render "
                                  "size %ux%u nor the eye's %ux%u: left as it is (into %s)",
                                  g.pass, v.Width, v.Height, sizeCut.fullW, sizeCut.fullH, sizeCut.eyeW, sizeCut.eyeH,
                                  describe(r.Get()).c_str());
                }
            }
            continue;
        }
        if (skip()) {
            ++sizeCut.left;
            continue;
        }
        v.Width = static_cast<float>(sizeCut.eyeW);
        v.Height = static_cast<float>(sizeCut.eyeH);
        ++sizeCut.viewports;
        any = true;
    }
    for (UINT i = 0; i < g.scCount; ++i) {
        D3D11_RECT& r = g.scs[i];
        if (!is_full(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                     static_cast<float>(r.bottom - r.top)))
            continue;
        if (skip())
            continue;
        r.right = static_cast<LONG>(sizeCut.eyeW);
        r.bottom = static_cast<LONG>(sizeCut.eyeH);
        ++sizeCut.scissors;
        any = true;
    }
    const bool changed = g.vpCount != vpBefore || g.scCount != scBefore ||
                         std::memcmp(before, g.vps, g.vpCount * sizeof(D3D11_VIEWPORT)) != 0 ||
                         std::memcmp(beforeSc, g.scs, g.scCount * sizeof(D3D11_RECT)) != 0;
    if (any && (sizeCut.cutLoggedW != sizeCut.fullW || sizeCut.cutLoggedH != sizeCut.fullH)) {
        sizeCut.cutLoggedW = sizeCut.fullW;
        sizeCut.cutLoggedH = sizeCut.fullH;
        simqvfr::logf("lmu mvr: live resolution: pass %d: the game's %ux%u viewports and scissors cut to the eye's %ux%u (the "
                      "first at this render size; counted from now on)",
                      g.pass, sizeCut.fullW, sizeCut.fullH, sizeCut.eyeW, sizeCut.eyeH);
    }
    return changed;
}

void apply_viewports() {
    if (g.on && g.vpCount == 1) {
        const D3D11_VIEWPORT first = first_viewport();
        D3D11_VIEWPORT two[2] = {first, focus_viewport(first)};
        update_bias_constants(&two[1]);
        if (g.overlayTarget) {  // after the post: drawn moved back by the jitter DLAA took out
            two[0] = unshifted(two[0], g.view.overlayShift[0]);
            two[1] = unshifted(two[1], g.view.overlayShift[1]);
        }
        realRSSetViewports(g.context, 2, two);
        std::memcpy(g.appliedVps, two, sizeof(two));
        g.twoVpsApplied = true;
        ++g.stats.viewportRewrites;
    } else {
        if (g.on)
            update_bias_constants(nullptr);
        realRSSetViewports(g.context, g.vpCount, g.vps);
        g.twoVpsApplied = false;
    }
}

void apply_scissors() {
    if (g.on && g.scCount == 1 && g.vpCount == 1) {
        const D3D11_RECT two[2] = {g.scs[0], focus_scissor(g.scs[0])};
        realRSSetScissorRects(g.context, 2, two);
        std::memcpy(g.appliedScs, two, sizeof(two));
        g.twoScsApplied = true;
        ++g.stats.scissorRewrites;
    } else if (g.scCount) {
        realRSSetScissorRects(g.context, g.scCount, g.scs);
        g.twoScsApplied = false;
    }
}

// Live resolution: the post-processing begins (the multisample resolve; MSAA off, the draw that starts it): from here every
// draw is at its target's size, as with the writes, so the game's own rects are given back (its first stage draws with
// the full viewport it set before binding its target, over the focus view beside the eye).
void rects_uncut() {
    if (!sizeCut.on || g.pass < 0 || g.pass > 1)
        return;
    const bool vpSame = g.vpCount == sizeCut.vpCount &&
                        std::memcmp(g.vps, sizeCut.vps, g.vpCount * sizeof(D3D11_VIEWPORT)) == 0;
    const bool scSame = g.scCount == sizeCut.scCount &&
                        std::memcmp(g.scs, sizeCut.scs, g.scCount * sizeof(D3D11_RECT)) == 0;
    if (vpSame && scSame)
        return;
    g.vpCount = sizeCut.vpCount;
    std::memcpy(g.vps, sizeCut.vps, g.vpCount * sizeof(D3D11_VIEWPORT));
    g.scCount = sizeCut.scCount;
    std::memcpy(g.scs, sizeCut.scs, g.scCount * sizeof(D3D11_RECT));
    apply_viewports();
    if (g.scCount)
        apply_scissors();
}

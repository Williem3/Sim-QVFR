// Part of mvr_pass.cpp's translation unit (included once, in order): the two views switched (update(), single-pass
// stereo refused or stuck) and the VS/GS/HS/DS/SO set hooks. Included only by mvr_pass.cpp, inside its anonymous
// namespace (lmu::<anonymous>), after mvr_pass_cars.h; never include it anywhere else: the per-draw path stays in one
// translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// Single-pass stereo refused: instancing for the rest of the session if its twins are there (the next pass's start
// hooks the draws it needs), else no focus views from the eye passes. True: instancing took over.
bool take_over_with_instancing() {
    if (!g.canInstance || g.instanced) {
        g.broken = true;
        simqvfr::log_line("lmu mvr: NVIDIA's multi-view rendering refused: only passes 2-3 draw the focus views");
        return false;
    }
    g.instanced = true;
    ++g.stats.instancedTakeovers;
    simqvfr::log_line("lmu mvr: NVIDIA's multi-view rendering refused: instancing draws the focus views from now on "
                      "(each draw twice, the vertex shader picking the viewport)");
    return true;
}

// Single-pass stereo wouldn't go off: the driver's state is unknown, so MVR stops for the session (mvr_broken: the
// frame end's fallback, the eyes alone). Logged once.
void stereo_off_failed(const char* where) {
    ++g.stats.stereoErrors;
    g.broken = true;
    if (!g.stereoOffLogged) {
        g.stereoOffLogged = true;
        simqvfr::logf("lmu mvr: NVIDIA's multi-view rendering couldn't be turned off (%s): off for the session", where);
    }
}

// Twin, two views and the second viewport exactly while a focus-drawing pass draws with a depth target (or an overlay
// into the eye's final) with one viewport and a shader with a twin.
void update() {
    ID3D11VertexShader* twin = nullptr;
    // The pass's draws with a depth target bound (any of them: the car shadows' own depth target is not the scene's,
    // 2026-11-15) and, after the focus view's final blit, the overlays into the eye's final. One viewport only: the
    // focus one is appended to it (apply_viewports); with more the twin would draw into the game's own second one.
    const bool can = g.drawFocus && !g.broken && (g.depthBound || g.overlayTarget) && g.vpCount == 1 && g.gameVs &&
                     !g.gsBound && !g.hsBound && !g.dsBound && !g.soBound;
    if (g.drawFocus && g.depthBound && g.vpCount != 1 && !g.manyViewportsLogged) {
        g.manyViewportsLogged = true;
        simqvfr::logf("lmu mvr: pass %d: a depth-bound draw with %u viewports: into the eye alone (logged once)", g.pass,
                      g.vpCount);
    }
    bool want = can;
    if (want) {
        twin = game_twin();
        want = twin != nullptr;
    }
    if (want != g.stereo && g.instanced) {  // no mode to switch: the draws are doubled while the twin is bound
        g.stereo = want;
        ++g.stats.toggles;
    } else if (want != g.stereo) {
        Timed timed(g.time.switches);
        if (g.twoViews(g.context, want) != 0) {
            if (want) {
                ++g.stats.stereoErrors;
                want = false;
                twin = nullptr;
                if (take_over_with_instancing() && can) {
                    twin = game_twin();
                    want = twin != nullptr;
                    g.stereo = want;
                }
            } else {
                stereo_off_failed("as a draw's two views ended");
            }
        } else {
            g.stereo = want;
            ++g.stats.toggles;
        }
    }
    ID3D11VertexShader* bind = want ? twin : g.gameVs;
    if (bind != g.boundVs) {
        realVSSetShader(g.context, bind, g.instanceCount ? g.instances : nullptr, g.instanceCount);
        g.boundVs = bind;
        ++(want ? g.stats.twinBinds : g.stats.plainBinds);
        g.passTwinBinds += want ? 1 : 0;
    }
    if (want != g.on) {
        g.on = want;
        if (want) {
            realVSSetConstantBuffers(g.context, 13, 1, g.remapCb.GetAddressOf());
            g.remapBound = true;  // (the game's b13 back at the pass end)
            g.biasCbBound = false;  // texture sharpening's buffer bound again with the next twin (the game unbinds it)
        }
        apply_viewports();
        apply_scissors();
        update_ps();  // texture sharpening: the twin exactly while two views are on
    }
}

void STDMETHODCALLTYPE vs_set_shader(ID3D11DeviceContext* context, ID3D11VertexShader* vs,
                                     ID3D11ClassInstance* const* instances, UINT count) {
    // The twin bound in the game's shader's place given back (a VSGetShader/VSSetShader round trip): the game's own.
    if (vs && vs == g.boundVs && vs != g.gameVs && context == g.context && !g.replaying && g.pass >= 0)
        vs = g.gameVs;
    if (context == g.context && !g.replaying) {  // every game bind, so a draw anywhere knows its shader's role (R6)
        const TwinEntry* e = vs ? &shader_entry(vs) : nullptr;
        g.vsRole = e ? e->role : VsRole::none;
        g.vsWorldSlot = e ? e->worldSlot : -1;  // car tracking: where it reads its worldMatrix
        g.vsWorldOffset = e ? e->worldOffset : 0;
        g.vsViewOffset = e ? e->viewOffset : ~0u;  // view tracking: its view matrices there
        g.vsViewProjOffset = e ? e->viewProjOffset : ~0u;
        g.vsOmniSlot = e ? e->omniSlot : -1;  // night car lighting: where it reads its floodlights
        g.vsOmniPos = e ? e->omniPos : 0;
        g.vsOmniColour = e ? e->omniColour : 0;
        g.vsOmniCount = e ? e->omniCount : 0;
        g.vsGameNormal = e && e->gameNormal;
        keep_game_entry(vs, e);
    }
    if (!ours(context)) {
        realVSSetShader(context, vs, instances, count);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    hold(g.gameVs, vs);
    hold_instances(g.instances, g.instanceCount, instances, std::min<UINT>(count, 256));
    g.boundVs = reinterpret_cast<ID3D11VertexShader*>(~uintptr_t{0});  // the game's call always takes effect
    update();
}

template <int H, typename Shader, bool State::*bound>
void STDMETHODCALLTYPE later_stage(ID3D11DeviceContext* context, Shader* shader, ID3D11ClassInstance* const* instances,
                                   UINT count) {
    if (!ours(context)) {
        call_real<H>(context, shader, instances, count);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    call_real<H>(context, shader, instances, count);
    g.*bound = shader != nullptr;
    update();
}

void STDMETHODCALLTYPE so_set_targets(ID3D11DeviceContext* context, UINT n, ID3D11Buffer* const* buffers,
                                      const UINT* offsets) {
    if (!ours(context)) {
        realSOSetTargets(context, n, buffers, offsets);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realSOSetTargets(context, n, buffers, offsets);
    g.soBound = false;
    for (UINT i = 0; i < n && buffers; ++i)
        g.soBound = g.soBound || buffers[i] != nullptr;
    update();
}

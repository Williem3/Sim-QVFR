// Part of mvr_pass.cpp's translation unit (included once, in order): the draw hook entry points (DrawIndexed, the other
// draws, the undoubled draws) and ExecuteCommandList. Included only by mvr_pass.cpp, inside its anonymous namespace
// (lmu::<anonymous>), after mvr_pass_post_roles.h; never include it anywhere else: the per-draw path stays in one
// translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// ---- Post census (mvr_post_census; diagnostics, log only): mvr_pass_census.cpp; census_here and its state in
// mvr_pass_internal.h ----

void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    if (g.slotsBound && g.vsRole == VsRole::lensMask && ours(context))
        bind_slot_view(-1);  // car slots: the lens mask is no car part (it may return before the parts' step below)
    if (context == g.context && !g.replaying) {
        ++tr.draws;
        if (g.vsRole == VsRole::lensMask && lens_mask_draw(context, "DrawIndexed", count, start))
            return;
        if (g.mask.on && g.drawFocus)
            focus_mask_before_draw(context);
        Timed timed(g.time.tracker);
        if (tr.on && tr.stageCandidate)
            learn_at_composite(context);
        if (tr.on && g.copyUnknown && !tr.open && !tr.msaaFrame)
            learn_by_signature(context);
        if (tr.offScene && ours(context) && g.pass < 2)
            stage_before(context);
        if (tr.on)
            copy_before(context);
        if (tr.open && tr.resolved && !tr.postSeen)
            note_post_seen(context);
    }
    const bool outside = context == g.context && !g.replaying && g.pass < 0 && (g.finalThisFrame[0] || g.finalThisFrame[1]);
    if (g.carTrack && g.carActive && ours(context)) {
        int slot = -1;
        {
            SampledTimed timed(g.time.carDraws, carSampleDraw);
            slot = car_draw(count, start, base);
        }
        if (g.slotsBound)
            bind_slot_view(slot);
    }
    if (census_here(context))  // the state as the game set it, before the draw
        census_any(context, "DrawIndexed", count, start);
    const bool censusDraw = census_here(context);
    if (!outside || !double_outside<kHookDrawIndexed>(context, count, start, base))
        game_draw<kHookDrawIndexed>(context, count, start, base);
    if (censusDraw)
        census_after(context);
    if (g.tracing && context == g.context && !g.replaying)
        trace_draw(context, "DrawIndexed", count, start);
    if (pt.recording && context == g.context && !g.replaying)
        pt_draw(context, "DrawIndexed");
    if (ours(context) && g.overlayTarget && g.on)
        ++g.stats.overlayDraws;
    if (tr.on && g.afterResolve && g.observeShape && ours(context) && g.pass < 2) {
        Timed timed(g.time.tracker);
        observe_post_draw(context, false);
    }
    if (!g.postWindow || !ours(context))
        return;
    Timed timed(g.time.hooks);
    if (!g.focusCopied)
        note_post_stage(context);
    focus_post_draw(context);
}

// Draw and the instanced draws: the real call (an overlay after the passes doubled: double_outside), the trace's line;
// Draw also the final blit's replay (focus_blit).
template <int H, typename... A>
void STDMETHODCALLTYPE draw_traced(ID3D11DeviceContext* context, A... a) {
    if (g.slotsBound && ours(context))
        bind_slot_view(-1);  // car slots: these draws are no car part's
    if (context == g.context && !g.replaying) {
        ++tr.draws;
        const long long args[] = {static_cast<long long>(a)...};
        if (g.vsRole == VsRole::lensMask && lens_mask_draw(context, kHookName[H], args[0], args[1]))
            return;
        if (g.mask.on && g.drawFocus)
            focus_mask_before_draw(context);
        Timed timed(g.time.tracker);
        if (tr.on && tr.stageCandidate)
            learn_at_composite(context);
        if (tr.offScene && ours(context) && g.pass < 2)
            stage_before(context);
        if (tr.open && tr.resolved && !tr.postSeen)
            note_post_seen(context);
    }
    const bool outside = context == g.context && !g.replaying && g.pass < 0 && (g.finalThisFrame[0] || g.finalThisFrame[1]);
    bool panel = false;
    if constexpr (H == kHookDraw) {  // R6b: the menus' panel (Draw: vertex count, start)
        panel = panel_draw_here(context);
        if (panel) {
            const long long args[] = {static_cast<long long>(a)...};
            call_panel_fn(context, false, static_cast<unsigned>(args[0]), static_cast<unsigned>(args[1]));
        }
    }
    if (census_here(context)) {
        const long long args[] = {static_cast<long long>(a)...};
        census_any(context, kHookName[H], args[0], args[1]);
    }
    const bool censusDraw = census_here(context);
    if (!outside || !double_outside<H>(context, a...))
        game_draw<H>(context, a...);
    if (censusDraw)
        census_after(context);
    if constexpr (H == kHookDraw) {
        if (panel) {
            const long long args[] = {static_cast<long long>(a)...};
            call_panel_fn(context, true, static_cast<unsigned>(args[0]), static_cast<unsigned>(args[1]));
        }
    }
    if (g.tracing && context == g.context && !g.replaying) {
        const long long args[] = {static_cast<long long>(a)...};
        trace_draw(context, kHookName[H], args[0], args[1]);
    }
    if (pt.recording && context == g.context && !g.replaying)
        pt_draw(context, kHookName[H]);
    if (ours(context) && g.overlayTarget && g.on)
        ++g.stats.overlayDraws;
    if (tr.on && g.afterResolve && g.observeShape && ours(context) && g.pass < 2) {
        Timed timed(g.time.tracker);
        observe_post_draw(context, H == kHookDraw);
    }
    if (g.postWindow && !g.focusCopied && ours(context)) {  // a stage between the resolve and YEBIS may draw this way
        Timed timed(g.time.hooks);
        note_post_stage(context);
    }
    if constexpr (H == kHookDraw) {
        if (ours(context) && tr.open && tr.singleSample && !tr.blitSeen)  // MSAA off: the pass's final blit
            note_final_blit(context);
        if (ours(context) && g.compositeOk && !g.focusBlitted && g.pass < 2 && g.drawFocus) {
            Timed timed(g.time.hooks);
            const long long args[] = {static_cast<long long>(a)...};
            focus_blit(context, static_cast<UINT>(args[0]), static_cast<UINT>(args[1]));
        }
    }
}

// DrawAuto and the indirect draws with instancing: no instance count to double (it's in a buffer, or there is none), so
// while two views are on the game's own shader draws it, into the eye's viewport alone; logged once.
template <int H, typename... A>
void STDMETHODCALLTYPE undoubled_draw(ID3D11DeviceContext* context, A... a) {
    if (context == g.context && !g.replaying)
        ++tr.draws;
    if (!(g.instanced && g.on && ours(context))) {
        call_real<H>(context, a...);
        return;
    }
    ++g.stats.eyeOnlyDraws;
    if (!g.undoubledLogged) {
        g.undoubledLogged = true;
        simqvfr::logf("lmu mvr: pass %d: a %s with two views on: drawn into the eye alone (instancing can't double it; "
                      "counted from now on)",
                      g.pass, kHookName[H]);
    }
    realVSSetShader(context, g.gameVs, g.instanceCount ? g.instances : nullptr, g.instanceCount);
    call_real<H>(context, a...);
    realVSSetShader(context, g.boundVs, g.instanceCount ? g.instances : nullptr, g.instanceCount);
}

void read_bindings();

// A command list of the game's (its sun shadow casters, docs/recon-lmu.md "MVR shadow complaint"): executed as recorded.
// Inside a pass the two views may be on (pass 0 begins before its Pre Render with the loop's passes) and d3d11 may replay
// the list through the context's own functions, which would make its casters' binds look like the scene's to the hooks
// (the twin, a second viewport beside the atlas tile). So: the two views off, the replay passed on untouched, and the
// state read again after it (RestoreContextState FALSE leaves the context cleared, TRUE as it was: the views back on).
void STDMETHODCALLTYPE execute_command_list(ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore) {
    if (context != g.context || g.replaying) {  // ours (the focus post's) or another context's
        call_real<kHookExecute>(context, list, restore);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    const bool inPass = g.pass >= 0;
    ++(inPass ? g.stats.listsInPass : g.stats.listsOutside);
    g.stats.listsTwoViews += g.stereo ? 1 : 0;
    const bool plain = inPass && !g.listsAsBefore.load(std::memory_order_relaxed);
    if (plain && g.stereo && !g.instanced) {
        Timed switching(g.time.switches);
        if (g.twoViews(g.context, false) == 0) {
            g.stereo = false;
            ++g.stats.toggles;
        } else {
            stereo_off_failed("before the game's command list");
        }
    }
    g.inList = true;
    g.replaying = plain;
    call_real<kHookExecute>(context, list, restore);
    g.replaying = false;
    g.inList = false;
    if (!plain)
        return;
    if (!restore) {  // the context's state cleared to its defaults: nothing of ours (twin, cb13, two viewports) bound
        // Nothing of ours bound now (slot target and view, blend twin, the b13 buffers): the flags as the context is,
        // the game's b13 none.
        g.slotsBound = g.blendSwapped = g.biasCbBound = g.biasCbUsed = g.remapBound = false;
        g.slotViewBound = nullptr;
        g.twoVpsApplied = g.twoScsApplied = false;
        hold(g.gameVsCb13, static_cast<ID3D11Buffer*>(nullptr));
        hold(g.gamePsCb13, static_cast<ID3D11Buffer*>(nullptr));
        read_bindings();
        keep_game_vps();  // live resolution: the game's own rects are the cleared ones now
        keep_game_scs();
        g.on = false;
    }
    update();
}

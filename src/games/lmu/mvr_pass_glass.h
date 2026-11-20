// Part of mvr_pass.cpp's translation unit (included once, in order): DLSS's depth before the transparents that write
// depth (mvr_glass_depth; the car's glass). Included only by mvr_pass.cpp, inside its anonymous namespace
// (lmu::<anonymous>), after mvr_pass_focus_mask.h; never include it anywhere else: the per-draw path stays in one
// translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// At most this many copies an eye pass (each a full-view draw; LMU has made one).
constexpr int kGlassCopies = 4;

// Before a draw of the game's in an eye pass with the scene's depth bound and written (the caller checked): an opaque
// draw notes that the depth moved on; the first transparent (blended) one after opaque depth, the depth copied as it
// stands for DLSS (dlss_depth_copy_list), so its vectors see what lies behind the glass. A pass may copy a few times
// (transparents, then opaque depth again, then transparents); each copy holds all the opaque depth before it.
void glass_before_draw(ID3D11DeviceContext* context) {
    auto& gl = g.glass;
    if (!gl.blends) {
        gl.opaqueSince = true;
        if (gl.copies)
            ++gl.opaqueAfterCopy;
        return;
    }
    ++g.stats.glassBlendedWriters;
    // The focus corridor's copy of the eye's opaque depth and its road (once a pass); none on the A/B's side without the
    // corridor (its whole cost left out), but for show mode, which never masks.
    if (g.corridor.on && (!g.corridor.abOff || g.corridor.show) && gl.opaqueSince && gl.dsv)
        corridor_copy(context, gl.dsv);
    if (g.pass == 0 && gl.opaqueSince && gl.dsv && !gl.copies && g.view.box.valid) {
        // The focus boxes' convergence reads the depth here too, before the glass (fixation.h), DLSS on or off.
        const bool was = g.replaying;
        g.replaying = true;
        fixation_pass(gl.dsv, g.view.box, g.view.eyeW, g.view.eyeH, false);
        g.replaying = was;
    }
    if (!gl.opaqueSince || !gl.dsv || !simqvfr::dlss_wanted())
        return;  // the same run of transparents (or nothing to copy for)
    gl.opaqueSince = false;
    if (gl.copies >= kGlassCopies) {
        ++g.stats.glassCapped;
        return;
    }
    const MvrPassView& v = g.view;
    const unsigned fw = g.drawFocus ? (v.focusRenderW ? v.focusRenderW : v.focusW) : 0;
    const unsigned fh = g.drawFocus ? (v.focusRenderH ? v.focusRenderH : v.focusH) : 0;
    ID3D11CommandList* list = simqvfr::dlss_depth_copy_list(gl.dsv, v.eyeW + fw, (std::max)(v.eyeH, fh));
    if (!list) {
        ++g.stats.glassRefused;
        return;
    }
    // NVIDIA's single-pass stereo off around it, as around the focus mask's list (instancing: its own shader binds).
    const bool stereo = g.stereo && !g.instanced;
    if (stereo) {
        Timed switching(g.time.switches);
        if (g.twoViews(g.context, false) != 0) {
            stereo_off_failed("before the depth's copy");
            return;
        }
    }
    const bool was = g.replaying;
    g.replaying = true;
    call_real<kHookExecute>(context, list, TRUE);
    g.replaying = was;
    if (stereo) {
        Timed switching(g.time.switches);
        if (g.twoViews(g.context, true) != 0) {
            ++g.stats.stereoErrors;
            g.stereo = false;
            update();
        }
    }
    if (!gl.copies)
        g.stats.glassFirstAt += tr.draws - tr.actualBegin[g.pass];
    ++gl.copies;
    ++g.stats.glassCopies;
    gl.opaqueAfterCopy = 0;
    gl.copied = true;
}

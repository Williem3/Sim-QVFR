#include "adapter.h"

#include <windows.h>

#include <algorithm>

#include "../../log.h"
#include "../diag_switches.h"
#include "../identity_adapter.h"
#include "../multi_view.h"
#include "../../engine/dlss_views.h"
#include "dlss_inputs.h"
#include "four_passes.h"
#include "game.h"
#include "glare_share.h"
#include "mvr_probe.h"
#include "view_probe.h"

namespace lmu {
namespace {

// QVFR in LMU: on unless [experiment] lmu_qvfr=0 in the launch's ini (SIM_QVFR_INI, which the launcher sets; an ini from
// before LMU has no key), with mode=views4. lmu_qvfr=0: identity only, the layer logging, with the gates and probes below.
bool qvfr_wanted() {
    static const bool wanted = [] {
        wchar_t ini[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_INI", ini, MAX_PATH);
        return n > 0 && n < MAX_PATH && GetPrivateProfileIntW(L"experiment", L"lmu_qvfr", 1, ini) != 0;
    }();
    return wanted;
}

// Identity only with lmu_qvfr=0: the layer logs, QVFR doesn't start. Its experiments run only when their
// [experiment] key asks, and not in a launch the anti-cheat protects unless the user chose it ([launcher] anticheat=1;
// otherwise adapter_for hands out the plain identity adapter there): the view probe (log only), the copies and focus
// gates (four passes, docs/recon-lmu.md), and QVFR.
class LmuAdapter final : public simqvfr::games::IdentityAdapter {
public:
    LmuAdapter() : IdentityAdapter(&kGame) {}

    // ---- QVFR (lmu_qvfr=1): the eye loop four times, the focus views composed from an atlas by the D3D11 compositor ----
    bool has_qvfr() const override { return analysis() && qvfr_wanted(); }
    bool qvfr_composable() const override { return false; }     // not by the D3D12 compositor:
    bool qvfr_compose_d3d11() const override { return true; }   // LMU is D3D11
    bool qvfr_separate_views() const override { return true; }  // every view an image of its own, one size
    // LMU uses no OpenXR input (no attach in a whole session, 2026-11-07 runs): the gaze set is attached alone.
    bool attaches_action_sets() const override { return false; }
    bool qvfr_start(const simqvfr::games::QvfrStart& s) override {
        if (!has_qvfr())
            return false;
        FourPassesRun run;
        run.qvfr = true;
        run.fraction = s.focusFraction > 0 ? s.focusFraction : 0.3;
        run.fractionY = s.focusFractionY;
        run.holdExposure = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_copies_hold_exposure", 1, s.ini) != 0;
        // Menus (frames without the eye loop) at the native eye, the UI drawn at that size (four_passes.h).
        run.menuNative = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_menu_native", 1, s.ini) != 0;
        run.layerWidth = s.layout.layer_width();
        run.layerHeight = s.layout.layer_height();
        // Multi-view rendering (mvr_pass.h): passes 0-1 draw their focus views too, beside their eyes, where the layer
        // made room (Views4Layout::secondViewRoom). The launcher's multi_view=1 (games/multi_view.h): the game's own two
        // eye passes make the focus views (focus post, focus finals, the d3d11 pass tracker), falling back to the four
        // passes if it can't (four_passes.h); [experiment] lmu_mvr_legacy=1: the four passes with MVR (gate D) as
        // before. Or lmu_mvr, its gates: 1 (C: passes 2-3 still render theirs, compared at the samples), 2 (D: they skip
        // the scene); with those and lmu_mvr_legacy the steps' own keys (lmu_focus_replay and on) apply.
        const UINT gate = simqvfr::games::lmu_mvr_gate(s.ini);
        const bool legacy = s.ini && GetPrivateProfileIntW(L"experiment", L"lmu_mvr_legacy", 0, s.ini) != 0;
        const UINT mvrLevel = gate ? gate : 2;
        run.mvr = s.layout.secondViewRoom;
        run.mvrSkip = run.mvr && mvrLevel >= 2;  // passes 2-3 skip the scene
        run.perAxis = run.mvr && s.layout.secondViewBeside;  // the images the outer views' shape, the focus views beside
        if (run.mvr) {
            run.mvrCompare = gate != 0;  // the gates' samples (images read back and saved): not in a player's run
            run.first = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_first", 300, s.ini));
            run.every = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_every", 300, s.ini));
            run.count = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_samples", 3, s.ini));
            // lmu_mvr_ab=N: N frames with MVR, N without, in turn, each side's cost logged (four_passes.h).
            run.mvrAb = (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mvr_ab", 0, s.ini)));
            // lmu_mvr_timing=1 (and any A/B run): the CPU in the context hooks, logged.
            run.mvrTiming = run.mvrAb > 0 || GetPrivateProfileIntW(L"experiment", L"lmu_mvr_timing", 0, s.ini) != 0;
            const bool player = gate == 0 && !legacy;  // multi_view=1: the two-pass path
            // lmu_focus_replay=1: the eye passes post-process their focus views too (focus_post.h).
            run.focusReplay = player || (s.ini && GetPrivateProfileIntW(L"experiment", L"lmu_focus_replay", 0, s.ini) != 0);
            // lmu_ui_mips=0: the VR UI panel samples the game's canvas (one mip) in the small periphery too.
            run.uiMips = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_ui_mips", 1, s.ini) != 0;
            // lmu_focus_final=1 (with it): two eye passes, the focus views from beside the eyes (four_passes.h).
            run.focusFinal = run.focusReplay && run.mvrSkip &&
                             (player || GetPrivateProfileIntW(L"experiment", L"lmu_focus_final", 0, s.ini) != 0);
            if (run.focusFinal) {
                run.focusFinalAb =
                    (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_focus_final_ab", 0, s.ini)));
                run.mvrAb = 0;  // one A/B at a time
                run.mvrTiming = run.mvrTiming || run.focusFinalAb > 0;
                run.focusFinalAbPlain =
                    run.focusFinalAb > 0 && GetPrivateProfileIntW(L"experiment", L"lmu_focus_final_ab_plain", 0, s.ini) != 0;
            }
            // lmu_pass_tracker=1 (with the focus post): R4b's d3d11 pass bounds, observe only (four_passes.h).
            const UINT tracker =
                player ? 2 : run.focusReplay ? GetPrivateProfileIntW(L"experiment", L"lmu_pass_tracker", 0, s.ini) : 0;
            run.passTracker = tracker != 0;
            run.trackerDrives = tracker >= 2 && run.focusFinal;  // =2: it drives the passes, the game hooks off (R4b-2)
            if (run.trackerDrives) {
                run.focusFinalAb = 0;  // the game's own two passes: nothing to alternate with
                run.mvrAb = 0;
                // lmu_mvr_backend_ab=N: N frames NVIDIA's single-pass stereo, N instancing, in turn (four_passes.h).
                run.backendAb = !s.ini ? 0 : (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mvr_backend_ab", 0,
                                                                                      s.ini)));
                run.mvrTiming = run.mvrTiming || run.backendAb > 0;
                // lmu_mvr_fallback_at=N: the fallback to the four passes forced at scene frame N (its test).
                run.fallbackAt = !s.ini ? 0 : (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mvr_fallback_at", 0,
                                                                                      s.ini)));
            }
        }
        // The focus views take their eye's glare (glare_share.h), so a light's halo doesn't stop at the box edge.
        run.glareShare = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_glare_share", 1, s.ini) != 0;
        if (s.ini)  // diagnostic: the render scale changed live for 120 frames (four_passes.h)
            run.renderScaleProbe =
                std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_render_scale_probe", 0, s.ini)), 0, 100);
        // Diagnostics (lmu_diag=1; games/diag_switches.h): Ctrl+Alt+1 the glare share (an A/B in one run).
        if (diag_.start(s.ini, L"lmu", {L"glare_share"}))
            simqvfr::logf("lmu: diagnostics on: Ctrl+Alt+1 switches the glare share (lmu_diag_glare_share_off=1 in %ls "
                          "turns it off)",
                          diag_.ini().c_str());
        glareAtLaunch_ = run.glareShare;
        // DLAA through NGX on the views ([experiment] lmu_dlss=1 the eyes, 2 the eyes and the focus views; engine/dlss_views.h):
        // with the multi-view path only (its eye passes found at the d3d11 level). lmu_dlss_folder: where nvngx_dlss.dll
        // is; lmu_dlss_preset: NGX's preset number; lmu_dlss_jitter=0, lmu_dlss_vectors=0: without that input (a
        // comparison); lmu_dlss_view_inverse, lmu_dlss_jitter_flip_x/_y: the inputs' conventions, should the log's
        // gate (lmu_dlss_gate=1) or the picture say they are the other way round.
        simqvfr::DlssRun dlss;
        dlss.tag = "lmu dlss";
        if (s.ini) {
            const auto key = [&](const wchar_t* name, int fallback) {
                return static_cast<int>(GetPrivateProfileIntW(L"experiment", name, fallback, s.ini));
            };
            dlss.views = std::clamp(key(L"lmu_dlss", 0), 0, 2);
            wchar_t folder[MAX_PATH] = {};
            GetPrivateProfileStringW(L"experiment", L"lmu_dlss_folder", L"", folder, MAX_PATH, s.ini);
            dlss.dllFolder = folder;
            dlss.preset = (std::max)(0, key(L"lmu_dlss_preset", 0));
            dlss.jitter = key(L"lmu_dlss_jitter", 1) != 0;
            dlss.vectors = key(L"lmu_dlss_vectors", 1) != 0;
            dlss.viewInverse = key(L"lmu_dlss_view_inverse", 0) != 0;
            dlss.depthReversed = key(L"lmu_dlss_depth_reversed", 1) != 0;
            // lmu_dlss_car=0: the cockpit treated as the world. lmu_dlss_car_box_cm: the car's box about the head, in
            // centimetres to the left, right, down, up, front and back (engine/dlss_views.h).
            dlss.car = key(L"lmu_dlss_car", 1) != 0;
            wchar_t box[96] = {};
            GetPrivateProfileStringW(L"experiment", L"lmu_dlss_car_box_cm", L"", box, 96, s.ini);
            int cm[6];
            if (swscanf_s(box, L"%d,%d,%d,%d,%d,%d", &cm[0], &cm[1], &cm[2], &cm[3], &cm[4], &cm[5]) == 6)
                for (int i = 0; i < 6; ++i)
                    dlss.carBox[i] = std::clamp(cm[i], 0, 2000) / 100.0;
            dlss.flipX = key(L"lmu_dlss_jitter_flip_x", 0) != 0;
            dlss.flipY = key(L"lmu_dlss_jitter_flip_y", 0) != 0;
            dlss.gate = key(L"lmu_dlss_gate", 0) != 0;
            if (dlss.views && !run.trackerDrives) {
                simqvfr::log_line("lmu dlss: asked for, but it needs the multi-view path (multi_view=1): off");
                dlss.views = 0;
            }
        }
        dlss_use_lmu_inputs();  // the pass's matrices from the game's renderer
        simqvfr::dlss_start(dlss);
        qvfr_ = four_passes_start(run);
        // The views' launch sizes inside the layer (larger with the menus' floor, views4_menu_scale): from the first
        // frame, as a live change would set them.
        if (qvfr_)
            set_scales(simqvfr::views4_live_one_size(s.layout, s.layout.outer, s.layout.focus, s.layout.focusHeight));
        return qvfr_;
    }
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool) override {
        if (qvfr_)
            four_passes_set_gaze(tan, valid);
    }
    void qvfr_set_live(double focus, double focusY, double, bool) override {
        if (qvfr_)
            four_passes_set_box(focus, focusY);
    }
    // Live resolution: each pass at its own render scale inside the launch-size targets (four_passes.h). On for QVFR
    // runs; GameFeatures::liveResolution stays off until it passes its card.
    bool qvfr_live_resolution() const override { return has_qvfr(); }
    // `live`: views4_live_one_size's layout (each view's share of its layer, halved). LMU scales both axes together, so
    // each view takes the larger share: at least the density asked on both axes.
    void qvfr_set_live_layout(const simqvfr::Views4Layout& live) override {
        if (qvfr_)
            set_scales(live);
    }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        four_passes_view_size(render);
        output[0] = render[0];
        output[1] = render[1];
    }
    // The FOV crop reaches LMU through the located FOV; its lens mask is built with the FOV located when it fetches the
    // mesh, so a live crop change leaves the mask off until the FOV matches again (four_passes.h).
    void qvfr_note_views(const XrView* views, uint32_t count) override {
        if (qvfr_)
            four_passes_note_views(views, count);
    }
    void session_destroyed() override {
        IdentityAdapter::session_destroyed();
        simqvfr::dlss_shutdown();  // its features and NGX belong to the session's device
    }
    // DLAA's jitter (engine/dlss_views.h): the last thing done to the FOV the game locates.
    void qvfr_jitter_views(XrView* views, uint32_t count) override {
        if (qvfr_)
            simqvfr::dlss_jitter_views(views, count);
    }
    void visibility_mask_taken(uint32_t viewIndex) override {
        if (qvfr_)
            four_passes_mask_taken(viewIndex);
    }
    bool qvfr_take_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && four_passes_record(frame); }
    bool qvfr_latest_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && four_passes_record(frame); }
    ID3D11Texture2D* qvfr_views_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) override {
        return qvfr_ ? four_passes_atlas(eyes, uv) : nullptr;
    }
    void qvfr_frame_end() override {
        if (!qvfr_)
            return;
        if (diag_.poll())  // a switch off = the launch setting flipped
            glare_share_set(diag_.off(0) ? !glareAtLaunch_ : glareAtLaunch_);
        four_passes_frame_end();
    }

    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        if (!analysis())
            return;
        viewProbe_ = GetPrivateProfileIntW(L"experiment", L"lmu_view_probe", 0, ini) != 0 && view_probe_start();
        // lmu_mvr_probe=1: every vertex shader converted for NVIDIA's two-view pass and created through NVAPI, log only.
        // Multi-view rendering (with lmu_qvfr: multi_view=1 on a GPU with it, or lmu_mvr): the probe keeps the converted
        // shaders, attached to the game's (mvr_pass.h). The layer decides the same at xrCreateInstance (config.cpp).
        const bool mvr = has_qvfr() && (simqvfr::games::lmu_mvr_gate(ini) != 0 ||
                                        simqvfr::games::multi_view_on(&kGame, ini));
        // The player path (multi_view=1, no gate or legacy key): its shaders named too, for the traces (R6).
        const bool player = mvr && simqvfr::games::lmu_mvr_gate(ini) == 0 &&
                            GetPrivateProfileIntW(L"experiment", L"lmu_mvr_legacy", 0, ini) == 0;
        mvrProbe_ = (mvr || GetPrivateProfileIntW(L"experiment", L"lmu_mvr_probe", 0, ini) != 0) &&
                    mvr_probe_install(ini, mvr, player);
        const bool focus = GetPrivateProfileIntW(L"experiment", L"lmu_focus", 0, ini) != 0;
        const bool copies = GetPrivateProfileIntW(L"experiment", L"lmu_copies", 0, ini) != 0;
        if (has_qvfr()) {
            if (focus || copies)
                simqvfr::log_line("lmu: lmu_qvfr=1: the copies and focus gates don't run (lmu_copies, lmu_focus ignored)");
            return;
        }
        // lmu_focus=1: the focus gate (focus views); lmu_copies=1: the copies gate. Both run the eye loop four times.
        FourPassesRun run;
        run.focus = focus;
        run.holdExposure = GetPrivateProfileIntW(L"experiment", L"lmu_copies_hold_exposure", 1, ini) != 0;
        run.fraction = GetPrivateProfileIntW(L"experiment", L"focus_percent", 30, ini) / 100.0;
        run.first = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_first", 300, ini));
        run.every = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_every", 300, ini));
        run.count = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_samples", 3, ini));
        run.warmup = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"gate_warmup", 60, ini));
        run.exitWhenDone = GetPrivateProfileIntW(L"experiment", L"exit_when_done", 0, ini) != 0;
        gates_ = (focus || copies) && four_passes_start(run);
    }
    void d3d11_session(ID3D11Device* device) override {
        if (mvrProbe_)
            mvr_probe_device(device);
        four_passes_device(device);
    }
    void frame_end() override {
        if (mvrProbe_)
            mvr_probe_frame_end();
        if (viewProbe_)
            view_probe_frame_end();
        if (gates_)
            four_passes_frame_end();
    }

private:
    static void set_scales(const simqvfr::Views4Layout& live) {
        const auto share = [&](int v) { return std::max(2 * live.width[v], 2 * live.height[v]); };
        four_passes_set_live_scales(share(0), share(2));
        // Per axis (side by side for MVR): each view's share of the target's width and height.
        four_passes_set_live_axes(2 * live.width[0], 2 * live.height[0], 2 * live.width[2], 2 * live.height[2]);
    }
    bool viewProbe_ = false;
    bool mvrProbe_ = false;
    bool gates_ = false;
    bool qvfr_ = false;
    simqvfr::games::DiagSwitches diag_;  // lmu_diag=1
    bool glareAtLaunch_ = true;
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static LmuAdapter a;
    return a;
}

}  // namespace lmu

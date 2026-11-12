#include "adapter.h"

#include <windows.h>

#include <algorithm>

#include "../../log.h"
#include "../identity_adapter.h"
#include "../diag_switches.h"
#include "../multi_view.h"
#include "../../engine/dlss_math.h"
#include "../../engine/dlss_views.h"
#include "../../engine/nr_views.h"
#include "dlss_inputs.h"
#include "four_passes.h"
#include "game.h"
#include "mvr_pass.h"
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

    // ---- QVFR (lmu_qvfr=1): the game's two eye passes draw the focus views too, composed by the D3D11 compositor ----
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
        // Menus (frames without the eye loop) at the native eye, the UI drawn at that size (four_passes.h).
        run.menuNative = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_menu_native", 1, s.ini) != 0;
        run.layerWidth = s.layout.layer_width();
        run.layerHeight = s.layout.layer_height();
        // Multi-view rendering (mvr_pass.h): the game's own two eye passes draw their focus views too, beside their eyes,
        // where the layer made room (Views4Layout::secondViewRoom), post-process them (focus post, focus finals) and are
        // found at the d3d11 level (the pass tracker): no game code hooked. The only way LMU's QVFR runs.
        run.mvr = s.layout.secondViewRoom;
        run.mvrSkip = run.mvr;
        run.perAxis = run.mvr && s.layout.secondViewBeside;  // the images the outer views' shape, the focus views beside
        if (run.mvr) {
            // lmu_mvr_timing=1 (and any A/B run): the CPU in the context hooks, logged.
            run.mvrTiming = GetPrivateProfileIntW(L"experiment", L"lmu_mvr_timing", 0, s.ini) != 0;
            // lmu_pass_trace=1 (diagnostics, log only): the pass tracker's view of each new shape of frame.
            run.passTrace = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_pass_trace", 0, s.ini));
            run.textureSharpening = s.ini && simqvfr::read_dlss_settings(s.ini).textureSharpening != 0;
            // Other cars' motion in DLSS's vectors (four_passes.h carTrack, carSlots): the player's [dlss] car_vectors
            // with DLSS on at launch; or as a diagnostic, [experiment] lmu_car_track=1 (the tracking, log only with
            // car_vectors=0) and lmu_car_vectors=1 (the slots and a gate image) or 2 (DLSS takes them); 0 (the shipped
            // ini's): as the player's.
            {
                const simqvfr::DlssSettings player = s.ini ? simqvfr::read_dlss_settings(s.ini) : simqvfr::DlssSettings{};
                const bool playerCars = player.on && player.carVectors;
                const int track = s.ini ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_car_track", 0, s.ini))
                                        : 0;
                const int slots =
                    s.ini ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_car_vectors", 0, s.ini)) : 0;
                run.carForced = track > 0 || slots > 0;
                run.carTrack = track > 0 || slots > 0 || playerCars;
                run.carSlots = slots > 0 ? slots : playerCars ? 2 : 0;
                // lmu_car_vectors_ab=N (with them, a measurement): N frames with the cars' motion, N without, in turn.
                run.carAb = s.ini ? (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment",
                                                                                         L"lmu_car_vectors_ab", 0, s.ini)))
                                  : 0;
                run.mvrTiming = run.mvrTiming || run.carAb > 0;  // its GPU per pass by side
            }
            run.focusReplay = true;  // the eye passes post-process their focus views too (focus_post.h)
            // lmu_ui_mips=0: the VR UI panel samples the game's canvas (one mip) in the small periphery too.
            run.uiMips = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_ui_mips", 1, s.ini) != 0;
            run.focusFinal = true;     // the focus views from beside the eyes
            run.passTracker = true;    // the eye passes from the d3d11 calls
            run.trackerDrives = true;  // and nothing of the game's hooked
            {
                // lmu_mvr_backend_ab=N: N frames NVIDIA's single-pass stereo, N instancing, in turn (four_passes.h).
                run.backendAb = !s.ini ? 0 : (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mvr_backend_ab", 0,
                                                                                      s.ini)));
                run.mvrTiming = run.mvrTiming || run.backendAb > 0;
                // lmu_mvr_lose_at=N: the focus views lost from scene frame N for 1800 scene frames (the recovery's test).
                run.loseAt = !s.ini ? 0 : (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mvr_lose_at", 0,
                                                                                  s.ini)));
            }
        }
        if (s.ini)  // diagnostic: the render scale changed live for 120 frames (four_passes.h)
            run.renderScaleProbe =
                std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_render_scale_probe", 0, s.ini)), 0, 100);
        // Sim-QVFR's own DLSS on the views (engine/dlss_views.h): the player's [dlss] (engine/dlss_settings.h: on, the
        // nvngx_dlss.dll folder, the focus views' mode and preset, the eyes' preset), live through qvfr_set_dlss; with
        // the multi-view path only (its eye passes found at the d3d11 level). The inputs' diagnostics are [experiment]
        // keys: lmu_dlss_jitter=0, lmu_dlss_vectors=0: without that input (a comparison); lmu_dlss_view_inverse,
        // lmu_dlss_jitter_flip_x/_y: the inputs' conventions, should the log's gate (lmu_dlss_gate=1) or the picture
        // say they are the other way round.
        simqvfr::DlssRun dlss;
        dlss.tag = "lmu dlss";
        dlssPossible_ = run.trackerDrives;
        if (s.ini) {
            const auto key = [&](const wchar_t* name, int fallback) {
                return static_cast<int>(GetPrivateProfileIntW(L"experiment", name, fallback, s.ini));
            };
            const simqvfr::DlssSettings player = simqvfr::read_dlss_settings(s.ini);
            dlss.views = player.on ? 2 : 0;
            dlss.dllFolder = player.folder;
            dlss.preset = player.focusPreset;
            dlss.eyePreset = player.eyePreset;
            dlss.focusScale = player.focusScale;
            dlss.textureSharpening = player.textureSharpening;
            dlssSettings_ = player;
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
                simqvfr::log_line("lmu dlss: asked for, but this run has no multi-view path: off");
                dlss.views = 0;
            }
        }
        dlss_use_lmu_inputs();  // the pass's matrices from the game's renderer
        simqvfr::dlss_start(dlss);
        // DLSS-NR on those views (engine/nr_views.h): the player's [dlss_nr], live through qvfr_set_nr. It takes DLSS's
        // depth and motion vectors, so it runs only while DLSS does; nvngx_dlssnr.dll is the player's own ([dlss_nr]
        // folder).
        nrSettings_ = s.ini ? simqvfr::read_nr_settings(s.ini) : simqvfr::NrSettings{};
        if (nrSettings_.on && !dlss.views)
            simqvfr::log_line("lmu dlss-nr: on, but DLSS is off: it runs once DLSS is on (it takes DLSS's depth and motion "
                              "vectors)");
        simqvfr::NrSettings nr = nrSettings_;
        nr.on = nr.on && dlssPossible_;
        simqvfr::nr_start("lmu dlss-nr", nr);
        // Diagnostics (lmu_diag=1; games/diag_switches.h), an A/B in one run: Ctrl+Alt+1 executes the game's command
        // lists (its sun shadows) as before their fix (mvr_pass.h mvr_replay_lists_plain).
        if (diag_.start(s.ini, L"lmu", {L"list_fix"}))
            simqvfr::logf("lmu: diagnostics on: Ctrl+Alt+1 switches the multi-view shadow fix (lmu_diag_list_fix_off=1 in "
                          "%ls turns it off)",
                          diag_.ini().c_str());
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
    // The player's DLSS settings changed in the launcher or the headset menu: the module takes them between frames.
    void qvfr_set_dlss(const simqvfr::DlssSettings& settings) override {
        if (!qvfr_ || settings == dlssSettings_)
            return;
        if (settings.on && !dlssPossible_) {
            if (!dlssRefusedLogged_)
                simqvfr::log_line("lmu dlss: turned on, but this run has no multi-view path: off");
            dlssRefusedLogged_ = true;
            dlssSettings_ = settings;
            return;
        }
        if (settings.folder != dlssSettings_.folder)
            simqvfr::log_line("lmu dlss: the nvngx_dlss.dll folder changed: it applies at the next launch");
        dlssSettings_ = settings;
        simqvfr::DlssLive live;
        live.on = settings.on;
        live.focusScale = settings.focusScale;
        live.preset = settings.focusPreset;
        live.eyePreset = settings.eyePreset;
        live.textureSharpening = settings.textureSharpening;
        simqvfr::dlss_live(live);
    }
    // The player's DLSS-NR settings changed: the module takes them between frames (another folder: the next launch).
    void qvfr_set_nr(const simqvfr::NrSettings& settings) override {
        if (!qvfr_ || settings == nrSettings_)
            return;
        if (settings.folder != nrSettings_.folder)
            simqvfr::log_line("lmu dlss-nr: the nvngx_dlssnr.dll folder changed: it applies at the next launch");
        nrSettings_ = settings;
        simqvfr::NrSettings live = settings;
        live.on = live.on && dlssPossible_;
        simqvfr::nr_live(live);
    }
    void visibility_mask_taken(uint32_t viewIndex) override {
        if (qvfr_)
            four_passes_mask_taken(viewIndex);
    }
    bool qvfr_take_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && four_passes_record(frame); }
    simqvfr::games::QvfrMultiView qvfr_multi_view() const override {
        return qvfr_ ? four_passes_multi_view() : simqvfr::games::QvfrMultiView::none;
    }
    bool qvfr_latest_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && four_passes_record(frame); }
    bool qvfr_eye_extent(unsigned (&width)[2], unsigned (&height)[2]) override {
        return qvfr_ && four_passes_eye_extent(width, height);
    }
    ID3D11Texture2D* qvfr_views_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) override {
        return qvfr_ ? four_passes_atlas(eyes, uv) : nullptr;
    }
    void qvfr_frame_end() override {
        if (!qvfr_)
            return;
        if (diag_.poll())  // the switch off: the lists executed as before the fix
            mvr_replay_lists_plain(!diag_.off(0));
        four_passes_frame_end();
    }

    void instance_created(const wchar_t* ini, const wchar_t* mode) override {
        IdentityAdapter::instance_created(ini, mode);
        if (!analysis())
            return;
        viewProbe_ = GetPrivateProfileIntW(L"experiment", L"lmu_view_probe", 0, ini) != 0 && view_probe_start();
        // Multi-view rendering (with lmu_qvfr): the probe keeps the converted shaders, attached to the game's
        // (mvr_pass.h), its shaders named for the traces. The layer decides the same at xrCreateInstance (config.cpp).
        // lmu_mvr_probe=1 without it: every vertex shader converted and created through NVAPI, log only.
        const bool mvr = has_qvfr() && simqvfr::games::multi_view_on(&kGame, ini);
        mvrProbe_ = (mvr || GetPrivateProfileIntW(L"experiment", L"lmu_mvr_probe", 0, ini) != 0) &&
                    mvr_probe_install(ini, mvr, mvr);
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
    bool qvfr_ = false;
    // The player's DLSS settings as last applied, and whether this run can run DLSS at all (the multi-view path).
    simqvfr::DlssSettings dlssSettings_;
    simqvfr::NrSettings nrSettings_;
    simqvfr::games::DiagSwitches diag_;  // lmu_diag=1
    bool dlssPossible_ = false, dlssRefusedLogged_ = false;
};

}  // namespace

simqvfr::games::GameAdapter& adapter() {
    static LmuAdapter a;
    return a;
}

}  // namespace lmu

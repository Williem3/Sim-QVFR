#include "adapter.h"

#include <windows.h>

#include <algorithm>

#include "../../log.h"
#include "../../quad/car_lighting.h"
#include "../identity_adapter.h"
#include "../diag_switches.h"
#include "../multi_view.h"
#include "../../engine/dlss_math.h"
#include "../../engine/dlss_views.h"
#include "../../engine/nr_views.h"
#include "../../quad/qvfr_settings.h"
#include "dlss_inputs.h"
#include "fixation.h"
#include "qvfr_frame.h"
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
// otherwise adapter_for hands out the plain identity adapter there): the view probe (log only), multi-view's shader
// probe, and QVFR.
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
        QvfrFrameRun run;
        run.qvfr = true;
        run.fraction = s.focusFraction > 0 ? s.focusFraction : 0.3;
        run.fractionY = s.focusFractionY;
        if (s.ini) {  // the focus mask at launch (live from then on: qvfr_set_focus_mask)
            const simqvfr::FocusArea area = simqvfr::focus_area_of(simqvfr::read_focus_shape(s.ini),
                                                                   simqvfr::ini_int(s.ini, simqvfr::kCornerRadiusPercent));
            qvfr_set_focus_mask_now(simqvfr::ini_flag(s.ini, simqvfr::kFocusMask), area,
                                    simqvfr::ini_int(s.ini, simqvfr::kFocusMaskMargin));
            // Road focus at launch, fixed foveation only (live from then on: qvfr_set_focus_corridor).
            qvfr_frame_set_corridor(simqvfr::ini_flag(s.ini, simqvfr::kFocusCorridor) &&
                                        !simqvfr::ini_flag(s.ini, simqvfr::kGaze),
                                    simqvfr::ini_int(s.ini, simqvfr::kFocusCorridorWidth),
                                    simqvfr::ini_int(s.ini, simqvfr::kFocusCorridorHeight),
                                    simqvfr::ini_int(s.ini, simqvfr::kFocusCorridorSpread),
                                    simqvfr::ini_int(s.ini, simqvfr::kFocusCorridorBlend),
                                    simqvfr::ini_flag(s.ini, simqvfr::kFocusCorridorShow));
        }
        // Menus (frames without the eye loop) at the native eye, the UI drawn at that size (qvfr_frame.h).
        run.menuNative = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_menu_native", 1, s.ini) != 0;
        run.layerWidth = s.layout.layer_width();
        run.layerHeight = s.layout.layer_height();
        // Multi-view rendering (mvr_pass.h): the game's own two eye passes draw their focus views too, beside their eyes,
        // where the layer made room (Views4Layout::secondViewRoom), post-process them (focus post, focus finals) and are
        // found at the d3d11 level (the pass tracker): no game code hooked. The only way LMU's QVFR runs.
        run.mvr = s.layout.secondViewRoom;
        run.perAxis = run.mvr && s.layout.secondViewBeside;  // the images the outer views' shape, the focus views beside
        if (run.mvr) {
            // lmu_mvr_timing=1 (and any A/B run): the CPU in the context hooks, logged.
            run.mvrTiming = GetPrivateProfileIntW(L"experiment", L"lmu_mvr_timing", 0, s.ini) != 0;
            // lmu_pass_trace=1 (diagnostics, log only): the pass tracker's view of each new shape of frame.
            run.passTrace = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_pass_trace", 0, s.ini));
            // lmu_post_census=1-3 (diagnostics, log only): the eye pass's draws in detail (docs/recon-lmu.md "Census").
            run.postCensus = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_post_census", 0, s.ini));
            run.textureSharpening = s.ini && simqvfr::read_dlss_settings(s.ini).textureSharpening != 0;
            // Other cars' motion in DLSS's vectors (qvfr_frame.h carTrack, carSlots): the player's [dlss] car_vectors
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
                // Night car lighting on at launch: the tracking and the slots for it too (they say which pixels are
                // cars'), DLSS's motion or not.
                run.carLight = s.ini && simqvfr::read_car_lighting(s.ini).on;
                run.carTrack = track > 0 || slots > 0 || playerCars || run.carLight;
                run.carSlots = slots > 0 ? slots : playerCars ? 2 : run.carLight ? 1 : 0;
                // lmu_car_vectors_ab=N (with them, a measurement): N frames with the cars' motion, N without, in turn.
                run.carAb = s.ini ? (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment",
                                                                                         L"lmu_car_vectors_ab", 0, s.ini)))
                                  : 0;
                run.mvrTiming = run.mvrTiming || run.carAb > 0;  // its GPU per pass by side
            }
            run.focusReplay = true;  // the eye passes post-process their focus views too (focus_post.h)
            // lmu_ui_mips=0: the VR UI panel samples the game's canvas (one mip) in the small periphery too.
            run.uiMips = !s.ini || GetPrivateProfileIntW(L"experiment", L"lmu_ui_mips", 1, s.ini) != 0;
            // lmu_dlss_glass_depth=0: DLSS reads the scene's depth as the pass ends it, the car's glass in it (mvr_pass.h
            // mvr_glass_depth).
            run.glassDepth = !s.ini ? 1 : static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_dlss_glass_depth", 1,
                                                                                s.ini));
            run.focusFinal = true;     // the focus views from beside the eyes
            run.passTracker = true;    // the eye passes from the d3d11 calls
            run.trackerDrives = true;  // and nothing of the game's hooked
            {
                // lmu_mvr_backend_ab=N: N frames NVIDIA's single-pass stereo, N instancing, in turn (qvfr_frame.h).
                run.backendAb = !s.ini ? 0 : (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mvr_backend_ab", 0,
                                                                                      s.ini)));
                run.mvrTiming = run.mvrTiming || run.backendAb > 0;
                // lmu_mask_ab=N: N frames with the lens mask's and the focus mask's draws, N without, in turn (qvfr_frame.h).
                run.maskAb = !s.ini ? 0 : (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mask_ab", 0,
                                                                                    s.ini)));
                run.mvrTiming = run.mvrTiming || run.maskAb > 0;
                // lmu_corridor_ab=N: N frames with the focus views kept to a corridor ahead, N without (qvfr_frame.h).
                const auto key = [&s](const wchar_t* name, int def) {
                    return !s.ini ? def : static_cast<int>(GetPrivateProfileIntW(L"experiment", name, def, s.ini));
                };
                run.corridorAb = (std::max)(0, key(L"lmu_corridor_ab", 0));
                // Missing: the player's [quad] focus_corridor_width (live).
                const int halfCm = key(L"lmu_corridor_half_cm", -1);
                run.corridorHalfM = halfCm < 0 ? 0.0f : (std::max)(50, halfCm) / 100.0f;
                // Missing: the player's [quad] focus_corridor_height (live).
                const int tallCm = key(L"lmu_corridor_tall_cm", -1);
                run.corridorTallM = tallCm < 0 ? 0.0f : (std::max)(50, tallCm) / 100.0f;
                run.corridorStartM = (std::max)(0, key(L"lmu_corridor_start_cm", 300)) / 100.0f;
                run.corridorFarM = static_cast<float>((std::max)(0, key(L"lmu_corridor_far_m", 0)));
                run.corridorEyeHeightM = (std::max)(30, key(L"lmu_corridor_eye_height_cm", 130)) / 100.0f;
                run.corridorShow = key(L"lmu_corridor_show", 0) != 0;
                run.corridorOn = key(L"lmu_corridor", 0) != 0;
                // Missing: the player's [quad] focus_corridor_blend (live).
                const int featherCm = key(L"lmu_corridor_feather_cm", -1);
                run.corridorFeatherM = featherCm < 0 ? -1.0f : featherCm / 100.0f;
                run.corridorBandPct = std::clamp(key(L"lmu_corridor_dlss_band_pct", -1), -1, 100);
                run.corridorBandPadPct = std::clamp(key(L"lmu_corridor_dlss_band_pad_pct", 6), 0, 50);
                run.corridorCarCm = std::clamp(key(L"lmu_corridor_car_cm", -1), -1, 1000);
                // Missing: the player's [quad] focus_corridor_spread (live).
                run.corridorSpreadDeg = std::clamp(key(L"lmu_corridor_spread_deg", -1), -1, 60);
                run.corridorFeatherGrowthPct = std::clamp(key(L"lmu_corridor_feather_growth_pct", 3), 0, 50);
                run.corridorDropPct = std::clamp(key(L"lmu_corridor_drop_pct", 15), 0, 100);
                run.corridorSlopeDeg = static_cast<float>(std::clamp(key(L"lmu_corridor_road_slope_deg", 35), 5, 75));
                run.corridorCentreM = (std::max)(0, key(L"lmu_corridor_centre_cm", 600)) / 100.0f;
                run.mvrTiming = run.mvrTiming || run.corridorAb > 0;
                // lmu_mvr_lose_at=N: the focus views lost from scene frame N for 1800 scene frames (the recovery's test).
                run.loseAt = !s.ini ? 0 : (std::max)(0, static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_mvr_lose_at", 0,
                                                                                  s.ini)));
                run.loseShape = s.ini && GetPrivateProfileIntW(L"experiment", L"lmu_mvr_lose_shape", 0, s.ini) != 0;
            }
        }
        // Sim-QVFR's own DLSS on the views (engine/dlss_views.h): the player's [dlss] (engine/dlss_settings.h: on, the
        // nvngx_dlss.dll folder, the focus views' mode and preset, the eyes' preset), live through qvfr_set_dlss; with
        // the multi-view path only (its eye passes found at the d3d11 level). The inputs' diagnostics are [experiment]
        // keys: lmu_dlss_jitter=0, lmu_dlss_vectors=0: without that input (a comparison); lmu_dlss_view_inverse,
        // lmu_dlss_jitter_flip_x/_y: the inputs' conventions, should the log's gate (lmu_dlss_gate=1) or the picture
        // say they are the other way round.
        // Any game build (qvfr_frame.h qvfr_frame_known_build): lmu_build_free=1 takes the game's facts from OpenXR and
        // Direct3D on a known build too, as on any other.
        run.buildFree = s.ini && GetPrivateProfileIntW(L"experiment", L"lmu_build_free", 0, s.ini) != 0;
        // lmu_viewport_reads=0/1/2 (an A/B; mvr_pass.h mvr_viewport_reads): who is given the game's own viewports.
        run.viewportReads = s.ini ? static_cast<int>(GetPrivateProfileIntW(L"experiment", L"lmu_viewport_reads", 1, s.ini))
                                  : 1;
        simqvfr::DlssRun dlss;
        dlss.tag = "lmu dlss";
        // Each pass's camera for DLSS (dlss_inputs.cpp): the game's renderer's on a known build, what the pass's draws
        // upload on any other (view_pair.h).
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
            // lmu_fixation=0: no depth read for the focus boxes' convergence (fixation.h): they stay parallel.
            fixation_ = key(L"lmu_fixation", 1) != 0;
            depthReversed_ = dlss.depthReversed;
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
            // lmu_dlss_glass_measure=1 (development builds: 1 by default): every ~2 s the depth before the transparents
            // against the pass's end, logged (engine/dlss_views.h DlssRun::depthMeasure).
            dlss.depthMeasure = key(L"lmu_dlss_glass_measure", simqvfr::kDevBuild ? 1 : 0) != 0;
            // lmu_dlss_glass_near_cm (300): DLSS's depth from before the transparents only where they lie nearer than
            // this (the player's car's glass); 0 everywhere (DlssRun::depthCopyNear).
            dlss.depthCopyNear = std::clamp(key(L"lmu_dlss_glass_near_cm", 300), 0, 100000) / 100.0;
            // AMD FSR in DLSS's place (engine/fsr_upscale.h; experimental): wherever DLSS can't run (not an NVIDIA GPU,
            // or NGX refuses to start), the launcher's FSR card. On an RTX card, lmu_fsr=1 (a test) lets [dlss]
            // upscaler=fsr choose it, live. lmu_fsr_provider=3 the newest FSR 3.x even where FSR 4 is offered;
            // lmu_fsr_folder AMD's two DLLs' folder (else beside the layer, then beside the game).
            fsrAllowed_ = key(L"lmu_fsr", 0) != 0;
            dlss.upscaler = fsrAllowed_ ? player.upscaler : 0;
            dlss.fsrWhereNoDlss = true;  // a PC without an NVIDIA RTX card: AMD FSR, the launcher's FSR card
            dlss.fsrProvider = key(L"lmu_fsr_provider", 0) == 3 ? 3 : 0;
            wchar_t fsrFolder[MAX_PATH] = {};
            GetPrivateProfileStringW(L"experiment", L"lmu_fsr_folder", L"", fsrFolder, MAX_PATH, s.ini);
            dlss.fsrFolder = fsrFolder;
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
        // Diagnostics (lmu_diag=1; games/diag_switches.h), A/Bs in one run: Ctrl+Alt+1 executes the game's command
        // lists (its sun shadows) as before their fix (mvr_pass.h mvr_replay_lists_plain); Ctrl+Alt+2 has DLSS read the
        // depth as the pass ends it, the car's glass in it (mvr_glass_read; the copy is still made and measured).
        if (diag_.start(s.ini, L"lmu", {L"list_fix", L"glass_depth"}))
            simqvfr::logf("lmu: diagnostics on: Ctrl+Alt+1 switches the multi-view shadow fix, Ctrl+Alt+2 DLSS's depth "
                          "before the car's glass (lmu_diag_list_fix_off=1, lmu_diag_glass_depth_off=1 in %ls turn them "
                          "off)",
                          diag_.ini().c_str());
        qvfr_ = qvfr_frame_start(run);
        // The views' launch sizes inside the layer (larger with the menus' floor, views4_menu_scale): from the first
        // frame, as a live change would set them.
        if (qvfr_)
            set_scales(simqvfr::views4_live_one_size(s.layout, s.layout.outer, s.layout.focus, s.layout.focusHeight));
        return qvfr_;
    }
    // How far the eyes look (fixation.h): the layer converges both eyes' focus boxes there.
    double qvfr_fixation_m() const override { return qvfr_ ? fixation_m() : 0.0; }
    void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool) override {
        if (qvfr_)
            qvfr_frame_set_gaze(tan, valid);
    }
    void qvfr_set_live(double focus, double focusY, double, bool) override {
        if (qvfr_)
            qvfr_frame_set_box(focus, focusY);
    }
    void qvfr_set_focus_mask(bool on, const simqvfr::FocusArea& area, int marginTexels) override {
        if (qvfr_)
            qvfr_set_focus_mask_now(on, area, marginTexels);
    }
    void qvfr_set_focus_visible(const float (&visible)[4]) override {
        if (qvfr_)
            qvfr_frame_set_focus_visible(visible);
    }
    void qvfr_set_focus_corridor(bool on, int widthM, int heightDm, int spreadDeg, int blendDm, bool show) override {
        if (qvfr_)
            qvfr_frame_set_corridor(on, widthM, heightDm, spreadDeg, blendDm, show);
    }
    static void qvfr_set_focus_mask_now(bool on, const simqvfr::FocusArea& area, int marginTexels) {
        qvfr_frame_set_focus_mask(on, area, marginTexels > 0 ? static_cast<unsigned>(marginTexels) : 0u);
    }
    // Live resolution: each pass at its own render scale inside the launch-size targets (qvfr_frame.h). On for QVFR
    // runs (GameFeatures::liveResolution too, for the launcher).
    bool qvfr_live_resolution() const override { return has_qvfr(); }
    // `live`: views4_live_one_size's layout (each view's share of its layer, halved). LMU scales both axes together, so
    // each view takes the larger share: at least the density asked on both axes.
    void qvfr_set_live_layout(const simqvfr::Views4Layout& live) override {
        if (qvfr_)
            set_scales(live);
    }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        qvfr_frame_view_size(render);
        output[0] = render[0];
        output[1] = render[1];
    }
    // The FOV crop reaches LMU through the located FOV; its lens mask is built with the FOV located when it fetches the
    // mesh, so a live crop change leaves the mask off until the FOV matches again (qvfr_frame.h).
    void qvfr_note_views(const XrView* views, uint32_t count) override {
        if (qvfr_)
            qvfr_frame_note_views(views, count);
    }
    void session_destroyed() override {
        IdentityAdapter::session_destroyed();
        fixation_stop();
        simqvfr::dlss_shutdown();  // its features and NGX belong to the session's device
    }
    // DLAA's jitter (engine/dlss_views.h): the last thing done to the FOV the game locates.
    void qvfr_jitter_views(XrView* views, uint32_t count) override {
        if (qvfr_) {
            simqvfr::dlss_jitter_views(views, count);
            qvfr_frame_game_views(views, count);  // the FOV as the game gets it: its eye projections on any build
        }
    }
    // The eye size the game is offered: its eye targets' size on any build (qvfr_frame.h).
    void views_offered(uint32_t width, uint32_t height) override { qvfr_frame_offered(width, height); }
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
        live.upscaler = fsrAllowed_ ? settings.upscaler : 0;
        simqvfr::dlss_live(live);
        qvfr_frame_cars_wanted(settings.on && settings.carVectors);  // the cars' tracking only for an upscaler using it
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
            qvfr_frame_mask_taken(viewIndex);
    }
    bool qvfr_take_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && qvfr_frame_record(frame); }
    bool qvfr_car_light_costs(double& gpuMs, double& cpuMs) const override {
        return mvr_car_light_costs(gpuMs, cpuMs);
    }
    simqvfr::games::QvfrMultiView qvfr_multi_view() const override {
        return qvfr_ ? qvfr_frame_multi_view() : simqvfr::games::QvfrMultiView::none;
    }
    bool qvfr_latest_frame(simqvfr::Views4Frame& frame) override { return qvfr_ && qvfr_frame_record(frame); }
    bool qvfr_eye_extent(unsigned (&width)[2], unsigned (&height)[2]) override {
        return qvfr_ && qvfr_frame_eye_extent(width, height);
    }
    ID3D11Texture2D* qvfr_views_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) override {
        return qvfr_ ? qvfr_frame_atlas(eyes, uv) : nullptr;
    }
    void qvfr_frame_end() override {
        if (!qvfr_)
            return;
        if (diag_.poll()) {  // a switch off: the lists executed as before the fix; DLSS's depth with the glass in it
            mvr_replay_lists_plain(!diag_.off(0));
            mvr_glass_read(!diag_.off(1));
            simqvfr::logf("lmu: diagnostics: the multi-view shadow fix %s, DLSS's depth before the car's glass %s",
                          diag_.off(0) ? "OFF" : "on", diag_.off(1) ? "OFF (the depth as the pass ends it)" : "on");
        }
        qvfr_frame_frame_end();
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
        qvfr_frame_device(device);
        if (qvfr_ && fixation_)
            fixation_start(device, depthReversed_);
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
        qvfr_frame_set_live_scales(share(0), share(2));
        // Per axis (side by side for MVR): each view's share of the target's width and height.
        qvfr_frame_set_live_axes(2 * live.width[0], 2 * live.height[0], 2 * live.width[2], 2 * live.height[2]);
    }
    bool viewProbe_ = false;
    bool mvrProbe_ = false;
    bool qvfr_ = false;
    bool fixation_ = true;       // [experiment] lmu_fixation
    bool depthReversed_ = true;  // lmu_dlss_depth_reversed
    // The player's DLSS settings as last applied, and whether this run can run DLSS at all (the multi-view path).
    simqvfr::DlssSettings dlssSettings_;
    bool fsrAllowed_ = false;  // [experiment] lmu_fsr: AMD FSR may run the views
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

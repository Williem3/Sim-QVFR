// sim-qvfr.ini at the start of a session: the experiment, the live keys, the crop, views4's sizes, runtime.ini.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include "dev_build.h"
#include "engine/dlss_math.h"
#include "probe/cpu_isolation.h"
#include "engine/dlss_settings.h"
#include "games/game.h"
#include "games/multi_view.h"
#include "ini_put.h"
#include "input/hotkeys.h"
#include "log.h"
#include "module_path.h"
#include "quad/focus_offset.h"
#include "quad/focus_shape.h"
#include "quad/fov_crop.h"
#include "quad/focus_quad_distance.h"
#include "quad/gaze_deadzone.h"
#include "quad/helmet.h"
#include "quad/qvfr_settings.h"
#include "quad/settings_menu.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

// [crop] (QVFR): the sides of each eye's FOV ACE does not render (fov_crop.h), each on its own.
FovCrop read_crop(const wchar_t* ini) {
    if (!ini_flag(ini, kCropEnabled))
        return {};
    return fov_crop(ini_int(ini, kCropTop), ini_int(ini, kCropBottom), ini_int(ini, kCropOuter), ini_int(ini, kCropInner));
}

}  // namespace

// What the game has beyond QVFR (games/game.h GameFeatures); anything else stays off whatever the ini says.
games::GameFeatures game_features() {
    const games::GameInfo* g = games::current_game();
    games::GameFeatures f = g ? g->features : games::GameFeatures{};
    f.liveResolution = g && game->qvfr_live_resolution();  // the adapter knows it for this run
    return f;
}

const char* stock_refusal() {
    if (experiment.crop().on())
        return "the FOV crop is on: Stock is the whole view (turn the crop off)";
    const Views4Layout& l = experiment.views4Layout;
    if (experiment.replayCompose && (experiment.eyeScale < 0.995 || l.cropWidth < 0.999 || l.cropHeight < 0.999))
        return "ACE launched below the native eye (periphery under 100% or a crop): Stock needs periphery 100% and no "
               "crop at launch, QVFR then lowers the periphery live";
    return nullptr;
}

LiveSettings read_live(const wchar_t* ini) {
    LiveSettings l;
    const games::GameFeatures features = game_features();
    if (features.fovCrop)
        l.crop = read_crop(ini);
    l.helmet = read_helmet_settings(ini);
    l.helmet.on = l.helmet.on && features.helmet;
    if (features.objectDetail)
        l.objectDetail = ini_int(ini, kObjectDetail);
    if (features.sharpening)
        l.sharpen = ini_int(ini, kSharpenPercent);
    if (features.peripheryTextureMatch)
        l.textureMatch = ini_int(ini, kPeripheryTextureMatch);
    if (features.sharpening)  // every game whose compositor runs (as sharpening)
        l.color = read_color_grade(ini);
    if (features.carLighting)  // night car lighting (the game reads it each frame: quad/car_lighting.h)
        l.carLight = read_car_lighting(ini);
    l.outputDensityMax = ini_int(ini, kOutputDensityMax);
    if (features.eyeOff)
        l.eyeOff = eye_off_from_ini(ini_int(ini, kEyeOff));
    if (features.showcase)
        l.stock = ini_flag(ini, kStock);
    const int isolateExperiment = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"cpu_isolate", 0, ini)), 0, 100000);
    l.cpuIsolate = isolateExperiment ? isolateExperiment : ini_flag(ini, kCpuIsolateMain) ? 1 : 0;
    l.focusFraction = ini_int(ini, kFocusPercent) / 100.0;
    // Missing (or 0): focus_percent's, as views4_sizes takes 0.
    const int heightPercent = ini_raw(ini, kFocusHeightPercent, 0);
    l.focusFractionY = heightPercent ? kFocusHeightPercent.clamp(heightPercent) / 100.0 : 0.0;
    l.feather = ini_int(ini, kFeatherPercent) / 100.0;
    l.cornerRadius = ini_int(ini, kCornerRadiusPercent) / 100.0;
    l.shape = read_focus_shape(ini);
    if (l.shape.custom)
        l.cornerRadius = 0;
    l.border = ini_flag(ini, kDebugBorder);
    // Linear values; teal is #00A0A0 in sRGB. Green (LiveSettings' own) for anything else.
    const float magentaColor[4] = {1, 0, 1, 1}, tealColor[4] = {0, 0.352f, 0.352f, 1};
    const BorderColor borderColor = read_border_color(ini);
    if (borderColor == BorderColor::Magenta)
        std::memcpy(l.borderColor, magentaColor, sizeof(magentaColor));
    else if (borderColor == BorderColor::Teal)
        std::memcpy(l.borderColor, tealColor, sizeof(tealColor));
    l.gaze = ini_flag(ini, kGaze);
    l.offset = read_focus_offset(ini);
    l.gazeDeadzone = read_gaze_deadzone(ini) / 10.0;
    l.focusQuadDistance = read_focus_quad_distance(ini);
    l.focusMask = ini_flag(ini, kFocusMask);
    l.focusMaskMargin = ini_int(ini, kFocusMaskMargin);
    if (features.focusCorridor) {
        l.focusCorridor = ini_flag(ini, kFocusCorridor);
        l.focusCorridorWidth = ini_int(ini, kFocusCorridorWidth);
        l.focusCorridorHeight = ini_int(ini, kFocusCorridorHeight);
        l.focusCorridorSpread = ini_int(ini, kFocusCorridorSpread);
        l.focusCorridorBlend = ini_int(ini, kFocusCorridorBlend);
        l.focusCorridorShow = ini_flag(ini, kFocusCorridorShow);
    }
    return l;
}

// The keys that apply only at the next launch, as one string (live_reload says when they change).
// views4 resizes its views live (views4_live), up to their launch size.
std::wstring restart_keys(const wchar_t* ini, bool views4) {
    std::wstring out;
    for (const wchar_t* key : {L"mode", L"eye_scale", L"periphery_scale", L"focus_density"}) {
        if (views4 && _wcsicmp(key, L"mode") != 0)
            continue;
        wchar_t value[64] = {};
        GetPrivateProfileStringW(L"experiment", key, L"", value, 64, ini);
        out += std::wstring(key) + L"=" + value + L" ";
    }
    return out;
}

Views4Sizes views4_sizes(const wchar_t* ini, double focusFraction, double focusFractionY) {
    const double eyeScale = ini_int(ini, kEyeScale) / 100.0;
    const int peripheryPercent = ini_raw(ini, kPeripheryScale, 0);
    const int focusDensityPercent = ini_raw(ini, kFocusDensity, 0);
    Views4Sizes s;
    s.fromEyeScale = !peripheryPercent && !focusDensityPercent;
    s.outer = peripheryPercent ? kPeripheryScale.clamp(peripheryPercent) / 100.0 : eyeScale;
    // The focus views' linear size against the native eye: density times the box's share of the FOV,
    // per axis. Unset, the density is eye_scale / focus_percent (equal quarters for a square box).
    s.density = focusDensityPercent ? kFocusDensity.clamp(focusDensityPercent) / 100.0 : eyeScale / focusFraction;
    // Unset, exactly eye_scale on the width (not density x fraction, whose rounding would leave equal quarters).
    s.focus = focusDensityPercent ? s.density * focusFraction : eyeScale;
    s.focusHeight = focusFractionY > 0 ? s.density * focusFractionY : s.focus;
    return s;
}

// What the launcher shows before the next launch (SIM_QVFR_RUNTIME_INFO, set by the launcher): the
// headset's recommended per-eye size and whether eye tracking delivered samples. An ini file.
void runtime_info(const wchar_t* key, const std::wstring& value) {
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_RUNTIME_INFO", buffer, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(buffer) : std::wstring();
    }();
    if (!path.empty())
        ini_put(path.c_str(), L"runtime", key, value.c_str());
}

// The folder this DLL is in.
std::wstring layer_directory() { return module_folder(reinterpret_cast<const void*>(&layer_directory)); }

std::wstring experiment_ini() {
    // A launcher passes its per-launch settings in SIM_QVFR_INI; otherwise the ini next to the DLL.
    std::wstring ini = (std::filesystem::path(layer_directory()) / L"sim-qvfr.ini").wstring();
    wchar_t env[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_INI", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH && std::filesystem::path(env).is_absolute())
        ini = env;
    return ini;
}

bool native_quad_wanted(std::string* why) {
    const std::wstring ini = experiment_ini();
    // Auto (1, the default): wherever the runtime composes quad views; 0: never.
    if (GetPrivateProfileIntW(L"experiment", L"native_quad_views", 1, ini.c_str()) == 0) {
        if (why)
            *why = "off ([experiment] native_quad_views=0)";
        return false;
    }
    // Cheeky's frame generation works on one composed image: composed then, as before.
    if (GetPrivateProfileIntW(L"cheeky", L"enabled", 0, ini.c_str()) != 0 &&
        GetPrivateProfileIntW(L"cheeky", L"fg", 0, ini.c_str()) != 0) {
        if (why)
            *why = "off with Cheeky's frame generation (it works on one composed image)";
        return false;
    }
    if (why)
        *why = "auto";
    return true;
}

void load_experiment() {
    const std::wstring ini = experiment_ini();
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"experiment", L"mode", L"off", mode, 32, ini.c_str());
    // [experiment] baseline=1: mode=off whatever `mode` says (the launcher writes its own mode at every launch),
    // with the measurement reports on: ACE's own two views, for comparing what the four views cost.
    const bool baseline = GetPrivateProfileIntW(L"experiment", L"baseline", 0, ini.c_str()) != 0;
    if (baseline) {
        logf("experiment: baseline=1: running as mode=off (ini mode=%ls), measuring", mode);
        wcscpy_s(mode, L"off");
    }
    // A game known by its identity alone (no QVFR yet, games/adapter.h): log only, whatever the ini says.
    // mode=off, the session and swapchain logging on (probes), and nothing that changes what it renders.
    // A game whose QVFR runs on its own path below the layer (the SCS games' D3D11 corner) is that too, as far as the
    // layer's views4 goes: mode=off here, its QVFR the adapter's.
    const bool logOnly = !game->has_qvfr();
    const bool ownPath = logOnly && !baseline && game->own_qvfr(ini.c_str());
    if (ownPath)
        logf("experiment: %ls runs QVFR on its own path (the layer's views4 off: mode=off underneath, ini mode=%ls)",
             game->info()->name, mode);
    else if (logOnly)
        logf("experiment: %ls has no QVFR yet: log only, running as mode=off (ini mode=%ls)", game->info()->name, mode);
    if (logOnly)
        wcscpy_s(mode, L"off");
    // ACE's QVFR: the API path (state.h apiPath) unless the deprecated 4views path is asked for. It runs as mode=off
    // underneath (ACE renders its own two eyes; nothing of views4's), with the replay keys defaulting on below.
    {
        const bool ace = std::strcmp(game->info()->id, "ace") == 0;
        const bool qvfr = !baseline && !logOnly && parse_render_mode(mode) == IniRenderMode::Qvfr;
        const bool legacy = GetPrivateProfileIntW(L"experiment", L"ace_legacy_views4", 0, ini.c_str()) != 0;
        experiment.apiPath = ace && qvfr && !legacy;
        experiment.ownQvfr = ownPath || experiment.apiPath;
        experiment.cpuSettings = !baseline && (!logOnly || ownPath);  // a baseline measures the game as it runs without us
        if (experiment.apiPath) {
            log_line("experiment: ACE's QVFR: the API path (its own two eyes, the focus views replayed at the D3D12 level; "
                     "ace_legacy_views4=1 for the old 4views path)");
            wcscpy_s(mode, L"off");
        } else if (ace && qvfr) {
            log_line("experiment: ACE's QVFR: the legacy 4views path (ace_legacy_views4=1); DEPRECATED, to be removed in a "
                     "later release: the API path is the default");
        }
    }
    experiment.focusOnly = _wcsicmp(mode, L"focus_only") == 0;
    experiment.doubleRender = _wcsicmp(mode, L"double_render") == 0;
    experiment.views3 = _wcsicmp(mode, L"views3") == 0;
    experiment.views4 = parse_render_mode(mode) == IniRenderMode::Qvfr;
    wcscpy_s(experiment.mode, mode);
    experiment.views4ShowCopies = GetPrivateProfileIntW(L"experiment", L"views4_show_copies", 0, ini.c_str()) != 0;
    experiment.views4Compose = experiment.views4 && !experiment.views4ShowCopies &&
                               GetPrivateProfileIntW(L"experiment", L"views4_compose", 0, ini.c_str()) != 0;
    if (experiment.views4Compose && !game->qvfr_composable()) {
        experiment.views4ComposeD3D11 = game->qvfr_compose_d3d11();
        if (experiment.views4ComposeD3D11)
            logf("experiment: %ls is composed by the D3D11 compositor",
                 game->info()->name);
        else
            logf("experiment: %ls can't be composed yet: views4_compose=0, the headset shows views 0-1", game->info()->name);
        experiment.views4Compose = false;
    }
    // focus_layer=1: the focus views as a layer of their own over the periphery, nothing composed at native size
    // (experiment.focusLayer); 2: as two quad layers, one per eye. A D3D11 game with separate views only (LMU) for now.
    experiment.focusLayer =
        static_cast<int>(std::min(2u, GetPrivateProfileIntW(L"experiment", L"focus_layer", 0, ini.c_str())));
    if (experiment.focusLayer && !(experiment.views4ComposeD3D11 && game->qvfr_separate_views())) {
        logf("experiment: focus_layer ignored: %ls is not a D3D11 game with separate views (LMU)", game->info()->name);
        experiment.focusLayer = 0;
    } else if (experiment.focusLayer) {
        logf("experiment: focus_layer=%d: the periphery submitted as the game rendered it, the focus views over it as %s "
             "(feather in alpha); composed as before with the helmet on",
             experiment.focusLayer,
             experiment.focusLayer == 2 ? "a quad layer per eye, 100 m out" : "a second projection layer");
    }
    // native_quad_views (auto by default): the runtime composes the four views if it offers quad views
    // (layer/native_quad.cpp). The instance already asked for the extension (native_quad_wanted); here only whether this
    // game can use it (one without the feature: silently not). A quad view session takes the focus views as views 2-3,
    // which focus_layer's own layers then give way to (views4_submit).
    experiment.nativeQuadViews = native_quad_wanted(nullptr) && game->info()->features.nativeQuadViews &&
                                 experiment.views4ComposeD3D11 && game->qvfr_separate_views();
    // A game with separate views (LMU) keeps its mask on the eyes; its adapter leaves it out of the focus views.
    experiment.views4NoMask =
        experiment.views4 &&
        GetPrivateProfileIntW(L"experiment", L"views4_mask",
                              experiment.composing() && !game->qvfr_separate_views() ? 0 : 1, ini.c_str()) == 0;
    experiment.views4TestVisibleMask =
        experiment.views4 && GetPrivateProfileIntW(L"experiment", L"views4_test_visible_mask", 0, ini.c_str()) != 0;
    experiment.views4TestHiddenMask = experiment.views4 ? std::clamp(static_cast<int>(GetPrivateProfileIntW(
                                                              L"experiment", L"views4_test_hidden_mask", 0, ini.c_str())),
                                                          0, 2)
                                                        : 0;
    experiment.views4TestMaskCut = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"experiment", L"views4_test_mask_cut", 30, ini.c_str())), 5, 45);
    experiment.views4EyeCamera =
        GetPrivateProfileIntW(L"experiment", L"views4_focus_eye_camera", 0, ini.c_str()) != 0;
    experiment.passCensus = GetPrivateProfileIntW(L"experiment", L"pass_census", 0, ini.c_str()) != 0;
    // The API path's steps: on by default there (each key still overrides), off otherwise.
    const UINT api = experiment.apiPath ? 1u : 0u;
    experiment.replayProbe =
        static_cast<int>(std::min(3u, GetPrivateProfileIntW(L"experiment", L"replay_probe", api * 3, ini.c_str())));
    experiment.replayFocusPercent = std::clamp(GetPrivateProfileIntW(L"experiment", L"replay_focus_percent", 20, ini.c_str()), 5u, 60u);
    experiment.replayFocusScalePercent =
        std::clamp(GetPrivateProfileIntW(L"experiment", L"replay_focus_scale_percent", 200, ini.c_str()), 50u, 1000u);
    experiment.replaySaveFirst = GetPrivateProfileIntW(L"experiment", L"replay_save_first", 1800, ini.c_str());
    experiment.replaySaveEvery = std::max(1u, GetPrivateProfileIntW(L"experiment", L"replay_save_every", 900, ini.c_str()));
    experiment.replaySaveCount = GetPrivateProfileIntW(L"experiment", L"replay_save_count", 0, ini.c_str());
    {
        wchar_t targets[16] = {};
        GetPrivateProfileStringW(L"experiment", L"replay_targets", api ? L"all" : L"depth", targets, 16, ini.c_str());
        experiment.replayAllTargets = _wcsicmp(targets, L"all") == 0;
        experiment.replayTiles = GetPrivateProfileIntW(L"experiment", L"replay_tiles", api, ini.c_str()) != 0;
        experiment.replayFog = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"replay_fog", api * 2, ini.c_str()));
        experiment.replayShadows = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"replay_shadows", api, ini.c_str()));
        experiment.replayTransparents =
            static_cast<int>(GetPrivateProfileIntW(L"experiment", L"replay_transparents", api, ini.c_str()));
        experiment.replayCostAb = GetPrivateProfileIntW(L"experiment", L"replay_cost_ab", 0, ini.c_str()) != 0;
        experiment.replaySky = GetPrivateProfileIntW(L"experiment", L"replay_sky", api, ini.c_str()) != 0;
        experiment.replayTyres = GetPrivateProfileIntW(L"experiment", L"replay_tyres", 1, ini.c_str()) != 0;
        experiment.replayFlagsWidth = GetPrivateProfileIntW(L"experiment", L"replay_flags_width", 1, ini.c_str()) != 0;
        experiment.replayDepthFade =GetPrivateProfileIntW(L"experiment", L"replay_depth_fade", 1, ini.c_str()) != 0;
        experiment.replayFixation = GetPrivateProfileIntW(L"experiment", L"replay_fixation", 1, ini.c_str()) != 0;
        experiment.replayDiagMode = GetPrivateProfileIntW(L"experiment", L"replay_diag", 0, ini.c_str());
        experiment.replayDiag = experiment.replayDiagMode == 1;
        experiment.replayPost = GetPrivateProfileIntW(L"experiment", L"replay_post", api, ini.c_str()) != 0;
        experiment.replayCompose = experiment.replayPost && !experiment.views4 &&
                                   GetPrivateProfileIntW(L"experiment", L"replay_compose", api, ini.c_str()) != 0;
        experiment.replayDlss = GetPrivateProfileIntW(L"experiment", L"replay_dlss", api, ini.c_str());
        experiment.replayEyeCapture = GetPrivateProfileIntW(L"experiment", L"replay_eye_capture", 0, ini.c_str()) != 0;
        experiment.replayGpuSteps = GetPrivateProfileIntW(L"experiment", L"replay_gpu_steps", 0, ini.c_str()) != 0;
        experiment.replayMaskAb = GetPrivateProfileIntW(L"experiment", L"replay_mask_ab", 0, ini.c_str()) != 0;
        experiment.replayRecordTiming =
            GetPrivateProfileIntW(L"experiment", L"replay_record_timing", 0, ini.c_str()) != 0;
        experiment.replayCullProbe = GetPrivateProfileIntW(L"experiment", L"replay_cull_probe", 0, ini.c_str()) != 0;
        experiment.replayCull = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"replay_cull", api * 2, ini.c_str()));
        {  // the API path's DLSS presets, by letter (F the cheaper older model, K, M ...); empty: ACE's own
            // [dlss] eye_preset (K or F) and focus_preset (empty: ACE's own), the headset menu's two items
            const DlssSettings player = read_dlss_settings(ini.c_str());
            // The periphery (ACE's own eyes): F. K smeared the right eye's periphery in ACE, on the game's own feature too
            // and with ACE's own jitter (2026-11-13 runs 22:39 and after), F was clean; pinned to F on the API path until
            // K's smear is explained (2026-11-14, the user: a later release).
            experiment.replayDlssEyePreset = experiment.apiPath ? 6 : player.eyePreset;
            experiment.replayDlssFocusPreset = player.focusPreset;
            experiment.replayDlssFocusScale = player.focusScale;  // the focus views' mode: DLAA 100, 67, 58, 50
            experiment.replayDlssJitterX =
                std::clamp(GetPrivateProfileIntW(L"experiment", L"replay_dlss_jitter_x_percent", 50, ini.c_str()), 10u, 200u) / 100.0;
            experiment.replayDlssPeripheryDlaa =
                GetPrivateProfileIntW(L"experiment", L"replay_dlss_periphery_dlaa", 0, ini.c_str()) != 0;
            if (experiment.replayDlss)
                logf("experiment: API path DLSS: NGX told %.0f%% of ACE's horizontal jitter; periphery DLAA in every in-game "
                     "mode %s", experiment.replayDlssJitterX * 100, experiment.replayDlssPeripheryDlaa ? "on" : "off");
            if (experiment.replayDlss && (experiment.replayDlssEyePreset || experiment.replayDlssFocusPreset))
                logf("experiment: API path DLSS presets: the eyes (periphery) %d, the focus views %d (0: ACE's own)",
                     experiment.replayDlssEyePreset, experiment.replayDlssFocusPreset);
        }
        experiment.replayAtmosphere =
            static_cast<int>(GetPrivateProfileIntW(L"experiment", L"replay_atmosphere", api * 4, ini.c_str()));
    }
    experiment.gpuFrameTiming = GetPrivateProfileIntW(L"experiment", L"gpu_frame_timing", 0, ini.c_str()) != 0;
    experiment.cpuProfile = GetPrivateProfileIntW(L"experiment", L"cpu_profile", 0, ini.c_str()) != 0;
    experiment.cpuProfileSeconds =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"cpu_profile_seconds", 20, ini.c_str())), 1, 300);
    experiment.cpuProfileThreads =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"cpu_profile_threads", 4, ini.c_str())), 1, 16);
    experiment.cpuProfileFrame = GetPrivateProfileIntW(L"experiment", L"cpu_profile_frame", 0, ini.c_str());
    experiment.cpuProfileSlowMs =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"cpu_profile_slow_ms", 0, ini.c_str())), 0, 1000);
    experiment.measure =
        GetPrivateProfileIntW(L"experiment", L"measure", 0, ini.c_str()) != 0 || experiment.cpuProfile || baseline ||
        experiment.apiPath;  // (the overlay's GPU and thread figures, as views4 has them)
    experiment.passCensusFrame = std::max(1u, GetPrivateProfileIntW(L"experiment", L"pass_census_frame", 1500, ini.c_str()));
    experiment.views4FocusCull = GetPrivateProfileIntW(L"experiment", L"views4_focus_cull", 0, ini.c_str()) != 0;
    experiment.views4FocusCullMargin =
        std::clamp(GetPrivateProfileIntW(L"experiment", L"views4_focus_cull_margin_percent", 25, ini.c_str()), 0u, 200u) /
        100.0;
    experiment.uavBarriers = GetPrivateProfileIntW(L"experiment", L"pass_census_uav_barriers", 0, ini.c_str()) != 0;
    experiment.views4DlssLive = GetPrivateProfileIntW(L"experiment", L"views4_dlss_live", 1, ini.c_str()) != 0;
    experiment.carLightAtLaunch = game_features().carLighting && read_car_lighting(ini.c_str()).on;
    experiment.textureSharpeningAtLaunch = simqvfr::read_dlss_settings(ini.c_str()).textureSharpening != 0;
    {
        wchar_t watch[512] = {};
        GetPrivateProfileStringW(L"experiment", L"pass_census_watch", L"", watch, 512, ini.c_str());
        std::snprintf(experiment.passCensusWatch, sizeof(experiment.passCensusWatch), "%ls", watch);
    }
    experiment.dred = GetPrivateProfileIntW(L"experiment", L"dred", 0, ini.c_str()) != 0;  // diagnosis only: costs GPU time
    experiment.d3d12Debug = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) != 0;
    experiment.gpuValidation = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) >= 2;
    experiment.pixCaptureFrame = GetPrivateProfileIntW(L"experiment", L"pix_capture_frame", 0, ini.c_str());
    experiment.pixCaptureHotkey = GetPrivateProfileIntW(L"experiment", L"pix_capture_hotkey", 0, ini.c_str()) != 0;
    // A diagnostic hotkey: development builds only (dev_build.h).
    experiment.logCppExceptions =
        simqvfr::kDevBuild && GetPrivateProfileIntW(L"experiment", L"log_cpp_exceptions", 0, ini.c_str()) != 0;
    experiment.replayCensusHotkey =
        simqvfr::kDevBuild && GetPrivateProfileIntW(L"experiment", L"replay_census_hotkey", 0, ini.c_str()) != 0;
    experiment.d3d11CaptureFrame = GetPrivateProfileIntW(L"experiment", L"d3d11_capture_frame", 0, ini.c_str());
    // d3d11_capture_hotkey=1: tracked from the start with a frame never reached; the frame capture hotkey arms the next.
    experiment.d3d11CaptureHotkey = GetPrivateProfileIntW(L"experiment", L"d3d11_capture_hotkey", 0, ini.c_str()) == 1;
    if (experiment.d3d11CaptureHotkey && experiment.d3d11CaptureFrame == 0)
        experiment.d3d11CaptureFrame = 0xFFFFFFFFu;
    experiment.quad = _wcsicmp(mode, L"quad") == 0;
    experiment.twoLoops = parse_render_mode(mode) == IniRenderMode::TwoLoop;  // quad too (its frame B the focus box)
    experiment.focusInB = experiment.quad || GetPrivateProfileIntW(L"experiment", L"b_focus", 0, ini.c_str()) != 0;
    const LiveSettings settings = read_live(ini.c_str());
    experiment.focusFraction = settings.focusFraction;
    experiment.focusFractionY = settings.focusFractionY;
    experiment.feather = settings.feather;
    experiment.cornerRadius = settings.cornerRadius;
    experiment.focusShape.store(focus_shape_words(settings.shape));
    // the focus mask at launch (live from then on: live_apply); a game that starts later keeps it until then
    game->qvfr_set_focus_mask(settings.focusMask, focus_area_from(focus_shape_words(settings.shape), settings.cornerRadius),
                              settings.focusMaskMargin);
    game->qvfr_set_focus_corridor(settings.focusCorridor && !settings.gaze,  // road focus, likewise (fixed foveation)
                                  settings.focusCorridorWidth, settings.focusCorridorHeight,
                                  settings.focusCorridorSpread, settings.focusCorridorBlend, settings.focusCorridorShow);
    experiment.border = settings.border;
    experiment.borderAtLaunch = settings.border;
    std::memcpy(experiment.borderColor, settings.borderColor, sizeof(experiment.borderColor));
    game->qvfr_set_look(settings.feather, settings.cornerRadius, settings.border, settings.borderColor);
    experiment.gaze = settings.gaze;
    experiment.gazeThread = GetPrivateProfileIntW(L"quad", L"gaze_thread", 1, ini.c_str()) != 0;
    experiment.views4Converge = GetPrivateProfileIntW(L"experiment", L"views4_converge", 1, ini.c_str()) != 0;
    {
        wchar_t text[32] = {};
        GetPrivateProfileStringW(L"quad", L"converge_m", L"0", text, 32, ini.c_str());
        experiment.convergeM = std::clamp(_wtof(text), 0.0, 1000.0);
    }
    if (experiment.views4 || experiment.replayCompose || game->wants_gaze())
        logf("experiment: the focus boxes converge %s%s", experiment.views4Converge ? "at the game's fixation distance" : "",
             experiment.convergeM > 0
                 ? (std::string(experiment.views4Converge ? ", else at " : "at ") + std::to_string(experiment.convergeM) +
                    " m ([quad] converge_m)").c_str()
                 : (experiment.views4Converge ? " where the game gives one, else parallel" : "nowhere: parallel (views4_converge=0)"));
    experiment.gazePollHz =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"quad", L"gaze_poll_hz", 250, ini.c_str())), 30, 1000);
    experiment.fixedOffsetY = settings.offset.fixed;
    experiment.gazeOffsetY = settings.offset.gaze;
    experiment.fixedOffsetX = settings.offset.fixedX;
    experiment.gazeOffsetX = settings.offset.gazeX;
    experiment.gazeDeadzone = settings.gazeDeadzone;
    experiment.focusQuadDistance = settings.focusQuadDistance;
    experiment.sharpen = settings.sharpen;
    experiment.textureMatch = settings.textureMatch;
    experiment.colorGrade = color_grade_bits(settings.color);
    set_car_lighting(settings.carLight);
    experiment.outputDensityMax = settings.outputDensityMax;
    set_eye_off(settings.eyeOff);
    // Applied on the render thread at its next xrEndFrame; nothing in a baseline or a log-only run.
    experiment.cpuIsolate = experiment.cpuSettings ? settings.cpuIsolate : 0;
    cpu_isolation_mode(experiment.cpuIsolate);
    if (settings.cpuIsolate > 1)
        logf("experiment: cpu_isolate=%d (probe/cpu_isolation.h): an A/B of %d frames a side", settings.cpuIsolate,
             settings.cpuIsolate);
    else if (!experiment.cpuSettings)
        log_line("cpu: the game's CPU scheduling left as it is (a baseline or log-only run)");
    if (settings.eyeOff != kEyeOffNone)
        logf("experiment: %s ([quad] eye_off: that eye shows black)", eye_off_name(settings.eyeOff));
    live.ini = ini;
    follow_language(ini.c_str());  // before anything shows text (the notice card, the menu)
    live.restartKeys = restart_keys(ini.c_str(), experiment.views4);
    live.menuPlacement = read_menu_placement(ini.c_str());
    hotkeys_changed(read_hotkeys(ini.c_str()));
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &attributes))
        live.written = attributes.ftLastWriteTime;
    experiment.captureEverySeconds = GetPrivateProfileIntW(L"quad", L"capture_every_s", 0, ini.c_str());
    // Diagnostics that write pictures by themselves, time the GPU or change the picture to measure it run only in a
    // Detailed logs launch (the launcher's Diagnostics pane: SIM_QVFR_LOG_LEVEL=debug), whatever the ini says. Captures
    // the player asks for with a hotkey stay.
    if (!log_debug()) {
        const bool any = experiment.replaySaveCount || experiment.captureEverySeconds || experiment.replayEyeCapture ||
                         experiment.replayGpuSteps || experiment.replayMaskAb || experiment.replayCullProbe ||
                         experiment.replayCull == 3 ||
                         experiment.replayCostAb || experiment.replayRecordTiming ||
                         experiment.pixCaptureFrame || (experiment.d3d11CaptureFrame && !experiment.d3d11CaptureHotkey);
        experiment.replaySaveCount = 0;
        experiment.captureEverySeconds = 0;
        experiment.replayEyeCapture = experiment.replayGpuSteps = experiment.replayCullProbe = experiment.replayCostAb = false;
        experiment.replayMaskAb = false;
        experiment.replayRecordTiming = false;
        if (experiment.replayCull == 3)  // the alternating A/B: the culling simply on
            experiment.replayCull = 2;
        experiment.pixCaptureFrame = 0;
        if (!experiment.d3d11CaptureHotkey)
            experiment.d3d11CaptureFrame = 0;
        if (any)
            log_line("experiment: diagnostics set in the ini (automatic captures, GPU step timing, A/B probes) are off: they "
                     "run only in a Detailed logs launch (the launcher's Diagnostics pane)");
    }
    experiment.probes = logOnly || GetPrivateProfileIntW(L"experiment", L"probes", 0, ini.c_str()) != 0;
    experiment.hangWatchSeconds = GetPrivateProfileIntW(L"experiment", L"hang_watch_s", game->hang_watch_s(), ini.c_str());
    // In quad mode this is the outer view's resolution; at most 100 (the shared table's range). Only the modes that render
    // through it scale what ACE is offered: with mode=off (baseline=1 too) ACE gets the runtime's own size, the
    // resolution the user set before launch (the launcher writes eye_scale for Two-loop and QVFR's fallback,
    // so a baseline run got 30% of it, 2026-11-03).
    const int eyePercent = ini_int(ini.c_str(), kEyeScale);
    const bool scaledMode = experiment.views4 || experiment.twoLoops || experiment.focusOnly || experiment.doubleRender ||
                            experiment.views3;
    experiment.eyeScale = scaledMode ? eyePercent / 100.0 : 1.0;
    // baseline_eye_scale=N (measurement only, baseline=1): the game's own two views at N% of the runtime's eye size,
    // for a baseline the native size can't run (ACE at a Pimax's native size runs out of VRAM).
    const UINT baselinePercent = GetPrivateProfileIntW(L"experiment", L"baseline_eye_scale", 0, ini.c_str());
    if (baseline && baselinePercent) {
        experiment.eyeScale = std::clamp(baselinePercent, 10u, 200u) / 100.0;
        logf("experiment: baseline_eye_scale=%u: the game's own two views at %.0f%% of the runtime's eye size",
             baselinePercent, experiment.eyeScale * 100);
    } else if (!scaledMode && eyePercent != 100)
        logf("experiment: mode=off: the game gets the runtime's own eye size (eye_scale=%d applies to the other modes)",
             eyePercent);
    // views4: periphery and focus resolution each on their own. This sizes ACE's target, the most
    // either can be live.
    const Views4Sizes sizes = views4_sizes(ini.c_str(), experiment.focusFraction, experiment.focusFractionY);
    const FovCrop launchCrop = experiment.views4 || experiment.replayCompose ? settings.crop : FovCrop{};
    // The API path (replay_compose): ACE's own two eyes are the periphery, at [quad] periphery_scale of the native eye
    // (set at launch: ACE sizes its targets once); the focus views take focus_percent and focus_density.
    if (experiment.replayCompose) {
        experiment.eyeScale = sizes.outer;
        logf("experiment: API path: ACE's two eyes at the periphery's %.0f%% of the native eye; focus views %.0f%% x %.0f%% "
             "of the native eye (box %.0f%%, density from focus_density)",
             sizes.outer * 100, sizes.focus * 100, sizes.focus * 100, experiment.focusFraction.load() * 100);
    }
    experiment.set_crop(launchCrop);
    helmetSettings = experiment.composing() ? settings.helmet : HelmetSettings{};  // before any session (either compositor)
    experiment.views4Layout = views4_layout(sizes.outer, sizes.focus, sizes.focusHeight, launchCrop.kept_width(),
                                            launchCrop.kept_height());
    // A game's copies gate (GameAdapter::qvfr_copies_layout): views 2-3 at the outer views' size, so each can be compared
    // with its eye pixel for pixel (the game gives them the eyes' cameras).
    if (experiment.views4 && game->qvfr_copies_layout(ini.c_str())) {
        const Views4Layout& o = experiment.views4Layout;
        experiment.views4Layout =
            views4_layout(sizes.outer, o.outer_width(), o.outer_height(), launchCrop.kept_width(), launchCrop.kept_height());
        logf("experiment: %ls's copies gate: views 2-3 at the outer views' size (%.0f%% x %.0f%% of the native eye)",
             game->info()->name, o.outer_width() * 100, o.outer_height() * 100);
    }
    // A game whose views are separate images (LMU) draws its menus into the whole of each: views4_menu_scale makes the
    // views at least that share of the native eye (100: menus as sharp as without QVFR), the scene rendering inside them
    // at the periphery and focus sizes (live resolution, from the first frame). Default 0 (the views' own size): in LMU
    // native-size targets cost ~3 ms of GPU per frame even with the scene in a corner of them (2026-11-08 runs).
    if (experiment.views4 && game->qvfr_separate_views()) {
        const UINT menuPercent = std::min(GetPrivateProfileIntW(L"experiment", L"views4_menu_scale", 0, ini.c_str()), 100u);
        experiment.views4Layout.layerFloor = menuPercent / 100.0;
        logf("experiment: views4_menu_scale=%u: the views are made at least %u%% of the native eye for the game's menus%s",
             menuPercent, menuPercent, menuPercent ? "; the scene renders inside them at the views' sizes" : " (off)");
        // Room for each focus view beside its eye: multi-view rendering (NVIDIA's on an RTX 20 series or newer,
        // instancing on any other GPU: games/multi_view.h).
        std::string why;
        const bool multiView = games::multi_view_on(games::current_game(), ini.c_str(), &why);
        if (multiView)
            logf("multi_view: on, %s: each eye's pass draws its focus view too", why.c_str());
        experiment.views4Layout.secondViewRoom = multiView;
        // lmu_mvr_beside (default 1): the images the outer views' shape, the focus views at their own width and height
        // beside them (Views4Layout::beside_scale); 0: twice each way.
        experiment.views4Layout.secondViewBeside =
            experiment.views4Layout.secondViewRoom &&
            GetPrivateProfileIntW(L"experiment", L"lmu_mvr_beside", 1, ini.c_str()) != 0;
        if (experiment.views4Layout.secondViewRoom)
            logf("multi_view: the views' images %s, room for a focus view beside its eye",
                 experiment.views4Layout.secondViewBeside ? "the outer views' shape" : "twice as large each way");
    }
    // Eye columns (ACC): each instanced pair its own width, unless acc_shared_width=1 (every view the periphery's width,
    // the focus views' horizontal density following it: the layout before 2026-11-08).
    experiment.views4Layout.sharedColumnWidth =
        GetPrivateProfileIntW(L"experiment", L"acc_shared_width", 0, ini.c_str()) != 0;
    if (experiment.views4 && experiment.views4Layout.sharedColumnWidth && game->qvfr_eye_columns())
        logf("experiment: acc_shared_width=1: every view the periphery's width (the focus views' horizontal density "
             "follows the periphery)");
    // For the launcher: what live sizes can reach (percent of the native eye).
    runtime_info(L"launch_mode", mode);
    runtime_info(L"launch_outer", std::to_wstring(std::lround((experiment.views4 ? sizes.outer : experiment.eyeScale) * 100)));
    runtime_info(L"launch_focus_width", std::to_wstring(std::lround(sizes.focus * 100)));
    runtime_info(L"launch_focus_height", std::to_wstring(std::lround(sizes.focusHeight * 100)));
    {
        const FovCrop& c = launchCrop;
        const auto percent = [](double share) { return std::to_wstring(std::lround(share * 100)); };
        runtime_info(L"launch_crop",
                     percent(c.top) + L"," + percent(c.bottom) + L"," + percent(c.outer) + L"," + percent(c.inner));
    }
    logf("experiment: mode=%ls focus_percent=%.0f b_focus=%d eye_scale=%d feather_percent=%.0f corner_radius_percent=%.0f "
         "debug_border=%d gaze=%d fixed_offset_y=%d gaze_offset_y=%d fixed_offset_x=%d gaze_offset_x=%d (from %ls)",
         mode, experiment.focusFraction * 100, experiment.focusInB ? 1 : 0, eyePercent, experiment.feather * 100,
         experiment.cornerRadius * 100,
         experiment.border ? 1 : 0, experiment.gaze ? 1 : 0, settings.offset.fixed, settings.offset.gaze,
         settings.offset.fixedX, settings.offset.gazeX, ini.c_str());
    logf("experiment: focus shape %ls", focus_shape_text(settings.shape).c_str());
    if (game_features().peripheryTextureMatch)
        logf("experiment: periphery texture match %d%%%s", settings.textureMatch, settings.textureMatch ? "" : " (off)");
    if (game_features().sharpening)
        logf("experiment: sharpening %d%%%s", settings.sharpen, settings.sharpen ? " (CAS on the focus views as they are composed)" : " (off)");
    if (game_features().sharpening)
        logf("experiment: colour correction %s%s", color_grade_log(settings.color).c_str(),
             settings.color.active() && !experiment.views4 ? " (not applied: QVFR only)" : "");
    if (experiment.composing())
        logf("experiment: output density up to %d%% of the native eye%s", settings.outputDensityMax,
             settings.outputDensityMax <= 100 ? " (composed at native size, [quad] output_density_max=100)"
                                              : " (the focus density's texels reach the runtime; its largest image caps it)");
    if (experiment.views4)
        logf("experiment: views4 periphery %.0f%% of native, focus density %.0f%% of native, focus box %.0f%% x %.0f%% "
             "(focus views %.0f%% x %.0f%% of the native eye)%s",
             sizes.outer * 100, sizes.density * 100, experiment.focusFraction * 100, experiment.focus_y() * 100,
             sizes.focus * 100, sizes.focusHeight * 100, sizes.fromEyeScale ? " (from eye_scale)" : "");
    if (launchCrop.on())
        logf("experiment: FOV crop top %.0f%%, bottom %.0f%%, outer %.0f%%, inner %.0f%%: outer views %.0f%% x %.0f%% of "
             "the eye (%.0f%% of their pixels)",
             launchCrop.top * 100, launchCrop.bottom * 100, launchCrop.outer * 100, launchCrop.inner * 100,
             launchCrop.kept_width() * 100, launchCrop.kept_height() * 100,
             launchCrop.kept_width() * launchCrop.kept_height() * 100);
}

}  // namespace simqvfr::layer

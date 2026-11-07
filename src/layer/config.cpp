// sim-qvfr.ini at the start of a session: the experiment, the live keys, the crop, views4's sizes, runtime.ini.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include "games/game.h"
#include "ini_put.h"
#include "input/hotkeys.h"
#include "log.h"
#include "quad/focus_offset.h"
#include "quad/focus_shape.h"
#include "quad/fov_crop.h"
#include "quad/gaze_deadzone.h"
#include "quad/helmet.h"
#include "quad/settings_menu.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

// [crop] (QVFR): the sides of each eye's FOV ACE does not render (fov_crop.h), each on its own.
FovCrop read_crop(const wchar_t* ini) {
    if (GetPrivateProfileIntW(L"crop", L"enabled", 0, ini) == 0)
        return {};
    const auto side = [&](const wchar_t* key) { return static_cast<int>(GetPrivateProfileIntW(L"crop", key, 0, ini)); };
    return fov_crop(side(L"top"), side(L"bottom"), side(L"outer"), side(L"inner"));
}

}  // namespace

// What the game has beyond QVFR (games/game.h GameFeatures); anything else stays off whatever the ini says.
games::GameFeatures game_features() {
    const games::GameInfo* g = games::current_game();
    games::GameFeatures f = g ? g->features : games::GameFeatures{};
    f.liveResolution = g && game->qvfr_live_resolution();  // the adapter knows it for this run
    return f;
}

LiveSettings read_live(const wchar_t* ini) {
    LiveSettings l;
    const games::GameFeatures features = game_features();
    if (features.fovCrop)
        l.crop = read_crop(ini);
    l.helmet = read_helmet_settings(ini);
    l.helmet.on = l.helmet.on && features.helmet;
    if (features.objectDetail)
        l.objectDetail = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"object_detail", 100, ini)), 0, 100);
    l.focusFraction = std::clamp(GetPrivateProfileIntW(L"experiment", L"focus_percent", 35, ini), 10u, 100u) / 100.0;
    const UINT heightPercent = GetPrivateProfileIntW(L"experiment", L"focus_height_percent", 0, ini);
    l.focusFractionY = heightPercent ? std::clamp(heightPercent, 10u, 100u) / 100.0 : 0.0;
    l.feather = std::clamp(GetPrivateProfileIntW(L"quad", L"feather_percent", 10, ini), 0u, 50u) / 100.0;
    l.cornerRadius = std::clamp(GetPrivateProfileIntW(L"quad", L"corner_radius_percent", 0, ini), 0u, 100u) / 100.0;
    l.shape = read_focus_shape(ini);
    if (l.shape.corners)
        l.cornerRadius = 0;
    l.border = GetPrivateProfileIntW(L"quad", L"debug_border", 0, ini) != 0;
    wchar_t borderColor[16] = {};
    GetPrivateProfileStringW(L"quad", L"debug_border_color", L"green", borderColor, 16, ini);
    // Linear values; teal is #00A0A0 in sRGB.
    const float magentaColor[4] = {1, 0, 1, 1}, tealColor[4] = {0, 0.352f, 0.352f, 1};
    if (_wcsicmp(borderColor, L"magenta") == 0)
        std::memcpy(l.borderColor, magentaColor, sizeof(magentaColor));
    else if (_wcsicmp(borderColor, L"teal") == 0)
        std::memcpy(l.borderColor, tealColor, sizeof(tealColor));
    l.gaze = GetPrivateProfileIntW(L"quad", L"gaze", 1, ini) != 0;
    l.offset = read_focus_offset(ini);
    l.gazeDeadzone = read_gaze_deadzone(ini) / 10.0;
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
    const double eyeScale = std::clamp(GetPrivateProfileIntW(L"experiment", L"eye_scale", 100, ini), 10u, 200u) / 100.0;
    const UINT peripheryPercent = GetPrivateProfileIntW(L"experiment", L"periphery_scale", 0, ini);
    const UINT focusDensityPercent = GetPrivateProfileIntW(L"experiment", L"focus_density", 0, ini);
    Views4Sizes s;
    s.fromEyeScale = !peripheryPercent && !focusDensityPercent;
    s.outer = peripheryPercent ? std::clamp(peripheryPercent, 10u, 200u) / 100.0 : eyeScale;
    // The focus views' linear size against the native eye: density times the box's share of the FOV,
    // per axis. Unset, the density is eye_scale / focus_percent (equal quarters for a square box).
    s.density = focusDensityPercent ? std::clamp(focusDensityPercent, 25u, 400u) / 100.0 : eyeScale / focusFraction;
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
std::wstring layer_directory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&layer_directory), &self);
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(self, module, MAX_PATH);
    return std::filesystem::path(module).parent_path().wstring();
}

void load_experiment() {
    // A launcher passes its per-launch settings in SIM_QVFR_INI; otherwise the ini next to the DLL.
    std::wstring ini = (std::filesystem::path(layer_directory()) / L"sim-qvfr.ini").wstring();
    wchar_t env[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_INI", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH && std::filesystem::path(env).is_absolute())
        ini = env;
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
    const bool logOnly = !game->has_qvfr();
    if (logOnly)
        logf("experiment: %ls has no QVFR yet: log only, running as mode=off (ini mode=%ls)", game->info()->name, mode);
    if (logOnly)
        wcscpy_s(mode, L"off");
    experiment.focusOnly = _wcsicmp(mode, L"focus_only") == 0;
    experiment.doubleRender = _wcsicmp(mode, L"double_render") == 0;
    experiment.views3 = _wcsicmp(mode, L"views3") == 0;
    experiment.views4 = _wcsicmp(mode, L"views4") == 0;
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
    experiment.views4NoMask =
        experiment.views4 &&
        GetPrivateProfileIntW(L"experiment", L"views4_mask", experiment.composing() ? 0 : 1, ini.c_str()) == 0;
    experiment.views4EyeCamera =
        GetPrivateProfileIntW(L"experiment", L"views4_focus_eye_camera", 0, ini.c_str()) != 0;
    experiment.passCensus = GetPrivateProfileIntW(L"experiment", L"pass_census", 0, ini.c_str()) != 0;
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
        GetPrivateProfileIntW(L"experiment", L"measure", 0, ini.c_str()) != 0 || experiment.cpuProfile || baseline;
    experiment.passCensusFrame = std::max(1u, GetPrivateProfileIntW(L"experiment", L"pass_census_frame", 1500, ini.c_str()));
    experiment.views4FocusCull = GetPrivateProfileIntW(L"experiment", L"views4_focus_cull", 0, ini.c_str()) != 0;
    experiment.views4FocusCullMargin =
        std::clamp(GetPrivateProfileIntW(L"experiment", L"views4_focus_cull_margin_percent", 25, ini.c_str()), 0u, 200u) /
        100.0;
    experiment.uavBarriers = GetPrivateProfileIntW(L"experiment", L"pass_census_uav_barriers", 0, ini.c_str()) != 0;
    experiment.views4DlssLive = GetPrivateProfileIntW(L"experiment", L"views4_dlss_live", 1, ini.c_str()) != 0;
    {
        wchar_t watch[512] = {};
        GetPrivateProfileStringW(L"experiment", L"pass_census_watch", L"", watch, 512, ini.c_str());
        std::snprintf(experiment.passCensusWatch, sizeof(experiment.passCensusWatch), "%ls", watch);
    }
    experiment.dred = GetPrivateProfileIntW(L"experiment", L"dred", 0, ini.c_str()) != 0;  // diagnosis only: costs GPU time
    experiment.d3d12Debug = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) != 0;
    experiment.gpuValidation = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) >= 2;
    experiment.pixCaptureFrame = GetPrivateProfileIntW(L"experiment", L"pix_capture_frame", 0, ini.c_str());
    experiment.d3d11CaptureFrame = GetPrivateProfileIntW(L"experiment", L"d3d11_capture_frame", 0, ini.c_str());
    // d3d11_capture_hotkey=1: tracked from the start with a frame never reached; the frame capture hotkey arms the next.
    experiment.d3d11CaptureHotkey = GetPrivateProfileIntW(L"experiment", L"d3d11_capture_hotkey", 0, ini.c_str()) == 1;
    if (experiment.d3d11CaptureHotkey && experiment.d3d11CaptureFrame == 0)
        experiment.d3d11CaptureFrame = 0xFFFFFFFFu;
    experiment.quad = _wcsicmp(mode, L"quad") == 0;
    experiment.twoLoops = experiment.quad || _wcsicmp(mode, L"two_loops") == 0;
    experiment.focusInB = experiment.quad || GetPrivateProfileIntW(L"experiment", L"b_focus", 0, ini.c_str()) != 0;
    const LiveSettings settings = read_live(ini.c_str());
    experiment.focusFraction = settings.focusFraction;
    experiment.focusFractionY = settings.focusFractionY;
    experiment.feather = settings.feather;
    experiment.cornerRadius = settings.cornerRadius;
    experiment.focusShape = focus_shape_bits(settings.shape);
    experiment.border = settings.border;
    experiment.borderAtLaunch = settings.border;
    std::memcpy(experiment.borderColor, settings.borderColor, sizeof(experiment.borderColor));
    experiment.gaze = settings.gaze;
    experiment.gazeThread = GetPrivateProfileIntW(L"quad", L"gaze_thread", 1, ini.c_str()) != 0;
    experiment.gazePollHz =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"quad", L"gaze_poll_hz", 250, ini.c_str())), 30, 1000);
    experiment.fixedOffsetY = settings.offset.fixed;
    experiment.gazeOffsetY = settings.offset.gaze;
    experiment.gazeDeadzone = settings.gazeDeadzone;
    live.ini = ini;
    live.restartKeys = restart_keys(ini.c_str(), experiment.views4);
    live.menuPlacement = read_menu_placement(ini.c_str());
    hotkeys_changed(read_hotkeys(ini.c_str()));
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &attributes))
        live.written = attributes.ftLastWriteTime;
    experiment.captureEverySeconds = GetPrivateProfileIntW(L"quad", L"capture_every_s", 0, ini.c_str());
    experiment.probes = logOnly || GetPrivateProfileIntW(L"experiment", L"probes", 1, ini.c_str()) != 0;
    experiment.hangWatchSeconds = GetPrivateProfileIntW(L"experiment", L"hang_watch_s", game->hang_watch_s(), ini.c_str());
    // Above 100 supersamples (in quad mode this is the outer view's resolution). Only the modes that render
    // through it scale what ACE is offered: with mode=off (baseline=1 too) ACE gets the runtime's own size, the
    // resolution the user set before launch (the launcher writes eye_scale for Two-loop and QVFR's fallback,
    // so a baseline run got 30% of it, 2026-11-03).
    const UINT eyePercent = std::clamp(GetPrivateProfileIntW(L"experiment", L"eye_scale", 100, ini.c_str()), 10u, 200u);
    const bool scaledMode = experiment.views4 || experiment.twoLoops || experiment.focusOnly || experiment.doubleRender ||
                            experiment.views3;
    experiment.eyeScale = scaledMode ? eyePercent / 100.0 : 1.0;
    if (!scaledMode && eyePercent != 100)
        logf("experiment: mode=off: the game gets the runtime's own eye size (eye_scale=%u applies to the other modes)",
             eyePercent);
    // views4: periphery and focus resolution each on their own. This sizes ACE's target, the most
    // either can be live.
    const Views4Sizes sizes = views4_sizes(ini.c_str(), experiment.focusFraction, experiment.focusFractionY);
    const FovCrop launchCrop = experiment.views4 ? settings.crop : FovCrop{};
    experiment.set_crop(launchCrop);
    helmetSettings = experiment.composing() ? settings.helmet : HelmetSettings{};  // before any session (either compositor)
    experiment.views4Layout = views4_layout(sizes.outer, sizes.focus, sizes.focusHeight, launchCrop.kept_width(),
                                            launchCrop.kept_height());
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
    logf("experiment: mode=%ls focus_percent=%.0f b_focus=%d eye_scale=%u feather_percent=%.0f corner_radius_percent=%.0f "
         "debug_border=%d gaze=%d fixed_offset_y=%d gaze_offset_y=%d (from %ls)",
         mode, experiment.focusFraction * 100, experiment.focusInB ? 1 : 0, eyePercent, experiment.feather * 100,
         experiment.cornerRadius * 100,
         experiment.border ? 1 : 0, experiment.gaze ? 1 : 0, settings.offset.fixed, settings.offset.gaze, ini.c_str());
    logf("experiment: focus shape %ls", focus_shape_text(settings.shape).c_str());
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

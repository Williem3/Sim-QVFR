// XR_APILAYER_SIM_qvfr: logs what Assetto Corsa EVO asks for (views, swapchains, frame
// submission) and starts the log-only engine probes (probe/probe.cpp) at xrCreateSession. With
// mode=off nothing alters rendering; the experiments and the quad pipeline are selected in
// sim-qvfr.ini.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "cheeky/cheeky_host.h"
#include "cheeky/cheeky_settings.h"
#include "games/adapter.h"
#include "layer_chain.h"
#include "log.h"
#include "probe/d3d11_capture.h"
#include "probe/d3d12_trace.h"
#include "probe/cpu_load.h"
#include "probe/cpu_profile.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "probe/pass_census.h"
#include "probe/pix_capture.h"
#include "quad/focus_turn.h"
#include "quad/fov_crop.h"
#include "quad/helmet.h"
#include "probe/dred.h"
#include "games/game.h"
#include "probe/hang_watch.h"
#include "probe/probe.h"
#include "probe/vram_census.h"
#include "quad/compositor.h"
#include "quad/d3d11_compositor.h"
#include "quad/session_graphics.h"
#include "quad/focus_offset.h"
#include "quad/focus_shape.h"
#include "quad/gaze_deadzone.h"
#include "quad/focus_outline.h"
#include "quad/menu_panel.h"
#include "input/hotkeys.h"
#include "input/pad_input.h"
#include "profile/profiles.h"
#include "quad/osd.h"
#include "quad/qvfr_optimizer.h"
#include "quad/settings_menu.h"
#include "xr.h"

#include <dxgi1_4.h>
#include <intrin.h>

namespace simqvfr {
namespace {

constexpr char kLayerName[] = "XR_APILAYER_SIM_qvfr";
constexpr uint64_t kDetailedFrames = 4;
constexpr uint64_t kSampleEveryFrames = 1800;

struct Dispatch {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrStringToPath StringToPath = nullptr;
    PFN_xrCreateActionSet CreateActionSet = nullptr;
    PFN_xrCreateAction CreateAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets AttachSessionActionSets = nullptr;
    PFN_xrSyncActions SyncActions = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrCreateActionSpace CreateActionSpace = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrGetActionStateBoolean GetActionStateBoolean = nullptr;
    PFN_xrGetActionStateVector2f GetActionStateVector2f = nullptr;
    PFN_xrGetActionStatePose GetActionStatePose = nullptr;
    PFN_xrGetCurrentInteractionProfile GetCurrentInteractionProfile = nullptr;
    PFN_xrPathToString PathToString = nullptr;
    PFN_xrGetVisibilityMaskKHR GetVisibilityMaskKHR = nullptr;  // optional (XR_KHR_visibility_mask)
};

Dispatch next;
std::mutex chainMutex;
std::map<XrSwapchain, std::pair<uint32_t, uint32_t>> chainSizes;

// Round 5 experiment, read from sim-qvfr.ini next to the DLL (ACE starts through Steam, so
// there is no per-launch environment): [experiment] mode=focus_only narrows every located view to a
// centred focus box. ACE builds its eye cameras and its submitted projection views from these FOVs.
struct Experiment {
    bool focusOnly = false;
    bool doubleRender = false;    // round 6: render_frame runs twice per frame, identical cameras
    bool views3 = false;          // single-frame quad views step 1: VR runs the renderer's 3-view mode
    bool views4 = false;          // QVFR: 4 views in one game frame (the game's adapter, games/adapter.h)
    wchar_t mode[32] = L"off";    // [experiment] mode in effect (baseline=1: off), for the game's adapter
    bool views4ShowCopies = false;         // views4 debug: submit quarters 2-3 (the copies) instead of 0-1
    bool views4Compose = false;            // views4: blend the focus views into the outer views (native-size output)
    bool views4ComposeD3D11 = false;       // views4_compose for a D3D11 game (qvfr_compose_d3d11; V4D3D11 below)
    bool views4NoMask = false;             // views4: empty hidden-area mesh (default when composing)
    bool views4EyeCamera = false;          // views4: focus views render the eyes' own cameras (diagnostic)
    bool passCensus = false;              // log a per-pipeline draw/dispatch census (probe/pass_census.h)
    UINT d3d11CaptureFrame = 0;           // D3D11 games: save each swapchain's image from this frame (probe/d3d11_capture.h)
    bool d3d11CaptureHotkey = false;      // D3D11 games: the frame capture hotkey saves the next frame's images
    bool views4FocusCull = false;          // views4: focus views skip draws outside their frustum (views4.h)
    double views4FocusCullMargin = 0.25;   // focus cull: extra extent beyond the feather band (gaze latency)
    bool views4DlssLive = true;            // live resolution below DLSS (views4.h); views4_dlss_live=0 holds the sizes
    bool uavBarriers = false;              // diagnostic: UAV barrier after every dispatch (probe/pass_census.h)
    bool gpuFrameTiming = false;           // views4: timestamps time ACE's frame on its queue (probe/gpu_frame_timer.h)
    // CPU sampling profiler (probe/cpu_profile.h): on (Ctrl+Alt+C starts a capture), seconds per capture, the
    // busiest threads it samples (and the render thread), and the frame a capture starts at by itself (0: none).
    bool cpuProfile = false;
    // [experiment] measure=1: the CPU and pacing reports (views4 cpu/pacing/load/threads lines) and the load
    // samplers in any mode, so a mode=off run measures what ACE costs without the four views. Always on in
    // views4; cpu_profile=1 turns it on too (its thread choice needs the load sampler).
    bool measure = false;
    int cpuProfileSeconds = 20, cpuProfileThreads = 4;
    uint64_t cpuProfileFrame = 0;
    UINT passCensusFrame = 1500;           // the census counts xrEndFrames from this one, for 10 frames
    char passCensusWatch[512] = "";        // the census logs stacks of these pipelines' draws and dispatches (comma-separated)
    bool dred = false;                     // D3D12 DRED report on device removal (dred=1; off by default, costs GPU time)
    bool d3d12Debug = false;               // D3D12 debug layer, errors into the log (slow; diagnosis only)
    bool gpuValidation = false;            // d3d12_debug=2: plus GPU-based validation (very slow)
    UINT pixCaptureFrame = 0;              // PIX GPU capture from this xrEndFrame (probe/pix_capture.h), 0 = off
    bool twoLoops = false;        // round 7: two complete ACE frames per display frame
    bool focusInB = false;        // round 8: two_loops frame B renders the focus FOV (A keeps the full FOV)
    double eyeScale = 1.0;        // round 8: scales the recommended eye size ACE sees (linear)
    Views4Layout views4Layout;    // views4: outer views at periphery_scale, focus views at focus_density x focus_percent
    // views4: [crop], the FOV ACE renders the eyes with (fov_crop.h); live (set_crop). The launch crop sizes
    // ACE's target (views4Layout's cropWidth and cropHeight).
    std::atomic<double> cropTop = 0, cropBottom = 0, cropOuter = 0, cropInner = 0;
    FovCrop crop() const { return {cropTop, cropBottom, cropOuter, cropInner}; }
    void set_crop(const FovCrop& c) {
        cropTop = c.top;
        cropBottom = c.bottom;
        cropOuter = c.outer;
        cropInner = c.inner;
    }
    // Atomic: live settings (live_reload), changed between frames while other threads read them.
    std::atomic<double> focusFraction = 0.35;  // of the view's tangent-space width (focus_percent)
    std::atomic<double> focusFractionY = 0;    // of its height (focus_height_percent); 0 = the same as the width
    double focus_y() const {
        const double y = focusFractionY;
        return y > 0 ? y : focusFraction.load();
    }
    // The focus box as a share of the view ACE renders: focus_percent is of the uncropped eye.
    double box_x(double fraction) const { return cropped_share(fraction, crop().kept_width()); }
    double box_y(double fraction) const { return cropped_share(fraction, crop().kept_height()); }
    double box_x() const { return box_x(focusFraction); }
    double box_y() const { return box_y(focus_y()); }
    // QVFR composes the views itself, by either compositor (views4_compose: ACE's D3D12 one; views4ComposeD3D11).
    bool composing() const { return views4Compose || views4ComposeD3D11; }
    bool quad = false;            // quad pipeline: two_loops + b_focus, composited into one native-size image
    std::atomic<double> feather = 0.10;    // quad: blend band, as a fraction of the focus box
    std::atomic<double> cornerRadius = 0;  // rounded focus box: radius as a share of half its shorter side (0 = square)
    std::atomic<uint64_t> focusShape = 0;  // the focus area's pulled-in corners (focus_shape.h's bits; 0 = the box)
    std::atomic<bool> border = false;      // quad: debug outline (borderColor) on the focus box (views4: on the outer views)
    bool borderAtLaunch = false;           // views4 without composition tracks the outline's chains only if on at launch
    float borderColor[4] = {0, 1, 0, 1};   // debug outline colour (linear RGBA): debug_border_color green|magenta|teal
    std::atomic<bool> gaze = true;         // quad: the focus box follows XR_EXT_eye_gaze_interaction
    // [quad] gaze_thread: the gaze is looked up on the layer's own thread, not inside ACE's xrLocateViews
    // (a lookup took ~1.5 ms of ACE's render thread every frame on the Pimax runtime, 2026-11-03).
    bool gazeThread = true;
    int gazePollHz = 250;
    // The focus box's vertical offset, degrees up (focus_offset.h): fixed, and added to a tracked gaze.
    std::atomic<int> fixedOffsetY = 0, gazeOffsetY = 0;
    // [quad] gaze_deadzone: the focus box holds still while the gaze stays this many degrees of where it
    // aims, so tracker noise does not turn the focus cameras every frame (MSAA shimmer in the box).
    std::atomic<double> gazeDeadzone = 1.5;
    UINT captureEverySeconds = 0; // quad debug: saves A, B and the output (left eye) as PNGs, 0 = off
    bool probes = true;           // engine probes and D3D12 trace (log-only reverse-engineering aids)
    UINT hangWatchSeconds = 30;   // no frame this long after xrCreateSession: modules and a minidump (probe/hang_watch.h;
                                  // default: the game's hang_watch_s)
};
Experiment experiment;
// The game this process is (games/adapter.h): its QVFR and its own experiments. Elsewhere one that does nothing.
games::GameAdapter* game = &games::adapter_for(nullptr);
bool focusLogged = false;
// The helmet overlay ([helmet], views4 composing; helmet.h): set at launch, then live under quad.mutex.
HelmetSettings helmetSettings;

// The settings a running game takes from its ini without a restart (live_reload): the focus box,
// its blend and outline, gaze and (views4) the FOV crop and the helmet overlay. Resolution is fixed at launch: ACE sizes its render target once,
// from the eye size offered at session start, so the focus views keep their pixels and a live box
// size changes their density.
struct LiveSettings {
    double focusFraction = 0.35, focusFractionY = 0, feather = 0.10, cornerRadius = 0;
    FocusShape shape;  // focus_shape.h; with corners the box's corner radius is not used
    bool border = false, gaze = true;
    FocusOffset offset;
    double gazeDeadzone = 1.5;  // degrees
    float borderColor[4] = {0, 1, 0, 1};
    FovCrop crop;  // views4 only
    HelmetSettings helmet;  // views4 composing only
    int objectDetail = 100;  // [experiment] object_detail, percent (a game with GameFeatures::objectDetail)
};

// [crop] (QVFR): the sides of each eye's FOV ACE does not render (fov_crop.h), each on its own.
FovCrop read_crop(const wchar_t* ini) {
    if (GetPrivateProfileIntW(L"crop", L"enabled", 0, ini) == 0)
        return {};
    const auto side = [&](const wchar_t* key) { return static_cast<int>(GetPrivateProfileIntW(L"crop", key, 0, ini)); };
    return fov_crop(side(L"top"), side(L"bottom"), side(L"outer"), side(L"inner"));
}

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

// views4: the outer and focus views' sizes, linear scales of the native eye (views4_layout.h), from
// periphery_scale and focus_density and the focus box. Unset, both follow eye_scale (equal quarters:
// periphery = eye_scale, focus density = eye_scale / focus_percent).
struct Views4Sizes {
    double outer = 1, density = 1, focus = 1, focusHeight = 1;
    bool fromEyeScale = false;
};

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

struct Live {
    std::wstring ini;          // the ini load_experiment read
    std::wstring restartKeys;  // restart_keys when last read
    MenuPlacement menuPlacement;  // the settings menu's place ([menu]) when last read
    FILETIME written{};        // the ini's last write time when last read
};
Live live;

// What the launcher shows before the next launch (SIM_QVFR_RUNTIME_INFO, set by the launcher): the
// headset's recommended per-eye size and whether eye tracking delivered samples. An ini file.
void runtime_info(const wchar_t* key, const std::wstring& value) {
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_RUNTIME_INFO", buffer, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(buffer) : std::wstring();
    }();
    if (!path.empty())
        WritePrivateProfileStringW(L"runtime", key, value.c_str(), path.c_str());
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

void hotkeys_changed(const Hotkeys& h);  // [hotkeys] (below)

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

// Round 7: two complete ACE frames per display frame. ACE's frames are paired A, B by its own
// xrWaitFrame calls. A gets the real xrWaitFrame/xrBeginFrame and its xrEndFrame is swallowed. B
// receives A's frame state at once (same display time, same pose), its xrBeginFrame is answered
// here and its xrEndFrame is the one the runtime sees. The runtime keeps one wait/begin/end per
// display frame. Arms only after ACE's own wait/begin/end calls ran 1:1:1 for a while (a frame
// generator above this layer breaks that), and switches on only between frames.
struct TwoLoops {
    std::mutex mutex;
    bool active = false;
    uint64_t waits = 0, begins = 0, ends = 0;  // app calls since session start
    uint64_t pairedWaits = 0, pairedBegins = 0, pairedEnds = 0;
    XrFrameState pendingState{XR_TYPE_FRAME_STATE};
    LARGE_INTEGER frequency{}, aWaitReturned{}, bWaitReturned{};
    double aMs = 0, bMs = 0, displayMs = 0;
    uint64_t pairs = 0;
    std::atomic<bool> inB{false};  // between B's xrWaitFrame and B's xrEndFrame
    LARGE_INTEGER lastReport{};
    bool broken = false;
};
TwoLoops loops;
constexpr uint64_t kArmAfterFrames = 30;
constexpr uint64_t kMaxFallbackRun = 90;  // ~1-2 s of display frames

double ms_between(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(loops.frequency.QuadPart);
}

// Narrows [lo, hi] (angles) to `fraction` of its tangent-space width, centred on the tangent
// `centre` (0 = straight ahead) and shifted inwards where the view ends.
void narrow(float& loAngle, float& hiAngle, double fraction, double centre = 0.0) {
    const double lo = std::tan(loAngle), hi = std::tan(hiAngle);
    const double half = (hi - lo) * fraction / 2;
    centre = std::clamp(centre, lo + half, hi - half);
    loAngle = static_cast<float>(std::atan(centre - half));
    hiAngle = static_cast<float>(std::atan(centre + half));
}

// Eye gaze for the quad focus box (XR_EXT_eye_gaze_interaction, which ACE enables itself). The
// layer has its own action set with one gaze pose action. OpenXR allows one attach per session and
// replaces a profile's suggested bindings on every call, so the set rides along in ACE's own calls:
// its gaze binding is added to ACE's suggestion for the eye gaze profile, and the set is added to
// ACE's xrAttachSessionActionSets and xrSyncActions.
// The same action set also carries the settings menu's controller actions (menuToggle: open or
// close, menuNavigate: select and step), bound for the common controller profiles next to ACE's own.
struct Gaze {
    std::mutex mutex;
    XrInstance instance = XR_NULL_HANDLE;
    XrActionSet set = XR_NULL_HANDLE;
    XrAction action = XR_NULL_HANDLE;
    XrAction menuToggle = XR_NULL_HANDLE, menuNavigate = XR_NULL_HANDLE;
    XrPath profile = XR_NULL_PATH, pose = XR_NULL_PATH;
    // The layer's bindings per interaction profile (the eye gaze one and the controllers'), and which
    // were suggested, merged into ACE's suggestion or on their own at attach.
    std::map<XrPath, std::vector<XrActionSuggestedBinding>> bindings;
    std::map<XrPath, bool> profileSuggested;
    bool suggested = false, attached = false;
    uint64_t appSyncs = 0;
    XrSpace space = XR_NULL_HANDLE;
    XrVector3f direction{0, 0, -1};  // last tracked gaze, in the space ACE locates its views in
    XrTime directionTime = 0;
    uint64_t queries = 0, tracked = 0;
    bool firstLogged = false;
    // The game's own gaze pose action (from its eye gaze suggestion; X-Plane binds one), and a space of it: the
    // lookups fall back to it while the layer's own action never tracks (its set not synced or not active).
    XrAction gameAction = XR_NULL_HANDLE;
    XrSpace gameSpace = XR_NULL_HANDLE;
    bool syncFailLogged = false, syncLogged = false;
};
Gaze gaze;
constexpr XrDuration kGazeHoldNs = 500'000'000;  // keep the last gaze this long, then recentre

// The game's head-locked reference spaces (XR_REFERENCE_SPACE_TYPE_VIEW, from its xrCreateReferenceSpace).
// OpenComposite (ACC) locates its views twice a frame: in the scene's space for rendering and in its VIEW
// space for the eyes-to-head transforms. The gaze is looked up in the space a call locates in and held there
// (steady_gaze), so taking it from both mixed two frames: the gaze thread's result from one space was used
// for the other, and the box jumped by the head's turn whenever the two came apart (every few seconds,
// whatever the dead-zone; 2026-11-04). The gaze now follows only the calls in a scene space while there
// are any; a game that locates only in VIEW space still gets its gaze there.
struct HeadSpaces {
    std::mutex mutex;
    std::vector<XrSpace> view;
    XrTime sceneLocated = 0;  // display time of the last xrLocateViews in a space that is not head-locked
    bool skipLogged = false;
};
HeadSpaces headSpaces;
constexpr XrDuration kSceneLocateRecentNs = 1'000'000'000;

// True when the gaze is not taken from this xrLocateViews: it locates in a head-locked space while the
// game also locates in a scene space.
bool gaze_skips_space(XrSpace space, XrTime time) {
    std::lock_guard guard(headSpaces.mutex);
    const bool headLocked = std::find(headSpaces.view.begin(), headSpaces.view.end(), space) != headSpaces.view.end();
    if (!headLocked) {
        headSpaces.sceneLocated = time;
        return false;
    }
    const XrDuration since = time - headSpaces.sceneLocated;
    const bool skip = headSpaces.sceneLocated != 0 && since > -kSceneLocateRecentNs && since < kSceneLocateRecentNs;
    if (skip && !headSpaces.skipLogged) {
        headSpaces.skipLogged = true;
        log_line("gaze: the game also locates its views in a head-locked (VIEW) space; the gaze follows only its "
                 "scene-space calls");
    }
    return skip;
}

XrResult XRAPI_CALL layerCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    const XrResult r = next.CreateReferenceSpace(session, info, space);
    if (XR_SUCCEEDED(r) && info && space && info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
        std::lock_guard guard(headSpaces.mutex);
        headSpaces.view.push_back(*space);
    }
    return r;
}

XrResult XRAPI_CALL layerDestroySpace(XrSpace space) {
    {
        std::lock_guard guard(headSpaces.mutex);
        std::erase(headSpaces.view, space);
    }
    return next.DestroySpace(space);
}

XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    // v + 2w(u x v) + 2u x (u x v), u = q.xyz
    const XrVector3f u{q.x, q.y, q.z};
    auto cross = [](const XrVector3f& a, const XrVector3f& b) {
        return XrVector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const XrVector3f t = cross(u, v);
    const XrVector3f t2{2 * t.x, 2 * t.y, 2 * t.z};
    const XrVector3f c = cross(u, t2);
    return {v.x + q.w * t2.x + c.x, v.y + q.w * t2.y + c.y, v.z + q.w * t2.z + c.z};
}

// Controller profiles for the settings menu: left stick (or pad) click opens and closes it, the right
// stick (or pad) selects and steps. Core profiles only: one that needs an extension ACE does not
// enable (HP's) is refused. Pimax controllers report as Touch controllers.
struct MenuProfile {
    const char* profile;
    const char* toggle;
    const char* navigate;
};
constexpr MenuProfile kMenuProfiles[] = {
    {"/interaction_profiles/oculus/touch_controller", "/user/hand/left/input/thumbstick/click",
     "/user/hand/right/input/thumbstick"},
    {"/interaction_profiles/valve/index_controller", "/user/hand/left/input/thumbstick/click",
     "/user/hand/right/input/thumbstick"},
    {"/interaction_profiles/microsoft/motion_controller", "/user/hand/left/input/thumbstick/click",
     "/user/hand/right/input/thumbstick"},
    {"/interaction_profiles/htc/vive_controller", "/user/hand/left/input/trackpad/click",
     "/user/hand/right/input/trackpad"},
};

// Gaze (and menu) actions. Also with gaze off at launch, so gaze can be turned on live.
void gaze_init(XrInstance instance) {
    {
        // Handles and suggestions belong to one instance. ACE creates a short-lived instance before its
        // real one, and an action from the destroyed instance in the real one's suggestion made the
        // Pimax runtime read freed memory in xrSuggestInteractionProfileBindings (start-up crash).
        std::lock_guard guard(gaze.mutex);
        gaze.instance = XR_NULL_HANDLE;
        gaze.set = XR_NULL_HANDLE;
        gaze.action = gaze.menuToggle = gaze.menuNavigate = XR_NULL_HANDLE;
        gaze.profile = gaze.pose = XR_NULL_PATH;
        gaze.gameAction = XR_NULL_HANDLE;
        gaze.gameSpace = XR_NULL_HANDLE;
        gaze.syncFailLogged = false;
        gaze.bindings.clear();
        gaze.profileSuggested.clear();
        gaze.suggested = gaze.attached = false;
        gaze.appSyncs = 0;
        gaze.space = XR_NULL_HANDLE;
    }
    if ((!experiment.quad && !experiment.composing()) || !next.CreateActionSet)
        return;
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "simqvfr_gaze");
    strcpy_s(setInfo.localizedActionSetName, "Sim-QVFR gaze");
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(actionInfo.actionName, "gaze");
    strcpy_s(actionInfo.localizedActionName, "Gaze");
    std::lock_guard guard(gaze.mutex);
    gaze.instance = instance;
    XrResult r = next.StringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &gaze.profile);
    if (XR_SUCCEEDED(r))
        r = next.StringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &gaze.pose);
    if (XR_SUCCEEDED(r))
        r = next.CreateActionSet(instance, &setInfo, &gaze.set);
    if (XR_SUCCEEDED(r))
        r = next.CreateAction(gaze.set, &actionInfo, &gaze.action);
    if (XR_FAILED(r)) {
        logf("gaze: action setup failed (%d); focus box stays centred", r);
        gaze.set = XR_NULL_HANDLE;
        runtime_info(L"eye_tracking", L"0");
        return;
    }
    gaze.bindings[gaze.profile].push_back({gaze.action, gaze.pose});
    log_line("gaze: action set created");
    // The settings menu's controller actions; without them the menu still works from the keyboard.
    XrActionCreateInfo toggleInfo{XR_TYPE_ACTION_CREATE_INFO};
    toggleInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strcpy_s(toggleInfo.actionName, "settings_menu");
    strcpy_s(toggleInfo.localizedActionName, "Sim-QVFR settings menu");
    XrActionCreateInfo navigateInfo{XR_TYPE_ACTION_CREATE_INFO};
    navigateInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strcpy_s(navigateInfo.actionName, "settings_navigate");
    strcpy_s(navigateInfo.localizedActionName, "Sim-QVFR settings navigate");
    if (XR_FAILED(next.CreateAction(gaze.set, &toggleInfo, &gaze.menuToggle)) ||
        XR_FAILED(next.CreateAction(gaze.set, &navigateInfo, &gaze.menuNavigate))) {
        log_line("settings menu: controller actions failed; keyboard only");
        gaze.menuToggle = gaze.menuNavigate = XR_NULL_HANDLE;
        return;
    }
    for (const MenuProfile& p : kMenuProfiles) {
        XrPath profile = XR_NULL_PATH, toggle = XR_NULL_PATH, navigate = XR_NULL_PATH;
        if (XR_SUCCEEDED(next.StringToPath(instance, p.profile, &profile)) &&
            XR_SUCCEEDED(next.StringToPath(instance, p.toggle, &toggle)) &&
            XR_SUCCEEDED(next.StringToPath(instance, p.navigate, &navigate))) {
            gaze.bindings[profile].push_back({gaze.menuToggle, toggle});
            gaze.bindings[profile].push_back({gaze.menuNavigate, navigate});
        }
    }
}

// A path's name for the log ("?" when the runtime cannot say).
std::string path_name(XrInstance instance, XrPath path) {
    char name[XR_MAX_PATH_LENGTH] = "?";
    uint32_t length = 0;
    if (next.PathToString)
        next.PathToString(instance, path, sizeof(name), &length, name);
    return name;
}

XrResult XRAPI_CALL layerSuggestInteractionProfileBindings(XrInstance instance,
                                                          const XrInteractionProfileSuggestedBinding* suggested) {
    std::lock_guard guard(gaze.mutex);
    if (suggested)
        logf("input: the game suggests %u binding(s) for %s", suggested->countSuggestedBindings,
             path_name(instance, suggested->interactionProfile).c_str());
    const auto own = suggested ? gaze.bindings.find(suggested->interactionProfile) : gaze.bindings.end();
    if (!gaze.set || own == gaze.bindings.end())
        return next.SuggestInteractionProfileBindings(instance, suggested);
    std::vector<XrActionSuggestedBinding> bindings(suggested->suggestedBindings,
                                                   suggested->suggestedBindings + suggested->countSuggestedBindings);
    if (suggested->interactionProfile == gaze.profile)
        for (const XrActionSuggestedBinding& b : bindings)
            if (b.binding == gaze.pose && b.action != gaze.action)
                gaze.gameAction = b.action;
    bindings.insert(bindings.end(), own->second.begin(), own->second.end());
    XrInteractionProfileSuggestedBinding merged = *suggested;
    merged.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    merged.suggestedBindings = bindings.data();
    XrResult r = next.SuggestInteractionProfileBindings(instance, &merged);
    logf("input: added %zu layer binding(s) to the game's %u, result=%d", own->second.size(),
         suggested->countSuggestedBindings, r);
    if (XR_FAILED(r))
        return next.SuggestInteractionProfileBindings(instance, suggested);
    gaze.profileSuggested[suggested->interactionProfile] = true;
    gaze.suggested |= suggested->interactionProfile == gaze.profile;
    return r;
}

XrResult XRAPI_CALL layerAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.set || !info)
        return next.AttachSessionActionSets(session, info);
    // Profiles ACE suggested nothing for: the layer's bindings alone.
    for (const auto& [profile, bindings] : gaze.bindings) {
        if (gaze.profileSuggested[profile])
            continue;
        XrInteractionProfileSuggestedBinding own{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        own.interactionProfile = profile;
        own.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        own.suggestedBindings = bindings.data();
        const XrResult r = next.SuggestInteractionProfileBindings(gaze.instance, &own);
        gaze.profileSuggested[profile] = XR_SUCCEEDED(r);
        if (profile == gaze.profile)
            gaze.suggested = XR_SUCCEEDED(r);
        logf("input: suggested the layer's own %zu binding(s) for %s, result=%d", bindings.size(),
             path_name(gaze.instance, profile).c_str(), r);
    }
    std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
    sets.push_back(gaze.set);
    XrSessionActionSetsAttachInfo merged = *info;
    merged.countActionSets = static_cast<uint32_t>(sets.size());
    merged.actionSets = sets.data();
    XrResult r = next.AttachSessionActionSets(session, &merged);
    logf("gaze: attached with the game's %u action set(s), result=%d", info->countActionSets, r);
    if (XR_SUCCEEDED(r)) {
        gaze.attached = true;
        return r;
    }
    return next.AttachSessionActionSets(session, info);
}

// Session state and interaction profile changes, for the log (actions are only active while the session is FOCUSED).
XrResult XRAPI_CALL layerPollEvent(XrInstance instance, XrEventDataBuffer* event) {
    const XrResult r = next.PollEvent(instance, event);
    if (r == XR_SUCCESS && event) {
        if (event->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            static const char* const kStates[] = {"UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
                                                  "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
            const auto state = reinterpret_cast<const XrEventDataSessionStateChanged*>(event)->state;
            logf("session state: %s", state >= 0 && state < 9 ? kStates[state] : "?");
        } else if (event->type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            log_line("input: interaction profile changed");
        }
    }
    return r;
}

XrResult XRAPI_CALL layerSyncActions(XrSession session, const XrActionsSyncInfo* info) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.attached || !info)
        return next.SyncActions(session, info);
    ++gaze.appSyncs;
    if (!gaze.syncLogged) {  // what the game syncs: a subaction path other than null leaves that set's other actions out
        gaze.syncLogged = true;
        std::string list;
        for (uint32_t i = 0; i < info->countActiveActionSets; ++i) {
            const XrActiveActionSet& a = info->activeActionSets[i];
            char entry[64];
            std::snprintf(entry, sizeof(entry), "%s%p ", i ? ", " : "", static_cast<void*>(a.actionSet));
            list += entry;
            list += a.subactionPath == XR_NULL_PATH ? std::string("(all)") : path_name(gaze.instance, a.subactionPath);
        }
        logf("gaze: the game syncs %u action set(s): %s; next %p", info->countActiveActionSets, list.c_str(), info->next);
    }
    std::vector<XrActiveActionSet> sets(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    sets.push_back({gaze.set, XR_NULL_PATH});
    XrActionsSyncInfo merged = *info;
    merged.countActiveActionSets = static_cast<uint32_t>(sets.size());
    merged.activeActionSets = sets.data();
    const XrResult r = next.SyncActions(session, &merged);
    if (XR_FAILED(r) && !gaze.syncFailLogged) {
        gaze.syncFailLogged = true;
        logf("gaze: the game's xrSyncActions with the layer's set failed (%d; the game's %u set(s)); synced without it",
             r, info->countActiveActionSets);
    }
    return XR_SUCCEEDED(r) ? r : next.SyncActions(session, info);
}

// Gaze direction in `base` at `time`; false while gaze is not tracked (then the last tracked
// direction is used for a short while, then straight ahead).
bool gaze_direction(XrSession session, XrSpace base, XrTime time, XrVector3f& direction) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.attached)
        return false;
    if (gaze.appSyncs == 0) {  // ACE never syncs actions: sync the layer's set alone
        const XrActiveActionSet active{gaze.set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        next.SyncActions(session, &sync);
    }
    if (!gaze.space) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = gaze.action;
        spaceInfo.poseInActionSpace.orientation.w = 1;
        if (XR_FAILED(next.CreateActionSpace(session, &spaceInfo, &gaze.space)))
            return false;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    ++gaze.queries;
    const bool ok = XR_SUCCEEDED(next.LocateSpace(gaze.space, base, time, &location)) &&
                    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
    if (ok) {
        ++gaze.tracked;
        gaze.direction = rotate(location.pose.orientation, {0, 0, -1});
        gaze.directionTime = time;
        if (!gaze.firstLogged)
            logf("gaze: first tracked sample, direction %.3f,%.3f,%.3f", gaze.direction.x, gaze.direction.y,
                 gaze.direction.z);
        gaze.firstLogged = true;
        runtime_info(L"eye_tracking", L"1");
    }
    if (!ok && (gaze.directionTime == 0 || time - gaze.directionTime > kGazeHoldNs))
        return false;
    direction = gaze.direction;
    return true;
}

// The gaze thread ([quad] gaze_thread, on by default): ACE's xrLocateViews only reads the latest gaze; this
// thread looks it up (xrLocateSpace on the gaze action's space) at gaze_poll_hz, at the display time ACE last
// located its views for, in the space it located them in. The lookup can be slow (~1.5 ms on the Pimax
// runtime) and now costs ACE's render thread nothing; the gaze it gets is at most a poll period plus a
// lookup old. It runs only while ACE syncs actions itself (it does: it binds eye gaze too), so the layer
// never syncs from two threads; otherwise, or with gaze_thread=0, xrLocateViews looks the gaze up itself.
// The lookup runs outside gaze.mutex, so ACE's xrSyncActions never waits for it.
struct GazeThread {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false;
    // From xrLocateViews: what to locate against.
    XrSession session = XR_NULL_HANDLE;
    XrSpace base = XR_NULL_HANDLE;
    XrTime time = 0;
    // The thread's last result: the gaze in `base` and the display time it was located at (0: none tracked).
    XrVector3f direction{0, 0, -1};
    XrTime directionTime = 0;
    // For the log: lookups and their time since the last report.
    uint64_t lookups = 0;
    double lookupMs = 0, lookupMaxMs = 0;
    // A running thread left at exit (no xrDestroySession) would end the process in std::terminate. At a
    // process exit it has already ended, so the join returns at once.
    ~GazeThread() {
        {
            std::lock_guard guard(mutex);
            stop = true;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
    }
};
GazeThread gazeThread;

// While the layer's own gaze action has never tracked: after kGazeUntrackedLookups lookups, logs why (the lookup's
// result and flags, whether each gaze action is active), and moves the lookups to a space of the game's own gaze
// action if it bound one (X-Plane: the layer's action stayed untracked while the game's set and its binding were in
// place, 2026-11-06). From the gaze thread.
constexpr uint64_t kGazeUntrackedLookups = 500;
void gaze_untracked_check(XrSession session, bool ok, XrResult located, XrSpaceLocationFlags flags) {
    std::lock_guard guard(gaze.mutex);
    if (ok || gaze.tracked > 0 || gaze.queries + 1 != kGazeUntrackedLookups)
        return;
    const auto active = [&](XrAction action) {
        if (!action || !next.GetActionStatePose)
            return std::string("none");
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        const XrResult r = next.GetActionStatePose(session, &info, &state);
        return XR_SUCCEEDED(r) ? std::string(state.isActive ? "active" : "inactive") : "error " + std::to_string(r);
    };
    std::string eyesProfile = "?";
    XrPath eyes = XR_NULL_PATH;
    if (next.GetCurrentInteractionProfile && XR_SUCCEEDED(next.StringToPath(gaze.instance, "/user/eyes_ext", &eyes))) {
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        const XrResult r = next.GetCurrentInteractionProfile(session, eyes, &state);
        eyesProfile = XR_FAILED(r) ? "error " + std::to_string(r)
                      : state.interactionProfile == XR_NULL_PATH ? std::string("none")
                                                                 : path_name(gaze.instance, state.interactionProfile);
    }
    logf("gaze: no tracked sample in %llu lookups (xrLocateSpace %d, flags 0x%llx; the layer's gaze action %s, the "
         "game's %s; %llu game syncs; /user/eyes_ext profile %s)",
         static_cast<unsigned long long>(kGazeUntrackedLookups), located, static_cast<unsigned long long>(flags),
         active(gaze.action).c_str(), active(gaze.gameAction).c_str(), static_cast<unsigned long long>(gaze.appSyncs),
         eyesProfile.c_str());
    if (gaze.gameAction && !gaze.gameSpace) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = gaze.gameAction;
        spaceInfo.poseInActionSpace.orientation.w = 1;
        const XrResult r = next.CreateActionSpace(session, &spaceInfo, &gaze.gameSpace);
        if (XR_FAILED(r))
            gaze.gameSpace = XR_NULL_HANDLE;
        logf("gaze: lookups moved to the game's own gaze action: %d", r);
    }
}

void gaze_thread_run() {
    std::unique_lock lock(gazeThread.mutex);
    const auto period = std::chrono::microseconds(1'000'000 / std::max(1, experiment.gazePollHz));
    while (!gazeThread.stop) {
        const auto due = std::chrono::steady_clock::now() + period;
        const XrSession session = gazeThread.session;
        const XrSpace base = gazeThread.base;
        const XrTime time = gazeThread.time;
        lock.unlock();
        XrSpace space = XR_NULL_HANDLE;
        {
            std::lock_guard guard(gaze.mutex);
            if (gaze.attached && !gaze.space) {
                XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                spaceInfo.action = gaze.action;
                spaceInfo.poseInActionSpace.orientation.w = 1;
                next.CreateActionSpace(session, &spaceInfo, &gaze.space);
            }
            space = gaze.attached ? (gaze.gameSpace ? gaze.gameSpace : gaze.space) : XR_NULL_HANDLE;
        }
        if (space && base && time) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            const auto t0 = std::chrono::steady_clock::now();
            const XrResult located = next.LocateSpace(space, base, time, &location);
            const bool ok = XR_SUCCEEDED(located) &&
                            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            gaze_untracked_check(session, ok, located, location.locationFlags);
            const XrVector3f direction = rotate(location.pose.orientation, {0, 0, -1});
            bool first = false;
            {
                std::lock_guard guard(gaze.mutex);  // the counts the overlay and the log read
                ++gaze.queries;
                if (ok) {
                    ++gaze.tracked;
                    gaze.direction = direction;
                    gaze.directionTime = time;
                    first = !gaze.firstLogged;
                    gaze.firstLogged = true;
                }
            }
            if (first) {
                logf("gaze: first tracked sample (gaze thread, %d Hz), direction %.3f,%.3f,%.3f", experiment.gazePollHz,
                     direction.x, direction.y, direction.z);
                runtime_info(L"eye_tracking", L"1");
            }
            lock.lock();
            if (ok) {
                gazeThread.direction = direction;
                gazeThread.directionTime = time;
            }
            ++gazeThread.lookups;
            gazeThread.lookupMs += ms;
            gazeThread.lookupMaxMs = std::max(gazeThread.lookupMaxMs, ms);
        } else {
            lock.lock();
        }
        gazeThread.wake.wait_until(lock, due, [] { return gazeThread.stop; });
    }
}

void gaze_thread_stop() {
    std::thread thread;
    {
        std::lock_guard guard(gazeThread.mutex);
        gazeThread.stop = true;
        thread.swap(gazeThread.thread);
    }
    gazeThread.wake.notify_all();
    if (thread.joinable())
        thread.join();
    std::lock_guard guard(gazeThread.mutex);  // ready for the next session
    gazeThread.stop = false;
    gazeThread.session = XR_NULL_HANDLE;
    gazeThread.base = XR_NULL_HANDLE;
    gazeThread.time = gazeThread.directionTime = 0;
    gazeThread.direction = {0, 0, -1};
    gazeThread.lookups = 0;
    gazeThread.lookupMs = gazeThread.lookupMaxMs = 0;
}

// The gaze for ACE's xrLocateViews in views4 (Two-loop looks it up per frame itself): from the gaze thread while it can run (it is started on the first
// call), else looked up here. False while not tracked (the last tracked gaze is kept kGazeHoldNs).
bool gaze_now(XrSession session, XrSpace base, XrTime time, XrVector3f& direction) {
    bool threaded = experiment.gazeThread;
    if (threaded) {
        std::lock_guard guard(gaze.mutex);
        threaded = gaze.attached && gaze.appSyncs > 0;
    }
    if (!threaded)
        return gaze_direction(session, base, time, direction);
    std::lock_guard guard(gazeThread.mutex);
    gazeThread.session = session;
    gazeThread.base = base;
    gazeThread.time = time;
    if (!gazeThread.thread.joinable() && !gazeThread.stop) {
        gazeThread.thread = std::thread(gaze_thread_run);
        logf("gaze: looked up on the layer's own thread at %d Hz (gaze_thread)", experiment.gazePollHz);
    }
    if (gazeThread.directionTime == 0 || time - gazeThread.directionTime > kGazeHoldNs)
        return false;
    direction = gazeThread.direction;
    return true;
}

// The gaze the focus box aims at (ACE's render thread only): held while the tracked gaze stays within
// gaze_deadzone of it, so a fixation keeps the focus cameras still, and moved to the gaze at once when it
// leaves (a saccade). Held in the located space, so a fixation on the scene holds through a head turn.
struct GazeHold {
    XrVector3f direction{0, 0, -1};
    bool held = false;
    std::atomic<uint64_t> samples = 0, moves = 0;  // for the log (gaze_hold_report)
};
GazeHold gazeHold;

// `direction` (unit, from gaze_now or gaze_direction) replaced by the held gaze; `tracked` false drops the hold.
void steady_gaze(bool tracked, XrVector3f& direction) {
    if (!tracked) {
        gazeHold.held = false;
        return;
    }
    const XrVector3f& h = gazeHold.direction;
    const double cosAngle = std::clamp(static_cast<double>(h.x * direction.x + h.y * direction.y + h.z * direction.z), -1.0, 1.0);
    const double deadzone = experiment.gazeDeadzone.load(std::memory_order_relaxed) * 3.14159265358979 / 180;
    ++gazeHold.samples;
    if (!gazeHold.held || std::acos(cosAngle) > deadzone) {
        gazeHold.direction = direction;
        gazeHold.held = true;
        ++gazeHold.moves;
    }
    direction = gazeHold.direction;
}

// For the log, every report: how often the held gaze moved, of the tracked frames since the last one.
void gaze_hold_report() {
    const uint64_t samples = gazeHold.samples.exchange(0), moves = gazeHold.moves.exchange(0);
    if (samples)
        logf("gaze dead-zone %.1f deg: the focus box moved on %llu of %llu tracked frames (%.0f%%)",
             experiment.gazeDeadzone.load(), moves, samples, 100.0 * moves / samples);
}

// For the log, every report: the gaze thread's lookups since the last one.
void gaze_thread_report(double seconds) {
    std::lock_guard guard(gazeThread.mutex);
    if (!gazeThread.lookups)
        return;
    logf("gaze thread: %.0f lookups/s, xrLocateSpace %.3f ms avg, %.3f ms max (off the game's render thread)",
         seconds > 0 ? gazeThread.lookups / seconds : 0.0, gazeThread.lookupMs / gazeThread.lookups, gazeThread.lookupMaxMs);
    gazeThread.lookups = 0;
    gazeThread.lookupMs = gazeThread.lookupMaxMs = 0;
}
// Quad pipeline (mode=quad), on top of two_loops with b_focus. ACE renders frame A with the full
// FOV and frame B with the focus FOV into its own (eye_scale-sized) double-wide swapchain, which
// the runtime never sees. A's image is copied when ACE releases it; B's release is held back until
// B's xrEndFrame, where the compositor draws the outer view with the focus view blended into its
// box into this layer's own double-wide swapchain at the runtime's native size. That swapchain
// replaces ACE's projection layer, using the poses and FOVs ACE rendered frame A with.
struct EyeView {
    XrPosef pose{};
    XrFovf fov{};
    XrRect2Di rect{};
};
struct Quad {
    std::mutex mutex;
    uint32_t nativeW = 0, nativeH = 0;      // runtime's recommended eye size (before eye_scale)
    uint32_t reportedW = 0, reportedH = 0;  // what ACE was offered
    XrSwapchain appChain = XR_NULL_HANDLE;  // ACE's double-wide colour swapchain
    uint32_t appW = 0, appH = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // ACE's eye chain format, also the output's
    std::vector<ID3D12Resource*> appImages;
    uint32_t appAcquired = 0;
    bool releaseHeld = false;               // B's image, released at B's xrEndFrame
    XrSwapchain outChain = XR_NULL_HANDLE;  // this layer's double-wide output
    std::vector<ID3D12Resource*> outImages;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    QuadCompositor compositor;
    bool outerCaptured = false;
    EyeView aViews[2];
    bool aViewsValid = false;
    uint64_t composed = 0, fallbacks = 0;
    uint64_t fallbackRun = 0;  // fallbacks in a row; a long run turns the pipeline off
    // views4 composition: done at ACE's image release, submitted at xrEndFrame.
    XrFovf v4Fov[2] = {};
    FovCrop v4Crop;  // the FOV crop ACE last located with (views4_output_extent, the helmet's uncropped eye)
    std::wstring helmetPiecesTried[3];  // the pieces' images last loaded (or tried) into the compositor, HelmetPiece order
    std::filesystem::file_time_type helmetPiecesWritten[3] = {};  // their files' write times then (min: missing)
    int64_t helmetPiecesCheck = 0;  // QPC of the next look at the files (helmet_pieces)
    XrQuaternionf v4EyeTurn[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};  // each eye against the head (canted displays), last located
    XrVector3f v4EyeOffset[2] = {{-0.032f, 0, 0}, {0.032f, 0, 0}};  // each eye from the eyes' middle, in the head's frame (m)
    double visorPosition = -1;  // the visor's slide shown (0 closed .. 1 open; < 0: not yet), towards visor_target
    int64_t visorAt = 0;        // QPC of its last step
    std::wstring helmetHinted;  // the settings the last crop hint was logged for
    // The helmet's pieces in their own head-locked quad layer (helmet_layer_resources): its swapchain in
    // VIEW space, drawn at each composition, submitted over the scene. helmetLayerFailed: the pieces go
    // into the scene image instead (its whole FOV; frame generation then mismatches).
    XrSwapchain helmetChain = XR_NULL_HANDLE;
    XrSpace helmetSpace = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> helmetImages;
    std::vector<ID3D11Texture2D*> helmetImages11;  // the same, for a D3D11 session (ACC)
    bool helmetPiecesLoaded = false;  // the composing compositor holds pieces (helmet_pieces), for the menu and submit
    bool helmetLayerFailed = false, helmetDrawn = false;
    // Its quads this frame: the bands the pieces draw on (helmet_bands), one or two, from the same image.
    XrCompositionLayerQuad helmetQuads[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    int helmetQuadCount = 0;
    uint32_t helmetSide = 0;   // its swapchain's side (helmet_layer_side when made)
    bool helmetResize = false;  // [helmet] detail changed: made again at the next submission
    std::string helmetBandsLogged;
    ULONGLONG helmetLightLogAt = 0;  // when to ask the compositor for the helmet's light again (log_helmet_light)
    bool v4FovValid = false;
    XrExtent2Di v4Extent[2] = {};  // each composed eye's size in the output (views4_compose), for views4_submit
    XrFovf v4SubmitFov[2] = {};  // the FOV the composed output covers (views4_compose), for views4_submit
    XrFovf v4LoggedGameFov[2] = {};  // ACE's submitted FOV, as last logged against v4SubmitFov
    bool v4Composed = false;
    int captures = 0;
    ULONGLONG lastCapture = 0;
    bool failureLogged = false;
};
Quad quad;
// The session's graphics API and devices (session_graphics.h), under quad.mutex: the panels and compositors draw with it.
SessionGraphics sessionGraphics;
SessionGraphics session_graphics_now() {
    std::lock_guard guard(quad.mutex);
    return sessionGraphics;
}

// views4 composition for a D3D11 game (experiment.views4ComposeD3D11; ACC): its eye swapchains each hold its whole
// target, all four views in eye columns, so at xrEndFrame the layer composes both eyes from the image the
// projection layer's first view shows into a double-wide output of its own, at the runtime's native eye size,
// and submits that in its place. The game's image was released before xrEndFrame; the runtime reads it only
// after, as the layer does.
struct V4D3D11 {
    std::mutex mutex;
    D3D11Compositor compositor;  // on the session's device (sessionGraphics)
    struct Chain {
        int64_t format = 0;
        std::vector<ID3D11Texture2D*> images;
        uint32_t acquired = 0;  // the image last acquired (released by the time of xrEndFrame)
    };
    std::map<XrSwapchain, Chain> chains;  // the game's
    XrSwapchain outChain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> outImages;
    uint32_t outW = 0, outH = 0;  // one eye's size in it
    bool failed = false;          // no compositor or output chain: views 0-1 as they are
    uint64_t composed = 0, fallbacks = 0;
    const char* lastFallback = nullptr;
};
V4D3D11 v4d3d11;

// The compositor that holds the helmet's pieces and light: the D3D11 one for a D3D11 game (ACC; under
// v4d3d11.mutex), else ACE's D3D12 one (under quad.mutex).
HelmetPieceSet& helmet_set() {
    if (experiment.views4ComposeD3D11)
        return v4d3d11.compositor;
    return quad.compositor;
}

// views4 debug: the focus region's outline drawn into each outer view (debug_border), on ACE's eye
// image just before ACE releases it. The chain is the one ACE's projection layer submits.
struct Views4Outline {
    std::mutex mutex;
    std::map<XrSwapchain, std::vector<ID3D12Resource*>> images;
    std::map<XrSwapchain, int64_t> formats;
    std::map<XrSwapchain, uint32_t> acquired;
    XrSwapchain chain = XR_NULL_HANDLE;
    XrFovf fov[2] = {};  // last located, per eye
    bool fovValid = false;
    FocusOutline outline;
    bool ready = false, failed = false;
    float rgba[4] = {0, 1, 0, 1};
};
Views4Outline v4outline;

// Without composition the outline goes on ACE's outer views; with it, on the composed output.
bool views4_outline_on() {
    return experiment.views4 && experiment.borderAtLaunch && !experiment.views4ShowCopies && !experiment.composing();
}

void views4_outline_draw(XrSwapchain swapchain) {
    std::lock_guard guard(v4outline.mutex);
    if (swapchain != v4outline.chain || !v4outline.fovValid || v4outline.failed || !experiment.border)
        return;
    if (!v4outline.ready) {
        if (!quad.device || !quad.queue)
            return;
        const auto format = v4outline.formats.find(swapchain);
        if (format == v4outline.formats.end() ||
            !v4outline.outline.initialize(quad.device, quad.queue, static_cast<DXGI_FORMAT>(format->second))) {
            v4outline.failed = true;
            log_line("views4: focus outline unavailable");
            return;
        }
        v4outline.ready = true;
        std::memcpy(v4outline.rgba, experiment.borderColor, sizeof(v4outline.rgba));
        logf("views4: focus outline on, colour %.2f/%.2f/%.2f", v4outline.rgba[0], v4outline.rgba[1], v4outline.rgba[2]);
    }
    const auto images = v4outline.images.find(swapchain);
    const auto index = v4outline.acquired.find(swapchain);
    if (images == v4outline.images.end() || index == v4outline.acquired.end() || index->second >= images->second.size())
        return;
    ID3D12Resource* image = images->second[index->second];
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    // Eye e's outer view is view e of the image (views4_submit); the focus box is focus_percent of
    // the eye's tangent extent around its forward axis, as views4's focus cameras use.
    std::vector<OutlineBox> boxes;
    int thickness = 2;
    Views4Frame frame{};
    const bool recorded = game->qvfr_latest_frame(frame);  // ACE's rendered FOV, else the located one
    const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
    for (int e = 0; e < 2; ++e) {
        const ViewRect v = views4_rect(layout, e, static_cast<int>(desc.Width), static_cast<int>(desc.Height));
        float tan[4];
        if (recorded)
            std::memcpy(tan, frame.outerTan[e], sizeof(tan));
        else
            fov_tangents(v4outline.fov[e], tan);
        const double l = tan[0], r = tan[1], u = tan[2], d = tan[3];
        double hw, hh;
        focus_extents(tan, experiment.box_x(), experiment.box_y(), hw, hh);
        if (!(r > l && u > d))
            return;
        boxes.push_back(OutlineBox{v.left + static_cast<int>((-hw - l) / (r - l) * v.width),
                                   static_cast<int>((u - hh) / (u - d) * v.height),
                                   v.left + static_cast<int>((hw - l) / (r - l) * v.width),
                                   static_cast<int>((u + hh) / (u - d) * v.height)});
        thickness = std::max(2, v.width / 300);
    }
    std::memcpy(v4outline.rgba, experiment.borderColor, sizeof(v4outline.rgba));  // live (debug_border_color)
    v4outline.outline.draw(image, boxes, thickness, v4outline.rgba);
}

void quad_fail(const char* why) {
    ++quad.fallbacks;
    ++quad.fallbackRun;
    if (!quad.failureLogged)
        logf("quad: showing ACE's frame B unchanged: %s", why);
    quad.failureLogged = true;
}

// two_loops arms only once ACE's eye swapchain is adopted: unarmed, ACE renders its normal full view.
bool quad_ready() {
    std::lock_guard guard(quad.mutex);
    return quad.outChain != XR_NULL_HANDLE;
}

// Quad frame B: instead of an off-centre window of the eye's view (a strongly asymmetric projection,
// which ACE's screen-space clouds and other effects get wrong: dark skies when the box is far from
// the centre), the eye camera is turned to face tangent (cx, cy) of the eye's view, without roll,
// and given a centred projection of the same tangent size.
void turn_view(XrView& view, double fractionX, double fractionY, double tanX, double tanY) {
    const double l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
    const double d = std::tan(view.fov.angleDown), u = std::tan(view.fov.angleUp);
    const double hw = (r - l) * fractionX / 2, hh = (u - d) * fractionY / 2;
    view.pose.orientation = qmul(view.pose.orientation, focus_turn(view.fov, fractionX, fractionY, tanX, tanY));
    view.fov = {static_cast<float>(-std::atan(hw)), static_cast<float>(std::atan(hw)),
                static_cast<float>(std::atan(hh)), static_cast<float>(-std::atan(hh))};
}

// Where frame B's camera points, as tangents of frame A's view (for logs).
void focus_centre(const EyeComposition& eye, float& tanX, float& tanY) {
    // B's forward (0,0,-1) in A's frame is the transpose applied: -(row 2) = (-m6, -m7, -m8).
    tanX = -eye.rotation[6] / eye.rotation[8];
    tanY = -eye.rotation[7] / eye.rotation[8];
}

uint64_t endFrames = 0;
uint64_t locates = 0;

bool sampled(uint64_t count) { return count <= kDetailedFrames || count % kSampleEveryFrames == 0; }

// views4 CPU cost of this layer's hot calls (the focus-cull measurement): each layer entry point
// in total, and the part spent below it (runtime, VectorXR), so the layer's own share is the
// difference. Logged every kCpuReportFrames xrEndFrames next to the views4 report.
struct CpuSpan {
    std::atomic<uint64_t> ticks{0}, maxTicks{0}, calls{0};
    void add(int64_t t) {
        const auto v = static_cast<uint64_t>(t);
        ticks.fetch_add(v, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
        for (uint64_t m = maxTicks.load(std::memory_order_relaxed); v > m && !maxTicks.compare_exchange_weak(m, v);) {
        }
    }
};
struct CpuTimes {
    CpuSpan endFrame, endFrameBelow, locate, locateBelow, compose;
    PFN_xrEndFrame realEndFrame = nullptr;
    PFN_xrLocateViews realLocateViews = nullptr;
    double msPerTick = 0;
};
CpuTimes cpu;
constexpr uint64_t kCpuReportFrames = 900;

// views4 frame pacing (render thread): ACE's xrEndFrame-to-xrEndFrame interval, the time it spends in
// xrWaitFrame (the runtime holding it to the display) and the display period the runtime predicts.
// Logged with the CPU times as a "views4 pacing:" line: frames that miss their display slot wait a whole
// extra period, so the frame rate falls below the refresh even with headroom on average.
struct Pacing {
    std::vector<float> interval, wait;  // ms
    int64_t lastEnd = 0;
    double periodMs = 0;
};
Pacing pacing;

int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

XrResult XRAPI_CALL timedEndFrameBelow(XrSession session, const XrFrameEndInfo* info) {
    const int64_t t0 = qpc_now();
    const XrResult r = cpu.realEndFrame(session, info);
    cpu.endFrameBelow.add(qpc_now() - t0);
    return r;
}

XrResult XRAPI_CALL timedLocateViewsBelow(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                          uint32_t capacity, uint32_t* countOutput, XrView* views) {
    const int64_t t0 = qpc_now();
    const XrResult r = cpu.realLocateViews(session, info, state, capacity, countOutput, views);
    cpu.locateBelow.add(qpc_now() - t0);
    return r;
}

// The pacing line, and resets: ACE's frame interval and its time in and outside xrWaitFrame (p50, p90),
// and how many frames took one display period, two, three or more (rounded to the nearest). Returns the
// mean frame interval (0 without frames).
// The latest report's numbers, for the launcher's Diagnostics pane (write_session_summary).
struct SessionSummary {
    double fps = 0, frameP50 = 0, frameP90 = 0, latePercent = 0, periodMs = 0;
    double cpuMs = -1, gpuMs = -1, gpuBusy = -1, cores = -1;  // the render thread's CPU and the whole GPU per frame
    std::vector<ThreadLoad> threads;
};
SessionSummary summary;
std::string layersBelow;  // the layers between this one and the runtime (log_layer_order)
bool frame_generation_on();  // Cheeky's, now (below)

double report_pacing() {
    if (pacing.interval.empty())
        return 0;
    const auto at = [](std::vector<float> v, double p) {
        if (v.empty())
            return 0.0;
        const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return static_cast<double>(v[k]);
    };
    double sum = 0, longest = 0;
    size_t periods[4] = {};  // 1, 2, 3, 4 or more
    for (const float i : pacing.interval) {
        sum += i;
        longest = std::max(longest, static_cast<double>(i));
        if (pacing.periodMs > 0)
            ++periods[std::clamp(static_cast<int>(std::lround(i / pacing.periodMs)), 1, 4) - 1];
    }
    const double mean = sum / static_cast<double>(pacing.interval.size());
    std::vector<float> outside;  // ACE's own frame time: the interval less its wait (pairs in order)
    for (size_t i = 0; i < pacing.interval.size() && i < pacing.wait.size(); ++i)
        outside.push_back(std::max(0.0f, pacing.interval[i] - pacing.wait[i]));
    logf("views4 pacing: display period %.2f ms; ACE frame interval p50 %.2f ms, p90 %.2f, max %.2f (%.1f fps over %zu "
         "frames); in xrWaitFrame p50 %.2f ms, p90 %.2f; outside it p50 %.2f ms, p90 %.2f; frames of 1/2/3/4+ display "
         "periods %zu/%zu/%zu/%zu",
         pacing.periodMs, at(pacing.interval, 0.5), at(pacing.interval, 0.9), longest, mean > 0 ? 1000 / mean : 0,
         pacing.interval.size(), at(pacing.wait, 0.5), at(pacing.wait, 0.9), at(outside, 0.5), at(outside, 0.9),
         periods[0], periods[1], periods[2], periods[3]);
    summary.fps = mean > 0 ? 1000 / mean : 0;
    summary.frameP50 = at(pacing.interval, 0.5);
    summary.frameP90 = at(pacing.interval, 0.9);
    summary.latePercent = 100.0 * static_cast<double>(periods[1] + periods[2] + periods[3]) /
                          static_cast<double>(pacing.interval.size());
    summary.periodMs = pacing.periodMs;
    pacing.interval.clear();
    pacing.wait.clear();
    return mean;
}

bool osd_shown(OsdSummary& out);  // the overlay's last numbers (below)

// The load line, over the same 900 frames: the whole GPU's busy share (ACE's part) and so its time per frame
// (`frameMs`: the mean interval), ACE's busiest thread and the render thread's share of a core, all ACE's
// threads; and, with the overlay on, what it last showed.
void report_load(double frameMs) {
    int gpuSamples = 0, cpuSamples = 0;
    const GpuLoad gpu = gpu_load_window(gpuSamples);
    const CpuLoad threads = cpu_load_window(cpuSamples);
    summary.gpuBusy = gpu.valid ? gpu.busy : -1;
    summary.gpuMs = gpu.valid ? gpu.busy / 100 * frameMs : -1;
    summary.cpuMs = threads.valid && threads.renderPercent >= 0 ? threads.renderPercent / 100 * frameMs : -1;
    summary.cores = threads.valid ? threads.processPercent / 100 : -1;
    if (!gpu.valid && !threads.valid)
        return;
    char gpuText[192] = "GPU load unknown", cpuText[256] = "threads unknown", shown[256] = "";
    if (gpu.valid)
        std::snprintf(gpuText, sizeof(gpuText), "GPU %.0f%% busy (%ls engine; game %.0f%%): %.1f ms of GPU per game frame",
                      gpu.busy, gpu.engine.c_str(), gpu.game, gpu.busy / 100 * frameMs);
    if (threads.valid)
        std::snprintf(cpuText, sizeof(cpuText),
                      "busiest game thread %.0f%% of a core (last: %u%s%ls%s), render thread %.0f%%, all threads %.0f%% "
                      "(%d)",
                      threads.busiestPercent, threads.busiestThread, threads.busiestName.empty() ? "" : " \"",
                      threads.busiestName.c_str(), threads.busiestName.empty() ? "" : "\"", threads.renderPercent,
                      threads.processPercent, threads.threads);
    OsdSummary s{};
    if (osd_shown(s)) {
        static const char* const kBounds[] = {"unknown", "neither", "CPU", "GPU", "pacing"};
        std::snprintf(shown, sizeof(shown),
                      "; overlay: CPU %.1f ms (%.0f%% headroom), GPU %.1f ms%s (%.0f%% headroom), limited by %s",
                      std::max(s.cpuMs, s.cpuThreadMs), s.cpuHeadroom, s.gpuMs, s.gpuWhole ? " all work" : "", s.gpuHeadroom,
                      kBounds[static_cast<int>(s.bound)]);
    }
    logf("views4 load: %s; %s%s (%d and %d samples)", gpuText, cpuText, shown, gpuSamples, cpuSamples);
    // Every busy thread, not only the busiest: a share of one core each (a thread near 100% limits the frame
    // however idle the other cores are), by ACE's own thread names where it set them.
    const std::vector<ThreadLoad> threadLoads = cpu_load_threads(8);
    summary.threads = threadLoads;
    std::string list;
    for (const ThreadLoad& t : threadLoads) {
        if (t.percent < 5)
            break;
        char item[160];
        std::snprintf(item, sizeof(item), "%s%ls%s%u %.0f%%", list.empty() ? "" : ", ", t.name.c_str(),
                      t.name.empty() ? "" : " ", t.id, t.percent);
        list += item;
    }
    if (!list.empty())
        logf("views4 threads: %s (of one core each)", list.c_str());
}

uint64_t ini_write_later(std::function<void()> write);  // (below)

// VRAM (views4): this process's use of the GPU's local memory and the census of ACE's render targets
// (probe/vram_census.h) against its render and output sizes, logged and written to the runtime info, where
// the launcher scales the part that follows the targets to estimate other settings (launcher/settings.h).
IDXGIAdapter3* vramAdapter = nullptr;
void report_vram() {
    int render[2], output[2];
    game->qvfr_target_sizes(render, output);
    if (output[0] <= 0 || !quad.device)
        return;
    if (!vramAdapter) {
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            if (FAILED(factory->EnumAdapterByLuid(quad.device->GetAdapterLuid(), IID_PPV_ARGS(&vramAdapter))))
                vramAdapter = nullptr;
            factory->Release();
        }
        if (!vramAdapter)
            return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (FAILED(vramAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
        return;
    const VramCensus c = vram_census(render[0], render[1], output[0], output[1]);
    constexpr double kMb = 1024.0 * 1024.0;
    logf("vram: %.0f MB used of %.0f MB; render targets %.0f MB that follow the %dx%d render and %dx%d output "
         "targets (%zu textures), %.0f MB other (%zu)",
         memory.CurrentUsage / kMb, memory.Budget / kMb, c.scaledBytes / kMb, render[0], render[1], output[0],
         output[1], c.scaledTextures, c.otherBytes / kMb, c.otherTextures);
    ini_write_later([used = memory.CurrentUsage / kMb, budget = memory.Budget / kMb, scaled = c.scaledBytes / kMb,
                     w = output[0], h = output[1]] {
        runtime_info(L"vram_used_mb", std::to_wstring(std::lround(used)));
        runtime_info(L"vram_budget_mb", std::to_wstring(std::lround(budget)));
        runtime_info(L"vram_scaled_mb", std::to_wstring(std::lround(scaled)));
        runtime_info(L"vram_target", std::to_wstring(w) + L"x" + std::to_wstring(h));
    });
}

// The session summary in the runtime info ([runtime] session_*), at every report: what the launcher's
// Diagnostics pane shows after the session (launcher/settings.h's SessionInfo). `layerMs`: the layer's own
// CPU time per frame on ACE's render thread.
void write_session_summary(double layerMs) {
    if (summary.fps <= 0)
        return;
    const SessionSummary s = summary;
    // What limits the frame: the GPU busy all the time, or the render thread near its whole budget (the
    // spinning Physics and eye-tracker threads are not counted: they look busy whatever limits the frame).
    const wchar_t* limited = s.gpuBusy >= 95                                ? L"GPU"
                             : s.cpuMs >= 0 && s.cpuMs >= 0.9 * s.periodMs ? L"CPU (render thread)"
                             : s.latePercent > 5                           ? L"Pacing (frames late, neither saturated)"
                                                                           : L"Neither (headroom)";
    std::wstring threads;
    for (const ThreadLoad& t : s.threads) {
        if (t.percent < 5)
            break;
        wchar_t item[160];
        _snwprintf_s(item, _TRUNCATE, L"%ls%ls=%.0f", threads.empty() ? L"" : L"|",
                   t.name.empty() ? (L"thread " + std::to_wstring(t.id)).c_str() : t.name.c_str(), t.percent);
        threads += item;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t time[32];
    swprintf_s(time, L"%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    const std::wstring mode = experiment.views4                       ? L"QVFR"
                              : experiment.quad || experiment.twoLoops ? L"Two-loop"
                                                                       : L"Off (the game's own two views)";
    const bool fg = frame_generation_on();
    double gazeMs = -1;
    {
        std::lock_guard guard(gazeThread.mutex);
        if (gazeThread.lookups)
            gazeMs = gazeThread.lookupMs / gazeThread.lookups;
    }
    wchar_t log[MAX_PATH] = {};
    GetEnvironmentVariableW(L"SIM_QVFR_LOG", log, MAX_PATH);
    const std::wstring profile = cpu_profile_last_report();
    const std::wstring layers(layersBelow.begin(), layersBelow.end());
    ini_write_later([s, limited = std::wstring(limited), threads, time = std::wstring(time), mode, fg, gazeMs, layerMs,
                     log = std::wstring(log), profile, layers] {
        const auto number = [](double v, int digits) {
            if (v < 0)
                return std::wstring();
            wchar_t text[32];
            swprintf_s(text, L"%.*f", digits, v);
            return std::wstring(text);
        };
        runtime_info(L"session_time", time);
        runtime_info(L"session_mode", mode);
        runtime_info(L"session_fps", number(s.fps, 1));
        runtime_info(L"session_frame_p50_ms", number(s.frameP50, 2));
        runtime_info(L"session_frame_p90_ms", number(s.frameP90, 2));
        runtime_info(L"session_late_percent", number(s.latePercent, 1));
        runtime_info(L"session_refresh_hz", number(s.periodMs > 0 ? 1000 / s.periodMs : -1, 0));
        runtime_info(L"session_cpu_ms", number(s.cpuMs, 2));
        runtime_info(L"session_gpu_ms", number(s.gpuMs, 2));
        runtime_info(L"session_gpu_busy", number(s.gpuBusy, 0));
        runtime_info(L"session_cpu_cores", number(s.cores, 1));
        runtime_info(L"session_limited", limited);
        runtime_info(L"session_threads", threads);
        runtime_info(L"session_frame_generation", fg ? L"1" : L"0");
        runtime_info(L"session_gaze_ms", number(gazeMs, 3));
        runtime_info(L"session_layer_ms", number(layerMs, 2));
        runtime_info(L"session_layers_below", layers);
        runtime_info(L"session_log", log);
        runtime_info(L"session_cpu_profile", profile);
    });
}

void report_cpu_times() {
    const auto line = [](CpuSpan& total, CpuSpan* below, char* out, size_t size) {
        const uint64_t n = total.calls.exchange(0), t = total.ticks.exchange(0), m = total.maxTicks.exchange(0);
        const uint64_t bt = below ? below->ticks.exchange(0) : 0;
        if (below) {
            below->calls = 0;
            below->maxTicks = 0;
        }
        const double per = n ? cpu.msPerTick / static_cast<double>(n) : 0;
        if (below)
            std::snprintf(out, size, "%.3f (layer %.3f, below %.3f, max %.3f) x%llu", t * per,
                          (t > bt ? t - bt : 0) * per, bt * per, m * cpu.msPerTick, static_cast<unsigned long long>(n));
        else
            std::snprintf(out, size, "%.3f (max %.3f) x%llu", t * per, m * cpu.msPerTick, static_cast<unsigned long long>(n));
    };
    // The layer's own time per frame on the render thread, before the lines below reset the spans.
    const auto own = [](const CpuSpan& total, const CpuSpan* below) {
        const uint64_t n = total.calls.load(), t = total.ticks.load(), b = below ? below->ticks.load() : 0;
        return n ? static_cast<double>(t > b ? t - b : 0) * cpu.msPerTick / static_cast<double>(n) : 0.0;
    };
    const double layerMs = own(cpu.endFrame, &cpu.endFrameBelow) + own(cpu.locate, &cpu.locateBelow);
    char end[128], locate[128], compose[128];
    line(cpu.endFrame, &cpu.endFrameBelow, end, sizeof(end));
    line(cpu.locate, &cpu.locateBelow, locate, sizeof(locate));
    line(cpu.compose, nullptr, compose, sizeof(compose));
    logf("views4 cpu: ms per call over the last %llu frames: xrEndFrame %s; xrLocateViews %s; compose %s",
         static_cast<unsigned long long>(kCpuReportFrames), end, locate, compose);
    const double frameMs = report_pacing();
    gpu_frame_timer_report();  // its own samples: it logs while the overlay collects too
    report_load(frameMs);
    report_vram();
    write_session_summary(layerMs);
    gaze_thread_report(frameMs * kCpuReportFrames / 1000);
    gaze_hold_report();
}

// A game in src/games, by its exe: anywhere else the layer passes through.
bool is_game_process() { return games::current_game() != nullptr; }

XrResult XRAPI_CALL layerEnumerateViewConfigurationViews(XrInstance instance, XrSystemId system,
                                                         XrViewConfigurationType type, uint32_t capacity,
                                                         uint32_t* countOutput, XrViewConfigurationView* views) {
    XrResult r = next.EnumerateViewConfigurationViews(instance, system, type, capacity, countOutput, views);
    const bool stereo = XR_SUCCEEDED(r) && views && countOutput && *countOutput > 0 && capacity > 0 &&
                        type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (stereo) {
        std::lock_guard guard(quad.mutex);
        quad.nativeW = views[0].recommendedImageRectWidth;
        quad.nativeH = views[0].recommendedImageRectHeight;
        runtime_info(L"recommended_width", std::to_wstring(views[0].recommendedImageRectWidth));
        runtime_info(L"recommended_height", std::to_wstring(views[0].recommendedImageRectHeight));
        runtime_info(L"max_width", std::to_wstring(views[0].maxImageRectWidth));
        runtime_info(L"max_height", std::to_wstring(views[0].maxImageRectHeight));
    }
    // views4: ACE's "eye" holds one outer and one focus view side by side (views4_layout.h), so its
    // double-wide target holds all four.
    const Views4Layout& layout = experiment.views4Layout;
    const bool columns = experiment.views4 && game->qvfr_eye_columns();  // eye columns: views4_rect_eye_columns
    // One view size (qvfr_one_view_size): every view the larger of the outer and focus sizes, the column two high.
    const bool oneSize = columns && game->qvfr_one_view_size();
    const double viewWidth = std::max(layout.outer_width(), layout.focus);
    const double viewHeight = std::max(layout.outer_height(), layout.focusHeight);
    const double widthScale = experiment.views4 ? (oneSize ? viewWidth : columns ? layout.column_eye_width() : layout.eye_width())
                                                : experiment.eyeScale;
    const double heightScale = experiment.views4
                                   ? (oneSize ? 2 * viewHeight : columns ? layout.column_eye_height() : layout.eye_height())
                                   : experiment.eyeScale;
    if (oneSize && stereo && (layout.outer_width() != layout.focus || layout.outer_height() != layout.focusHeight))
        logf("views4: the game draws every view at one size: %.2f x %.2f of the native eye (outer asked %.2f x %.2f, "
             "focus %.2f x %.2f)", viewWidth, viewHeight, layout.outer_width(), layout.outer_height(), layout.focus,
             layout.focusHeight);
    if (XR_SUCCEEDED(r) && views && countOutput && (experiment.eyeScale != 1.0 || experiment.views4))
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i) {
            // ACE's swapchain is double-wide, so twice the width must stay within the runtime's maximum.
            const uint32_t maxW = views[i].maxImageRectWidth ? std::max(64u, views[i].maxImageRectWidth / 2) : UINT32_MAX;
            const uint32_t maxH = views[i].maxImageRectHeight ? std::max(64u, views[i].maxImageRectHeight) : UINT32_MAX;
            const auto wanted = static_cast<uint32_t>(views[i].recommendedImageRectWidth * widthScale);
            if (experiment.views4 && wanted > maxW)
                logf("views4: eye width %u exceeds the runtime maximum %u; every view loses resolution, lower "
                     "periphery_scale or focus_density", wanted, maxW);
            views[i].recommendedImageRectWidth = std::clamp(wanted, 64u, maxW);
            views[i].recommendedImageRectHeight = std::clamp(
                static_cast<uint32_t>(views[i].recommendedImageRectHeight * heightScale), 64u, maxH);
        }
    if (stereo) {
        std::lock_guard guard(quad.mutex);
        quad.reportedW = views[0].recommendedImageRectWidth;
        quad.reportedH = views[0].recommendedImageRectHeight;
    }
    if (XR_SUCCEEDED(r) && views && countOutput)
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
            logf("xrEnumerateViewConfigurationViews type=%d view=%u recommended=%ux%u max=%ux%u samples=%u", type, i,
                 views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight, views[i].maxImageRectWidth,
                 views[i].maxImageRectHeight, views[i].recommendedSwapchainSampleCount);
    return r;
}

XrResult XRAPI_CALL layerCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
    const auto* binding = static_cast<const XrBaseInStructure*>(info->next);
    game->session_running(false);  // no views until it begins
    XrResult r = next.CreateSession(instance, info, session);
    logf("xrCreateSession result=%d bindingType=%d", r, binding ? binding->type : 0);
    if (XR_SUCCEEDED(r)) {
        {
            std::lock_guard guard(quad.mutex);
            sessionGraphics = session_graphics(binding);
        }
        hang_watch_arm(experiment.hangWatchSeconds);
        game->session_created(experiment.probes && probe_start());
        if (!experiment.probes)
            log_line(experiment.views3 ? "engine probes and D3D12 trace off (probes=0); views3 cannot report the view mode"
                                       : "engine probes and D3D12 trace off (probes=0)");
        if (binding && binding->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            ID3D11Device* device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(binding)->device;
            d3d11_capture_start(device, experiment.d3d11CaptureFrame);
            if (experiment.views4ComposeD3D11)
                game->d3d11_session(device);
            cheeky_session_d3d11(device);  // when Cheeky runs in this game (loads_cheeky)
        }
        if (binding && binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto* d3d12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding);
            if (experiment.probes)
                d3d12_trace_start(d3d12->device);
            if (experiment.views4)  // before ACE makes its renderer: what VRAM follows the resolution
                vram_census_start(d3d12->device);
            if (experiment.dred)
                dred_watch(d3d12->device);
            if ((experiment.passCensus || experiment.uavBarriers) && pass_census_start(d3d12->device)) {
                pass_census_set_uav_barriers(experiment.uavBarriers);
                if (experiment.uavBarriers)
                    log_line("pass census: a UAV barrier follows every dispatch (pass_census_uav_barriers)");
                if (experiment.passCensus) {
                    pass_census_watch(experiment.passCensusWatch);
                    pass_census_arm(experiment.passCensusFrame, 10);
                }
            }
            if (experiment.d3d12Debug)
                d3d12_debug_attach(d3d12->device);
            std::lock_guard guard(quad.mutex);
            quad.device = d3d12->device;
            quad.queue = d3d12->queue;
            if (experiment.views4 && experiment.gpuFrameTiming)
                gpu_frame_timer_start(d3d12->device, d3d12->queue);
            if (experiment.views4 || experiment.measure) {  // the whole GPU's and ACE's threads' load (the overlay, the log)
                const LUID adapter = d3d12->device->GetAdapterLuid();
                gpu_load_start(static_cast<uint32_t>(adapter.HighPart), adapter.LowPart);
                cpu_load_start();
            }
            cheeky_session(d3d12->device, d3d12->queue);
        } else if (experiment.quad) {
            log_line("quad: session is not D3D12 (a layer above converted it?); quad pipeline off");
        }
    }
    return r;
}

void menu_shutdown();  // the settings menu's panel (below)
void osd_shutdown();   // the performance overlay's card (below)
void pad_watch_stop();  // the hotkeys' wheel and controller thread (below)

void ini_worker_sync();  // (below)
void helmet_layer_release();  // (below)

XrResult XRAPI_CALL layerDestroySession(XrSession session) {
    game->session_running(false);
    hang_watch_disarm();
    ini_worker_sync();  // the menu's and hotkeys' last changes reach the ini
    menu_shutdown();  // its swapchain and space belong to the session
    osd_shutdown();
    probe_stop();
    game->session_destroyed();
    dred_stop();
    d3d11_capture_stop();
    {
        std::lock_guard guard(v4d3d11.mutex);
        v4d3d11.compositor.shutdown();
        v4d3d11.chains.clear();
        v4d3d11.outChain = XR_NULL_HANDLE;  // the session's: destroyed with it
        v4d3d11.outImages.clear();
        v4d3d11.failed = false;
    }
    {
        std::lock_guard guard(gaze.mutex);
        if (gaze.queries > 900 && gaze.tracked == 0)  // asked for ~10 s, never tracked
            runtime_info(L"eye_tracking", L"0");
    }
    {
        std::lock_guard guard(quad.mutex);
        quad.v4Composed = quad.v4FovValid = false;
    }
    {
        std::lock_guard guard(v4outline.mutex);
        v4outline.outline.shutdown();
        v4outline.ready = v4outline.failed = v4outline.fovValid = false;
        v4outline.chain = XR_NULL_HANDLE;
        v4outline.images.clear();
        v4outline.acquired.clear();
    }
    {
        std::lock_guard guard(quad.mutex);
        quad.compositor.shutdown();
        quad.appChain = quad.outChain = XR_NULL_HANDLE;
        quad.appImages.clear();
        quad.outImages.clear();
        quad.outerCaptured = quad.aViewsValid = quad.releaseHeld = false;
        quad.device = nullptr;
        quad.queue = nullptr;
        sessionGraphics = {};
        // The helmet's layer goes with the session; its pieces load again into the next one's compositor.
        helmet_layer_release();
        quad.helmetLayerFailed = false;
        quad.helmetBandsLogged.clear();
        for (int p = 0; p < 3; ++p) {
            quad.helmetPiecesTried[p].clear();
            quad.helmetPiecesWritten[p] = {};
        }
        quad.helmetHinted.clear();
        quad.helmetPiecesLoaded = false;
    }
    gpu_frame_timer_stop();
    gpu_load_stop();
    cpu_load_stop();
    pad_watch_stop();
    gaze_thread_stop();  // before the spaces it locates go
    {
        std::lock_guard guard(gaze.mutex);  // the session's spaces and attachment go with it
        gaze.space = gaze.gameSpace = XR_NULL_HANDLE;
        gaze.attached = false;
        gaze.appSyncs = 0;
    }
    cheeky_session(nullptr, nullptr);
    log_line("xrDestroySession");
    return next.DestroySession(session);
}

void osd_waited(XrDuration period);  // the performance overlay's frame budget (below)
void osd_wait_returned(double waitMs);  // where the render thread's work for a frame starts (below)

XrResult XRAPI_CALL layerWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    if (!experiment.twoLoops) {
        const int64_t t0 = qpc_now();
        const XrResult r = next.WaitFrame(session, info, state);
        if (XR_SUCCEEDED(r)) {
            osd_waited(state->predictedDisplayPeriod);
            const double waitMs = (qpc_now() - t0) * cpu.msPerTick;
            if ((experiment.views4 || experiment.measure) && pacing.wait.size() < 4 * kCpuReportFrames) {
                pacing.wait.push_back(static_cast<float>(waitMs));
                pacing.periodMs = state->predictedDisplayPeriod / 1e6;
            }
            osd_wait_returned(waitMs);
        }
        return r;
    }
    std::unique_lock lock(loops.mutex);
    if (!loops.active && !loops.broken && loops.waits >= kArmAfterFrames && loops.waits == loops.begins &&
        loops.begins == loops.ends && (!experiment.quad || quad_ready())) {
        loops.active = true;
        loops.pairedWaits = loops.pairedBegins = loops.pairedEnds = 0;
        QueryPerformanceFrequency(&loops.frequency);
        QueryPerformanceCounter(&loops.lastReport);
        logf("two_loops: ARMED after %llu frames; ACE now runs two frames (A, B) per display frame",
             static_cast<unsigned long long>(loops.waits));
    }
    if (!loops.active) {
        lock.unlock();
        const XrResult r = next.WaitFrame(session, info, state);
        lock.lock();
        if (++loops.waits == 20 * kArmAfterFrames && !loops.broken)
            log_line(experiment.quad && !quad_ready()
                         ? "two_loops: not armed: ACE's eye swapchain was not adopted (see the quad: lines above); "
                           "ACE runs unchanged"
                         : "two_loops: not armed yet: no idle point between frames (is ACE's option to overlap the "
                           "OpenXR frame wait with the previous frame on?)");
        return r;
    }
    const uint64_t index = loops.pairedWaits++;
    if (index % 2 == 1) {  // B: same display time and pose as A, no real wait
        loops.inB = true;
        state->predictedDisplayTime = loops.pendingState.predictedDisplayTime;
        state->predictedDisplayPeriod = loops.pendingState.predictedDisplayPeriod;
        state->shouldRender = loops.pendingState.shouldRender;
        QueryPerformanceCounter(&loops.bWaitReturned);
        return XR_SUCCESS;
    }
    lock.unlock();  // never hold the lock across the runtime's blocking wait
    const XrResult r = next.WaitFrame(session, info, state);
    lock.lock();
    loops.pendingState.predictedDisplayTime = state->predictedDisplayTime;
    loops.pendingState.predictedDisplayPeriod = state->predictedDisplayPeriod;
    loops.pendingState.shouldRender = state->shouldRender;
    QueryPerformanceCounter(&loops.aWaitReturned);
    if (XR_SUCCEEDED(r))
        osd_waited(state->predictedDisplayPeriod / 2);  // two ACE frames per display frame
    return r;
}

XrResult XRAPI_CALL layerBeginFrame(XrSession session, const XrFrameBeginInfo* info) {
    if (!experiment.twoLoops) {
        const XrResult r = next.BeginFrame(session, info);
        if (XR_SUCCEEDED(r))
            gpu_frame_timer_mark(GpuMark::kBegin);
        return r;
    }
    std::unique_lock lock(loops.mutex);
    if (!loops.active) {
        ++loops.begins;
        lock.unlock();
        return next.BeginFrame(session, info);
    }
    const uint64_t index = loops.pairedBegins++;
    lock.unlock();
    return index % 2 == 0 ? next.BeginFrame(session, info) : XR_SUCCESS;  // B's frame is already begun
}

XrResult XRAPI_CALL layerBeginSession(XrSession session, const XrSessionBeginInfo* info) {
    XrResult r = next.BeginSession(session, info);
    logf("xrBeginSession viewConfig=%d result=%d", info->primaryViewConfigurationType, r);
    if (XR_SUCCEEDED(r))
        game->session_running(true);
    return r;
}

// Takes ACE's double-wide eye swapchain as the quad input and creates the output swapchain.
void quad_adopt_chain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain chain) {
    std::lock_guard guard(quad.mutex);
    if (quad.appChain || !quad.device || !quad.nativeW || !quad.reportedH || !info->height)
        return;
    // The double-wide eye target, matched by shape rather than exact width: ACE's own VR resolution
    // setting scales it from what it was offered. The compositor reads the image rects ACE submits.
    const double wanted = 2.0 * quad.reportedW / quad.reportedH;
    const double aspect = static_cast<double>(info->width) / info->height;
    if (std::abs(aspect / wanted - 1) > 0.03) {
        logf("quad: %ux%u is not ACE's double-wide eye target (offered %ux%u per eye); not adopted", info->width,
             info->height, quad.reportedW, quad.reportedH);
        return;
    }
    if (info->arraySize != 1 || info->sampleCount != 1) {
        logf("quad: eye target %ux%u has array=%u samples=%u, which the compositor cannot read; ACE runs unchanged",
             info->width, info->height, info->arraySize, info->sampleCount);
        return;
    }
    if (!quad.compositor.initialize(quad.device, quad.queue, static_cast<DXGI_FORMAT>(info->format))) {
        log_line("quad: compositor unavailable; ACE runs unchanged");
        return;
    }
    XrSwapchainCreateInfo out = *info;
    out.next = nullptr;
    out.width = 2 * quad.nativeW;
    out.height = quad.nativeH;
    out.mipCount = 1;
    out.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchain outChain = XR_NULL_HANDLE;
    XrResult r = next.CreateSwapchain(session, &out, &outChain);
    uint32_t count = 0;
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(outChain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(outChain, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r)) {
        logf("quad: output swapchain %ux%u failed (%d); quad pipeline off", out.width, out.height, r);
        return;
    }
    quad.appChain = chain;
    quad.appW = info->width;
    quad.appH = info->height;
    quad.format = static_cast<DXGI_FORMAT>(info->format);
    quad.outChain = outChain;
    for (const auto& image : images)
        quad.outImages.push_back(image.texture);
    logf("quad: ACE eye chain %p %ux%u (%.2fx the %ux%u offered: ACE's own resolution setting) -> output chain %p "
         "%ux%u (%u images), focus %.0f%% of the FOV, feather %.0f%%",
         static_cast<void*>(chain), info->width, info->height, info->height / static_cast<double>(quad.reportedH),
         quad.reportedW, quad.reportedH, static_cast<void*>(outChain), out.width, out.height, count,
         experiment.focusFraction * 100, experiment.feather * 100);
}

XrResult XRAPI_CALL layerCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    XrResult r = next.CreateSwapchain(session, info, swapchain);
    logf("xrCreateSwapchain %ux%u array=%u faces=%u mips=%u samples=%u format=%lld usage=0x%llx flags=0x%llx "
         "result=%d handle=%p",
         info->width, info->height, info->arraySize, info->faceCount, info->mipCount, info->sampleCount,
         static_cast<long long>(info->format), static_cast<unsigned long long>(info->usageFlags),
         static_cast<unsigned long long>(info->createFlags), r, XR_SUCCEEDED(r) ? static_cast<void*>(*swapchain) : nullptr);
    if (XR_SUCCEEDED(r)) {
        std::lock_guard guard(chainMutex);
        chainSizes[*swapchain] = {info->width, info->height};
    }
    if (XR_SUCCEEDED(r) && views4_outline_on()) {
        std::lock_guard guard(v4outline.mutex);
        v4outline.formats[*swapchain] = info->format;
    }
    if (XR_SUCCEEDED(r) && experiment.views4ComposeD3D11) {
        std::lock_guard guard(v4d3d11.mutex);
        v4d3d11.chains[*swapchain].format = info->format;
    }
    if (XR_SUCCEEDED(r) && (experiment.quad || experiment.views4Compose) &&
        (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
        quad_adopt_chain(session, info, *swapchain);
    return r;
}

XrResult XRAPI_CALL layerEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* countOutput,
                                                  XrSwapchainImageBaseHeader* images) {
    XrResult r = next.EnumerateSwapchainImages(swapchain, capacity, countOutput, images);
    if (XR_SUCCEEDED(r) && capacity > 0 && images && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR &&
        experiment.d3d11CaptureFrame)
        d3d11_capture_images(swapchain, reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images),
                             std::min(*countOutput, capacity));
    if (XR_SUCCEEDED(r) && capacity > 0 && images && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR &&
        experiment.views4ComposeD3D11) {
        std::lock_guard guard(v4d3d11.mutex);
        if (auto it = v4d3d11.chains.find(swapchain); it != v4d3d11.chains.end()) {
            const auto* d3d11Images = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
            it->second.images.clear();
            for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
                it->second.images.push_back(d3d11Images[i].texture);
        }
    }
    if (XR_FAILED(r) || capacity == 0 || !images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)
        return r;
    std::pair<uint32_t, uint32_t> size{0, 0};
    {
        std::lock_guard guard(chainMutex);
        if (auto it = chainSizes.find(swapchain); it != chainSizes.end())
            size = it->second;
    }
    auto* d3d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
        d3d12_trace_register_image(d3d12Images[i].texture, size.first, size.second, i);
    {
        std::lock_guard guard(quad.mutex);
        if (swapchain == quad.appChain) {
            quad.appImages.clear();
            for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
                quad.appImages.push_back(d3d12Images[i].texture);
        }
    }
    if (views4_outline_on()) {
        std::lock_guard guard(v4outline.mutex);
        auto& list = v4outline.images[swapchain];
        list.clear();
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
            list.push_back(d3d12Images[i].texture);
    }
    logf("xrEnumerateSwapchainImages chain=%p %ux%u images=%u registered for D3D12 tracing",
         static_cast<void*>(swapchain), size.first, size.second, *countOutput);
    return r;
}

XrResult XRAPI_CALL layerAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info,
                                               uint32_t* index) {
    const XrResult r = next.AcquireSwapchainImage(swapchain, info, index);
    if (XR_SUCCEEDED(r) && experiment.d3d11CaptureFrame)
        d3d11_capture_acquired(swapchain, *index);
    if (XR_SUCCEEDED(r) && experiment.views4ComposeD3D11) {
        std::lock_guard guard(v4d3d11.mutex);
        if (auto it = v4d3d11.chains.find(swapchain); it != v4d3d11.chains.end())
            it->second.acquired = *index;
    }
    if (XR_SUCCEEDED(r) && views4_outline_on()) {
        std::lock_guard guard(v4outline.mutex);
        v4outline.acquired[swapchain] = *index;
    }
    if (XR_SUCCEEDED(r) && (experiment.quad || experiment.views4Compose)) {
        std::lock_guard guard(quad.mutex);
        if (swapchain == quad.appChain)
            quad.appAcquired = *index;
    }
    return r;
}

bool quad_capture_due();
void views4_capture(ID3D12Resource* views, ID3D12Resource* output);

int64_t qpc_ms(int64_t ms);

// The helmet's pieces (under quad.mutex): each image, relative to the layer's folder, loaded into the
// compositor when its path changes or, looked at once a second, its file appears, changes or goes (a
// load stalls the GPU once); a missing file is no piece. So images can be dropped in or edited while
// the game runs.
void helmet_pieces(const HelmetSettings& h) {
    const int64_t now = qpc_now();
    const std::wstring* paths[3] = {&h.visorImage, &h.bottomImage, &h.topImage};  // HelmetPiece order
    const bool look = now >= quad.helmetPiecesCheck;
    if (look)
        quad.helmetPiecesCheck = now + qpc_ms(1000);
    for (int p = 0; p < 3; ++p) {
        const bool samePath = *paths[p] == quad.helmetPiecesTried[p];
        if (samePath && !look)
            continue;
        const std::filesystem::path image(*paths[p]);
        const std::filesystem::path full = image.is_absolute() ? image : std::filesystem::path(layer_directory()) / image;
        std::error_code error;
        std::filesystem::file_time_type written = std::filesystem::file_time_type::min();
        if (!paths[p]->empty() && std::filesystem::is_regular_file(full, error)) {
            written = std::filesystem::last_write_time(full, error);
            if (error)
                written = std::filesystem::file_time_type::min();
        }
        if (samePath && written == quad.helmetPiecesWritten[p])
            continue;
        const bool first = !samePath;
        quad.helmetPiecesTried[p] = *paths[p];
        quad.helmetPiecesWritten[p] = written;
        const bool exists = written != std::filesystem::file_time_type::min();
        helmet_set().load_helmet_piece(static_cast<HelmetPiece>(p), exists ? full.wstring() : std::wstring());
        if (!paths[p]->empty() && !exists && first)
            logf("helmet: no %ls (that piece is left out until it appears)", full.c_str());
    }
    quad.helmetPiecesLoaded = helmet_set().has_helmet_pieces();
}

// The visor's slide this frame (under quad.mutex): it moves towards the asked position, all the way
// from closed to open in kVisorSlideS.
constexpr double kVisorSlideS = 0.3;
double visor_step(const HelmetSettings& h) {
    const double target = visor_target(h);
    const int64_t now = qpc_now();
    if (quad.visorPosition < 0) {
        quad.visorPosition = target;
    } else {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double step = static_cast<double>(now - quad.visorAt) / frequency.QuadPart / kVisorSlideS;
        quad.visorPosition = quad.visorPosition < target ? std::min(target, quad.visorPosition + step)
                                                         : std::max(target, quad.visorPosition - step);
    }
    quad.visorAt = now;
    return quad.visorPosition;
}

// The helmet layer's swapchain and VIEW space (under quad.mutex), once the pieces are loaded; square,
// helmet_layer_side ([helmet] detail: 2048 or 4096) a side.
bool helmet_layer_resources(XrSession session) {
    if (quad.helmetChain)
        return true;
    if (quad.helmetLayerFailed)
        return false;
    const uint32_t side = helmet_layer_side(helmetSettings);
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    chainInfo.format = kHelmetLayerFormat;
    chainInfo.sampleCount = 1;
    chainInfo.width = chainInfo.height = side;
    chainInfo.faceCount = chainInfo.arraySize = chainInfo.mipCount = 1;
    uint32_t count = 0;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &quad.helmetSpace);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &quad.helmetChain);
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(quad.helmetChain, 0, &count, nullptr);
    const bool d3d11 = experiment.views4ComposeD3D11;
    std::vector<XrSwapchainImageD3D12KHR> images(d3d11 ? 0 : count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    std::vector<XrSwapchainImageD3D11KHR> images11(d3d11 ? count : 0, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r) && count)
        r = next.EnumerateSwapchainImages(quad.helmetChain, count, &count,
                                          d3d11 ? reinterpret_cast<XrSwapchainImageBaseHeader*>(images11.data())
                                                : reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r) || !count) {
        logf("helmet: its own layer failed (%d); the pieces are drawn into the scene image instead (frame "
             "generation may then smear)", r);
        if (quad.helmetChain)
            next.DestroySwapchain(quad.helmetChain);
        if (quad.helmetSpace)
            next.DestroySpace(quad.helmetSpace);
        quad.helmetChain = XR_NULL_HANDLE;
        quad.helmetSpace = XR_NULL_HANDLE;
        quad.helmetLayerFailed = true;
        return false;
    }
    quad.helmetImages.clear();
    quad.helmetImages11.clear();
    for (const auto& image : images)
        quad.helmetImages.push_back(image.texture);
    for (const auto& image : images11)
        quad.helmetImages11.push_back(image.texture);
    for (XrCompositionLayerQuad& l : quad.helmetQuads) {
        l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
        l.space = quad.helmetSpace;
        l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        l.subImage.swapchain = quad.helmetChain;
    }
    quad.helmetSide = side;
    logf("helmet: own head-locked layer, %ux%u (%u images; %s)", side, side, count,
         side == kHelmetDetailSide ? "detail" : "performance");
    return true;
}

// The helmet's layer goes (under quad.mutex): its swapchain and space, made again when needed.
void helmet_layer_release() {
    if (quad.helmetChain)
        next.DestroySwapchain(quad.helmetChain);
    if (quad.helmetSpace)
        next.DestroySpace(quad.helmetSpace);
    quad.helmetChain = XR_NULL_HANDLE;
    quad.helmetSpace = XR_NULL_HANDLE;
    quad.helmetImages.clear();
    quad.helmetImages11.clear();
    quad.helmetSide = 0;
    quad.helmetQuadCount = 0;
    quad.helmetDrawn = quad.helmetResize = false;
}

// The helmet layer for this frame (under quad.mutex, on ACE's queue after the composition): the quad
// placed at the helmet's distance and height, sized to cover both eyes' uncropped views (`full`), and the
// pieces drawn into it. False if it was not drawn (nothing is submitted then). `views11`: a D3D11 game's image
// of its views (the D3D11 compositor draws it, the frame's light from it); null: ACE's D3D12 compositor.
bool helmet_layer_draw(const HelmetSettings& h, const EyeComposition (&eyes)[2], const float (&full)[2][4],
                       double visorOffset, ID3D11Texture2D* views11) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const size_t images = views11 ? quad.helmetImages11.size() : quad.helmetImages.size();
    if (XR_FAILED(next.AcquireSwapchainImage(quad.helmetChain, &acquire, &index)) || index >= images)
        return false;
    const double tilt = h.height * kHelmetDegrees, distance = helmet_distance(h);
    const XrQuaternionf up{static_cast<float>(std::sin(tilt / 2)), 0, 0, static_cast<float>(std::cos(tilt / 2))};
    float rotation[2][9], offset[2][3];
    for (int e = 0; e < 2; ++e) {
        to_matrix(qmul(qconj(up), quad.v4EyeTurn[e]), rotation[e]);
        const XrVector3f o = rotate(qconj(up), quad.v4EyeOffset[e]);  // in the images' frame
        offset[e][0] = o.x;
        offset[e][1] = o.y;
        offset[e][2] = o.z;
    }
    double coverX, coverY, imageX, imageY;
    helmet_cover(rotation, offset, full, distance, coverX, coverY);
    helmet_tan_half(h, helmet_set().helmet_aspect(), imageX, imageY);
    // Only the bands the pieces draw on, with the visor where it is now.
    const std::vector<HelmetBand> bands =
        helmet_bands(helmet_set().helmet_rows(visorOffset, h.visorBlend), imageY, coverY);
    HelmetLayerDraw draw{};
    draw.coverTan[0] = static_cast<float>(coverX);
    draw.coverTan[1] = static_cast<float>(coverY);
    draw.imageTan[0] = static_cast<float>(imageX);
    draw.imageTan[1] = static_cast<float>(imageY);
    draw.visorOffset = static_cast<float>(visorOffset);
    draw.light = static_cast<float>(h.light) / 100.0f;
    draw.visorBlend = h.visorBlend;
    draw.checkLight = h.debugLight;
    std::memcpy(draw.outerSource[0], eyes[0].outerSource, sizeof(draw.outerSource[0]));
    std::memcpy(draw.outerSource[1], eyes[1].outerSource, sizeof(draw.outerSource[1]));
    draw.bands = bands;
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(quad.helmetChain, &wait)) &&
                    (views11 ? v4d3d11.compositor.draw_helmet_layer(views11, quad.helmetImages11[index], draw)
                             : quad.compositor.draw_helmet_layer(quad.helmetImages[index], draw));
    next.ReleaseSwapchainImage(quad.helmetChain, &release);
    // The quads, in VIEW space: the whole quad centred `height` above straight ahead, `distance` away, facing
    // the eyes; each band its slice of it (along the quad's own up) and of the image.
    const float width = static_cast<float>(2 * distance * coverX), height = static_cast<float>(2 * distance * coverY);
    const int32_t side = static_cast<int32_t>(quad.helmetSide);
    quad.helmetQuadCount = 0;
    for (const HelmetBand& b : bands) {
        XrCompositionLayerQuad& l = quad.helmetQuads[quad.helmetQuadCount++];
        const int32_t top = static_cast<int32_t>(std::floor(b.top * side)), bottom = static_cast<int32_t>(std::ceil(b.bottom * side));
        const float rise = (0.5f - 0.5f * (top + bottom) / side) * height;  // the band's centre above the quad's
        l.pose.orientation = up;
        l.pose.position = rotate(up, XrVector3f{0, rise, static_cast<float>(-distance)});
        l.size = {width, height * (bottom - top) / side};
        l.subImage.imageRect = {{0, top}, {side, bottom - top}};
        if (quad.helmetQuadCount == 2)
            break;
    }
    char text[96] = "whole";
    if (bands.empty()) {
        std::snprintf(text, sizeof(text), "none (no piece in view)");
    } else if (bands.size() != 1 || bands[0].top > 0 || bands[0].bottom < 1) {
        int n = 0;
        double drawn = 0;
        for (const HelmetBand& b : bands) {
            n += std::snprintf(text + n, sizeof(text) - n, "%s%.2f-%.2f", n ? ", " : "", b.top, b.bottom);
            drawn += b.bottom - b.top;
        }
        std::snprintf(text + n, sizeof(text) - n, " (%.0f%% of it)", drawn * 100);
    }
    if (quad.helmetBandsLogged != text) {
        quad.helmetBandsLogged = text;
        logf("helmet: layer drawn and submitted over %s of the quad (top to bottom)", text);
    }
    return ok;
}

// Once per change of the pieces or their size and height (under quad.mutex): the crop that meets the
// pieces' opening straight ahead, for the log (`full`: eye 0's uncropped view).
void helmet_crop_log(const HelmetSettings& h, const float (&full)[4]) {
    const double top = helmet_set().helmet_edge(HelmetPiece::Top), bottom = helmet_set().helmet_edge(HelmetPiece::Bottom);
    const std::wstring key = quad.helmetPiecesTried[1] + L"|" + quad.helmetPiecesTried[2] + L"|" +
                             std::to_wstring(quad.helmetPiecesWritten[1].time_since_epoch().count()) + L"|" +
                             std::to_wstring(quad.helmetPiecesWritten[2].time_since_epoch().count()) + L"|" +
                             std::to_wstring(h.size) + L"|" + std::to_wstring(h.height);
    if (key == quad.helmetHinted || (top < 0 && bottom < 0))
        return;
    quad.helmetHinted = key;
    int cropTop = 0, cropBottom = 0;
    helmet_crop_hint(h, helmet_set().helmet_aspect(), top, bottom < 0 ? -1 : 1 - bottom, full[2], full[3], cropTop,
                     cropBottom);
    logf("helmet: pieces at size %d deg, height %d deg: their opening straight ahead meets [crop] top=%d bottom=%d",
         h.size, h.height, cropTop, cropBottom);
}

// The composed eyes' size in the output (under quad.mutex), from the top left of each eye's half:
// native, or with the FOV crop its kept share (the same density over the cropped FOV). The crop ACE
// located its views with, so a live change follows ACE's frames (at most a frame apart).
XrExtent2Di views4_output_extent() {
    const auto kept = [](uint32_t native, double share) {
        return static_cast<int32_t>(std::clamp<long>(std::lround(native * share), 1, static_cast<long>(native)));
    };
    return {kept(quad.nativeW, quad.v4Crop.kept_width()), kept(quad.nativeH, quad.v4Crop.kept_height())};
}

// With the helmet on, every 10 s (under quad.mutex): the helmet's light estimate in the log, per eye and
// side, linear RGB, so a log shows whether it follows the scene (a tunnel, a bright sky). The copy is
// asked for here and logged at a later composition, once the GPU has made it.
void log_helmet_light() {
    float rgb[2][4][3];
    if (helmet_set().read_light(rgb)) {
        char text[512];
        int n = 0;
        static const char* const kSides[4] = {"left", "right", "top", "bottom"};
        for (int e = 0; e < 2; ++e) {
            n += std::snprintf(text + n, sizeof(text) - n, "%seye %d", e ? "; " : "", e);
            for (int s = 0; s < 4; ++s)
                n += std::snprintf(text + n, sizeof(text) - n, " %s %.3f/%.3f/%.3f", kSides[s], rgb[e][s][0],
                                   rgb[e][s][1], rgb[e][s][2]);
        }
        logf("helmet light (linear RGB per side of the rendered view): %s", text);
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= quad.helmetLightLogAt) {
        helmet_set().request_light();
        quad.helmetLightLogAt = now + 10000;
    }
}

// A frame record for a frame the game recorded nothing for (before its first, or its projection unknown): the
// eyes' FOVs, the focus box centred on each, unturned.
Views4Frame views4_unrecorded_frame(const XrFovf (&fov)[2]) {
    Views4Frame frame{};
    for (int e = 0; e < 2; ++e) {
        fov_tangents(fov[e], frame.outerTan[e]);
        double hw, hh;
        focus_extents(frame.outerTan[e], experiment.box_x(), experiment.box_y(), hw, hh);
        const float focusTan[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                   static_cast<float>(-hh)};
        std::memcpy(frame.focusTan[e], focusTan, sizeof(focusTan));
        frame.turn[e] = {0, 0, 0, 1};
    }
    return frame;
}

// Where view `view` sits in the game's image: its rect in the game's target (targetW x targetH, at `origin` in a
// texture of textureW x textureH), as a texture region (u0, v0, du, dv).
void views4_source(const Views4Layout& layout, int view, bool eyeColumns, int targetW, int targetH, XrOffset2Di origin,
                   int textureW, int textureH, float (&uv)[4]) {
    const ViewRect r = eyeColumns ? views4_rect_eye_columns(layout, view, targetW, targetH)
                                  : views4_rect(layout, view, targetW, targetH);
    uv[0] = static_cast<float>(origin.x + r.left) / static_cast<float>(textureW);
    uv[1] = static_cast<float>(origin.y + r.top) / static_cast<float>(textureH);
    uv[2] = static_cast<float>(r.width) / static_cast<float>(textureW);
    uv[3] = static_cast<float>(r.height) / static_cast<float>(textureH);
}

// One eye's composition, for either compositor: what its views were rendered with (the frame's record: outer and
// focus tangents, the focus turn), where they sit in the game's image (`source`: the outer view e, the focus view
// e + 2), and the look from the live settings (feather, outline, corners, shape).
template <class Source>
void views4_eye(const Views4Frame& frame, int e, const Source& source, EyeComposition& c) {
    std::memcpy(c.outerTan, frame.outerTan[e], sizeof(c.outerTan));
    std::memcpy(c.focusTan, frame.focusTan[e], sizeof(c.focusTan));
    // The focus camera is the eye's turned by turn[e] (in the eye's frame): a direction in the outer view's
    // frame is in the focus view's frame after the inverse turn.
    to_matrix(qconj(frame.turn[e]), c.rotation);
    source(e, c.outerSource);
    source(e + 2, c.focusSource);
    c.feather = static_cast<float>(experiment.feather);
    // The outline (debug_border) is drawn by the compositor along the focus area's exact edge, turned and
    // rounded as it is.
    c.border = experiment.border ? 0.008f : 0.0f;
    std::memcpy(c.borderColor, experiment.borderColor, sizeof(c.borderColor));
    c.cornerRadius = static_cast<float>(experiment.cornerRadius);
    focus_shape_insets(experiment.focusShape.load(std::memory_order_relaxed), c.cornerInset);
}

// The helmet's part of one composition (under quad.mutex; for the D3D11 compositor, under v4d3d11.mutex too), for
// either compositor: its pieces looked at, the visor's slide, and whether the pieces go into their own layer or
// (that layer failed) into the scene image over the whole eye. The built-in look draws its lip inside the
// rendered view.
struct V4Helmet {
    bool pieces = false, piecesInScene = false;
    double visorOffset = 0;
    float fullView[2][4] = {};  // each eye's uncropped view (the helmet layer covers both)
};
V4Helmet views4_helmet_start(const HelmetSettings& helmet) {
    V4Helmet h;
    if (helmet.on)
        helmet_pieces(helmet);
    h.visorOffset = helmet.on ? visor_offset(helmet, visor_step(helmet)) : 0;
    h.pieces = helmet.on && helmet_set().has_helmet_pieces();
    h.piecesInScene = h.pieces && quad.helmetLayerFailed;
    return h;
}

// Eye e's helmet and output (as views4_helmet_start), once views4_eye has filled `c` from the frame: the FOV the
// output is submitted with, and the eye's size in the output from the top left of its half (native, or with the
// FOV crop its kept share; with the pieces in the scene, the whole uncropped eye at native size).
void views4_helmet_eye(const HelmetSettings& helmet, V4Helmet& h, int e, EyeComposition& c, XrFovf& submitFov,
                       XrExtent2Di& extent) {
    // The output covers the outer view's tangents, so that is the FOV it is submitted with.
    submitFov = tangents_fov(c.outerTan);
    float full[4];
    uncrop_tangents(c.outerTan, quad.v4Crop, e, full);
    std::memcpy(h.fullView[e], full, sizeof(full));
    if (h.pieces) {
        // The crop hint once the game renders the located view (not its own projection on the loading screens).
        float located[4];
        fov_tangents(quad.v4Fov[e], located);
        bool rendersLocated = true;
        for (int i = 0; i < 4; ++i)
            rendersLocated &= std::fabs(c.outerTan[i] - located[i]) < 0.01f * std::max(1.0f, std::fabs(located[i]));
        if (e == 0 && rendersLocated)
            helmet_crop_log(helmet, full);
    }
    if (helmet.on && !h.pieces) {
        // The built-in look, inside the rendered view: its lip reaches in from the opening's edge.
        const HelmetShape shape = helmet_shape(helmet, full, c.outerTan);
        std::memcpy(c.helmetOpening, shape.opening, sizeof(shape.opening));
        c.helmetRim = shape.rim;
        c.helmetRadius = shape.radius;
        c.helmetLight = static_cast<float>(helmet.light) / 100.0f;
        c.helmet = helmet.debugLight ? 3 : 1;
    }
    if (h.piecesInScene) {
        // Fallback (no layer of their own): the output covers the whole uncropped eye at native size;
        // the pieces fill what the game did not render.
        std::memcpy(c.viewTan, full, sizeof(full));
        submitFov = tangents_fov(full);
        extent = {static_cast<int32_t>(quad.nativeW), static_cast<int32_t>(quad.nativeH)};
        const HelmetShape shape = helmet_shape(helmet, full, c.outerTan);
        std::memcpy(c.helmetOpening, shape.opening, sizeof(shape.opening));
        c.helmetRim = shape.rim;
        c.helmetRadius = shape.radius;
        c.helmetLight = static_cast<float>(helmet.light) / 100.0f;
        c.helmet = helmet.debugLight ? 3 : 2;
        // The pieces, fixed to the head: the eye's turn against the head, then the images' height.
        const double tilt = helmet.height * kHelmetDegrees;
        const XrQuaternionf up{static_cast<float>(std::sin(tilt / 2)), 0, 0, static_cast<float>(std::cos(tilt / 2))};
        to_matrix(qmul(qconj(up), quad.v4EyeTurn[e]), c.helmetRotation);
        double tx, ty;
        helmet_tan_half(helmet, helmet_set().helmet_aspect(), tx, ty);
        c.helmetTanHalf[0] = static_cast<float>(tx);
        c.helmetTanHalf[1] = static_cast<float>(ty);
        const XrVector3f offset = rotate(qconj(up), quad.v4EyeOffset[e]);  // in the images' frame
        const float eyeOffset[3] = {offset.x, offset.y, offset.z};
        helmet_parallax(helmet, eyeOffset, c.helmetShift);
        c.visorOffset = static_cast<float>(h.visorOffset);
        c.visorBlend = helmet.visorBlend;
        return;
    }
    // The FOV crop: the output keeps native density over the cropped FOV (views4_output_extent).
    extent = views4_output_extent();
    c.outputSize[0] = static_cast<float>(extent.width) / static_cast<float>(quad.nativeW);
    c.outputSize[1] = static_cast<float>(extent.height) / static_cast<float>(quad.nativeH);
}

// After a composition (as views4_helmet_start): the helmet's own layer drawn for it, unless its size changed
// ([helmet] detail: the next submission makes it again). `views11`: as helmet_layer_draw's.
void views4_helmet_layer(const HelmetSettings& helmet, const V4Helmet& h, const EyeComposition (&eyes)[2],
                         ID3D11Texture2D* views11) {
    if (quad.helmetChain && helmet_layer_side(helmet) != quad.helmetSide)
        quad.helmetResize = true;
    quad.helmetDrawn = h.pieces && !h.piecesInScene && quad.helmetChain && !quad.helmetResize &&
                       helmet_layer_draw(helmet, eyes, h.fullView, h.visorOffset, views11);
}

// At the submission of composed projection layer `layer` (under quad.mutex): the helmet layer drawn for it goes
// over it (`quads`, `count`, `after`); its swapchain goes when its size changed, and is made once there are pieces
// (drawn from the next composition on).
void views4_helmet_submit(XrSession session, int layer, XrCompositionLayerQuad (&quads)[2], int& count, int& after) {
    if (quad.helmetDrawn) {
        count = quad.helmetQuadCount;
        std::copy(quad.helmetQuads, quad.helmetQuads + count, quads);
        after = layer;
    }
    quad.helmetDrawn = false;
    if (quad.helmetResize)
        helmet_layer_release();  // nothing of it submitted this frame
    if (helmetSettings.on && quad.helmetPiecesLoaded)
        helmet_layer_resources(session);
}

// views4: ACE's eye image holds the outer views as views 0-1 and the focus views as views 2-3 of the
// layout (the focus cameras: focus_percent of each eye's tangent extent around its forward axis). Composes
// both eyes into the output chain at native size, on ACE's queue, before ACE releases the image.
void views4_compose() {
    quad.v4Composed = false;
    // What this frame's views were rendered with (ACE's own eye projections, the focus boxes and
    // turns, views4.h); before the first focus cameras are filled, the located FOV, unturned.
    // Taken before anything can skip this frame: a frame filled but not taken would leave every later
    // composition a frame behind, its focus placed by an older gaze (focus lagging the eyes, 2026-11-02).
    Views4Frame frame{};
    const bool recorded = game->qvfr_take_frame(frame);
    if (!quad.outChain || quad.appAcquired >= quad.appImages.size() || !quad.v4FovValid)
        return;
    if (!recorded)
        frame = views4_unrecorded_frame(quad.v4Fov);
    ID3D12Resource* app = quad.appImages[quad.appAcquired];
    EyeComposition eyes[2] = {};
    const HelmetSettings& helmet = helmetSettings;
    V4Helmet helmetFrame = views4_helmet_start(helmet);
    // Outer source = view e, focus source = view e + 2, wherever the layout puts them, at the sizes this frame was
    // rendered with (live resolution); launch sizes until the first record.
    const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
    const int w = static_cast<int>(quad.appW), h = static_cast<int>(quad.appH);
    const auto source = [&](int view, float (&uv)[4]) { views4_source(layout, view, false, w, h, {0, 0}, w, h, uv); };
    for (int e = 0; e < 2; ++e) {
        views4_eye(frame, e, source, eyes[e]);
        views4_helmet_eye(helmet, helmetFrame, e, eyes[e], quad.v4SubmitFov[e], quad.v4Extent[e]);
    }
    if (helmet.on)
        log_helmet_light();
    uint32_t outIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(quad.outChain, &acquire, &outIndex)) || outIndex >= quad.outImages.size()) {
        quad_fail("views4 output acquire failed");
        return;
    }
    ID3D12Resource* out = quad.outImages[outIndex];
    bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(quad.outChain, &wait)) && quad.compositor.capture_outer(app) &&
              quad.compositor.compose(app, out, eyes);
    if (ok && quad_capture_due())
        views4_capture(app, out);
    next.ReleaseSwapchainImage(quad.outChain, &release);
    if (!ok) {
        quad_fail("views4 compose failed");
        return;
    }
    quad.v4Composed = true;
    quad.fallbackRun = 0;
    views4_helmet_layer(helmet, helmetFrame, eyes, nullptr);
    if (quad.composed++ == 0)
        logf("views4: first composed frame: eye 0 outer tangents %.3f/%.3f/%.3f/%.3f, focus +-%.3f x +-%.3f, output "
             "%ux%u per eye",
             eyes[0].outerTan[0], eyes[0].outerTan[1], eyes[0].outerTan[2], eyes[0].outerTan[3], eyes[0].focusTan[1],
             eyes[0].focusTan[2], quad.nativeW, quad.nativeH);
}

// Frame A's image is copied before the runtime gets it back; frame B's stays acquired (so this
// layer may still read it) until B's xrEndFrame has composed it.
XrResult XRAPI_CALL layerReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
    if (experiment.d3d11CaptureFrame)
        d3d11_capture_release(swapchain, endFrames);
    if (views4_outline_on())
        views4_outline_draw(swapchain);  // on ACE's queue, after its frame, before the runtime reads it
    if (experiment.views4Compose) {
        std::lock_guard guard(quad.mutex);
        if (swapchain == quad.appChain) {
            gpu_frame_timer_mark(GpuMark::kRendered);
            const int64_t t0 = qpc_now();
            views4_compose();  // reads the image on ACE's queue before the runtime gets it back
            cpu.compose.add(qpc_now() - t0);
            gpu_frame_timer_mark(GpuMark::kComposed);
        }
    }
    if (experiment.quad) {
        bool active;
        {
            std::lock_guard guard(loops.mutex);
            active = loops.active;
        }
        std::lock_guard guard(quad.mutex);
        if (active && swapchain == quad.appChain && quad.appAcquired < quad.appImages.size()) {
            if (loops.inB) {
                quad.releaseHeld = true;
                return XR_SUCCESS;
            }
            quad.outerCaptured = quad.compositor.capture_outer(quad.appImages[quad.appAcquired]);
        }
    }
    return next.ReleaseSwapchainImage(swapchain, info);
}

// Frame A's xrEndFrame is swallowed: keep the views ACE rendered it with.
void quad_record_a(const XrFrameEndInfo* info) {
    std::lock_guard guard(quad.mutex);
    quad.aViewsValid = false;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        if (proj->viewCount != 2 || proj->views[0].subImage.swapchain != quad.appChain)
            continue;
        for (int e = 0; e < 2; ++e)
            quad.aViews[e] = {proj->views[e].pose, proj->views[e].fov, proj->views[e].subImage.imageRect};
        quad.aViewsValid = true;
        return;
    }
}

constexpr int kMaxCaptures = 8;

bool quad_capture_due() {
    if (!experiment.captureEverySeconds || quad.captures >= kMaxCaptures)
        return false;
    const ULONGLONG now = GetTickCount64();
    if (quad.lastCapture == 0) {  // first capture one interval after the pipeline starts
        quad.lastCapture = now;
        return false;
    }
    return now - quad.lastCapture >= experiment.captureEverySeconds * 1000ull;
}

// Called with quad.mutex held, after compose and before the output image is released.
void quad_capture(ID3D12Resource* focus, ID3D12Resource* output, const EyeComposition& eye) {
    quad.lastCapture = GetTickCount64();
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"cap%02d-%02u%02u%02u", ++quad.captures, t.wHour, t.wMinute, t.wSecond);
    const bool ok = quad.compositor.save_captures(focus, output, (dir / name).wstring());
    float cx, cy;
    focus_centre(eye, cx, cy);
    logf("quad: capture %ls %s; eye 0 frame B faces A tangent %.3f,%.3f, gaze %.3f,%.3f,%.3f", name,
         ok ? "saved" : "FAILED", cx, cy, gaze.direction.x, gaze.direction.y, gaze.direction.z);
}

// Called with quad.mutex held, after compose and before the output image is released.
void views4_capture(ID3D12Resource* views, ID3D12Resource* output) {
    quad.lastCapture = GetTickCount64();
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"v4cap%02d-%02u%02u%02u", ++quad.captures, t.wHour, t.wMinute, t.wSecond);
    const bool ok = quad.compositor.save_views4_captures(views, output, (dir / name).wstring());
    logf("views4: capture %ls %s (all four views, and the composed left eye)", name, ok ? "saved" : "FAILED");
}

// Frame B's xrEndFrame: composes A (outer) and B (focus) into the output swapchain and submits it in
// place of ACE's projection layer. Any missing piece shows ACE's frame B as it is (the focus box
// with black around it) rather than dropping the frame.
XrResult quad_submit_b(XrSession session, const XrFrameEndInfo* info) {
    std::unique_lock lock(quad.mutex);
    auto releaseHeld = [&] {
        if (quad.releaseHeld) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            next.ReleaseSwapchainImage(quad.appChain, &release);
            quad.releaseHeld = false;
        }
    };
    auto fallback = [&](const char* why) {
        quad_fail(why);
        releaseHeld();
        lock.unlock();
        return next.EndFrame(session, info);
    };
    int projIndex = -1;
    for (uint32_t i = 0; i < info->layerCount && projIndex < 0; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        if (proj->viewCount == 2 && proj->views[0].subImage.swapchain == quad.appChain)
            projIndex = static_cast<int>(i);
    }
    if (projIndex < 0) {  // ACE submitted no eye view this frame (shouldRender false, loading): nothing to compose
        releaseHeld();
        lock.unlock();
        return next.EndFrame(session, info);
    }
    if (!quad.outChain)
        return fallback("no output swapchain");
    if (!quad.releaseHeld || quad.appAcquired >= quad.appImages.size())
        return fallback("frame B's image was not held");
    if (!quad.outerCaptured || !quad.aViewsValid)
        return fallback("frame A missing");

    auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[projIndex]);
    EyeComposition eyes[2] = {};
    const float appW = static_cast<float>(quad.appW), appH = static_cast<float>(quad.appH);
    for (int e = 0; e < 2; ++e) {
        const EyeView& a = quad.aViews[e];
        const XrCompositionLayerProjectionView& b = proj->views[e];
        const float outerTan[4] = {std::tan(a.fov.angleLeft), std::tan(a.fov.angleRight), std::tan(a.fov.angleUp),
                                   std::tan(a.fov.angleDown)};
        const float focusTan[4] = {std::tan(b.fov.angleLeft), std::tan(b.fov.angleRight), std::tan(b.fov.angleUp),
                                   std::tan(b.fov.angleDown)};
        std::memcpy(eyes[e].outerTan, outerTan, sizeof(outerTan));
        std::memcpy(eyes[e].focusTan, focusTan, sizeof(focusTan));
        // A direction in A's eye frame, to world, to B's eye frame. Also absorbs any head turn
        // between the two frames.
        to_matrix(qmul(qconj(b.pose.orientation), a.pose.orientation), eyes[e].rotation);
        const XrRect2Di& ar = a.rect;
        const XrRect2Di& br = b.subImage.imageRect;
        const float outer[4] = {ar.offset.x / appW, ar.offset.y / appH, ar.extent.width / appW, ar.extent.height / appH};
        const float focus[4] = {br.offset.x / appW, br.offset.y / appH, br.extent.width / appW, br.extent.height / appH};
        std::memcpy(eyes[e].outerSource, outer, sizeof(outer));
        std::memcpy(eyes[e].focusSource, focus, sizeof(focus));
        eyes[e].feather = static_cast<float>(experiment.feather);
        eyes[e].border = experiment.border ? 0.008f : 0.0f;
        std::memcpy(eyes[e].borderColor, experiment.borderColor, sizeof(eyes[e].borderColor));
        eyes[e].cornerRadius = static_cast<float>(experiment.cornerRadius);
        focus_shape_insets(experiment.focusShape.load(std::memory_order_relaxed), eyes[e].cornerInset);
    }

    uint32_t outIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(quad.outChain, &acquire, &outIndex)) || outIndex >= quad.outImages.size())
        return fallback("output acquire failed");
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(quad.outChain, &wait)) &&
                    quad.compositor.compose(quad.appImages[quad.appAcquired], quad.outImages[outIndex], eyes);
    if (ok && quad_capture_due())
        quad_capture(quad.appImages[quad.appAcquired], quad.outImages[outIndex], eyes[0]);
    next.ReleaseSwapchainImage(quad.outChain, &release);
    if (!ok)
        return fallback("compose failed");
    releaseHeld();

    XrCompositionLayerProjectionView views[2];
    for (int e = 0; e < 2; ++e) {
        views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[e].pose = quad.aViews[e].pose;
        views[e].fov = quad.aViews[e].fov;
        views[e].subImage.swapchain = quad.outChain;
        views[e].subImage.imageRect = {{static_cast<int32_t>(e * quad.nativeW), 0},
                                       {static_cast<int32_t>(quad.nativeW), static_cast<int32_t>(quad.nativeH)}};
    }
    XrCompositionLayerProjection layer = *proj;
    layer.next = nullptr;
    layer.viewCount = 2;
    layer.views = views;
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    layers[projIndex] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    XrFrameEndInfo end = *info;
    end.layers = layers.data();
    quad.fallbackRun = 0;
    if (quad.composed++ == 0) {
        float cx, cy;
        focus_centre(eyes[0], cx, cy);
        logf("quad: first composed frame: eye 0 frame B faces A tangent %.3f,%.3f with tangents %.3f/%.3f/%.3f/%.3f, "
             "outer src %.3f,%.3f %.3fx%.3f, focus src %.3f,%.3f %.3fx%.3f",
             cx, cy, eyes[0].focusTan[0], eyes[0].focusTan[1], eyes[0].focusTan[2], eyes[0].focusTan[3],
             eyes[0].outerSource[0], eyes[0].outerSource[1], eyes[0].outerSource[2], eyes[0].outerSource[3],
             eyes[0].focusSource[0], eyes[0].focusSource[1], eyes[0].focusSource[2], eyes[0].focusSource[3]);
    }
    lock.unlock();
    return next.EndFrame(session, &end);
}

// views4 composing: the game draws each eye's hidden-area mesh into every view, so the focus views got the
// lens mask's black corners in their own frame (ACE: composition run 2; ACC through OpenComposite: a dark rim
// round the focus area, 2026-11-04). views4_mask=0 answers the hidden
// mesh with one zero-area triangle: nothing is masked in any view (the lenses hide those pixels anyway).
bool visibilityMaskLogged = false;
XrResult XRAPI_CALL layerGetVisibilityMaskKHR(XrSession session, XrViewConfigurationType type, uint32_t viewIndex,
                                              XrVisibilityMaskTypeKHR maskType, XrVisibilityMaskKHR* mask) {
    if (!experiment.views4NoMask || maskType != XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR || !mask)
        return next.GetVisibilityMaskKHR(session, type, viewIndex, maskType, mask);
    mask->vertexCountOutput = 3;
    mask->indexCountOutput = 3;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < 3 || mask->indexCapacityInput < 3 || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < 3; ++i) {
        mask->vertices[i] = {0.0f, 0.0f};
        mask->indices[i] = i;
    }
    if (!visibilityMaskLogged) {
        visibilityMaskLogged = true;
        log_line("views4: hidden-area mesh replaced by an empty one (views4_mask=0)");
    }
    return XR_SUCCESS;
}

XrResult locate_views(XrSession session, const XrViewLocateInfo* info, XrViewState* state, uint32_t capacity,
                      uint32_t* countOutput, XrView* views) {
    XrResult r = next.LocateViews(session, info, state, capacity, countOutput, views);
    // The FOV crop: ACE renders, and submits, the cropped FOV (everything below follows it).
    const FovCrop crop = experiment.crop();
    const bool stereo = XR_SUCCEEDED(r) && views && countOutput && std::min(*countOutput, capacity) >= 2 &&
                        info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (stereo)
        for (int i = 0; i < 2; ++i)
            views[i].fov = crop_fov(views[i].fov, crop, i);
    if (experiment.views4 && XR_SUCCEEDED(r) && views && countOutput) {
        game->qvfr_note_views(views, std::min(*countOutput, capacity));
        if (views4_outline_on() && *countOutput >= 2 && capacity >= 2) {
            std::lock_guard guard(v4outline.mutex);
            v4outline.fov[0] = views[0].fov;
            v4outline.fov[1] = views[1].fov;
            v4outline.fovValid = true;
        }
        if (experiment.composing() && *countOutput >= 2 && capacity >= 2) {
            {
                std::lock_guard guard(quad.mutex);
                quad.v4Fov[0] = views[0].fov;
                quad.v4Fov[1] = views[1].fov;
                if (stereo) {
                    quad.v4Crop = crop;
                    // Each eye against the head (the eyes' middle orientation): canted displays turn the eyes.
                    XrQuaternionf a = views[0].pose.orientation, b = views[1].pose.orientation;
                    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0)
                        b = {-b.x, -b.y, -b.z, -b.w};
                    XrQuaternionf head{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
                    const float n = std::sqrt(head.x * head.x + head.y * head.y + head.z * head.z + head.w * head.w);
                    if (n > 1e-6f) {
                        head = {head.x / n, head.y / n, head.z / n, head.w / n};
                        quad.v4EyeTurn[0] = qmul(qconj(head), views[0].pose.orientation);
                        quad.v4EyeTurn[1] = qmul(qconj(head), views[1].pose.orientation);
                        // Each eye from the eyes' middle (the helmet's depth): half the IPD, in the head's frame.
                        const XrVector3f& p0 = views[0].pose.position;
                        const XrVector3f& p1 = views[1].pose.position;
                        const XrVector3f half{(p0.x - p1.x) / 2, (p0.y - p1.y) / 2, (p0.z - p1.z) / 2};
                        quad.v4EyeOffset[0] = rotate(qconj(head), half);
                        quad.v4EyeOffset[1] = {-quad.v4EyeOffset[0].x, -quad.v4EyeOffset[0].y, -quad.v4EyeOffset[0].z};
                    }
                }
                quad.v4FovValid = true;
            }
            // Gaze: where the gaze ray points in each eye's frame (canted displays rotate each eye), as
            // tangents; the focus cameras turn towards it, clamped against the FOV ACE renders with
            // (fill_focus_cameras). Untracked gaze recentres after kGazeHoldNs. The vertical offset
            // (focus_offset.h) moves the tracked gaze, or the fixed box off each eye's forward axis.
            // A head-locked call (OpenComposite's eyes-to-head query) leaves the gaze to the scene-space one
            // (gaze_skips_space).
            if (!(experiment.gaze && gaze_skips_space(info->space, info->displayTime))) {
                float tan[2][2] = {};
                bool valid[2] = {};
                XrVector3f direction{0, 0, -1};
                const bool gazing = experiment.gaze && gaze_now(session, info->space, info->displayTime, direction);
                steady_gaze(gazing, direction);
                const int offset = gazing ? experiment.gazeOffsetY : experiment.fixedOffsetY;
                for (int e = 0; e < 2; ++e) {
                    if (gazing) {
                        const XrVector3f d = rotate(qconj(views[e].pose.orientation), direction);
                        valid[e] = d.z < -0.1f;
                        if (valid[e]) {
                            tan[e][0] = d.x / -d.z;
                            tan[e][1] = d.y / -d.z;
                        }
                    } else {
                        valid[e] = offset != 0;
                    }
                    if (valid[e] && offset != 0)
                        tan[e][1] = static_cast<float>(offset_tan_y(tan[e][1], offset));
                }
                game->qvfr_set_gaze(tan, valid, gazing);
            }
        }
    }
    const bool narrowThisFrame = experiment.focusOnly || (experiment.twoLoops && experiment.focusInB && loops.inB);
    if (narrowThisFrame && capacity > 0 && XR_SUCCEEDED(r)) {
        XrVector3f direction{0, 0, -1};
        const bool gazing = experiment.quad && experiment.gaze &&
                            gaze_direction(session, info->space, info->displayTime, direction);
        if (experiment.quad)
            steady_gaze(gazing, direction);
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i) {
            const XrFovf before = views[i].fov;
            double tanX = 0, tanY = 0;
            if (gazing) {  // gaze ray direction in this eye's frame (canted displays rotate each eye)
                const XrQuaternionf& q = views[i].pose.orientation;
                const XrVector3f d = rotate({-q.x, -q.y, -q.z, q.w}, direction);
                if (d.z < -0.1f) {
                    tanX = d.x / -d.z;
                    tanY = d.y / -d.z;
                }
            }
            if (experiment.quad)  // the vertical offset (focus_offset.h), on the gaze or the fixed box
                tanY = offset_tan_y(tanY, gazing ? experiment.gazeOffsetY : experiment.fixedOffsetY);
            if (experiment.quad) {
                turn_view(views[i], experiment.focusFraction, experiment.focus_y(), tanX, tanY);
            } else {
                narrow(views[i].fov.angleLeft, views[i].fov.angleRight, experiment.focusFraction, tanX);
                narrow(views[i].fov.angleDown, views[i].fov.angleUp, experiment.focus_y(), tanY);
            }
            if (!focusLogged)
                logf("experiment focus_only view=%u fov %.4f/%.4f/%.4f/%.4f -> %.4f/%.4f/%.4f/%.4f", i,
                     before.angleLeft, before.angleRight, before.angleUp, before.angleDown, views[i].fov.angleLeft,
                     views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown);
        }
        focusLogged = true;
    }
    if (capacity > 0 && sampled(++locates) && XR_SUCCEEDED(r))
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
            logf("xrLocateViews call=%llu config=%d time=%lld view=%u fov=%.4f/%.4f/%.4f/%.4f pos=%.4f,%.4f,%.4f",
                 static_cast<unsigned long long>(locates), info->viewConfigurationType,
                 static_cast<long long>(info->displayTime), i, views[i].fov.angleLeft, views[i].fov.angleRight,
                 views[i].fov.angleUp, views[i].fov.angleDown, views[i].pose.position.x, views[i].pose.position.y,
                 views[i].pose.position.z);
    return r;
}

// views4: ACE's two "eyes" are each an outer plus a focus view wide and hold views 0+1 and 2+3 side
// by side (views4_layout.h). Submit views 0 and 1, the real eyes, as the projection views.
bool views4RectsLogged = false;
bool v4EyesOnlyLogged = false;  // views4_submit: an eyes-only frame (Views4Frame::eyesOnly) was reported

// Logs ACE's own submitted FOV next to the one the output is submitted with, once and on a change.
void log_submitted_fov(uint32_t eye, const XrFovf& submitted, const XrFovf& composed) {
    XrFovf& last = quad.v4LoggedGameFov[eye];
    const auto same = [](const XrFovf& a, const XrFovf& b, float eps) {
        return std::fabs(a.angleLeft - b.angleLeft) < eps && std::fabs(a.angleRight - b.angleRight) < eps &&
               std::fabs(a.angleUp - b.angleUp) < eps && std::fabs(a.angleDown - b.angleDown) < eps;
    };
    if (same(submitted, last, 0.002f))
        return;
    last = submitted;
    logf("views4: eye %u ACE submits FOV %.4f/%.4f/%.4f/%.4f, output submitted with %.4f/%.4f/%.4f/%.4f (its "
         "rendered projection)%s",
         eye, submitted.angleLeft, submitted.angleRight, submitted.angleUp, submitted.angleDown, composed.angleLeft, composed.angleRight,
         composed.angleUp, composed.angleDown, same(submitted, composed, 0.002f) ? "" : "  <-- DIFFERENT");
}

// The D3D11 output chain and compositor, made at the first composition (the game's format, the native eye size,
// double wide). False: v4d3d11.failed, with the reason logged once.
bool v4d3d11_resources(XrSession session, int64_t format) {
    if (v4d3d11.outChain)
        return true;
    if (v4d3d11.failed)
        return false;
    v4d3d11.failed = true;  // until everything below is made
    uint32_t nativeW, nativeH;
    ID3D11Device* device;
    {
        std::lock_guard guard(quad.mutex);
        nativeW = quad.nativeW;
        nativeH = quad.nativeH;
        device = sessionGraphics.d3d11;
    }
    if (!device || !nativeW || !nativeH) {
        log_line("views4 d3d11: no D3D11 device or native eye size; the headset shows views 0-1");
        return false;
    }
    if (!v4d3d11.compositor.initialize(device, static_cast<DXGI_FORMAT>(format))) {
        log_line("views4 d3d11: compositor unavailable; the headset shows views 0-1");
        return false;
    }
    XrSwapchainCreateInfo out{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    out.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    out.format = format;
    out.sampleCount = out.faceCount = out.arraySize = out.mipCount = 1;
    out.width = 2 * nativeW;
    out.height = nativeH;
    XrSwapchain chain = XR_NULL_HANDLE;
    XrResult r = next.CreateSwapchain(session, &out, &chain);
    uint32_t count = 0;
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r)) {
        logf("views4 d3d11: output swapchain %ux%u failed (%d); the headset shows views 0-1", out.width, out.height, r);
        if (chain)
            next.DestroySwapchain(chain);
        v4d3d11.compositor.shutdown();
        return false;
    }
    v4d3d11.outChain = chain;
    for (const auto& image : images)
        v4d3d11.outImages.push_back(image.texture);
    d3d11_capture_images(chain, images.data(), count);  // d3d11_capture_frame saves the composed output too
    v4d3d11.outW = nativeW;
    v4d3d11.outH = nativeH;
    v4d3d11.failed = false;
    logf("views4 d3d11: output chain %p %ux%u (%u images, format %lld), focus box %.0f%% x %.0f%%, feather %.0f%%",
         static_cast<void*>(chain), out.width, out.height, count, static_cast<long long>(format),
         experiment.box_x() * 100, experiment.box_y() * 100, experiment.feather * 100);
    return true;
}

// Composes both eyes of `proj` (the game's two eye views, eye columns of `layout`) into the output chain and
// points `copy` (its views) at it. False: nothing changed, the caller submits views 0-1 as they are.
bool v4d3d11_compose(XrSession session, const XrCompositionLayerProjection* proj,
                     std::vector<XrCompositionLayerProjectionView>& copy) {
    // What this frame's views were rendered with (the game's record, frame_records.h), taken before anything can
    // skip the frame, so later compositions stay in step with the frames rendered.
    Views4Frame frame{};
    const bool recorded = game->qvfr_take_frame(frame);
    std::lock_guard guard(v4d3d11.mutex);
    const auto fallback = [](const char* why) {
        ++v4d3d11.fallbacks;
        if (v4d3d11.lastFallback != why)
            logf("views4 d3d11: not composed (%s); the headset shows views 0-1", why);
        v4d3d11.lastFallback = why;
        return false;
    };
    const auto it = v4d3d11.chains.find(proj->views[0].subImage.swapchain);
    if (it == v4d3d11.chains.end() || it->second.acquired >= it->second.images.size())
        return fallback("the game's eye image is unknown");
    if (!v4d3d11_resources(session, it->second.format))
        return false;
    ID3D11Texture2D* views = it->second.images[it->second.acquired];
    D3D11_TEXTURE2D_DESC desc;
    views->GetDesc(&desc);
    const float texW = static_cast<float>(desc.Width), texH = static_cast<float>(desc.Height);
    // Before the game's first record (or with no projection in it), the submitted FOVs, centred and unturned.
    if (!recorded || !(frame.outerTan[0][1] > frame.outerTan[0][0]) || !(frame.outerTan[1][1] > frame.outerTan[1][0]))
        frame = views4_unrecorded_frame({proj->views[0].fov, proj->views[1].fov});
    // The game's target as the eyes' rects give it (eye e: the half from e x W), as views4_submit reads it.
    const XrExtent2Di eye = proj->views[0].subImage.imageRect.extent;
    const XrOffset2Di origin = proj->views[0].subImage.imageRect.offset;
    const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
    const auto source = [&](int view, float (&uv)[4]) {
        views4_source(layout, view, true, 2 * eye.width, eye.height, origin, static_cast<int>(desc.Width),
                      static_cast<int>(desc.Height), uv);
    };
    // The helmet and the FOV crop, as the D3D12 path (views4_helmet_eye): each eye's submitted FOV and its size in
    // the output, from the top left of its half.
    std::lock_guard quadGuard(quad.mutex);
    const HelmetSettings& helmet = helmetSettings;
    V4Helmet helmetFrame = views4_helmet_start(helmet);
    EyeComposition eyes[2] = {};
    XrFovf submitFov[2];
    XrExtent2Di extent[2];
    for (int e = 0; e < 2; ++e) {
        views4_eye(frame, e, source, eyes[e]);
        views4_helmet_eye(helmet, helmetFrame, e, eyes[e], submitFov[e], extent[e]);
    }
    if (helmet.on)
        log_helmet_light();
    uint32_t outIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(v4d3d11.outChain, &acquire, &outIndex)) ||
        outIndex >= v4d3d11.outImages.size())
        return fallback("output acquire failed");
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(v4d3d11.outChain, &wait)) &&
                    v4d3d11.compositor.compose(views, v4d3d11.outImages[outIndex], eyes);
    if (ok && experiment.d3d11CaptureFrame) {
        d3d11_capture_acquired(v4d3d11.outChain, outIndex);
        d3d11_capture_release(v4d3d11.outChain, endFrames);
    }
    next.ReleaseSwapchainImage(v4d3d11.outChain, &release);
    if (!ok)
        return fallback("compose failed");
    views4_helmet_layer(helmet, helmetFrame, eyes, views);
    if (v4d3d11.composed++ == 0 || v4d3d11.lastFallback) {
        logf("views4 d3d11: composing (eye 0: outer %.0f,%.0f %.0fx%.0f, focus %.0f,%.0f %.0fx%.0f of the game's "
             "%ux%u image; focus tangents +-%.3f x +-%.3f)",
             eyes[0].outerSource[0] * texW, eyes[0].outerSource[1] * texH, eyes[0].outerSource[2] * texW,
             eyes[0].outerSource[3] * texH, eyes[0].focusSource[0] * texW, eyes[0].focusSource[1] * texH,
             eyes[0].focusSource[2] * texW, eyes[0].focusSource[3] * texH, desc.Width, desc.Height, eyes[0].focusTan[1],
             eyes[0].focusTan[2]);
        v4d3d11.lastFallback = nullptr;
    }
    for (uint32_t v = 0; v < 2; ++v) {
        // The game's own pose; the FOV the output was composed over (the game's rendered projection, or with the
        // helmet's pieces in the scene the uncropped eye), which the runtime must display it with.
        copy[v].fov = submitFov[v];
        copy[v].next = nullptr;  // depth info would describe the game's views, not the output
        copy[v].subImage.swapchain = v4d3d11.outChain;
        copy[v].subImage.imageRect = {{static_cast<int32_t>(v * v4d3d11.outW), 0}, extent[v]};
        copy[v].subImage.imageArrayIndex = 0;
    }
    return true;
}

XrResult views4_submit(XrSession session, const XrFrameEndInfo* info) {
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    XrCompositionLayerQuad helmetLayers[2] = {};  // the helmet's own layer (its bands), over the composed scene
    int helmetCount = 0, helmetAfter = -1;
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<std::vector<XrCompositionLayerProjectionView>> views;
    projections.reserve(info->layerCount);
    views.reserve(info->layerCount);
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layers[i]);
        if (views4_outline_on() && proj->viewCount > 0) {
            std::lock_guard guard(v4outline.mutex);
            if (!v4outline.chain)
                v4outline.chain = proj->views[0].subImage.swapchain;  // ACE's eye chain
        }
        auto& copy = views.emplace_back(proj->views, proj->views + proj->viewCount);
        if (experiment.views4Compose && proj->viewCount == 2) {
            std::lock_guard guard(quad.mutex);
            if (quad.v4Composed && proj->views[0].subImage.swapchain == quad.appChain) {
                // The composed output, at native size, with ACE's own pose and FOV for each eye.
                quad.v4Composed = false;
                for (uint32_t v = 0; v < 2; ++v) {
                    log_submitted_fov(v, copy[v].fov, quad.v4SubmitFov[v]);
                    // The FOV the output was composed over (ACE's rendered projection), which is what
                    // the runtime must display it with, whatever ACE's own projection view says.
                    copy[v].fov = quad.v4SubmitFov[v];
                    copy[v].next = nullptr;  // depth info would describe ACE's quarters, not the output
                    copy[v].subImage.swapchain = quad.outChain;
                    copy[v].subImage.imageRect = {{static_cast<int32_t>(v * quad.nativeW), 0}, quad.v4Extent[v]};
                    copy[v].subImage.imageArrayIndex = 0;
                }
                projections.push_back(*proj);
                projections.back().views = copy.data();
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
                views4_helmet_submit(session, static_cast<int>(i), helmetLayers, helmetCount, helmetAfter);
                continue;
            }
        }
        Views4Frame latest{};
        game->qvfr_latest_frame(latest);  // the rendered sizes (live resolution)
        // The game drew only its eyes this frame, each over its whole half (X-Plane's menu): submitted as it is.
        if (latest.eyesOnly) {
            if (!v4EyesOnlyLogged) {
                logf("views4: the game drew its eyes only (no four views): submitted as it is, uncomposed");
                v4EyesOnlyLogged = true;
            }
            continue;
        }
        v4EyesOnlyLogged = false;
        const Views4Layout layout = views4_with_live(experiment.views4Layout, latest.live, latest.width, latest.height);
        bool gameLayout = true;
        for (uint32_t v = 0; v < proj->viewCount; ++v)
            gameLayout &= copy[v].subImage.imageRect.offset.x == static_cast<int32_t>(v) * copy[v].subImage.imageRect.extent.width;
        if (!gameLayout)
            continue;
        if (experiment.views4ComposeD3D11 && proj->viewCount == 2 && game->qvfr_eye_columns() &&
            v4d3d11_compose(session, proj, copy)) {
            projections.push_back(*proj);
            projections.back().views = copy.data();
            layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
            std::lock_guard guard(quad.mutex);
            views4_helmet_submit(session, static_cast<int>(i), helmetLayers, helmetCount, helmetAfter);
            continue;
        }
        for (uint32_t v = 0; v < proj->viewCount; ++v) {
            // ACE submits halves of its target (W/2 x H each): point them at view v (or v + 2). A game in eye
            // columns (ACC) has eye v's views in its half: keep the offset, move and cut to the view.
            XrRect2Di& rect = copy[v].subImage.imageRect;
            const int shown = static_cast<int>(v + (experiment.views4ShowCopies ? 2 : 0));
            const ViewRect view = game->qvfr_eye_columns()
                                      ? views4_rect_eye_columns(layout, shown, 2 * rect.extent.width, rect.extent.height)
                                      : views4_rect(layout, shown, 2 * rect.extent.width, rect.extent.height);
            if (game->qvfr_eye_columns()) {
                rect.offset.x += view.left - static_cast<int>(v) * rect.extent.width;
                rect.offset.y += view.top;
            } else {
                rect.offset.x = view.left;
            }
            rect.extent.width = view.width;
            rect.extent.height = view.height;
            if (!views4RectsLogged)
                logf("views4: projection view %u submitted as rect=%d,%d %dx%d", v, rect.offset.x, rect.offset.y,
                     rect.extent.width, rect.extent.height);
        }
        views4RectsLogged = true;
        projections.push_back(*proj);
        projections.back().views = copy.data();
        layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
    }
    for (int b = helmetCount - 1; b >= 0; --b)  // over the scene, under the overlay and the menu
        layers.insert(layers.begin() + helmetAfter + 1, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&helmetLayers[b]));
    XrFrameEndInfo submitted = *info;
    submitted.layers = layers.data();
    submitted.layerCount = static_cast<uint32_t>(layers.size());
    return next.EndFrame(session, &submitted);
}

XrResult XRAPI_CALL layerLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                     uint32_t capacity, uint32_t* countOutput, XrView* views) {
    if (!cpu.realLocateViews)
        return locate_views(session, info, state, capacity, countOutput, views);
    const int64_t t0 = qpc_now();
    const XrResult r = locate_views(session, info, state, capacity, countOutput, views);
    cpu.locate.add(qpc_now() - t0);
    return r;
}

XrResult end_frame(XrSession session, const XrFrameEndInfo* info);

// Live resolution (views4): periphery_scale and focus_density resize the views inside ACE's target,
// up to their launch size (views4_live). A larger focus box at the same density needs more pixels;
// beyond the launch size the density gives.
void live_resolution(const LiveSettings& l) {
    const Views4Layout& launch = experiment.views4Layout;
    const Views4Sizes wanted = views4_sizes(live.ini.c_str(), l.focusFraction, l.focusFractionY);
    const double cw = l.crop.kept_width(), ch = l.crop.kept_height();  // the outer views take the live crop
    // One view size (X-Plane): periphery and focus each sized on its own inside the launch layers. Eye columns (ACC):
    // one width for all four views, the outer and focus rows sharing the height.
    const bool columns = game->qvfr_eye_columns(), oneSize = columns && game->qvfr_one_view_size();
    game->qvfr_set_live_layout(oneSize   ? views4_live_one_size(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
                               : columns ? views4_live_eye_columns(launch, wanted.outer, wanted.focusHeight, cw, ch)
                                         : views4_live(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch));
    const Views4Fit fit = oneSize   ? views4_fit_one_size(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
                          : columns ? views4_fit_eye_columns(launch, wanted.outer, wanted.focusHeight, cw, ch)
                                    : views4_fit(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch);
    logf("live: periphery %.0f%% of native, focus density %.0f%% x %.0f%% (focus views %.0f%% x %.0f%% of the native "
         "eye; launch %.0f%%, %.0f%% x %.0f%%)%s%s",
         fit.outer * 100, fit.focus / l.focusFraction * 100, fit.focusHeight / experiment.focus_y() * 100,
         fit.focus * 100, fit.focusHeight * 100, launch.outer * 100, launch.focus * 100, launch.focusHeight * 100,
         fit.focusCapped ? " (focus capped: larger than the launch target allows)" : "",
         fit.outerCapped ? (fit.outerByRoom ? " (periphery capped: the focus views use the width)"
                                            : " (periphery capped: larger than the launch target allows)")
                         : "");
}

// Live settings: applies the ini's live keys (LiveSettings). Called between display frames.
void live_apply(const LiveSettings& l) {
    if (experiment.composing()) {  // the helmet: either compositor
        std::lock_guard guard(quad.mutex);
        if (!(helmetSettings == l.helmet))
            logf("live: helmet %d, rim %d%%, corners %d%%, light %d%%, size %d deg, height %d deg, depth %d cm, visor %ls%s",
                 l.helmet.on ? 1 : 0, l.helmet.rim, l.helmet.corners, l.helmet.light, l.helmet.size, l.helmet.height,
                 l.helmet.depth,
                 kVisorPositionNames[static_cast<int>(l.helmet.visor)], l.helmet.debugLight ? ", showing its light" : "");
        helmetSettings = l.helmet;
    }
    if (experiment.views4) {
        const FovCrop before = experiment.crop();
        experiment.set_crop(l.crop);
        if (before.top != l.crop.top || before.bottom != l.crop.bottom || before.outer != l.crop.outer ||
            before.inner != l.crop.inner)
            logf("live: FOV crop top %.0f%%, bottom %.0f%%, outer %.0f%%, inner %.0f%%%s", l.crop.top * 100,
                 l.crop.bottom * 100, l.crop.outer * 100, l.crop.inner * 100,
                 game->qvfr_dlss_held() ? " (DLSS is on: the views keep their launch sizes until the next launch)" : "");
    }
    experiment.focusFraction = l.focusFraction;
    experiment.focusFractionY = l.focusFractionY;
    experiment.feather = l.feather;
    experiment.cornerRadius = l.cornerRadius;
    experiment.focusShape = focus_shape_bits(l.shape);
    std::memcpy(experiment.borderColor, l.borderColor, sizeof(experiment.borderColor));
    experiment.border = l.border;
    experiment.gaze = l.gaze;
    experiment.fixedOffsetY = l.offset.fixed;
    experiment.gazeOffsetY = l.offset.gaze;
    experiment.gazeDeadzone = l.gazeDeadzone;
    bool gazeAvailable;
    {
        std::lock_guard guard(gaze.mutex);
        gazeAvailable = gaze.set != XR_NULL_HANDLE;
    }
    if (experiment.views4 && game_features().objectDetail)
        game->qvfr_set_object_detail(l.objectDetail);  // the adapter logs a change
    if (experiment.views4 && !experiment.views4EyeCamera)
        game->qvfr_set_live(experiment.box_x(l.focusFraction),
                        experiment.box_y(l.focusFractionY > 0 ? l.focusFractionY : l.focusFraction),
                        1.0 + l.feather + experiment.views4FocusCullMargin,
                        l.gaze && experiment.composing());
    logf("live: focus box %.0f%% x %.0f%%, feather %.0f%%, corners %.0f%%, outline %d, gaze %d, offset fixed %d deg, "
         "gaze %d deg, gaze dead-zone %.1f deg",
         l.focusFraction * 100, experiment.focus_y() * 100, l.feather * 100, l.cornerRadius * 100, l.border ? 1 : 0,
         l.gaze ? 1 : 0, l.offset.fixed, l.offset.gaze, l.gazeDeadzone);
    logf("live: focus shape %ls", focus_shape_text(l.shape).c_str());
    if (experiment.views4 && game_features().liveResolution)
        live_resolution(l);
    else if (experiment.views4)
        logf("live: %ls resizes its views at the next launch (no live resolution yet)", game->info()->shortName);
    if (l.gaze && !gazeAvailable && (experiment.quad || experiment.composing()))
        log_line("live: gaze turned on, but eye tracking was off at launch; it starts at the next launch");
    if (l.border && !experiment.borderAtLaunch && experiment.views4 && !experiment.composing())
        log_line("live: debug_border without views4_compose starts at the next launch");
}

struct IniSnapshot;
void menu_values_changed(const IniSnapshot& s);  // the settings menu shows the ini's values (below)

// Cheeky's runtime gets [cheeky]'s live settings and, in QVFR, the quad view roles (cheeky_settings.h).
void cheeky_apply_settings(const CheekySettings& settings, bool gazeOn) {
    if (!cheeky_running())
        return;
    CheekySettings c = settings;
    c.fg = c.fg && game_features().frameGeneration;  // a game without it: off, whatever the ini says
    cheeky_apply(cheeky_set_lines(c, experiment.views4, gazeOn, experiment.views4ComposeD3D11));
}
void cheeky_apply_ini() {
    if (!live.ini.empty())
        cheeky_apply_settings(read_cheeky_settings(live.ini.c_str()), read_live(live.ini.c_str()).gaze);
}
void menu_placement_changed(const MenuPlacement& placement);  // the launcher moved the settings menu's panel (below)
void osd_placement_changed(const OsdPlacement& placement, const CheekySettings& cheeky);  // the overlay (below)
void osd_placement_changed();
void hotkeys_changed(const Hotkeys& h);  // [hotkeys] (below)

constexpr int64_t kLiveCheckMs = 250;

// Everything the live reload takes from the ini, read in one go (on the ini worker).
struct IniSnapshot {
    LiveSettings live;
    CheekySettings cheeky;
    MenuValues menu;
    MenuPlacement menuPlacement;
    OsdPlacement osd;
    Hotkeys hotkeys;
    std::wstring restartKeys;
    uint64_t writesBefore = 0;  // ini_write_later's writes done before it was read (menu_values_changed)
};

IniSnapshot read_snapshot(const wchar_t* ini) {
    IniSnapshot s;
    s.live = read_live(ini);
    s.cheeky = read_cheeky_settings(ini);
    s.menu = read_menu_values(ini);
    s.menuPlacement = read_menu_placement(ini);
    s.osd = read_osd_placement(ini);
    s.hotkeys = read_hotkeys(ini);
    s.restartKeys = restart_keys(ini, experiment.views4);
    return s;
}

// The ini worker: every read and write of the ini while the game runs, off the render thread (each
// Get/WritePrivateProfileString opens the file; a menu step wrote and re-read about a hundred keys, up to
// ~130 ms in xrEndFrame). It runs the queued writes (the menu's, the hotkeys'), then, every kLiveCheckMs or
// when woken, reads a snapshot when the ini's write time changed; live_reload applies the latest.
struct IniWorker {
    std::mutex mutex;
    std::condition_variable wake, settled;
    std::deque<std::function<void()>> writes;
    std::wstring ini;     // the ini watched
    FILETIME written{};   // its write time when last read
    std::optional<IniSnapshot> ready;  // the latest read, not yet applied
    uint64_t requested = 0, done = 0;  // passes asked for (ini_worker_sync) and finished
    uint64_t writesQueued = 0, writesDone = 0;  // ini_write_later's writes, numbered from 1
    bool poke = false, started = false;
};
IniWorker iniWorker;

void ini_worker_loop() {
    std::unique_lock lock(iniWorker.mutex);
    for (;;) {
        iniWorker.wake.wait_for(lock, std::chrono::milliseconds(kLiveCheckMs),
                                [] { return iniWorker.poke || !iniWorker.writes.empty(); });
        iniWorker.poke = false;
        std::deque<std::function<void()>> writes;
        writes.swap(iniWorker.writes);
        const std::wstring ini = iniWorker.ini;
        const FILETIME seen = iniWorker.written;
        const uint64_t pass = iniWorker.requested;
        const uint64_t through = iniWorker.writesDone + writes.size();
        lock.unlock();
        for (auto& write : writes)
            write();
        // After its own writes it reads the ini whatever its write time says (a write can leave it unchanged).
        std::optional<IniSnapshot> snapshot;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const bool stamped = !ini.empty() && GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &attributes);
        const bool changed = stamped && (!writes.empty() || CompareFileTime(&attributes.ftLastWriteTime, &seen) != 0);
        if (changed) {
            snapshot = read_snapshot(ini.c_str());
            snapshot->writesBefore = through;
        }
        lock.lock();
        iniWorker.writesDone = through;
        if (snapshot && ini == iniWorker.ini) {
            iniWorker.ready = std::move(snapshot);
            iniWorker.written = attributes.ftLastWriteTime;
        }
        iniWorker.done = std::max(iniWorker.done, pass);
        iniWorker.settled.notify_all();
    }
}

// Starts the worker (once) and points it at `ini`, read from `written` on (a new path drops what it read).
// Under iniWorker.mutex.
void ini_worker_watch(const std::wstring& ini, const FILETIME& written) {
    if (!iniWorker.started) {
        iniWorker.started = true;
        std::thread(ini_worker_loop).detach();  // ends with the process (ExitProcess stops it before teardown)
    }
    if (ini != iniWorker.ini) {
        iniWorker.ini = ini;
        iniWorker.written = written;
        iniWorker.ready.reset();
    }
}

// Queues a write to the ini (or the launcher's) and wakes the worker; the live reload follows it. Returns
// the write's number: snapshots read after it carry writesBefore >= it.
uint64_t ini_write_later(std::function<void()> write) {
    std::lock_guard guard(iniWorker.mutex);
    ini_worker_watch(live.ini, live.written);
    iniWorker.writes.push_back(std::move(write));
    iniWorker.wake.notify_one();
    return ++iniWorker.writesQueued;
}

// Waits until the writes queued so far are done and the ini has been read after them (tests; the
// session's end, so no change is lost).
void ini_worker_sync() {
    std::unique_lock lock(iniWorker.mutex);
    if (!iniWorker.started)
        return;
    const uint64_t want = ++iniWorker.requested;
    iniWorker.poke = true;
    iniWorker.wake.notify_one();
    iniWorker.settled.wait(lock, [want] { return iniWorker.done >= want; });
}

// Applies the ini's live keys when the worker read a change. Only between display frames: with
// two_loops, never between frame A and frame B. No file access here.
void live_reload() {
    if (live.ini.empty())
        return;
    {
        std::lock_guard guard(iniWorker.mutex);
        ini_worker_watch(live.ini, live.written);
    }
    {
        std::lock_guard guard(loops.mutex);
        if (loops.active && loops.pairedEnds % 2 != 0)
            return;  // A ended, B not yet: apply after B
    }
    std::optional<IniSnapshot> s;
    {
        std::lock_guard guard(iniWorker.mutex);
        s.swap(iniWorker.ready);
    }
    if (!s)
        return;
    live_apply(s->live);
    cheeky_apply_settings(s->cheeky, s->live.gaze);
    menu_values_changed(*s);
    menu_placement_changed(s->menuPlacement);
    osd_placement_changed(s->osd, s->cheeky);
    hotkeys_changed(s->hotkeys);
    if (s->restartKeys != live.restartKeys) {
        logf("live: %ls-> %ls(applies at the next launch)", live.restartKeys.c_str(), s->restartKeys.c_str());
        live.restartKeys = s->restartKeys;
    }
}

// The settings menu's QVFR optimizer (quad/qvfr_optimizer.h; run below, after the overlay): the frames
// the overlay's measurement gives it while it runs (osd_measure).
struct Optimize {
    QvfrOptimizer optimizer;
    std::vector<OsdFrame> frames;  // since the last window
    int64_t nextWindow = 0;
    bool shown = false;  // the menu shows its detail (from the first run until the menu reopens)
    std::wstring note;   // why it did not start, shown instead of its own status
};
Optimize optimize;
constexpr int64_t kOptimizeWindowMs = 500;

// ---- Hotkeys ---------------------------------------------------------------------------------
// [hotkeys] (input/hotkeys.h), reloaded live: each action's keyboard combination, held while ACE has focus,
// and its wheel or controller button, read by a thread of its own through DirectInput (input/pad_input.h)
// whenever one is bound. The thread polls every kPadPollMs and publishes the actions held as bits.
Hotkeys hotkeys = default_hotkeys();  // the frame thread's

struct PadWatch {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false;
    Hotkeys bindings;              // a copy for the thread
    std::atomic<uint32_t> held{0};  // bit per HotkeyAction
    // Left running at exit (no xrDestroySession), the thread would end the process in std::terminate.
    ~PadWatch() {
        {
            std::lock_guard guard(mutex);
            stop = true;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
    }
};
PadWatch padWatch;
constexpr int kPadPollMs = 8, kPadRescanMs = 3000;

bool pad_bound(const Hotkeys& h) {
    for (const Hotkey& k : h)
        if (k.pad.set())
            return true;
    return false;
}

void pad_watch_run() {
    // DirectInput wants a top-level window of this process; a hidden one of the thread's own.
    HWND window = CreateWindowExW(0, L"STATIC", L"Sim-QVFR input", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    PadInput pads;
    if (!window || !pads.open(window)) {
        log_line("hotkeys: DirectInput not available; wheel and controller buttons do nothing");
        if (window)
            DestroyWindow(window);
        return;
    }
    {
        std::wstring found;
        for (const auto& [id, name] : pads.devices())
            found += (found.empty() ? L"" : L", ") + name + L" (" + id + L")";
        logf("hotkeys: game controllers: %ls", found.empty() ? L"none" : found.c_str());
    }
    int64_t nextRescan = qpc_now() + qpc_ms(kPadRescanMs);
    std::unique_lock lock(padWatch.mutex);
    while (!padWatch.stop) {
        const Hotkeys bindings = padWatch.bindings;
        lock.unlock();
        const std::vector<PadHeld> held = pads.poll();
        const auto devices = pads.devices();
        uint32_t bits = 0;
        bool missing = false;
        for (int a = 0; a < kHotkeyActions; ++a) {
            const PadControl& want = bindings[a].pad;
            if (!want.set())
                continue;
            bool connected = false;
            for (const PadHeld& h : held)
                if (_wcsicmp(h.device.c_str(), want.device.c_str()) == 0 && h.control == want.control)
                    bits |= 1u << a;
            for (const auto& [id, name] : devices)
                connected |= _wcsicmp(id.c_str(), want.device.c_str()) == 0;
            missing |= !connected;
        }
        padWatch.held.store(bits, std::memory_order_relaxed);
        if (missing && qpc_now() >= nextRescan) {  // a bound device plugged in since
            pads.rescan();
            nextRescan = qpc_now() + qpc_ms(kPadRescanMs);
        }
        lock.lock();
        padWatch.wake.wait_for(lock, std::chrono::milliseconds(kPadPollMs), [] { return padWatch.stop; });
    }
    lock.unlock();
    pads.close();
    DestroyWindow(window);
}

// Starts the thread once a wheel or controller button is bound (from the frame thread, each frame).
void pad_watch_ensure() {
    if (!pad_bound(hotkeys))
        return;
    std::lock_guard guard(padWatch.mutex);
    if (!padWatch.thread.joinable()) {
        padWatch.stop = false;
        padWatch.bindings = hotkeys;
        padWatch.thread = std::thread(pad_watch_run);
    }
}

void pad_watch_stop() {
    std::thread thread;
    {
        std::lock_guard guard(padWatch.mutex);
        padWatch.stop = true;
        thread.swap(padWatch.thread);
    }
    padWatch.wake.notify_all();
    if (thread.joinable())
        thread.join();
    padWatch.held.store(0);
}

void hotkeys_changed(const Hotkeys& h) {
    if (h == hotkeys)
        return;
    hotkeys = h;
    {
        std::lock_guard guard(padWatch.mutex);
        padWatch.bindings = h;
    }
    std::wstring line;
    for (int a = 0; a < kHotkeyActions; ++a) {
        const std::wstring key = key_text(h[a].key), pad = control_text(h[a].pad.control);
        line += std::wstring(a ? L", " : L"") + hotkey_ini_key(static_cast<HotkeyAction>(a)) + L"=" + (key.empty() ? L"-" : key) +
                (pad.empty() ? L"" : L" / " + pad);
    }
    logf("hotkeys: %ls", line.c_str());
}

bool game_focused();

// Exactly `key`'s modifiers and its key held, while ACE has focus.
bool key_held(const KeyCombo& key) {
    if (!key.set() || !game_focused())
        return false;
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return down(key.vk) && down(VK_CONTROL) == key.ctrl && down(VK_MENU) == key.alt && down(VK_SHIFT) == key.shift;
}

bool pad_held(HotkeyAction action) {
    return (padWatch.held.load(std::memory_order_relaxed) >> static_cast<int>(action)) & 1;
}

bool hotkey_held(HotkeyAction action) { return key_held(hotkeys[static_cast<int>(action)].key) || pad_held(action); }

// The settings menu's hint: how to select and change, and how to close.
std::wstring menu_hint() {
    const Hotkeys defaults = default_hotkeys();
    bool defaultKeys = true, padKeys = false;
    for (int a = static_cast<int>(HotkeyAction::MenuUp); a <= static_cast<int>(HotkeyAction::MenuRight); ++a) {
        defaultKeys &= hotkeys[a].key == defaults[a].key;
        padKeys |= hotkeys[a].pad.set();
    }
    const Hotkey& toggle = hotkeys[static_cast<int>(HotkeyAction::MenuToggle)];
    std::wstring hint = defaultKeys ? L"Ctrl+Alt+arrows" : L"Your menu hotkeys";
    hint += padKeys ? L", wheel buttons or right stick: select and change" : L", or right stick: select and change";
    hint += toggle.key.set() || toggle.pad.set() ? L" · open/close hotkey or left stick click: close" : L" · left stick click: close";
    return hint;
}

// The menu's hotkeys footer ([menu] hotkey_footer): the in-game actions' keys and wheel buttons, those that apply
// (the visor with the helmet on, the CPU capture with cpu_profile, the frame capture with d3d11_capture_hotkey); empty
// when none is bound.
std::wstring hotkey_footer(bool helmetOn) {
    struct Entry {
        HotkeyAction action;
        const wchar_t* name;
        bool shown;
    };
    const Entry entries[] = {{HotkeyAction::Overlay, L"Overlay", true},
                             {HotkeyAction::Visor, L"Visor", helmetOn},
                             {HotkeyAction::CpuCapture, L"CPU capture", experiment.cpuProfile},
                             {HotkeyAction::FrameCapture, L"Frame capture", experiment.d3d11CaptureHotkey}};
    std::wstring out;
    for (const Entry& e : entries) {
        const Hotkey& h = hotkeys[static_cast<int>(e.action)];
        if (!e.shown || (!h.key.set() && !h.pad.set()))
            continue;
        std::wstring binding = key_text(h.key);
        if (h.pad.set())
            binding += (binding.empty() ? L"" : L" / ") + std::wstring(L"wheel ") + control_text(h.pad.control);
        out += (out.empty() ? L"" : L"  \u00b7  ") + std::wstring(e.name) + L" " + binding;
    }
    return out;
}

// ---- Settings menu ---------------------------------------------------------------------------
// In the headset (settings_menu.h): its hotkeys (above; Ctrl+Alt+arrows by default), or a controller's left
// stick click (open, close) and right stick (select, change). Changes are written to the ini the
// layer reloads from and to the launcher's (SIM_QVFR_SETTINGS), so they apply as the launcher's do.
// Shown as a head-locked quad layer on top of whatever this layer submits, where the launcher's
// Headset menu pane places it ([menu], live); hidden kMenuHideMs after the last input.
struct Menu {
    SettingsMenu model;
    bool visible = false, dirty = true, failed = false, uploaded = false;
    int64_t hideAt = 0;  // QPC
    std::wstring settingsIni;
    XrSpace view = XR_NULL_HANDLE;
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<void*> images;  // the session API's textures
    std::unique_ptr<PanelUpload> upload;
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    int heldKey = -1;  // keyboard repeat: the held arrow (index into kMenuKeys)
    int64_t keyRepeat = 0;
    int stickDir = -1;  // controller repeat: the held stick direction
    int64_t stickRepeat = 0;
    bool togglePrevious = false;
    bool hotkeyTogglePrevious = false;  // the open/close hotkey, held at the last frame
    bool profileLogged = false;
    uint64_t lastWrite = 0;  // ini_write_later's number for the latest step
};
Menu menu;
constexpr int64_t kMenuHideMs = 6000, kRepeatDelayMs = 400, kRepeatMs = 90;
constexpr MenuKey kMenuKeyOf[] = {MenuKey::Up, MenuKey::Down, MenuKey::Left, MenuKey::Right};

int64_t qpc_ms(int64_t ms) {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency * ms / 1000;
}

bool game_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

double native_center_ppd();  // below

void menu_place(const MenuPlacement& placement) {
    const MenuPose pose = menu_pose(placement, static_cast<float>(kPanelHeight) / kPanelWidth);
    menu.layer.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    menu.layer.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    menu.layer.size = {pose.width, pose.height};
}

// The launcher changed the ini: an open menu shows the new values.
// A snapshot read before the latest step was written would put the menu back a step (and the next step's
// write would carry the old value: two quick steps lost the second), so the menu waits for a newer one.
void menu_values_changed(const IniSnapshot& s) {
    if (!menu.visible || s.writesBefore < menu.lastWrite)
        return;
    menu.model.values = s.menu;
    menu.model.cheeky.values = s.cheeky;
    menu.dirty = true;
}

void menu_open(int64_t now);

// Moved from the launcher (its Headset menu pane): the panel goes there and shows, so you can see it.
void menu_placement_changed(const MenuPlacement& placement) {
    if (placement == live.menuPlacement)
        return;
    live.menuPlacement = placement;
    menu_place(placement);
    logf("settings menu: moved to %d deg right, %d deg up, %d deg wide", placement.yaw, placement.pitch, placement.width);
    if (experiment.views4 || experiment.quad)
        menu_open(qpc_now());
}

// Profiles (profile/profiles.h) in the launcher's folder (SIM_QVFR_SETTINGS), else next to the ini the layer reads.
std::wstring menu_profiles_ini() { return menu.settingsIni.empty() ? live.ini : menu.settingsIni; }

void menu_profiles_read() {
    const std::wstring ini = menu_profiles_ini();
    menu.model.profiles = profile_names(profiles_folder(ini));
    const std::wstring current = current_profile(ini.c_str());
    menu.model.profile = -1;
    for (size_t i = 0; i < menu.model.profiles.size(); ++i)
        if (_wcsicmp(menu.model.profiles[i].c_str(), current.c_str()) == 0)
            menu.model.profile = static_cast<int>(i);
    Profile p;
    menu.model.profileChanged = menu.model.profile >= 0 && load_profile(profiles_folder(ini), current, p, menu.model.values.eyeTracking) &&
                                !profile_matches(live.ini.c_str(), p);
}

// The profile the Profile line stepped to: its keys written to the ini the layer reloads and the launcher's, as
// the menu's own steps are; the menu shows its values once the live reload has them.
void menu_profile_use() {
    const int request = std::exchange(menu.model.profileRequest, -1);
    if (request < 0 || request >= static_cast<int>(menu.model.profiles.size()))
        return;
    const std::wstring ini = menu_profiles_ini();
    Profile p;
    if (!load_profile(profiles_folder(ini), menu.model.profiles[request], p, menu.model.values.eyeTracking)) {
        logf("settings menu: profile %ls not found", menu.model.profiles[request].c_str());
        return;
    }
    if (optimize.optimizer.running()) {
        optimize.optimizer.cancel();
        log_line("optimize: stopped (a profile was used)");
    }
    menu.lastWrite = ini_write_later([live = live.ini, settings = menu.settingsIni, p] {
        apply_profile(live.c_str(), p);
        set_current_profile(live.c_str(), p.name);
        if (!settings.empty()) {
            apply_profile(settings.c_str(), p);
            set_current_profile(settings.c_str(), p.name);
        }
    });
    logf("settings menu: profile %ls (%zu keys)", p.name.c_str(), p.keys.size());
}

void menu_open(int64_t now) {
    if (menu.settingsIni.empty()) {  // the launcher's own ini, where the next launch reads them
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            menu.settingsIni = path;
    }
    if (!menu.visible) {
        bool stepPending;  // reopened before its last step reached the ini: keep the values it shows
        {
            std::lock_guard guard(iniWorker.mutex);
            stepPending = iniWorker.writesDone < menu.lastWrite;
        }
        if (!stepPending)
            menu.model.values = read_menu_values(live.ini.c_str());
        menu.model.cheeky = {cheeky_running(), cheeky_version(),
                             stepPending ? menu.model.cheeky.values : read_cheeky_settings(live.ini.c_str())};
        const Views4Layout& l = experiment.views4Layout;
        {
            std::lock_guard guard(quad.mutex);  // the native eye, for the notes' pixel sizes
            menu.model.launch = {experiment.views4, l.outer * 100, l.focus * 100, l.focusHeight * 100,
                                 static_cast<int>(quad.nativeW), static_cast<int>(quad.nativeH), 0,
                                 experiment.eyeScale * 100};
        }
        menu.model.launch.cropWidth = l.cropWidth * 100;  // the launch crop
        menu.model.launch.cropHeight = l.cropHeight * 100;
        const games::GameFeatures features = game_features();  // what the game has: the rest is left out
        menu.model.launch.cropPage = features.fovCrop;
        menu.model.launch.helmetPage = features.helmet;
        menu.model.launch.fgPage = features.frameGeneration;
        menu.model.launch.objectDetail = features.objectDetail;  // X-Plane only
        menu.model.launch.liveResolution = features.liveResolution;
        {
            std::lock_guard guard(quad.mutex);
            menu.model.launch.helmetPieces = quad.helmetPiecesLoaded;
        }
        menu.model.launch.nativePpd = native_center_ppd();  // takes quad.mutex itself
        if (!optimize.optimizer.running()) {  // a finished run's detail is not kept over a reopen
            optimize.shown = false;
            optimize.note.clear();
        }
        if (!stepPending)  // the profiles, and the one in use unless a step of the menu's is on its way
            menu_profiles_read();
        menu.visible = menu.dirty = true;
        log_line("settings menu: open");
    }
    menu.hideAt = now + qpc_ms(kMenuHideMs);
}

void menu_press(MenuKey key, int64_t now) {
    if (!menu.visible) {  // the first press only opens it
        menu_open(now);
        return;
    }
    menu.hideAt = now + qpc_ms(kMenuHideMs);
    menu.dirty = true;
    if (!menu_key(menu.model, key)) {
        menu_profile_use();
        return;
    }
    menu.model.profileChanged = menu.model.profile >= 0;
    if (optimize.optimizer.running()) {  // a value changed by hand: the optimizer's measurements no longer hold
        optimize.optimizer.cancel();
        log_line("optimize: stopped (a setting was changed)");
    }
    {  // what was set, to check against what applies ("live:", "cheeky: settings applied")
        const std::vector<MenuLine> lines = menu_lines(menu.model);
        const int i = menu.model.selected;
        if (i >= 0 && i < static_cast<int>(lines.size()))
            logf("settings menu: %ls: %ls -> %ls", menu_title(menu.model).c_str(), lines[i].label.c_str(),
                 lines[i].value.c_str());
    }
    // Written on the ini worker (off the render thread), to the ini the layer reloads and the launcher's
    // (the next launch starts from them); the live reload follows within a frame or two.
    menu.lastWrite = ini_write_later([ini = live.ini, settings = menu.settingsIni, values = menu.model.values,
                     cheeky = menu.model.cheeky, qvfr = menu.model.launch.qvfr, detail = menu.model.launch.objectDetail] {
        write_menu_values(ini.c_str(), values, qvfr, detail);
        if (cheeky.available)
            write_cheeky_settings(ini.c_str(), cheeky.values, false);
        if (!settings.empty()) {
            write_menu_values(settings.c_str(), values, qvfr, detail);
            if (cheeky.available)
                write_cheeky_settings(settings.c_str(), cheeky.values, false);
        }
    });
}

// Held keys and stick directions repeat after kRepeatDelayMs, every kRepeatMs.
bool repeat(int& held, int64_t& nextAt, int direction, int64_t now) {
    if (direction < 0) {
        held = -1;
        return false;
    }
    if (held != direction) {
        held = direction;
        nextAt = now + qpc_ms(kRepeatDelayMs);
        return true;
    }
    if (now < nextAt)
        return false;
    nextAt = now + qpc_ms(kRepeatMs);
    return true;
}

// The menu's hotkeys: the open/close one, and the four directions. A key's first press opens the menu; a wheel's
// directions act only while it shows (the game may use the same switch).
void menu_keyboard(int64_t now) {
    pad_watch_ensure();
    const bool toggle = hotkey_held(HotkeyAction::MenuToggle);
    if (toggle && !menu.hotkeyTogglePrevious) {
        if (menu.visible) {
            menu.visible = false;
            log_line("settings menu: closed");
        } else {
            menu_open(now);
        }
    }
    menu.hotkeyTogglePrevious = toggle;
    int down = -1;
    for (int i = 0; i < 4 && down < 0; ++i) {
        const HotkeyAction a = static_cast<HotkeyAction>(static_cast<int>(HotkeyAction::MenuUp) + i);
        if (key_held(hotkeys[static_cast<int>(a)].key) || (menu.visible && pad_held(a)))
            down = i;
    }
    if (repeat(menu.heldKey, menu.keyRepeat, down, now))
        menu_press(kMenuKeyOf[down], now);
}

void menu_controllers(XrSession session, int64_t now) {
    bool toggle = false;
    XrVector2f stick{};
    {
        std::lock_guard guard(gaze.mutex);
        if (!gaze.attached || !gaze.menuToggle)
            return;
        if (gaze.appSyncs == 0) {  // ACE never syncs actions: sync the layer's set alone
            const XrActiveActionSet active{gaze.set, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            next.SyncActions(session, &sync);
        }
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = gaze.menuToggle;
        XrActionStateBoolean pressed{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(next.GetActionStateBoolean(session, &get, &pressed)) && pressed.isActive)
            toggle = pressed.currentState;
        get.action = gaze.menuNavigate;
        XrActionStateVector2f axes{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(next.GetActionStateVector2f(session, &get, &axes)) && axes.isActive)
            stick = axes.currentState;
        if (!menu.profileLogged && (toggle || stick.x != 0 || stick.y != 0)) {
            menu.profileLogged = true;
            XrPath hand = XR_NULL_PATH;
            XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
            const bool known = XR_SUCCEEDED(next.StringToPath(gaze.instance, "/user/hand/right", &hand)) &&
                               XR_SUCCEEDED(next.GetCurrentInteractionProfile(session, hand, &state));
            logf("settings menu: controller input from %s",
                 known ? path_name(gaze.instance, state.interactionProfile).c_str() : "?");
        }
    }
    if (toggle && !menu.togglePrevious) {
        if (menu.visible) {
            menu.visible = false;
            log_line("settings menu: closed");
        } else {
            menu_open(now);
        }
    }
    menu.togglePrevious = toggle;
    // A clear push in one direction; back below kRelease ends it.
    constexpr float kPush = 0.6f, kRelease = 0.3f;
    int dir = menu.stickDir;
    const float ax = std::fabs(stick.x), ay = std::fabs(stick.y);
    if (std::max(ax, ay) < kRelease)
        dir = -1;
    else if (ay >= kPush && ay > ax)
        dir = stick.y > 0 ? 0 : 1;
    else if (ax >= kPush && ax > ay)
        dir = stick.x < 0 ? 2 : 3;
    if (menu.visible && repeat(menu.stickDir, menu.stickRepeat, dir, now))
        menu_press(kMenuKeyOf[dir], now);
    else if (!menu.visible)
        menu.stickDir = dir;  // a push held while opening does not step
}

bool menu_resources(XrSession session) {
    if (!game->has_qvfr())  // log only: nothing of ours in the headset
        return false;
    if (menu.chain)
        return true;
    if (menu.failed)
        return false;
    const SessionGraphics graphics = session_graphics_now();
    if (!graphics.valid())
        return false;  // no D3D11 or D3D12 session (yet)
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    chainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    chainInfo.sampleCount = 1;
    chainInfo.width = kPanelWidth;
    chainInfo.height = kPanelHeight;
    chainInfo.faceCount = 1;
    chainInfo.arraySize = 1;
    chainInfo.mipCount = 1;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &menu.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &menu.chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, menu.chain, menu.images);
    if (XR_SUCCEEDED(r) && !menu.images.empty())
        menu.upload = make_panel_upload(graphics);
    if (XR_FAILED(r) || menu.images.empty() || !menu.upload) {
        logf("settings menu: panel setup failed (%d, %s); the menu still changes settings", r, graphics.name());
        menu.failed = true;
        return false;
    }
    XrCompositionLayerQuad& l = menu.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    l.space = menu.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = menu.chain;
    l.subImage.imageRect = {{0, 0}, {kPanelWidth, kPanelHeight}};
    const MenuPlacement& placement = live.menuPlacement;
    menu_place(placement);
    logf("settings menu: panel ready, %d deg right, %d deg up, %d deg wide", placement.yaw, placement.pitch, placement.width);
    return true;
}

// Pixels per degree at the centre of the view at the runtime's recommended size (100% density), over
// the FOV ACE renders with: views4 reads it back from ACE's projection (the focus density is against
// it), Two-loop uses frame A's. 0 until known.
double native_center_ppd() {
    float tan[4] = {};
    Views4Frame frame{};
    if (experiment.views4 && game->qvfr_latest_frame(frame))
        std::memcpy(tan, frame.outerTan[0], sizeof(tan));
    std::lock_guard guard(quad.mutex);
    if (!(tan[1] > tan[0])) {
        if (experiment.views4 && quad.v4FovValid)
            fov_tangents(quad.v4Fov[0], tan);
        else if (quad.aViewsValid)
            fov_tangents(quad.aViews[0].fov, tan);
    }
    // The native size spans the uncropped FOV (the crop keeps the density).
    const double width = (static_cast<double>(tan[1]) - tan[0]) / experiment.crop().kept_width();
    return quad.nativeW && width > 0.02 ? quad.nativeW / width * 3.14159265358979 / 180 : 0;
}

void menu_render() {
    const std::wstring title = menu_title(menu.model);
    const std::wstring hint = menu_hint();
    if (menu.model.launch.nativePpd <= 0)  // known once ACE has rendered a frame
        menu.model.launch.nativePpd = native_center_ppd();
    menu.model.launch.dlssHeld = experiment.views4 && game->qvfr_dlss_held();
    const std::vector<uint32_t> pixels = draw_menu_panel(title, menu_resolution(menu.model), menu_stats(menu.model),
                                                         menu_lines(menu.model),
                                                         menu_notes(menu.model), hint, menu_subtitle(menu.model),
                                                         menu.model.values.hotkeyFooter
                                                             ? hotkey_footer(menu.model.values.helmet.on && experiment.composing())
                                                             : L"");
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100'000'000;
    if (XR_FAILED(next.AcquireSwapchainImage(menu.chain, nullptr, &index)))
        return;
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(menu.chain, &wait)) && index < menu.images.size() &&
                    menu.upload && menu.upload->upload(menu.images[index], pixels);
    next.ReleaseSwapchainImage(menu.chain, nullptr);
    if (ok) {
        menu.uploaded = true;
        menu.dirty = false;
    }
}

const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now);  // the overlay's card (below)
void optimize_tick(int64_t now);  // the QVFR optimizer (below)
bool notice_pending();            // a notice for the start of the session (below)

// This frame's layers with the performance overlay and the menu panel on top while they are shown;
// `info` itself otherwise.
const XrFrameEndInfo* menu_frame(XrSession session, const XrFrameEndInfo* info, XrFrameEndInfo& storage,
                                 std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    // The performance overlay also while measuring without QVFR (baseline=1), to compare the two; the settings
    // menu only with a method of the layer's (its settings would change nothing in mode=off).
    const bool method = experiment.views4 || experiment.quad;
    if (live.ini.empty() || (!method && !experiment.measure && !notice_pending()))
        return info;
    // For the launcher's Center PPD: once, when ACE has rendered.
    static bool ppdReported = false;
    if (method && !ppdReported && endFrames % 90 == 0) {
        if (const double ppd = native_center_ppd(); ppd > 0) {
            wchar_t text[32];
            swprintf_s(text, L"%.2f", ppd);
            runtime_info(L"native_center_ppd", text);
            logf("settings menu: %.2f px per degree at the centre at 100%% density", ppd);
            ppdReported = true;
        }
    }
    const int64_t now = qpc_now();
    if (method) {
        menu_keyboard(now);
        menu_controllers(session, now);
        optimize_tick(now);
    }
    if (menu.visible && now >= menu.hideAt) {
        menu.visible = false;
        log_line("settings menu: hidden");
    }
    bool showMenu = menu.visible && menu_resources(session);
    if (showMenu && menu.dirty)
        menu_render();
    showMenu = showMenu && menu.uploaded;
    const XrCompositionLayerBaseHeader* card = osd_frame(session, now);
    if (!showMenu && !card)
        return info;
    layers.assign(info->layers, info->layers + info->layerCount);
    if (card)
        layers.push_back(card);
    if (showMenu)  // the menu over the card
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu.layer));
    storage = *info;
    storage.layerCount = static_cast<uint32_t>(layers.size());
    storage.layers = layers.data();
    return &storage;
}

void menu_shutdown() {
    menu.upload.reset();
    if (menu.chain)
        next.DestroySwapchain(menu.chain);
    if (menu.view)
        next.DestroySpace(menu.view);
    menu.chain = XR_NULL_HANDLE;
    menu.view = XR_NULL_HANDLE;
    menu.images.clear();
    menu.visible = menu.uploaded = menu.failed = false;
    menu.dirty = true;
}

// ---- Performance overlay ---------------------------------------------------------------------
// [osd] (quad/osd.h): a head-locked card where the launcher's Headset menu pane or the menu places it, redrawn every kOsdDrawMs from the
// frames since the last draw. The numbers are gathered on the frame thread; the card is drawn on a
// worker (GDI takes milliseconds) and uploaded at a later frame. Ctrl+Alt+P while ACE has focus turns it on and off, written to the ini
// the layer reloads from and to the launcher's, as the launcher's Headset menu pane does.
// ACE's frame: xrEndFrame to xrEndFrame; its CPU time: the cycles the thread calling xrEndFrame ran
// between the two (ACE's render thread; time it spends blocked is not counted); its GPU time: the
// views4 GPU frame timer (probe/gpu_frame_timer.h), started when the card first shows.
struct Osd {
    OsdPlacement placement;  // [osd] when last read
    bool read = false, failed = false, uploaded = false, hotkeyHeld = false;
    XrSpace view = XR_NULL_HANDLE;
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<void*> images;  // the session API's textures
    std::unique_ptr<PanelUpload> upload;
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::atomic<int64_t> periodNs{0};  // the display's, from xrWaitFrame (any thread)
    // On the thread calling xrEndFrame.
    int64_t lastEnd = 0;
    DWORD thread = 0;
    uint64_t lastCycles = 0;
    int64_t tscStartQpc = 0;
    uint64_t tscStart = 0;
    double cyclesPerMs = 0;  // the TSC's rate, measured against QPC
    std::vector<OsdFrame> frames;  // since the last draw
    std::deque<double> history;    // the graph: the last kOsdHistory frame times
    int64_t nextDraw = 0;
    uint64_t gazeQueries = 0, gazeTracked = 0;  // at the last draw
    std::future<std::vector<uint32_t>> drawing;  // the card being drawn
    CheekySettings cheeky;                         // [cheeky] when last read
    std::string cheekySnapshot;
    int64_t nextCheeky = 0;
    IDXGIAdapter3* adapter = nullptr;
    // xrWaitFrame's last return (the render thread's work for the frame runs from it to xrEndFrame).
    int64_t waitReturnQpc = 0;
    uint64_t waitReturnCycles = 0;
    DWORD waitThread = 0;
    double waitMs = -1;
    OsdSummary shown;  // the card's numbers at its last draw (for the log)
    bool shownValid = false;
};
Osd osd;
constexpr int64_t kOsdDrawMs = 500, kOsdCheekyMs = 1000;

// A notice in the performance overlay's card, for the first kNoticeMs of the session's frames, when the layer
// could not do what it was asked: views4 refused, an ACE build it does not know (the game updated). Without
// it the game would just look as it always does, and nothing would say why. Straight ahead, a little low.
struct Notice {
    std::wstring title;
    std::vector<std::wstring> lines;
    int64_t until = 0;  // set at its first frame
    bool uploaded = false;
};
Notice notice;
constexpr int64_t kNoticeMs = 30000;
constexpr OsdPlacement kNoticePlacement{true, 0, -6, 34};

void notice_set(std::wstring title, std::vector<std::wstring> lines) {
    notice.title = std::move(title);
    notice.lines = std::move(lines);
}

bool notice_pending() { return !notice.title.empty(); }
constexpr size_t kOsdHistory = 150;


void osd_waited(XrDuration period) { osd.periodNs.store(period, std::memory_order_relaxed); }

bool frame_generation_on() { return cheeky_running() && osd.cheeky.processing && osd.cheeky.fg; }

bool osd_shown(OsdSummary& out) {
    if (!osd.placement.enabled || !osd.shownValid)
        return false;
    out = osd.shown;
    return true;
}

void osd_wait_returned(double waitMs) {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    osd.waitReturnQpc = qpc_now();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    osd.waitReturnCycles = cycles;
    osd.waitThread = GetCurrentThreadId();
    osd.waitMs = waitMs;
}

void osd_measure() {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    const int64_t now = qpc_now();
    const DWORD thread = GetCurrentThreadId();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    if (!osd.tscStartQpc) {
        osd.tscStartQpc = now;
        osd.tscStart = __rdtsc();
    } else if (osd.cyclesPerMs == 0 && now - osd.tscStartQpc > qpc_ms(500)) {
        osd.cyclesPerMs = static_cast<double>(__rdtsc() - osd.tscStart) /
                          (static_cast<double>(now - osd.tscStartQpc) * 1000.0 / static_cast<double>(qpc_ms(1000)));
    }
    if (osd.lastEnd) {
        OsdFrame frame;
        frame.intervalMs = static_cast<double>(now - osd.lastEnd) * 1000.0 / static_cast<double>(qpc_ms(1000));
        if (thread == osd.waitThread && osd.waitReturnQpc > osd.lastEnd && osd.cyclesPerMs > 0) {
            // The render thread's work for the frame: from xrWaitFrame's return, its wall time and what of it ran.
            frame.workMs = static_cast<double>(now - osd.waitReturnQpc) * 1000.0 / static_cast<double>(qpc_ms(1000));
            frame.cpuMs = static_cast<double>(cycles - osd.waitReturnCycles) / osd.cyclesPerMs;
            frame.waitMs = osd.waitMs;
        } else if (thread == osd.thread && osd.cyclesPerMs > 0) {
            frame.cpuMs = static_cast<double>(cycles - osd.lastCycles) / osd.cyclesPerMs;
        }
        if (osd.placement.enabled) {
            osd.frames.push_back(frame);
            osd.history.push_back(frame.intervalMs);
            if (osd.history.size() > kOsdHistory)
                osd.history.pop_front();
        }
        if (optimize.optimizer.running())
            optimize.frames.push_back(frame);
    }
    osd.lastEnd = now;
    osd.thread = thread;
    osd.lastCycles = cycles;
}

void osd_place() {
    const OsdPlacement& p = notice_pending() ? kNoticePlacement : osd.placement;
    const MenuPose pose = menu_pose({p.yaw, p.pitch, p.width}, static_cast<float>(kOsdHeight) / kOsdWidth);
    osd.layer.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    osd.layer.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    osd.layer.size = {pose.width, pose.height};
}

void osd_placement_changed() {
    osd_placement_changed(read_osd_placement(live.ini.c_str()), read_cheeky_settings(live.ini.c_str()));
}

void osd_placement_changed(const OsdPlacement& placement, const CheekySettings& cheeky) {
    osd.cheeky = cheeky;  // frame generation on or off, its multiplier
    if (osd.read && placement == osd.placement)
        return;
    if (osd.read || placement.enabled)
        logf("performance overlay: %s, %d deg right, %d deg up, %d deg wide", placement.enabled ? "on" : "off",
             placement.yaw, placement.pitch, placement.width);
    if (placement.enabled && !osd.placement.enabled) {  // a fresh window
        osd.frames.clear();
        osd.history.clear();
        osd.lastEnd = 0;
        osd.nextDraw = 0;
    }
    osd.placement = placement;
    osd.read = true;
    osd_place();
}

// The visor hotkey (Ctrl+Alt+V by default) steps the helmet's visor (closed, half, open, closed), written to the
// ini the layer reloads from and to the launcher's, as the headset menu writes it.
bool visorHotkeyHeld = false;
void visor_hotkey() {
    const bool held = hotkey_held(HotkeyAction::Visor);
    if (held && !visorHotkeyHeld && experiment.composing()) {
        HelmetSettings h;
        {
            std::lock_guard guard(quad.mutex);  // at once; the ini on the worker
            helmetSettings.visor = static_cast<VisorPosition>((static_cast<int>(helmetSettings.visor) + 1) % 3);
            h = helmetSettings;
        }
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        const std::wstring settings = n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
        ini_write_later([ini = live.ini, settings, h] {
            write_helmet_settings(ini.c_str(), h);
            if (!settings.empty())
                write_helmet_settings(settings.c_str(), h);
        });
        logf("helmet: visor %ls (hotkey)", kVisorPositionNames[static_cast<int>(h.visor)]);
    }
    visorHotkeyHeld = held;
}

void osd_hotkey() {
    pad_watch_ensure();  // also without a method of the layer's (baseline=1)
    const bool held = hotkey_held(HotkeyAction::Overlay);
    if (held && !osd.hotkeyHeld) {
        OsdPlacement placement = osd.placement;
        placement.enabled = !placement.enabled;
        wchar_t path[MAX_PATH] = {};  // the launcher's own ini: the next launch starts with it
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        const std::wstring settings = n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
        ini_write_later([ini = live.ini, settings, placement] {
            write_osd_placement(ini.c_str(), placement);
            if (!settings.empty())
                write_osd_placement(settings.c_str(), placement);
        });
        osd_placement_changed(placement, osd.cheeky);  // at once; the ini on the worker
    }
    osd.hotkeyHeld = held;
}

bool osd_resources(XrSession session) {
    if (!game->has_qvfr())  // log only: nothing of ours in the headset
        return false;
    if (osd.chain)
        return true;
    if (osd.failed)
        return false;
    const SessionGraphics graphics = session_graphics_now();
    if (!graphics.valid())
        return false;
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    chainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    chainInfo.sampleCount = 1;
    chainInfo.width = kOsdWidth;
    chainInfo.height = kOsdHeight;
    chainInfo.faceCount = 1;
    chainInfo.arraySize = 1;
    chainInfo.mipCount = 1;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &osd.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &osd.chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, osd.chain, osd.images);
    if (XR_SUCCEEDED(r) && !osd.images.empty())
        osd.upload = make_panel_upload(graphics, kOsdWidth, kOsdHeight);
    if (XR_FAILED(r) || osd.images.empty() || !osd.upload) {
        logf("performance overlay: card setup failed (%d, %s)", r, graphics.name());
        osd.failed = true;
        return false;
    }
    XrCompositionLayerQuad& l = osd.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    l.space = osd.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = osd.chain;
    l.subImage.imageRect = {{0, 0}, {kOsdWidth, kOsdHeight}};
    osd_place();
    if (experiment.views4 && graphics.d3d12)  // the GPU frame timer reads D3D12 timestamps (D3D11: no GPU times)
        gpu_frame_timer_start(graphics.d3d12, graphics.queue);
    // VRAM: this process's use of the adapter's local memory against the budget Windows gives it.
    IDXGIFactory4* factory = nullptr;
    LUID luid{};
    if (graphics.adapter_luid(luid) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&osd.adapter))))
            osd.adapter = nullptr;
        factory->Release();
    }
    // D3D11 (ACC, X-Plane): no queue timestamps, so the GPU tile is the whole GPU's load from Windows' engine
    // counters (probe/gpu_load.h). A D3D12 session started it at xrCreateSession; it stops with the session.
    if (!graphics.d3d12 && graphics.adapter_luid(luid))
        gpu_load_start(static_cast<uint32_t>(luid.HighPart), luid.LowPart);
    log_line(osd.adapter ? "performance overlay: card ready" : "performance overlay: card ready (no VRAM numbers)");
    return true;
}

OsdInput osd_input(int64_t now) {
    OsdInput in;
    in.frames.swap(osd.frames);
    in.periodMs = static_cast<double>(osd.periodNs.load(std::memory_order_relaxed)) / 1e6;
    if (experiment.views4) {
        const GpuFrameStats gpu = gpu_frame_timer_collect();
        if (gpu.frames) {
            in.gpuMs = gpu.renderAvg;
            if (experiment.views4Compose)
                in.composeMs = gpu.composeAvg;
        }
    }
    if (osd.adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (SUCCEEDED(osd.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            in.vramUsed = memory.CurrentUsage;
            in.vramBudget = memory.Budget;
        }
    }
    {
        std::lock_guard guard(gaze.mutex);
        const uint64_t queries = gaze.queries - osd.gazeQueries, tracked = gaze.tracked - osd.gazeTracked;
        osd.gazeQueries = gaze.queries;
        osd.gazeTracked = gaze.tracked;
        if (!experiment.gaze || queries == 0)  // off, or not asked (the focus does not follow the eyes here)
            in.gaze = gaze.action || !experiment.gaze ? OsdGaze::Off : OsdGaze::Unavailable;
        else
            in.gaze = tracked * 10 >= queries * 9 ? OsdGaze::Tracking : tracked * 2 >= queries ? OsdGaze::Partial : OsdGaze::Lost;
    }
    // Frame generation: Cheeky's, when the layer runs it with [cheeky] fg on.
    const CheekySettings& cheeky = osd.cheeky;
    if (cheeky_running() && cheeky.processing && cheeky.fg) {
        if (now >= osd.nextCheeky) {
            osd.cheekySnapshot = cheeky_snapshot();
            osd.nextCheeky = now + qpc_ms(kOsdCheekyMs);
        }
        const OsdCheeky c = osd_cheeky(osd.cheekySnapshot);
        in.fg = c.active ? OsdFg::On : OsdFg::Starting;
        in.fgMultiplier = cheeky.fgMultiplier;
        in.fgOutputFps = c.outputFps;
        in.fgMs = c.fgMs;
        in.fgState = c.state;
        in.gpuBusyPercent = c.gpuBusyPercent;
        if (c.refreshHz > 0)
            in.refreshMs = 1000.0 / c.refreshHz;  // the headset's own refresh (ACE's period may be multiplied)
    }
    // The whole GPU and ACE's threads, from the layer's own samplers (Cheeky's GPU share only without them),
    // averaged over the card's window (since its last draw).
    int samples = 0;
    if (const GpuLoad gpu = gpu_load_window(samples, 0); gpu.valid) {
        in.gpuBusyPercent = gpu.busy;
        in.gpuGamePercent = gpu.game;
    }
    if (const CpuLoad threads = cpu_load_window(samples, 0); threads.valid) {
        in.cpuBusiestPercent = threads.busiestPercent;
        in.cpuBusiestName = threads.busiestName;
        in.cpuBusiestIsRender = threads.busiestThread == osd.thread;
    }
    return in;
}

// The notice's card while it lasts (null once it is over, or if the card cannot be made); the overlay's card
// afterwards, redrawn before it shows again.
const XrCompositionLayerBaseHeader* notice_frame(XrSession session, int64_t now) {
    if (!notice_pending())
        return nullptr;
    if (!notice.until)
        notice.until = now + qpc_ms(kNoticeMs);
    if (now >= notice.until || !osd_resources(session)) {
        notice = {};
        osd.uploaded = false;  // the card holds the notice's pixels
        osd.nextDraw = 0;
        osd_place();
        log_line("notice: hidden");
        return nullptr;
    }
    if (!notice.uploaded) {
        osd_place();
        const std::vector<uint32_t> pixels = draw_notice_panel(notice.title, notice.lines);  // once: a few ms
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;
        if (XR_SUCCEEDED(next.AcquireSwapchainImage(osd.chain, nullptr, &index))) {
            notice.uploaded = XR_SUCCEEDED(next.WaitSwapchainImage(osd.chain, &wait)) && index < osd.images.size() &&
                              osd.upload && osd.upload->upload(osd.images[index], pixels);
            next.ReleaseSwapchainImage(osd.chain, nullptr);
        }
        if (notice.uploaded)
            log_line("notice: shown");
    }
    return notice.uploaded ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&osd.layer) : nullptr;
}

const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now) {
    if (const XrCompositionLayerBaseHeader* card = notice_frame(session, now))
        return card;
    if (!osd.read)
        osd_placement_changed();
    osd_hotkey();
    visor_hotkey();
    if (!osd.placement.enabled || !osd_resources(session))
        return nullptr;
    if (now >= osd.nextDraw && !osd.drawing.valid()) {
        osd.nextDraw = now + qpc_ms(kOsdDrawMs);
        OsdInput in = osd_input(now);
        osd.shown = osd_summary(in);  // also for the log
        osd.shownValid = true;
        osd.drawing = std::async(std::launch::async, [in = std::move(in), summary = osd.shown,
                                                      history = std::vector<double>(osd.history.begin(), osd.history.end())] {
            return draw_osd_panel(summary, history, osd_tiles(in, summary), osd_rows(in, summary));
        });
    }
    if (osd.drawing.valid() && osd.drawing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::vector<uint32_t> pixels = osd.drawing.get();
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;
        if (XR_SUCCEEDED(next.AcquireSwapchainImage(osd.chain, nullptr, &index))) {
            const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(osd.chain, &wait)) && index < osd.images.size() &&
                            osd.upload && osd.upload->upload(osd.images[index], pixels);
            next.ReleaseSwapchainImage(osd.chain, nullptr);
            osd.uploaded = osd.uploaded || ok;
        }
    }
    return osd.uploaded ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&osd.layer) : nullptr;
}

void osd_shutdown() {
    if (osd.drawing.valid())
        osd.drawing.wait();
    osd.drawing = {};
    osd.upload.reset();
    if (osd.chain)
        next.DestroySwapchain(osd.chain);
    if (osd.view)
        next.DestroySpace(osd.view);
    if (osd.adapter)
        osd.adapter->Release();
    osd.chain = XR_NULL_HANDLE;
    osd.view = XR_NULL_HANDLE;
    osd.adapter = nullptr;
    osd.images.clear();
    osd.uploaded = osd.failed = false;
    osd.frames.clear();
    osd.history.clear();
    osd.lastEnd = 0;
}

// ---- QVFR optimizer --------------------------------------------------------------------------
// The settings menu's Optimize (quad/qvfr_optimizer.h): Right runs it (or stops a run), Left undoes it.
// Every kOptimizeWindowMs a window of ACE's frames goes to the optimizer: the GPU timer's own samples
// (90th percentile) and the render thread's work, which the overlay's rule (osd_summary) also uses to say
// whether the CPU limits the frame. The values it asks for are written as a menu step is, so the live
// reload resizes the views (or, with DLSS holding them, the next launch does). [optimize] in the ini:
// target_percent (of the frame budget), min_periphery, min_density, min_focus (the floors it lowers to).
void optimize_apply(const QvfrValues& q) {
    MenuValues& v = menu.model.values;
    v.periphery = q.periphery;
    v.density = q.density;
    v.width = q.width;
    v.height = q.height;
    menu.dirty = true;
    menu.lastWrite = ini_write_later([ini = live.ini, settings = menu.settingsIni, values = v] {
        write_menu_values(ini.c_str(), values, true);
        if (!settings.empty())
            write_menu_values(settings.c_str(), values, true);
    });
    logf("optimize: periphery %d%%, focus density %d%%, focus box %d%% x %d%%", q.periphery, q.density, q.width,
         q.height);
}

void optimize_start(int64_t now) {
    optimize.note.clear();
    // It measures what each step does, so the views must resize live (with DLSS: views4_dlss_live).
    if (game->qvfr_dlss_held()) {
        optimize.note = L"DLSS holds the view sizes until the next launch (DLAA, or views4_dlss_live=0): it can't measure live";
        return;
    }
    int nativeW = 0;
    {
        std::lock_guard guard(quad.mutex);
        if (!gpu_frame_timer_start(quad.device, quad.queue)) {
            optimize.note = L"No GPU frame timing on this device";
            return;
        }
        nativeW = static_cast<int>(quad.nativeW);
    }
    gpu_frame_timer_collect_optimizer();  // drop what the windows gathered before the run
    int samples = 0;
    gpu_load_window(samples, 2);
    cpu_load_window(samples, 2);
    optimize.frames.clear();
    optimize.nextWindow = now + qpc_ms(kOptimizeWindowMs);
    const wchar_t* ini = live.ini.c_str();
    const auto read = [ini](const wchar_t* key, int fallback, int lo, int hi) {
        return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"optimize", key, fallback, ini)), lo, hi);
    };
    const MenuValues& v = menu.model.values;
    const MenuLaunch& l = menu.model.launch;
    QvfrLimits limits;
    const int minFocus = read(L"min_focus", limits.floor.width, 10, 100);
    limits.floor = {read(L"min_periphery", limits.floor.periphery, 10, 100), read(L"min_density", limits.floor.density, 50, 400),
                    minFocus, minFocus};
    limits.cropWidth = v.crop_width();
    limits.cropHeight = v.crop_height();
    limits.nativeWidth = nativeW;
    // The views fit in the launch target (views4_live), at most their launch sizes.
    limits.ceiling.periphery = static_cast<int>(l.outer);
    limits.ceiling.density = static_cast<int>(std::min(l.focusWidth * 100 / v.width, l.focusHeight * 100 / v.height));
    if (nativeW > 0)
        limits.maxTargetWidth = static_cast<int>(2.0 * nativeW * (l.outer * l.cropWidth / 1e4 + l.focusWidth / 100));
    // From what is in effect (a value above the launch size renders at what fits).
    const MenuFit fit = menu_fit(menu.model);
    const QvfrValues current{static_cast<int>(std::lround(fit.periphery)), static_cast<int>(std::lround(fit.densityX)),
                             v.width, v.height};
    optimize.optimizer.start(current, limits, read(L"target_percent", 88, 50, 98) / 100.0);
    logf("optimize: start from periphery %d%%, focus density %d%%, box %d%% x %d%%; floors %d%%, %d%%, %d%%; "
         "ceilings %d%%, %d%%",
         current.periphery, current.density, current.width, current.height, limits.floor.periphery,
         limits.floor.density, limits.floor.width, limits.ceiling.periphery, limits.ceiling.density);
}

// The budget is the period xrWaitFrame gives ACE: with Cheeky's frame generation already the multiplied
// one, so `in` leaves frame generation off. The GPU time is the whole GPU's per ACE frame (frame
// generation, the composition and the runtime's compositor too) when the load sampler has it, else ACE's
// own queue (90th percentile).
QvfrWindow optimize_window() {
    const GpuFrameStats gpu = gpu_frame_timer_collect_optimizer();
    OsdInput in;
    in.frames.swap(optimize.frames);
    in.periodMs = static_cast<double>(osd.periodNs.load(std::memory_order_relaxed)) / 1e6;
    in.gpuMs = gpu.frames ? gpu.renderP90 : -1;
    int samples = 0;
    if (const GpuLoad load = gpu_load_window(samples, 2); load.valid) {
        in.gpuBusyPercent = load.busy;
        in.gpuGamePercent = load.game;
    }
    if (const CpuLoad threads = cpu_load_window(samples, 2); threads.valid) {
        in.cpuBusiestPercent = threads.busiestPercent;
        in.cpuBusiestIsRender = threads.busiestThread == osd.thread;
    }
    const OsdSummary s = osd_summary(in);
    QvfrWindow w;
    w.gpuMs = s.gpuMs;
    w.cpuMs = s.cpuMs;
    w.budgetMs = s.budgetMs;
    w.cpuBound = s.bound == OsdBound::Cpu;
    w.frames = gpu.frames;
    return w;
}

void optimize_tick(int64_t now) {
    QvfrOptimizer& o = optimize.optimizer;
    if (const int request = std::exchange(menu.model.optimizeRequest, 0); request != 0 && experiment.views4) {
        optimize.shown = true;
        if (request > 0 && o.running()) {
            o.cancel();
            log_line("optimize: stopped");
        } else if (request > 0) {
            optimize_start(now);
        } else {
            o.cancel();
            optimize.note.clear();
            if (const QvfrAction back = o.undo(); back.apply)
                optimize_apply(back.values);
        }
    }
    if (o.running()) {
        if (menu.visible)  // shown until it is done
            menu.hideAt = std::max(menu.hideAt, now + qpc_ms(kMenuHideMs));
        if (now >= optimize.nextWindow) {
            optimize.nextWindow = now + qpc_ms(kOptimizeWindowMs);
            if (const QvfrAction a = o.feed(optimize_window()); a.apply)
                optimize_apply(a.values);
        }
    } else {
        optimize.frames.clear();
    }
    // The menu's line and detail.
    std::wstring line, detail;
    if (optimize.shown) {
        detail = optimize.note.empty() ? o.status() : optimize.note;
        switch (o.state()) {
        case QvfrState::Measuring:
        case QvfrState::Settling: line = L"Running…"; break;
        case QvfrState::Done: line = o.can_undo() ? L"Done (Left: undo)" : L"Done"; break;
        case QvfrState::Failed: line = L"Not possible"; break;
        default: line = optimize.note.empty() ? L"" : L"Not possible"; break;
        }
    }
    if (line != menu.model.optimizeStatus || detail != menu.model.optimizeDetail) {
        if (detail != menu.model.optimizeDetail && !detail.empty())
            logf("optimize: %ls", detail.c_str());
        menu.model.optimizeStatus = line;
        menu.model.optimizeDetail = detail;
        menu.dirty = true;
    }
}

// cpu_profile=1: a capture of the busiest threads (and this one, the render thread) at cpu_profile_frame, and
// whenever Ctrl+Alt+C is pressed while ACE has focus and none is running.
bool cpuProfileHotkeyHeld = false;
bool frameCaptureHotkeyHeld = false;
// The capture's start and end in the headset: the settings menu's panel shows for a few seconds with a line
// under its title (QVFR and Two-loop: the panel is theirs; with mode=off only the log says).
struct ProfileNotice {
    int64_t until = 0;
    bool wasRunning = false;
};
ProfileNotice profileNotice;
void profile_notice(const std::wstring& text, int64_t ms) {
    if (!experiment.views4 && !experiment.quad)
        return;  // no settings menu to show it in (baseline): the log has it
    const int64_t now = qpc_now();
    menu.model.notice = text;
    menu.dirty = true;
    menu_open(now);
    menu.hideAt = now + qpc_ms(ms);
    profileNotice.until = menu.hideAt;
}

// The frame capture hotkey (d3d11_capture_hotkey, probe/d3d11_capture.h): each press saves the next frame's swapchain
// images, the game's and the composed output: what the headset gets at that moment.
void frame_capture_poll() {
    if (!experiment.d3d11CaptureHotkey)
        return;
    const bool held = hotkey_held(HotkeyAction::FrameCapture);
    const bool pressed = held && !frameCaptureHotkeyHeld;
    frameCaptureHotkeyHeld = held;
    if (pressed && d3d11_capture_arm(endFrames + 1))
        profile_notice(L"Frame capture: saved to the logs folder (captures)", 2500);
}

void cpu_profile_poll() {
    if (!experiment.cpuProfile)
        return;
    const bool running = cpu_profile_running();
    if (profileNotice.wasRunning && !running)
        profile_notice(L"CPU capture done: the report is in the logs folder", 5000);
    profileNotice.wasRunning = running;
    if (profileNotice.until && qpc_now() >= profileNotice.until) {
        profileNotice.until = 0;
        menu.model.notice.clear();
        menu.dirty = true;
    }
    const bool held = hotkey_held(HotkeyAction::CpuCapture);
    const bool pressed = held && !cpuProfileHotkeyHeld;
    cpuProfileHotkeyHeld = held;
    // cpu_profile_frame=0 is no capture by itself, not one at the session's first frame.
    if (!pressed && (experiment.cpuProfileFrame == 0 || endFrames != experiment.cpuProfileFrame))
        return;
    if (cpu_profile_running()) {
        log_line("cpu profile: a capture is running already");
        return;
    }
    // The render thread (this one), ACE's game and physics threads by name (busy or not at this moment: a
    // capture started on a loading screen still gets them), then the busiest others.
    CpuProfileRequest request;
    request.seconds = experiment.cpuProfileSeconds;
    request.threads.push_back(GetCurrentThreadId());
    const std::vector<ThreadLoad> all = cpu_load_latest_threads(256);
    for (const ThreadLoad& t : all)
        if (t.name == L"GameThread" || t.name == L"Physics")
            request.threads.push_back(t.id);
    for (size_t i = 0, added = 0; i < all.size() && added < static_cast<size_t>(experiment.cpuProfileThreads); ++i)
        if (std::find(request.threads.begin(), request.threads.end(), all[i].id) == request.threads.end()) {
            request.threads.push_back(all[i].id);
            ++added;
        }
    if (cpu_profile_start(request)) {
        profileNotice.wasRunning = true;
        profile_notice(L"CPU capture: " + std::to_wstring(experiment.cpuProfileSeconds) + L" s, keep driving", 4000);
    }
}

XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info) {
    hang_watch_frame();
    cpu_load_render_thread(GetCurrentThreadId());
    cpu_profile_poll();
    frame_capture_poll();
    if ((experiment.views4 || experiment.measure) && cpu.msPerTick > 0) {
        const int64_t now = qpc_now();
        if (pacing.lastEnd && pacing.interval.size() < 4 * kCpuReportFrames)
            pacing.interval.push_back(static_cast<float>((now - pacing.lastEnd) * cpu.msPerTick));
        pacing.lastEnd = now;
    }
    osd_measure();   // before the layer's own work in the frame
    cheeky_frame();  // Cheeky's frame callback, after ACE's frame is recorded
    XrResult r;
    if (!cpu.realEndFrame) {
        r = end_frame(session, info);
    } else {
        const int64_t t0 = qpc_now();
        r = end_frame(session, info);
        cpu.endFrame.add(qpc_now() - t0);
        if (endFrames % kCpuReportFrames == 0)
            report_cpu_times();
    }
    live_reload();
    return r;
}

XrResult end_frame(XrSession session, const XrFrameEndInfo* frameInfo) {
    XrFrameEndInfo withMenu{};
    std::vector<const XrCompositionLayerBaseHeader*> menuLayers;
    const XrFrameEndInfo* info = menu_frame(session, frameInfo, withMenu, menuLayers);
    ++endFrames;
    if (experiment.pixCaptureFrame)
        pix_capture_frame(endFrames);
    if (sampled(endFrames)) {
        logf("xrEndFrame frame=%llu time=%lld layers=%u", static_cast<unsigned long long>(endFrames),
             static_cast<long long>(info->displayTime), info->layerCount);
        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* layer = info->layers[i];
            if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                logf("  layer %u type=%d flags=0x%llx", i, layer->type,
                     static_cast<unsigned long long>(layer->layerFlags));
                continue;
            }
            auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            for (uint32_t v = 0; v < proj->viewCount; ++v) {
                const XrSwapchainSubImage& s = proj->views[v].subImage;
                const auto* chained = static_cast<const XrBaseInStructure*>(proj->views[v].next);
                logf("  projection view %u swapchain=%p rect=%d,%d %dx%d array=%u next=%d", v,
                     static_cast<void*>(s.swapchain), s.imageRect.offset.x, s.imageRect.offset.y,
                     s.imageRect.extent.width, s.imageRect.extent.height, s.imageArrayIndex,
                     chained ? chained->type : 0);
            }
        }
    }
    if (experiment.probes) {
        probe_frame_end();
        d3d12_trace_frame_end(probe_sequence_active());
    }
    if (experiment.passCensus)
        pass_census_frame_end();
    game->frame_end();
    if (experiment.views4) {
        game->qvfr_frame_end();
        return views4_submit(session, info);
    }
    if (!experiment.twoLoops)
        return next.EndFrame(session, info);

    std::unique_lock lock(loops.mutex);
    if (!loops.active) {
        ++loops.ends;
        if (loops.ends > loops.begins && !loops.broken) {
            loops.broken = true;
            log_line("two_loops: refused: xrEndFrame calls outnumber xrBeginFrame (a frame generator above this "
                     "layer, e.g. VectorXR Frame Generation?); turn it off for the game");
        }
        lock.unlock();
        return next.EndFrame(session, info);
    }
    LARGE_INTEGER now;
    const uint64_t index = loops.pairedEnds++;
    if (index % 2 == 0) {  // A: rendered for the display time, never submitted
        QueryPerformanceCounter(&now);
        loops.aMs += ms_between(loops.aWaitReturned, now);
        if (experiment.quad)
            quad_record_a(info);
        return XR_SUCCESS;
    }
    loops.inB = false;
    lock.unlock();
    const XrResult r = experiment.quad ? quad_submit_b(session, info) : next.EndFrame(session, info);
    lock.lock();
    // A fallback shows only frame B's focus region (black around it). Brief ones are harmless; a run of
    // them means composition does not work here, so stop pairing frames and let ACE render normally.
    if (experiment.quad) {
        std::lock_guard guard(quad.mutex);
        if (quad.fallbackRun >= kMaxFallbackRun) {
            loops.active = false;
            loops.broken = true;
            loops.inB = false;
            logf("quad: %llu frames in a row could not be composed; quad off, ACE runs unchanged",
                 static_cast<unsigned long long>(quad.fallbackRun));
        }
    }
    QueryPerformanceCounter(&now);
    loops.bMs += ms_between(loops.bWaitReturned, now);
    ++loops.pairs;
    const double sinceReport = ms_between(loops.lastReport, now);
    if (sinceReport >= 2000.0) {
        logf("two_loops: %.1f display frames/s, ACE frame A %.2f ms, frame B %.2f ms (wait return to end), "
             "result=%d, quad composed %llu fallbacks %llu, gaze tracked %llu/%llu (%.2f,%.2f,%.2f)",
             loops.pairs * 1000.0 / sinceReport, loops.aMs / loops.pairs, loops.bMs / loops.pairs, r,
             static_cast<unsigned long long>(quad.composed), static_cast<unsigned long long>(quad.fallbacks),
             static_cast<unsigned long long>(gaze.tracked), static_cast<unsigned long long>(gaze.queries),
             gaze.direction.x, gaze.direction.y, gaze.direction.z);
        loops.pairs = 0;
        loops.aMs = loops.bMs = 0;
        loops.lastReport = now;
    }
    return r;
}

XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

#define SIMQVFR_INTERCEPT(fn)                                              \
    if (std::strcmp(name, "xr" #fn) == 0) {                              \
        *function = reinterpret_cast<PFN_xrVoidFunction>(&layer##fn);    \
        return XR_SUCCESS;                                               \
    }

XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    SIMQVFR_INTERCEPT(GetInstanceProcAddr)
    if (next.EndFrame) {
        SIMQVFR_INTERCEPT(EnumerateViewConfigurationViews)
        SIMQVFR_INTERCEPT(CreateSession)
        SIMQVFR_INTERCEPT(DestroySession)
        SIMQVFR_INTERCEPT(BeginSession)
        SIMQVFR_INTERCEPT(WaitFrame)
        SIMQVFR_INTERCEPT(BeginFrame)
        SIMQVFR_INTERCEPT(CreateSwapchain)
        SIMQVFR_INTERCEPT(EnumerateSwapchainImages)
        SIMQVFR_INTERCEPT(LocateViews)
        SIMQVFR_INTERCEPT(CreateReferenceSpace)
        SIMQVFR_INTERCEPT(DestroySpace)
        SIMQVFR_INTERCEPT(EndFrame)
        SIMQVFR_INTERCEPT(AcquireSwapchainImage)
        SIMQVFR_INTERCEPT(ReleaseSwapchainImage)
        SIMQVFR_INTERCEPT(SuggestInteractionProfileBindings)
        SIMQVFR_INTERCEPT(AttachSessionActionSets)
        SIMQVFR_INTERCEPT(SyncActions)
        SIMQVFR_INTERCEPT(PollEvent)
        if (next.GetVisibilityMaskKHR)
            SIMQVFR_INTERCEPT(GetVisibilityMaskKHR)
    }
    return next.GetInstanceProcAddr ? next.GetInstanceProcAddr(instance, name, function) : XR_ERROR_HANDLE_INVALID;
}

// The layers between this one and the runtime. VectorXR's frame generation has to be among them:
// above this layer it would get ACE's four-view image instead of the composed stereo frame.
void log_layer_order(const XrApiLayerNextInfo* below) {
    std::string names;
    bool vectorxrBelow = false;
    for (const XrApiLayerNextInfo* n = below; n; n = n->next) {
        names += names.empty() ? "" : ", ";
        names += n->layerName;
        vectorxrBelow |= std::strstr(n->layerName, "VECTORXR") != nullptr;
    }
    logf("layers below this one: [%s]", names.c_str());
    layersBelow = names;
    if (!vectorxrBelow && GetModuleHandleW(L"XR_APILAYER_DIENERTECH_VECTORXR.dll"))
        log_line("VectorXR is loaded above this layer: its frame generation would see the game's four-view image; move "
                 "this layer above VectorXR (machine-wide registration, then VectorXR's layer order)");
}

// Whether the layer adds XR_EXT_eye_gaze_interaction to the game's instance (layerCreateApiLayerInstance).
enum class GazeExtension {
    Requested,  // the game enables it itself
    Add,        // the runtime (through the layers below) lists it: add it
    Try,        // the list didn't answer: add it, and create the instance without it if that fails
    Absent,     // the runtime doesn't list it: leave the request as it is
};

GazeExtension gaze_extension(const XrInstanceCreateInfo& info, PFN_xrGetInstanceProcAddr gipa) {
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        if (std::strcmp(info.enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Requested;
    PFN_xrVoidFunction function = nullptr;
    if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function)) || !function)
        return GazeExtension::Try;
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    uint32_t count = 0;
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)))
        return GazeExtension::Try;
    std::vector<XrExtensionProperties> properties(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (count && XR_FAILED(enumerate(nullptr, count, &count, properties.data())))
        return GazeExtension::Try;
    for (const XrExtensionProperties& p : properties)
        if (std::strcmp(p.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Add;
    return GazeExtension::Absent;
}

// Why a game can start flat with this layer loaded: the OpenXR loader negotiates every layer, then can still
// fail xrCreateInstance before the chain reaches this one (a layer named in XR_ENABLE_API_LAYERS whose manifest
// it refuses, an extension nobody provides). The loader prints its reason on the game's stderr, which the launcher
// keeps in the run folder's openxr-loader.log; this log names the layers and says when that happened.
std::atomic<int> loaderNegotiations{0};
std::atomic<bool> instanceReachedLayer{false};

void note_loader_negotiation() {
    const int n = ++loaderNegotiations;
    if (n == 1) {
        char enabled[2048] = {};
        GetEnvironmentVariableA("XR_ENABLE_API_LAYERS", enabled, static_cast<DWORD>(std::size(enabled)));
        logf("OpenXR layers named for this launch: %s", enabled[0] ? enabled : "(none: implicit layers only)");
    } else if (!instanceReachedLayer) {
        log_line("the game's previous xrCreateInstance failed before reaching this layer (no VR: the game starts "
                 "flat). The OpenXR loader's reason is in this launch's run folder, openxr-loader.log");
    }
}

// The layers named in SIM_QVFR_CHAIN, loaded from their manifests in XR_API_LAYER_PATH and linked below
// `self` (layer_chain.h: the launcher does this for a game whose OpenXR loader can't take a list of layers).
// A layer that fails to load is left out and logged, as the loader would. `self` when there is none.
const XrApiLayerNextInfo* chain_layers_below(const XrApiLayerNextInfo* self) {
    static std::vector<XrApiLayerNextInfo> storage;  // outlives the create call down the chain
    wchar_t value[4096] = {}, folder[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(chain::kChainVariable, value, static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value))
        return self;
    GetEnvironmentVariableW(L"XR_API_LAYER_PATH", folder, static_cast<DWORD>(std::size(folder)));
    std::vector<chain::Layer> layers;
    char names[4096] = {};  // layer names are ASCII
    WideCharToMultiByte(CP_UTF8, 0, value, -1, names, static_cast<int>(std::size(names)), nullptr, nullptr);
    for (const std::string& name : chain::split_names(names)) {
        chain::Layer layer;
        std::string why;
        if (chain::load(folder, name, layer, why)) {
            layers.push_back(layer);
            logf("chain: %s loaded below this layer", name.c_str());
        } else {
            logf("chain: %s left out: %s", name.c_str(), why.c_str());
        }
    }
    return chain::link(layers, self, storage);
}

XrResult XRAPI_CALL layerCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!info || !layerInfo || !layerInfo->nextInfo || !instance ||
        std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    instanceReachedLayer = true;
    // This layer's node, with the layers the launcher asked it to chain (SIM_QVFR_CHAIN) linked below it.
    const XrApiLayerNextInfo* own = chain_layers_below(layerInfo->nextInfo);
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = own->next;
    PFN_xrGetInstanceProcAddr gipa = own->nextGetInstanceProcAddr;
    // Registered as an implicit layer, so it loads into every OpenXR app: stay silent and
    // pass-through everywhere except a supported game (src/games).
    const games::GameInfo* thisGame = games::current_game();
    if (!thisGame) {
        next = Dispatch{gipa};
        return own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    }
    // Eye tracking needs XR_EXT_eye_gaze_interaction on the instance. ACE and X-Plane ask for it themselves; ACC
    // (through OpenComposite) doesn't, so its gaze worked only with a layer below that adds it (Cheeky, OpenXR Toolkit,
    // VectorXR), and failed with none (the eye gaze profile's bindings rejected, XR_ERROR_PATH_UNSUPPORTED). So the
    // layer adds it when the game hasn't and the runtime offers it; with the offer unknown it tries, and creates the
    // instance without it if the runtime refuses.
    const GazeExtension gazeExtension = gaze_extension(*info, gipa);
    XrResult r = XR_ERROR_EXTENSION_NOT_PRESENT;
    if (gazeExtension == GazeExtension::Add || gazeExtension == GazeExtension::Try) {
        std::vector<const char*> names(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        names.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        XrInstanceCreateInfo withGaze = *info;
        withGaze.enabledExtensionCount = static_cast<uint32_t>(names.size());
        withGaze.enabledExtensionNames = names.data();
        r = own->nextCreateApiLayerInstance(&withGaze, &nextLayerInfo, instance);
    }
    const bool gazeAdded = XR_SUCCEEDED(r);
    if (!gazeAdded)
        r = own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    game = &games::adapter_for(thisGame);
    load_experiment();
    // Where the launcher writes live settings while this ACE runs (live_reload).
    runtime_info(L"live_ini", live.ini);
    runtime_info(L"live_pid", std::to_wstring(GetCurrentProcessId()));
    log_layer_order(own->next);
    logf("xrCreateApiLayerInstance app=%s engine=%s extensions=%u result=%d", info->applicationInfo.applicationName,
         info->applicationInfo.engineName, info->enabledExtensionCount, r);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        logf("  extension %s", info->enabledExtensionNames[i]);
    switch (gazeExtension) {
    case GazeExtension::Requested: log_line("gaze: the game enables XR_EXT_eye_gaze_interaction itself"); break;
    case GazeExtension::Absent: log_line("gaze: the runtime doesn't offer XR_EXT_eye_gaze_interaction (no eye tracking)"); break;
    case GazeExtension::Add:
    case GazeExtension::Try:
        logf("gaze: XR_EXT_eye_gaze_interaction %s (%s)",
             gazeAdded ? "added for the game" : "NOT added: the runtime refused the instance with it",
             gazeExtension == GazeExtension::Add ? "the runtime offers it" : "the runtime's offer unknown, tried");
        break;
    }
    if (XR_FAILED(r))
        return r;
    // Seconds before ACE initialises NGX, so Cheeky's hooks see its DLSS features created. Only in a game
    // it's made for.
    if (game->loads_cheeky()) {
        cheeky_start(cheeky_config(live.ini.c_str(), layer_directory()));
        {  // for the launcher's Foveated DLSS pane
            static const wchar_t* const kStatus[] = {L"off", L"missing", L"refused", L"running"};
            runtime_info(L"cheeky_status", kStatus[static_cast<int>(cheeky_status())]);
            if (cheeky_running())
                runtime_info(L"cheeky_version", cheeky_version());
        }
        cheeky_apply_ini();
    }
    // Before ACE creates its D3D12 device and renderer, so renderer init sees the patched mode.
    if (experiment.dred)
        dred_enable();
    if (experiment.d3d12Debug)
        d3d12_debug_enable(experiment.gpuValidation);
    if (experiment.pixCaptureFrame)  // before ACE creates its D3D12 device
        pix_capture_load(experiment.pixCaptureFrame);
    game->instance_created(live.ini.c_str(), experiment.mode);
    if (experiment.views4) {
        games::QvfrStart start;
        start.layout = experiment.views4Layout;
        start.focusFraction = experiment.views4EyeCamera ? 0.0 : experiment.box_x();
        start.focusFractionY = experiment.box_y();
        start.dlssLive = experiment.views4DlssLive;
        // The cull box: the focus box plus its feather band plus a gaze-latency allowance.
        start.focusCull = experiment.views4FocusCull;
        start.focusCullScale = 1.0 + experiment.feather + experiment.views4FocusCullMargin;
        start.focusCullGaze = experiment.gaze && experiment.composing();  // gaze turns only when composing
        start.ini = live.ini.c_str();
        if (!game->qvfr_start(start)) {
            experiment.views4 = false;  // no doubled eye width or rect rewrite without the engine side
            notice_set(L"Sim-QVFR is off",
                       {L"This " + std::wstring(thisGame->name) +
                            L" build isn’t one Sim-QVFR supports, so the game runs as normal, "
                            L"without foveated rendering. The game has probably been updated.",
                        L"Look for an Sim-QVFR release for the new build. The launcher’s Launch checks say the same "
                        L"before a launch."});
        }
    }
    Dispatch d;
    d.GetInstanceProcAddr = gipa;
    bool ok = true;
#define SIMQVFR_LOAD(fn) ok &= XR_SUCCEEDED(gipa(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction*>(&d.fn))) && d.fn;
    SIMQVFR_LOAD(EnumerateViewConfigurationViews)
    SIMQVFR_LOAD(CreateSession)
    SIMQVFR_LOAD(DestroySession)
    SIMQVFR_LOAD(BeginSession)
    SIMQVFR_LOAD(WaitFrame)
    SIMQVFR_LOAD(BeginFrame)
    SIMQVFR_LOAD(CreateSwapchain)
    SIMQVFR_LOAD(EnumerateSwapchainImages)
    SIMQVFR_LOAD(LocateViews)
    SIMQVFR_LOAD(EndFrame)
    SIMQVFR_LOAD(AcquireSwapchainImage)
    SIMQVFR_LOAD(WaitSwapchainImage)
    SIMQVFR_LOAD(ReleaseSwapchainImage)
    SIMQVFR_LOAD(StringToPath)
    SIMQVFR_LOAD(CreateActionSet)
    SIMQVFR_LOAD(CreateAction)
    SIMQVFR_LOAD(SuggestInteractionProfileBindings)
    SIMQVFR_LOAD(AttachSessionActionSets)
    SIMQVFR_LOAD(SyncActions)
    SIMQVFR_LOAD(PollEvent)
    SIMQVFR_LOAD(CreateActionSpace)
    SIMQVFR_LOAD(LocateSpace)
    SIMQVFR_LOAD(CreateReferenceSpace)
    SIMQVFR_LOAD(DestroySpace)
    SIMQVFR_LOAD(DestroySwapchain)
    SIMQVFR_LOAD(GetActionStateBoolean)
    SIMQVFR_LOAD(GetActionStateVector2f)
    SIMQVFR_LOAD(GetActionStatePose)
    SIMQVFR_LOAD(GetCurrentInteractionProfile)
    SIMQVFR_LOAD(PathToString)
#undef SIMQVFR_LOAD
    gipa(*instance, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&d.GetVisibilityMaskKHR));
    if (ok && (experiment.views4 || experiment.measure)) {  // time what runs below this layer (report_cpu_times)
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        cpu.msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
        cpu.realEndFrame = d.EndFrame;
        cpu.realLocateViews = d.LocateViews;
        d.EndFrame = timedEndFrameBelow;
        d.LocateViews = timedLocateViewsBelow;
    }
    next = ok ? d : Dispatch{gipa};
    if (!ok)
        log_line("missing downstream functions; pass-through only");
    else
        gaze_init(*instance);
    return r;
}

}  // namespace
}  // namespace simqvfr

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !layerName || !request || std::strcmp(layerName, simqvfr::kLayerName) != 0 ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_API_VERSION_1_0;
    request->getInstanceProcAddr = simqvfr::layerGetInstanceProcAddr;
    request->createApiLayerInstance = simqvfr::layerCreateApiLayerInstance;
    if (simqvfr::is_game_process()) {
        simqvfr::log_line("Loader negotiated API layer interface v1 (Sim-QVFR probe build)");
        simqvfr::note_loader_negotiation();
    }
    return XR_SUCCESS;
}

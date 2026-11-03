// XR_APILAYER_ACE_quadview: logs what Assetto Corsa EVO asks for (views, swapchains, frame
// submission) and starts the log-only engine probes (probe/probe.cpp) at xrCreateSession. With
// mode=off nothing alters rendering; the experiments and the quad pipeline are selected in
// ace-quadview.ini.
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
#include "experiment/double_render.h"
#include "experiment/views3.h"
#include "experiment/views4.h"
#include "log.h"
#include "probe/d3d12_trace.h"
#include "probe/cpu_load.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "probe/pass_census.h"
#include "probe/pix_capture.h"
#include "quad/focus_turn.h"
#include "quad/fov_crop.h"
#include "quad/helmet.h"
#include "probe/dred.h"
#include "probe/batch_watch.h"
#include "probe/hang_watch.h"
#include "probe/probe.h"
#include "quad/compositor.h"
#include "quad/focus_offset.h"
#include "quad/focus_outline.h"
#include "quad/menu_panel.h"
#include "quad/osd.h"
#include "quad/settings_menu.h"
#include "xr.h"

#include <dxgi1_4.h>
#include <intrin.h>

namespace aceqv {
namespace {

constexpr char kLayerName[] = "XR_APILAYER_ACE_quadview";
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
    PFN_xrCreateActionSpace CreateActionSpace = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrGetActionStateBoolean GetActionStateBoolean = nullptr;
    PFN_xrGetActionStateVector2f GetActionStateVector2f = nullptr;
    PFN_xrGetCurrentInteractionProfile GetCurrentInteractionProfile = nullptr;
    PFN_xrPathToString PathToString = nullptr;
    PFN_xrGetVisibilityMaskKHR GetVisibilityMaskKHR = nullptr;  // optional (XR_KHR_visibility_mask)
};

Dispatch next;
std::mutex chainMutex;
std::map<XrSwapchain, std::pair<uint32_t, uint32_t>> chainSizes;

// Round 5 experiment, read from ace-quadview.ini next to the DLL (ACE starts through Steam, so
// there is no per-launch environment): [experiment] mode=focus_only narrows every located view to a
// centred focus box. ACE builds its eye cameras and its submitted projection views from these FOVs.
struct Experiment {
    bool focusOnly = false;
    bool doubleRender = false;    // round 6: render_frame runs twice per frame, identical cameras
    bool views3 = false;          // single-frame quad views step 1: VR runs the renderer's 3-view mode
    bool views4 = false;          // stage gate: 4 views in one VR frame (views 2-3 copy the eyes)
    wchar_t views4Callers[512] = L"init";  // viewCount call sites that get 4 views (views4.h)
    wchar_t views4Borrow[256] = L"";     // lighting-pass offsets views 2-3 take from their eye (views4.h)
    bool views4ShowCopies = false;         // views4 debug: submit quarters 2-3 (the copies) instead of 0-1
    bool views4EyesOnlyAdvance = false;    // views4: skip the history-slot advance for views 2-3 (views4.h)
    bool views4Compose = false;            // views4: blend the focus views into the outer views (native-size output)
    bool views4NoMask = false;             // views4: empty hidden-area mesh (default when composing)
    bool views4EyeCamera = false;          // views4: focus views render the eyes' own cameras (diagnostic)
    bool passCensus = false;              // log a per-pipeline draw/dispatch census (probe/pass_census.h)
    bool views4FocusCull = false;          // views4: focus views skip draws outside their frustum (views4.h)
    double views4FocusCullMargin = 0.25;   // focus cull: extra extent beyond the feather band (gaze latency)
    bool views4ShadowView1Plain = false;   // diagnostic: no view-1 shortcut in deferredShadows (views4.h)
    bool views4ScreenSizeLegacy = false;   // diagnostic: screenSizeDiv keeps the stereo ratio (views4.h)
    bool uavBarriers = false;              // diagnostic: UAV barrier after every dispatch (probe/pass_census.h)
    bool gpuFrameTiming = false;           // views4: timestamps time ACE's frame on its queue (probe/gpu_frame_timer.h)
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
    bool quad = false;            // quad pipeline: two_loops + b_focus, composited into one native-size image
    std::atomic<double> feather = 0.10;    // quad: blend band, as a fraction of the focus box
    std::atomic<double> cornerRadius = 0;  // rounded focus box: radius as a share of half its shorter side (0 = square)
    std::atomic<bool> border = false;      // quad: debug outline (borderColor) on the focus box (views4: on the outer views)
    bool borderAtLaunch = false;           // views4 without composition tracks the outline's chains only if on at launch
    float borderColor[4] = {0, 1, 0, 1};   // debug outline colour (linear RGBA): debug_border_color green|magenta|teal
    std::atomic<bool> gaze = true;         // quad: the focus box follows XR_EXT_eye_gaze_interaction
    // The focus box's vertical offset, degrees up (focus_offset.h): fixed, and added to a tracked gaze.
    std::atomic<int> fixedOffsetY = 0, gazeOffsetY = 0;
    UINT captureEverySeconds = 0; // quad debug: saves A, B and the output (left eye) as PNGs, 0 = off
    bool probes = true;           // engine probes and D3D12 trace (log-only reverse-engineering aids)
    bool batchWatch = true;       // logs ACE's instance visibility batch against its limits (probe/batch_watch.h)
    UINT hangWatchSeconds = 30;   // no frame this long after xrCreateSession: modules and a minidump (probe/hang_watch.h)
};
Experiment experiment;
bool focusLogged = false;
// The helmet overlay ([helmet], views4 composing; helmet.h): set at launch, then live under quad.mutex.
HelmetSettings helmetSettings;

// The settings a running game takes from its ini without a restart (live_reload): the focus box,
// its blend and outline, gaze and (views4) the FOV crop and the helmet overlay. Resolution is fixed at launch: ACE sizes its render target once,
// from the eye size offered at session start, so the focus views keep their pixels and a live box
// size changes their density.
struct LiveSettings {
    double focusFraction = 0.35, focusFractionY = 0, feather = 0.10, cornerRadius = 0;
    bool border = false, gaze = true;
    FocusOffset offset;
    float borderColor[4] = {0, 1, 0, 1};
    FovCrop crop;  // views4 only
    HelmetSettings helmet;  // views4 composing only
};

// [crop] (QVFR): the sides of each eye's FOV ACE does not render (fov_crop.h), each on its own.
FovCrop read_crop(const wchar_t* ini) {
    if (GetPrivateProfileIntW(L"crop", L"enabled", 0, ini) == 0)
        return {};
    const auto side = [&](const wchar_t* key) { return static_cast<int>(GetPrivateProfileIntW(L"crop", key, 0, ini)); };
    return fov_crop(side(L"top"), side(L"bottom"), side(L"outer"), side(L"inner"));
}

LiveSettings read_live(const wchar_t* ini) {
    LiveSettings l;
    l.crop = read_crop(ini);
    l.helmet = read_helmet_settings(ini);
    l.focusFraction = std::clamp(GetPrivateProfileIntW(L"experiment", L"focus_percent", 35, ini), 10u, 100u) / 100.0;
    const UINT heightPercent = GetPrivateProfileIntW(L"experiment", L"focus_height_percent", 0, ini);
    l.focusFractionY = heightPercent ? std::clamp(heightPercent, 10u, 100u) / 100.0 : 0.0;
    l.feather = std::clamp(GetPrivateProfileIntW(L"quad", L"feather_percent", 10, ini), 0u, 50u) / 100.0;
    l.cornerRadius = std::clamp(GetPrivateProfileIntW(L"quad", L"corner_radius_percent", 0, ini), 0u, 100u) / 100.0;
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

// What the launcher shows before the next launch (ACE_QV_RUNTIME_INFO, set by the launcher): the
// headset's recommended per-eye size and whether eye tracking delivered samples. An ini file.
void runtime_info(const wchar_t* key, const std::wstring& value) {
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"ACE_QV_RUNTIME_INFO", buffer, MAX_PATH);
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

void load_experiment() {
    // A launcher passes its per-launch settings in ACE_QV_INI; otherwise the ini next to the DLL.
    std::wstring ini = (std::filesystem::path(layer_directory()) / L"ace-quadview.ini").wstring();
    wchar_t env[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"ACE_QV_INI", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH && std::filesystem::path(env).is_absolute())
        ini = env;
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"experiment", L"mode", L"off", mode, 32, ini.c_str());
    experiment.focusOnly = _wcsicmp(mode, L"focus_only") == 0;
    experiment.doubleRender = _wcsicmp(mode, L"double_render") == 0;
    experiment.views3 = _wcsicmp(mode, L"views3") == 0;
    experiment.views4 = _wcsicmp(mode, L"views4") == 0;
    GetPrivateProfileStringW(L"experiment", L"views4_callers", L"init", experiment.views4Callers,
                             static_cast<DWORD>(std::size(experiment.views4Callers)), ini.c_str());
    GetPrivateProfileStringW(L"experiment", L"views4_borrow", L"", experiment.views4Borrow,
                             static_cast<DWORD>(std::size(experiment.views4Borrow)), ini.c_str());
    experiment.views4ShowCopies = GetPrivateProfileIntW(L"experiment", L"views4_show_copies", 0, ini.c_str()) != 0;
    experiment.views4EyesOnlyAdvance =
        GetPrivateProfileIntW(L"experiment", L"views4_eyes_only_advance", 0, ini.c_str()) != 0;
    experiment.views4Compose = experiment.views4 && !experiment.views4ShowCopies &&
                               GetPrivateProfileIntW(L"experiment", L"views4_compose", 0, ini.c_str()) != 0;
    experiment.views4NoMask =
        experiment.views4 &&
        GetPrivateProfileIntW(L"experiment", L"views4_mask", experiment.views4Compose ? 0 : 1, ini.c_str()) == 0;
    experiment.views4EyeCamera =
        GetPrivateProfileIntW(L"experiment", L"views4_focus_eye_camera", 0, ini.c_str()) != 0;
    experiment.passCensus = GetPrivateProfileIntW(L"experiment", L"pass_census", 0, ini.c_str()) != 0;
    experiment.gpuFrameTiming = GetPrivateProfileIntW(L"experiment", L"gpu_frame_timing", 0, ini.c_str()) != 0;
    experiment.passCensusFrame = std::max(1u, GetPrivateProfileIntW(L"experiment", L"pass_census_frame", 1500, ini.c_str()));
    experiment.views4FocusCull = GetPrivateProfileIntW(L"experiment", L"views4_focus_cull", 0, ini.c_str()) != 0;
    experiment.views4FocusCullMargin =
        std::clamp(GetPrivateProfileIntW(L"experiment", L"views4_focus_cull_margin_percent", 25, ini.c_str()), 0u, 200u) /
        100.0;
    experiment.uavBarriers = GetPrivateProfileIntW(L"experiment", L"pass_census_uav_barriers", 0, ini.c_str()) != 0;
    experiment.views4ShadowView1Plain =
        GetPrivateProfileIntW(L"experiment", L"views4_shadow_view1_plain", 0, ini.c_str()) != 0;
    experiment.views4ScreenSizeLegacy =
        GetPrivateProfileIntW(L"experiment", L"views4_screen_size_legacy", 0, ini.c_str()) != 0;
    {
        wchar_t watch[512] = {};
        GetPrivateProfileStringW(L"experiment", L"pass_census_watch", L"", watch, 512, ini.c_str());
        std::snprintf(experiment.passCensusWatch, sizeof(experiment.passCensusWatch), "%ls", watch);
    }
    experiment.dred = GetPrivateProfileIntW(L"experiment", L"dred", 0, ini.c_str()) != 0;  // diagnosis only: costs GPU time
    experiment.d3d12Debug = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) != 0;
    experiment.gpuValidation = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) >= 2;
    experiment.pixCaptureFrame = GetPrivateProfileIntW(L"experiment", L"pix_capture_frame", 0, ini.c_str());
    experiment.quad = _wcsicmp(mode, L"quad") == 0;
    experiment.twoLoops = experiment.quad || _wcsicmp(mode, L"two_loops") == 0;
    experiment.focusInB = experiment.quad || GetPrivateProfileIntW(L"experiment", L"b_focus", 0, ini.c_str()) != 0;
    const LiveSettings settings = read_live(ini.c_str());
    experiment.focusFraction = settings.focusFraction;
    experiment.focusFractionY = settings.focusFractionY;
    experiment.feather = settings.feather;
    experiment.cornerRadius = settings.cornerRadius;
    experiment.border = settings.border;
    experiment.borderAtLaunch = settings.border;
    std::memcpy(experiment.borderColor, settings.borderColor, sizeof(experiment.borderColor));
    experiment.gaze = settings.gaze;
    experiment.fixedOffsetY = settings.offset.fixed;
    experiment.gazeOffsetY = settings.offset.gaze;
    live.ini = ini;
    live.restartKeys = restart_keys(ini.c_str(), experiment.views4);
    live.menuPlacement = read_menu_placement(ini.c_str());
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &attributes))
        live.written = attributes.ftLastWriteTime;
    experiment.captureEverySeconds = GetPrivateProfileIntW(L"quad", L"capture_every_s", 0, ini.c_str());
    experiment.probes = GetPrivateProfileIntW(L"experiment", L"probes", 1, ini.c_str()) != 0;
    experiment.batchWatch = GetPrivateProfileIntW(L"experiment", L"batch_watch", 1, ini.c_str()) != 0;
    experiment.hangWatchSeconds = GetPrivateProfileIntW(L"experiment", L"hang_watch_s", 30, ini.c_str());
    // Above 100 supersamples (in quad mode this is the outer view's resolution).
    const UINT eyePercent = std::clamp(GetPrivateProfileIntW(L"experiment", L"eye_scale", 100, ini.c_str()), 10u, 200u);
    experiment.eyeScale = eyePercent / 100.0;
    // views4: periphery and focus resolution each on their own. This sizes ACE's target, the most
    // either can be live.
    const Views4Sizes sizes = views4_sizes(ini.c_str(), experiment.focusFraction, experiment.focusFractionY);
    const FovCrop launchCrop = experiment.views4 ? settings.crop : FovCrop{};
    experiment.set_crop(launchCrop);
    helmetSettings = experiment.views4Compose ? settings.helmet : HelmetSettings{};  // before any session
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
};
Gaze gaze;
constexpr XrDuration kGazeHoldNs = 500'000'000;  // keep the last gaze this long, then recentre

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
        gaze.bindings.clear();
        gaze.profileSuggested.clear();
        gaze.suggested = gaze.attached = false;
        gaze.appSyncs = 0;
        gaze.space = XR_NULL_HANDLE;
    }
    if ((!experiment.quad && !experiment.views4Compose) || !next.CreateActionSet)
        return;
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "acequadview_gaze");
    strcpy_s(setInfo.localizedActionSetName, "ace-quadview gaze");
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
    strcpy_s(toggleInfo.localizedActionName, "ace-quadview settings menu");
    XrActionCreateInfo navigateInfo{XR_TYPE_ACTION_CREATE_INFO};
    navigateInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strcpy_s(navigateInfo.actionName, "settings_navigate");
    strcpy_s(navigateInfo.localizedActionName, "ace-quadview settings navigate");
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
        logf("input: ACE suggests %u binding(s) for %s", suggested->countSuggestedBindings,
             path_name(instance, suggested->interactionProfile).c_str());
    const auto own = suggested ? gaze.bindings.find(suggested->interactionProfile) : gaze.bindings.end();
    if (!gaze.set || own == gaze.bindings.end())
        return next.SuggestInteractionProfileBindings(instance, suggested);
    std::vector<XrActionSuggestedBinding> bindings(suggested->suggestedBindings,
                                                   suggested->suggestedBindings + suggested->countSuggestedBindings);
    bindings.insert(bindings.end(), own->second.begin(), own->second.end());
    XrInteractionProfileSuggestedBinding merged = *suggested;
    merged.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    merged.suggestedBindings = bindings.data();
    XrResult r = next.SuggestInteractionProfileBindings(instance, &merged);
    logf("input: added %zu layer binding(s) to ACE's %u, result=%d", own->second.size(),
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
    logf("gaze: attached with ACE's %u action set(s), result=%d", info->countActionSets, r);
    if (XR_SUCCEEDED(r)) {
        gaze.attached = true;
        return r;
    }
    return next.AttachSessionActionSets(session, info);
}

XrResult XRAPI_CALL layerSyncActions(XrSession session, const XrActionsSyncInfo* info) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.attached || !info)
        return next.SyncActions(session, info);
    ++gaze.appSyncs;
    std::vector<XrActiveActionSet> sets(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    sets.push_back({gaze.set, XR_NULL_PATH});
    XrActionsSyncInfo merged = *info;
    merged.countActiveActionSets = static_cast<uint32_t>(sets.size());
    merged.activeActionSets = sets.data();
    const XrResult r = next.SyncActions(session, &merged);
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
    XrFovf v4LoggedAceFov[2] = {};  // ACE's submitted FOV, as last logged against v4SubmitFov
    bool v4Composed = false;
    int captures = 0;
    ULONGLONG lastCapture = 0;
    bool failureLogged = false;
};
Quad quad;

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
    return experiment.views4 && experiment.borderAtLaunch && !experiment.views4ShowCopies && !experiment.views4Compose;
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
    const bool recorded = views4_latest_frame(frame);  // ACE's rendered FOV, else the located one
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
    if (!gpu.valid && !threads.valid)
        return;
    char gpuText[192] = "GPU load unknown", cpuText[256] = "threads unknown", shown[256] = "";
    if (gpu.valid)
        std::snprintf(gpuText, sizeof(gpuText), "GPU %.0f%% busy (%ls engine; ACE %.0f%%): %.1f ms of GPU per ACE frame",
                      gpu.busy, gpu.engine.c_str(), gpu.ace, gpu.busy / 100 * frameMs);
    if (threads.valid)
        std::snprintf(cpuText, sizeof(cpuText),
                      "busiest ACE thread %.0f%% of a core (last: %u%s%ls%s), render thread %.0f%%, all threads %.0f%% "
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
    char end[128], locate[128], compose[128];
    line(cpu.endFrame, &cpu.endFrameBelow, end, sizeof(end));
    line(cpu.locate, &cpu.locateBelow, locate, sizeof(locate));
    line(cpu.compose, nullptr, compose, sizeof(compose));
    logf("views4 cpu: ms per call over the last %llu frames: xrEndFrame %s; xrLocateViews %s; compose %s",
         static_cast<unsigned long long>(kCpuReportFrames), end, locate, compose);
    const double frameMs = report_pacing();
    gpu_frame_timer_report();  // its own samples: it logs while the overlay collects too
    report_load(frameMs);
}

bool is_ace_process() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return n > 0 && n < MAX_PATH && _wcsicmp(name ? name + 1 : path, L"AssettoCorsaEVO.exe") == 0;
}

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
    const double widthScale = experiment.views4 ? layout.eye_width() : experiment.eyeScale;
    const double heightScale = experiment.views4 ? layout.eye_height() : experiment.eyeScale;
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
    XrResult r = next.CreateSession(instance, info, session);
    logf("xrCreateSession result=%d bindingType=%d", r, binding ? binding->type : 0);
    if (XR_SUCCEEDED(r)) {
        hang_watch_arm(experiment.hangWatchSeconds);
        if (experiment.probes && probe_start()) {
            if (experiment.doubleRender)
                double_render_start();
            if (experiment.views3)
                views3_watch();
        }
        if (experiment.batchWatch)
            batch_watch_start();
        if (!experiment.probes)
            log_line(experiment.views3 ? "engine probes and D3D12 trace off (probes=0); views3 cannot report the view mode"
                                       : "engine probes and D3D12 trace off (probes=0)");
        if (binding && binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto* d3d12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding);
            if (experiment.probes)
                d3d12_trace_start(d3d12->device);
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
            if (experiment.views4) {  // the whole GPU's and ACE's threads' load (the overlay, the log)
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

void ini_worker_sync();  // (below)
void helmet_layer_release();  // (below)

XrResult XRAPI_CALL layerDestroySession(XrSession session) {
    ini_worker_sync();  // the menu's and hotkeys' last changes reach the ini
    menu_shutdown();  // its swapchain and space belong to the session
    osd_shutdown();
    probe_stop();
    batch_watch_stop();
    dred_stop();
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
        // The helmet's layer goes with the session; its pieces load again into the next one's compositor.
        helmet_layer_release();
        quad.helmetLayerFailed = false;
        quad.helmetBandsLogged.clear();
        for (int p = 0; p < 3; ++p) {
            quad.helmetPiecesTried[p].clear();
            quad.helmetPiecesWritten[p] = {};
        }
        quad.helmetHinted.clear();
    }
    gpu_frame_timer_stop();
    gpu_load_stop();
    cpu_load_stop();
    {
        std::lock_guard guard(gaze.mutex);  // the session's spaces and attachment go with it
        gaze.space = XR_NULL_HANDLE;
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
            if (experiment.views4 && pacing.wait.size() < 4 * kCpuReportFrames) {
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
    if (XR_SUCCEEDED(r) && (experiment.quad || experiment.views4Compose) &&
        (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
        quad_adopt_chain(session, info, *swapchain);
    return r;
}

XrResult XRAPI_CALL layerEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* countOutput,
                                                  XrSwapchainImageBaseHeader* images) {
    XrResult r = next.EnumerateSwapchainImages(swapchain, capacity, countOutput, images);
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
        quad.compositor.load_helmet_piece(static_cast<HelmetPiece>(p), exists ? full.wstring() : std::wstring());
        if (!paths[p]->empty() && !exists && first)
            logf("helmet: no %ls (that piece is left out until it appears)", full.c_str());
    }
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
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_SUCCEEDED(r) && count)
        r = next.EnumerateSwapchainImages(quad.helmetChain, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
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
    for (const auto& image : images)
        quad.helmetImages.push_back(image.texture);
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
    quad.helmetSide = 0;
    quad.helmetQuadCount = 0;
    quad.helmetDrawn = quad.helmetResize = false;
}

// The helmet layer for this frame (under quad.mutex, on ACE's queue after the composition): the quad
// placed at the helmet's distance and height, sized to cover both eyes' uncropped views (`full`), and the
// pieces drawn into it. False if it was not drawn (nothing is submitted then).
bool helmet_layer_draw(const HelmetSettings& h, const EyeComposition (&eyes)[2], const float (&full)[2][4],
                       double visorOffset) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(quad.helmetChain, &acquire, &index)) || index >= quad.helmetImages.size())
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
    helmet_tan_half(h, quad.compositor.helmet_aspect(), imageX, imageY);
    // Only the bands the pieces draw on, with the visor where it is now.
    const std::vector<HelmetBand> bands =
        helmet_bands(quad.compositor.helmet_rows(visorOffset, h.visorBlend), imageY, coverY);
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
                    quad.compositor.draw_helmet_layer(quad.helmetImages[index], draw);
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
    const double top = quad.compositor.helmet_edge(HelmetPiece::Top), bottom = quad.compositor.helmet_edge(HelmetPiece::Bottom);
    const std::wstring key = quad.helmetPiecesTried[1] + L"|" + quad.helmetPiecesTried[2] + L"|" +
                             std::to_wstring(quad.helmetPiecesWritten[1].time_since_epoch().count()) + L"|" +
                             std::to_wstring(quad.helmetPiecesWritten[2].time_since_epoch().count()) + L"|" +
                             std::to_wstring(h.size) + L"|" + std::to_wstring(h.height);
    if (key == quad.helmetHinted || (top < 0 && bottom < 0))
        return;
    quad.helmetHinted = key;
    int cropTop = 0, cropBottom = 0;
    helmet_crop_hint(h, quad.compositor.helmet_aspect(), top, bottom < 0 ? -1 : 1 - bottom, full[2], full[3], cropTop,
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
    if (quad.compositor.read_light(rgb)) {
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
        quad.compositor.request_light();
        quad.helmetLightLogAt = now + 10000;
    }
}

// views4: ACE's eye image holds the outer views as views 0-1 and the focus views as views 2-3 of the
// layout (the focus cameras: focus_percent of each eye's tangent extent around its forward axis). Composes
// both eyes into the output chain at native size, on ACE's queue, before ACE releases the image.
void views4_compose() {
    quad.v4Composed = false;
    if (!quad.outChain || quad.appAcquired >= quad.appImages.size() || !quad.v4FovValid)
        return;
    ID3D12Resource* app = quad.appImages[quad.appAcquired];
    // What this frame's views were rendered with (ACE's own eye projections, the focus boxes and
    // turns, views4.h); before the first focus cameras are filled, the located FOV, unturned.
    Views4Frame frame{};
    const bool recorded = views4_take_frame(frame);
    XrQuaternionf turns[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    EyeComposition eyes[2] = {};
    const HelmetSettings& helmet = helmetSettings;
    if (helmet.on)
        helmet_pieces(helmet);
    const double visorOffset = helmet.on ? visor_offset(helmet, visor_step(helmet)) : 0;
    // The pieces go into their own layer (the scene keeps ACE's FOV); only if that layer failed, into the
    // scene image over the whole eye. The built-in look draws its lip inside the rendered view.
    const bool pieces = helmet.on && quad.compositor.has_helmet_pieces();
    const bool piecesInScene = pieces && quad.helmetLayerFailed;
    float fullView[2][4] = {};
    for (int e = 0; e < 2; ++e) {
        if (recorded) {
            std::memcpy(eyes[e].outerTan, frame.outerTan[e], sizeof(eyes[e].outerTan));
            std::memcpy(eyes[e].focusTan, frame.focusTan[e], sizeof(eyes[e].focusTan));
            turns[e] = frame.turn[e];
        } else {
            fov_tangents(quad.v4Fov[e], eyes[e].outerTan);
            double hw, hh;
            focus_extents(eyes[e].outerTan, experiment.box_x(), experiment.box_y(), hw, hh);
            const float focusTan[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                       static_cast<float>(-hh)};
            std::memcpy(eyes[e].focusTan, focusTan, sizeof(focusTan));
        }
        // The output covers the outer view's tangents, so that is the FOV it is submitted with.
        quad.v4SubmitFov[e] = tangents_fov(eyes[e].outerTan);
        // The focus camera is the eye's turned by turns[e] (in the eye's frame): a direction in the
        // outer view's frame is in the focus view's frame after the inverse turn.
        to_matrix(qconj(turns[e]), eyes[e].rotation);
        // Outer source = view e, focus source = view e + 2, wherever the layout puts them.
        const int w = static_cast<int>(quad.appW), h = static_cast<int>(quad.appH);
        // At the sizes this frame was rendered with (live resolution); launch sizes until the first record.
        const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
        views4_uv(layout, e, w, h, eyes[e].outerSource);
        views4_uv(layout, e + 2, w, h, eyes[e].focusSource);
        eyes[e].feather = static_cast<float>(experiment.feather);
        // The outline (debug_border) is drawn by the compositor along the focus area's exact edge,
        // turned and rounded as it is.
        eyes[e].border = experiment.border ? 0.008f : 0.0f;
        std::memcpy(eyes[e].borderColor, experiment.borderColor, sizeof(eyes[e].borderColor));
        eyes[e].cornerRadius = static_cast<float>(experiment.cornerRadius);
        float full[4];
        uncrop_tangents(eyes[e].outerTan, quad.v4Crop, e, full);
        std::memcpy(fullView[e], full, sizeof(full));
        if (pieces) {
            // The crop hint once ACE renders the located view (not its own projection on the loading screens).
            float located[4];
            fov_tangents(quad.v4Fov[e], located);
            bool rendersLocated = true;
            for (int i = 0; i < 4; ++i)
                rendersLocated &= std::fabs(eyes[e].outerTan[i] - located[i]) < 0.01f * std::max(1.0f, std::fabs(located[i]));
            if (e == 0 && rendersLocated)
                helmet_crop_log(helmet, full);
        }
        if (helmet.on && !pieces) {
            // The built-in look, inside the rendered view: its lip reaches in from the opening's edge.
            const HelmetShape shape = helmet_shape(helmet, full, eyes[e].outerTan);
            std::memcpy(eyes[e].helmetOpening, shape.opening, sizeof(shape.opening));
            eyes[e].helmetRim = shape.rim;
            eyes[e].helmetRadius = shape.radius;
            eyes[e].helmetLight = static_cast<float>(helmet.light) / 100.0f;
            eyes[e].helmet = helmet.debugLight ? 3 : 1;
        }
        if (piecesInScene) {
            // Fallback (no layer of their own): the output covers the whole uncropped eye at native size;
            // the pieces fill what ACE did not render.
            std::memcpy(eyes[e].viewTan, full, sizeof(full));
            quad.v4SubmitFov[e] = tangents_fov(full);
            quad.v4Extent[e] = {static_cast<int32_t>(quad.nativeW), static_cast<int32_t>(quad.nativeH)};
            const HelmetShape shape = helmet_shape(helmet, full, eyes[e].outerTan);
            std::memcpy(eyes[e].helmetOpening, shape.opening, sizeof(shape.opening));
            eyes[e].helmetRim = shape.rim;
            eyes[e].helmetRadius = shape.radius;
            eyes[e].helmetLight = static_cast<float>(helmet.light) / 100.0f;
            eyes[e].helmet = helmet.debugLight ? 3 : 2;
            // The pieces, fixed to the head: the eye's turn against the head, then the images' height.
            const double tilt = helmet.height * kHelmetDegrees;
            const XrQuaternionf up{static_cast<float>(std::sin(tilt / 2)), 0, 0, static_cast<float>(std::cos(tilt / 2))};
            to_matrix(qmul(qconj(up), quad.v4EyeTurn[e]), eyes[e].helmetRotation);
            double tx, ty;
            helmet_tan_half(helmet, quad.compositor.helmet_aspect(), tx, ty);
            eyes[e].helmetTanHalf[0] = static_cast<float>(tx);
            eyes[e].helmetTanHalf[1] = static_cast<float>(ty);
            const XrVector3f offset = rotate(qconj(up), quad.v4EyeOffset[e]);  // in the images' frame
            const float eyeOffset[3] = {offset.x, offset.y, offset.z};
            helmet_parallax(helmet, eyeOffset, eyes[e].helmetShift);
            eyes[e].visorOffset = static_cast<float>(visorOffset);
            eyes[e].visorBlend = helmet.visorBlend;
            continue;
        }
        // The FOV crop: the output keeps native density over the cropped FOV (views4_output_extent).
        const XrExtent2Di extent = quad.v4Extent[e] = views4_output_extent();
        eyes[e].outputSize[0] = static_cast<float>(extent.width) / static_cast<float>(quad.nativeW);
        eyes[e].outputSize[1] = static_cast<float>(extent.height) / static_cast<float>(quad.nativeH);
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
    if (quad.helmetChain && helmet_layer_side(helmet) != quad.helmetSide)
        quad.helmetResize = true;  // [helmet] detail changed: the next submission makes it again
    quad.helmetDrawn = pieces && !piecesInScene && quad.helmetChain && !quad.helmetResize &&
                       helmet_layer_draw(helmet, eyes, fullView, visorOffset);
    if (quad.composed++ == 0)
        logf("views4: first composed frame: eye 0 outer tangents %.3f/%.3f/%.3f/%.3f, focus +-%.3f x +-%.3f, output "
             "%ux%u per eye",
             eyes[0].outerTan[0], eyes[0].outerTan[1], eyes[0].outerTan[2], eyes[0].outerTan[3], eyes[0].focusTan[1],
             eyes[0].focusTan[2], quad.nativeW, quad.nativeH);
}

// Frame A's image is copied before the runtime gets it back; frame B's stays acquired (so this
// layer may still read it) until B's xrEndFrame has composed it.
XrResult XRAPI_CALL layerReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
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

// views4 composing: ACE draws each eye's hidden-area mesh into every view, so the focus views got the
// lens mask's black corners in their own frame (composition run 2). views4_mask=0 answers the hidden
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
        views4_note_views(views, std::min(*countOutput, capacity));
        if (views4_outline_on() && *countOutput >= 2 && capacity >= 2) {
            std::lock_guard guard(v4outline.mutex);
            v4outline.fov[0] = views[0].fov;
            v4outline.fov[1] = views[1].fov;
            v4outline.fovValid = true;
        }
        if (experiment.views4Compose && *countOutput >= 2 && capacity >= 2) {
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
            float tan[2][2] = {};
            bool valid[2] = {};
            XrVector3f direction{0, 0, -1};
            const bool gazing = experiment.gaze && gaze_direction(session, info->space, info->displayTime, direction);
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
            views4_set_gaze(tan, valid, gazing);
        }
    }
    const bool narrowThisFrame = experiment.focusOnly || (experiment.twoLoops && experiment.focusInB && loops.inB);
    if (narrowThisFrame && capacity > 0 && XR_SUCCEEDED(r)) {
        XrVector3f direction{0, 0, -1};
        const bool gazing = experiment.quad && experiment.gaze &&
                            gaze_direction(session, info->space, info->displayTime, direction);
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

// Logs ACE's own submitted FOV next to the one the output is submitted with, once and on a change.
void log_submitted_fov(uint32_t eye, const XrFovf& ace, const XrFovf& composed) {
    XrFovf& last = quad.v4LoggedAceFov[eye];
    const auto same = [](const XrFovf& a, const XrFovf& b, float eps) {
        return std::fabs(a.angleLeft - b.angleLeft) < eps && std::fabs(a.angleRight - b.angleRight) < eps &&
               std::fabs(a.angleUp - b.angleUp) < eps && std::fabs(a.angleDown - b.angleDown) < eps;
    };
    if (same(ace, last, 0.002f))
        return;
    last = ace;
    logf("views4: eye %u ACE submits FOV %.4f/%.4f/%.4f/%.4f, output submitted with %.4f/%.4f/%.4f/%.4f (its "
         "rendered projection)%s",
         eye, ace.angleLeft, ace.angleRight, ace.angleUp, ace.angleDown, composed.angleLeft, composed.angleRight,
         composed.angleUp, composed.angleDown, same(ace, composed, 0.002f) ? "" : "  <-- DIFFERENT");
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
                if (quad.helmetDrawn) {
                    helmetCount = quad.helmetQuadCount;
                    std::copy(quad.helmetQuads, quad.helmetQuads + helmetCount, helmetLayers);
                    helmetAfter = static_cast<int>(i);
                }
                quad.helmetDrawn = false;
                if (quad.helmetResize)
                    helmet_layer_release();  // nothing of it submitted this frame
                if (helmetSettings.on && quad.compositor.has_helmet_pieces())
                    helmet_layer_resources(session);  // drawn from the next composition on
                continue;
            }
        }
        Views4Frame latest{};
        views4_latest_frame(latest);  // the rendered sizes (live resolution)
        const Views4Layout layout = views4_with_live(experiment.views4Layout, latest.live, latest.width, latest.height);
        bool aceLayout = true;
        for (uint32_t v = 0; v < proj->viewCount; ++v)
            aceLayout &= copy[v].subImage.imageRect.offset.x == static_cast<int32_t>(v) * copy[v].subImage.imageRect.extent.width;
        if (!aceLayout)
            continue;
        for (uint32_t v = 0; v < proj->viewCount; ++v) {
            // ACE submits halves of its target (W/2 x H each): point them at view v (or v + 2).
            XrRect2Di& rect = copy[v].subImage.imageRect;
            const ViewRect view = views4_rect(layout, static_cast<int>(v + (experiment.views4ShowCopies ? 2 : 0)),
                                              2 * rect.extent.width, rect.extent.height);
            rect.offset.x = view.left;
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
    views4_set_live_layout(views4_live(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch));
    const Views4Fit fit = views4_fit(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch);
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
    if (experiment.views4Compose) {
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
                 views4_dlss_held() ? " (DLSS is on: the views keep their launch sizes until the next launch)" : "");
    }
    experiment.focusFraction = l.focusFraction;
    experiment.focusFractionY = l.focusFractionY;
    experiment.feather = l.feather;
    experiment.cornerRadius = l.cornerRadius;
    std::memcpy(experiment.borderColor, l.borderColor, sizeof(experiment.borderColor));
    experiment.border = l.border;
    experiment.gaze = l.gaze;
    experiment.fixedOffsetY = l.offset.fixed;
    experiment.gazeOffsetY = l.offset.gaze;
    bool gazeAvailable;
    {
        std::lock_guard guard(gaze.mutex);
        gazeAvailable = gaze.set != XR_NULL_HANDLE;
    }
    if (experiment.views4 && !experiment.views4EyeCamera)
        views4_set_live(experiment.box_x(l.focusFraction),
                        experiment.box_y(l.focusFractionY > 0 ? l.focusFractionY : l.focusFraction),
                        1.0 + l.feather + experiment.views4FocusCullMargin,
                        l.gaze && experiment.views4Compose);
    logf("live: focus box %.0f%% x %.0f%%, feather %.0f%%, corners %.0f%%, outline %d, gaze %d, offset fixed %d deg, "
         "gaze %d deg",
         l.focusFraction * 100, experiment.focus_y() * 100, l.feather * 100, l.cornerRadius * 100, l.border ? 1 : 0,
         l.gaze ? 1 : 0, l.offset.fixed, l.offset.gaze);
    if (experiment.views4)
        live_resolution(l);
    if (l.gaze && !gazeAvailable && (experiment.quad || experiment.views4Compose))
        log_line("live: gaze turned on, but eye tracking was off at launch; it starts at the next launch");
    if (l.border && !experiment.borderAtLaunch && experiment.views4 && !experiment.views4Compose)
        log_line("live: debug_border without views4_compose starts at the next launch");
}

struct IniSnapshot;
void menu_values_changed(const IniSnapshot& s);  // the settings menu shows the ini's values (below)

// Cheeky's runtime gets [cheeky]'s live settings and, in QVFR, the quad view roles (cheeky_settings.h).
void cheeky_apply_settings(const CheekySettings& c, bool gazeOn) {
    if (cheeky_running())
        cheeky_apply(cheeky_set_lines(c, experiment.views4, gazeOn));
}
void cheeky_apply_ini() {
    if (!live.ini.empty())
        cheeky_apply_settings(read_cheeky_settings(live.ini.c_str()), read_live(live.ini.c_str()).gaze);
}
void menu_placement_changed(const MenuPlacement& placement);  // the launcher moved the settings menu's panel (below)
void osd_placement_changed(const OsdPlacement& placement, const CheekySettings& cheeky);  // the overlay (below)
void osd_placement_changed();

constexpr int64_t kLiveCheckMs = 250;

// Everything the live reload takes from the ini, read in one go (on the ini worker).
struct IniSnapshot {
    LiveSettings live;
    CheekySettings cheeky;
    MenuValues menu;
    MenuPlacement menuPlacement;
    OsdPlacement osd;
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
    if (s->restartKeys != live.restartKeys) {
        logf("live: %ls-> %ls(applies at the next launch)", live.restartKeys.c_str(), s->restartKeys.c_str());
        live.restartKeys = s->restartKeys;
    }
}

// ---- Settings menu ---------------------------------------------------------------------------
// In the headset (settings_menu.h): Ctrl+Alt+arrows while ACE has focus, or a controller's left
// stick click (open, close) and right stick (select, change). Changes are written to the ini the
// layer reloads from and to the launcher's (ACE_QV_SETTINGS), so they apply as the launcher's do.
// Shown as a head-locked quad layer on top of whatever this layer submits, where the launcher's
// Headset menu pane places it ([menu], live); hidden kMenuHideMs after the last input.
struct Menu {
    SettingsMenu model;
    bool visible = false, dirty = true, failed = false, uploaded = false;
    int64_t hideAt = 0;  // QPC
    std::wstring settingsIni;
    XrSpace view = XR_NULL_HANDLE;
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> images;
    PanelUpload upload;
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    int heldKey = -1;  // keyboard repeat: the held arrow (index into kMenuKeys)
    int64_t keyRepeat = 0;
    int stickDir = -1;  // controller repeat: the held stick direction
    int64_t stickRepeat = 0;
    bool togglePrevious = false;
    bool profileLogged = false;
    uint64_t lastWrite = 0;  // ini_write_later's number for the latest step
};
Menu menu;
constexpr int64_t kMenuHideMs = 6000, kRepeatDelayMs = 400, kRepeatMs = 90;
constexpr int kMenuKeys[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
constexpr MenuKey kMenuKeyOf[] = {MenuKey::Up, MenuKey::Down, MenuKey::Left, MenuKey::Right};

int64_t qpc_ms(int64_t ms) {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency * ms / 1000;
}

bool ace_focused() {
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

void menu_open(int64_t now) {
    if (menu.settingsIni.empty()) {  // the launcher's own ini, where the next launch reads them
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"ACE_QV_SETTINGS", path, MAX_PATH);
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
        {
            std::lock_guard guard(quad.mutex);
            menu.model.launch.helmetPieces = quad.compositor.has_helmet_pieces();
        }
        menu.model.launch.nativePpd = native_center_ppd();  // takes quad.mutex itself
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
    if (!menu_key(menu.model, key))
        return;
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
                     cheeky = menu.model.cheeky, qvfr = menu.model.launch.qvfr] {
        write_menu_values(ini.c_str(), values, qvfr);
        if (cheeky.available)
            write_cheeky_settings(ini.c_str(), cheeky.values, false);
        if (!settings.empty()) {
            write_menu_values(settings.c_str(), values, qvfr);
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

void menu_keyboard(int64_t now) {
    int down = -1;
    if (ace_focused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
        for (int i = 0; i < 4 && down < 0; ++i)
            if (GetAsyncKeyState(kMenuKeys[i]) & 0x8000)
                down = i;
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
    if (menu.chain)
        return true;
    if (menu.failed)
        return false;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    {
        std::lock_guard guard(quad.mutex);
        device = quad.device;
        queue = quad.queue;
    }
    if (!device || !queue)
        return false;  // not a D3D12 session (yet)
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
    uint32_t count = 0;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &menu.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &menu.chain);
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(menu.chain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_SUCCEEDED(r) && count)
        r = next.EnumerateSwapchainImages(menu.chain, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r) || !count || !menu.upload.initialize(device, queue)) {
        logf("settings menu: panel setup failed (%d); the menu still changes settings", r);
        menu.failed = true;
        return false;
    }
    for (const auto& image : images)
        menu.images.push_back(image.texture);
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
    if (experiment.views4 && views4_latest_frame(frame))
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
    const std::wstring hint = L"Ctrl+Alt+arrows, or right stick: select and change · left stick click: close";
    if (menu.model.launch.nativePpd <= 0)  // known once ACE has rendered a frame
        menu.model.launch.nativePpd = native_center_ppd();
    menu.model.launch.dlssHeld = experiment.views4 && views4_dlss_held();
    const std::vector<uint32_t> pixels = draw_menu_panel(title, menu_resolution(menu.model), menu_stats(menu.model),
                                                         menu_lines(menu.model),
                                                         menu_notes(menu.model), hint, menu_subtitle(menu.model));
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100'000'000;
    if (XR_FAILED(next.AcquireSwapchainImage(menu.chain, nullptr, &index)))
        return;
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(menu.chain, &wait)) && index < menu.images.size() &&
                    menu.upload.upload(menu.images[index], pixels);
    next.ReleaseSwapchainImage(menu.chain, nullptr);
    if (ok) {
        menu.uploaded = true;
        menu.dirty = false;
    }
}

const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now);  // the overlay's card (below)

// This frame's layers with the performance overlay and the menu panel on top while they are shown;
// `info` itself otherwise.
const XrFrameEndInfo* menu_frame(XrSession session, const XrFrameEndInfo* info, XrFrameEndInfo& storage,
                                 std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    if (live.ini.empty() || (!experiment.views4 && !experiment.quad))
        return info;
    // For the launcher's Center PPD: once, when ACE has rendered.
    static bool ppdReported = false;
    if (!ppdReported && endFrames % 90 == 0) {
        if (const double ppd = native_center_ppd(); ppd > 0) {
            wchar_t text[32];
            swprintf_s(text, L"%.2f", ppd);
            runtime_info(L"native_center_ppd", text);
            logf("settings menu: %.2f px per degree at the centre at 100%% density", ppd);
            ppdReported = true;
        }
    }
    const int64_t now = qpc_now();
    menu_keyboard(now);
    menu_controllers(session, now);
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
    menu.upload.shutdown();
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
    std::vector<ID3D12Resource*> images;
    PanelUpload upload;
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
constexpr size_t kOsdHistory = 150;


void osd_waited(XrDuration period) { osd.periodNs.store(period, std::memory_order_relaxed); }

bool osd_shown(OsdSummary& out) {
    if (!osd.placement.enabled || !osd.shownValid)
        return false;
    out = osd.shown;
    return true;
}

void osd_wait_returned(double waitMs) {
    if (!osd.placement.enabled)
        return;
    osd.waitReturnQpc = qpc_now();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    osd.waitReturnCycles = cycles;
    osd.waitThread = GetCurrentThreadId();
    osd.waitMs = waitMs;
}

void osd_measure() {
    if (!osd.placement.enabled)
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
        osd.frames.push_back(frame);
        osd.history.push_back(frame.intervalMs);
        if (osd.history.size() > kOsdHistory)
            osd.history.pop_front();
    }
    osd.lastEnd = now;
    osd.thread = thread;
    osd.lastCycles = cycles;
}

void osd_place() {
    const OsdPlacement& p = osd.placement;
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

// Ctrl+Alt+V while ACE has focus steps the helmet's visor (closed, half, open, closed), written to the ini
// the layer reloads from and to the launcher's, as the headset menu writes it.
bool visorHotkeyHeld = false;
void visor_hotkey() {
    const bool held = ace_focused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                      (GetAsyncKeyState('V') & 0x8000);
    if (held && !visorHotkeyHeld && experiment.views4Compose) {
        HelmetSettings h;
        {
            std::lock_guard guard(quad.mutex);  // at once; the ini on the worker
            helmetSettings.visor = static_cast<VisorPosition>((static_cast<int>(helmetSettings.visor) + 1) % 3);
            h = helmetSettings;
        }
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"ACE_QV_SETTINGS", path, MAX_PATH);
        const std::wstring settings = n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
        ini_write_later([ini = live.ini, settings, h] {
            write_helmet_settings(ini.c_str(), h);
            if (!settings.empty())
                write_helmet_settings(settings.c_str(), h);
        });
        logf("helmet: visor %ls (Ctrl+Alt+V)", kVisorPositionNames[static_cast<int>(h.visor)]);
    }
    visorHotkeyHeld = held;
}

void osd_hotkey() {
    const bool held = ace_focused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                      (GetAsyncKeyState('P') & 0x8000);
    if (held && !osd.hotkeyHeld) {
        OsdPlacement placement = osd.placement;
        placement.enabled = !placement.enabled;
        wchar_t path[MAX_PATH] = {};  // the launcher's own ini: the next launch starts with it
        const DWORD n = GetEnvironmentVariableW(L"ACE_QV_SETTINGS", path, MAX_PATH);
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
    if (osd.chain)
        return true;
    if (osd.failed)
        return false;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    {
        std::lock_guard guard(quad.mutex);
        device = quad.device;
        queue = quad.queue;
    }
    if (!device || !queue)
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
    uint32_t count = 0;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &osd.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &osd.chain);
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(osd.chain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_SUCCEEDED(r) && count)
        r = next.EnumerateSwapchainImages(osd.chain, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r) || !count || !osd.upload.initialize(device, queue, kOsdWidth, kOsdHeight)) {
        logf("performance overlay: card setup failed (%d)", r);
        osd.failed = true;
        return false;
    }
    for (const auto& image : images)
        osd.images.push_back(image.texture);
    XrCompositionLayerQuad& l = osd.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    l.space = osd.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = osd.chain;
    l.subImage.imageRect = {{0, 0}, {kOsdWidth, kOsdHeight}};
    osd_place();
    if (experiment.views4)
        gpu_frame_timer_start(device, queue);
    // VRAM: this process's use of the adapter's local memory against the budget Windows gives it.
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&osd.adapter))))
            osd.adapter = nullptr;
        factory->Release();
    }
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
        in.gpuAcePercent = gpu.ace;
    }
    if (const CpuLoad threads = cpu_load_window(samples, 0); threads.valid) {
        in.cpuBusiestPercent = threads.busiestPercent;
        in.cpuBusiestName = threads.busiestName;
        in.cpuBusiestIsRender = threads.busiestThread == osd.thread;
    }
    return in;
}

const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now) {
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
                            osd.upload.upload(osd.images[index], pixels);
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
    osd.upload.shutdown();
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

XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info) {
    hang_watch_frame();
    cpu_load_render_thread(GetCurrentThreadId());
    if (experiment.views4 && cpu.msPerTick > 0) {
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
    batch_watch_frame_end();
    if (experiment.views4) {
        views4_frame_end();
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
                     "layer, e.g. VectorXR Frame Generation?); turn it off for ACE");
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

#define ACEQV_INTERCEPT(fn)                                              \
    if (std::strcmp(name, "xr" #fn) == 0) {                              \
        *function = reinterpret_cast<PFN_xrVoidFunction>(&layer##fn);    \
        return XR_SUCCESS;                                               \
    }

XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    ACEQV_INTERCEPT(GetInstanceProcAddr)
    if (next.EndFrame) {
        ACEQV_INTERCEPT(EnumerateViewConfigurationViews)
        ACEQV_INTERCEPT(CreateSession)
        ACEQV_INTERCEPT(DestroySession)
        ACEQV_INTERCEPT(BeginSession)
        ACEQV_INTERCEPT(WaitFrame)
        ACEQV_INTERCEPT(BeginFrame)
        ACEQV_INTERCEPT(CreateSwapchain)
        ACEQV_INTERCEPT(EnumerateSwapchainImages)
        ACEQV_INTERCEPT(LocateViews)
        ACEQV_INTERCEPT(EndFrame)
        ACEQV_INTERCEPT(AcquireSwapchainImage)
        ACEQV_INTERCEPT(ReleaseSwapchainImage)
        ACEQV_INTERCEPT(SuggestInteractionProfileBindings)
        ACEQV_INTERCEPT(AttachSessionActionSets)
        ACEQV_INTERCEPT(SyncActions)
        if (next.GetVisibilityMaskKHR)
            ACEQV_INTERCEPT(GetVisibilityMaskKHR)
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
    if (!vectorxrBelow && GetModuleHandleW(L"XR_APILAYER_DIENERTECH_VECTORXR.dll"))
        log_line("VectorXR is loaded above this layer: its frame generation would see ACE's four-view image; move "
                 "this layer above VectorXR (machine-wide registration, then VectorXR's layer order)");
}

XrResult XRAPI_CALL layerCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!info || !layerInfo || !layerInfo->nextInfo || !instance ||
        std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = layerInfo->nextInfo->next;
    PFN_xrGetInstanceProcAddr gipa = layerInfo->nextInfo->nextGetInstanceProcAddr;
    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    // Registered as an implicit layer, so it loads into every OpenXR app: stay silent and
    // pass-through everywhere except Assetto Corsa EVO.
    if (!is_ace_process()) {
        next = Dispatch{gipa};
        return r;
    }
    load_experiment();
    // Where the launcher writes live settings while this ACE runs (live_reload).
    runtime_info(L"live_ini", live.ini);
    runtime_info(L"live_pid", std::to_wstring(GetCurrentProcessId()));
    log_layer_order(layerInfo->nextInfo->next);
    logf("xrCreateApiLayerInstance app=%s engine=%s extensions=%u result=%d", info->applicationInfo.applicationName,
         info->applicationInfo.engineName, info->enabledExtensionCount, r);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        logf("  extension %s", info->enabledExtensionNames[i]);
    if (XR_FAILED(r))
        return r;
    // Seconds before ACE initialises NGX, so Cheeky's hooks see its DLSS features created.
    cheeky_start(cheeky_config(live.ini.c_str(), layer_directory()));
    {  // for the launcher's Foveated DLSS pane
        static const wchar_t* const kStatus[] = {L"off", L"missing", L"refused", L"running"};
        runtime_info(L"cheeky_status", kStatus[static_cast<int>(cheeky_status())]);
        if (cheeky_running())
            runtime_info(L"cheeky_version", cheeky_version());
    }
    cheeky_apply_ini();
    // Before ACE creates its D3D12 device and renderer, so renderer init sees the patched mode.
    if (experiment.dred)
        dred_enable();
    if (experiment.d3d12Debug)
        d3d12_debug_enable(experiment.gpuValidation);
    if (experiment.pixCaptureFrame)  // before ACE creates its D3D12 device
        pix_capture_load(experiment.pixCaptureFrame);
    if (experiment.views3)
        views3_patch();
    if (experiment.views4) {
        views4_set_borrow(experiment.views4Borrow);
        views4_set_layout(experiment.views4Layout);
        views4_set_screen_size_legacy(experiment.views4ScreenSizeLegacy);
        // The cull box: the focus box plus its feather band plus a gaze-latency allowance.
        views4_set_focus_cull(experiment.views4FocusCull, 1.0 + experiment.feather + experiment.views4FocusCullMargin,
                              experiment.gaze && experiment.views4Compose);  // gaze turns only when composing
    }
    if (experiment.views4 &&
        !views4_start(experiment.views4Callers, experiment.views4EyesOnlyAdvance,
                      experiment.views4EyeCamera ? 0.0 : experiment.box_x(), experiment.box_y()))
        experiment.views4 = false;  // no doubled eye width or rect rewrite without the engine side
    if (experiment.views4 && experiment.views4ShadowView1Plain)
        views4_shadow_view1_plain();
    Dispatch d;
    d.GetInstanceProcAddr = gipa;
    bool ok = true;
#define ACEQV_LOAD(fn) ok &= XR_SUCCEEDED(gipa(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction*>(&d.fn))) && d.fn;
    ACEQV_LOAD(EnumerateViewConfigurationViews)
    ACEQV_LOAD(CreateSession)
    ACEQV_LOAD(DestroySession)
    ACEQV_LOAD(BeginSession)
    ACEQV_LOAD(WaitFrame)
    ACEQV_LOAD(BeginFrame)
    ACEQV_LOAD(CreateSwapchain)
    ACEQV_LOAD(EnumerateSwapchainImages)
    ACEQV_LOAD(LocateViews)
    ACEQV_LOAD(EndFrame)
    ACEQV_LOAD(AcquireSwapchainImage)
    ACEQV_LOAD(WaitSwapchainImage)
    ACEQV_LOAD(ReleaseSwapchainImage)
    ACEQV_LOAD(StringToPath)
    ACEQV_LOAD(CreateActionSet)
    ACEQV_LOAD(CreateAction)
    ACEQV_LOAD(SuggestInteractionProfileBindings)
    ACEQV_LOAD(AttachSessionActionSets)
    ACEQV_LOAD(SyncActions)
    ACEQV_LOAD(CreateActionSpace)
    ACEQV_LOAD(LocateSpace)
    ACEQV_LOAD(CreateReferenceSpace)
    ACEQV_LOAD(DestroySpace)
    ACEQV_LOAD(DestroySwapchain)
    ACEQV_LOAD(GetActionStateBoolean)
    ACEQV_LOAD(GetActionStateVector2f)
    ACEQV_LOAD(GetCurrentInteractionProfile)
    ACEQV_LOAD(PathToString)
#undef ACEQV_LOAD
    gipa(*instance, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&d.GetVisibilityMaskKHR));
    if (ok && experiment.views4) {  // time what runs below this layer (report_cpu_times)
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
}  // namespace aceqv

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !layerName || !request || std::strcmp(layerName, aceqv::kLayerName) != 0 ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_API_VERSION_1_0;
    request->getInstanceProcAddr = aceqv::layerGetInstanceProcAddr;
    request->createApiLayerInstance = aceqv::layerCreateApiLayerInstance;
    if (aceqv::is_ace_process())
        aceqv::log_line("Loader negotiated API layer interface v1 (ace-quadview probe build)");
    return XR_SUCCESS;
}

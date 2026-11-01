// XR_APILAYER_ACE_quadview: logs what Assetto Corsa EVO asks for (views, swapchains, frame
// submission) and starts the log-only engine probes (probe/probe.cpp) at xrCreateSession. With
// mode=off nothing alters rendering; the experiments and the quad pipeline are selected in
// ace-quadview.ini.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "experiment/double_render.h"
#include "experiment/views3.h"
#include "experiment/views4.h"
#include "log.h"
#include "probe/d3d12_trace.h"
#include "probe/gpu_frame_timer.h"
#include "probe/pass_census.h"
#include "probe/pix_capture.h"
#include "quad/focus_turn.h"
#include "probe/dred.h"
#include "probe/probe.h"
#include "quad/compositor.h"
#include "quad/focus_outline.h"
#include "xr.h"

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
    bool uavBarriers = false;              // diagnostic: UAV barrier after every dispatch (probe/pass_census.h)
    bool gpuFrameTiming = false;           // views4: timestamps time ACE's frame on its queue (probe/gpu_frame_timer.h)
    UINT passCensusFrame = 1500;           // the census counts xrEndFrames from this one, for 10 frames
    char passCensusWatch[512] = "";        // the census logs stacks of these pipelines' draws and dispatches (comma-separated)
    bool dred = false;                     // D3D12 DRED report on device removal (default on with views4)
    bool d3d12Debug = false;               // D3D12 debug layer, errors into the log (slow; diagnosis only)
    bool gpuValidation = false;            // d3d12_debug=2: plus GPU-based validation (very slow)
    UINT pixCaptureFrame = 0;              // PIX GPU capture from this xrEndFrame (probe/pix_capture.h), 0 = off
    bool twoLoops = false;        // round 7: two complete ACE frames per display frame
    bool focusInB = false;        // round 8: two_loops frame B renders the focus FOV (A keeps the full FOV)
    double eyeScale = 1.0;        // round 8: scales the recommended eye size ACE sees (linear)
    Views4Layout views4Layout;    // views4: outer views at periphery_scale, focus views at focus_density x focus_percent
    double focusFraction = 0.35;  // of the view's tangent-space width and height
    bool quad = false;            // quad pipeline: two_loops + b_focus, composited into one native-size image
    double feather = 0.10;        // quad: blend band, as a fraction of the focus box
    bool border = false;          // quad: debug green outline on the focus box (views4: on the outer views)
    bool borderMagenta = false;   // views4 outline colour: magenta instead of green
    bool gaze = true;             // quad: the focus box follows XR_EXT_eye_gaze_interaction
    UINT captureEverySeconds = 0; // quad debug: saves A, B and the output (left eye) as PNGs, 0 = off
    bool probes = true;           // engine probes and D3D12 trace (log-only reverse-engineering aids)
};
Experiment experiment;
bool focusLogged = false;

void load_experiment() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&load_experiment), &self);
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(self, module, MAX_PATH);
    // A launcher passes its per-launch settings in ACE_QV_INI; otherwise the ini next to the DLL.
    std::wstring ini = (std::filesystem::path(module).parent_path() / L"ace-quadview.ini").wstring();
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
    {
        wchar_t watch[512] = {};
        GetPrivateProfileStringW(L"experiment", L"pass_census_watch", L"", watch, 512, ini.c_str());
        std::snprintf(experiment.passCensusWatch, sizeof(experiment.passCensusWatch), "%ls", watch);
    }
    experiment.dred = GetPrivateProfileIntW(L"experiment", L"dred", experiment.views4 ? 1 : 0, ini.c_str()) != 0;
    experiment.d3d12Debug = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) != 0;
    experiment.gpuValidation = GetPrivateProfileIntW(L"experiment", L"d3d12_debug", 0, ini.c_str()) >= 2;
    experiment.pixCaptureFrame = GetPrivateProfileIntW(L"experiment", L"pix_capture_frame", 0, ini.c_str());
    experiment.quad = _wcsicmp(mode, L"quad") == 0;
    experiment.twoLoops = experiment.quad || _wcsicmp(mode, L"two_loops") == 0;
    const UINT percent = GetPrivateProfileIntW(L"experiment", L"focus_percent", 35, ini.c_str());
    experiment.focusFraction = std::clamp(percent, 10u, 90u) / 100.0;
    experiment.focusInB = experiment.quad || GetPrivateProfileIntW(L"experiment", L"b_focus", 0, ini.c_str()) != 0;
    const UINT featherPercent = std::clamp(GetPrivateProfileIntW(L"quad", L"feather_percent", 10, ini.c_str()), 0u, 50u);
    experiment.feather = featherPercent / 100.0;
    experiment.border = GetPrivateProfileIntW(L"quad", L"debug_border", 0, ini.c_str()) != 0;
    wchar_t borderColor[16] = {};
    GetPrivateProfileStringW(L"quad", L"debug_border_color", L"green", borderColor, 16, ini.c_str());
    experiment.borderMagenta = _wcsicmp(borderColor, L"magenta") == 0;  // views4 outline; quad mode stays green
    experiment.gaze = GetPrivateProfileIntW(L"quad", L"gaze", 1, ini.c_str()) != 0;
    experiment.captureEverySeconds = GetPrivateProfileIntW(L"quad", L"capture_every_s", 0, ini.c_str());
    experiment.probes = GetPrivateProfileIntW(L"experiment", L"probes", 1, ini.c_str()) != 0;
    // Above 100 supersamples (in quad mode this is the outer view's resolution).
    const UINT eyePercent = std::clamp(GetPrivateProfileIntW(L"experiment", L"eye_scale", 100, ini.c_str()), 25u, 200u);
    experiment.eyeScale = eyePercent / 100.0;
    // views4: periphery and focus resolution each on their own. Unset, both follow eye_scale (equal
    // quarters: periphery = eye_scale, focus density = eye_scale / focus_percent).
    const UINT peripheryPercent = GetPrivateProfileIntW(L"experiment", L"periphery_scale", 0, ini.c_str());
    const UINT focusDensityPercent = GetPrivateProfileIntW(L"experiment", L"focus_density", 0, ini.c_str());
    const double outer = peripheryPercent ? std::clamp(peripheryPercent, 10u, 200u) / 100.0 : experiment.eyeScale;
    // The focus views' linear size against the native eye: density times the box's share of the FOV.
    const double focus = focusDensityPercent
                             ? std::clamp(focusDensityPercent, 25u, 400u) / 100.0 * experiment.focusFraction
                             : experiment.eyeScale;
    experiment.views4Layout = views4_layout(outer, focus);
    logf("experiment: mode=%ls focus_percent=%u b_focus=%d eye_scale=%u feather_percent=%u debug_border=%d gaze=%d "
         "(from %ls)",
         mode, percent, experiment.focusInB ? 1 : 0, eyePercent, featherPercent, experiment.border ? 1 : 0,
         experiment.gaze ? 1 : 0, ini.c_str());
    if (experiment.views4)
        logf("experiment: views4 periphery %.0f%% of native, focus density %.0f%% of native (focus views %.0f%% of the "
             "native eye size)%s",
             outer * 100, focus / experiment.focusFraction * 100, focus * 100,
             peripheryPercent || focusDensityPercent ? "" : " (from eye_scale)");
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
struct Gaze {
    std::mutex mutex;
    XrInstance instance = XR_NULL_HANDLE;
    XrActionSet set = XR_NULL_HANDLE;
    XrAction action = XR_NULL_HANDLE;
    XrPath profile = XR_NULL_PATH, pose = XR_NULL_PATH;
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

void gaze_init(XrInstance instance) {
    if ((!experiment.quad && !experiment.views4Compose) || !experiment.gaze || !next.CreateActionSet)
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
        return;
    }
    log_line("gaze: action set created");
}

XrResult XRAPI_CALL layerSuggestInteractionProfileBindings(XrInstance instance,
                                                          const XrInteractionProfileSuggestedBinding* suggested) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.set || !suggested || suggested->interactionProfile != gaze.profile)
        return next.SuggestInteractionProfileBindings(instance, suggested);
    std::vector<XrActionSuggestedBinding> bindings(suggested->suggestedBindings,
                                                   suggested->suggestedBindings + suggested->countSuggestedBindings);
    bindings.push_back({gaze.action, gaze.pose});
    XrInteractionProfileSuggestedBinding merged = *suggested;
    merged.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    merged.suggestedBindings = bindings.data();
    XrResult r = next.SuggestInteractionProfileBindings(instance, &merged);
    logf("gaze: added to ACE's eye gaze bindings (%u of its own), result=%d", suggested->countSuggestedBindings, r);
    if (XR_FAILED(r))
        return next.SuggestInteractionProfileBindings(instance, suggested);
    gaze.suggested = true;
    return r;
}

XrResult XRAPI_CALL layerAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.set || !info)
        return next.AttachSessionActionSets(session, info);
    if (!gaze.suggested) {  // ACE suggested nothing for the eye gaze profile: suggest the layer's binding alone
        const XrActionSuggestedBinding binding{gaze.action, gaze.pose};
        XrInteractionProfileSuggestedBinding own{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        own.interactionProfile = gaze.profile;
        own.countSuggestedBindings = 1;
        own.suggestedBindings = &binding;
        gaze.suggested = XR_SUCCEEDED(next.SuggestInteractionProfileBindings(gaze.instance, &own));
        logf("gaze: suggested the layer's own eye gaze binding, %s", gaze.suggested ? "ok" : "failed");
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
    bool v4FovValid = false;
    XrFovf v4SubmitFov[2] = {};  // the FOV the composed output covers (views4_compose), for views4_submit
    XrFovf v4LoggedAceFov[2] = {};  // ACE's submitted FOV, as last logged against v4SubmitFov
    bool v4Composed = false;
    FocusOutline v4Outline;
    bool v4OutlineReady = false;
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
    return experiment.views4 && experiment.border && !experiment.views4ShowCopies && !experiment.views4Compose;
}

void views4_outline_draw(XrSwapchain swapchain) {
    std::lock_guard guard(v4outline.mutex);
    if (swapchain != v4outline.chain || !v4outline.fovValid || v4outline.failed)
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
        if (experiment.borderMagenta) {
            v4outline.rgba[0] = 1;
            v4outline.rgba[1] = 0;
            v4outline.rgba[2] = 1;
        }
        logf("views4: focus outline on, %s", experiment.borderMagenta ? "magenta" : "green");
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
    for (int e = 0; e < 2; ++e) {
        const ViewRect v = views4_rect(experiment.views4Layout, e, static_cast<int>(desc.Width), static_cast<int>(desc.Height));
        float tan[4];
        if (recorded)
            std::memcpy(tan, frame.outerTan[e], sizeof(tan));
        else
            fov_tangents(v4outline.fov[e], tan);
        const double l = tan[0], r = tan[1], u = tan[2], d = tan[3];
        double hw, hh;
        focus_extents(tan, experiment.focusFraction, hw, hh);
        if (!(r > l && u > d))
            return;
        boxes.push_back(OutlineBox{v.left + static_cast<int>((-hw - l) / (r - l) * v.width),
                                   static_cast<int>((u - hh) / (u - d) * v.height),
                                   v.left + static_cast<int>((hw - l) / (r - l) * v.width),
                                   static_cast<int>((u + hh) / (u - d) * v.height)});
        thickness = std::max(2, v.width / 300);
    }
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
void turn_view(XrView& view, double fraction, double tanX, double tanY) {
    const double l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
    const double d = std::tan(view.fov.angleDown), u = std::tan(view.fov.angleUp);
    const double hw = (r - l) * fraction / 2, hh = (u - d) * fraction / 2;
    view.pose.orientation = qmul(view.pose.orientation, focus_turn(view.fov, fraction, tanX, tanY));
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
    gpu_frame_timer_report();
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
        if (experiment.probes && probe_start()) {
            if (experiment.doubleRender)
                double_render_start();
            if (experiment.views3)
                views3_watch();
        }
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
        } else if (experiment.quad) {
            log_line("quad: session is not D3D12 (a layer above converted it?); quad pipeline off");
        }
    }
    return r;
}

XrResult XRAPI_CALL layerDestroySession(XrSession session) {
    probe_stop();
    dred_stop();
    {
        std::lock_guard guard(quad.mutex);
        quad.v4Outline.shutdown();
        quad.v4OutlineReady = quad.v4Composed = quad.v4FovValid = false;
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
    }
    gpu_frame_timer_stop();
    {
        std::lock_guard guard(gaze.mutex);  // the session's spaces and attachment go with it
        gaze.space = XR_NULL_HANDLE;
        gaze.attached = false;
        gaze.appSyncs = 0;
    }
    log_line("xrDestroySession");
    return next.DestroySession(session);
}

XrResult XRAPI_CALL layerWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    if (!experiment.twoLoops)
        return next.WaitFrame(session, info, state);
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
    for (int e = 0; e < 2; ++e) {
        if (recorded) {
            std::memcpy(eyes[e].outerTan, frame.outerTan[e], sizeof(eyes[e].outerTan));
            std::memcpy(eyes[e].focusTan, frame.focusTan[e], sizeof(eyes[e].focusTan));
            turns[e] = frame.turn[e];
        } else {
            fov_tangents(quad.v4Fov[e], eyes[e].outerTan);
            double hw, hh;
            focus_extents(eyes[e].outerTan, experiment.focusFraction, hw, hh);
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
        views4_uv(experiment.views4Layout, e, w, h, eyes[e].outerSource);
        views4_uv(experiment.views4Layout, e + 2, w, h, eyes[e].focusSource);
        eyes[e].feather = static_cast<float>(experiment.feather);
        eyes[e].border = 0;  // the outline is drawn below, in the chosen colour
    }
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
    if (ok && experiment.border) {
        if (!quad.v4OutlineReady)
            quad.v4OutlineReady = quad.v4Outline.initialize(quad.device, quad.queue, quad.format);
        if (quad.v4OutlineReady) {
            std::vector<OutlineBox> boxes;
            const int w = static_cast<int>(quad.nativeW), h = static_cast<int>(quad.nativeH);
            for (int e = 0; e < 2; ++e) {
                // The turned focus view's corners, as tangents of the outer view: their bounding box.
                const float l = eyes[e].outerTan[0], r = eyes[e].outerTan[1], u = eyes[e].outerTan[2],
                            d = eyes[e].outerTan[3], hw = eyes[e].focusTan[1], hh = eyes[e].focusTan[2];
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                for (const float sx : {-hw, hw})
                    for (const float sy : {-hh, hh}) {
                        const XrVector3f c = rotate(turns[e], {sx, sy, -1});
                        const float tx = c.x / -c.z, ty = c.y / -c.z;
                        x0 = std::min(x0, tx), x1 = std::max(x1, tx), y0 = std::min(y0, ty), y1 = std::max(y1, ty);
                    }
                boxes.push_back(OutlineBox{e * w + static_cast<int>((x0 - l) / (r - l) * w),
                                           static_cast<int>((u - y1) / (u - d) * h),
                                           e * w + static_cast<int>((x1 - l) / (r - l) * w),
                                           static_cast<int>((u - y0) / (u - d) * h)});
            }
            const float magenta[4] = {1, 0, 1, 1}, green[4] = {0, 1, 0, 1};
            quad.v4Outline.draw(out, boxes, std::max(2, w / 300), experiment.borderMagenta ? magenta : green);
        }
    }
    if (ok && quad_capture_due())
        views4_capture(app, out);
    next.ReleaseSwapchainImage(quad.outChain, &release);
    if (!ok) {
        quad_fail("views4 compose failed");
        return;
    }
    quad.v4Composed = true;
    quad.fallbackRun = 0;
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
                quad.v4FovValid = true;
            }
            // Gaze: where the gaze ray points in each eye's frame (canted displays rotate each eye), as
            // tangents; the focus cameras turn towards it, clamped against the FOV ACE renders with
            // (fill_focus_cameras). Untracked gaze recentres after kGazeHoldNs.
            float tan[2][2] = {};
            bool valid[2] = {};
            XrVector3f direction{0, 0, -1};
            const bool gazing = experiment.gaze && gaze_direction(session, info->space, info->displayTime, direction);
            if (gazing)
                for (int e = 0; e < 2; ++e) {
                    const XrVector3f d = rotate(qconj(views[e].pose.orientation), direction);
                    valid[e] = d.z < -0.1f;
                    if (valid[e]) {
                        tan[e][0] = d.x / -d.z;
                        tan[e][1] = d.y / -d.z;
                    }
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
            if (experiment.quad) {
                turn_view(views[i], experiment.focusFraction, tanX, tanY);
            } else {
                narrow(views[i].fov.angleLeft, views[i].fov.angleRight, experiment.focusFraction, tanX);
                narrow(views[i].fov.angleDown, views[i].fov.angleUp, experiment.focusFraction, tanY);
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
                    copy[v].subImage.imageRect = {{static_cast<int32_t>(v * quad.nativeW), 0},
                                                  {static_cast<int32_t>(quad.nativeW), static_cast<int32_t>(quad.nativeH)}};
                    copy[v].subImage.imageArrayIndex = 0;
                }
                projections.push_back(*proj);
                projections.back().views = copy.data();
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
                continue;
            }
        }
        bool aceLayout = true;
        for (uint32_t v = 0; v < proj->viewCount; ++v)
            aceLayout &= copy[v].subImage.imageRect.offset.x == static_cast<int32_t>(v) * copy[v].subImage.imageRect.extent.width;
        if (!aceLayout)
            continue;
        for (uint32_t v = 0; v < proj->viewCount; ++v) {
            // ACE submits halves of its target (W/2 x H each): point them at view v (or v + 2).
            XrRect2Di& rect = copy[v].subImage.imageRect;
            const ViewRect view = views4_rect(experiment.views4Layout, static_cast<int>(v + (experiment.views4ShowCopies ? 2 : 0)),
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
    XrFrameEndInfo submitted = *info;
    submitted.layers = layers.data();
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

XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info) {
    if (!cpu.realEndFrame)
        return end_frame(session, info);
    const int64_t t0 = qpc_now();
    const XrResult r = end_frame(session, info);
    cpu.endFrame.add(qpc_now() - t0);
    if (endFrames % kCpuReportFrames == 0)
        report_cpu_times();
    return r;
}

XrResult end_frame(XrSession session, const XrFrameEndInfo* info) {
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
    log_layer_order(layerInfo->nextInfo->next);
    logf("xrCreateApiLayerInstance app=%s engine=%s extensions=%u result=%d", info->applicationInfo.applicationName,
         info->applicationInfo.engineName, info->enabledExtensionCount, r);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        logf("  extension %s", info->enabledExtensionNames[i]);
    if (XR_FAILED(r))
        return r;
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
        // The cull box: the focus box plus its feather band plus a gaze-latency allowance.
        views4_set_focus_cull(experiment.views4FocusCull, 1.0 + experiment.feather + experiment.views4FocusCullMargin,
                              experiment.gaze && experiment.views4Compose);  // gaze turns only when composing
    }
    if (experiment.views4 &&
        !views4_start(experiment.views4Callers, experiment.views4EyesOnlyAdvance,
                      experiment.views4EyeCamera ? 0.0 : experiment.focusFraction))
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

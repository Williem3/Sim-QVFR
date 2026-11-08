// The layer's state: the structs behind its globals (the dispatch table, the experiment, gaze, the quad
// pipeline, the menu, the overlay, ...), the constants more than one file uses, and the globals' extern
// declarations. The globals themselves are in globals.cpp, in one file so they're built and torn down in
// the order they always were.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dxgi1_4.h>

#include "cheeky/cheeky_settings.h"
#include "games/adapter.h"
#include "input/hotkeys.h"
#include "probe/cpu_load.h"
#include "quad/compositor.h"
#include "quad/d3d11_compositor.h"
#include "quad/focus_offset.h"
#include "quad/focus_outline.h"
#include "quad/focus_shape.h"
#include "quad/fov_crop.h"
#include "quad/helmet.h"
#include "quad/menu_panel.h"
#include "quad/osd.h"
#include "quad/qvfr_optimizer.h"
#include "quad/session_graphics.h"
#include "quad/settings_menu.h"
#include "xr.h"

namespace simqvfr::layer {

inline constexpr char kLayerName[] = "XR_APILAYER_SIM_qvfr";

inline constexpr uint64_t kDetailedFrames = 4;

inline constexpr uint64_t kSampleEveryFrames = 1800;

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
    int cpuProfileSlowMs = 0;  // cpu_profile_slow_ms: a slow frame is longer than this (0: 1.25x the median)
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

// views4: the outer and focus views' sizes, linear scales of the native eye (views4_layout.h), from
// periphery_scale and focus_density and the focus box. Unset, both follow eye_scale (equal quarters:
// periphery = eye_scale, focus density = eye_scale / focus_percent).
struct Views4Sizes {
    double outer = 1, density = 1, focus = 1, focusHeight = 1;
    bool fromEyeScale = false;
};

struct Live {
    std::wstring ini;          // the ini load_experiment read
    std::wstring restartKeys;  // restart_keys when last read
    MenuPlacement menuPlacement;  // the settings menu's place ([menu]) when last read
    FILETIME written{};        // the ini's last write time when last read
};

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

inline constexpr uint64_t kArmAfterFrames = 30;

inline constexpr uint64_t kMaxFallbackRun = 90;  // ~1-2 s of display frames

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

// The gaze the focus box aims at (ACE's render thread only): held while the tracked gaze stays within
// gaze_deadzone of it, so a fixation keeps the focus cameras still, and moved to the gaze at once when it
// leaves (a saccade). Held in the located space, so a fixation on the scene holds through a head turn.
struct GazeHold {
    XrVector3f direction{0, 0, -1};
    bool held = false;
    std::atomic<uint64_t> samples = 0, moves = 0;  // for the log (gaze_hold_report)
};

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

inline constexpr uint64_t kCpuReportFrames = 900;

// views4 frame pacing (render thread): ACE's xrEndFrame-to-xrEndFrame interval, the time it spends in
// xrWaitFrame (the runtime holding it to the display) and the display period the runtime predicts.
// Logged with the CPU times as a "views4 pacing:" line: frames that miss their display slot wait a whole
// extra period, so the frame rate falls below the refresh even with headroom on average.
struct Pacing {
    std::vector<float> interval, wait;  // ms
    int64_t lastEnd = 0;
    double periodMs = 0;
};

// The pacing line, and resets: ACE's frame interval and its time in and outside xrWaitFrame (p50, p90),
// and how many frames took one display period, two, three or more (rounded to the nearest). Returns the
// mean frame interval (0 without frames).
// The latest report's numbers, for the launcher's Diagnostics pane (write_session_summary).
struct SessionSummary {
    double fps = 0, frameP50 = 0, frameP90 = 0, latePercent = 0, periodMs = 0;
    double cpuMs = -1, gpuMs = -1, gpuBusy = -1, cores = -1;  // the render thread's CPU and the whole GPU per frame
    std::vector<ThreadLoad> threads;
};

// A write queued for the ini worker (ini_write_later). MenuStep: a settings menu step, which writes every value the
// menu holds, so a newer step queued right behind an older one stands in for it (held keys don't queue for seconds).
enum class IniWrite { Once, MenuStep };

// The helmet's part of one composition (under quad.mutex; for the D3D11 compositor, under v4d3d11.mutex too), for
// either compositor: its pieces looked at, the visor's slide, and whether the pieces go into their own layer or
// (that layer failed) into the scene image over the whole eye. The built-in look draws its lip inside the
// rendered view.
struct V4Helmet {
    bool pieces = false, piecesInScene = false;
    double visorOffset = 0;
    float fullView[2][4] = {};  // each eye's uncropped view (the helmet layer covers both)
};

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

// The ini worker: every read and write of the ini while the game runs, off the render thread (each
// Get/WritePrivateProfileString opens the file; a menu step wrote and re-read about a hundred keys, up to
// ~130 ms in xrEndFrame). It runs the queued writes (the menu's, the hotkeys'), then, every kLiveCheckMs or
// when woken, reads a snapshot when the ini's write time changed; live_reload applies the latest. Writes go
// through ini_put (only values that differ: each WritePrivateProfileString rewrites the whole ini, ~5 ms).
struct QueuedIniWrite {
    uint64_t number;  // ini_write_later's
    IniWrite kind;
    std::function<void()> write;
};

struct IniWorker {
    std::mutex mutex;
    std::condition_variable wake, settled;
    std::deque<QueuedIniWrite> writes;
    std::wstring ini;     // the ini watched
    FILETIME written{};   // its write time when last read
    std::optional<IniSnapshot> ready;  // the latest read, not yet applied
    uint64_t requested = 0, done = 0;  // passes asked for (ini_worker_sync) and finished
    uint64_t writesQueued = 0, writesDone = 0;  // ini_write_later's writes, numbered from 1
    bool poke = false, started = false;
};

// The settings menu's QVFR optimizer (quad/qvfr_optimizer.h; run below, after the overlay): the frames
// the overlay's measurement gives it while it runs (osd_measure).
struct Optimize {
    QvfrOptimizer optimizer;
    std::vector<OsdFrame> frames;  // since the last window
    int64_t nextWindow = 0;
    bool shown = false;  // the menu shows its detail (from the first run until the menu reopens)
    std::wstring note;   // why it did not start, shown instead of its own status
};

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

inline constexpr int64_t kMenuHideMs = 6000, kRepeatDelayMs = 400, kRepeatMs = 90;

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

// A notice in the performance overlay's card, for the first kNoticeMs of the session's frames, when the layer
// could not do what it was asked: views4 refused, an ACE build it does not know (the game updated). Without
// it the game would just look as it always does, and nothing would say why. Straight ahead, a little low.
struct Notice {
    std::wstring title;
    std::vector<std::wstring> lines;
    int64_t until = 0;  // set at its first frame
    bool uploaded = false;
};

// The capture's start and end in the headset: the settings menu's panel shows for a few seconds with a line
// under its title (QVFR and Two-loop: the panel is theirs; with mode=off only the log says).
struct ProfileNotice {
    int64_t until = 0;
    bool wasRunning = false;
};

// The globals (globals.cpp).
extern Dispatch next;
extern std::mutex chainMutex;
extern std::map<XrSwapchain, std::pair<uint32_t, uint32_t>> chainSizes;
extern Experiment experiment;
extern games::GameAdapter* game;
extern bool focusLogged;
extern HelmetSettings helmetSettings;
extern Live live;
extern TwoLoops loops;
extern Gaze gaze;
extern HeadSpaces headSpaces;
extern GazeThread gazeThread;
extern GazeHold gazeHold;
extern Quad quad;
extern SessionGraphics sessionGraphics;
extern V4D3D11 v4d3d11;
extern Views4Outline v4outline;
extern uint64_t endFrames;
extern uint64_t locates;
extern CpuTimes cpu;
extern Pacing pacing;
extern SessionSummary summary;
extern std::string layersBelow;
extern IDXGIAdapter3* vramAdapter;
extern bool visibilityMaskLogged;
extern bool views4RectsLogged;
extern bool v4EyesOnlyLogged;
extern IniWorker iniWorker;
extern Optimize optimize;
extern Hotkeys hotkeys;
extern PadWatch padWatch;
extern Menu menu;
extern Osd osd;
extern Notice notice;
extern bool visorHotkeyHeld;
extern bool cpuProfileHotkeyHeld;
extern bool frameCaptureHotkeyHeld;
extern ProfileNotice profileNotice;
extern std::atomic<int> loaderNegotiations;
extern std::atomic<bool> instanceReachedLayer;

}  // namespace simqvfr::layer

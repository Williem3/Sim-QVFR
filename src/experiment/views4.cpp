#include "views4.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iterator>

#include <MinHook.h>
#include <intrin.h>

#include "../engine/ace_abi.h"
#include "../engine/code_patch.h"
#include "../engine/view3_stubs.h"
#include "../log.h"
#include "../probe/pass_census.h"
#include "../quad/focus_cull.h"
#include "../quad/focus_turn.h"
#include "../xr.h"

namespace aceqv {
namespace {

constexpr int kViews = 4;
constexpr uint64_t kReportEveryFrames = 900;

using ViewCountFn = int (*)(void* renderer);
using ViewportFn = void* (*)(void* renderer, void* out, const float* renderSize, int view);
using ViewRecordFn = void* (*)(void* renderer, int view);
using PassSetupFn = void (*)(void* renderer, void* pass, int view);

ViewCountFn viewCountOriginal = nullptr;
ViewportFn viewportOriginal = nullptr;
ViewRecordFn viewRecordOriginal = nullptr;
PassSetupFn passSetupOriginal[std::size(ace::kPassSetupRvas)] = {};
uintptr_t base = 0;
bool started = false;

// Return addresses (call site + 5) that get 4 views; filled before the hooks go live.
uintptr_t allowed[64];
size_t allowedCount = 0;
bool allowAll = false;
bool drawLoops = false;  // "lighting" (or "draw"): the builder's lighting loop runs 4 views
// "depth" (or "draw"): the builder's ZPrepass/Velocity/Flags loop runs 4 views (allowlisted sites).
bool tileViews = false;  // "tiles": tiled light culling builds tile lists for 4 views
bool slotViews = false;  // "slots": views 2-3 get their own history slots (view 3's at a phantom renderer)
bool cameraViews = false;  // "cameras": views 2-3 are focus cameras with their own camera slots and records
Views4Layout layout;       // where the four views sit in the target (views4_set_layout; equal quarters by default)
bool volumetricViews = false;  // "fog": the volumetrics/atmosphere system has resources for 4 views
bool foldVolumetrics = false;  // "fogfold": views 2-3 still read the eyes' volumetric resources (diagnostic)
// The focus box's share of the eye's tangent width / height; changed live under turnLock (views4_set_live).
double focusFraction = 0.35, focusFractionY = 0.35;
uint64_t cameraPhantom2 = 0;  // C2: C2 + kCameraSlotBase is view 2's camera slot
uint64_t cameraPhantom = 0;   // C3: the same for view 3
float lastFocusProjection[2][16];
bool lastFocusValid[2] = {};

// Gaze and frame records. `gazeTan`/`gazeValid`: where each eye looks (tangents of its view), the
// latest from xrLocateViews. `lastTurn`: what the previous focus cameras used (their previous-frame
// matrices are turned by it). `frameRing`: what each filled frame was rendered with (Views4Frame),
// taken in the same order by the composition.
constexpr XrQuaternionf kNoTurn{0, 0, 0, 1};
SRWLOCK turnLock = SRWLOCK_INIT;
float gazeTan[2][2] = {};
bool gazeValid[2] = {};
XrQuaternionf lastTurn[2] = {kNoTurn, kNoTurn};
Views4Frame frameRing[8], lastTaken{}, lastPushed{};
uint64_t turnsPushed = 0, turnsTaken = 0, turnResyncs = 0, turnEmptyTakes = 0;
constexpr uint64_t kMaxTurnBacklog = 3;
// Rendered-vs-located check: the outer tangents last logged per eye (logged again on a change).
float loggedTan[2][4] = {};
std::atomic<uint64_t> fovMismatchFrames{0};

// Focus cull (views4_focus_cull): each filled frame's focus frusta in world space, widened by
// cullScale, under turnLock. The latest two fills are kept (cullFrusta[fill % 2]) in case recording
// of one frame overlaps filling the next. No frusta while gaze is on but no sample has arrived.
bool focusCull = false;
double cullScale = 1.0;
bool gazeExpected = false, gazeSampled = false;
struct CullFrusta {
    FocusFrustum view[2];  // views 2 and 3
    uint64_t fill = 0;     // turnsPushed when built, + 1 (0 = never)
};
CullFrusta cullFrusta[2];
uint64_t cullBuilt = 0, cullWaitingForGaze = 0;

// CPU time of fill_focus_cameras (the focus-cull measurement), reported with the views4 counters.
std::atomic<uint64_t> fillTicks{0}, fillMaxTicks{0}, fillCalls{0};
double msPerTick = 0;

// Every distinct viewCount caller, logged once.
SRWLOCK callersLock = SRWLOCK_INIT;
uintptr_t callersSeen[256];
size_t callersSeenCount = 0;

std::atomic<uint64_t> widened{0}, viewportCalls[kViews + 1], viewportMismatch{0}, recordsFolded{0};
std::atomic<uint64_t> passCalls[kViews + 1], passMismatch{0};
std::atomic<bool> passLogged{false};
uint64_t frames = 0;
std::atomic<bool> layoutLogged{false};

// Camera dump (to identify the slot matrices): the renderer, and the views ACE last located.
std::atomic<void*> lastRenderer{nullptr};
SRWLOCK viewsLock = SRWLOCK_INIT;
XrView lastViews[2] = {};
constexpr uint64_t kDumpFrames[] = {600, 1800};

// Pass snapshot (darkness bisection): one frame's pass-setup outputs per (setup, pass type, view),
// diffed view 1 vs 3 and 0 vs 2 at the frame's end. With views4_focus_eye_camera the focus views
// should equal their eyes, so whatever differs beyond the view index and viewport is per-view state.
constexpr uint64_t kSnapshotFrames[] = {1200, 2400};
constexpr size_t kPassSnapshotSize = 0x9c8;  // the builder fills the pass data up to +0x9c4
struct PassSnapshot {
    int setup, type, view;
    uint8_t data[kPassSnapshotSize];
};
// views4_borrow (diagnostic): dwords of the lighting pass (type 10) that views 2-3 take from their
// eye's pass, which the builder sets up just before them in the same frame.
constexpr int32_t kLightingPassType = 10;
size_t borrowOffsets[16];
size_t borrowCount = 0;
uint32_t borrowed[2][2][std::size(borrowOffsets)];  // [setup][eye][offset]
bool borrowValid[2][2] = {};
SRWLOCK snapshotLock = SRWLOCK_INIT;
std::atomic<bool> snapshotArmed{false};
PassSnapshot snapshots[32];
size_t snapshotCount = 0;

int mode_of(const void* renderer) {
    return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(renderer) + ace::kRendererActiveViewMode);
}

bool is_allowed(uintptr_t returnRva) {
    if (allowAll)
        return true;
    for (size_t i = 0; i < allowedCount; ++i)
        if (allowed[i] == returnRva)
            return true;
    return false;
}

void note_caller(uintptr_t returnRva, int mode, int returned) {
    AcquireSRWLockShared(&callersLock);
    bool known = false;
    for (size_t i = 0; i < callersSeenCount && !known; ++i)
        known = callersSeen[i] == returnRva;
    ReleaseSRWLockShared(&callersLock);
    if (known)
        return;
    AcquireSRWLockExclusive(&callersLock);
    for (size_t i = 0; i < callersSeenCount; ++i)
        if (callersSeen[i] == returnRva) {
            ReleaseSRWLockExclusive(&callersLock);
            return;
        }
    if (callersSeenCount < std::size(callersSeen))
        callersSeen[callersSeenCount++] = returnRva;
    ReleaseSRWLockExclusive(&callersLock);
    logf("views4: viewCount caller call_site=0x%llx mode=%d returned=%d", static_cast<unsigned long long>(returnRva - 5),
         mode, returned);
}

// The per-view vectors (sized for 4 at init) of view `view` from those of eye `eye`.
void copy_per_view(uint8_t* renderer, int eye, int view) {
    const auto copy = [&](std::ptrdiff_t offset, size_t size) {
        auto* const* bounds = reinterpret_cast<uint8_t* const*>(renderer + offset);
        if (static_cast<size_t>(bounds[1] - bounds[0]) >= size * kViews)
            std::memcpy(bounds[0] + view * size, bounds[0] + eye * size, size);
    };
    copy(ace::kPerViewMatrix, 0x40);
    copy(ace::kPerViewTanX, 4);
    copy(ace::kPerViewTanY, 4);
}

// Turns a slot's 4x4 matrix at `offset` (V <- V * R, or for the per-view inverse R^T * M).
void turn_matrix_at(uint8_t* p, const XrQuaternionf& turn, bool inverse) {
    float m[16];
    std::memcpy(m, p, sizeof(m));
    if (inverse)
        turn_inverse_view_matrix(m, turn);
    else
        turn_view_matrix(m, turn);
    std::memcpy(p, m, sizeof(m));
}

// Logs each eye's rendered tangents (from ACE's projection) next to the located FOV, once and again
// whenever they change by more than 0.5%; warns when they disagree, since everything the focus
// views and the composition do follows the rendered ones.
void check_rendered_fov(int eye, const float (&rendered)[4], const XrFovf& located) {
    float loc[4];
    fov_tangents(located, loc);
    bool changed = false, differs = false;
    for (int i = 0; i < 4; ++i) {
        changed |= std::fabs(rendered[i] - loggedTan[eye][i]) > 0.005f * std::max(1.0f, std::fabs(rendered[i]));
        differs |= std::fabs(rendered[i] - loc[i]) > 0.005f * std::max(1.0f, std::fabs(loc[i]));
    }
    if (differs)
        ++fovMismatchFrames;
    if (!changed)
        return;
    std::memcpy(loggedTan[eye], rendered, sizeof(loggedTan[eye]));
    logf("views4: eye %d rendered with tangents %.4f/%.4f/%.4f/%.4f (ACE's projection), located %.4f/%.4f/%.4f/%.4f%s",
         eye, rendered[0], rendered[1], rendered[2], rendered[3], loc[0], loc[1], loc[2], loc[3],
         differs ? "  <-- DIFFERENT: the focus views and the composition follow ACE's projection" : "");
}

// Focus cameras for views 2-3, written just before the builder copies the camera slots into the
// view records (the VR branch has filled the eyes' slots by then). Each focus view is its eye's
// slot with the projection replaced by a symmetric box of focusFraction of the tangent extent ACE
// actually renders the eye with (read back from its projection, not assumed from xrLocateViews),
// its camera turned towards the gaze, and its own previous-frame projection and turn. The frame's
// geometry goes to the composition as a Views4Frame.
void fill_focus_cameras(uint8_t* renderer) {
    XrView views[2];
    AcquireSRWLockShared(&viewsLock);
    std::memcpy(views, lastViews, sizeof(views));
    ReleaseSRWLockShared(&viewsLock);
    // The phantom renderer's fields the pass setup reads for views 2-3 (before anything can return:
    // those setups restore rdi from kPhantomRenderer).
    {
        auto* phantom = reinterpret_cast<uint8_t*>(cameraPhantom2);
        std::memcpy(phantom + ace::kRendererField8, renderer + ace::kRendererField8, sizeof(void*));
        std::memcpy(phantom + ace::kPhantomRenderer, &renderer, sizeof(void*));
    }
    float gaze[2][2];
    bool gazing[2];
    XrQuaternionf previousTurns[2];
    AcquireSRWLockShared(&turnLock);
    std::memcpy(gaze, gazeTan, sizeof(gaze));
    std::memcpy(gazing, gazeValid, sizeof(gazing));
    std::memcpy(previousTurns, lastTurn, sizeof(previousTurns));
    // One focus box for the whole frame, whatever views4_set_live does meanwhile.
    const double boxX = focusFraction, boxY = focusFractionY, cull = cullScale;
    ReleaseSRWLockShared(&turnLock);
    Views4Frame frame{};
    XrQuaternionf turns[2] = {kNoTurn, kNoTurn};
    FocusFrustum frusta[2];
    for (int eye = 0; eye < 2; ++eye) {
        const uint8_t* src = renderer + ace::kCameraSlotBase + eye * ace::kCameraSlotStride;
        float projection[16];
        std::memcpy(projection, src + ace::kCameraProjection, sizeof(projection));
        // What ACE renders this eye with; the located FOV only if the projection is unreadable.
        float* outer = frame.outerTan[eye];
        if (!projection_tangents(projection, frame.outerTan[eye]))
            fov_tangents(views[eye].fov, frame.outerTan[eye]);
        if (!(outer[1] - outer[0] > 0.02f && outer[2] - outer[3] > 0.02f))
            return;  // no located views yet
        check_rendered_fov(eye, frame.outerTan[eye], views[eye].fov);
        if (boxX <= 0) {
            // Diagnostic (views4_focus_eye_camera): the focus views are exact copies of the eyes,
            // so any difference between quarters e and e + 2 comes from the view index.
            std::memcpy(reinterpret_cast<uint8_t*>((eye == 0 ? cameraPhantom2 : cameraPhantom) + ace::kCameraSlotBase),
                        src, ace::kCameraSlotStride);
            copy_per_view(renderer, eye, eye + 2);
            std::memcpy(frame.focusTan[eye], outer, sizeof(frame.focusTan[eye]));
            frame.turn[eye] = kNoTurn;
            continue;
        }
        double hw, hh;
        focus_extents(frame.outerTan[eye], boxX, boxY, hw, hh);
        if (gazing[eye])
            turns[eye] = focus_turn(frame.outerTan[eye], boxX, boxY, gaze[eye][0], gaze[eye][1]);
        const float focusTan[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                   static_cast<float>(-hh)};
        std::memcpy(frame.focusTan[eye], focusTan, sizeof(focusTan));
        frame.turn[eye] = turns[eye];
        const int view = eye + 2;
        uint8_t* dst = reinterpret_cast<uint8_t*>((view == 2 ? cameraPhantom2 : cameraPhantom) + ace::kCameraSlotBase);
        std::memcpy(dst, src, ace::kCameraSlotStride);
        projection[0] = static_cast<float>(1.0 / hw);  // 2 / (tanR - tanL) with tanR = -tanL = hw
        projection[5] = static_cast<float>(1.0 / hh);
        projection[8] = 0.0f;
        projection[9] = 0.0f;
        std::memcpy(dst + ace::kCameraProjection, projection, sizeof(projection));
        std::memcpy(dst + ace::kCameraPreviousProjection,
                    lastFocusValid[eye] ? lastFocusProjection[eye] : projection, sizeof(projection));
        std::memcpy(lastFocusProjection[eye], projection, sizeof(projection));
        lastFocusValid[eye] = true;
        // The camera turned towards the gaze; last frame's matrices by last frame's turn, so motion
        // vectors and temporal history follow the focus camera's own motion.
        turn_matrix_at(dst + ace::kCameraView, turns[eye], false);
        turn_matrix_at(dst + ace::kCameraWorldView, turns[eye], false);
        turn_matrix_at(dst + ace::kCameraPreviousView, previousTurns[eye], false);
        turn_matrix_at(dst + ace::kCameraPreviousWorldView, previousTurns[eye], false);
        if (focusCull) {
            float worldView[16];
            std::memcpy(worldView, dst + ace::kCameraWorldView, sizeof(worldView));
            frusta[eye] = focus_frustum(worldView, hw * cull, hh * cull);
        }
        // Per-view vectors (sized for 4 at init): the eye's inverse view, turned; the focus tangent extents.
        const auto entries = [&](std::ptrdiff_t offset, size_t size) -> uint8_t* {
            auto* const* bounds = reinterpret_cast<uint8_t* const*>(renderer + offset);
            return static_cast<size_t>(bounds[1] - bounds[0]) >= size * kViews ? bounds[0] : nullptr;
        };
        if (uint8_t* m = entries(ace::kPerViewMatrix, 0x40)) {
            std::memcpy(m + view * 0x40, m + eye * 0x40, 0x40);
            turn_matrix_at(m + view * 0x40, turns[eye], true);
        }
        if (auto* x = reinterpret_cast<float*>(entries(ace::kPerViewTanX, 4)))
            x[view] = static_cast<float>(hw);
        if (auto* y = reinterpret_cast<float*>(entries(ace::kPerViewTanY, 4)))
            y[view] = static_cast<float>(hh);
    }
    views4_push_frame(frame);
    AcquireSRWLockExclusive(&turnLock);
    std::memcpy(lastTurn, turns, sizeof(turns));
    if (focusCull && boxX > 0 && (gazeSampled || !gazeExpected)) {
        CullFrusta& c = cullFrusta[(turnsPushed - 1) % 2];
        std::memcpy(c.view, frusta, sizeof(frusta));
        c.fill = turnsPushed;
        ++cullBuilt;
    } else if (focusCull) {
        ++cullWaitingForGaze;
    }
    ReleaseSRWLockExclusive(&turnLock);
}

int viewCountHook(void* renderer) {
    const uintptr_t returnRva = reinterpret_cast<uintptr_t>(_ReturnAddress()) - base;
    const int mode = mode_of(renderer);
    int n = mode + 1;
    if (mode == 1 && is_allowed(returnRva)) {
        n = kViews;
        widened.fetch_add(1, std::memory_order_relaxed);
        if (cameraViews && returnRva == ace::kRecordLoopViewCountCallSites[0] + 5) {
            LARGE_INTEGER from, to;
            QueryPerformanceCounter(&from);
            fill_focus_cameras(static_cast<uint8_t*>(renderer));
            QueryPerformanceCounter(&to);
            const auto t = static_cast<uint64_t>(to.QuadPart - from.QuadPart);
            fillTicks.fetch_add(t, std::memory_order_relaxed);
            fillCalls.fetch_add(1, std::memory_order_relaxed);
            for (uint64_t m = fillMaxTicks.load(std::memory_order_relaxed); t > m && !fillMaxTicks.compare_exchange_weak(m, t);) {
            }
        }
    }
    note_caller(returnRva, mode, n);
    return n;
}

void put_f(uint8_t* p, size_t at, float v) { *reinterpret_cast<float*>(p + at) = v; }
void put_i(uint8_t* p, size_t at, int32_t v) { *reinterpret_cast<int32_t*>(p + at) = v; }
float get_f(const uint8_t* p, size_t at) { return *reinterpret_cast<const float*>(p + at); }

// Mode 1 gives two halves; with the doubled eye width the target holds the four views of `layout`.
void* viewportHook(void* renderer, void* out, const float* renderSize, int view) {
    lastRenderer.store(renderer, std::memory_order_relaxed);
    if (mode_of(renderer) != 1 || view < 0 || view >= kViews)
        return viewportOriginal(renderer, out, renderSize, view);
    // With focus cameras, views 2-3 have their own per-view vector entries; otherwise use the eyes'.
    void* result = viewportOriginal(renderer, out, renderSize, cameraViews ? view : view & 1);
    viewportCalls[view].fetch_add(1, std::memory_order_relaxed);
    auto* o = static_cast<uint8_t*>(out);
    const int width = static_cast<int>(renderSize[0]), height = static_cast<int>(renderSize[1]);
    // Only rewrite the layout we know: the mode-1 halves (another display path leaves it alone).
    if (width < kViews || get_f(o, 0x28) != renderSize[0] * 0.5f) {
        viewportMismatch.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const ViewRect r = views4_rect(layout, view, width, height);
    const int left = r.left;
    const float w = static_cast<float>(r.width), h = static_cast<float>(r.height);
    put_f(o, 0x00, static_cast<float>(left));  // D3D12_VIEWPORT
    put_f(o, 0x04, 0.0f);
    put_f(o, 0x08, w);
    put_f(o, 0x0c, h);
    put_i(o, 0x18, left);  // D3D12_RECT scissor
    put_i(o, 0x1c, 0);
    put_i(o, 0x20, left + r.width);
    put_i(o, 0x24, r.height);
    put_f(o, 0x28, w);  // view size and reciprocal
    put_f(o, 0x2c, h);
    put_f(o, 0x30, 1.0f / w);
    put_f(o, 0x34, 1.0f / h);
    put_f(o, 0x38, static_cast<float>(left));  // pixel offset
    put_f(o, 0x3c, 0.0f);
    put_f(o, 0x40, w / static_cast<float>(width));  // UV scale
    put_f(o, 0x44, h / static_cast<float>(height));
    put_f(o, 0x48, static_cast<float>(left) / static_cast<float>(width));  // UV offset
    put_f(o, 0x4c, 0.0f);
    // The tile grid and the volumetrics are sized once from view 0 for every view (ace_abi.h): they
    // get the largest view's size. Equal views leave this unchanged.
    const uintptr_t returnRva = reinterpret_cast<uintptr_t>(_ReturnAddress()) - base;
    if (!layout.equal() && view == 0 &&
        (returnRva == ace::kTileGridViewportCallSite + 5 || returnRva == ace::kFogViewportCallSite + 5)) {
        int maxW = 0, maxH = 0;
        for (int v = 0; v < kViews; ++v) {
            const ViewRect e = views4_rect(layout, v, width, height);
            maxW = std::max(maxW, e.width);
            maxH = std::max(maxH, e.height);
        }
        const float mw = static_cast<float>(maxW), mh = static_cast<float>(maxH);
        put_f(o, 0x08, mw);
        put_f(o, 0x0c, mh);
        put_i(o, 0x20, maxW);
        put_i(o, 0x24, maxH);
        put_f(o, 0x28, mw);
        put_f(o, 0x2c, mh);
        put_f(o, 0x30, 1.0f / mw);
        put_f(o, 0x34, 1.0f / mh);
        put_f(o, 0x40, mw / static_cast<float>(width));
        put_f(o, 0x44, mh / static_cast<float>(height));
        logf("views4: %s sized for the largest view, %dx%d of %dx%d",
             returnRva == ace::kFogViewportCallSite + 5 ? "volumetrics" : "tile grid", maxW, maxH, width, height);
    }
    if (!layoutLogged.exchange(true)) {
        int lefts[kViews], widths[kViews];
        char line[256];
        int n = std::snprintf(line, sizeof(line), "views4: viewport layout render=%dx%d, views (x, w x h):", width, height);
        for (int v = 0; v < kViews; ++v) {
            const ViewRect e = views4_rect(layout, v, width, height);
            lefts[v] = e.left;
            widths[v] = e.width;
            n += std::snprintf(line + n, sizeof(line) - n, " %d: %d, %dx%d;", v, e.left, e.width, e.height);
        }
        log_line(line);
        pass_census_set_view_columns(lefts, widths);
    }
    return result;
}

// The pass setup halves its target; the half it wrote gives the target width W, its viewport the
// height H. View i's rect of W x H.
// It gets the real view: setting up view 2 as view 0 made it a first view, whose passes clear the
// target and wiped the eyes (views4 run 5). Its camera-slot reads are folded by the imul patches.
template <size_t I>
void passSetupHook(void* renderer, void* pass, int view) {
    const bool vr = mode_of(renderer) == 1 && view >= 0 && view < kViews;
    passSetupOriginal[I](renderer, pass, view);
    if (!vr)
        return;
    passCalls[view].fetch_add(1, std::memory_order_relaxed);
    auto* p = static_cast<uint8_t*>(pass);
    if (borrowCount && *reinterpret_cast<const int32_t*>(p) == kLightingPassType) {
        const int eye = view & 1;
        for (size_t i = 0; i < borrowCount; ++i) {
            if (view < 2)
                std::memcpy(&borrowed[I][eye][i], p + borrowOffsets[i], 4);
            else if (borrowValid[I][eye])
                std::memcpy(p + borrowOffsets[i], &borrowed[I][eye][i], 4);
        }
        if (view < 2)
            borrowValid[I][eye] = true;
    }
    if (snapshotArmed.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&snapshotLock);
        if (snapshotCount < std::size(snapshots)) {
            PassSnapshot& s = snapshots[snapshotCount++];
            s.setup = static_cast<int>(I);
            s.type = *reinterpret_cast<const int32_t*>(p);
            s.view = view;
            std::memcpy(s.data, p, kPassSnapshotSize);
        }
        ReleaseSRWLockExclusive(&snapshotLock);
    }
    const float half = get_f(p, ace::kPassViewport + 0x08);
    const int width = static_cast<int>(half * 2.0f + 0.5f);
    const int height = static_cast<int>(get_f(p, ace::kPassViewport + 0x0c) + 0.5f);
    if (width < kViews || height < 1) {
        passMismatch.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const ViewRect r = views4_rect(layout, view, width, height);
    put_f(p, ace::kPassViewport + 0x00, static_cast<float>(r.left));
    put_f(p, ace::kPassViewport + 0x08, static_cast<float>(r.width));
    put_f(p, ace::kPassViewport + 0x0c, static_cast<float>(r.height));
    put_i(p, ace::kPassScissor + 0x00, r.left);
    put_i(p, ace::kPassScissor + 0x08, r.left + r.width);
    put_i(p, ace::kPassScissor + 0x0c, r.height);
    put_f(p, ace::kPassScreenOffset + 0x00, static_cast<float>(r.left));
    put_f(p, ace::kPassScreenOffset + 0x04, 0.0f);
    if (!layout.equal()) {
        // Same ratio to this view's size as it had to an equal quarter of the target.
        const int quarter = width / kViews;
        put_f(p, ace::kPassViewExtent + 0x00,
              get_f(p, ace::kPassViewExtent + 0x00) * static_cast<float>(r.width) / static_cast<float>(quarter));
        put_f(p, ace::kPassViewExtent + 0x04,
              get_f(p, ace::kPassViewExtent + 0x04) * static_cast<float>(r.height) / static_cast<float>(height));
    }
    if (!passLogged.exchange(true))
        logf("views4: pass viewport (setup 0x%llx) target %dx%d -> view %d at x=%d, %dx%d",
             static_cast<unsigned long long>(ace::kPassSetupRvas[I]), width, height, view, r.left, r.width, r.height);
}

void* viewRecordHook(void* renderer, int view) {
    if (!cameraViews && view >= 2 && view < kViews && mode_of(renderer) == 1) {
        recordsFolded.fetch_add(1, std::memory_order_relaxed);
        view &= 1;
    }
    return viewRecordOriginal(renderer, view);
}

// normalReconstructionViewSpace picks one of two view records per pixel by a split x; with four
// quarters it needs two passes (ace_abi.h). First the focus views (records 2 and 3, split 3/4 of the
// width) over the whole target, a UAV barrier, then the eyes (the caller's records 0 and 1, split
// 1/4) over the first half only, so each quarter ends up reconstructed with its own view.
using NormalReconstructFn = void (*)(void* params);
NormalReconstructFn normalReconstructOriginal = nullptr;
uint32_t normalDispatchGroupsX = 0;  // read by the dispatch stub: nonzero caps the x group count
uint64_t tileEyeStruct[kViews] = {};  // diagnostic: the tile culling's eye structure per view
constexpr bool kNormalReconstructPerQuarter = false;  // two-pass normal reconstruction (no visible effect)
constexpr bool kTileCullingProbe = false;             // record the tile culling's eye structures for the dump
std::atomic<uint64_t> normalPasses{0};

void normalReconstructHook(void* params) {
    auto* p = static_cast<uint8_t*>(params);
    void* recordB = *reinterpret_cast<void**>(p + ace::kNormalParamsRecordB);
    void* renderer = cameraPhantom2 ? *reinterpret_cast<void**>(cameraPhantom2 + ace::kPhantomRenderer) : nullptr;
    const auto* target = *reinterpret_cast<const uint8_t* const*>(p + ace::kNormalParamsTarget);
    const int width = target ? *reinterpret_cast<const int32_t*>(target + ace::kTextureWidth) : 0;
    const int eye1 = views4_rect(layout, 1, width, 1).left, focus0 = views4_rect(layout, 2, width, 1).left,
              focus1 = views4_rect(layout, 3, width, 1).left;
    if (!recordB || !renderer || mode_of(renderer) != 1 || eye1 <= 0 || !pass_census_installed()) {
        normalReconstructOriginal(params);
        return;
    }
    alignas(16) uint8_t focus[ace::kNormalParamsSize];
    std::memcpy(focus, p, sizeof(focus));
    void* record2 = viewRecordOriginal(renderer, 2);
    void* record3 = viewRecordOriginal(renderer, 3);
    std::memcpy(focus + ace::kNormalParamsRecordA, &record2, sizeof(void*));
    std::memcpy(focus + ace::kNormalParamsRecordB, &record3, sizeof(void*));
    const int32_t focusSplit = focus1;
    std::memcpy(focus + ace::kNormalParamsSplit, &focusSplit, sizeof(focusSplit));
    pass_census_barrier_after_next_dispatch();
    normalReconstructOriginal(focus);

    alignas(16) uint8_t eyes[ace::kNormalParamsSize];
    std::memcpy(eyes, p, sizeof(eyes));
    const int32_t eyeSplit = eye1;
    std::memcpy(eyes + ace::kNormalParamsSplit, &eyeSplit, sizeof(eyeSplit));
    normalDispatchGroupsX = static_cast<uint32_t>((focus0 + 7) / 8);
    normalReconstructOriginal(eyes);
    normalDispatchGroupsX = 0;
    if (normalPasses.fetch_add(1, std::memory_order_relaxed) == 0)
        logf("views4: normal reconstruction per view: focus split %d, eyes split %d over %u x groups", focusSplit,
             eyeSplit, (focus0 + 7) / 8);
}

bool parse_callers(const wchar_t* spec) {
    allowedCount = 0;
    allowAll = false;
    drawLoops = false;
    tileViews = false;
    slotViews = false;
    cameraViews = false;
    volumetricViews = false;
    foldVolumetrics = false;
    wchar_t buffer[1024];
    wcsncpy_s(buffer, spec ? spec : L"init", _TRUNCATE);
    wchar_t* context = nullptr;
    for (wchar_t* token = wcstok_s(buffer, L", ", &context); token; token = wcstok_s(nullptr, L", ", &context)) {
        if (_wcsicmp(token, L"all") == 0) {
            allowAll = true;
        } else if (_wcsicmp(token, L"init") == 0) {
            for (uintptr_t site : ace::kInitViewCountCallSites)
                if (allowedCount < std::size(allowed))
                    allowed[allowedCount++] = site + 5;
        } else if (_wcsicmp(token, L"draw") == 0 || _wcsicmp(token, L"depth") == 0 ||
                   _wcsicmp(token, L"lighting") == 0) {
            const bool depth = _wcsicmp(token, L"lighting") != 0, lighting = _wcsicmp(token, L"depth") != 0;
            drawLoops |= lighting;
            if (depth)
                for (uintptr_t site : ace::kDepthLoopViewCountCallSites)
                    if (allowedCount < std::size(allowed))
                        allowed[allowedCount++] = site + 5;
        } else if (const auto* group = std::find_if(std::begin(ace::kOptionalViewCountGroups),
                                                    std::end(ace::kOptionalViewCountGroups),
                                                    [&](const auto& g) { return _wcsicmp(token, g.token) == 0; });
                   group != std::end(ace::kOptionalViewCountGroups)) {
            volumetricViews |= _wcsicmp(token, L"fog") == 0;
            for (const uintptr_t site : group->sites)
                if (site && allowedCount < std::size(allowed))
                    allowed[allowedCount++] = site + 5;
        } else if (_wcsicmp(token, L"fogfold") == 0) {
            foldVolumetrics = true;
        } else if (_wcsicmp(token, L"cameras") == 0) {
            cameraViews = true;
            for (uintptr_t site : ace::kRecordLoopViewCountCallSites)
                if (allowedCount < std::size(allowed))
                    allowed[allowedCount++] = site + 5;
        } else if (_wcsicmp(token, L"slots") == 0) {
            slotViews = true;
            for (uintptr_t site : ace::kImportViewCountCallSites)
                if (allowedCount < std::size(allowed))
                    allowed[allowedCount++] = site + 5;
        } else if (_wcsicmp(token, L"tiles") == 0) {
            tileViews = true;
            for (uintptr_t site : ace::kTileCullingViewCountCallSites)
                if (allowedCount < std::size(allowed))
                    allowed[allowedCount++] = site + 5;
        } else {
            wchar_t* end = nullptr;
            const unsigned long long site = wcstoull(token, &end, 16);
            if (!end || *end || site == 0) {
                logf("views4: bad caller '%ls' (want init, all or a hex call-site RVA)", token);
                return false;
            }
            if (allowedCount < std::size(allowed))
                allowed[allowedCount++] = static_cast<uintptr_t>(site) + 5;  // `call rel32` is 5 bytes
        }
    }
    return true;
}

// The focus views' seven history slots each, in layer memory, built with ACE's own element ctor,
// and every view-indexed access redirected there. ACE's own slot 2 is left unused: view 2 on it
// came out shifted against its eye (composition run 2). Called before the hooks go live; anchors
// were checked by the caller.
bool install_view_slots() {
    const size_t span = static_cast<size_t>(ace::kSlotArrayOffsets[std::size(ace::kSlotArrayOffsets) - 1] -
                                            ace::kSlotArrayOffsets[0]) + ace::kSlotSize;
    using SlotCtor = void* (*)(void* slot);
    const auto construct = reinterpret_cast<SlotCtor>(base + ace::kSlotElementCtorRva);
    uint64_t phantoms[2] = {};  // views 2 and 3: P + array offset = that view's slot of the array
    for (uint64_t& phantom : phantoms) {
        auto* scratch = static_cast<uint8_t*>(VirtualAlloc(nullptr, span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!scratch) {
            log_line("views4: no memory for the focus views' slots");
            return false;
        }
        phantom = reinterpret_cast<uint64_t>(scratch) - static_cast<uint64_t>(ace::kSlotArrayOffsets[0]);
        for (const auto offset : ace::kSlotArrayOffsets)
            construct(reinterpret_cast<void*>(phantom + static_cast<uint64_t>(offset)));
    }
    const uint64_t phantom2 = phantoms[0], phantom = phantoms[1];

    const auto redirect = [](uintptr_t rva, const uint8_t* anchor, size_t size, const std::vector<uint8_t>& stub) {
        if (code_redirect(rva, anchor, size, stub.data(), stub.size()))
            return true;
        logf("views4: slot patch at 0x%llx failed", static_cast<unsigned long long>(rva));
        return false;
    };
    for (const auto& site : ace::kHistorySlotIndexSites)
        if (!redirect(site.rva, site.anchor, sizeof(site.anchor), stub_history_index(site.anchor, phantom2, phantom)))
            return false;
    if (!redirect(ace::kCreatorBaseRva, ace::kCreatorBaseAnchor, sizeof(ace::kCreatorBaseAnchor),
                  stub_creator_base(ace::kCreatorBaseAnchor, phantom2, phantom)))
        return false;
    for (const auto rva : ace::kCreatorIndexRvas)
        if (!redirect(rva, ace::kCreatorIndexAnchor, sizeof(ace::kCreatorIndexAnchor),
                      stub_creator_index(ace::kCreatorIndexAnchor, phantom2, phantom)))
            return false;
    for (const auto rva : ace::kImportStepRvas)
        if (!redirect(rva, ace::kImportStepAnchor, sizeof(ace::kImportStepAnchor),
                      stub_import_step(ace::kImportStepAnchor, phantom2, phantom)))
            return false;
    const uint8_t bound[4] = {0x49, 0x83, 0xfc, 0x04};  // cmp r12, 4: the creator builds 4 slots
    if (!code_write(ace::kCreatorBoundRva, bound, sizeof(bound))) {
        log_line("views4: creator bound patch failed");
        return false;
    }
    logf("views4: focus views' history slots at phantom renderers 0x%llx (view 2) and 0x%llx (view 3)",
         static_cast<unsigned long long>(phantom2), static_cast<unsigned long long>(phantom));
    return true;
}

bool create_hook(uintptr_t rva, void* detour, void** original, const char* name) {
    const MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(base + rva), detour, original);
    if (s != MH_OK) {
        logf("views4: %s hook failed (MinHook %s)", name, MH_StatusToString(s));
        return false;
    }
    return true;
}

}  // namespace

bool views4_start(const wchar_t* callers, bool eyesOnlySlotAdvance, double focus, double focusY) {
    if (started)
        return true;
    focusFraction = focus;
    focusFractionY = focusY > 0 ? focusY : focus;
    base = ace_base();
    if (!code_matches(ace::kViewCountRva, ace::kViewCountAnchor, sizeof(ace::kViewCountAnchor)) ||
        !code_matches(ace::kViewportRva, ace::kViewportAnchor, sizeof(ace::kViewportAnchor)) ||
        !code_matches(ace::kViewRecordRva, ace::kViewRecordAnchor, sizeof(ace::kViewRecordAnchor)) ||
        !code_matches(ace::kPassSetupRvas[0], ace::kPassSetupAnchors[0], sizeof(ace::kPassSetupAnchors[0])) ||
        !code_matches(ace::kPassSetupRvas[1], ace::kPassSetupAnchors[1], sizeof(ace::kPassSetupAnchors[1]))) {
        log_line("views4: anchor mismatch (different ACE build?); experiment refused");
        return false;
    }
    const auto sites_match = [](const auto& sites) {
        for (const auto& site : sites)
            if (!code_matches(site.rva, site.anchor, sizeof(site.anchor))) {
                logf("views4: slot-index anchor mismatch at 0x%llx; experiment refused",
                     static_cast<unsigned long long>(site.rva));
                return false;
            }
        return true;
    };
    if (!sites_match(ace::kCameraSlotIndexSites) || !sites_match(ace::kHistorySlotIndexSites) ||
        !sites_match(ace::kPassViewReadSites) || !sites_match(ace::kSlotAdvanceSites))
        return false;
    if (!code_matches(ace::kLightingLoopBoundRva, ace::kLightingLoopBoundAnchor, sizeof(ace::kLightingLoopBoundAnchor)) ||
        !code_matches(ace::kVelocityPassViewRva, ace::kVelocityPassViewAnchor, sizeof(ace::kVelocityPassViewAnchor))) {
        log_line("views4: lighting loop or velocity pass anchor mismatch; experiment refused");
        return false;
    }
    if (!parse_callers(callers))
        return false;
    if (slotViews) {
        bool ok = code_matches(ace::kSlotElementCtorRva, ace::kSlotElementCtorAnchor, sizeof(ace::kSlotElementCtorAnchor)) &&
                  code_matches(ace::kCreatorBaseRva, ace::kCreatorBaseAnchor, sizeof(ace::kCreatorBaseAnchor)) &&
                  code_matches(ace::kCreatorBoundRva, ace::kCreatorBoundAnchor, sizeof(ace::kCreatorBoundAnchor));
        for (const auto rva : ace::kCreatorIndexRvas)
            ok &= code_matches(rva, ace::kCreatorIndexAnchor, sizeof(ace::kCreatorIndexAnchor));
        for (const auto rva : ace::kImportStepRvas)
            ok &= code_matches(rva, ace::kImportStepAnchor, sizeof(ace::kImportStepAnchor));
        if (!ok) {
            log_line("views4: slot-site anchor mismatch; experiment refused");
            return false;
        }
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("views4: MH_Initialize failed; experiment refused");
        return false;
    }
    const struct {
        uintptr_t rva;
        void* detour;
        void** original;
        const char* name;
    } hooks[] = {
        {ace::kViewCountRva, reinterpret_cast<void*>(&viewCountHook), reinterpret_cast<void**>(&viewCountOriginal), "viewCount"},
        {ace::kViewportRva, reinterpret_cast<void*>(&viewportHook), reinterpret_cast<void**>(&viewportOriginal), "viewport"},
        {ace::kViewRecordRva, reinterpret_cast<void*>(&viewRecordHook), reinterpret_cast<void**>(&viewRecordOriginal), "view record"},
        {ace::kPassSetupRvas[0], reinterpret_cast<void*>(&passSetupHook<0>), reinterpret_cast<void**>(&passSetupOriginal[0]), "pass setup 0"},
        {ace::kPassSetupRvas[1], reinterpret_cast<void*>(&passSetupHook<1>), reinterpret_cast<void**>(&passSetupOriginal[1]), "pass setup 1"},
    };
    const auto remove_all = [&] {
        for (const auto& h : hooks)
            MH_RemoveHook(reinterpret_cast<void*>(base + h.rva));
    };
    for (const auto& h : hooks)
        if (!create_hook(h.rva, h.detour, h.original, h.name)) {
            remove_all();
            return false;
        }
    // The index masks are plain patches (not MinHook); views 2 and 3 use the eyes' camera and
    // history slots. Harmless for views 0 and 1, so applied whether or not the loops are widened.
    if (cameraViews) {
        // The focus views' camera slots live in layer memory at C2 and C3 (ACE's slot 2 stays unused),
        // laid out as slots 0 and 1 of a phantom renderer at C2's base: the pass setup reads them
        // with the eyes' fixed displacements from it. Its first page (renderer fields read on those
        // paths) and the slots' pages are committed; the rest of the range is only reserved.
        const size_t slotsEnd = static_cast<size_t>(ace::kCameraSlotBase) + 2 * ace::kCameraSlotStride;
        auto* phantom = static_cast<uint8_t*>(VirtualAlloc(nullptr, slotsEnd, MEM_RESERVE, PAGE_READWRITE));
        uint8_t* slots = phantom ? phantom + ace::kCameraSlotBase : nullptr;
        if (!phantom || !VirtualAlloc(phantom, 0x1000, MEM_COMMIT, PAGE_READWRITE) ||
            !VirtualAlloc(slots, 2 * ace::kCameraSlotStride, MEM_COMMIT, PAGE_READWRITE)) {
            log_line("views4: no memory for the focus views' camera slots");
            remove_all();
            return false;
        }
        cameraPhantom2 = reinterpret_cast<uint64_t>(phantom);
        cameraPhantom = cameraPhantom2 + ace::kCameraSlotStride;
        for (size_t i = 0; i < std::size(ace::kCameraSlotIndexSites); ++i) {
            const auto& site = ace::kCameraSlotIndexSites[i];
            const auto stub = stub_camera_index(site.anchor, ace::kCameraSlotIndexBase[i], cameraPhantom2, cameraPhantom);
            if (!code_redirect(site.rva, site.anchor, sizeof(site.anchor), stub.data(), stub.size())) {
                logf("views4: camera-slot patch at 0x%llx failed", static_cast<unsigned long long>(site.rva));
                remove_all();
                return false;
            }
        }
        const auto branch = stub_eye_branch(ace_base() + ace::kPassEyeBranchTarget, cameraPhantom2);
        const auto rejoin = stub_pass_rejoin(ace::kPassRejoinAnchor, static_cast<uint8_t>(ace::kPhantomRenderer));
        const bool patched =
            code_redirect(ace::kPassEyeBranchRva, ace::kPassEyeBranchAnchor, sizeof(ace::kPassEyeBranchAnchor),
                          branch.data(), branch.size()) &&
            code_redirect(ace::kPassRejoinRva, ace::kPassRejoinAnchor, sizeof(ace::kPassRejoinAnchor), rejoin.data(),
                          rejoin.size());
        if (!patched) {
            log_line("views4: pass-setup eye branch patch failed");
            remove_all();
            return false;
        }
    } else {
        for (const auto& site : ace::kCameraSlotIndexSites)
            if (!code_mask_imul_index(site.rva, site.anchor, 1)) {
                logf("views4: camera-slot patch at 0x%llx failed; continuing without views4 hooks",
                     static_cast<unsigned long long>(site.rva));
                remove_all();
                return false;
            }
    }
    if (slotViews) {
        if (!install_view_slots()) {
            remove_all();
            return false;
        }
    } else {
        for (const auto& site : ace::kHistorySlotIndexSites)
            if (!code_mask_movsxd_shl_index(site.rva, site.anchor, 1)) {
                logf("views4: history-slot patch at 0x%llx failed; continuing without views4 hooks",
                     static_cast<unsigned long long>(site.rva));
                remove_all();
                return false;
            }
    }
    // These reads take each view's volumetric/atmosphere resources (tables at +0x3f40/+0x3fd0/+0x4000
    // of the system at renderer + 0x13e0). With "fog" the system has 4 views and each reads its own;
    // otherwise views 2-3 borrow the eyes' (wrong frustum: darker image, dark sky).
    for (const auto& site : ace::kPassViewReadSites)
        if ((!volumetricViews || foldVolumetrics) && !code_mask_load32(site.rva, site.anchor, 1)) {
            logf("views4: pass-view patch at 0x%llx failed; continuing without views4 hooks",
                 static_cast<unsigned long long>(site.rva));
            remove_all();
            return false;
        }
    for (const auto& site : ace::kSlotAdvanceSites)
        if (eyesOnlySlotAdvance && !code_call_if_below(site.rva, site.anchor, base + ace::kSlotAdvanceRva, site.viewRegister, 2,
                                ace::kSlotFrameFlag)) {
            logf("views4: slot-advance patch at 0x%llx failed; continuing without views4 hooks",
                 static_cast<unsigned long long>(site.rva));
            remove_all();
            return false;
        }
    // With "tiles" the tile lists have 4 entries and views 2-3 read their own.
    if (!tileViews && !code_redirect(ace::kVelocityPassViewRva, ace::kVelocityPassViewAnchor,
                                     sizeof(ace::kVelocityPassViewAnchor), ace::kVelocityPassViewStub,
                                     sizeof(ace::kVelocityPassViewStub))) {
        log_line("views4: velocity pass patch failed; continuing without views4 hooks");
        remove_all();
        return false;
    }
    if (drawLoops && !code_redirect(ace::kLightingLoopBoundRva, ace::kLightingLoopBoundAnchor,
                                    sizeof(ace::kLightingLoopBoundAnchor), ace::kLightingLoopBoundStub,
                                    sizeof(ace::kLightingLoopBoundStub))) {
        log_line("views4: lighting loop patch failed; continuing without views4 hooks");
        remove_all();
        return false;
    }
    // Normal reconstruction per quarter: disabled (no effect on the view 1 bands or the view 2 stepped
    // shadow; recon 'Normal-reconstruction fix run'). Kept for reference behind kNormalReconstructPerQuarter.
    bool normalHooked = false;
    if (cameraViews && kNormalReconstructPerQuarter) {
        const auto stub = stub_dispatch_x_override(ace::kNormalReconstructDispatchAnchor,
                                                   reinterpret_cast<uint64_t>(&normalDispatchGroupsX));
        normalHooked = code_matches(ace::kNormalReconstructRva, ace::kNormalReconstructAnchor,
                                    sizeof(ace::kNormalReconstructAnchor)) &&
                       code_redirect(ace::kNormalReconstructDispatchRva, ace::kNormalReconstructDispatchAnchor,
                                     sizeof(ace::kNormalReconstructDispatchAnchor), stub.data(), stub.size()) &&
                       create_hook(ace::kNormalReconstructRva, reinterpret_cast<void*>(&normalReconstructHook),
                                   reinterpret_cast<void**>(&normalReconstructOriginal), "normal reconstruction");
        if (!normalHooked)
            log_line("views4: normal reconstruction patch failed; views 1-3 keep the eyes' reconstruction");
        const auto record = stub_record_rdi_by_view(ace::kTileCullingEyeStructAnchor, reinterpret_cast<uint64_t>(tileEyeStruct));
        if (kTileCullingProbe && !code_redirect(ace::kTileCullingEyeStructRva, ace::kTileCullingEyeStructAnchor,
                           sizeof(ace::kTileCullingEyeStructAnchor), record.data(), record.size()))
            log_line("views4: tile culling eye-structure probe not installed");
    }
    for (const auto& h : hooks)
        MH_QueueEnableHook(reinterpret_cast<void*>(base + h.rva));
    if (normalHooked)
        MH_QueueEnableHook(reinterpret_cast<void*>(base + ace::kNormalReconstructRva));
    if (MH_ApplyQueued() != MH_OK) {
        log_line("views4: enabling hooks failed");
        return false;
    }
    started = true;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    if (focusCull && !cameraViews) {
        focusCull = false;
        log_line("views4: views4_focus_cull needs the \"cameras\" token; focus cull off");
    }
    if (focusCull)
        logf("views4: focus cull: focus frusta built every frame at %.0f%% of the focus extent%s; the draw-loop hook is "
             "not located yet, so no draws are skipped",
             cullScale * 100, gazeExpected ? " (from the first gaze sample)" : "");
    char list[512];
    int n = std::snprintf(list, sizeof(list), "%s", allowAll ? "all" : "");
    for (size_t i = 0; i < allowedCount && n < static_cast<int>(sizeof(list)) - 16; ++i)
        n += std::snprintf(list + n, sizeof(list) - n, "%s0x%llx", n ? "," : "",
                           static_cast<unsigned long long>(allowed[i] - 5));
    logf("views4: ACTIVE, 4 views in VR for call sites [%s]%s%s; views 0-1 at %.0f%% and 2-3 at %.0f%% of the native "
         "eye size%s, views 2-3 %s, history slot advance %s",
         list, tileViews ? " (tile culling included)" : "", drawLoops ? ", lighting loop 4 views" : "",
         layout.outer * 100, layout.focus * 100, layout.equal() ? " (equal quarters)" : "",
         cameraViews ? "are focus cameras" : "copy the eyes", eyesOnlySlotAdvance ? "eyes only" : "every view (ACE's)");
    if (cameraViews)
        logf("views4: focus cameras cover %.0f%% x %.0f%% of each eye's tangent extent, camera slots at 0x%llx and 0x%llx",
             focusFraction * 100, focusFractionY * 100, static_cast<unsigned long long>(cameraPhantom2 + ace::kCameraSlotBase),
             static_cast<unsigned long long>(cameraPhantom + ace::kCameraSlotBase));
    return true;
}

namespace {

// No C++ objects with destructors: __try needs a plain frame.
bool copy_floats(const uint8_t* from, float* to, size_t count) {
    __try {
        std::memcpy(to, from, count * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void log_matrix(const char* what, const float* m) {
    logf("views4 dump: %s [%9.5f %9.5f %9.5f %9.5f | %9.5f %9.5f %9.5f %9.5f | %9.5f %9.5f %9.5f %9.5f | %9.5f %9.5f "
         "%9.5f %9.5f]",
         what, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

// Camera slots of views 0 and 1 (the six 0x40 matrices and the float at +0x180), the per-view
// vectors +0x640 (0x40 each), +0x658 and +0x670 (floats), and the located views of this frame.
void dump_cameras() {
    const auto* renderer = static_cast<const uint8_t*>(lastRenderer.load());
    if (!renderer)
        return;
    XrView views[2];
    AcquireSRWLockShared(&viewsLock);
    std::memcpy(views, lastViews, sizeof(views));
    ReleaseSRWLockShared(&viewsLock);
    for (int e = 0; e < 2; ++e) {
        const XrFovf& f = views[e].fov;
        const XrPosef& p = views[e].pose;
        logf("views4 dump: located eye %d fov L/R/U/D %.5f/%.5f/%.5f/%.5f (tan %.5f/%.5f/%.5f/%.5f) pos %.4f,%.4f,%.4f "
             "quat %.5f,%.5f,%.5f,%.5f",
             e, f.angleLeft, f.angleRight, f.angleUp, f.angleDown, std::tan(f.angleLeft), std::tan(f.angleRight),
             std::tan(f.angleUp), std::tan(f.angleDown), p.position.x, p.position.y, p.position.z, p.orientation.x,
             p.orientation.y, p.orientation.z, p.orientation.w);
    }
    static const char* const names[] = {"+0x000", "+0x040", "+0x080", "+0x0c0", "+0x100", "+0x140"};
    const int views_dumped = cameraViews ? kViews : 2;
    for (int view = 0; view < views_dumped; ++view) {
        const uint8_t* slot =
            view >= 2 ? reinterpret_cast<const uint8_t*>((view == 2 ? cameraPhantom2 : cameraPhantom) + ace::kCameraSlotBase)
                      : renderer + ace::kCameraSlotBase + view * ace::kCameraSlotStride;
        float m[16];
        char what[64];
        for (int k = 0; k < 6; ++k) {
            std::snprintf(what, sizeof(what), "camera slot %d %s", view, names[k]);
            if (copy_floats(slot + k * 0x40, m, 16))
                log_matrix(what, m);
        }
        float f = 0;
        if (copy_floats(slot + 0x180, &f, 1))
            logf("views4 dump: camera slot %d +0x180 %.6f", view, f);
    }
    const auto* const* v640 = reinterpret_cast<const uint8_t* const*>(renderer + 0x640);
    const auto* const* v658 = reinterpret_cast<const uint8_t* const*>(renderer + 0x658);
    const auto* const* v670 = reinterpret_cast<const uint8_t* const*>(renderer + 0x670);
    for (int view = 0; view < views_dumped; ++view) {
        float m[16], a = 0, b = 0;
        char what[64];
        std::snprintf(what, sizeof(what), "per-view +0x640[%d]", view);
        if (copy_floats(*v640 + view * 0x40, m, 16))
            log_matrix(what, m);
        if (copy_floats(*v658 + view * 4, &a, 1) && copy_floats(*v670 + view * 4, &b, 1))
            logf("views4 dump: per-view +0x658[%d] %.6f +0x670[%d] %.6f", view, a, view, b);
    }
    // Tiled light culling's eye structure, as each view last saw it, and both eye blocks' matrices.
    for (int view = 0; view < kViews; ++view) {
        const auto* s = reinterpret_cast<const uint8_t*>(tileEyeStruct[view]);
        logf("views4 dump: tile culling view %d eye structure %p (block %s)", view, s, view == 0 ? "+0x50" : "+0x5b0");
        if (!s || (view > 0 && tileEyeStruct[view] == tileEyeStruct[view - 1]))
            continue;
        for (int e = 0; e < 2; ++e) {
            float m[16];
            char what[96];
            std::snprintf(what, sizeof(what), "tile culling view %d eye block %d +0x224", view, e);
            if (copy_floats(s + ace::kTileEyeBlocks[e] + ace::kTileEyeMatrix, m, 16))
                log_matrix(what, m);
        }
    }
}

// The pass name (MSVC std::string at pass +8: inline buffer or pointer, size +0x10, capacity +0x18).
std::string pass_name(const uint8_t* data) {
    const auto size = *reinterpret_cast<const uint64_t*>(data + 0x18);
    const auto capacity = *reinterpret_cast<const uint64_t*>(data + 0x20);
    if (size == 0 || size > 64 || capacity < size)
        return "?";
    if (capacity < 16)
        return std::string(reinterpret_cast<const char*>(data + 8), size);
    const char* text = *reinterpret_cast<const char* const*>(data + 8);
    char copy[65] = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), text, copy, size, &read) || read != size)
        return "?";
    return std::string(copy, size);
}

// Differing dwords of two snapshots of one pass, as runs, skipping the viewport, scissor and view.
void log_pass_diff(const PassSnapshot& a, const PassSnapshot& b) {
    const auto skipped = [](size_t off) {
        return (off >= static_cast<size_t>(ace::kPassViewport) && off < static_cast<size_t>(ace::kPassScissor) + 0x10) ||
               off == 0x9ac;
    };
    int runs = 0;
    for (size_t off = 0; off + 4 <= kPassSnapshotSize && runs < 24;) {
        if (skipped(off) || std::memcmp(a.data + off, b.data + off, 4) == 0) {
            off += 4;
            continue;
        }
        size_t end = off;
        while (end + 4 <= kPassSnapshotSize && !skipped(end) && std::memcmp(a.data + end, b.data + end, 4) != 0)
            end += 4;
        char line[640];
        int len = std::snprintf(line, sizeof(line), "views4 snapshot:   +0x%03zx..+0x%03zx:", off, end);
        for (size_t o = off; o < end && o < off + 32 && len < 560; o += 4) {
            uint32_t x, y;
            float fx, fy;
            std::memcpy(&x, a.data + o, 4);
            std::memcpy(&y, b.data + o, 4);
            std::memcpy(&fx, &x, 4);
            std::memcpy(&fy, &y, 4);
            len += std::snprintf(line + len, sizeof(line) - len, " %08x/%08x (%g/%g)", x, y, fx, fy);
        }
        log_line(line);
        ++runs;
        off = end;
    }
    if (runs == 0)
        log_line("views4 snapshot:   identical");
}

void report_snapshots() {
    AcquireSRWLockExclusive(&snapshotLock);
    const PassSnapshot* by[2][16][kViews] = {};  // [setup][pass index in arrival order][view]
    int types[2][16];
    int typeCount[2] = {};
    for (size_t i = 0; i < snapshotCount; ++i) {
        const PassSnapshot& s = snapshots[i];
        int k = 0;
        while (k < typeCount[s.setup] && types[s.setup][k] != s.type)
            ++k;
        if (k == typeCount[s.setup]) {
            if (k >= 16)
                continue;
            types[s.setup][typeCount[s.setup]++] = s.type;
        }
        if (!by[s.setup][k][s.view])
            by[s.setup][k][s.view] = &s;
    }
    logf("views4 snapshot: %zu pass setups captured", snapshotCount);
    for (int setup = 0; setup < 2; ++setup)
        for (int k = 0; k < typeCount[setup]; ++k) {
            const auto* const* v = by[setup][k];
            const PassSnapshot* any = v[0] ? v[0] : v[1] ? v[1] : v[2] ? v[2] : v[3];
            logf("views4 snapshot: setup %d type %d \"%s\" views %d%d%d%d", setup, types[setup][k],
                 pass_name(any->data).c_str(), v[0] != nullptr, v[1] != nullptr, v[2] != nullptr, v[3] != nullptr);
            static const int pairs[][2] = {{1, 3}, {0, 2}, {0, 1}};
            for (const auto& pr : pairs)
                if (v[pr[0]] && v[pr[1]]) {
                    logf("views4 snapshot:  view %d vs %d", pr[0], pr[1]);
                    log_pass_diff(*v[pr[0]], *v[pr[1]]);
                }
        }
    snapshotCount = 0;
    ReleaseSRWLockExclusive(&snapshotLock);
}

}  // namespace

void views4_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool gazeSample) {
    AcquireSRWLockExclusive(&turnLock);
    std::memcpy(gazeTan, tan, sizeof(gazeTan));
    std::memcpy(gazeValid, valid, sizeof(gazeValid));
    gazeSampled |= gazeSample;
    ReleaseSRWLockExclusive(&turnLock);
}

void views4_push_frame(const Views4Frame& frame) {
    AcquireSRWLockExclusive(&turnLock);
    frameRing[turnsPushed % std::size(frameRing)] = frame;
    lastPushed = frame;
    ++turnsPushed;
    ReleaseSRWLockExclusive(&turnLock);
}

bool views4_latest_frame(Views4Frame& frame) {
    AcquireSRWLockShared(&turnLock);
    const bool any = turnsPushed > 0;
    frame = lastPushed;
    ReleaseSRWLockShared(&turnLock);
    return any;
}

void views4_set_layout(const Views4Layout& l) { layout = l; }

void views4_set_focus_cull(bool on, double scale, bool gaze) {
    focusCull = on;
    cullScale = scale;
    gazeExpected = gaze;
}

void views4_set_live(double focus, double focusY, double scale, bool gaze) {
    AcquireSRWLockExclusive(&turnLock);
    focusFraction = focus;
    focusFractionY = focusY > 0 ? focusY : focus;
    cullScale = scale;
    gazeExpected = gaze;
    ReleaseSRWLockExclusive(&turnLock);
}

bool views4_focus_frustum(int view, FocusFrustum& out) {
    if (!focusCull || view < 2 || view >= kViews)
        return false;
    AcquireSRWLockShared(&turnLock);
    const CullFrusta& c = cullFrusta[0].fill > cullFrusta[1].fill ? cullFrusta[0] : cullFrusta[1];
    const bool valid = c.fill != 0;
    if (valid)
        out = c.view[view - 2];
    ReleaseSRWLockShared(&turnLock);
    return valid;
}

bool views4_take_frame(Views4Frame& frame) {
    AcquireSRWLockExclusive(&turnLock);
    const bool any = turnsPushed > 0;
    if (turnsTaken < turnsPushed) {
        // Frames filled but never composed (loading, a dropped frame) would leave every later
        // composition using an older frame's geometry: keep at most a short backlog.
        if (turnsPushed - turnsTaken > kMaxTurnBacklog) {
            turnsTaken = turnsPushed - 1;
            ++turnResyncs;
        }
        lastTaken = frameRing[turnsTaken % std::size(frameRing)];
        ++turnsTaken;
    } else if (any) {
        ++turnEmptyTakes;  // composed before this frame's focus cameras were filled: reuse the last record
    }
    frame = lastTaken;
    ReleaseSRWLockExclusive(&turnLock);
    return any;
}

bool views4_shadow_view1_plain() {
    const bool ok = code_matches(ace::kShadowView1BranchRva, ace::kShadowView1BranchAnchor,
                                 sizeof(ace::kShadowView1BranchAnchor)) &&
                    code_write(ace::kShadowView1BranchRva, ace::kShadowView1BranchPatch, sizeof(ace::kShadowView1BranchPatch));
    log_line(ok ? "views4: deferredShadows view 1 takes its own path (views4_shadow_view1_plain)"
                : "views4: deferredShadows view-1 branch patch failed (anchor mismatch)");
    return ok;
}

void views4_set_borrow(const wchar_t* offsets) {
    borrowCount = 0;
    for (const wchar_t* s = offsets; s && *s && borrowCount < std::size(borrowOffsets);) {
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(s, &end, 16);
        if (end == s)
            break;
        if (v % 4 == 0 && v + 4 <= kPassSnapshotSize && v != 0)
            borrowOffsets[borrowCount++] = v;
        s = *end == L',' ? end + 1 : end;
    }
    for (size_t i = 0; i < borrowCount; ++i)
        logf("views4: views 2-3 borrow lighting pass +0x%zx from their eye", borrowOffsets[i]);
}

void views4_note_views(const XrView* views, uint32_t count) {
    AcquireSRWLockExclusive(&viewsLock);
    for (uint32_t i = 0; i < count && i < 2; ++i)
        lastViews[i] = views[i];
    ReleaseSRWLockExclusive(&viewsLock);
}

void views4_frame_end() {
    if (!started)
        return;
    ++frames;
    for (const uint64_t f : kDumpFrames)
        if (frames == f)
            dump_cameras();
    for (const uint64_t f : kSnapshotFrames) {
        if (frames == f)
            snapshotArmed = true;
        if (frames == f + 1) {
            snapshotArmed = false;
            report_snapshots();
        }
    }
    if (frames % kReportEveryFrames != 0)
        return;
    unsigned long long v[kViews], p[kViews];
    for (int i = 0; i < kViews; ++i) {
        v[i] = viewportCalls[i].exchange(0);
        p[i] = passCalls[i].exchange(0);
    }
    logf("views4: last %llu frames: viewport calls per view %llu/%llu/%llu/%llu (not rewritten %llu), pass setups "
         "per view %llu/%llu/%llu/%llu (not rewritten %llu), viewCount widened %llu, view records folded %llu",
         static_cast<unsigned long long>(kReportEveryFrames), v[0], v[1], v[2], v[3],
         static_cast<unsigned long long>(viewportMismatch.exchange(0)), p[0], p[1], p[2], p[3],
         static_cast<unsigned long long>(passMismatch.exchange(0)), static_cast<unsigned long long>(widened.exchange(0)),
         static_cast<unsigned long long>(recordsFolded.exchange(0)));
    if (cameraViews) {
        AcquireSRWLockShared(&turnLock);
        const XrQuaternionf t = lastTurn[0];
        logf("views4: frames filled %llu, composed %llu (backlog %llu, resyncs %llu, composed before filling %llu); "
             "eye 0 turn %.3f,%.3f,%.3f,%.3f; frames rendered with a FOV other than the located one %llu",
             static_cast<unsigned long long>(turnsPushed), static_cast<unsigned long long>(turnsTaken),
             static_cast<unsigned long long>(turnsPushed - turnsTaken), static_cast<unsigned long long>(turnResyncs),
             static_cast<unsigned long long>(turnEmptyTakes), t.x, t.y, t.z, t.w,
             static_cast<unsigned long long>(fovMismatchFrames.load()));
        if (focusCull)
            logf("views4: focus cull: frusta built %llu, frames waiting for the first gaze sample %llu",
                 static_cast<unsigned long long>(cullBuilt), static_cast<unsigned long long>(cullWaitingForGaze));
        ReleaseSRWLockShared(&turnLock);
        const uint64_t calls = fillCalls.exchange(0), ticks = fillTicks.exchange(0), maxTicks = fillMaxTicks.exchange(0);
        logf("views4 cpu: fill_focus_cameras %.4f ms per call (max %.4f) x%llu",
             calls ? static_cast<double>(ticks) * msPerTick / static_cast<double>(calls) : 0.0,
             static_cast<double>(maxTicks) * msPerTick, static_cast<unsigned long long>(calls));
    }
}

}  // namespace aceqv

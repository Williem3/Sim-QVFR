#include "views.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../../quad/frame_records.h"
#include "abi.h"
#include "views_map.h"

namespace acc {
namespace {

// The engine's signatures, `this` first (x64: one calling convention). Passes are EStereoscopicPass (int);
// GetStereoProjectionMatrix returns its FMatrix through a hidden pointer after `this`.
struct FMatrix {
    float m[4][4];
};
using AdjustViewRectFn = void (*)(void* self, int pass, int* x, int* y, unsigned* sizeX, unsigned* sizeY);
using ProjectionFn = FMatrix* (*)(const void* self, FMatrix* out, int pass);
using EyePoseFn = bool (*)(void* self, int deviceId, int pass, void* orientation, void* position);

AdjustViewRectFn originalAdjust = nullptr;
ProjectionFn originalProjection = nullptr;
EyePoseFn originalEyePose = nullptr;
simqvfr::Views4Layout layout;
std::atomic<double> focusX{0}, focusY{0};  // the focus box (views_set_focus); 0: the focus views copy the eyes
std::atomic<std::uint64_t> focused{0};      // side-pass projections narrowed to it
std::atomic<int> targetWidth{0}, targetHeight{0};
std::atomic<std::uint64_t> adjustCalls[5], projectionCalls[5], poseCalls[5];
std::atomic<std::uint64_t> steamProjections{0}, steamPoses{0};  // calls that reached SteamVR (OpenComposite)
std::uint64_t frames = 0;

void count(std::atomic<std::uint64_t> (&calls)[5], int pass) {
    if (pass >= 0 && pass < 5)
        calls[pass].fetch_add(1, std::memory_order_relaxed);
}

int desired_views(void*, bool stereo) { return views_for(stereo); }
// The renderer's final rect per pass (FIntRect: min x, min y, max x, max y), for the log.
std::atomic<int> finalRect[5][4];
// The render thread's frame boundary: SetFinalViewRect for the right side pass, once per frame, from the main
// renderer only (FSceneRenderer::PrepareViewRectsForRendering, after the late update; planar reflections render
// views 0-1 alone). The next view the render thread hands over (primary_view) records the frame.
std::atomic<bool> recordArmed{false};
std::atomic<unsigned long> renderThread{0}, composeThread{0};
void final_view_rect(void*, int pass, const int* rect) {
    if (pass >= 0 && pass < 5)
        for (int i = 0; i < 4; ++i)
            finalRect[pass][i].store(rect[i], std::memory_order_relaxed);
    if (pass == kPassRightSide) {
        renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        recordArmed.store(true, std::memory_order_release);
    }
}
bool primary(void*, int pass) { return primary_pass(pass); }
void record_rendered(const unsigned char* view);
bool primary_view(void*, const unsigned char* view) {
    if (recordArmed.load(std::memory_order_acquire) &&
        GetCurrentThreadId() == renderThread.load(std::memory_order_relaxed))
        record_rendered(view);
    return primary_pass(*reinterpret_cast<const int*>(view + kViewStereoPass));
}
int view_pass(const void*, bool stereo, unsigned view) { return pass_for_view(stereo, view); }
unsigned pass_view(const void*, int pass) { return view_for_pass(pass); }

// Live resolution (views_set_live_layout): the layout asked for, the one this frame's views get (the game thread
// latches it at the left eye's rect, the first of a frame's), and the last layouts handed out, for the render thread
// to tell which one each frame it records was rendered with (match_layout).
struct LiveLayouts {
    std::mutex mutex;
    simqvfr::Views4Layout pending, frame;
    bool pendingSet = false;
    static constexpr int kKept = 8;
    HandedLayout handed[kKept];  // oldest first
    int count = 0;
    std::uint64_t matched = 0, unmatched = 0;  // records, since the last report
};
LiveLayouts liveLayouts;

// Under liveLayouts.mutex: `h` handed out from now on, unless its sizes are the newest's.
void remember_layout(const HandedLayout& h) {
    LiveLayouts& l = liveLayouts;
    if (l.count > 0 && same_sizes(l.handed[l.count - 1], h))
        return;
    if (l.count == LiveLayouts::kKept) {
        for (int i = 1; i < l.count; ++i)
            l.handed[i - 1] = l.handed[i];
        --l.count;
    }
    l.handed[l.count++] = h;
}

// The target's size from the engine's own code: it gives the left eye half of it (CeilToInt(ideal size x
// pixel density)); then the pass's view in the four-view layout (the launch one, or a live one from the frame's
// left eye on).
void adjust_view_rect(void* self, int pass, int* x, int* y, unsigned* sizeX, unsigned* sizeY) {
    count(adjustCalls, pass);
    if (pass == kPassFull) {
        originalAdjust(self, pass, x, y, sizeX, sizeY);
        return;
    }
    int x0 = 0, y0 = 0;
    unsigned half = 0, height = 0;
    originalAdjust(self, kPassLeft, &x0, &y0, &half, &height);
    const int width = static_cast<int>(2 * half);
    targetWidth.store(width, std::memory_order_relaxed);
    targetHeight.store(static_cast<int>(height), std::memory_order_relaxed);
    simqvfr::Views4Layout frameLayout;
    {
        std::lock_guard guard(liveLayouts.mutex);
        if (pass == kPassLeft) {
            if (liveLayouts.pendingSet) {
                liveLayouts.frame = liveLayouts.pending;
                liveLayouts.pendingSet = false;
            }
            remember_layout(handed_layout(liveLayouts.frame, width, static_cast<int>(height)));
        }
        frameLayout = liveLayouts.frame;
    }
    const PassRect r = pass_rect(frameLayout, pass, width, static_cast<int>(height));
    *x += r.x;
    *y += r.y;
    *sizeX = static_cast<unsigned>(r.width);
    *sizeY = static_cast<unsigned>(r.height);
}

// A side pass starts from its eye: it takes what its eye got last (the eyes come first in every frame) instead of
// asking SteamVR again: OpenComposite aborts when asked while it rebuilds its session (no cached views;
// docs/recon-acc.md), so the side views keep its calls to what ACC makes without them. Its projection is then narrowed
// to the focus box; its pose stays its eye's.
struct EyeCache {
    std::mutex mutex;
    bool projection[3] = {}, pose[3] = {}, poseOk[3] = {};
    FMatrix matrix[3] = {};
    unsigned char orientation[3][16] = {}, position[3][12] = {};  // FQuat, FVector
};
EyeCache cache;
// OpenXR's session runs (views_session_running): only then is SteamVR (OpenComposite) asked.
std::atomic<bool> sessionRunning{false};
std::atomic<std::uint64_t> served{0};  // answers from the cache while no session ran

// The focus turn (focus_turn.h, as ACE's): each frame's chosen at its first side-pass pose (UE asks for a view's
// eye pose, then its projection, on the game thread: ULocalPlayer::GetProjectionData), applied to both side passes'
// poses, until the right side pass's projection (once per frame, after its pose). The late update on the render
// thread only adds the head's movement since (DefaultXRCamera.cpp), so the turn chosen here is the one rendered;
// the composition takes it from the rendered views (record_rendered), not from here: the game thread runs a frame
// or two ahead of the composition by an amount no count tells.
struct FocusFrame {
    std::mutex mutex;
    float gaze[2][2] = {};  // tangents of each eye's view, the latest from xrLocateViews (views_set_gaze)
    bool gazeValid[2] = {};
    bool latched = false;
    simqvfr::Views4Frame frame{};
};
FocusFrame focusFrame;
std::atomic<std::uint64_t> turned{0};  // side poses turned
// Recorded by the render thread as it renders a frame (record_rendered), taken at that frame's xrEndFrame on the
// same thread (D3D11 has no RHI thread here): none in flight at a composition.
simqvfr::FrameRecords<simqvfr::Views4Frame> records{0, 3};

// This frame's tangents and turns (under focusFrame.mutex): the eyes' cached projections, the focus box, the gaze.
void latch_frame() {
    float matrix[2][16] = {};
    bool have[2] = {};
    {
        std::lock_guard guard(cache.mutex);
        for (int e = 0; e < 2; ++e) {
            have[e] = cache.projection[kPassLeft + e];
            std::memcpy(matrix[e], cache.matrix[kPassLeft + e].m, sizeof(matrix[e]));
        }
    }
    simqvfr::Views4Frame& f = focusFrame.frame;
    f = {};
    const double boxX = focusX.load(std::memory_order_relaxed), boxY = focusY.load(std::memory_order_relaxed);
    for (int e = 0; e < 2; ++e) {
        f.turn[e] = {0, 0, 0, 1};
        if (!have[e] || !ue_projection_tangents(matrix[e], f.outerTan[e]))
            continue;  // no projection yet: an empty record (the composition uses the submitted FOV)
        if (boxX <= 0) {
            std::memcpy(f.focusTan[e], f.outerTan[e], sizeof(f.focusTan[e]));
            continue;
        }
        double hw = 0, hh = 0;
        simqvfr::focus_extents(f.outerTan[e], boxX, boxY, hw, hh);
        const float focusTan[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                   static_cast<float>(-hh)};
        std::memcpy(f.focusTan[e], focusTan, sizeof(focusTan));
        if (focusFrame.gazeValid[e])
            f.turn[e] = simqvfr::focus_turn(f.outerTan[e], boxX, boxY, focusFrame.gaze[e][0], focusFrame.gaze[e][1]);
    }
    focusFrame.latched = true;
}

// Diagnostics: each view's projection as projection() handed it to UE, and as the rendered view holds it (with and
// without the temporal jitter), as tangents (left, right, up, down), logged every kReportFrames.
struct ProjectionLog {
    std::mutex mutex;
    float handed[4][16] = {}, stored[4][16] = {}, jittered[4][16] = {};
};
ProjectionLog projectionLog;

void eye_projection(const void* self, FMatrix* out, int pass, int eye) {
    const bool idle = !sessionRunning.load(std::memory_order_acquire);
    if ((pass != eye || idle) && (eye == kPassLeft || eye == kPassRight)) {
        std::lock_guard guard(cache.mutex);
        if (cache.projection[eye]) {
            *out = cache.matrix[eye];
            served.fetch_add(idle ? 1 : 0, std::memory_order_relaxed);
            return;
        }
    }
    steamProjections.fetch_add(1, std::memory_order_relaxed);
    originalProjection(self, out, eye);
    if (eye == kPassLeft || eye == kPassRight) {
        std::lock_guard guard(cache.mutex);
        cache.matrix[eye] = *out;
        cache.projection[eye] = true;
    }
}
FMatrix* projection(const void* self, FMatrix* out, int pass) {
    count(projectionCalls, pass);
    const int eye = eye_pass(pass);
    eye_projection(self, out, pass, eye);
    if (pass != eye && focus_projection(reinterpret_cast<float(&)[16]>(out->m), focusX.load(std::memory_order_relaxed),
                                        focusY.load(std::memory_order_relaxed)))
        focused.fetch_add(1, std::memory_order_relaxed);
    if (pass >= kPassLeft && pass <= kPassRightSide) {
        std::lock_guard guard(projectionLog.mutex);
        std::memcpy(projectionLog.handed[pass - kPassLeft], out->m, sizeof(projectionLog.handed[0]));
    }
    if (pass == kPassRightSide) {  // the frame's last view: the next side pose chooses the next frame's turn
        std::lock_guard guard(focusFrame.mutex);
        focusFrame.latched = false;
    }
    return out;
}
// The frame being rendered, recorded from its views as they are drawn (the render thread, after the late update):
// the composition, at the same frame's xrEndFrame, places the focus views exactly where they were rendered whatever
// the game thread chose meanwhile. Only a family of four views is the main one (captures and reflections have one
// or two); a mirrored or unreadable one is skipped (rendered_frame).
std::atomic<std::uint64_t> renderedRecords{0}, renderedSkipped{0};
std::atomic<bool> familyOrderLogged{false};
void record_rendered(const unsigned char* view) {
    const auto* family = *reinterpret_cast<const unsigned char* const*>(view);  // FSceneView::Family
    if (!family)
        return;
    const auto* const* views = *reinterpret_cast<const unsigned char* const* const*>(family);  // Views.Data
    if (*reinterpret_cast<const int*>(family + 8) != kViews || !views)  // Views.Num
        return;
    if (!recordArmed.exchange(false, std::memory_order_acq_rel))
        return;
    // Each view by its own StereoPass, not by its place in the family: view v is pass kPassLeft + v.
    float matrices[4][16], projections[4][16];
    int order[4] = {};
    bool seen[4] = {};
    for (int i = 0; i < 4; ++i) {
        const int pass = *reinterpret_cast<const int*>(views[i] + kViewStereoPass);
        order[i] = pass;
        const int v = pass - kPassLeft;
        if (v < 0 || v > 3 || seen[v]) {
            renderedSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        seen[v] = true;
        std::memcpy(matrices[v], views[i] + kViewMatrix, sizeof(matrices[v]));
        std::memcpy(projections[v], views[i] + kViewProjectionNoAA, sizeof(projections[v]));
        std::lock_guard guard(projectionLog.mutex);
        std::memcpy(projectionLog.stored[v], projections[v], sizeof(projectionLog.stored[v]));
        std::memcpy(projectionLog.jittered[v], views[i] + kViewProjection, sizeof(projectionLog.jittered[v]));
    }
    if (!familyOrderLogged.exchange(true))
        simqvfr::logf("acc views: the rendered family's views are passes %d, %d, %d, %d", order[0], order[1], order[2],
                    order[3]);
    simqvfr::Views4Frame frame{};
    if (rendered_frame(matrices, projections, frame)) {
        // The layout it was rendered with, from its final rects (stored by this frame's SetFinalViewRect calls, on
        // this thread): a live one goes into the record, so the composition reads its views where they were drawn.
        int finals[kViews][4];
        for (int v = 0; v < kViews; ++v)
            for (int i = 0; i < 4; ++i)
                finals[v][i] = finalRect[kPassLeft + v][i].load(std::memory_order_relaxed);
        {
            std::lock_guard guard(liveLayouts.mutex);
            int found = match_layout(liveLayouts.handed, liveLayouts.count, finals);
            if (found >= 0)
                ++liveLayouts.matched;
            else
                ++liveLayouts.unmatched;
            if (found < 0)
                found = liveLayouts.count - 1;  // none (a frame of two layouts): the newest
            if (found >= 0 && liveLayouts.handed[found].layout.live) {
                const simqvfr::Views4Layout& l = liveLayouts.handed[found].layout;
                frame.live = true;
                std::memcpy(frame.width, l.width, sizeof(frame.width));
                std::memcpy(frame.height, l.height, sizeof(frame.height));
                std::memcpy(frame.renderWidth, l.width, sizeof(frame.renderWidth));
                std::memcpy(frame.renderHeight, l.height, sizeof(frame.renderHeight));
            }
        }
        records.push(frame);
        renderedRecords.fetch_add(1, std::memory_order_relaxed);
    } else {
        renderedSkipped.fetch_add(1, std::memory_order_relaxed);
    }
}

bool eye_pose(void* self, int deviceId, int pass, void* orientation, void* position) {
    count(poseCalls, pass);
    const int eye = eye_pass(pass);
    const bool hmdEye = deviceId == 0 && (eye == kPassLeft || eye == kPassRight);
    const bool idle = !sessionRunning.load(std::memory_order_acquire);
    bool ok = false, cached = false;
    if (hmdEye && (pass != eye || idle)) {
        std::lock_guard guard(cache.mutex);
        if (cache.pose[eye]) {
            std::memcpy(orientation, cache.orientation[eye], sizeof(cache.orientation[eye]));
            std::memcpy(position, cache.position[eye], sizeof(cache.position[eye]));
            served.fetch_add(idle ? 1 : 0, std::memory_order_relaxed);
            ok = cache.poseOk[eye];
            cached = true;
        }
    }
    if (!cached) {
        steamPoses.fetch_add(1, std::memory_order_relaxed);
        ok = originalEyePose(self, deviceId, eye, orientation, position);
        if (hmdEye) {
            std::lock_guard guard(cache.mutex);
            std::memcpy(cache.orientation[eye], orientation, sizeof(cache.orientation[eye]));
            std::memcpy(cache.position[eye], position, sizeof(cache.position[eye]));
            cache.poseOk[eye] = ok;
            cache.pose[eye] = true;
        }
    }
    if (hmdEye && pass != eye && ok && focusX.load(std::memory_order_relaxed) > 0) {
        UeQuat turn;
        {
            std::lock_guard guard(focusFrame.mutex);
            if (!focusFrame.latched)
                latch_frame();
            turn = ue_quat(focusFrame.frame.turn[eye - kPassLeft]);
        }
        UeQuat o;
        std::memcpy(&o, orientation, sizeof(o));
        o = ue_mul(o, turn);  // turned in the eye's own frame
        std::memcpy(orientation, &o, sizeof(o));
        turned.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

std::atomic<bool> started{false};

// The 7 bytes at a kRightEyePicks site, and that GetStereoEyeView is called right after (directly, or after
// `mov rcx, [rcx]`).
bool right_eye_picks_match() {
    const std::uintptr_t base = simqvfr::module_base();
    for (const RightEyePick& p : kRightEyePicks) {
        const std::uint8_t pick[7] = {0xf6, 0xd8, 0x1b, 0xd2, 0x83, 0xe2, 0x02};
        std::uint8_t anchor[7];
        std::memcpy(anchor, pick, sizeof(anchor));
        if (p.passInEax)
            anchor[0] = 0xf7;  // neg eax
        const std::uint8_t loadFamily[3] = {0x48, 0x8b, 0x09};
        const std::uintptr_t call = p.rva + 7 + (simqvfr::code_matches(p.rva + 7, loadFamily, 3) ? 3 : 0);
        std::int32_t rel = 0;
        const bool ok = simqvfr::code_matches(p.rva, anchor, sizeof(anchor)) &&
                        *reinterpret_cast<const std::uint8_t*>(base + call) == 0xe8 &&
                        (std::memcpy(&rel, reinterpret_cast<const void*>(base + call + 1), 4), call + 5 + rel == kGetStereoEyeView);
        if (!ok) {
            simqvfr::logf("acc views: no right-eye pick at 0x%llx", static_cast<unsigned long long>(p.rva));
            return false;
        }
    }
    return true;
}

// Each kRightEyePicks site: edx = (al, or eax) ? (View.StereoPass + 1) & ~1 : 0.
bool pair_right_views() {
    bool ok = true;
    for (const RightEyePick& p : kRightEyePicks) {
        std::uint8_t anchor[7] = {0xf6, 0xd8, 0x1b, 0xd2, 0x83, 0xe2, 0x02};
        if (p.passInEax)
            anchor[0] = 0xf7;
        std::uint8_t stub[24];
        std::size_t n = 0;
        stub[n++] = p.passInEax ? 0x85 : 0x84;  // test eax, eax / test al, al
        stub[n++] = 0xc0;
        stub[n++] = 0x74;  // jz zero (offset below)
        const std::size_t jz = n++;
        if (p.view >= 8)
            stub[n++] = 0x41;
        stub[n++] = 0x8b;  // mov edx, [view + 0xad0]
        stub[n++] = static_cast<std::uint8_t>(0x90 | (p.view & 7));
        const std::uint32_t disp = static_cast<std::uint32_t>(kViewStereoPass);
        std::memcpy(stub + n, &disp, 4);
        n += 4;
        const std::uint8_t pairRight[] = {0xff, 0xc2, 0x83, 0xe2, 0xfe, 0xeb, 0x02};  // inc edx; and edx, -2; jmp done
        std::memcpy(stub + n, pairRight, sizeof(pairRight));
        n += sizeof(pairRight);
        stub[jz] = static_cast<std::uint8_t>(n - (jz + 1));
        stub[n++] = 0x33;  // zero: xor edx, edx
        stub[n++] = 0xd2;
        ok = simqvfr::code_redirect(p.rva, anchor, sizeof(anchor), stub, n) && ok;
    }
    return ok;
}

bool counts_match() {
    for (const CountRead& r : kInstancedViewCounts)
        if (!simqvfr::code_matches(r.rva, r.bytes, r.size)) {
            simqvfr::logf("acc views: no instanced view count read at 0x%llx", static_cast<unsigned long long>(r.rva));
            return false;
        }
    for (const HeldCvar& c : kHeldCvars)
        if (!simqvfr::code_matches(c.ref, c.refAnchor, sizeof(c.refAnchor)) ||
            !simqvfr::code_matches(c.store, c.storeAnchor, sizeof(c.storeAnchor)))
            return false;
    if (!simqvfr::code_matches(kPreExposureSecondary, kPreExposureSecondaryAnchor, sizeof(kPreExposureSecondaryAnchor)) ||
        !simqvfr::code_matches(kPreExposurePrimary[0], kPreExposurePrimaryAnchor[0], sizeof(kPreExposurePrimaryAnchor[0])) ||
        !simqvfr::code_matches(kPreExposurePrimary[1], kPreExposurePrimaryAnchor[1], sizeof(kPreExposurePrimaryAnchor[1])))
        return false;
    if (!simqvfr::code_matches(kTonemapperVignette, kTonemapperVignetteAnchor, sizeof(kTonemapperVignetteAnchor)))
        return false;
    for (std::uintptr_t rva : kFoliageLodProjection)
        if (!simqvfr::code_matches(rva, kFoliageLodProjectionAnchor, sizeof(kFoliageLodProjectionAnchor)))
            return false;
    if (!simqvfr::code_matches(kFoliageShaderLodProjection, kFoliageShaderLodProjectionAnchor,
                             sizeof(kFoliageShaderLodProjectionAnchor)))
        return false;
    if (!simqvfr::code_matches(kShadowCascadesCreate, kShadowCascadesCreateAnchor, sizeof(kShadowCascadesCreateAnchor)) ||
        !simqvfr::code_matches(kShadowVisibilitySecondary, kShadowVisibilitySecondaryAnchor,
                             sizeof(kShadowVisibilitySecondaryAnchor)) ||
        !simqvfr::code_matches(kShadowVisibilityPrimary, kShadowVisibilityPrimaryAnchor, sizeof(kShadowVisibilityPrimaryAnchor)))
        return false;
    if (!simqvfr::code_matches(kExposureSecondary, kExposureSecondaryAnchor, sizeof(kExposureSecondaryAnchor)) ||
        !simqvfr::code_matches(kExposurePrimary[0], kExposurePrimaryAnchor[0], sizeof(kExposurePrimaryAnchor[0])) ||
        !simqvfr::code_matches(kExposurePrimary[1], kExposurePrimaryAnchor[1], sizeof(kExposurePrimaryAnchor[1])))
        return false;
    return simqvfr::code_matches(kViewInstancedCount, kViewInstancedCountAnchor, sizeof(kViewInstancedCountAnchor)) &&
           simqvfr::code_matches(kStereoPassIndexCall, kStereoPassIndexCallAnchor, sizeof(kStereoPassIndexCallAnchor)) &&
           simqvfr::code_matches(kStereoPassIndexStore, kStereoPassIndexStoreAnchor, sizeof(kStereoPassIndexStoreAnchor)) &&
           right_eye_picks_match() &&
           simqvfr::code_matches(kFoliageUnionGate, kFoliageUnionGateAnchor, sizeof(kFoliageUnionGateAnchor)) &&
           simqvfr::code_matches(kFoliageLeftView, kFoliageLeftViewAnchor, sizeof(kFoliageLeftViewAnchor)) &&
           simqvfr::code_matches(kFoliageRightView, kFoliageRightViewAnchor, sizeof(kFoliageRightViewAnchor)) &&
           simqvfr::code_matches(kFoliageRightViewRead, kFoliageRightViewReadAnchor, sizeof(kFoliageRightViewReadAnchor));
}

// Two views per instanced draw (abi.h), so the family's four views draw as two instanced pairs.
bool pairs_per_draw() {
    bool ok = true;
    for (const CountRead& r : kInstancedViewCounts) {
        const std::uint8_t reg = (r.bytes[r.size - 2] >> 3) & 7;  // ModRM reg
        const std::uint8_t three[3] = {0x6a, 0x02, static_cast<std::uint8_t>(0x58 + reg)};        // push 2; pop r64
        const std::uint8_t four[4] = {0x6a, 0x02, 0x41, static_cast<std::uint8_t>(0x58 + reg)};   // push 2; pop r8-r15
        ok = simqvfr::code_write(r.rva, r.size == 3 ? three : four, r.size) && ok;
    }
    // cmp ecx, 2; jle +5; mov ecx, 2; then the moved `mov rdi, [rbp + 0xe8]`.
    const std::uint8_t clamp[] = {0x83, 0xf9, 0x02, 0x7e, 0x05, 0xb9, 0x02, 0x00, 0x00, 0x00,
                                  0x48, 0x8b, 0xbd, 0xe8, 0x00, 0x00, 0x00};
    ok = simqvfr::code_redirect(kViewInstancedCount, kViewInstancedCountAnchor, sizeof(kViewInstancedCountAnchor), clamp,
                              sizeof(clamp)) && ok;
    // StereoPassIndex: the eye within its pair (and eax, 1; then the moved store).
    const std::uint8_t pairEye[] = {0x83, 0xe0, 0x01, 0x89, 0x86, 0x5c, 0x0c, 0x00, 0x00};
    ok = simqvfr::code_redirect(kStereoPassIndexStore, kStereoPassIndexStoreAnchor, sizeof(kStereoPassIndexStoreAnchor),
                              pairEye, sizeof(pairEye)) && ok;
    ok = pair_right_views() && ok;
    // Exposure (abi.h): every view but the left eye takes its exposure, and only the left eye computes it.
    const std::uint8_t secondary[] = {0x83, 0xb9, 0xd0, 0x0a, 0x00, 0x00, 0x02, 0x0f, 0x9d, 0xc0};  // cmp [rcx+ad0],2; setge al
    const std::uint8_t primary[] = {0x83, 0xb9, 0xd0, 0x0a, 0x00, 0x00, 0x01, 0x0f, 0x9e, 0xc0};    // cmp [rcx+ad0],1; setle al
    ok = simqvfr::code_redirect(kExposureSecondary, kExposureSecondaryAnchor, sizeof(kExposureSecondaryAnchor), secondary,
                              sizeof(secondary)) && ok;
    for (int i = 0; i < 2; ++i)
        ok = simqvfr::code_redirect(kExposurePrimary[i], kExposurePrimaryAnchor[i], sizeof(kExposurePrimaryAnchor[i]), primary,
                                  sizeof(primary)) && ok;
    // No tonemapper vignette (abi.h): `xor al, al` and an 8-byte nop in place of the test.
    const std::uint8_t noVignette[] = {0x32, 0xc0, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    ok = simqvfr::code_write(kTonemapperVignette, noVignette, sizeof(noVignette)) && ok;
    // Pre-exposure (abi.h): the left eye's reaches every other view.
    ok = simqvfr::code_redirect(kPreExposureSecondary, kPreExposureSecondaryAnchor, sizeof(kPreExposureSecondaryAnchor),
                              secondary, sizeof(secondary)) && ok;
    for (int i = 0; i < 2; ++i)
        ok = simqvfr::code_redirect(kPreExposurePrimary[i], kPreExposurePrimaryAnchor[i], sizeof(kPreExposurePrimaryAnchor[i]),
                                  primary, sizeof(primary)) && ok;
    // The sun's cascades (abi.h): only the left eye creates them, and every view projects them.
    const std::uint8_t leftEyeCascades[] = {0x41, 0x83, 0xbe, 0xd0, 0x0a, 0x00, 0x00, 0x01, 0x0f, 0x96, 0xc0};
    ok = simqvfr::code_write(kShadowCascadesCreate, leftEyeCascades, sizeof(leftEyeCascades)) && ok;
    ok = simqvfr::code_redirect(kShadowVisibilitySecondary, kShadowVisibilitySecondaryAnchor,
                              sizeof(kShadowVisibilitySecondaryAnchor), secondary, sizeof(secondary)) && ok;
    ok = simqvfr::code_redirect(kShadowVisibilityPrimary, kShadowVisibilityPrimaryAnchor, sizeof(kShadowVisibilityPrimaryAnchor),
                              primary, sizeof(primary)) && ok;
    // Foliage: each pair's primary culls with its own left plane and its partner's right (abi.h).
    const std::uint8_t gate[] = {0xf7, 0x44, 0x24, 0x38, 0xfd, 0xff, 0xff, 0xff};  // test dword [rsp + 0x38], ~2
    const std::uint8_t left[] = {0x48, 0x63, 0x44, 0x24, 0x38,  // movsxd rax, dword [rsp + 0x38]
                                 0x48, 0x8b, 0x04, 0xc1,        // mov rax, [rcx + rax*8]
                                 0x41, 0x0f, 0x28, 0xe7};       // movaps xmm4, xmm15
    const std::uint8_t right[] = {0x48, 0x63, 0x44, 0x24, 0x38,  // movsxd rax, dword [rsp + 0x38]
                                  0x48, 0x83, 0xc0, 0x02,        // add rax, 2
                                  0x0f, 0x59, 0xc7};             // mulps xmm0, xmm7
    ok = simqvfr::code_redirect(kFoliageUnionGate, kFoliageUnionGateAnchor, sizeof(kFoliageUnionGateAnchor), gate,
                              sizeof(gate)) && ok;
    ok = simqvfr::code_redirect(kFoliageLeftView, kFoliageLeftViewAnchor, sizeof(kFoliageLeftViewAnchor), left,
                              sizeof(left)) && ok;
    ok = simqvfr::code_redirect(kFoliageRightView, kFoliageRightViewAnchor, sizeof(kFoliageRightViewAnchor), right,
                              sizeof(right)) && ok;
    // Foliage LOD distances from the left eye's projection in the focus views, on the CPU and in the shader (abi.h).
    const std::uint8_t cpuLod[] = {0x4d, 0x8d, 0x87, 0x80, 0x02, 0x00, 0x00,        // lea r8, [r15 + 0x280]
                                   0x41, 0x83, 0xbf, 0xd0, 0x0a, 0x00, 0x00, 0x03,  // cmp dword [r15 + 0xad0], 3
                                   0x72, 0x10,                                      // jb done
                                   0x4d, 0x8b, 0x07,                                // mov r8, [r15]  (Family)
                                   0x4d, 0x8b, 0x00,                                // mov r8, [r8]   (Views.Data)
                                   0x4d, 0x8b, 0x00,                                // mov r8, [r8]   (Views[0])
                                   0x49, 0x81, 0xc0, 0x80, 0x02, 0x00, 0x00};       // add r8, 0x280
    for (std::uintptr_t rva : kFoliageLodProjection)
        ok = simqvfr::code_redirect(rva, kFoliageLodProjectionAnchor, sizeof(kFoliageLodProjectionAnchor), cpuLod,
                                  sizeof(cpuLod)) && ok;
    const std::uint8_t shaderLod[] = {0x41, 0x83, 0xbe, 0xd0, 0x0a, 0x00, 0x00, 0x03,  // cmp dword [r14 + 0xad0], 3
                                      0x72, 0x09,                                      // jb own
                                      0x4d, 0x8b, 0x36,                                // mov r14, [r14]  (Family)
                                      0x4d, 0x8b, 0x36,                                // mov r14, [r14]  (Views.Data)
                                      0x4d, 0x8b, 0x36,                                // mov r14, [r14]  (Views[0])
                                      0x49, 0x81, 0xc6, 0x80, 0x02, 0x00, 0x00};       // own: add r14, 0x280
    ok = simqvfr::code_redirect(kFoliageShaderLodProjection, kFoliageShaderLodProjectionAnchor,
                              sizeof(kFoliageShaderLodProjectionAnchor), shaderLod, sizeof(shaderLod)) && ok;
    simqvfr::logf("acc views: %s", ok ? "two views per instanced draw (21 count reads, view constants clamped, "
                                      "StereoPassIndex the eye within its pair, 12 right-eye picks per pair, "
                                      "foliage culled per pair and its LOD the left eye's in the focus views, "
                                      "the sun's shadow cascades the left eye's in every view)"
                                    : "a views-per-draw patch failed");
    return ok;
}

}  // namespace

bool views_start(const simqvfr::Views4Layout& l, double boxX, double boxY) {
    if (started.load())
        return true;
    const Slot slots[] = {kGetDesiredNumberOfViews, kGetViewPassForIndex, kGetViewIndexForPass, kAdjustViewRect,
                          kGetStereoProjectionMatrix, kGetRelativeEyePose, kDeviceIsAPrimaryPass, kDeviceIsAPrimaryView,
                          kSetFinalViewRect};
    for (const Slot& s : slots)
        if (simqvfr::vtable_slot(s.rva) != s.function) {
            simqvfr::logf("acc views: slot 0x%llx holds 0x%llx, expected 0x%llx: not the build these addresses are "
                        "from; nothing patched",
                        static_cast<unsigned long long>(s.rva), static_cast<unsigned long long>(simqvfr::vtable_slot(s.rva)),
                        static_cast<unsigned long long>(s.function));
            return false;
        }
    if (!simqvfr::code_matches(kAllowSubPrimitiveQueriesRef, kAllowSubPrimitiveQueriesRefAnchor,
                             sizeof(kAllowSubPrimitiveQueriesRefAnchor))) {
        simqvfr::log_line("acc views: r.AllowSubPrimitiveQueries' registration isn't this build's; nothing patched");
        return false;
    }
    if (!counts_match()) {
        simqvfr::log_line("acc views: the views-per-draw code isn't this build's; nothing patched");
        return false;
    }
    const std::uintptr_t base = simqvfr::module_base();
    // Foliage's per-cluster occlusion results crash with the side views on track (abi.h): one query per proxy.
    auto* subPrimitiveQueries = reinterpret_cast<volatile std::int32_t*>(base + kAllowSubPrimitiveQueries);
    simqvfr::logf("acc views: r.AllowSubPrimitiveQueries %d -> 0 (foliage occlusion per proxy, not per cluster)",
                static_cast<int>(*subPrimitiveQueries));
    *subPrimitiveQueries = 0;
    layout = l;
    {
        std::lock_guard guard(liveLayouts.mutex);
        liveLayouts.frame = l;
        liveLayouts.pendingSet = false;
        liveLayouts.count = 0;
    }
    views_set_focus(boxX, boxY);
    originalAdjust = reinterpret_cast<AdjustViewRectFn>(base + kAdjustViewRect.function);
    originalProjection = reinterpret_cast<ProjectionFn>(base + kGetStereoProjectionMatrix.function);
    originalEyePose = reinterpret_cast<EyePoseFn>(base + kGetRelativeEyePose.function);
    // Pose, projection, rects and pairs before the count, so no frame sees four views without them.
    bool ok = pairs_per_draw();
    ok = ok && simqvfr::vtable_slot_replace(kGetRelativeEyePose.rva, kGetRelativeEyePose.function,
                                         reinterpret_cast<void*>(&eye_pose));
    ok = ok && simqvfr::vtable_slot_replace(kGetStereoProjectionMatrix.rva, kGetStereoProjectionMatrix.function,
                                          reinterpret_cast<void*>(&projection));
    ok = ok && simqvfr::vtable_slot_replace(kAdjustViewRect.rva, kAdjustViewRect.function,
                                          reinterpret_cast<void*>(&adjust_view_rect));
    ok = ok && simqvfr::vtable_slot_replace(kSetFinalViewRect.rva, kSetFinalViewRect.function,
                                          reinterpret_cast<void*>(&final_view_rect));
    ok = ok && simqvfr::vtable_slot_replace(kDeviceIsAPrimaryView.rva, kDeviceIsAPrimaryView.function,
                                          reinterpret_cast<void*>(&primary_view));
    ok = ok && simqvfr::vtable_slot_replace(kDeviceIsAPrimaryPass.rva, kDeviceIsAPrimaryPass.function,
                                          reinterpret_cast<void*>(&primary));
    ok = ok && simqvfr::vtable_slot_replace(kGetViewIndexForPass.rva, kGetViewIndexForPass.function,
                                          reinterpret_cast<void*>(&pass_view));
    ok = ok && simqvfr::vtable_slot_replace(kGetViewPassForIndex.rva, kGetViewPassForIndex.function,
                                          reinterpret_cast<void*>(&view_pass));
    ok = ok && simqvfr::vtable_slot_replace(kGetDesiredNumberOfViews.rva, kGetDesiredNumberOfViews.function,
                                          reinterpret_cast<void*>(&desired_views));
    simqvfr::logf("acc views: %s; layout widths %.3f %.3f %.3f %.3f heights %.3f %.3f %.3f %.3f of the target",
                ok ? (boxX > 0 ? "four views patched in (views 2-3 focus views, centred)"
                               : "four views patched in (views 2-3 copy the eyes)")
                   : "a slot write failed",
                l.width[0], l.width[1], l.width[2], l.width[3], l.height[0], l.height[1], l.height[2], l.height[3]);
    started.store(ok);
    return ok;
}

void views_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]) {
    std::lock_guard guard(focusFrame.mutex);
    std::memcpy(focusFrame.gaze, tan, sizeof(focusFrame.gaze));
    std::memcpy(focusFrame.gazeValid, valid, sizeof(focusFrame.gazeValid));
}

bool views_take_frame(simqvfr::Views4Frame& frame) {
    composeThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    bool drifted = false;
    std::uint64_t behind = 0;
    const bool any = records.take(frame, &drifted, &behind);
    if (drifted)
        simqvfr::logf("acc views: frame records stayed %llu behind the compositions: resynced",
                    static_cast<unsigned long long>(behind));
    return any;
}

bool views_latest_frame(simqvfr::Views4Frame& frame) { return records.latest(frame); }

void views_set_focus(double boxX, double boxY) {
    const double y = boxY > 0 ? boxY : boxX;
    const double oldX = focusX.exchange(boxX, std::memory_order_relaxed);
    const double oldY = focusY.exchange(y, std::memory_order_relaxed);
    if (oldX == boxX && oldY == y)
        return;  // the layer passes the live box again now and then
    simqvfr::logf("acc views: focus box %.0f%% x %.0f%% of each eye's tangent extent%s", boxX * 100,
                y * 100, boxX > 0 ? ", centred" : " (none: the focus views copy the eyes)");
}

void views_set_live_layout(const simqvfr::Views4Layout& l) {
    {
        std::lock_guard guard(liveLayouts.mutex);
        liveLayouts.pending = l;
        liveLayouts.pendingSet = true;
    }
    simqvfr::logf("acc views: live layout: every view %.3f of the target wide, outer %.3f and focus %.3f of its height "
                "(from the next frame UE sets up)",
                l.width[0], l.height[0], l.height[2]);
}

void views_session_running(bool running) {
    sessionRunning.store(running, std::memory_order_release);
    simqvfr::logf("acc views: OpenXR session %s; %llu projection/pose answers came from the cache while none ran",
                running ? "running: SteamVR answers again" : "not running: answers from the cache",
                static_cast<unsigned long long>(served.exchange(0)));
}

void views_target(int (&size)[2]) {
    size[0] = targetWidth.load(std::memory_order_relaxed);
    size[1] = targetHeight.load(std::memory_order_relaxed);
}

bool views_view_rect(int view, int (&rect)[4]) {
    if (view < 0 || view >= kViews)
        return false;
    const auto& r = finalRect[kPassLeft + view];
    const int x0 = r[0].load(std::memory_order_relaxed), y0 = r[1].load(std::memory_order_relaxed);
    const int x1 = r[2].load(std::memory_order_relaxed), y1 = r[3].load(std::memory_order_relaxed);
    if (x1 <= x0 || y1 <= y0)
        return false;
    rect[0] = x0;
    rect[1] = y0;
    rect[2] = x1 - x0;
    rect[3] = y1 - y0;
    return true;
}

bool views_focused() { return started.load(std::memory_order_relaxed) && focusX.load(std::memory_order_relaxed) > 0; }

// abi.h kHeldCvars: each held at its value, both threads' copies, looked at every frame since ACC's graphics settings
// set them whenever they apply. The first change of each is logged.
std::atomic<bool> cvarLogged[std::size(kHeldCvars)];
void hold_cvars() {
    if (!started.load(std::memory_order_relaxed))
        return;
    for (std::size_t i = 0; i < std::size(kHeldCvars); ++i) {
        const HeldCvar& c = kHeldCvars[i];
        auto* data = *reinterpret_cast<int* const*>(simqvfr::module_base() + c.data);
        if (!data || (data[0] == c.value && data[1] == c.value))
            continue;
        if (!cvarLogged[i].exchange(true))
            simqvfr::logf("acc views: %s %d -> %d (screen-space lens effects off while QVFR runs)", c.name, data[1], c.value);
        data[0] = data[1] = c.value;
    }
}

// abi.h kDiagCvars, switched off live (views_set_diag): held at 0, the game's value kept to put back. Both threads'
// copies, as hold_cvars. Only this thread (xrEndFrame) reads or writes `diagSaved` / `diagHeld`.
std::atomic<bool> diagReady{false}, diagOff[std::size(kDiagCvars)];
int diagSaved[std::size(kDiagCvars)][2];
bool diagHeld[std::size(kDiagCvars)];
void hold_diag_cvars() {
    if (!diagReady.load(std::memory_order_relaxed))
        return;
    for (std::size_t i = 0; i < std::size(kDiagCvars); ++i) {
        const HeldCvar& c = kDiagCvars[i];
        auto* data = *reinterpret_cast<int* const*>(simqvfr::module_base() + c.data);
        if (!data)
            continue;
        if (diagOff[i].load(std::memory_order_relaxed)) {
            if (!diagHeld[i]) {
                diagSaved[i][0] = data[0];
                diagSaved[i][1] = data[1];
                diagHeld[i] = true;
                simqvfr::logf("acc diagnostics: %s %d -> %d (switched off)", c.name, data[1], c.value);
            }
            data[0] = data[1] = c.value;
        } else if (diagHeld[i]) {
            data[0] = diagSaved[i][0];
            data[1] = diagSaved[i][1];
            diagHeld[i] = false;
            simqvfr::logf("acc diagnostics: %s back to %d", c.name, data[1]);
        }
    }
}

void views_frame_end() {
    hold_cvars();
    hold_diag_cvars();
    if (!started.load() || ++frames % 900 != 0)
        return;
    const auto line = [](const char* what, std::atomic<std::uint64_t> (&calls)[5]) {
        simqvfr::logf("acc views: %s per pass full/left/right/left side/right side = %llu/%llu/%llu/%llu/%llu", what,
                    static_cast<unsigned long long>(calls[0].exchange(0)), static_cast<unsigned long long>(calls[1].exchange(0)),
                    static_cast<unsigned long long>(calls[2].exchange(0)), static_cast<unsigned long long>(calls[3].exchange(0)),
                    static_cast<unsigned long long>(calls[4].exchange(0)));
    };
    line("AdjustViewRect", adjustCalls);
    line("projection", projectionCalls);
    line("eye pose", poseCalls);
    for (int pass = 1; pass < 5; ++pass)
        simqvfr::logf("acc views: renderer's final rect for pass %d: %d,%d - %d,%d", pass, finalRect[pass][0].load(),
                    finalRect[pass][1].load(), finalRect[pass][2].load(), finalRect[pass][3].load());
    simqvfr::logf("acc views: target %dx%d over the last 900 frames; %llu projection and %llu eye pose calls reached "
                "SteamVR", targetWidth.load(), targetHeight.load(), static_cast<unsigned long long>(steamProjections.exchange(0)),
                static_cast<unsigned long long>(steamPoses.exchange(0)));
    simqvfr::logf("acc views: %llu side-pass projections narrowed to the focus box, %llu side poses turned",
                static_cast<unsigned long long>(focused.exchange(0)), static_cast<unsigned long long>(turned.exchange(0)));
    {
        std::lock_guard guard(liveLayouts.mutex);
        if (liveLayouts.count > 1 || liveLayouts.unmatched)
            simqvfr::logf("acc views: records matched to the layout they were rendered with: %llu, unmatched %llu "
                        "(given the newest); %d layouts kept",
                        static_cast<unsigned long long>(liveLayouts.matched),
                        static_cast<unsigned long long>(liveLayouts.unmatched), liveLayouts.count);
        liveLayouts.matched = liveLayouts.unmatched = 0;
    }
    const auto n = records.counts();
    simqvfr::Views4Frame last{};
    records.latest(last);
    // The latest rendered turns against what the game thread chose, as yaw / pitch in degrees (OpenXR: yaw left +).
    simqvfr::Views4Frame chosen{};
    {
        std::lock_guard guard(focusFrame.mutex);
        chosen = focusFrame.frame;
    }
    const auto yaw_pitch = [](const XrQuaternionf& q, double& yaw, double& pitch) {
        float m[9];
        simqvfr::to_matrix(q, m);  // the turned camera's forward is -column 2
        const double fx = -m[2], fy = -m[5], fz = -m[8];
        yaw = std::atan2(-fx, -fz) * 57.29577951;
        pitch = std::atan2(fy, std::sqrt(fx * fx + fz * fz)) * 57.29577951;
    };
    double ry[2], rp[2], cy[2], cp[2];
    for (int e = 0; e < 2; ++e) {
        yaw_pitch(last.turn[e], ry[e], rp[e]);
        yaw_pitch(chosen.turn[e], cy[e], cp[e]);
    }
    simqvfr::logf("acc views: focus turns, yaw/pitch deg: rendered left %.1f/%.1f right %.1f/%.1f; chosen left %.1f/%.1f "
                "right %.1f/%.1f",
                ry[0], rp[0], ry[1], rp[1], cy[0], cp[0], cy[1], cp[1]);
    {
        std::lock_guard guard(projectionLog.mutex);
        const auto tangents = [](const float (&m)[16], char* text, size_t size) {
            float t[4] = {};
            if (ue_projection_tangents(m, t))
                std::snprintf(text, size, "%.4f/%.4f/%.4f/%.4f", t[0], t[1], t[2], t[3]);
            else
                std::snprintf(text, size, "none");
        };
        for (int v = 0; v < 4; ++v) {
            char handed[64], stored[64], jittered[64];
            tangents(projectionLog.handed[v], handed, sizeof(handed));
            tangents(projectionLog.stored[v], stored, sizeof(stored));
            tangents(projectionLog.jittered[v], jittered, sizeof(jittered));
            simqvfr::logf("acc views: view %d projection tangents l/r/u/d: handed to UE %s; rendered view's %s, jittered %s",
                        v, handed, stored, jittered);
        }
    }
    simqvfr::logf("acc views: rendered frames recorded %llu (skipped %llu); render thread %lu, composing thread %lu",
                static_cast<unsigned long long>(renderedRecords.exchange(0)),
                static_cast<unsigned long long>(renderedSkipped.exchange(0)), renderThread.load(), composeThread.load());
    simqvfr::logf("acc views: frames recorded %llu, composed %llu (backlog %llu, resyncs %llu, composed before "
                "recording %llu); eye 0 turn %.3f,%.3f,%.3f,%.3f",
                static_cast<unsigned long long>(n.pushed), static_cast<unsigned long long>(n.taken),
                static_cast<unsigned long long>(n.pushed - n.taken), static_cast<unsigned long long>(n.resyncs),
                static_cast<unsigned long long>(n.emptyTakes), last.turn[0].x, last.turn[0].y, last.turn[0].z,
                last.turn[0].w);
}

bool views_diag_start() {
    for (const HeldCvar& c : kDiagCvars)
        if (!simqvfr::code_matches(c.ref, c.refAnchor, sizeof(c.refAnchor)) ||
            !simqvfr::code_matches(c.store, c.storeAnchor, sizeof(c.storeAnchor))) {
            simqvfr::logf("acc diagnostics: %s isn't where this build had it: the switches do nothing", c.name);
            return false;
        }
    diagReady = true;
    return true;
}

void views_set_diag(int which, bool off) {
    if (which >= 0 && which < static_cast<int>(std::size(kDiagCvars)))
        diagOff[which] = off;
}

}  // namespace acc

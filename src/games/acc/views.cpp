#include "views.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "../../engine/code_patch.h"
#include "../../log.h"
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
aceqv::Views4Layout layout;
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
void final_view_rect(void*, int pass, const int* rect) {
    if (pass >= 0 && pass < 5)
        for (int i = 0; i < 4; ++i)
            finalRect[pass][i].store(rect[i], std::memory_order_relaxed);
}
bool primary(void*, int pass) { return primary_pass(pass); }
bool primary_view(void*, const unsigned char* view) {
    return primary_pass(*reinterpret_cast<const int*>(view + kViewStereoPass));
}
int view_pass(const void*, bool stereo, unsigned view) { return pass_for_view(stereo, view); }
unsigned pass_view(const void*, int pass) { return view_for_pass(pass); }

// The target's size from the engine's own code: it gives the left eye half of it (CeilToInt(ideal size x
// pixel density)); then the pass's view in the four-view layout.
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
    const PassRect r = pass_rect(layout, pass, width, static_cast<int>(height));
    *x += r.x;
    *y += r.y;
    *sizeX = static_cast<unsigned>(r.width);
    *sizeY = static_cast<unsigned>(r.height);
}

// A side pass starts from its eye: it takes what its eye got last (the eyes come first in every frame) instead of
// asking SteamVR again: OpenComposite aborts when asked while it rebuilds its session (no cached views; README.md),
// so the side views keep its calls to what ACC makes without them. Its projection is then narrowed to the focus
// box; its pose stays its eye's.
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
    return out;
}
bool eye_pose(void* self, int deviceId, int pass, void* orientation, void* position) {
    count(poseCalls, pass);
    const int eye = eye_pass(pass);
    const bool hmdEye = deviceId == 0 && (eye == kPassLeft || eye == kPassRight);
    const bool idle = !sessionRunning.load(std::memory_order_acquire);
    if (hmdEye && (pass != eye || idle)) {
        std::lock_guard guard(cache.mutex);
        if (cache.pose[eye]) {
            std::memcpy(orientation, cache.orientation[eye], sizeof(cache.orientation[eye]));
            std::memcpy(position, cache.position[eye], sizeof(cache.position[eye]));
            served.fetch_add(idle ? 1 : 0, std::memory_order_relaxed);
            return cache.poseOk[eye];
        }
    }
    steamPoses.fetch_add(1, std::memory_order_relaxed);
    const bool ok = originalEyePose(self, deviceId, eye, orientation, position);
    if (hmdEye) {
        std::lock_guard guard(cache.mutex);
        std::memcpy(cache.orientation[eye], orientation, sizeof(cache.orientation[eye]));
        std::memcpy(cache.position[eye], position, sizeof(cache.position[eye]));
        cache.poseOk[eye] = ok;
        cache.pose[eye] = true;
    }
    return ok;
}

std::atomic<bool> started{false};

bool counts_match() {
    for (const CountRead& r : kInstancedViewCounts)
        if (!aceqv::code_matches(r.rva, r.bytes, r.size)) {
            aceqv::logf("acc views: no instanced view count read at 0x%llx", static_cast<unsigned long long>(r.rva));
            return false;
        }
    return aceqv::code_matches(kViewInstancedCount, kViewInstancedCountAnchor, sizeof(kViewInstancedCountAnchor));
}

// Two views per instanced draw (abi.h), so the family's four views draw as two instanced pairs.
bool pairs_per_draw() {
    bool ok = true;
    for (const CountRead& r : kInstancedViewCounts) {
        const std::uint8_t reg = (r.bytes[r.size - 2] >> 3) & 7;  // ModRM reg
        const std::uint8_t three[3] = {0x6a, 0x02, static_cast<std::uint8_t>(0x58 + reg)};        // push 2; pop r64
        const std::uint8_t four[4] = {0x6a, 0x02, 0x41, static_cast<std::uint8_t>(0x58 + reg)};   // push 2; pop r8-r15
        ok = aceqv::code_write(r.rva, r.size == 3 ? three : four, r.size) && ok;
    }
    // cmp ecx, 2; jle +5; mov ecx, 2; then the moved `mov rdi, [rbp + 0xe8]`.
    const std::uint8_t clamp[] = {0x83, 0xf9, 0x02, 0x7e, 0x05, 0xb9, 0x02, 0x00, 0x00, 0x00,
                                  0x48, 0x8b, 0xbd, 0xe8, 0x00, 0x00, 0x00};
    ok = aceqv::code_redirect(kViewInstancedCount, kViewInstancedCountAnchor, sizeof(kViewInstancedCountAnchor), clamp,
                              sizeof(clamp)) && ok;
    aceqv::logf("acc views: %s", ok ? "two views per instanced draw (21 count reads, view constants clamped)"
                                    : "a views-per-draw patch failed");
    return ok;
}

}  // namespace

bool views_start(const aceqv::Views4Layout& l, double boxX, double boxY) {
    if (started.load())
        return true;
    const Slot slots[] = {kGetDesiredNumberOfViews, kGetViewPassForIndex, kGetViewIndexForPass, kAdjustViewRect,
                          kGetStereoProjectionMatrix, kGetRelativeEyePose, kDeviceIsAPrimaryPass, kDeviceIsAPrimaryView,
                          kSetFinalViewRect};
    for (const Slot& s : slots)
        if (aceqv::vtable_slot(s.rva) != s.function) {
            aceqv::logf("acc views: slot 0x%llx holds 0x%llx, expected 0x%llx: not the build these addresses are "
                        "from; nothing patched",
                        static_cast<unsigned long long>(s.rva), static_cast<unsigned long long>(aceqv::vtable_slot(s.rva)),
                        static_cast<unsigned long long>(s.function));
            return false;
        }
    if (!aceqv::code_matches(kAllowSubPrimitiveQueriesRef, kAllowSubPrimitiveQueriesRefAnchor,
                             sizeof(kAllowSubPrimitiveQueriesRefAnchor))) {
        aceqv::log_line("acc views: r.AllowSubPrimitiveQueries' registration isn't this build's; nothing patched");
        return false;
    }
    if (!counts_match()) {
        aceqv::log_line("acc views: the views-per-draw code isn't this build's; nothing patched");
        return false;
    }
    const std::uintptr_t base = aceqv::module_base();
    // Foliage's per-cluster occlusion results crash with the side views on track (abi.h): one query per proxy.
    auto* subPrimitiveQueries = reinterpret_cast<volatile std::int32_t*>(base + kAllowSubPrimitiveQueries);
    aceqv::logf("acc views: r.AllowSubPrimitiveQueries %d -> 0 (foliage occlusion per proxy, not per cluster)",
                static_cast<int>(*subPrimitiveQueries));
    *subPrimitiveQueries = 0;
    layout = l;
    views_set_focus(boxX, boxY);
    originalAdjust = reinterpret_cast<AdjustViewRectFn>(base + kAdjustViewRect.function);
    originalProjection = reinterpret_cast<ProjectionFn>(base + kGetStereoProjectionMatrix.function);
    originalEyePose = reinterpret_cast<EyePoseFn>(base + kGetRelativeEyePose.function);
    // Pose, projection, rects and pairs before the count, so no frame sees four views without them.
    bool ok = pairs_per_draw();
    ok = ok && aceqv::vtable_slot_replace(kGetRelativeEyePose.rva, kGetRelativeEyePose.function,
                                         reinterpret_cast<void*>(&eye_pose));
    ok = ok && aceqv::vtable_slot_replace(kGetStereoProjectionMatrix.rva, kGetStereoProjectionMatrix.function,
                                          reinterpret_cast<void*>(&projection));
    ok = ok && aceqv::vtable_slot_replace(kAdjustViewRect.rva, kAdjustViewRect.function,
                                          reinterpret_cast<void*>(&adjust_view_rect));
    ok = ok && aceqv::vtable_slot_replace(kSetFinalViewRect.rva, kSetFinalViewRect.function,
                                          reinterpret_cast<void*>(&final_view_rect));
    ok = ok && aceqv::vtable_slot_replace(kDeviceIsAPrimaryView.rva, kDeviceIsAPrimaryView.function,
                                          reinterpret_cast<void*>(&primary_view));
    ok = ok && aceqv::vtable_slot_replace(kDeviceIsAPrimaryPass.rva, kDeviceIsAPrimaryPass.function,
                                          reinterpret_cast<void*>(&primary));
    ok = ok && aceqv::vtable_slot_replace(kGetViewIndexForPass.rva, kGetViewIndexForPass.function,
                                          reinterpret_cast<void*>(&pass_view));
    ok = ok && aceqv::vtable_slot_replace(kGetViewPassForIndex.rva, kGetViewPassForIndex.function,
                                          reinterpret_cast<void*>(&view_pass));
    ok = ok && aceqv::vtable_slot_replace(kGetDesiredNumberOfViews.rva, kGetDesiredNumberOfViews.function,
                                          reinterpret_cast<void*>(&desired_views));
    aceqv::logf("acc views: %s; layout widths %.3f %.3f %.3f %.3f heights %.3f %.3f %.3f %.3f of the target",
                ok ? (boxX > 0 ? "four views patched in (views 2-3 focus views, centred)"
                               : "four views patched in (views 2-3 copy the eyes)")
                   : "a slot write failed",
                l.width[0], l.width[1], l.width[2], l.width[3], l.height[0], l.height[1], l.height[2], l.height[3]);
    started.store(ok);
    return ok;
}

void views_set_focus(double boxX, double boxY) {
    focusX.store(boxX, std::memory_order_relaxed);
    focusY.store(boxY > 0 ? boxY : boxX, std::memory_order_relaxed);
    aceqv::logf("acc views: focus box %.0f%% x %.0f%% of each eye's tangent extent%s", boxX * 100,
                (boxY > 0 ? boxY : boxX) * 100, boxX > 0 ? ", centred" : " (none: the focus views copy the eyes)");
}

void views_session_running(bool running) {
    sessionRunning.store(running, std::memory_order_release);
    aceqv::logf("acc views: OpenXR session %s; %llu projection/pose answers came from the cache while none ran",
                running ? "running: SteamVR answers again" : "not running: answers from the cache",
                static_cast<unsigned long long>(served.exchange(0)));
}

void views_target(int (&size)[2]) {
    size[0] = targetWidth.load(std::memory_order_relaxed);
    size[1] = targetHeight.load(std::memory_order_relaxed);
}

void views_frame_end() {
    if (!started.load() || ++frames % 900 != 0)
        return;
    const auto line = [](const char* what, std::atomic<std::uint64_t> (&calls)[5]) {
        aceqv::logf("acc views: %s per pass full/left/right/left side/right side = %llu/%llu/%llu/%llu/%llu", what,
                    static_cast<unsigned long long>(calls[0].exchange(0)), static_cast<unsigned long long>(calls[1].exchange(0)),
                    static_cast<unsigned long long>(calls[2].exchange(0)), static_cast<unsigned long long>(calls[3].exchange(0)),
                    static_cast<unsigned long long>(calls[4].exchange(0)));
    };
    line("AdjustViewRect", adjustCalls);
    line("projection", projectionCalls);
    line("eye pose", poseCalls);
    for (int pass = 1; pass < 5; ++pass)
        aceqv::logf("acc views: renderer's final rect for pass %d: %d,%d - %d,%d", pass, finalRect[pass][0].load(),
                    finalRect[pass][1].load(), finalRect[pass][2].load(), finalRect[pass][3].load());
    aceqv::logf("acc views: target %dx%d over the last 900 frames; %llu projection and %llu eye pose calls reached "
                "SteamVR", targetWidth.load(), targetHeight.load(), static_cast<unsigned long long>(steamProjections.exchange(0)),
                static_cast<unsigned long long>(steamPoses.exchange(0)));
    aceqv::logf("acc views: %llu side-pass projections narrowed to the focus box",
                static_cast<unsigned long long>(focused.exchange(0)));
}

}  // namespace acc

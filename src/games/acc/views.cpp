#include "views.h"

#include <atomic>
#include <cstdint>

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
std::atomic<int> targetWidth{0}, targetHeight{0};
std::atomic<std::uint64_t> adjustCalls[5], projectionCalls[5], poseCalls[5];
std::uint64_t frames = 0;

void count(std::atomic<std::uint64_t> (&calls)[5], int pass) {
    if (pass >= 0 && pass < 5)
        calls[pass].fetch_add(1, std::memory_order_relaxed);
}

int desired_views(void*, bool stereo) { return views_for(stereo); }
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
    *sizeX = static_cast<unsigned>(r.width);
    *sizeY = static_cast<unsigned>(r.height);
}

// Stage gate: a side pass is a copy of its eye.
FMatrix* projection(const void* self, FMatrix* out, int pass) {
    count(projectionCalls, pass);
    return originalProjection(self, out, eye_pass(pass));
}
bool eye_pose(void* self, int deviceId, int pass, void* orientation, void* position) {
    count(poseCalls, pass);
    return originalEyePose(self, deviceId, eye_pass(pass), orientation, position);
}

std::atomic<bool> started{false};

}  // namespace

bool views_start(const aceqv::Views4Layout& l) {
    if (started.load())
        return true;
    const Slot slots[] = {kGetDesiredNumberOfViews, kGetViewPassForIndex, kGetViewIndexForPass, kAdjustViewRect,
                          kGetStereoProjectionMatrix, kGetRelativeEyePose};
    for (const Slot& s : slots)
        if (aceqv::vtable_slot(s.rva) != s.function) {
            aceqv::logf("acc views: slot 0x%llx holds 0x%llx, expected 0x%llx: not the build these addresses are "
                        "from; nothing patched",
                        static_cast<unsigned long long>(s.rva), static_cast<unsigned long long>(aceqv::vtable_slot(s.rva)),
                        static_cast<unsigned long long>(s.function));
            return false;
        }
    const std::uintptr_t base = aceqv::module_base();
    layout = l;
    originalAdjust = reinterpret_cast<AdjustViewRectFn>(base + kAdjustViewRect.function);
    originalProjection = reinterpret_cast<ProjectionFn>(base + kGetStereoProjectionMatrix.function);
    originalEyePose = reinterpret_cast<EyePoseFn>(base + kGetRelativeEyePose.function);
    // Pose, projection and rects before the count, so no frame sees four views without them.
    bool ok = aceqv::vtable_slot_replace(kGetRelativeEyePose.rva, kGetRelativeEyePose.function,
                                         reinterpret_cast<void*>(&eye_pose));
    ok = ok && aceqv::vtable_slot_replace(kGetStereoProjectionMatrix.rva, kGetStereoProjectionMatrix.function,
                                          reinterpret_cast<void*>(&projection));
    ok = ok && aceqv::vtable_slot_replace(kAdjustViewRect.rva, kAdjustViewRect.function,
                                          reinterpret_cast<void*>(&adjust_view_rect));
    ok = ok && aceqv::vtable_slot_replace(kGetViewIndexForPass.rva, kGetViewIndexForPass.function,
                                          reinterpret_cast<void*>(&pass_view));
    ok = ok && aceqv::vtable_slot_replace(kGetViewPassForIndex.rva, kGetViewPassForIndex.function,
                                          reinterpret_cast<void*>(&view_pass));
    ok = ok && aceqv::vtable_slot_replace(kGetDesiredNumberOfViews.rva, kGetDesiredNumberOfViews.function,
                                          reinterpret_cast<void*>(&desired_views));
    aceqv::logf("acc views: %s; layout widths %.3f %.3f %.3f %.3f heights %.3f %.3f %.3f %.3f of the target",
                ok ? "four views patched in (views 2-3 copy the eyes)" : "a slot write failed",
                l.width[0], l.width[1], l.width[2], l.width[3], l.height[0], l.height[1], l.height[2], l.height[3]);
    started.store(ok);
    return ok;
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
    aceqv::logf("acc views: target %dx%d over the last 900 frames", targetWidth.load(), targetHeight.load());
}

}  // namespace acc

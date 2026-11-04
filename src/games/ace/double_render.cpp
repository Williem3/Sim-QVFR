#include "double_render.h"

#include <cstdint>
#include <cstring>

#include <MinHook.h>

#include "abi.h"
#include "../../log.h"
#include "../../xr.h"

namespace aceqv {
namespace {

using RenderFrameFn = void (*)(void* self, float a, float b, void* scene);

RenderFrameFn renderFrameOriginal = nullptr;
bool started = false;
bool inRender = false;  // main thread only: render_frame has a single caller in the frame loop

LARGE_INTEGER frequency{};
uint64_t frames = 0;
double firstMs = 0, secondMs = 0;
constexpr uint64_t kReportEvery = 450;

double elapsed_ms(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

void renderFrameHook(void* self, float a, float b, void* scene) {
    if (inRender) {  // never stack a third render on top of ours
        renderFrameOriginal(self, a, b, scene);
        return;
    }
    inRender = true;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    renderFrameOriginal(self, a, b, scene);
    QueryPerformanceCounter(&t1);
    renderFrameOriginal(self, a, b, scene);
    QueryPerformanceCounter(&t2);
    inRender = false;

    ++frames;
    firstMs += elapsed_ms(t0, t1);
    secondMs += elapsed_ms(t1, t2);
    if (frames <= 3 || frames % kReportEvery == 0) {
        logf("double_render frames=%llu cpu ms per frame: first=%.3f second=%.3f (args a=%.5f b=%.5f scene=%p)",
             static_cast<unsigned long long>(frames), firstMs / static_cast<double>(frames),
             secondMs / static_cast<double>(frames), a, b, scene);
        if (frames >= kReportEvery) {
            frames = 0;
            firstMs = secondMs = 0;
        }
    }
}

}  // namespace

bool double_render_start() {
    if (started)
        return true;
    QueryPerformanceFrequency(&frequency);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* target = reinterpret_cast<void*>(base + ace::kRenderFrame.rva);
    if (std::memcmp(target, ace::kRenderFrame.anchor, ace::kRenderFrame.anchorSize) != 0) {
        log_line("double_render: render_frame anchor mismatch; experiment refused");
        return false;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&renderFrameHook), reinterpret_cast<void**>(&renderFrameOriginal)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        log_line("double_render: hook failed; experiment refused");
        return false;
    }
    started = true;
    log_line("double_render: ACTIVE, every frame is rendered twice with identical cameras");
    return true;
}

}  // namespace aceqv

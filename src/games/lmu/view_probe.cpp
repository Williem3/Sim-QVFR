#include "view_probe.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../../sha256_file.h"
#include "abi.h"
#include "game.h"

namespace lmu {
namespace {

using RenderViewFn = void (*)(void* view, void* a2, void* a3, unsigned char a4, char a5);
using SetViewParamsFn = void (*)(void* renderer, void* params);

RenderViewFn originalRenderView = nullptr;
SetViewParamsFn originalSetViewParams = nullptr;
bool installed = false;

// One frame's events, in order. The render thread appends; xrEndFrame (maybe another thread) takes them.
struct Event {
    bool setParams;     // false: RenderView entered
    void* view;         // RenderView: the view; SetViewParams: the view being rendered then (innermost)
    unsigned flags;     // RenderView: the view's flags
    int subViews;       // RenderView: sub-view records
    unsigned enabled;   // RenderView: bit i, sub-view i enabled (first 32)
    int cameraEye;      // RenderView: the eye index on its camera
    int depth;          // RenderView nesting (child views render inside their parent)
    int eye;            // SetViewParams: params eye
    float nearZ, farZ;  // SetViewParams
};
std::mutex mutex;
std::vector<Event> frame;
thread_local std::vector<void*> viewStack;
thread_local int depth = 0;
unsigned long renderThread = 0;
bool threadChanged = false;

// Per-summary counters.
unsigned long long frames = 0, renderViews = 0, setParams = 0, eyeCalls[4] = {}, otherEye = 0;
int maxViewsPerFrame = 0, maxParamsPerFrame = 0;

template <typename T>
T read(const void* base, std::size_t offset) {
    T v{};
    std::memcpy(&v, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return v;
}

void render_view(void* view, void* a2, void* a3, unsigned char a4, char a5) {
    Event e = {};
    e.view = view;
    if (view) {
        e.flags = read<unsigned>(view, kViewFlags);
        const auto begin = read<std::uintptr_t>(view, kViewSubViews);
        const auto end = read<std::uintptr_t>(view, kViewSubViews + 8);
        e.subViews = (begin && end >= begin) ? static_cast<int>((end - begin) / kSubViewSize) : -1;
        for (int i = 0; i < e.subViews && i < 32; ++i)
            if (*reinterpret_cast<const std::uint8_t*>(begin + i * kSubViewSize))
                e.enabled |= 1u << i;
        const void* camera = (e.flags >> 30 & 1) ? read<void*>(view, kViewLinkedCamera) : view;
        e.cameraEye = camera ? read<int>(camera, kCameraEye) : -1;
    }
    e.depth = depth;
    {
        std::lock_guard g(mutex);
        const unsigned long tid = GetCurrentThreadId();
        if (renderThread && renderThread != tid)
            threadChanged = true;
        renderThread = tid;
        if (frame.size() < 256)
            frame.push_back(e);
    }
    viewStack.push_back(view);
    ++depth;
    originalRenderView(view, a2, a3, a4, a5);
    --depth;
    viewStack.pop_back();
}

void set_view_params(void* renderer, void* params) {
    Event e = {};
    e.setParams = true;
    e.view = viewStack.empty() ? nullptr : viewStack.back();
    e.depth = depth;
    if (params) {
        e.eye = read<int>(params, kParamsEye);
        e.nearZ = read<float>(params, kParamsNear);
        e.farZ = read<float>(params, kParamsFar);
    }
    {
        std::lock_guard g(mutex);
        if (frame.size() < 256)
            frame.push_back(e);
    }
    originalSetViewParams(renderer, params);
}

std::string describe(const std::vector<Event>& events) {
    std::string s;
    char buf[200];
    for (const Event& e : events) {
        if (e.setParams)
            std::snprintf(buf, sizeof(buf), "\n    %*sSetViewParams eye %d near %.3g far %.4g (in view %p)", e.depth * 2, "",
                          e.eye, e.nearZ, e.farZ, e.view);
        else
            std::snprintf(buf, sizeof(buf), "\n    %*sRenderView %p flags 0x%08x sub-views %d enabled 0x%x camera eye %d",
                          e.depth * 2, "", e.view, e.flags, e.subViews, e.enabled, e.cameraEye);
        s += buf;
    }
    return s;
}

}  // namespace

bool view_probe_start() {
    if (installed)
        return true;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!simqvfr::games::supported_build(kGame, simqvfr::sha256_file(exe))) {
        simqvfr::log_line("lmu view probe: not the build its addresses are from; not installed");
        return false;
    }
    if (!simqvfr::code_matches(kRenderView, kRenderViewAnchor, sizeof(kRenderViewAnchor)) ||
        simqvfr::vtable_slot(kSetViewParamsSlot) != kSetViewParams) {
        simqvfr::log_line("lmu view probe: the code isn't this build's (anchor or vtable slot); not installed");
        return false;
    }
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        simqvfr::log_line("lmu view probe: MH_Initialize failed");
        return false;
    }
    void* target = reinterpret_cast<void*>(simqvfr::module_base() + kRenderView);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&render_view), reinterpret_cast<void**>(&originalRenderView)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        simqvfr::log_line("lmu view probe: RenderView couldn't be hooked; not installed");
        return false;
    }
    originalSetViewParams = reinterpret_cast<SetViewParamsFn>(simqvfr::module_base() + kSetViewParams);
    if (!simqvfr::vtable_slot_replace(kSetViewParamsSlot, kSetViewParams, reinterpret_cast<void*>(&set_view_params))) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        simqvfr::log_line("lmu view probe: SetViewParams' vtable slot couldn't be replaced; not installed");
        return false;
    }
    installed = true;
    simqvfr::log_line("lmu view probe: on (log only): RenderView and SetViewParams recorded per frame; frames 300, 301, "
                      "1200, 3600 and 7200 in full, a summary every 1800 frames");
    return true;
}

void view_probe_frame_end() {
    if (!installed)
        return;
    std::vector<Event> events;
    {
        std::lock_guard g(mutex);
        events.swap(frame);
    }
    ++frames;
    int views = 0, params = 0;
    for (const Event& e : events) {
        if (e.setParams) {
            ++params;
            ++setParams;
            if (e.eye >= 0 && e.eye < 4)
                ++eyeCalls[e.eye];
            else
                ++otherEye;
        } else {
            ++views;
            ++renderViews;
        }
    }
    maxViewsPerFrame = (std::max)(maxViewsPerFrame, views);
    maxParamsPerFrame = (std::max)(maxParamsPerFrame, params);
    const bool sample = frames == 300 || frames == 301 || frames == 1200 || frames == 3600 || frames == 7200;
    if (sample)
        simqvfr::logf("lmu view probe: frame %llu: %d RenderView, %d SetViewParams%s", frames, views, params,
                      describe(events).c_str());
    if (frames % 1800 == 0) {
        simqvfr::logf("lmu view probe: over the last 1800 frames: RenderView %llu, SetViewParams %llu (eye 0: %llu, 1: "
                      "%llu, 2: %llu, 3: %llu, other %llu); at most %d views and %d view params in a frame%s",
                      renderViews, setParams, eyeCalls[0], eyeCalls[1], eyeCalls[2], eyeCalls[3], otherEye,
                      maxViewsPerFrame, maxParamsPerFrame,
                      threadChanged ? "; RenderView ran on more than one thread" : "");
        renderViews = setParams = otherEye = 0;
        for (auto& c : eyeCalls)
            c = 0;
        maxViewsPerFrame = maxParamsPerFrame = 0;
    }
}

}  // namespace lmu

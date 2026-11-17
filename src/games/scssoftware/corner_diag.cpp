// Where the time goes (the perf lines) and the dev builds' diagnostic hotkeys (corner_internal.h).
#include "corner_internal.h"

#include <dxgi1_4.h>

#include <algorithm>

#include "../../dev_build.h"
#include "../../log.h"

namespace scs {
namespace detail {

void perf_line() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const uint64_t frames = g.stats.frames - g.perfFrame;
    if (g.perfSince && frames) {
        const double seconds = static_cast<double>(now.QuadPart - g.perfSince) / freq.QuadPart;
        const auto ms = [&](int b) { return g.ticks[b] * 1000.0 / freq.QuadPart / frames; };
        const auto per = [&](int b) { return static_cast<double>(g.timed[b]) / frames; };
        simqvfr::logf("scs perf: %.1f fps over %.1f s, the focus view %s; per frame on the render thread: two-view draws "
                      "%.0f in %.2f ms, replays %.0f in %.2f ms, eye rewrites %.0f in %.2f ms, mirrors %.1f in %.2f ms, "
                      "release %.1f in %.2f ms",
                      frames / seconds, seconds,
                      g.focusOff ? "OFF (Ctrl+Alt+G)"
                                 : !g.focusPercent ? "not set"
                                                   : g.instanced && (g.preferInstanced || !g.mvr) ? "on (instancing)"
                                                                                                  : g.mvr ? "on (NVIDIA's)" : "on (passes only)",
                      per(State::kTimeFocusDraws), ms(State::kTimeFocusDraws), per(State::kTimeReplays), ms(State::kTimeReplays),
                      per(State::kTimeRewrites), ms(State::kTimeRewrites), per(State::kTimeMirrors), ms(State::kTimeMirrors),
                      per(State::kTimeRelease), ms(State::kTimeRelease));
    }
    g.perfSince = now.QuadPart;
    g.perfFrame = g.stats.frames;
    for (int b = 0; b < State::kTimes; ++b)
        g.ticks[b] = 0, g.timed[b] = 0;
}

// The diagnostic hotkeys, Ctrl+Alt with: B a burst from the next release; K the focus view composed or not; J bursts at
// the next three box jumps; H the reprojections' box; G the focus view's work off; I NVIDIA's multi-view or instancing;
// A the focus view's own occlusion; N the mirrors off. (Not F: the player's frame capture hotkey.)
[[maybe_unused]] void poll_diag_keys() {  // unused in a release (poll_diag_hotkeys)
    const bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    // `key` pressed with the chord since the last poll (`held`: its state then).
    const auto pressed = [chord](int key, bool& held) {
        const bool down = chord && (GetAsyncKeyState(key) & 0x8000);
        const bool edge = down && !held;
        held = down;
        return edge;
    };
    if (pressed('B', g.hotkeyDown) && g.burstFramesLeft == 0 && g.shots.empty()) {
        g.burstFramesLeft = kBurstFrames;
        g.burstStartFrame = g.stats.frames;
        simqvfr::logf("scs corner: Ctrl+Alt+B: recording the next %d frames' released images (before and after the "
                      "scale-up, a quarter size)", kBurstFrames);
    }
    if (pressed('K', g.composeKeyDown) && g.focusPercent) {
        g.composeFocus = !g.composeFocus;
        simqvfr::logf("scs focus: Ctrl+Alt+K: the focus view %s", g.composeFocus ? "composed over the box" : "not composed");
    }
    if (pressed('J', g.jumpKeyDown) && g.focusPercent) {
        g.jumpBursts = 3;
        simqvfr::log_line("scs focus: Ctrl+Alt+J: the next three jumps of the box are recorded");
    }
    if (pressed('H', g.historyKeyDown) && g.focusPercent) {
        g.historyCurrentBox = !g.historyCurrentBox;
        simqvfr::logf("scs focus: Ctrl+Alt+H: the reprojections use %s", g.historyCurrentBox ? "this frame's box" : "the frame before's box");
    }
    if (pressed('G', g.focusKeyDown) && g.focusPercent) {
        perf_line();  // what the state so far cost, then the other
        g.focusOff = !g.focusOff;
        simqvfr::logf("scs focus: Ctrl+Alt+G: the focus view's draws and passes are %s", g.focusOff ? "OFF" : "on");
    }
    if (pressed('I', g.instancingKeyDown) && g.focusPercent) {
        if (g.mvr && g.instanced) {
            perf_line();  // what the way so far cost, then the other
            g.preferInstanced = !g.preferInstanced;
            simqvfr::logf("scs focus: Ctrl+Alt+I: the focus view's scene draws by %s",
                          g.preferInstanced ? "Sim-QVFR's own multi-view rendering (instancing)" : "NVIDIA's multi-view rendering");
        } else {
            simqvfr::logf("scs focus: Ctrl+Alt+I: only one way here (NVIDIA's %d, instancing %d)", g.mvr ? 1 : 0, g.instanced ? 1 : 0);
        }
    }
    if (pressed('A', g.occlusionKeyDown) && g.focusPercent) {
        g.ownOcclusion = !g.ownOcclusion;
        simqvfr::logf("scs focus: Ctrl+Alt+A: the focus view's ambient occlusion is %s",
                      g.ownOcclusion ? "its own (the half-size passes replayed)" : "the eye's, through the box");
    }
    if (pressed('N', g.mirrorsKeyDown)) {
        g.mirrorsOff = !g.mirrorsOff;
        if (g.active) {
            const ch::Bypass bypass;  // our own binds (at a release: outside an observer), not seen by on_constant_buffers
            swap_bound(!g.mirrorsOff);  // what is bound now: the game's buffers back, or our copies
        }
        simqvfr::logf("scs corner: Ctrl+Alt+N: the scene draws read %s (writes to them from other threads so far: %llu, "
                      "%llu with the eyes in the corner)",
                      g.mirrorsOff ? "the game's own constant buffers (mirrors OFF)" : "our copies (mirrors on)",
                      static_cast<unsigned long long>(g.otherWrites.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g.otherWritesActive.load(std::memory_order_relaxed)));
    }
}

namespace {

// The device watch's own state (device_watch): the adapter for the VRAM readings, and what they showed.
struct DeviceWatch {
    ComPtr<IDXGIAdapter3> adapter;
    bool adapterTried = false;
    uint64_t usedMb = 0, peakMb = 0, budgetMb = 0;  // the last reading, the session's highest, the budget then
    uint64_t readFrame = 0;                          // g.stats.frames of the last reading
};
DeviceWatch dw;
bool removedLogged = false;  // once per process: a removed device stays removed

const char* removed_reason(HRESULT hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG: the GPU took too long on the work it was given, and Windows reset it (a TDR)";
        case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET: the device was reset after a badly formed command";
        case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED: the GPU was removed (a GPU page fault, a driver update or the card dropping off)";
        case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR: the driver hit an internal error";
        case 0x887A0001: return "DXGI_ERROR_INVALID_CALL: an invalid call";
        case 0x8007000E: return "E_OUTOFMEMORY: out of memory";
        default: return "an unknown reason";
    }
}

void read_vram() {
    if (!dw.adapterTried) {
        dw.adapterTried = true;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(g.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
            adapter.As(&dw.adapter);
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO m{};
    if (!dw.adapter || FAILED(dw.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m)))
        return;
    dw.usedMb = m.CurrentUsage >> 20;
    dw.budgetMb = m.Budget >> 20;
    dw.peakMb = std::max(dw.peakMb, dw.usedMb);
    dw.readFrame = g.stats.frames;
}

}  // namespace

// Every xrEndFrame with the corner installed: the GPU device checked (GetDeviceRemovedReason, a cheap call on a
// free-threaded device). Testers' ATS runs (2026-11-17) lost the device seconds into driving, and the game then crashed
// on its next allocation with nothing in this log: the first failure is logged once with its reason, what the focus
// view was doing and the VRAM, so a run says whether it was a hang, a fault or memory. The VRAM is read every 90 frames.
void device_watch() {
    if (!g.device || removedLogged)
        return;
    const HRESULT hr = g.device->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) {
        if (!dw.adapterTried || g.stats.frames - dw.readFrame >= 90)
            read_vram();
        return;
    }
    removedLogged = true;
    read_vram();  // the adapter's numbers outlive the device
    simqvfr::logf("scs corner: the GPU device was REMOVED (0x%08lX, %s), seen at frame %llu (%llu with the eyes in the "
                  "corner); the focus view %s, the corner %.0f%%, the box %d%% x %d%% at %.0f%% density, eye targets %ux%u; "
                  "twins %zu (%.0f MB at most), mirrors %zu; this frame's eye pass %s; VRAM %llu MB used of a %llu MB budget "
                  "(at frame %llu), the session's highest %llu MB",
                  static_cast<unsigned long>(hr), removed_reason(hr), static_cast<unsigned long long>(g.stats.frames),
                  static_cast<unsigned long long>(g.stats.cornerFrames),
                  !g.focusPercent ? "off"
                  : g.focusOff     ? "OFF (Ctrl+Alt+G)"
                  : g.instanced && (g.preferInstanced || !g.mvr) ? "by instancing"
                  : g.mvr          ? "by NVIDIA's multi-view rendering"
                                   : "passes only",
                  g.percent, g.focusPercent, g.focusPercentY, g.density * 100, g.eyeW, g.eyeH, g.texes.size(),
                  g.twinBytes / 1048576.0, g.mirrors.size(), g.active ? "under way" : "not under way",
                  static_cast<unsigned long long>(dw.usedMb), static_cast<unsigned long long>(dw.budgetMb),
                  static_cast<unsigned long long>(dw.readFrame), static_cast<unsigned long long>(dw.peakMb));
}

void device_watch_reset() { dw = {}; }

// The diagnostic hotkeys exist in development builds only (dev_build.h): a release reads no key here.
void poll_diag_hotkeys() {
    if constexpr (simqvfr::kDevBuild)
        poll_diag_keys();
}

}  // namespace detail

}  // namespace scs

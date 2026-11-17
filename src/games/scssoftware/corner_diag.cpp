// Where the time goes (the perf lines) and the dev builds' diagnostic hotkeys (corner_internal.h).
#include "corner_internal.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../dev_build.h"
#include "../../log.h"
#include "corner_rules.h"
#include "../../probe/aftermath.h"

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
    if (g.gpuTimes)
        gpu_times_line();
    if (g.largeReplays)
        simqvfr::logf("scs focus: large replays (pixel range over %u vectors) so far: %llu, skipped %llu", kLargeRange,
                      static_cast<unsigned long long>(g.largeReplays), static_cast<unsigned long long>(g.largeSkipped));
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
std::atomic<bool> removedLogged{false};  // once per process: a removed device stays removed (two threads may see it)

// The focus view's GPU times (gpu_replay_begin and on): a ring of frames' queries, each read back once the GPU is done
// with it (its disjoint query answers; a few frames later), never waited for.
constexpr int kGpuSlots = 4;
constexpr unsigned kGpuReplays = 64;
struct ReplayInfo {
    unsigned n = 0;             // the frame's replay number (as scs_replay_skip counts them)
    unsigned w = 0, h = 0;      // the target
    UINT psFirst = 0, psCount = 0;  // the pixel shader's constant range (tells the passes apart)
    bool swapchain = false;     // the headset's image (the final blit)
};
struct GpuSlot {
    ComPtr<ID3D11Query> disjoint, passStart, frameEnd, begin[kGpuReplays], end[kGpuReplays];
    ReplayInfo info[kGpuReplays];
    unsigned replays = 0;  // timed this frame
    bool open = false, pass = false, pending = false;
    uint64_t frame = 0;
};
struct GpuWindow {  // since the last gpu_times_line
    double sum[kGpuReplays] = {}, max[kGpuReplays] = {};
    unsigned count[kGpuReplays] = {};
    ReplayInfo info[kGpuReplays];
    double passSum = 0, passMax = 0;
    unsigned frames = 0, passFrames = 0, late = 0, disjointed = 0;
};
GpuSlot gpuSlots[kGpuSlots];
int gpuCur = 0;
GpuWindow gw;
unsigned gpuSpikes = 0;
// What the hang watch's thread reads (hang_check): the newest frame the GPU finished, as read back.
std::atomic<uint64_t> gpuDoneFrame{0};
std::atomic<float> gpuDonePassMs{0}, gpuDoneReplayMs{0};
std::atomic<unsigned> gpuDoneReplays{0};
constexpr double kSpikeReplayMs = 5, kSpikePassMs = 40;
constexpr unsigned kMaxSpikeLines = 30;

ID3D11Query* query(ComPtr<ID3D11Query>& q, D3D11_QUERY type) {
    if (!q) {
        const D3D11_QUERY_DESC d = {type, 0};
        g.device->CreateQuery(&d, &q);
    }
    return q.Get();
}

// The frame's queries opened at its first timestamp (the disjoint query brackets them).
GpuSlot* open_slot() {
    GpuSlot& s = gpuSlots[gpuCur];
    if (s.open)
        return &s;
    if (s.pending) {  // not read back in kGpuSlots frames: dropped
        s.pending = false;
        ++gw.late;
    }
    ID3D11Query* d = query(s.disjoint, D3D11_QUERY_TIMESTAMP_DISJOINT);
    if (!d)
        return nullptr;
    g.context->Begin(d);
    s.open = true;
    s.pass = false;
    s.replays = 0;
    s.frame = g.stats.frames;
    return &s;
}

double ms(uint64_t a, uint64_t b, uint64_t frequency) { return b > a ? (b - a) * 1000.0 / frequency : 0; }

// One finished frame's times into the window; false: not ready yet.
bool read_slot(GpuSlot& s) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (g.context->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return false;
    s.pending = false;
    if (dj.Disjoint || !dj.Frequency) {
        ++gw.disjointed;
        return true;
    }
    const auto stamp = [](ID3D11Query* q, uint64_t& t) {
        return q && g.context->GetData(q, &t, sizeof(t), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    };
    ++gw.frames;
    double passMs = 0, replayMs = 0, slowest = 0;
    unsigned slowIndex = 0;
    uint64_t a = 0, b = 0;
    if (s.pass && stamp(s.passStart.Get(), a) && stamp(s.frameEnd.Get(), b)) {
        passMs = ms(a, b, dj.Frequency);
        gw.passSum += passMs;
        gw.passMax = std::max(gw.passMax, passMs);
        ++gw.passFrames;
    }
    for (unsigned i = 0; i < s.replays; ++i) {
        if (!stamp(s.begin[i].Get(), a) || !stamp(s.end[i].Get(), b))
            continue;
        const double t = ms(a, b, dj.Frequency);
        replayMs += t;
        const unsigned k = s.info[i].n >= 1 && s.info[i].n <= kGpuReplays ? s.info[i].n - 1 : i;
        gw.sum[k] += t;
        gw.max[k] = std::max(gw.max[k], t);
        ++gw.count[k];
        gw.info[k] = s.info[i];
        if (t > slowest)
            slowest = t, slowIndex = i;
    }
    gpuDoneFrame.store(s.frame, std::memory_order_relaxed);
    gpuDonePassMs.store(static_cast<float>(passMs), std::memory_order_relaxed);
    gpuDoneReplayMs.store(static_cast<float>(replayMs), std::memory_order_relaxed);
    gpuDoneReplays.store(s.replays, std::memory_order_relaxed);
    if ((slowest > kSpikeReplayMs || passMs > kSpikePassMs) && gpuSpikes < kMaxSpikeLines) {
        ++gpuSpikes;
        const ReplayInfo& r = s.info[slowIndex];
        simqvfr::logf("scs gpu: SLOW frame %llu: the eye passes and ours %.2f ms, its %u replays %.2f ms, the slowest replay "
                      "%u of the frame %.2f ms (into %ux%u%s, pixel range %u+%u)%s",
                      static_cast<unsigned long long>(s.frame), passMs, s.replays, replayMs, r.n, slowest, r.w, r.h,
                      r.swapchain ? ", the headset's image" : "", r.psFirst, r.psCount,
                      gpuSpikes == kMaxSpikeLines ? " (no more of these lines this session)" : "");
    }
    return true;
}

}  // namespace

void gpu_pass_begin() {
    GpuSlot* s = open_slot();
    if (!s || s->pass)
        return;
    if (ID3D11Query* q = query(s->passStart, D3D11_QUERY_TIMESTAMP)) {
        g.context->End(q);
        s->pass = true;
    }
}

namespace {
bool gpuInReplay = false;  // gpu_replay_begin issued its timestamp
}

void gpu_replay_begin(UINT psFirst, UINT psCount) {
    gpuInReplay = false;
    GpuSlot* s = open_slot();
    if (!s || s->replays >= kGpuReplays)
        return;
    const unsigned i = s->replays;
    ID3D11Query* q = query(s->begin[i], D3D11_QUERY_TIMESTAMP);
    if (!q || !query(s->end[i], D3D11_QUERY_TIMESTAMP))
        return;
    g.context->End(q);
    s->info[i] = {g.frameReplay, g.boundW, g.boundH, psFirst, psCount, g.boundSwapchain};
    gpuInReplay = true;
}

void gpu_replay_end() {
    GpuSlot& s = gpuSlots[gpuCur];
    if (!gpuInReplay || !s.open)
        return;
    gpuInReplay = false;
    g.context->End(s.end[s.replays].Get());
    ++s.replays;
}

void gpu_times_frame_end() {
    GpuSlot& s = gpuSlots[gpuCur];
    if (s.open) {
        if (s.pass) {
            if (ID3D11Query* q = query(s.frameEnd, D3D11_QUERY_TIMESTAMP))
                g.context->End(q);
            else
                s.pass = false;
        }
        g.context->End(s.disjoint.Get());
        s.open = false;
        s.pending = true;
        gpuCur = (gpuCur + 1) % kGpuSlots;
    }
    for (int i = 1; i < kGpuSlots; ++i) {  // the older frames, oldest first
        GpuSlot& old = gpuSlots[(gpuCur + i) % kGpuSlots];
        if (old.pending && !read_slot(old))
            break;  // the GPU does them in order: the newer ones aren't done either
    }
}

// With each perf line: the window's eye passes and every replay that took 0.05 ms or more at its slowest.
void gpu_times_line() {
    if (!gw.frames) {
        if (gw.late)
            simqvfr::logf("scs gpu: no frame read back in time (%u dropped)", gw.late);
        gw = {};
        return;
    }
    std::string list;
    double total = 0;
    for (unsigned i = 0; i < kGpuReplays; ++i) {
        if (!gw.count[i])
            continue;
        total += gw.sum[i] / gw.frames;
        if (gw.max[i] < 0.05)
            continue;
        char one[128];
        snprintf(one, sizeof(one), "%s#%u %ux%u%s p%u+%u %.2f/%.2f", list.empty() ? "" : ", ", i + 1, gw.info[i].w,
                 gw.info[i].h, gw.info[i].swapchain ? "(hmd)" : "", gw.info[i].psFirst, gw.info[i].psCount,
                 gw.sum[i] / gw.count[i], gw.max[i]);
        list += one;
    }
    simqvfr::logf("scs gpu: %u frames read back (%u late, %u disjoint): the eye passes and ours %.2f ms a frame (slowest "
                  "%.2f), the focus view's replays %.2f ms a frame; replays (#n target, pixel range, average/slowest ms): %s",
                  gw.frames, gw.late, gw.disjointed, gw.passFrames ? gw.passSum / gw.passFrames : 0, gw.passMax, total,
                  list.empty() ? "none over 0.05 ms" : list.c_str());
    gw = {};
}

void gpu_times_reset() {
    for (GpuSlot& s : gpuSlots)
        s = {};
    gpuCur = 0;
    gw = {};
}

namespace {

const char* removed_reason(HRESULT hr);

void log_removed(HRESULT hr, const char* seen) {
    simqvfr::logf("scs corner: the GPU device was REMOVED (0x%08lX, %s), seen %s at frame %llu (%llu with the eyes in the "
                  "corner; the GPU's newest frame read back %llu: eye passes %.2f ms, %u replays %.2f ms); the focus view %s, "
                  "the corner %.0f%%, the box %d%% x %d%% at %.0f%% density, eye targets %ux%u; twins %zu (%.0f MB at most), "
                  "mirrors %zu; this frame's eye pass %s; the boxes' top-left (left eye, right eye; screen 0..1) %.3f,%.3f and "
                  "%.3f,%.3f, the frame before %.3f,%.3f and %.3f,%.3f, gaze %.3f,%.3f and %.3f,%.3f; VRAM %llu MB used of a %llu "
                  "MB budget (at frame %llu), the session's highest %llu MB",
                  static_cast<unsigned long>(hr), removed_reason(hr), seen, static_cast<unsigned long long>(g.stats.frames),
                  static_cast<unsigned long long>(g.stats.cornerFrames),
                  static_cast<unsigned long long>(gpuDoneFrame.load(std::memory_order_relaxed)),
                  gpuDonePassMs.load(std::memory_order_relaxed), gpuDoneReplays.load(std::memory_order_relaxed),
                  gpuDoneReplayMs.load(std::memory_order_relaxed),
                  !g.focusPercent ? "off"
                  : g.focusOff     ? "OFF (Ctrl+Alt+G)"
                  : g.instanced && (g.preferInstanced || !g.mvr) ? "by instancing"
                  : g.mvr          ? "by NVIDIA's multi-view rendering"
                                   : "passes only",
                  g.percent, g.focusPercent, g.focusPercentY, g.density * 100, g.eyeW, g.eyeH, g.texes.size(),
                  g.twinBytes / 1048576.0, g.mirrors.size(), g.active ? "under way" : "not under way", g.box[0][0],
                  g.box[0][1], g.box[1][0], g.box[1][1], g.prevBox[0][0], g.prevBox[0][1], g.prevBox[1][0], g.prevBox[1][1],
                  g.gaze[0][0].load(std::memory_order_relaxed), g.gaze[0][1].load(std::memory_order_relaxed),
                  g.gaze[1][0].load(std::memory_order_relaxed), g.gaze[1][1].load(std::memory_order_relaxed),
                  static_cast<unsigned long long>(dw.usedMb), static_cast<unsigned long long>(dw.budgetMb),
                  static_cast<unsigned long long>(dw.readFrame), static_cast<unsigned long long>(dw.peakMb));
}

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
    if (!g.device || removedLogged.load(std::memory_order_relaxed))
        return;
    const HRESULT hr = g.device->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) {
        if (!dw.adapterTried || g.stats.frames - dw.readFrame >= 90)
            read_vram();
        return;
    }
    if (removedLogged.exchange(true))
        return;
    read_vram();  // the adapter's numbers outlive the device
    log_removed(hr, "at xrEndFrame");
    simqvfr::aftermath::after_removal(5000);  // the last marker the GPU reached, and the dump written
}

void device_watch_reset() { dw = {}; }

// The hang watch fired (its own thread: no xrEndFrame for its seconds): the device asked from here, since a frame stuck
// inside the runtime's xrEndFrame never reaches device_watch. Only the device's own free-threaded call; the context,
// which the stuck thread may be inside, is left alone, and the rest is read without a lock (a log line).
void hang_check() {
    ID3D11Device* device = g.device.Get();
    if (!device) {
        simqvfr::log_line("scs corner: hang watch: the corner has no device now");
        return;
    }
    const HRESULT hr = device->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) {
        simqvfr::logf("scs corner: hang watch: the GPU device is fine (not removed): the stall is not a GPU reset; the "
                      "CPU at frame %llu, the GPU's newest frame read back %llu (eye passes %.2f ms, %u replays %.2f ms)",
                      static_cast<unsigned long long>(g.stats.frames),
                      static_cast<unsigned long long>(gpuDoneFrame.load(std::memory_order_relaxed)),
                      gpuDonePassMs.load(std::memory_order_relaxed), gpuDoneReplays.load(std::memory_order_relaxed),
                      gpuDoneReplayMs.load(std::memory_order_relaxed));
        return;
    }
    if (!removedLogged.exchange(true))
        log_removed(hr, "by the hang watch");
}

namespace {
unsigned largeDumps = 0;
uint64_t largeDumpFrame = 0;

std::string vec_text(const float (&v)[4]) {
    char s[160];
    uint32_t u[4];
    std::memcpy(u, v, 16);
    snprintf(s, sizeof(s), "(%g, %g, %g, %g | %08x %08x %08x %08x)", v[0], v[1], v[2], v[3], u[0], u[1], u[2], u[3]);
    return s;
}
}  // namespace

void dump_large_range(unsigned n) {
    if (largeDumps >= 4 || (largeDumps && g.stats.frames - largeDumpFrame < 300))
        return;
    ID3D11Buffer* bound = nullptr;
    UINT first = 0, count = 0;
    g.context->PSGetConstantBuffers1(0, 1, &bound, &first, &count);
    ComPtr<ID3D11Buffer> buf;
    buf.Attach(bound);
    if (!buf || !count)
        return;
    D3D11_BUFFER_DESC bd{};
    buf->GetDesc(&bd);
    const UINT bytes = std::min(count * 16, bd.ByteWidth > first * 16 ? bd.ByteWidth - first * 16 : 0);
    if (!bytes)
        return;
    const D3D11_BUFFER_DESC sd = {bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(g.device->CreateBuffer(&sd, nullptr, &staging)))
        return;
    const D3D11_BOX box = {first * 16, 0, 0, first * 16 + bytes, 1, 1};
    g.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buf.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return;
    std::vector<float> data(bytes / 4);
    std::memcpy(data.data(), m.pData, bytes);
    g.context->Unmap(staging.Get(), 0);
    ++largeDumps;
    largeDumpFrame = g.stats.frames;
    const std::wstring path = simqvfr::log_directory() + L"\\scs-large-" + std::to_wstring(largeDumps) + L".bin";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fwrite(data.data(), 1, bytes, f);
        fclose(f);
    }
    auto* v = reinterpret_cast<float(*)[4]>(data.data());
    const int vectors = static_cast<int>(bytes / 16);
    simqvfr::logf("scs focus: large range %u (frame %llu, replay %u, buffer %u bytes, %s): %d vectors from %u, saved as %ls",
                  largeDumps, static_cast<unsigned long long>(g.stats.frames), n, bd.ByteWidth,
                  bd.ByteWidth >= kMirrorMin ? "a mirror or the game's large buffer" : "the game's", vectors, first, path.c_str());
    // Where the focus view's rules (kCornerPatchCs with flags 2) would write in it.
    const float ones[4] = {1, 1, 1, 1}, last[4] = {0, 0, 0, 1};
    const auto eq = [](const float (&a)[4], const float (&b)[4]) { return std::memcmp(a, b, 16) == 0; };
    const auto size = [](const float (&a)[4]) { return corner_size(a); };
    unsigned lines = 0;
    for (int p = 0; p + 1 < vectors && lines < 24; ++p)
        if (eq(v[p], ones) && size(v[p + 1])) {
            ++lines;
            simqvfr::logf("scs focus:   pair at %d: %s %s; the ray rewrite at %d: %s", p, vec_text(v[p]).c_str(),
                          vec_text(v[p + 1]).c_str(), p + 3, p + 3 < vectors ? vec_text(v[p + 3]).c_str() : "(past the end)");
            ++p;
        }
    if (vectors >= 5 && v[0][0] == 0.5f && v[0][1] == 0.5f && eq(v[3], ones) && size(v[4]))
        simqvfr::logf("scs focus:   the AO rule at 0: %s", vec_text(v[0]).c_str());
    for (int k = 0; k + 4 <= vectors && lines < 48; ++k) {
        if (!eq(v[k + 3], last))
            continue;
        float back[4] = {-v[k + 2][0], -v[k + 2][1], -v[k + 2][2], -v[k + 2][3]};
        if (k >= 4 && eq(v[k - 1], back)) {
            ++lines;
            simqvfr::logf("scs focus:   matrix remap at %d (rows %d, %d): %s %s", k - 4, k - 4, k - 3, vec_text(v[k - 4]).c_str(),
                          vec_text(v[k - 3]).c_str());
        }
        if (k + 8 <= vectors && eq(v[k + 7], back)) {
            ++lines;
            simqvfr::logf("scs focus:   matrix remap at %d (rows %d, %d): %s %s", k + 4, k + 4, k + 5, vec_text(v[k + 4]).c_str(),
                          vec_text(v[k + 5]).c_str());
        }
    }
    if (!lines)
        simqvfr::log_line("scs focus:   no rule matches in it");
}

void mark(MarkKind kind, uint32_t value) {
    if (simqvfr::aftermath::marking())
        simqvfr::aftermath::mark((static_cast<uint64_t>(kind) << 60) | ((g.stats.frames & 0xFFFFFFFull) << 32) | value);
}

void marker_name(uint64_t code, char* out, unsigned size) {
    const unsigned kind = static_cast<unsigned>(code >> 60);
    const unsigned long long frame = (code >> 32) & 0xFFFFFFF;
    const uint32_t v = static_cast<uint32_t>(code);
    switch (kind) {
        case kMarkDraw:
            snprintf(out, size, "frame %llu, before eye-pass draw %u (%s%s; scene draws are marked every %u)", frame, v & 0xFFFFFF,
                     (v >> 24) & 1 ? "indexed: the scene" : "non-indexed: a full-screen pass", (v >> 25) & 1 ? ", with the focus view" : "", kMarkEveryDraws);
            break;
        case kMarkReplay: snprintf(out, size, "frame %llu, before the focus replay %u (pixel range %u vectors)", frame, v >> 16, v & 0xFFFF); break;
        case kMarkReplayDone: snprintf(out, size, "frame %llu, after the focus replay %u", frame, v >> 16); break;
        case kMarkRelease: snprintf(out, size, "frame %llu, before our release work for eye %u (scale-up, focus composed)", frame, v); break;
        case kMarkReleaseDone: snprintf(out, size, "frame %llu, after our release work for eye %u", frame, v); break;
        case kMarkFrameEnd: snprintf(out, size, "frame %llu, its end (the next frame's first call of ours)", frame); break;
        default: snprintf(out, size, "unknown marker %016llx", static_cast<unsigned long long>(code)); break;
    }
}

// The diagnostic hotkeys exist in development builds only (dev_build.h): a release reads no key here.
void poll_diag_hotkeys() {
    if constexpr (simqvfr::kDevBuild)
        poll_diag_keys();
}

}  // namespace detail

}  // namespace scs

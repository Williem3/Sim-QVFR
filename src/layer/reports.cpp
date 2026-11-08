// What the layer measures and logs: CPU times, pacing (the timed hooks below), load, VRAM, and the session
// summary.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include <dxgi1_4.h>

#include "games/game.h"
#include "log.h"
#include "probe/cpu_load.h"
#include "probe/cpu_profile.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "probe/vram_census.h"
#include "quad/osd.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

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

double report_pacing() {
    if (pacing.interval.empty())
        return 0;
    const auto at = [](std::vector<float> v, double p) {
        if (v.empty())
            return 0.0;
        const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return static_cast<double>(v[k]);
    };
    double sum = 0, longest = 0;
    size_t periods[4] = {};  // 1, 2, 3, 4 or more
    for (const float i : pacing.interval) {
        sum += i;
        longest = std::max(longest, static_cast<double>(i));
        if (pacing.periodMs > 0)
            ++periods[std::clamp(static_cast<int>(std::lround(i / pacing.periodMs)), 1, 4) - 1];
    }
    const double mean = sum / static_cast<double>(pacing.interval.size());
    std::vector<float> outside;  // ACE's own frame time: the interval less its wait (pairs in order)
    for (size_t i = 0; i < pacing.interval.size() && i < pacing.wait.size(); ++i)
        outside.push_back(std::max(0.0f, pacing.interval[i] - pacing.wait[i]));
    logf("views4 pacing: display period %.2f ms; ACE frame interval p50 %.2f ms, p90 %.2f, max %.2f (%.1f fps over %zu "
         "frames); in xrWaitFrame p50 %.2f ms, p90 %.2f; outside it p50 %.2f ms, p90 %.2f; frames of 1/2/3/4+ display "
         "periods %zu/%zu/%zu/%zu",
         pacing.periodMs, at(pacing.interval, 0.5), at(pacing.interval, 0.9), longest, mean > 0 ? 1000 / mean : 0,
         pacing.interval.size(), at(pacing.wait, 0.5), at(pacing.wait, 0.9), at(outside, 0.5), at(outside, 0.9),
         periods[0], periods[1], periods[2], periods[3]);
    summary.fps = mean > 0 ? 1000 / mean : 0;
    summary.frameP50 = at(pacing.interval, 0.5);
    summary.frameP90 = at(pacing.interval, 0.9);
    summary.latePercent = 100.0 * static_cast<double>(periods[1] + periods[2] + periods[3]) /
                          static_cast<double>(pacing.interval.size());
    summary.periodMs = pacing.periodMs;
    pacing.interval.clear();
    pacing.wait.clear();
    return mean;
}

// The load line, over the same 900 frames: the whole GPU's busy share (ACE's part) and so its time per frame
// (`frameMs`: the mean interval), ACE's busiest thread and the render thread's share of a core, all ACE's
// threads; and, with the overlay on, what it last showed.
void report_load(double frameMs) {
    int gpuSamples = 0, cpuSamples = 0;
    const GpuLoad gpu = gpu_load_window(gpuSamples);
    const CpuLoad threads = cpu_load_window(cpuSamples);
    summary.gpuBusy = gpu.valid ? gpu.busy : -1;
    summary.gpuMs = gpu.valid ? gpu.busy / 100 * frameMs : -1;
    summary.cpuMs = threads.valid && threads.renderPercent >= 0 ? threads.renderPercent / 100 * frameMs : -1;
    summary.cores = threads.valid ? threads.processPercent / 100 : -1;
    if (!gpu.valid && !threads.valid)
        return;
    char gpuText[192] = "GPU load unknown", cpuText[256] = "threads unknown", shown[256] = "";
    if (gpu.valid)
        std::snprintf(gpuText, sizeof(gpuText), "GPU %.0f%% busy (%ls engine; game %.0f%%): %.1f ms of GPU per game frame",
                      gpu.busy, gpu.engine.c_str(), gpu.game, gpu.busy / 100 * frameMs);
    if (threads.valid)
        std::snprintf(cpuText, sizeof(cpuText),
                      "busiest game thread %.0f%% of a core (last: %u%s%ls%s), render thread %.0f%%, all threads %.0f%% "
                      "(%d)",
                      threads.busiestPercent, threads.busiestThread, threads.busiestName.empty() ? "" : " \"",
                      threads.busiestName.c_str(), threads.busiestName.empty() ? "" : "\"", threads.renderPercent,
                      threads.processPercent, threads.threads);
    OsdSummary s{};
    if (osd_shown(s)) {
        static const char* const kBounds[] = {"unknown", "neither", "CPU", "GPU", "pacing"};
        std::snprintf(shown, sizeof(shown),
                      "; overlay: CPU %.1f ms (%.0f%% headroom), GPU %.1f ms%s (%.0f%% headroom), limited by %s",
                      std::max(s.cpuMs, s.cpuThreadMs), s.cpuHeadroom, s.gpuMs, s.gpuWhole ? " all work" : "", s.gpuHeadroom,
                      kBounds[static_cast<int>(s.bound)]);
    }
    logf("views4 load: %s; %s%s (%d and %d samples)", gpuText, cpuText, shown, gpuSamples, cpuSamples);
    // Every busy thread, not only the busiest: a share of one core each (a thread near 100% limits the frame
    // however idle the other cores are), by ACE's own thread names where it set them.
    const std::vector<ThreadLoad> threadLoads = cpu_load_threads(8);
    summary.threads = threadLoads;
    std::string list;
    for (const ThreadLoad& t : threadLoads) {
        if (t.percent < 5)
            break;
        char item[160];
        std::snprintf(item, sizeof(item), "%s%ls%s%u %.0f%%", list.empty() ? "" : ", ", t.name.c_str(),
                      t.name.empty() ? "" : " ", t.id, t.percent);
        list += item;
    }
    if (!list.empty())
        logf("views4 threads: %s (of one core each)", list.c_str());
}

namespace {

// VRAM (views4): this process's use of the GPU's local memory and the census of ACE's render targets
// (probe/vram_census.h) against its render and output sizes, logged and written to the runtime info, where
// the launcher scales the part that follows the targets to estimate other settings (launcher/settings.h).
void report_vram() {
    int render[2], output[2];
    game->qvfr_target_sizes(render, output);
    if (output[0] <= 0 || !quad.device)
        return;
    if (!vramAdapter) {
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            if (FAILED(factory->EnumAdapterByLuid(quad.device->GetAdapterLuid(), IID_PPV_ARGS(&vramAdapter))))
                vramAdapter = nullptr;
            factory->Release();
        }
        if (!vramAdapter)
            return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (FAILED(vramAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
        return;
    const VramCensus c = vram_census(render[0], render[1], output[0], output[1]);
    constexpr double kMb = 1024.0 * 1024.0;
    logf("vram: %.0f MB used of %.0f MB; render targets %.0f MB that follow the %dx%d render and %dx%d output "
         "targets (%zu textures), %.0f MB other (%zu)",
         memory.CurrentUsage / kMb, memory.Budget / kMb, c.scaledBytes / kMb, render[0], render[1], output[0],
         output[1], c.scaledTextures, c.otherBytes / kMb, c.otherTextures);
    ini_write_later([used = memory.CurrentUsage / kMb, budget = memory.Budget / kMb, scaled = c.scaledBytes / kMb,
                     w = output[0], h = output[1]] {
        runtime_info(L"vram_used_mb", std::to_wstring(std::lround(used)));
        runtime_info(L"vram_budget_mb", std::to_wstring(std::lround(budget)));
        runtime_info(L"vram_scaled_mb", std::to_wstring(std::lround(scaled)));
        runtime_info(L"vram_target", std::to_wstring(w) + L"x" + std::to_wstring(h));
    });
}

// The session summary in the runtime info ([runtime] session_*), at every report: what the launcher's
// Diagnostics pane shows after the session (launcher/settings.h's SessionInfo). `layerMs`: the layer's own
// CPU time per frame on ACE's render thread.
void write_session_summary(double layerMs) {
    if (summary.fps <= 0)
        return;
    const SessionSummary s = summary;
    // What limits the frame: the GPU busy all the time, or the render thread near its whole budget (the
    // spinning Physics and eye-tracker threads are not counted: they look busy whatever limits the frame).
    const wchar_t* limited = s.gpuBusy >= 95                                ? L"GPU"
                             : s.cpuMs >= 0 && s.cpuMs >= 0.9 * s.periodMs ? L"CPU (render thread)"
                             : s.latePercent > 5                           ? L"Pacing (frames late, neither saturated)"
                                                                           : L"Neither (headroom)";
    std::wstring threads;
    for (const ThreadLoad& t : s.threads) {
        if (t.percent < 5)
            break;
        wchar_t item[160];
        _snwprintf_s(item, _TRUNCATE, L"%ls%ls=%.0f", threads.empty() ? L"" : L"|",
                   t.name.empty() ? (L"thread " + std::to_wstring(t.id)).c_str() : t.name.c_str(), t.percent);
        threads += item;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t time[32];
    swprintf_s(time, L"%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    const std::wstring mode = experiment.views4                       ? L"QVFR"
                              : experiment.quad || experiment.twoLoops ? L"Two-loop"
                                                                       : L"Off (the game's own two views)";
    const bool fg = frame_generation_on();
    double gazeMs = -1;
    {
        std::lock_guard guard(gazeThread.mutex);
        if (gazeThread.lookups)
            gazeMs = gazeThread.lookupMs / gazeThread.lookups;
    }
    wchar_t log[MAX_PATH] = {};
    GetEnvironmentVariableW(L"SIM_QVFR_LOG", log, MAX_PATH);
    const std::wstring profile = cpu_profile_last_report();
    const std::wstring layers(layersBelow.begin(), layersBelow.end());
    ini_write_later([s, limited = std::wstring(limited), threads, time = std::wstring(time), mode, fg, gazeMs, layerMs,
                     log = std::wstring(log), profile, layers] {
        const auto number = [](double v, int digits) {
            if (v < 0)
                return std::wstring();
            wchar_t text[32];
            swprintf_s(text, L"%.*f", digits, v);
            return std::wstring(text);
        };
        runtime_info(L"session_time", time);
        runtime_info(L"session_mode", mode);
        runtime_info(L"session_fps", number(s.fps, 1));
        runtime_info(L"session_frame_p50_ms", number(s.frameP50, 2));
        runtime_info(L"session_frame_p90_ms", number(s.frameP90, 2));
        runtime_info(L"session_late_percent", number(s.latePercent, 1));
        runtime_info(L"session_refresh_hz", number(s.periodMs > 0 ? 1000 / s.periodMs : -1, 0));
        runtime_info(L"session_cpu_ms", number(s.cpuMs, 2));
        runtime_info(L"session_gpu_ms", number(s.gpuMs, 2));
        runtime_info(L"session_gpu_busy", number(s.gpuBusy, 0));
        runtime_info(L"session_cpu_cores", number(s.cores, 1));
        runtime_info(L"session_limited", limited);
        runtime_info(L"session_threads", threads);
        runtime_info(L"session_frame_generation", fg ? L"1" : L"0");
        runtime_info(L"session_gaze_ms", number(gazeMs, 3));
        runtime_info(L"session_layer_ms", number(layerMs, 2));
        runtime_info(L"session_layers_below", layers);
        runtime_info(L"session_log", log);
        runtime_info(L"session_cpu_profile", profile);
    });
}

}  // namespace

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
    // The layer's own time per frame on the render thread, before the lines below reset the spans.
    const auto own = [](const CpuSpan& total, const CpuSpan* below) {
        const uint64_t n = total.calls.load(), t = total.ticks.load(), b = below ? below->ticks.load() : 0;
        return n ? static_cast<double>(t > b ? t - b : 0) * cpu.msPerTick / static_cast<double>(n) : 0.0;
    };
    const double layerMs = own(cpu.endFrame, &cpu.endFrameBelow) + own(cpu.locate, &cpu.locateBelow);
    char end[128], locate[128], compose[128];
    line(cpu.endFrame, &cpu.endFrameBelow, end, sizeof(end));
    line(cpu.locate, &cpu.locateBelow, locate, sizeof(locate));
    line(cpu.compose, nullptr, compose, sizeof(compose));
    logf("views4 cpu: ms per call over the last %llu frames: xrEndFrame %s; xrLocateViews %s; compose %s",
         static_cast<unsigned long long>(kCpuReportFrames), end, locate, compose);
    const double frameMs = report_pacing();
    gpu_frame_timer_report();  // its own samples: it logs while the overlay collects too
    report_load(frameMs);
    report_vram();
    write_session_summary(layerMs);
    gaze_thread_report(frameMs * kCpuReportFrames / 1000);
    gaze_hold_report();
}

// A game in src/games, by its exe: anywhere else the layer passes through.
bool is_game_process() { return games::current_game() != nullptr; }

}  // namespace simqvfr::layer

// Offline check of the load samplers (probe/gpu_load.h, probe/cpu_load.h): the GPU engine counters'
// instance names, an adapter's busiest render engine from a sample (all processes summed, video and copy
// engines left out, other adapters ignored), the threads' share of a core, and a live run that finds a
// busy thread.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "probe/cpu_load.h"
#include "probe/gpu_load.h"

using namespace aceqv;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool close_to(double a, double b, double tolerance = 1e-6) { return a > b - tolerance && a < b + tolerance; }

}  // namespace

int main() {
    {
        const GpuEngineName n = parse_gpu_engine_name(L"pid_1234_luid_0x00000000_0x0000D1B6_phys_0_eng_3_engtype_3D");
        check(n.valid && n.pid == 1234 && n.luidHigh == 0 && n.luidLow == 0xD1B6 && n.engine == 3 && n.type == L"3D",
              "GPU engine counter name: pid, adapter, engine and type");
        check(!parse_gpu_engine_name(L"_Total").valid && !parse_gpu_engine_name(L"pid_1_luid_0x0").valid,
              "names that are not engines are skipped");
        const GpuEngineName c = parse_gpu_engine_name(L"pid_9_luid_0x00000001_0x00002A10_phys_0_eng_7_engtype_Compute_0");
        check(c.valid && c.luidHigh == 1 && c.luidLow == 0x2A10 && c.engine == 7 && c.type == L"Compute_0",
              "a compute engine on another adapter");
    }
    {
        // ACE (pid 10) and the compositor (pid 20) share the 3D engine; video encode is busier but not a render
        // engine; another adapter's 3D engine is busier still but not the game's.
        const std::vector<std::pair<std::wstring, double>> sample = {
            {L"pid_10_luid_0x00000000_0x0000D1B6_phys_0_eng_0_engtype_3D", 70},
            {L"pid_20_luid_0x00000000_0x0000D1B6_phys_0_eng_0_engtype_3D", 22},
            {L"pid_10_luid_0x00000000_0x0000D1B6_phys_0_eng_2_engtype_Compute_0", 15},
            {L"pid_30_luid_0x00000000_0x0000D1B6_phys_0_eng_9_engtype_VideoEncode", 99},
            {L"pid_10_luid_0x00000000_0x0000D1B6_phys_0_eng_4_engtype_Copy", 50},
            {L"pid_40_luid_0x00000000_0x00001111_phys_0_eng_0_engtype_3D", 100},
        };
        const GpuLoad load = gpu_load_from(sample, 0, 0xD1B6, 10);
        check(load.valid && close_to(load.busy, 92) && close_to(load.ace, 70) && load.engine == L"3D",
              "the adapter's busiest render engine: every process summed (92%), ACE's share (70%)");
        const GpuLoad full = gpu_load_from({{L"pid_10_luid_0x00000000_0x0000D1B6_phys_0_eng_0_engtype_3D", 80},
                                            {L"pid_20_luid_0x00000000_0x0000D1B6_phys_0_eng_0_engtype_3D", 40}},
                                           0, 0xD1B6, 10);
        check(close_to(full.busy, 100) && close_to(full.ace, 80), "an engine's processes sum to at most 100%");
        check(!gpu_load_from(sample, 0, 0x9999, 10).valid, "no engine of the adapter: no load");
    }
    {
        const std::map<uint32_t, uint64_t> before = {{1, 1000}, {2, 5000}, {3, 0}};
        const std::map<uint32_t, uint64_t> after = {{1, 2000}, {2, 95000}, {3, 40000}, {4, 70000}};
        const CpuLoad load = cpu_load_from(before, after, 100000, 3);
        check(load.valid && load.busiestThread == 2 && close_to(load.busiestPercent, 90) && close_to(load.renderPercent, 40) &&
                  close_to(load.processPercent, 131) && load.threads == 3,
              "threads' share of a core: the busiest (90%), the render thread's (40%), all of them; a new thread skipped");
        check(!cpu_load_from(before, after, 0, 3).valid, "no elapsed cycles: no load");
        const CpuLoad spun = cpu_load_from(before, after, 100000, 3, {2});
        check(spun.valid && spun.busiestThread == 3 && close_to(spun.busiestPercent, 40) && close_to(spun.processPercent, 131),
              "a spinning thread is never the busiest (the next one is, 40%) but counts in the process's total");
        check(cpu_load_spins(L"Physics") && cpu_load_spins(L"(aSeeVRClient.dll)") && !cpu_load_spins(L"GameThread") &&
                  !cpu_load_spins(L""),
              "spinning threads by description: Physics and the eye tracker's client");
        const std::map<uint32_t, double> shares = cpu_load_shares(before, after, 100000);
        check(shares.size() == 3 && close_to(shares.at(1), 1) && close_to(shares.at(2), 90) && close_to(shares.at(3), 40) &&
                  !shares.count(4),
              "every thread's share of a core (the thread table); a new thread skipped");
    }
    {
        // Live: a thread spinning flat out is the busiest, near a whole core.
        std::atomic<bool> spin{true};
        std::atomic<DWORD> spinner{0};
        std::thread busy([&] {
            spinner = GetCurrentThreadId();
            while (spin.load(std::memory_order_relaxed)) {
            }
        });
        cpu_load_start();
        CpuLoad load;
        for (int i = 0; i < 40 && !(load.valid && load.busiestThread == spinner.load()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            load = cpu_load_latest();
        }
        spin = false;
        busy.join();
        cpu_load_stop();
        int overlaySamples = 0, logSamples = 0;
        const CpuLoad overlay = cpu_load_window(overlaySamples, 0);
        const CpuLoad logged = cpu_load_window(logSamples, 1);
        int again = 0;
        cpu_load_window(again, 0);
        check(overlaySamples > 0 && overlaySamples == logSamples && overlay.valid && logged.valid && again == 0,
              "the overlay's and the log's windows each get every sample, and empty on their own");
        std::printf("     live: busiest thread %u at %.0f%% of a core (the spinner is %u), %d threads\n", load.busiestThread,
                    load.busiestPercent, static_cast<unsigned>(spinner.load()), load.threads);
        check(load.valid && load.busiestThread == spinner.load() && load.busiestPercent > 60,
              "live: the spinning thread is the busiest, most of a core");
    }
    {
        // Live GPU counters: start and stop cleanly; on an adapter that matches nothing, no load.
        gpu_load_start(0xFFFFFFFF, 0xFFFFFFFF);
        std::this_thread::sleep_for(std::chrono::milliseconds(3 * kGpuLoadMs));
        const GpuLoad none = gpu_load_latest();
        gpu_load_stop();
        check(!none.valid, "live GPU counters: an unknown adapter has no load; the worker stops");
        // This machine's first adapter: its engines are found (busy may be 0 when idle).
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)) &&
            SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            gpu_load_start(static_cast<uint32_t>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
            GpuLoad real;
            for (int i = 0; i < 12 && !real.valid; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kGpuLoadMs));
                real = gpu_load_latest();
            }
            gpu_load_stop();
            std::printf("     live: %ls, busiest render engine %ls at %.1f%%\n", desc.Description, real.engine.c_str(), real.busy);
            check(real.valid && real.busy >= 0 && real.busy <= 100, "live GPU counters: this machine's adapter has its engines");
        } else {
            std::printf("     live: no hardware adapter; skipped\n");
        }
    }
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

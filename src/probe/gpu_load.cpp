#include "gpu_load.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "../log.h"

namespace simqvfr {
namespace {

// The number after `key` in `name` (hex with 0x), or false.
bool number_after(const std::wstring& name, const wchar_t* key, uint32_t& out, size_t& end) {
    const size_t at = name.find(key);
    if (at == std::wstring::npos)
        return false;
    const wchar_t* start = name.c_str() + at + wcslen(key);
    wchar_t* stop = nullptr;
    const unsigned long v = wcstoul(start, &stop, 0);
    if (stop == start)
        return false;
    out = static_cast<uint32_t>(v);
    end = static_cast<size_t>(stop - name.c_str());
    return true;
}

bool render_engine(const std::wstring& type) {
    return type.rfind(L"3D", 0) == 0 || type.rfind(L"Compute", 0) == 0 || type.rfind(L"Graphics", 0) == 0;
}

struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false, running = false;
    uint32_t luidHigh = 0, luidLow = 0;
    GpuLoad latest;
    double busySum[3] = {}, gameSum[3] = {};  // gpu_load_window's, per window
    int samples[3] = {};
};
Worker worker;

void run() {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
        log_line("gpu load: the GPU engine counters are not available; the overlay uses the game's GPU time");
        if (query)
            PdhCloseQuery(query);
        return;
    }
    PdhCollectQueryData(query);  // the first sample: utilization needs two
    const uint32_t pid = GetCurrentProcessId();
    std::vector<BYTE> buffer;
    std::unique_lock lock(worker.mutex);
    while (!worker.stop) {
        worker.wake.wait_for(lock, std::chrono::milliseconds(kGpuLoadMs), [] { return worker.stop; });
        if (worker.stop)
            break;
        const uint32_t high = worker.luidHigh, low = worker.luidLow;
        lock.unlock();
        GpuLoad load;
        if (PdhCollectQueryData(query) == ERROR_SUCCESS) {
            DWORD size = 0, count = 0;
            PDH_STATUS s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
            if (s == static_cast<PDH_STATUS>(PDH_MORE_DATA) && size) {
                buffer.resize(size);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
                if (s == ERROR_SUCCESS) {
                    std::vector<std::pair<std::wstring, double>> samples;
                    samples.reserve(count);
                    for (DWORD i = 0; i < count; ++i)
                        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                            items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                            samples.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
                    load = gpu_load_from(samples, high, low, pid);
                }
            }
        }
        lock.lock();
        worker.latest = load;
        if (load.valid)
            for (int w = 0; w < 3; ++w) {
                worker.busySum[w] += load.busy;
                worker.gameSum[w] += load.game;
                ++worker.samples[w];
            }
    }
    lock.unlock();
    PdhCloseQuery(query);
}

}  // namespace

GpuEngineName parse_gpu_engine_name(const std::wstring& name) {
    GpuEngineName n;
    size_t end = 0;
    uint32_t engine = 0;
    if (!number_after(name, L"pid_", n.pid, end) || !number_after(name, L"luid_", n.luidHigh, end))
        return n;
    if (end >= name.size() || name[end] != L'_')
        return n;
    const wchar_t* start = name.c_str() + end + 1;
    wchar_t* stop = nullptr;
    n.luidLow = static_cast<uint32_t>(wcstoul(start, &stop, 0));
    if (stop == start || !number_after(name, L"_eng_", engine, end))
        return n;
    const size_t type = name.find(L"engtype_");
    if (type == std::wstring::npos)
        return n;
    n.engine = static_cast<int>(engine);
    n.type = name.substr(type + 8);
    n.valid = true;
    return n;
}

GpuLoad gpu_load_from(const std::vector<std::pair<std::wstring, double>>& samples, uint32_t luidHigh, uint32_t luidLow,
                      uint32_t pid) {
    struct Engine {
        double total = 0, game = 0;
        std::wstring type;
    };
    std::map<int, Engine> engines;
    for (const auto& [name, value] : samples) {
        const GpuEngineName n = parse_gpu_engine_name(name);
        if (!n.valid || n.luidHigh != luidHigh || n.luidLow != luidLow || !render_engine(n.type))
            continue;
        Engine& e = engines[n.engine];
        e.type = n.type;
        e.total += std::max(0.0, value);
        if (n.pid == pid)
            e.game += std::max(0.0, value);
    }
    GpuLoad load;
    for (const auto& [index, e] : engines) {
        const double total = std::min(100.0, e.total);
        if (!load.valid || total > load.busy) {
            load.valid = true;
            load.busy = total;
            load.game = std::min(total, e.game);
            load.engine = e.type;
        }
    }
    return load;
}

bool gpu_load_start(uint32_t luidHigh, uint32_t luidLow) {
    std::lock_guard guard(worker.mutex);
    worker.luidHigh = luidHigh;
    worker.luidLow = luidLow;
    if (worker.running)
        return true;
    worker.stop = false;
    worker.latest = GpuLoad{};
    worker.running = true;
    worker.thread = std::thread(run);
    logf("gpu load: sampling the GPU engine counters every %d ms (adapter 0x%08x_0x%08x)", kGpuLoadMs, luidHigh, luidLow);
    return true;
}

void gpu_load_stop() {
    std::thread thread;
    {
        std::lock_guard guard(worker.mutex);
        if (!worker.running)
            return;
        worker.stop = true;
        worker.running = false;
        thread.swap(worker.thread);
    }
    worker.wake.notify_all();
    if (thread.joinable())
        thread.join();
}

GpuLoad gpu_load_latest() {
    std::lock_guard guard(worker.mutex);
    return worker.latest;
}

GpuLoad gpu_load_window(int& samples, int window) {
    std::lock_guard guard(worker.mutex);
    const int w = window == 0 || window == 2 ? window : 1;
    GpuLoad load = worker.latest;
    samples = worker.samples[w];
    load.valid = samples > 0;
    if (samples) {
        load.busy = worker.busySum[w] / samples;
        load.game = worker.gameSum[w] / samples;
    }
    worker.busySum[w] = worker.gameSum[w] = 0;
    worker.samples[w] = 0;
    return load;
}

}  // namespace simqvfr

#include "cpu_load.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <intrin.h>
#include <windows.h>
#include <tlhelp32.h>

#include "../log.h"

namespace aceqv {
namespace {

struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false, running = false;
    CpuLoad latest;
    std::atomic<uint32_t> render{0};
    double busiestSum[2] = {}, renderSum[2] = {}, processSum[2] = {};  // cpu_load_window's, per window
    int samples[2] = {}, renderSamples[2] = {};
};
Worker worker;

// This process's threads and their cycles so far.
std::map<uint32_t, uint64_t> snapshot() {
    std::map<uint32_t, uint64_t> cycles;
    const DWORD pid = GetCurrentProcessId();
    HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (all == INVALID_HANDLE_VALUE)
        return cycles;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(all, &entry); more; more = Thread32Next(all, &entry)) {
        if (entry.th32OwnerProcessID != pid)
            continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!t)
            continue;
        ULONG64 c = 0;
        if (QueryThreadCycleTime(t, &c))
            cycles[entry.th32ThreadID] = c;
        CloseHandle(t);
    }
    CloseHandle(all);
    return cycles;
}

// A thread's description (Windows 10 1607 on), or empty.
std::wstring describe(uint32_t id) {
    using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get = reinterpret_cast<Describe>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    std::wstring name;
    HANDLE t = get ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id) : nullptr;
    if (!t)
        return name;
    PWSTR text = nullptr;
    if (SUCCEEDED(get(t, &text)) && text) {
        name = text;
        LocalFree(text);
    }
    CloseHandle(t);
    return name;
}

void run() {
    std::map<uint32_t, uint64_t> before = snapshot();
    uint64_t tsc = __rdtsc();
    std::unique_lock lock(worker.mutex);
    while (!worker.stop) {
        worker.wake.wait_for(lock, std::chrono::milliseconds(kCpuLoadMs), [] { return worker.stop; });
        if (worker.stop)
            break;
        lock.unlock();
        const std::map<uint32_t, uint64_t> after = snapshot();
        const uint64_t now = __rdtsc();
        CpuLoad load = cpu_load_from(before, after, now - tsc, worker.render.load());
        if (load.valid)
            load.busiestName = describe(load.busiestThread);
        before = after;
        tsc = now;
        lock.lock();
        worker.latest = load;
        if (load.valid)
            for (int w = 0; w < 2; ++w) {
                worker.busiestSum[w] += load.busiestPercent;
                worker.processSum[w] += load.processPercent;
                ++worker.samples[w];
                if (load.renderPercent >= 0) {
                    worker.renderSum[w] += load.renderPercent;
                    ++worker.renderSamples[w];
                }
            }
    }
}

}  // namespace

CpuLoad cpu_load_from(const std::map<uint32_t, uint64_t>& before, const std::map<uint32_t, uint64_t>& after,
                      uint64_t elapsedCycles, uint32_t renderThread) {
    CpuLoad load;
    if (!elapsedCycles)
        return load;
    for (const auto& [id, cycles] : after) {
        const auto was = before.find(id);
        if (was == before.end() || cycles < was->second)
            continue;  // new this span: its cycles are not all from it
        const double percent = 100.0 * static_cast<double>(cycles - was->second) / static_cast<double>(elapsedCycles);
        ++load.threads;
        load.processPercent += percent;
        if (id == renderThread)
            load.renderPercent = percent;
        if (!load.valid || percent > load.busiestPercent) {
            load.valid = true;
            load.busiestThread = id;
            load.busiestPercent = percent;
        }
    }
    return load;
}

bool cpu_load_start() {
    std::lock_guard guard(worker.mutex);
    if (worker.running)
        return true;
    worker.stop = false;
    worker.latest = CpuLoad{};
    worker.running = true;
    worker.thread = std::thread(run);
    logf("cpu load: sampling ACE's threads every %d ms", kCpuLoadMs);
    return true;
}

void cpu_load_stop() {
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

void cpu_load_render_thread(uint32_t thread) { worker.render.store(thread, std::memory_order_relaxed); }

CpuLoad cpu_load_latest() {
    std::lock_guard guard(worker.mutex);
    return worker.latest;
}

CpuLoad cpu_load_window(int& samples, int window) {
    std::lock_guard guard(worker.mutex);
    const int w = window == 0 ? 0 : 1;
    CpuLoad load = worker.latest;
    samples = worker.samples[w];
    load.valid = samples > 0;
    if (samples) {
        load.busiestPercent = worker.busiestSum[w] / samples;
        load.processPercent = worker.processSum[w] / samples;
    }
    load.renderPercent = worker.renderSamples[w] ? worker.renderSum[w] / worker.renderSamples[w] : -1;
    worker.busiestSum[w] = worker.renderSum[w] = worker.processSum[w] = 0;
    worker.samples[w] = worker.renderSamples[w] = 0;
    return load;
}

}  // namespace aceqv

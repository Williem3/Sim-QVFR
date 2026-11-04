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
    double busiestSum[3] = {}, renderSum[3] = {}, processSum[3] = {};  // cpu_load_window's, per window
    int samples[3] = {}, renderSamples[3] = {};
    std::map<uint32_t, double> threadSum;  // cpu_load_threads': each thread's share summed over its samples
    std::map<uint32_t, double> latestShares;  // the latest sample's
    int threadSamples = 0;
    // Left running at exit (no xrDestroySession), the thread would end the process in std::terminate. At a
    // process exit it has ended, so the join returns at once.
    ~Worker() {
        {
            std::lock_guard guard(mutex);
            stop = true;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
    }
};
Worker worker;

// This process's threads, kept open between samples. Listing them takes a snapshot of every thread on the
// system (thousands): done every kRefreshSamples samples, not every one (every half second it cost the
// sampler ~15% of a core, CPU profile 2026-11-03). Between refreshes a new thread is not seen yet.
constexpr int kRefreshSamples = 10;
std::map<uint32_t, HANDLE> kept;  // the worker's only

void refresh_threads() {
    const DWORD pid = GetCurrentProcessId();
    HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (all == INVALID_HANDLE_VALUE)
        return;
    std::map<uint32_t, HANDLE> listed;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(all, &entry); more; more = Thread32Next(all, &entry)) {
        if (entry.th32OwnerProcessID != pid)
            continue;
        const auto known = kept.find(entry.th32ThreadID);
        DWORD code = 0;
        // A known id is the same thread unless that one has ended (ids are reused).
        if (known != kept.end() && GetExitCodeThread(known->second, &code) && code == STILL_ACTIVE) {
            listed[entry.th32ThreadID] = known->second;
            kept.erase(known);
        } else if (HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
            listed[entry.th32ThreadID] = t;
        }
    }
    CloseHandle(all);
    for (const auto& [id, handle] : kept)  // ended, or reused by another
        CloseHandle(handle);
    kept.swap(listed);
}

void close_threads() {
    for (const auto& [id, handle] : kept)
        CloseHandle(handle);
    kept.clear();
}

// The kept threads' cycles so far.
std::map<uint32_t, uint64_t> snapshot() {
    std::map<uint32_t, uint64_t> cycles;
    for (const auto& [id, handle] : kept) {
        ULONG64 c = 0;
        if (QueryThreadCycleTime(handle, &c))
            cycles[id] = c;
    }
    return cycles;
}

// A thread's description (Windows 10 1607 on), or empty.
// For a thread without a description: the module its start address is in, "(aSeeVRClient.dll)" (empty if
// unknown, or ACE's own). The eye tracker's client and drivers start threads ACE never names.
std::wstring started_in(uint32_t id) {
    using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    constexpr int kThreadQuerySetWin32StartAddress = 9;
    HANDLE t = query ? OpenThread(THREAD_QUERY_INFORMATION, FALSE, id) : nullptr;
    if (!t)
        return {};
    void* start = nullptr;
    const bool ok = query(t, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) >= 0 && start;
    CloseHandle(t);
    HMODULE module = nullptr;
    if (!ok || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(start), &module))
        return {};
    if (module == GetModuleHandleW(nullptr))  // the game's own exe
        return {};
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* slash = wcsrchr(path, L'\\');
    const std::wstring file = slash ? slash + 1 : path;
    if (file.empty())
        return {};
    return L"(" + file + L")";
}

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
    if (name.empty())
        name = started_in(id);
    return name;
}

void run() {
    refresh_threads();
    std::map<uint32_t, uint64_t> before = snapshot();
    uint64_t tsc = __rdtsc();
    int sampled = 0;
    std::map<uint32_t, bool> spins;  // thread id -> it spins (cpu_load_spins), cleared at each refresh (ids are reused)
    std::unique_lock lock(worker.mutex);
    while (!worker.stop) {
        worker.wake.wait_for(lock, std::chrono::milliseconds(kCpuLoadMs), [] { return worker.stop; });
        if (worker.stop)
            break;
        lock.unlock();
        // A refresh only adds threads (they count from their next sample, as cpu_load_from skips new ones).
        if (++sampled % kRefreshSamples == 0) {
            refresh_threads();
            spins.clear();
        }
        const std::map<uint32_t, uint64_t> after = snapshot();
        const uint64_t now = __rdtsc();
        const std::map<uint32_t, double> shares = cpu_load_shares(before, after, now - tsc);
        // The spinning threads among the busiest: described busiest first, until one that does not spin.
        std::vector<std::pair<double, uint32_t>> busiest;
        for (const auto& [id, percent] : shares)
            busiest.push_back({percent, id});
        std::sort(busiest.rbegin(), busiest.rend());
        std::set<uint32_t> spinning;
        for (const auto& [percent, id] : busiest) {
            auto known = spins.find(id);
            if (known == spins.end())
                known = spins.emplace(id, cpu_load_spins(describe(id))).first;
            if (!known->second)
                break;
            spinning.insert(id);
        }
        CpuLoad load = cpu_load_from(before, after, now - tsc, worker.render.load(), spinning);
        if (load.busiestThread)
            load.busiestName = describe(load.busiestThread);
        before = after;
        tsc = now;
        lock.lock();
        for (const auto& [id, percent] : shares)
            worker.threadSum[id] += percent;
        worker.latestShares = shares;
        ++worker.threadSamples;
        worker.latest = load;
        if (load.valid)
            for (int w = 0; w < 3; ++w) {
                worker.busiestSum[w] += load.busiestPercent;
                worker.processSum[w] += load.processPercent;
                ++worker.samples[w];
                if (load.renderPercent >= 0) {
                    worker.renderSum[w] += load.renderPercent;
                    ++worker.renderSamples[w];
                }
            }
    }
    lock.unlock();
    close_threads();
}

}  // namespace

CpuLoad cpu_load_from(const std::map<uint32_t, uint64_t>& before, const std::map<uint32_t, uint64_t>& after,
                      uint64_t elapsedCycles, uint32_t renderThread, const std::set<uint32_t>& spinning) {
    CpuLoad load;
    if (!elapsedCycles)
        return load;
    for (const auto& [id, cycles] : after) {
        const auto was = before.find(id);
        if (was == before.end() || cycles < was->second)
            continue;  // new this span: its cycles are not all from it
        const double percent = 100.0 * static_cast<double>(cycles - was->second) / static_cast<double>(elapsedCycles);
        load.valid = true;
        ++load.threads;
        load.processPercent += percent;
        if (id == renderThread)
            load.renderPercent = percent;
        if (!spinning.count(id) && (!load.busiestThread || percent > load.busiestPercent)) {
            load.busiestThread = id;
            load.busiestPercent = percent;
        }
    }
    return load;
}

bool cpu_load_spins(const std::wstring& name) {
    return name == L"Physics" || name.find(L"aSeeVRClient") != std::wstring::npos;
}

std::map<uint32_t, double> cpu_load_shares(const std::map<uint32_t, uint64_t>& before,
                                           const std::map<uint32_t, uint64_t>& after, uint64_t elapsedCycles) {
    std::map<uint32_t, double> shares;
    if (!elapsedCycles)
        return shares;
    for (const auto& [id, cycles] : after) {
        const auto was = before.find(id);
        if (was != before.end() && cycles >= was->second)
            shares[id] = 100.0 * static_cast<double>(cycles - was->second) / static_cast<double>(elapsedCycles);
    }
    return shares;
}

bool cpu_load_start() {
    std::lock_guard guard(worker.mutex);
    if (worker.running)
        return true;
    worker.stop = false;
    worker.latest = CpuLoad{};
    worker.running = true;
    worker.thread = std::thread(run);
    logf("cpu load: sampling ACE's threads every %d ms (the busiest leaves out Physics and the eye tracker's client: they spin)",
         kCpuLoadMs);
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

std::vector<ThreadLoad> cpu_load_threads(size_t count) {
    std::map<uint32_t, double> sums;
    int samples = 0;
    {
        std::lock_guard guard(worker.mutex);
        sums.swap(worker.threadSum);
        samples = worker.threadSamples;
        worker.threadSamples = 0;
    }
    std::vector<ThreadLoad> threads;
    if (!samples)
        return threads;
    for (const auto& [id, sum] : sums)
        threads.push_back({id, sum / samples, {}});
    std::sort(threads.begin(), threads.end(), [](const ThreadLoad& a, const ThreadLoad& b) { return a.percent > b.percent; });
    if (threads.size() > count)
        threads.resize(count);
    for (ThreadLoad& t : threads)
        t.name = describe(t.id);
    return threads;
}

std::vector<ThreadLoad> cpu_load_latest_threads(size_t count) {
    std::vector<ThreadLoad> threads;
    {
        std::lock_guard guard(worker.mutex);
        for (const auto& [id, percent] : worker.latestShares)
            threads.push_back({id, percent, {}});
    }
    std::sort(threads.begin(), threads.end(), [](const ThreadLoad& a, const ThreadLoad& b) { return a.percent > b.percent; });
    if (threads.size() > count)
        threads.resize(count);
    for (ThreadLoad& t : threads)
        t.name = describe(t.id);
    return threads;
}

CpuLoad cpu_load_window(int& samples, int window) {
    std::lock_guard guard(worker.mutex);
    const int w = window == 0 || window == 2 ? window : 1;
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

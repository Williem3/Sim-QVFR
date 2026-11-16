#include "cpu_placement.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <intrin.h>
#include <windows.h>

#include "../log.h"

namespace simqvfr {

CpuKinds cpu_kinds_from(const std::vector<LogicalCpu>& cpus, bool amd) {
    CpuKinds kinds;
    std::vector<int> l3Order;  // the L3s in the order their first processor comes
    std::map<int, uint64_t> l3Bytes;
    std::set<uint8_t> classes;
    for (const LogicalCpu& c : cpus) {
        if (c.l3 >= 0 && !l3Bytes.count(c.l3)) {
            l3Order.push_back(c.l3);
            l3Bytes[c.l3] = c.l3Bytes;
        }
        classes.insert(c.efficiency);
    }
    uint64_t largest = 0, smallest = UINT64_MAX;
    for (const auto& [id, bytes] : l3Bytes) {
        largest = std::max(largest, bytes);
        smallest = std::min(smallest, bytes);
    }
    const bool byL3 = l3Order.size() > 1, byClass = classes.size() > 1;
    const bool sizesDiffer = byL3 && largest != smallest;
    const auto mb = [](uint64_t bytes) { return static_cast<unsigned>(bytes / (1024 * 1024)); };
    std::map<std::pair<int, int>, int> index;  // (L3, class) -> kind
    std::vector<std::set<int>> cores;
    kinds.kindOf.reserve(cpus.size());
    for (const LogicalCpu& c : cpus) {
        const std::pair<int, int> key{byL3 ? c.l3 : -1, byClass ? c.efficiency : 0};
        auto known = index.find(key);
        if (known == index.end()) {
            std::string label;
            if (byL3) {
                const auto at = std::find(l3Order.begin(), l3Order.end(), c.l3);
                const int ordinal = at == l3Order.end() ? -1 : static_cast<int>(at - l3Order.begin());
                char text[96];
                if (c.l3 < 0)
                    std::snprintf(text, sizeof(text), "no L3");
                else
                    std::snprintf(text, sizeof(text), "%s%d %u MB L3%s", amd ? "CCD" : "L3 group ", ordinal, mb(c.l3Bytes),
                                  amd && sizesDiffer && c.l3Bytes == largest ? " (V-Cache)" : "");
                label = text;
            }
            if (byClass) {
                const char* name = c.efficiency == *classes.rbegin() ? "P-cores"
                                   : c.efficiency == *classes.begin() ? "E-cores"
                                                                      : nullptr;
                std::string part = name ? name : "class " + std::to_string(c.efficiency);
                label = label.empty() ? part : label + " " + part;
            }
            if (label.empty()) {
                label = "all";
                if (c.l3 >= 0)
                    label += ", " + std::to_string(mb(c.l3Bytes)) + " MB L3";
            }
            known = index.emplace(key, static_cast<int>(kinds.labels.size())).first;
            kinds.labels.push_back(label);
            kinds.logical.push_back(0);
            cores.emplace_back();
        }
        const int k = known->second;
        kinds.kindOf.push_back(k);
        ++kinds.logical[k];
        cores[k].insert(c.core);
    }
    for (const std::set<int>& s : cores)
        kinds.cores.push_back(static_cast<int>(s.size()));
    return kinds;
}

std::vector<double> processor_busy_from(const std::vector<uint64_t>& idleBefore, const std::vector<uint64_t>& idleAfter,
                                        uint64_t elapsedCycles) {
    std::vector<double> busy;
    if (!elapsedCycles || idleBefore.size() != idleAfter.size())
        return busy;
    busy.reserve(idleAfter.size());
    for (size_t i = 0; i < idleAfter.size(); ++i) {
        const uint64_t idle = idleAfter[i] >= idleBefore[i] ? idleAfter[i] - idleBefore[i] : 0;
        const double share = 100.0 * (1.0 - static_cast<double>(idle) / static_cast<double>(elapsedCycles));
        busy.push_back(std::clamp(share, 0.0, 100.0));
    }
    return busy;
}

std::vector<LogicalCpu> cpu_topology() {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &size);
    std::vector<uint8_t> buffer(size);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!size || !GetLogicalProcessorInformationEx(RelationAll, first, &size))
        return {};
    std::map<std::pair<uint16_t, uint8_t>, LogicalCpu> byNumber;
    const auto each = [&](const GROUP_AFFINITY* masks, WORD count, auto&& set) {
        for (WORD g = 0; g < std::max<WORD>(count, 1); ++g)
            for (uint8_t bit = 0; bit < 64; ++bit)
                if (masks[g].Mask & (KAFFINITY{1} << bit)) {
                    LogicalCpu& c = byNumber[{masks[g].Group, bit}];
                    c.group = masks[g].Group;
                    c.number = bit;
                    set(c);
                }
    };
    int core = 0, l3 = 0;
    for (DWORD at = 0; at < size;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
        if (info->Relationship == RelationProcessorCore) {
            const PROCESSOR_RELATIONSHIP& p = info->Processor;
            each(p.GroupMask, p.GroupCount, [&](LogicalCpu& c) {
                c.core = core;
                c.efficiency = p.EfficiencyClass;
            });
            ++core;
        } else if (info->Relationship == RelationCache && info->Cache.Level == 3) {
            const CACHE_RELATIONSHIP& cache = info->Cache;
            each(&cache.GroupMask, cache.GroupCount, [&](LogicalCpu& c) {
                c.l3 = l3;
                c.l3Bytes = cache.CacheSize;
            });
            ++l3;
        }
        if (!info->Size)
            break;
        at += info->Size;
    }
    std::vector<LogicalCpu> cpus;
    for (const auto& [key, c] : byNumber)
        cpus.push_back(c);
    return cpus;
}

std::string cpu_brand(bool* amd) {
    int regs[4] = {};
    __cpuid(regs, 0);
    char vendor[13] = {};
    std::memcpy(vendor, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    if (amd)
        *amd = std::string(vendor) == "AuthenticAMD";
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004)
        return vendor;
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + 16 * i, regs, 16);
    }
    std::string text = brand;
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    const size_t start = text.find_first_not_of(' ');
    return start == std::string::npos ? std::string(vendor) : text.substr(start);
}

double sleep_one_ms(int count) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    double sum = 0;
    for (int i = 0; i < count; ++i) {
        QueryPerformanceCounter(&t0);
        Sleep(1);
        QueryPerformanceCounter(&t1);
        sum += 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    }
    return count > 0 ? sum / count : 0;
}

const char* priority_class_name(uint32_t priorityClass) {
    switch (priorityClass) {
    case IDLE_PRIORITY_CLASS: return "Idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "Below normal";
    case NORMAL_PRIORITY_CLASS: return "Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "Above normal";
    case HIGH_PRIORITY_CLASS: return "High";
    case REALTIME_PRIORITY_CLASS: return "Realtime";
    default: return "unknown";
    }
}

namespace {

constexpr int kMaxKinds = 8;
constexpr int kMaxGroups = 4;
constexpr int kWorkerMs = 5000;

struct Placement {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false, running = false;
    std::vector<LogicalCpu> cpus;
    CpuKinds kinds;
    int8_t kindAt[kMaxGroups][64];  // (group, number) -> kind, -1 none
    std::atomic<uint32_t> renderOn[kMaxKinds];
    std::atomic<uint32_t> renderElsewhere{0};
    // The worker's samples since the last report.
    int samples = 0, front = 0, minimized = 0, noWindow = 0, gameModeOn = 0, gameModeKnown = 0;
    double sleepSum = 0, sleepMax = 0;
    // The last report's idle cycles per processor (cpus' order) and time-stamp counter.
    std::vector<uint64_t> idle;
    uint64_t idleTsc = 0;
    Placement() {
        std::memset(kindAt, -1, sizeof(kindAt));
        for (auto& n : renderOn)
            n.store(0);
    }
    ~Placement() {
        {
            std::lock_guard guard(mutex);
            stop = true;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
    }
};
Placement placement;

std::vector<uint64_t> idle_cycles(const std::vector<LogicalCpu>& cpus) {
    std::vector<uint64_t> out(cpus.size(), 0);
    std::map<uint16_t, std::vector<ULONG64>> groups;
    for (const LogicalCpu& c : cpus) {
        auto& g = groups[c.group];
        if (g.empty()) {
            const DWORD n = GetMaximumProcessorCount(c.group);
            g.assign(std::max<DWORD>(n, 1), 0);
            ULONG length = static_cast<ULONG>(g.size() * sizeof(ULONG64));
            if (!QueryIdleProcessorCycleTimeEx(c.group, &length, g.data()))
                std::fill(g.begin(), g.end(), 0);
        }
    }
    for (size_t i = 0; i < cpus.size(); ++i) {
        const auto& g = groups[cpus[i].group];
        if (cpus[i].number < g.size())
            out[i] = g[cpus[i].number];
    }
    return out;
}

// Parked logical processors per kind (GetSystemCpuSetInformation): AMD's driver parks a dual-CCD X3D's other CCD
// under Game Mode.
std::vector<int> parked_per_kind() {
    std::vector<int> parked(placement.kinds.labels.size(), 0);
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (!length)
        return parked;
    std::vector<uint8_t> buffer(length);
    if (!GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()), length, &length,
                                    GetCurrentProcess(), 0))
        return parked;
    for (ULONG at = 0; at < length;) {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + at);
        if (info->Type == CpuSetInformation && info->CpuSet.Parked && info->CpuSet.Group < kMaxGroups &&
            info->CpuSet.LogicalProcessorIndex < 64) {
            const int k = placement.kindAt[info->CpuSet.Group][info->CpuSet.LogicalProcessorIndex];
            if (k >= 0 && k < static_cast<int>(parked.size()))
                ++parked[k];
        }
        if (!info->Size)
            break;
        at += info->Size;
    }
    return parked;
}

// The process's own window that is the largest visible one: -1 none, 0 behind another, 1 in front, 2 minimized.
int window_state() {
    struct Find {
        DWORD pid;
        HWND best = nullptr;
        LONG64 area = 0;
    } find{GetCurrentProcessId()};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != f->pid || !IsWindowVisible(w))
                return TRUE;
            RECT r{};
            GetWindowRect(w, &r);
            const LONG64 area = static_cast<LONG64>(r.right - r.left) * (r.bottom - r.top);
            if (IsIconic(w) || area > f->area) {
                f->best = w;
                f->area = std::max<LONG64>(area, 1);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (!find.best)
        return -1;
    if (IsIconic(find.best))
        return 2;
    DWORD frontPid = 0;
    if (HWND front = GetForegroundWindow())
        GetWindowThreadProcessId(front, &frontPid);
    return frontPid == find.pid ? 1 : 0;
}

// Game Mode's resources for this process (HasExpandedResources): -1 unknown, 0 no, 1 yes.
int game_mode() {
    using Has = HRESULT(WINAPI*)(BOOL*);
    static const Has has = [] {
        HMODULE m = LoadLibraryExW(L"api-ms-win-gaming-expandedresources-l1-1-0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<Has>(reinterpret_cast<void*>(GetProcAddress(m, "HasExpandedResources"))) : nullptr;
    }();
    BOOL yes = FALSE;
    if (!has || FAILED(has(&yes)))
        return -1;
    return yes ? 1 : 0;
}

std::string power_plan() {
    using Active = DWORD(WINAPI*)(HKEY, GUID**);
    using Friendly = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);
    HMODULE m = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m)
        return "unknown";
    const auto active = reinterpret_cast<Active>(reinterpret_cast<void*>(GetProcAddress(m, "PowerGetActiveScheme")));
    const auto friendly = reinterpret_cast<Friendly>(reinterpret_cast<void*>(GetProcAddress(m, "PowerReadFriendlyName")));
    std::string name = "unknown";
    GUID* scheme = nullptr;
    if (active && friendly && active(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
        wchar_t text[128] = {};
        DWORD bytes = sizeof(text) - sizeof(wchar_t);
        if (friendly(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR*>(text), &bytes) == ERROR_SUCCESS) {
            char narrow[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
            name = narrow;
        }
        LocalFree(scheme);
    }
    FreeLibrary(m);
    return name;
}

std::string power_throttling() {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)))
        return "EcoQoS unknown";
    std::string text = !(s.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) ? "EcoQoS left to Windows"
                       : (s.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED)  ? "EcoQoS on (set)"
                                                                                   : "EcoQoS off (set: full speed)";
    constexpr ULONG kIgnoreTimer = 0x4;  // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    if (s.ControlMask & kIgnoreTimer)
        text += (s.StateMask & kIgnoreTimer) ? ", timer requests ignored when hidden (set)" : ", timer requests kept when hidden (set)";
    return text;
}

std::string process_state() {
    char text[320];
    DWORD_PTR mine = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &mine, &system);
    ULONG sets = 0;
    const BOOL noSets = GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &sets);
    const int own = static_cast<int>(__popcnt64(mine)), all = static_cast<int>(__popcnt64(system));
    char affinity[64];
    if (own == all)
        std::snprintf(affinity, sizeof(affinity), "all %d", all);
    else
        std::snprintf(affinity, sizeof(affinity), "%d of %d (mask 0x%llx)", own, all, static_cast<unsigned long long>(mine));
    char cpuSets[48];
    if (noSets || !sets)
        std::snprintf(cpuSets, sizeof(cpuSets), "none");
    else
        std::snprintf(cpuSets, sizeof(cpuSets), "%lu", sets);
    std::snprintf(text, sizeof(text), "priority %s, affinity %s, default CPU sets %s, %s",
                  priority_class_name(GetPriorityClass(GetCurrentProcess())), affinity, cpuSets, power_throttling().c_str());
    return text;
}

void run() {
    std::unique_lock lock(placement.mutex);
    while (!placement.stop) {
        lock.unlock();
        const double sleep = sleep_one_ms();
        const int window = window_state();
        const int mode = game_mode();
        lock.lock();
        ++placement.samples;
        placement.sleepSum += sleep;
        placement.sleepMax = std::max(placement.sleepMax, sleep);
        placement.front += window == 1;
        placement.minimized += window == 2;
        placement.noWindow += window < 0;
        placement.gameModeKnown += mode >= 0;
        placement.gameModeOn += mode == 1;
        placement.wake.wait_for(lock, std::chrono::milliseconds(kWorkerMs), [] { return placement.stop; });
    }
}

std::string kind_of_processor(const PROCESSOR_NUMBER& n) {
    if (n.Group >= kMaxGroups || n.Number >= 64)
        return "?";
    const int k = placement.kindAt[n.Group][n.Number];
    return k >= 0 ? placement.kinds.labels[k] : "?";
}

}  // namespace

void cpu_placement_start() {
    {
        std::lock_guard guard(placement.mutex);
        if (placement.running)
            return;
        placement.running = true;
        placement.stop = false;
    }
    bool amd = false;
    const std::string brand = cpu_brand(&amd);
    placement.cpus = cpu_topology();
    placement.kinds = cpu_kinds_from(placement.cpus, amd);
    for (size_t i = 0; i < placement.cpus.size(); ++i) {
        const LogicalCpu& c = placement.cpus[i];
        if (c.group < kMaxGroups && c.number < 64 && placement.kinds.kindOf[i] < kMaxKinds)
            placement.kindAt[c.group][c.number] = static_cast<int8_t>(placement.kinds.kindOf[i]);
    }
    std::string kinds;
    for (size_t k = 0; k < placement.kinds.labels.size(); ++k) {
        char item[160];
        std::snprintf(item, sizeof(item), "%s%s (%d logical, %d cores)", kinds.empty() ? "" : "; ",
                      placement.kinds.labels[k].c_str(), placement.kinds.logical[k], placement.kinds.cores[k]);
        kinds += item;
    }
    int cores = 0;
    for (int c : placement.kinds.cores)
        cores += c;
    logf("cpu placement (read only, nothing set): %s, %zu logical processors, %d cores, %zu kind%s: %s; power plan \"%s\"",
         brand.c_str(), placement.cpus.size(), cores, placement.kinds.labels.size(),
         placement.kinds.labels.size() == 1 ? "" : "s", kinds.c_str(), power_plan().c_str());
    logf("cpu placement: the process at its start: %s; Sleep(1) takes %.1f ms here; Game Mode %s", process_state().c_str(),
         sleep_one_ms(), game_mode() == 1 ? "yes" : game_mode() == 0 ? "no" : "unknown");
    placement.idle = idle_cycles(placement.cpus);
    placement.idleTsc = __rdtsc();
    std::lock_guard guard(placement.mutex);
    placement.thread = std::thread(run);
}

void cpu_placement_stop() {
    std::thread thread;
    {
        std::lock_guard guard(placement.mutex);
        if (!placement.running)
            return;
        placement.stop = true;
        placement.running = false;
        thread.swap(placement.thread);
    }
    placement.wake.notify_all();
    if (thread.joinable())
        thread.join();
}

void cpu_placement_frame() {
    PROCESSOR_NUMBER n;
    GetCurrentProcessorNumberEx(&n);
    const int k = n.Group < kMaxGroups && n.Number < 64 ? placement.kindAt[n.Group][n.Number] : -1;
    if (k >= 0)
        placement.renderOn[k].fetch_add(1, std::memory_order_relaxed);
    else
        placement.renderElsewhere.fetch_add(1, std::memory_order_relaxed);
}

void cpu_placement_report(const std::vector<ThreadLoad>& busiest) {
    const size_t kinds = placement.kinds.labels.size();
    if (!kinds)
        return;
    // The render thread's kinds, at its xrEndFrame calls.
    uint32_t frames = placement.renderElsewhere.exchange(0);
    std::vector<uint32_t> on(kinds);
    for (size_t k = 0; k < kinds && k < kMaxKinds; ++k)
        frames += on[k] = placement.renderOn[k].exchange(0);
    std::string render;
    for (size_t k = 0; k < kinds && frames; ++k)
        if (on[k]) {
            char item[128];
            std::snprintf(item, sizeof(item), "%s%s %.0f%%", render.empty() ? "" : ", ", placement.kinds.labels[k].c_str(),
                          100.0 * on[k] / frames);
            render += item;
        }
    // Every process's load per kind since the last report, and what is parked now.
    const std::vector<uint64_t> idle = idle_cycles(placement.cpus);
    const uint64_t tsc = __rdtsc();
    const std::vector<double> busy = processor_busy_from(placement.idle, idle, tsc - placement.idleTsc);
    placement.idle = idle;
    placement.idleTsc = tsc;
    std::vector<double> kindBusy(kinds, 0);
    for (size_t i = 0; i < busy.size(); ++i)
        kindBusy[placement.kinds.kindOf[i]] += busy[i];
    const std::vector<int> parked = parked_per_kind();
    std::string load;
    for (size_t k = 0; k < kinds; ++k) {
        char item[160];
        std::snprintf(item, sizeof(item), "%s%s %.0f%% (%d of %d parked)", load.empty() ? "" : ", ",
                      placement.kinds.labels[k].c_str(), kindBusy[k] / std::max(placement.kinds.logical[k], 1), parked[k],
                      placement.kinds.logical[k]);
        load += item;
    }
    // The busiest threads (5% of a core or more): their ideal processor's kind and their priority.
    std::string threads;
    int listed = 0;
    for (const ThreadLoad& t : busiest) {
        if (t.percent < 5 || listed == 4)
            break;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, t.id);
        if (!h)
            continue;
        PROCESSOR_NUMBER ideal{};
        const bool known = GetThreadIdealProcessorEx(h, &ideal);
        const int priority = GetThreadPriority(h);
        ULONG sets = 0;
        const BOOL noSets = GetThreadSelectedCpuSets(h, nullptr, 0, &sets);
        CloseHandle(h);
        // The ideal processor's kind only says something where there is more than one.
        const std::string where = kinds > 1 ? " ideal " + (known ? kind_of_processor(ideal) : std::string("?")) + "," : "";
        char item[256];
        std::snprintf(item, sizeof(item), "%s%ls%s%u %.0f%%%s priority %d%s", threads.empty() ? "" : "; ", t.name.c_str(),
                      t.name.empty() ? "" : " ", t.id, t.percent, where.c_str(), priority,
                      noSets || !sets ? "" : ", own CPU sets");
        threads += item;
        ++listed;
    }
    int samples, front, minimized, noWindow, modeOn, modeKnown;
    double sleepMean, sleepMax;
    {
        std::lock_guard guard(placement.mutex);
        samples = placement.samples;
        front = placement.front;
        minimized = placement.minimized;
        noWindow = placement.noWindow;
        modeOn = placement.gameModeOn;
        modeKnown = placement.gameModeKnown;
        sleepMean = samples ? placement.sleepSum / samples : 0;
        sleepMax = placement.sleepMax;
        placement.samples = placement.front = placement.minimized = placement.noWindow = 0;
        placement.gameModeOn = placement.gameModeKnown = 0;
        placement.sleepSum = placement.sleepMax = 0;
    }
    char state[256] = "no samples";
    if (samples)
        std::snprintf(state, sizeof(state),
                      "Sleep(1) %.1f ms (max %.1f); window in front %d of %d samples, minimized %d, none %d; Game Mode %s",
                      sleepMean, sleepMax, front, samples, minimized, noWindow,
                      !modeKnown ? "unknown" : modeOn == modeKnown ? "yes" : modeOn ? "partly" : "no");
    logf("cpu placement: render thread on %s (%u frames); busy, every process: %s; %s; %s",
         render.empty() ? "?" : render.c_str(), frames, load.c_str(), process_state().c_str(), state);
    if (!threads.empty())
        logf("cpu placement: busiest threads: %s", threads.c_str());
}

}  // namespace simqvfr

#include "cpu_isolation.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <intrin.h>
#include <windows.h>

#include "../log.h"

namespace simqvfr {

IsolationPlan isolation_plan(const std::vector<LogicalCpu>& cpus) {
    IsolationPlan plan;
    if (cpus.empty())
        return plan;
    std::pair<uint64_t, int> best{0, -1};
    for (const LogicalCpu& c : cpus)
        best = std::max(best, std::pair<uint64_t, int>{c.l3Bytes, c.efficiency});
    int coreZero = -1;
    for (const LogicalCpu& c : cpus)
        if (c.group == 0 && c.number == 0)
            coreZero = c.core;
    for (const LogicalCpu& c : cpus)
        if (c.l3Bytes == best.first && c.efficiency == best.second && c.core != coreZero)
            plan.core = std::max(plan.core, c.core);
    if (plan.core < 0)
        return plan;
    for (int i = 0; i < static_cast<int>(cpus.size()); ++i)
        (cpus[i].core == plan.core ? plan.mainCpus : plan.others).push_back(i);
    std::set<int> cores;
    for (const LogicalCpu& c : cpus)
        cores.insert(c.core);
    if (static_cast<int>(cores.size()) < kIsolationMinCores) {
        plan.why = "fewer than 6 cores: one taken from the game would cost more than it saves";
        return plan;
    }
    if (plan.mainCpus.size() < 2) {
        plan.why = "one thread per core (no SMT): no other half of a core to keep free";
        return plan;
    }
    plan.valid = !plan.others.empty();
    return plan;
}

SideStats side_stats(std::vector<float> cpuMs, std::vector<float> frameMs, int onCore) {
    SideStats s;
    s.frames = static_cast<int>(cpuMs.size());
    if (cpuMs.empty())
        return s;
    const auto mean = [](const std::vector<float>& v) {
        double sum = 0;
        for (float x : v)
            sum += x;
        return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
    };
    const auto p90 = [](std::vector<float>& v) {
        if (v.empty())
            return 0.0;
        const size_t at = std::min(v.size() - 1, v.size() * 9 / 10);
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
        return static_cast<double>(v[at]);
    };
    s.cpuMean = mean(cpuMs);
    s.frameMean = mean(frameMs);
    s.cpuP90 = p90(cpuMs);
    s.frameP90 = p90(frameMs);
    s.onCore = 100.0 * onCore / s.frames;
    return s;
}

namespace {

constexpr int kSettleFrames = 15;  // after a change of side, frames left out (the scheduler moving the threads)
constexpr int kLogFrames = 900;    // each side's frames per A/B line

struct Isolation {
    std::atomic<int> mode{0};
    // The render thread's own from here on.
    bool planned = false, isolated = false, abSide = true;
    bool refused = false, refusedLogged = false;  // CPU sets someone else set were there: left alone
    int moves = 0;                                // the render thread changed (logged a few times)
    IsolationPlan plan;
    std::vector<LogicalCpu> cpus;
    std::vector<ULONG> ids;  // CPU set id per cpus entry (0: unknown)
    HANDLE thread = nullptr;  // the thread given the core
    DWORD threadId = 0;
    int lastMode = 0, sideFrames = 0, settle = 0;
    ULONG64 lastCycles = 0;
    int64_t lastQpc = 0;
    uint64_t tsc0 = 0;
    int64_t qpc0 = 0;
    std::vector<float> cpuMs[2], frameMs[2];  // [1] isolated, [0] shared
    int onCore[2] = {};
};
Isolation iso;

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double qpc_ms(int64_t ticks) {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return static_cast<double>(ticks) / perMs;
}

// Time-stamp counter cycles per ms, from the span since the first measured frame.
double tsc_per_ms() {
    const double ms = qpc_ms(qpc() - iso.qpc0);
    return ms > 0 ? static_cast<double>(__rdtsc() - iso.tsc0) / ms : 0;
}

void plan_once() {
    if (iso.planned)
        return;
    iso.planned = true;
    iso.cpus = cpu_topology();
    iso.plan = isolation_plan(iso.cpus);
    iso.ids.assign(iso.cpus.size(), 0);
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<uint8_t> buffer(length);
    if (length && GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()), length, &length,
                                             GetCurrentProcess(), 0)) {
        std::map<std::pair<WORD, BYTE>, ULONG> byNumber;
        for (ULONG at = 0; at < length;) {
            const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + at);
            if (info->Type == CpuSetInformation)
                byNumber[{info->CpuSet.Group, info->CpuSet.LogicalProcessorIndex}] = info->CpuSet.Id;
            if (!info->Size)
                break;
            at += info->Size;
        }
        for (size_t i = 0; i < iso.cpus.size(); ++i) {
            const auto found = byNumber.find({iso.cpus[i].group, iso.cpus[i].number});
            if (found != byNumber.end())
                iso.ids[i] = found->second;
        }
    }
    for (ULONG id : iso.ids)
        iso.plan.valid &= id != 0;
    if (!iso.plan.valid)
        logf("cpu isolation: off on this PC (%s); nothing changes",
             iso.plan.why ? iso.plan.why : "its CPU sets are unknown");
}

std::vector<ULONG> ids_of(const std::vector<int>& indexes) {
    std::vector<ULONG> out;
    for (int i : indexes)
        out.push_back(iso.ids[i]);
    return out;
}

std::string logical_list(const std::vector<int>& indexes) {
    std::string text;
    for (int i : indexes)
        text += (text.empty() ? "" : "+") + std::to_string(iso.cpus[i].number);
    return text;
}

// Isolated: the process's default the other logical processors, this (the calling) thread the core's. Shared: ours
// taken back. CPU sets someone else set (the game, a tool such as Process Lasso) are never replaced or cleared: with
// any there, isolation stays off. A half-made change is undone, so no core is left unused.
bool apply(bool on) {
    if (!on) {
        if (!iso.isolated)
            return true;  // nothing of ours to take back
        bool ok = SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0) != FALSE;
        if (iso.thread)
            ok &= SetThreadSelectedCpuSets(iso.thread, nullptr, 0) != FALSE;
        iso.isolated = false;
        return ok;
    }
    if (iso.isolated && iso.threadId == GetCurrentThreadId())
        return true;
    ULONG processSets = 0, threadSets = 0;
    GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &processSets);
    GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0, &threadSets);
    // An affinity narrower than every processor (a Process Lasso rule pinning the game to one CCD, the game's own):
    // the player or the game chose the cores already.
    DWORD_PTR processMask = 0, systemMask = 0;
    const bool affinityKnown = GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) != FALSE;
    const bool narrowAffinity = affinityKnown && processMask != systemMask;
    iso.refused = processSets || threadSets || narrowAffinity;
    if (iso.refused) {
        if (!iso.refusedLogged) {
            if (narrowAffinity)
                logf("cpu isolation: left off: the game's affinity is %d of %d logical processors (mask 0x%llx), set by "
                     "the game or a tool such as Process Lasso; it stays as it is",
                     static_cast<int>(__popcnt64(processMask)), static_cast<int>(__popcnt64(systemMask)),
                     static_cast<unsigned long long>(processMask));
            else
                logf("cpu isolation: left off: the game already has CPU sets (the process %lu, the main thread %lu), "
                     "set by the game or a tool such as Process Lasso; they stay as they are",
                     static_cast<unsigned long>(processSets), static_cast<unsigned long>(threadSets));
        }
        iso.refusedLogged = true;
        return false;
    }
    if (iso.threadId != GetCurrentThreadId()) {
        if (iso.thread)
            CloseHandle(iso.thread);
        iso.thread = nullptr;
        iso.threadId = 0;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &iso.thread,
                             THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
            iso.thread = nullptr;
            return false;
        }
        iso.threadId = GetCurrentThreadId();
    }
    const std::vector<ULONG> others = ids_of(iso.plan.others), main = ids_of(iso.plan.mainCpus);
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), others.data(), static_cast<ULONG>(others.size())))
        return false;
    if (!SetThreadSelectedCpuSets(iso.thread, main.data(), static_cast<ULONG>(main.size()))) {
        SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);  // undone: the core free for every thread again
        return false;
    }
    iso.isolated = true;
    return true;
}

void log_change(bool on, bool ok, bool ab) {
    if (ab || (on && iso.refused))
        return;  // the A/B's sides are in its lines; a refusal has its own
    if (on)
        logf("cpu isolation: %s: the main thread %lu alone on core %d (logical %s); the game's other threads on the other %zu",
             ok ? "on" : "FAILED", static_cast<unsigned long>(GetCurrentThreadId()), iso.plan.core,
             logical_list(iso.plan.mainCpus).c_str(), iso.plan.others.size());
    else
        logf("cpu isolation: %s: the game's threads anywhere again", ok ? "off" : "off FAILED");
}

void reset_stats() {
    for (int s = 0; s < 2; ++s) {
        iso.cpuMs[s].clear();
        iso.frameMs[s].clear();
        iso.onCore[s] = 0;
    }
}

void log_ab() {
    const SideStats on = side_stats(iso.cpuMs[1], iso.frameMs[1], iso.onCore[1]);
    const SideStats off = side_stats(iso.cpuMs[0], iso.frameMs[0], iso.onCore[0]);
    const double d = on.cpuMean - off.cpuMean;
    logf("cpu isolation A/B (sides of %d frames; core %d, logical %s): isolated: main thread %.2f ms CPU per frame (p90 "
         "%.2f), frame %.2f ms (p90 %.2f), on its core %.0f%%, %d frames; shared: %.2f ms (p90 %.2f), frame %.2f ms (p90 "
         "%.2f), %d frames; isolated - shared %+.2f ms CPU (%+.1f%%)",
         iso.lastMode, iso.plan.core, logical_list(iso.plan.mainCpus).c_str(), on.cpuMean, on.cpuP90, on.frameMean,
         on.frameP90, on.onCore, on.frames, off.cpuMean, off.cpuP90, off.frameMean, off.frameP90, off.frames, d,
         off.cpuMean > 0 ? 100.0 * d / off.cpuMean : 0.0);
    reset_stats();
}

}  // namespace

void cpu_isolation_mode(int mode) { iso.mode.store(std::max(mode, 0), std::memory_order_relaxed); }

void cpu_isolation_frame() {
    const int mode = iso.mode.load(std::memory_order_relaxed);
    if (mode == 0 && iso.lastMode == 0)
        return;
    plan_once();
    if (!iso.plan.valid)
        return;
    // The frames now end on another thread: our sets taken back from the old one, and given to this one below.
    if (iso.isolated && GetCurrentThreadId() != iso.threadId) {
        const DWORD was = iso.threadId;
        apply(false);
        if (iso.moves++ < 3)
            logf("cpu isolation: the frames now end on thread %lu (was %lu): the core goes to it",
                 static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(was));
        if (iso.lastMode == 1)
            iso.lastMode = -1;  // re-applied below, logged as a change
    }
    if (mode != iso.lastMode) {
        const bool ab = mode > 1;
        if (iso.lastMode > 1 && !ab)
            reset_stats();
        if (ab)
            logf("cpu isolation: A/B of %d frames a side (main thread %lu, core %d, logical %s; the other %zu for the rest)",
                 mode, static_cast<unsigned long>(GetCurrentThreadId()), iso.plan.core,
                 logical_list(iso.plan.mainCpus).c_str(), iso.plan.others.size());
        const bool want = mode == 1 || ab;
        const bool ok = apply(want);
        log_change(want, ok, ab);
        iso.lastMode = mode;
        iso.abSide = want;
        iso.sideFrames = 0;
        iso.settle = kSettleFrames;
        iso.lastCycles = 0;
        if (mode == 0)
            return;
    }
    if (mode > 1 && ++iso.sideFrames >= mode) {
        iso.abSide = !iso.abSide;
        iso.sideFrames = 0;
        iso.settle = kSettleFrames;
        apply(iso.abSide);
    }
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    const int64_t now = qpc();
    if (!iso.tsc0) {
        iso.tsc0 = __rdtsc();
        iso.qpc0 = now;
    }
    if (mode > 1 && iso.lastCycles && iso.settle == 0) {
        const double perMs = tsc_per_ms();
        if (perMs > 0) {
            const int side = iso.isolated ? 1 : 0;
            iso.cpuMs[side].push_back(static_cast<float>(static_cast<double>(cycles - iso.lastCycles) / perMs));
            iso.frameMs[side].push_back(static_cast<float>(qpc_ms(now - iso.lastQpc)));
            PROCESSOR_NUMBER n;
            GetCurrentProcessorNumberEx(&n);
            for (int i : iso.plan.mainCpus)
                if (iso.cpus[i].group == n.Group && iso.cpus[i].number == n.Number)
                    ++iso.onCore[side];
        }
    }
    if (iso.settle > 0)
        --iso.settle;
    iso.lastCycles = cycles;
    iso.lastQpc = now;
    if (mode > 1 && static_cast<int>(iso.cpuMs[0].size()) >= kLogFrames && static_cast<int>(iso.cpuMs[1].size()) >= kLogFrames)
        log_ab();
}

void cpu_isolation_end() {
    apply(false);  // only ours, if any
    if (iso.thread)
        CloseHandle(iso.thread);
    iso.thread = nullptr;
    iso.threadId = 0;
    iso.lastMode = 0;
    iso.refused = iso.refusedLogged = false;
    iso.moves = 0;
    reset_stats();
}

}  // namespace simqvfr

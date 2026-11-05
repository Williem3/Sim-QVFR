// ACE's threads' CPU load, sampled every kCpuLoadMs on a worker: each thread's cycles (QueryThreadCycleTime)
// against the processor's time-stamp counter over the same span, i.e. its share of one core. The
// busiest thread says whether a thread other than the render thread (ACE's game or physics thread)
// holds the frame up, which the render thread's own time cannot show. It leaves out the threads that spin
// while they wait (cpu_load_spins): always near a whole core, they would read as holding every frame up.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace simqvfr {

inline constexpr int kCpuLoadMs = 500;

// One thread's load, averaged over a window: its share of one core, and its description when ACE set one.
struct ThreadLoad {
    uint32_t id = 0;
    double percent = 0;
    std::wstring name;
};

struct CpuLoad {
    bool valid = false;
    uint32_t busiestThread = 0;  // of the threads not left out (0: none)
    double busiestPercent = 0;  // of one core
    std::wstring busiestName;   // its description, when ACE set one
    double renderPercent = -1;  // the render thread (cpu_load_render_thread), -1: not seen
    double processPercent = 0;  // every thread together, in cores x 100
    int threads = 0;
};
// From two snapshots of thread id -> cycles and the time-stamp counter's cycles between them; the threads in
// `spinning` count in the process's total but are never the busiest.
CpuLoad cpu_load_from(const std::map<uint32_t, uint64_t>& before, const std::map<uint32_t, uint64_t>& after,
                      uint64_t elapsedCycles, uint32_t renderThread, const std::set<uint32_t>& spinning = {});
// A thread that spins while it waits, by its description: ACE's Physics (on QueryPerformanceCounter, for its
// next step) and the Pimax eye tracker's client (polling the tracker).
bool cpu_load_spins(const std::wstring& name);

bool cpu_load_start();
void cpu_load_stop();
// The thread that calls xrEndFrame (marked as the render thread in the load).
void cpu_load_render_thread(uint32_t thread);
CpuLoad cpu_load_latest();
// The average of every sample since the last call (the log's window: the busiest thread's share and the
// render thread's averaged; the name and id the last sample's), and resets.
CpuLoad cpu_load_window(int& samples, int window = 1);  // 0 the overlay's, 1 the log's, 2 the QVFR optimizer's
// Every thread's average load since the last call, busiest first (at most `count`), and resets. A thread
// counts from the samples it was seen in, over the whole window (a thread that ran only part of it shows less).
std::vector<ThreadLoad> cpu_load_threads(size_t count);
// The threads' loads in the latest sample (half a second), busiest first (at most `count`); nothing is reset.
std::vector<ThreadLoad> cpu_load_latest_threads(size_t count);
// Per-thread shares from two snapshots, as cpu_load_from (offline tests).
std::map<uint32_t, double> cpu_load_shares(const std::map<uint32_t, uint64_t>& before,
                                           const std::map<uint32_t, uint64_t>& after, uint64_t elapsedCycles);

}  // namespace simqvfr

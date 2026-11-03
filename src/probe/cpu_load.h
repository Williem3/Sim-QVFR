// ACE's threads' CPU load, sampled every kCpuLoadMs on a worker: each thread's cycles (QueryThreadCycleTime)
// against the processor's time-stamp counter over the same span, i.e. its share of one core. The
// busiest thread says whether a thread other than the render thread (ACE's game or physics thread)
// holds the frame up, which the render thread's own time cannot show.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace aceqv {

inline constexpr int kCpuLoadMs = 500;

struct CpuLoad {
    bool valid = false;
    uint32_t busiestThread = 0;
    double busiestPercent = 0;  // of one core
    std::wstring busiestName;   // its description, when ACE set one
    double renderPercent = -1;  // the render thread (cpu_load_render_thread), -1: not seen
    double processPercent = 0;  // every thread together, in cores x 100
    int threads = 0;
};
// From two snapshots of thread id -> cycles and the time-stamp counter's cycles between them.
CpuLoad cpu_load_from(const std::map<uint32_t, uint64_t>& before, const std::map<uint32_t, uint64_t>& after,
                      uint64_t elapsedCycles, uint32_t renderThread);

bool cpu_load_start();
void cpu_load_stop();
// The thread that calls xrEndFrame (marked as the render thread in the load).
void cpu_load_render_thread(uint32_t thread);
CpuLoad cpu_load_latest();
// The average of every sample since the last call (the log's window: the busiest thread's share and the
// render thread's averaged; the name and id the last sample's), and resets.
CpuLoad cpu_load_window(int& samples, int window = 1);  // 0 the overlay's, 1 the log's

}  // namespace aceqv

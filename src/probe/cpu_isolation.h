// Core isolation, an experiment ([experiment] cpu_isolate, live): the render thread (the game's main thread, the one
// that calls xrEndFrame) gets one physical core to itself. Its CPU sets are that core's logical processors, and the
// process's default CPU sets are every other one, so no other thread of the game (its physics thread spinning a core,
// its workers, the driver's, ours) runs on the core's other half, where it would share the core's execution units with
// the main thread. Other processes are not touched. Only the game's own process and thread scheduling change: no game
// code, no Windows setting.
//   cpu_isolate=0 off (the default), 1 on, N > 1 an interleaved A/B: N frames isolated, N shared, and so on, logging the
// main thread's CPU time per frame on each side (its cycles at each xrEndFrame), so one run measures the lever.
#pragma once

#include <cstdint>
#include <vector>

#include "probe/cpu_placement.h"

namespace simqvfr {

// The core picked for the main thread: of the fastest kind (the most L3, then the highest efficiency class), the
// highest-numbered core that is not core 0 (which takes most interrupts). Indexes into `cpus`.
struct IsolationPlan {
    bool valid = false;
    int core = -1;              // LogicalCpu::core
    std::vector<int> mainCpus;  // the core's logical processors
    std::vector<int> others;    // every other logical processor
};
IsolationPlan isolation_plan(const std::vector<LogicalCpu>& cpus);

// One side's main-thread CPU time per frame (ms) and frame intervals (ms): mean and 90th percentile.
struct SideStats {
    int frames = 0;
    double cpuMean = 0, cpuP90 = 0, frameMean = 0, frameP90 = 0;
    double onCore = 0;  // share of frames the main thread ended on the isolated core (percent)
};
SideStats side_stats(std::vector<float> cpuMs, std::vector<float> frameMs, int onCore);

// The live key: 0 off, 1 on, N > 1 the A/B's side length in frames.
void cpu_isolation_mode(int mode);
// On the render thread at each xrEndFrame: applies a change of side, measures, logs the A/B.
void cpu_isolation_frame();
// At the session's end: everything back as the game had it.
void cpu_isolation_end();

}  // namespace simqvfr

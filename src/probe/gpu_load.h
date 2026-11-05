// The whole GPU's load, from Windows' GPU engine counters (\GPU Engine(*)\Utilization Percentage, what
// Task Manager shows), sampled every kGpuLoadMs on a worker: the busiest 3D or compute engine of the
// game's adapter, counting every process (the runtime's compositor, frame generation, other apps), and
// ACE's own share of it. ACE's queue timestamps (gpu_frame_timer.h) see only ACE's work; this is what
// says whether the GPU itself has any time left.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace simqvfr {

inline constexpr int kGpuLoadMs = 250;

// One counter instance's name: "pid_1234_luid_0x00000000_0x0000D1B6_phys_0_eng_3_engtype_3D".
struct GpuEngineName {
    bool valid = false;
    uint32_t pid = 0;
    uint32_t luidHigh = 0, luidLow = 0;
    int engine = -1;
    std::wstring type;  // "3D", "Compute_0", "Copy", "VideoDecode", ...
};
GpuEngineName parse_gpu_engine_name(const std::wstring& name);

struct GpuLoad {
    bool valid = false;
    double busy = 0;     // percent: the adapter's busiest 3D or compute engine, all processes
    double game = 0;     // percent: this process on that engine
    std::wstring engine; // its type, e.g. "3D"
};
// One sample (instance name, utilization percent) for the adapter (`luidHigh`, `luidLow`) and process
// `pid`: each engine's processes summed (at most 100), the busiest 3D/compute/graphics engine taken.
GpuLoad gpu_load_from(const std::vector<std::pair<std::wstring, double>>& samples, uint32_t luidHigh, uint32_t luidLow,
                      uint32_t pid);

// The worker, for the adapter of the session's device; stop joins it. latest: the last sample (not valid
// before the second, or without the counters).
bool gpu_load_start(uint32_t luidHigh, uint32_t luidLow);
void gpu_load_stop();
GpuLoad gpu_load_latest();
// The average of every sample since the last call for `window` (0 the overlay's, 1 the log's, 2 the QVFR
// optimizer's; `samples` how
// many), and resets that window.
GpuLoad gpu_load_window(int& samples, int window = 1);

}  // namespace simqvfr

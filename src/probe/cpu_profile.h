// CPU sampling profiler ([experiment] cpu_profile=1, for diagnosis): where ACE's busiest threads spend their
// CPU time, to find what to make cheaper. For `seconds`, every kCpuProfileSampleMs it pauses each profiled
// thread just long enough to copy its registers and the top of its stack, resumes it, and walks the copy
// afterwards (x64 unwind data), so a paused thread never waits on anything the profiler holds. Each sample
// is weighted by the CPU cycles the thread used since the previous one (QueryThreadCycleTime): a blocked
// thread adds nothing, so the report is CPU time, not wall time.
// The report goes to logs\cpu-profile-<time>.txt (and a summary to the log): per thread its load and the top
// functions by self time (where it was running) and total time (in the function or anything it called),
// named through DbgHelp where symbols exist, by known RVA for ACE's mapped functions, else module+offset;
// and the time per module. Pausing costs the profiled threads a little (tens of microseconds per sample).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace simqvfr {

inline constexpr int kCpuProfileSampleMs = 2;

struct CpuProfileRequest {
    std::vector<uint32_t> threads;  // thread ids to profile (this process's)
    int seconds = 20;
    std::wstring reportPath;  // empty: logs\cpu-profile-<time>.txt
    // Slow frames (with cpu_profile_mark_frame): a frame longer than this, in ms, is slow; 0: 1.25x the median frame
    // (a frame that misses one display period is caught, even at a steady half rate).
    double slowFrameMs = 0;
};
// Starts a capture on a worker thread; false if one is running or no thread could be opened.
bool cpu_profile_start(const CpuProfileRequest& request);
// The end of a game frame (the layer calls it as each xrEndFrame starts), from any one thread. While a capture runs,
// every sample is put in the frame it fell in. The report then also compares the frames longer than
// `slowFrameMs` with the others, per thread: what the threads do more of in the slow frames, in ms per frame.
// It does nothing when no capture is running.
void cpu_profile_mark_frame();
bool cpu_profile_running();
// Waits for a running capture and its report (tests; the layer never waits).
void cpu_profile_wait();
// The last finished report's path (empty before the first).
std::wstring cpu_profile_last_report();

}  // namespace simqvfr

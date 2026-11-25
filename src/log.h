#pragma once

#include <cstdint>
#include <string>

namespace simqvfr {

// Appends "HH:MM:SS.mmm [tid] message" to SIM_QVFR_LOG, or <package>/logs/sim-qvfr.log.
//
// Two levels. Info (log_line, logf) is every session's: what was set up, decided, refused or failed, and the periodic
// summaries; enough to triage a report. Debug (logd) is the detail behind it (one line per shader patched, per traced
// draw) and is written only when SIM_QVFR_LOG_LEVEL=debug, which the launcher sets for its Diagnostics pane's
// "Detailed logs" launches. One process writes at most kLogLimitBytes (kLogDebugLimitBytes with debug), then a line
// saying so and nothing more. A log that is over kLogRotateBytes when it is opened (the default path, reused by
// launches without the launcher) is moved to <name>.old.log first, so it never grows past twice that.
inline constexpr uint64_t kLogLimitBytes = 32ull << 20;
inline constexpr uint64_t kLogDebugLimitBytes = 256ull << 20;
inline constexpr uint64_t kLogRotateBytes = 8ull << 20;

void log_line(const char* message);
void logf(const char* format, ...);
// Debug level: nothing (not even the formatting) unless debug logs are on.
void logd(const char* format, ...);
// Whether debug lines are written (for a caller that builds a costly message).
bool log_debug();
// Folder the log file is in (debug captures go next to it).
std::wstring log_directory();
// <log folder>/captures, created if missing (the frame captures' folder).
std::wstring captures_directory();
// The core's own folder (the module this is linked into: the layer's DLL, a check's or test's exe): where the NVIDIA and
// AMD runtime DLLs it ships sit. From a game mod, the host's (mod_api/core_shim.cpp).
std::wstring core_directory();
// The log file itself.
std::wstring log_file();
// On (the layer, while a game's session runs): lines are queued and a thread of ours writes them, so the thread that logs
// doesn't wait on the disk. A crash can lose the lines queued in its last moment (the writer takes them at once). Off
// (the session's end, the process's exit): what is queued is written, then each line at once again.
void log_async(bool on);
// DllMain at DLL_PROCESS_DETACH when the process ends: the writer's end waits for nothing then (the other threads are
// gone, perhaps holding a mutex).
void log_process_ending();

}  // namespace simqvfr

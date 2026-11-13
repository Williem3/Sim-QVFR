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
// The log file itself.
std::wstring log_file();

}  // namespace simqvfr

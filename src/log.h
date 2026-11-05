#pragma once

#include <string>

namespace simqvfr {

// Appends "HH:MM:SS.mmm [tid] message" to SIM_QVFR_LOG, or <package>/logs/sim-qvfr.log.
void log_line(const char* message);
void logf(const char* format, ...);
// Folder the log file is in (debug captures go next to it).
std::wstring log_directory();

}  // namespace simqvfr

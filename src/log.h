#pragma once

#include <string>

namespace aceqv {

// Appends "HH:MM:SS.mmm [tid] message" to ACE_QV_LOG, or <package>/logs/ace-quadview.log.
void log_line(const char* message);
void logf(const char* format, ...);
// Folder the log file is in (debug captures go next to it).
std::wstring log_directory();

}  // namespace aceqv

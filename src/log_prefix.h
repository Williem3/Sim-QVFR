// The time stamps Sim-QVFR writes: each log line's "HH:MM:SS.mmm [tid] " (log.cpp, and the hang dump helper that
// appends to the same log) and the files' "<yyyymmdd>-<hhmmss>". Header-only and windows.h alone, for hang_dump's exe.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#include <windows.h>

namespace simqvfr {

// "HH:MM:SS.mmm [tid] " into `out`; snprintf's count (what it needs, as for a cut line).
inline int log_prefix(char* out, size_t n, const SYSTEMTIME& t, DWORD tid) {
    return std::snprintf(out, n, "%02u:%02u:%02u.%03u [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tid);
}

// "<yyyymmdd>-<hhmmss>", the session logs' and the captures' file names.
inline std::wstring file_stamp(const SYSTEMTIME& t) {
    wchar_t text[32];
    swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return text;
}

}  // namespace simqvfr

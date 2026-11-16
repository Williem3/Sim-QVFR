// What every NGX user of ours shares (ngx_dlss.cpp, ngx_nr.cpp, probe/focus_ngx.cpp, games/ace/ngx_extent.h). No NGX
// headers: builds without the SDK include it too.
#pragma once

#include <cctype>
#include <string>

namespace simqvfr {

// A project id of our own (a GUID, custom engine): Sim-QVFR has no NVIDIA application id.
inline constexpr const char* kNgxProjectId = "5b1c0f1e-6a57-4c1b-9d0e-51a0c7f3d2a4";

// NVSDK_NGX_SUCCEED: any result outside the 0xbad00000 block.
inline bool ngx_succeeded(unsigned result) { return (result & 0xfff00000u) != 0xbad00000u; }

inline std::string ngx_lower(const std::string& line) {
    std::string lower = line;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// One of NGX's own lines (lower case) worth our log: its errors and warnings (it writes hundreds of lines a feature).
inline bool ngx_line_logged(const std::string& lower) {
    if (lower.find("error") == std::string::npos && lower.find("warn") == std::string::npos && lower.find("fail") == std::string::npos)
        return false;
    // Not failures: its first two guesses for its core before the driver's own, and model downloads it has no copy of.
    return lower.find("failed to load ngxcore") == std::string::npos && lower.find("from cache") == std::string::npos;
}

inline void ngx_trim_line_end(std::string& line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
}

}  // namespace simqvfr

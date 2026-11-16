// The ini adapters the settings_*.cpp files share (read and write sim-qvfr.ini keys); inline, no state.
#pragma once

#include <algorithm>
#include <string>

#include <windows.h>

#include "../ini.h"
#include "settings.h"

namespace simqvfr::launcher::settings_ini {

inline int read_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

inline std::wstring read_string(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    return ini_get(ini.c_str(), section, key);
}

inline int read_signed(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return ini_signed(ini.c_str(), section, key, fallback);
}

inline int clamp_to(int v, Range r) { return std::clamp(v, r.min, r.max); }

// Only a value that differs is written: a change saves ~100 keys, each write rewrites the whole ini (ini_put.h).
inline bool write(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    return ini_put(ini.c_str(), section, key, value.c_str());
}

inline bool write(const std::wstring& ini, const IniInt& k, const std::wstring& value) { return write(ini, k.section, k.key, value); }

}  // namespace simqvfr::launcher::settings_ini

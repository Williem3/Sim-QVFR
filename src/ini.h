// Reading and writing ini keys, shared by the launcher, the layer and the headset menu.
//
// Writing one key without rewriting the file for nothing. Windows' WritePrivateProfileString rewrites the
// whole file on every call, even when the key already holds that value: about 5 ms a key on the documented
// sim-qvfr.ini (34 KB). A headset menu step wrote ~150 keys that way (0.75 s), so quick steps queued for seconds.
// ini_put reads the key first (about 0.25 ms) and writes only a value that differs.
#pragma once

#include <cwchar>
#include <cstdlib>
#include <iterator>
#include <string>

#include <windows.h>

namespace simqvfr {

// A fallback no value of ours holds: ini_get returns it for a key that isn't there.
inline constexpr wchar_t kIniAbsent[] = L"\x01";

// [section] key in `ini`, `fallback` if it isn't there, at any length: GetPrivateProfileString cuts a value that
// doesn't fit (returning the buffer's size - 1), so a cut read is tried again with more room.
inline std::wstring ini_get(const wchar_t* ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    std::wstring value(256, L'\0');
    for (;;) {
        const DWORD n = GetPrivateProfileStringW(section, key, fallback, value.data(), static_cast<DWORD>(value.size()), ini);
        if (n + 1 < value.size() || value.size() >= 65536) {  // fits (or a line past any ini's)
            value.resize(n);
            return value;
        }
        value.resize(value.size() * 4);
    }
}

// Whether [section] key is in `ini` (an empty value is).
inline bool ini_has(const wchar_t* ini, const wchar_t* section, const wchar_t* key) {
    return ini_get(ini, section, key, kIniAbsent) != kIniAbsent;
}

// A signed number, `fallback` when missing or empty. As a string: GetPrivateProfileInt reads negative values as 0.
inline int ini_signed(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int fallback) {
    const std::wstring text = ini_get(ini, section, key);
    return text.empty() ? fallback : _wtoi(text.c_str());
}

// [section] key = value in `ini`, written only when the file doesn't hold exactly that already (a missing key is
// written). True when the file holds it afterwards.
inline bool ini_put(const wchar_t* ini, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    wchar_t current[128];
    // kIniAbsent: no such key. A value that fills the buffer may be longer: written.
    const DWORD n = GetPrivateProfileStringW(section, key, kIniAbsent, current, static_cast<DWORD>(std::size(current)), ini);
    if (n + 1 < std::size(current) && wcscmp(current, value) == 0)
        return true;
    return WritePrivateProfileStringW(section, key, value, ini) != 0;
}

inline bool ini_put(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int value) {
    return ini_put(ini, section, key, std::to_wstring(value).c_str());
}

}  // namespace simqvfr

// Writing one ini key without rewriting the file for nothing. Windows' WritePrivateProfileString rewrites the
// whole file on every call, even when the key already holds that value: about 5 ms a key on the documented
// sim-qvfr.ini (34 KB). A headset menu step wrote ~150 keys that way (0.75 s), so quick steps queued for seconds.
// ini_put reads the key first (about 0.25 ms) and writes only a value that differs.
#pragma once

#include <cwchar>
#include <iterator>
#include <string>

#include <windows.h>

namespace simqvfr {

// [section] key = value in `ini`, written only when the file doesn't hold exactly that already (a missing key is
// written). True when the file holds it afterwards.
inline bool ini_put(const wchar_t* ini, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    wchar_t current[128];
    // \x01: no such key (no value of ours has it). A value that fills the buffer may be longer: written.
    const DWORD n = GetPrivateProfileStringW(section, key, L"\x01", current, static_cast<DWORD>(std::size(current)), ini);
    if (n + 1 < std::size(current) && wcscmp(current, value) == 0)
        return true;
    return WritePrivateProfileStringW(section, key, value, ini) != 0;
}

inline bool ini_put(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int value) {
    return ini_put(ini, section, key, std::to_wstring(value).c_str());
}

}  // namespace simqvfr

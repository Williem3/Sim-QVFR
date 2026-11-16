// UTF-8 <-> UTF-16 and the small string tests the launcher, the layer and the hang dump helper share. Header-only
// and windows.h alone, so hang_dump's exe can use it.
#pragma once

#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>

#include <windows.h>

namespace simqvfr {

// UTF-8 to UTF-16; invalid bytes become U+FFFD.
inline std::wstring widen(std::string_view utf8) {
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// UTF-8 to UTF-16, empty if `utf8` isn't valid UTF-8 (a pasted profile: refused rather than read with U+FFFD).
inline std::wstring widen_strict(std::string_view utf8) {
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// UTF-16 to UTF-8.
inline std::string narrow(std::wstring_view wide) {
    if (wide.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Case-insensitive as _wcsicmp (file names, layer names, environment variable names).
inline bool iequals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

inline bool starts_with(std::wstring_view s, std::wstring_view prefix) { return s.starts_with(prefix); }
inline bool ends_with(std::wstring_view s, std::wstring_view suffix) { return s.ends_with(suffix); }
// `suffix` in any case (".JSON" as ".json").
inline bool iends_with(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = std::wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// Without leading and trailing white space (iswspace).
inline std::wstring trim(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i]))
        ++i;
    return s.substr(i);
}

}  // namespace simqvfr

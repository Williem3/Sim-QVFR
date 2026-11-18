// The launcher's and the headset menu's text in the player's language: loose files in the release's lang\ folder
// (lang\de.ini, lang\pl.ini…), English compiled in. Header-only, windows.h alone, so the launcher and the layer share it.
//
// The English text is the key: `tr(L"Check now")` shows the current language's line for "Check now", or the English
// when there is none (no file, a line not translated yet, or one refused below). A file lists each English line and
// its translation under it (lang\_template.ini, written by tools/lang_extract.py from every tr / trf / I18N in src\):
//
//     [language]
//     code = de
//     name = Deutsch
//     [strings]
//     en = Launch {game}
//     de = {game} starten
//
// A language file isn't signed, so it is read as untrusted text: size limits, invalid UTF-8 skipped, and a
// translation whose {placeholders} differ from the English's is refused (English shown), so a file can't make the
// launcher print what a placeholder holds where the English doesn't, or drop it. Placeholders are filled by name
// (trf), in one pass: a value holding "{x}" is never expanded again. Loaded tables are kept for the process's
// lifetime, so a pointer tr returned stays valid after the language changes.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "i18n.h"  // I18N(L"...") for tables

namespace simqvfr::lang {

inline constexpr size_t kMaxFileBytes = 4u << 20;   // a full translation is ~150 KB
inline constexpr size_t kMaxValueChars = 8192;       // the longest help text is ~1,000
inline constexpr size_t kMaxPlaceholderChars = 32;

// Lookups by a wstring_view of the English, so tr() allocates nothing.
struct TextHash {
    using is_transparent = void;
    size_t operator()(std::wstring_view s) const noexcept { return std::hash<std::wstring_view>{}(s); }
};

struct Table {
    std::wstring code;  // the file's name: de, pl, pt-BR
    std::wstring name;  // its own name for itself: Deutsch, Polski
    std::unordered_map<std::wstring, std::wstring, TextHash, std::equal_to<>> text;  // English -> translation
    size_t refused = 0;  // translations refused (placeholders differ, too long, invalid UTF-8)
};

// One language file found (available): its code and own name.
struct Info {
    std::wstring code, name;
};

namespace detail {

inline bool utf8_to_wide(std::string_view s, std::wstring& out) {
    out.clear();
    if (s.empty())
        return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return false;
    out.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return true;
}

inline std::wstring_view trim(std::wstring_view s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r'))
        s.remove_suffix(1);
    return s;
}

// A value as the file writes it: bare (trimmed) or in double quotes (spaces kept); \n \t \\ \" escapes in both.
// False for an unterminated quote or an unknown escape.
inline bool unescape(std::wstring_view raw, std::wstring& out) {
    out.clear();
    raw = trim(raw);
    bool quoted = false;
    if (!raw.empty() && raw.front() == L'"') {
        if (raw.size() < 2 || raw.back() != L'"')
            return false;
        raw = raw.substr(1, raw.size() - 2);
        quoted = true;
    }
    for (size_t i = 0; i < raw.size(); ++i) {
        const wchar_t c = raw[i];
        if (c != L'\\') {
            if (quoted && c == L'"')
                return false;  // an inner quote must be escaped
            out += c;
            continue;
        }
        if (++i == raw.size())
            return false;
        switch (raw[i]) {
        case L'n': out += L'\n'; break;
        case L't': out += L'\t'; break;
        case L'\\': out += L'\\'; break;
        case L'"': out += L'"'; break;
        default: return false;
        }
    }
    return true;
}

}  // namespace detail

// The {names} in `s`, sorted, one entry per use ("{{" is a literal brace, as in trf). False when a brace isn't
// closed or a name isn't [A-Za-z0-9_], 1-32 long.
inline bool placeholders(std::wstring_view s, std::vector<std::wstring>& names) {
    names.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'}') {
            if (i + 1 < s.size() && s[i + 1] == L'}') {
                ++i;
                continue;
            }
            return false;
        }
        if (s[i] != L'{')
            continue;
        if (i + 1 < s.size() && s[i + 1] == L'{') {
            ++i;
            continue;
        }
        const size_t end = s.find(L'}', i + 1);
        if (end == std::wstring_view::npos || end == i + 1 || end - i - 1 > kMaxPlaceholderChars)
            return false;
        const std::wstring_view name = s.substr(i + 1, end - i - 1);
        for (const wchar_t c : name)
            if (!(c == L'_' || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')))
                return false;
        names.emplace_back(name);
        i = end;
    }
    std::sort(names.begin(), names.end());
    return true;
}

// Whether `translation` may stand for `english`: the same placeholders, each as often, and within the length limit.
inline bool fits(std::wstring_view english, std::wstring_view translation) {
    if (translation.size() > kMaxValueChars)
        return false;
    std::vector<std::wstring> a, b;
    return placeholders(english, a) && placeholders(translation, b) && a == b;
}

// A language file's text (UTF-8, a BOM allowed) into `out`. Lines: [section], key = value, ; or # comments. In
// [language]: code, name. In [strings]: `en = <English>`, then the next other key's value is its translation; an
// empty translation is left out (English shows). False only when the file is no language file at all (too large, or
// no [strings]); a bad line is skipped and counted in `refused`.
inline bool parse(std::string_view utf8, Table& out) {
    out = {};
    if (utf8.size() > kMaxFileBytes)
        return false;
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF && static_cast<unsigned char>(utf8[1]) == 0xBB &&
        static_cast<unsigned char>(utf8[2]) == 0xBF)
        utf8.remove_prefix(3);
    enum class Section { None, Language, Strings, Other } section = Section::None;
    bool sawStrings = false, haveEnglish = false;
    std::wstring english, line, value;
    size_t start = 0;
    while (start <= utf8.size()) {
        size_t end = utf8.find('\n', start);
        if (end == std::string_view::npos)
            end = utf8.size();
        const std::string_view raw = utf8.substr(start, end - start);
        start = end + 1;
        if (!detail::utf8_to_wide(raw, line)) {
            if (section == Section::Strings) {
                ++out.refused;
                haveEnglish = false;  // its pair is unreadable: don't pair the English with a later line
            }
            continue;
        }
        const std::wstring_view l = detail::trim(line);
        if (l.empty() || l.front() == L';' || l.front() == L'#')
            continue;
        if (l.front() == L'[') {
            const std::wstring_view name = l.size() >= 2 && l.back() == L']' ? detail::trim(l.substr(1, l.size() - 2))
                                                                            : std::wstring_view{};
            const auto is = [&name](std::wstring_view s) {
                return name.size() == s.size() && _wcsnicmp(name.data(), s.data(), s.size()) == 0;
            };
            section = is(L"language") ? Section::Language : is(L"strings") ? Section::Strings : Section::Other;
            sawStrings |= section == Section::Strings;
            haveEnglish = false;
            continue;
        }
        const size_t eq = l.find(L'=');
        if (eq == std::wstring_view::npos)
            continue;
        const std::wstring key(detail::trim(l.substr(0, eq)));
        const bool ok = detail::unescape(l.substr(eq + 1), value) && value.size() <= kMaxValueChars;
        if (section == Section::Language) {
            if (ok && value.size() <= 64 && _wcsicmp(key.c_str(), L"code") == 0)
                out.code = value;
            else if (ok && value.size() <= 64 && _wcsicmp(key.c_str(), L"name") == 0)
                out.name = value;
            continue;
        }
        if (section != Section::Strings)
            continue;
        if (_wcsicmp(key.c_str(), L"en") == 0) {
            haveEnglish = ok && !value.empty();
            if (haveEnglish)
                english = value;
            else
                ++out.refused;
            continue;
        }
        if (!haveEnglish)
            continue;  // a translation without its English line
        haveEnglish = false;
        if (!ok || (!value.empty() && !fits(english, value))) {
            ++out.refused;
            continue;
        }
        if (!value.empty())
            out.text[english] = value;
    }
    return sawStrings;
}

// A code as a file name may carry it: de, pl, pt-BR (letters, 2-3, then -region of 2-4).
inline bool valid_code(std::wstring_view code) {
    size_t i = 0;
    while (i < code.size() && std::iswalpha(code[i]) && code[i] < 0x80)
        ++i;
    if (i < 2 || i > 3)
        return false;
    if (i == code.size())
        return true;
    if (code[i] != L'-')
        return false;
    const size_t region = code.size() - i - 1;
    if (region < 2 || region > 4)
        return false;
    for (++i; i < code.size(); ++i)
        if (!(code[i] < 0x80 && std::iswalnum(code[i])))
            return false;
    return true;
}

// lang\<code>.ini read and parsed; false when it isn't there or isn't a language file. The code is the file's name.
inline bool load_file(const std::filesystem::path& file, Table& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > kMaxFileBytes)
        return false;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!parse(bytes, out))
        return false;
    out.code = file.stem().wstring();
    if (out.name.empty())
        out.name = out.code;
    return true;
}

// The language files in `folder` (lang\), by code; names starting with _ (the template) and invalid codes left out.
inline std::vector<Info> available(const std::filesystem::path& folder) {
    std::vector<Info> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        if (_wcsicmp(p.extension().c_str(), L".ini") != 0)
            continue;
        const std::wstring code = p.stem().wstring();
        if (code.empty() || code.front() == L'_' || !valid_code(code) || _wcsicmp(code.c_str(), L"en") == 0)
            continue;
        Table t;
        if (load_file(p, t))
            found.push_back({t.code, t.name});
    }
    std::sort(found.begin(), found.end(), [](const Info& a, const Info& b) { return _wcsicmp(a.code.c_str(), b.code.c_str()) < 0; });
    return found;
}

// Windows' display language for this user, as a code (de-DE): the language "auto" picks.
inline std::wstring windows_language() {
    ULONG count = 0, size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0)
        return L"en";
    std::wstring list(size, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list.data(), &size))
        return L"en";
    return list.c_str();  // the first of the double-null-terminated list
}

// The file `choice` ([launcher] language: auto, en or a code) stands for among `files`: the exact code, else its
// language without the region (pt-BR -> pt), else a file for that language in another region; empty for English.
inline std::wstring resolve(std::wstring choice, const std::vector<Info>& files) {
    if (choice.empty() || _wcsicmp(choice.c_str(), L"auto") == 0)
        choice = windows_language();
    const auto base = [](const std::wstring& c) { return c.substr(0, c.find(L'-')); };
    for (const Info& f : files)
        if (_wcsicmp(f.code.c_str(), choice.c_str()) == 0)
            return f.code;
    for (const Info& f : files)
        if (_wcsicmp(f.code.c_str(), base(choice).c_str()) == 0)
            return f.code;
    for (const Info& f : files)
        if (_wcsicmp(base(f.code).c_str(), base(choice).c_str()) == 0)
            return f.code;
    return {};
}

namespace detail {

struct State {
    std::mutex mutex;
    std::vector<std::unique_ptr<Table>> loaded;  // never freed: tr's pointers outlive a change of language
    std::atomic<const Table*> current{nullptr};
    std::atomic<uint32_t> generation{0};  // counts changes of language (generation())
};

inline State& state() {
    static State s;
    return s;
}

}  // namespace detail

// Makes `table` the current language (nullptr: English). Kept until the process ends.
inline const Table* use(std::unique_ptr<Table> table) {
    detail::State& s = detail::state();
    if (!table) {
        s.current.store(nullptr, std::memory_order_release);
        s.generation.fetch_add(1, std::memory_order_release);
        return nullptr;
    }
    std::lock_guard lock(s.mutex);
    s.loaded.push_back(std::move(table));
    const Table* t = s.loaded.back().get();
    s.current.store(t, std::memory_order_release);
    s.generation.fetch_add(1, std::memory_order_release);
    return t;
}

// `choice` ([launcher] language) from `folder`: its table made current, or English. The code in use ("" English).
inline std::wstring select(const std::filesystem::path& folder, const std::wstring& choice) {
    const std::wstring code = resolve(choice, available(folder));
    if (code.empty()) {
        use(nullptr);
        return {};
    }
    auto t = std::make_unique<Table>();
    if (!load_file(folder / (code + L".ini"), *t)) {
        use(nullptr);
        return {};
    }
    use(std::move(t));
    return code;
}

// How many times the language has changed: a drawing kept as an image (the headset menu's panel) redraws when it moves.
inline uint32_t generation() { return detail::state().generation.load(std::memory_order_acquire); }

// The current table (nullptr: English).
inline const Table* current() { return detail::state().current.load(std::memory_order_acquire); }

// `english` in the current language, or itself.
inline const wchar_t* tr(const wchar_t* english) {
    const Table* t = current();
    if (!t || !english)
        return english;
    const auto it = t->text.find(std::wstring_view(english));
    return it == t->text.end() ? english : it->second.c_str();
}

// tr for a std::wstring holding English (a table's entry copied): the translation, or the text itself.
inline std::wstring tr(const std::wstring& english) { return tr(english.c_str()); }

// {name}s in `text` filled from `args` in one pass ({{ and }} are literal braces); an unknown name stays as written.
inline std::wstring fill(std::wstring_view text, std::initializer_list<std::pair<const wchar_t*, std::wstring>> args) {
    std::wstring out;
    out.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if ((c == L'{' || c == L'}') && i + 1 < text.size() && text[i + 1] == c) {
            out += c;
            ++i;
            continue;
        }
        if (c != L'{') {
            out += c;
            continue;
        }
        const size_t end = text.find(L'}', i + 1);
        if (end == std::wstring_view::npos) {
            out.append(text.substr(i));
            break;
        }
        const std::wstring_view name = text.substr(i + 1, end - i - 1);
        const auto arg = std::find_if(args.begin(), args.end(), [&](const auto& a) { return name == a.first; });
        if (arg == args.end())
            out.append(text.substr(i, end - i + 1));
        else
            out += arg->second;
        i = end;
    }
    return out;
}

// `english` (with {placeholders}) in the current language, the placeholders filled.
inline std::wstring trf(const wchar_t* english, std::initializer_list<std::pair<const wchar_t*, std::wstring>> args) {
    return fill(tr(english), args);
}

// The folder lang\ files are in for a module in `moduleFolder`: beside it (the launcher, a development build's layer)
// or one up (a release's layer\).
inline std::filesystem::path folder_near(const std::filesystem::path& moduleFolder) {
    std::error_code ec;
    if (std::filesystem::is_directory(moduleFolder / L"lang", ec))
        return moduleFolder / L"lang";
    return moduleFolder.parent_path() / L"lang";
}

}  // namespace simqvfr::lang

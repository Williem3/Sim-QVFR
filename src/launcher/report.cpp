#include "report.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <windows.h>
#include <dxgi.h>
#include <shlobj.h>

#include "json.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace simqvfr::launcher {

namespace fs = std::filesystem;

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// `format` with one number, at whatever length it comes to.
std::wstring formatted(const wchar_t* format, double v) {
    const int n = _scwprintf(format, v);
    if (n <= 0)
        return {};
    std::wstring text(static_cast<size_t>(n), L'\0');
    swprintf_s(text.data(), static_cast<size_t>(n) + 1, format, v);
    return text;
}

std::wstring number(int v) { return std::to_wstring(v); }

bool starts_with(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }
bool ends_with(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct Found {
    fs::path path;
    uint64_t time = 0, size = 0;  // last write (FILETIME units), bytes
};

// The files in `folder` named `prefix`*`suffix`, newest first. MSVC's file clock counts FILETIME's
// 100 ns ticks from 1601.
std::vector<Found> find(const std::wstring& folder, const wchar_t* prefix, const wchar_t* suffix,
                        bool (*accept)(const std::wstring& name) = nullptr) {
    std::vector<Found> out;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const std::wstring name = it->path().filename().wstring();
        if (!starts_with(name, prefix) || !ends_with(name, suffix) || (accept && !accept(name)))
            continue;
        Found f{it->path(), static_cast<uint64_t>(it->last_write_time(ec).time_since_epoch().count()), it->file_size(ec)};
        if (!ec)
            out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.time > b.time; });
    return out;
}

// The session logs, session-<date>-<time>.log.
bool session_log(const std::wstring& name) { return name.size() > 8 && iswdigit(name[8]); }  // "session-" then the date

std::wstring windows_error(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out = text ? text : L"error " + std::to_wstring(code);
    LocalFree(text);
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    return out;
}

// A whole file, shared with whoever has it open (a running game still writes its log).
bool read_file(const std::wstring& path, std::string& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart < (1ll << 31);
    out.assign(ok ? static_cast<size_t>(size.QuadPart) : 0, '\0');
    for (size_t at = 0; ok && at < out.size();) {
        DWORD got = 0;
        ok = ReadFile(file, out.data() + at, static_cast<DWORD>(std::min<size_t>(out.size() - at, 1u << 20)), &got,
                      nullptr) != 0;
        if (ok && got == 0) {  // shorter than it was a moment ago
            out.resize(at);
            break;
        }
        at += got;
    }
    CloseHandle(file);
    return ok;
}

void put16(std::string& out, uint32_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}
void put32(std::string& out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

std::wstring registry_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[512] = {};
    DWORD bytes = sizeof(buffer);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS)
        return {};
    std::wstring s = buffer;
    s.erase(0, s.find_first_not_of(L' '));
    s.erase(s.find_last_not_of(L' ') + 1);
    return s;
}

DWORD registry_dword(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD v = 0, bytes = sizeof(v);
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &bytes) == ERROR_SUCCESS ? v : 0;
}

// The session summary's "Log:" and "CPU capture:" lines name files by their full path, which holds the
// user's name: the issue gets the file name only (the zip has the file).
std::wstring without_folders(const std::wstring& summary) {
    std::wstring out;
    for (size_t at = 0; at < summary.size();) {
        const size_t nl = summary.find(L'\n', at), end = nl == std::wstring::npos ? summary.size() : nl;
        std::wstring line = summary.substr(at, end - at);
        for (const wchar_t* key : {L"Log: ", L"CPU capture: "})
            if (starts_with(line, key)) {
                const size_t slash = line.find_last_of(L"\\/");
                if (slash != std::wstring::npos)
                    line = key + line.substr(slash + 1);
            }
        out += line + L"\n";
        at = end + 1;
    }
    return out;
}

constexpr uint64_t kTicksPerSecond = 10'000'000;  // FILETIME's 100 ns

// A session's start from its log's name, session-<yyyymmdd>-<hhmmss>.log (local time), in FILETIME ticks; 0
// when the name has no time.
uint64_t session_start(const std::wstring& name) {
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (swscanf_s(name.c_str(), L"session-%4u%2u%2u-%2u%2u%2u", &y, &mo, &d, &h, &mi, &s) != 6)
        return 0;
    const SYSTEMTIME local{static_cast<WORD>(y), static_cast<WORD>(mo), 0, static_cast<WORD>(d),
                           static_cast<WORD>(h), static_cast<WORD>(mi), static_cast<WORD>(s), 0};
    SYSTEMTIME utc{};
    FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft))
        return 0;
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// FILETIME ticks as local "<yyyymmdd>-<hhmmss>", the session logs' style.
std::wstring local_stamp(uint64_t ticks) {
    const FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return L"unknown";
    wchar_t text[32];
    swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
               local.wSecond);
    return text;
}

// The game's crash folders (any folder in `folder` holding a minidump or a CrashContext), newest first by their
// newest file: the crash reporter writes them at the crash.
struct GameCrash {
    fs::path folder;
    uint64_t time = 0;
};
std::vector<GameCrash> game_crashes(const std::wstring& folder) {
    std::vector<GameCrash> out;
    if (folder.empty())
        return out;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_directory(fec))
            continue;
        GameCrash c{it->path()};
        bool report = false;
        for (fs::directory_iterator f(it->path(), fec), fend; !fec && f != fend; f.increment(fec)) {
            std::error_code e;
            if (!f->is_regular_file(e))
                continue;
            const fs::path& p = f->path();
            report |= _wcsicmp(p.extension().c_str(), L".dmp") == 0 || p.filename() == kCrashContextName;
            const auto written = f->last_write_time(e);
            if (!e)
                c.time = std::max(c.time, static_cast<uint64_t>(written.time_since_epoch().count()));
        }
        if (report)
            out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const GameCrash& a, const GameCrash& b) { return a.time > b.time; });
    return out;
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty())
        return;
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
        text.replace(at, from.size(), to);
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t'))
        s.pop_back();
    s.erase(0, s.find_first_not_of(" \t"));
    return s;
}

}  // namespace

std::vector<ReportFile> report_files(const ReportSources& sources, uint64_t now) {
    constexpr uint64_t kDay = 24ull * 3600 * 10'000'000;
    const uint64_t since = now > kReportMaxAgeDays * kDay ? now - kReportMaxAgeDays * kDay : 0;
    std::vector<ReportFile> out;
    uint64_t total = 0;
    const auto add = [&](const fs::path& path, const std::wstring& name, uint64_t size) {
        if (total + size > kReportMaxBytes)
            return;
        total += size;
        out.push_back({name, path.wstring(), {}, size});
    };
    const auto add_file = [&](const std::wstring& path, const wchar_t* name) {
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        if (!path.empty() && !ec)
            add(path, name, size);
    };
    // In order of use, so the size cap leaves out what matters least.
    add_file(sources.settingsIni, L"sim-qvfr.ini");
    add_file(sources.runtimeIni, L"runtime.ini");
    // The newest launch: its plan, and what the OpenXR loader said (a game it failed starts flat, no layer log).
    fs::path newestRun;
    {
        std::error_code ec;
        for (fs::directory_iterator it(sources.logs, ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (it->is_directory(ec) && starts_with(name, L"run-") &&
                (newestRun.empty() || name > newestRun.filename().wstring()))
                newestRun = it->path();
        }
    }
    if (!newestRun.empty()) {
        add_file((newestRun / L"launch.txt").wstring(), L"launch.txt");
        const fs::path output = newestRun / kLoaderOutputName;
        std::error_code ec;
        const uint64_t size = fs::file_size(output, ec);
        if (!ec && size <= kReportLoaderOutputBytes) {
            add(output, kLoaderOutputName, size);
        } else if (!ec && total + kReportLoaderOutputBytes <= kReportMaxBytes) {  // its end: the newest lines
            std::ifstream in(output, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(size - kReportLoaderOutputBytes));
            std::string tail(kReportLoaderOutputBytes, '\0');
            in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
            tail.resize(static_cast<size_t>(in.gcount()));
            total += tail.size();
            out.push_back({kLoaderOutputName, {}, std::move(tail), 0});
            out.back().size = out.back().data.size();
        }
    }
    const std::vector<Found> logs = find(sources.logs, L"session-", L".log", session_log);
    std::vector<std::pair<uint64_t, uint64_t>> sessions;  // the zipped sessions: start, last line
    for (size_t i = 0; i < logs.size() && i < kReportLogs; ++i)
        if (i == 0 || logs[i].time >= since) {
            add(logs[i].path, logs[i].path.filename().wstring(), logs[i].size);
            if (const uint64_t start = session_start(logs[i].path.filename().wstring()))
                sessions.emplace_back(start, logs[i].time);
        }
    // The game's crashes during those sessions (one that ended the session leaves the log's last line just before
    // it), redacted: CrashContext and the game's log name the player; the zip goes into a public issue.
    const auto add_data = [&](const std::wstring& name, std::string data) {
        if (total + data.size() > kReportMaxBytes)
            return;
        total += data.size();
        const uint64_t size = data.size();
        out.push_back({name, {}, std::move(data), size});
    };
    int crashes = 0;
    for (const GameCrash& c : game_crashes(sources.gameCrashes)) {
        if (crashes >= kReportGameCrashes)
            break;
        const bool during = std::any_of(sessions.begin(), sessions.end(), [&](const auto& s) {
            return c.time >= s.first && c.time <= s.second + kReportCrashSlackSeconds * kTicksPerSecond;
        });
        if (!during)
            continue;
        ++crashes;
        const std::wstring folder = L"game-crash-" + local_stamp(c.time) + L"/";
        // The context first: its ids are what the dump and the log must lose too. A dump without one can't be
        // redacted, so it stays out.
        std::string context;
        const bool hasContext = read_file((c.folder / kCrashContextName).wstring(), context);
        const std::vector<std::string> ids = hasContext ? crash_context_ids(context) : std::vector<std::string>{};
        if (hasContext)
            add_data(folder + kCrashContextName, redact_crash_context(std::move(context)));
        std::error_code ec;
        for (fs::directory_iterator f(c.folder, ec), end; !ec && f != end; f.increment(ec)) {
            std::error_code e;
            if (!f->is_regular_file(e))
                continue;
            const fs::path& p = f->path();
            const std::wstring name = p.filename().wstring();
            const uint64_t size = f->file_size(e);
            if (e)
                continue;
            std::string text;
            if (_wcsicmp(p.extension().c_str(), L".dmp") == 0) {
                if (hasContext && size <= kReportMaxDumpBytes && total + size <= kReportMaxBytes &&
                    read_file(p.wstring(), text))
                    add_data(folder + name, redact_dump(std::move(text), ids));
            } else if (_wcsicmp(p.extension().c_str(), L".log") == 0 && read_file(p.wstring(), text)) {
                // The whole log: its names come from the first lines.
                text = redact_dump(redact_game_log(std::move(text)), ids);
                if (text.size() > kReportLoaderOutputBytes) {  // its end, from a line's start
                    text.erase(0, text.size() - kReportLoaderOutputBytes);
                    text.erase(0, text.find('\n') + 1);
                }
                add_data(folder + name, std::move(text));
            }
        }
    }
    const std::vector<Found> profiles = find(sources.logs, L"cpu-profile-", L".txt");
    for (size_t i = 0; i < profiles.size() && i < kReportCpuProfiles; ++i)
        if (profiles[i].time >= since)
            add(profiles[i].path, profiles[i].path.filename().wstring(), profiles[i].size);
    for (const Found& f : find(sources.logs, L"cheeky-snapshot", L".json"))
        if (f.path.filename() == L"cheeky-snapshot.json")
            add(f.path, L"cheeky-snapshot.json", f.size);
    int dumps = 0;
    for (const Found& f : find(sources.logs, L"hang-", L".dmp"))
        if (dumps < kReportDumps && f.time >= since && f.size <= kReportMaxDumpBytes) {
            add(f.path, f.path.filename().wstring(), f.size);
            ++dumps;
        }
    return out;
}

std::wstring game_crashes_folder(const wchar_t* crashReports) {
    if (!crashReports || !*crashReports)
        return {};
    wchar_t* local = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local)
        out = (fs::path(local) / crashReports).wstring();
    CoTaskMemFree(local);
    return out;
}

std::string redact_crash_context(std::string xml) {
    for (const char* tag : {"LoginId", "MachineId", "EpicAccountId", "UserName"}) {
        const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
        for (size_t at = 0; (at = xml.find(open, at)) != std::string::npos;) {
            const size_t begin = at + open.size(), end = xml.find(close, begin);
            if (end == std::string::npos)
                break;
            if (end > begin)
                xml.replace(begin, end - begin, "redacted");
            at = begin;
        }
    }
    return xml;
}

std::vector<std::string> crash_context_ids(const std::string& xml) {
    std::vector<std::string> ids;
    for (const char* tag : {"LoginId", "MachineId", "EpicAccountId"}) {
        const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
        for (size_t at = 0; (at = xml.find(open, at)) != std::string::npos;) {
            const size_t begin = at + open.size(), end = xml.find(close, begin);
            if (end == std::string::npos)
                break;
            const std::string id = trimmed(xml.substr(begin, end - begin));
            if (id.size() >= 8 && std::find(ids.begin(), ids.end(), id) == ids.end())  // shorter: no id
                ids.push_back(id);
            at = end;
        }
    }
    return ids;
}

std::string redact_dump(std::string dump, const std::vector<std::string>& ids) {
    const auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::string folded(dump.size(), '\0');  // lower case, for a match in any case at the same offsets
    std::transform(dump.begin(), dump.end(), folded.begin(), lower);
    for (const std::string& id : ids) {
        std::string ascii(id.size(), '\0'), wide;
        std::transform(id.begin(), id.end(), ascii.begin(), lower);
        for (char ch : ascii) {
            wide += ch;
            wide += '\0';
        }
        for (const std::string* form : {&ascii, &wide})
            for (size_t at = 0; (at = folded.find(*form, at)) != std::string::npos; at += form->size())
                for (size_t i = 0; i < form->size(); i += form == &wide ? 2 : 1)
                    dump[at + i] = folded[at + i] = '0';
    }
    return dump;
}

std::string redact_game_log(std::string log) {
    // The lines that name the player, each value up to `stop` (the line's end when empty) replaced on the spot;
    // the longer values then wherever else they appear (a short one could match ordinary text).
    struct Rule {
        const char *key, *stop, *as;
    };
    constexpr Rule kRules[] = {
        {"LogInit: Computer: ", "", "<computer>"},
        {"LogInit: User: ", "", "<user>"},
        {"loaded local account: ", "", "<account>"},          // ACC: "S<SteamID64> <name> <nickname>#0"
        {"steam user stats, user: ", " [", "<steam name>"},  // Steam's "has no stats entries" warning
    };
    constexpr size_t kMinAnywhere = 4;
    std::vector<std::pair<std::string, std::string>> anywhere;  // value, placeholder
    std::vector<std::string> users;
    for (const Rule& r : kRules)
        for (size_t at = 0; (at = log.find(r.key, at)) != std::string::npos;) {
            const size_t begin = at + std::strlen(r.key);
            const size_t eol = std::min(log.find('\n', begin), log.size());
            size_t end = *r.stop ? log.find(r.stop, begin) : eol;
            end = std::min(end, eol);
            const std::string value = trimmed(log.substr(begin, end - begin));
            if (value.empty()) {
                at = begin;
                continue;
            }
            log.replace(begin, log.find(value, begin) + value.size() - begin, r.as);
            at = begin + std::strlen(r.as);
            if (std::strcmp(r.as, "<user>") == 0) {
                users.push_back(value);
                continue;
            }
            std::vector<std::string> values = {value};
            if (std::strcmp(r.as, "<account>") == 0) {  // the name after the id, without and with the nickname
                const size_t space = value.find(' ');
                if (space != std::string::npos) {
                    const std::string name = trimmed(value.substr(space + 1));
                    values.push_back(name);
                    const size_t last = name.rfind(' ');
                    if (last != std::string::npos) {
                        values.push_back(trimmed(name.substr(0, last)));
                        values.push_back(trimmed(name.substr(last + 1)));
                    }
                }
            }
            for (const std::string& v : values)
                if (v.size() >= kMinAnywhere)
                    anywhere.emplace_back(v, r.as);
        }
    std::sort(anywhere.begin(), anywhere.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [value, as] : anywhere)
        replace_all(log, value, as);
    for (const std::string& user : users)  // in paths only: a short name would match ordinary words
        for (const char* sep : {"/", "\\"}) {
            replace_all(log, std::string("Users") + sep + user + sep, std::string("Users") + sep + "<user>" + sep);
            replace_all(log, std::string("Users") + sep + sep + user + sep + sep,
                        std::string("Users") + sep + sep + "<user>" + sep + sep);
        }
    // SteamID64s: 7656119 and ten more digits, not inside a longer number.
    const std::string prefix = "7656119", steamId = "<steam id>";
    for (size_t at = 0; (at = log.find(prefix, at)) != std::string::npos;) {
        size_t end = at + prefix.size();
        while (end < log.size() && end - at < 18 && std::isdigit(static_cast<unsigned char>(log[end])))
            ++end;
        const bool before = at > 0 && std::isdigit(static_cast<unsigned char>(log[at - 1]));
        if (end - at == 17 && !before) {
            log.replace(at, 17, steamId);
            at += steamId.size();
        } else {
            at = end;
        }
    }
    return log;
}

uint32_t crc32(const void* data, size_t size, uint32_t crc) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::wstring write_zip(const std::wstring& zip, const std::vector<ReportFile>& files) {
    // Stored entries (method 0), names in UTF-8 (flag bit 11), all stamped with the time now (DOS time).
    SYSTEMTIME t;
    GetLocalTime(&t);
    const uint32_t dosTime = (t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2);
    const uint32_t dosDate = ((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay;
    std::string out, directory;
    uint32_t entries = 0;
    for (const ReportFile& f : files) {
        std::string data;
        if (f.path.empty())
            data = f.data;
        else if (!read_file(f.path, data))
            continue;  // gone or locked meanwhile: the rest still helps
        if (out.size() + data.size() > 0xffffffffull - (1u << 20))
            return L"too large for a zip";
        const std::string name = utf8(f.name);
        const uint32_t crc = crc32(data.data(), data.size()), offset = static_cast<uint32_t>(out.size());
        const uint32_t size = static_cast<uint32_t>(data.size());
        std::string common;  // version needed .. extra length, shared by both headers
        put16(common, 10);
        put16(common, 1u << 11);
        put16(common, 0);
        put16(common, dosTime);
        put16(common, dosDate);
        put32(common, crc);
        put32(common, size);
        put32(common, size);
        put16(common, static_cast<uint32_t>(name.size()));
        put16(common, 0);
        put32(out, 0x04034b50);
        out += common + name + data;
        put32(directory, 0x02014b50);
        put16(directory, 10);  // made by: MS-DOS, 1.0
        directory += common;
        put16(directory, 0);  // comment length
        put16(directory, 0);  // disk
        put16(directory, 0);  // internal attributes
        put32(directory, 0);  // external attributes
        put32(directory, offset);
        directory += name;
        ++entries;
    }
    const uint32_t directoryAt = static_cast<uint32_t>(out.size());
    out += directory;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, entries);
    put16(out, entries);
    put32(out, static_cast<uint32_t>(directory.size()));
    put32(out, directoryAt);
    put16(out, 0);
    const HANDLE file = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return windows_error(GetLastError());
    DWORD written = 0;
    const bool ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) && written == out.size();
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(zip.c_str());
        return windows_error(error);
    }
    return {};
}

std::wstring windows_name(int build, const std::wstring& displayVersion, int ubr) {
    std::wstring s = build >= 22000 ? L"Windows 11" : L"Windows 10";
    if (!displayVersion.empty())
        s += L" " + displayVersion;
    s += L" (build " + number(build);
    if (ubr > 0)
        s += L"." + number(ubr);
    return s + L")";
}

std::wstring gpu_driver_text(uint32_t vendorId, uint64_t umd) {
    const unsigned a = static_cast<unsigned>(umd >> 48), b = static_cast<unsigned>((umd >> 32) & 0xffff),
                   c = static_cast<unsigned>((umd >> 16) & 0xffff), d = static_cast<unsigned>(umd & 0xffff);
    std::wstring s = number(a) + L"." + number(b) + L"." + number(c) + L"." + number(d);
    if (vendorId == 0x10de) {  // NVIDIA's number: the last five digits of the last two parts, "560.94"
        const unsigned n = (c % 10) * 10000 + d;
        wchar_t nv[16];
        swprintf_s(nv, L"%u.%02u", n / 100, n % 100);
        s = nv + std::wstring(L" (") + s + L")";
    }
    return s;
}

std::wstring openxr_runtime_name(const std::string& manifestJson, const std::wstring& manifestPath) {
    json::Value root;
    if (json::parse(manifestJson, root))
        if (const json::Value* runtime = root.find("runtime")) {
            const std::string name = runtime->string_at("name");
            if (!name.empty())
                return widen(name);
        }
    return fs::path(manifestPath).filename().wstring();
}

SystemInfo system_info() {
    SystemInfo info;
    info.cpu = registry_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                               L"ProcessorNameString");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    info.cpu += (info.cpu.empty() ? L"" : L", ") + number(static_cast<int>(si.dwNumberOfProcessors)) + L" threads";
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb))
        info.ramGb = kb / (1024.0 * 1024.0);
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    info.windows = windows_name(_wtoi(registry_string(HKEY_LOCAL_MACHINE, nt, L"CurrentBuildNumber").c_str()),
                                registry_string(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion"),
                                static_cast<int>(registry_dword(HKEY_LOCAL_MACHINE, nt, L"UBR")));
    // The GPU: the hardware adapter with the most dedicated memory (a laptop's discrete one, not the iGPU).
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        SIZE_T best = 0;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (info.gpu.empty() || desc.DedicatedVideoMemory > best)) {
                best = desc.DedicatedVideoMemory;
                info.gpu = desc.Description + formatted(L", %.0f GB", desc.DedicatedVideoMemory / (1024.0 * 1024 * 1024));
                info.gpuDriver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))
                                     ? gpu_driver_text(desc.VendorId, static_cast<uint64_t>(umd.QuadPart))
                                     : L"";
            }
            adapter->Release();
        }
        factory->Release();
    }
    const std::wstring manifest = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    std::string text;
    if (!manifest.empty())
        info.openxrRuntime = read_file(manifest, text) ? openxr_runtime_name(text, manifest)
                                                       : fs::path(manifest).filename().wstring() + L" (missing)";
    return info;
}

std::string utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<Cause> likely_causes(const Settings& s, const RuntimeInfo& rt) {
    std::vector<Cause> out;
    const SessionInfo& session = rt.session;
    if (s.eyeTracking && rt.eyeTracker == EyeTracker::Missing)
        out.push_back({L"Eye tracking is on, but the tracker sent nothing last session",
                       L"The focus area stayed in the centre. Start the headset's eye tracking (and calibrate it) "
                       L"before the game, or choose Fixed in Foveation."});
    if (s.cheeky.load && (rt.cheekyStatus == L"missing" || rt.cheekyStatus == L"refused"))
        out.push_back({L"Foveated DLSS did not start at the last launch (Cheeky " + rt.cheekyStatus + L")",
                       rt.cheekyStatus == L"missing"
                           ? L"Choose Cheeky's runtime (CheekyFoveatedDLSSRuntime.dll) in Foveated DLSS."
                           : L"Cheeky's runtime refused to load: use the build the README lists, and check the session log."});
    if (rt.known() && readouts(s, rt).overRuntimeLimit)
        out.push_back({L"The view is larger than the headset runtime allows",
                       L"Lower the outer resolution or the focus density in Resolution."});
    if (rt.vramUsedMb > 0 && rt.vramBudgetMb > 0 && rt.vramUsedMb >= 0.9 * rt.vramBudgetMb)
        out.push_back({formatted(L"VRAM was nearly full (%.0f%% of what Windows allows the game)", 100 * rt.vramUsedMb / rt.vramBudgetMb),
                       L"Stutter and hitches follow when it runs out. Lower the outer resolution or the focus density, "
                       L"or the game's texture quality."});
    if (!session.known())
        return out;
    if (session.layersBelow.find(L"MBUCCHIA_toolkit") != std::wstring::npos)
        out.push_back({L"OpenXR Toolkit is loaded under Sim-QVFR",
                       L"It adds about 0.8 ms to every frame's submission. If you don't use it for this game, switch it off in "
                       L"OpenXR layers."});
    if (session.limited == L"GPU")
        out.push_back({formatted(L"The GPU was busy all the time (%.0f%%)", session.gpuBusy),
                       L"This is the GPU's limit. Lower the outer resolution or the focus density, or the game's "
                       L"graphics settings."});
    double gameThread = 0;
    for (const auto& [name, percent] : session.threads)
        if (name == L"GameThread")
            gameThread = percent;
    if (session.limited.starts_with(L"CPU") || gameThread >= 90)
        out.push_back({L"The game's CPU work limited the frame rate",
                       L"QVFR lowers the GPU's work, not the CPU's. Lower the game's CPU-heavy settings (opponents, "
                       L"mirrors, shadows)."});
    if (session.limited.starts_with(L"Pacing"))
        out.push_back({formatted(L"%.0f%% of frames were late with neither the CPU nor the GPU full", session.latePercent),
                       L"Often something else in the chain: another OpenXR layer, an overlay, or the runtime's motion "
                       L"smoothing. Try with them off."});
    return out;
}

std::wstring issue_title(const IssueReport& r) {
    const std::wstring kind = L"[" + std::wstring(r.kind > 0 && r.kind < kIssueKindCount ? kIssueKinds[r.kind] : L"Problem") + L"] ";
    std::wstring line = r.description.substr(0, r.description.find_first_of(L"\r\n"));
    line.erase(0, line.find_first_not_of(L" \t"));
    line.erase(line.find_last_not_of(L" \t") + 1);
    if (line.empty())
        return kind + L"Sim-QVFR " + r.version + L": ";
    if (line.size() > 70) {  // at a word
        const size_t space = line.find_last_of(L' ', 70);
        line = line.substr(0, space == std::wstring::npos || space < 30 ? 70 : space) + L"…";
    }
    return kind + line;
}

std::wstring issue_details(const IssueReport& r) {
    const Settings& s = r.settings;
    const RuntimeInfo& rt = r.runtime;
    const bool qvfr = s.mode == RenderMode::Qvfr;
    const std::wstring unknown = L"not known";
    std::wstring b;
    b += L"- **Version:** Sim-QVFR " + (r.version.empty() ? unknown : r.version) + L" (launcher and layer)\n";
    const SystemInfo& sys = r.system;
    b += L"- **CPU:** " + (sys.cpu.empty() ? unknown : sys.cpu) + L"\n";
    b += L"- **GPU:** " + (sys.gpu.empty() ? unknown : sys.gpu);
    if (!sys.gpuDriver.empty())
        b += L", driver " + sys.gpuDriver;
    b += L"\n";
    b += L"- **RAM:** " + (sys.ramGb > 0 ? formatted(L"%.0f GB", sys.ramGb) : unknown) + L"\n";
    b += L"- **Windows:** " + (sys.windows.empty() ? unknown : sys.windows) + L"\n";
    b += L"- **OpenXR runtime:** " + (sys.openxrRuntime.empty() ? unknown : sys.openxrRuntime) + L"\n";
    std::wstring headset = rt.known() ? number(rt.width) + L"×" + number(rt.height) + L" per eye recommended" : unknown;
    if (rt.maxWidth > 0)
        headset += L" (max " + number(rt.maxWidth) + L"×" + number(rt.maxHeight) + L")";
    if (rt.centerPpd > 0)
        headset += formatted(L", %.1f px/° at the centre", rt.centerPpd);
    b += L"- **Headset:** " + headset + L"\n";
    b += L"- **Eye tracker:** " +
         std::wstring(rt.eyeTracker == EyeTracker::Detected  ? L"detected"
                      : rt.eyeTracker == EyeTracker::Missing ? L"no samples"
                                                             : L"not known") +
         L"\n";
    b += L"- **Method:** " + std::wstring(qvfr ? L"Quad View Foveated Rendering (QVFR)" : L"Two-loop Foveated Interlaced") +
         L", " + (s.eyeTracking ? L"eye tracked" : L"fixed") + L"\n";
    std::wstring res = qvfr ? L"outer " + number(s.outer) + L"%, focus density " + number(s.inner) + L"%"
                            : L"eye scale " + number(s.outer) + L"%";
    res += L", focus area " + number(s.width) + L"×" + number(s.height) + L"%";
    if (rt.known()) {
        const Readouts ro = readouts(s, rt);
        res += L" (outer " + number(ro.outer.width) + L"×" + number(ro.outer.height);
        if (qvfr)
            res += L", focus " + number(ro.focus.width) + L"×" + number(ro.focus.height);
        res += L" px)";
    }
    b += L"- **Resolution:** " + res + L"\n";
    b += L"- **Focus area:** feather " + number(s.feather) + L"%, " +
         (s.shape.corners ? std::wstring(L"corners") : L"corner radius " + number(s.corner) + L"%") +
         formatted(L", gaze dead-zone %.1f°", s.gazeDeadzone / 10.0) + L", offset " +
         number(s.offset.for_mode(s.eyeTracking)) + L"°\n";
    if (qvfr)
        b += L"- **FOV crop:** " + (s.crop ? crop_key(s) + L" % (top, bottom, outer, inner)" : std::wstring(L"off")) + L"\n";
    std::wstring dlss = s.cheeky.load ? L"on" : L"off";
    if (!rt.cheekyStatus.empty())
        dlss += L" (last launch: " + rt.cheekyStatus + (rt.cheekyVersion.empty() ? L"" : L" " + rt.cheekyVersion) + L")";
    if (s.cheeky.load)
        dlss += s.cheeky.fg ? L", frame generation " + number(s.cheeky.fgMultiplier) + L"x" : L", frame generation off";
    b += L"- **Foveated DLSS:** " + dlss + L"\n";
    if (qvfr && !rt.quadViews.empty())  // the runtime composing the views (Pimax, Varjo): what the last launch did
        b += L"- **Runtime composition:** " + std::wstring(s.nativeQuad ? L"auto" : L"off") + L" (last launch: " +
             (rt.quadViewsUsed               ? L"the runtime composed the views"
              : rt.quadViews == L"offered"   ? L"offered by the runtime, not used"
              : rt.quadViews == L"none"      ? L"not offered by the runtime"
                                             : L"not tried, another OpenXR layer between") +
             L")\n";
    b += L"\n**Last session**\n```text\n" + without_folders(session_summary_text(rt)) + L"```\n";
    return b;
}

namespace {

std::wstring checks_text(const std::vector<Cause>& causes) {
    if (causes.empty())
        return L"None found.\n";
    std::wstring t;
    for (const Cause& c : causes)
        t += L"- **" + c.title + L".** " + c.fix + L"\n";
    return t;
}

}  // namespace

std::wstring issue_body(const IssueReport& r) {
    std::wstring b;
    b += L"### What happened?\n\n";
    b += r.description.empty() ? L"<!-- What did you see, and what did you expect instead? -->\n\n\n" : r.description + L"\n\n";
    b += L"### Steps to reproduce\n\n<!-- From starting the launcher: the settings you changed, the track and car, what "
         L"you did. -->\n1. \n2. \n\n";
    b += L"### When does it happen?\n\n<!-- Every launch, after some minutes, on one track or car, since an update... "
         L"-->\n\n\n";
    b += L"### Screenshots and logs\n\nPlease drag in screenshots (in the headset, and the launcher's Diagnostics page)";
    if (!r.zipName.empty())
        b += L" and **`" + r.zipName + L"`**, the logs the launcher just zipped (the launcher's Report pane shows it to drag in, and Explorer shows it too)";
    b += L".\n\n---\n\n";
    b += L"### Launcher checks\n\n" + checks_text(r.causes) + L"\n";
    b += L"### Collected by the launcher\n\n" + issue_details(r);
    return b;
}

std::vector<IssueField> issue_fields(const IssueReport& r) {
    const std::wstring files =
        r.zipName.empty() ? L"" : L"Drag in `" + r.zipName + L"` (drag it here from the launcher's Report pane, or from its logs folder) and any screenshots.\n";
    return {{"what", r.description}, {"files", files}, {"checks", checks_text(r.causes)}, {"details", issue_details(r)}};
}

std::string url_encode(const std::wstring& text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : utf8(text)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string issue_url(const std::wstring& repo, const char* form, const std::wstring& title,
                      const std::vector<IssueField>& fields, size_t max) {
    const std::string head =
        "https://github.com/" + utf8(repo) + "/issues/new?template=" + form + "&title=" + url_encode(title);
    const auto build = [&](const std::vector<IssueField>& f) {
        std::string url = head;
        for (const IssueField& field : f)
            if (!field.value.empty())
                url += "&" + field.id + "=" + url_encode(field.value);
        return url;
    };
    std::vector<IssueField> kept = fields;
    std::string url = build(kept);
    // Too long: drop whole lines from the end of the last field (the zip's issue.md has it all), keeping a
    // ``` block closed, then from the field before it.
    const std::wstring note = L"\n_(Trimmed to fit the link: the zip's issue.md has all of it.)_\n";
    for (size_t i = kept.size(); url.size() > max && i-- > 0;) {
        std::wstring text = fields[i].value;
        while (url.size() > max && !text.empty()) {
            const size_t nl = text.find_last_of(L'\n', text.size() >= 2 ? text.size() - 2 : 0);
            text.resize(nl == std::wstring::npos ? 0 : nl + 1);
            size_t fences = 0;  // an odd count: the cut is inside a ``` block
            for (size_t at = text.find(L"```"); at != std::wstring::npos; at = text.find(L"```", at + 3))
                ++fences;
            kept[i].value = text + (fences % 2 == 1 ? L"```\n" : L"") + note;
            url = build(kept);
        }
        if (url.size() > max) {
            kept[i].value.clear();
            url = build(kept);
        }
    }
    return url.size() <= max ? url : head.substr(0, std::min(head.size(), max));
}

}  // namespace simqvfr::launcher

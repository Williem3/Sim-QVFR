#include "log_retention.h"

#include "leftovers.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>

namespace fs = std::filesystem;

namespace simqvfr::launcher {
namespace {

bool starts_with(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

bool ends_with(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = std::wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

uint64_t size_of(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        const uintmax_t n = fs::file_size(p, ec);
        return ec ? 0 : n;
    }
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e;
        if (it->is_regular_file(e)) {
            const uintmax_t n = it->file_size(e);
            total += e ? 0 : n;
        }
    }
    return total;
}

struct Entry {
    fs::path path;
    std::wstring name;
    fs::file_time_type time;
};

void remove(const Entry& e, PruneResult& r) {
    const uint64_t bytes = size_of(e.path);
    std::error_code ec;
    if (fs::remove_all(e.path, ec) > 0 && !ec) {
        ++r.removed;
        r.bytes += bytes;
    }
}

// All but the newest `keep` of `list`, newest by name (their names hold a sortable stamp) or by file time.
void keep_newest(std::vector<Entry> list, size_t keep, bool byName, PruneResult& r) {
    if (list.size() <= keep)
        return;
    std::sort(list.begin(), list.end(), [byName](const Entry& a, const Entry& b) {
        return byName ? a.name > b.name : a.time > b.time;
    });
    for (size_t i = keep; i < list.size(); ++i)
        remove(list[i], r);
}

}  // namespace

PruneResult prune_logs(const std::wstring& logsFolder) {
    PruneResult r;
    std::vector<Entry> sessions, runs, dumps, machineRegs, userRegs, reports, zips, profiles;
    std::error_code ec;
    for (auto it = fs::directory_iterator(logsFolder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e;
        const std::wstring name = it->path().filename().wstring();
        Entry entry{it->path(), name, it->last_write_time(e)};
        if (it->is_directory(e)) {
            if (starts_with(name, L"run-"))
                runs.push_back(entry);
            continue;
        }
        if (starts_with(name, L"session-") && ends_with(name, L".log"))
            sessions.push_back(entry);
        else if (starts_with(name, L"hang-") && ends_with(name, L".dmp"))
            dumps.push_back(entry);
        else if (starts_with(name, L"openxr-layers-machine-") && ends_with(name, L".reg"))
            machineRegs.push_back(entry);
        else if (starts_with(name, L"openxr-layers-user-") && ends_with(name, L".reg"))
            userRegs.push_back(entry);
        else if (starts_with(name, L"sim-qvfr-report-") && ends_with(name, L".zip"))
            reports.push_back(entry);
        else if (starts_with(name, L"sim-qvfr-logs-") && ends_with(name, L".zip"))
            zips.push_back(entry);
        else if (starts_with(name, L"cpu-profile-") && ends_with(name, L".txt"))
            profiles.push_back(entry);
    }
    keep_newest(sessions, kKeepSessions, true, r);
    keep_newest(runs, kKeepSessions, true, r);
    keep_newest(dumps, kKeepHangDumps, false, r);
    keep_newest(reports, kKeepReports, true, r);
    keep_newest(zips, kKeepReports, true, r);
    keep_newest(profiles, kKeepCpuProfiles, false, r);
    for (std::vector<Entry>* regs : {&machineRegs, &userRegs}) {
        if (regs->size() <= static_cast<size_t>(kKeepRegistryBackups) + 1)
            continue;
        std::sort(regs->begin(), regs->end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
        regs->erase(regs->begin());  // the oldest stays: Windows' layers before the first change
        keep_newest(*regs, kKeepRegistryBackups, true, r);
    }
    return r;
}

std::vector<std::wstring> diagnostics_paths(const std::wstring& logsFolder) {
    std::vector<std::wstring> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(logsFolder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e;
        const std::wstring name = it->path().filename().wstring();
        const bool folder = it->is_directory(e);
        if (folder ? name == L"captures" || starts_with(name, L"captures-") || name == L"pix" || name == L"shaders"
                   : is_leftover(it->path()))  // an older version's copy of a game's program, by leftovers.h's rules
            out.push_back(it->path().wstring());
    }
    std::sort(out.begin(), out.end());
    return out;
}

LogsUse logs_use(const std::wstring& logsFolder) {
    LogsUse u;
    u.bytes = size_of(logsFolder);
    for (const std::wstring& path : diagnostics_paths(logsFolder))
        u.diagnosticsBytes += size_of(path);
    return u;
}

std::wstring size_text(uint64_t bytes) {
    wchar_t s[32];
    if (bytes >= (1ull << 30))
        swprintf_s(s, L"%.1f GB", bytes / double(1ull << 30));
    else if (bytes >= (1ull << 20))
        swprintf_s(s, L"%.1f MB", bytes / double(1ull << 20));
    else
        swprintf_s(s, L"%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    return s;
}

}  // namespace simqvfr::launcher

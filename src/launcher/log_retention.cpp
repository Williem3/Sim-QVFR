#include "log_retention.h"

#include "../text.h"
#include "leftovers.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace simqvfr::launcher {
namespace {

// `name` is exactly `pattern`, each '#' a digit ("session-########-######.log").
bool named(const std::wstring& name, const wchar_t* pattern) {
    const size_t n = std::wcslen(pattern);
    if (name.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (pattern[i] == L'#' ? !(name[i] >= L'0' && name[i] <= L'9') : name[i] != pattern[i])
            return false;
    return true;
}

// A link, junction or other reparse point: never followed, never removed.
bool reparse_point(const fs::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// A plain file (not a link) that starts with `magic` (nullptr: any content).
bool plain_file(const fs::path& p, const char* magic = nullptr) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(p, ec)) || ec || reparse_point(p))
        return false;
    if (!magic)
        return true;
    char head[8] = {};
    std::ifstream in(p, std::ios::binary);
    const size_t n = std::strlen(magic);
    return in.read(head, static_cast<std::streamsize>(n)) && std::memcmp(head, magic, n) == 0;
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
    bool folder = false;
};

// What a launch writes into its run folder (main.cpp, launch_plan.cpp): the only files a pruned one may lose.
bool run_file(const fs::path& p) {
    const std::wstring name = p.filename().wstring();
    return plain_file(p) && (name == L"launch.txt" || name == L"sim-qvfr.ini" || name == L"ace-quadview.ini" ||
                             name == L"openxr-loader.log" || name == L"openxr-explicit-before.reg" ||
                             iends_with(name, L".json"));
}

// A file: removed. A run folder: the launch's own files in it removed, then the folder if that left it empty; anything
// else in it (a subfolder, a file a launch doesn't write) keeps the folder and itself. Nothing recursive.
void remove(const Entry& e, PruneResult& r) {
    std::error_code ec;
    if (!e.folder) {
        const uint64_t bytes = size_of(e.path);
        if (fs::remove(e.path, ec) && !ec) {
            ++r.removed;
            r.bytes += bytes;
        }
        return;
    }
    uint64_t bytes = 0;
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(e.path, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (run_file(it->path()))
            files.push_back(it->path());
    for (const fs::path& f : files) {
        std::error_code fe;
        const uintmax_t n = fs::file_size(f, fe);
        if (!fe && fs::remove(f, fe) && !fe)
            bytes += n;
    }
    r.bytes += bytes;
    if (RemoveDirectoryW(e.path.c_str()))  // only when empty
        ++r.removed;
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
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(logsFolder, ec)) || ec || reparse_point(logsFolder))
        return r;  // missing, or a link or junction: not looked into
    std::vector<Entry> sessions, runs, dumps, machineRegs, userRegs, reports, zips, profiles;
    for (auto it = fs::directory_iterator(logsFolder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e;
        const fs::path& p = it->path();
        const std::wstring name = p.filename().wstring();
        Entry entry{p, name, it->last_write_time(e)};
        // Only the exact names Sim-QVFR writes, and a zip or minidump only with its own signature.
        if (named(name, L"run-########-######")) {
            if (fs::is_directory(fs::symlink_status(p, e)) && !reparse_point(p)) {
                entry.folder = true;
                runs.push_back(entry);
            }
        } else if (named(name, L"session-########-######.log") && plain_file(p)) {
            sessions.push_back(entry);
        } else if (named(name, L"hang-######.dmp") && plain_file(p, "MDMP")) {
            dumps.push_back(entry);
        } else if (named(name, L"openxr-layers-machine-########-######.reg") && plain_file(p)) {
            machineRegs.push_back(entry);
        } else if (named(name, L"openxr-layers-user-########-######.reg") && plain_file(p)) {
            userRegs.push_back(entry);
        } else if (named(name, L"sim-qvfr-report-########-######.zip") && plain_file(p, "PK")) {
            reports.push_back(entry);
        } else if (named(name, L"sim-qvfr-logs-########-######.zip") && plain_file(p, "PK")) {
            zips.push_back(entry);
        } else if (named(name, L"cpu-profile-########-######.txt") && plain_file(p)) {
            profiles.push_back(entry);
        }
    }
    keep_newest(sessions, kKeepSessions, true, r);
    keep_newest(runs, kKeepSessions, true, r);
    keep_newest(dumps, kKeepHangDumps, false, r);
    keep_newest(reports, kKeepReports, true, r);
    keep_newest(zips, kKeepReports, true, r);
    keep_newest(profiles, kKeepCpuProfiles, true, r);
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
    if (!fs::is_directory(fs::symlink_status(logsFolder, ec)) || ec || reparse_point(logsFolder))
        return out;  // missing, or a link or junction: not looked into
    for (auto it = fs::directory_iterator(logsFolder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e;
        const std::wstring name = it->path().filename().wstring();
        if (reparse_point(it->path()))  // a link or junction: never sent to the Recycle Bin
            continue;
        const bool folder = fs::is_directory(fs::symlink_status(it->path(), e));
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

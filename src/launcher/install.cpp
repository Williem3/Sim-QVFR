#include "install.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include <windows.h>
#include <winhttp.h>

#include "../lang.h"
#include "../sha256_file.h"
#include "launch_plan.h"

namespace simqvfr::launcher {
namespace {

namespace fs = std::filesystem;
using lang::tr;
using lang::trf;

constexpr unsigned long long kMaxDownload = 256ull << 20;  // far more than a release zip
constexpr wchar_t kSettingsFile[] = L"sim-qvfr.ini";
constexpr wchar_t kHashList[] = L"SHA256.txt";

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A version as a folder name: up to its first space, only letters, digits, dots and dashes.
std::wstring version_folder(const std::wstring& version) {
    std::wstring out;
    for (const wchar_t c : version) {
        if (c == L' ')
            break;
        if (std::iswalnum(c) || c == L'.' || c == L'-')
            out += c;
    }
    return out.empty() ? L"previous" : out;
}

bool download(const std::string& url, const fs::path& file, unsigned long long size,
              const std::function<void(const InstallProgress&)>& progress, std::wstring& error) {
    const std::wstring wide = widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = tr(L"the download address isn't one to trust");
        return false;
    }
    HINTERNET session = WinHttpOpen(L"sim-qvfr-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    // GitHub answers with a redirect to its download host; WinHTTP follows it (HTTPS to HTTPS only).
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = false;
    HANDLE out = INVALID_HANDLE_VALUE;
    if (request) {
        WinHttpSetTimeouts(request, 10000, 10000, 10000, 30000);
        DWORD status = 0, statusSize = sizeof(status);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            unsigned long long got = 0;
            ok = out != INVALID_HANDLE_VALUE;
            std::vector<char> chunk(64 * 1024);
            for (DWORD available = 0; ok && WinHttpQueryDataAvailable(request, &available) && available > 0;) {
                DWORD read = 0, written = 0;
                const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
                ok = WinHttpReadData(request, chunk.data(), want, &read) && WriteFile(out, chunk.data(), read, &written, nullptr) &&
                     written == read;
                got += read;
                if (got > kMaxDownload || got > size) {
                    ok = false;
                    error = tr(L"the download is larger than the release says");
                }
                progress({tr(L"Downloading"), size ? static_cast<double>(got) / size : 0});
            }
            if (ok && got != size) {
                ok = false;
                error = trf(L"the download ended early ({got} of {size} bytes)",
                            {{L"got", std::to_wstring(got)}, {L"size", std::to_wstring(size)}});
            }
        } else {
            error = status ? trf(L"GitHub answered {status}", {{L"status", std::to_wstring(status)}})
                           : std::wstring(tr(L"GitHub could not be reached"));
        }
    } else {
        error = tr(L"GitHub could not be reached");
    }
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    for (HINTERNET h : {request, connect, session})
        if (h)
            WinHttpCloseHandle(h);
    if (!ok && error.empty())
        error = tr(L"the download failed");
    return ok;
}

// Windows' own tar.exe (System32) unpacks the zip; nothing is run from the download.
bool unpack(const fs::path& zip, const fs::path& into, std::wstring& error) {
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const fs::path tar = fs::path(system) / L"tar.exe";
    std::wstring command = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + into.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, into.c_str(), &si, &pi)) {
        error = trf(L"Windows' tar.exe couldn't start (error {code})", {{L"code", std::to_wstring(GetLastError())}});
        return false;
    }
    CloseHandle(pi.hThread);
    const bool done = WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0;
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (!done)
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    if (!done || code != 0) {
        error = tr(L"the zip couldn't be unpacked");
        return false;
    }
    return true;
}

std::map<std::wstring, std::string> read_hash_list(const fs::path& file) {
    std::string text;
    return read_file(file.wstring(), text) ? parse_sha256_list(text) : std::map<std::wstring, std::string>{};
}

}  // namespace

std::map<std::wstring, std::string> parse_sha256_list(const std::string& text) {
    std::map<std::wstring, std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find('\n', at), text.size());
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t gap = line.find("  ");
        if (gap != 64 || line.size() <= 66)
            continue;
        const std::string hex = lower(line.substr(0, 64));
        if (hex.find_first_not_of("0123456789abcdef") != std::string::npos)
            continue;
        std::wstring path = widen(line.substr(66));
        std::replace(path.begin(), path.end(), L'/', L'\\');
        // No way out of the install folder.
        if (path.empty() || path.find(L"..") != std::wstring::npos || path[0] == L'\\' || path.find(L':') != std::wstring::npos)
            continue;
        out[path] = hex;
    }
    return out;
}

std::vector<InstallStep> plan_install(const std::map<std::wstring, std::string>& release,
                                      const std::map<std::wstring, std::string>& oldRelease,
                                      const std::function<std::string(const std::wstring&)>& current) {
    std::vector<InstallStep> out;
    for (const auto& [path, hash] : release) {
        InstallStep step{path, InstallAction::Add};
        const std::string now = lower(current(path));
        if (_wcsicmp(path.c_str(), kSettingsFile) == 0) {
            step.action = now.empty() ? InstallAction::Add : InstallAction::KeepSettings;
        } else if (now.empty()) {
            step.action = InstallAction::Add;
        } else if (now == hash) {
            step.action = InstallAction::Same;
        } else {
            // Not as the old release had it: the user's own. Without the old release's list, the helmet images
            // count as the user's (they are meant to be replaced) and the program files as the old release's.
            const auto old = oldRelease.find(path);
            const bool helmet = _wcsnicmp(path.c_str(), L"layer\\helmet\\", 13) == 0;
            const bool changed = old != oldRelease.end() ? old->second != now : helmet;
            step.action = changed ? InstallAction::KeepChanged : InstallAction::Replace;
        }
        out.push_back(step);
    }
    return out;
}

bool trusted_download(const std::string& url) {
    return url.rfind(kReleaseDownloadPrefix, 0) == 0 && url.find("..") == std::string::npos &&
           url.size() > sizeof(kReleaseDownloadPrefix) && url.size() < 1024;
}

bool packaged_install(const std::wstring& folder) {
    std::error_code ec;
    return fs::exists(fs::path(folder) / L"VERSION.txt", ec) && fs::exists(fs::path(folder) / kHashList, ec);
}

InstallResult install_release(const std::wstring& folderText, const std::string& url, const std::string& sha256,
                              unsigned long long size, const std::wstring& oldVersion,
                              const std::function<void(const InstallProgress&)>& progress) {
    InstallResult r;
    const fs::path folder = folderText, update = folder / L"update", staged = update / L"staged";
    std::error_code ec;
    if (!trusted_download(url) || sha256.size() != 64 || size == 0 || size > kMaxDownload) {
        r.error = tr(L"the release's download isn't one to trust");
        return r;
    }
    fs::remove_all(staged, ec);
    fs::create_directories(staged, ec);
    const fs::path zip = update / L"release.zip";

    // 1. Download, and the zip as GitHub has it.
    progress({tr(L"Downloading"), 0});
    if (!download(url, zip, size, progress, r.error))
        return r;
    progress({tr(L"Checking the download"), 1});
    if (lower(sha256_file(zip.c_str())) != lower(sha256)) {
        r.error = tr(L"the download doesn't match the release (its SHA-256 differs)");
        return r;
    }
    // 2. Unpacked, each file as its SHA256.txt says.
    progress({tr(L"Unpacking"), 0});
    if (!unpack(zip, staged, r.error))
        return r;
    return install_staged(folder.wstring(), staged.wstring(), oldVersion, progress);
}

InstallResult install_staged(const std::wstring& folderText, const std::wstring& stagedText, const std::wstring& oldVersion,
                             const std::function<void(const InstallProgress&)>& progress) {
    InstallResult r;
    const fs::path folder = folderText, staged = stagedText;
    const fs::path backup = folder / L"update" / (L"backup-" + version_folder(oldVersion));
    r.backup = backup.wstring();
    std::error_code ec;
    const std::map<std::wstring, std::string> release = read_hash_list(staged / kHashList);
    if (!release.count(L"sim-qvfr-launcher.exe") || !release.count(L"layer\\sim-qvfr.dll")) {
        r.error = tr(L"the release has no launcher or layer in it");
        return r;
    }
    int checked = 0;
    for (const auto& [path, hash] : release) {
        progress({tr(L"Checking the files"), static_cast<double>(++checked) / release.size()});
        if (lower(sha256_file((staged / path).c_str())) != hash) {
            r.error = trf(L"{file} in the release doesn't match its SHA256.txt", {{L"file", path}});
            return r;
        }
    }
    // 3. What to replace, add and keep.
    const std::map<std::wstring, std::string> oldRelease = read_hash_list(folder / kHashList);
    std::vector<InstallStep> plan = plan_install(release, oldRelease, [&](const std::wstring& path) {
        return fs::exists(folder / path, ec) ? sha256_file((folder / path).c_str()) : std::string();
    });
    plan.push_back({kHashList, fs::exists(folder / kHashList, ec) ? InstallAction::Replace : InstallAction::Add});  // last: the new list

    // 4. Old files into the backup, new ones in place; on a failure, everything back as it was.
    progress({tr(L"Installing"), 0});
    fs::remove_all(backup, ec);
    std::vector<std::wstring> moved, added;
    bool ok = true;
    for (size_t i = 0; i < plan.size() && ok; ++i) {
        const InstallStep& s = plan[i];
        progress({tr(L"Installing"), static_cast<double>(i + 1) / plan.size()});
        if (s.action == InstallAction::KeepSettings || s.action == InstallAction::KeepChanged) {
            ++r.kept;
            continue;
        }
        if (s.action == InstallAction::Same)
            continue;
        const fs::path target = folder / s.path, source = staged / s.path;
        fs::create_directories(target.parent_path(), ec);
        if (s.action == InstallAction::Replace) {
            fs::create_directories((backup / s.path).parent_path(), ec);
            // A rename: allowed for the running launcher, and the same volume as update\.
            if (!MoveFileExW(target.c_str(), (backup / s.path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
                r.error = trf(L"couldn't move {file} aside (error {code}): is it in use?",
                              {{L"file", s.path}, {L"code", std::to_wstring(GetLastError())}});
                ok = false;
                break;
            }
            moved.push_back(s.path);
        }
        if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            r.error = trf(L"couldn't write {file} (error {code})",
                          {{L"file", s.path}, {L"code", std::to_wstring(GetLastError())}});
            ok = false;
            break;
        }
        (s.action == InstallAction::Replace ? r.replaced : r.added) += 1;
        if (s.action == InstallAction::Add)
            added.push_back(s.path);
    }
    if (!ok) {
        for (const std::wstring& path : added)
            DeleteFileW((folder / path).c_str());
        for (const std::wstring& path : moved) {
            DeleteFileW((folder / path).c_str());
            MoveFileExW((backup / path).c_str(), (folder / path).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        r.replaced = r.added = r.kept = 0;
        return r;
    }
    r.ok = true;
    return r;
}

namespace {

// update\backup-* folders, newest first.
std::vector<fs::path> backups_newest_first(const fs::path& update) {
    std::error_code ec;
    std::vector<fs::directory_entry> backups;
    for (const auto& entry : fs::directory_iterator(update, ec))
        if (entry.is_directory(ec) && entry.path().filename().wstring().rfind(L"backup-", 0) == 0)
            backups.push_back(entry);
    std::sort(backups.begin(), backups.end(), [&](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.last_write_time(ec) > b.last_write_time(ec);
    });
    std::vector<fs::path> out;
    for (const auto& entry : backups)
        out.push_back(entry.path());
    return out;
}

}  // namespace

void clean_update_folder(const std::wstring& folderText) {
    const fs::path update = fs::path(folderText) / L"update";
    std::error_code ec;
    if (!fs::exists(update, ec))
        return;
    fs::remove_all(update / L"staged", ec);
    fs::remove(update / L"release.zip", ec);
    // The newest backup stays (Go back); older ones go.
    const std::vector<fs::path> backups = backups_newest_first(update);
    for (size_t i = 1; i < backups.size(); ++i)
        fs::remove_all(backups[i], ec);
}

bool find_backup(const std::wstring& folderText, Backup& backup) {
    std::error_code ec;
    const std::vector<fs::path> backups = backups_newest_first(fs::path(folderText) / L"update");
    if (backups.empty() || !fs::exists(backups[0] / kHashList, ec))
        return false;
    backup.folder = backups[0].wstring();
    backup.version = backups[0].filename().wstring().substr(7);  // after "backup-"
    // VERSION.txt's first line, "Sim-QVFR <version>", when the backup has it (the install replaced it).
    std::string text;
    if (read_file((backups[0] / L"VERSION.txt").wstring(), text)) {
        std::string line = text.substr(0, text.find_first_of("\r\n"));
        constexpr char kPrefix[] = "Sim-QVFR ";
        if (line.rfind(kPrefix, 0) == 0 && line.size() > sizeof(kPrefix) - 1 && line.size() < 80)
            backup.version = widen(line.substr(sizeof(kPrefix) - 1));
    }
    return true;
}

std::vector<RestoreStep> plan_restore(const std::map<std::wstring, std::string>& backupRelease,
                                      const std::map<std::wstring, std::string>& release,
                                      const std::function<bool(const std::wstring&)>& inBackup,
                                      const std::function<std::string(const std::wstring&)>& current) {
    std::map<std::wstring, bool> paths;  // both lists' files, once
    for (const auto& [path, hash] : backupRelease)
        paths[path] = true;
    for (const auto& [path, hash] : release)
        paths[path] = true;
    std::vector<RestoreStep> out;
    for (const auto& [path, unused] : paths) {
        if (_wcsicmp(path.c_str(), kSettingsFile) == 0 || _wcsicmp(path.c_str(), kHashList) == 0)
            continue;
        const auto mine = release.find(path);
        const std::string now = lower(current(path));
        // As the release in place has it (or not there at all): the install put it there, nobody changed it since.
        const bool asInstalled = now.empty() || (mine != release.end() && mine->second == now);
        if (inBackup(path)) {
            out.push_back({path, asInstalled ? RestoreAction::Restore : RestoreAction::Keep});
        } else if (mine != release.end() && !backupRelease.count(path) && !now.empty()) {
            out.push_back({path, asInstalled ? RestoreAction::Remove : RestoreAction::Keep});
        }
    }
    return out;
}

InstallResult restore_backup(const std::wstring& folderText, const Backup& backupInfo, const std::wstring& currentVersion,
                             const std::function<void(const InstallProgress&)>& progress) {
    InstallResult r;
    const fs::path folder = folderText, from = backupInfo.folder;
    const fs::path into = folder / L"update" / (L"backup-" + version_folder(currentVersion));
    r.backup = into.wstring();
    std::error_code ec;
    if (fs::equivalent(from, into, ec)) {
        r.error = tr(L"the backup is of the version in place");
        return r;
    }
    const std::map<std::wstring, std::string> backupRelease = read_hash_list(from / kHashList);
    if (backupRelease.empty()) {
        r.error = tr(L"the backup has no SHA256.txt");
        return r;
    }
    // The backup's files as its release had them (one not on its list was the user's at the install: as it is).
    int checked = 0;
    for (const auto& [path, hash] : backupRelease) {
        progress({tr(L"Checking the backup"), static_cast<double>(++checked) / backupRelease.size()});
        if (fs::exists(from / path, ec) && lower(sha256_file((from / path).c_str())) != hash) {
            r.error = trf(L"{file} in the backup doesn't match its SHA256.txt", {{L"file", path}});
            return r;
        }
    }
    // Its launcher and layer, as an install asks of a release: each in the backup, or the one in place already its
    // release's (an install left a file it didn't change in place). Otherwise going back would mix two versions.
    for (const wchar_t* path : {L"sim-qvfr-launcher.exe", L"layer\\sim-qvfr.dll"}) {
        const auto listed = backupRelease.find(path);
        const bool there = listed != backupRelease.end() &&
                           (fs::is_regular_file(from / path, ec) ||
                            (fs::is_regular_file(folder / path, ec) && lower(sha256_file((folder / path).c_str())) == listed->second));
        if (!there) {
            r.error = tr(L"the backup has no launcher or layer in it");
            return r;
        }
    }
    const std::map<std::wstring, std::string> release = read_hash_list(folder / kHashList);
    std::vector<RestoreStep> plan = plan_restore(
        backupRelease, release, [&](const std::wstring& path) { return fs::is_regular_file(from / path, ec); },
        [&](const std::wstring& path) {
            return fs::exists(folder / path, ec) ? sha256_file((folder / path).c_str()) : std::string();
        });
    plan.push_back({kHashList, RestoreAction::Restore});  // last: the backup's list

    // The release in place into its own backup, the old files back; on a failure, everything back as it was.
    progress({tr(L"Going back"), 0});
    fs::remove_all(into, ec);
    std::vector<std::wstring> movedOut, movedIn;  // folder -> into; from -> folder
    const auto move = [&](const fs::path& source, const fs::path& target, const std::wstring& path) {
        fs::create_directories(target.parent_path(), ec);
        if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            return true;
        r.error = trf(L"couldn't move {file} (error {code}): is it in use?",
                      {{L"file", path}, {L"code", std::to_wstring(GetLastError())}});
        return false;
    };
    bool ok = true;
    for (size_t i = 0; i < plan.size() && ok; ++i) {
        const RestoreStep& s = plan[i];
        progress({tr(L"Going back"), static_cast<double>(i + 1) / plan.size()});
        if (s.action == RestoreAction::Keep) {
            ++r.kept;
            continue;
        }
        if (fs::exists(folder / s.path, ec)) {
            if (!(ok = move(folder / s.path, into / s.path, s.path)))
                break;
            movedOut.push_back(s.path);
        }
        if (s.action == RestoreAction::Restore) {
            if (!(ok = move(from / s.path, folder / s.path, s.path)))
                break;
            movedIn.push_back(s.path);
            ++r.replaced;
        }
    }
    if (!ok) {
        for (auto it = movedIn.rbegin(); it != movedIn.rend(); ++it)
            MoveFileExW((folder / *it).c_str(), (from / *it).c_str(), MOVEFILE_REPLACE_EXISTING);
        for (auto it = movedOut.rbegin(); it != movedOut.rend(); ++it)
            MoveFileExW((into / *it).c_str(), (folder / *it).c_str(), MOVEFILE_REPLACE_EXISTING);
        fs::remove_all(into, ec);
        r.replaced = r.added = r.kept = 0;
        return r;
    }
    // The backup is in place now; what's left of it (the old release's copies of files the user changed since) goes,
    // and the release that was in place is the newest backup: Go back again returns to it.
    fs::remove_all(from, ec);
    fs::last_write_time(into, fs::file_time_type::clock::now(), ec);
    r.ok = true;
    return r;
}

}  // namespace simqvfr::launcher

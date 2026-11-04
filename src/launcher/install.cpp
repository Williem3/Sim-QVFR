#include "install.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include <windows.h>
#include <winhttp.h>

#include "../sha256_file.h"
#include "launch_plan.h"

namespace aceqv::launcher {
namespace {

namespace fs = std::filesystem;

constexpr unsigned long long kMaxDownload = 256ull << 20;  // far more than a release zip
constexpr wchar_t kSettingsFile[] = L"ace-quadview.ini";
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
        error = L"the download address isn't one to trust";
        return false;
    }
    HINTERNET session = WinHttpOpen(L"ace-quadview-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
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
                    error = L"the download is larger than the release says";
                }
                progress({L"Downloading", size ? static_cast<double>(got) / size : 0});
            }
            if (ok && got != size) {
                ok = false;
                error = L"the download ended early (" + std::to_wstring(got) + L" of " + std::to_wstring(size) + L" bytes)";
            }
        } else {
            error = status ? L"GitHub answered " + std::to_wstring(status) : L"GitHub could not be reached";
        }
    } else {
        error = L"GitHub could not be reached";
    }
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    for (HINTERNET h : {request, connect, session})
        if (h)
            WinHttpCloseHandle(h);
    if (!ok && error.empty())
        error = L"the download failed";
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
        error = L"Windows' tar.exe couldn't start (error " + std::to_wstring(GetLastError()) + L")";
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
        error = L"the zip couldn't be unpacked";
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
            const bool helmet = _wcsnicmp(path.c_str(), L"ace\\helmet\\", 11) == 0;
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
        r.error = L"the release's download isn't one to trust";
        return r;
    }
    fs::remove_all(staged, ec);
    fs::create_directories(staged, ec);
    const fs::path zip = update / L"release.zip";

    // 1. Download, and the zip as GitHub has it.
    progress({L"Downloading", 0});
    if (!download(url, zip, size, progress, r.error))
        return r;
    progress({L"Checking the download", 1});
    if (lower(sha256_file(zip.c_str())) != lower(sha256)) {
        r.error = L"the download doesn't match the release (its SHA-256 differs)";
        return r;
    }
    // 2. Unpacked, each file as its SHA256.txt says.
    progress({L"Unpacking", 0});
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
    if (!release.count(L"ace-quadview-launcher.exe") || !release.count(L"ace\\ace-quadview.dll")) {
        r.error = L"the release has no launcher or layer in it";
        return r;
    }
    int checked = 0;
    for (const auto& [path, hash] : release) {
        progress({L"Checking the files", static_cast<double>(++checked) / release.size()});
        if (lower(sha256_file((staged / path).c_str())) != hash) {
            r.error = path + L" in the release doesn't match its SHA256.txt";
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
    progress({L"Installing", 0});
    fs::remove_all(backup, ec);
    std::vector<std::wstring> moved, added;
    bool ok = true;
    for (size_t i = 0; i < plan.size() && ok; ++i) {
        const InstallStep& s = plan[i];
        progress({L"Installing", static_cast<double>(i + 1) / plan.size()});
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
                r.error = L"couldn't move " + s.path + L" aside (error " + std::to_wstring(GetLastError()) + L"): is it in use?";
                ok = false;
                break;
            }
            moved.push_back(s.path);
        }
        if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
            r.error = L"couldn't write " + s.path + L" (error " + std::to_wstring(GetLastError()) + L")";
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

void clean_update_folder(const std::wstring& folderText) {
    const fs::path update = fs::path(folderText) / L"update";
    std::error_code ec;
    if (!fs::exists(update, ec))
        return;
    fs::remove_all(update / L"staged", ec);
    fs::remove(update / L"release.zip", ec);
    // The newest backup stays (to go back by hand); older ones go.
    std::vector<fs::directory_entry> backups;
    for (const auto& entry : fs::directory_iterator(update, ec))
        if (entry.is_directory(ec) && entry.path().filename().wstring().rfind(L"backup-", 0) == 0)
            backups.push_back(entry);
    std::sort(backups.begin(), backups.end(), [&](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.last_write_time(ec) > b.last_write_time(ec);
    });
    for (size_t i = 1; i < backups.size(); ++i)
        fs::remove_all(backups[i].path(), ec);
}

}  // namespace aceqv::launcher

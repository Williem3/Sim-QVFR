// Install: a newer release put in place by the launcher itself (the About pane's Install, after the update
// check found one, update_check.h). Only in a packaged install (VERSION.txt next to the launcher), never while
// ACE runs (the layer's DLL is in use).
//
// 1. The release's zip is downloaded into update\ next to the launcher, from the repository's release
//    downloads only, and checked against the SHA-256 GitHub gives for it.
// 2. Windows' own tar.exe unpacks it into update\staged, and every file is checked against its SHA256.txt.
// 3. Program files are replaced; the user's are kept: ace-quadview.ini always, anything the release doesn't
//    have (profiles, logs, Cheeky's folder), and any file of the old release the user changed (their helmet
//    images): one that differs from the old release's SHA256.txt (plan_install).
// 4. Each file replaced moves into update\backup-<old version>\ first (the running launcher too: a running exe
//    can be renamed). If a step fails, the files moved go back.
// 5. The launcher restarts as the new one; the next start clears update\ but the backup.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aceqv::launcher {

inline constexpr char kReleaseDownloadPrefix[] = "https://github.com/Williem3/Sim-QVFR/releases/download/";

// A SHA256.txt ("HEX  relative/path" lines, as package.ps1 writes it): path (backslashes) -> lowercase hex.
std::map<std::wstring, std::string> parse_sha256_list(const std::string& text);

enum class InstallAction { Add, Replace, KeepSettings, KeepChanged, Same };
struct InstallStep {
    std::wstring path;  // relative, backslashes
    InstallAction action = InstallAction::Add;
};
// What each file of the new release does: Add (not there), Replace (there, as the old release had it), Same
// (there and already the new one), KeepSettings (ace-quadview.ini), KeepChanged (there but not as the old release
// had it: the user's own). `current` gives an installed file's hash (lowercase hex; empty: not there).
std::vector<InstallStep> plan_install(const std::map<std::wstring, std::string>& release,
                                      const std::map<std::wstring, std::string>& oldRelease,
                                      const std::function<std::string(const std::wstring&)>& current);

// The download URL a release asset may have: the repository's release downloads, over HTTPS.
bool trusted_download(const std::string& url);

// Whether this launcher runs from a packaged install (VERSION.txt next to it).
bool packaged_install(const std::wstring& folder);

// Where an install stands, for the window (step and a share of it, 0..1), and its outcome.
struct InstallProgress {
    std::wstring step;
    double share = 0;
};
struct InstallResult {
    bool ok = false;
    std::wstring error;
    std::wstring backup;  // where the old files went
    int replaced = 0, added = 0, kept = 0;
};
// Downloads, checks and installs `url` (its SHA-256 `sha256`, lowercase hex, and `size` bytes) into `folder`
// (the launcher's), the old files into update\backup-<oldVersion>. `progress` is called from this thread.
InstallResult install_release(const std::wstring& folder, const std::string& url, const std::string& sha256, unsigned long long size,
                              const std::wstring& oldVersion, const std::function<void(const InstallProgress&)>& progress);

// Steps 2 to 4 for a release already unpacked into `staged`: its files checked, then installed into `folder`.
InstallResult install_staged(const std::wstring& folder, const std::wstring& staged, const std::wstring& oldVersion,
                             const std::function<void(const InstallProgress&)>& progress);

// At a start: update\ cleared but the newest backup.
void clean_update_folder(const std::wstring& folder);

}  // namespace aceqv::launcher

// The update check (opt-in: [launcher] update_check, the About pane): the newest release on GitHub against this
// launcher's version. It asks GitHub's API for the repository's releases (pre-releases too: every ace-quadview
// release is an alpha); nothing else is sent. The parsing and comparing are pure, for the tests.
#pragma once

#include <string>

namespace aceqv::launcher {

inline constexpr wchar_t kReleasesApiHost[] = L"api.github.com";
inline constexpr wchar_t kReleasesApiPath[] = L"/repos/Williem3/Sim-QVFR/releases?per_page=10";

struct Release {
    std::wstring tag, url;  // "v2026.11.3.3-alpha", its page
    // Its zip ("ace-quadview-<version>.zip", install.h): the download, its size and SHA-256 as GitHub gives them
    // (lowercase hex; empty: no zip, or no digest for it: then it can't be installed, only downloaded by hand).
    std::string zipUrl, zipSha256;
    unsigned long long zipSize = 0;
};

// The newest release that is not a draft in GitHub's releases JSON (newest first). False if there is none.
bool newest_release(const std::string& json, Release& release);

// The version's numbers in order ("v2026.11.3.3-alpha (1a2b3c4)" -> 2026, 11, 3, 3), compared: <0, 0, >0. A
// version without numbers ("dev") compares as unknown: 0 from known_version.
int compare_versions(const std::wstring& a, const std::wstring& b);
bool known_version(const std::wstring& version);

// GETs the releases JSON (WinHTTP). False with `error` set on failure.
bool fetch_releases(std::string& body, std::wstring& error);

}  // namespace aceqv::launcher

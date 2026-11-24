// The update check (opt-in: [launcher] update_check, the About pane): the newest release on GitHub against this
// launcher's version. It asks GitHub's API for the repository's releases (pre-releases too: every Sim-QVFR
// release is an alpha); nothing else is sent. The parsing and comparing are pure, for the tests.
#pragma once

#include <string>

namespace simqvfr::launcher {

inline constexpr wchar_t kReleasesApiHost[] = L"api.github.com";
inline constexpr wchar_t kReleasesApiPath[] = L"/repos/Williem3/Sim-QVFR/releases?per_page=10";
// The same repository's release pages: the only html_url the About pane opens.
// The mods' updates (mod_update.h): their releases are among the app's, so more of the list is read.
inline constexpr wchar_t kModReleasesApiPath[] = L"/repos/Williem3/Sim-QVFR/releases?per_page=100";
inline constexpr wchar_t kReleasePagePrefix[] = L"https://github.com/Williem3/Sim-QVFR/releases/";

// `url` (a release's html_url, from the network) is one of this repository's release pages: kReleasePagePrefix, then
// a path of plain URL characters (no control characters, spaces, "..", or anything a shell would read as more).
bool trusted_release_page(const std::wstring& url);

// The release channel a launcher was built for (SIM_QVFR_CHANNEL, launcher_version.h). Each looks only at its own
// packages: a stable launcher at "sim-qvfr-<version>.zip", a nightly one at "sqvfr-nightly-<version>.zip". Launchers
// shipped before the nightly line know only the stable name, so they never offer a nightly.
enum class Channel { Stable, Nightly };
// "nightly" (any case) is Nightly; anything else Stable.
Channel channel_named(const char* name);
// The package asset's name prefix for `channel` ("sim-qvfr-", "sqvfr-nightly-").
const char* package_prefix(Channel channel);

struct Release {
    std::wstring tag, url;  // "v2026.11.3.3-alpha", its page
    // Its zip ("sim-qvfr-<version>.zip", install.h): the download, its size and SHA-256 as GitHub gives them
    // (lowercase hex; empty: no zip, or no digest for it: then it can't be installed, only downloaded by hand).
    std::string zipUrl, zipSha256;
    unsigned long long zipSize = 0;
};

// The newest release that is not a draft in GitHub's releases JSON (newest first) with a package of `channel`. False if
// there is none.
bool newest_release(const std::string& json, Release& release, Channel channel = Channel::Stable);

// The version's numbers in order ("v2026.11.3.3-alpha (1a2b3c4)" -> 2026, 11, 3, 3), compared: <0, 0, >0. A
// version without numbers ("dev") compares as unknown: 0 from known_version. A nightly's date counts as one more
// number ("1.3.0-nightly.20261124" -> 1, 3, 0, 20261124), so a later nightly of the same release is newer.
int compare_versions(const std::wstring& a, const std::wstring& b);
bool known_version(const std::wstring& version);

// A release the user skipped ([launcher] update_skip): `tag` is the same version as `skip` ("v1.2.0-beta" and
// "1.2.0-beta (1a2b3c4)" are). A newer one isn't, and nothing is when `skip` has no version.
bool skipped_release(const std::wstring& tag, const std::wstring& skip);

// GETs the releases JSON (WinHTTP). False with `error` set on failure.
// `path`: another page of the same repository's releases (the mods' updates read more of them).
bool fetch_releases(std::string& body, std::wstring& error, const wchar_t* path = kReleasesApiPath);

}  // namespace simqvfr::launcher

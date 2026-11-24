// Updates for Sim-QVFR's own game mods (docs/architecture/mod-manager-plan.md phase 7), from the Settings panel's Mods
// section: each mod is released on its own, on this repository's releases, tagged "mod-<id>-v<version>" with one
// asset "sqvfr-mod-<id>-<version>.zip" (package.ps1 makes it: the mod folder's files and their SHA256.txt).
//
// Install: the zip downloaded into mods\.update\ (the repository's release downloads only) and checked against the
// SHA-256 GitHub gives; unpacked by Windows' tar.exe; every file checked against its SHA256.txt, and its mod.json must
// be the same mod; then mods\<id> moves to mods\.backup\<id>-<old version> and the new folder takes its place (on a
// failure the old one goes back). Go back swaps the backup in again the same way. Never while a game runs.
// Only first-party mods (mod_api/mod_trust.h first_party_mod) are offered updates: a third-party mod's author ships it.
#pragma once

#include <functional>
#include <string>

#include "install.h"

namespace simqvfr::launcher {

struct ModRelease {
    std::wstring tag, version;  // "mod-scs-v1.3.0", "1.3.0"
    std::string zipUrl, zipSha256;
    unsigned long long zipSize = 0;
};

// The newest release of mod `id` in GitHub's releases JSON: not a draft, tagged mod-<id>-v<version>, with its
// sqvfr-mod-<id>-<version>.zip and a SHA-256 for it; pre-releases only with `prereleases` (a nightly launcher). False:
// none.
bool newest_mod_release(const std::string& json, const std::wstring& id, bool prereleases, ModRelease& release);

// mod.json's "version" vs a release's: newer (>0), same (0), older (<0); a version without numbers ("dev") is older
// than any.
int compare_mod_versions(const std::wstring& installed, const std::wstring& release);

// The newest backup of mod `id` (mods\.backup\<id>-<version>), for Go back. False: none.
bool find_mod_backup(const std::wstring& modsFolder, const std::wstring& id, std::wstring& folder, std::wstring& version);

// Checks a staged mod folder: every file in it listed in its SHA256.txt with that hash (and nothing listed missing),
// its mod.json the mod `id`. Empty when it is good, else why not.
std::wstring check_staged_mod(const std::wstring& staged, const std::wstring& id);

// Adding a mod from a zip the user chose or dropped on the window (Settings > Mods: Add a mod…). Step 1, on a worker:
// unpacked by Windows' tar.exe into mods\.update\add, the mod's folder found (the zip's root, or its one folder),
// its mod.json read (an id a folder can have, a DLL that is there) and, when it has a SHA256.txt, every file checked.
// Nothing is approved by it: a third-party mod still runs only once the user approves it.
struct StagedMod {
    std::wstring folder;  // where it is unpacked
    std::wstring id, name, author, version, source, license;
    bool replaces = false;     // mods\<id> is there already (it is kept as mods\.backup\<id>-<its version>)
    std::wstring error;        // why it can't be added; empty: it can
};
StagedMod stage_mod_zip(const std::wstring& modsFolder, const std::wstring& zip);
// Step 2: the staged mod into mods\<id> (one in place goes to mods\.backup, as an update's does).
InstallResult add_staged_mod(const std::wstring& modsFolder, const StagedMod& staged, const std::wstring& oldVersion);

InstallResult install_mod_release(const std::wstring& modsFolder, const std::wstring& id, const ModRelease& release,
                                  const std::wstring& oldVersion, const std::function<void(const InstallProgress&)>& progress);
// Go back: the backup in place again, the mod in place to mods\.backup\<id>-<currentVersion>.
InstallResult restore_mod_backup(const std::wstring& modsFolder, const std::wstring& id, const std::wstring& currentVersion);

}  // namespace simqvfr::launcher

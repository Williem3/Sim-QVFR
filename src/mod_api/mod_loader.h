// Game mods from their own DLLs (docs/architecture/mod-manager-plan.md): where the package keeps them, and loading one
// into the game's process, after the trust check (mod_trust.h).
#pragma once

#include <memory>
#include <string>

#include "mod_trust.h"
#include "plugin_adapter.h"

namespace simqvfr::mod_api {

// The package's mods folder: <package>\mods beside the core's layer folder (sim-qvfr.dll in <package>\layer), or
// <core's folder>\mods for a development build (sim-qvfr.dll in build\Release).
std::wstring mods_folder();
// <mods folder>\<folder>\<dll>.
std::wstring mod_dll_path(const wchar_t* folder, const wchar_t* dll);

// Loads the mod DLL at `path` (a full path: only its own folder and System32 are searched for what it needs), calls its
// sqvfr_mod_entry with the host table naming `game`, and returns its adapter. Null, with the reason in `why`, when the
// file isn't there, won't load, has no entry, or its table is refused (PluginAdapter::create). The DLL stays loaded for
// the process either way.
// No trust check: load_checked_mod's last step (and the tests').
std::unique_ptr<PluginAdapter> load_mod_dll(const std::wstring& path, const games::GameInfo* game, std::string& why);

// Mod `id` at `path`, checked first (mod_trust.h decide_mod_trust): its file held open against changes from the check
// until it is loaded, its approval read from `ini`. `verdict` says what it was taken for, or why it was refused (then
// null, nothing loaded).
std::unique_ptr<PluginAdapter> load_checked_mod(const std::wstring& path, const std::wstring& id, const std::wstring& ini,
                                                const games::GameInfo* game, bool firstPartyPath, bool onlineLaunch,
                                                TrustVerdict& verdict, std::string& why);

}  // namespace simqvfr::mod_api

// The launcher's view of the installed game mods (docs/architecture/mod-manager-plan.md phase 5): each
// <package>\mods\<id>\mod.json, its optional settings.json, and what the core's trust check (mod_api/mod_trust.h) will
// decide for it, offline and online. The launcher never loads a mod's DLL: it reads these files and hashes the DLL.
//
// settings.json (schema version 1):
//   { "settings": [ { "key": "glare", "type": "toggle", "label": "Glare", "help": "...", "default": true },
//                   { "key": "mode", "type": "choice", "label": "Mode", "default": "fast",
//                     "options": [ { "value": "fast", "label": "Fast" }, { "value": "fine", "label": "Fine" } ] } ] }
// Values go to [mod_<id>] <key> of sim-qvfr.ini (toggle: 1 or 0; choice: the option's value), which every launch
// passes to the game. Up to kModSettingsMax settings; a choice has 2 to 4 options.
#pragma once

#include <string>
#include <vector>

#include "../mod_api/mod_trust.h"

namespace simqvfr::launcher {

inline constexpr int kModsMax = 8;
inline constexpr int kModSettingsMax = 16;

struct ModOption {
    std::string value;
    std::wstring label;
};

struct ModSetting {
    enum class Kind { Toggle, Choice } kind = Kind::Toggle;
    std::string key;  // [a-z0-9_], 1..32
    std::wstring label, help;
    std::string def;  // toggle: "1" or "0"; choice: an option's value
    std::vector<ModOption> options;
};

struct ModEntry {
    std::wstring id;       // its folder's name, and mod.json's id
    std::wstring folder;   // <mods>\<id>
    std::wstring name, author, version;
    std::wstring dll;      // its file name
    std::wstring source;   // its source code (an https link; required of a mod not made by Sim-QVFR)
    std::wstring license;  // its source's licence (an SPDX id: "MIT", "GPL-3.0-only"; required with source)
    std::wstring dllPath;  // <folder>\<dll>
    std::wstring sdk;      // the SDK it was built with (mod.json "sdk", stamped by SqvfrMod.cmake); empty: unknown
    int abi = 0;           // the mod ABI it says it uses (mod.json "abi"); 0: unknown
    std::vector<std::wstring> games;  // the games it covers (their ids)
    std::vector<ModSetting> settings;
    std::wstring error;    // why the mod is unusable (mod.json missing or wrong); settings.json errors go to settingsError
    std::wstring settingsError;
};

// A name a mod or a setting key may have: 1..32 of a-z, 0-9, '_' and (`dash`) '-'.
bool valid_mod_name(const std::string& name, bool dash);

// mod.json's text into `mod` (id, name, author, version, dll, source, license, games). False, with `error`, when it
// can't be used: a mod not made by Sim-QVFR must link its source code and name its licence.
bool valid_source_link(const std::string& url);
bool parse_mod_manifest(const std::string& text, ModEntry& mod, std::wstring& error);
// settings.json's text. False, with `error`, when it can't be used (then no settings show).
bool parse_mod_settings(const std::string& text, std::vector<ModSetting>& settings, std::wstring& error);

// The mods in `modsFolder` (each subfolder with a mod.json), by id, at most kModsMax. A folder that is a link or a
// junction is left out.
std::vector<ModEntry> scan_mods(const std::wstring& modsFolder);

// What the core will decide for a mod (mod_api::decide_mod_trust), as the launcher shows it.
struct ModStatus {
    mod_api::ModFacts facts;
    mod_api::ModApproval approval;
    mod_api::TrustVerdict offline, online;
    bool approvable = false;  // third-party: the user's approval decides
};
// `coreOfficial`: the layer DLL is officially signed.
ModStatus mod_status(const ModEntry& mod, const std::wstring& ini, bool coreOfficial);

// A setting's value in `ini` ([mod_<id>] <key>), its default when unset or not one of its values.
std::string mod_setting_value(const std::wstring& ini, const std::wstring& id, const ModSetting& setting);
bool write_mod_setting(const std::wstring& ini, const std::wstring& id, const ModSetting& setting, const std::string& value);

}  // namespace simqvfr::launcher

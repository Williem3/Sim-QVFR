// Sim-QVFR launcher: a launch from a launcher running as administrator, its layers registered in Windows for that
// launch only.
//
// Why: a game started from an elevated launcher inherits its rights, and the game's OpenXR loader then ignores
// XR_API_LAYER_PATH (the run folder) and reads explicit layers from HKLM\...\ApiLayers\Explicit alone (not HKCU).
// It still reads XR_ENABLE_API_LAYERS, so it looks for the launch's layers by name, finds none, and xrCreateInstance
// fails: the game starts flat. The launcher, elevated already, adds one value to that key for each manifest the
// launch names, and takes them out when the game exits (a copy of the launcher waits for it: --unregister-after).
// An explicit layer loads only in an app that names it, so nothing else on the PC changes, nor the game from Steam.
//
// The user's registry isn't ours, so only values are added and removed. The key is never deleted, re-created or
// rewritten: it is created only when missing, and stays after. No other value is touched. Each value goes into a
// ledger file before it is added, and comes out only if the ledger lists it and it still holds what was written
// (DWORD 0); one changed since is left as it is. A value already there is left alone and isn't listed. A launch
// whose removal never ran (the PC went off) is swept from its ledger at the next start as administrator.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace simqvfr::launcher {

inline constexpr wchar_t kExplicitLayersKey[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit";

// One process, by id and start time (an id alone is reused once the process is gone).
struct ProcessStamp {
    DWORD pid = 0;
    uint64_t created = 0;  // FILETIME units
    bool operator==(const ProcessStamp&) const = default;
};

ProcessStamp process_stamp(HANDLE process);
// Still running. One that can't be looked at counts as running: its values wait.
bool stamp_running(const ProcessStamp& p);

// What one launch added: its launcher, its game once started, and the values (manifest paths).
struct Ledger {
    ProcessStamp launcher;
    ProcessStamp game;  // pid 0: not started (yet)
    std::vector<std::wstring> values;
    bool operator==(const Ledger&) const = default;
};

std::string ledger_text(const Ledger& ledger);
bool parse_ledger(const std::string& text, Ledger& out);

// Adds a value (REG_DWORD 0) for each of `manifests` under root\subkey, each written into the ledger at
// `ledgerPath` before it is added. `backup`: a .reg of the key as it was, written first if the key exists (empty:
// none). `notes`: one line per thing done, for launch.txt. On failure the values added are taken out again.
bool register_layers(HKEY root, const wchar_t* subkey, bool machine, const std::vector<std::wstring>& manifests,
                     const std::wstring& ledgerPath, const std::wstring& backup, std::vector<std::wstring>& notes,
                     std::wstring& error);
// Writes the game into the ledger once it has started.
bool ledger_game_started(const std::wstring& ledgerPath, const ProcessStamp& game);

// Takes out the values the ledger lists that still hold DWORD 0; the key stays. The ledger goes when nothing it
// lists is ours any more (taken out, gone already, or changed since); a value that couldn't be taken out (no
// rights) stays listed for the next time. False if any couldn't.
bool unregister_layers(HKEY root, const wchar_t* subkey, const std::wstring& ledgerPath, std::vector<std::wstring>& notes);

// Every ledger in `folder` whose launch is over (its game gone; not started and its launcher gone): unregistered.
void sweep_ledgers(HKEY root, const wchar_t* subkey, const std::wstring& folder, std::vector<std::wstring>& notes,
                   const std::function<bool(const ProcessStamp&)>& running = stamp_running);

}  // namespace simqvfr::launcher

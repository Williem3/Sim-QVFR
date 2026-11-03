// The OpenXR layers pane's two profiles:
//  - With the launcher: which layers each launch loads below the ACE layer, and in what order
//    ([launcher] layer_order, off_layers, on_layers in ace-quadview.ini; launch_plan.h applies them).
//    Windows' registrations are left as they are; this profile changes ACE launched from here only.
//  - Without the launcher: Windows' own implicit layer registrations, their order and on/off. This is
//    what every other app loads, and ACE started from Steam. Written to the registry; HKLM needs
//    administrator rights (the launcher runs itself elevated with --apply-system-layers).
// Neither can break the mod or ACE's start: the ACE layer is always nearest the game, Two-loop and
// Cheeky's frame generation keep VectorXR's frame generation off, and with Cheeky loaded its layer
// stays on (all for launches from here). In Windows' list an installed ACE layer stays first.
#pragma once

#include <string>
#include <vector>

#include <windows.h>

#include "launch_plan.h"

namespace aceqv::launcher {

// ---- With the launcher --------------------------------------------------------------------------

struct LauncherLayers {
    std::vector<std::string> order;  // layer_order: below the ACE layer, nearest the game first
    std::vector<std::string> off;    // off_layers: off for ACE although on in Windows
    std::vector<std::string> on;     // on_layers: on for ACE although off in Windows
    bool operator==(const LauncherLayers&) const = default;
};

LauncherLayers read_launcher_layers(const std::wstring& ini);
bool write_launcher_layers(const std::wstring& ini, const LauncherLayers& layers);
std::vector<std::string> split_layer_names(const std::wstring& list);

// The options of a launch: the profile, and what the launch locks. Two-loop (two ACE frames per headset
// frame) and Cheeky's frame generation turn VectorXR's frame generation off; Cheeky loaded keeps its layer on.
Options launch_options(const LauncherLayers& layers, bool twoLoop, bool cheekyLoad, bool cheekyFg,
                       std::vector<std::wstring> setVariables = {});
// Moves `name` one place among `plan.layers` (step -1: toward the game, +1: toward the runtime) and
// writes the whole order into the profile. False when it is already at that end.
bool move_launcher_layer(LauncherLayers& layers, const LayerPlan& plan, const std::string& name, int step);
// Turns `name` on or off for launches; `systemOn` (its state in Windows) is the default and is not listed.
void set_launcher_layer(LauncherLayers& layers, const std::string& name, bool on, bool systemOn);

// ---- Without the launcher (Windows) -------------------------------------------------------------

inline constexpr wchar_t kImplicitLayersKey[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";

struct SystemLayer {
    std::wstring manifest;  // the registry value's name
    bool machine = true;    // HKLM (else HKCU, which the loader reads after HKLM)
    bool on = true;         // DWORD 0
    std::string name;       // the layer's name from its manifest (empty: unreadable)
    std::string lock;       // why it cannot move (empty: free)
    bool operator==(const SystemLayer& o) const { return manifest == o.manifest && machine == o.machine && on == o.on; }
};

// Both hives' registrations in the loader's order (HKLM, then HKCU), named from their manifests and locked.
// `machine`/`user`: the roots to read `subkey` under (tests use a scratch key).
// `lockMoved`: the lock moved the ACE layer up (the registry has it below another layer of its hive).
std::vector<SystemLayer> read_system_layers(HKEY machine = HKEY_LOCAL_MACHINE, HKEY user = HKEY_CURRENT_USER,
                                            const wchar_t* subkey = kImplicitLayersKey, bool* lockMoved = nullptr);
// Locks the ACE layer first within its hive (moving it there if it is not). True if it moved.
bool lock_system_layers(std::vector<SystemLayer>& layers);
// Moves `layers[index]` one place within its hive, never above a locked layer. False if it cannot move.
bool move_system_layer(std::vector<SystemLayer>& layers, size_t index, int step);
// Whether applying `want` over `current` writes HKLM (administrator rights).
bool system_needs_admin(const std::vector<SystemLayer>& current, const std::vector<SystemLayer>& want);

// Writes one hive's entries of `want` (in order, with their switches) under root\subkey. Refuses when the
// key's value names differ from `want`'s (something was installed or removed meanwhile). A change of order
// deletes and re-creates every value in the new order (the loader reads them in that order); on failure the
// key is put back as it was. `backup`: a .reg file of the key as it was, written first (empty: none).
bool write_system_hive(HKEY root, const wchar_t* subkey, bool machine, const std::vector<SystemLayer>& want,
                       const std::wstring& backup, std::wstring& error);

// The elevated helper's request: one "M|U <tab> 1|0 <tab> manifest" line per layer, UTF-8.
std::string system_request(const std::vector<SystemLayer>& want);
bool parse_system_request(const std::string& text, std::vector<SystemLayer>& want);

}  // namespace aceqv::launcher

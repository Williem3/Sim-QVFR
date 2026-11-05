// Shared between the launch (main.cpp) and the settings window (window.cpp).
#pragma once

#include <string>
#include <vector>

#include <windows.h>

#include "launch_plan.h"
#include "layer_order.h"
#include "preflight.h"

namespace simqvfr::launcher {

// sim-qvfr.ini next to the launcher.
std::wstring settings_path();
// runtime.ini next to the launcher: the layer's report of the last session's headset (recommended
// per-eye size, eye tracking), written while the game runs.
std::wstring runtime_info_path();
// The folder holding sim-qvfr.dll: next to the launcher (development build) or its layer folder.
std::wstring layer_folder();
// One launch (or dry run) with the settings in that file; `report` gets what it did. 0 on success.
int launch_game(bool dryRun, std::wstring& report);
// The layers a launch with the settings in that file would load, and how (the OpenXR layers pane).
LayerPlan launcher_layer_plan();
// The chosen game's exe as a launch would start it ([launcher] game_dir_<id>, else through Steam); empty if not
// found.
std::wstring launcher_game_exe();
// Makes the game in [launcher] game (ace, acc; games::game_by_id) the one the launcher starts.
void choose_launcher_game();
// The launch checks' input (preflight.h) for `exe` from the ini, Steam, the game folder and Windows' layers;
// the exe's build is `build` (the window hashes the exe on a worker).
PreflightInput launcher_preflight(const std::wstring& exe, BuildState build);
// Writes Windows' implicit layer list `want` (layer_order.h): HKCU here, HKLM through an elevated copy of
// the launcher (--apply-system-layers), each hive backed up into logs first. Empty on success, else why.
std::wstring apply_system_layers(HWND owner, const std::vector<SystemLayer>& want);
// The settings window; returns the process exit code.
int run_window(HINSTANCE instance, int show);

}  // namespace simqvfr::launcher

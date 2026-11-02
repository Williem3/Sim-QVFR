// Shared between the launch (main.cpp) and the settings window (window.cpp).
#pragma once

#include <string>

#include <windows.h>

namespace aceqv::launcher {

// ace-quadview.ini next to the launcher.
std::wstring settings_path();
// runtime.ini next to the launcher: the layer's report of the last session's headset (recommended
// per-eye size, eye tracking), written while the game runs.
std::wstring runtime_info_path();
// The folder holding ace-quadview.dll: next to the launcher (development build) or its ace folder.
std::wstring layer_folder();
// One launch (or dry run) with the settings in that file; `report` gets what it did. 0 on success.
int launch_ace(bool dryRun, std::wstring& report);
// The settings window; returns the process exit code.
int run_window(HINSTANCE instance, int show);

}  // namespace aceqv::launcher

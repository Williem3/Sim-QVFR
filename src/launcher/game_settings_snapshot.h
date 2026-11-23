// The game's own graphics settings at a launch, copied into its run folder (logs\run-<time>\game-settings.txt) so a
// report carries what each run actually used, not what the files say when the report is made. Read-only: the game's
// files are read, never written. Only the settings listed here are copied (key=value, one per line, ini-like so two
// copies diff line by line); the rest of the game's files (driver names, liveries, paths) stays out of a public issue.
//
// The file:
//   ; <game>'s graphics settings at this launch, read from its files (Sim-QVFR changed nothing in them)
//   ; summary: Post effects High, MSAA Off, ...        (the report's details.md line)
//   [Config_DX11_VR.ini]
//   EPostProcessingSettings=2 ; Post effects: High
//   RenderScale=1.000
//   [Settings.JSON Graphic Options]
//   ...
// A setting the file lacks is "Key=" with "; not in the file"; a file that can't be read is a section with only
// "; could not read the file". Games without a reader get no file.
#pragma once

#include <string>

namespace simqvfr::launcher {

inline constexpr wchar_t kGameSettingsName[] = L"game-settings.txt";

// The copy for the game `gameId` (GameInfo::id) at `exe`; empty for a game with no reader yet.
std::string game_settings_text(const char* gameId, const std::wstring& exe);

// Le Mans Ultimate's copy from its two files' bytes (pure, for the test): UserData\Config_DX11_VR.ini's [COMPONENTS]
// and UserData\player\Settings.JSON's "Graphic Options".
std::string lmu_settings_text(bool configOk, const std::string& config, bool playerOk, const std::string& player);

// The "; summary:" line of a copy, without its prefix (UTF-8 to UTF-16); empty if it has none.
std::wstring game_settings_summary(const std::string& text);

}  // namespace simqvfr::launcher

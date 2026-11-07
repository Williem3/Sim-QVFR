// Who a game is: what the launcher and the layer need to find, start and recognise it (src/games/README.md).
// Plain data with no Windows, OpenXR or D3D dependency, so the launcher and the layer share it. Each game
// defines its own in src/games/<id>/game.h; games.cpp lists them.
#pragma once

#include <cstddef>
#include <string>

namespace simqvfr::games {

// What Sim-QVFR does in a game so far, beyond QVFR itself (four views, composed, following the gaze, the
// headset menu and overlay). The launcher greys the rest out, the headset menu leaves it out, and the layer keeps it
// off whatever the ini says.
struct GameFeatures {
    bool foveatedDlss;    // Cheeky Foveated DLSS ([cheeky]) and its frame generation
    bool helmet;          // the helmet overlay ([helmet])
    bool liveResolution;  // resolution changes while the game runs (otherwise: at the next launch)
    bool fovCrop;         // the field of view crop ([crop])
    bool frameGeneration; // Cheeky's frame generation ([cheeky] fg), within Foveated DLSS
};

struct GameInfo {
    const char* id;              // "ace": its folder in src/games, and [launcher] game in the ini
    const wchar_t* name;         // "Assetto Corsa EVO", in the launcher and the headset notices
    const wchar_t* shortName;    // "ACE", in short messages
    const wchar_t* exeName;      // "AssettoCorsaEVO.exe": the process the layer recognises
    const wchar_t* steamFolder;  // its folder in steamapps\common
    const wchar_t* exeFolder;    // exeName's folder under steamFolder ("": steamFolder itself)
    const wchar_t* steamAppId;   // SteamAppId / SteamGameId for a launch from outside Steam
    const wchar_t* launchArgs;   // the command line that starts it in VR ("-vr")
    // A game that reaches OpenXR through OpenComposite: its openvr_api.dll under steamFolder, which the launcher checks
    // is OpenComposite's (docs/acc-opencomposite.md); null for a game with OpenXR of its own.
    const wchar_t* openComposite;
    // The game's DLSS (nvngx_dlss.dll) under steamFolder, whose version the launcher checks with Foveated DLSS on:
    // Cheeky's own DLSS passes need the 310 series (DLSS 4: its presets E, K, L and M). Null: not checked.
    const wchar_t* dlssDll;
    GameFeatures features;
    const char* const* builds;   // the exe SHA-256s (lowercase hex) the game's addresses were taken from
    std::size_t buildCount;
    // The launcher loads the layer into the game before it runs (started suspended): for a game that creates its
    // shaders before it starts OpenXR, so the layer's shader edits reach them (X-Plane). The layer then sets the
    // game up on load (GameAdapter::early_start), not at xrCreateInstance.
    bool earlyLoad = false;
    // The game's own crash reports, under %LOCALAPPDATA%: Unreal's crash reporter writes a folder per crash (the
    // minidump, CrashContext.runtime-xml, the game's log), which Report an issue zips when it falls in a session
    // (src/launcher/report.h). Null: none known.
    const wchar_t* crashReports = nullptr;
};

// The games Sim-QVFR supports, in the launcher's order.
const GameInfo* const* all_games(std::size_t& count);
// The game with this id ("ace", "acc"; any case); null for none.
const GameInfo* game_by_id(const char* id);
// The game the launcher starts: the one chosen (set_launcher_game, from [launcher] game), ACE until then.
const GameInfo& launcher_game();
void set_launcher_game(const GameInfo& game);
// The game whose exe is `fileName` (no folder; any case); null for none.
const GameInfo* game_for_exe(const wchar_t* fileName);
// The game whose exe is the current process's; null for none.
const GameInfo* current_game();
// `sha256` (lowercase hex) is one of the game's builds.
bool supported_build(const GameInfo& game, const std::string& sha256);

}  // namespace simqvfr::games

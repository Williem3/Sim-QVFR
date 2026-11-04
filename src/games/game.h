// Who a game is: what the launcher and the layer need to find, start and recognise it (src/games/README.md).
// Plain data with no Windows, OpenXR or D3D dependency, so the launcher and the layer share it. Each game
// defines its own in src/games/<id>/game.h; games.cpp lists them.
#pragma once

#include <cstddef>
#include <string>

namespace aceqv::games {

struct GameInfo {
    const char* id;              // "ace": its folder in src/games
    const wchar_t* name;         // "Assetto Corsa EVO", in the launcher and the headset notices
    const wchar_t* shortName;    // "ACE", in short messages
    const wchar_t* exeName;      // "AssettoCorsaEVO.exe": the process the layer recognises
    const wchar_t* steamFolder;  // its folder in steamapps\common, where the launcher looks for exeName
    const wchar_t* steamAppId;   // SteamAppId / SteamGameId for a launch from outside Steam
    const wchar_t* launchArgs;   // the command line that starts it in OpenXR VR ("-vr")
    const char* const* builds;   // the exe SHA-256s (lowercase hex) the game's addresses were taken from
    std::size_t buildCount;
};

// The game the launcher starts (one per release for now).
const GameInfo& launcher_game();
// The game whose exe is `fileName` (no folder; any case); null for none.
const GameInfo* game_for_exe(const wchar_t* fileName);
// The game whose exe is the current process's; null for none.
const GameInfo* current_game();
// `sha256` (lowercase hex) is one of the game's builds.
bool supported_build(const GameInfo& game, const std::string& sha256);

}  // namespace aceqv::games

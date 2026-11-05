#include "game.h"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <iterator>

#include "ace/game.h"
#include "acc/game.h"
#include "xplane/game.h"

namespace simqvfr::games {

namespace {

// Every game Sim-QVFR supports. A new game adds its game.h here (src/games/README.md).
constexpr const GameInfo* kGames[] = {&::ace::kGame, &::acc::kGame, &::xplane::kGame};

const GameInfo* chosen = &::ace::kGame;

}  // namespace

const GameInfo* const* all_games(std::size_t& count) {
    count = std::size(kGames);
    return kGames;
}

const GameInfo* game_by_id(const char* id) {
    for (const GameInfo* g : kGames)
        if (id && _stricmp(id, g->id) == 0)
            return g;
    return nullptr;
}

const GameInfo& launcher_game() { return *chosen; }

void set_launcher_game(const GameInfo& game) { chosen = &game; }

const GameInfo* game_for_exe(const wchar_t* fileName) {
    for (const GameInfo* g : kGames)
        if (fileName && _wcsicmp(fileName, g->exeName) == 0)
            return g;
    return nullptr;
}

const GameInfo* current_game() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return nullptr;
    const wchar_t* name = wcsrchr(path, L'\\');
    return game_for_exe(name ? name + 1 : path);
}

bool supported_build(const GameInfo& game, const std::string& sha256) {
    for (std::size_t i = 0; i < game.buildCount; ++i)
        if (sha256 == game.builds[i])
            return true;
    return false;
}

}  // namespace simqvfr::games

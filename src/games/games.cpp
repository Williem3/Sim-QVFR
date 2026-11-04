#include "game.h"

#include <windows.h>

#include <cwchar>

#include "ace/game.h"
#include "acc/game.h"

namespace aceqv::games {

namespace {

// Every game ace-quadview supports. A new game adds its game.h here (src/games/README.md).
constexpr const GameInfo* kGames[] = {&::ace::kGame, &::acc::kGame};

}  // namespace

const GameInfo& launcher_game() { return ::ace::kGame; }

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

}  // namespace aceqv::games

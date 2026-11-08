#include "game.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwchar>
#include <iterator>

#include "ace/game.h"
#include "acc/game.h"
#include "lmu/game.h"
#include "xplane/game.h"

namespace simqvfr::games {

namespace {

// Every game Sim-QVFR supports. A new game adds its game.h here (src/games/README.md).
constexpr const GameInfo* kGames[] = {&::ace::kGame, &::acc::kGame, &::xplane::kGame, &::lmu::kGame};

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

bool anticheat_running(const GameInfo& game, std::wstring& found) {
    found.clear();
    if (!game.antiCheat)
        return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    const DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    PROCESSENTRY32W e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == self)
            parent = e.th32ParentProcessID;
    e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok && found.empty(); ok = Process32NextW(snap, &e))
        for (const wchar_t* const* name = game.antiCheat; *name; ++name)
            if (_wcsicmp(e.szExeFile, *name) == 0) {
                // A bootstrapper that is still up, anywhere, counts too: it may have started this process and its
                // parent link can't be told from a reused PID.
                found = std::wstring(e.szExeFile) + (e.th32ProcessID == parent ? L" (started this game)" : L"");
                break;
            }
    CloseHandle(snap);
    return !found.empty();
}

}  // namespace simqvfr::games

#include "adapter.h"

#include <windows.h>

#include <string>

#include "../log.h"
#include "../probe/local_analysis.h"
#include "identity_adapter.h"

#include "ace/adapter.h"
#include "ace/game.h"
#include "acc/adapter.h"
#include "acc/game.h"
#include "lmu/adapter.h"
#include "lmu/game.h"
#include "xplane/adapter.h"
#include "xplane/game.h"

namespace simqvfr::games {

void IdentityAdapter::instance_created(const wchar_t* ini, const wchar_t*) {
#ifdef SIM_QVFR_LOCAL_ANALYSIS
    if (analysis())
        local_analysis_instance_created(ini);
#else
    (void)ini;
#endif
}

GameAdapter& adapter_for(const GameInfo* game) {
    static IdentityAdapter none(nullptr);
    if (!game)  // the layer's globals, before it knows the game
        return none;
    // A launch the game's anti-cheat protects gets nothing of ours, whatever the game's adapter: the layer only logs
    // (GameInfo::antiCheat). One game per process, so this is decided once.
    static std::wstring antiCheat;
    static const bool protectedLaunch = anticheat_running(*game, antiCheat);
    // The user's choice to run with the anti-cheat ([launcher] anticheat=1, in this launch's ini copy): the launcher
    // started the game through its bootstrapper, and Sim-QVFR runs as in any launch.
    static const bool chosen = [] {
        wchar_t ini[MAX_PATH] = {};
        return GetEnvironmentVariableW(L"SIM_QVFR_INI", ini, MAX_PATH) != 0 && anticheat_chosen(ini);
    }();
    static const bool guarded = protectedLaunch && !(chosen && game->antiCheatStart);
    if (protectedLaunch && !guarded) {
        static const bool logged = (logf("game: %ls's anti-cheat is running (%ls) and [launcher] anticheat=1: Sim-QVFR "
                                         "runs with it, as the user chose (the anti-cheat may kick or ban for it).",
                                         game->name, antiCheat.c_str()),
                                    true);
        (void)logged;
    }
    if (guarded) {
        static IdentityAdapter off(game, 30, false);
        static const bool logged = (logf("game: %ls's anti-cheat is running (%ls): Sim-QVFR stays off in this launch "
                                         "and only logs. Start the game from the Sim-QVFR launcher, offline.",
                                         game->name, antiCheat.c_str()),
                                    true);
        (void)logged;
        return off;
    }
    // A new game adds its adapter here (src/games/README.md).
    if (game == &::ace::kGame)
        return ::ace::adapter();
    if (game == &::acc::kGame)
        return ::acc::adapter();
    if (game == &::xplane::kGame)  // QVFR through the early load (docs/recon-xplane.md)
        return ::xplane::adapter();
    if (game == &::lmu::kGame)  // identity only, with log-only probes (docs/recon-lmu.md)
        return ::lmu::adapter();
    return none;
}

void early_load() {
    const GameInfo* game = current_game();
    if (!game || !game->earlyLoad)
        return;
    wchar_t ini[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"SIM_QVFR_INI", ini, MAX_PATH))
        return;
    adapter_for(game).early_start(ini);
}

}  // namespace simqvfr::games

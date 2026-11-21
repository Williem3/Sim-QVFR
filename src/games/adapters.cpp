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
#include "scssoftware/adapter.h"
#include "scssoftware/ats/game.h"
#include "scssoftware/ets2/game.h"
#include "lmu/game.h"

#include "xplane/adapter.h"
#include "xplane/game.h"

#include "rf2/adapter.h"
#include "rf2/game.h"


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
    if (!game)
        return none;
    static std::wstring antiCheat;
    static const bool protectedLaunch = anticheat_running(*game, antiCheat);
    static const bool chosen = [] {
        wchar_t ini[MAX_PATH] = {};
        return GetEnvironmentVariableW(L"SIM_QVFR_INI", ini, MAX_PATH) != 0 && anticheat_chosen(ini);
    }();
    static const bool guarded = protectedLaunch && !(chosen && game->antiCheatStart);
    if (protectedLaunch && !guarded) {
        static const bool logged = (logf("game: %ls's anti-cheat is running (%ls) and [launcher] anticheat=1: Sim-QVFR "
                                         "runs with it, as the user chose (the anti-cheat may kick or ban for it).",
                                         game->name, antiCheat.c_str()), true);
        (void)logged;
    }
    if (guarded) {
        static IdentityAdapter off(game, 30, false);
        static const bool logged = (logf("game: %ls's anti-cheat is running (%ls): Sim-QVFR stays off in this launch "
                                         "and only logs. Start the game from the Sim-QVFR launcher, offline.",
                                         game->name, antiCheat.c_str()), true);
        (void)logged;
        return off;
    }
    if (game == &::ace::kGame)
        return ::ace::adapter();
    if (game == &::acc::kGame)
        return ::acc::adapter();
    if (game == &::xplane::kGame)
        return ::xplane::adapter();
    if (game == &::lmu::kGame)
        return ::lmu::adapter();
    // rFactor 2: the layer measures and steers; the game renders QVFR itself in its own D3D11 pass.
    if (game == &::rf2::kGame)
        return ::rf2::adapter();
    if (game == &::ats::kGame || game == &::ets2::kGame)  // SCS Software's: QVFR at the D3D11 level
        return ::scs::adapter(game);
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

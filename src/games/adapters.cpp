#include "adapter.h"

#include <windows.h>

#include <string>

#include "../log.h"
#include "../mod_api/mod_loader.h"
#include "../mod_api/plugin_adapter.h"
#include "../probe/local_analysis.h"
#include "identity_adapter.h"

#include "ace/adapter.h"
#include "ace/game.h"
#include "acc/adapter.h"
#include "acc/game.h"
#include "lmu/adapter.h"
#ifndef SIM_QVFR_SCS_AS_MOD
#include "scssoftware/adapter.h"
#endif
#include "scssoftware/ats/game.h"
#include "scssoftware/ets2/game.h"
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

namespace {

#ifndef SIM_QVFR_SCS_AS_MOD
// `game`'s adapter from its mod's entry (mod_api/plugin_adapter.h), kept for the process; a mod the core refuses leaves
// the game known by its identity alone (the layer only logs there), saying why.
GameAdapter& through_mod(const GameInfo* game, SqvfrModEntry entry) {
    std::string why;
    if (std::unique_ptr<mod_api::PluginAdapter> mod = mod_api::PluginAdapter::load(game, entry, why))
        return *mod.release();
    logf("game: %ls's mod refused (%s): Sim-QVFR only logs in this launch.", game->name, why.c_str());
    return *new IdentityAdapter(game);
}
#endif

#ifdef SIM_QVFR_SCS_AS_MOD
// `game`'s adapter from the mod DLL <mods>\<folder>\<dll> (mod_api/mod_loader.h), kept for the process. Not verified
// yet: no signature or approval check before it loads (phase 3 of docs/architecture/mod-manager-plan.md).
GameAdapter& through_mod_dll(const GameInfo* game, const wchar_t* folder, const wchar_t* dll) {
    const std::wstring path = mod_api::mod_dll_path(folder, dll);
    std::string why;
    if (std::unique_ptr<mod_api::PluginAdapter> mod = mod_api::load_mod_dll(path, game, why)) {
        logf("game: %ls's mod loaded from %ls, UNVERIFIED (no signature or approval check yet), ABI %u, its table %u "
             "bytes",
             game->name, path.c_str(), mod->table().abi, mod->table().size);
        return *mod.release();
    }
    logf("game: %ls's mod not loaded (%s): Sim-QVFR only logs in this launch.", game->name, why.c_str());
    return *new IdentityAdapter(game);
}
#endif

}  // namespace

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
    // SCS Software's: QVFR at the D3D11 level, through the game-mod ABI (mod_api.h), one mod for both games
    // (scssoftware/mod_entry.cpp): built in, or with SIM_QVFR_SCS_AS_MOD its own DLL, mods\scs\sim-qvfr-scs.dll.
#ifdef SIM_QVFR_SCS_AS_MOD
    if (game == &::ats::kGame) {
        static GameAdapter& ats = through_mod_dll(game, L"scs", L"sim-qvfr-scs.dll");
        return ats;
    }
    if (game == &::ets2::kGame) {
        static GameAdapter& ets2 = through_mod_dll(game, L"scs", L"sim-qvfr-scs.dll");
        return ets2;
    }
#else
    if (game == &::ats::kGame) {
        static GameAdapter& ats = through_mod(game, &::scs::mod_entry);
        return ats;
    }
    if (game == &::ets2::kGame) {
        static GameAdapter& ets2 = through_mod(game, &::scs::mod_entry);
        return ets2;
    }
#endif
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

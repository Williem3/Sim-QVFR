// The SCS games' mod tables (adapter.h mod_entry): one per game, each its GameAdapter forwarded entry for entry
// (mod_api/adapter_mod.h).
#include "adapter.h"

#include <cstring>

#include "../../mod_api/adapter_mod.h"
#include "ats/game.h"
#include "ets2/game.h"

namespace scs {

const SqvfrMod* mod_entry(const SqvfrHost* host) {
    if (!host || host->abi != SQVFR_ABI_VERSION || !host->gameId)
        return nullptr;
    const bool ets2 = _stricmp(host->gameId, ::ets2::kGame.id) == 0;
    if (!ets2 && _stricmp(host->gameId, ::ats::kGame.id) != 0)
        return nullptr;  // not one of SCS Software's games
    static simqvfr::mod_api::AdapterMod atsMod(adapter(&::ats::kGame)), ets2Mod(adapter(&::ets2::kGame));
    static const SqvfrMod atsTable = sqvfr::mod_table(atsMod, ::ats::kGame.id);
    static const SqvfrMod ets2Table = sqvfr::mod_table(ets2Mod, ::ets2::kGame.id);
    return ets2 ? &ets2Table : &atsTable;
}

}  // namespace scs

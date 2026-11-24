// Assetto Corsa EVO's mod table (adapter.h mod_entry): its GameAdapter forwarded entry for entry
// (mod_api/adapter_mod.h), its focus views (focus_images.h) through the table's focus_images.
#include "adapter.h"

#include <cstring>

#include "../../mod_api/adapter_mod.h"
#include "game.h"

namespace ace {

const SqvfrMod* mod_entry(const SqvfrHost* host) {
    if (!host || host->abi != SQVFR_ABI_VERSION || !host->gameId || _stricmp(host->gameId, kGame.id) != 0)
        return nullptr;
    static simqvfr::mod_api::AdapterMod mod(adapter());
    static const SqvfrMod table = sqvfr::mod_table(mod, kGame.id);
    return &table;
}

}  // namespace ace

// SCS Software's truck simulators (American Truck Simulator, Euro Truck Simulator 2: one engine, Prism3D): their
// adapter (../adapter.h). QVFR runs through the OpenXR and D3D11 layers alone: no game code hooked or called, no game
// memory written (corner.h). The games differ by their identity only (ats/game.h, ets2/game.h).
#pragma once

#include "../../mod_api/mod_api.h"
#include "../adapter.h"

namespace scs {

// The adapter for `game` (one of the two identities).
simqvfr::games::GameAdapter& adapter(const simqvfr::games::GameInfo* game);

// The SCS games as one game mod (mod_api.h; mod_entry.cpp): adapter() behind the C table, the game picked from
// SqvfrHost::gameId ("ats" or "ets2"). Still built into sim-qvfr.dll (games/adapters.cpp); once SCS is its own DLL
// this is its sqvfr_mod_entry export.
const SqvfrMod* mod_entry(const SqvfrHost* host);

}  // namespace scs

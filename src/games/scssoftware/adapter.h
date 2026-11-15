// SCS Software's truck simulators (American Truck Simulator, Euro Truck Simulator 2: one engine, Prism3D): their
// adapter (../adapter.h). QVFR runs through the OpenXR and D3D11 layers alone: no game code hooked or called, no game
// memory written (corner.h). The games differ by their identity only (ats/game.h, ets2/game.h).
#pragma once

#include "../adapter.h"

namespace scs {

// The adapter for `game` (one of the two identities).
simqvfr::games::GameAdapter& adapter(const simqvfr::games::GameInfo* game);

}  // namespace scs

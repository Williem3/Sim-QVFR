// X-Plane 12's adapter (../adapter.h). Identity only: no QVFR yet, so the layer only logs. Its experiments
// (docs/recon-xplane.md) measure the engine for QVFR.
#pragma once

#include "../adapter.h"
#include "../../mod_api/mod_api.h"

namespace xplane {

simqvfr::games::GameAdapter& adapter();

// Its game-mod table (mod_entry.cpp): the core calls it built in, or the mod DLL exports it (SIM_QVFR_XPLANE_AS_MOD).
const SqvfrMod* mod_entry(const SqvfrHost* host);

}  // namespace xplane

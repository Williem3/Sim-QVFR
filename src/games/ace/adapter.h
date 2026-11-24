// Assetto Corsa EVO's adapter (../adapter.h): QVFR is views4.h, the experiments views3.h, double_render.h
// and batch_watch.h.
#pragma once

#include "../adapter.h"
#include "../../mod_api/mod_api.h"

namespace ace {

simqvfr::games::GameAdapter& adapter();

// Its game-mod table (mod_entry.cpp): the core calls it built in, or the mod DLL exports it (SIM_QVFR_ACE_AS_MOD).
const SqvfrMod* mod_entry(const SqvfrHost* host);

}  // namespace ace

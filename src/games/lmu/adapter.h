// Le Mans Ultimate's adapter (../adapter.h): identity only with its probes and gates, or QVFR in development
// ([experiment] lmu_qvfr=1; qvfr_frame.h).
#pragma once

#include "../adapter.h"
#include "../../mod_api/mod_api.h"

namespace lmu {

simqvfr::games::GameAdapter& adapter();

// Its game-mod table (mod_entry.cpp): the core calls it built in, or the mod DLL exports it (SIM_QVFR_LMU_AS_MOD).
const SqvfrMod* mod_entry(const SqvfrHost* host);

}  // namespace lmu

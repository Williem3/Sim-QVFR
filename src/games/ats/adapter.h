// American Truck Simulator's adapter (../adapter.h): identity only, with log-only D3D11 probes (docs/recon-ats.md).
// ATS's QVFR is to come through the OpenXR and D3D11 layers alone: no game code hooked, no game memory written.
#pragma once

#include "../adapter.h"

namespace ats {

simqvfr::games::GameAdapter& adapter();

}  // namespace ats

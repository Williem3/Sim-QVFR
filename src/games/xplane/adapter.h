// X-Plane 12's adapter (../adapter.h). Identity only: no QVFR yet, so the layer only logs. Its experiments
// (docs/recon-xplane.md) measure the engine for QVFR.
#pragma once

#include "../adapter.h"

namespace xplane {

simqvfr::games::GameAdapter& adapter();

}  // namespace xplane

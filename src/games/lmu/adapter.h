// Le Mans Ultimate's adapter (../adapter.h): identity only with its probes and gates, or QVFR in development
// ([experiment] lmu_qvfr=1; qvfr_frame.h).
#pragma once

#include "../adapter.h"

namespace lmu {

simqvfr::games::GameAdapter& adapter();

}  // namespace lmu

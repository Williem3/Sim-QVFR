// Analysis tools built only from a local folder (CMake's SIM_QVFR_LOCAL_DIR, which defines SIM_QVFR_LOCAL_ANALYSIS):
// the developer's own, not in this repository and never in a release (package.ps1 refuses them).
#pragma once

namespace simqvfr {

// A game's first OpenXR instance, while the game is analysed (IdentityAdapter::analysis); `ini`: this launch's ini.
void local_analysis_instance_created(const wchar_t* ini);

}  // namespace simqvfr

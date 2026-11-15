// A development build or a release (alpha, beta, rc, final). CMake defines SIM_QVFR_DEV_BUILD 1 while SIM_QVFR_VERSION is
// "dev" (every local build) and 0 when package.ps1 sets a release's version. Diagnostic hotkeys and switches exist in
// development builds only: a release has none of them.
#pragma once

#ifndef SIM_QVFR_DEV_BUILD  // fails closed: a target built without CMake's definition is neither (CMakeLists.txt)
#error "SIM_QVFR_DEV_BUILD is not defined: CMakeLists.txt defines it for every target (1 for a dev build, 0 for a release)"
#endif

namespace simqvfr {

inline constexpr bool kDevBuild = SIM_QVFR_DEV_BUILD != 0;

}  // namespace simqvfr

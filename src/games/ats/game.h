// American Truck Simulator's identity (../game.h). Known by its identity alone so far: the layer only logs there
// (docs/recon-ats.md). Its VR renders with D3D11 and reaches OpenXR on its own (a Khronos loader linked into the exe).
// Its QVFR is to come through the OpenXR and D3D11 layers alone: no game code hooked, no game memory written.
#pragma once

#include <iterator>

#include "../game.h"

namespace ats {

// 1.62.2.2 (file version), Steam build.
inline constexpr const char* kBuilds[] = {"63ecae85a070934530d9fd971d4d56e5479f1214d7077b403f79b8981c613e08"};

inline constexpr simqvfr::games::GameInfo kGame = {
    "ats", L"American Truck Simulator", L"ATS", L"amtrucks.exe", L"American Truck Simulator", L"bin\\win_x64", L"270880",
    // Its own OpenXR; VR has a D3D11 binding only. The default branch turns VR off without -experimental_vr ("Experimental
    // VR is not supported in this build by default", game.log.txt).
    L"-openxr -experimental_vr -rdevice dx11",
    nullptr,                   // OpenXR of its own
    nullptr,                   // no DLSS
    {false, false, false, false, false, false},
    kBuilds, std::size(kBuilds),
};

}  // namespace ats

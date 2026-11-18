// American Truck Simulator's identity (../../game.h). Its VR renders with D3D11 and reaches OpenXR on its own (a Khronos
// loader linked into the exe). Its QVFR runs through the OpenXR and D3D11 layers alone (../corner.h): no game code
// hooked, no game memory written.
#pragma once

#include <iterator>

#include "../../game.h"

namespace ats {

// 1.62.2.2 (file version), Steam build.
inline constexpr const char* kBuilds[] = {"63ecae85a070934530d9fd971d4d56e5479f1214d7077b403f79b8981c613e08"};

inline constexpr simqvfr::games::GameInfo kGame = {
    "ats", L"American Truck Simulator", L"ATS", L"amtrucks.exe", L"American Truck Simulator", L"bin\\win_x64", L"270880",
    // Its own OpenXR; VR has a D3D11 binding only. The default branch turns VR off without -experimental_vr ("Experimental
    // VR is not supported in this build by default", game.log.txt).
    L"-openxr -experimental_vr -rdevice dx11 -nointro",  // -nointro: straight to the menu, no logo videos
    nullptr,                   // OpenXR of its own
    nullptr,                   // no DLSS
    // QVFR through the D3D11 layer, the focus view by multi-view rendering (layerMultiView); nothing else yet.
    {false, false, false, false, false, false, false, false, false, false, true},
    kBuilds, std::size(kBuilds),
    false,                     // no early load
    nullptr, nullptr, nullptr, // no crash reporter of its own, no anti-cheat
    {40, 150, 25, 25},         // a fresh install: periphery 40%, focus density 150%, focus box 25% x 25%
};

}  // namespace ats

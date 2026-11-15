// Euro Truck Simulator 2's identity (../../game.h). SCS Software's engine, as American Truck Simulator's
// (../ats/game.h): its VR renders with D3D11 and reaches OpenXR on its own, and its QVFR runs through the OpenXR and
// D3D11 layers alone (../corner.h): no game code hooked, no game memory written.
#pragma once

#include <iterator>

#include "../../game.h"

namespace ets2 {

// 1.62.3.3 (file version), Steam build.
inline constexpr const char* kBuilds[] = {"055ec5343d53bd427de54b06eca74937f0d348dbb0c5d71862edddcf45e2db66"};

inline constexpr simqvfr::games::GameInfo kGame = {
    "ets2", L"Euro Truck Simulator 2", L"ETS2", L"eurotrucks2.exe", L"Euro Truck Simulator 2", L"bin\\win_x64", L"227300",
    // Its own OpenXR; VR has a D3D11 binding only. The default branch turns VR off without -experimental_vr.
    L"-openxr -experimental_vr -rdevice dx11",
    nullptr,                   // OpenXR of its own
    nullptr,                   // no DLSS
    // QVFR through the D3D11 layer, the focus view by multi-view rendering (layerMultiView); nothing else yet.
    {false, false, false, false, false, false, false, false, false, false, true},
    kBuilds, std::size(kBuilds),
    false,                     // no early load
    nullptr, nullptr, nullptr, // no crash reporter of its own, no anti-cheat
    {40, 150, 25, 25},         // a fresh install: periphery 40%, focus density 150%, focus box 25% x 25%
};

}  // namespace ets2

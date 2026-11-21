// rFactor 2's identity (../game.h). Experimental support: the layer recognises the game,
// starts it through OpenComposite/OpenXR and runs log-only D3D11 probes. No QVFR yet.
#pragma once
#include <iterator>
#include "../game.h"


namespace rf2 {

inline constexpr const char* kBuilds[] = {
    "cf5a0e640befce57407c1f086e7635b45fcbf9c19af3b90f694384ffaa463048"
};

inline constexpr simqvfr::games::GameInfo kGame = {
    "rf2",
    L"rFactor 2",
    L"rF2",
    L"rFactor2.exe",
    L"rFactor 2",
    L"Bin64",
    L"365960",
    L"+path=\"..\" +VR",
    L"Bin64\\openvr_api.dll",
    nullptr,

    // Identity/probe stage only. Do not expose launcher controls for features that
    // the RF2 adapter does not implement yet. By name: GameFeatures has more members than it had.
    {.gpuRegulator = true},

    kBuilds,
    std::size(kBuilds),
    false,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace rf2

// rFactor 2's identity (../game.h): started through OpenComposite in VR; QVFR at the D3D11 level (adapter.cpp), no game
// code hooked or called, so it runs on any build.
#pragma once

#include <iterator>

#include "../game.h"

namespace rf2 {

inline constexpr const char* kBuilds[] = {
    "cf5a0e640befce57407c1f086e7635b45fcbf9c19af3b90f694384ffaa463048",
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
    // The launcher offers what the adapter has: the GPU regulator (no crop, helmet, focus shape or eye tracking yet).
    {.gpuRegulator = true},
    kBuilds,
    std::size(kBuilds),
    false,    // earlyLoad
    nullptr,  // crashReports
    nullptr,  // antiCheat
    nullptr,  // antiCheatStart
    {},       // defaults
    true,     // anyBuild: nothing of the game's code is used
    simqvfr::games::QvfrImage::DoubleWide,
    true,  // quietGameOutput: its OpenXR loader and OpenComposite write tens of MB to stdout in a few minutes
};

}  // namespace rf2

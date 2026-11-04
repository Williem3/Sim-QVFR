// Assetto Corsa Competizione's identity (../game.h). Identity only for now: the layer logs in ACC and
// changes nothing (adapters.cpp, README.md). VR reaches OpenXR through OpenComposite.
#pragma once

#include <iterator>

#include "../game.h"

namespace acc {

// 1.10.4, Steam build 21257365.
inline constexpr const char* kBuilds[] = {"2b2679c269ad710f61ac5c0fe84439799f770c79535378bc98983268c1f937e0"};

inline constexpr aceqv::games::GameInfo kGame = {
    "acc", L"Assetto Corsa Competizione", L"ACC", L"AC2-Win64-Shipping.exe", L"Assetto Corsa Competizione",
    L"805550", L"-vr hmd=SteamVR", kBuilds, std::size(kBuilds),
};

}  // namespace acc

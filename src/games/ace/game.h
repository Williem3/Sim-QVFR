// Assetto Corsa EVO's identity (../game.h). Its addresses are in abi.h, taken from the build below.
#pragma once

#include <iterator>

#include "../game.h"

namespace ace {

inline constexpr const char* kBuilds[] = {"ab11fc2a35ef9c2c61c6e0bcbdeb755b0d7787d101c3fdebfee12df3eadedbcb"};

inline constexpr aceqv::games::GameInfo kGame = {
    "ace", L"Assetto Corsa EVO", L"ACE", L"AssettoCorsaEVO.exe", L"Assetto Corsa EVO", L"", L"3058630", L"-vr",
    nullptr,                   // OpenXR of its own
    L"nvngx_dlss.dll",         // its DLSS (310.9.1 in this build)
    {true, true, true, true},  // everything
    kBuilds, std::size(kBuilds),
};

}  // namespace ace

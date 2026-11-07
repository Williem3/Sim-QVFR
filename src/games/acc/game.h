// Assetto Corsa Competizione's identity (../game.h): QVFR through its adapter (adapter.cpp, docs/recon-acc.md). VR
// reaches OpenXR through OpenComposite.
#pragma once

#include <iterator>

#include "../game.h"

namespace acc {

// 1.10.4, Steam build 21257365.
inline constexpr const char* kBuilds[] = {"2b2679c269ad710f61ac5c0fe84439799f770c79535378bc98983268c1f937e0"};

inline constexpr simqvfr::games::GameInfo kGame = {
    "acc", L"Assetto Corsa Competizione", L"ACC", L"AC2-Win64-Shipping.exe", L"Assetto Corsa Competizione",
    L"AC2\\Binaries\\Win64", L"805550", L"-vr hmd=SteamVR",
    L"Engine\\Binaries\\ThirdParty\\OpenVR\\OpenVRv1_5_17\\Win64\\openvr_api.dll",  // VR through OpenComposite
    L"AC2\\Plugins\\DLSS\\Binaries\\ThirdParty\\Win64\\nvngx_dlss.dll",  // DLSS 2.2.15 as shipped: too old for Cheeky
    // QVFR, gaze, the headset menu and overlay, the FOV crop, the helmet, live resolution, Foveated DLSS (Cheeky's
    // runtime on the session's D3D11 device; its quad roles need a Cheeky build that registers D3D11 game views).
    // Not frame generation: Cheeky's layer can't capture our D3D11 frames for it (native passthrough), so ACC was
    // never paced to the generated rate (docs/recon-acc.md).
    {true, true, true, true, false, false},
    kBuilds, std::size(kBuilds),
    false,
    L"AC2\\Saved\\Crashes",  // UE4CC-Windows-<id>_0000\: UE4Minidump.dmp, CrashContext.runtime-xml, AC2.log
};

}  // namespace acc

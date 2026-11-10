// X-Plane 12's identity (../game.h); QVFR in xplane/adapter.cpp (docs/recon-xplane.md). It renders with Vulkan and
// reaches OpenXR on its own (no OpenComposite).
#pragma once

#include <iterator>

#include "../game.h"

namespace xplane {

// 12.4.3-r2 (15ff1e4d), Steam build 23899835.
inline constexpr const char* kBuilds[] = {"2ca099b7c6b218e78d02bdbe298be2aebf670b67c5adff827449b065e1107936"};

inline constexpr simqvfr::games::GameInfo kGame = {
    "xplane", L"X-Plane 12", L"X-Plane", L"X-Plane.exe", L"X-Plane 12", L"", L"2014780",
    // OpenXR, its frames handed over in D3D11 swapchains (vr_hardware_openxr_d3d11), which our D3D11 compositor
    // can work on; --native_vr instead submits Vulkan swapchains (docs/recon-xplane.md).
    L"--open_xr --d3d11_vr",
    nullptr,  // OpenXR of its own
    nullptr,  // no DLSS
    // Live resolution and object detail; the adapter decides it per run (per-view sizes, qvfr_live_resolution). And
    // sharpening (the D3D11 compositor's CAS), on top of X-Plane's own FSR sharpening pass if its settings run one.
    {false, false, true, false, false, true, false, false, true},
    kBuilds, std::size(kBuilds),
    true,  // early load: X-Plane creates its shader modules at startup, before OpenXR (docs/recon-xplane.md)
};

}  // namespace xplane

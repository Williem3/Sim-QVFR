// The NVIDIA GPUs in this machine, asked of the driver (nvapi64.dll, through nvapi_QueryInterface) without a device:
// the launcher's check for NVIDIA multi-view rendering and the layer's, before the game makes its device. No D3D
// dependency (the launcher has none). Also NVAPI's loader for nvapi_stereo.cpp.
//
// Multi-view rendering draws each focus view in its eye's pass with single pass stereo XYZW (nvapi_stereo.h), which
// Turing (architecture 0x160, the RTX 20 series) and newer have. Offered on RTX cards only: Turing's GTX 16 series
// (TU116, TU117) is left out.
#pragma once

#include <string>
#include <vector>

namespace simqvfr::nvapi {

struct Gpu {
    std::string name;           // "NVIDIA GeForce RTX 5070 Ti"
    unsigned architecture = 0;    // NV_GPU_ARCHITECTURE_ID (0x160 Turing, 0x170 Ampere, 0x190 Ada, 0x1B0 Blackwell)
    unsigned implementation = 0;  // NV_GPU_ARCH_IMPLEMENTATION_ID (Turing: 7 TU117, 8 TU116)
    bool twoViews = false;        // RTX 20 series or newer: two full views per draw
};

// Every NVIDIA GPU the driver reports; empty without an NVIDIA driver. Asked once, then kept.
const std::vector<Gpu>& gpus();
// The first GPU that draws two full views per draw, or null.
const Gpu* two_views_gpu();
// Turing (RTX 20) or newer, from the architecture alone (Tegra's 0xE00000xx ids aren't).
bool has_two_views(unsigned architecture);
// Multi-view rendering offered on this GPU: Turing or newer and not a GTX (the GTX 16 series: TU116, TU117, or named
// GTX, as a GTX 1650 on a TU106).
bool offers_multi_view(unsigned architecture, unsigned implementation, const std::string& name);

// The driver's NVAPI function `name` ("NvAPI_D3D_SetSinglePassStereoMode"), NvAPI_Initialize called once before: null
// without an NVIDIA driver, if it didn't initialize or hasn't the function. `status`: NvAPI_Initialize's NvAPI_Status
// (NVAPI_LIBRARY_NOT_FOUND without nvapi64.dll).
void* function(const char* name, int* status = nullptr);

}  // namespace simqvfr::nvapi

// NVIDIA single-pass stereo through NVAPI (third_party/nvapi headers; the functions come from nvapi64.dll, which every
// NVIDIA driver installs, through nvapi_QueryInterface: nothing for players to install, nothing linked).
//
// Two views per draw: a vertex shader converted by dxbc::add_second_view writes the second view's clip position
// (NV_XYZW_RIGHT) and the viewport mask (low 16 bits view 0's viewports, high 16 bits view 1's); set_two_views turns
// the mode on for a context with the independent viewport mask, so view 0 lands in viewport 0 and view 1 in viewport 1
// of the same target. Needs a GPU with single pass stereo XYZW (Turing and newer: Support::xyzw).
#pragma once

#include <d3d11.h>

#include "engine/dxbc_patch.h"

namespace simqvfr::nvapi {

struct Support {
    bool loaded = false;            // nvapi64.dll found and NvAPI_Initialize succeeded
    bool singlePassStereo = false;  // two views, the second differing in x only (Pascal and newer)
    bool xyzw = false;              // two views, the second a full position of its own (what add_second_view needs)
    bool multiView = false;         // up to four views (Turing and newer)
    int status = 0;                 // the last NvAPI_Status (0: NVAPI_OK)
};

Support query(ID3D11Device* device);

// The converted shader, created with NVAPI's custom semantics for its second position and mask. NvAPI_Status (0: OK).
int create_second_view_shader(ID3D11Device* device, const dxbc::SecondView& converted, ID3D11VertexShader** shader);

// Two views per draw in `context` from now on (on), or one again (off). `independentMasks`: each view's viewport mask
// from the shader; `targetOffset`: view 1 renders into the render target array slice that much further. NvAPI_Status.
int set_two_views(ID3D11DeviceContext* context, bool on, bool independentMasks = true, unsigned targetOffset = 0);

const char* status_name(int status);

}  // namespace simqvfr::nvapi

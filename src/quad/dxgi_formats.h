// DXGI format helpers both compositors share (compositor.cpp, d3d11_compositor.cpp).
#pragma once

#include <dxgiformat.h>

namespace simqvfr {

// The typeless colour formats the compositors' images may come in: their views take the compositor's view format.
inline bool is_typeless_colour(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return true;
    default:
        return false;
    }
}

}  // namespace simqvfr

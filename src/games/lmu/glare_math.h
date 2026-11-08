// LMU QVFR's glare share (glare_share.h): YEBIS's glare result as a D3D11 texture and region, and a focus view's mapping
// onto its eye's. Header-only, for tests/lmu_glare_test.cpp.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cmath>
#include <cstdint>

#include "../../quad/d3d11_view_resample.h"
#include "abi.h"
#include "focus_math.h"

namespace lmu {

// The top-left region of a YEBIS render texture that a pass uses: its size times the viewport scale plus the offset
// (YEBIS's SetRenderSceneViewportScale; LMU's render scale), rounded, at least a texel and at most the texture.
inline simqvfr::ViewRegion yebis_region(unsigned width, unsigned height, const float (&scaleOffset)[4]) {
    const auto axis = [](unsigned size, float scale, float offset) {
        const double v = std::isfinite(scale) && std::isfinite(offset) ? size * static_cast<double>(scale) + offset : size;
        const double r = std::floor(v + 0.5);
        return static_cast<int>(r < 1 ? 1 : r > size ? size : r);
    };
    return {0, 0, axis(width, scaleOffset[0], scaleOffset[2]), axis(height, scaleOffset[1], scaleOffset[3])};
}

// A focus view's glare from its eye's: each focus texel takes the eye's glare in the same direction. The focus projection
// is the eye's with a narrower window (focus_math.h), so no turn. FocusBox holds tangents left, right, down, up;
// ViewMapping left, right, up, down.
inline simqvfr::ViewMapping glare_mapping(const FocusBox& box) {
    simqvfr::ViewMapping m{};
    const float* f = box.focusTan;
    const float* e = box.eyeTan;
    const float focus[4] = {f[0], f[1], f[3], f[2]}, eye[4] = {e[0], e[1], e[3], e[2]};
    for (int i = 0; i < 4; ++i) {
        m.focusTan[i] = focus[i];
        m.eyeTan[i] = eye[i];
    }
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i)
        m.rotation[i] = identity[i];
    return m;
}

// CRenderGlare's result after its composite (abi.h [Glare share]): the texture (AddRef'd), its size and viewport scale.
struct YebisGlare {
    ID3D11Texture2D* texture = nullptr;
    unsigned width = 0, height = 0;
    float scaleOffset[4] = {};
};

// Reads it under SEH (no C++ objects with destructors here: __try needs a plain frame). 0: read; 1: a pointer on the way
// is null; 2: unreadable (an access violation); 3: the resource isn't a 2D texture of the size the render texture says.
inline int read_yebis_glare(const void* renderGlare, YebisGlare& out) {
    out = YebisGlare{};
    __try {
        const auto* glare = static_cast<const std::uint8_t*>(renderGlare);
        const auto* surface = glare ? *reinterpret_cast<const std::uint8_t* const*>(glare + kGlareResult) : nullptr;
        const auto* gpu = surface ? *reinterpret_cast<const std::uint8_t* const*>(surface + kRenderTextureGpu) : nullptr;
        auto* resource = gpu ? *reinterpret_cast<ID3D11Resource* const*>(gpu + kGpuTextureResource) : nullptr;
        if (!resource)
            return 1;
        out.width = *reinterpret_cast<const unsigned*>(surface + kRenderTextureSize);
        out.height = *reinterpret_cast<const unsigned*>(surface + kRenderTextureSize + 4);
        for (int i = 0; i < 4; ++i)
            out.scaleOffset[i] = reinterpret_cast<const float*>(surface + kRenderTextureScale)[i];
        ID3D11Texture2D* texture = nullptr;
        if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) || !texture)
            return 3;
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        if (d.Width != out.width || d.Height != out.height || d.SampleDesc.Count != 1) {
            texture->Release();
            return 3;
        }
        out.texture = texture;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.texture = nullptr;
        return 2;
    }
}

}  // namespace lmu

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

// The top-left region of a YEBIS render texture that a pass uses. Its UV scale and offset (+0x90: UV per texel of its
// size, 1 / width at full size) give the UV extent YEBIS samples, size x scale + offset (1: all of it; LMU's render
// scale below its launch value: less); the region is that extent of the D3D11 texture (`textureWidth`, `textureHeight`),
// rounded, at least a texel and at most the texture. A non-finite extent counts as the whole texture.
inline simqvfr::ViewRegion yebis_region(unsigned textureWidth, unsigned textureHeight, unsigned width, unsigned height,
                                        const float (&scaleOffset)[4]) {
    const auto axis = [](unsigned texture, unsigned size, float scale, float offset) {
        const double uv = size * static_cast<double>(scale) + offset;
        const double v = std::isfinite(uv) ? uv * texture : texture;
        const double r = std::floor(v + 0.5);
        return static_cast<int>(r < 1 ? 1 : r > texture ? texture : r);
    };
    return {0, 0, axis(textureWidth, width, scaleOffset[0], scaleOffset[2]),
            axis(textureHeight, height, scaleOffset[1], scaleOffset[3])};
}

// The UV extent itself (for the log).
inline double yebis_uv_extent(unsigned size, float scale, float offset) { return size * static_cast<double>(scale) + offset; }

// The region of the glare texture that holds a pass's view. YEBIS builds the glare from the scene target at the same UV
// as its tonemap samples it, so with LMU's live resolution (a pass rendering the top-left `shareX` x `shareY` of its
// target) the view's glare is that share of the region YEBIS uses (whose UV extent is 1 in LMU). Run 2026-11-08 10:52:
// focus passes at 0.938 of their target, the glare stretched over the whole texture showed a light's halo 21 eye px
// below the light, as this predicts (24). A share <= 0 (not known yet) is no region (width 0).
// The same holds for MVR's side-by-side passes (yebis_covers): the share is the pass's own width and height over its
// target's, not YEBIS's one scale.
inline simqvfr::ViewRegion yebis_pass_region(const simqvfr::ViewRegion& yebisRegion, double shareX, double shareY) {
    if (!(shareX > 0) || !(shareY > 0))
        return {0, 0, 0, 0};
    const auto axis = [](int size, double share) {
        const double r = std::floor(size * (share > 1 ? 1.0 : share) + 0.5);
        return static_cast<int>(r < 1 ? 1 : r);
    };
    return {0, 0, axis(yebisRegion.width, shareX), axis(yebisRegion.height, shareY)};
}

// Whether YEBIS post-processed all of a pass's view. It runs on the top-left of its targets at LMU's render scale on
// both axes (`scale`: the render scale over the one the targets were made at), whatever the pass rendered per axis: MVR's
// focus passes render 1514x1266 of 3228x1266 targets (0.469 x 1.0) at scale 1.0, their eye passes 0.5 x 0.5 at 0.5.
// The view's glare is still where its pixels are (yebis_pass_region with the per-axis share), as YEBIS samples the glare
// at the scene's UV; a view reaching past the processed square would read glare YEBIS didn't make this pass. Rendered
// sizes round up (focus_math.h scaled_size): a pixel over is allowed.
inline bool yebis_covers(unsigned passW, unsigned passH, unsigned viewW, unsigned viewH, double scale) {
    if (!(scale > 0) || viewW == 0 || viewH == 0)
        return false;
    return passW <= std::ceil(viewW * scale) + 1 && passH <= std::ceil(viewH * scale) + 1;
}

// A region a pass can plausibly use: LMU's live resolution goes down to 10% of the launch size; anything under 5% of the
// texture per axis is a misread layout, and sharing into it would change nothing that shows.
inline bool yebis_region_plausible(const simqvfr::ViewRegion& r, unsigned textureWidth, unsigned textureHeight) {
    return r.width * 20 >= static_cast<int>(textureWidth) && r.height * 20 >= static_cast<int>(textureHeight);
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

// CRenderGlare's result after its composite (abi.h [Glare share]): the texture (AddRef'd) and its size, the render
// texture's own size and UV scale and offset.
struct YebisGlare {
    ID3D11Texture2D* texture = nullptr;
    unsigned textureWidth = 0, textureHeight = 0;
    unsigned width = 0, height = 0;
    float scaleOffset[4] = {};
};

// Reads it under SEH (no C++ objects with destructors here: __try needs a plain frame). 0: read; 1: a pointer on the way
// is null; 2: unreadable (an access violation); 3: the resource isn't a 2D texture at least the size the render texture
// says.
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
        if (d.Width < out.width || d.Height < out.height || out.width == 0 || out.height == 0 || d.SampleDesc.Count != 1) {
            texture->Release();
            return 3;
        }
        out.texture = texture;
        out.textureWidth = d.Width;
        out.textureHeight = d.Height;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.texture = nullptr;
        return 2;
    }
}

}  // namespace lmu

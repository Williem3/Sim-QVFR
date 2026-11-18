// LMU's focus projection (docs/recon-lmu.md "Design notes"): the shared off-axis focus maths (quad/offaxis_focus.h; its
// projection slots hold that shape), and LMU's own size rounding. Header-only, for tests/lmu_focus_test.cpp.
#pragma once

#include <cmath>

#include "../../quad/offaxis_focus.h"

namespace lmu {

using simqvfr::clamp_focus_centre;
using simqvfr::clip_layout;
using simqvfr::clip_remap;
using simqvfr::ClipRemap;
using simqvfr::focus_projection;
using simqvfr::FocusBox;
using simqvfr::projection_shape;
using simqvfr::remap_clip;

// LMU's size for a scale: ceil(recommended x scale), as cubeDX11_GetRenderSize rounds (a float product).
inline unsigned scaled_size(unsigned recommended, float scale) {
    const float x = static_cast<float>(recommended) * scale;
    const auto i = static_cast<unsigned>(x);
    return static_cast<float>(i) == x ? i : i + 1;
}

// An eye's projection in the slots' shape (offaxis_focus.h projection_shape) from the FOV the game was handed, as
// tangents left, right, down, up: what the focus maths read of the game's slot (m[0], m[5], m[8], m[9]), from OpenXR
// instead of the game's memory, so it holds on any build. Depth (m[10], m[14]) isn't the game's: nothing here reads it.
// False (`m` left as it was): not a FOV (an empty or inverted span).
inline bool projection_from_tangents(const float (&t)[4], float (&m)[16]) {
    const double w = static_cast<double>(t[1]) - t[0], h = static_cast<double>(t[3]) - t[2];
    if (!(w > 1e-4) || !(h > 1e-4) || !std::isfinite(w) || !std::isfinite(h))
        return false;
    for (float& x : m)
        x = 0;
    m[0] = static_cast<float>(2 / w);
    m[8] = static_cast<float>(-(static_cast<double>(t[1]) + t[0]) / w);
    m[5] = static_cast<float>(2 / h);
    m[9] = static_cast<float>(-(static_cast<double>(t[3]) + t[2]) / h);
    m[10] = 0;     // (reverse depth, far at infinity: a placeholder, see above)
    m[11] = 1.0f;  // w = z
    m[14] = 0.05f;
    return true;
}

// The game's render scale from its main targets' size and the recommended size, for a build whose scale can't be read:
// the game's setting is a whole percent times 0.01, so the percent whose scaled_size gives both sizes (as the game
// rounds them); failing that the width's ratio. 0: no sizes yet.
inline float render_scale_from_sizes(unsigned mainW, unsigned mainH, unsigned recommendedW, unsigned recommendedH) {
    if (!mainW || !mainH || !recommendedW || !recommendedH)
        return 0;
    const double ratio = static_cast<double>(mainW) / recommendedW;
    const int percent = static_cast<int>(std::lround(ratio * 100));
    for (int p = percent - 1; p <= percent + 1; ++p) {
        if (p <= 0)
            continue;
        const float candidates[2] = {static_cast<float>(p) * 0.01f, static_cast<float>(p) / 100.0f};
        for (float s : candidates)
            if (scaled_size(recommendedW, s) == mainW && scaled_size(recommendedH, s) == mainH)
                return s;
    }
    return static_cast<float>(ratio);
}

// Where a view lands in its eye target (docs/recon-lmu.md "FSR and the game's render scale"). The scene, the MSAA
// resolve and YEBIS run in the main targets, made at scaled_size(recommended, the game's render scale); the eye targets
// are the recommended size. The last step of a view's post-processing stretches the whole main target over the whole
// eye target: the final blit (a quad the recommended size, UV 0-1) or, with FSR on, FSR 1's upscale (EASU from the whole
// source texture to the whole $PostUpscaleTarget, then RCAS 1:1 into the eye target). So a view rendered into the
// top-left `rendered` texels of a `main`-wide target covers rendered x recommended / main texels of the eye target:
// itself at a render scale of 1 (main = recommended), more below it (FSR's Ultra Quality 77%: 1.3 times).
inline double final_extent(unsigned rendered, unsigned main, unsigned recommended) {
    if (main == 0 || recommended == 0 || main == recommended)
        return rendered;
    return static_cast<double>(rendered) * recommended / main;
}

// The eye target's texels that hold an extent: rounded up (the last one blends a texel past the view in), at least one
// and at most `limit` (0: no limit).
inline unsigned final_texels(double extent, unsigned limit) {
    const double up = std::ceil(extent - 1e-6);
    unsigned n = up < 1 ? 1u : static_cast<unsigned>(up);
    if (limit && n > limit)
        n = limit;
    return n;
}

}  // namespace lmu

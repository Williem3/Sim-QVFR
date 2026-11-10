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

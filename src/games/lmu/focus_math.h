// LMU's focus projection (docs/recon-lmu.md "Design notes"): the shared off-axis focus maths (quad/offaxis_focus.h; its
// projection slots hold that shape), and LMU's own size rounding. Header-only, for tests/lmu_focus_test.cpp.
#pragma once

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

}  // namespace lmu

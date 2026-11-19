// views4's helpers shared by its D3D12 composition (views4.cpp) and its D3D11 one (views4_d3d11.cpp).
#pragma once

#include <atomic>
#include <cstring>

#include "games/adapter.h"
#include "quad/compositor.h"
#include "quad/focus_shape.h"
#include "quad/focus_turn.h"
#include "quad/views4_layout.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

// A frame record for a frame the game recorded nothing for (before its first, or its projection unknown): the
// eyes' FOVs, the focus box centred on each, unturned.
inline Views4Frame views4_unrecorded_frame(const XrFovf (&fov)[2]) {
    Views4Frame frame{};
    for (int e = 0; e < 2; ++e) {
        fov_tangents(fov[e], frame.outerTan[e]);
        double hw, hh;
        focus_extents(frame.outerTan[e], experiment.box_x(), experiment.box_y(), hw, hh);
        const float focusTan[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                   static_cast<float>(-hh)};
        std::memcpy(frame.focusTan[e], focusTan, sizeof(focusTan));
        frame.turn[e] = {0, 0, 0, 1};
    }
    return frame;
}

// Where view `view` sits in the game's image: its rect in the game's target (targetW x targetH, at `origin` in a
// texture of textureW x textureH), as a texture region (u0, v0, du, dv).
inline void views4_source(const Views4Layout& layout, int view, bool eyeColumns, int targetW, int targetH,
                          XrOffset2Di origin, int textureW, int textureH, float (&uv)[4]) {
    const ViewRect r = eyeColumns ? views4_rect_eye_columns(layout, view, targetW, targetH)
                                  : views4_rect(layout, view, targetW, targetH);
    uv[0] = static_cast<float>(origin.x + r.left) / static_cast<float>(textureW);
    uv[1] = static_cast<float>(origin.y + r.top) / static_cast<float>(textureH);
    uv[2] = static_cast<float>(r.width) / static_cast<float>(textureW);
    uv[3] = static_cast<float>(r.height) / static_cast<float>(textureH);
}

// One eye's composition, for either compositor: what its views were rendered with (the frame's record: outer and
// focus tangents, the focus turn), where they sit in the game's image (`source`: the outer view e, the focus view
// e + 2), and the look from the live settings (feather, outline, corners, shape, sharpening).
template <class Source>
void views4_eye(const Views4Frame& frame, int e, const Source& source, EyeComposition& c) {
    std::memcpy(c.outerTan, frame.outerTan[e], sizeof(c.outerTan));
    std::memcpy(c.focusTan, frame.focusTan[e], sizeof(c.focusTan));
    // The focus camera is the eye's turned by turn[e] (in the eye's frame): a direction in the outer view's
    // frame is in the focus view's frame after the inverse turn.
    to_matrix(qconj(frame.turn[e]), c.rotation);
    source(e, c.outerSource);
    source(e + 2, c.focusSource);
    c.feather = static_cast<float>(experiment.feather);
    // The outline (debug_border) is drawn by the compositor along the focus area's exact edge, turned and
    // rounded as it is.
    c.border = experiment.border ? 0.008f : 0.0f;
    std::memcpy(c.borderColor, experiment.borderColor, sizeof(c.borderColor));
    c.cornerRadius = static_cast<float>(experiment.cornerRadius);
    focus_shape_insets(experiment.focusShape.load(std::memory_order_relaxed), c.cornerInset);
    c.sharpness = static_cast<float>(experiment.sharpen.load(std::memory_order_relaxed)) / 100.0f;
    color_grade_constants(color_grade_from_bits(experiment.colorGrade.load(std::memory_order_relaxed)), c.color);
}

}  // namespace simqvfr::layer

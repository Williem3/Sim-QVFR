// Native quad views (layer/native_quad.cpp): a focus view whose box sits off its own axis (LMU renders each focus view
// as an off-centre crop of its eye's image plane, following the gaze) re-expressed as one centred on its axis: the view
// turned towards the box's middle, its FOV symmetric about that and just covering the box. Pimax's runtime corrected
// off-centre views 2-3 for head movement as if they were centred, so the focus area seemed to turn with the head while
// the gaze held it off-centre, and not in fixed mode, where the box is centred (2026-11-09).
#pragma once

#include <algorithm>
#include <cmath>

#include "focus_turn.h"

namespace simqvfr {

struct CentredFocus {
    XrQuaternionf turn{0, 0, 0, 1};  // from the box's frame to the centred view's (its forward axis through the box)
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // to_matrix(turn): a direction in the centred view's frame into the box's
    float tan[4] = {};  // the centred view's FOV: left, right, up, down tangents (symmetric)
    float scaleX = 1, scaleY = 1;  // its tangent width and height over the box's (the image grows by these, same density)
};

// `box`: the focus box in its own frame as tangents left, right, up, down (Views4Frame::focusTan's order).
inline CentredFocus centred_focus(const float (&box)[4]) {
    CentredFocus c;
    const double l = box[0], r = box[1], u = box[2], d = box[3];
    const double cx = 0.5 * (l + r), cy = 0.5 * (u + d);
    // Yaw about +Y, then pitch about the yawed +X: (0, 0, -1) turns to (cx, cy, -1), normalised; no roll.
    const double yaw = -std::atan(cx), pitch = std::atan2(cy, std::sqrt(1 + cx * cx));
    const XrQuaternionf qYaw{0, static_cast<float>(std::sin(yaw / 2)), 0, static_cast<float>(std::cos(yaw / 2))};
    const XrQuaternionf qPitch{static_cast<float>(std::sin(pitch / 2)), 0, 0, static_cast<float>(std::cos(pitch / 2))};
    c.turn = qmul(qYaw, qPitch);
    to_matrix(c.turn, c.rotation);
    // The box's corners seen from the centred view (its frame: the transpose takes a box-frame direction there).
    double hx = 0, hy = 0;
    for (const double x : {l, r})
        for (const double y : {u, d}) {
            const double v[3] = {x, y, -1};
            double o[3];
            for (int i = 0; i < 3; ++i)
                o[i] = c.rotation[0 * 3 + i] * v[0] + c.rotation[1 * 3 + i] * v[1] + c.rotation[2 * 3 + i] * v[2];
            const double z = std::max(-o[2], 1e-6);
            hx = std::max(hx, std::fabs(o[0] / z));
            hy = std::max(hy, std::fabs(o[1] / z));
        }
    c.tan[0] = static_cast<float>(-hx);
    c.tan[1] = static_cast<float>(hx);
    c.tan[2] = static_cast<float>(hy);
    c.tan[3] = static_cast<float>(-hy);
    c.scaleX = r > l ? static_cast<float>(2 * hx / (r - l)) : 1.0f;
    c.scaleY = u > d ? static_cast<float>(2 * hy / (u - d)) : 1.0f;
    return c;
}

}  // namespace simqvfr

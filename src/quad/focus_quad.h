// focus_layer=2: a focus view (an eye's pose and the view's own FOV) as a quad layer `distance` metres out along the
// view's axis, centred on the FOV's centre and as large as the FOV there. A plane at any distance projects onto the
// eye exactly as the view's image plane (z = -1) does, so the quad shows the same pixels in the same directions as a
// projection layer with that FOV; far away, it keeps still against the periphery's image as the head moves.
#pragma once

#include <cmath>

#include "focus_turn.h"

namespace simqvfr {

struct FocusQuad {
    XrPosef pose{};
    XrExtent2Df size{};
};

inline FocusQuad focus_quad(const XrPosef& eye, const XrFovf& fov, float distance) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), u = std::tan(fov.angleUp),
                d = std::tan(fov.angleDown);
    float m[9];
    to_matrix(eye.orientation, m);
    const float x = 0.5f * (l + r) * distance, y = 0.5f * (u + d) * distance, z = -distance;
    FocusQuad q;
    q.pose.orientation = eye.orientation;
    q.pose.position = {eye.position.x + m[0] * x + m[1] * y + m[2] * z, eye.position.y + m[3] * x + m[4] * y + m[5] * z,
                       eye.position.z + m[6] * x + m[7] * y + m[8] * z};
    q.size = {(r - l) * distance, (u - d) * distance};
    return q;
}

}  // namespace simqvfr

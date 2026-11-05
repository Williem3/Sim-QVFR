// Focus-view culling: the four side planes of a focus camera's frustum in world space, and
// conservative sphere/box tests against them, so draws wholly outside a focus view can be skipped
// for that view. The focus projection is symmetric (tangent half-extents hw, hh) and reverse-Z
// infinite, so the near plane is irrelevant and there is no far plane. ACE's matrices are row-vector
// (p_camera = p * V) with x right, y up, looking down -z (quad/focus_turn.h); pass the turned world
// view, so the planes face the gaze.
#pragma once

#include <cmath>

namespace simqvfr {

struct FocusFrustum {
    // Planes (nx, ny, nz, d) with unit normals pointing out: n . p + d > 0 is outside that plane.
    // Order: right, left, top, bottom.
    float plane[4][4] = {};
};

// The frustum of a camera with row-vector world view `v` and tangent half-extents (hw, hh).
inline FocusFrustum focus_frustum(const float (&v)[16], double hw, double hh) {
    // Camera space: inside the right side when x <= hw * -z, i.e. x + hw * z <= 0, and so on.
    const double camera[4][3] = {{1, 0, hw}, {-1, 0, hw}, {0, 1, hh}, {0, -1, hh}};
    FocusFrustum f;
    for (int k = 0; k < 4; ++k) {
        // n_camera . (p * V) = p . (V3x3 * n_camera) + (translation row . n_camera).
        double w[4];
        for (int r = 0; r < 4; ++r)
            w[r] = v[r * 4 + 0] * camera[k][0] + v[r * 4 + 1] * camera[k][1] + v[r * 4 + 2] * camera[k][2];
        const double length = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        for (int c = 0; c < 4; ++c)
            f.plane[k][c] = static_cast<float>(length > 0 ? w[c] / length : 0);
    }
    return f;
}

// Whether a sphere lies wholly outside the frustum (then it can be skipped). Conservative: a sphere
// near a frustum edge may test inside while missing the frustum; never the other way round.
inline bool sphere_outside(const FocusFrustum& f, const float (&centre)[3], float radius) {
    for (const auto& p : f.plane)
        if (p[0] * centre[0] + p[1] * centre[1] + p[2] * centre[2] + p[3] > radius)
            return true;
    return false;
}

// Whether an axis-aligned box (centre, half-extents) lies wholly outside the frustum.
inline bool box_outside(const FocusFrustum& f, const float (&centre)[3], const float (&extent)[3]) {
    for (const auto& p : f.plane) {
        const float reach = std::fabs(p[0]) * extent[0] + std::fabs(p[1]) * extent[1] + std::fabs(p[2]) * extent[2];
        if (p[0] * centre[0] + p[1] * centre[1] + p[2] * centre[2] + p[3] > reach)
            return true;
    }
    return false;
}

}  // namespace simqvfr

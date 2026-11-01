// Gaze-turned focus cameras: the focus view keeps a centred projection and its camera turns (yaw,
// then pitch, no roll) to face the gaze, because off-centre projections darken ACE's screen-space
// clouds (recon "frame B turned instead of skewed"). Quaternion helpers, and the turn applied to
// ACE's row-vector matrices (p_camera = p * V, OpenXR-like axes: x right, y up, looking down -z).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../xr.h"

namespace aceqv {

inline XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline XrQuaternionf qconj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

// Row-major matrix of q (m * v == rotate(q, v)).
inline void to_matrix(const XrQuaternionf& q, float (&m)[9]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                        2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    std::memcpy(m, r, sizeof(r));
}

// The turn (in the eye's own frame) that points the focus camera at tangent (tanX, tanY) of an eye
// view with field of view `fov`, the centre clamped so the focus box (`fraction` of the eye's tangent
// extent) stays inside the eye's view.
// Same, for a view given by its tangents (left, right, up, down).
inline XrQuaternionf focus_turn(const float (&tan)[4], double fraction, double tanX, double tanY) {
    const double l = tan[0], r = tan[1], u = tan[2], d = tan[3];
    const double hw = (r - l) * fraction / 2, hh = (u - d) * fraction / 2;
    const double cx = std::clamp(tanX, l + hw, r - hw), cy = std::clamp(tanY, d + hh, u - hh);
    const double yaw = -std::atan(cx), pitch = std::atan(cy / std::sqrt(1 + cx * cx));
    const XrQuaternionf qYaw{0, static_cast<float>(std::sin(yaw / 2)), 0, static_cast<float>(std::cos(yaw / 2))};
    const XrQuaternionf qPitch{static_cast<float>(std::sin(pitch / 2)), 0, 0, static_cast<float>(std::cos(pitch / 2))};
    return qmul(qYaw, qPitch);
}

inline void fov_tangents(const XrFovf& fov, float (&tan)[4]) {
    tan[0] = std::tan(fov.angleLeft);
    tan[1] = std::tan(fov.angleRight);
    tan[2] = std::tan(fov.angleUp);
    tan[3] = std::tan(fov.angleDown);
}

inline XrFovf tangents_fov(const float (&tan)[4]) {
    return {std::atan(tan[0]), std::atan(tan[1]), std::atan(tan[2]), std::atan(tan[3])};
}

inline XrQuaternionf focus_turn(const XrFovf& fov, double fraction, double tanX, double tanY) {
    float tan[4];
    fov_tangents(fov, tan);
    return focus_turn(tan, fraction, tanX, tanY);
}

// The view tangents (left, right, up, down) a projection renders, from ACE's row-major projection
// (row-vector convention, as dumped in gate run 18): [0] = 2 / (r - l), [8] = (r + l) / (r - l),
// [5] = 2 / (u - d), [9] = (u + d) / (u - d). This is what ACE actually rendered a view with,
// whatever FOV it was given. False for anything that is not such a projection.
inline bool projection_tangents(const float (&p)[16], float (&tan)[4]) {
    if (!(p[0] > 0.01f && p[5] > 0.01f && std::isfinite(p[8]) && std::isfinite(p[9])))
        return false;
    tan[0] = (p[8] - 1) / p[0];
    tan[1] = (p[8] + 1) / p[0];
    tan[2] = (p[9] + 1) / p[5];
    tan[3] = (p[9] - 1) / p[5];
    return true;
}

// The focus box: `fraction` of a view's tangent extent, as half-extents (hw, hh) around its axis.
inline void focus_extents(const float (&tan)[4], double fraction, double& hw, double& hh) {
    hw = (tan[1] - tan[0]) * fraction / 2;
    hh = (tan[2] - tan[3]) * fraction / 2;
}

// A camera turned by `turn` in its own frame sees p * V * R (R = to_matrix(turn)): V <- V * R.
inline void turn_view_matrix(float (&v)[16], const XrQuaternionf& turn) {
    float r[9];
    to_matrix(turn, r);
    for (int row = 0; row < 4; ++row) {
        float out[3];
        for (int c = 0; c < 3; ++c)
            out[c] = v[row * 4 + 0] * r[0 * 3 + c] + v[row * 4 + 1] * r[1 * 3 + c] + v[row * 4 + 2] * r[2 * 3 + c];
        std::memcpy(&v[row * 4], out, sizeof(out));
    }
}

// Its inverse (camera to world, row-vector): inverse(V * R) = R^T * inverse(V), so M <- R^T * M.
inline void turn_inverse_view_matrix(float (&m)[16], const XrQuaternionf& turn) {
    float r[9];
    to_matrix(turn, r);
    float out[12];
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 4; ++c)
            out[row * 4 + c] = r[0 * 3 + row] * m[0 * 4 + c] + r[1 * 3 + row] * m[1 * 4 + c] + r[2 * 3 + row] * m[2 * 4 + c];
    std::memcpy(m, out, sizeof(out));
}

}  // namespace aceqv

// Focus boxes that converge where the eyes do. One gaze ray (or the head's forward axis) turned into each eye's frame
// points both boxes the same world direction: they meet at infinity. Looking at something near (a car's dash) the eyes
// converge and the boxes don't: on ACC's dash, 2026-11-08, each eye's box sat 3.5 deg from the other's (the IPD at about
// a metre), so the user saw two outlines that didn't fuse, and near a box edge an object sharp or shaded one way in one
// eye only. Here each eye's box is aimed at the fixation point instead: the gaze origin plus `distance` along the
// gaze, as that eye sees it. Pure, for tests/focus_converge_test.cpp.
#pragma once

#include <cmath>

#include "focus_turn.h"

namespace simqvfr {

// `v` rotated by `q` (to_matrix: m * v).
inline XrVector3f rotate_by(const XrQuaternionf& q, const XrVector3f& v) {
    float m[9];
    to_matrix(q, m);
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// Each eye's box centre as tangents (x right, y up, of that eye's view: the eye looks down -z) of the point `distance`
// metres from `origin` along `direction` (unit; the reference space's), eyes at `eyePos` turned by `eyeOri` (as
// xrLocateViews gives them). `valid[e]` false: the point is behind or beside eye e (its tangents unchanged).
inline void converged_tangents(const XrVector3f (&eyePos)[2], const XrQuaternionf (&eyeOri)[2], const XrVector3f& origin,
                               const XrVector3f& direction, double distance, float (&tan)[2][2], bool (&valid)[2]) {
    const XrVector3f p{static_cast<float>(origin.x + direction.x * distance),
                       static_cast<float>(origin.y + direction.y * distance),
                       static_cast<float>(origin.z + direction.z * distance)};
    for (int e = 0; e < 2; ++e) {
        const XrVector3f v{p.x - eyePos[e].x, p.y - eyePos[e].y, p.z - eyePos[e].z};
        const XrVector3f d = rotate_by(qconj(eyeOri[e]), v);
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        valid[e] = len > 0 && d.z < -0.1f * len;
        if (valid[e]) {
            tan[e][0] = d.x / -d.z;
            tan[e][1] = d.y / -d.z;
        }
    }
}

// The eyes' middle, and their middle orientation (the head's, for canted displays: each eye turned out of it).
inline XrVector3f eyes_middle(const XrVector3f (&eyePos)[2]) {
    return {(eyePos[0].x + eyePos[1].x) / 2, (eyePos[0].y + eyePos[1].y) / 2, (eyePos[0].z + eyePos[1].z) / 2};
}
inline XrQuaternionf eyes_middle(const XrQuaternionf (&eyeOri)[2]) {
    XrQuaternionf a = eyeOri[0], b = eyeOri[1];
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0)
        b = {-b.x, -b.y, -b.z, -b.w};
    XrQuaternionf h{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    const float n = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z + h.w * h.w);
    return n > 1e-6f ? XrQuaternionf{h.x / n, h.y / n, h.z / n, h.w / n} : a;
}

// A fixation distance (metres) from the depth a game sampled along an eye's view: `viewZ` the view-space depth along
// that eye's forward axis, at tangents (tx, ty): the distance along the ray.
inline double ray_distance(double viewZ, double tx, double ty) { return viewZ * std::sqrt(1 + tx * tx + ty * ty); }

// A scene depth turned into a view-space distance (metres) through the projection's depth terms, depth = a + b / z
// (z the view distance along the eye's forward axis): z = b / (depth - a). UE's reversed infinite depth (ACC) is a = 0,
// b = the near plane; LMU's reversed finite one has both. Negative: no distance (the far plane, the sky, past 10 km).
inline double depth_view_z(double depth, double a, double b) {
    const double z = depth != a ? b / (depth - a) : -1;
    return std::isfinite(z) && z > 0 && z <= 1e4 ? z : -1;
}

// A few depths read round one ray (tangents tx, ty of the eye) turned into the fixation distance along that ray: the
// median, the sky at 10 km, kept 0.2 m - 10 km. `sky`: how many were the sky.
inline double fixation_from_depths(const float* values, int n, double tx, double ty, double a, double b, int& sky) {
    double metres[16];
    n = n < 16 ? n : 16;
    sky = 0;
    for (int i = 0; i < n; ++i) {
        const double z = depth_view_z(values[i], a, b);
        sky += z < 0 ? 1 : 0;
        metres[i] = z < 0 ? 1e4 : ray_distance(z, tx, ty);
    }
    for (int i = 1; i < n; ++i)  // insertion sort: a handful
        for (int j = i; j > 0 && metres[j] < metres[j - 1]; --j) {
            const double t = metres[j];
            metres[j] = metres[j - 1];
            metres[j - 1] = t;
        }
    const double median = n ? metres[n / 2] : 1e4;
    return median < 0.2 ? 0.2 : median > 1e4 ? 1e4 : median;
}

// Smoothing a fixation distance: in log space (the vergence angle goes as 1 / distance), a time constant of `tau`
// seconds, and the value handed on only when it moved by more than `band` (a share) from the last one handed on, so a
// steady gaze keeps the boxes still.
struct FixationFilter {
    double smoothed = 0;  // log metres; 0 with `have` false: none yet
    double published = 0;
    bool have = false;
    double update(double metres, double dt, double tau = 0.12, double band = 0.05) {
        if (!(metres > 0) || !std::isfinite(metres))
            return published;
        const double l = std::log(metres);
        if (!have) {
            smoothed = l;
            published = metres;
            have = true;
            return published;
        }
        const double a = tau > 0 ? 1 - std::exp(-dt / tau) : 1;
        smoothed += (l - smoothed) * a;
        const double now = std::exp(smoothed);
        if (std::fabs(now / published - 1) > band)
            published = now;
        return published;
    }
};

}  // namespace simqvfr

// LMU's focus projection (docs/recon-lmu.md "Design notes"). The per-eye projection slots hold a row-major 4x4 for row
// vectors, left-handed (w = z): x_ndc = m[0] t + m[8] and y_ndc = m[5] t + m[9] for the view-space tangents t = x / z,
// t = y / z. So the eye spans x tangents [(-1 - m[8]) / m[0], (1 - m[8]) / m[0]] and likewise y. A focus view looks the
// same way with a narrower window: a box of half-extents (hw, hh) around tangents (cx, cy) is m[0] = 1 / hw,
// m[8] = -cx / hw, m[5] = 1 / hh, m[9] = -cy / hh, depth and w unchanged. Header-only, for tests/lmu_focus_test.cpp.
#pragma once

#include <cmath>

namespace lmu {

// The matrix is a perspective projection of that shape (no rotation or skew, w = z).
inline bool projection_shape(const float (&m)[16]) {
    return m[0] > 0.01f && m[5] > 0.01f && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[6] == 0 && m[7] == 0 &&
           m[11] == 1.0f && m[15] == 0 && std::isfinite(m[8]) && std::isfinite(m[9]);
}

struct FocusBox {
    bool valid = false;
    float eyeTan[4] = {};    // the eye: left, right, down, up
    float focusTan[4] = {};  // the focus view: left, right, down, up
    double rect[4] = {};     // the box in the eye's image: x0, y0, x1, y1 as fractions of width and height (y down)
};

// The box centre (cx, cy) moved the least so a box of `fractionX` x `fractionY` of the eye's tangent span stays inside
// the eye (QVFR: the gaze, which can look past the box's reach). A box as wide as the eye is centred on it. False:
// not a projection of the expected shape (cx, cy unchanged).
inline bool clamp_focus_centre(const float (&eye)[16], double fractionX, double fractionY, double& cx, double& cy) {
    if (!projection_shape(eye))
        return false;
    const auto clamp_axis = [](double t, double lo, double hi, double fraction) {
        const double half = (hi - lo) * fraction / 2;
        if (lo + half >= hi - half)
            return (lo + hi) / 2;
        return t < lo + half ? lo + half : t > hi - half ? hi - half : t;
    };
    cx = clamp_axis(cx, (-1 - eye[8]) / eye[0], (1 - eye[8]) / eye[0], fractionX);
    cy = clamp_axis(cy, (-1 - eye[9]) / eye[5], (1 - eye[9]) / eye[5], fractionY);
    return true;
}

// `out` = the focus projection for a box of `fractionX` x `fractionY` of the eye's tangent span, centred on tangents
// (cx, cy); `box` says where it is. False (`out` = `eye`): not a projection of the expected shape.
inline bool focus_projection(const float (&eye)[16], double fractionX, double fractionY, double cx, double cy, FocusBox& box,
                             float (&out)[16]) {
    for (int i = 0; i < 16; ++i)
        out[i] = eye[i];
    box = FocusBox{};
    if (!projection_shape(eye) || !(fractionX > 0) || !(fractionY > 0))
        return false;
    box.eyeTan[0] = (-1 - eye[8]) / eye[0];
    box.eyeTan[1] = (1 - eye[8]) / eye[0];
    box.eyeTan[2] = (-1 - eye[9]) / eye[5];
    box.eyeTan[3] = (1 - eye[9]) / eye[5];
    const double hw = (box.eyeTan[1] - box.eyeTan[0]) * fractionX / 2;
    const double hh = (box.eyeTan[3] - box.eyeTan[2]) * fractionY / 2;
    box.focusTan[0] = static_cast<float>(cx - hw);
    box.focusTan[1] = static_cast<float>(cx + hw);
    box.focusTan[2] = static_cast<float>(cy - hh);
    box.focusTan[3] = static_cast<float>(cy + hh);
    const auto ndcx = [&](double t) { return eye[0] * t + eye[8]; };
    const auto ndcy = [&](double t) { return eye[5] * t + eye[9]; };
    box.rect[0] = (ndcx(cx - hw) + 1) / 2;
    box.rect[2] = (ndcx(cx + hw) + 1) / 2;
    box.rect[1] = (1 - ndcy(cy + hh)) / 2;  // image rows run down, NDC y up
    box.rect[3] = (1 - ndcy(cy - hh)) / 2;
    out[0] = static_cast<float>(1 / hw);
    out[8] = static_cast<float>(-cx / hw);
    out[5] = static_cast<float>(1 / hh);
    out[9] = static_cast<float>(-cy / hh);
    box.valid = true;
    return true;
}

// ---- The focus view of a matrix that ends in the eye's clip space (the VR UI's projection x view) ----
// Eye and focus NDC are both affine in the view-space tangent, so the focus view's is the eye's remapped: x' = a x + b,
// y' = c y + d in NDC, i.e. clip' = (a x + b w, c y + d w, z, w). From the eye's and the focus projection
// (focus_projection's shape: NDC = m[0] t + m[8], m[5] t + m[9]).
struct ClipRemap {
    double a = 1, b = 0, c = 1, d = 0;
};

inline ClipRemap clip_remap(const float (&eye)[16], const float (&focus)[16]) {
    ClipRemap r;
    r.a = static_cast<double>(focus[0]) / eye[0];
    r.b = focus[8] - r.a * eye[8];
    r.c = static_cast<double>(focus[5]) / eye[5];
    r.d = focus[9] - r.c * eye[9];
    return r;
}

// Where a projection x view keeps its clip outputs: 1 in its columns (row vectors, clip = v M), 2 in its rows
// (column vectors), 0 neither. The w output (the view's forward axis in the world, w = +-z) has a unit vector in its
// first three entries; the other way round those are the translation's, which aren't.
inline int clip_layout(const float (&m)[16], double tolerance = 0.02) {
    const auto unit = [&](double x, double y, double z) { return std::fabs(std::sqrt(x * x + y * y + z * z) - 1) < tolerance; };
    const bool columns = unit(m[3], m[7], m[11]), rows = unit(m[12], m[13], m[14]);
    return columns == rows ? 0 : columns ? 1 : 2;
}

// LMU's size for a scale: ceil(recommended x scale), as cubeDX11_GetRenderSize rounds (a float product).
inline unsigned scaled_size(unsigned recommended, float scale) {
    const float x = static_cast<float>(recommended) * scale;
    const auto i = static_cast<unsigned>(x);
    return static_cast<float>(i) == x ? i : i + 1;
}

// The smallest recommended size LMU scales to `size` or more at `scale`: what a pass is given so its render size is
// what it should be on each axis while the game's render scale stays its own. Exact at scales up to 1; above 1 (the
// game supersampling) some sizes can't be reached, and scaled_size(result, scale) is at most one pixel more.
inline unsigned virtual_recommended(unsigned size, float scale) {
    if (!(scale > 0) || size == 0)
        return size;
    auto r = static_cast<unsigned>(std::ceil(static_cast<double>(size) / scale));
    while (r > 1 && scaled_size(r - 1, scale) >= size)
        --r;
    while (scaled_size(r, scale) < size)
        ++r;
    return r;
}

// `m` (clip outputs as clip_layout says: 1 columns, 2 rows) remapped to the focus view.
inline void remap_clip(float (&m)[16], const ClipRemap& r, int layout) {
    for (int i = 0; i < 4; ++i) {
        // Output o's entry for input i: columns hold m[i * 4 + o], rows m[o * 4 + i].
        const auto at = [&](int o) -> float& { return layout == 1 ? m[i * 4 + o] : m[o * 4 + i]; };
        const double w = at(3);
        at(0) = static_cast<float>(r.a * at(0) + r.b * w);
        at(1) = static_cast<float>(r.c * at(1) + r.d * w);
    }
}

}  // namespace lmu

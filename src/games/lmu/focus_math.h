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

}  // namespace lmu

// A pass's camera from what its draws upload, for DLSS on any game build (dlss_inputs.cpp): LMU writes each scene draw's
// view and view-projection matrices into the draw's vertex constant buffer (its shaders name them viewMatrix and
// viewProjMatrix), so the projection is inverse(view) x viewProj, as the GPU applies it (row vectors, reversed depth:
// docs/recon-lmu.md "DLSS step 1, run 3"). A pass draws other cameras too (the mirrors, side targets): the pass's own is
// the pair whose projection is the eye's, the FOV the game was handed. Header-only, for tests/lmu_dlss_test.cpp.
#pragma once

#include <cmath>

#include "../../engine/dlss_math.h"

namespace lmu {

// The projection of a (view, viewProj) pair, in the convention the eye's projection slot keeps (ordinary depth: the
// DLSS module reverses it again, dlss_math.h reversed_depth), its shape's zeros and ones made exact. False: the view
// isn't rigid, or the product isn't a projection of LMU's shape.
inline bool projection_of_pair(const float (&view)[16], const float (&viewProj)[16], float (&projection)[16]) {
    namespace dlss = simqvfr::dlss;
    const dlss::M4 v = dlss::from_floats(view), vp = dlss::from_floats(viewProj);
    dlss::M4 inverse;
    if (!dlss::rigid(v) || !dlss::invert(v, inverse))
        return false;
    dlss::M4 p = dlss::mul(inverse, vp);
    // Zero in a projection of this shape (x and y rows' depth and w terms, the off-diagonal x/y terms, the last row's
    // x and y): the product's rounding left there. m[10] and m[14] are depth's own: kept.
    const int zeros[] = {1, 2, 3, 4, 6, 7, 12, 13, 15};
    for (int i : zeros) {
        if (std::fabs(p[i]) > 1e-3)
            return false;
        p[i] = 0;
    }
    if (std::fabs(p[11] - 1) > 1e-3)
        return false;
    p[11] = 1;
    const dlss::M4 ordinary = dlss::reversed_depth(p);  // its own inverse: z' = w - z both ways
    if (!dlss::projection(ordinary))
        return false;
    for (int i = 0; i < 16; ++i)
        projection[i] = static_cast<float>(ordinary[i]);
    return true;
}

// `projection` is the eye's (`eye`: the FOV the game was handed, focus_math.h projection_from_tangents): its scale
// terms (m0, m5: the FOV's width and height, which DLSS's jitter doesn't move) to 0.1%, its offsets (m8, m9) within
// 0.01: DLSS's jitter moves those by up to a pixel (2 / width in NDC) and the FOV located last may be a frame on from
// the pass's. A mirror's or side target's camera is another FOV altogether.
inline bool same_eye(const float (&projection)[16], const float (&eye)[16]) {
    const auto rel = [](double a, double b) { return std::fabs(a - b) <= 1e-3 * std::fabs(b); };
    return rel(projection[0], eye[0]) && rel(projection[5], eye[5]) && std::fabs(projection[8] - eye[8]) <= 0.01 &&
           std::fabs(projection[9] - eye[9]) <= 0.01;
}

}  // namespace lmu

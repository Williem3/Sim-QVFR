// How ACC's four views map onto UE 4.26's stereo passes (pure; views.cpp patches them in, tests check them).
// Views follow views4_layout.h: 0 and 1 the eyes' outer views, 2 and 3 their focus views, in eye columns of one
// target (0 | 1 over 2 | 3): instanced stereo needs the eyes side by side from x = 0, and OpenComposite gives
// each eye only its half of the texture ACC submits.
// UE's passes: eSSP_FULL 0, eSSP_LEFT_EYE 1, eSSP_RIGHT_EYE 2, eSSP_LEFT_EYE_SIDE 3, eSSP_RIGHT_EYE_SIDE 4,
// as Epic's 4.26 OpenXR plugin maps them for quad views (pass = eSSP_LEFT_EYE + view).
#pragma once

#include "../../quad/focus_turn.h"
#include "../../quad/views4_layout.h"

namespace acc {

inline constexpr int kPassFull = 0, kPassLeft = 1, kPassRight = 2, kPassLeftSide = 3, kPassRightSide = 4;
inline constexpr int kViews = 4;

inline int views_for(bool stereo) { return stereo ? kViews : 1; }
inline int pass_for_view(bool stereo, unsigned view) { return stereo ? kPassLeft + static_cast<int>(view) : kPassFull; }
// eSSP_FULL is view 0, as in the engine's default.
inline unsigned view_for_pass(int pass) { return pass <= kPassLeft ? 0u : static_cast<unsigned>(pass - kPassLeft); }
// The eye a pass belongs to (a side pass is its eye's focus view).
inline int eye_pass(int pass) { return pass >= kPassLeftSide ? pass - 2 : pass; }
// Primary passes draw meshes; under instanced stereo each draws itself and the next pass (UE 4.26 pairs pass n
// with n + 1), so the focus views are a second pair, led by the left one, as the eyes are by the left eye.
inline bool primary_pass(int pass) { return pass == kPassFull || pass == kPassLeft || pass == kPassLeftSide; }

// A stereo pass's view in the target (its width, height) the game sized for all four
// (aceqv::views4_rect_eye_columns).
struct PassRect {
    int x = 0, y = 0, width = 0, height = 0;
};
inline PassRect pass_rect(const aceqv::Views4Layout& layout, int pass, int targetWidth, int targetHeight) {
    const aceqv::ViewRect r =
        aceqv::views4_rect_eye_columns(layout, static_cast<int>(view_for_pass(pass)), targetWidth, targetHeight);
    return {r.left, r.top, r.width, r.height};
}

// A focus view's projection: its eye's, narrowed to the focus box (`fractionX` of the eye's tangent width,
// `fractionY` of its height), centred on the eye's forward axis. UE 4.26's FMatrix is row-major for row vectors,
// as SteamVR's GetStereoProjectionMatrix fills it: [0] = 2 / (r - l), [8] = (r + l) / (r - l), [5] = 2 / (u - d),
// [9] = (u + d) / (u - d), reversed infinite depth in [11] and [14] (kept). The same box ACE's focus views get
// (focus_extents). False, `m` unchanged: no box (`fractionX` <= 0, the focus views copy the eyes) or not such a
// projection.
inline bool focus_projection(float (&m)[16], double fractionX, double fractionY) {
    float tan[4];
    if (!(fractionX > 0) || !aceqv::projection_tangents(m, tan))
        return false;
    double hw = 0, hh = 0;
    aceqv::focus_extents(tan, fractionX, fractionY > 0 ? fractionY : fractionX, hw, hh);
    m[0] = static_cast<float>(1.0 / hw);  // 2 / (r - l) with r = -l = hw
    m[5] = static_cast<float>(1.0 / hh);
    m[8] = 0.0f;
    m[9] = 0.0f;
    return true;
}

// UE 4.26's FQuat (X, Y, Z, W as floats; left-handed, X forward, Y right, Z up) of an OpenXR rotation (right-handed,
// x right, y up, looking down -z), as Epic's OpenXR plugin converts it (ToFQuat: -z, x, y, -w).
struct UeQuat {
    float x = 0, y = 0, z = 0, w = 1;
};
inline UeQuat ue_quat(const XrQuaternionf& q) { return {-q.z, q.x, q.y, -q.w}; }
// a * b: b first, then a (FQuat's operator*). A rotation in an orientation's own frame goes on the right.
inline UeQuat ue_mul(const UeQuat& a, const UeQuat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
// A vector rotated by q.
inline void ue_rotate(const UeQuat& q, const float (&v)[3], float (&out)[3]) {
    const UeQuat p{v[0], v[1], v[2], 0};
    const UeQuat c{-q.x, -q.y, -q.z, q.w};
    const UeQuat r = ue_mul(ue_mul(q, p), c);
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}

}  // namespace acc

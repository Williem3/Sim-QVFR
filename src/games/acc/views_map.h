// How ACC's four views map onto UE 4.26's stereo passes (pure; views.cpp patches them in, tests check them).
// Views follow views4_layout.h: 0 and 1 the eyes' outer views, 2 and 3 their focus views, in eye columns of one
// target (0 | 1 over 2 | 3): instanced stereo needs the eyes side by side from x = 0, and OpenComposite gives
// each eye only its half of the texture ACC submits.
// UE's passes: eSSP_FULL 0, eSSP_LEFT_EYE 1, eSSP_RIGHT_EYE 2, eSSP_LEFT_EYE_SIDE 3, eSSP_RIGHT_EYE_SIDE 4,
// as Epic's 4.26 OpenXR plugin maps them for quad views (pass = eSSP_LEFT_EYE + view).
#pragma once

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

}  // namespace acc

// focus_layer=2 ([experiment] focus_quad_distance, metres; live, and the headset menu's Focus quad distance): how far
// out the focus views' quads sit (focus_quad.h). A test of how SteamVR corrects the periphery's image for head
// movement: the quads line up with it at the distance it assumes. The menu steps through kFocusQuadDistances.
#pragma once

#include <algorithm>
#include <cstdlib>

#include <windows.h>

namespace simqvfr {

inline constexpr int kFocusQuadDistances[] = {1, 2, 3, 4, 5, 7, 10, 15, 20, 30, 50, 100};
inline constexpr int kFocusQuadSteps = static_cast<int>(sizeof(kFocusQuadDistances) / sizeof(kFocusQuadDistances[0]));
inline constexpr int kFocusQuadDistanceDefault = 100;

// The step nearest `metres`.
inline int focus_quad_step(int metres) {
    int best = 0;
    for (int i = 1; i < kFocusQuadSteps; ++i)
        if (std::abs(kFocusQuadDistances[i] - metres) < std::abs(kFocusQuadDistances[best] - metres))
            best = i;
    return best;
}

inline int focus_quad_metres(int step) { return kFocusQuadDistances[std::clamp(step, 0, kFocusQuadSteps - 1)]; }

inline int read_focus_quad_distance(const wchar_t* ini) {
    return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"experiment", L"focus_quad_distance",
                                                             kFocusQuadDistanceDefault, ini)),
                      1, 1000);
}

}  // namespace simqvfr

// The focus box's vertical offset ([quad] fixed_offset_y, gaze_offset_y; live): degrees up (+) or down
// (-) of where it would sit. Fixed (gaze=0, or eye tracking lost) the box sits on each eye's forward
// axis, so the offset picks what stays sharp without eye tracking (the road, the dash). Eye tracked it
// is added to the gaze, a bias for trackers that read high or low. Each mode keeps its own value, so
// switching between them keeps both. Shared by the layer, the launcher and the in-headset menu.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <windows.h>

#include "../ini_put.h"

namespace simqvfr {

inline constexpr int kFocusOffsetMax = 20;  // degrees either way

struct FocusOffset {
    int fixed = 0;  // [quad] fixed_offset_y
    int gaze = 0;   // [quad] gaze_offset_y
    int for_mode(bool eyeTracking) const { return eyeTracking ? gaze : fixed; }
    int& for_mode(bool eyeTracking) { return eyeTracking ? gaze : fixed; }
    bool operator==(const FocusOffset&) const = default;
};

inline FocusOffset read_focus_offset(const wchar_t* ini) {
    const auto number = [ini](const wchar_t* key) {
        wchar_t text[16] = {};  // as a string: GetPrivateProfileInt reads negative values as 0
        GetPrivateProfileStringW(L"quad", key, L"", text, 16, ini);
        return std::clamp(text[0] ? _wtoi(text) : 0, -kFocusOffsetMax, kFocusOffsetMax);
    };
    return {number(L"fixed_offset_y"), number(L"gaze_offset_y")};
}

inline bool write_focus_offset(const wchar_t* ini, const FocusOffset& o) {
    const auto put = [ini](const wchar_t* key, int value) {
        const int v = std::clamp(value, -kFocusOffsetMax, kFocusOffsetMax);
        return ini_put(ini, L"quad", key, v);
    };
    return put(L"fixed_offset_y", o.fixed) & put(L"gaze_offset_y", o.gaze);
}

// The vertical tangent `tanY` (up positive) turned `degrees` further up, kept short of straight up or down.
inline double offset_tan_y(double tanY, double degrees) {
    constexpr double kRad = 3.14159265358979 / 180;
    const double angle = std::clamp(std::atan(tanY) + degrees * kRad, -80 * kRad, 80 * kRad);
    return std::tan(angle);
}

}  // namespace simqvfr

// The focus box's offset ([quad] fixed_offset_y, gaze_offset_y, fixed_offset_x, gaze_offset_x; live): degrees up (+)
// or down (-), right (+) or left (-) of where it would sit. Fixed (gaze=0, or eye tracking lost) the box sits on each eye's forward
// axis, so the offset picks what stays sharp without eye tracking (the road, the dash). Eye tracked it
// is added to the gaze, a bias for trackers that read high or low. Each mode keeps its own value, so
// switching between them keeps both. Shared by the layer, the launcher and the in-headset menu.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <windows.h>

#include "../ini.h"

namespace simqvfr {

inline constexpr int kFocusOffsetMax = 20;  // degrees either way

struct FocusOffset {
    int fixed = 0;   // [quad] fixed_offset_y: degrees up
    int gaze = 0;    // [quad] gaze_offset_y
    int fixedX = 0;  // [quad] fixed_offset_x: degrees right
    int gazeX = 0;   // [quad] gaze_offset_x
    int for_mode(bool eyeTracking) const { return eyeTracking ? gaze : fixed; }
    int& for_mode(bool eyeTracking) { return eyeTracking ? gaze : fixed; }
    int for_mode_x(bool eyeTracking) const { return eyeTracking ? gazeX : fixedX; }
    int& for_mode_x(bool eyeTracking) { return eyeTracking ? gazeX : fixedX; }
    bool operator==(const FocusOffset&) const = default;
};

inline FocusOffset read_focus_offset(const wchar_t* ini) {
    const auto number = [ini](const wchar_t* key) {
        return std::clamp(ini_signed(ini, L"quad", key, 0), -kFocusOffsetMax, kFocusOffsetMax);
    };
    return {number(L"fixed_offset_y"), number(L"gaze_offset_y"), number(L"fixed_offset_x"), number(L"gaze_offset_x")};
}

inline bool write_focus_offset(const wchar_t* ini, const FocusOffset& o) {
    const auto put = [ini](const wchar_t* key, int value) {
        const int v = std::clamp(value, -kFocusOffsetMax, kFocusOffsetMax);
        return ini_put(ini, L"quad", key, v);
    };
    return put(L"fixed_offset_y", o.fixed) & put(L"gaze_offset_y", o.gaze) & put(L"fixed_offset_x", o.fixedX) &
           put(L"gaze_offset_x", o.gazeX);
}

// The vertical tangent `tanY` (up positive) turned `degrees` further up, kept short of straight up or down.
inline double offset_tan_y(double tanY, double degrees) {
    constexpr double kRad = 3.14159265358979 / 180;
    const double angle = std::clamp(std::atan(tanY) + degrees * kRad, -80 * kRad, 80 * kRad);
    return std::tan(angle);
}

// The horizontal tangent `tanX` (right positive) turned `degrees` further right: the same turn as offset_tan_y.
inline double offset_tan_x(double tanX, double degrees) { return offset_tan_y(tanX, degrees); }

// A view's gaze tangents (x right, y up) moved by the offset of degrees right and up.
inline void offset_tan(float (&tan)[2], int right, int up) {
    if (right)
        tan[0] = static_cast<float>(offset_tan_x(tan[0], right));
    if (up)
        tan[1] = static_cast<float>(offset_tan_y(tan[1], up));
}

}  // namespace simqvfr

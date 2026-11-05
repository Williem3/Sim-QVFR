// The gaze dead-zone ([quad] gaze_deadzone, degrees; live): the focus box holds still while a tracked gaze
// stays this close to where it aims, and jumps to the gaze when it moves further, so tracker noise does not
// turn the focus cameras every frame (shimmer in the box with MSAA). Kept in tenths of a degree, the
// sliders' and the menu's step. Shared by the layer, the launcher and the in-headset menu.
#pragma once

#include <algorithm>
#include <cwchar>
#include <string>

#include <windows.h>

namespace simqvfr {

inline constexpr int kGazeDeadzoneDefault = 15;  // tenths of a degree
inline constexpr int kGazeDeadzoneMax = 50;

inline int read_gaze_deadzone(const wchar_t* ini) {
    wchar_t text[16] = {};
    GetPrivateProfileStringW(L"quad", L"gaze_deadzone", L"", text, 16, ini);
    if (!text[0])
        return kGazeDeadzoneDefault;
    return std::clamp(static_cast<int>(_wtof(text) * 10 + 0.5), 0, kGazeDeadzoneMax);
}

// "1.5": tenths as degrees.
inline std::wstring gaze_deadzone_text(int tenths) {
    const int v = std::clamp(tenths, 0, kGazeDeadzoneMax);
    return std::to_wstring(v / 10) + L"." + std::to_wstring(v % 10);
}

inline bool write_gaze_deadzone(const wchar_t* ini, int tenths) {
    return WritePrivateProfileStringW(L"quad", L"gaze_deadzone", gaze_deadzone_text(tenths).c_str(), ini) != 0;
}

}  // namespace simqvfr

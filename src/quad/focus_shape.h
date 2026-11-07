// The focus area's shape ([quad] focus_shape and its corners; live): a box (its corners rounded by
// corner_radius_percent), or a quadrilateral whose four corners are pulled in from the box's, each on its
// own. The focus views still render the whole box; only the area blended into the outer views changes.
// With the box held low and its top corners pulled in, a trapezoid keeps more of the dash sharp and
// less of the far field (fixed foveation). Shared by the layer, the launcher and the in-headset menu.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <windows.h>

#include "../ini_put.h"

namespace simqvfr {

// Each corner moves at most this far in, percent of the box's width or height: the corners stay in their
// own quarters of the box, so the edges never cross.
inline constexpr int kFocusCornerMax = 45;

enum class FocusCorner { TopLeft, TopRight, BottomRight, BottomLeft };
inline constexpr int kFocusCorners = 4;

struct FocusShape {
    bool corners = false;  // [quad] focus_shape=corners (else box)
    // Per corner (FocusCorner's order), percent of the box: `in` toward its middle across, `vertical` toward
    // its middle up or down (top corners down, bottom corners up).
    int in[kFocusCorners] = {};
    int vertical[kFocusCorners] = {};
    bool operator==(const FocusShape&) const = default;
};

// The ini keys of a corner's insets: [quad] shape_top_left_in, shape_top_left_down, ... shape_bottom_left_up.
inline const wchar_t* focus_corner_key(FocusCorner corner, bool vertical) {
    static const wchar_t* const keys[kFocusCorners][2] = {{L"shape_top_left_in", L"shape_top_left_down"},
                                                          {L"shape_top_right_in", L"shape_top_right_down"},
                                                          {L"shape_bottom_right_in", L"shape_bottom_right_up"},
                                                          {L"shape_bottom_left_in", L"shape_bottom_left_up"}};
    return keys[static_cast<int>(corner)][vertical ? 1 : 0];
}

inline FocusShape read_focus_shape(const wchar_t* ini) {
    FocusShape s;
    wchar_t text[16] = {};
    GetPrivateProfileStringW(L"quad", L"focus_shape", L"box", text, 16, ini);
    s.corners = _wcsicmp(text, L"corners") == 0;
    for (int i = 0; i < kFocusCorners; ++i) {
        const auto corner = static_cast<FocusCorner>(i);
        s.in[i] = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"quad", focus_corner_key(corner, false), 0, ini)), 0,
                             kFocusCornerMax);
        s.vertical[i] = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"quad", focus_corner_key(corner, true), 0, ini)),
                                   0, kFocusCornerMax);
    }
    return s;
}

inline bool write_focus_shape(const wchar_t* ini, const FocusShape& s) {
    const auto put = [ini](const wchar_t* key, int value) {
        return ini_put(ini, L"quad", key, std::clamp(value, 0, kFocusCornerMax));
    };
    bool ok = ini_put(ini, L"quad", L"focus_shape", s.corners ? L"corners" : L"box");
    for (int i = 0; i < kFocusCorners; ++i) {
        ok &= put(focus_corner_key(static_cast<FocusCorner>(i), false), s.in[i]);
        ok &= put(focus_corner_key(static_cast<FocusCorner>(i), true), s.vertical[i]);
    }
    return ok;
}

// The corners in one word, a byte each (FocusCorner's order, `in` then `vertical`, percent), for the layer's
// atomic; 0 for a box, or corners all left where the box has them.
inline uint64_t focus_shape_bits(const FocusShape& s) {
    uint64_t bits = 0;
    if (!s.corners)
        return 0;
    for (int i = 0; i < kFocusCorners; ++i) {
        bits |= static_cast<uint64_t>(std::clamp(s.in[i], 0, kFocusCornerMax)) << (16 * i);
        bits |= static_cast<uint64_t>(std::clamp(s.vertical[i], 0, kFocusCornerMax)) << (16 * i + 8);
    }
    return bits;
}

// focus_shape_bits as the compositor takes them (EyeComposition::cornerInset): shares of the box.
inline void focus_shape_insets(uint64_t bits, float (&inset)[2 * kFocusCorners]) {
    for (int i = 0; i < 2 * kFocusCorners; ++i)
        inset[i] = static_cast<float>((bits >> (8 * i)) & 0xff) / 100.0f;
}

// For the log: "box", or "corners: top left 20/0, top right 20/0, bottom right 0/0, bottom left 0/0" (percent
// in, then down or up).
inline std::wstring focus_shape_text(const FocusShape& s) {
    if (!s.corners)
        return L"box";
    static const wchar_t* const names[kFocusCorners] = {L"top left", L"top right", L"bottom right", L"bottom left"};
    std::wstring text = L"corners:";
    for (int i = 0; i < kFocusCorners; ++i)
        text += std::wstring(i ? L", " : L" ") + names[i] + L" " + std::to_wstring(s.in[i]) + L"/" +
                std::to_wstring(s.vertical[i]);
    return text;
}

// A corner's place in the box: 0..1 across from the left and down from the top.
inline void focus_corner_point(const FocusShape& s, FocusCorner corner, float& x, float& y) {
    const int i = static_cast<int>(corner);
    const float in = s.corners ? std::clamp(s.in[i], 0, kFocusCornerMax) / 100.0f : 0.0f;
    const float v = s.corners ? std::clamp(s.vertical[i], 0, kFocusCornerMax) / 100.0f : 0.0f;
    const bool right = corner == FocusCorner::TopRight || corner == FocusCorner::BottomRight;
    const bool bottom = corner == FocusCorner::BottomRight || corner == FocusCorner::BottomLeft;
    x = right ? 1 - in : in;
    y = bottom ? 1 - v : v;
}

}  // namespace simqvfr

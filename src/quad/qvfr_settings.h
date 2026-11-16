// The core QVFR settings' keys, defaults, ranges and steps: one table the launcher (launcher/settings.cpp), the
// headset menu (settings_menu.cpp) and the layer (layer/config.cpp) all read, so none shows or applies a value the
// others don't. Each reader keeps its own derived logic (periphery and density from eye_scale, a writer's QVFR-only
// keys); only the names, defaults and ranges live here.
#pragma once

#include <algorithm>
#include <cwchar>

#include <windows.h>

#include "../ini.h"

namespace simqvfr {

// One integer ini key: [section] key, its value when missing, its range and the menu's step.
struct IniInt {
    const wchar_t* section;
    const wchar_t* key;
    int def, min, max, step;
    constexpr int clamp(int v) const { return std::clamp(v, min, max); }
};

// [experiment]: the resolution. periphery_scale and focus_density 0 (or missing) follow eye_scale (equal quarters: the
// periphery at eye_scale, the density eye_scale / focus_percent); their `def` is the launcher's starting value (Restore
// Defaults writes both), not what a missing key means.
inline constexpr IniInt kEyeScale{L"experiment", L"eye_scale", 50, 10, 100, 5};  // Two-loop's resolution; QVFR's fallback
inline constexpr IniInt kPeripheryScale{L"experiment", L"periphery_scale", 50, 10, 100, 5};
inline constexpr IniInt kFocusDensity{L"experiment", L"focus_density", 100, 50, 400, 5};
inline constexpr IniInt kObjectDetail{L"experiment", L"object_detail", 100, 0, 100, 10};  // X-Plane's
// The focus box, percent of the outer view. focus_height_percent missing (or 0): focus_percent's.
inline constexpr IniInt kFocusPercent{L"experiment", L"focus_percent", 35, 10, 100, 1};
inline constexpr IniInt kFocusHeightPercent{L"experiment", L"focus_height_percent", 35, 10, 100, 1};
// [quad]: the blend, the outline, eye tracking, sharpening and the eye off.
inline constexpr IniInt kFeatherPercent{L"quad", L"feather_percent", 10, 0, 50, 1};
inline constexpr IniInt kCornerRadiusPercent{L"quad", L"corner_radius_percent", 0, 0, 100, 5};
inline constexpr IniInt kDebugBorder{L"quad", L"debug_border", 0, 0, 1, 1};
// The focus mask (quad/focus_mask.h; a game with it, LMU): the focus views skip rendering their texels outside the focus
// shape (pulled-in corners or rounding). 0: they render the whole box, as before. Live.
inline constexpr IniInt kFocusMask{L"quad", L"focus_mask", 1, 0, 1, 1};
// Diagnostics: the mask's margin past the shape, focus view texels (what DLSS, sharpening and screen-space effects read
// beside the shown texels). Live.
inline constexpr IniInt kFocusMaskMargin{L"experiment", L"focus_mask_margin", 16, 0, 64, 1};
inline constexpr IniInt kGaze{L"quad", L"gaze", 1, 0, 1, 1};
inline constexpr IniInt kSharpenPercent{L"quad", L"sharpen_percent", 0, 0, 100, 5};
inline constexpr IniInt kEyeOff{L"quad", L"eye_off", 0, 0, 2, 1};  // 0 both eyes on, 1 the left off, 2 the right
// [crop] (QVFR): fov_crop.h's kCropMax is the sides' 40.
inline constexpr IniInt kCropEnabled{L"crop", L"enabled", 0, 0, 1, 1};
inline constexpr IniInt kCropTop{L"crop", L"top", 0, 0, 40, 1};
inline constexpr IniInt kCropBottom{L"crop", L"bottom", 0, 0, 40, 1};
inline constexpr IniInt kCropOuter{L"crop", L"outer", 0, 0, 40, 1};
inline constexpr IniInt kCropInner{L"crop", L"inner", 0, 0, 40, 1};
inline constexpr IniInt kHotkeyFooter{L"menu", L"hotkey_footer", 1, 0, 1, 1};

// The key as written, `fallback` when missing; not clamped (0 keeps its "unset" meaning where a key has one).
// GetPrivateProfileInt, as every reader had it: a negative value reads as 0.
inline int ini_raw(const wchar_t* ini, const IniInt& k, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(k.section, k.key, fallback, ini));
}
// The key in its range, its default when missing.
inline int ini_int(const wchar_t* ini, const IniInt& k) { return k.clamp(ini_raw(ini, k, k.def)); }
inline int ini_int(const wchar_t* ini, const IniInt& k, int fallback) { return k.clamp(ini_raw(ini, k, fallback)); }
inline bool ini_flag(const wchar_t* ini, const IniInt& k) { return ini_raw(ini, k, k.def) != 0; }

// [quad] debug_border_color: green (also anything unknown), magenta or teal.
enum class BorderColor { Green, Magenta, Teal };
inline constexpr wchar_t kBorderColorSection[] = L"quad", kBorderColorKey[] = L"debug_border_color";
inline BorderColor parse_border_color(const wchar_t* text) {
    return _wcsicmp(text, L"magenta") == 0 ? BorderColor::Magenta : _wcsicmp(text, L"teal") == 0 ? BorderColor::Teal : BorderColor::Green;
}
inline BorderColor read_border_color(const wchar_t* ini) {
    return parse_border_color(ini_get(ini, kBorderColorSection, kBorderColorKey).c_str());
}

// [experiment] mode as the layer runs it: views4 QVFR, quad or two_loops Two-loop (quad: frame B the focus box),
// off or missing nothing, the research modes (focus_only, double_render, views3) and anything else Other.
enum class IniRenderMode { Off, Qvfr, TwoLoop, Other };
inline IniRenderMode parse_render_mode(const wchar_t* mode) {
    if (!mode || !*mode || _wcsicmp(mode, L"off") == 0)
        return IniRenderMode::Off;
    if (_wcsicmp(mode, L"views4") == 0)
        return IniRenderMode::Qvfr;
    if (_wcsicmp(mode, L"quad") == 0 || _wcsicmp(mode, L"two_loops") == 0)
        return IniRenderMode::TwoLoop;
    return IniRenderMode::Other;
}

}  // namespace simqvfr

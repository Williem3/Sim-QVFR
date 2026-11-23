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
// Road focus (a game with GameFeatures::focusCorridor, LMU: games/lmu/mvr_pass_corridor.h): the focus views render only
// a corridor along the road ahead and the player's own car, the periphery's image everywhere else in them. Fixed
// foveation only (off while eye tracking is on: the gaze leaves the road). Live; on by default.
inline constexpr IniInt kFocusCorridor{L"quad", L"focus_corridor", 0, 0, 1, 1};
// Road width: metres either side of the road's centre kept sharp. 5 holds the track's surface on most circuits (the
// centre follows the road); 20 takes in run-off and barriers, the corridor then a good part of the box (less saved).
inline constexpr IniInt kFocusCorridorWidth{L"quad", L"focus_corridor_width", 10, 5, 20, 1};
// Road height: how high above the road stays sharp, tenths of a metre (2 m: the road and the cars; 8 m: signs, gantries
// and the grandstands' lower rows too, less saved).
inline constexpr IniInt kFocusCorridorHeight{L"quad", L"focus_corridor_height", 35, 20, 80, 5};
// Road widening: degrees either side the corridor opens by with distance past its start, so a bend leaving the head's
// line far ahead (Eau Rouge, a hairpin, the pit exit) stays inside. 10-25: below ~10 the corridor is a straight band
// again and corners fall out (a 1-degree run at Spa, 2026-11-23); above 25 it takes in most of the far scenery.
inline constexpr IniInt kFocusCorridorSpread{L"quad", L"focus_corridor_spread", 15, 10, 25, 1};
// The cockpit preset Road focus sets when it's switched on (quad/road_focus_shape.h): 0 A, 1 B.
inline constexpr IniInt kFocusCorridorPreset{L"quad", L"focus_corridor_preset", 0, 0, 1, 1};
// Road edge blend: how soft its edges are, tenths of a metre near (growing with distance in proportion); 0 a hard edge.
inline constexpr IniInt kFocusCorridorBlend{L"quad", L"focus_corridor_blend", 10, 0, 30, 1};  // 0.1 m a step
// Show Road focus area: what it takes from the periphery tinted blue in the headset (it works as ever). Live.
inline constexpr IniInt kFocusCorridorShow{L"quad", L"focus_corridor_show", 0, 0, 1, 1};
// Diagnostics: the mask's margin past the shape, focus view texels (what DLSS, sharpening and screen-space effects read
// beside the shown texels). Live.
inline constexpr IniInt kFocusMaskMargin{L"experiment", L"focus_mask_margin", 16, 0, 64, 1};
inline constexpr IniInt kGaze{L"quad", L"gaze", 1, 0, 1, 1};
inline constexpr IniInt kSharpenPercent{L"quad", L"sharpen_percent", 0, 0, 100, 5};
// Periphery texture match (a game with GameFeatures::peripheryTextureMatch, ACE): the periphery samples its textures
// sharper, by this share of the mip levels the focus views are sharper by, so see-through surfaces (fences, nets,
// lattices) look the same in both: at the periphery's resolution they otherwise blend into a light solid shade the
// focus views don't show. 100 the same level as the focus views (the default), 0 off (the game's own). Live.
inline constexpr IniInt kPeripheryTextureMatch{L"quad", L"periphery_texture_match", 100, 0, 100, 10};
// Colour correction (quad/color_grade.h; a game whose compositor runs, GameFeatures::sharpening): the switch, then the
// exposure in tenths of a stop and brightness, contrast and saturation in percent, each 0 unchanged. Live.
inline constexpr IniInt kColorCorrection{L"quad", L"color_correction", 0, 0, 1, 1};
inline constexpr IniInt kColorExposure{L"quad", L"color_exposure", 0, -30, 30, 1};
inline constexpr IniInt kColorBrightness{L"quad", L"color_brightness", 0, -50, 50, 2};
inline constexpr IniInt kColorContrast{L"quad", L"color_contrast", 0, -50, 50, 2};
inline constexpr IniInt kColorSaturation{L"quad", L"color_saturation", 0, -100, 100, 5};
// Night car lighting (quad/car_lighting.h; a game with GameFeatures::carLighting, LMU): the switch (its hooks at
// launch: turned on later, the next launch), then the game's light kept on the cars, ambient occlusion's strength, the
// floodlights' highlights and the lacquer's gloss, in percent. Live once on at launch.
inline constexpr IniInt kCarLight{L"quad", L"car_light", 0, 0, 1, 1};
inline constexpr IniInt kCarLightFill{L"quad", L"car_light_fill", 80, 30, 100, 5};
inline constexpr IniInt kCarLightOcclusion{L"quad", L"car_light_occlusion", 70, 0, 100, 5};
inline constexpr IniInt kCarLightHighlights{L"quad", L"car_light_highlights", 100, 0, 200, 10};
inline constexpr IniInt kCarLightGloss{L"quad", L"car_light_gloss", 70, 0, 100, 5};
// The width the cars' surface directions are smoothed over, cm (0: pixel to pixel, the triangles' facets show).
inline constexpr IniInt kCarLightSmooth{L"quad", L"car_light_smooth", 15, 0, 50, 1};
// The floodlights' shadows (the depth searched toward each light: a wing on the deck, a car on the next) and the paint's
// fine detail (the game's normal maps: orange peel, carbon weave), each on or off.
inline constexpr IniInt kCarLightShadows{L"quad", L"car_light_shadows", 1, 0, 1, 1};
inline constexpr IniInt kCarLightDetail{L"quad", L"car_light_detail", 1, 0, 1, 1};
// The lacquer's reflections of what the eye sees (screen-space: the grandstands' lights, other cars), on or off.
inline constexpr IniInt kCarLightReflections{L"quad", L"car_light_reflections", 1, 0, 1, 1};
// The headlights as the floodlights (their highlights on the paint), the cars' shadows on the ground (the floodlights'
// and the cars' own occlusion), and night vision's colour (the dark loses its colour, a little blue), each on or off.
inline constexpr IniInt kCarLightHeadlights{L"quad", L"car_light_headlights", 1, 0, 1, 1};
inline constexpr IniInt kCarLightGround{L"quad", L"car_light_ground", 1, 0, 1, 1};
inline constexpr IniInt kCarLightNightVision{L"quad", L"car_light_night_vision", 0, 0, 1, 1};
// Night car lighting's quality: each effect's samples and steps (0 Low, 1 Medium, 2 High; the periphery a tier below
// the focus view). The switches stay as they are.
inline constexpr IniInt kCarLightQuality{L"quad", L"car_light_quality", 2, 0, 2, 1};
// The composed output's largest size for the focus density (quad/output_density.h), percent of the native eye; the
// runtime's largest image caps it too. 100: composed at the native size, as before. Live.
inline constexpr IniInt kOutputDensityMax{L"quad", L"output_density_max", 400, 100, 400, 25};
inline constexpr IniInt kEyeOff{L"quad", L"eye_off", 0, 0, 2, 1};  // 0 both eyes on, 1 the left off, 2 the right
// [crop] (QVFR): fov_crop.h's kCropMax is the sides' 40.
inline constexpr IniInt kCropEnabled{L"crop", L"enabled", 0, 0, 1, 1};
inline constexpr IniInt kCropTop{L"crop", L"top", 0, 0, 40, 1};
inline constexpr IniInt kCropBottom{L"crop", L"bottom", 0, 0, 40, 1};
inline constexpr IniInt kCropOuter{L"crop", L"outer", 0, 0, 40, 1};
inline constexpr IniInt kCropInner{L"crop", L"inner", 0, 0, 40, 1};
inline constexpr IniInt kHotkeyFooter{L"menu", L"hotkey_footer", 1, 0, 1, 1};
// [cpu] (probe/cpu_isolation.h): 1 the game's main thread gets a core to itself, the game's other threads the rest; 0
// they share every core (the default). Live; not in a baseline or a log-only run. [experiment] cpu_isolate > 0 overrides it.
inline constexpr IniInt kCpuIsolateMain{L"cpu", L"isolate_main_thread", 0, 0, 1, 1};

// The key as written, `fallback` when missing; not clamped (0 keeps its "unset" meaning where a key has one).
// GetPrivateProfileInt, as every reader had it: a negative value reads as 0; a key whose range goes below 0 (the
// colour correction's) is read signed.
inline int ini_raw(const wchar_t* ini, const IniInt& k, int fallback) {
    if (k.min < 0)
        return ini_signed(ini, k.section, k.key, fallback);
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

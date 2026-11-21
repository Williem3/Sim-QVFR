// Road focus's cockpit-shaped focus area (the launcher's Foveation card and the headset menu's Foveation page): switching
// Road focus on ([quad] focus_corridor) keeps the player's own shape, size and position and sets the chosen cockpit
// preset ([quad] focus_corridor_preset: A or B); picking the other preset while it's on sets that one; switching it off
// puts the player's own back. Any later change to the shape, size or position is the player's (kept until Road focus
// goes off). Pure: ini keys and the focus shape only.
#pragma once

#include <algorithm>
#include <string>

#include "../ini.h"
#include "../ini_put.h"
#include "focus_offset.h"
#include "focus_shape.h"
#include "qvfr_settings.h"

namespace simqvfr {

// The cockpit outline, percent of the box from its centre (x right, y up), clockwise from the windscreen's top left: the
// windscreen's top between the pillars, the pillars out to the dash line a little below the middle (the horizon), the
// full width down over the dash and wheel. (A raised bottom with the passenger's footwell cut, and the user's own traced
// shape, were tried 2026-11-20; the user kept this one.)
inline FocusShape road_focus_cockpit_shape() {
    FocusShape s = focus_shape_of({{-34, 50}, {34, 50}, {50, -10}, {50, -50}, {-50, -50}, {-50, -10}});
    s.custom = true;
    return s;
}

// A preset: the outline, its size (percent of the view) and its place (degrees up and right, fixed foveation). Under
// native quad views the size is at most the runtime's own focus region and the place doesn't apply (the runtime places
// it). A: large, centred (the windscreen, dash and wheel). B: the user's tuning (2026-11-20), smaller and lower, the
// road and the dash.
struct RoadFocusPreset {
    FocusShape shape;
    int width = 0, height = 0;
    int offsetY = 0, offsetX = 0;
};
inline constexpr int kRoadFocusPresets = 2;
inline constexpr int kRoadFocusCockpitWidth = 70, kRoadFocusCockpitHeight = 60;  // preset A's size
inline RoadFocusPreset road_focus_preset(int index) {
    if (index == 1)
        return {road_focus_cockpit_shape(), 46, 43, -8, 0};
    return {road_focus_cockpit_shape(), kRoadFocusCockpitWidth, kRoadFocusCockpitHeight, 0, 0};
}

// The player's own focus area, kept while a Road focus preset is set ([quad] road_focus_saved and its keys).
struct RoadFocusSaved {
    bool have = false;
    FocusShape shape;
    int width = kFocusPercent.def, height = kFocusHeightPercent.def;
    int offsetY = 0, offsetX = 0;  // fixed foveation's place (fixed_offset_y, fixed_offset_x)
    bool operator==(const RoadFocusSaved&) const = default;
};

inline RoadFocusSaved read_road_focus_saved(const wchar_t* ini) {
    RoadFocusSaved r;
    r.have = GetPrivateProfileIntW(L"quad", L"road_focus_saved", 0, ini) != 0;
    if (!r.have)
        return r;
    wchar_t kind[16] = {};
    GetPrivateProfileStringW(L"quad", L"road_focus_saved_shape", L"box", kind, 16, ini);
    r.shape.custom = _wcsicmp(kind, L"points") == 0;
    wchar_t points[256] = {};
    GetPrivateProfileStringW(L"quad", L"road_focus_saved_points", L"", points, 256, ini);
    parse_focus_points(points, r.shape);
    r.width = kFocusPercent.clamp(static_cast<int>(GetPrivateProfileIntW(L"quad", L"road_focus_saved_width",
                                                                         kFocusPercent.def, ini)));
    r.height = kFocusHeightPercent.clamp(static_cast<int>(GetPrivateProfileIntW(L"quad", L"road_focus_saved_height",
                                                                               r.width, ini)));
    r.offsetY = ini_signed(ini, L"quad", L"road_focus_saved_offset_y", 0);
    r.offsetX = ini_signed(ini, L"quad", L"road_focus_saved_offset_x", 0);
    return r;
}

inline bool write_road_focus_saved(const wchar_t* ini, const RoadFocusSaved& r) {
    bool ok = ini_put(ini, L"quad", L"road_focus_saved", r.have ? 1 : 0);
    ok &= ini_put(ini, L"quad", L"road_focus_saved_shape", r.shape.custom ? L"points" : L"box");
    ok &= ini_put(ini, L"quad", L"road_focus_saved_points", focus_points_text(r.shape).c_str());
    ok &= ini_put(ini, L"quad", L"road_focus_saved_width", kFocusPercent.clamp(r.width));
    ok &= ini_put(ini, L"quad", L"road_focus_saved_height", kFocusHeightPercent.clamp(r.height));
    ok &= ini_put(ini, L"quad", L"road_focus_saved_offset_y", r.offsetY);
    ok &= ini_put(ini, L"quad", L"road_focus_saved_offset_x", r.offsetX);
    return ok;
}

// Preset `index` set: its shape, its size (no larger than maxWidth x maxHeight, the runtime's focus region under native
// quad views, percent; 0: no limit) and its place for fixed foveation.
inline void road_focus_apply(int index, FocusShape& shape, int& width, int& height, FocusOffset& offset, int maxWidth = 0,
                             int maxHeight = 0) {
    const RoadFocusPreset p = road_focus_preset(std::clamp(index, 0, kRoadFocusPresets - 1));
    shape = p.shape;
    width = maxWidth > 0 ? std::min(p.width, kFocusPercent.clamp(maxWidth)) : p.width;
    height = maxHeight > 0 ? std::min(p.height, kFocusHeightPercent.clamp(maxHeight)) : p.height;
    offset.fixed = p.offsetY;
    offset.fixedX = p.offsetX;
}

// Road focus switched (`on`: going on): on, the player's shape, size and place kept in `saved` and preset `index` set;
// off, the kept ones put back (nothing kept: left as they are).
inline void road_focus_switched(bool on, int index, FocusShape& shape, int& width, int& height, FocusOffset& offset,
                                RoadFocusSaved& saved, int maxWidth = 0, int maxHeight = 0) {
    if (on) {
        saved = {true, shape, width, height, offset.fixed, offset.fixedX};
        road_focus_apply(index, shape, width, height, offset, maxWidth, maxHeight);
    } else if (saved.have) {
        shape = saved.shape;
        width = saved.width;
        height = saved.height;
        offset.fixed = saved.offsetY;
        offset.fixedX = saved.offsetX;
        saved = {};
    }
}

}  // namespace simqvfr

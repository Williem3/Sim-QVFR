// Road focus's cockpit-shaped focus area (the launcher's Foveation card and the headset menu's Foveation page): switching
// Road focus on ([quad] focus_corridor) keeps the player's own shape and size and sets the cockpit outline as seen from
// the driver's seat (the windscreen between the A-pillars over the road, then the full width over the dash and wheel) at
// the size that holds it; switching it off puts the player's own back. Any later change to the shape or size is the
// player's (kept until Road focus goes off). Pure: ini keys and the focus shape only.
#pragma once

#include <algorithm>
#include <string>

#include "../ini.h"
#include "../ini_put.h"
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
// Its size, percent of the view; under native quad views at most the runtime's own focus region (what it shows of the
// focus view: road_focus_switched's maxWidth, maxHeight).
inline constexpr int kRoadFocusCockpitWidth = 70, kRoadFocusCockpitHeight = 60;

// The player's own focus area, kept while Road focus's cockpit shape is set ([quad] road_focus_saved and its keys).
struct RoadFocusSaved {
    bool have = false;
    FocusShape shape;
    int width = kFocusPercent.def, height = kFocusHeightPercent.def;
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
    return r;
}

inline bool write_road_focus_saved(const wchar_t* ini, const RoadFocusSaved& r) {
    bool ok = ini_put(ini, L"quad", L"road_focus_saved", r.have ? 1 : 0);
    ok &= ini_put(ini, L"quad", L"road_focus_saved_shape", r.shape.custom ? L"points" : L"box");
    ok &= ini_put(ini, L"quad", L"road_focus_saved_points", focus_points_text(r.shape).c_str());
    ok &= ini_put(ini, L"quad", L"road_focus_saved_width", kFocusPercent.clamp(r.width));
    ok &= ini_put(ini, L"quad", L"road_focus_saved_height", kFocusHeightPercent.clamp(r.height));
    return ok;
}

// Road focus switched (`on`: going on): on, the player's shape and size kept in `saved` and the cockpit's set, no larger
// than maxWidth x maxHeight (the runtime's focus region under native quad views, percent; 0: no limit); off, the kept
// ones put back (nothing kept: left as they are).
inline void road_focus_switched(bool on, FocusShape& shape, int& width, int& height, RoadFocusSaved& saved,
                                int maxWidth = 0, int maxHeight = 0) {
    if (on) {
        saved = {true, shape, width, height};
        shape = road_focus_cockpit_shape();
        width = maxWidth > 0 ? std::min(kRoadFocusCockpitWidth, kFocusPercent.clamp(maxWidth)) : kRoadFocusCockpitWidth;
        height = maxHeight > 0 ? std::min(kRoadFocusCockpitHeight, kFocusHeightPercent.clamp(maxHeight))
                               : kRoadFocusCockpitHeight;
    } else if (saved.have) {
        shape = saved.shape;
        width = saved.width;
        height = saved.height;
        saved = {};
    }
}

}  // namespace simqvfr

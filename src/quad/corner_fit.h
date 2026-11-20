// The sizes in effect on a corner path (the SCS games: src/games/scssoftware/corner.cpp): each eye drawn into the
// top-left of the game's own targets, its focus region beside it. Shared by that path and the headset menu, so the menu
// says what the path really draws (2026-11-19: a tester's Performance profile asked a 20% periphery, the path drew 25%
// and the menu said 20%).
//
// The focus region is the box x density of the targets: it keeps what it asks for first, up to the room beside a
// minimum corner and the targets' height; the corner (the periphery) takes what the region leaves, never less than
// kCornerMinPeriphery.
#pragma once

#include <algorithm>
#include <cmath>

namespace simqvfr {

inline constexpr int kCornerMinPeriphery = 25;  // percent of the eye each way: the corner's floor

struct CornerFit {
    int periphery = 0;              // percent of the eye each way, in effect
    int focusX = 0, focusY = 0;     // the box, percent of the eye (0: no focus view)
    double density = 1;             // the focus view's, a share of native, in effect
    bool peripheryRaised = false;   // asked below the floor: raised to it
    bool peripheryByRoom = false;   // asked more than the room the focus region leaves: held to it
    bool densityCapped = false;     // the region didn't fit beside the floor's corner or the targets' height
};

// `periphery`, `focusX`, `focusY` (0: no focus view; focusY 0: as focusX) and `density` in percent, as the menu has them.
inline CornerFit corner_fit(int periphery, int focusX, int focusY, int density) {
    CornerFit f;
    f.focusX = focusX ? std::clamp(focusX, 10, 60) : 0;
    f.focusY = f.focusX ? std::clamp(focusY > 0 ? focusY : f.focusX, 10, 80) : 0;
    const double asked = std::clamp(density, 25, 400) / 100.0;
    f.density = asked;
    if (f.focusX)
        f.density = std::min({asked, (100.0 - kCornerMinPeriphery) / f.focusX, 100.0 / f.focusY});
    f.densityCapped = f.density < asked;
    const int room = f.focusX ? static_cast<int>(std::floor(100 - f.focusX * f.density)) : 99;
    f.periphery = std::clamp(periphery, kCornerMinPeriphery, std::max(kCornerMinPeriphery, room));
    f.peripheryRaised = periphery < kCornerMinPeriphery;
    f.peripheryByRoom = periphery > f.periphery;
    return f;
}

}  // namespace simqvfr

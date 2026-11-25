// Two views of an eye in the eye's own render target (the "atlas"): the outer view at the top left, the focus view at the
// top right, both drawn in the eye's pass (multi-view) and composed into the whole target at the eye's resolve (rF2:
// games/rf2/). The pure rule of how the ini's sizes fit a target, no D3D: the game side lays its views out by it and the
// headset menu shows what it gives (settings_menu.cpp menu_fit), so the two cannot drift apart.
#pragma once

namespace simqvfr {

struct TwoViewAtlas {
    unsigned focusW = 0, focusH = 0, outerW = 0, outerH = 0;  // pixels in the target
    double outer = 0;          // the outer view's size as a share of the native eye (the target) per axis
    bool focusCapped = false;  // the focus view is smaller than asked (the target's width less kTwoViewOuterMinWidth)
    bool outerCapped = false;  // the outer view is smaller than asked: the focus view uses the width
};

inline constexpr unsigned kTwoViewOuterMinWidth = 320;  // target pixels always kept beside the focus view for the outer one
inline constexpr unsigned kTwoViewFocusMin = 64;        // the focus view's smallest side

// `width` x `height`: the eye's target, which is the native eye's size. `outerScale`: periphery_scale as a share of the
// native eye per axis. `focusW`/`focusH`: the focus view's size as shares of the native eye (focus density x the box's
// share of the field of view). The focus view comes first; the outer view takes the width left beside it, at the eye's
// aspect. A target too small for the rule gives an empty atlas (all zero).
inline TwoViewAtlas fit_two_view_atlas(unsigned width, unsigned height, double outerScale, double focusW, double focusH) {
    TwoViewAtlas f;
    if (width < kTwoViewOuterMinWidth + kTwoViewFocusMin || height < kTwoViewFocusMin)
        return f;
    const auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };
    const double maxFocusW = static_cast<double>(width - kTwoViewOuterMinWidth), maxFocusH = static_cast<double>(height);
    const double wantW = focusW * width + 0.5, wantH = focusH * height + 0.5;
    f.focusW = static_cast<unsigned>(clampd(wantW, kTwoViewFocusMin, maxFocusW));
    f.focusH = static_cast<unsigned>(clampd(wantH, kTwoViewFocusMin, maxFocusH));
    f.focusCapped = wantW > maxFocusW + 1.0 || wantH > maxFocusH + 1.0;
    const double room = static_cast<double>(width - f.focusW) / width;  // what is left of the width
    f.outer = clampd(outerScale, 0.10, room);
    f.outerCapped = outerScale > room + 1e-9;
    f.outerW = static_cast<unsigned>(f.outer * width);
    f.outerH = static_cast<unsigned>(f.outer * height);
    return f;
}

}  // namespace simqvfr

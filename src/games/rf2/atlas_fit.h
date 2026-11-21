#pragma once

// RF2's two-view atlas: how the ini's sizes fit the scene target. The pure rule, no D3D: the game side builds its layout
// from it (render_backend.h make_atlas_layout) and the headset menu shows what it gives (quad/settings_menu.cpp
// menu_fit), so the two cannot drift apart.

namespace simqvfr::games::rf2 {

constexpr unsigned kAtlasWidth = 3184, kAtlasHeight = 3096;  // the verified VR target (DXGI format 10, 4x MSAA)

struct AtlasFit {
    unsigned focusW = 0, focusH = 0, outerW = 0, outerH = 0;  // pixels in the target
    double outer = 0;                                         // the outer view's size as a share of the native eye
    bool focusCapped = false;  // the focus view is smaller than asked (the target's width, 320 px kept for the outer view)
    bool outerCapped = false;  // the outer view is smaller than asked: the focus view uses the width
};

// `outerScale`: periphery_scale as a share of the native eye per axis. `focusW`/`focusH`: the focus view's size as shares
// of the native eye (focus density x the box's share of the field of view). The focus view comes first; the outer view
// takes the width left beside it, at the eye's aspect.
inline AtlasFit fit_atlas(double outerScale, double focusW, double focusH) {
    const auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };
    const double maxFocusW = kAtlasWidth - 320.0, maxFocusH = static_cast<double>(kAtlasHeight);
    const double wantW = focusW * kAtlasWidth + 0.5, wantH = focusH * kAtlasHeight + 0.5;
    AtlasFit f;
    f.focusW = static_cast<unsigned>(clampd(wantW, 64.0, maxFocusW));
    f.focusH = static_cast<unsigned>(clampd(wantH, 64.0, maxFocusH));
    f.focusCapped = wantW > maxFocusW + 1.0 || wantH > maxFocusH + 1.0;
    const double room = static_cast<double>(kAtlasWidth - f.focusW) / kAtlasWidth;  // what is left of the width
    f.outer = clampd(outerScale, 0.10, room);
    f.outerCapped = outerScale > room + 1e-9;
    f.outerW = static_cast<unsigned>(f.outer * kAtlasWidth);
    f.outerH = static_cast<unsigned>(f.outer * kAtlasHeight);
    return f;
}

}  // namespace simqvfr::games::rf2

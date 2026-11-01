// views4 layout: where the four views sit in ACE's four-view render target. Views 0-1 (the outer,
// full-FOV eyes) and 2-3 (the focus views) each have their own size, side by side from x = 0 and
// top-aligned: 0 | 1 | 2 | 3. The target is 2 * (outer + focus) native eye widths wide and as high
// as the taller of the two, so rows below the shorter pair are unused.
//
// Sizes are linear scales of the runtime's native eye size: the outer views render at `outer` of
// native on both axes, the focus views at `focus` of the native width and `focusHeight` of the native
// height (their density times the focus box's share of the eye's tangent width and height). All
// three equal gives the old equal quarters.
// Everything is kept as fractions of the target, so it holds for any target ACE derives from the
// offered size (its own resolution setting, half-resolution passes).
#pragma once

#include <algorithm>
#include <cmath>

namespace aceqv {

struct Views4Layout {
    double outer = 1.0, focus = 1.0, focusHeight = 1.0;  // linear scale of the native eye width / height
    double width[4] = {0.25, 0.25, 0.25, 0.25};  // fraction of the target width
    double height[4] = {1, 1, 1, 1};             // fraction of the target height
    // `outer`, `focus` and `focusHeight` size ACE's target (at launch); live resolution (views4_live)
    // changes only `width` and `height`, laying the views out again inside that target.
    // The eye size ACE is offered, in native eye widths and heights (its double-wide target is two of these).
    double eye_width() const { return outer + focus; }
    double eye_height() const { return std::max(outer, focusHeight); }
    // Equal quarters of the target (the old layout, also after a live change).
    bool equal() const {
        for (int v = 0; v < 4; ++v)
            if (width[v] != 0.25 || height[v] != 1)
                return false;
        return true;
    }
};

// `focusHeight` < 0: the same as `focus` (a focus box with the same share on both axes).
inline Views4Layout views4_layout(double outer, double focus, double focusHeight = -1) {
    if (focusHeight < 0)
        focusHeight = focus;
    Views4Layout l;
    l.outer = outer;
    l.focus = focus;
    l.focusHeight = focusHeight;
    const double total = 2 * (outer + focus), tall = std::max(outer, focusHeight);
    for (int v = 0; v < 4; ++v) {
        l.width[v] = (v < 2 ? outer : focus) / total;
        l.height[v] = (v < 2 ? outer : focusHeight) / tall;
    }
    return l;
}

// Live resolution: sizes (the units of views4_layout) that fit the target ACE made at launch. The
// views stay one row, laid out again, so periphery and focus trade width:
//  - the row, 2 x (outer + focus), no wider than the target,
//  - no view taller than the target,
//  - no view larger than the largest launch view (the tile grid and the volumetrics are sized for it),
//  - each at least kMinLive of its launch size.
// Focus first: the periphery (square) takes the width the focus views leave.
inline constexpr double kMinLive = 0.1;
struct Views4Fit {
    double outer = 0, focus = 0, focusHeight = 0;  // in effect
    bool outerCapped = false, focusCapped = false;
    bool outerByRoom = false;  // the periphery is held by the focus views' width (less focus makes room)
};
inline Views4Fit views4_fit(const Views4Layout& launch, double outer, double focus, double focusHeight) {
    constexpr double kSlack = 1e-4;  // the launch sizes come from the same integers
    const double row = launch.outer + launch.focus, tall = launch.eye_height(),
                 largestW = std::max(launch.outer, launch.focus), minOuter = kMinLive * launch.outer;
    Views4Fit f;
    f.focus = std::clamp(focus, kMinLive * launch.focus, std::min(largestW, row - minOuter));
    f.focusHeight = std::clamp(focusHeight, kMinLive * launch.focusHeight, tall);
    f.focusCapped = f.focus < focus - kSlack || f.focusHeight < focusHeight - kSlack;
    const double ownLimit = std::min(largestW, tall), room = row - f.focus;
    f.outer = std::clamp(outer, minOuter, std::min(ownLimit, room));
    f.outerCapped = f.outer < outer - kSlack;
    f.outerByRoom = f.outerCapped && room < ownLimit;
    return f;
}

// The launch layout `launch` with its views at live sizes (views4_fit), side by side from x = 0.
inline Views4Layout views4_live(const Views4Layout& launch, double outer, double focus, double focusHeight) {
    const Views4Fit f = views4_fit(launch, outer, focus, focusHeight);
    Views4Layout l = launch;
    const double total = 2 * launch.eye_width(), tall = launch.eye_height();
    for (int v = 0; v < 4; ++v) {
        l.width[v] = (v < 2 ? f.outer : f.focus) / total;
        l.height[v] = (v < 2 ? f.outer : f.focusHeight) / tall;
    }
    return l;
}

// `l` with a frame's recorded view layout (`live`: recorded; otherwise `l` as it is).
inline Views4Layout views4_with_live(Views4Layout l, bool live, const double (&width)[4], const double (&height)[4]) {
    if (live)
        for (int v = 0; v < 4; ++v) {
            l.width[v] = width[v];
            l.height[v] = height[v];
        }
    return l;
}

// A view's pixel rect in a target of the given size. Widths are whole pixels and the views abut, so
// equal quarters reproduce the old `quarter = width / 4, left = view * quarter` exactly.
struct ViewRect {
    int left = 0, width = 0, height = 0;
};

inline ViewRect views4_rect(const Views4Layout& l, int view, int targetWidth, int targetHeight) {
    const auto pixels = [](double fraction, int size) { return static_cast<int>(std::floor(fraction * size + 1e-6)); };
    ViewRect r;
    for (int v = 0; v < view; ++v)
        r.left += pixels(l.width[v], targetWidth);
    r.width = std::max(1, pixels(l.width[view], targetWidth));
    r.height = std::max(1, pixels(l.height[view], targetHeight));
    return r;
}

// The rect as a texture region (u0, v0, du, dv) of a target of that size.
inline void views4_uv(const Views4Layout& l, int view, int targetWidth, int targetHeight, float (&uv)[4]) {
    const ViewRect r = views4_rect(l, view, targetWidth, targetHeight);
    uv[0] = static_cast<float>(r.left) / static_cast<float>(targetWidth);
    uv[1] = 0;
    uv[2] = static_cast<float>(r.width) / static_cast<float>(targetWidth);
    uv[3] = static_cast<float>(r.height) / static_cast<float>(targetHeight);
}

}  // namespace aceqv

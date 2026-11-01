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
    // Live resolution: view v renders in the top-left part of its slot, liveX[v] of the slot's width
    // and liveY[v] of its height (1 = the whole slot). The slots stay where the launch layout put
    // them, since ACE sizes its target once, so live sizes can only shrink (views4_live).
    double liveX[4] = {1, 1, 1, 1}, liveY[4] = {1, 1, 1, 1};
    // The eye size ACE is offered, in native eye widths and heights (its double-wide target is two of these).
    double eye_width() const { return outer + focus; }
    double eye_height() const { return std::max(outer, focusHeight); }
    bool full() const {
        for (int v = 0; v < 4; ++v)
            if (liveX[v] != 1 || liveY[v] != 1)
                return false;
        return true;
    }
    bool equal() const { return outer == focus && outer == focusHeight && full(); }
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

// The launch layout `launch` at live sizes (the same units as views4_layout), each view clamped to its
// slot: no larger than at launch, and at least kMinLive of it.
inline constexpr double kMinLive = 0.1;
inline Views4Layout views4_live(const Views4Layout& launch, double outer, double focus, double focusHeight) {
    Views4Layout l = launch;
    const auto share = [](double wanted, double slot) { return slot > 0 ? std::clamp(wanted / slot, kMinLive, 1.0) : 1.0; };
    for (int v = 0; v < 4; ++v) {
        l.liveX[v] = share(v < 2 ? outer : focus, v < 2 ? launch.outer : launch.focus);
        l.liveY[v] = share(v < 2 ? outer : focusHeight, v < 2 ? launch.outer : launch.focusHeight);
    }
    return l;
}

// `l` with the live sizes of another (a frame's record).
inline Views4Layout views4_with_live(Views4Layout l, const double (&liveX)[4], const double (&liveY)[4]) {
    for (int v = 0; v < 4; ++v) {
        l.liveX[v] = liveX[v];
        l.liveY[v] = liveY[v];
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
    r.width = pixels(l.width[view], targetWidth);
    r.height = pixels(l.height[view], targetHeight);
    if (l.liveX[view] != 1)
        r.width = std::max(1, static_cast<int>(std::floor(r.width * l.liveX[view] + 1e-6)));
    if (l.liveY[view] != 1)
        r.height = std::max(1, static_cast<int>(std::floor(r.height * l.liveY[view] + 1e-6)));
    return r;
}

// The view's whole slot (its rect at launch size, whatever the live sizes).
inline ViewRect views4_slot(Views4Layout l, int view, int targetWidth, int targetHeight) {
    l.liveX[view] = l.liveY[view] = 1;
    return views4_rect(l, view, targetWidth, targetHeight);
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

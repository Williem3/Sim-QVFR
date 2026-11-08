// views4 layout: where the four views sit in ACE's four-view render target. Views 0-1 (the outer,
// full-FOV eyes) and 2-3 (the focus views) each have their own size, side by side from x = 0 and
// top-aligned: 0 | 1 | 2 | 3. The target is 2 * (outer + focus) native eye widths wide and as high
// as the taller of the two, so rows below the shorter pair are unused.
//
// Sizes are linear scales of the runtime's native eye size: the outer views render at `outer` of
// native density over `cropWidth` x `cropHeight` of the eye (the FOV crop, fov_crop.h; 1 x 1 without
// one), the focus views at `focus` of the native width and `focusHeight` of the native
// height (their density times the focus box's share of the eye's tangent width and height). All
// three equal gives the old equal quarters.
// Everything is kept as fractions of the target, so it holds for any target ACE derives from the
// offered size (its own resolution setting, half-resolution passes).
#pragma once

#include <algorithm>
#include <cmath>

namespace simqvfr {

struct Views4Layout {
    double outer = 1.0, focus = 1.0, focusHeight = 1.0;  // linear scale of the native eye width / height
    double cropWidth = 1.0, cropHeight = 1.0;            // the outer views' kept share of the eye (FOV crop)
    double width[4] = {0.25, 0.25, 0.25, 0.25};  // fraction of the target width
    double height[4] = {1, 1, 1, 1};             // fraction of the target height
    // `width` and `height` come from a live layout (views4_live, views4_live_eye_columns, a frame's record):
    // eye columns (views4_rect_eye_columns) read them only then, and derive the launch layout otherwise.
    bool live = false;
    // `outer`, `focus` and `focusHeight` size ACE's target (at launch); live resolution (views4_live)
    // changes only `width` and `height`, laying the views out again inside that target.
    // Eye columns only: every view as wide as the outer view, so the focus views' horizontal density follows the
    // periphery (ACC before the instanced width per pair; [experiment] acc_shared_width=1 brings it back).
    bool sharedColumnWidth = false;
    // The eye size ACE is offered, in native eye widths and heights (its double-wide target is two of these).
    double eye_width() const { return outer_width() + focus; }
    double eye_height() const { return std::max(outer_height(), focusHeight); }
    // The outer views' size, in native eye widths and heights.
    double outer_width() const { return outer * cropWidth; }
    double outer_height() const { return outer * cropHeight; }
    // The eye size in eye columns (views4_rect_eye_columns): the wider of the outer and focus views (the outer view's
    // with a shared width), outer over focus height.
    double column_eye_width() const { return sharedColumnWidth ? outer_width() : std::max(outer_width(), focus); }
    double column_eye_height() const { return outer_height() + focusHeight; }
    // One view size (views4_live_one_size; separate views): every view a layer this large, in native eye widths and
    // heights: the larger of the outer and focus sizes, and at least `layerFloor` (LMU: what its menus render at, which
    // fill the layer; the scene passes then render inside it at their live sizes).
    double layerFloor = 0;
    // Room for a second view in the same image (LMU's multi-view rendering, multi_view / [experiment] lmu_mvr): the focus
    // view drawn beside its eye. secondViewBeside: the layer twice as wide, and at least twice the outer view's height.
    // The game renders an eye at one scale of the whole image on both axes (its post-processing follows that scale), so
    // the eye's region keeps the image's shape: at most half its width and half its height, the focus view beside it
    // in the other half of the width, as tall as the image. A vertically cropped eye (as wide as about twice its height)
    // fills its half with the image as high as the focus view; one without a crop makes the image twice as high (as
    // large as twice each way). Otherwise: twice as large each way.
    bool secondViewRoom = false, secondViewBeside = false;
    double layer_width() const { return (secondViewRoom ? 2 : 1) * std::max({outer_width(), focus, layerFloor}); }
    double layer_height() const {
        if (secondViewRoom && secondViewBeside)
            return std::max({2 * outer_height(), focusHeight, layerFloor});
        return (secondViewRoom ? 2 : 1) * std::max({outer_height(), focusHeight, layerFloor});
    }
    // The most of the layer one view may take: with room for a second view, half its width (and height, unless beside).
    double view_room_width() const { return layer_width() / (secondViewRoom ? 2 : 1); }
    double view_room_height() const { return layer_height() / (secondViewRoom && !secondViewBeside ? 2 : 1); }
    // The outer views' own: beside, half the height too (their one scale at most half the image's, so the focus view
    // still fits beside them: a live crop that makes them taller lowers their density instead).
    double outer_room_height() const { return view_room_height() / (secondViewRoom && secondViewBeside ? 2 : 1); }
    // In eye columns, view `view`'s width as a share of its column (1: the whole column).
    double column_share(int view) const {
        const double column = column_eye_width();
        if (sharedColumnWidth || !(column > 0))
            return 1.0;
        return (view < 2 ? outer_width() : focus) / column;
    }
    // Equal quarters of the target (the old layout, also after a live change).
    bool equal() const {
        for (int v = 0; v < 4; ++v)
            if (width[v] != 0.25 || height[v] != 1)
                return false;
        return true;
    }
};

// `focusHeight` < 0: the same as `focus` (a focus box with the same share on both axes).
// `cropWidth` / `cropHeight`: the outer views' kept share of the eye (FovCrop::kept_width / kept_height).
inline Views4Layout views4_layout(double outer, double focus, double focusHeight = -1, double cropWidth = 1,
                                  double cropHeight = 1) {
    if (focusHeight < 0)
        focusHeight = focus;
    Views4Layout l;
    l.outer = outer;
    l.focus = focus;
    l.focusHeight = focusHeight;
    l.cropWidth = cropWidth;
    l.cropHeight = cropHeight;
    const double total = 2 * l.eye_width(), tall = l.eye_height();
    for (int v = 0; v < 4; ++v) {
        l.width[v] = (v < 2 ? l.outer_width() : focus) / total;
        l.height[v] = (v < 2 ? l.outer_height() : focusHeight) / tall;
    }
    return l;
}

// Live resolution: sizes (the units of views4_layout) that fit the target ACE made at launch. The
// views stay one row, laid out again, so periphery and focus trade width:
//  - the row, 2 x (outer + focus), no wider than the target,
//  - no view taller than the target,
//  - no view larger than the largest launch view (the tile grid and the volumetrics are sized for it),
//  - each at least kMinLive of its launch size.
// Focus first: the periphery takes the width the focus views leave. `outer` is the periphery's
// density; its views are `cropWidth` x `cropHeight` of the eye (the live FOV crop; < 0: the launch's).
inline constexpr double kMinLive = 0.1;
struct Views4Fit {
    double outer = 0, focus = 0, focusHeight = 0;  // in effect (outer: the periphery's density)
    bool outerCapped = false, focusCapped = false;
    bool outerByRoom = false;  // the periphery is held by the focus views' width (less focus makes room)
};
inline Views4Fit views4_fit(const Views4Layout& launch, double outer, double focus, double focusHeight,
                            double cropWidth = -1, double cropHeight = -1) {
    constexpr double kSlack = 1e-4;  // the launch sizes come from the same integers
    const double cw = cropWidth > 0 ? cropWidth : launch.cropWidth, ch = cropHeight > 0 ? cropHeight : launch.cropHeight;
    const double row = launch.eye_width(), tall = launch.eye_height(),
                 largestW = std::max(launch.outer_width(), launch.focus), minOuter = kMinLive * launch.outer;
    Views4Fit f;
    f.focus = std::clamp(focus, kMinLive * launch.focus, std::min(largestW, row - minOuter * cw));
    f.focusHeight = std::clamp(focusHeight, kMinLive * launch.focusHeight, tall);
    f.focusCapped = f.focus < focus - kSlack || f.focusHeight < focusHeight - kSlack;
    // In density: the outer views no wider than the largest launch view nor taller than the target.
    const double ownLimit = std::min(largestW / cw, tall / ch), room = (row - f.focus) / cw;
    f.outer = std::clamp(outer, minOuter, std::min(ownLimit, room));
    f.outerCapped = f.outer < outer - kSlack;
    f.outerByRoom = f.outerCapped && room < ownLimit;
    return f;
}

// The launch layout `launch` with its views at live sizes (views4_fit), side by side from x = 0.
inline Views4Layout views4_live(const Views4Layout& launch, double outer, double focus, double focusHeight,
                               double cropWidth = -1, double cropHeight = -1) {
    const Views4Fit f = views4_fit(launch, outer, focus, focusHeight, cropWidth, cropHeight);
    const double cw = cropWidth > 0 ? cropWidth : launch.cropWidth, ch = cropHeight > 0 ? cropHeight : launch.cropHeight;
    Views4Layout l = launch;
    const double total = 2 * launch.eye_width(), tall = launch.eye_height();
    for (int v = 0; v < 4; ++v) {
        l.width[v] = (v < 2 ? f.outer * cw : f.focus) / total;
        l.height[v] = (v < 2 ? f.outer * ch : f.focusHeight) / tall;
    }
    l.live = true;
    return l;
}

// Live resolution in eye columns (views4_rect_eye_columns): sizes that fit the target the game made at launch (two
// column_eye_width() wide, column_eye_height() high). Instanced stereo draws each pair in one viewport from x = 0,
// halved (ACC: docs/recon-acc.md), so the two views of a pair share one width; each pair has its own (ACC's instanced
// width per pair), at most the column's. The outer views' rows and the focus views' rows share the target's height.
// Focus first: the periphery takes the height the focus views leave. With `sharedColumnWidth` all four views share the
// periphery's width instead, so the focus views' horizontal density follows the periphery and `focus` is unused.
inline Views4Fit views4_fit_eye_columns(const Views4Layout& launch, double outer, double focus, double focusHeight,
                                        double cropWidth = -1, double cropHeight = -1) {
    constexpr double kSlack = 1e-4;
    const double cw = cropWidth > 0 ? cropWidth : launch.cropWidth, ch = cropHeight > 0 ? cropHeight : launch.cropHeight;
    const double column = launch.column_eye_width(), tall = launch.column_eye_height();
    const double minOuterHeight = kMinLive * launch.outer_height();
    Views4Fit f;
    f.focusHeight = std::clamp(focusHeight, kMinLive * launch.focusHeight, tall - minOuterHeight);
    if (!launch.sharedColumnWidth)
        f.focus = std::clamp(focus, kMinLive * launch.focus, column);
    f.focusCapped = f.focusHeight < focusHeight - kSlack || (!launch.sharedColumnWidth && f.focus < focus - kSlack);
    const double byWidth = column / cw, byRoom = (tall - f.focusHeight) / ch;
    f.outer = std::clamp(outer, kMinLive * launch.outer, std::min(byWidth, byRoom));
    f.outerCapped = f.outer < outer - kSlack;
    f.outerByRoom = f.outerCapped && byRoom < byWidth;
    if (launch.sharedColumnWidth)
        f.focus = f.outer * cw;
    return f;
}

// The launch layout `launch` with its views at live sizes in eye columns (views4_fit_eye_columns): the outer views
// `width[0..1]` of the target wide and `height[0..1]` of it high from the top, the focus views `width[2..3]` by
// `height[2..3]` below.
inline Views4Layout views4_live_eye_columns(const Views4Layout& launch, double outer, double focus, double focusHeight,
                                            double cropWidth = -1, double cropHeight = -1) {
    const Views4Fit f = views4_fit_eye_columns(launch, outer, focus, focusHeight, cropWidth, cropHeight);
    const double cw = cropWidth > 0 ? cropWidth : launch.cropWidth, ch = cropHeight > 0 ? cropHeight : launch.cropHeight;
    Views4Layout l = launch;
    const double total = 2 * launch.column_eye_width(), tall = launch.column_eye_height();
    for (int v = 0; v < 4; ++v) {
        l.width[v] = std::min(0.5, (v < 2 ? f.outer * cw : f.focus) / total);
        l.height[v] = (v < 2 ? f.outer * ch : f.focusHeight) / tall;
    }
    l.live = true;
    return l;
}

// Live resolution with one view size (qvfr_one_view_size: X-Plane): the game's layers keep their launch size, the larger
// of the outer and focus views per axis, and each kind of view renders in a rect of its own size inside them. So the
// periphery and the focus views are sized independently, neither trading room with the other:
//  - each no larger than the launch layer, scaled down evenly on both axes when it is (its density stays the same
//    across and down),
//  - each at least kMinLive of its launch size.
// `outer` is the periphery's density over `cropWidth` x `cropHeight` of the eye (< 0: the launch's); Views4Fit's
// outerByRoom stays false (nothing shares the room).
inline Views4Fit views4_fit_one_size(const Views4Layout& launch, double outer, double focus, double focusHeight,
                                     double cropWidth = -1, double cropHeight = -1) {
    constexpr double kSlack = 1e-4;
    const double cw = cropWidth > 0 ? cropWidth : launch.cropWidth, ch = cropHeight > 0 ? cropHeight : launch.cropHeight;
    const double layerW = launch.view_room_width(), layerH = launch.view_room_height();  // one view's most
    Views4Fit f;
    f.outer = std::clamp(outer, kMinLive * launch.outer, std::min(layerW / cw, launch.outer_room_height() / ch));
    f.outerCapped = f.outer < outer - kSlack;
    const double fit = std::min({1.0, focus > 0 ? layerW / focus : 1.0, focusHeight > 0 ? layerH / focusHeight : 1.0});
    f.focus = std::clamp(focus * fit, kMinLive * launch.focus, layerW);
    f.focusHeight = std::clamp(focusHeight * fit, kMinLive * launch.focusHeight, layerH);
    f.focusCapped = f.focus < focus - kSlack || f.focusHeight < focusHeight - kSlack;
    return f;
}

// The launch layout `launch` with its views at live sizes with one view size (views4_fit_one_size). The target is two
// layers wide and two high (each eye's column: its view over its focus view), so width[v] and height[v] are half the
// view's share of its layers (0.5: the whole layer); the views sit at their layers' top-left.
inline Views4Layout views4_live_one_size(const Views4Layout& launch, double outer, double focus, double focusHeight,
                                         double cropWidth = -1, double cropHeight = -1) {
    const Views4Fit f = views4_fit_one_size(launch, outer, focus, focusHeight, cropWidth, cropHeight);
    const double cw = cropWidth > 0 ? cropWidth : launch.cropWidth, ch = cropHeight > 0 ? cropHeight : launch.cropHeight;
    const double layerW = launch.layer_width(), layerH = launch.layer_height();
    Views4Layout l = launch;
    for (int v = 0; v < 4; ++v) {
        l.width[v] = 0.5 * std::min(1.0, (v < 2 ? f.outer * cw : f.focus) / layerW);
        l.height[v] = 0.5 * std::min(1.0, (v < 2 ? f.outer * ch : f.focusHeight) / layerH);
    }
    l.live = true;
    return l;
}

// `l` with a frame's recorded view layout (`live`: recorded; otherwise `l` as it is).
inline Views4Layout views4_with_live(Views4Layout l, bool live, const double (&width)[4], const double (&height)[4]) {
    if (live) {
        for (int v = 0; v < 4; ++v) {
            l.width[v] = width[v];
            l.height[v] = height[v];
        }
        l.live = true;
    }
    return l;
}

// A view's pixel rect in a target of the given size. Widths are whole pixels and the views abut, so
// equal quarters reproduce the old `quarter = width / 4, left = view * quarter` exactly.
struct ViewRect {
    int left = 0, width = 0, height = 0;
    int top = 0;  // 0 but in eye columns (views4_rect_eye_columns)
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

// The same views in eye columns: the outer views across the top of the target and the focus views below, 0 | 1 over
// 2 | 3. For a game that renders with instanced stereo (ACC, X-Plane). Instanced stereo draws a pair (the eyes, then
// the focus views) in one viewport from x = 0, halved: so each pair sits side by side from x = 0, both its views one
// width. The target is two column_eye_width() wide and column_eye_height() high; the wider pair fills it, its views
// meeting in the middle, the narrower one leaves the right of its rows unused (ACC's runtime, OpenComposite, copies
// the whole texture into each eye's swapchain, so a view needn't sit in its eye's half). With `sharedColumnWidth` every
// view is half the target wide. A live layout (`l.live`, views4_live_eye_columns): every view width[v] of the target
// wide, each pair side by side from x = 0, the outer views height[0] of it from the top, the focus views height[2]
// below them.
inline ViewRect views4_rect_eye_columns(const Views4Layout& l, int view, int targetWidth, int targetHeight) {
    const auto pixels = [](double fraction, int size) { return static_cast<int>(std::floor(fraction * size + 1e-6)); };
    if (l.live) {
        const int width = std::clamp(pixels(l.width[view], targetWidth), 1, targetWidth / 2);
        const int outerRows = std::clamp(pixels(l.height[0], targetHeight), 1, targetHeight - 1);
        ViewRect r;
        r.left = (view & 1) * width;
        r.width = width;
        if (view < 2) {
            r.height = outerRows;
        } else {
            r.top = outerRows;
            r.height = std::clamp(pixels(l.height[view], targetHeight), 1, targetHeight - outerRows);
        }
        return r;
    }
    const int half = targetWidth / 2;
    const double outerRows = l.outer_height() / l.column_eye_height();
    ViewRect r;
    r.width = std::clamp(pixels(l.column_share(view), half), 1, std::max(1, half));
    r.left = (view & 1) * r.width;
    if (view < 2) {
        r.height = std::max(1, pixels(outerRows, targetHeight));
    } else {
        r.top = pixels(outerRows, targetHeight);
        r.height = std::max(1, targetHeight - r.top);
    }
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

}  // namespace simqvfr

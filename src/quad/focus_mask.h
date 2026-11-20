// The focus mask: the focus view's texels the compositor never shows (outside the focus area's shape: focus_shape.h's
// free-form outline or the rounded box), as a mesh a game draws into the focus view's depth before its scene, so the
// scene's draws skip them (early depth). The compositor gives a texel weight saturate(inside / feather) of how far inside
// the shape it is (compose_shader.h inside_box): zero where that is negative, in both compositors and under native quad
// views (where our layer blends the outer view into the focus view's edge itself). The mesh covers only texels more than
// a margin outside the shape, so nothing that is shown, nor what DLSS, sharpening or ambient occlusion read next to it,
// is lost. Header-only, for tests/focus_mask_test.cpp. Measured on LMU 2026-11-15 (30% of a 40% box: -2.0 ms GPU).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "focus_shape.h"

namespace simqvfr {

// The focus area as the compositor composes it: the free-form outline's points (focus_shape_points: shares of the box,
// across from the left and down from the top; none: the box), the corner radius (a share of half the box's shorter
// side; an outline wins over it, as in the shader) and the box's width over its height in tangents (its aspect).
struct FocusArea {
    float point[2 * kFocusPointsMax] = {};
    int points = 0;
    float radius = 0;
    float aspect = 1;
    // The part of the box the headset shows (shares of the box: left, top, right, bottom): under native quad views the
    // runtime's own focus region, where a box larger than it hangs past (its texels never seen); else all of it.
    float visible[4] = {0, 0, 1, 1};
    bool outline() const { return points >= kFocusPointsMin; }
    bool clipped() const { return visible[0] > 0 || visible[1] > 0 || visible[2] < 1 || visible[3] < 1; }
    bool operator==(const FocusArea&) const = default;
};

// The area of an outline's words (FocusShapeWords) and the box's corner radius (unused with an outline).
inline FocusArea focus_area_from(const FocusShapeWords& words, double cornerRadius) {
    FocusArea a;
    a.points = focus_shape_points(words, a.point);
    a.radius = a.outline() ? 0.0f : static_cast<float>(std::clamp(cornerRadius, 0.0, 1.0));
    return a;
}

// compose_shader.h's inside_box on the CPU: how far inside the focus area the point f is (0..1 across the box, y down),
// in the shader's own measure; negative outside. Kept equal to the shader's (tests/focus_mask_test.cpp holds the two
// side by side).
inline double focus_area_inside(const FocusArea& a, double fx, double fy) {
    if (a.outline()) {
        // The outline: the distance to its nearest edge in shares of each axis, its sign from the crossings of a ray
        // to the right (inside_poly).
        const int n = std::min(a.points, kFocusPointsMax);
        double d = 1e9;
        bool outside = true;
        for (int j = 0; j < n; ++j) {
            const int k = (j + 1) % n;
            const double ax = a.point[2 * j], ay = a.point[2 * j + 1];
            const double ex = a.point[2 * k] - ax, ey = a.point[2 * k + 1] - ay;
            const double wx = fx - ax, wy = fy - ay;
            const double len = ex * ex + ey * ey;
            const double t = len > 1e-12 ? std::clamp((wx * ex + wy * ey) / len, 0.0, 1.0) : 0.0;
            const double bx = wx - ex * t, by = wy - ey * t;
            d = std::min(d, bx * bx + by * by);
            if ((ay > fy) != (ay + ey > fy) && fx < ax + ex * (fy - ay) / ey)
                outside = !outside;
        }
        return outside ? -std::sqrt(d) : std::sqrt(d);
    }
    if (a.radius <= 0)
        return std::min(std::min(fx, 1 - fx), std::min(fy, 1 - fy));
    // The rounded box in tangent proportions (only the aspect matters: the result is over the shorter side).
    const double sx = a.aspect >= 1 ? a.aspect : 1.0, sy = a.aspect >= 1 ? 1.0 : 1.0 / a.aspect;
    const double hx = sx / 2, hy = sy / 2;
    const double r = a.radius * std::min(hx, hy);
    const double qx = std::fabs((fx - 0.5) * sx) - hx + r, qy = std::fabs((fy - 0.5) * sy) - hy + r;
    const double d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
    return -d / std::min(sx, sy);
}

// The margin in focus_area_inside's measure for `texels` focus view texels: its distances are in shares of the box
// (each axis's own for an outline, the shorter side for rounding), so one share is at least the shorter side's
// texels; the margin is at least `texels` texels in every direction.
inline double focus_mask_margin(unsigned texels, unsigned width, unsigned height) {
    const unsigned shorter = std::min(width, height);
    return shorter ? static_cast<double>(texels) / shorter : 1.0;
}

namespace focus_mask_detail {

struct P {
    double x, y;
};
inline P operator+(P a, P b) { return {a.x + b.x, a.y + b.y}; }
inline P operator-(P a, P b) { return {a.x - b.x, a.y - b.y}; }
inline P operator*(P a, double s) { return {a.x * s, a.y * s}; }
// A point of the box (0..1, y down) as the focus view's clip space: x right, y up.
inline void push(std::vector<float>& ndc, P f) {
    ndc.push_back(static_cast<float>(2 * f.x - 1));
    ndc.push_back(static_cast<float>(1 - 2 * f.y));
}
inline void push_quad(std::vector<float>& ndc, P a, P b, P c, P d) {  // a b c d around: two triangles
    for (P p : {a, b, c, a, c, d})
        push(ndc, p);
}

// The free-form outline: the box cut into a grid, every cell all of whose points are more than `margin` outside the
// outline (its centre that far plus half its diagonal: focus_area_inside's distance moves no faster than the point), in
// runs along each row, rows with the same runs as one. Any outline, concave or crossing itself; what it gives up is
// under a cell along the edge. Runs reaching the box's edge go past it (the viewport clips), so no texel there is missed.
inline constexpr int kOutlineMeshGrid = 192;
inline bool outline_mesh(const FocusArea& a, double margin, std::vector<float>& ndc) {
    constexpr int kGrid = kOutlineMeshGrid;
    constexpr double kCell = 1.0 / kGrid;
    const double reach = margin + kCell * 0.70710678118654752;
    std::vector<std::pair<int, int>> open, row;  // runs [first, last + 1) of the rows since `openRow`
    int openRow = 0;
    const auto flush = [&](int endRow) {
        for (const auto& [x0, x1] : open) {
            const double l = x0 == 0 ? -1.0 : x0 * kCell, r = x1 == kGrid ? 2.0 : x1 * kCell;
            const double t = openRow == 0 ? -1.0 : openRow * kCell, b = endRow == kGrid ? 2.0 : endRow * kCell;
            push_quad(ndc, P{l, t}, P{r, t}, P{r, b}, P{l, b});
        }
    };
    for (int y = 0; y < kGrid; ++y) {
        row.clear();
        int start = -1;
        for (int x = 0; x <= kGrid; ++x) {
            const bool masked = x < kGrid && focus_area_inside(a, (x + 0.5) * kCell, (y + 0.5) * kCell) < -reach;
            if (masked && start < 0)
                start = x;
            if (!masked && start >= 0) {
                row.emplace_back(start, x);
                start = -1;
            }
        }
        if (row != open) {
            flush(y);
            open = row;
            openRow = y;
        }
    }
    flush(kGrid);
    return !ndc.empty();
}

// The rounded box: at each corner, the region past the circle of radius r + margin around the corner's arc centre
// (inside the box only there can a point be more than `margin` outside the rounded edge). The arc is followed by a
// polygon round the circle (its edges touch the circle at their middles, so it stays outside it), out to points far
// past the box; the viewport clips what lies outside the box. In tangent proportions (the shader's), back to the box.
inline bool rounded_mesh(const FocusArea& a, double margin, std::vector<float>& ndc) {
    const double sx = a.aspect >= 1 ? a.aspect : 1.0, sy = a.aspect >= 1 ? 1.0 : 1.0 / a.aspect;
    const double hx = sx / 2, hy = sy / 2;
    const double r = a.radius * std::min(hx, hy);
    const double reach = r + margin;  // in the shorter side's shares, as the shader's distance (shorter side = 1)
    if (reach >= r * std::sqrt(2.0) - 1e-9)
        return false;  // the margin covers the whole corner: nothing to mask
    constexpr int kSegments = 12;
    constexpr double kQuarter = 1.5707963267948966;
    const double step = kQuarter / kSegments, outer = reach / std::cos(step / 2), distant = 2 * (sx + sy);
    for (int corner = 0; corner < 4; ++corner) {
        const double mx = corner == 1 || corner == 2 ? 1 : -1, my = corner >= 2 ? 1 : -1;  // y down: top is -1
        const P centre{mx * (hx - r), my * (hy - r)};
        const auto box = [&](P s) { return P{s.x / sx + 0.5, s.y / sy + 0.5}; };
        for (int k = 0; k < kSegments; ++k) {  // the polygon's corners at the segment ends, `outer` from the centre
            const double t0 = k * step, t1 = (k + 1) * step;
            const P d0{mx * std::cos(t0), my * std::sin(t0)}, d1{mx * std::cos(t1), my * std::sin(t1)};
            push_quad(ndc, box(centre + d0 * outer), box(centre + d0 * distant), box(centre + d1 * distant), box(centre + d1 * outer));
        }
    }
    return true;
}

}  // namespace focus_mask_detail

// The area of a custom shape through `points` (focus_shape_of).
inline FocusArea focus_area_points(std::initializer_list<FocusPoint> points) {
    return focus_area_from(focus_shape_words(focus_shape_of(points)), 0);
}

// The focus area of the settings: the free-form outline (focus_shape.h) or, without one, the box rounded by
// corner_radius_percent, as the layer hands it to the game (qvfr_set_focus_mask).
inline FocusArea focus_area_of(const FocusShape& shape, int cornerRadiusPercent) {
    return focus_area_from(focus_shape_words(shape), std::clamp(cornerRadiusPercent, 0, 100) / 100.0);
}

// The share (0..1) of a width x height focus view the mask skips: its texels more than `marginTexels` outside the shape
// (what the mesh covers; sampled on a grid). The aspect is the view's (square texels). 0 for the plain box.
inline double focus_mask_share(FocusArea area, unsigned width, unsigned height, unsigned marginTexels) {
    if (!width || !height || (!area.outline() && area.radius <= 0 && !area.clipped()))
        return 0;
    area.aspect = static_cast<float>(width) / static_cast<float>(height);
    const double margin = focus_mask_margin(marginTexels, width, height);
    constexpr int kGrid = 64;
    int masked = 0;
    const bool shape = area.outline() || area.radius > 0;
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x) {
            const double fx = (x + 0.5) / kGrid, fy = (y + 0.5) / kGrid;
            const bool unseen = fx < area.visible[0] - margin || fy < area.visible[1] - margin ||
                                fx > area.visible[2] + margin || fy > area.visible[3] + margin;
            masked += unseen || (shape && focus_area_inside(area, fx, fy) < -margin) ? 1 : 0;
        }
    return static_cast<double>(masked) / (kGrid * kGrid);
}

// The focus mask's triangles (a list, x and y per vertex, the focus view's clip space: x right, y up, the box +-1) for
// `area` with `margin` (focus_mask_margin). Empty: nothing to mask (the plain box, a margin past the rounding, a shape it
// can't follow). Points past the box are the viewport's to clip.
inline std::vector<float> focus_mask_mesh(const FocusArea& area, double margin) {
    std::vector<float> ndc;
    bool ok = false;
    if (area.outline())
        ok = focus_mask_detail::outline_mesh(area, margin, ndc);
    else if (area.radius > 0)
        ok = focus_mask_detail::rounded_mesh(area, margin, ndc);
    if (!ok)
        ndc.clear();
    if (area.clipped()) {  // what the runtime never shows, `margin` out from it, past the box (the viewport clips)
        using focus_mask_detail::P;
        const double l = area.visible[0] - margin, t = area.visible[1] - margin;
        const double r = area.visible[2] + margin, b = area.visible[3] + margin;
        if (l > 0)
            focus_mask_detail::push_quad(ndc, P{-1, -1}, P{l, -1}, P{l, 2}, P{-1, 2});
        if (r < 1)
            focus_mask_detail::push_quad(ndc, P{r, -1}, P{2, -1}, P{2, 2}, P{r, 2});
        if (t > 0)
            focus_mask_detail::push_quad(ndc, P{l, -1}, P{r, -1}, P{r, t}, P{l, t});
        if (b < 1)
            focus_mask_detail::push_quad(ndc, P{l, b}, P{r, b}, P{r, 2}, P{l, 2});
    }
    return ndc;
}

}  // namespace simqvfr

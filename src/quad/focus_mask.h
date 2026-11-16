// The focus mask: the focus view's texels the compositor never shows (outside the focus area's shape: focus_shape.h's
// pulled-in corners or the rounded box), as a mesh a game draws into the focus view's depth before its scene, so the
// scene's draws skip them (early depth). The compositor gives a texel weight saturate(inside / feather) of how far inside
// the shape it is (compose_shader.h inside_box): zero where that is negative, in both compositors and under native quad
// views (where our layer blends the outer view into the focus view's edge itself). The mesh covers only texels more than
// a margin outside the shape, so nothing that is shown, nor what DLSS, sharpening or ambient occlusion read next to it,
// is lost. Header-only, for tests/focus_mask_test.cpp. Measured on LMU 2026-11-15 (30% of a 40% box: -2.0 ms GPU).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "focus_shape.h"

namespace simqvfr {

// The focus area as the compositor composes it: focus_shape_insets' insets (per corner, FocusCorner's order, `in` then
// `vertical`, shares of the box; all 0: no pulled-in corners), the corner radius (a share of half the box's shorter side;
// pulled-in corners win over it, as in the shader) and the box's width over its height in tangents (its aspect).
struct FocusArea {
    float inset[2 * kFocusCorners] = {};
    float radius = 0;
    float aspect = 1;
    bool corners() const {
        for (float v : inset)
            if (v > 0)
                return true;
        return false;
    }
    bool operator==(const FocusArea&) const = default;
};

// compose_shader.h's inside_box on the CPU: how far inside the focus area the point f is (0..1 across the box, y down),
// in the shader's own measure; negative outside. Kept equal to the shader's (tests/focus_mask_test.cpp holds the two
// side by side).
inline double focus_area_inside(const FocusArea& a, double fx, double fy) {
    if (a.corners()) {
        // The quadrilateral of the pulled-in corners: the distance to its nearest edge in shares of each axis, its sign
        // from the crossings of a ray to the right (inside_quad).
        double cx[4], cy[4];
        for (int i = 0; i < 4; ++i) {
            const double in = a.inset[2 * i], v = a.inset[2 * i + 1];
            cx[i] = i == 1 || i == 2 ? 1 - in : in;
            cy[i] = i >= 2 ? 1 - v : v;
        }
        double d = 1e9;
        bool outside = true;
        for (int j = 0; j < 4; ++j) {
            const double ax = cx[j], ay = cy[j], ex = cx[(j + 1) & 3] - ax, ey = cy[(j + 1) & 3] - ay;
            const double wx = fx - ax, wy = fy - ay;
            const double t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0, 1.0);
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
// (each axis's own for pulled-in corners, the shorter side for rounding), so one share is at least the shorter side's
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
inline double cross(P a, P b) { return a.x * b.y - a.y * b.x; }
inline double dot(P a, P b) { return a.x * b.x + a.y * b.y; }

// Where the ray from `o` along `d` meets the line through a and b (d not parallel to it).
inline bool ray_line(P o, P d, P a, P b, P& out) {
    const P e = b - a;
    const double den = cross(d, e);
    if (std::fabs(den) < 1e-12)
        return false;
    out = o + d * (cross(a - o, e) / den);
    return true;
}

// A point of the box (0..1, y down) as the focus view's clip space: x right, y up.
inline void push(std::vector<float>& ndc, P f) {
    ndc.push_back(static_cast<float>(2 * f.x - 1));
    ndc.push_back(static_cast<float>(1 - 2 * f.y));
}
inline void push_quad(std::vector<float>& ndc, P a, P b, P c, P d) {  // a b c d around: two triangles
    for (P p : {a, b, c, a, c, d})
        push(ndc, p);
}

// The pulled-in corners: the region outside the quadrilateral pushed out by `margin` (each edge's line moved out, the
// corners where they meet: it holds every point within `margin` of the quadrilateral). Seen from the box's centre, which
// it contains, the pushed-out quadrilateral is star-shaped (each corner stays in its own quarter), so the region splits
// by the rays through its corners: per edge, the strip between it and points far past the box (the viewport clips what
// lies outside the box), cut along evenly turned rays into four-sided pieces.
inline bool corner_mesh(const FocusArea& a, double margin, std::vector<float>& ndc) {
    P c[4];
    for (int i = 0; i < 4; ++i) {
        const double in = a.inset[2 * i], v = a.inset[2 * i + 1];
        c[i] = {i == 1 || i == 2 ? 1 - in : in, i >= 2 ? 1 - v : v};
    }
    double area = 0;
    for (int j = 0; j < 4; ++j)
        area += cross(c[j], c[(j + 1) & 3]);
    const double side = area > 0 ? 1 : -1;  // the outward normal of edge e: side * (e.y, -e.x)
    P n[4];
    for (int j = 0; j < 4; ++j) {
        const P e = c[(j + 1) & 3] - c[j];
        const double len = std::hypot(e.x, e.y);
        if (len < 1e-9)
            return false;
        n[j] = P{e.y, -e.x} * (side / len);
    }
    P q[4];  // the pushed-out corners: where the moved lines of edges j - 1 and j meet
    for (int j = 0; j < 4; ++j) {
        const int k = (j + 3) & 3;
        const P a0 = c[k] + n[k] * margin, a1 = c[j] + n[k] * margin;
        const P b0 = c[j] + n[j] * margin, b1 = c[(j + 1) & 3] + n[j] * margin;
        if (!ray_line(a0, a1 - a0, b0, b1, q[j]))
            q[j] = b0;  // the two edges in line
    }
    const P centre{0.5, 0.5};
    for (int j = 0; j < 4; ++j)  // star-shaped: each edge turns the same way round the centre, by under half a turn
        if (cross(q[j] - centre, q[(j + 1) & 3] - centre) * side <= 0)
            return false;
    // Each edge's sector cut into kRays even turns; the far points kFar from the centre (the box's corners are 0.71
    // away, and a cut of under 180 / kRays degrees keeps its far chord past them).
    constexpr int kRays = 8;
    constexpr double kFar = 4.0;
    for (int j = 0; j < 4; ++j) {
        const P qa = q[j], qb = q[(j + 1) & 3];
        const P da = qa - centre, db = qb - centre;
        const double start = std::atan2(da.y, da.x);
        const double sweep = std::atan2(cross(da, db), dot(da, db));  // signed, under half a turn
        P inner = qa, outer = centre + da * (kFar / std::hypot(da.x, da.y));
        for (int s = 1; s <= kRays; ++s) {
            P nextInner = qb, nextOuter;
            if (s < kRays) {
                const double t = start + sweep * s / kRays;
                const P d{std::cos(t), std::sin(t)};
                nextOuter = centre + d * kFar;
                if (!ray_line(centre, d, qa, qb, nextInner))
                    return false;
            } else {
                nextOuter = centre + db * (kFar / std::hypot(db.x, db.y));
            }
            push_quad(ndc, inner, outer, nextOuter, nextInner);
            inner = nextInner;
            outer = nextOuter;
        }
    }
    return true;
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

// The focus area of the settings: the pulled-in corners (focus_shape.h) or, without them, the box rounded by
// corner_radius_percent, as the layer hands it to the game (qvfr_set_focus_mask).
inline FocusArea focus_area_of(const FocusShape& shape, int cornerRadiusPercent) {
    FocusArea a;
    focus_shape_insets(focus_shape_bits(shape), a.inset);
    a.radius = a.corners() ? 0.0f : static_cast<float>(std::clamp(cornerRadiusPercent, 0, 100)) / 100.0f;
    return a;
}

// The share (0..1) of a width x height focus view the mask skips: its texels more than `marginTexels` outside the shape
// (what the mesh covers; sampled on a grid). The aspect is the view's (square texels). 0 for the plain box.
inline double focus_mask_share(FocusArea area, unsigned width, unsigned height, unsigned marginTexels) {
    if (!width || !height || (!area.corners() && area.radius <= 0))
        return 0;
    area.aspect = static_cast<float>(width) / static_cast<float>(height);
    const double margin = focus_mask_margin(marginTexels, width, height);
    constexpr int kGrid = 64;
    int masked = 0;
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x)
            masked += focus_area_inside(area, (x + 0.5) / kGrid, (y + 0.5) / kGrid) < -margin ? 1 : 0;
    return static_cast<double>(masked) / (kGrid * kGrid);
}

// The focus mask's triangles (a list, x and y per vertex, the focus view's clip space: x right, y up, the box +-1) for
// `area` with `margin` (focus_mask_margin). Empty: nothing to mask (the plain box, a margin past the rounding, a shape it
// can't follow). Points past the box are the viewport's to clip.
inline std::vector<float> focus_mask_mesh(const FocusArea& area, double margin) {
    std::vector<float> ndc;
    bool ok = false;
    if (area.corners())
        ok = focus_mask_detail::corner_mesh(area, margin, ndc);
    else if (area.radius > 0)
        ok = focus_mask_detail::rounded_mesh(area, margin, ndc);
    if (!ok)
        ndc.clear();
    return ndc;
}

}  // namespace simqvfr

// X-Plane's focus projections (README.md, "focus stage"): a focus view looks the same way as its eye with a narrower
// frustum, a box of the eye's tangent span. Built from the eye's own projection matrix, so X-Plane's conventions (axis
// signs, reversed-Z depth) carry over: only the x and y rows change.
//
// With row 0 = (a, 0, b, 0) and w = -z, x_ndc = a*t - b for the view-space tangent t = x / -z, so the eye spans
// t in [(-1 + b) / a, (1 + b) / a] (either order, if a < 0). A focus box of half-width h around tangent t0 has
// x_ndc' = sign(a) * (t - t0) / h, i.e. row 0 = (sign(a) / h, 0, sign(a) * t0 / h, 0). Likewise row 1 for y.
#pragma once

namespace xplane {

// The 4x4 double matrix's layout in memory, found from where the -1 of w = -z sits: index 11 column-major, 14
// row-major. False: neither (not a perspective projection this code understands).
bool projection_layout(const double (&m)[16], bool& columnMajor);

// The eye's tangent span from its projection: left/right (x) and down/up (y), each pair in ascending order.
bool projection_tangents(const double (&m)[16], double (&tan)[4]);

// `out` = the focus projection: `fraction` (0-1] of the eye's tangent width and `fractionY` of its height, centred on
// tangents (cx, cy). Rows 2-3 (depth, w) as the eye's. False (and `out` = `eye`): not a projection it understands.
bool focus_projection(const double (&eye)[16], double fraction, double fractionY, double cx, double cy, double (&out)[16]);

}  // namespace xplane

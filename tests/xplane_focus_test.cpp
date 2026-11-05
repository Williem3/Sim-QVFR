// Offline check of X-Plane's focus projections (src/games/xplane/focus_projection.h), on the left eye's projection
// from a capture (P00 0.7648, P02 -0.0722, P11 0.9163; reversed-Z infinite).
#include <cmath>
#include <cstdio>

#include "games/xplane/focus_projection.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

// x_ndc of view-space point (x, y, z) through matrix m in the given layout.
double ndc(const double (&m)[16], bool cm, int row, double x, double y, double z) {
    auto e = [&](int r, int c) { return cm ? m[c * 4 + r] : m[r * 4 + c]; };
    const double v = e(row, 0) * x + e(row, 1) * y + e(row, 2) * z + e(row, 3);
    const double w = e(3, 0) * x + e(3, 1) * y + e(3, 2) * z + e(3, 3);
    return v / w;
}

}  // namespace

int main() {
    // The captured left eye, logical rows: (0.7648, 0, -0.0722, 0), (0, 0.9163, 0, 0), (0, 0, 0, 0.0081), (0, 0, -1, 0).
    const double rows[4][4] = {{0.7648, 0, -0.0722, 0}, {0, 0.9163, 0, 0}, {0, 0, 0, 0.0081}, {0, 0, -1, 0}};
    for (int layout = 0; layout < 2; ++layout) {
        const bool cm = layout == 1;
        double eye[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                eye[cm ? c * 4 + r : r * 4 + c] = rows[r][c];
        bool found = !cm;
        check(xplane::projection_layout(eye, found) && found == cm, cm ? "column-major found" : "row-major found");
        double tan[4];
        check(xplane::projection_tangents(eye, tan) && near(tan[0], -1.4019, 1e-3) && near(tan[1], 1.2131, 1e-3) &&
                  near(tan[2], -1.0913, 1e-3) && near(tan[3], 1.0913, 1e-3),
              "eye tangents: left -1.402 (tan 0.9512), right 1.213 (tan 0.8814), +-1.091 (tan 0.829)");
        double focus[16];
        check(xplane::focus_projection(eye, 0.2, 0.25, 0.0, 0.0, focus), "focus projection built");
        // A focus box 20% of the width (half-width 0.2615) and 25% of the height around tangent 0: its edges map to ndc
        // -1/+1, its centre to 0, depth and w unchanged.
        const double hx = 0.5 * 0.2 * (1.2131 + 1.4019), hy = 0.5 * 0.25 * (2 * 1.0913);
        check(near(ndc(focus, cm, 0, -hx, 0, -1), -1.0, 2e-3) && near(ndc(focus, cm, 0, hx, 0, -1), 1.0, 2e-3) &&
                  near(ndc(focus, cm, 0, 0, 0, -1), 0.0),
              "x: focus edges at ndc -1/+1, forward at 0");
        check(near(ndc(focus, cm, 1, 0, -hy, -1), -1.0, 2e-3) && near(ndc(focus, cm, 1, 0, hy, -1), 1.0, 2e-3),
              "y: focus edges at ndc -1/+1");
        check(near(ndc(focus, cm, 2, 0, 0, -5), ndc(eye, cm, 2, 0, 0, -5)), "depth unchanged");
        double off[16];
        xplane::focus_projection(eye, 0.2, 0.2, 0.3, -0.1, off);
        check(near(ndc(off, cm, 0, 0.3, 0, -1), 0.0) && near(ndc(off, cm, 1, 0, -0.1, -1), 0.0), "an off-centre focus box");
    }
    double flat[16] = {};
    double out[16];
    check(!xplane::focus_projection(flat, 0.2, 0.2, 0, 0, out), "not a projection: refused");
    std::printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: pass");
    return failures ? 1 : 0;
}

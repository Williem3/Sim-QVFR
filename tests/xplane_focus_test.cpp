// Offline check of X-Plane's focus projections (src/games/xplane/focus_projection.h), on the left eye's projection
// from a capture (P00 0.7648, P02 -0.0722, P11 0.9163; reversed-Z infinite).
#include <cmath>
#include <cstdio>

#include "games/xplane/focus_projection.h"
#include "quad/views4_layout.h"

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
    {
        // Live resolution with one view size (X-Plane): launch periphery 1.0, focus 0.3 x 0.3 at density 1.5 (0.45 of
        // the native eye): the layers 1.0 x 1.0, the outer views the whole layer, the focus views 45% of it.
        const simqvfr::Views4Layout launch = simqvfr::views4_layout(1.0, 0.45, 0.45);
        const simqvfr::Views4Layout atLaunch = simqvfr::views4_live_one_size(launch, 1.0, 0.45, 0.45);
        check(atLaunch.live && near(2 * atLaunch.width[0], 1.0) && near(2 * atLaunch.height[0], 1.0) &&
                  near(2 * atLaunch.width[2], 0.45) && near(2 * atLaunch.height[3], 0.45),
              "one size at the launch sizes: the launch shares");
        // A lower periphery doesn't give the focus views room, nor they it: each sized on its own.
        const simqvfr::Views4Layout low = simqvfr::views4_live_one_size(launch, 0.6, 0.45, 0.45);
        check(near(2 * low.width[0], 0.6) && near(2 * low.height[1], 0.6) && near(2 * low.width[2], 0.45),
              "one size, periphery 60%: the outer views 60% of their layers, the focus views as before");
        const simqvfr::Views4Fit big = simqvfr::views4_fit_one_size(launch, 1.0, 0.8, 0.6);
        check(!big.outerCapped && !big.focusCapped && near(big.focus, 0.8) && near(big.focusHeight, 0.6),
              "one size, a larger focus box within the layer: as asked");
        // Larger than the layer: capped at it, evenly on both axes (the density stays square).
        const simqvfr::Views4Fit over = simqvfr::views4_fit_one_size(launch, 1.4, 1.2, 0.9);
        check(over.outerCapped && near(over.outer, 1.0) && over.focusCapped && near(over.focus, 1.0) &&
                  near(over.focusHeight, 0.75) && !over.outerByRoom,
              "one size, both above the layer: each capped at it, the focus views' aspect kept");
        const simqvfr::Views4Fit tiny = simqvfr::views4_fit_one_size(launch, 0.01, 0.01, 0.01);
        check(near(tiny.outer, 0.1) && near(tiny.focus, 0.045) && near(tiny.focusHeight, 0.045),
              "one size, at least kMinLive of the launch sizes");
        // The periphery the smaller view (a large fixed focus box): the layers its size, the periphery a share.
        const simqvfr::Views4Layout fixed = simqvfr::views4_layout(0.5, 0.9, 0.9);
        const simqvfr::Views4Layout fixedLive = simqvfr::views4_live_one_size(fixed, 0.5, 0.9, 0.9);
        check(near(2 * fixedLive.width[0], 0.5 / 0.9) && near(2 * fixedLive.width[2], 1.0),
              "one size, the periphery smaller: its share of the focus-sized layers");
        // A layer floor (LMU's menus at native): periphery 0.3, focus 0.3 x 0.3 inside native-size layers.
        simqvfr::Views4Layout floored = simqvfr::views4_layout(0.3, 0.3, 0.3);
        floored.layerFloor = 1.0;
        check(near(floored.layer_width(), 1.0) && near(floored.layer_height(), 1.0) && near(fixed.layer_width(), 0.9),
              "a layer floor sizes the layers (none: the larger view)");
        const simqvfr::Views4Layout flooredLive = simqvfr::views4_live_one_size(floored, 0.3, 0.3, 0.3);
        check(near(2 * flooredLive.width[0], 0.3) && near(2 * flooredLive.height[1], 0.3) &&
                  near(2 * flooredLive.width[2], 0.3) && near(2 * flooredLive.height[3], 0.3),
              "with the floor the launch sizes are their share of the layer");
        const simqvfr::Views4Fit up = simqvfr::views4_fit_one_size(floored, 0.8, 0.6, 0.6);
        check(!up.outerCapped && near(up.outer, 0.8) && !up.focusCapped && near(up.focus, 0.6),
              "with the floor the views can grow live up to the layer");
        simqvfr::Views4Layout croppedFloor = simqvfr::views4_layout(0.3, 0.3, 0.3, 0.8, 0.6);
        croppedFloor.layerFloor = 1.0;
        const simqvfr::Views4Layout croppedLive = simqvfr::views4_live_one_size(croppedFloor, 0.3, 0.3, 0.3);
        check(near(2 * croppedLive.width[0], 0.24) && near(2 * croppedLive.height[0], 0.18),
              "with the floor and a crop the outer views keep their density over the cropped FOV");
    }
    double flat[16] = {};
    double out[16];
    check(!xplane::focus_projection(flat, 0.2, 0.2, 0, 0, out), "not a projection: refused");
    std::printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: pass");
    return failures ? 1 : 0;
}

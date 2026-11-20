// Offline check of the SCS games' depth terms from a scene draw's world-view-projection (games/scssoftware/fixation.h):
// whatever the world matrix (turned, scaled, moved) and whichever way the rows lie, depth = a + b / w comes out as the
// projection's; a matrix that isn't a perspective view's is passed over.
#include <cmath>
#include <cstdio>

#include "games/scssoftware/fixation.h"
#include "test_util.h"

namespace {

using M = double[4][4];

void mul(const M& a, const M& b, M& out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            out[r][c] = 0;
            for (int k = 0; k < 4; ++k)
                out[r][c] += a[r][k] * b[k][c];
        }
}

void to_floats(const M& m, bool transpose, float (&out)[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r][c] = static_cast<float>(transpose ? m[c][r] : m[r][c]);
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // A reversed finite projection, near 0.1 m, far 5000 m, w = the view's forward distance: depth = A + B / w.
    const double n = 0.1, f = 5000, A = -n / (f - n), B = n * f / (f - n);
    const M projection = {{1.2, 0, 0.05, 0}, {0, 1.1, -0.03, 0}, {0, 0, A, B}, {0, 0, 1, 0}};
    // The camera turned 30 deg about y and moved; an object scaled 2.5 and moved.
    const double c = std::cos(0.5236), s = std::sin(0.5236);
    const M view = {{c, 0, -s, 3}, {0, 1, 0, -1.5}, {s, 0, c, 12}, {0, 0, 0, 1}};
    const M world = {{2.5, 0, 0, 100}, {0, 2.5, 0, 4}, {0, 0, 2.5, -40}, {0, 0, 0, 1}};
    M vw, wvp;
    mul(view, world, vw);
    mul(projection, vw, wvp);

    float rows[4][4], columns[4][4];
    to_floats(wvp, false, rows);
    to_floats(wvp, true, columns);
    double a = 0, b = 0;
    check(scs::fixation_depth_terms(rows, true, a, b) && std::fabs(a - A) < 1e-6 && std::fabs(b - B) < 1e-6,
          "a scene draw's world-view-projection by rows: the projection's depth terms");
    a = b = 0;
    check(scs::fixation_depth_terms(columns, false, a, b) && std::fabs(a - A) < 1e-6 && std::fabs(b - B) < 1e-6,
          "the same matrix transposed, read by columns");
    check(!scs::fixation_depth_terms(columns, true, a, b), "the transposed matrix read by rows: passed over");

    float worldOnly[4][4];
    to_floats(world, false, worldOnly);
    check(!scs::fixation_depth_terms(worldOnly, true, a, b), "a world matrix (no perspective): passed over");

    // A reversed infinite projection (UE-like): depth = near / w.
    const M infinite = {{1.2, 0, 0, 0}, {0, 1.1, 0, 0}, {0, 0, 0, 0.05}, {0, 0, 1, 0}};
    M wvp2;
    mul(infinite, vw, wvp2);
    float rows2[4][4];
    to_floats(wvp2, false, rows2);
    check(scs::fixation_depth_terms(rows2, true, a, b) && std::fabs(a) < 1e-7 && std::fabs(b - 0.05) < 1e-6,
          "a reversed infinite projection: a 0, b the near plane");
    return test_result();
}

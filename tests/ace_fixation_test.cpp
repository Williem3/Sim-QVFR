// Offline check of the API path's depth terms from a focus view's inverse projection (games/ace/replay_fixation.cpp):
// whatever the projection (reversed finite or infinite, off-axis as the focus remap makes it), depth = a + b / w comes
// out as the projection's, and the centre ray's tangents as the view's off-axis centre; a matrix that isn't a
// perspective view's is passed over.
#include <cmath>

#include "games/ace/replay_probe.h"
#include "test_util.h"

namespace {

using M = double[4][4];

// The 4x4 inverse by cofactors (the test's own, not the code's).
bool inverse(const M& m, M& out) {
    double inv[16], a[16];
    for (int i = 0; i < 16; ++i)
        a[i] = m[i / 4][i % 4];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-15)
        return false;
    for (int i = 0; i < 16; ++i)
        out[i / 4][i % 4] = inv[i] / det;
    return true;
}

// The tile culling's layout: q[i][k] = inverse[k * 4 + i].
void to_layout(const M& q, float (&out)[16]) {
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            out[k * 4 + i] = static_cast<float>(q[i][k]);
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // A reversed finite projection, view looking down -z (w = -z), near 0.1 m, far 5000 m, off-axis like a focus view
    // aimed 0.3 to the right and 0.1 down: x' = sx x + cx w.
    const double n = 0.1, f = 5000, A = n / (f - n), B = n * f / (f - n);
    const double sx = 4.0, sy = 4.4, cx = -0.3 * 4.0, cy = 0.1 * 4.4;
    // clip = P . (x, y, z, 1), w = -z; x/w = sx (x/-z) + cx: at NDC 0 the ray's tangent is -cx / sx.
    const M p = {{sx, 0, -cx, 0}, {0, sy, -cy, 0}, {0, 0, -A, B}, {0, 0, -1, 0}};
    M q;
    check(inverse(p, q), "the test projection inverts");
    float layout[16];
    to_layout(q, layout);
    double a = 0, b = 0, tx = 0, ty = 0;
    check(simqvfr::replay_fixation_terms(layout, a, b, tx, ty) && std::fabs(a - A) < 1e-6 && std::fabs(b - B) < 1e-6,
          "a reversed finite focus projection: its depth terms");
    check(std::fabs(std::fabs(tx) - 0.3) < 1e-4 && std::fabs(std::fabs(ty) - 0.1) < 1e-4,
          "the off-axis centre ray's tangents");

    // A reversed infinite projection: depth = near / w.
    const M pi = {{sx, 0, 0, 0}, {0, sy, 0, 0}, {0, 0, 0, 0.05}, {0, 0, -1, 0}};
    M qi;
    check(inverse(pi, qi), "the infinite projection inverts");
    to_layout(qi, layout);
    check(simqvfr::replay_fixation_terms(layout, a, b, tx, ty) && std::fabs(a) < 1e-7 && std::fabs(b - 0.05) < 1e-6,
          "a reversed infinite projection: a 0, b the near plane");

    // An orthographic matrix's inverse: passed over.
    const M ortho = {{0.1, 0, 0, 0}, {0, 0.1, 0, 0}, {0, 0, 0.001, 0.5}, {0, 0, 0, 1}};
    M qo;
    check(inverse(ortho, qo), "the orthographic matrix inverts");
    to_layout(qo, layout);
    check(!simqvfr::replay_fixation_terms(layout, a, b, tx, ty), "an orthographic view: passed over");
    return test_result();
}

// Offline check of the gaze turn applied to ACE's row-vector view matrices (quad/focus_turn.h): a
// point along the gaze must land on the turned camera's forward axis, the per-view inverse must stay
// the view's inverse, the compositor's rotation must bring the gaze to the focus centre, and the
// clamp must keep the focus box inside the eye's view.
#include <cmath>
#include <cstdio>

#include "quad/focus_turn.h"

using namespace simqvfr;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    failures += !ok;
}

XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    float m[9];
    to_matrix(q, m);
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// p (row) * M (row-major 4x4), w = 1.
void transform(const float (&m)[16], const float (&p)[3], float (&out)[3]) {
    for (int c = 0; c < 3; ++c)
        out[c] = p[0] * m[0 * 4 + c] + p[1] * m[1 * 4 + c] + p[2] * m[2 * 4 + c] + m[3 * 4 + c];
}

// The row-vector view matrix of a camera at `position` with orientation `q` (camera to world).
void view_matrix(const XrQuaternionf& q, const float (&position)[3], float (&v)[16]) {
    float r[9];
    to_matrix(q, r);  // columns: the camera's axes in the world
    // p_cam = R^T (p - t): as rows, p * R - t * R; the 3x3 block is R itself.
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c)
            v[row * 4 + c] = r[row * 3 + c];
    for (int c = 0; c < 3; ++c)
        v[3 * 4 + c] = -(position[0] * r[0 * 3 + c] + position[1] * r[1 * 3 + c] + position[2] * r[2 * 3 + c]);
    v[3] = v[7] = v[11] = 0;
    v[15] = 1;
}

}  // namespace

int main() {
    const XrFovf fov{-0.9512f, 0.8814f, 0.8290f, -0.8290f};  // a Pimax-like left eye
    const double fraction = 0.35;
    // An eye looking somewhere arbitrary, away from the origin.
    const XrQuaternionf eye = qmul(XrQuaternionf{0, 0.3826834f, 0, 0.9238795f}, XrQuaternionf{0.0871557f, 0, 0, 0.9961947f});
    const float position[3] = {2.5f, 1.2f, -7.0f};
    float v[16];
    view_matrix(eye, position, v);

    // Gaze 20 degrees right and 10 up of the eye's forward axis.
    const double tanX = std::tan(0.349), tanY = std::tan(0.1745);
    const XrQuaternionf turn = focus_turn(fov, fraction, tanX, tanY);
    float turned[16];
    std::memcpy(turned, v, sizeof(v));
    turn_view_matrix(turned, turn);

    // A point 5 m along the gaze ray (in the eye's frame (tanX, tanY, -1)), into the world.
    const XrVector3f gazeEye{static_cast<float>(tanX), static_cast<float>(tanY), -1};
    const float len = std::sqrt(gazeEye.x * gazeEye.x + gazeEye.y * gazeEye.y + 1);
    const XrVector3f gw = rotate(eye, {gazeEye.x * 5 / len, gazeEye.y * 5 / len, -5 / len});
    const float world[3] = {position[0] + gw.x, position[1] + gw.y, position[2] + gw.z};
    float cam[3];
    transform(turned, world, cam);
    std::printf("gaze point in the turned camera: %.4f %.4f %.4f\n", cam[0], cam[1], cam[2]);
    expect(std::fabs(cam[0]) < 1e-3f && std::fabs(cam[1]) < 1e-3f && cam[2] < -4.99f && cam[2] > -5.01f,
           "gaze lands on the turned camera's forward axis at 5 m");

    // The untouched view still sees that point at the gaze tangents.
    float before[3];
    transform(v, world, before);
    expect(std::fabs(before[0] / -before[2] - tanX) < 1e-3 && std::fabs(before[1] / -before[2] - tanY) < 1e-3,
           "unturned view sees the point at the gaze tangents");

    // Inverse view (camera to world, row-vector): turned by R^T it must still invert the turned view.
    float inverse[16];
    {
        float r[9];
        to_matrix(eye, r);
        for (int row = 0; row < 3; ++row)
            for (int c = 0; c < 3; ++c)
                inverse[row * 4 + c] = r[c * 3 + row];  // R^T
        inverse[3] = inverse[7] = inverse[11] = 0;
        for (int c = 0; c < 3; ++c)
            inverse[12 + c] = position[c];
        inverse[15] = 1;
    }
    turn_inverse_view_matrix(inverse, turn);
    float back[3];
    transform(inverse, cam, back);
    expect(std::fabs(back[0] - world[0]) < 1e-3f && std::fabs(back[1] - world[1]) < 1e-3f &&
               std::fabs(back[2] - world[2]) < 1e-3f,
           "turned inverse view maps the camera point back to the world");

    // The compositor's rotation (outer frame -> focus frame) takes the gaze direction to (0, 0, -1).
    {
        float m[9];
        to_matrix(qconj(turn), m);
        const XrVector3f g{gazeEye.x / len, gazeEye.y / len, -1 / len};
        const XrVector3f f{m[0] * g.x + m[1] * g.y + m[2] * g.z, m[3] * g.x + m[4] * g.y + m[5] * g.z,
                           m[6] * g.x + m[7] * g.y + m[8] * g.z};
        expect(std::fabs(f.x) < 1e-4f && std::fabs(f.y) < 1e-4f && f.z < -0.9999f,
               "compositor rotation puts the gaze at the focus centre");
    }

    // No roll: the turned camera's x axis stays horizontal in the eye's frame (its y component is 0).
    {
        const XrVector3f x = rotate(turn, {1, 0, 0});
        expect(std::fabs(x.y) < 1e-5f, "turned camera has no roll");
    }

    // Clamp: a gaze far outside the view keeps the turned focus box's centre inside, box inside the view.
    {
        const XrQuaternionf outside = focus_turn(fov, fraction, 5.0, -5.0);
        const double l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
        const double u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
        const XrVector3f c = rotate(outside, {0, 0, -1});
        const double cx = c.x / -c.z, cy = c.y / -c.z, hw = (r - l) * fraction / 2, hh = (u - d) * fraction / 2;
        std::printf("clamped centre %.4f,%.4f (limits x %.4f..%.4f, y %.4f..%.4f)\n", cx, cy, l + hw, r - hw, d + hh,
                    u - hh);
        expect(cx <= r - hw + 1e-4 && cx >= l + hw - 1e-4 && cy >= d + hh - 1e-4 && cy <= u - hh + 1e-4,
               "clamped centre inside the allowed range");
    }

    // Centred gaze: no turn at all.
    {
        const XrQuaternionf none = focus_turn(fov, fraction, 0, 0);
        expect(std::fabs(none.x) < 1e-6f && std::fabs(none.y) < 1e-6f && std::fabs(none.z) < 1e-6f,
               "centred gaze gives no turn");
    }

    // Rendered tangents read back from a projection in ACE's convention, for several headsets: the
    // user's (vertically symmetric, dumped in gate run 18), a vertically asymmetric one, a wide one.
    {
        const float headsets[][4] = {
            {-1.40195f, 1.21310f, 1.09131f, -1.09131f},
            {-0.94f, 0.80f, 0.80f, -0.94f},
            {-2.10f, 1.60f, 1.20f, -1.45f},
        };
        bool all = true;
        for (const auto& t : headsets) {
            float p[16] = {};
            p[0] = 2 / (t[1] - t[0]);
            p[8] = (t[1] + t[0]) / (t[1] - t[0]);
            p[5] = 2 / (t[2] - t[3]);
            p[9] = (t[2] + t[3]) / (t[2] - t[3]);
            p[11] = -1;
            p[14] = 0.1f;
            float back[4];
            const bool ok = projection_tangents(p, back);
            for (int i = 0; i < 4; ++i)
                all &= ok && std::fabs(back[i] - t[i]) < 1e-5f;
        }
        // The dumped eye-0 projection itself.
        float dumped[16] = {};
        dumped[0] = 0.76480f;
        dumped[5] = 0.91633f;
        dumped[8] = -0.07222f;
        float back[4];
        all &= projection_tangents(dumped, back) && std::fabs(back[0] + 1.40195f) < 1e-3f &&
               std::fabs(back[1] - 1.21310f) < 1e-3f && std::fabs(back[2] - 1.09131f) < 1e-3f;
        float none[16] = {};
        all &= !projection_tangents(none, back);
        expect(all, "projection tangents round-trip for three headsets and the dumped projection");

        // The tangent form of focus_turn clamps exactly like the XrFovf form.
        const XrFovf asym{std::atan(-0.94f), std::atan(0.80f), std::atan(0.80f), std::atan(-0.94f)};
        float t[4];
        fov_tangents(asym, t);
        bool same = true;
        for (const double g : {-3.0, -0.5, 0.0, 0.4, 3.0}) {
            const XrQuaternionf a = focus_turn(asym, 0.3, g, -g), b = focus_turn(t, 0.3, g, -g);
            same &= std::fabs(a.x - b.x) < 1e-6f && std::fabs(a.y - b.y) < 1e-6f && std::fabs(a.w - b.w) < 1e-6f;
        }
        const XrFovf round = tangents_fov(t);
        same &= std::fabs(round.angleLeft - asym.angleLeft) < 1e-6f && std::fabs(round.angleDown - asym.angleDown) < 1e-6f;
        expect(same, "focus_turn on tangents matches focus_turn on the FOV; tangents_fov inverts fov_tangents");

        // A wide, short box (60% of the width, 20% of the height): the clamp uses each axis's own half-extent.
        const float sym[4] = {-1, 1, 1, -1};
        const XrQuaternionf corner = focus_turn(sym, 0.6, 0.2, 5.0, 5.0);
        const XrVector3f c = rotate(corner, {0, 0, -1});
        const double cx = c.x / -c.z, cy = c.y / -c.z;
        std::printf("non-square clamp centre %.4f,%.4f (limits 0.4, 0.8)\n", cx, cy);
        double hw, hh;
        focus_extents(sym, 0.6, 0.2, hw, hh);
        expect(std::fabs(cx - 0.4) < 1e-4 && std::fabs(cy - 0.8) < 1e-4 && std::fabs(hw - 0.6) < 1e-9 && std::fabs(hh - 0.2) < 1e-9,
               "separate width and height fractions clamp and size each axis on its own");
    }

    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}

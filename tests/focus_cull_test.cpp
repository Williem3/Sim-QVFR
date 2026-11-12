// Offline check of the focus-view cull (quad/focus_cull.h): spheres and boxes inside, outside and
// straddling each side plane, behind the camera, margin widening, and frusta of gaze-turned cameras
// built from ACE's row-vector view matrices (quad/focus_turn.h), against a direct projection test.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "quad/focus_cull.h"
#include "quad/focus_turn.h"

using namespace simqvfr;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    failures += !ok;
}

// p (row) * M (row-major 4x4), w = 1.
void transform(const float (&m)[16], const float (&p)[3], float (&out)[3]) {
    for (int c = 0; c < 3; ++c)
        out[c] = p[0] * m[0 * 4 + c] + p[1] * m[1 * 4 + c] + p[2] * m[2 * 4 + c] + m[3 * 4 + c];
}

// The row-vector view matrix of a camera at `position` with orientation `q` (as focus_turn_test).
void view_matrix(const XrQuaternionf& q, const float (&position)[3], float (&v)[16]) {
    float r[9];
    to_matrix(q, r);
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c)
            v[row * 4 + c] = r[row * 3 + c];
    for (int c = 0; c < 3; ++c)
        v[3 * 4 + c] = -(position[0] * r[0 * 3 + c] + position[1] * r[1 * 3 + c] + position[2] * r[2 * 3 + c]);
    v[3] = v[7] = v[11] = 0;
    v[15] = 1;
}

bool sphere_out(const FocusFrustum& f, float x, float y, float z, float r) {
    const float c[3] = {x, y, z};
    return sphere_outside(f, c, r);
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // An unturned camera at the origin looking down -z, half-extents 0.5 x 0.25.
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const FocusFrustum f = focus_frustum(identity, 0.5, 0.25);
    expect(!sphere_out(f, 0, 0, -10, 0.1f), "centred sphere is inside");
    expect(sphere_out(f, 6, 0, -10, 0.1f), "sphere right of the box is outside");
    expect(sphere_out(f, -6, 0, -10, 0.1f), "sphere left of the box is outside");
    expect(sphere_out(f, 0, 3, -10, 0.1f), "sphere above the box is outside");
    expect(sphere_out(f, 0, -3, -10, 0.1f), "sphere below the box is outside");
    expect(!sphere_out(f, 5.2f, 0, -10, 0.5f), "sphere straddling the right plane is kept");
    expect(!sphere_out(f, 0, -2.6f, -10, 0.5f), "sphere straddling the bottom plane is kept");
    expect(sphere_out(f, 0, 0, 10, 1.0f), "sphere behind the camera is outside");
    expect(!sphere_out(f, 0, 0, 0.5f, 1.0f), "sphere around the camera is kept");

    // Margin: widening the extents by 50% brings the right-hand sphere in.
    const FocusFrustum wide = focus_frustum(identity, 0.5 * 1.5, 0.25 * 1.5);
    expect(!sphere_out(wide, 6, 0, -10, 0.1f), "margin keeps a sphere just outside the box");
    expect(sphere_out(wide, 8, 0, -10, 0.1f), "margin still culls beyond it");

    // Boxes: outside, straddling, and a box near the corner whose closest point is outside.
    {
        const float out[3] = {6, 0, -10}, in[3] = {5.4f, 0, -10}, corner[3] = {5.6f, 2.8f, -10};
        const float e[3] = {0.3f, 0.3f, 0.3f};
        expect(box_outside(f, out, e), "box right of the box is outside");
        expect(!box_outside(f, in, e), "box straddling the right plane is kept");
        expect(box_outside(f, corner, e), "box beyond the top-right corner is outside");
    }

    // A turned focus camera of an arbitrary eye: the cull agrees with projecting points through the
    // turned matrix (|x| <= hw * -z, |y| <= hh * -z), so it follows ACE's row-vector convention.
    const XrFovf fov{-0.9512f, 0.8814f, 0.8290f, -0.8290f};
    const double fraction = 0.35;
    const double hw = (std::tan(fov.angleRight) - std::tan(fov.angleLeft)) * fraction / 2;
    const double hh = (std::tan(fov.angleUp) - std::tan(fov.angleDown)) * fraction / 2;
    const XrQuaternionf eye = qmul(XrQuaternionf{0, 0.3826834f, 0, 0.9238795f}, XrQuaternionf{0.0871557f, 0, 0, 0.9961947f});
    const float position[3] = {2.5f, 1.2f, -7.0f};
    const double gazes[][2] = {{0, 0}, {std::tan(0.349), std::tan(0.1745)}, {-0.5, -0.4}, {0.7, 0.6}};
    for (const auto& g : gazes) {
        float v[16];
        view_matrix(eye, position, v);
        turn_view_matrix(v, focus_turn(fov, fraction, g[0], g[1]));
        const FocusFrustum t = focus_frustum(v, hw, hh);
        int agree = 0, total = 0, inside = 0;
        unsigned seed = 12345;
        const auto next = [&] {
            seed = seed * 1103515245u + 12345u;
            return static_cast<float>((seed >> 8) & 0xffff) / 65535.0f * 2 - 1;  // -1..1
        };
        for (int i = 0; i < 20000; ++i) {
            const float p[3] = {position[0] + next() * 20, position[1] + next() * 20, position[2] + next() * 20};
            float cam[3];
            transform(v, p, cam);
            const float depth = -cam[2];
            const float sx = std::fabs(cam[0]) - static_cast<float>(hw) * depth;
            const float sy = std::fabs(cam[1]) - static_cast<float>(hh) * depth;
            if (std::fabs(sx) < 1e-3f || std::fabs(sy) < 1e-3f)
                continue;  // on a plane: either answer is fine
            const bool projectedInside = depth > 0 && sx < 0 && sy < 0;
            const bool culled = sphere_outside(t, p, 0.0f);
            agree += projectedInside != culled;
            inside += projectedInside;
            ++total;
        }
        char what[128];
        std::snprintf(what, sizeof(what), "gaze %.2f,%.2f: cull matches the projection (%d/%d agree, %d inside)", g[0],
                      g[1], agree, total, inside);
        expect(agree == total && inside > 0, what);

        // The point 10 m along the turned camera's axis is inside.
        float r[9];  // the turned camera's axes in the world (columns): eye * turn
        to_matrix(qmul(eye, focus_turn(fov, fraction, g[0], g[1])), r);
        const float axis[3] = {position[0] - r[2] * 10, position[1] - r[5] * 10, position[2] - r[8] * 10};
        expect(!sphere_outside(t, axis, 0.0f), "the turned camera's own axis is inside");
    }

    // Spheres of growing radius around an outside point stop being culled exactly when they reach
    // the plane (signed distances are in metres: unit normals).
    {
        const float c[3] = {6, 0, -10};
        const double distance = (6 - 0.5 * 10) / std::sqrt(1 + 0.25);
        expect(sphere_outside(f, c, static_cast<float>(distance) - 0.01f) &&
                   !sphere_outside(f, c, static_cast<float>(distance) + 0.01f),
               "plane distances are metric");
    }

    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}

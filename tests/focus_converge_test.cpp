// Offline check of the converging focus boxes (src/quad/focus_converge.h): each eye's box aimed at the fixation point.
#include <cmath>
#include <cstdio>

#include "quad/focus_converge.h"
#include "test_util.h"

namespace {

XrQuaternionf yaw(double degrees) {  // about +y: positive turns -z (forward) towards -x (left)
    const double h = degrees * 3.14159265358979 / 360;
    return {0, static_cast<float>(std::sin(h)), 0, static_cast<float>(std::cos(h))};
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    using namespace simqvfr;
    const XrVector3f eyes[2] = {{-0.032f, 0, 0}, {0.032f, 0, 0}};  // a 64 mm IPD
    const XrQuaternionf straight[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    const XrVector3f forward{0, 0, -1};
    float tan[2][2] = {};
    bool valid[2] = {};

    converged_tangents(eyes, straight, eyes_middle(eyes), forward, 1.0, tan, valid);
    check(valid[0] && valid[1] && std::fabs(tan[0][0] - 0.032f) < 1e-5f && std::fabs(tan[1][0] + 0.032f) < 1e-5f &&
              std::fabs(tan[0][1]) < 1e-6f,
          "straight ahead at 1 m: each box turned in by half the IPD over the distance (the left eye's to its right)");

    converged_tangents(eyes, straight, eyes_middle(eyes), forward, 1e6, tan, valid);
    check(valid[0] && valid[1] && std::fabs(tan[0][0]) < 1e-6f && std::fabs(tan[1][0]) < 1e-6f,
          "far away: the boxes parallel (as before the convergence)");

    // A gaze 20 deg to the right and 10 deg down at 0.7 m: the same point seen from each eye.
    const double gx = std::tan(20 * 3.14159265358979 / 180), gy = -std::tan(10 * 3.14159265358979 / 180);
    const double n = std::sqrt(gx * gx + gy * gy + 1);
    const XrVector3f gaze{static_cast<float>(gx / n), static_cast<float>(gy / n), static_cast<float>(-1 / n)};
    converged_tangents(eyes, straight, eyes_middle(eyes), gaze, 0.7, tan, valid);
    // The point: 0.7 m along the gaze from the middle.
    const double px = gaze.x * 0.7, py = gaze.y * 0.7, pz = gaze.z * 0.7;
    const bool sees = valid[0] && valid[1] && std::fabs(tan[0][0] - (px + 0.032) / -pz) < 1e-4 &&
                      std::fabs(tan[1][0] - (px - 0.032) / -pz) < 1e-4 && std::fabs(tan[0][1] - py / -pz) < 1e-4 &&
                      std::fabs(tan[1][1] - py / -pz) < 1e-4;
    check(sees, "a gaze to the side at 0.7 m: each eye's box on the same point");

    // Canted displays (each eye turned 10 deg out): far away, both boxes on the head's forward axis, 10 deg in from each
    // eye's own.
    const XrQuaternionf canted[2] = {yaw(10), yaw(-10)};
    const XrVector3f headForward = rotate_by(eyes_middle(canted), forward);
    converged_tangents(eyes, canted, eyes_middle(eyes), headForward, 1e6, tan, valid);
    const float t10 = static_cast<float>(std::tan(10 * 3.14159265358979 / 180));
    check(valid[0] && valid[1] && std::fabs(tan[0][0] - t10) < 1e-4f && std::fabs(tan[1][0] + t10) < 1e-4f,
          "canted eyes, far away: each box turned in by the cant onto the head's forward axis");

    check(std::fabs(ray_distance(1.0, 0.75, 0) - 1.25) < 1e-9, "a ray's distance from its view depth and tangents");

    // Depth into distance: depth = a + b / z.
    check(std::fabs(depth_view_z(0.1 / 2.0, 0, 0.1) - 2.0) < 1e-9, "UE's reversed infinite depth (a 0, b the near plane)");
    check(depth_view_z(0, 0, 0.1) < 0, "reversed infinite: depth 0 is the sky (no distance)");
    {  // a reversed finite depth, near 0.05 m, far 2000 m: depth 1 at the near plane, 0 at the far one
        const double n = 0.05, fr = 2000, a = -n / (fr - n), b = n * fr / (fr - n);
        check(std::fabs(depth_view_z(1.0, a, b) - n) < 1e-9 && std::fabs(depth_view_z(a + b / 0.8, a, b) - 0.8) < 1e-9,
              "a reversed finite depth: the near plane, and a dash 0.8 m away");
        check(depth_view_z(0.0, a, b) < 0 || depth_view_z(0.0, a, b) > 1999, "a reversed finite depth: far is far");
    }
    {
        const float dash = static_cast<float>(0.1 / 0.8);  // 0.8 m on UE's reversed infinite depth, near 0.1 m
        const float values[9] = {dash, dash, dash, dash, dash, 0, 0, 0, 0};
        int sky = 0;
        const double m = fixation_from_depths(values, 9, 0.75, 0, 0, 0.1, sky);
        check(std::fabs(m - 1.0) < 1e-6 && sky == 4, "five texels on the dash, four on the sky: the dash, along the ray");
        const float sky9[9] = {};
        check(fixation_from_depths(sky9, 9, 0, 0, 0, 0.1, sky) == 1e4 && sky == 9, "all sky: 10 km");
    }

    FixationFilter f;
    const double first = f.update(1.0, 0.011);
    const double held = f.update(1.03, 0.011);
    check(first == 1.0 && held == 1.0, "a fixation's first value handed on at once; a 3% change held (the boxes still)");
    double now = 1.0;
    for (int i = 0; i < 90; ++i)  // one second at 90 Hz looking 10 m away
        now = f.update(10.0, 1.0 / 90);
    check(now > 9.0 && now < 10.5, "a new fixation followed within a second");
    check(f.update(-1, 0.011) == now && f.update(std::nan(""), 0.011) == now, "no depth: the last fixation kept");

    return test_result();
}

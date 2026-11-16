// Offline check of the focus mask (quad/focus_mask.h): its triangles cover only points of the focus box more than the
// margin outside the focus area's shape (so no texel the compositor shows, nor one next to it, is masked), and nearly
// all of those; the plain box and a margin past the rounding mask nothing; focus_area_inside agrees with the shader's
// measure at known points.
#include <cmath>
#include <cstdio>
#include <vector>

#include "quad/focus_mask.h"

using namespace simqvfr;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    failures += !ok;
}

bool close(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// p inside triangle abc (either winding, edges included).
bool in_triangle(double px, double py, const float* t) {
    double s[3];
    for (int i = 0; i < 3; ++i) {
        const double ax = t[2 * i], ay = t[2 * i + 1], bx = t[2 * ((i + 1) % 3)], by = t[2 * ((i + 1) % 3) + 1];
        s[i] = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    }
    return (s[0] >= 0 && s[1] >= 0 && s[2] >= 0) || (s[0] <= 0 && s[1] <= 0 && s[2] <= 0);
}

struct Coverage {
    int wrong = 0;          // covered points not more than the margin outside the shape
    double worst = 1e9;     // the least inside() of a covered point (should be < -margin)
    int eligible = 0;       // points comfortably past the margin (it may mask them)
    int missed = 0;         // of those, not covered
    double covered = 0;     // share of the box covered
};

Coverage cover(const FocusArea& area, double margin, const std::vector<float>& mesh, int grid = 500) {
    Coverage c;
    int coveredCount = 0;
    const size_t triangles = mesh.size() / 6;
    for (int y = 0; y < grid; ++y)
        for (int x = 0; x < grid; ++x) {
            const double fx = (x + 0.5) / grid, fy = (y + 0.5) / grid;
            const double nx = 2 * fx - 1, ny = 1 - 2 * fy;
            bool covered = false;
            for (size_t t = 0; t < triangles && !covered; ++t)
                covered = in_triangle(nx, ny, &mesh[6 * t]);
            const double inside = focus_area_inside(area, fx, fy);
            if (covered) {
                ++coveredCount;
                c.worst = std::min(c.worst, -inside);
                if (inside > -margin + 1e-9)
                    ++c.wrong;
            }
            if (inside < -(margin + 0.01)) {
                ++c.eligible;
                c.missed += covered ? 0 : 1;
            }
        }
    c.covered = static_cast<double>(coveredCount) / (grid * grid);
    return c;
}

void check_shape(const char* name, const FocusArea& area, double margin, double wantCoveredMin, double wantCoveredMax) {
    const std::vector<float> mesh = focus_mask_mesh(area, margin);
    const Coverage c = cover(area, margin, mesh);
    std::printf("%s: %zu triangles, %.1f%% of the box covered, %d of %d eligible points missed, nearest covered point "
                "%.4f outside (margin %.4f)\n",
                name, mesh.size() / 6, c.covered * 100, c.missed, c.eligible, c.worst, margin);
    char what[160];
    std::snprintf(what, sizeof(what), "%s: nothing shown or within the margin is masked", name);
    expect(!mesh.empty() && c.wrong == 0, what);
    std::snprintf(what, sizeof(what), "%s: nearly all of what may be masked is", name);
    expect(c.eligible > 0 && c.missed <= c.eligible / 50, what);
    std::snprintf(what, sizeof(what), "%s: covers %.0f-%.0f%% of the box", name, wantCoveredMin * 100, wantCoveredMax * 100);
    expect(c.covered >= wantCoveredMin && c.covered <= wantCoveredMax, what);
}

FocusArea corners(float tlIn, float tlDown, float trIn, float trDown, float brIn, float brUp, float blIn, float blUp) {
    FocusArea a;
    const float v[8] = {tlIn, tlDown, trIn, trDown, brIn, brUp, blIn, blUp};
    for (int i = 0; i < 8; ++i)
        a.inset[i] = v[i];
    return a;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // The shader's measure at known points.
    FocusArea box;
    expect(close(focus_area_inside(box, 0.5, 0.5), 0.5) && close(focus_area_inside(box, 0.1, 0.5), 0.1) &&
               close(focus_area_inside(box, -0.1, 0.5), -0.1),
           "the plain box: the nearer edge's share");
    const FocusArea trapezoid = corners(0.3f, 0, 0.3f, 0, 0, 0, 0, 0);
    expect(focus_area_inside(trapezoid, 0.05, 0.05) < 0 && focus_area_inside(trapezoid, 0.5, 0.05) > 0 &&
               focus_area_inside(trapezoid, 0.05, 0.95) > 0,
           "the trapezoid: the cut top corners outside, the middle and the bottom corners inside");
    FocusArea round;
    round.radius = 1.0f;  // a circle in a square box
    expect(close(focus_area_inside(round, 0.5, 0.5), 0.5) && close(focus_area_inside(round, 0.02, 0.02), 0.5 - std::hypot(0.48, 0.48), 1e-9) &&
               close(focus_area_inside(round, 0.5, 0.0), 0.0),
           "the rounded box: its centre half the side in, the corner well outside, the edges' middles on the edge");

    // Nothing to mask.
    expect(focus_mask_mesh(box, 0.01).empty(), "the plain box: no mask");
    FocusArea slight;
    slight.radius = 0.02f;
    expect(focus_mask_mesh(slight, 0.01).empty(), "a rounding the margin covers: no mask");

    // 16 texels of a 4152x3334 focus view (the probe run's) and of a small one (800x640).
    const double margin = focus_mask_margin(16, 4152, 3334), wide = focus_mask_margin(16, 800, 640);
    expect(close(margin, 16.0 / 3334) && close(wide, 0.025), "the margin: texels over the shorter side");

    // Pulled-in corners: the probe's trapezoid (30% of the box, less the margin along its two slanted edges).
    check_shape("trapezoid 30% (probe)", trapezoid, margin, 0.28, 0.30);
    check_shape("trapezoid 30%, small view", trapezoid, wide, 0.24, 0.30);
    // Every corner 20% in and 10% down or up: a 60% x 80% rectangle left, 52% outside.
    check_shape("all corners 20/10", corners(0.2f, 0.1f, 0.2f, 0.1f, 0.2f, 0.1f, 0.2f, 0.1f), margin, 0.50, 0.52);
    // The top left corner at 45/45: a concave quadrilateral, 45% outside.
    check_shape("one corner far in (concave)", corners(0.45f, 0.45f, 0, 0, 0, 0, 0, 0), margin, 0.43, 0.45);
    // (0.4, 0), (0.9, 0.3), (1, 0.8), (0.35, 0.95): 41.5% inside, 58.5% outside.
    check_shape("lopsided", corners(0.4f, 0, 0.1f, 0.3f, 0, 0.2f, 0.35f, 0.05f), margin, 0.55, 0.585);

    // Rounding: four corners of r x r less the quarter circles (1 - pi / 4 of r^2 each), r a share of half the
    // shorter side, less the margin.
    FocusArea r25;
    r25.radius = 0.25f;
    check_shape("rounded 25%, square", r25, margin, 0.008, 0.0135);
    FocusArea r100;
    r100.radius = 1.0f;
    check_shape("rounded 100%, square (a circle)", r100, margin, 0.19, 0.215);
    FocusArea r50wide;
    r50wide.radius = 0.5f;
    r50wide.aspect = 1.25f;
    check_shape("rounded 50%, wide box", r50wide, margin, 0.035, 0.045);
    FocusArea r50tall;
    r50tall.radius = 0.5f;
    r50tall.aspect = 0.8f;
    check_shape("rounded 50%, tall box", r50tall, margin, 0.035, 0.045);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

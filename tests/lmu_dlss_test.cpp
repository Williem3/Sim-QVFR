// Offline check of the maths behind the DLAA inputs (engine/dlss_math.h), as LMU has them: the jitter put into the
// located FOV moves the image by the pixels asked, and depth reprojection gives a still world's motion vectors, with the
// eye's and the focus view's jitter taken out. The projection is LMU's (tests/lmu_focus_test.cpp; docs/recon-lmu.md A2).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/dlss_math.h"
#include "quad/offaxis_focus.h"

namespace {

using namespace simqvfr::dlss;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// LMU's eye projection from the view's tangents (left-handed, row vectors, w = z).
M4 projection_of(const Tangents& t, double n, double f) {
    M4 m{};
    m[0] = 2 / (t.right - t.left);
    m[5] = 2 / (t.up - t.down);
    m[8] = -(t.right + t.left) / (t.right - t.left);
    m[9] = -(t.up + t.down) / (t.up - t.down);
    m[10] = f / (f - n);
    m[11] = 1;
    m[14] = -n * f / (f - n);
    return m;
}

// A view matrix: the camera at (x, y, z) turned by `yaw` about the up axis (world -> view, row vectors).
M4 view_of(double x, double y, double z, double yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    const M4 world = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};  // the camera's world matrix
    M4 view;
    invert(world, view);
    return view;
}

struct Pixel {
    double x = 0, y = 0, depth = 0;
};

// A world point through a view x projection into a width x height image.
Pixel pixel_of(const M4& vp, unsigned w, unsigned h, double wx, double wy, double wz) {
    const double cx = wx * vp[0] + wy * vp[4] + wz * vp[8] + vp[12], cy = wx * vp[1] + wy * vp[5] + wz * vp[9] + vp[13];
    const double cz = wx * vp[2] + wy * vp[6] + wz * vp[10] + vp[14], cw = wx * vp[3] + wy * vp[7] + wz * vp[11] + vp[15];
    return {(cx / cw * 0.5 + 0.5) * w, (0.5 - cy / cw * 0.5) * h, cz / cw};
}

}  // namespace

int main() {
    const unsigned W = 1600, H = 1280;
    const Tangents fov{std::tan(-0.9512), std::tan(0.8814), std::tan(-0.8290), std::tan(0.8290)};
    const M4 p = projection_of(fov, 0.05, 20000);
    check(projection(p), "LMU's projection has the shape expected");
    check(rigid(view_of(120, 3, -45, 0.4)) && !rigid(p), "a view matrix is rigid, a projection isn't");

    M4 inverse;
    check(invert(mul(view_of(1, 2, 3, 0.7), p), inverse), "a view x projection inverts");
    const M4 one = mul(mul(view_of(1, 2, 3, 0.7), p), inverse);
    bool unit = true;
    for (int i = 0; i < 16; ++i)
        unit = unit && near(one[i], i % 5 == 0 ? 1 : 0, 1e-9);
    check(unit, "the inverse times the matrix is the identity");

    // The jitter sequence stays inside the pixel and averages out.
    double sumX = 0, sumY = 0;
    bool inside = true;
    for (unsigned n = 0; n < kJitterPhases; ++n) {
        double x, y;
        jitter_pixels(n, x, y);
        inside = inside && x >= -0.5 && x < 0.5 && y >= -0.5 && y < 0.5;
        sumX += x;
        sumY += y;
    }
    check(inside, "every jitter phase is within half a pixel");
    check(std::fabs(sumX / kJitterPhases) < 0.05 && std::fabs(sumY / kJitterPhases) < 0.05, "the phases average to the centre");

    // The located FOV moved by the jitter: LMU's projection of it shows the image moved by exactly those pixels.
    const double sx = 0.3, sy = -0.2;
    double cx, cy;
    clip_shift_for(sx, sy, W, H, cx, cy);
    Tangents moved = fov;
    shift_tangents(moved, cx, cy);
    const M4 pj = projection_of(moved, 0.05, 20000);
    check(near(moved.right - moved.left, fov.right - fov.left) && near(moved.up - moved.down, fov.up - fov.down),
          "the FOV keeps its size");
    check(near(pj[8] - p[8], cx, 1e-12) && near(pj[9] - p[9], cy, 1e-12), "the projection's third row moves by the clip shift");
    const M4 view = view_of(120, 3, -45, 0.4);
    const Pixel plain = pixel_of(mul(view, p), W, H, 118, 2.5, -20), shifted = pixel_of(mul(view, pj), W, H, 118, 2.5, -20);
    check(near(shifted.x - plain.x, sx, 1e-6) && near(shifted.y - plain.y, sy, 1e-6),
          "the image moves by the pixels asked (x right, y down)");
    check(near(mul(mul(view, pj), clip_shift(-cx, -cy))[12], mul(view, p)[12], 1e-9), "the clip shift taken off gives the plain matrix");

    // A still world, the camera moving and turning, both frames jittered: the vector at the point's pixel leads to where
    // it was, both positions without jitter.
    {
        const M4 viewBefore = view_of(119.2, 3.02, -45.9, 0.38);
        double bx, by, cbx, cby;
        jitter_pixels(3, bx, by);
        clip_shift_for(bx, by, W, H, cbx, cby);
        const M4 renderedNow = mul(view, pj);                      // this frame, with its jitter (sx, sy)
        const M4 unjitteredBefore = mul(viewBefore, p);            // the frame before, its jitter taken off
        (void)cbx;
        (void)cby;
        M4 r;
        check(reprojection(renderedNow, unjitteredBefore, r), "the reprojection is made");
        bool all = true;
        const double points[][3] = {{118, 2.5, -20}, {125, 4, -10}, {110, 1, 30}, {121, 3, -44.2}};
        for (const auto& w : points) {
            const Pixel now = pixel_of(renderedNow, W, H, w[0], w[1], w[2]);
            const Pixel nowPlain = pixel_of(mul(view, p), W, H, w[0], w[1], w[2]);
            const Pixel before = pixel_of(unjitteredBefore, W, H, w[0], w[1], w[2]);
            double mx, my;
            all = all && motion_vector(r, cx, cy, W, H, now.x, now.y, now.depth, mx, my) &&
                  near(mx, before.x - nowPlain.x, 1e-5) && near(my, before.y - nowPlain.y, 1e-5);
        }
        check(all, "depth reprojection gives a still point's vector, jitter out of both ends");

        // Nothing moved: the vectors are zero whatever the jitter.
        M4 still;
        reprojection(renderedNow, mul(view, p), still);
        const Pixel now = pixel_of(renderedNow, W, H, 118, 2.5, -20);
        double mx, my;
        motion_vector(still, cx, cy, W, H, now.x, now.y, now.depth, mx, my);
        check(near(mx, 0, 1e-6) && near(my, 0, 1e-6), "a still camera gives zero vectors under jitter");
    }

    // LMU's scene depth is reversed: the same projection with 1 at the near plane and 0 far away. The image is the
    // same, and the reprojection works through it as through the ordinary one.
    {
        const M4 rev = reversed_depth(p);
        const M4 viewNow = view_of(120, 3, -45, 0.4), viewBefore = view_of(119.2, 3.02, -45.9, 0.38);
        const Pixel a = pixel_of(mul(viewNow, p), W, H, 118, 2.5, -20), b = pixel_of(mul(viewNow, rev), W, H, 118, 2.5, -20);
        check(near(a.x, b.x, 1e-9) && near(a.y, b.y, 1e-9) && near(a.depth + b.depth, 1, 1e-9),
              "reversed depth: the same pixel, the depth 1 minus the ordinary one");
        check(near(rev[10], 0.05 / (0.05 - 20000), 1e-12) && near(rev[14], 0.05 * 20000 / (20000 - 0.05), 1e-9),
              "reversed depth: the matrix a reversed projection has (n / (n - f), f n / (f - n))");
        M4 r;
        reprojection(mul(viewNow, rev), mul(viewBefore, rev), r);
        const Pixel before = pixel_of(mul(viewBefore, rev), W, H, 118, 2.5, -20);
        double mx, my;
        check(motion_vector(r, 0, 0, W, H, b.x, b.y, b.depth, mx, my) && near(mx, before.x - b.x, 1e-5) &&
                  near(my, before.y - b.y, 1e-5),
              "reversed depth: the reprojection's vector is right");
        // The mistake of run 3: reversed depth read through the ordinary projection moves a far point as a near one.
        M4 wrong;
        reprojection(mul(viewNow, p), mul(viewBefore, p), wrong);
        double wx, wy;
        const Pixel far = pixel_of(mul(viewNow, rev), W, H, 118, 40, 900);
        const Pixel farBefore = pixel_of(mul(viewBefore, rev), W, H, 118, 40, 900);
        const bool ok = motion_vector(wrong, 0, 0, W, H, far.x, far.y, far.depth, wx, wy);
        check(!ok || std::fabs(wx - (farBefore.x - far.x)) > 20 || std::fabs(wy - (farBefore.y - far.y)) > 20,
              "reversed depth read as ordinary gives a far point a wildly wrong vector (run 3)");
    }

    // The player's car: fixed in the tracking space, it moves on screen with the head alone. The eye's pose (OpenXR:
    // right-handed, -z forward) gives the view; a point straight ahead of an unturned eye lands in the image's middle
    // band, and two poses give its vector whatever the car did in the world.
    {
        const double q0[4] = {0, 0, 0, 1}, t0[3] = {0.1, 1.2, -0.3};
        const M4 h0 = head_view(q0, t0);
        check(rigid(h0), "the head's view matrix is rigid");
        const Pixel ahead = pixel_of(mul(h0, p), W, H, 0.1, 1.2, -0.3 - 0.8);  // 0.8 m in front of the eye (its -z)
        check(ahead.depth > 0 && ahead.depth < 1 && near(ahead.y, H / 2.0, 1e-6), "a point ahead of the eye is in front of it");
        const Pixel right = pixel_of(mul(h0, p), W, H, 0.1 + 0.2, 1.2, -0.3 - 0.8);
        const Pixel up = pixel_of(mul(h0, p), W, H, 0.1, 1.2 + 0.2, -0.3 - 0.8);
        check(right.x > ahead.x && up.y < ahead.y, "the tracking space's right and up are the image's right and up");
        // The head turns 3 degrees about the vertical and shifts 2 cm between the frames.
        const double a = 0.0524 / 2, q1[4] = {0, std::sin(a), 0, std::cos(a)}, t1[3] = {0.12, 1.2, -0.31};
        const M4 h1 = head_view(q1, t1), rev = reversed_depth(p);
        M4 r;
        reprojection(mul(h1, rev), mul(h0, rev), r);
        const double dash[3] = {0.25, 1.0, -1.0};  // a point on the dashboard, in the tracking space
        const Pixel now = pixel_of(mul(h1, rev), W, H, dash[0], dash[1], dash[2]);
        const Pixel before = pixel_of(mul(h0, rev), W, H, dash[0], dash[1], dash[2]);
        double mx, my;
        check(motion_vector(r, 0, 0, W, H, now.x, now.y, now.depth, mx, my) && near(mx, before.x - now.x, 1e-5) &&
                  near(my, before.y - now.y, 1e-5) && std::fabs(mx) > 5,
              "the car's vector is the head's motion alone");
    }

    // The car's box: a pixel's place in the tracking space, from its depth through the head's view, against a box
    // around the head. The dashboard and the nose are the car; the road ahead of the nose and the road beside the car
    // (within 2.5 m of the eye, which a distance threshold took for the car) are the world.
    {
        const double q[4] = {0, std::sin(0.1), 0, std::cos(0.1)}, head[3] = {0.35, 1.1, 0.2};  // the head turned a little
        const M4 rendered = mul(head_view(q, head), reversed_depth(p));
        M4 clipToTracking;
        check(invert(rendered, clipToTracking), "the head's view x projection inverts");
        const CarBox box;
        const auto in_box = [&](double x, double y, double z) {
            const Pixel px = pixel_of(rendered, W, H, x, y, z);
            return in_car_box(clipToTracking, px.x / W * 2 - 1, 1 - px.y / H * 2, px.depth, head, box);
        };
        check(in_box(0.35 + 0.2, 1.1 - 0.4, 0.2 - 0.7), "the dashboard is in the car's box");
        check(in_box(0.35, 1.1 - 0.6, 0.2 - 2.4), "the nose, 2.4 m ahead, is in the car's box");
        check(!in_box(0.35, 1.1 - 1.2, 0.2 - 6.0), "the road 6 m ahead is the world");
        check(!in_box(0.35 - 1.9, 1.1 - 1.2, 0.2 - 1.2), "the road beside the car, 2.3 m from the eye, is the world");
        check(!in_box(0.35 - 30, 1.1 + 5, 0.2 - 400), "the far scenery is the world");
    }

    // The focus view: the eye's clip through a remap that carries the focus view's own jitter; its box moves between
    // the frames (the gaze).
    {
        const unsigned FW = 1280, FH = 1300;
        const double a = 4, b = -0.3, c = 1.6, dd = -0.08;         // this frame's remap from the jittered eye
        const double ab = 4, bb = -0.25, cb = 1.6, db = -0.1;      // the frame before's (the box elsewhere)
        double fx, fy;
        clip_shift_for(-0.4, 0.25, FW, FH, fx, fy);
        const M4 eyeNow = mul(view, pj), eyeBefore = mul(view_of(119.2, 3.02, -45.9, 0.38), p);
        const M4 renderedNow = mul(eyeNow, clip_remap_matrix(a, b + fx, c, dd + fy));
        const M4 plainNow = mul(eyeNow, clip_remap_matrix(a, b, c, dd));
        const M4 unjitteredBefore = mul(eyeBefore, clip_remap_matrix(ab, bb, cb, db));
        M4 r;
        reprojection(renderedNow, unjitteredBefore, r);
        const Pixel now = pixel_of(renderedNow, FW, FH, 121, 3, -30), nowPlain = pixel_of(plainNow, FW, FH, 121, 3, -30);
        const Pixel before = pixel_of(unjitteredBefore, FW, FH, 121, 3, -30);
        double mx, my;
        check(near(now.x - nowPlain.x, -0.4, 1e-6) && near(now.y - nowPlain.y, 0.25, 1e-6),
              "the remap's shift moves the focus view by its own jitter");
        check(motion_vector(r, fx, fy, FW, FH, now.x, now.y, now.depth, mx, my) && near(mx, before.x - nowPlain.x, 1e-5) &&
                  near(my, before.y - nowPlain.y, 1e-5),
              "the focus view's vectors follow the camera and its moving box");
    }

    // The focus box measured on the eye's jittered projection, moved back by the clip shift: as measured on the plain
    // one (DLAA's output has no jitter, and the layer composes the periphery with these eye tangents). The focus view's
    // own tangents and projection don't move (the box is the gaze's).
    {
        float pf[16], pjf[16], focusPlain[16], focusJittered[16];
        for (int i = 0; i < 16; ++i) {
            pf[i] = static_cast<float>(p[i]);
            pjf[i] = static_cast<float>(pj[i]);
        }
        simqvfr::FocusBox plainBox, box;
        simqvfr::focus_projection(pf, 0.3, 0.3, 0.1, -0.05, plainBox, focusPlain);
        simqvfr::focus_projection(pjf, 0.3, 0.3, 0.1, -0.05, box, focusJittered);
        check(!near(box.eyeTan[0], plainBox.eyeTan[0], 1e-5) && !near(box.rect[0], plainBox.rect[0], 1e-6),
              "the jitter moves the box's eye tangents and its place in the eye's image");
        unshift_box(box, cx, cy);
        bool same = true;
        for (int i = 0; i < 4; ++i)
            same = same && near(box.eyeTan[i], plainBox.eyeTan[i], 1e-5) && near(box.focusTan[i], plainBox.focusTan[i], 1e-6) &&
                   near(box.rect[i], plainBox.rect[i], 1e-5);
        check(same, "moved back by the clip shift, the box is the plain projection's");
        check(near(focusPlain[0], focusJittered[0]) && near(focusPlain[8], focusJittered[8]) &&
                  near(focusPlain[5], focusJittered[5]) && near(focusPlain[9], focusJittered[9]),
              "the focus view's projection is the same either way");
    }

    // The gate's verdicts (values from lmu_dlss_check's moving scene and its control).
    check(!std::strcmp(gate_verdict(0.084, 0.026, 0.045), ": fit"), "colour: well below standing is a fit");
    check(!std::strcmp(gate_verdict(0.03, 0.029, 0.05), ": fit to the pixel"),
          "colour: little motion but well below a pixel off is a fit to the pixel");
    check(!std::strcmp(gate_verdict(0.0505, 0.0516, 0.0526), ": no better than none"),
          "colour: detail finer than a pixel reads no better than none");
    check(!std::strcmp(gate_verdict(0.05, 0.07, 0.07), ": WORSE than none"), "colour: above standing is worse");
    check(!std::strcmp(gate_depth_verdict(0.44, 0.93), ": fit") && !std::strcmp(gate_depth_verdict(1, 1), ": fit (little motion)"),
          "depth: 85% or more agreeing is a fit, with standing too a fit with little motion");
    check(!std::strcmp(gate_depth_verdict(0.70, 0.62), ": WORSE than none") && !std::strcmp(gate_depth_verdict(0.5, 0.6), ": partly"),
          "depth: fewer than standing is worse, else below 85% partly");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

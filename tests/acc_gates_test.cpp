// Offline check of ACC's gate comparisons (src/games/acc/gate_compare.h) on synthetic images: the copies gate finds equal
// views equal, a shifted copy by its shift, an exposure gain by its sign and band; the focus gate finds a focus view
// rendered from the same scene at a higher density aligned and at 1.00, a darker one by its ratio, a misplaced one by
// its offset, and an edge band by the ring against the middle.
#include <cmath>
#include <cstdio>
#include <vector>

#include "games/acc/gate_compare.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

// A scene as a function of the eye's image coordinates (0-1, rows down): smooth, with detail at several scales.
double scene(double u, double v) {
    constexpr double pi = 3.14159265358979;
    return 120 + 50 * std::sin(2 * pi * 5 * u) * std::cos(2 * pi * 3 * v) + 30 * std::sin(2 * pi * 13 * u * v + 1) +
           20 * std::cos(2 * pi * 9 * (u - v));
}

struct Canvas {
    int w, h;
    std::vector<std::uint8_t> px;
    Canvas(int width, int height) : w(width), h(height), px(static_cast<std::size_t>(width) * height * 4, 0) {}
    void put(int x, int y, double value, double gain = 1) {
        const double g = std::pow(std::fmax(0.0, value) / 255.0, 2.2) * gain;  // a gain in linear light
        const int v = static_cast<int>(std::lround(255 * std::pow(std::fmin(1.0, g), 1 / 2.2)));
        std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
        p[0] = p[1] = p[2] = static_cast<std::uint8_t>(std::fmax(0, std::fmin(255, v)));
        p[3] = 255;
    }
    acc::GateImage image() const { return {px.data(), w, h}; }
};

// Renders the eye (its whole view) into `eye` and the box (x0, y0, x1, y1 of the eye) into `focus`, both of the scene;
// `gain` on the focus view, the focus view sampled `shift` eye pixels off, `edge` darkening its outer tenth.
void render(Canvas& c, const acc::GateRect& eye, const acc::GateRect& focus, const double (&box)[4], double gain = 1,
            double shiftX = 0, double edge = 1) {
    for (int y = 0; y < eye.h; ++y)
        for (int x = 0; x < eye.w; ++x)
            c.put(eye.x + x, eye.y + y, scene((x + 0.5) / eye.w, (y + 0.5) / eye.h));
    for (int y = 0; y < focus.h; ++y)
        for (int x = 0; x < focus.w; ++x) {
            const double fu = (x + 0.5) / focus.w, fv = (y + 0.5) / focus.h;
            const double u = box[0] + fu * (box[2] - box[0]) + shiftX / eye.w, v = box[1] + fv * (box[3] - box[1]);
            const bool rim = fu < 0.1 || fu > 0.9 || fv < 0.1 || fv > 0.9;
            c.put(focus.x + x, focus.y + y, scene(u, v), gain * (rim ? edge : 1));
        }
}

}  // namespace

int main() {
    using namespace acc;
    {
        // Copies: two views of the same scene.
        Canvas c(640, 520);
        const GateRect a{0, 0, 300, 240}, b{0, 260, 300, 240};
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x) {
                const double s = scene((x + 0.5) / a.w, (y + 0.5) / a.h);
                c.put(a.x + x, a.y + y, s);
                c.put(b.x + x, b.y + y, s);
            }
        CopiesResult r = compare_copies(c.image(), a, b);
        check(r.valid && r.differing == 0 && r.dx == 0 && r.dy == 0 && r.pixels == 300u * 240u, "equal copies: no pixel differs");
        // The copy two pixels to the right.
        Canvas s(640, 520);
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x) {
                s.put(a.x + x, a.y + y, scene((x + 0.5) / a.w, (y + 0.5) / a.h));
                s.put(b.x + x, b.y + y, scene((x - 2 + 0.5) / a.w, (y + 0.5) / a.h));
            }
        r = compare_copies(s.image(), a, b);
        check(r.valid && r.dx == 2 && r.dy == 0 && r.percent() < 1.5, "a copy placed two pixels off: found by its shift");
        // An exposure gain of 5% on the copy: one sign everywhere, the band ratios ~1.05.
        Canvas g(640, 520);
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x) {
                const double v = scene((x + 0.5) / a.w, (y + 0.5) / a.h);
                g.put(a.x + x, a.y + y, v);
                g.put(b.x + x, b.y + y, v, 1.05);
            }
        r = compare_copies(g.image(), a, b);
        bool bands = true;
        for (int i = 0; i < 8; ++i)
            if (r.bandCount[i] > 1000)
                bands &= std::fabs(r.bandRatio[i] - 1.05) < 0.02;
        check(r.valid && r.dx == 0 && r.signedSum[0] > 0 && r.signedSum[1] > 0 && r.signedSum[2] > 0 && bands &&
                  !r.within() && std::fabs(r.worst_band() - 0.05) < 0.02,
              "a 5% exposure gain: positive in every channel, ~1.05 in every band, out of range");
        // The tonemapper's dither a step apart: half the pixels one step off, no sign, within range.
        Canvas t(640, 520);
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x) {
                t.put(a.x + x, a.y + y, scene((x + 0.5) / a.w, (y + 0.5) / a.h));
                t.put(b.x + x, b.y + y, scene((x + 0.5) / a.w, (y + 0.5) / a.h));
                std::uint8_t* p = &t.px[(static_cast<std::size_t>(b.y + y) * t.w + b.x + x) * 4];
                const int step = ((x * 7 + y * 13) % 4) - 1;  // -1, 0, +1, +2 -> clamp to one step
                const int dither = step > 1 ? 0 : step;
                for (int ch = 0; ch < 3; ++ch)
                    p[ch] = static_cast<std::uint8_t>(std::max(0, std::min(255, p[ch] + dither)));
            }
        r = compare_copies(t.image(), a, b);
        check(r.valid && r.differing > 0 && r.over1 == 0 && r.within(), "a dither a step apart: within range");
        check(compare_copies(c.image(), a, b).within(), "equal copies: within range");
        // Single pixels far off on edges (sub-pixel noise) stay within range; an object missing from the copy doesn't.
        Canvas n = c;
        for (int y = 3; y < a.h; y += 9)
            for (int x = 5; x + 1 < a.w; x += 11) {
                // An edge a sub-pixel off: a pixel and its neighbour trade 40 steps.
                std::uint8_t* p = &n.px[(static_cast<std::size_t>(b.y + y) * n.w + b.x + x) * 4];
                std::uint8_t* q = p + 4;
                if (p[1] < 60 || p[1] > 190 || q[1] < 60 || q[1] > 190)
                    continue;
                p[0] = p[1] = p[2] = static_cast<std::uint8_t>(p[1] + 40);
                q[0] = q[1] = q[2] = static_cast<std::uint8_t>(q[1] - 40);
            }
        r = compare_copies(n.image(), a, b);
        check(r.valid && r.over16 > 0 && r.within(), "scattered single pixels far off: within range (8x8 blocks agree)");
        Canvas o = c;
        for (int y = 100; y < 140; ++y)
            for (int x = 120; x < 160; ++x)
                o.put(b.x + x, b.y + y, 20);
        r = compare_copies(o.image(), a, b);
        check(r.valid && !r.within() && r.blocksOver8 >= 16, "an object missing from the copy: out of range");
        check(!compare_copies(c.image(), a, GateRect{0, 260, 299, 240}).valid, "views of two sizes aren't compared");
    }
    {
        // Focus: the eye 400 x 320, its box (0.30-0.60, 0.25-0.55) rendered 360 x 288 below it (3x the density).
        Canvas c(800, 700);
        const GateRect eye{0, 0, 400, 320}, focus{0, 340, 360, 288};
        const double box[4] = {0.30, 0.25, 0.60, 0.55};
        render(c, eye, focus, box);
        FocusResult r = compare_focus(c.image(), eye, focus, box);
        check(r.valid && std::fabs(r.flat - 1) < 0.01 && std::fabs(r.overall - 1) < 0.01 && r.dx == 0 && r.dy == 0 &&
                  std::fabs(r.edge - r.middle) < 0.01 && r.boxW == 120 && r.boxH == 96 && r.flatBlocks > 500 && r.within(),
              "a focus view of the box at 3x: 1.00 in flat blocks, aligned, edge as the middle, within range");
        Canvas d(800, 700);
        render(d, eye, focus, box, 0.9);
        r = compare_focus(d.image(), eye, focus, box);
        check(r.valid && std::fabs(r.flat - 0.9) < 0.01 && r.dx == 0 && !r.within(), "a focus view 10% darker: 0.90, out of range");
        Canvas s(800, 700);
        render(s, eye, focus, box, 1, 3);
        r = compare_focus(s.image(), eye, focus, box);
        check(r.valid && r.dx == 3 && r.dy == 0 && r.sad < r.sadAtZero && !r.within(),
              "a focus view three eye pixels off: found by its offset");
        Canvas e(800, 700);
        render(e, eye, focus, box, 1, 0, 0.8);
        r = compare_focus(e.image(), eye, focus, box);
        check(r.valid && r.edge < 0.95 && std::fabs(r.middle - 1) < 0.01 && !r.within(),
              "a darker rim: the edge ring under the middle, out of range");
        // Fine texture the focus view resolves (a checker at its own pixels in the left half, 30% brighter on average,
        // as the tonemapper's toe lifts resolved detail): classed as detailed, the flat half decides.
        Canvas t(800, 700);
        render(t, eye, focus, box);
        for (int y = 0; y < focus.h; ++y)
            for (int x = 0; x < focus.w / 2; ++x) {
                const std::uint8_t v = t.px[(static_cast<std::size_t>(focus.y + y) * t.w + focus.x + x) * 4 + 1];
                t.put(focus.x + x, focus.y + y, v, ((x + y) % 2) ? 0.5 : 2.1);
            }
        r = compare_focus(t.image(), eye, focus, box);
        check(r.valid && r.detailedBlocks > 200 && r.detailed > 1.2 && std::fabs(r.flat - 1) < 0.01 && r.within() &&
                  r.darkerCore == 0 && r.brighterCore == 0,
              "resolved fine detail brighter on average: classed as detail, no flips, the flat blocks within range");
        r = compare_focus(c.image(), eye, focus, box);
        check(r.darkerCore == 0 && r.brighterCore == 0, "a matching focus view: no light/shadow flips");
        // A shadow across a tenth of the focus view's height that its eye doesn't have (the lap of 2026-11-08: the sunlit
        // band below the windscreen base): medians barely move, the flip region fails it.
        Canvas sh(800, 700);
        render(sh, eye, focus, box);
        for (int y = focus.h * 3 / 10; y < focus.h * 4 / 10; ++y)
            for (int x = 0; x < focus.w; ++x) {
                const std::uint8_t v = sh.px[(static_cast<std::size_t>(focus.y + y) * sh.w + focus.x + x) * 4 + 1];
                sh.put(focus.x + x, focus.y + y, v, 0.3);
            }
        r = compare_focus(sh.image(), eye, focus, box);
        check(r.valid && std::fabs(r.flat - 1) < 0.02 && r.darkerCore > 200 && r.brighterCore == 0 && !r.within() &&
                  r.darkerAt[1] >= 26 && r.darkerAt[3] <= 40,
              "a shadow band only the focus view has: medians near 1, a large darker flip, out of range");
        double b[4];
        const float eyeTan[4] = {-1.0f, 1.0f, 0.8f, -0.8f}, focusTan[4] = {-0.2f, 0.4f, 0.4f, -0.08f};
        check(box_in_eye(eyeTan, focusTan, b) && std::fabs(b[0] - 0.4) < 1e-6 && std::fabs(b[2] - 0.7) < 1e-6 &&
                  std::fabs(b[1] - 0.25) < 1e-6 && std::fabs(b[3] - 0.55) < 1e-6,
              "a focus view's box in its eye from the two tangent sets");
    }
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

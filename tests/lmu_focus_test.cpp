// Offline check of LMU's focus projection (games/lmu/focus_math.h). The eye projection is built as LMU's
// VR_GetEyeProjectionLH builds it (docs/recon-lmu.md "Engine map" A2): the right-handed off-centre projection from the
// XrView's tangents, its third row negated. Tangents from the identity run (Pimax: fov -0.9512/0.8814/0.8290/-0.8290).
#include <cmath>
#include <cstdio>

#include "games/lmu/focus_math.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

// LMU's projection: VR_GetEyeProjection's right-handed matrix, then VR_GetEyeProjectionLH negates row 2.
void lmu_projection(double l, double r, double d, double u, double n, double f, float (&m)[16]) {
    for (float& v : m)
        v = 0;
    m[0] = static_cast<float>(2 / (r - l));
    m[5] = static_cast<float>(2 / (u - d));
    m[8] = static_cast<float>(-(r + l) / (r - l));  // row 2, negated for left-handed
    m[9] = static_cast<float>(-(u + d) / (u - d));
    m[10] = static_cast<float>(f / (f - n));
    m[11] = 1.0f;
    m[14] = static_cast<float>(-n * f / (f - n));
}

// x and y NDC of the view-space direction with tangents (tx, ty), through row-vector matrix m (z = 1, left-handed).
void project(const float (&m)[16], double tx, double ty, double& x, double& y) {
    const double cx = tx * m[0] + ty * m[4] + m[8] + m[12];
    const double cy = tx * m[1] + ty * m[5] + m[9] + m[13];
    const double cw = tx * m[3] + ty * m[7] + m[11] + m[15];
    x = cx / cw;
    y = cy / cw;
}

}  // namespace

int main() {
    const double l = std::tan(-0.9512), r = std::tan(0.8814), u = std::tan(0.8290), d = std::tan(-0.8290);
    float eye[16];
    lmu_projection(l, r, d, u, 0.05, 20000, eye);
    check(lmu::projection_shape(eye), "LMU's eye projection has the expected shape");

    double x = 0, y = 0;
    project(eye, l, d, x, y);
    check(near(x, -1) && near(y, -1), "the eye's left-bottom tangents land at NDC (-1, -1)");
    project(eye, r, u, x, y);
    check(near(x, 1) && near(y, 1), "the eye's right-top tangents land at NDC (1, 1)");

    lmu::FocusBox box;
    float focus[16];
    check(lmu::focus_projection(eye, 0.3, 0.3, 0, 0, box, focus), "a 30% centred box is made");
    check(near(box.eyeTan[0], l) && near(box.eyeTan[1], r) && near(box.eyeTan[2], d) && near(box.eyeTan[3], u),
          "the eye's tangents are read back from its matrix");
    const double hw = (r - l) * 0.3 / 2, hh = (u - d) * 0.3 / 2;
    check(near(box.focusTan[0], -hw) && near(box.focusTan[1], hw) && near(box.focusTan[2], -hh) && near(box.focusTan[3], hh),
          "the box is 30% of the span, centred on the forward axis");
    project(focus, -hw, -hh, x, y);
    const bool corner0 = near(x, -1) && near(y, -1);
    project(focus, hw, hh, x, y);
    check(corner0 && near(x, 1) && near(y, 1), "the box's corners land at the focus view's NDC corners");
    project(focus, 0, 0, x, y);
    check(near(x, 0) && near(y, 0), "the forward axis is the focus view's centre");
    check(focus[10] == eye[10] && focus[11] == eye[11] && focus[14] == eye[14], "depth and w are the eye's");

    // Where the box sits in the eye's image: the forward axis is right of centre on this headset (l is wider than r).
    project(eye, 0, 0, x, y);
    const double axisX = (x + 1) / 2, axisY = (1 - y) / 2;
    check(near((box.rect[0] + box.rect[2]) / 2, axisX) && near((box.rect[1] + box.rect[3]) / 2, axisY),
          "the box's rect in the eye image is centred on the forward axis");
    check(near(box.rect[2] - box.rect[0], 0.3) && near(box.rect[3] - box.rect[1], 0.3),
          "the box covers 30% of the eye image's width and height");
    check(box.rect[1] < box.rect[3] && box.rect[0] < box.rect[2], "the rect runs left to right and top to bottom");
    std::printf("eye image rect x %.4f-%.4f y %.4f-%.4f\n", box.rect[0], box.rect[2], box.rect[1], box.rect[3]);

    // An off-centre box (gaze later): centred on tangents (0.2, -0.1).
    check(lmu::focus_projection(eye, 0.3, 0.2, 0.2, -0.1, box, focus), "an off-centre box is made");
    project(focus, 0.2, -0.1, x, y);
    check(near(x, 0) && near(y, 0), "its centre is the focus view's centre");

    // The gaze centre kept so the box stays inside the eye (QVFR).
    double cx = 0.2, cy = -0.1;
    check(lmu::clamp_focus_centre(eye, 0.3, 0.3, cx, cy) && near(cx, 0.2) && near(cy, -0.1),
          "a gaze well inside the eye moves nothing");
    cx = 5;
    cy = -5;
    check(lmu::clamp_focus_centre(eye, 0.3, 0.3, cx, cy) && near(cx, r - hw) && near(cy, d + hh),
          "a gaze past the right-bottom edge puts the box against it");
    lmu::focus_projection(eye, 0.3, 0.3, cx, cy, box, focus);
    check(near(box.focusTan[1], r) && near(box.focusTan[2], d) && near(box.rect[2], 1) && near(box.rect[3], 1),
          "that box ends at the eye's own edges");
    cx = -5;
    cy = 5;
    check(lmu::clamp_focus_centre(eye, 0.3, 0.3, cx, cy) && near(cx, l + hw) && near(cy, u - hh),
          "a gaze past the left-top edge puts the box against it");
    cx = 0.7;
    cy = 0.4;
    check(lmu::clamp_focus_centre(eye, 1.0, 1.0, cx, cy) && near(cx, (l + r) / 2) && near(cy, (u + d) / 2),
          "a box as large as the eye is centred on it");

    // The VR UI's matrix: a world-to-view (turned and moved) times the eye's right-handed projection, row vectors. Its
    // focus version must put the focus box's corners at the focus view's NDC corners, in either storage order.
    {
        lmu::FocusBox ubox;
        float ufocus[16];
        const double ucx = 0.15, ucy = -0.05;
        lmu::focus_projection(eye, 0.3, 0.25, ucx, ucy, ubox, ufocus);
        const lmu::ClipRemap remap = lmu::clip_remap(eye, ufocus);
        float rh[16] = {};  // VR_GetEyeProjection: right-handed (w = -z), the LH's row 2 negated
        for (int i = 0; i < 16; ++i)
            rh[i] = eye[i];
        for (int i = 8; i < 12; ++i)
            rh[i] = -eye[i];
        // World to view: a turn of 0.4 rad about y then 0.2 about x, and a move.
        const double ya = 0.4, pa = 0.2;
        const double ry[9] = {std::cos(ya), 0, -std::sin(ya), 0, 1, 0, std::sin(ya), 0, std::cos(ya)};
        const double rx[9] = {1, 0, 0, 0, std::cos(pa), std::sin(pa), 0, -std::sin(pa), std::cos(pa)};
        double r3[9] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r3[i * 3 + j] += ry[i * 3 + k] * rx[k * 3 + j];
        const double t3[3] = {0.3, -1.2, 0.7};
        double view[16] = {r3[0], r3[1], r3[2], 0, r3[3], r3[4], r3[5], 0, r3[6], r3[7], r3[8], 0, t3[0], t3[1], t3[2], 1};
        float pv[16] = {}, pvT[16] = {};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double s = 0;
                for (int k = 0; k < 4; ++k)
                    s += view[i * 4 + k] * rh[k * 4 + j];
                pv[i * 4 + j] = static_cast<float>(s);
                pvT[j * 4 + i] = static_cast<float>(s);
            }
        check(lmu::clip_layout(pv) == 1 && lmu::clip_layout(pvT) == 2, "the UI matrix's storage order is recognised");
        float fpv[16], fpvT[16];
        for (int i = 0; i < 16; ++i) {
            fpv[i] = pv[i];
            fpvT[i] = pvT[i];
        }
        lmu::remap_clip(fpv, remap, 1);
        lmu::remap_clip(fpvT, remap, 2);
        // A world point seen at view tangents (tx, ty), 3 m ahead: its NDC through either focus matrix.
        const auto ndc = [&](const float (&m)[16], bool rows, double tx, double ty, double& nx, double& ny) {
            const double p[3] = {tx * 3, ty * 3, -3};  // view space, right-handed: forward is -z
            double w4[4] = {0, 0, 0, 1};               // world = (p - t) R^T
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    w4[j] += (p[k] - t3[k]) * r3[j * 3 + k];
            double c[4] = {};
            for (int o = 0; o < 4; ++o)
                for (int i = 0; i < 4; ++i)
                    c[o] += w4[i] * (rows ? m[o * 4 + i] : m[i * 4 + o]);
            nx = c[0] / c[3];
            ny = c[1] / c[3];
        };
        double nx = 0, ny = 0, mx = 0, my = 0;
        ndc(fpv, false, ubox.focusTan[0], ubox.focusTan[2], nx, ny);
        ndc(fpvT, true, ubox.focusTan[0], ubox.focusTan[2], mx, my);
        const bool lowCorner = near(nx, -1) && near(ny, -1) && near(mx, -1) && near(my, -1);
        ndc(fpv, false, ubox.focusTan[1], ubox.focusTan[3], nx, ny);
        ndc(fpvT, true, ubox.focusTan[1], ubox.focusTan[3], mx, my);
        check(lowCorner && near(nx, 1) && near(ny, 1) && near(mx, 1) && near(my, 1),
              "the focus UI matrix puts the box's corners at the focus view's NDC corners");
        ndc(pv, false, ubox.focusTan[0], ubox.focusTan[3], nx, ny);
        check(near((nx + 1) / 2, ubox.rect[0]) && near((1 - ny) / 2, ubox.rect[1]),
              "and the eye's UI matrix puts that corner where the box sits in the eye image");
        float flat[16] = {};
        check(lmu::clip_layout(flat) == 0, "a matrix of no such shape is refused");
    }

    float skewed[16];
    for (int i = 0; i < 16; ++i)
        skewed[i] = eye[i];
    skewed[1] = 0.1f;
    check(!lmu::focus_projection(skewed, 0.3, 0.3, 0, 0, box, focus) && !box.valid, "a matrix of another shape is refused");
    cx = 1;
    cy = 1;
    check(!lmu::clamp_focus_centre(skewed, 0.3, 0.3, cx, cy) && cx == 1 && cy == 1, "and its centre is left alone");

    std::printf(failures ? "lmu_focus_test: %d failure(s)\n" : "lmu_focus_test: ok\n", failures);
    return failures ? 1 : 0;
}

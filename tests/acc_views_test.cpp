// Offline check of ACC's four-view mapping onto UE 4.26's stereo passes (src/games/acc/views_map.h): views,
// passes and eyes; the eye-column layout instanced stereo and OpenComposite need; and that each eye the layer
// submits is the view UE rendered for it; and the focus views' projection.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "games/acc/gate_compare.h"
#include "games/acc/views_map.h"
#include "quad/offaxis_focus.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    using namespace acc;
    check(views_for(true) == 4 && views_for(false) == 1, "four views in stereo, one without");
    bool passes = pass_for_view(false, 0) == kPassFull;
    for (unsigned v = 0; v < 4; ++v)
        passes &= pass_for_view(true, v) == kPassLeft + static_cast<int>(v) && view_for_pass(pass_for_view(true, v)) == v;
    check(passes && view_for_pass(kPassFull) == 0, "view <-> pass both ways, as Epic's OpenXR plugin maps quad views");
    check(eye_pass(kPassLeftSide) == kPassLeft && eye_pass(kPassRightSide) == kPassRight && eye_pass(kPassLeft) == kPassLeft &&
              eye_pass(kPassRight) == kPassRight && eye_pass(kPassFull) == kPassFull,
          "a side pass belongs to its eye");
    check(primary_pass(kPassFull) && primary_pass(kPassLeft) && !primary_pass(kPassRight) && primary_pass(kPassLeftSide) &&
              !primary_pass(kPassRightSide),
          "primary passes: mono, the left eye and the left focus view (each leads an instanced pair)");

    // Equal outer and focus views: each eye's half holds its outer view over its focus view.
    const simqvfr::Views4Layout equal;
    const PassRect l = pass_rect(equal, kPassLeft, 8000, 3000), r = pass_rect(equal, kPassRight, 8000, 3000),
                   ls = pass_rect(equal, kPassLeftSide, 8000, 3000), rs = pass_rect(equal, kPassRightSide, 8000, 3000);
    check(l.x == 0 && l.y == 0 && l.width == 4000 && l.height == 1500 && r.x == 4000 && r.y == 0 && ls.x == 0 &&
              ls.y == 1500 && ls.width == 4000 && ls.height == 1500 && rs.x == 4000 && rs.y == 1500,
          "eye columns: outer views on top, focus views below, each in its eye's half");

    // Instanced stereo draws a pair in one viewport from x = 0 to the right view's edge, halved: the right view must
    // start where the left one ends. Each pair has its own width (the instanced stereo width per pair).
    const simqvfr::Views4Layout gate = simqvfr::views4_layout(0.6, 0.4);  // acc-gate.ini
    const int width = 4844, height = 3376;
    const PassRect gl = pass_rect(gate, kPassLeft, width, height), gr = pass_rect(gate, kPassRight, width, height),
                   gls = pass_rect(gate, kPassLeftSide, width, height), grs = pass_rect(gate, kPassRightSide, width, height);
    check(gl.x == 0 && gr.x == gl.width && gr.width == gl.width && gl.width == width / 2,
          "instanced stereo: the eyes (the wider pair) side by side from x = 0, meeting in the middle");
    check(gls.x == 0 && grs.x == gls.width && gls.width == (width / 2) * 2 / 3 && grs.width == gls.width &&
              gls.y == grs.y && gls.y >= gl.height && gls.y + gls.height <= height,
          "instanced stereo, second pair: the focus views side by side from x = 0 at their own width, below the eyes");
    check(gate.column_eye_width() == 0.6 && gate.column_eye_height() == 1.0 && gl.height == 2025 && gls.height == 1351,
          "gate layout: eye 60% wide, outer 60% over focus 40% high");
    {
        // acc_shared_width=1: every view the outer view's width, as before the width per pair.
        simqvfr::Views4Layout shared = gate;
        shared.sharedColumnWidth = true;
        const PassRect sls = pass_rect(shared, kPassLeftSide, width, height), srs = pass_rect(shared, kPassRightSide, width, height);
        check(shared.column_eye_width() == 0.6 && sls.width == width / 2 && srs.x == width / 2 && srs.width == width / 2,
              "shared width: the focus views as wide as the outer views");
    }
    {
        // A tester's run (2026-11-07, Quest 3 at 3656 x 3448): periphery 25%, focus density 300% over a 60% x 45% box.
        // The focus views are 1.8 x 1.35 of the native eye; with one shared width they were 914 px wide (42% density
        // across). With a width per pair the column is the focus views' width and the outer views take their own.
        const simqvfr::Views4Layout tester = simqvfr::views4_layout(0.25, 1.8, 1.35);
        const int nativeW = 3656, nativeH = 3448;
        const int eyeW = static_cast<int>(nativeW * tester.column_eye_width()),
                  eyeH = static_cast<int>(nativeH * tester.column_eye_height());
        const PassRect o0 = pass_rect(tester, kPassLeft, 2 * eyeW, eyeH), o1 = pass_rect(tester, kPassRight, 2 * eyeW, eyeH),
                       f0 = pass_rect(tester, kPassLeftSide, 2 * eyeW, eyeH),
                       f1 = pass_rect(tester, kPassRightSide, 2 * eyeW, eyeH);
        check(eyeW == 6580 && eyeH == 5516, "the offered eye: the focus views' width, outer over focus height");
        check(o0.x == 0 && o0.width == 913 && o1.x == 913 && o1.width == 913 && o0.height == 861,
              "the outer views at their own 25% (913 px), side by side from x = 0");
        check(f0.x == 0 && f0.width == 6580 && f1.x == 6580 && f1.y == 861 && f0.height == eyeH - 861,
              "the focus views at their own width, filling the column");
        const double densityAcross = f0.width / (0.6 * nativeW);
        check(std::fabs(densityAcross - 3.0) < 0.01, "the focus views' density across is the 300% asked for, not 42%");
        // Each pair's instanced viewport from x = 0 to its right view's edge, with its own padding offset, puts each
        // view exactly on its rect: at UE's 66.7% screen percentage, the right views' left edges rounded up to 4.
        const auto scaled = [](const PassRect& p, int (&r)[4]) {
            const int w = static_cast<int>(std::ceil(p.width * 2.0 / 3)), h = static_cast<int>(std::ceil(p.height * 2.0 / 3));
            const int x = (static_cast<int>(std::ceil(p.x * 2.0 / 3)) + 3) / 4 * 4, y = (static_cast<int>(std::ceil(p.y * 2.0 / 3)) + 3) / 4 * 4;
            r[0] = x;
            r[1] = y;
            r[2] = x + w;
            r[3] = y + h;
        };
        int fr[4][4];
        scaled(o0, fr[0]);
        scaled(o1, fr[1]);
        scaled(f0, fr[2]);
        scaled(f1, fr[3]);
        bool onRects = true;
        double worstShared = 0;
        for (int pair = 0; pair < 2; ++pair) {
            const int(&left)[4] = fr[2 * pair];
            const int(&right)[4] = fr[2 * pair + 1];
            const double own = right[2], p = eye_padding_offset(left, right);
            for (int side = 0; side < 2; ++side) {
                double x0, x1;
                instanced_span(own, p, side == 1, x0, x1);
                const int(&rect)[4] = side ? right : left;
                onRects &= std::fabs(x0 - rect[0]) < 0.01 && std::fabs(x1 - rect[2]) < 0.01;
                // UE's own: one width for the family (its widest right edge) and the eyes' padding offset.
                instanced_span(fr[3][2] > fr[1][2] ? fr[3][2] : fr[1][2], eye_padding_offset(fr[0], fr[1]), side == 1, x0, x1);
                worstShared = std::max(worstShared, std::fabs(x1 - rect[2]));
            }
        }
        check(onRects, "each pair's instanced viewport and padding offset put its views on their rects");
        check(worstShared > 100, "with UE's one width for the family the outer views would land far off theirs");
    }

    // The layer submits eye e as OpenComposite's half (offset e x W/2) moved and cut to view e: the view UE drew.
    bool same = true;
    for (int e = 0; e < 2; ++e) {
        const simqvfr::ViewRect view = simqvfr::views4_rect_eye_columns(gate, e, 2 * (width / 2), height);
        const int submittedX = e * (width / 2) + view.left - e * (width / 2), submittedY = view.top;
        const PassRect rendered = pass_rect(gate, kPassLeft + e, width, height);
        same &= submittedX == rendered.x && submittedY == rendered.y && view.width == rendered.width &&
                view.height == rendered.height;
    }
    check(same, "each eye is submitted as the view UE rendered for it");

    {
        // SteamVR's eye projection (GetStereoProjectionMatrix): tangents left, right, up, down; reversed infinite
        // depth, near 10.
        const float tl = -1.2f, tr = 0.9f, tu = 1.1f, td = -1.0f;
        const float eye[16] = {2 / (tr - tl), 0, 0, 0, 0, 2 / (tu - td), 0, 0,
                               -(tr + tl) / (tr - tl), (tu + td) / (tu - td), 0, 1, 0, 0, 10, 0};
        float m[16];
        std::memcpy(m, eye, sizeof(m));
        const bool narrowed = focus_projection(m, 0.4, 0.3);
        const auto close = [](float a, double b) { return std::fabs(a - b) < 1e-5; };
        check(narrowed && close(m[0], 1 / (2.1 * 0.4 / 2)) && close(m[5], 1 / (2.1 * 0.3 / 2)) && m[8] == 0 && m[9] == 0,
              "a focus view's projection: the box's share of its eye's tangent extent, centred");
        check(m[11] == 1 && m[14] == 10 && m[10] == 0 && m[15] == 0, "the focus view keeps its eye's depth");
        std::memcpy(m, eye, sizeof(m));
        check(focus_projection(m, 0.4, 0) && close(m[5], 1 / (2.1 * 0.4 / 2)), "no focus height: the box's width share");
        std::memcpy(m, eye, sizeof(m));
        check(!focus_projection(m, 0, 0) && std::memcmp(m, eye, sizeof(m)) == 0, "no focus box: the eye's own projection");
    }
    {
        // The focus turn in UE's frame (X forward, Y right, Z up): a gaze right of centre turns the forward axis
        // right, a gaze above it turns it up; applied in the eye's own frame (on the right of its orientation).
        const float eye[4] = {-1.2f, 1.0f, 1.0f, -1.1f};
        const float forward[3] = {1, 0, 0};
        float f[3];
        ue_rotate(ue_quat(simqvfr::focus_turn(eye, 0.4, 0.4, 0.3, 0.0)), forward, f);
        check(f[1] > 0.25f && std::fabs(f[2]) < 1e-5f && f[0] > 0.9f, "a gaze to the right turns the focus view right");
        ue_rotate(ue_quat(simqvfr::focus_turn(eye, 0.4, 0.4, 0.0, 0.3)), forward, f);
        check(f[2] > 0.25f && std::fabs(f[1]) < 1e-5f, "a gaze upwards turns the focus view up");
        // An eye yawed 90 degrees right (forward = +Y): the same right turn moves forward from +Y towards -X.
        const float s = std::sqrt(0.5f);
        const UeQuat yawed{0, 0, s, s};
        ue_rotate(ue_mul(yawed, ue_quat(simqvfr::focus_turn(eye, 0.4, 0.4, 0.3, 0.0))), forward, f);
        check(f[1] > 0.9f && f[0] < -0.25f, "the turn applies in the eye's own frame");
    }
    {
        // A frame's record from its views as rendered: each eye's view matrix from its orientation (UE: local to
        // world), each focus view's from the eye's turned in its own frame; the record's turns are the ones applied.
        const auto view_matrix = [](const UeQuat& q, float (&v)[16]) {
            float c[9];  // column form: world = C local
            simqvfr::to_matrix(XrQuaternionf{q.x, q.y, q.z, q.w}, c);
            // Row vectors: local = world C; then UE's view axes (x right = local Y, y up = local Z, z forward = X).
            for (int r = 0; r < 3; ++r) {
                v[r * 4 + 0] = c[r * 3 + 1];
                v[r * 4 + 1] = c[r * 3 + 2];
                v[r * 4 + 2] = c[r * 3 + 0];
                v[r * 4 + 3] = 0;
            }
            v[12] = v[13] = v[14] = 0;
            v[15] = 1;
        };
        const float eyeTan[2][4] = {{-1.2f, 1.0f, 1.0f, -1.1f}, {-1.0f, 1.2f, 1.0f, -1.1f}};
        const XrQuaternionf turn[2] = {simqvfr::focus_turn(eyeTan[0], 0.4, 0.4, 0.3, -0.2),
                                       simqvfr::focus_turn(eyeTan[1], 0.4, 0.4, -0.25, 0.1)};
        const UeQuat head = ue_mul(UeQuat{0, 0, std::sin(0.3f), std::cos(0.3f)}, UeQuat{0, std::sin(-0.1f), 0, std::cos(-0.1f)});
        const UeQuat eyes[2] = {ue_mul(head, UeQuat{0, 0, std::sin(-0.02f), std::cos(-0.02f)}), head};
        float views[4][16], projections[4][16];
        for (int e = 0; e < 2; ++e) {
            view_matrix(eyes[e], views[e]);
            view_matrix(ue_mul(eyes[e], ue_quat(turn[e])), views[e + 2]);
            const float* t = eyeTan[e];
            const float eyeProjection[16] = {2 / (t[1] - t[0]), 0, 0, 0, 0, 2 / (t[2] - t[3]), 0, 0,
                                             -(t[1] + t[0]) / (t[1] - t[0]), (t[2] + t[3]) / (t[2] - t[3]), 0, 1,
                                             0, 0, 10, 0};
            std::memcpy(projections[e], eyeProjection, sizeof(eyeProjection));
            std::memcpy(projections[e + 2], eyeProjection, sizeof(eyeProjection));
            focus_projection(projections[e + 2], 0.4, 0.4);
        }
        simqvfr::Views4Frame frame{};
        const bool ok = rendered_frame(views, projections, frame);
        bool match = ok;
        for (int e = 0; e < 2 && ok; ++e) {
            float want[9], got[9];
            simqvfr::to_matrix(simqvfr::qconj(turn[e]), want);
            simqvfr::to_matrix(simqvfr::qconj(frame.turn[e]), got);
            for (int i = 0; i < 9; ++i)
                match &= std::fabs(want[i] - got[i]) < 1e-4f;
            match &= std::fabs(frame.outerTan[e][0] - eyeTan[e][0]) < 1e-4f &&
                    std::fabs(frame.focusTan[e][1] - 0.4f * (eyeTan[e][1] - eyeTan[e][0]) / 2) < 1e-4f;
        }
        check(match, "a frame's record from its rendered views: the turns and tangents applied");
        views[1][0] = -views[1][0];  // a mirrored view (a planar reflection's)
        views[1][1] = -views[1][1];
        views[1][2] = -views[1][2];
        simqvfr::Views4Frame kept = frame;
        check(!rendered_frame(views, projections, kept) && std::memcmp(&kept, &frame, sizeof(kept)) == 0,
              "a mirrored view: no record");
    }

    {
        // The left eye as UE's SteamVR plugin builds it from OpenComposite (this headset, 2026-11-04): the located FOV's
        // tangents l, r, u, d = -1.4019, 1.2131, 1.0913, -1.0913; UE passes &Right, &Left, &Top, &Bottom to
        // GetProjectionRaw, negates them, and fills its matrix: [8] = -(r + l) / (r - l), [9] = -(u + d) / (u - d).
        const float eL = -1.4019f, eR = 1.2131f, eU = 1.0913f, eD = -1.0913f;
        const float ue[16] = {2 / (eR - eL), 0, 0, 0, 0, 2 / (eU - eD), 0, 0, -(eR + eL) / (eR - eL),
                              -(eU + eD) / (eU - eD), 0, 1, 0, 0, 10, 0};
        float t[4];
        check(ue_projection_tangents(ue, t) && std::fabs(t[0] - eL) < 1e-4f && std::fabs(t[1] - eR) < 1e-4f &&
                  std::fabs(t[2] - eU) < 1e-4f && std::fabs(t[3] - eD) < 1e-4f,
              "UE's eye projection reads back as the located FOV, not mirrored");
        // A live crop of top 21%, bottom 20%, outer and inner 5% (the run that showed the flip): the located up
        // tangent is 1.0913 - 0.21 x 2.1826 = 0.6330, the down -0.6548. UE's vertical term from that run's log
        // (read with the old sign as up 0.6548, down -0.6330) must read back with the short side at the top.
        const float cu = 0.6330f, cd = -0.6548f;
        float cropped[16];
        std::memcpy(cropped, ue, sizeof(cropped));
        cropped[5] = 2 / (cu - cd);
        cropped[9] = -(cu + cd) / (cu - cd);
        check(cropped[9] > 0 && ue_projection_tangents(cropped, t) && std::fabs(t[2] - cu) < 1e-4f &&
                  std::fabs(t[3] - cd) < 1e-4f,
              "a top crop narrows the top of UE's projection, not its bottom");

        // Off-axis focus views (quad/offaxis_focus.h, views.cpp offaxis_projection): UE's matrix has the shared shape
        // (w = z in [11], depth in [14]), so the eye's projection narrowed to a 30% box at the gaze (0.3, -0.2) reads
        // back with ue_projection_tangents as exactly that box.
        check(simqvfr::projection_shape(ue), "UE's eye projection has the off-axis maths' shape");
        const double fx = 0.3, fy = 0.3;
        double cx = 0.3, cy = -0.2;
        simqvfr::FocusBox box;
        float focus[16];
        const bool built = simqvfr::clamp_focus_centre(ue, fx, fy, cx, cy) &&
                           simqvfr::focus_projection(ue, fx, fy, cx, cy, box, focus);
        const double hw = (eR - eL) * fx / 2, hh = (eU - eD) * fy / 2;
        float ft[4];
        check(built && cx == 0.3 && cy == -0.2 && ue_projection_tangents(focus, ft) && std::fabs(ft[0] - (0.3 - hw)) < 1e-4 &&
                  std::fabs(ft[1] - (0.3 + hw)) < 1e-4 && std::fabs(ft[2] - (-0.2 + hh)) < 1e-4 &&
                  std::fabs(ft[3] - (-0.2 - hh)) < 1e-4 && focus[10] == ue[10] && focus[11] == ue[11] && focus[14] == ue[14],
              "an off-axis focus projection is the box at the gaze, depth and w kept");
        // The box in the eye's image from the two tangent sets (gate_compare.h box_in_eye: what the focus gate crops)
        // is where the projection puts it.
        double rect[4];
        float eyeTan[4];
        ue_projection_tangents(ue, eyeTan);
        check(box_in_eye(eyeTan, ft, rect) && std::fabs(rect[0] - box.rect[0]) < 1e-5 && std::fabs(rect[1] - box.rect[1]) < 1e-5 &&
                  std::fabs(rect[2] - box.rect[2]) < 1e-5 && std::fabs(rect[3] - box.rect[3]) < 1e-5,
              "the focus gate's box in the eye matches the projection's own");
        // A gaze past the eye's edge keeps the box inside the eye.
        double ex = 3.0, ey = -3.0;
        check(simqvfr::clamp_focus_centre(ue, fx, fy, ex, ey) && std::fabs(ex - (eR - hw)) < 1e-4 &&
                  std::fabs(ey - (eD + hh)) < 1e-4,
              "a gaze past the eye's edge: the box held inside it");
        // A whole-eye box is the eye itself (centred on the eye, not its forward axis).
        double wx = 0, wy = 0;
        float whole[16];
        simqvfr::FocusBox wbox;
        float wt[4];
        check(simqvfr::clamp_focus_centre(ue, 1, 1, wx, wy) && simqvfr::focus_projection(ue, 1, 1, wx, wy, wbox, whole) &&
                  ue_projection_tangents(whole, wt) && std::fabs(wt[0] - eL) < 1e-4f && std::fabs(wt[1] - eR) < 1e-4f &&
                  std::fabs(wt[2] - eU) < 1e-4f && std::fabs(wt[3] - eD) < 1e-4f,
              "a box of the whole eye is the eye's own projection");
        // The record of an off-axis frame (same view matrices for eye and focus view): no turn, the box's tangents.
        float views[4][16] = {}, projections[4][16];
        for (int v = 0; v < 4; ++v) {
            views[v][0] = views[v][5] = views[v][10] = views[v][15] = 1;
            std::memcpy(projections[v], v < 2 ? ue : focus, sizeof(projections[v]));
        }
        simqvfr::Views4Frame frame{};
        check(rendered_frame(views, projections, frame) && std::fabs(frame.turn[0].w) > 0.99999f &&
                  std::fabs(frame.turn[1].w) > 0.99999f && std::fabs(frame.focusTan[0][0] - ft[0]) < 1e-5f &&
                  std::fabs(frame.focusTan[0][3] - ft[3]) < 1e-5f,
              "an off-axis frame's record: no turn, the box's own tangents");
    }

    {
        // A live layout at the launch sizes hands out the launch rects (the Cheeky run of 2026-11-08: periphery 40%, focus
        // views 37.5% x 37.5% of the native eye, target 3228 x 2616; the live focus rows came out 1265 against the
        // launch's 1266 and ACC re-created its focus views' DLSS features at every live change).
        const simqvfr::Views4Layout cheekyLaunch = simqvfr::views4_layout(0.4, 0.375, 0.375);
        const simqvfr::Views4Layout cheekyLive = simqvfr::views4_live_eye_columns(cheekyLaunch, 0.4, 0.375, 0.375);
        bool rectsEqual = cheekyLive.live;
        for (int v = 0; v < 4; ++v) {
            const simqvfr::ViewRect a = simqvfr::views4_rect_eye_columns(cheekyLaunch, v, 3228, 2616);
            const simqvfr::ViewRect b = simqvfr::views4_rect_eye_columns(cheekyLive, v, 3228, 2616);
            rectsEqual &= a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
        }
        const simqvfr::ViewRect focusRect = simqvfr::views4_rect_eye_columns(cheekyLive, 2, 3228, 2616);
        check(rectsEqual && focusRect.top == 1350 && focusRect.height == 1266,
              "a live layout at the launch sizes: the launch rects");
    }

    {
        // Live resolution in eye columns: launch periphery 0.5, focus 0.3 x 0.3, no crop: the target two 0.5 wide,
        // 0.8 high (2000 x 1600 for a 2000 x 2000 native eye); the launch eyes 1000 x 1000, the focus views
        // 600 x 600 under them.
        const simqvfr::Views4Layout launch = simqvfr::views4_layout(0.5, 0.3, 0.3);
        constexpr int W = 2000, H = 1600;
        const auto rect = [&](const simqvfr::Views4Layout& lay, int pass) { return pass_rect(lay, pass, W, H); };
        const simqvfr::Views4Layout atLaunch = simqvfr::views4_live_eye_columns(launch, 0.5, 0.3, 0.3);
        bool unchanged = atLaunch.live;
        for (int pass = kPassLeft; pass <= kPassRightSide; ++pass) {
            const PassRect a = rect(launch, pass), b = rect(atLaunch, pass);
            unchanged &= a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        }
        check(unchanged && rect(launch, kPassLeftSide).width == 600 && rect(launch, kPassRightSide).x == 600,
              "live layout at the launch sizes: the launch rects (the focus views 600 wide)");
        // A lower periphery: the outer views narrower and lower, the focus views as they were, right under them;
        // each pair side by side from x = 0.
        const simqvfr::Views4Layout low = simqvfr::views4_live_eye_columns(launch, 0.25, 0.3, 0.3);
        const PassRect lo = rect(low, kPassLeft), ro = rect(low, kPassRight), lf = rect(low, kPassLeftSide),
                       rf = rect(low, kPassRightSide);
        check(lo.x == 0 && lo.width == 500 && lo.height == 500 && ro.x == 500 && ro.width == 500 && lf.x == 0 &&
                  lf.y == 500 && lf.width == 600 && lf.height == 600 && rf.x == 600 && rf.y == 500,
              "live periphery 25%: eyes 500 x 500, the focus views keep their 600 x 600 under them");
        // The focus density live on its own: wider focus views up to the column, the periphery untouched.
        const simqvfr::Views4Fit denser = simqvfr::views4_fit_eye_columns(launch, 0.25, 0.4, 0.4);
        check(!denser.focusCapped && std::fabs(denser.focus - 0.4) < 1e-9 && std::fabs(denser.outer - 0.25) < 1e-9,
              "live focus density up: the focus views wider, the periphery as asked");
        const simqvfr::Views4Fit widest = simqvfr::views4_fit_eye_columns(launch, 0.5, 0.8, 0.3);
        check(widest.focusCapped && std::fabs(widest.focus - 0.5) < 1e-9, "live focus views no wider than the column");
        // Caps: the periphery no wider than half the target; a taller focus takes the height first.
        const simqvfr::Views4Fit wide = simqvfr::views4_fit_eye_columns(launch, 1.0, 0.3, 0.3);
        check(wide.outerCapped && !wide.outerByRoom && std::fabs(wide.outer - 0.5) < 1e-9,
              "live periphery above the target's half: capped at it");
        const simqvfr::Views4Fit tall = simqvfr::views4_fit_eye_columns(launch, 0.5, 0.3, 0.7);
        check(!tall.focusCapped && std::fabs(tall.focusHeight - 0.7) < 1e-9 && tall.outerByRoom &&
                  std::fabs(tall.outer - 0.1) < 1e-9,
              "a taller focus view first: the periphery takes the height left (held by room)");
        {
            // acc_shared_width=1: every view the periphery's width, the focus width asked for unused.
            simqvfr::Views4Layout sharedLaunch = launch;
            sharedLaunch.sharedColumnWidth = true;
            const simqvfr::Views4Fit sharedFit = simqvfr::views4_fit_eye_columns(sharedLaunch, 0.25, 0.4, 0.3);
            const simqvfr::Views4Layout sharedLow = simqvfr::views4_live_eye_columns(sharedLaunch, 0.25, 0.4, 0.3);
            check(std::fabs(sharedFit.focus - 0.25) < 1e-9 && rect(sharedLow, kPassLeftSide).width == 500 &&
                      rect(sharedLow, kPassRightSide).x == 500,
                  "shared width live: the focus views follow the periphery's width");
        }
        const simqvfr::Views4Layout tallLayout = simqvfr::views4_live_eye_columns(launch, 0.5, 0.3, 0.7);
        const PassRect tf = rect(tallLayout, kPassLeftSide);
        check(tf.y + tf.height <= H && rect(tallLayout, kPassLeft).height + tf.height <= H,
              "the outer and focus rows fit the target");
        // A frame's record (width, height) gives the compositor the same rects.
        const simqvfr::Views4Layout recorded = simqvfr::views4_with_live(launch, true, low.width, low.height);
        bool sameRects = recorded.live;
        for (int pass = kPassLeft; pass <= kPassRightSide; ++pass) {
            const PassRect a = rect(low, pass), b = rect(recorded, pass);
            sameRects &= a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        }
        check(sameRects, "a record's live layout: the rects the views were rendered in");
        // Which layout a frame was rendered with, from its final rects: the newest with their sizes at one scale (the
        // renderer's resolution fraction, its places rounded to 4); none for sizes of no layout.
        const HandedLayout handed[2] = {handed_layout(launch, W, H), handed_layout(low, W, H)};
        const auto finals = [&](const simqvfr::Views4Layout& lay, double scale, int (&out)[kViews][4]) {
            for (int v = 0; v < kViews; ++v) {
                const PassRect p = rect(lay, kPassLeft + v);
                const int x = static_cast<int>(p.x * scale) / 4 * 4, y = static_cast<int>(p.y * scale) / 4 * 4;
                out[v][0] = x;
                out[v][1] = y;
                out[v][2] = x + static_cast<int>(std::lround(p.width * scale));
                out[v][3] = y + static_cast<int>(std::lround(p.height * scale));
            }
        };
        int lowFinals[kViews][4], launchFinals[kViews][4], scaledFinals[kViews][4];
        finals(low, 1.0, lowFinals);
        finals(launch, 1.0, launchFinals);
        finals(low, 0.7, scaledFinals);
        int odd[kViews][4];
        std::memcpy(odd, launchFinals, sizeof(odd));
        odd[2][3] -= 200;  // the focus views 200 shorter: no layout handed out
        odd[3][3] -= 200;
        check(match_layout(handed, 2, lowFinals) == 1 && match_layout(handed, 2, launchFinals) == 0 &&
                  match_layout(handed, 2, scaledFinals) == 1 && match_layout(handed, 2, odd) == -1 &&
                  match_layout(handed, 0, lowFinals) == -1,
              "a frame's final rects find the layout it was rendered with, at any resolution fraction");
        check(same_sizes(handed_layout(launch, W, H), handed_layout(atLaunch, W, H)) && !same_sizes(handed[0], handed[1]),
              "handed-out layouts compare by their views' sizes");
    }
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

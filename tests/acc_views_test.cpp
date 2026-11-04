// Offline check of ACC's four-view mapping onto UE 4.26's stereo passes (src/games/acc/views_map.h): views,
// passes and eyes; the eye-column layout instanced stereo and OpenComposite need; and that each eye the layer
// submits is the view UE rendered for it; and the focus views' projection.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "games/acc/views_map.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

}  // namespace

int main() {
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
    const aceqv::Views4Layout equal;
    const PassRect l = pass_rect(equal, kPassLeft, 8000, 3000), r = pass_rect(equal, kPassRight, 8000, 3000),
                   ls = pass_rect(equal, kPassLeftSide, 8000, 3000), rs = pass_rect(equal, kPassRightSide, 8000, 3000);
    check(l.x == 0 && l.y == 0 && l.width == 4000 && l.height == 1500 && r.x == 4000 && r.y == 0 && ls.x == 0 &&
              ls.y == 1500 && ls.width == 4000 && ls.height == 1500 && rs.x == 4000 && rs.y == 1500,
          "eye columns: outer views on top, focus views below, each in its eye's half");

    // Instanced stereo draws both eyes in one viewport from x = 0 to the right eye's edge, halved: the right eye
    // must start where the left one ends. OpenComposite copies each eye's half: each eye's views stay in it.
    const aceqv::Views4Layout gate = aceqv::views4_layout(0.6, 0.4);  // acc-gate.ini
    const int width = 4844, height = 3376;
    const PassRect gl = pass_rect(gate, kPassLeft, width, height), gr = pass_rect(gate, kPassRight, width, height),
                   gls = pass_rect(gate, kPassLeftSide, width, height), grs = pass_rect(gate, kPassRightSide, width, height);
    check(gl.x == 0 && gr.x == gl.width && gr.width == gl.width && gl.width == width / 2,
          "instanced stereo: the eyes side by side from x = 0, meeting in the middle");
    check(gls.x == 0 && grs.x == gls.width && gls.width == width / 2 && grs.width == gls.width && gls.y == grs.y &&
              gls.y >= gl.height && gls.y + gls.height <= height,
          "instanced stereo, second pair: the focus views side by side from x = 0, below the eyes");
    check(gate.column_eye_width() == 0.6 && gate.column_eye_height() == 1.0 && gl.height == 2025 && gls.height == 1351,
          "gate layout: eye 60% wide, outer 60% over focus 40% high");

    // The layer submits eye e as OpenComposite's half (offset e x W/2) moved and cut to view e: the view UE drew.
    bool same = true;
    for (int e = 0; e < 2; ++e) {
        const aceqv::ViewRect view = aceqv::views4_rect_eye_columns(gate, e, 2 * (width / 2), height);
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
        ue_rotate(ue_quat(aceqv::focus_turn(eye, 0.4, 0.4, 0.3, 0.0)), forward, f);
        check(f[1] > 0.25f && std::fabs(f[2]) < 1e-5f && f[0] > 0.9f, "a gaze to the right turns the focus view right");
        ue_rotate(ue_quat(aceqv::focus_turn(eye, 0.4, 0.4, 0.0, 0.3)), forward, f);
        check(f[2] > 0.25f && std::fabs(f[1]) < 1e-5f, "a gaze upwards turns the focus view up");
        // An eye yawed 90 degrees right (forward = +Y): the same right turn moves forward from +Y towards -X.
        const float s = std::sqrt(0.5f);
        const UeQuat yawed{0, 0, s, s};
        ue_rotate(ue_mul(yawed, ue_quat(aceqv::focus_turn(eye, 0.4, 0.4, 0.3, 0.0))), forward, f);
        check(f[1] > 0.9f && f[0] < -0.25f, "the turn applies in the eye's own frame");
    }
    {
        // A frame's record from its views as rendered: each eye's view matrix from its orientation (UE: local to
        // world), each focus view's from the eye's turned in its own frame; the record's turns are the ones applied.
        const auto view_matrix = [](const UeQuat& q, float (&v)[16]) {
            float c[9];  // column form: world = C local
            aceqv::to_matrix(XrQuaternionf{q.x, q.y, q.z, q.w}, c);
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
        const XrQuaternionf turn[2] = {aceqv::focus_turn(eyeTan[0], 0.4, 0.4, 0.3, -0.2),
                                       aceqv::focus_turn(eyeTan[1], 0.4, 0.4, -0.25, 0.1)};
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
        aceqv::Views4Frame frame{};
        const bool ok = rendered_frame(views, projections, frame);
        bool match = ok;
        for (int e = 0; e < 2 && ok; ++e) {
            float want[9], got[9];
            aceqv::to_matrix(aceqv::qconj(turn[e]), want);
            aceqv::to_matrix(aceqv::qconj(frame.turn[e]), got);
            for (int i = 0; i < 9; ++i)
                match &= std::fabs(want[i] - got[i]) < 1e-4f;
            match &= std::fabs(frame.outerTan[e][0] - eyeTan[e][0]) < 1e-4f &&
                    std::fabs(frame.focusTan[e][1] - 0.4f * (eyeTan[e][1] - eyeTan[e][0]) / 2) < 1e-4f;
        }
        check(match, "a frame's record from its rendered views: the turns and tangents applied");
        views[1][0] = -views[1][0];  // a mirrored view (a planar reflection's)
        views[1][1] = -views[1][1];
        views[1][2] = -views[1][2];
        aceqv::Views4Frame kept = frame;
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
    }

    {
        // Live resolution in eye columns: launch periphery 0.5, focus 0.3 x 0.3, no crop: the target two 0.5 wide,
        // 0.8 high (2000 x 1600 for a 2000 x 2000 native eye); the launch eyes 1000 x 1000, the focus views
        // 1000 x 600 under them.
        const aceqv::Views4Layout launch = aceqv::views4_layout(0.5, 0.3, 0.3);
        constexpr int W = 2000, H = 1600;
        const auto rect = [&](const aceqv::Views4Layout& lay, int pass) { return pass_rect(lay, pass, W, H); };
        const aceqv::Views4Layout atLaunch = aceqv::views4_live_eye_columns(launch, 0.5, 0.3);
        bool unchanged = atLaunch.live;
        for (int pass = kPassLeft; pass <= kPassRightSide; ++pass) {
            const PassRect a = rect(launch, pass), b = rect(atLaunch, pass);
            unchanged &= a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        }
        check(unchanged, "live layout at the launch sizes: the launch rects");
        // A lower periphery: every view narrower (one width: instanced stereo), the outer rows lower, the focus rows
        // as tall as before, right under them; each pair side by side from x = 0.
        const aceqv::Views4Layout low = aceqv::views4_live_eye_columns(launch, 0.25, 0.3);
        const PassRect lo = rect(low, kPassLeft), ro = rect(low, kPassRight), lf = rect(low, kPassLeftSide),
                       rf = rect(low, kPassRightSide);
        check(lo.x == 0 && lo.width == 500 && lo.height == 500 && ro.x == 500 && ro.width == 500 && lf.x == 0 &&
                  lf.y == 500 && lf.width == 500 && lf.height == 600 && rf.x == 500 && rf.y == 500,
              "live periphery 25%: every view 500 wide, eyes 500 high, the focus views 600 high under them");
        // Caps: the periphery no wider than half the target; a taller focus takes the height first.
        const aceqv::Views4Fit wide = aceqv::views4_fit_eye_columns(launch, 1.0, 0.3);
        check(wide.outerCapped && !wide.outerByRoom && std::fabs(wide.outer - 0.5) < 1e-9,
              "live periphery above the target's half: capped at it");
        const aceqv::Views4Fit tall = aceqv::views4_fit_eye_columns(launch, 0.5, 0.7);
        check(!tall.focusCapped && std::fabs(tall.focusHeight - 0.7) < 1e-9 && tall.outerByRoom &&
                  std::fabs(tall.outer - 0.1) < 1e-9,
              "a taller focus view first: the periphery takes the height left (held by room)");
        const aceqv::Views4Layout tallLayout = aceqv::views4_live_eye_columns(launch, 0.5, 0.7);
        const PassRect tf = rect(tallLayout, kPassLeftSide);
        check(tf.y + tf.height <= H && rect(tallLayout, kPassLeft).height + tf.height <= H,
              "the outer and focus rows fit the target");
        // A frame's record (width, height) gives the compositor the same rects.
        const aceqv::Views4Layout recorded = aceqv::views4_with_live(launch, true, low.width, low.height);
        bool sameRects = recorded.live;
        for (int pass = kPassLeft; pass <= kPassRightSide; ++pass) {
            const PassRect a = rect(low, pass), b = rect(recorded, pass);
            sameRects &= a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        }
        check(sameRects, "a record's live layout: the rects the views were rendered in");
        // Which layout a frame was rendered with, from its final rects: the newest with their sizes at one scale (the
        // renderer's resolution fraction, its places rounded to 4); none for sizes of no layout.
        const HandedLayout handed[2] = {handed_layout(launch, W, H), handed_layout(low, W, H)};
        const auto finals = [&](const aceqv::Views4Layout& lay, double scale, int (&out)[kViews][4]) {
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

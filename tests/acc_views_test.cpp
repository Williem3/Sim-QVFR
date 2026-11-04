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
                               (tr + tl) / (tr - tl), (tu + td) / (tu - td), 0, 1, 0, 0, 10, 0};
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
                                             (t[1] + t[0]) / (t[1] - t[0]), (t[2] + t[3]) / (t[2] - t[3]), 0, 1,
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

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

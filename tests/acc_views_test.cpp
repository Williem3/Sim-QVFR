// Offline check of ACC's four-view mapping onto UE 4.26's stereo passes (src/games/acc/views_map.h): views,
// passes and eyes; the eye-column layout instanced stereo and OpenComposite need; and that each eye the layer
// submits is the view UE rendered for it.
#include <cstdio>

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
    check(gls.x + gls.width <= width / 2 && grs.x >= width / 2 && gls.y >= gl.height && gls.y + gls.height <= height,
          "each focus view in its eye's half, below the outer view");
    check(gate.column_eye_width() == 0.6 && gate.column_eye_height() == 1.0 && gl.height == 2025 && gls.width == 1614,
          "gate layout: eye 60% wide, outer 60% over focus 40% high, focus 40% of the eye wide");

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

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

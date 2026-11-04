// Offline check of ACC's four-view mapping onto UE 4.26's stereo passes (src/games/acc/views_map.h): views,
// passes and eyes, and that each pass's rect is the one the layer submits for that view.
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

    const aceqv::Views4Layout quarters;
    const PassRect l = pass_rect(quarters, kPassLeft, 8000, 3000), r = pass_rect(quarters, kPassRight, 8000, 3000),
                   ls = pass_rect(quarters, kPassLeftSide, 8000, 3000), rs = pass_rect(quarters, kPassRightSide, 8000, 3000);
    check(l.x == 0 && r.x == 2000 && ls.x == 4000 && rs.x == 6000 && l.width == 2000 && rs.width == 2000 && l.height == 3000,
          "equal quarters side by side");

    // The layer submits eye e as views4_rect(layout, e, 2 x the eye's half, height): the same rect.
    const aceqv::Views4Layout unequal = aceqv::views4_layout(0.5, 0.25);
    bool same = true;
    for (int e = 0; e < 2; ++e) {
        const aceqv::ViewRect submitted = aceqv::views4_rect(unequal, e, 2 * 3001, 1700);
        const PassRect rendered = pass_rect(unequal, kPassLeft + e, 6002, 1700);
        same &= submitted.left == rendered.x && submitted.width == rendered.width && submitted.height == rendered.height;
    }
    const PassRect focus = pass_rect(unequal, kPassLeftSide, 6002, 1700);
    check(same && focus.x > 0 && focus.width > 0, "unequal views: the eyes render where the layer submits them");

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

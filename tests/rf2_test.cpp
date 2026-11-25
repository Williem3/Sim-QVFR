// Offline checks of rFactor 2's QVFR pieces that need no game or GPU: the two-view atlas rule (quad/two_view_atlas.h) on
// any eye size, the views' layout and per-draw viewports (games/rf2/render_backend.h, draw_backend.h), the focus box's
// mapping, the GPU regulator (games/rf2/regulator.h) with its GPU-busy rule, and the eye-pass rule (pass_state.h).
#include <cmath>
#include <cstdio>

#include <windows.h>

#include "games/rf2/draw_backend.h"
#include "games/rf2/pass_state.h"
#include "games/rf2/regulator.h"
#include "games/rf2/render_backend.h"
#include "quad/two_view_atlas.h"
#include "test_util.h"

using namespace simqvfr;
using namespace simqvfr::games::rf2;

namespace {

void atlas_checks() {
    // The first measured target: 3184x3096, periphery 25%, focus 50% x 50% of the eye.
    const TwoViewAtlas a = fit_two_view_atlas(3184, 3096, 0.25, 0.5, 0.5);
    check(a.focusW == 1592 && a.focusH == 1548 && a.outerW == 796 && a.outerH == 774 && !a.focusCapped && !a.outerCapped,
          "atlas: 3184x3096 at 25% / 50% gives the 796x774 outer and 1592x1548 focus views first measured");
    // Another headset's eye (Quest 3 at 1.0): the same shares, its own pixels.
    const TwoViewAtlas q = fit_two_view_atlas(2064, 2208, 0.25, 0.5, 0.5);
    check(q.focusW == 1032 && q.focusH == 1104 && q.outerW == 516 && q.outerH == 552,
          "atlas: a 2064x2208 eye gets its own sizes at the same shares");
    // The focus view takes the width first and keeps 320 px for the outer view; the outer view takes what is left.
    const TwoViewAtlas wide = fit_two_view_atlas(2000, 2000, 0.6, 0.9, 0.5);
    check(wide.focusW == 1680 && wide.focusCapped && wide.outerCapped && close_to(wide.outer, 320.0 / 2000, 1e-9) &&
              wide.outerW == 320,
          "atlas: a focus view wider than the room is capped, the outer view gets the 320 px left");
    const TwoViewAtlas tiny = fit_two_view_atlas(300, 300, 0.5, 0.5, 0.5);
    check(tiny.focusW == 0 && tiny.outerW == 0, "atlas: a target too small for the rule gives no views");
}

void layout_checks() {
    const SceneTarget target{2064, 2208, 4, DXGI_FORMAT_R16G16B16A16_FLOAT};
    const AtlasLayout l = make_atlas_layout(target, 0.25, 0.5, 0.5);
    check(l.valid() && l.width == 2064 && l.focus_x() == 2064 - 1032, "layout: the focus view at the target's top right");
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 2064;
    d.Height = 2208;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 4;
    check(target.matches(d), "layout: the locked target matches its own description");
    d.SampleDesc.Count = 1;
    check(!target.matches(d), "layout: its resolve (one sample) is no scene target");

    const D3D11_VIEWPORT full{0, 0, 2064, 2208, 0.1f, 0.9f};
    const DrawAtlas a = make_draw_atlas(full, l);
    check(a.viewport[0].Width == 516 && a.viewport[0].Height == 552 && a.viewport[1].TopLeftX == 1032 &&
              a.viewport[1].Width == 1032 && a.viewport[1].Height == 1104 && a.viewport[1].MinDepth == 0.1f &&
              a.scissor[1].left == 1032 && a.scissor[1].right == 2064 && a.scissor[1].bottom == 1104,
          "draw: the two views' viewports and scissors, the game's depth range kept");
}

void focus_checks() {
    const FocusMapping c = centred_focus_mapping(0.5, 0.4);
    check(close_to(c.remap[0][0], 2.0, 1e-6) && close_to(c.remap[1][2], 2.5, 1e-6) && close_to(c.rect[0][0], 0.25, 1e-6) &&
              close_to(c.rect[0][3], 0.7, 1e-6),
          "focus: before a FOV, the centred box and its remap");
    // A symmetric 100 x 100 degree FOV: the box from the FOV is the centred one.
    const float t = static_cast<float>(std::tan(50.0 * 3.14159265358979 / 180.0));
    const float sym[2][4] = {{-t, t, -t, t}, {-t, t, -t, t}};
    FocusMapping m;
    check(calculate_focus_mapping(sym, 0.5, 0.5, m) && close_to(m.rect[0][0], 0.25, 1e-4) && close_to(m.rect[1][2], 0.75, 1e-4),
          "focus: a symmetric FOV gives the centred box");
    FocusMapping up;
    check(calculate_focus_mapping(sym, 0.5, 0.5, up, 10.0) && up.rect[0][1] < m.rect[0][1],
          "focus: an offset up moves the box up in the image");
    const float bad[2][4] = {{t, -t, -t, t}, {-t, t, -t, t}};
    FocusMapping kept = m;
    check(!calculate_focus_mapping(bad, 0.5, 0.5, kept) && kept.rect[0][0] == m.rect[0][0],
          "focus: a FOV with no width gives no mapping and leaves the last one");
}

void regulator_checks() {
    Regulator r;
    GpuRegulatorSettings s;
    s.focus = s.periphery = true;
    s.decreaseAbove = 100;  // 10 ms
    s.increaseBelow = 90;   // 9 ms
    s.lowerFrames = 3;
    s.raiseFrames = 50;
    r.set_settings(s);
    r.set_limits(80, 50);
    check(!r.feed(12) && !r.feed(12) && r.feed(12) && r.periphery_cut() == 1 && r.density_cut() == 1,
          "regulator: three frames over the limit take both axes down 1%");
    for (int i = 0; i < 3; ++i)
        r.feed(12, false);
    check(r.periphery_cut() == 1 && r.held_not_gpu_bound() == 3,
          "regulator: frames over the limit while the GPU isn't busy (CPU-bound) take nothing down");
    r.feed(12);
    r.feed(12);
    r.feed(9.5);  // in the band: counts for neither, the count goes on
    check(r.feed(12) && r.periphery_cut() == 2, "regulator: a frame in the band between the limits doesn't break the count");
    bool up = false;
    for (int i = 0; i < 50; ++i)
        up = r.feed(5);
    check(up && r.periphery_cut() == 1, "regulator: 50 calm frames bring both up 1%");
    check(!r.feed(500), "regulator: a hitch (over 100 ms) is ignored");
    r.set_limits(0, 0);
    check(r.periphery_cut() == 0 && r.density_cut() == 0, "regulator: cuts never go below the floor's limit");
    GpuRegulatorSettings off = s;
    off.periphery = false;
    r.set_limits(80, 50);
    r.set_settings(off);
    for (int i = 0; i < 3; ++i)
        r.feed(12);
    check(r.periphery_cut() == 0 && r.density_cut() == 1, "regulator: an axis switched off stays at its set value");
}

void pass_checks() {
    PassState p;
    int doubled[2] = {}, composed = -1;
    for (unsigned pass = 1; pass <= 8; ++pass) {
        p.note_full_scene_bind(true);
        if (p.real_eye_pass())
            ++doubled[p.eye_for_current_pass()];
        p.note_full_scene_bind(false);
        if (pass == 3 || pass == 6) {
            const int eye = p.consume_composite_eye(1);
            composed = composed < 0 ? eye : composed * 10 + eye;
        }
    }
    check(doubled[0] == 3 && doubled[1] == 2 && composed == 1, "passes: 2, 3, 5 draw the left eye, 6 and 8 the right one; "
                                                               "left composed after 3, right after 6");
    check(p.consume_composite_eye(1) == -1, "passes: an eye is composed once a frame");
    p.frame_end();
    check(!p.real_eye_pass() && p.fullScenePassInFrame == 0, "passes: a frame end starts the count again");
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    atlas_checks();
    layout_checks();
    focus_checks();
    regulator_checks();
    pass_checks();
    return test_result();
}

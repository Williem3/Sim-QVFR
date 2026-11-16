// Offline check of LMU MVR's pass tracker on a WARP device (games/lmu/mvr_pass.h): observing the game's two eye
// passes (a menu frame's multisample bind a false find), then driving them (begun at the scene's bind, ended after
// the blit or at the frame end) with the focus post and finals as with the game's bounds.
#include "lmu_focus_post_kit.h"

int SIM_QVFR_TEST_MAIN() {
    const std::string folder;  // none: no YEBIS shaders of a capture here (lmu_focus_post_test takes them)
    char what[256];
#include "lmu_focus_post_rig.h"

    {  // R4b's pass tracker (observe only): a frame of two eye passes found where the game's are; a frame without the
       // game's passes (a menu) with a multisample bind reported as a false find.
        lmu::mvr_track_passes(true);
        lmu::mvr_frame_end(100000);  // a fresh frame for the tracker
        const lmu::MvrTrackStats before = lmu::mvr_track_stats();
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        eye_pass(0, &view, false, standIn);
        eye_pass(1, &view1, false, standIn);
        lmu::mvr_frame_end(100001);
        const lmu::MvrTrackStats after = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "pass tracker: the game's two passes found as its own (%llu frame matched, %llu passes compared; "
                      "draws left out at the start %llu, at the end %llu)",
                      static_cast<unsigned long long>(after.matched - before.matched),
                      static_cast<unsigned long long>(after.passesCompared - before.passesCompared),
                      static_cast<unsigned long long>(after.startGapSum - before.startGapSum),
                      static_cast<unsigned long long>(after.endGapSum - before.endGapSum));
        check(after.frames == before.frames + 1 && after.matched == before.matched + 1 &&
                  after.passesCompared == before.passesCompared + 2 && after.beganBefore == before.beganBefore &&
                  after.endedAfter == before.endedAfter && after.countDiffers == before.countDiffers &&
                  after.startGapSum == before.startGapSum && after.endGapSum == before.endGapSum,
              what);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), dsv.Get());
        context->Draw(3, 0);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        lmu::mvr_frame_end(100002);
        const lmu::MvrTrackStats menu = lmu::mvr_track_stats();
        check(menu.framesWithoutGame == after.framesWithoutGame + 1 && menu.passesWithoutGame == after.passesWithoutGame + 1,
              "pass tracker: a frame without the game's passes with a multisample scene bind: counted as a false find");
        lmu::mvr_track_passes(false);
    }

    {  // R4b-2: the tracker drives the passes: begun at the scene's bind, ended after the blit or at the frame end, the
       // focus post and finals as with the game's bounds.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100010);
        driveLog.clear();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        driveFinals = false;
        lmu::mvr_frame_end(100011);  // pass 1 ends here (no other target after its blit)
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        const bool finals = driveFinals;
        std::snprintf(what, sizeof(what),
                      "the tracker drives the passes:%s; %llu composites and %llu final blits, both eyes' focus finals made",
                      driveLog.c_str(), static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits));
        check(driveLog == " b0 e0 b1 e1" && after.focusComposites == before.focusComposites + 2 &&
                  after.focusBlits == before.focusBlits + 2 && finals,
              what);
        driveLog.clear();
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        lmu::mvr_frame_end(100012);
        check(driveLog.empty(), "the tracker drives no pass in a frame without a multisample scene bind (a menu)");
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

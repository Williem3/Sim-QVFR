// Offline check of LMU MVR's focus post past a stage between the resolve and YEBIS, with MSAA off (the scene's
// target learned, the passes driven from its binds; past a stage too) and with FSR on (no final blit, the focus
// final found at the eye's image's bind), on a WARP device.
#include "lmu_focus_post_kit.h"

int SIM_QVFR_TEST_MAIN() {
    const std::string folder;  // none: no YEBIS shaders of a capture here (lmu_focus_post_test takes them)
    char what[256];
#include "lmu_focus_post_rig.h"

    {  // A stage between the resolve and YEBIS (reflections high, the 2026-11-11 pass trace): a single-sample depth
       // drawn, the scene's target bound again, YEBIS's chain from the stage's image. Its image over the focus view:
       // the focus view's scene from it (the eye's picture by construction); over the eye only: from the resolve.
        Kit staged = standIn;
        staged.stage = 1;
        lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, staged);
        lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusStageCopies == before.focusStageCopies + 1 &&
                  after.focusComposites == before.focusComposites + 1 && after.focusFailures == before.focusFailures,
              "past a stage over the whole target: the focus view's scene from its image, then the composite");
        same_as_eye("past a stage over the whole target", 100, true);
        final_as_eye("past a stage over the whole target", 100);
        staged.stage = 2;
        before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, staged);
        after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusStageResolves == before.focusStageResolves + 1 &&
                  after.focusComposites == before.focusComposites + 1 && after.focusFailures == before.focusFailures,
              "past a stage over the eye only: the focus view's scene from the resolve, then the composite");

        // The tracker driving: the rebind of the pass's own scene before YEBIS keeps the pass open (before, it ended
        // there at "the next pass's scene" and YEBIS ran outside it: no focus finals, the fallback after 90 frames).
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100015);
        driveLog.clear();
        staged.stage = 1;
        before = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackBefore = lmu::mvr_track_stats();
        eye_pass(0, nullptr, false, staged, false, true);
        eye_pass(1, nullptr, false, staged, false, true);
        driveFinals = false;
        lmu::mvr_frame_end(100016);
        after = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackAfter = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "past a stage, the tracker driving:%s; scene rebinds kept %llu; %llu composites, %llu final blits, "
                      "both eyes' focus finals made",
                      driveLog.c_str(), static_cast<unsigned long long>(trackAfter.sceneRebinds - trackBefore.sceneRebinds),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits));
        check(driveLog == " b0 e0 b1 e1" && trackAfter.sceneRebinds == trackBefore.sceneRebinds + 2 &&
                  after.focusComposites == before.focusComposites + 2 && after.focusBlits == before.focusBlits + 2 &&
                  driveFinals,
              what);
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    {  // MSAA off: no multisample target, no resolve; YEBIS's copy reads the scene's own target. A frame of it without
       // passes teaches the tracker that target (what the copy reads, drawn with depth); from the next frame it drives
       // the passes from its binds with depth, and their post-processing begins at the copy: the focus post and finals
       // as with MSAA. A multisample frame after it: the passes from the multisample binds again.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100020);
        driveLog.clear();
        // The frame it learns in: both eyes, no pass begun (eye 1's scene bind comes after the learning, mid-frame).
        eye_pass(0, nullptr, false, standIn, false, true, true);
        eye_pass(1, nullptr, false, standIn, false, true, true);
        lmu::mvr_frame_end(100021);
        const bool learnedWithoutPasses = driveLog.empty();
        // A frame whose focus views don't fit (the first after loading, at the game's own size: no view handed over):
        // no focus post, yet both passes found, pass 0 ending after its own final blit (after which pass 1's end
        // applies the live resolution).
        driveLog.clear();
        driveViews[0] = driveViews[1] = nullptr;
        const lmu::MvrTrackStats unfitBefore = lmu::mvr_track_stats();
        const lmu::MvrPassStats unfitPost = lmu::mvr_pass_stats();
        eye_pass(0, nullptr, false, standIn, false, true, true);
        eye_pass(1, nullptr, false, standIn, false, true, true);
        lmu::mvr_frame_end(100024);
        const lmu::MvrTrackStats unfit = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "MSAA off, the focus views not fitting: passes%s, %llu final blits seen, %llu composites, the "
                      "fallback refused (single-sample: %s)",
                      driveLog.c_str(), static_cast<unsigned long long>(unfit.singleSampleBlits - unfitBefore.singleSampleBlits),
                      static_cast<unsigned long long>(lmu::mvr_pass_stats().focusComposites - unfitPost.focusComposites),
                      lmu::mvr_single_sample_passes() ? "yes" : "NO");
        check(driveLog == " b0 e0 b1 e1" && unfit.singleSampleBlits == unfitBefore.singleSampleBlits + 2 &&
                  lmu::mvr_pass_stats().focusComposites == unfitPost.focusComposites && lmu::mvr_single_sample_passes(),
              what);
        // A blit reading the scene's target before the pass's post-processing began (a draw like the final blit
        // mid-pass): not the pass's end, so the eye's pass stays one and the other eye is still found.
        driveLog.clear();
        const lmu::MvrTrackStats strayBefore = lmu::mvr_track_stats();
        const uint64_t postsBefore = lmu::mvr_pass_stats().singleSamplePosts;
        {
            ID3D11RenderTargetView* const sceneTarget = sceneRtv.Get();
            context->OMSetRenderTargets(1, &sceneTarget, dsv1.Get());  // eye 0's scene: its pass begins
            context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
            context->IASetInputLayout(blitLayout.Get());
            const UINT stride = sizeof(BlitVertex), offset = 0;
            context->IASetVertexBuffers(0, 1, blitVb.GetAddressOf(), &stride, &offset);
            context->VSSetShader(blitVs.Get(), nullptr, 0);
            context->PSSetShader(blitPs.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            const D3D11_VIEWPORT whole = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
            context->RSSetViewports(1, &whole);
            context->Draw(6, 6);  // the stray blit, before any copy
            context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);  // another target after it
        }
        eye_pass(0, nullptr, false, standIn, false, true, true);  // the rest of eye 0 (its scene bind again, its post)
        eye_pass(1, nullptr, false, standIn, false, true, true);
        lmu::mvr_frame_end(100025);
        const lmu::MvrTrackStats stray = lmu::mvr_track_stats();
        const uint64_t posts = lmu::mvr_pass_stats().singleSamplePosts - postsBefore;
        std::snprintf(what, sizeof(what),
                      "MSAA off, a blit reading the scene before the copy: passes%s, both eyes' post-processing begun (%llu), "
                      "%llu blits before a post not taken, %llu passes ended before their post",
                      driveLog.c_str(), static_cast<unsigned long long>(posts),
                      static_cast<unsigned long long>(stray.blitsBeforePost - strayBefore.blitsBeforePost),
                      static_cast<unsigned long long>(stray.passesWithoutPost - strayBefore.passesWithoutPost));
        check(driveLog == " b0 e0 b1 e1" && posts == 2 && stray.blitsBeforePost == strayBefore.blitsBeforePost + 1 &&
                  stray.passesWithoutPost == strayBefore.passesWithoutPost,
              what);
        {  // A small view of the world post-processed the same way, outside the passes (LMU's mirror: 2560x160 beside a
           // 3120x2505 scene): drawn with a depth target, read by YEBIS's copy. It must not be learned in the scene's
           // place: the next passes are still found on the scene's own target.
            const UINT mh = H / 8 ? H / 8 : 1;
            ComPtr<ID3D11Texture2D> mirror = texture(device.Get(), W, mh, DXGI_FORMAT_R16G16B16A16_FLOAT, 1,
                                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            ComPtr<ID3D11Texture2D> mirrorDepth = texture(device.Get(), W, mh, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
            ComPtr<ID3D11RenderTargetView> mirrorRtv;
            ComPtr<ID3D11ShaderResourceView> mirrorSrv;
            ComPtr<ID3D11DepthStencilView> mirrorDsv;
            device->CreateRenderTargetView(mirror.Get(), nullptr, &mirrorRtv);
            device->CreateShaderResourceView(mirror.Get(), nullptr, &mirrorSrv);
            device->CreateDepthStencilView(mirrorDepth.Get(), nullptr, &mirrorDsv);
            const lmu::MvrTrackStats mirrorBefore = lmu::mvr_track_stats();
            driveLog.clear();
            context->OMSetRenderTargets(1, mirrorRtv.GetAddressOf(), mirrorDsv.Get());  // drawn with depth, before the eyes
            context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
            context->IASetInputLayout(nullptr);
            context->VSSetShader(yebisVs.Get(), nullptr, 0);
            context->VSSetConstantBuffers(1, 1, rectCb.GetAddressOf());
            write(context.Get(), standIn.copyCb, standIn.copyData, standIn.copyBytes);
            context->PSSetShader(standIn.copy, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &standIn.copyCb);
            context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
            context->PSSetShaderResources(0, 1, mirrorSrv.GetAddressOf());  // YEBIS's copy reads it
            context->RSSetViewports(1, &eyeVp);
            context->DrawIndexed(3, 0, 0);
            ID3D11ShaderResourceView* noInput = nullptr;
            context->PSSetShaderResources(0, 1, &noInput);
            eye_pass(0, nullptr, false, standIn, false, true, true);
            eye_pass(1, nullptr, false, standIn, false, true, true);
            lmu::mvr_frame_end(100026);
            const lmu::MvrTrackStats mirrorAfter = lmu::mvr_track_stats();
            std::snprintf(what, sizeof(what),
                          "MSAA off, a %ux%u view post-processed outside the passes (a mirror): passes%s, the scene's target "
                          "learned again %llu times, %llu candidates refused",
                          W, mh, driveLog.c_str(),
                          static_cast<unsigned long long>(mirrorAfter.sceneRelearned - mirrorBefore.sceneRelearned),
                          static_cast<unsigned long long>(mirrorAfter.sceneCandidatesRefused - mirrorBefore.sceneCandidatesRefused));
            check(driveLog == " b0 e0 b1 e1" && mirrorAfter.sceneRelearned == mirrorBefore.sceneRelearned &&
                      mirrorAfter.sceneCandidatesRefused == mirrorBefore.sceneCandidatesRefused + 1,
                  what);
        }
        driveViews[0] = &view;
        driveViews[1] = &view1;
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackBefore = lmu::mvr_track_stats();
        driveLog.clear();
        eye_pass(0, nullptr, false, standIn, false, true, true);
        eye_pass(1, nullptr, false, standIn, false, true, true);
        driveFinals = false;
        lmu::mvr_frame_end(100022);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackAfter = lmu::mvr_track_stats();
        const bool finals = driveFinals, single = lmu::mvr_single_sample_passes();
        std::snprintf(what, sizeof(what),
                      "MSAA off: the scene's target learned in a frame without passes (%s), then the tracker drives the "
                      "passes:%s; post-processings begun at the copy %llu, resolves %llu; %llu composites and %llu final "
                      "blits, both eyes' focus finals made; the passes said single-sample",
                      learnedWithoutPasses ? "none driven" : "SOME DRIVEN", driveLog.c_str(),
                      static_cast<unsigned long long>(after.singleSamplePosts - before.singleSamplePosts),
                      static_cast<unsigned long long>(after.resolves - before.resolves),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits));
        check(learnedWithoutPasses && driveLog == " b0 e0 b1 e1" && after.singleSamplePosts == before.singleSamplePosts + 2 &&
                  after.resolves == before.resolves && after.focusComposites == before.focusComposites + 2 &&
                  after.focusBlits == before.focusBlits + 2 && finals && single &&
                  trackAfter.singleSamplePasses == trackBefore.singleSamplePasses + 2,
              what);
        driveLog.clear();
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        lmu::mvr_frame_end(100023);
        const lmu::MvrPassStats msaaAgain = lmu::mvr_pass_stats();
        check(driveLog == " b0 e0 b1 e1" && msaaAgain.resolves == after.resolves + 2 &&
                  msaaAgain.singleSamplePosts == after.singleSamplePosts && !lmu::mvr_single_sample_passes(),
              "MSAA on again: the passes from the multisample binds, their post-processing at the resolve");
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }
    {  // MSAA off past a stage (post effects medium and up, the 2026-11-12 pass trace): the stage draws its image of
       // the scene, YEBIS's copy starts from that image and its composite draws into the scene's target. The copy
       // never reads the scene's target, so it is learned from the composite (a draw reading the copy's source into a
       // target of its size drawn with depth); in the passes the post-processing begins at the stage's draw.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        Kit staged = standIn;
        staged.stage = 1;
        lmu::mvr_track_passes(true);  // (off above: the scene's target forgotten)
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100040);
        const lmu::MvrTrackStats learnBefore = lmu::mvr_track_stats();
        driveLog.clear();
        eye_pass(0, nullptr, false, staged, false, true, true);
        eye_pass(1, nullptr, false, staged, false, true, true);
        lmu::mvr_frame_end(100041);
        const lmu::MvrTrackStats learnAfter = lmu::mvr_track_stats();
        const bool learnedQuietly = driveLog.empty();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackBefore = lmu::mvr_track_stats();
        eye_pass(0, nullptr, false, staged, false, true, true);
        eye_pass(1, nullptr, false, staged, false, true, true);
        driveFinals = false;
        lmu::mvr_frame_end(100042);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackAfter = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "MSAA off past a stage: the scene's target learned at the composite %llu times (%s), then the "
                      "tracker drives the passes:%s; post-processings begun %llu (%llu at the stage's draw); the focus "
                      "view's scene from the stage's image %llu, %llu composites, %llu final blits, %llu final blits seen, "
                      "%llu steps failed, finals %s",
                      static_cast<unsigned long long>(learnAfter.sceneAtComposite - learnBefore.sceneAtComposite),
                      learnedQuietly ? "none driven" : "SOME DRIVEN", driveLog.c_str(),
                      static_cast<unsigned long long>(after.singleSamplePosts - before.singleSamplePosts),
                      static_cast<unsigned long long>(after.singleSampleStagePosts - before.singleSampleStagePosts),
                      static_cast<unsigned long long>(after.focusStageCopies - before.focusStageCopies),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits),
                      static_cast<unsigned long long>(trackAfter.singleSampleBlits - trackBefore.singleSampleBlits),
                      static_cast<unsigned long long>(after.focusFailures - before.focusFailures),
                      driveFinals ? "made" : "NOT MADE");
        check(learnAfter.sceneAtComposite == learnBefore.sceneAtComposite + 1 && learnedQuietly &&
                  driveLog == " b0 e0 b1 e1" && after.singleSamplePosts == before.singleSamplePosts + 2 &&
                  after.singleSampleStagePosts == before.singleSampleStagePosts + 2 &&
                  after.focusStageCopies == before.focusStageCopies + 2 &&
                  after.focusComposites == before.focusComposites + 2 && after.focusBlits == before.focusBlits + 2 &&
                  trackAfter.singleSampleBlits == trackBefore.singleSampleBlits + 2 &&
                  after.focusFailures == before.focusFailures && driveFinals && lmu::mvr_single_sample_passes(),
              what);
        same_as_eye("MSAA off past a stage", 100, true);  // (the finals: driveFinals, read at pass 1's end)
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    {  // FSR on (mvr_fsr): no final blit; after YEBIS's composite the game's compute upscale writes the whole scene
       // target into the eye's image, then binds that image. The focus composite is put beside the eye in the scene's
       // target right after the composite, so the upscale (here: a copy of the whole target, by an unmarked shader after
       // the bind) carries it beside the eye; the focus final is found at the eye's image's bind.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        Kit fsr = standIn;
        fsr.fsr = true;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_fsr(true, eyeFinal.Get(), eyeFinal.Get());
        lmu::mvr_frame_end(100050);
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        driveLog.clear();
        driveFinals = false;
        eye_pass(0, nullptr, false, fsr, false, true);
        unsigned x = 0, y = 0, fw = 0, fh = 0;
        const bool final0 = lmu::mvr_focus_final(0, x, y, fw, fh) && x == eyeW && y == 0 && fw == focusW && fh == focusH;
        const std::vector<uint8_t> all = read_rgba8(device.Get(), context.Get(), eyeFinal.Get(), W, H);
        int worst = 0;
        unsigned lo = 255, hi = 0;
        for (unsigned yy = 0; yy < focusH; ++yy)
            for (unsigned xx = 0; xx < focusW; ++xx)
                for (unsigned c = 0; c < 3; ++c) {
                    const uint8_t e = all[(static_cast<size_t>(yy) * W + xx) * 4 + c];
                    const uint8_t f = all[(static_cast<size_t>(yy) * W + eyeW + xx) * 4 + c];
                    worst = (std::max)(worst, std::abs(static_cast<int>(e) - static_cast<int>(f)));
                    lo = (std::min)(lo, static_cast<unsigned>(e));
                    hi = (std::max)(hi, static_cast<unsigned>(e));
                }
        eye_pass(1, nullptr, false, fsr, false, true);
        lmu::mvr_frame_end(100051);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        std::snprintf(what, sizeof(what),
                      "FSR on, the tracker driving:%s; %llu focus composites put beside the eye, %llu finals found at the "
                      "eye's image's bind (eye 0's %s), finals %s; the focus final equals the eye's own over the same crop: "
                      "largest difference %d/255 (the eye's values %u..%u)",
                      driveLog.c_str(), static_cast<unsigned long long>(after.fsrPlaced - before.fsrPlaced),
                      static_cast<unsigned long long>(after.fsrFinals - before.fsrFinals), final0 ? "beside the eye" : "NOT THERE",
                      driveFinals ? "made" : "NOT MADE", worst, lo, hi);
        check(driveLog == " b0 e0 b1 e1" && after.fsrPlaced == before.fsrPlaced + 2 && after.fsrFinals == before.fsrFinals + 2 &&
                  after.focusFailures == before.focusFailures && final0 && driveFinals && worst <= 3 && hi - lo >= 100,
              what);
        lmu::mvr_fsr(false, nullptr, nullptr);
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

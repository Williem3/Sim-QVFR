// Offline check of LMU MVR's frame shape and the draws known by role on a WARP device: the shape the recovery waits
// for, no YEBIS shader known by checksum (taken by their bindings), the lens mask, a VR UI panel redrawn, the game's
// CopySubresourceRegion remembered, and the desktop mirror's back buffer never held.
#include "lmu_focus_post_kit.h"

int SIM_QVFR_TEST_MAIN() {
    const std::string folder;  // none: no YEBIS shaders of a capture here (lmu_focus_post_test takes them)
    char what[256];
#include "lmu_focus_post_rig.h"

    {  // The frame's shape (mvr_shape_ready): the steps the focus views are made at, seen on the eyes' own
       // post-processing whether or not focus views are drawn, so the recovery only tries where a try can work.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100060);
        const bool none = !lmu::mvr_shape_ready();  // a frame without passes (a menu)
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        lmu::mvr_frame_end(100061);
        const bool foveated = lmu::mvr_shape_ready();
        // Focus views after a frame with both: the draws' shape checks skipped (their CPU), the frame ready all the same.
        const uint64_t skippedBefore = lmu::mvr_pass_stats().shapeSkippedPasses;
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        lmu::mvr_frame_end(100062);
        const bool skipped = lmu::mvr_shape_ready() && lmu::mvr_pass_stats().shapeSkippedPasses == skippedBefore + 2;
        driveViews[0] = driveViews[1] = nullptr;  // the eyes alone (plain: no focus view handed over)
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        lmu::mvr_frame_end(100063);
        const bool plain = lmu::mvr_shape_ready() && lmu::mvr_pass_stats().focusComposites == before.focusComposites;
        Kit staged = standIn;
        staged.stage = 1;
        eye_pass(0, nullptr, false, staged, false, true);
        eye_pass(1, nullptr, false, staged, false, true);
        lmu::mvr_frame_end(100064);
        const bool pastStage = lmu::mvr_shape_ready();
        lmu::mvr_shape_force_bad(true);
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        lmu::mvr_frame_end(100065);
        const bool forced = !lmu::mvr_shape_ready();
        lmu::mvr_shape_force_bad(false);
        std::snprintf(what, sizeof(what),
                      "the frame's shape: not ready without passes (%d), ready with focus views (%d), ready and not "
                      "looked at after a frame with them (%d), ready with the eyes alone and no focus post run (%d), past "
                      "a stage (%d), not ready when forced (%d)",
                      none, foveated, skipped, plain, pastStage, forced);
        check(none && foveated && skipped && plain && pastStage && forced, what);
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }
    {  // No YEBIS shader known by its checksum (a game update compiled them anew): the copy and the final blit by what
       // they bind, the composite as before; MSAA on, then off (the scene's target learned by the composite's
       // bindings alone).
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        ComPtr<ID3D11PixelShader> copyPlain = ps(device.Get(), kCopyPs), compositePlain = ps(device.Get(), kCompositePs),
                                  blitPlain = ps(device.Get(), kBlitPs);
        Kit unknown = standIn;
        unknown.copy = copyPlain.Get();
        unknown.composite = compositePlain.Get();
        unknown.blitPs = blitPlain.Get();
        lmu::mvr_roles_known_for_tests(0, 0);
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100070);
        lmu::MvrPassStats before = lmu::mvr_pass_stats();
        driveLog.clear();
        driveFinals = false;
        eye_pass(0, nullptr, false, unknown, false, true);
        eye_pass(1, nullptr, false, unknown, false, true);
        lmu::mvr_frame_end(100071);
        lmu::MvrPassStats after = lmu::mvr_pass_stats();
        std::snprintf(what, sizeof(what),
                      "no shader known by checksum, MSAA on:%s; copies by their bindings %llu, composites %llu (%llu by "
                      "bindings), final blits %llu (%llu by bindings), %llu steps failed, finals %s, the shape %s",
                      driveLog.c_str(), static_cast<unsigned long long>(after.focusCopiesByBindings - before.focusCopiesByBindings),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusCompositesByBindings - before.focusCompositesByBindings),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits),
                      static_cast<unsigned long long>(after.focusBlitsByBindings - before.focusBlitsByBindings),
                      static_cast<unsigned long long>(after.focusFailures - before.focusFailures),
                      driveFinals ? "made" : "NOT MADE", lmu::mvr_shape_ready() ? "ready" : "NOT READY");
        check(driveLog == " b0 e0 b1 e1" && after.focusCopiesByBindings == before.focusCopiesByBindings + 2 &&
                  after.focusComposites == before.focusComposites + 2 &&
                  after.focusCompositesByBindings == before.focusCompositesByBindings + 2 &&
                  after.focusBlits == before.focusBlits + 2 && after.focusBlitsByBindings == before.focusBlitsByBindings + 2 &&
                  after.focusFailures == before.focusFailures && driveFinals && lmu::mvr_shape_ready(),
              what);
        same_as_eye("no shader known by checksum", 100, true);
        // MSAA off: the tracker off and on (the learned target forgotten), a frame to learn in, then driven.
        lmu::mvr_track_passes(false);
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100072);
        const lmu::MvrTrackStats learnBefore = lmu::mvr_track_stats();
        driveLog.clear();
        eye_pass(0, nullptr, false, unknown, false, true, true);
        eye_pass(1, nullptr, false, unknown, false, true, true);
        lmu::mvr_frame_end(100073);
        const bool learned = lmu::mvr_track_stats().sceneAtComposite == learnBefore.sceneAtComposite + 1 && driveLog.empty();
        before = lmu::mvr_pass_stats();
        driveFinals = false;
        eye_pass(0, nullptr, false, unknown, false, true, true);
        eye_pass(1, nullptr, false, unknown, false, true, true);
        lmu::mvr_frame_end(100074);
        after = lmu::mvr_pass_stats();
        std::snprintf(what, sizeof(what),
                      "no shader known by checksum, MSAA off: the scene's target learned by the composite's bindings (%s), "
                      "then:%s; composites %llu, final blits %llu, %llu steps failed, finals %s, the shape %s",
                      learned ? "yes" : "NO", driveLog.c_str(),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits),
                      static_cast<unsigned long long>(after.focusFailures - before.focusFailures),
                      driveFinals ? "made" : "NOT MADE", lmu::mvr_shape_ready() ? "ready" : "NOT READY");
        check(learned && driveLog == " b0 e0 b1 e1" && after.focusComposites == before.focusComposites + 2 &&
                  after.focusBlits == before.focusBlits + 2 && after.focusFailures == before.focusFailures && driveFinals &&
                  lmu::mvr_shape_ready(),
              what);
        lmu::mvr_roles_known_for_tests(1, 1);
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    const auto marked = [&](lmu::VsRole role) {  // a vertex shader with a role, as the probe marks the game's
        ComPtr<ID3D11VertexShader> s = vs(device.Get(), kPlainVs);
        const UINT r = static_cast<UINT>(role);
        if (s)
            s->SetPrivateData(lmu::kVsRoleGuid, sizeof(r), &r);
        return s;
    };
    {  // R6c: the lens mask's draw by its vertex shader's role: in a pass drawing its focus view, like the scene's (the
       // twin, two views); while the mask is stale, not drawn at all.
        lmu::mvr_frame_end(100030);
        ComPtr<ID3D11VertexShader> maskVs = marked(lmu::VsRole::lensMask), maskTwin = vs(device.Get(), kPlainVs);
        maskVs->SetPrivateDataInterface(lmu::kMvrTwinGuid, maskTwin.Get());
        lmu::mvr_twins_changed();
        std::atomic<bool> stale{false};
        lmu::mvr_mask_stale(&stale);
        ComPtr<ID3D11Query> query;
        const D3D11_QUERY_DESC qd = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
        device->CreateQuery(&qd, &query);
        const auto vertices_drawn = [&]() -> UINT64 {
            context->Begin(query.Get());
            context->Draw(3, 0);
            context->End(query.Get());
            D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
            while (context->GetData(query.Get(), &s, sizeof(s), 0) == S_FALSE) {
            }
            return s.IAVertices;
        };
        lmu::mvr_pass_begin(0, &view, false);
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), dsv.Get());
        context->IASetInputLayout(nullptr);
        context->VSSetShader(maskVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->RSSetViewports(1, &eyeVp);
        const lmu::MvrPassStats s0 = lmu::mvr_pass_stats();
        const UINT64 drawn = vertices_drawn();
        const bool twoViews = bound_vs(context.Get()) == maskTwin.Get() && twoViewsOn == 1;
        stale = true;
        const UINT64 dropped = vertices_drawn();
        stale = false;
        const lmu::MvrPassStats s1 = lmu::mvr_pass_stats();
        lmu::mvr_pass_end(0);
        lmu::mvr_mask_stale(nullptr);
        std::snprintf(what, sizeof(what),
                      "R6c: the lens mask's draw (its shader's role) drawn with two views like the scene's (%llu vertices), "
                      "not at all while stale (%llu); counted %llu drawn, %llu with two views, %llu dropped",
                      static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(dropped),
                      static_cast<unsigned long long>(s1.maskDraws - s0.maskDraws),
                      static_cast<unsigned long long>(s1.maskDrawsTwo - s0.maskDrawsTwo),
                      static_cast<unsigned long long>(s1.maskDropped - s0.maskDropped));
        check(drawn == 3 && twoViews && dropped == 0 && s1.maskDraws == s0.maskDraws + 1 &&
                  s1.maskDrawsTwo == s0.maskDrawsTwo + 1 && s1.maskDropped == s0.maskDropped + 1,
              what);
    }
    {  // R6b: a VR UI panel's draw outside the passes in a frame without focus finals (a menu): the callback right before
       // and right after it; mvr_redraw_panel draws it again into another image at that image's size, the game's target,
       // viewport and scissor put back. Another shader's draw: no callback.
        lmu::mvr_frame_end(100031);  // a frame without focus finals
        ComPtr<ID3D11VertexShader> panelVs = marked(lmu::VsRole::vruiPanel), otherVs = vs(device.Get(), kPlainVs);
        lmu::mvr_twins_changed();
        ComPtr<ID3D11PixelShader> panelPs =
            ps(device.Get(), "float4 main() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }");
        constexpr unsigned mw = 64, mh = 32;
        ComPtr<ID3D11Texture2D> menu = texture(device.Get(), mw, mh, DXGI_FORMAT_R8G8B8A8_UNORM);
        ComPtr<ID3D11RenderTargetView> menuRtv;
        device->CreateRenderTargetView(menu.Get(), nullptr, &menuRtv);
        const float zero[4] = {};
        context->ClearRenderTargetView(menuRtv.Get(), zero);
        panelLog.clear();
        panelTarget = menuRtv.Get();
        panelW = mw;
        panelH = mh;
        lmu::mvr_panel_draws(&panel_cb);
        const lmu::MvrPassStats s0 = lmu::mvr_pass_stats();
        const D3D11_VIEWPORT gameVp = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};  // the eye's image
        const D3D11_RECT gameSc = {0, 0, static_cast<LONG>(W), static_cast<LONG>(H)};
        context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(panelVs.Get(), nullptr, 0);
        context->PSSetShader(panelPs.Get(), nullptr, 0);
        context->RSSetViewports(1, &gameVp);
        context->RSSetScissorRects(1, &gameSc);
        context->Draw(3, 0);
        ID3D11RenderTargetView* r = nullptr;
        context->OMGetRenderTargets(1, &r, nullptr);
        D3D11_VIEWPORT vp[16];
        UINT n = 16;
        context->RSGetViewports(&n, vp);
        D3D11_RECT sc[16];
        UINT sn = 16;
        context->RSGetScissorRects(&sn, sc);
        const std::vector<uint8_t> px = read_rgba8(device.Get(), context.Get(), menu.Get(), mw, mh);
        bool filled = px.size() == static_cast<size_t>(mw) * mh * 4;
        const int want[4] = {64, 128, 191, 255};
        for (size_t i = 0; filled && i < px.size(); ++i)
            filled = std::abs(static_cast<int>(px[i]) - want[i % 4]) <= 1;
        const std::string first = panelLog;
        context->VSSetShader(otherVs.Get(), nullptr, 0);
        context->Draw(3, 0);
        const lmu::MvrPassStats s1 = lmu::mvr_pass_stats();
        lmu::mvr_panel_draws(nullptr);
        panelTarget = nullptr;
        std::snprintf(what, sizeof(what),
                      "R6b: a menu frame's VR UI panel draw: the callback before and after it ('%s', then '%s' with another "
                      "shader), drawn again over the whole %ux%u image, the game's target, viewport and scissor back",
                      first.c_str(), panelLog.c_str(), mw, mh);
        check(first == " b a" && panelLog == first && filled && bound(r) == eyeFinalRtv.Get() && n == 1 &&
                  same_vp(vp[0], 0, 0, W, H) && sn == 1 && sc[0].right == static_cast<LONG>(W) &&
                  sc[0].bottom == static_cast<LONG>(H) && s1.panelReplays == s0.panelReplays + 1,
              what);
    }

    {  // R5c: the game's CopySubresourceRegion remembered: its source found by the destination (LMU's submit names its eye
       // images this way, at an offset into the swapchain image), only this frame's or the one before's.
        ComPtr<ID3D11Texture2D> image0 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                image1 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                chain0 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                chain1 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                other = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM);
        lmu::mvr_frame_end(100040);
        context->CopySubresourceRegion(chain0.Get(), 0, 0, 0, 0, image0.Get(), 0, nullptr);
        context->CopySubresourceRegion(chain1.Get(), 0, 0, 0, 0, image1.Get(), 0, nullptr);
        const bool found = lmu::mvr_copy_source(chain0.Get()) == image0.Get() &&
                           lmu::mvr_copy_source(chain1.Get()) == image1.Get() && !lmu::mvr_copy_source(other.Get());
        lmu::mvr_frame_end(100041);
        const bool lastFrame = lmu::mvr_copy_source(chain0.Get()) == image0.Get();
        lmu::mvr_frame_end(100042);
        const bool old = lmu::mvr_copy_source(chain0.Get()) == nullptr;
        check(found && lastFrame && old,
              "R5c: the game's CopySubresourceRegion into a swapchain image names its source (this frame's or the one "
              "before's, not older)");
    }

    {  // The desktop mirror as LMU's: its swapchain's back buffer bound with a depth target outside the passes, the
       // tracker on (it notes what is drawn with depth there, for MSAA off). Nothing of it may be held: ResizeBuffers
       // fails while any reference to a back buffer is (it did, "Resize swap chain failed" at the session's start,
       // 2026-11-10 19:13). The control: a view of ours kept alive makes it fail, so the check can see a reference.
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"lmu_focus_post_test";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance,
                                  nullptr);
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        ComPtr<IDXGISwapChain> chain;
        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferDesc.Width = scd.BufferDesc.Height = 64;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.OutputWindow = hwnd;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const bool made = hwnd && SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
                          SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) &&
                          SUCCEEDED(factory->CreateSwapChain(device.Get(), &scd, &chain));
        lmu::mvr_track_passes(true);
        lmu::mvr_frame_end(100030);
        ComPtr<ID3D11Texture2D> mirrorDepth =
            texture(device.Get(), 64, 64, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
        ComPtr<ID3D11DepthStencilView> mirrorDsv;
        device->CreateDepthStencilView(mirrorDepth.Get(), nullptr, &mirrorDsv);
        const auto mirror_frame = [&](ComPtr<ID3D11RenderTargetView>* keep) {
            ComPtr<ID3D11Texture2D> back;
            ComPtr<ID3D11RenderTargetView> backRtv;
            if (!chain || FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
                FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &backRtv)))
                return false;
            context->OMSetRenderTargets(1, backRtv.GetAddressOf(), mirrorDsv.Get());
            context->VSSetShader(plainVs.Get(), nullptr, 0);
            context->PSSetShader(scenePs.Get(), nullptr, 0);
            const D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
            context->RSSetViewports(1, &vp);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            if (keep)
                *keep = backRtv;
            return true;
        };
        const bool drawn = mirror_frame(nullptr);
        context->Flush();
        const HRESULT resized = chain ? chain->ResizeBuffers(0, 96, 96, DXGI_FORMAT_UNKNOWN, 0) : E_FAIL;
        ComPtr<ID3D11RenderTargetView> kept;
        mirror_frame(&kept);
        context->Flush();
        const HRESULT held = chain ? chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0) : S_OK;
        kept.Reset();
        context->Flush();
        const HRESULT released = chain ? chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0) : E_FAIL;
        lmu::mvr_frame_end(100031);
        lmu::mvr_track_passes(false);
        std::snprintf(what, sizeof(what),
                      "the mirror's back buffer bound with depth outside the passes, the tracker on: ResizeBuffers 0x%08lx "
                      "(the control, a view kept: 0x%08lx; let go: 0x%08lx)",
                      static_cast<unsigned long>(resized), static_cast<unsigned long>(held),
                      static_cast<unsigned long>(released));
        check(made && drawn && SUCCEEDED(resized) && FAILED(held) && SUCCEEDED(released), what);
        chain.Reset();
        if (hwnd)
            DestroyWindow(hwnd);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

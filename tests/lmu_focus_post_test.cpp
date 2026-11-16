// Offline check of LMU MVR's focus post (games/lmu/focus_post.h, mvr_pass.h mvr_focus_replay) on a WARP device. An eye
// pass as LMU's runs on the hooked immediate context: the scene (with the focus view beside the eye) resolved, then
// YEBIS-like draws with stand-ins for its copy and composite pixel shaders (same inputs: the copy's texture at TEXCOORD0
// times a constant; the composite's scene copy, glare and 1x1 exposure at TEXCOORD0, 1 and 3), marked with their role as
// the probe marks the game's. The scene's focus region holds the same picture as the eye's top-left, and the focus box
// is exactly that crop, so the focus post's image must equal the eye's own composite over it: the copy's region, the
// constants as bound for the eye (the game rewrites its buffer right after), the glare resampled through the box and
// the exposure all show there. And the game's state after each step as it set it, nothing replayed outside passes 0-1.
#include "lmu_focus_post_kit.h"

// Optional: a folder with YEBIS's own copy and composite (copy-ps.dxbc, composite-ps.dxbc and their constant buffers
// copy-ps-cb0.bin, composite-ps-cb0.bin, from a capture: tools/rdc/shader_bytes.py; the game's code, kept local). Then
// the same pass runs with them, under the debug layer where it is installed (linkage between our vertex shaders and
// theirs checked).
int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    const std::string folder = argc > 1 ? argv[1] : "";
    char what[256];
    {  // The roles by checksum (the probe's marking at creation).
        unsigned char code[24] = {'D', 'X', 'B', 'C'};
        std::memcpy(code + 4, lmu::kYebisCompositePs, 16);
        const lmu::YebisRole composite = lmu::yebis_role(code, sizeof(code));
        std::memcpy(code + 4, lmu::kYebisCopyPs, 16);
        const lmu::YebisRole copy = lmu::yebis_role(code, sizeof(code));
        code[4] ^= 1;
        check(composite == lmu::YebisRole::composite && copy == lmu::YebisRole::copy &&
                  lmu::yebis_role(code, sizeof(code)) == lmu::YebisRole::none && lmu::yebis_role(code, 12) == lmu::YebisRole::none,
              "roles by DXBC checksum: YEBIS's copy and composite known, any other shader none");
    }
    {  // R6: the vertex shaders' roles (the probe's marking at creation).
        unsigned char code[24] = {'D', 'X', 'B', 'C'};
        std::memcpy(code + 4, lmu::kLensMaskVs, 16);
        const lmu::VsRole mask = lmu::vs_role(code, sizeof(code));
        std::memcpy(code + 4, lmu::kVruiPanelVs, 16);
        const lmu::VsRole panel = lmu::vs_role(code, sizeof(code));
        code[5] ^= 1;
        check(mask == lmu::VsRole::lensMask && panel == lmu::VsRole::vruiPanel &&
                  lmu::vs_role(code, sizeof(code)) == lmu::VsRole::none,
              "vertex shader roles by DXBC checksum: the lens mask's and the VR UI panel's known, any other none");
    }

#include "lmu_focus_post_rig.h"

    eye_pass(0, &view, true, standIn);
    lmu::MvrPassStats s = lmu::mvr_pass_stats();
    unsigned long long composites = 0;
    ID3D11Texture2D* focus = lmu::mvr_focus_image(0, composites);
    std::snprintf(what, sizeof(what),
                  "pass 0: one copy and one composite for its focus view (%llu, %llu; %llu failed; the downsample not "
                  "taken for the copy), eye 0's focus image made",
                  static_cast<unsigned long long>(s.focusCopies), static_cast<unsigned long long>(s.focusComposites),
                  static_cast<unsigned long long>(s.focusFailures));
    check(s.focusCopies == 1 && s.focusComposites == 1 && s.focusFailures == 0 && focus && composites == 1, what);

    if (focus) {
        D3D11_TEXTURE2D_DESC fd;
        focus->GetDesc(&fd);
        check(fd.Width == focusW && fd.Height == focusH && fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
              "the focus image: the focus view's size, RGBA16F (as the eye's composite)");
        same_as_eye("stand-ins", 100, true);
        final_as_eye("stand-ins", 100);
    }

    {  // A fade (the blit's vertex colour at half): the focus final fades with the eye.
        blitColour[0] = blitColour[1] = blitColour[2] = 0.5f;
        eye_pass(0, &view, false, standIn);
        final_as_eye("a fade to half", 50);
        blitColour[0] = blitColour[1] = blitColour[2] = 1;
    }

    {  // Overlays: in the pass after the blit (doubled by MVR, checked in the pass), and after the passes into the eye's
       // final (the VR UI): drawn with two views and put back.
        eye_pass(0, &view, false, standIn, true);
        const int callsBefore = twoViewsCalls;
        const uint64_t outsideBefore = lmu::mvr_pass_stats().overlaysOutside;
        context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
        context->VSSetShader(twinned.Get(), nullptr, 0);
        context->PSSetShader(nullptr, nullptr, 0);
        const D3D11_VIEWPORT whole = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        context->RSSetViewports(1, &whole);
        context->Draw(3, 0);
        D3D11_VIEWPORT vp[16];
        UINT n = 16;
        context->RSGetViewports(&n, vp);
        check(lmu::mvr_pass_stats().overlaysOutside == outsideBefore + 1 && twoViewsCalls == callsBefore + 2 &&
                  twoViewsOn == 0 && bound_vs(context.Get()) == twinned.Get() && n == 1 && same_vp(vp[0], 0, 0, W, H),
              "an overlay after the passes into the eye's final (the VR UI): drawn with two views, the game's state back");
        const uint64_t again = lmu::mvr_pass_stats().overlaysOutside;
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        check(lmu::mvr_pass_stats().overlaysOutside == again, "a draw into another target after the passes: as it is");
        lmu::mvr_pass_begin(0, nullptr, false);  // the next frame: no focus final yet, nothing doubled
        lmu::mvr_pass_end(0);
        unsigned x = 0, y = 0, fw = 0, fh = 0;
        check(!lmu::mvr_focus_final(0, x, y, fw, fh), "a new frame: no focus final until its pass makes one");
    }

    {  // The glare's part: without the glare (its weight 0) the images still agree, so the glare does show above.
        compositeConstants[20][1] = 0;
        eye_pass(0, &view, false, standIn);
        const std::vector<uint8_t> noGlare = read_rgba8(device.Get(), context.Get(), focus, focusW, focusH);
        compositeConstants[20][1] = 0.6f;
        eye_pass(0, &view, false, standIn);
        const std::vector<uint8_t> withGlare = read_rgba8(device.Get(), context.Get(), focus, focusW, focusH);
        int most = 0;
        for (size_t i = 0; i < noGlare.size() && noGlare.size() == withGlare.size(); ++i)
            most = (std::max)(most, std::abs(static_cast<int>(noGlare[i]) - static_cast<int>(withGlare[i])));
        std::snprintf(what, sizeof(what), "the glare shows in the focus image (up to %d/255 with it vs without)", most);
        check(most > 20, what);
    }

    {  // Not replayed: pass 2 (renders its focus view itself), a pass that drew no focus view, the replay off.
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(2, nullptr, false, standIn);
        lmu::MvrPassView noBox = view;
        noBox.box.valid = false;
        eye_pass(1, &noBox, false, standIn);
        lmu::mvr_focus_replay(false);
        eye_pass(0, &view, false, standIn);
        lmu::mvr_focus_replay(true);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies && after.focusComposites == before.focusComposites &&
                  after.focusFailures == before.focusFailures,
              "no focus post in pass 2, in a pass without the focus box, or with the replay off");
        eye_pass(1, &view, false, standIn);
        unsigned long long c1 = 0;
        check(lmu::mvr_focus_image(1, c1) && c1 == 1 && lmu::mvr_pass_stats().focusComposites == after.focusComposites + 1,
              "pass 1: eye 1's focus image of its own");
    }

    {  // A composite of a checksum not known (another tone curve): taken by its bindings, the same image.
        ComPtr<ID3D11PixelShader> unmarked = ps(device.Get(), kCompositePs);
        Kit other = standIn;
        other.composite = unmarked.Get();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, other);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites + 1 &&
                  after.focusCompositesByBindings == before.focusCompositesByBindings + 1 &&
                  after.focusNoComposite == before.focusNoComposite,
              "a composite of another checksum: taken by its bindings (the copy's output, a glare, the 1x1 exposure)");
        same_as_eye("a composite taken by its bindings", 100, true);
    }

    {  // A draw into the resolve's target that isn't the composite (here: reading the scene, not the copy's output):
       // not taken; the pass counted without a composite, the draw logged.
        Kit other = standIn;
        ComPtr<ID3D11PixelShader> unmarked = ps(device.Get(), kCompositePs);
        other.composite = unmarked.Get();
        ID3D11ShaderResourceView* wrong = sceneSrv.Get();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        // The pass as eye_pass runs it, but the last draw reads the resolved scene at t0 (into a copy of it: the game
        // can't read and write one texture at once).
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), dsv.Get());
        context->VSSetShader(twinned.Get(), nullptr, 0);
        lmu::mvr_pass_begin(0, &view, false);
        context->ResolveSubresource(scene.Get(), 0, msaa.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
        context->VSSetShader(yebisVs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(1, 1, rectCb.GetAddressOf());
        write(context.Get(), copyCb.Get(), copyConstants, sizeof(copyConstants));
        context->PSSetShader(copyPs.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, copyCb.GetAddressOf());
        context->PSSetSamplers(0, 1, point.GetAddressOf());
        context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, &wrong);
        context->RSSetViewports(1, &eyeVp);
        context->DrawIndexed(3, 0, 0);  // the copy
        ID3D11ShaderResourceView* inputs[3] = {copySrv.Get(), glareSrv.Get(), exposureSrv.Get()};
        inputs[0] = glareSrv.Get();      // not the copy's output
        context->PSSetShader(unmarked.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 3, inputs);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->DrawIndexed(3, 0, 0);
        lmu::mvr_pass_end(0);
        context->PSSetShaderResources(0, 3, std::vector<ID3D11ShaderResourceView*>(3, nullptr).data());
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites &&
                  after.focusNoComposite == before.focusNoComposite + 1 &&
                  after.focusUnknownComposites == before.focusUnknownComposites + 1,
              "a draw into the resolve's target that doesn't read the copy's output: not the composite, logged");
    }

    {  // The overlay trace's first frame (600 frame ends after the install): a pass and a draw outside it, logged
       // (SIM_QVFR_LOG), the focus post as before.
        for (unsigned long long f = 1; f < 600; ++f)
            lmu::mvr_frame_end(f);
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, standIn);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        lmu::mvr_frame_end(600);
        check(lmu::mvr_pass_stats().focusComposites == before.focusComposites + 1,
              "a traced frame: the focus post as before (the trace's lines in SIM_QVFR_LOG)");
    }

    if (!folder.empty()) {  // YEBIS's own copy and composite with a capture's constants
        const std::vector<uint8_t> copyCode = file_bytes(folder + "\\copy-ps.dxbc"),
                                   compositeCode = file_bytes(folder + "\\composite-ps.dxbc"),
                                   copyData = file_bytes(folder + "\\copy-ps-cb0.bin"),
                                   compositeData = file_bytes(folder + "\\composite-ps-cb0.bin");
        const lmu::YebisRole copyRole = lmu::yebis_role(copyCode.data(), copyCode.size()),
                             compositeRole = lmu::yebis_role(compositeCode.data(), compositeCode.size());
        check(copyRole == lmu::YebisRole::copy && compositeRole == lmu::YebisRole::composite,
              "YEBIS's own copy and composite (a capture's bytecode): their roles by checksum, as the probe marks them");
        ComPtr<ID3D11PixelShader> realCopy, realComposite;
        if (!copyCode.empty())
            device->CreatePixelShader(copyCode.data(), copyCode.size(), nullptr, &realCopy);
        if (!compositeCode.empty())
            device->CreatePixelShader(compositeCode.data(), compositeCode.size(), nullptr, &realComposite);
        for (auto [shader, role] : {std::pair{realCopy.Get(), copyRole}, std::pair{realComposite.Get(), compositeRole}})
            if (shader) {
                const UINT r = static_cast<UINT>(role);
                shader->SetPrivateData(lmu::kYebisRoleGuid, sizeof(r), &r);
            }
        // YEBIS's $Globals: one 64 KB buffer, bound whole (tools/rdc/shader_bytes.py: offset 0, size 65536).
        constexpr UINT kGlobals = 65536;
        ComPtr<ID3D11Buffer> realCopyCb = constants(device.Get(), kGlobals), realCompositeCb = constants(device.Get(), kGlobals);
        const std::vector<uint8_t> zeros(copyData.size(), 0);
        check(realCopy && realComposite && !copyData.empty() && copyData.size() <= kGlobals && !compositeData.empty() &&
                  compositeData.size() <= kGlobals,
              "YEBIS's own shaders created, their constants read");
        if (realCopy && realComposite && !copyData.empty() && !compositeData.empty()) {
            // The game's own final blit too, when the folder has it (blit-vs.dxbc, blit-ps.dxbc): our blit's pixel
            // shader must link with its vertex shader.
            const std::vector<uint8_t> blitVsCode2 = file_bytes(folder + "\\blit-vs.dxbc"),
                                       blitPsCode = file_bytes(folder + "\\blit-ps.dxbc");
            ComPtr<ID3D11VertexShader> realBlitVs;
            ComPtr<ID3D11PixelShader> realBlitPs;
            if (!blitVsCode2.empty())
                device->CreateVertexShader(blitVsCode2.data(), blitVsCode2.size(), nullptr, &realBlitVs);
            if (!blitPsCode.empty() && lmu::yebis_role(blitPsCode.data(), blitPsCode.size()) == lmu::YebisRole::blit) {
                device->CreatePixelShader(blitPsCode.data(), blitPsCode.size(), nullptr, &realBlitPs);
                const UINT r = static_cast<UINT>(lmu::YebisRole::blit);
                if (realBlitPs)
                    realBlitPs->SetPrivateData(lmu::kYebisRoleGuid, sizeof(r), &r);
            }
            std::printf("     the game's final blit: %s\n", realBlitVs && realBlitPs ? "its own shaders (role by checksum)"
                                                                                     : "not in the folder: the stand-in");
            const Kit real = {realCopy.Get(),  realComposite.Get(), realCopyCb.Get(),     realCompositeCb.Get(),
                              copyData.data(), copyData.size(),     zeros.data(),         zeros.size(),
                              compositeData.data(), compositeData.size(),
                              realBlitVs && realBlitPs ? realBlitVs.Get() : blitVs.Get(),
                              realBlitVs && realBlitPs ? realBlitPs.Get() : blitPs.Get()};
            if (infoQueue)
                infoQueue->ClearStoredMessages();
            const lmu::MvrPassStats before = lmu::mvr_pass_stats();
            eye_pass(0, &view, true, real);
            const lmu::MvrPassStats after = lmu::mvr_pass_stats();
            check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites + 1 &&
                      after.focusFailures == before.focusFailures,
                  "YEBIS's own shaders: its copy and composite replayed once each");
            same_as_eye("YEBIS's own shaders", 20, true);
            final_as_eye("YEBIS's own shaders and the game's blit", 20);
            if (infoQueue) {
                int errors = 0;
                const UINT64 n = infoQueue->GetNumStoredMessages();
                for (UINT64 i = 0; i < n; ++i) {
                    SIZE_T length = 0;
                    infoQueue->GetMessage(i, nullptr, &length);
                    std::vector<uint64_t> storage(length / sizeof(uint64_t) + 1);
                    auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                    if (SUCCEEDED(infoQueue->GetMessage(i, m, &length)) && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                        std::printf("     debug layer: %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
                        errors += m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR ? 1 : 0;
                    }
                }
                std::snprintf(what, sizeof(what),
                              "the debug layer: no errors in that pass, %llu messages (our vertex shaders link with "
                              "YEBIS's pixel shaders)",
                              static_cast<unsigned long long>(n));
                check(errors == 0, what);
            }
        }
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

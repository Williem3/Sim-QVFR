// The rig of LMU MVR's focus post tests, included in each test's main body (after `const std::string folder` and
// `char what[256]`): a WARP device (under the debug layer with a folder, where installed), mvr_pass installed on its
// immediate context with the fake two-view switch, the focus replay and the stand-ins' roles; the targets, the
// stand-in shaders, the glare drawn, the focus box (`view`), the stand-ins' Kit (`standIn`), and the eye pass
// (`eye_pass`) with its comparisons (`same_as_eye`, `final_as_eye`). Not a header of its own: no include guard.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const bool debug = !folder.empty() &&
                       SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                                   nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    if (!debug && FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &device, nullptr, &context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    ComPtr<ID3D11InfoQueue> infoQueue;
    if (debug)
        device.As(&infoQueue);
    if (!folder.empty())
        std::printf("     the debug layer: %s\n", infoQueue ? "on" : "not installed (no linkage check)");
    lmu::mvr_set_two_views(&fake_two_views);
    lmu::mvr_focus_replay(true);  // before the install: DrawIndexed hooked too
    lmu::mvr_roles_known_for_tests(1, 1);  // the stand-ins carry their roles, as the probe marks the game's
    check(lmu::mvr_pass_install(device.Get()) && lmu::mvr_pass_stats().contextEntries == 18,
          "the immediate context hooked, the four draws, CopySubresourceRegion and VSSetConstantBuffers among its entries "
          "(the focus post on)");

    // The targets: the eye 100x80 at the top-left of 256x128, its focus view 60x50 beside it at (100, 0).
    constexpr unsigned W = 256, H = 128, eyeW = 100, eyeH = 80, focusW = 60, focusH = 50, glareW = 102, glareH = 51;
    ComPtr<ID3D11Texture2D> msaa = texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, D3D11_BIND_RENDER_TARGET),
                            depth = texture(device.Get(), W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, D3D11_BIND_DEPTH_STENCIL),
                            scene = texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT),
                            sceneCopy = texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT),
                            half = texture(device.Get(), W / 2, H / 2, DXGI_FORMAT_R16G16B16A16_FLOAT),
                            glare = texture(device.Get(), glareW, glareH, DXGI_FORMAT_R11G11B10_FLOAT);
    const uint32_t grey = 0xff808080;
    ComPtr<ID3D11Texture2D> exposure =
        texture(device.Get(), 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, &grey, 4);
    ComPtr<ID3D11RenderTargetView> msaaRtv, sceneRtv, copyRtv, halfRtv, glareRtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> sceneSrv, copySrv, glareSrv, exposureSrv;
    device->CreateRenderTargetView(msaa.Get(), nullptr, &msaaRtv);
    device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    device->CreateRenderTargetView(scene.Get(), nullptr, &sceneRtv);
    device->CreateRenderTargetView(sceneCopy.Get(), nullptr, &copyRtv);
    device->CreateRenderTargetView(half.Get(), nullptr, &halfRtv);
    device->CreateRenderTargetView(glare.Get(), nullptr, &glareRtv);
    device->CreateShaderResourceView(scene.Get(), nullptr, &sceneSrv);
    device->CreateShaderResourceView(sceneCopy.Get(), nullptr, &copySrv);
    device->CreateShaderResourceView(glare.Get(), nullptr, &glareSrv);
    device->CreateShaderResourceView(exposure.Get(), nullptr, &exposureSrv);
    // The eye's final (RGBA8, as LMU's): the game's blit puts the eye there, the focus post its focus view beside it.
    ComPtr<ID3D11Texture2D> eyeFinal = texture(device.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D11RenderTargetView> eyeFinalRtv;
    device->CreateRenderTargetView(eyeFinal.Get(), nullptr, &eyeFinalRtv);

    ComPtr<ID3D11VertexShader> plainVs = vs(device.Get(), kPlainVs), yebisVs = vs(device.Get(), kYebisVs),
                               twinned = vs(device.Get(), kPlainVs), twin = vs(device.Get(), kPlainVs);
    twinned->SetPrivateDataInterface(simqvfr::mvs::kMvrTwinGuid, twin.Get());
    ComPtr<ID3D11PixelShader> scenePs = ps(device.Get(), kScenePs), glarePs = ps(device.Get(), kGlarePs),
                              copyPs = ps(device.Get(), kCopyPs, lmu::YebisRole::copy),
                              compositePs = ps(device.Get(), kCompositePs, lmu::YebisRole::composite);
    ComPtr<ID3DBlob> blitVsCode = compile(kBlitVs, "vs_5_0");
    ComPtr<ID3D11VertexShader> blitVs;
    ComPtr<ID3D11InputLayout> blitLayout;
    if (blitVsCode) {
        device->CreateVertexShader(blitVsCode->GetBufferPointer(), blitVsCode->GetBufferSize(), nullptr, &blitVs);
        device->CreateInputLayout(kBlitLayout, static_cast<UINT>(std::size(kBlitLayout)), blitVsCode->GetBufferPointer(),
                                  blitVsCode->GetBufferSize(), &blitLayout);
    }
    ComPtr<ID3D11PixelShader> blitPs = ps(device.Get(), kBlitPs, lmu::YebisRole::blit);
    // Its vertices: 6 others first (the game draws from a start vertex), then the eye's quad.
    const D3D11_BUFFER_DESC vbd = {12 * sizeof(BlitVertex), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
                                   D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> blitVb;
    device->CreateBuffer(&vbd, nullptr, &blitVb);
    float blitColour[4] = {1, 1, 1, 1};  // the game's fades
    check(plainVs && yebisVs && twinned && scenePs && glarePs && copyPs && compositePs && blitVs && blitLayout && blitPs &&
              blitVb,
          "the stand-in shaders compiled");
    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ComPtr<ID3D11SamplerState> point, linear;
    device->CreateSamplerState(&sd, &point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device->CreateSamplerState(&sd, &linear);
    const uint16_t indices[3] = {0, 1, 2};
    const D3D11_BUFFER_DESC ibd = {sizeof(indices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA ibi = {indices, 0, 0};
    ComPtr<ID3D11Buffer> ib, copyCb = constants(device.Get(), 15 * 16), compositeCb = constants(device.Get(), 21 * 16),
                             rectCb = constants(device.Get(), 16), glareCb = constants(device.Get(), 16);
    device->CreateBuffer(&ibd, &ibi, &ib);

    // The glare: its UV across the frame, at 0.4 of the scene's size (as LMU's: 646x506 for 1614x1266).
    {
        const float size[4] = {static_cast<float>(glareW), static_cast<float>(glareH), 0, 0};
        write(context.Get(), glareCb.Get(), size, sizeof(size));
        const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(glareW), static_cast<float>(glareH), 0, 1};
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(glarePs.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, glareCb.GetAddressOf());
        context->RSSetViewports(1, &vp);
        context->OMSetRenderTargets(1, glareRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
    }

    // The focus box: exactly the eye's top-left 60x50 (the eye's tangents -1..1 across 100x80 pixels).
    lmu::MvrPassView view;
    view.eye = 0;
    view.eyeW = eyeW;
    view.eyeH = eyeH;
    view.focusW = focusW;
    view.focusH = focusH;
    view.box.valid = true;
    const float eyeTan[4] = {-1, 1, -1, 1};  // left, right, down, up
    const float focusTan[4] = {-1, -1 + 2.0f * focusW / eyeW, 1 - 2.0f * focusH / eyeH, 1};
    std::memcpy(view.box.eyeTan, eyeTan, sizeof(eyeTan));
    std::memcpy(view.box.focusTan, focusTan, sizeof(focusTan));

    const float copyConstants[15][4] = {{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {1.5f, 1.5f, 1.5f, 1}};
    const float unitConstants[15][4] = {{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {1, 1, 1, 1}};
    const float downConstants[15][4] ={{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {0.1f, 0.1f, 0.1f, 1}};
    float compositeConstants[21][4] = {};
    compositeConstants[20][0] = 1.7f;  // exposure gain
    compositeConstants[20][1] = 0.6f;  // glare
    const float eyeRect[4] = {0, 0, static_cast<float>(eyeW) / W, static_cast<float>(eyeH) / H};
    const D3D11_VIEWPORT eyeVp = {0, 0, static_cast<float>(eyeW), static_cast<float>(eyeH), 0, 1};

    // One eye pass as LMU's: the scene (the focus view drawn beside the eye: a twin bound on the depth target), the
    // resolve, then the post-processing's copy, a downsample, the composite.
    const Kit standIn = {copyPs.Get(),    compositePs.Get(),      copyCb.Get(),         compositeCb.Get(),
                         copyConstants,   sizeof(copyConstants),  downConstants,        sizeof(downConstants),
                         compositeConstants, sizeof(compositeConstants), blitVs.Get(), blitPs.Get()};
    // MSAA off (`single`): the scene drawn into its single-sample target with a single-sample depth target, no resolve;
    // the post-processing reads that target directly (docs/recon-lmu.md "MSAA off").
    ComPtr<ID3D11Texture2D> depth1 = texture(device.Get(), W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
    ComPtr<ID3D11DepthStencilView> dsv1;
    device->CreateDepthStencilView(depth1.Get(), nullptr, &dsv1);
    // The stage's single-sample depth for YEBIS (as LMU's `$Z Buffer Single Sample`, R32) and its pixel shader: the copy's
    // code, not marked (not YEBIS's), its gain in constants of its own.
    ComPtr<ID3D11Texture2D> zSingle = texture(device.Get(), W, H, DXGI_FORMAT_R32_FLOAT);
    ComPtr<ID3D11RenderTargetView> zSingleRtv;
    device->CreateRenderTargetView(zSingle.Get(), nullptr, &zSingleRtv);
    ComPtr<ID3D11Texture2D> depthZ = texture(device.Get(), W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
    ComPtr<ID3D11DepthStencilView> dsvZ;
    device->CreateDepthStencilView(depthZ.Get(), nullptr, &dsvZ);
    ComPtr<ID3D11PixelShader> stagePs = ps(device.Get(), kCopyPs);
    ComPtr<ID3D11Buffer> stageCb = constants(device.Get(), 15 * 16);
    const float wholeRect[4] = {0, 0, 1, 1};
    const auto eye_pass = [&](int pass, const lmu::MvrPassView* v, bool checkState, const Kit& k,
                              bool overlayCheck = false, bool driven = false, bool single = false) {
        if (!driven)  // driven: the tracker begins it at the scene's bind below
            lmu::mvr_pass_begin(pass, v, false);  // as the game's scene render: the pass begins, then its scene's bind
        ID3D11RenderTargetView* const sceneTarget = single ? sceneRtv.Get() : msaaRtv.Get();
        context->OMSetRenderTargets(1, &sceneTarget, single ? dsv1.Get() : dsv.Get());
        context->VSSetShader(twinned.Get(), nullptr, 0);
        const D3D11_VIEWPORT whole = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->OMSetRenderTargets(1, &sceneTarget, nullptr);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->RSSetViewports(1, &whole);
        context->Draw(3, 0);
        if (!single)
            context->ResolveSubresource(scene.Get(), 0, msaa.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (!single && k.resolveTwice)
            context->ResolveSubresource(scene.Get(), 0, msaa.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);

        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->IASetInputLayout(nullptr);
        write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
        context->VSSetShader(yebisVs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(1, 1, rectCb.GetAddressOf());
        if (k.stage) {
            // A stage between the resolve and YEBIS, as the pass trace showed it: the single-sample depth drawn, the
            // scene's target bound again with its depth, then the stage's image of the resolved scene (x1.5) into the
            // scene copy, which YEBIS's chain starts from.
            context->VSSetShader(plainVs.Get(), nullptr, 0);
            context->PSSetShader(scenePs.Get(), nullptr, 0);
            context->OMSetRenderTargets(1, zSingleRtv.GetAddressOf(), dsvZ.Get());
            context->RSSetViewports(1, &whole);
            context->Draw(3, 0);
            context->OMSetRenderTargets(1, &sceneTarget, single ? dsv1.Get() : dsv.Get());
            context->OMSetRenderTargets(1, &sceneTarget, nullptr);
            write(context.Get(), rectCb.Get(), k.stage == 1 ? wholeRect : eyeRect, sizeof(eyeRect));
            write(context.Get(), stageCb.Get(), copyConstants, sizeof(copyConstants));
            context->VSSetShader(yebisVs.Get(), nullptr, 0);
            context->PSSetShader(stagePs.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, stageCb.GetAddressOf());
            context->PSSetSamplers(0, 1, point.GetAddressOf());
            context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            context->RSSetViewports(1, k.stage == 1 ? &whole : &eyeVp);
            context->DrawIndexed(3, 0, 0);
            write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
            context->PSSetShader(k.copy, nullptr, 0);  // YEBIS's chain from here: its downsample first
            context->PSSetConstantBuffers(0, 1, &k.copyCb);
        }
        // The copy of the resolved scene (none past a stage: its image is the scene copy).
        if (!k.stage) {
        write(context.Get(), k.copyCb, k.copyData, k.copyBytes);
        context->PSSetShader(k.copy, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &k.copyCb);
        context->PSSetSamplers(0, 1, point.GetAddressOf());
        context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
        context->RSSetViewports(1, &eyeVp);
        context->DrawIndexed(3, 0, 0);
        if (checkState) {
            ID3D11PixelShader* p = nullptr;
            ID3D11VertexShader* q = nullptr;
            ID3D11RenderTargetView* r = nullptr;
            ID3D11ShaderResourceView* t = nullptr;
            ID3D11Buffer* b = nullptr;
            ID3D11Buffer* b12 = nullptr;
            context->PSGetShader(&p, nullptr, nullptr);
            context->VSGetShader(&q, nullptr, nullptr);
            context->OMGetRenderTargets(1, &r, nullptr);
            context->PSGetShaderResources(0, 1, &t);
            context->PSGetConstantBuffers(0, 1, &b);
            context->VSGetConstantBuffers(12, 1, &b12);
            D3D11_VIEWPORT vp[16];
            UINT n = 16;
            context->RSGetViewports(&n, vp);
            check(bound(p) == k.copy && bound(q) == yebisVs.Get() && bound(r) == copyRtv.Get() &&
                      bound(t) == sceneSrv.Get() && bound(b) == k.copyCb && !bound(b12) && n == 1 &&
                      vp[0].Width == eyeW && vp[0].Height == eyeH,
                  "after the copy: the game's shaders, target, texture, constants and viewport as it set them");
        }
        }
        // A downsample (the copy's shader on another texture; the game's constants rewritten): not the scene's copy.
        write(context.Get(), k.copyCb, k.downData, k.downBytes);
        const D3D11_VIEWPORT halfVp = {0, 0, eyeW / 2.0f, eyeH / 2.0f, 0, 1};
        context->OMSetRenderTargets(1, halfRtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, copySrv.GetAddressOf());
        context->RSSetViewports(1, &halfVp);
        context->DrawIndexed(3, 0, 0);
        // The composite: the scene copy, the glare, the exposure; into the resolved scene (as LMU's).
        write(context.Get(), k.compositeCb, k.compositeData, k.compositeBytes);
        context->PSSetShader(k.composite, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &k.compositeCb);
        ID3D11SamplerState* samplers[3] = {point.Get(), linear.Get(), point.Get()};
        context->PSSetSamplers(0, 3, samplers);
        ID3D11ShaderResourceView* inputs[3] = {copySrv.Get(), glareSrv.Get(), exposureSrv.Get()};
        context->PSSetShaderResources(0, 3, inputs);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->RSSetViewports(1, &eyeVp);
        context->DrawIndexed(3, 0, 0);
        if (checkState) {
            ID3D11PixelShader* p = nullptr;
            ID3D11RenderTargetView* r = nullptr;
            ID3D11ShaderResourceView* t[3] = {};
            ID3D11SamplerState* s[3] = {};
            ID3D11Buffer* b = nullptr;
            context->PSGetShader(&p, nullptr, nullptr);
            context->OMGetRenderTargets(1, &r, nullptr);
            context->PSGetShaderResources(0, 3, t);
            context->PSGetSamplers(0, 3, s);
            context->PSGetConstantBuffers(0, 1, &b);
            bool same = bound(p) == k.composite && bound(r) == sceneRtv.Get() && bound(b) == k.compositeCb;
            for (int i = 0; i < 3; ++i)
                same = (bound(t[i]) == inputs[i]) && (bound(s[i]) == samplers[i]) && same;
            check(same, "after the composite: the game's shader, target, textures, samplers and constants as it set them");
        }
        // The game's final blit: the resolved scene's eye part into the eye's final, the quad over the eye from the
        // whole target's viewport, times the vertex colour.
        // FSR on: no final blit; the upscale's stand-in after the eye's image is bound (the game's compute pass writes
        // it before the bind): the whole scene target into the eye's image, by a shader that isn't YEBIS's.
        if (k.fsr) {
            ID3D11ShaderResourceView* none[3] = {};
            context->PSSetShaderResources(0, 3, none);
            context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
            write(context.Get(), rectCb.Get(), wholeRect, sizeof(wholeRect));
            write(context.Get(), stageCb.Get(), unitConstants, sizeof(unitConstants));
            context->VSSetShader(yebisVs.Get(), nullptr, 0);
            context->PSSetShader(stagePs.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, stageCb.GetAddressOf());
            context->PSSetSamplers(0, 1, point.GetAddressOf());
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            context->RSSetViewports(1, &whole);
            context->DrawIndexed(3, 0, 0);
            write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
        } else
        {
            BlitVertex verts[12] = {};
            const float x1 = -1 + 2.0f * eyeW / W, y1 = 1 - 2.0f * eyeH / H, u1 = static_cast<float>(eyeW) / W,
                        v1 = static_cast<float>(eyeH) / H;
            const float corners[6][4] = {{-1, 1, 0, 0}, {x1, 1, u1, 0}, {-1, y1, 0, v1},
                                         {x1, 1, u1, 0}, {x1, y1, u1, v1}, {-1, y1, 0, v1}};
            for (int i = 0; i < 6; ++i) {
                BlitVertex& q = verts[6 + i];
                q.pos[0] = corners[i][0];
                q.pos[1] = corners[i][1];
                q.pos[3] = 1;
                std::memcpy(q.col, blitColour, sizeof(blitColour));
                q.t[0][0] = corners[i][2];
                q.t[0][1] = corners[i][3];
            }
            write(context.Get(), blitVb.Get(), verts, sizeof(verts));
            const UINT stride = sizeof(BlitVertex), offset = 0;
            context->IASetInputLayout(blitLayout.Get());
            context->IASetVertexBuffers(0, 1, blitVb.GetAddressOf(), &stride, &offset);
            context->VSSetShader(k.blitVs, nullptr, 0);
            context->PSSetShader(k.blitPs, nullptr, 0);
            context->PSSetSamplers(0, 1, point.GetAddressOf());
            ID3D11ShaderResourceView* none[3] = {};
            context->PSSetShaderResources(0, 3, none);
            context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            context->RSSetViewports(1, &whole);  // the whole target, as the scene's
            context->Draw(6, 6);
            if (checkState) {
                ID3D11PixelShader* p = nullptr;
                ID3D11ShaderResourceView* s0 = nullptr;
                D3D11_VIEWPORT vp[16];
                UINT n = 16;
                context->PSGetShader(&p, nullptr, nullptr);
                context->PSGetShaderResources(0, 1, &s0);
                context->RSGetViewports(&n, vp);
                check(bound(p) == k.blitPs && bound(s0) == sceneSrv.Get() && n >= 1 && vp[0].Width == W && vp[0].Height == H,
                      "after the final blit: the game's pixel shader, texture and viewport as it set them");
            }
        }
        if (overlayCheck && pass < 2) {  // an overlay after the blit (the driver tags): doubled by MVR, its viewport cut
            context->VSSetShader(twinned.Get(), nullptr, 0);
            context->PSSetShader(nullptr, nullptr, 0);  // nothing written: only the state checked
            context->RSSetViewports(1, &whole);
            D3D11_VIEWPORT vp[16];
            UINT n = 16;
            context->RSGetViewports(&n, vp);
            const uint64_t before = lmu::mvr_pass_stats().overlayDraws;
            context->Draw(3, 0);
            std::snprintf(what, sizeof(what),
                          "an overlay into the eye's final after the blit: the twin, two views, its whole-target viewport "
                          "cut to the eye (%u: %.0fx%.0f, then %.0f,%.0f %.0fx%.0f), counted",
                          n, n ? vp[0].Width : 0.f, n ? vp[0].Height : 0.f, n > 1 ? vp[1].TopLeftX : 0.f,
                          n > 1 ? vp[1].TopLeftY : 0.f, n > 1 ? vp[1].Width : 0.f, n > 1 ? vp[1].Height : 0.f);
            check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1 && n == 2 && same_vp(vp[0], 0, 0, eyeW, eyeH) &&
                      same_vp(vp[1], eyeW, 0, focusW, focusH) && lmu::mvr_pass_stats().overlayDraws == before + 1,
                  what);
        }
        if (!driven)  // driven: the tracker ends it (another target after the blit, the next scene, the frame end)
            lmu::mvr_pass_end(pass);
        context->PSSetShaderResources(0, 3, std::vector<ID3D11ShaderResourceView*>(3, nullptr).data());
        context->IASetInputLayout(nullptr);
    };

    // Eye 0's focus image against the eye's own composite over the same crop: colour within 3/255 (mean < 0.5), the
    // eye's values spanning at least `range` (a picture, not a flat colour); alpha within 1 if `alpha`.
    const auto same_as_eye = [&](const char* label, unsigned range, bool alpha) {
        unsigned long long n = 0;
        ID3D11Texture2D* image = lmu::mvr_focus_image(0, n);
        if (!image) {
            check(false, "a focus image to compare");
            return;
        }
        const std::vector<uint8_t> mine = read_rgba8(device.Get(), context.Get(), image, focusW, focusH);
        const std::vector<uint8_t> eye = read_rgba8(device.Get(), context.Get(), scene.Get(), focusW, focusH);
        int worst = 0, alphaWorst = 0;
        double sum = 0;
        unsigned lo = 255, hi = 0;
        for (size_t i = 0; i < mine.size() && mine.size() == eye.size(); ++i) {
            const int d = std::abs(static_cast<int>(mine[i]) - static_cast<int>(eye[i]));
            if (i % 4 == 3) {  // alpha: the composite's luminance (YEBIS's: for later passes)
                alphaWorst = (std::max)(alphaWorst, d);
                continue;
            }
            if (d > 3 && d > worst)
                std::printf("     differs at %zu,%zu channel %zu: focus post %u, eye %u\n", (i / 4) % focusW,
                            (i / 4) / focusW, i % 4, mine[i], eye[i]);
            worst = d > worst ? d : worst;
            sum += d;
            lo = eye[i] < lo ? eye[i] : lo;
            hi = eye[i] > hi ? eye[i] : hi;
        }
        const double mean = mine.empty() ? 1e9 : sum / static_cast<double>(mine.size() * 3 / 4);
        std::snprintf(what, sizeof(what),
                      "%s: the focus image equals the eye's own composite over the same crop: largest difference %d/255, "
                      "mean %.3f (the eye's values %u..%u); alpha largest difference %d",
                      label, worst, mean, lo, hi, alphaWorst);
        check(!mine.empty() && mine.size() == eye.size() && worst <= 3 && mean < 0.5 && hi - lo >= range &&
                  (!alpha || alphaWorst <= 1),
              what);
    };

    // Eye 0's focus final (the focus view beside the eye in the eye's final) against the eye's own final over the same
    // crop: within 1/255 (the eye's values spanning at least `range`).
    const auto final_as_eye = [&](const char* label, unsigned range) {
        unsigned x = 0, y = 0, fw = 0, fh = 0;
        if (!lmu::mvr_focus_final(0, x, y, fw, fh) || x != eyeW || y != 0 || fw != focusW || fh != focusH) {
            check(false, "a focus final beside the eye (100,0 60x50)");
            return;
        }
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
        std::snprintf(what, sizeof(what),
                      "%s: the focus final beside the eye equals the eye's own final over the same crop: largest "
                      "difference %d/255 (the eye's values %u..%u)",
                      label, worst, lo, hi);
        check(worst <= 1 && hi - lo >= range, what);
    };
    (void)same_as_eye;  // not every test compares the images
    (void)final_as_eye;

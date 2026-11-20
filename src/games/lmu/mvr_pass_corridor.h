// Part of mvr_pass.cpp's translation unit (included once, in order): the focus corridor's measurement (mvr_corridor;
// [experiment] lmu_corridor_ab). The focus views keep only what lies in a corridor ahead in the world (lmu_corridor_*:
// its half width, its height above the road, where it starts and ends): every other texel of the focus viewport gets
// the nearest depth before the scene, as the focus mask's do, so the scene's draws skip it. Which texels: the eye's own
// depth over the focus box from the last frame (its first transparent draw, after the opaque scene: copied into our
// texture), each focus texel's direction (the box's tangents) out to that distance, measured against the road plane
// (the pass camera's world up and the eye's height above the road) and the head's level heading. The eye is never
// masked, so a texel the mask skipped comes back as soon as the eye sees something in the corridor there.
// A MEASUREMENT: the skipped texels show whatever the focus view held (nothing is composed from the periphery yet).
// Included only by mvr_pass.cpp, inside its anonymous namespace (lmu::<anonymous>), after mvr_pass_focus_mask.h and
// before mvr_pass_glass.h; never include it anywhere else. No includes of its own: mvr_pass.cpp's.
#pragma once

// The shaders: corridor_shader.h (kCorridorShaders); their constant buffer's floats.
constexpr int kCorridorConstants = 32;

bool corridor_resources(ID3D11DeviceContext* context) {
    auto& c = g.corridor;
    if (c.vs)
        return true;
    if (c.failed)
        return false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    const auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& code) {
        ComPtr<ID3DBlob> errors;
        return SUCCEEDED(D3DCompile(kCorridorShaders, sizeof(kCorridorShaders) - 1, "corridor", nullptr, nullptr, entry,
                                    target, 0, 0, &code, &errors));
    };
    ComPtr<ID3DBlob> vs, ms, ss, mask, tint, classify, columns, centre, colourMs, colourSs, fillOutside, fillEdge, margin;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_DEPTH_STENCIL_DESC none{};
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;  // the tint multiplies: dest x src
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC fb{};
    fb.RenderTarget[0].BlendEnable = TRUE;
    fb.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    fb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    fb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    fb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;  // the target's alpha left as it was
    fb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    fb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    fb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC fo{};
    fo.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    const D3D11_BUFFER_DESC cb = {kCorridorConstants * sizeof(float), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const bool ok = device && compile("vs_main", "vs_5_0", vs) && compile("copy_ms", "ps_5_0", ms) &&
                    compile("copy_ss", "ps_5_0", ss) && compile("mask_main", "ps_5_0", mask) &&
                    compile("show_main", "ps_5_0", tint) && compile("columns_main", "cs_5_0", columns) &&
                    compile("classify_main", "cs_5_0", classify) && compile("margin_main", "cs_5_0", margin) &&
                    compile("copy_colour_ms", "ps_5_0", colourMs) && compile("copy_colour_ss", "ps_5_0", colourSs) &&
                    compile("fill_outside_main", "ps_5_0", fillOutside) && compile("fill_edge_main", "ps_5_0", fillEdge) &&
                    compile("centre_main", "cs_5_0", centre) &&
                    SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &c.vs)) &&
                    SUCCEEDED(device->CreatePixelShader(ms->GetBufferPointer(), ms->GetBufferSize(), nullptr, &c.copyMs)) &&
                    SUCCEEDED(device->CreatePixelShader(ss->GetBufferPointer(), ss->GetBufferSize(), nullptr, &c.copySs)) &&
                    SUCCEEDED(device->CreatePixelShader(mask->GetBufferPointer(), mask->GetBufferSize(), nullptr, &c.mask)) &&
                    SUCCEEDED(device->CreatePixelShader(tint->GetBufferPointer(), tint->GetBufferSize(), nullptr, &c.tint)) &&
                    SUCCEEDED(device->CreateBlendState(&bd, &c.blend)) &&
                    SUCCEEDED(device->CreateComputeShader(columns->GetBufferPointer(), columns->GetBufferSize(), nullptr,
                                                          &c.columns)) &&
                    SUCCEEDED(device->CreateComputeShader(classify->GetBufferPointer(), classify->GetBufferSize(),
                                                          nullptr, &c.classify)) &&
                    SUCCEEDED(device->CreateComputeShader(margin->GetBufferPointer(), margin->GetBufferSize(), nullptr,
                                                          &c.margin)) &&
                    SUCCEEDED(device->CreatePixelShader(colourMs->GetBufferPointer(), colourMs->GetBufferSize(), nullptr,
                                                        &c.copyColourMs)) &&
                    SUCCEEDED(device->CreatePixelShader(colourSs->GetBufferPointer(), colourSs->GetBufferSize(), nullptr,
                                                        &c.copyColourSs)) &&
                    SUCCEEDED(device->CreatePixelShader(fillOutside->GetBufferPointer(), fillOutside->GetBufferSize(),
                                                        nullptr, &c.fillOutside)) &&
                    SUCCEEDED(device->CreatePixelShader(fillEdge->GetBufferPointer(), fillEdge->GetBufferSize(), nullptr,
                                                        &c.fillEdge)) &&
                    SUCCEEDED(device->CreateBlendState(&fb, &c.fillBlend)) &&
                    SUCCEEDED(device->CreateBlendState(&fo, &c.fillOpaque)) &&
                    SUCCEEDED(device->CreateSamplerState(&sampler, &c.linear)) &&
                    SUCCEEDED(device->CreateComputeShader(centre->GetBufferPointer(), centre->GetBufferSize(), nullptr,
                                                          &c.centre)) &&
                    SUCCEEDED(device->CreateDepthStencilState(&ds, &c.depth)) &&
                    SUCCEEDED(device->CreateDepthStencilState(&none, &c.noDepth)) &&
                    SUCCEEDED(device->CreateRasterizerState(&rs, &c.raster)) &&
                    SUCCEEDED(device->CreateBuffer(&cb, nullptr, &c.cb)) &&
                    SUCCEEDED(device->CreateDeferredContext(0, &c.deferred));
    if (!ok) {
        c.vs.Reset();
        c.failed = true;
        simqvfr::log_line("lmu mvr: corridor: its shaders, buffer or states couldn't be made; off for the session");
    }
    return ok;
}

// A command list run on the immediate context with its state restored, NVIDIA's single-pass stereo off around it (as
// the focus mask's).
bool corridor_execute(ID3D11DeviceContext* context, ID3D11CommandList* list, const char* what) {
    const bool stereo = g.stereo && !g.instanced;
    if (stereo) {
        Timed switching(g.time.switches);
        if (g.twoViews(g.context, false) != 0) {
            stereo_off_failed(what);
            return false;
        }
    }
    const bool was = g.replaying;
    g.replaying = true;
    call_real<kHookExecute>(context, list, TRUE);
    g.replaying = was;
    if (stereo) {
        Timed switching(g.time.switches);
        if (g.twoViews(g.context, true) != 0) {
            ++g.stats.stereoErrors;
            g.stereo = false;
            update();
        }
    }
    return true;
}

// Pass p's mask constants (the shaders' cbuffer) from its copy: false without the depth terms.
bool corridor_constants(int p, float (&k)[kCorridorConstants], float (&upV)[3]) {
    auto& c = g.corridor;
    const auto& t = c.copy[p];
    double a = 0, bTerm = 0;
    if (!fixation_depth_terms(a, bTerm))
        return false;
    // The world's up in the eye's view space: the pass camera's view matrix (row vectors: its row 1 is the world's y
    // axis in view space); level if it isn't known.
    float view[16], viewProj[16];
    upV[0] = 0;
    upV[1] = 1;
    upV[2] = 0;
    if (lmu_pass_camera(p, view, viewProj)) {
        const float n = std::sqrt(view[4] * view[4] + view[5] * view[5] + view[6] * view[6]);
        if (n > 0.5f) {
            upV[0] = view[4] / n;
            upV[1] = view[5] / n;
            upV[2] = view[6] / n;
        }
    } else {
        ++g.stats.corridorLevelUp;
    }
    const auto& b = g.view.box;  // focusTan: left, right, down, up
    const float slopeCos = static_cast<float>(std::cos(std::clamp(c.roadSlopeDeg, 1.0f, 80.0f) * 3.14159265 / 180));
    const float k2[kCorridorConstants] = {c.copyBox[p][0], c.copyBox[p][1], c.copyBox[p][2], c.copyBox[p][3],
                                          b.focusTan[0], b.focusTan[1], b.focusTan[3], b.focusTan[2],
                                          static_cast<float>(a), static_cast<float>(bTerm), c.startM, c.tallM,
                                          upV[0], upV[1], upV[2], c.eyeHeightM,
                                          c.halfM, c.farM, static_cast<float>(t.w), static_cast<float>(t.h),
                                          slopeCos, 0.1f, c.centreReachM, 0,
                                          0, c.featherM, static_cast<float>(t.fw), static_cast<float>(t.fh),
                                          g.appliedVps[1].TopLeftX, g.appliedVps[1].TopLeftY, 0, 0};
    std::memcpy(k, k2, sizeof(k2));
    // The stamp the fill looks for: this pass's if its mask ran, else one no texel holds yet (the next).
    const uint32_t stamp = c.maskedThisPass[p] ? c.stamp[p] : c.stamp[p] + 1;
    std::memcpy(&k[30], &stamp, sizeof(stamp));
    return true;
}

// Every kCorridorSayEvery passes, pass p's geometry: the eye's and the focus view's viewports, the box as a rectangle of
// the eye and as tangents (with the rectangle the tangents give: the two should agree), the copy's region, and the
// world's up in the eye's view (its roll and pitch).
constexpr uint64_t kCorridorSayEvery = 1800;
void corridor_geometry_line(int p, const float (&upV)[3]) {
    auto& c = g.corridor;
    if (c.geometrySaid[p]++ % kCorridorSayEvery != 0)
        return;
    const auto& b = g.view.box;
    const D3D11_VIEWPORT& e = g.appliedVps[0];
    const D3D11_VIEWPORT& f = g.appliedVps[1];
    const double spanX = b.eyeTan[1] - b.eyeTan[0], spanY = b.eyeTan[3] - b.eyeTan[2];  // eyeTan: left right down up
    const double tx0 = spanX != 0 ? (b.focusTan[0] - b.eyeTan[0]) / spanX : 0, tx1 = spanX != 0 ? (b.focusTan[1] - b.eyeTan[0]) / spanX : 0;
    const double ty0 = spanY != 0 ? (b.eyeTan[3] - b.focusTan[3]) / spanY : 0, ty1 = spanY != 0 ? (b.eyeTan[3] - b.focusTan[2]) / spanY : 0;
    constexpr double kDeg = 57.29577951308232;
    simqvfr::logf("lmu mvr: corridor geometry, pass %d: eye viewport %.0f,%.0f %.0fx%.0f (eye %ux%u), focus viewport "
                  "%.0f,%.0f %.0fx%.0f; box rect %.4f %.4f %.4f %.4f, from its tangents %.4f %.4f %.4f %.4f; eye tangents "
                  "%.3f/%.3f/%.3f/%.3f, focus %.3f/%.3f/%.3f/%.3f (left right down up); copy %.0f,%.0f %.0fx%.0f; up in "
                  "view %.3f %.3f %.3f (roll %.1f deg, pitch %.1f deg)",
                  p, e.TopLeftX, e.TopLeftY, e.Width, e.Height, g.view.eyeW, g.view.eyeH, f.TopLeftX, f.TopLeftY, f.Width,
                  f.Height, b.rect[0], b.rect[1], b.rect[2], b.rect[3], tx0, ty0, tx1, ty1, b.eyeTan[0], b.eyeTan[1],
                  b.eyeTan[2], b.eyeTan[3], b.focusTan[0], b.focusTan[1], b.focusTan[2], b.focusTan[3], c.copyBox[p][0],
                  c.copyBox[p][1], c.copyBox[p][2], c.copyBox[p][3], upV[0], upV[1], upV[2],
                  std::atan2(upV[0], upV[1]) * kDeg, std::asin(std::clamp(upV[2], -1.0f, 1.0f)) * kDeg);
}

// A view of the game's resource for reading (its multisampled-ness out), cached by the resource; null if it can't be read.
ComPtr<ID3D11ShaderResourceView> corridor_colour_view(ID3D11Device* device, ID3D11Resource* resource) {
    auto& c = g.corridor;
    if (resource == c.colourFor)
        return c.colourSrv;
    c.colourFor = resource;
    c.colourSrv.Reset();
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        return nullptr;
    D3D11_TEXTURE2D_DESC td;
    texture->GetDesc(&td);
    c.colourMs = td.SampleDesc.Count > 1;
    DXGI_FORMAT format = td.Format;
    if (format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    else if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
        format = format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM : format;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = format;
    sd.ViewDimension = c.colourMs ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        FAILED(device->CreateShaderResourceView(resource, &sd, &c.colourSrv))) {
        c.colourSrv.Reset();
        if (!c.colourUnreadableLogged) {
            c.colourUnreadableLogged = true;
            simqvfr::logf("lmu mvr: corridor: the scene's colour can't be read (format %d x%u, bind 0x%x): no fill",
                          static_cast<int>(td.Format), td.SampleDesc.Count, td.BindFlags);
        }
    }
    return c.colourSrv;
}

// The fill (corridor_shader.h fill_colour_main, fill_depth_main), at the copy's moment (the opaque scene drawn, the
// transparents next) after this frame's road pass: the eye's colour over the box and the focus view's depth copied,
// then the eye's colour blended into the focus view outside the corridor and at its edge, and the eye's depth into the
// texels the mask skipped. Everything after it (the transparents, the post-processing, DLSS, any compositor or the
// runtime's own) sees a whole focus view.
void corridor_fill(ID3D11DeviceContext* context, int p, const float (&k)[kCorridorConstants],
                   ID3D11DepthStencilView* dsv) {
    auto& c = g.corridor;
    auto& t = c.copy[p];
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) {
        ++g.stats.corridorFillRefused;
        return;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    ComPtr<ID3D11Resource> colourResource;
    rtv->GetResource(&colourResource);
    const ComPtr<ID3D11ShaderResourceView> colourView = corridor_colour_view(device.Get(), colourResource.Get());
    if (!colourView) {
        ++g.stats.corridorFillRefused;
        return;
    }
    if (!t.colour) {
        const D3D11_TEXTURE2D_DESC cd = {t.w, t.h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(device->CreateTexture2D(&cd, nullptr, &t.colour)) ||
            FAILED(device->CreateRenderTargetView(t.colour.Get(), nullptr, &t.colourRtv)) ||
            FAILED(device->CreateShaderResourceView(t.colour.Get(), nullptr, &t.colourSrv))) {
            t.colour.Reset();
            ++g.stats.corridorFillRefused;
            return;
        }
    }
    const D3D11_VIEWPORT fvp = g.appliedVps[1];
    ID3D11DeviceContext* dc = c.deferred.Get();
    corridor_stamp_in(dc, p, 7);  // the list's start (after its entry)
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(nullptr);
    dc->VSSetShader(c.vs.Get(), nullptr, 0);
    dc->PSSetConstantBuffers(0, 1, c.cb.GetAddressOf());
    dc->RSSetState(c.raster.Get());
    dc->UpdateSubresource(c.cb.Get(), 0, nullptr, k, 0, 0);
    // 1. The eye's colour over the box (its texels: the box constant as the depth copy has it).
    const D3D11_VIEWPORT copyVp{0, 0, static_cast<float>(t.w), static_cast<float>(t.h), 0, 1};
    dc->RSSetViewports(1, &copyVp);
    dc->OMSetDepthStencilState(c.noDepth.Get(), 0);
    dc->OMSetRenderTargets(1, t.colourRtv.GetAddressOf(), nullptr);
    dc->PSSetShader(c.colourMs ? c.copyColourMs.Get() : c.copyColourSs.Get(), nullptr, 0);
    dc->PSSetShaderResources(0, 1, colourView.GetAddressOf());
    dc->Draw(3, 0);
    corridor_stamp_in(dc, p, 8);
    // 2. Outside the corridor and where the mask skipped: the eye's colour and depth, written as they are. Our colour copy
    // unbound as a target first: bound for reading while still a target, d3d11 drops the view (the fill read black).
    ID3D11ShaderResourceView* unbind[1] = {};
    dc->PSSetShaderResources(0, 1, unbind);
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* srvs[7] = {t.srv.Get(), t.groundSrv.Get(), t.centreSrv.Get(), nullptr, t.stampsSrv.Get(),
                                         t.colourSrv.Get(), t.marginSrv.Get()};
    dc->PSSetShaderResources(0, 7, srvs);
    dc->PSSetSamplers(0, 1, c.linear.GetAddressOf());
    dc->RSSetViewports(1, &fvp);
    dc->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv);
    dc->OMSetDepthStencilState(c.depth.Get(), 0);  // always, written
    dc->OMSetBlendState(c.fillOpaque.Get(), nullptr, 0xffffffff);
    dc->PSSetShader(c.fillOutside.Get(), nullptr, 0);
    dc->Draw(3, 0);
    corridor_stamp_in(dc, p, 9);
    // 3. Within the feather of the corridor's edge: the eye's colour blended over the focus view.
    dc->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    dc->OMSetDepthStencilState(c.noDepth.Get(), 0);
    dc->OMSetBlendState(c.fillBlend.Get(), nullptr, 0xffffffff);
    dc->PSSetShader(c.fillEdge.Get(), nullptr, 0);
    dc->Draw(3, 0);
    corridor_stamp_in(dc, p, 10);
    ComPtr<ID3D11CommandList> list;
    if (FAILED(dc->FinishCommandList(FALSE, &list))) {
        ++g.stats.corridorFillRefused;
        return;
    }
    corridor_stamp(p, 5);
    if (!corridor_execute(context, list.Get(), "before the corridor's fill")) {
        ++g.stats.corridorFillRefused;
        return;
    }
    corridor_stamp(p, 6);
    ++g.stats.corridorFills;
    c.filledThisPass[p] = true;
    std::memcpy(&c.fillStamp[p], &k[30], sizeof(c.fillStamp[p]));
}

// What DLSS keeps of this pass's focus view (DlssPass::focusKeep): with the fill run this pass, the margin map (above 0
// inside the corridor, blended over the feather) and the mask's stamps; after DLSS the rest takes the eye's finished
// image, as the fill took its raw one. False: nothing to say (no fill this pass).
bool corridor_keep(int p, simqvfr::DlssPass& dp) {
    auto& c = g.corridor;
    if (p < 0 || p > 1 || !c.filledThisPass[p])
        return false;
    auto& t = c.copy[p];
    if (!t.marginSrv || !t.stampsSrv || !t.w || !t.h)
        return false;
    dp.focusKeep = t.marginSrv.Get();
    dp.focusKeepWidth = t.w;
    dp.focusKeepHeight = t.h;
    dp.focusKeepFeather = c.featherM;
    dp.focusStamps = t.stampsSrv.Get();
    dp.focusStamp = c.fillStamp[p];
    return true;
}

// The eye's depth over the focus box, as the opaque scene left it (at the pass's first transparent draw writing depth:
// mvr_pass_glass.h's moment), into pass p's copy. Once per pass.
void corridor_copy(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv) {
    auto& c = g.corridor;
    const int p = g.pass;
    if (p < 0 || p > 1 || c.copiedThisPass[p] || !g.drawFocus || !g.view.box.valid || !dsv)
        return;
    c.copiedThisPass[p] = true;
    if (!corridor_resources(context))
        return;
    ComPtr<ID3D11Resource> resource;
    dsv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)))
        return;
    D3D11_TEXTURE2D_DESC td;
    texture->GetDesc(&td);
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (resource.Get() != c.depthFor) {  // a view of the game's depth target we can read
        c.depthSrv.Reset();
        c.depthFor = resource.Get();
        c.depthMs = td.SampleDesc.Count > 1;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format == DXGI_FORMAT_R32_TYPELESS        ? DXGI_FORMAT_R32_FLOAT
                    : td.Format == DXGI_FORMAT_R24G8_TYPELESS    ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                    : td.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                                                                 : DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = c.depthMs ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (sd.Format == DXGI_FORMAT_UNKNOWN || !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            FAILED(device->CreateShaderResourceView(resource.Get(), &sd, &c.depthSrv))) {
            c.depthSrv.Reset();
            if (!c.unreadableLogged) {
                c.unreadableLogged = true;
                simqvfr::logf("lmu mvr: corridor: the scene's depth can't be read (format %d x%u, bind 0x%x): no corridor",
                              static_cast<int>(td.Format), td.SampleDesc.Count, td.BindFlags);
            }
        }
    }
    if (!c.depthSrv)
        return;
    // The box in the eye's corner of the target, in texels.
    const D3D11_VIEWPORT& eye = g.appliedVps[0];
    const auto& b = g.view.box;
    const int x0 = static_cast<int>(eye.TopLeftX + b.rect[0] * eye.Width), y0 = static_cast<int>(eye.TopLeftY + b.rect[1] * eye.Height);
    const int x1 = static_cast<int>(eye.TopLeftX + b.rect[2] * eye.Width), y1 = static_cast<int>(eye.TopLeftY + b.rect[3] * eye.Height);
    const unsigned w = static_cast<unsigned>((std::max)(1, x1 - x0)), h = static_cast<unsigned>((std::max)(1, y1 - y0));
    auto& t = c.copy[p];
    if (!t.texture || t.w != w || t.h != h) {
        t = {};
        const D3D11_TEXTURE2D_DESC desc = {w, h, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &t.texture)) ||
            FAILED(device->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv)) ||
            FAILED(device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv))) {
            t = {};
            return;
        }
        // The road beneath each texel, each column's sums (w of them), and the centre (once: kept across sizes).
        // The road grid: half the copy each way (corridor_shader.h grid_size).
        const unsigned gw = (w + 1) / 2, gh = (h + 1) / 2;
        const D3D11_TEXTURE2D_DESC rd = {gw, gh, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        const D3D11_TEXTURE2D_DESC md = {w, h, 1, 1, DXGI_FORMAT_R16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        const D3D11_TEXTURE2D_DESC gd = {gw, gh, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        const D3D11_BUFFER_DESC sd = {gw * 2 * sizeof(float), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                      D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 2 * sizeof(float)};
        const D3D11_BUFFER_DESC cd = {sizeof(float), D3D11_USAGE_DEFAULT,
                                      D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                                      D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(float)};
        const float zero = 0;
        const D3D11_SUBRESOURCE_DATA zeroData = {&zero, 0, 0};
        if (FAILED(device->CreateTexture2D(&gd, nullptr, &t.ground)) ||
            FAILED(device->CreateUnorderedAccessView(t.ground.Get(), nullptr, &t.groundUav)) ||
            FAILED(device->CreateTexture2D(&rd, nullptr, &t.road)) ||
            FAILED(device->CreateUnorderedAccessView(t.road.Get(), nullptr, &t.roadUav)) ||
            FAILED(device->CreateShaderResourceView(t.road.Get(), nullptr, &t.roadSrv)) ||
            FAILED(device->CreateTexture2D(&md, nullptr, &t.marginMap)) ||
            FAILED(device->CreateUnorderedAccessView(t.marginMap.Get(), nullptr, &t.marginUav)) ||
            FAILED(device->CreateShaderResourceView(t.marginMap.Get(), nullptr, &t.marginSrv)) ||
            FAILED(device->CreateShaderResourceView(t.ground.Get(), nullptr, &t.groundSrv)) ||
            FAILED(device->CreateBuffer(&sd, nullptr, &t.sums)) ||
            FAILED(device->CreateUnorderedAccessView(t.sums.Get(), nullptr, &t.sumsUav)) ||
            FAILED(device->CreateBuffer(&cd, &zeroData, &t.centre)) ||
            FAILED(device->CreateUnorderedAccessView(t.centre.Get(), nullptr, &t.centreUav)) ||
            FAILED(device->CreateShaderResourceView(t.centre.Get(), nullptr, &t.centreSrv))) {
            t = {};
            return;
        }
        // The corridor's rows (two uints, raw) and their staging ring.
        const D3D11_BUFFER_DESC rowsDesc = {8, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                            D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC rowsView{};
        rowsView.Format = DXGI_FORMAT_R32_TYPELESS;
        rowsView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        rowsView.Buffer.NumElements = 2;
        rowsView.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        const D3D11_BUFFER_DESC readDesc = {8, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        bool rowsOk = SUCCEEDED(device->CreateBuffer(&rowsDesc, nullptr, &t.rows)) &&
                      SUCCEEDED(device->CreateUnorderedAccessView(t.rows.Get(), &rowsView, &t.rowsUav));
        for (auto& rb : t.rowsRead)
            rowsOk = rowsOk && SUCCEEDED(device->CreateBuffer(&readDesc, nullptr, &rb));
        if (!rowsOk) {  // no band (the corridor itself goes on)
            t.rows.Reset();
            t.rowsUav.Reset();
            for (auto& rb : t.rowsRead)
                rb.Reset();
        }
        t.w = w;
        t.h = h;
        t.gw = gw;
        t.gh = gh;
    }
    // Its constants: only the box's corner (the mask's are set at the mask).
    c.copyBox[p][0] = static_cast<float>(x0);
    c.copyBox[p][1] = static_cast<float>(y0);
    c.copyBox[p][2] = static_cast<float>(w);
    c.copyBox[p][3] = static_cast<float>(h);
    float constants[kCorridorConstants] = {};
    std::memcpy(constants, c.copyBox[p], sizeof(c.copyBox[p]));
    ID3D11DeviceContext* dc = c.deferred.Get();
    dc->UpdateSubresource(c.cb.Get(), 0, nullptr, constants, 0, 0);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(nullptr);
    dc->VSSetShader(c.vs.Get(), nullptr, 0);
    dc->PSSetShader(c.depthMs ? c.copyMs.Get() : c.copySs.Get(), nullptr, 0);
    dc->PSSetConstantBuffers(0, 1, c.cb.GetAddressOf());
    dc->PSSetShaderResources(0, 1, c.depthSrv.GetAddressOf());
    dc->RSSetState(c.raster.Get());
    dc->RSSetViewports(1, &vp);
    dc->OMSetDepthStencilState(c.noDepth.Get(), 0);
    dc->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);  // the game's depth unbound while we read it
    dc->Draw(3, 0);
    ComPtr<ID3D11CommandList> list;
    if (FAILED(dc->FinishCommandList(FALSE, &list)))
        return;
    corridor_stamp(p, 0);
    if (!corridor_execute(context, list.Get(), "before the corridor's copy"))
        return;
    corridor_stamp(p, 1);
    t.valid = true;
    ++g.stats.corridorCopies;
    float k[kCorridorConstants], upV[3];
    if (!corridor_constants(p, k, upV))
        return;
    corridor_geometry_line(p, upV);
    // The road beneath each texel and the road's centre, on the half-size grid (corridor_shader.h classify_main: every
    // grid texel at once; columns_main: a thread per column; centre_main).
    {
        dc->UpdateSubresource(c.cb.Get(), 0, nullptr, k, 0, 0);
        dc->CSSetShader(c.classify.Get(), nullptr, 0);
        dc->CSSetConstantBuffers(0, 1, c.cb.GetAddressOf());
        dc->CSSetShaderResources(0, 1, t.srv.GetAddressOf());
        dc->CSSetUnorderedAccessViews(3, 1, t.roadUav.GetAddressOf(), nullptr);
        dc->Dispatch((t.gw + 7) / 8, (t.gh + 7) / 8, 1);
        ID3D11UnorderedAccessView* noUav = nullptr;
        dc->CSSetUnorderedAccessViews(3, 1, &noUav, nullptr);  // the road texels read next
        dc->CSSetShader(c.columns.Get(), nullptr, 0);
        dc->CSSetShaderResources(3, 1, t.roadSrv.GetAddressOf());
        ID3D11UnorderedAccessView* uavs[3] = {t.groundUav.Get(), t.sumsUav.Get(), t.centreUav.Get()};
        dc->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        dc->Dispatch((t.gw + 63) / 64, 1, 1);
        dc->CSSetShader(c.centre.Get(), nullptr, 0);
        dc->Dispatch(1, 1, 1);
        // The margin map: the road's and the centre's results read now (their views unbound as UAVs first).
        ID3D11UnorderedAccessView* noUavs[3] = {};
        dc->CSSetUnorderedAccessViews(0, 3, noUavs, nullptr);
        ID3D11ShaderResourceView* marginIn[3] = {t.srv.Get(), t.groundSrv.Get(), t.centreSrv.Get()};
        dc->CSSetShaderResources(0, 3, marginIn);
        // With the corridor's rows (for DLSS's band) where it has them: cleared, gathered, copied out to the ring.
        const bool rows = t.rowsUav && c.bandPct > 0;
        if (rows) {
            const UINT zero[4] = {};
            dc->ClearUnorderedAccessViewUint(t.rowsUav.Get(), zero);
        }
        ID3D11UnorderedAccessView* marginUavs[2] = {t.marginUav.Get(), rows ? t.rowsUav.Get() : nullptr};
        dc->CSSetUnorderedAccessViews(4, 2, marginUavs, nullptr);
        dc->CSSetShader(c.margin.Get(), nullptr, 0);
        dc->Dispatch((t.w + 7) / 8, (t.h + 7) / 8, 1);
        int rowsSlot = -1;
        if (rows) {
            rowsSlot = static_cast<int>(t.rowsSerial % 3);
            dc->CopyResource(t.rowsRead[rowsSlot].Get(), t.rows.Get());
        }
        ComPtr<ID3D11CommandList> groundList;
        if (FAILED(dc->FinishCommandList(FALSE, &groundList)) ||
            !corridor_execute(context, groundList.Get(), "before the corridor's road"))
            return;
        corridor_stamp(p, 2);
        if (rowsSlot >= 0)
            t.rowsWritten[rowsSlot] = ++t.rowsSerial;
        t.groundValid = true;
        ++g.stats.corridorGrounds;
    }
    if (!c.show) {
        if (!c.abOff)  // the mask's side: the periphery into the focus view outside the corridor
            corridor_fill(context, p, k, dsv);
        return;
    }
    // Show mode: the focus viewport of the game's bound colour target tinted where this frame's points lie outside.
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv)
        return;
    dc->UpdateSubresource(c.cb.Get(), 0, nullptr, k, 0, 0);
    const D3D11_VIEWPORT fvp = g.appliedVps[1];
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(nullptr);
    dc->VSSetShader(c.vs.Get(), nullptr, 0);
    dc->PSSetShader(c.tint.Get(), nullptr, 0);
    dc->PSSetConstantBuffers(0, 1, c.cb.GetAddressOf());
    ID3D11ShaderResourceView* tintSrvs[7] = {t.srv.Get(), t.groundSrv.Get(), t.centreSrv.Get(), nullptr, nullptr,
                                             nullptr, t.marginSrv.Get()};
    dc->PSSetShaderResources(0, 7, tintSrvs);
    dc->PSSetSamplers(0, 1, c.linear.GetAddressOf());
    dc->RSSetState(c.raster.Get());
    dc->RSSetViewports(1, &fvp);
    dc->OMSetDepthStencilState(c.noDepth.Get(), 0);
    dc->OMSetBlendState(c.blend.Get(), nullptr, 0xffffffff);
    dc->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    dc->Draw(3, 0);
    ComPtr<ID3D11CommandList> tintList;
    if (SUCCEEDED(dc->FinishCommandList(FALSE, &tintList)) &&
        corridor_execute(context, tintList.Get(), "before the corridor's tint"))
        ++g.stats.corridorTints;
}

// DLSS's band for this pass's focus view (fh rows; DlssPass::focusBandTop): the corridor's rows as last read back (the
// newest copy that has arrived, never waited for), `bandPct` of the view's height over them with `bandPadPct` kept
// either side; taller than the band, from their top (the far road). False: no band (off, the A/B's side without the
// corridor, show mode, or nothing read back yet: DLAA over the whole view).
bool corridor_band(ID3D11DeviceContext* context, int p, unsigned fh, unsigned& top, unsigned& height) {
    auto& c = g.corridor;
    if (p < 0 || p > 1 || c.bandPct <= 0 || c.bandPct >= 100 || !c.on || c.abOff || c.show || !fh)
        return false;
    auto& t = c.copy[p];
    if (!t.valid || !t.rows || !t.h)
        return false;
    // The newest copy that has arrived; it and those older are done with.
    for (uint64_t back = 0; back < 3; ++back) {
        if (t.rowsSerial < back + 1)
            break;
        const uint64_t serial = t.rowsSerial - back;
        const int slot = static_cast<int>((serial - 1) % 3);
        if (t.rowsWritten[slot] != serial)
            continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (context->Map(t.rowsRead[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK)
            continue;
        const uint32_t* v = static_cast<const uint32_t*>(m.pData);
        const uint32_t up = v[0], down = v[1];
        context->Unmap(t.rowsRead[slot].Get(), 0);
        for (uint64_t& w : t.rowsWritten)
            if (w && w <= serial)
                w = 0;
        t.rowsHave = true;
        t.rowsTop = down && up <= t.h ? t.h - up : 0;
        t.rowsBottom = down ? (std::min)(down, t.h) : 0;
        break;
    }
    if (!t.rowsHave)
        return false;
    height = (std::max)(2u, (static_cast<unsigned>(fh * c.bandPct / 100.0) + 1) & ~1u);
    if (height >= fh)
        return false;
    ++c.bands;
    if (t.rowsBottom <= t.rowsTop) {  // no corridor in view: the band where it was
        ++c.bandsNoRows;
        top = c.bandPlaced[p] ? (std::min)(c.bandTop[p], fh - height) : (fh - height) / 2;
    } else {
        const double scale = static_cast<double>(fh) / t.h, pad = fh * c.bandPadPct / 100.0;
        const double need0 = (std::max)(0.0, t.rowsTop * scale - pad);
        const double need1 = (std::min)(static_cast<double>(fh), t.rowsBottom * scale + pad);
        c.bandNeeded += (need1 - need0) / fh;
        double at;
        if (need1 - need0 <= height) {
            at = 0.5 * (need0 + need1) - 0.5 * height;
        } else {
            at = need0;
            ++c.bandsClipped;
        }
        top = static_cast<unsigned>(std::clamp(at, 0.0, static_cast<double>(fh - height)));
    }
    c.bandTop[p] = top;
    c.bandPlaced[p] = true;
    return true;
}

// In an eye pass drawing its focus view, at the pass's first draw testing the scene's depth (the focus mask's moment):
// the focus viewport's texels outside the corridor, by the eye's depth over the box last frame, get the nearest depth.
void corridor_before_draw(ID3D11DeviceContext* context) {
    auto& c = g.corridor;
    const int p = g.pass;
    if (p < 0 || p > 1 || c.done[p] || !g.on || !g.twoVpsApplied || g.afterResolve || g.overlayTarget)
        return;
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv)
        return;
    ComPtr<ID3D11Resource> depthTarget;
    dsv->GetResource(&depthTarget);
    if (depthTarget.Get() != g.mask.sceneDepth[p])
        return;
    ComPtr<ID3D11DepthStencilState> state;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&state, &stencilRef);
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthFunc = D3D11_COMPARISON_LESS;
    if (state)
        state->GetDesc(&d);
    if (!d.DepthEnable)
        return;
    c.done[p] = true;
    if (c.abOff) {
        ++g.stats.corridorAbSkipped;
        return;
    }
    const bool greater = d.DepthFunc == D3D11_COMPARISON_GREATER || d.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
    const bool less = d.DepthFunc == D3D11_COMPARISON_LESS || d.DepthFunc == D3D11_COMPARISON_LESS_EQUAL;
    auto& t = c.copy[p];
    float constants[kCorridorConstants], upV[3];
    if ((!greater && !less) || !t.valid || !t.groundValid || !g.view.box.valid || !corridor_constants(p, constants, upV)) {
        ++g.stats.corridorNoInput;
        return;
    }
    if (!corridor_resources(context))
        return;
    // The mask's stamps (the focus viewport's size), made again when it changes.
    const unsigned fw = static_cast<unsigned>(std::lround(g.appliedVps[1].Width));
    const unsigned fh = static_cast<unsigned>(std::lround(g.appliedVps[1].Height));
    if (!fw || !fh)
        return;
    if (!t.stamps || t.fw != fw || t.fh != fh) {
        t.stamps.Reset();
        t.stampsSrv.Reset();
        t.stampsUav.Reset();
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        const D3D11_TEXTURE2D_DESC sd = {fw, fh, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &t.stamps)) ||
            FAILED(device->CreateUnorderedAccessView(t.stamps.Get(), nullptr, &t.stampsUav)) ||
            FAILED(device->CreateShaderResourceView(t.stamps.Get(), nullptr, &t.stampsSrv))) {
            t.stamps.Reset();
            return;
        }
        t.fw = fw;
        t.fh = fh;
    }
    const uint32_t stamp = ++c.stamp[p] ? c.stamp[p] : ++c.stamp[p];  // never 0 (a new texture's texels)
    std::memcpy(&constants[30], &stamp, sizeof(stamp));
    D3D11_VIEWPORT vp = g.appliedVps[1];  // the focus view's
    vp.MinDepth = vp.MaxDepth = greater ? vp.MaxDepth : vp.MinDepth;  // the nearest depth
    ID3D11DeviceContext* dc = c.deferred.Get();
    dc->UpdateSubresource(c.cb.Get(), 0, nullptr, constants, 0, 0);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(nullptr);
    dc->VSSetShader(c.vs.Get(), nullptr, 0);
    dc->PSSetShader(c.mask.Get(), nullptr, 0);
    dc->PSSetConstantBuffers(0, 1, c.cb.GetAddressOf());
    ID3D11ShaderResourceView* maskSrvs[3] = {t.srv.Get(), t.groundSrv.Get(), t.centreSrv.Get()};
    dc->PSSetShaderResources(0, 3, maskSrvs);
    dc->PSSetShaderResources(6, 1, t.marginSrv.GetAddressOf());
    dc->PSSetSamplers(0, 1, c.linear.GetAddressOf());
    dc->RSSetState(c.raster.Get());
    dc->RSSetViewports(1, &vp);
    dc->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, dsv.Get(), 0, 1, t.stampsUav.GetAddressOf(), nullptr);
    dc->OMSetDepthStencilState(c.depth.Get(), 0);
    dc->Draw(3, 0);
    ComPtr<ID3D11CommandList> list;
    if (FAILED(dc->FinishCommandList(FALSE, &list)))
        return;
    corridor_stamp(p, 3);
    if (corridor_execute(context, list.Get(), "before the corridor's mask")) {
        corridor_stamp(p, 4);
        c.maskedThisPass[p] = true;
        ++g.stats.corridorDraws;
    }
}

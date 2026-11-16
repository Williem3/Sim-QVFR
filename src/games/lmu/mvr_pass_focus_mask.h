// Part of mvr_pass.cpp's translation unit (included once, in order): the focus mask (mvr_focus_mask; quad/focus_mask.h),
// its depth drawn into the focus viewport before an eye pass's scene. Included only by mvr_pass.cpp, inside its
// anonymous namespace (lmu::<anonymous>), after mvr_pass_post_roles.h and before mvr_pass_hooks_entry.h (whose draw
// hooks call it); never include it anywhere else. No includes of its own: mvr_pass.cpp's.
#pragma once

constexpr UINT kMaskMaxVertices = 512;  // the meshes have at most 288 (rounding: 4 corners x 12 segments x 6)

// Its vertex shader, depth-only state and the deferred context it's recorded on; once. False: off for the session.
bool mask_resources(ID3D11DeviceContext* context) {
    auto& m = g.mask;
    if (m.vs)
        return true;
    if (m.failed)
        return false;
    // z 0.5 at a viewport whose depth range is one value: that value.
    static const char kSource[] = "float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0.5, 1); }";
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    ComPtr<ID3DBlob> code, errors;
    const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const D3D11_BUFFER_DESC vb = {kMaskMaxVertices * 2 * sizeof(float), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    const bool ok =
        device && SUCCEEDED(D3DCompile(kSource, sizeof(kSource) - 1, "focus-mask", nullptr, nullptr, "main", "vs_4_0", 0, 0,
                                       &code, &errors)) &&
        SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m.vs)) &&
        SUCCEEDED(device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &m.layout)) &&
        SUCCEEDED(device->CreateBuffer(&vb, nullptr, &m.vb)) && SUCCEEDED(device->CreateDepthStencilState(&ds, &m.depth)) &&
        SUCCEEDED(device->CreateRasterizerState(&rs, &m.raster)) && SUCCEEDED(device->CreateDeferredContext(0, &m.deferred));
    if (!ok) {
        m.vs.Reset();
        m.failed = true;
        simqvfr::log_line("lmu mvr: focus mask: its shader, buffer or states couldn't be made; off for the session");
    }
    return ok;
}

// The mesh for a w x h focus viewport, made again when the shape, the margin or the size changed. False: nothing to mask.
bool mask_mesh(unsigned w, unsigned h) {
    auto& m = g.mask;
    simqvfr::FocusArea area = m.area;
    area.aspect = h ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;  // square texels: the box's tangent aspect
    if (m.meshMade && area == m.meshArea && m.margin == m.meshMargin && w == m.meshW && h == m.meshH)
        return m.vertices > 0;
    m.meshMade = true;
    m.meshArea = area;
    m.meshMargin = m.margin;
    m.meshW = w;
    m.meshH = h;
    const double margin = simqvfr::focus_mask_margin(m.margin, w, h);
    std::vector<float> mesh = simqvfr::focus_mask_mesh(area, margin);
    m.vertices = static_cast<UINT>(std::min<size_t>(mesh.size() / 2, kMaskMaxVertices));
    if (mesh.size() / 2 > kMaskMaxVertices)
        m.vertices = 0;  // (never: the meshes are smaller) rather none than a cut one
    ++m.epoch;
    // The share of the focus view masked, for the log: the texels more than the margin outside the shape.
    int masked = 0;
    constexpr int kGrid = 64;
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x)
            masked += simqvfr::focus_area_inside(area, (x + 0.5) / kGrid, (y + 0.5) / kGrid) < -margin ? 1 : 0;
    char shape[160];
    if (area.corners())
        std::snprintf(shape, sizeof(shape), "corners %.0f/%.0f %.0f/%.0f %.0f/%.0f %.0f/%.0f", area.inset[0] * 100,
                      area.inset[1] * 100, area.inset[2] * 100, area.inset[3] * 100, area.inset[4] * 100, area.inset[5] * 100,
                      area.inset[6] * 100, area.inset[7] * 100);
    else
        std::snprintf(shape, sizeof(shape), "rounded %.0f%%", area.radius * 100);
    if (m.vertices) {
        mesh.resize(kMaskMaxVertices * 2, 0.0f);
        g.context->UpdateSubresource(m.vb.Get(), 0, nullptr, mesh.data(), 0, 0);
        simqvfr::logf("lmu mvr: focus mask: %s, margin %u texels: %u triangles over about %.0f%% of the %ux%u focus view",
                      shape, m.margin, m.vertices / 3, 100.0 * masked / (kGrid * kGrid), w, h);
    } else if (m.on) {
        simqvfr::logf("lmu mvr: focus mask: %s at %ux%u leaves nothing past the %u-texel margin: none", shape, w, h,
                      m.margin);
    }
    return m.vertices > 0;
}

// In an eye pass drawing its focus view, before a draw of the game's: at the pass's first draw that tests the scene's
// depth (the game has cleared it by then), the mask's depth into the focus viewport. Once per pass.
void focus_mask_before_draw(ID3D11DeviceContext* context) {
    auto& m = g.mask;
    const int p = g.pass;
    if (p < 0 || p > 1 || m.done[p] || !g.on || !g.twoVpsApplied || g.afterResolve || g.overlayTarget)
        return;
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv)
        return;
    ComPtr<ID3D11Resource> depthTarget;
    dsv->GetResource(&depthTarget);
    if (depthTarget.Get() != m.sceneDepth[p])
        return;  // another depth target in the pass (the car shadows' own): not the scene's
    ComPtr<ID3D11DepthStencilState> state;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&state, &stencilRef);
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthFunc = D3D11_COMPARISON_LESS;  // the default state's
    if (state)
        state->GetDesc(&d);
    if (!d.DepthEnable)
        return;  // a draw that doesn't test depth (the sky, a clear by a quad): the next one
    m.done[p] = true;
    if (g.maskAbDrop) {
        ++g.stats.focusMaskAbSkipped;
        return;
    }
    const bool greater = d.DepthFunc == D3D11_COMPARISON_GREATER || d.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
    const bool less = d.DepthFunc == D3D11_COMPARISON_LESS || d.DepthFunc == D3D11_COMPARISON_LESS_EQUAL;
    if (!greater && !less) {
        ++g.stats.focusMaskOtherTest;
        if (!m.otherTestLogged) {
            m.otherTestLogged = true;
            simqvfr::logf("lmu mvr: focus mask: pass %d's first depth-tested draw compares with %d: not masked (logged once)",
                          p, static_cast<int>(d.DepthFunc));
        }
        return;
    }
    if (!mask_resources(context))
        return;
    D3D11_VIEWPORT vp = g.appliedVps[1];  // the focus view's, as the scene's draws have it
    const unsigned w = static_cast<unsigned>(std::lround(vp.Width)), h = static_cast<unsigned>(std::lround(vp.Height));
    if (!mask_mesh(w, h)) {
        ++g.stats.focusMaskEmpty;
        return;
    }
    vp.MinDepth = vp.MaxDepth = greater ? vp.MaxDepth : vp.MinDepth;  // the nearest depth
    if (!m.list || m.listDsv.Get() != dsv.Get() || m.listEpoch != m.epoch ||
        std::memcmp(&m.listVp, &vp, sizeof(vp)) != 0) {
        m.list.Reset();
        ID3D11DeviceContext* dc = m.deferred.Get();
        const UINT stride = 2 * sizeof(float), offset = 0;
        dc->IASetInputLayout(m.layout.Get());
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->IASetVertexBuffers(0, 1, m.vb.GetAddressOf(), &stride, &offset);
        dc->VSSetShader(m.vs.Get(), nullptr, 0);
        dc->PSSetShader(nullptr, nullptr, 0);  // depth only
        dc->RSSetState(m.raster.Get());
        dc->RSSetViewports(1, &vp);
        dc->OMSetRenderTargets(0, nullptr, dsv.Get());
        dc->OMSetDepthStencilState(m.depth.Get(), 0);
        dc->Draw(m.vertices, 0);
        if (FAILED(dc->FinishCommandList(FALSE, &m.list))) {
            m.list.Reset();
            return;
        }
        m.listDsv = dsv;
        m.listVp = vp;
        m.listEpoch = m.epoch;
        ++g.stats.focusMaskRecords;
    }
    // NVIDIA's single-pass stereo would reach into the list (the game's command lists: execute_command_list): off
    // around it. Instancing has no mode to switch (the list binds its own shader).
    const bool stereo = g.stereo && !g.instanced;
    if (stereo) {
        Timed switching(g.time.switches);
        if (g.twoViews(g.context, false) != 0) {
            stereo_off_failed("before the focus mask");
            return;
        }
    }
    const bool was = g.replaying;
    g.replaying = true;  // its calls, if d3d11 replays the list through the context's functions, pass the hooks
    call_real<kHookExecute>(context, m.list.Get(), TRUE);
    g.replaying = was;
    if (stereo) {
        Timed switching(g.time.switches);
        if (g.twoViews(g.context, true) != 0) {  // as a draw's two views refused: update() tries, then instancing
            ++g.stats.stereoErrors;
            g.stereo = false;
            update();
        }
    }
    ++g.stats.focusMaskDraws;
}


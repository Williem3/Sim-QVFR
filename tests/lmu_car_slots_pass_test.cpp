// Offline check of the car slots in the eye passes (games/lmu/mvr_pass.h mvr_car_slots) on a WARP device (--hardware:
// the GPU's): the scene's bind in a pass gets the slot target at render target 1, the game's pixel shader its slot twin,
// the game's blend state a twin (target 0 as the game set it; target 1 written where depth is, kept where it isn't); a
// car part with a frame before writes its slot + 1 there, the world 0, the game's image as without; a bind of another
// target and the pass's end put the game's own state back.
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/dxbc_patch.h"
#include "games/lmu/mvr_pass.h"
#include "games/lmu/mvr_probe.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

int fake_two_views(ID3D11DeviceContext*, bool) { return 0; }

ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "t", nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
        std::printf("FAIL compile: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

template <typename T>
T* bound(T* p) {  // a Get* result as a raw pointer (its reference released: the pipeline holds it)
    if (p)
        p->Release();
    return p;
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    const bool hardware = argc > 1 && std::strcmp(argv[1], "--hardware") == 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr,
                                 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::printf("FAIL no %s device\n", hardware ? "hardware" : "WARP");
        return 1;
    }
    // The game's shaders: a vertex shader marked as reading its worldMatrix at slot 3 (as the probe marks LMU's), covering
    // the viewport; a pixel shader with its slot twin attached (as the probe attaches it).
    const ComPtr<ID3DBlob> vsCode = compile(
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1); }\n",
        "vs_5_0");
    const ComPtr<ID3DBlob> psCode =
        compile("float4 main(float4 p : SV_Position) : SV_Target0 { return float4(0.5, 0.25, 0.125, 1); }\n", "ps_5_0");
    if (!vsCode || !psCode)
        return 1;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> gamePs, slotPs;
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &gamePs);
    const simqvfr::dxbc::SlotOutput twin = simqvfr::dxbc::add_slot_output(
        static_cast<const uint8_t*>(psCode->GetBufferPointer()), psCode->GetBufferSize());
    if (!vs || !gamePs || twin.bytecode.empty() ||
        FAILED(device->CreatePixelShader(twin.bytecode.data(), twin.bytecode.size(), nullptr, &slotPs))) {
        std::printf("FAIL shaders\n");
        return 1;
    }
    const UINT place[2] = {3, 0};
    vs->SetPrivateData(lmu::kVsWorldMatrixGuid, sizeof(place), place);
    gamePs->SetPrivateDataInterface(lmu::kMvrSlotTwinGuid, slotPs.Get());

    // The hooks: the ten as always, DrawIndexed, car tracking's five, PSSetShader, the blend and depth-stencil states.
    lmu::mvr_set_two_views(&fake_two_views);
    lmu::mvr_car_track(true);
    lmu::mvr_car_slots(1);
    check(lmu::mvr_pass_install(device.Get()), "the immediate context hooked");
    char what[260];
    std::snprintf(what, sizeof(what), "%llu entries hooked (twenty with car slots, ExecuteCommandList among them)",
                  static_cast<unsigned long long>(lmu::mvr_pass_stats().contextEntries));
    check(lmu::mvr_pass_stats().contextEntries == 20, what);

    // The game's resources: the scene's target and depth, another target, the per-draw buffer, two meshes, an index buffer.
    constexpr UINT W = 64, H = 32;
    const auto texture = [&](DXGI_FORMAT f, UINT bind) {
        const D3D11_TEXTURE2D_DESC t = {W, H, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
        ComPtr<ID3D11Texture2D> tex;
        device->CreateTexture2D(&t, nullptr, &tex);
        return tex;
    };
    const ComPtr<ID3D11Texture2D> scene = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET),
                                  other = texture(DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET),
                                  depth = texture(DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL);
    ComPtr<ID3D11RenderTargetView> sceneRtv, otherRtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    device->CreateRenderTargetView(scene.Get(), nullptr, &sceneRtv);
    device->CreateRenderTargetView(other.Get(), nullptr, &otherRtv);
    device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    ComPtr<ID3D11Buffer> perDraw, vbA, vbB, ib;
    {
        const D3D11_BUFFER_DESC d = {64, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        device->CreateBuffer(&d, nullptr, &perDraw);
        const float verts[16] = {};
        const D3D11_BUFFER_DESC vd = {sizeof(verts), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA vi = {verts, 0, 0};
        device->CreateBuffer(&vd, &vi, &vbA);
        device->CreateBuffer(&vd, &vi, &vbB);
        const uint16_t indices[3] = {0, 1, 2};
        const D3D11_BUFFER_DESC id = {sizeof(indices), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA ii = {indices, 0, 0};
        device->CreateBuffer(&id, &ii, &ib);
    }
    // LMU's kind of blend state: independent blend on, target 0 blended, target 1's write mask 0; a depth state that
    // doesn't write depth (a transparent's).
    ComPtr<ID3D11BlendState> gameBlend;
    {
        D3D11_BLEND_DESC b{};
        b.IndependentBlendEnable = TRUE;
        b.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                             D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        device->CreateBlendState(&b, &gameBlend);
    }
    ComPtr<ID3D11DepthStencilState> noDepthWrite;
    {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        device->CreateDepthStencilState(&d, &noDepthWrite);
    }
    lmu::mvr_test_scene_target(scene.Get());  // as the tracker learns it (MSAA off)

    const auto draw = [&](const float* world, ID3D11Buffer* vb, float x) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(perDraw.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, world, 64);
            context->Unmap(perDraw.Get(), 0);
        }
        const UINT stride = 16, offset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        const D3D11_VIEWPORT half = {x, 0, W / 2.0f, static_cast<float>(H), 0, 1};
        context->RSSetViewports(1, &half);
        context->DrawIndexed(3, 0, 0);
    };
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float moved5[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 0, 0, 1};
    const float moved7[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, 0, 0, 1};
    lmu::MvrPassView view;
    view.eyeW = W;
    view.eyeH = H;
    bool inPassChecked = false;
    // A frame's two eye passes, each: the game's blend state, the scene's bind with depth, a car part on the left half
    // (after the world draw that teaches the per-draw buffer), the static world on the right half.
    const auto frame = [&](const float* part) {
        for (int pass = 0; pass < 2; ++pass) {
            view.eye = pass;
            lmu::mvr_pass_begin(pass, &view, false, false);
            context->OMSetBlendState(gameBlend.Get(), nullptr, 0xffffffff);
            context->OMSetDepthStencilState(nullptr, 0);
            context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), dsv.Get());
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);  // as the game's pass does
            const float black[4] = {0, 0, 0, 0};
            context->ClearRenderTargetView(sceneRtv.Get(), black);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(gamePs.Get(), nullptr, 0);
            context->VSSetConstantBuffers(3, 1, perDraw.GetAddressOf());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            draw(identity, vbA.Get(), W / 2.0f);
            draw(part, vbB.Get(), 0);
            draw(identity, vbA.Get(), W / 2.0f);
            if (!inPassChecked) {
                inPassChecked = true;
                ID3D11RenderTargetView* rtvs[2] = {};
                context->OMGetRenderTargets(2, rtvs, nullptr);
                ComPtr<ID3D11Resource> r1;
                if (rtvs[1])
                    rtvs[1]->GetResource(&r1);
                check(bound(rtvs[0]) == sceneRtv.Get() && bound(rtvs[1]) && r1.Get() == lmu::mvr_car_slot_texture(),
                      "in the pass: the scene's target at 0, the slot target at 1");
                ID3D11PixelShader* ps = nullptr;
                context->PSGetShader(&ps, nullptr, nullptr);
                check(bound(ps) == slotPs.Get(), "the game's pixel shader's slot twin bound in its place");
                ID3D11BlendState* bs = nullptr;
                FLOAT factor[4];
                UINT mask = 0;
                context->OMGetBlendState(&bs, factor, &mask);
                D3D11_BLEND_DESC d{}, gd{};
                if (bound(bs))
                    bs->GetDesc(&d);
                gameBlend->GetDesc(&gd);
                check(bs && bs != gameBlend.Get() && d.IndependentBlendEnable &&
                          std::memcmp(&d.RenderTarget[0], &gd.RenderTarget[0], sizeof(d.RenderTarget[0])) == 0 &&
                          !d.RenderTarget[1].BlendEnable && d.RenderTarget[1].RenderTargetWriteMask == D3D11_COLOR_WRITE_ENABLE_RED,
                      "the game's blend state's twin bound: target 0 as the game's, target 1 unblended and written");
                context->OMSetDepthStencilState(noDepthWrite.Get(), 0);
                context->OMGetBlendState(&bs, factor, &mask);
                if (bound(bs))
                    bs->GetDesc(&d);
                check(d.RenderTarget[1].RenderTargetWriteMask == 0, "a depth state that doesn't write depth: target 1 kept");
                context->OMSetDepthStencilState(nullptr, 0);
                // Another target with depth in the pass: no slot target, the game's own shader and blend state.
                context->OMSetRenderTargets(1, otherRtv.GetAddressOf(), dsv.Get());
                context->OMGetRenderTargets(2, rtvs, nullptr);
                context->PSGetShader(&ps, nullptr, nullptr);
                context->OMGetBlendState(&bs, factor, &mask);
                check(bound(rtvs[0]) == otherRtv.Get() && !bound(rtvs[1]) && bound(ps) == gamePs.Get() &&
                          bound(bs) == gameBlend.Get(),
                      "another target bound in the pass: the game's own targets, shader and blend state");
                context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), dsv.Get());
            }
            lmu::mvr_pass_end(pass);
        }
        lmu::mvr_frame_end(0);
    };
    // The slot target's word at (x, y).
    const auto word_at = [&](UINT x, UINT y) -> int {
        ID3D11Texture2D* slots = lmu::mvr_car_slot_texture();
        if (!slots)
            return -1;
        D3D11_TEXTURE2D_DESC t;
        slots->GetDesc(&t);
        t.Usage = D3D11_USAGE_STAGING;
        t.BindFlags = 0;
        t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        device->CreateTexture2D(&t, nullptr, &staging);
        context->CopyResource(staging.Get(), slots);
        D3D11_MAPPED_SUBRESOURCE m{};
        int v = -1;
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            uint16_t w = 0;
            std::memcpy(&w, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 2, 2);
            v = w;
            context->Unmap(staging.Get(), 0);
        }
        return v;
    };

    frame(moved5);
    std::snprintf(what, sizeof(what), "frame 1 (the part first seen: no frame before): its half %d, the world's %d (0: none)",
                  word_at(8, 8), word_at(W - 8, 8));
    check(word_at(8, 8) == 0 && word_at(W - 8, 8) == 0, what);
    frame(moved7);
    std::snprintf(what, sizeof(what), "frame 2: the part's half holds its slot + 1 (%d), the world's 0 (%d)", word_at(8, 8),
                  word_at(W - 8, 8));
    check(word_at(8, 8) == 1 && word_at(W - 8, 8) == 0, what);
    {  // the game's image as its shader draws it (target 0's blend: source alpha 1, so the source): (0.5, 0.25, 0.125, 1)
        D3D11_TEXTURE2D_DESC t;
        scene->GetDesc(&t);
        t.Usage = D3D11_USAGE_STAGING;
        t.BindFlags = 0;
        t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        device->CreateTexture2D(&t, nullptr, &staging);
        context->CopyResource(staging.Get(), scene.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        uint16_t px[2][4] = {};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            std::memcpy(px[0], static_cast<const uint8_t*>(m.pData) + 8 * m.RowPitch + 8 * 8, 8);
            std::memcpy(px[1], static_cast<const uint8_t*>(m.pData) + 8 * m.RowPitch + (W - 8) * 8, 8);
            context->Unmap(staging.Get(), 0);
        }
        const uint16_t expect[4] = {0x3800, 0x3400, 0x3000, 0x3c00};
        check(std::memcmp(px[0], expect, 8) == 0 && std::memcmp(px[1], expect, 8) == 0,
              "the game's image as its own shader and blend state draw it, both halves");
    }
    {  // after the pass: the game's own state
        ID3D11RenderTargetView* rtvs[2] = {};
        context->OMGetRenderTargets(2, rtvs, nullptr);
        ID3D11PixelShader* ps = nullptr;
        context->PSGetShader(&ps, nullptr, nullptr);
        ID3D11BlendState* bs = nullptr;
        FLOAT factor[4];
        UINT mask = 0;
        context->OMGetBlendState(&bs, factor, &mask);
        ID3D11ShaderResourceView* view127 = nullptr;
        context->PSGetShaderResources(127, 1, &view127);
        check(bound(rtvs[0]) == sceneRtv.Get() && !bound(rtvs[1]) && bound(ps) == gamePs.Get() &&
                  bound(bs) == gameBlend.Get() && !bound(view127),
              "after the pass: the game's target alone, its pixel shader and blend state, no slot view");
    }
    const lmu::MvrPassStats s = lmu::mvr_pass_stats();
    std::snprintf(what, sizeof(what),
                  "counted: %llu passes with the slot target, %llu binds, %llu slot views, %llu twin binds, %llu blend twins",
                  static_cast<unsigned long long>(s.slotPasses), static_cast<unsigned long long>(s.slotBinds),
                  static_cast<unsigned long long>(s.slotViews), static_cast<unsigned long long>(s.slotTwinBinds),
                  static_cast<unsigned long long>(s.blendTwins));
    check(s.slotPasses == 4 && s.slotBinds >= 4 && s.slotViews == 2 && s.slotTwinBinds >= 4 && s.blendTwins == 2, what);

    // The game's MSAA on: the scene's target and its depth multisample (4x here). The slot target beside it must be
    // multisample too (render targets bound together share the sample count), and every sample of the part's pixels
    // takes its slot: DLSS's vectors read the first, as they do of the depth.
    {
        const D3D11_TEXTURE2D_DESC ms = {W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {4, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
        D3D11_TEXTURE2D_DESC msDepth = ms;
        msDepth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        msDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> sceneMs, depthMs;
        device->CreateTexture2D(&ms, nullptr, &sceneMs);
        device->CreateTexture2D(&msDepth, nullptr, &depthMs);
        sceneRtv.Reset();
        dsv.Reset();
        check(sceneMs && depthMs && SUCCEEDED(device->CreateRenderTargetView(sceneMs.Get(), nullptr, &sceneRtv)) &&
                  SUCCEEDED(device->CreateDepthStencilView(depthMs.Get(), nullptr, &dsv)),
              "MSAA: a 4x multisample scene target and depth are made");
        const unsigned long long before = lmu::mvr_pass_stats().slotPasses;
        frame(moved5);  // the part has its frame before from above
        D3D11_TEXTURE2D_DESC slotDesc{};
        if (ID3D11Texture2D* slots = lmu::mvr_car_slot_texture())
            slots->GetDesc(&slotDesc);
        std::snprintf(what, sizeof(what), "MSAA: the slot target is %ux%u x%u, %llu passes with it", slotDesc.Width, slotDesc.Height,
                      slotDesc.SampleDesc.Count, static_cast<unsigned long long>(lmu::mvr_pass_stats().slotPasses - before));
        check(slotDesc.SampleDesc.Count == 4 && slotDesc.Width == W && slotDesc.Height == H && lmu::mvr_pass_stats().slotPasses - before == 2,
              what);
        // Its samples read out: sample `n` of each pixel into a single-sample R16_UINT target.
        const auto vsFull = compile("float4 main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); "
                                    "return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }",
                                    "vs_5_0");
        const auto psRead = compile("Texture2DMS<uint> t : register(t0); cbuffer C : register(b0) { uint n; }; "
                                    "uint main(float4 p : SV_Position) : SV_Target { return t.Load(int2(p.xy), n); }",
                                    "ps_5_0");
        ComPtr<ID3D11VertexShader> full;
        ComPtr<ID3D11PixelShader> read;
        ComPtr<ID3D11ShaderResourceView> slotSrv;
        ComPtr<ID3D11Buffer> sampleCb;
        const ComPtr<ID3D11Texture2D> out = texture(DXGI_FORMAT_R16_UINT, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11RenderTargetView> outRtv;
        const D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        bool ok = vsFull && psRead && lmu::mvr_car_slot_texture() &&
                  SUCCEEDED(device->CreateVertexShader(vsFull->GetBufferPointer(), vsFull->GetBufferSize(), nullptr, &full)) &&
                  SUCCEEDED(device->CreatePixelShader(psRead->GetBufferPointer(), psRead->GetBufferSize(), nullptr, &read)) &&
                  SUCCEEDED(device->CreateShaderResourceView(lmu::mvr_car_slot_texture(), nullptr, &slotSrv)) &&
                  SUCCEEDED(device->CreateBuffer(&cd, nullptr, &sampleCb)) &&
                  SUCCEEDED(device->CreateRenderTargetView(out.Get(), nullptr, &outRtv));
        int part[2] = {-1, -1}, world[2] = {-1, -1};
        for (int k = 0; k < 2 && ok; ++k) {
            const UINT sample[4] = {k == 0 ? 0u : 3u, 0, 0, 0};
            context->UpdateSubresource(sampleCb.Get(), 0, nullptr, sample, 0, 0);
            context->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
            context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            const D3D11_VIEWPORT all = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
            context->RSSetViewports(1, &all);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(full.Get(), nullptr, 0);
            context->PSSetShader(read.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, sampleCb.GetAddressOf());
            context->PSSetShaderResources(0, 1, slotSrv.GetAddressOf());
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            context->PSSetShaderResources(0, 1, &none);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            D3D11_TEXTURE2D_DESC t;
            out->GetDesc(&t);
            t.Usage = D3D11_USAGE_STAGING, t.BindFlags = 0, t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            device->CreateTexture2D(&t, nullptr, &staging);
            context->CopyResource(staging.Get(), out.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                uint16_t a = 0, b = 0;
                std::memcpy(&a, static_cast<const uint8_t*>(m.pData) + 8 * m.RowPitch + 8 * 2, 2);
                std::memcpy(&b, static_cast<const uint8_t*>(m.pData) + 8 * m.RowPitch + (W - 8) * 2, 2);
                part[k] = a, world[k] = b;
                context->Unmap(staging.Get(), 0);
            }
        }
        std::snprintf(what, sizeof(what), "MSAA: the part's half holds its slot + 1 in sample 0 (%d) and sample 3 (%d), the world's 0 (%d, %d)",
                      part[0], part[1], world[0], world[1]);
        check(ok && part[0] == 1 && part[1] == 1 && world[0] == 0 && world[1] == 0, what);
    }
    context->Flush();
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

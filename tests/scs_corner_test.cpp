// Offline check of the SCS games' corner (games/scssoftware/corner.h, corner_rules.h; docs/recon-ats.md "Stage 1 design notes").
// 1. The rules on the constant ranges captured from the game (ats_frame5585.rdc): the deferred pair, the source
//    rectangle, the histogram's pixel rectangle, and ranges left alone.
// 2. On a WARP device through the real hooks: a G-buffer bind starts the corner; a full-screen pass shaped like Prism's
//    (its vertex shader's rectangle and normalized_to_texture/viewport_size pair, the pixel shader's pixel lookups and
//    screen fractions, constants bound by range in a dynamic buffer) draws only into the corner, with screen fractions
//    0..1 across it and texture fractions 0..f; the game's own constants are bound again after the draw; the frame's
//    end cuts the submitted rectangle and the next frame's shared work gets the game's own viewport back.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/dxbc_patch.h"
#include "games/scssoftware/corner.h"
#include "games/scssoftware/corner_internal.h"
#include "engine/multi_view_shaders.h"
#include "games/scssoftware/corner_rules.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;

namespace {

// The game's main thread at xrDestroySession (not the one drawing).
DWORD WINAPI end_session_elsewhere(void*) {
    scs::corner_session_end();
    return 0;
}

// A stand-in for the runtime's xrEnumerateSwapchainImages: two images, the test's eye target and a decoy.
ID3D11Texture2D* swapImage = nullptr;
ID3D11Texture2D* decoyImage = nullptr;
XrResult XRAPI_CALL fake_enumerate(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = 2;
    if (capacity >= 2 && images) {
        reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture = swapImage;
        reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[1].texture = decoyImage;
    }
    return XR_SUCCESS;
}

void rules_checks() {
    using scs::CornerRule;
    {  // VS 152: the screen rectangle stays, the pair is rewritten
        float v[4][4] = {{0, 0, 1, 1}, {1, -0.5f, 0.5f, 25.4547f}, {1, 1, 1, 1}, {5192, 4168, 1, 1}};
        const CornerRule r = scs::corner_patch(v, 4, 0.5f, 0.25f);
        check(r == CornerRule::pair && scs::corner_eq(v[0], 0, 0, 1, 1) && scs::corner_eq(v[2], 0.5f, 0.25f, 2, 4) &&
                  scs::corner_eq(v[3], 2596, 1042, 0.5f, 0.25f) && v[1][3] == 25.4547f,
              "rules: VS 152's pair rewritten, its screen rectangle and parameters kept");
    }
    {  // VS 629: the pair at 3
        float v[5][4] = {{0, 0, 1, 1}, {1, -0.5f, 0.5f, 0}, {0.5f, 0.5f, 5, 0}, {1, 1, 1, 1}, {5192, 4168, 1, 1}};
        check(scs::corner_patch(v, 5, 0.5f, 0.5f) == CornerRule::pair && scs::corner_eq(v[3], 0.5f, 0.5f, 2, 2) &&
                  scs::corner_eq(v[4], 2596, 2084, 0.5f, 0.5f) && scs::corner_eq(v[2], 0.5f, 0.5f, 5, 0),
              "rules: VS 629's pair at its fourth vector rewritten");
    }
    {  // PS 631: other vectors around the pair kept
        float v[7][4] = {{0.5f, 0.5f, 1, 1}, {2.0944f, 0, 8, 1.2f}, {0.974004f, 0.25f, 0.2f, 0}, {1, 1, 1, 1},
                         {5192, 4168, 1, 1}, {2.70692f, 3.66836f, 5.02198f, 0}, {1.01879f, -0.984434f, -2.45128f, 1.96858f}};
        const CornerRule r = scs::corner_patch(v, 7, 0.75f, 0.75f, false);
        check(r == CornerRule::pair && scs::corner_eq(v[0], 0.5f, 0.5f, 1, 1) && close_to(v[3][2], 1 / 0.75f, 1e-4f) &&
                  scs::corner_eq(v[4], 3894, 3126, 0.75f, 0.75f) && v[6][0] == 1.01879f,
              "rules: PS 631's pair rewritten, the AO parameters and view-ray vectors kept");
    }
    {  // VS 593 (the luminance downsample): the source rectangle
        float v[4][4] = {{1, -0.5f, 0.5f, 0.121109f}, {0, 0, 1, 1}, {5192, 4168, 1, 1}, {1.52588e-05f, 26.3437f, 0.102486f, 0}};
        const CornerRule r = scs::corner_patch(v, 4, 0.5f, 0.5f);
        check(r == CornerRule::rect && scs::corner_eq(v[1], 0, 0, 0.5f, 0.5f) && scs::corner_eq(v[2], 5192, 4168, 1, 1),
              "rules: VS 593's source rectangle cut, its source size kept");
    }
    {  // VS 571 (a bloom downsample, two vectors) with another draw's pair left behind it in the block: the cause of
       // the strobe. The rectangle is cut all the same, the left-over untouched.
        float v[16][4] = {{1, -0.5f, 0.5f, 0.121109f}, {0, 0, 1, 1}, {1, 1, 1, 1}, {5192, 4168, 1, 1}};
        const CornerRule r = scs::corner_patch(v, 16, 0.7f, 0.7f);
        check(r == CornerRule::rect && scs::corner_eq(v[1], 0, 0, 0.7f, 0.7f) && scs::corner_eq(v[2], 1, 1, 1, 1) &&
                  scs::corner_eq(v[3], 5192, 4168, 1, 1),
              "rules: a downsample's rectangle cut although another draw's pair is left behind it");
    }
    {  // VS 394 (a blur: rectangle times the pair): only the rectangle, or the blur would shrink twice
        float v[5][4] = {{1, -0.5f, 0.5f, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, {5192, 4168, 1, 1}, {2.7f, 3.6f, 5, 0}};
        check(scs::corner_patch(v, 5, 0.7f, 0.7f) == CornerRule::rect && scs::corner_eq(v[1], 0, 0, 0.7f, 0.7f) &&
                  scs::corner_eq(v[2], 1, 1, 1, 1),
              "rules: VS 394's rectangle cut once (its pair kept whole)");
    }
    {  // VS 62 (the final blit) with a left-over pair behind it: the source cut, the destination kept
        float v[16][4] = {{0, 0, 1, 1}, {0, 0, 1, 1}, {7, 7, 7, 7}, {1, 1, 1, 1}, {1298, 1042, 1, 1}};
        const CornerRule r = scs::corner_patch(v, 16, 0.5f, 0.5f);
        check(r == CornerRule::rect && scs::corner_eq(v[0], 0, 0, 0.5f, 0.5f) && scs::corner_eq(v[1], 0, 0, 1, 1) &&
                  scs::corner_eq(v[3], 1, 1, 1, 1),
              "rules: VS 62's source rectangle cut, its destination kept, a left-over pair ignored");
    }
    {  // VS 388 (the histogram): its pixel rectangle in the luminance target
        float v[3][4] = {{1298, 1042, 1, 1}, {0, 24.75f, 1290, 992}, {0, 0, 0, 0}};
        const CornerRule r = scs::corner_patch(v, 3, 0.5f, 0.5f);
        check(r == CornerRule::pixelRect && scs::corner_eq(v[1], 0, 12.375f, 645, 496) &&
                  scs::corner_eq(v[0], 1298, 1042, 1, 1),
              "rules: VS 388's pixel rectangle cut");
    }
    {  // a scene draw's matrix rows, and a pixel range without a pair: nothing
        float v[4][4] = {{0.999436f, 0.0133385f, 0.0307816f, 0.45f}, {0.1f, 0.9f, 0, 0}, {1, 1, 1, 1}, {5192, 4168, 1, 1}};
        float before[4][4];
        std::memcpy(before, v, sizeof(v));
        float w[2][4] = {{0.000192604f, 0.000239923f, 0.000856936f, -0.00152038f}, {0, 0, 1, 1}};
        float wBefore[2][4];
        std::memcpy(wBefore, w, sizeof(w));
        check(scs::corner_patch(v, 4, 0.5f, 0.5f) == CornerRule::none && std::memcmp(before, v, sizeof(v)) == 0 &&
                  scs::corner_patch(w, 2, 0.5f, 0.5f, false) == CornerRule::none && std::memcmp(wBefore, w, sizeof(w)) == 0,
              "rules: a vertex range of another layout and a pixel range without a pair left alone");
    }
}

// ---- WARP ----

const char kVs[] = R"(
cbuffer c0 : register(b0) { float4 rect; float4 p1; float4 nt; float4 vsz; };
struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };
O main(float2 v : POSITION) {
    O o;
    o.pos = float4(v.x * 2 - 1, -(v.y * 2 - 1) * p1.x, 0.5, 1);
    o.t.xy = (v * rect.zw + rect.xy) * nt.xy;
    o.t.zw = v * vsz.xy;
    return o;
}
)";
const char kPs[] = R"(
cbuffer c0 : register(b0) { float4 nt; float4 vsz; };
float4 main(float4 pos : SV_Position, float4 t : TEXCOORD0) : SV_Target {
    return float4(t.xy * nt.zw, t.zw / (vsz.xy / vsz.zw));
}
)";

ComPtr<ID3DBlob> compile(const char* src, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(src, std::strlen(src), "test", nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
        std::printf("compile: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

void warp_checks() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context0))) {
        check(false, "a WARP device");
        return;
    }
    ComPtr<ID3D11DeviceContext1> context;
    context0.As(&context);
    constexpr UINT W = 1280, H = 1024;  // a G-buffer counts from 1024x1024 up (corner.cpp, pass_probe.h)
    const auto rt = [&](UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& v) {
        const D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
        device->CreateTexture2D(&d, nullptr, &t);
        device->CreateRenderTargetView(t.Get(), nullptr, &v);
    };
    ComPtr<ID3D11Texture2D> gbTex[4], outTex, depthTex;
    ComPtr<ID3D11RenderTargetView> gb[4], out;
    for (int i = 0; i < 4; ++i)
        rt(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, gbTex[i], gb[i]);
    rt(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, outTex, out);
    const D3D11_TEXTURE2D_DESC dd = {W, H, 1, 1, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_DEPTH_STENCIL, 0, 0};
    ComPtr<ID3D11DepthStencilView> dsv;
    device->CreateTexture2D(&dd, nullptr, &depthTex);
    device->CreateDepthStencilView(depthTex.Get(), nullptr, &dsv);

    const ComPtr<ID3DBlob> vsCode = compile(kVs, "vs_5_0"), psCode = compile(kPs, "ps_5_0");
    if (!vsCode || !psCode) {
        check(false, "the test's shaders");
        return;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps);
    const D3D11_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    device->CreateInputLayout(&el, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout);
    const float tri[] = {0, 0, 2, 0, 0, 2};
    const D3D11_BUFFER_DESC vbd = {sizeof(tri), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA vbi = {tri, 0, 0};
    ComPtr<ID3D11Buffer> vb;
    device->CreateBuffer(&vbd, &vbi, &vb);

    // The game's constants: one dynamic buffer, the VS range at constant 16 and the PS range at 32 (bound by range).
    const D3D11_BUFFER_DESC cbd = {64 * 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> gameCb;
    device->CreateBuffer(&cbd, nullptr, &gameCb);
    {
        D3D11_MAPPED_SUBRESOURCE m;
        context->Map(gameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        float* f = static_cast<float*>(m.pData);
        std::memset(f, 0, 64 * 16);
        const float vsRange[4][4] = {{0, 0, 1, 1}, {1, -0.5f, 0.5f, 7}, {1, 1, 1, 1}, {W, H, 1, 1}};
        const float psRange[2][4] = {{1, 1, 1, 1}, {W, H, 1, 1}};
        std::memcpy(f + 16 * 4, vsRange, sizeof(vsRange));
        std::memcpy(f + 32 * 4, psRange, sizeof(psRange));
        context->Unmap(gameCb.Get(), 0);
    }

    // The install is asked for at xrCreateSession and done at the session's first xrEndFrame (the render thread's).
    // The GPU timing on, as in dev builds: its queries run beside everything checked below.
    scs::corner_set_diag(false, false, 0, true);
    scs::corner_violet_log(true);  // the anomaly log on too: every check below runs with its dispatch and draw records
    check(scs::corner_install(device.Get(), 50, 30) && !scs::corner_on(), "corner install asked for, nothing done yet");
    check(scs::corner_frame_end(nullptr, &fake_enumerate) == nullptr && scs::corner_on(),
          "corner installed (50%, focus 30%) on a WARP device at the first xrEndFrame");

    // The game's frame: a G-buffer bind (the corner starts), then a full-screen pass into an eye-sized target.
    ID3D11RenderTargetView* const gbs[] = {gb[0].Get(), gb[1].Get(), gb[2].Get(), gb[3].Get()};
    context->OMSetRenderTargets(4, gbs, dsv.Get());
    const float clear[4] = {-1, -1, -1, -1};
    context->ClearRenderTargetView(out.Get(), clear);
    ID3D11RenderTargetView* const outs[] = {out.Get()};
    context->OMSetRenderTargets(1, outs, nullptr);
    const D3D11_VIEWPORT full = {0, 0, W, H, 0, 1};
    context->RSSetViewports(1, &full);
    D3D11_VIEWPORT seen{};
    UINT seenCount = 1;
    context->RSGetViewports(&seenCount, &seen);
    check(seenCount == 1 && seen.Width == W / 2 && seen.Height == H / 2, "an eye-shaped target's viewport cut to the corner");

    const UINT stride = 8, offset = 0;
    ID3D11Buffer* const vbs[] = {vb.Get()};
    context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11Buffer* const cbs[] = {gameCb.Get()};
    const UINT vsFirst = 16, psFirst = 32, num = 16;
    context->VSSetConstantBuffers1(0, 1, cbs, &vsFirst, &num);
    context->PSSetConstantBuffers1(0, 1, cbs, &psFirst, &num);
    context->Draw(3, 0);

    ID3D11Buffer* boundVs = nullptr;
    UINT bvFirst = 0, bvNum = 0;
    context->VSGetConstantBuffers1(0, 1, &boundVs, &bvFirst, &bvNum);
    check(boundVs == gameCb.Get() && bvFirst == vsFirst && bvNum == num, "the game's own range bound again after the draw");
    if (boundVs)
        boundVs->Release();

    // Read the target back.
    D3D11_TEXTURE2D_DESC sd;
    outTex->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    device->CreateTexture2D(&sd, nullptr, &staging);
    context->CopyResource(staging.Get(), outTex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        const auto px = [&](UINT x, UINT y) { return reinterpret_cast<const float*>(static_cast<const char*>(m.pData) + y * m.RowPitch) + x * 4; };
        const float* a = px(0, 0);
        const float* b = px(W / 2 - 1, H / 2 - 1);
        const float* outside = px(W / 2 + 4, H / 2 + 4);
        const float* right = px(W - 1, 0);
        std::printf("     corner pixel 0,0: %.4f %.4f %.4f %.4f; last %u,%u: %.4f %.4f %.4f %.4f; outside: %.1f\n", a[0], a[1],
                    a[2], a[3], W / 2 - 1, H / 2 - 1, b[0], b[1], b[2], b[3], outside[0]);
        check(close_to(a[0], 0.5f / (W / 2), 2e-3f) && close_to(b[0], 1 - 0.5f / (W / 2), 2e-3f) && close_to(b[1], 1 - 0.5f / (H / 2), 2e-3f),
              "screen fractions run 0..1 across the corner (the view-ray math sees the whole eye)");
        check(close_to(b[2], 0.5f - 0.5f / W, 2e-3f) && close_to(b[3], 0.5f - 0.5f / H, 2e-3f),
              "texture fractions run 0..f (the lookups stay inside the corner)");
        check(outside[0] == -1 && right[0] == -1, "nothing drawn outside the corner and the focus region");
        // Stage 3: the pass drawn again over the focus region (right of the corner, 30% each way), addressed from the
        // origin of its inputs' twins: screen fractions 0..1 across the region, texture fractions 0..the region's share
        // of a twin (a twin is the region and 4% more, in multiples of 8 texels: 400x320 for 384x307).
        const UINT fx0 = W / 2, fw = 384, fh = 307;
        const float* f0 = px(fx0, 0);
        const float* f1 = px(fx0 + fw - 1, fh - 1);
        const float* below = px(fx0 + 8, fh + 4);
        std::printf("     focus region first %.4f %.4f %.4f %.4f; last %.4f %.4f %.4f %.4f; below %.1f\n", f0[0], f0[1], f0[2],
                    f0[3], f1[0], f1[1], f1[2], f1[3], below[0]);
        check(close_to(f0[0], 0.5f / fw, 2e-3f) && close_to(f1[0], 1 - 0.5f / fw, 2e-3f) && close_to(f1[1], 1 - 0.5f / fh, 3e-3f) &&
                  close_to(f1[2], 384.0f / 400, 3e-3f) && close_to(f1[3], 307.0f / 320, 4e-3f) && below[0] == -1,
              "the pass drawn again over the focus region: screen fractions 0..1, texture fractions 0..its share of a twin");
        context->Unmap(staging.Get(), 0);
    } else {
        check(false, "the target read back");
    }

    // The frame's end: the submitted rectangle cut; the next frame's first bind puts the game's own viewport back.
    XrCompositionLayerProjectionView views[2] = {};
    for (auto& v : views) {
        v.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        v.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(W), static_cast<int32_t>(H)}};
    }
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    proj.viewCount = 2;
    proj.views = views;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj)};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.layerCount = 1;
    info.layers = layers;
    swapImage = outTex.Get();
    ComPtr<ID3D11Texture2D> decoyTex;
    {
        D3D11_TEXTURE2D_DESC dd2;
        outTex->GetDesc(&dd2);
        device->CreateTexture2D(&dd2, nullptr, &decoyTex);
        ComPtr<ID3D11RenderTargetView> dv;
        device->CreateRenderTargetView(decoyTex.Get(), nullptr, &dv);
        const float decoyClear[4] = {-2, -2, -2, -2};
        context->ClearRenderTargetView(dv.Get(), decoyClear);
    }
    decoyImage = decoyTex.Get();
    const XrFrameEndInfo* sent = scs::corner_frame_end(&info, &fake_enumerate);  // the image not seen bound yet: the cut
    const auto* sentProj = reinterpret_cast<const XrCompositionLayerProjection*>(sent->layers[0]);
    check(sent != &info && sentProj->views[1].subImage.imageRect.extent.width == static_cast<int32_t>(W / 2) &&
              sentProj->views[1].subImage.imageRect.extent.height == static_cast<int32_t>(H / 2) &&
              views[1].subImage.imageRect.extent.width == static_cast<int32_t>(W),
          "the frame's end submits each eye's corner (the game's own structures untouched)");
    check(scs::corner_frame_end(&info, &fake_enumerate) == &info, "a frame without its eyes in the corner (a menu) is submitted as drawn");
    context->OMSetRenderTargets(1, outs, nullptr);
    seenCount = 1;
    context->RSGetViewports(&seenCount, &seen);
    check(seen.Width == W && seen.Height == H, "the next frame's work before its G-buffer gets the game's own viewport");

    // Frame 2: the same pass drawn indexed (as ATS's light volumes and forward draws are), its constants in a large
    // dynamic buffer like the game's 2 MB ones: no draw of its own is rewritten, the frame's mirror carries the corner.
    const D3D11_BUFFER_DESC bigd = {512 * 1024, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> bigCb;
    device->CreateBuffer(&bigd, nullptr, &bigCb);
    {
        D3D11_MAPPED_SUBRESOURCE bm;
        context->Map(bigCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &bm);
        float* f = static_cast<float*>(bm.pData);
        std::memset(f, 0, 512 * 1024);
        const float vsRange[4][4] = {{0, 0, 1, 1}, {1, -0.5f, 0.5f, 7}, {1, 1, 1, 1}, {W, H, 1, 1}};
        const float psRange[2][4] = {{1, 1, 1, 1}, {W, H, 1, 1}};
        const float shadowPair[2][4] = {{1, 1, 1, 1}, {4096, 4096, 1, 1}};  // not the eye's shape: left alone
        std::memcpy(f + 64 * 4, vsRange, sizeof(vsRange));
        std::memcpy(f + 80 * 4, psRange, sizeof(psRange));
        std::memcpy(f + 96 * 4, shadowPair, sizeof(shadowPair));
        context->Unmap(bigCb.Get(), 0);
    }
    const unsigned short idx[] = {0, 1, 2};
    const D3D11_BUFFER_DESC ibd = {sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA ibi = {idx, 0, 0};
    ComPtr<ID3D11Buffer> ib;
    device->CreateBuffer(&ibd, &ibi, &ib);
    context->OMSetRenderTargets(4, gbs, dsv.Get());  // frame 2's G-buffer: the corner again, the mirror refreshed
    {  // Stage 2 without NVIDIA's: an indexed scene draw whose shader carries an instanced twin goes out doubled, its
       // second instance into the focus region beside the corner (WARP lets a vertex shader pick the viewport).
        D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
        const bool can = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3))) &&
                         o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
        const ComPtr<ID3DBlob> sceneVs = compile(
            "float4 main(float2 v : POSITION) : SV_Position { return float4(v.x * 2 - 1, 1 - v.y * 2, 0.5, 1); }", "vs_5_0");
        const ComPtr<ID3DBlob> scenePs = compile("float4 main() : SV_Target { return 5; }", "ps_5_0");
        ComPtr<ID3D11VertexShader> svs, twin;
        ComPtr<ID3D11PixelShader> sps;
        device->CreateVertexShader(sceneVs->GetBufferPointer(), sceneVs->GetBufferSize(), nullptr, &svs);
        device->CreatePixelShader(scenePs->GetBufferPointer(), scenePs->GetBufferSize(), nullptr, &sps);
        const simqvfr::dxbc::InstancedView iv = simqvfr::dxbc::add_instanced_view(
            static_cast<const uint8_t*>(sceneVs->GetBufferPointer()), sceneVs->GetBufferSize());
        if (!iv.bytecode.empty())
            device->CreateVertexShader(iv.bytecode.data(), iv.bytecode.size(), nullptr, &twin);
        if (!can) {
            std::printf("     (this device has no viewport index from a vertex shader: the instancing check is skipped)\n");
        } else if (!svs || !sps || !twin) {
            check(false, "the scene shader and its instanced twin created");
        } else {
            svs->SetPrivateDataInterface(simqvfr::mvs::kMvrInstancedTwinGuid, twin.Get());
            context->ClearRenderTargetView(gb[0].Get(), clear);
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            context->RSSetViewports(1, &full);
            context->VSSetShader(svs.Get(), nullptr, 0);
            context->PSSetShader(sps.Get(), nullptr, 0);
            context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
            context->DrawIndexed(3, 0, 0);
            ID3D11VertexShader* after = nullptr;
            context->VSGetShader(&after, nullptr, nullptr);
            check(after == svs.Get(), "the game's own vertex shader bound again after the doubled draw");
            if (after)
                after->Release();
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(ps.Get(), nullptr, 0);
            D3D11_TEXTURE2D_DESC gd;
            gbTex[0]->GetDesc(&gd);
            gd.Usage = D3D11_USAGE_STAGING;
            gd.BindFlags = 0;
            gd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> gs;
            device->CreateTexture2D(&gd, nullptr, &gs);
            context->CopyResource(gs.Get(), gbTex[0].Get());
            D3D11_MAPPED_SUBRESOURCE gm;
            if (SUCCEEDED(context->Map(gs.Get(), 0, D3D11_MAP_READ, 0, &gm))) {
                // R16G16B16A16_FLOAT: 5.0 is 0x4500, the clear's -1.0 is 0xBC00.
                const auto red = [&](UINT x, UINT y) {
                    return *(reinterpret_cast<const unsigned short*>(static_cast<const char*>(gm.pData) + y * gm.RowPitch) + x * 4);
                };
                const UINT fx0 = W / 2, fw = 384, fh = 307;
                std::printf("     instancing: corner %04x %04x, focus region %04x %04x, right of it %04x, below it %04x\n",
                            red(4, 4), red(W / 2 - 2, H / 2 - 2), red(fx0 + 2, 2), red(fx0 + fw - 2, fh - 2), red(fx0 + fw + 8, 8),
                            red(fx0 + 8, fh + 8));
                check(red(4, 4) == 0x4500 && red(W / 2 - 2, H / 2 - 2) == 0x4500, "instancing: the eye's instance fills the corner");
                check(red(fx0 + 2, 2) == 0x4500 && red(fx0 + fw - 2, fh - 2) == 0x4500,
                      "instancing: the second instance fills the focus region beside the corner");
                check(red(fx0 + fw + 8, 8) == 0xBC00 && red(fx0 + 8, fh + 8) == 0xBC00 && red(8, H / 2 + 8) == 0xBC00,
                      "instancing: nothing drawn outside the corner and the focus region");
                context->Unmap(gs.Get(), 0);
            } else {
                check(false, "the G-buffer read back (instancing)");
            }
        }
    }
    context->ClearRenderTargetView(out.Get(), clear);
    context->OMSetRenderTargets(1, outs, nullptr);
    context->RSSetViewports(1, &full);
    ID3D11Buffer* const bigs[] = {bigCb.Get()};
    const UINT bigVs = 64, bigPs = 80;
    context->VSSetConstantBuffers1(0, 1, bigs, &bigVs, &num);
    context->PSSetConstantBuffers1(0, 1, bigs, &bigPs, &num);
    ID3D11Buffer* boundPs = nullptr;
    UINT bpFirst = 0, bpNum = 0;
    context->PSGetConstantBuffers1(0, 1, &boundPs, &bpFirst, &bpNum);
    check(boundPs && boundPs != bigCb.Get() && bpFirst == bigPs && bpNum == num,
          "a large constant buffer bound during the corner goes to our copy, its range kept");
    if (boundPs)
        boundPs->Release();
    context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->DrawIndexed(3, 0, 0);
    context->CopyResource(staging.Get(), outTex.Get());
    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        const auto px2 = [&](UINT x, UINT y) { return reinterpret_cast<const float*>(static_cast<const char*>(m.pData) + y * m.RowPitch) + x * 4; };
        const float* b2 = px2(W / 2 - 1, H / 2 - 1);
        const float* out2 = px2(W / 2 + 4, H / 2 + 4);
        std::printf("     indexed: last corner pixel %.4f %.4f %.4f %.4f; outside %.1f\n", b2[0], b2[1], b2[2], b2[3], out2[0]);
        check(close_to(b2[0], 1 - 0.5f / (W / 2), 2e-3f) && close_to(b2[2], 0.5f - 0.5f / W, 2e-3f) && out2[0] == -1,
              "an indexed draw reading the mirrored pair: screen fractions 0..1 across the corner, texture 0..f");
        context->Unmap(staging.Get(), 0);
    } else {
        check(false, "the target read back (indexed)");
    }
    {  // the game writes the buffer again mid-frame (its pool, S1 run 2): the next bind gets a fresh copy
        D3D11_MAPPED_SUBRESOURCE bm;
        context->Map(bigCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &bm);
        float* f = static_cast<float*>(bm.pData);
        std::memset(f, 0, 512 * 1024);
        const float vsRange[4][4] = {{0, 0, 1, 1}, {1, -0.5f, 0.5f, 7}, {1, 1, 1, 1}, {W, H, 1, 1}};
        const float psRange[3][4] = {{1, 1, 1, 1}, {W, H, 1, 1}, {5, 6, 7, 8}};
        const float shadowPair[2][4] = {{1, 1, 1, 1}, {4096, 4096, 1, 1}};
        std::memcpy(f + 64 * 4, vsRange, sizeof(vsRange));
        std::memcpy(f + 80 * 4, psRange, sizeof(psRange));
        std::memcpy(f + 96 * 4, shadowPair, sizeof(shadowPair));
        context->Unmap(bigCb.Get(), 0);
        context->PSSetConstantBuffers1(0, 1, bigs, &bigPs, &num);
    }
    {  // what the copy holds: the eye's pair rewritten, the shadow map's pair as it was
        ID3D11Buffer* copy = nullptr;
        UINT cf = 0, cn = 0;
        context->PSGetConstantBuffers1(0, 1, &copy, &cf, &cn);
        const D3D11_BUFFER_DESC rd = {512 * 1024, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ComPtr<ID3D11Buffer> readBack;
        device->CreateBuffer(&rd, nullptr, &readBack);
        if (copy) {
            context->CopyResource(readBack.Get(), copy);
            copy->Release();
        }
        D3D11_MAPPED_SUBRESOURCE rm;
        if (SUCCEEDED(context->Map(readBack.Get(), 0, D3D11_MAP_READ, 0, &rm))) {
            const float* v = static_cast<const float*>(rm.pData);
            check(close_to(v[80 * 4], 0.5f, 1e-4f) && close_to(v[80 * 4 + 2], 2, 1e-4f) && close_to(v[81 * 4], W / 2.0f, 1e-4f) &&
                      v[96 * 4] == 1 && v[97 * 4] == 4096 && v[65 * 4 + 3] == 7,
                  "the copy: the eye's pairs rewritten, a shadow map's pair and other values as the game wrote them");
            check(v[82 * 4] == 5 && v[82 * 4 + 3] == 8, "a buffer the game wrote again mid-frame is copied again at its next bind");
            context->Unmap(readBack.Get(), 0);
        } else {
            check(false, "the copy read back");
        }
    }
    scs::corner_acquired(XR_NULL_HANDLE, 0, &fake_enumerate);  // the game's image for this frame
    scs::corner_acquired(XR_NULL_HANDLE, 1, &fake_enumerate);  // and its next one, before releasing this (as ATS does)
    scs::corner_releasing(XR_NULL_HANDLE);                      // hands back image 0: scaled before the runtime reads it
    {
        D3D11_TEXTURE2D_DESC dsd;
        decoyTex->GetDesc(&dsd);
        dsd.Usage = D3D11_USAGE_STAGING;
        dsd.BindFlags = 0;
        dsd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> ds;
        device->CreateTexture2D(&dsd, nullptr, &ds);
        context->CopyResource(ds.Get(), decoyTex.Get());
        D3D11_MAPPED_SUBRESOURCE dm;
        if (SUCCEEDED(context->Map(ds.Get(), 0, D3D11_MAP_READ, 0, &dm))) {
            check(static_cast<const float*>(dm.pData)[0] == -2,
                  "with two images held, the release scales the oldest (the one drawn), not the next");
            context->Unmap(ds.Get(), 0);
        } else {
            check(false, "the decoy read back");
        }
    }
    const XrFrameEndInfo* sent2 = scs::corner_frame_end(&info, &fake_enumerate);
    const auto* sentProj2 = reinterpret_cast<const XrCompositionLayerProjection*>(sent2->layers[0]);
    check(sentProj2->views[0].subImage.imageRect.extent.width == static_cast<int32_t>(W) &&
              sentProj2->views[0].subImage.imageRect.extent.height == static_cast<int32_t>(H),
          "the image scaled back at its release: submitted whole");
    context->CopyResource(staging.Get(), outTex.Get());
    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        const auto px3 = [&](UINT x, UINT y) { return reinterpret_cast<const float*>(static_cast<const char*>(m.pData) + y * m.RowPitch) + x * 4; };
        const float* farPx = px3(W - 1, H - 1);
        const float* mid = px3(W / 2, H / 2);
        std::printf("     scaled back: pixel %u,%u: %.4f %.4f; %u,%u: %.4f %.4f\n", W - 1, H - 1, farPx[0], farPx[1], W / 2, H / 2,
                    mid[0], mid[1]);
        check(close_to(farPx[0], 1, 3e-3f) && close_to(farPx[1], 1, 3e-3f) && close_to(mid[0], 0.5f, 3e-3f) && close_to(mid[1], 0.5f, 3e-3f),
              "the corner scaled back over the whole image (its far corner the corner's, its middle the corner's middle)");
        context->Unmap(staging.Get(), 0);
    } else {
        check(false, "the scaled image read back");
    }
    {  // the game's state as it was before xrEndFrame
        ID3D11RenderTargetView* boundRt = nullptr;
        context->OMGetRenderTargets(1, &boundRt, nullptr);
        ID3D11VertexShader* v = nullptr;
        context->VSGetShader(&v, nullptr, nullptr);
        check(boundRt == out.Get() && v == vs.Get(), "the game's targets and shaders bound again after the scaling (at the release)");
        if (boundRt)
            boundRt->Release();
        if (v)
            v->Release();
    }
    context->OMSetRenderTargets(1, outs, nullptr);  // frame 3's first bind: the corner ends
    boundPs = nullptr;
    context->PSGetConstantBuffers1(0, 1, &boundPs, &bpFirst, &bpNum);
    check(boundPs == bigCb.Get() && bpFirst == bigPs, "after the frame, the game's own buffer is bound again");
    if (boundPs)
        boundPs->Release();
    {  // the anomaly log: frame 2's mirrored pair read back a few frame ends later (the log is read without waiting)
        for (int f = 0; f < 60 && scs::detail::anomaly_entries() == 0; ++f) {
            context->Flush();
            Sleep(15);  // WARP finishes the copy in its own time
            scs::corner_frame_end(nullptr, &fake_enumerate);
            context->OMSetRenderTargets(1, outs, nullptr);  // the next frame's first bind: its end taken
        }
        check(scs::detail::anomaly_entries() >= 1 && scs::detail::anomaly_kinds() >= 1,
              "anomaly log: the mirrored pair of frame 2 logged and set against its draws, frames later");
    }
    check(SUCCEEDED(device->GetDeviceRemovedReason()), "the device is fine");

    // The game remakes its large constant buffers (a load, a menu: ATS 2026-11-16 filled all 16 mirror slots and left
    // 3.5 million binds unmirrored): once it lets the old ones go, the next frame mirrors the new ones.
    {
        const auto bound_is_copy = [&](ID3D11Buffer* b) {
            ID3D11Buffer* const one[] = {b};
            context->PSSetConstantBuffers1(1, 1, one, &bigPs, &num);
            ID3D11Buffer* got = nullptr;
            UINT gf = 0, gn = 0;
            context->PSGetConstantBuffers1(1, 1, &got, &gf, &gn);
            const bool copy = got && got != b;
            if (got)
                got->Release();
            return copy;
        };
        const auto make_set = [&](std::vector<ComPtr<ID3D11Buffer>>& set) {
            set.clear();
            for (int i = 0; i < 15; ++i) {  // with bigCb's, all 16 slots
                ComPtr<ID3D11Buffer> b;
                device->CreateBuffer(&bigd, nullptr, &b);
                set.push_back(b);
            }
        };
        std::vector<ComPtr<ID3D11Buffer>> set;
        make_set(set);
        context->OMSetRenderTargets(4, gbs, dsv.Get());  // a corner frame
        bool all = true;
        for (auto& b : set)
            all = bound_is_copy(b.Get()) && all;
        check(all, "fifteen large buffers bound in the corner all mirrored (the 16 slots full)");
        ID3D11Buffer* const none[] = {nullptr};
        context->PSSetConstantBuffers(1, 1, none);
        scs::corner_frame_end(&info, &fake_enumerate);
        make_set(set);  // the game's next set; the old one let go (the vector held the game's only references)
        context->OMSetRenderTargets(1, outs, nullptr);  // the next frame's first bind: its start
        context->OMSetRenderTargets(4, gbs, dsv.Get());
        all = true;
        for (auto& b : set)
            all = bound_is_copy(b.Get()) && all;
        check(all, "the game's new set mirrored on the next frame: the old set's slots freed when it let them go");
        context->PSSetConstantBuffers(1, 1, none);
        scs::corner_frame_end(&info, &fake_enumerate);
        context->OMSetRenderTargets(1, outs, nullptr);
        set.clear();
    }

    // A session restart as ATS does it (2026-11-15: three in 13 s before a freeze): the main thread ends the session and
    // starts the next; nothing is released or installed until the render thread's next xrEndFrame does both, in order.
    scs::corner_session_end();
    check(scs::corner_on(), "session end asked for: the state stays until the render thread's xrEndFrame");
    context->OMSetRenderTargets(1, outs, nullptr);
    context->RSSetViewports(1, &full);
    seenCount = 1;
    context->RSGetViewports(&seenCount, &seen);
    check(seenCount == 1 && seen.Width == W, "after the session's end, the game's viewport passes uncut");
    check(scs::corner_install(device.Get(), 50, 30), "the next session's install asked for");
    scs::corner_frame_end(nullptr, &fake_enumerate);
    check(scs::corner_on(), "the old session released and the new one installed at that xrEndFrame");
    // A session ending mid-frame from the main thread (no xrEndFrame after it: flat mode, quit): the eyes in the corner,
    // the viewport cut, our copy bound; the main thread leaves the context alone, the render thread's next call puts the
    // game's state back and observing stops.
    context->OMSetRenderTargets(4, gbs, dsv.Get());
    context->OMSetRenderTargets(1, outs, nullptr);
    context->RSSetViewports(1, &full);
    context->PSSetConstantBuffers1(0, 1, bigs, &bigPs, &num);
    seenCount = 1;
    context->RSGetViewports(&seenCount, &seen);
    boundPs = nullptr;
    context->PSGetConstantBuffers1(0, 1, &boundPs, &bpFirst, &bpNum);
    check(seen.Width == W / 2 && boundPs && boundPs != bigCb.Get(), "mid-frame: the viewport cut, our copy bound");
    if (boundPs)
        boundPs->Release();
    {
        const HANDLE main = CreateThread(nullptr, 0, &end_session_elsewhere, nullptr, 0, nullptr);
        if (main) {
            WaitForSingleObject(main, INFINITE);
            CloseHandle(main);
        }
    }
    seenCount = 1;
    context->RSGetViewports(&seenCount, &seen);
    check(seen.Width == W / 2, "session end on another thread: the context left alone there");
    context->OMSetRenderTargets(1, outs, nullptr);  // the render thread's next call
    seenCount = 1;
    context->RSGetViewports(&seenCount, &seen);
    boundPs = nullptr;
    context->PSGetConstantBuffers1(0, 1, &boundPs, &bpFirst, &bpNum);
    check(seen.Width == W && seen.Height == H && boundPs == bigCb.Get() && bpFirst == bigPs,
          "the render thread's next call puts back the game's viewport and its own constant buffer");
    if (boundPs)
        boundPs->Release();
    context->OMSetRenderTargets(4, gbs, dsv.Get());
    context->OMSetRenderTargets(1, outs, nullptr);
    context->RSSetViewports(1, &full);
    seenCount = 1;
    context->RSGetViewports(&seenCount, &seen);
    check(seen.Width == W, "then observing has stopped: a G-buffer bind starts no corner");
    scs::corner_frame_end(nullptr, &fake_enumerate);
    check(!scs::corner_on() && SUCCEEDED(device->GetDeviceRemovedReason()), "the last session's end done at its xrEndFrame");
    std::printf("     anomaly log: %llu pairs logged, %zu kinds\n", static_cast<unsigned long long>(scs::detail::anomaly_entries()),
                scs::detail::anomaly_kinds());
    scs::corner_violet_log(false);
}

}  // namespace

// The eye's shape: its targets and their halvings only, not every texture of its proportions (a near-square Pimax eye
// let small squares through, 2026-11-17).
void eye_shape_checks() {
    using scs::detail::eye_shaped;
    using scs::detail::g;
    g.eyeW = 6240, g.eyeH = 6280;  // Pimax Crystal Super, ATS at 100% Scaling
    check(eye_shaped(6240, 6280) && eye_shaped(3120, 3140) && eye_shaped(1560, 1570) && eye_shaped(780, 785) &&
              eye_shaped(390, 392) && eye_shaped(390, 393) && eye_shaped(195, 196),
          "the eye's targets and their halvings are eye-shaped");
    check(!eye_shaped(128, 128) && !eye_shaped(64, 64) && !eye_shaped(156, 157) && !eye_shaped(2272, 2288) &&
              !eye_shaped(4096, 4122),
          "small squares, twins and other sizes of the eye's proportions are not");
    g.eyeW = 5192, g.eyeH = 4168;  // ATS on another headset: the headset's images within a few texels count
    check(eye_shaped(5190, 4166) && eye_shaped(2596, 2084) && eye_shaped(649, 521) && !eye_shaped(5180, 4158) &&
              !eye_shaped(2590, 2079),
          "the headset's images within 8 texels at full size, halvings within a pixel");
    g.eyeW = g.eyeH = 0;  // as before the first session
}

namespace {

ComPtr<ID3DBlob> compile_blob(const char* src, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(src, std::strlen(src), "test", nullptr, nullptr, entry, profile, 0, 0, &code, &errors))) {
        std::printf("     compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

// kCornerPatchCs on a vertex range (flags 1 | 2, plus 8 for the position twin): the range's 4096 vectors back.
std::vector<float> run_patch(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ComputeShader* cs,
                             const std::vector<float>& range, const void* params) {
    std::vector<float> data(4096 * 4, 0.0f);
    std::memcpy(data.data(), range.data(), range.size() * 4);
    const D3D11_BUFFER_DESC raw = {4096 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0,
                                   D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    const D3D11_BUFFER_DESC pd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_BUFFER_DESC rd = {512, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_BUFFER_DESC sd = {4096 * 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    const D3D11_SUBRESOURCE_DATA init = {data.data(), 0, 0}, pInit = {params, 0, 0};
    ComPtr<ID3D11Buffer> in, out, p, rows, staging;
    if (FAILED(device->CreateBuffer(&raw, &init, &in)) || FAILED(device->CreateBuffer(&raw, nullptr, &out)) ||
        FAILED(device->CreateBuffer(&pd, &pInit, &p)) || FAILED(device->CreateBuffer(&rd, nullptr, &rows)) ||
        FAILED(device->CreateBuffer(&sd, nullptr, &staging)))
        return {};
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx = {0, 4096 * 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer = {0, 4096 * 4, D3D11_BUFFER_UAV_FLAG_RAW};
    ComPtr<ID3D11ShaderResourceView> inView;
    ComPtr<ID3D11UnorderedAccessView> outView;
    if (FAILED(device->CreateShaderResourceView(in.Get(), &sv, &inView)) ||
        FAILED(device->CreateUnorderedAccessView(out.Get(), &uv, &outView)))
        return {};
    ID3D11Buffer* const cbs[] = {p.Get(), rows.Get()};
    context->CSSetShader(cs, nullptr, 0);
    context->CSSetShaderResources(0, 1, inView.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, outView.GetAddressOf(), nullptr);
    context->CSSetConstantBuffers(0, 2, cbs);
    context->Dispatch(1, 1, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    context->CopyResource(staging.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return {};
    std::memcpy(data.data(), map.pData, data.size() * 4);
    context->Unmap(staging.Get(), 0);
    return data;
}

// Three vertices through a full-screen vertex shader: its clip position and TEXCOORD0 (stream output).
bool stream_vs(ID3D11Device* device, ID3D11DeviceContext* context, const void* code, size_t size, const float* cb0,
               const float* cb12, float (&out)[3][8]) {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> so;
    const D3D11_SO_DECLARATION_ENTRY decl[] = {{0, "SV_POSITION", 0, 0, 4, 0}, {0, "TEXCOORD", 0, 0, 4, 0}};
    UINT stride = 32;
    const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    if (FAILED(device->CreateVertexShader(code, size, nullptr, &vs)) ||
        FAILED(device->CreateGeometryShaderWithStreamOutput(code, size, decl, 2, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM,
                                                            nullptr, &so)) ||
        FAILED(device->CreateInputLayout(&element, 1, code, size, &layout)))
        return false;
    const float points[3][2] = {{0, 0}, {2, 0}, {0, 2}};
    const D3D11_SUBRESOURCE_DATA vInit = {points, 0, 0}, cInit = {cb0, 0, 0}, rInit = {cb12, 0, 0};
    const D3D11_BUFFER_DESC bd = {sizeof(points), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    const D3D11_BUFFER_DESC cd = {256, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_BUFFER_DESC od = {stride * 3, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
    const D3D11_BUFFER_DESC sd = {stride * 3, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ComPtr<ID3D11Buffer> vb, c0, c12, o, staging;
    if (FAILED(device->CreateBuffer(&bd, &vInit, &vb)) || FAILED(device->CreateBuffer(&cd, &cInit, &c0)) ||
        FAILED(device->CreateBuffer(&cd, cb12 ? &rInit : nullptr, &c12)) || FAILED(device->CreateBuffer(&od, nullptr, &o)) ||
        FAILED(device->CreateBuffer(&sd, nullptr, &staging)))
        return false;
    const UINT vbStride = 8, offset = 0;
    context->IASetInputLayout(layout.Get());
    context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &vbStride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, c0.GetAddressOf());
    context->VSSetConstantBuffers(12, 1, c12.GetAddressOf());
    context->GSSetShader(so.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->SOSetTargets(1, o.GetAddressOf(), &offset);
    context->Draw(3, 0);
    ID3D11Buffer* none = nullptr;
    context->SOSetTargets(1, &none, &offset);
    context->GSSetShader(nullptr, nullptr, 0);
    context->CopyResource(staging.Get(), o.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    std::memcpy(out, map.pData, sizeof(out));
    context->Unmap(staging.Get(), 0);
    return true;
}

bool close_to(float a, float b) { return std::fabs(a - b) <= 1e-4f * (1 + std::fabs(a)); }

}  // namespace

// The focus replay at the box's place (kCornerPatchCs flag 8 + dxbc::add_position_remap): a V1 vertex range like the
// light pass's (rectangle, flip, normalized_to_texture, viewport_size). Against the replay as before, the twin keeps
// the clip position and the texture coordinates and turns the screen pixels into the eye's under the box.
void position_checks() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &context))) {
        check(false, "position: a WARP device");
        return;
    }
    const ComPtr<ID3DBlob> csCode = compile_blob(scs::kCornerPatchCs, "main", "cs_5_0");
    const char* vsSource = R"(
cbuffer C : register(b0) { float4 rect; float4 flip; float4 ntt; float4 size; };
struct Out { float4 pos : SV_Position; float4 t : TEXCOORD0; };
Out main(float2 v : POSITION) {
    Out o;
    o.pos = float4(v.x * 2 - 1, -(v.y * 2 - 1) * flip.x, flip.z, 1);
    o.t.xy = (v * rect.zw + rect.xy) * ntt.xy;
    o.t.zw = v * size.xy;
    return o;
}
)";
    const ComPtr<ID3DBlob> vsCode = compile_blob(vsSource, "main", "vs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    if (!csCode || !vsCode || FAILED(device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, &cs))) {
        check(false, "position: the shaders compiled");
        return;
    }
    const auto* vsBytes = static_cast<const uint8_t*>(vsCode->GetBufferPointer());
    const simqvfr::dxbc::PositionRemap pr = simqvfr::dxbc::add_position_remap(vsBytes, vsCode->GetBufferSize());
    check(!pr.bytecode.empty() && pr.rectRow == 0 && pr.sizeRow == 3, "position: the twin made, rectangle row 0, size row 3");
    if (pr.bytecode.empty())
        return;
    const float W = 4040, H = 3376;
    const std::vector<float> range = {0, 0, 1, 1, 1, -0.5f, 0.5f, 0, 1, 1, 1, 1, W, H, 1, 1};
    struct {
        float fx, fy;
        uint32_t count, flags;
        float box[4], remap[4];
        float px, py;
        uint32_t rectRow, sizeRow;
    } p = {0.9f, 0.85f, 4, 3, {0.35f, 0.3f, 0.25f, 0.3f}, {1, 0, 1, 0}, 0.25f, 0.3f, ~0u, ~0u};
    const std::vector<float> before = run_patch(device.Get(), context.Get(), cs.Get(), range, &p);
    p.flags |= 8, p.rectRow = 0, p.sizeRow = 3;
    const std::vector<float> after = run_patch(device.Get(), context.Get(), cs.Get(), range, &p);
    if (before.empty() || after.empty()) {
        check(false, "position: the rules ran");
        return;
    }
    float a[3][8] = {}, b[3][8] = {};
    const float identity[8] = {1, 1, 0, 0, 1, 1, 0, 0};
    const bool ran = stream_vs(device.Get(), context.Get(), vsBytes, vsCode->GetBufferSize(), before.data(), identity, a) &&
                     stream_vs(device.Get(), context.Get(), pr.bytecode.data(), pr.bytecode.size(), after.data(),
                               after.data() + scs::detail::kPositionAt * 4, b);
    bool position = ran, texture = ran, pixels = ran;
    const float unit[3][2] = {{0, 0}, {2, 0}, {0, 2}};
    for (int v = 0; v < 3 && ran; ++v) {
        for (int c = 0; c < 4; ++c)
            position = position && close_to(a[v][c], b[v][c]);
        texture = texture && close_to(a[v][4], b[v][4]) && close_to(a[v][5], b[v][5]);
        pixels = pixels && close_to(b[v][6], (p.box[0] + unit[v][0] * p.box[2]) * W) && close_to(b[v][7], (p.box[1] + unit[v][1] * p.box[3]) * H);
    }
    check(position, "position: the twin's clip position the replay's (the same pixels drawn)");
    check(texture, "position: its texture coordinates the replay's (the twins read as before)");
    check(pixels, "position: its screen pixels the eye's under the box (light list tiles)");
    check(close_to(after[3 * 4 + 2] / after[3 * 4 + 0], before[3 * 4 + 2] / before[3 * 4 + 0]), "position: the size row's zw / xy kept");
    // The eye's own pass (no focus flag): the pair rewritten for the corner, but the size row the eye's (v * size the
    // eye's screen pixel: the periphery's light tiles), its rectangle untouched.
    {
        auto q = p;
        q.flags = 1 | 8, q.fx = q.fy = q.px = q.py = 0.4f;
        const std::vector<float> eye = run_patch(device.Get(), context.Get(), cs.Get(), range, &q);
        check(!eye.empty() && eye[12] == W && eye[13] == H && close_to(eye[14], 1) && close_to(eye[15], 1) &&
                  close_to(eye[8], 0.4f) && eye[0] == 0 && eye[2] == 1,
              "position: the eye's pass keeps the eye's size in the size row, the pair's scale the corner's");
    }
    // Not the V1 shape (a rectangle left of a flip: V2): the identity, so the twin is the game's shader.
    const std::vector<float> v2 = {1, -0.5f, 0.5f, 0, 0, 0, 1, 1, W, H, 1 / W, 1 / H, 0, 0, 0, 0};
    const std::vector<float> other = run_patch(device.Get(), context.Get(), cs.Get(), v2, &p);
    const float* id = other.empty() ? nullptr : other.data() + scs::detail::kPositionAt * 4;
    check(id && std::memcmp(id, identity, sizeof(identity)) == 0, "position: another shape gets the identity");
}

namespace {

// The mirror pass's anomaly log (corner_anomaly.cpp): with a tag, each pair it rewrites is appended (tag, index, the
// vectors around it); a (1,1,1,1) ending one 16-vector block before a size opening the next isn't a pair, nor logged;
// without a tag nothing is logged.
// The violet flash's fix: the shader analysis finds a material's colour row (cb0[6].xyz) apart from its scale row
// (cb0[7].xy); the mirror pass's tinted copy rewrites every pair's size as the plain copy does but leaves its white
// white (a tint before the viewport size), the plain copy unchanged.
void tint_checks() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &context))) {
        check(false, "tint: a WARP device");
        return;
    }
    const ComPtr<ID3DBlob> ps = compile_blob(
        "cbuffer C : register(b0) { float4 pad[6]; float4 tint; float4 vsz; };"
        "float4 main(float4 c : COLOR0, float2 uv : TEXCOORD0) : SV_Target { return float4(c.xyz * tint.xyz, 1) +"
        " float4(uv * vsz.xy, 0, 0); }",
        "main", "ps_5_0");
    uint64_t colour[64] = {}, rowsRead[64] = {};
    const bool read = ps && simqvfr::dxbc::constant_buffer_colour_rows(static_cast<const uint8_t*>(ps->GetBufferPointer()),
                                                                       ps->GetBufferSize(), 0, colour) &&
                      simqvfr::dxbc::constant_buffer_static_rows(static_cast<const uint8_t*>(ps->GetBufferPointer()),
                                                                 ps->GetBufferSize(), 0, rowsRead);
    check(read && colour[0] == (1ull << 6) && (rowsRead[0] & (3ull << 6)) == (3ull << 6),
          "tint: row 6 read as a colour (xyz), row 7 as a scale (xy): only 6 is a colour row");

    const ComPtr<ID3DBlob> code = compile_blob(scs::kCornerMirrorCs, "main", "cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    if (!code || FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) {
        check(false, "tint: the mirror shader");
        return;
    }
    constexpr UINT kVectors = 16;
    std::vector<float> in(kVectors * 4, 0.0f);
    const auto put = [&](UINT i, float x, float y, float z, float w) {
        in[i * 4] = x, in[i * 4 + 1] = y, in[i * 4 + 2] = z, in[i * 4 + 3] = w;
    };
    put(5, 0.2f, 0.3f, 0.4f, 0.5f);
    put(6, 1, 1, 1, 1);           // a material's white tint
    put(7, 1280, 1024, 1, 1);     // the scene's viewport size
    put(8, 3.7f, 4.6f, 6.1f, 0);  // its view ray
    const D3D11_BUFFER_DESC raw = {kVectors * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    const D3D11_BUFFER_DESC pd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const struct {
        float fx, fy;
        UINT count, tag;
        float w, h;
        UINT tint, p1;
        float unused[8];
    } p = {0.5f, 0.5f, kVectors, 0, 1280, 1024, 1, 0, {}};
    const D3D11_SUBRESOURCE_DATA init = {in.data(), 0, 0}, pInit = {&p, 0, 0};
    ComPtr<ID3D11Buffer> src, dst, tinted, params;
    device->CreateBuffer(&raw, &init, &src);
    device->CreateBuffer(&raw, nullptr, &dst);
    device->CreateBuffer(&raw, nullptr, &tinted);
    device->CreateBuffer(&pd, &pInit, &params);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx = {0, kVectors * 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer = {0, kVectors * 4, D3D11_BUFFER_UAV_FLAG_RAW};
    ComPtr<ID3D11ShaderResourceView> srcView;
    ComPtr<ID3D11UnorderedAccessView> dstView, tintView;
    device->CreateShaderResourceView(src.Get(), &sv, &srcView);
    device->CreateUnorderedAccessView(dst.Get(), &uv, &dstView);
    device->CreateUnorderedAccessView(tinted.Get(), &uv, &tintView);
    ID3D11UnorderedAccessView* const uavs[] = {dstView.Get(), nullptr, tintView.Get()};
    context->CSSetShader(cs.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, srcView.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    context->CSSetConstantBuffers(0, 1, params.GetAddressOf());
    context->Dispatch(1, 1, 1);
    const auto read_back = [&](ID3D11Buffer* from) {
        std::vector<float> out(kVectors * 4, 0.0f);
        const D3D11_BUFFER_DESC sd = {kVectors * 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ComPtr<ID3D11Buffer> s;
        device->CreateBuffer(&sd, nullptr, &s);
        context->CopyResource(s.Get(), from);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(context->Map(s.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            std::memcpy(out.data(), m.pData, kVectors * 16);
            context->Unmap(s.Get(), 0);
        }
        return out;
    };
    const std::vector<float> plain = read_back(dst.Get()), tint = read_back(tinted.Get());
    check(plain[6 * 4] == 0.5f && plain[6 * 4 + 2] == 2 && plain[7 * 4] == 640,
          "tint: the plain copy rewrites the pair as before");
    check(tint[6 * 4] == 1 && tint[6 * 4 + 1] == 1 && tint[6 * 4 + 2] == 1 && tint[6 * 4 + 3] == 1,
          "tint: the tinted copy leaves the white tint white (no violet)");
    check(tint[7 * 4] == 640 && tint[7 * 4 + 1] == 512 && tint[7 * 4 + 2] == 0.5f,
          "tint: the tinted copy's viewport size takes the corner's");
    check(tint[8 * 4] == 3.7f && tint[5 * 4] == 0.2f && plain[8 * 4] == 3.7f, "tint: the other rows as the game wrote them");
}

void anomaly_cs_checks() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &context))) {
        check(false, "anomaly log: a WARP device");
        return;
    }
    const ComPtr<ID3DBlob> code = compile_blob(scs::kCornerMirrorCs, "main", "cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    if (!code || FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) {
        check(false, "anomaly log: the mirror shader");
        return;
    }
    constexpr UINT kVectors = 32;
    std::vector<float> in(kVectors * 4, 0.0f);
    const auto put = [&](UINT i, float x, float y, float z, float w) {
        in[i * 4] = x, in[i * 4 + 1] = y, in[i * 4 + 2] = z, in[i * 4 + 3] = w;
    };
    put(0, 5, 6, 7, 8);
    put(1, 1, 1, 1, 1);
    put(2, 1280, 1024, 1, 1);
    put(3, 9, 9, 9, 9);
    put(15, 1, 1, 1, 1);  // a white closing a block
    put(16, 1280, 1024, 1, 1);  // a size opening the next: not a pair
    const auto run = [&](uint32_t tag, std::vector<float>& out, std::vector<uint32_t>& log) {
        const D3D11_BUFFER_DESC raw = {kVectors * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                       0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        const D3D11_BUFFER_DESC logd = {16 + 4 * 80, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        const D3D11_BUFFER_DESC pd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const struct {
            float fx, fy;
            UINT count, tag;
            float w, h, p0, p1;
            float unused[8];
        } p = {0.5f, 0.5f, kVectors, tag, 1280, 1024, 0, 0, {}};
        const D3D11_SUBRESOURCE_DATA init = {in.data(), 0, 0}, pInit = {&p, 0, 0};
        ComPtr<ID3D11Buffer> src, dst, logBuf, params, stage;
        device->CreateBuffer(&raw, &init, &src);
        device->CreateBuffer(&raw, nullptr, &dst);
        device->CreateBuffer(&logd, nullptr, &logBuf);
        device->CreateBuffer(&pd, &pInit, &params);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx = {0, kVectors * 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uv.Buffer = {0, kVectors * 4, D3D11_BUFFER_UAV_FLAG_RAW};
        D3D11_UNORDERED_ACCESS_VIEW_DESC lv = uv;
        lv.Buffer = {0, logd.ByteWidth / 4, D3D11_BUFFER_UAV_FLAG_RAW};
        ComPtr<ID3D11ShaderResourceView> srcView;
        ComPtr<ID3D11UnorderedAccessView> dstView, logView;
        device->CreateShaderResourceView(src.Get(), &sv, &srcView);
        device->CreateUnorderedAccessView(dst.Get(), &uv, &dstView);
        device->CreateUnorderedAccessView(logBuf.Get(), &lv, &logView);
        const UINT zero[4] = {};
        context->ClearUnorderedAccessViewUint(logView.Get(), zero);
        ID3D11UnorderedAccessView* const uavs[] = {dstView.Get(), logView.Get()};
        context->CSSetShader(cs.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, srcView.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        context->CSSetConstantBuffers(0, 1, params.GetAddressOf());
        context->Dispatch(1, 1, 1);
        const auto read = [&](ID3D11Buffer* b, UINT bytes, void* to) {
            const D3D11_BUFFER_DESC sd = {bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            ComPtr<ID3D11Buffer> s;
            device->CreateBuffer(&sd, nullptr, &s);
            context->CopyResource(s.Get(), b);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(context->Map(s.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                std::memcpy(to, m.pData, bytes);
                context->Unmap(s.Get(), 0);
            }
        };
        out.assign(kVectors * 4, 0.0f);
        log.assign((16 + 4 * 80) / 4, 0u);
        read(dst.Get(), kVectors * 16, out.data());
        read(logBuf.Get(), 16 + 4 * 80, log.data());
    };
    std::vector<float> out;
    std::vector<uint32_t> log;
    run(0x85000007u, out, log);
    float v[4][4];
    std::memcpy(v, log.data() + 4 + 4, sizeof(v));
    check(log[0] == 1 && log[4] == 0x85000007u && log[5] == 1, "anomaly log: the one pair logged with its tag and index");
    check(v[0][0] == 5 && v[1][0] == 1 && v[1][3] == 1 && v[2][0] == 1280 && v[3][0] == 9,
          "anomaly log: the vectors before, at, after and past it, as the game wrote them");
    check(out[4] == 0.5f && out[6] == 2 && out[8] == 640 && out[15 * 4] == 1 && out[16 * 4] == 1280,
          "anomaly log: the pair rewritten as before, the white closing a block left alone");
    run(0, out, log);
    check(log[0] == 0 && out[4] == 0.5f, "anomaly log: without a tag nothing logged, the pair still rewritten");
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    eye_shape_checks();
    rules_checks();
    warp_checks();
    position_checks();
    anomaly_cs_checks();
    tint_checks();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

// Offline check of LMU's multi-view rendering in the eye passes (games/lmu/mvr_pass.h) on a WARP device, with a stand-in
// for NVAPI's single-pass stereo switch: inside a pass that draws its focus view, a shader with a twin and a depth
// target bound gets the twin, two views and a second viewport (and scissor) beside the game's, scaled to the focus
// view; anything else, and every pass end, the game's own state again. And the comparison of two HDR images.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "games/lmu/mvr_pass.h"
#include "games/lmu/mvr_probe.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

int twoViewsOn = 0, twoViewsCalls = 0;
int fake_two_views(ID3D11DeviceContext*, bool on) {
    ++twoViewsCalls;
    twoViewsOn = on ? 1 : 0;
    return 0;
}
int refusing_two_views(ID3D11DeviceContext*, bool on) { return on ? -1 : 0; }

ComPtr<ID3D11VertexShader> vertex_shader(ID3D11Device* device, const char* scale) {
    const std::string source = std::string("float4 main(float4 p : POSITION) : SV_Position { return p * ") + scale + "; }";
    ComPtr<ID3DBlob> code, errors;
    ComPtr<ID3D11VertexShader> vs;
    if (SUCCEEDED(D3DCompile(source.data(), source.size(), "t", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &errors)))
        device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
    return vs;
}

ID3D11VertexShader* bound_vs(ID3D11DeviceContext* context) {
    ID3D11VertexShader* vs = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    if (vs)
        vs->Release();
    return vs;
}

UINT viewports(ID3D11DeviceContext* context, D3D11_VIEWPORT (&vps)[16]) {
    UINT n = 16;
    context->RSGetViewports(&n, vps);
    return n;
}

bool same(const D3D11_VIEWPORT& v, float x, float y, float w, float h) {
    return std::fabs(v.TopLeftX - x) < 1e-3f && std::fabs(v.TopLeftY - y) < 1e-3f && std::fabs(v.Width - w) < 1e-3f &&
           std::fabs(v.Height - h) < 1e-3f;
}

uint16_t to_half(float f) {  // positive normal values only
    if (f <= 0)
        return 0;
    int e;
    const float m = std::frexp(f, &e);  // f = m * 2^e, m in [0.5, 1)
    const int exponent = e - 1 + 15;
    const auto mantissa = static_cast<uint16_t>(std::lround((m * 2 - 1) * 1024)) & 0x3ff;
    return static_cast<uint16_t>((exponent << 10) | mantissa);
}

std::vector<uint16_t> image(unsigned w, unsigned h, int shiftX, int shiftY, float gain) {
    std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const double sx = static_cast<double>(x) - shiftX, sy = static_cast<double>(y) - shiftY;
            const float v = gain * static_cast<float>(0.3 + 0.2 * std::sin(sx * 0.21) * std::cos(sy * 0.17) +
                                                      0.1 * std::sin((sx + sy) * 0.05));
            for (int c = 0; c < 3; ++c)
                px[(static_cast<size_t>(y) * w + x) * 4 + c] = to_half(v * (0.8f + 0.1f * static_cast<float>(c)));
            px[(static_cast<size_t>(y) * w + x) * 4 + 3] = to_half(1.0f);
        }
    return px;
}

}  // namespace

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    lmu::mvr_set_two_views(&fake_two_views);
    check(lmu::mvr_pass_install(device.Get()), "the immediate context hooked");

    ComPtr<ID3D11VertexShader> a = vertex_shader(device.Get(), "1"), twin = vertex_shader(device.Get(), "2"),
                               b = vertex_shader(device.Get(), "3");
    a->SetPrivateDataInterface(lmu::kMvrTwinGuid, twin.Get());
    D3D11_TEXTURE2D_DESC td = {256, 128, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    ComPtr<ID3D11Texture2D> colour, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    device->CreateTexture2D(&td, nullptr, &colour);
    device->CreateRenderTargetView(colour.Get(), nullptr, &rtv);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    device->CreateTexture2D(&td, nullptr, &depth);
    device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);

    const D3D11_VIEWPORT eye = {0, 0, 100, 80, 0, 1};
    context->RSSetViewports(1, &eye);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    context->VSSetShader(a.Get(), nullptr, 0);
    D3D11_VIEWPORT vps[16];
    check(bound_vs(context.Get()) == a.Get() && viewports(context.Get(), vps) == 1, "outside a pass: the game's state as set");

    lmu::MvrPassView view;
    view.eye = 0;
    view.eyeW = 100;
    view.eyeH = 80;
    view.focusW = 60;
    view.focusH = 50;
    lmu::mvr_pass_begin(0, &view, false);
    UINT n = viewports(context.Get(), vps);
    check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1 && n == 2 && same(vps[0], 0, 0, 100, 80) &&
              same(vps[1], 100, 0, 60, 50),
          "pass 0 begins on a depth target: the twin, two views, the focus viewport beside the eye (100,0 60x50)");
    ID3D11Buffer* cb = nullptr;
    context->VSGetConstantBuffers(13, 1, &cb);
    check(cb != nullptr, "the remap bound at cb13");
    if (cb)
        cb->Release();

    context->VSSetShader(b.Get(), nullptr, 0);
    check(bound_vs(context.Get()) == b.Get() && twoViewsOn == 0 && viewports(context.Get(), vps) == 1,
          "a shader without a twin: itself, one view, one viewport");
    context->VSSetShader(a.Get(), nullptr, 0);
    check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1, "a shader with a twin again: the twin");
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    check(bound_vs(context.Get()) == a.Get() && twoViewsOn == 0 && viewports(context.Get(), vps) == 1,
          "no depth target (post-processing): the game's shader, one view");
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    const D3D11_VIEWPORT quarter = {0, 0, 25, 20, 0, 1};
    context->RSSetViewports(1, &quarter);
    n = viewports(context.Get(), vps);
    check(n == 2 && same(vps[1], 25, 0, 15, 12.5f), "a quarter-size pass: its focus viewport at quarter size (25,0 15x12.5)");
    const D3D11_RECT scissor = {0, 0, 25, 20};
    context->RSSetScissorRects(1, &scissor);
    D3D11_RECT rects[16];
    UINT rn = 16;
    context->RSGetScissorRects(&rn, rects);
    check(rn == 2 && rects[1].left == 25 && rects[1].top == 0 && rects[1].right == 40 && rects[1].bottom == 13,
          "its scissor mapped onto the focus viewport (25,0 - 40,13)");
    {  // a geometry shader (here the vertex shader's own pass-through, as stream output would make it): no two views
        ComPtr<ID3DBlob> code, errors;
        const char* gsSource = "struct V { float4 p : SV_Position; }; [maxvertexcount(3)] void main(triangle V v[3], "
                               "inout TriangleStream<V> s) { s.Append(v[0]); s.Append(v[1]); s.Append(v[2]); }";
        ComPtr<ID3D11GeometryShader> gs;
        if (SUCCEEDED(D3DCompile(gsSource, std::strlen(gsSource), "g", nullptr, nullptr, "main", "gs_5_0", 0, 0, &code,
                                 &errors)))
            device->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gs);
        context->GSSetShader(gs.Get(), nullptr, 0);
        check(gs && bound_vs(context.Get()) == a.Get() && twoViewsOn == 0,
              "a geometry shader bound: the game's shader, one view");
        context->GSSetShader(nullptr, nullptr, 0);
        check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1, "the geometry shader unbound: the twin again");
        D3D11_BUFFER_DESC sd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
        ComPtr<ID3D11Buffer> so;
        device->CreateBuffer(&sd, nullptr, &so);
        const UINT offset = 0;
        context->SOSetTargets(1, so.GetAddressOf(), &offset);
        check(bound_vs(context.Get()) == a.Get() && twoViewsOn == 0, "stream output bound: the game's shader, one view");
        ID3D11Buffer* none = nullptr;
        context->SOSetTargets(1, &none, &offset);
        check(bound_vs(context.Get()) == twin.Get(), "stream output unbound: the twin again");
        // A multisample resolve in the pass is seen (the region a sample copies).
        D3D11_TEXTURE2D_DESC md = {64, 64, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {4, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ComPtr<ID3D11Texture2D> ms, single;
        device->CreateTexture2D(&md, nullptr, &ms);
        md.SampleDesc.Count = 1;
        device->CreateTexture2D(&md, nullptr, &single);
        const uint64_t before = lmu::mvr_pass_stats().resolves;
        context->ResolveSubresource(single.Get(), 0, ms.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        check(ms && single && lmu::mvr_pass_stats().resolves == before + 1, "a multisample resolve in the pass is seen");
    }
    lmu::mvr_suspend(true);
    check(bound_vs(context.Get()) == a.Get() && twoViewsOn == 0 && viewports(context.Get(), vps) == 1,
          "suspended (the lens mask): the eye only");
    lmu::mvr_suspend(false);
    check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1 && viewports(context.Get(), vps) == 2,
          "resumed: the twin and two viewports again");
    lmu::mvr_pass_end(0);
    n = viewports(context.Get(), vps);
    rn = 16;
    context->RSGetScissorRects(&rn, rects);
    check(bound_vs(context.Get()) == a.Get() && twoViewsOn == 0 && n == 1 && same(vps[0], 0, 0, 25, 20) && rn == 1,
          "pass end: the game's shader, one view, its viewport and scissor");

    lmu::mvr_pass_begin(2, nullptr, false);
    context->VSSetShader(a.Get(), nullptr, 0);
    context->RSSetViewports(1, &eye);
    check(bound_vs(context.Get()) == a.Get() && twoViewsOn == 0 && viewports(context.Get(), vps) == 1,
          "pass 2 (renders the focus view itself): the game's state");
    lmu::mvr_pass_end(2);

    {  // The twin lookup: once per shader (the cache, across frames); a twin attached since seen once announced.
        lmu::mvr_pass_begin(0, &view, false);
        const lmu::MvrPassStats s0 = lmu::mvr_pass_stats();
        for (int i = 0; i < 5; ++i) {
            context->VSSetShader(b.Get(), nullptr, 0);
            context->VSSetShader(a.Get(), nullptr, 0);
        }
        const lmu::MvrPassStats s1 = lmu::mvr_pass_stats();
        check(s1.lookups - s0.lookups == 10 && s1.lookupMisses - s0.lookupMisses <= 2 && bound_vs(context.Get()) == twin.Get(),
              "ten binds of two shaders in a frame: at most two lookups not cached, the twin bound");
        lmu::mvr_pass_end(0);
        lmu::mvr_pass_begin(0, &view, false);  // the next frame (its start looks up the bound shader): still cached
        context->VSSetShader(b.Get(), nullptr, 0);
        context->VSSetShader(a.Get(), nullptr, 0);
        const lmu::MvrPassStats s2 = lmu::mvr_pass_stats();
        check(s2.lookups - s1.lookups == 3 && s2.lookupMisses == s1.lookupMisses && bound_vs(context.Get()) == twin.Get(),
              "the next frame: every lookup answered by the cache");
        lmu::mvr_pass_end(0);
        ComPtr<ID3D11VertexShader> twinB = vertex_shader(device.Get(), "4");
        b->SetPrivateDataInterface(lmu::kMvrTwinGuid, twinB.Get());
        lmu::mvr_twins_changed();
        lmu::mvr_pass_begin(0, &view, false);
        context->VSSetShader(b.Get(), nullptr, 0);
        check(bound_vs(context.Get()) == twinB.Get() && twoViewsOn == 1, "a twin attached and announced: b's twin bound");
        lmu::mvr_pass_end(0);
        b->SetPrivateDataInterface(lmu::kMvrTwinGuid, nullptr);
        lmu::mvr_twins_changed();
    }

    {  // Gate D: pass 0's resolve keeps its focus region; pass 2's resolve becomes a copy of it into the top-left.
        lmu::mvr_keep_focus(true);
        D3D11_TEXTURE2D_DESC md = {256, 128, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {4, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ComPtr<ID3D11Texture2D> ms, resolved, staging;
        ComPtr<ID3D11RenderTargetView> msRtv;
        device->CreateTexture2D(&md, nullptr, &ms);
        device->CreateRenderTargetView(ms.Get(), nullptr, &msRtv);
        md.SampleDesc.Count = 1;
        device->CreateTexture2D(&md, nullptr, &resolved);
        md.Usage = D3D11_USAGE_STAGING;
        md.BindFlags = 0;
        md.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&md, nullptr, &staging);
        const auto pixel = [&](unsigned x, unsigned y) {  // the resolved texture's red, as a half
            context->CopyResource(staging.Get(), resolved.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            uint16_t red = 0;
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                std::memcpy(&red, static_cast<const uint8_t*>(map.pData) + y * map.RowPitch + x * 8, 2);
                context->Unmap(staging.Get(), 0);
            }
            return red;
        };
        const float first[4] = {0.5f, 0, 0, 1}, second[4] = {0.25f, 0, 0, 1};
        lmu::mvr_set_two_views(&fake_two_views);
        context->RSSetViewports(1, &eye);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        context->VSSetShader(a.Get(), nullptr, 0);
        lmu::mvr_pass_begin(0, &view, false);  // draws its focus view (the twin bound)
        context->ClearRenderTargetView(msRtv.Get(), first);
        context->ResolveSubresource(resolved.Get(), 0, ms.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        lmu::mvr_pass_end(0);
        check(lmu::mvr_focus_ready(0) && !lmu::mvr_focus_ready(1), "gate D: pass 0's focus region kept (eye 0 only)");
        lmu::mvr_pass_begin(2, nullptr, false, true);
        context->ClearRenderTargetView(msRtv.Get(), second);
        context->ResolveSubresource(resolved.Get(), 0, ms.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        lmu::mvr_pass_end(2);
        // The focus region (100..160 x 0..50 of pass 0's) at the top-left (0..60 x 0..50); the rest cleared to black.
        check(pixel(10, 10) == to_half(0.5f) && pixel(59, 49) == to_half(0.5f) && pixel(60, 10) == 0 &&
                  pixel(200, 100) == 0 && lmu::mvr_pass_stats().replacedResolves == 1,
              "gate D: pass 2's resolve replaced by the kept focus region at the top-left, the rest black");
        lmu::mvr_pass_begin(3, nullptr, false, true);  // eye 1's region wasn't kept: a real resolve
        context->ClearRenderTargetView(msRtv.Get(), second);
        context->ResolveSubresource(resolved.Get(), 0, ms.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        lmu::mvr_pass_end(3);
        check(pixel(10, 10) == to_half(0.25f) && lmu::mvr_pass_stats().replacedResolves == 1,
              "gate D: no region kept for eye 1: pass 3 resolves as the game asked");
        lmu::mvr_keep_focus(false);
    }

    lmu::mvr_set_two_views(&refusing_two_views);
    lmu::mvr_pass_begin(1, &view, false);
    const lmu::MvrPassStats s = lmu::mvr_pass_stats();
    check(bound_vs(context.Get()) == a.Get() && viewports(context.Get(), vps) == 1 && s.stereoErrors == 1,
          "single-pass stereo refused: the game's state, off for the session");
    lmu::mvr_pass_end(1);

    const unsigned w = 160, h = 120;
    const std::vector<uint16_t> base = image(w, h, 0, 0, 1.0f);
    const lmu::MvrComparison same0 = lmu::mvr_compare(base, base, w, h);
    check(same0.ok && same0.shiftX == 0 && same0.shiftY == 0 && same0.meanDiff < 1e-6 &&
              std::fabs(same0.brightness - 1) < 1e-6 && same0.differingPct == 0,
          "compare: an image with itself: aligned, brightness 1, nothing differs");
    const lmu::MvrComparison moved = lmu::mvr_compare(base, image(w, h, 2, -1, 1.0f), w, h);
    char what[160];
    std::snprintf(what, sizeof(what), "compare: moved by (2, -1): found at %+d,%+d, mean difference %.4f", moved.shiftX,
                  moved.shiftY, moved.meanDiff);
    check(moved.ok && moved.shiftX == 2 && moved.shiftY == -1 && moved.meanDiff < 0.002, what);
    const lmu::MvrComparison bright = lmu::mvr_compare(base, image(w, h, 0, 0, 1.1f), w, h);
    std::snprintf(what, sizeof(what), "compare: 10%% brighter: brightness %.4f, cell median %.4f", bright.brightness,
                  bright.cellMedian);
    check(bright.ok && std::fabs(bright.brightness - 1.1) < 0.005 && std::fabs(bright.cellMedian - 1.1) < 0.005, what);

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

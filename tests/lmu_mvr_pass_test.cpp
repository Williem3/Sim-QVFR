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

int SIM_QVFR_TEST_MAIN() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    lmu::mvr_set_two_views(&fake_two_views);
    // The hooks are on every function the context's twelve entries hold (with multithread protection off and on, idle
    // and busy), in d3d11's code: ten, PSSetShader's with texture sharpening possible (mvr_mip_bias_hooks, before the
    // install) and ExecuteCommandList's (the game's command lists); DrawIndexed's only with the focus post
    // (lmu_focus_post_test). The context's vtable pointer as it was (its entries are d3d11's to rewrite).
    lmu::mvr_mip_bias_hooks(true);
    void** const ownVtable = *reinterpret_cast<void***>(context.Get());
    check(lmu::mvr_pass_install(device.Get()), "the immediate context hooked");
    check(*reinterpret_cast<void***>(context.Get()) == ownVtable, "the context's vtable pointer as it was");
    char what[200];
    const lmu::MvrPassStats installed = lmu::mvr_pass_stats();
    std::snprintf(what, sizeof(what),
                  "%llu functions hooked for %llu entries (twelve with PSSetShader and ExecuteCommandList: no DrawIndexed "
                  "without the focus post; two each at least: protection off and on), %llu of them ResolveSubresource's",
                  static_cast<unsigned long long>(installed.contextFunctions),
                  static_cast<unsigned long long>(installed.contextEntries),
                  static_cast<unsigned long long>(installed.resolveFunctions));
    check(installed.contextEntries == 12 && installed.contextFunctions >= 24 && installed.resolveFunctions >= 2, what);
    ComPtr<ID3D11DeviceContext4> context4;
    check(SUCCEEDED(context.As(&context4)) && static_cast<ID3D11DeviceContext*>(context4.Get()) == context.Get(),
          "ID3D11DeviceContext4 is the same pointer: calls through it reach the hooks too");

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
    {  // The game's command list (LMU's sun shadow casters: a deferred context's, its own shader, depth target, viewport)
       // executed in the pass with the two views on: executed with them off, its calls (WARP replays a list through the
       // immediate context's own functions) not taken for the game's, the state read again after it.
        ComPtr<ID3D11DeviceContext> deferred;
        check(SUCCEEDED(device->CreateDeferredContext(0, &deferred)), "a deferred context");
        const D3D11_VIEWPORT tile = {0, 0, 64, 64, 0, 1};
        auto record = [&] {
            ComPtr<ID3D11CommandList> list;
            deferred->OMSetRenderTargets(0, nullptr, dsv.Get());
            deferred->RSSetViewports(1, &tile);
            deferred->VSSetShader(a.Get(), nullptr, 0);
            deferred->FinishCommandList(FALSE, &list);
            return list;
        };
        context->RSSetViewports(1, &eye);
        check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1 && viewports(context.Get(), vps) == 2,
              "before the list: the twin, two views");
        const lmu::MvrPassStats s0 = lmu::mvr_pass_stats();
        int calls = twoViewsCalls;
        ComPtr<ID3D11CommandList> list = record();
        context->ExecuteCommandList(list.Get(), FALSE);
        const lmu::MvrPassStats s1 = lmu::mvr_pass_stats();
        std::snprintf(what, sizeof(what),
                      "a list executed (state not kept): the two views switched off once (%d calls) and left off, the "
                      "context cleared (no shader, %u viewports); counted in the pass with the views on (%llu hooked calls "
                      "while it executed)",
                      twoViewsCalls - calls, viewports(context.Get(), vps),
                      static_cast<unsigned long long>(s1.callsInLists - s0.callsInLists));
        check(twoViewsCalls == calls + 1 && twoViewsOn == 0 && bound_vs(context.Get()) == nullptr &&
                  viewports(context.Get(), vps) == 0 && s1.listsInPass == s0.listsInPass + 1 &&
                  s1.listsTwoViews == s0.listsTwoViews + 1,
              what);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        context->RSSetViewports(1, &eye);
        context->VSSetShader(a.Get(), nullptr, 0);
        n = viewports(context.Get(), vps);
        ID3D11Buffer* remap = nullptr;
        context->VSGetConstantBuffers(13, 1, &remap);
        check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1 && n == 2 && same(vps[1], 100, 0, 60, 50) &&
                  remap != nullptr,
              "the game's state set again after it: the twin, two views, the focus viewport and the remap at cb13");
        if (remap)
            remap->Release();
        calls = twoViewsCalls;
        list = record();
        context->ExecuteCommandList(list.Get(), TRUE);
        check(twoViewsCalls == calls + 2 && twoViewsOn == 1 && bound_vs(context.Get()) == twin.Get() &&
                  viewports(context.Get(), vps) == 2,
              "a list executed keeping the state: the two views off around it, the twin and both viewports after it");
        lmu::mvr_replay_lists_plain(false);  // the A/B's other side: as before the fix
        list = record();
        context->ExecuteCommandList(list.Get(), TRUE);
        check(lmu::mvr_pass_stats().listsInPass == s1.listsInPass + 2,
              "the fix switched off (lmu_diag switch 2): the list still executed and counted");
        lmu::mvr_replay_lists_plain(true);
        // As before the fix the list's calls went to the hooks as the game's: their copy of the state is stale. The
        // game's state set again, as the checks below expect it.
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        context->VSSetShader(a.Get(), nullptr, 0);
        context->RSSetViewports(1, &quarter);
        context->RSSetScissorRects(1, &scissor);
    }
    lmu::mvr_pass_end(0);
    n = viewports(context.Get(), vps);
    rn = 16;
    context->RSGetScissorRects(&rn, rects);
    std::snprintf(what, sizeof(what), "pass end: the game's shader (%s), one view (%d), its viewport (%u: %.0f,%.0f %.0fx%.0f) and "
                  "scissor (%u)", bound_vs(context.Get()) == a.Get() ? "yes" : "no", twoViewsOn, n, vps[0].TopLeftX,
                  vps[0].TopLeftY, vps[0].Width, vps[0].Height, rn);
    check(bound_vs(context.Get()) == a.Get() && twoViewsOn == 0 && n == 1 && same(vps[0], 0, 0, 25, 20) && rn == 1, what);

    lmu::mvr_pass_begin(2, nullptr, false);
    context->VSSetShader(a.Get(), nullptr, 0);
    context->RSSetViewports(1, &eye);
    check(bound_vs(context.Get()) == a.Get() && twoViewsOn == 0 && viewports(context.Get(), vps) == 1,
          "pass 2 (renders the focus view itself): the game's state");
    lmu::mvr_pass_end(2);

    {  // DLSS super resolution on the focus view (MvrPassView::focusRenderW): the scene draws it at its render size in its
       // corner; once the pass's post begins (its resolve) the focus viewport is its region again, as the post reads it.
        lmu::MvrPassView reduced = view;
        reduced.focusRenderW = 30;
        reduced.focusRenderH = 25;
        context->VSSetShader(a.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        context->RSSetViewports(1, &eye);
        lmu::mvr_pass_begin(0, &reduced, false);
        n = viewports(context.Get(), vps);
        check(bound_vs(context.Get()) == twin.Get() && n == 2 && same(vps[0], 0, 0, 100, 80) && same(vps[1], 100, 0, 30, 25),
              "super resolution: the scene's focus viewport at the render size (100,0 30x25)");
        const D3D11_RECT whole = {0, 0, 100, 80};
        context->RSSetScissorRects(1, &whole);
        rn = 16;
        context->RSGetScissorRects(&rn, rects);
        check(rn == 2 && rects[1].left == 100 && rects[1].top == 0 && rects[1].right == 130 && rects[1].bottom == 25,
              "super resolution: the scissor mapped onto it (100,0 - 130,25)");
        D3D11_TEXTURE2D_DESC md = {64, 64, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {4, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ComPtr<ID3D11Texture2D> ms, single;
        device->CreateTexture2D(&md, nullptr, &ms);
        md.SampleDesc.Count = 1;
        device->CreateTexture2D(&md, nullptr, &single);
        context->ResolveSubresource(single.Get(), 0, ms.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        context->RSSetViewports(1, &eye);
        n = viewports(context.Get(), vps);
        check(ms && single && n == 2 && same(vps[1], 100, 0, 60, 50),
              "super resolution: after the resolve (the post) the focus viewport at its size (100,0 60x50)");
        lmu::mvr_pass_end(0);
        context->RSSetScissorRects(0, nullptr);
    }

    {  // Multithread protection on: d3d11 puts other functions in the context's entries; the hooks are on those too.
        ComPtr<ID3D11Multithread> mt;
        check(SUCCEEDED(context.As(&mt)), "the context's multithread protection");
        void* const plainEntry = ownVtable[lmu::context_slot::kRSSetViewports];
        const BOOL was = mt ? mt->SetMultithreadProtected(TRUE) : FALSE;
        check(mt && ownVtable[lmu::context_slot::kRSSetViewports] != plainEntry,
              "protection on: d3d11 rewrote the context's entries");
        for (int round = 0; round < 2; ++round) {  // protection on, then off again
            const uint64_t functions = lmu::mvr_pass_stats().contextFunctions;
            context->RSSetViewports(1, &eye);
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
            context->VSSetShader(b.Get(), nullptr, 0);
            lmu::mvr_pass_begin(0, &view, false);
            context->VSSetShader(a.Get(), nullptr, 0);
            n = viewports(context.Get(), vps);
            const bool twinned = bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1 && n == 2 &&
                                 same(vps[1], 100, 0, 60, 50);
            context->RSSetViewports(1, &quarter);
            n = viewports(context.Get(), vps);
            const bool quartered = n == 2 && same(vps[1], 25, 0, 15, 12.5f);
            context->VSSetShader(b.Get(), nullptr, 0);
            const bool plain = bound_vs(context.Get()) == b.Get() && twoViewsOn == 0;
            lmu::mvr_pass_end(0);
            std::snprintf(what, sizeof(what),
                          "protection %s: the game's shader and viewport calls still seen (twin and focus viewport %d, "
                          "quarter size %d, a shader without a twin %d); no function new since install",
                          round == 0 ? "on" : "off again", twinned, quartered, plain);
            check(twinned && quartered && plain && lmu::mvr_pass_stats().contextFunctions == functions, what);
            if (mt && round == 0)
                mt->SetMultithreadProtected(was);
        }
    }

    {  // The twin lookup: once per shader (the cache, across frames); a twin attached since seen once announced.
        lmu::mvr_pass_begin(0, &view, false);
        const lmu::MvrPassStats s0 = lmu::mvr_pass_stats();
        for (int i = 0; i < 5; ++i) {
            context->VSSetShader(b.Get(), nullptr, 0);
            context->VSSetShader(a.Get(), nullptr, 0);
        }
        const lmu::MvrPassStats s1 = lmu::mvr_pass_stats();
        // Two lookups per bind (its role, R6, then its twin), one entry per shader.
        check(s1.lookups - s0.lookups == 20 && s1.lookupMisses - s0.lookupMisses <= 2 && bound_vs(context.Get()) == twin.Get(),
              "ten binds of two shaders in a frame: at most two lookups not cached, the twin bound");
        lmu::mvr_pass_end(0);
        lmu::mvr_pass_begin(0, &view, false);  // the next frame (its start looks up the bound shader): still cached
        context->VSSetShader(b.Get(), nullptr, 0);
        context->VSSetShader(a.Get(), nullptr, 0);
        const lmu::MvrPassStats s2 = lmu::mvr_pass_stats();
        check(s2.lookups - s1.lookups == 6 && s2.lookupMisses == s1.lookupMisses && bound_vs(context.Get()) == twin.Get(),
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

    {  // Texture sharpening: the game's pixel shader swapped for its mip bias twin exactly while two views are on in a
       // pass with a bias; the focus viewport and the two biases in pixel shader buffer 13.
        const auto pixel_shader = [&](const char* value) {
            const std::string source = std::string("float4 main() : SV_Target { return ") + value + "; }";
            ComPtr<ID3DBlob> code, errors;
            ComPtr<ID3D11PixelShader> ps;
            if (SUCCEEDED(D3DCompile(source.data(), source.size(), "p", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code,
                                     &errors)))
                device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
            return ps;
        };
        const auto bound_ps = [&] {
            ID3D11PixelShader* ps = nullptr;
            context->PSGetShader(&ps, nullptr, nullptr);
            if (ps)
                ps->Release();
            return ps;
        };
        ComPtr<ID3D11PixelShader> gamePs = pixel_shader("1"), psTwin = pixel_shader("2"), plainPs = pixel_shader("3");
        gamePs->SetPrivateDataInterface(lmu::kMvrMipBiasTwinGuid, psTwin.Get());
        context->RSSetViewports(1, &eye);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        context->VSSetShader(a.Get(), nullptr, 0);
        context->PSSetShader(gamePs.Get(), nullptr, 0);
        check(bound_ps() == gamePs.Get(), "texture sharpening: outside a pass, the game's pixel shader");
        lmu::MvrPassView biased = view;
        biased.mipBias[0] = -1.0f;   // the eye's
        biased.mipBias[1] = -1.58f;  // the focus view's
        lmu::mvr_pass_begin(0, &biased, false);
        D3D11_BUFFER_DESC sd = {32, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ComPtr<ID3D11Buffer> staging;
        device->CreateBuffer(&sd, nullptr, &staging);
        float constants[8] = {};
        ID3D11Buffer* bcb = nullptr;
        context->PSGetConstantBuffers(13, 1, &bcb);
        if (bcb && staging) {
            context->CopyResource(staging.Get(), bcb);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                std::memcpy(constants, map.pData, sizeof(constants));
                context->Unmap(staging.Get(), 0);
            }
        }
        if (bcb)
            bcb->Release();
        check(bound_ps() == psTwin.Get() && twoViewsOn == 1, "texture sharpening: a pass with a bias, two views: the twin");
        std::snprintf(what, sizeof(what), "texture sharpening: buffer 13 = the focus viewport (%.0f,%.0f - %.0f,%.0f), focus "
                      "bias %.2f, eye bias %.2f", constants[0], constants[1], constants[2], constants[3], constants[4],
                      constants[5]);
        check(bcb && constants[0] == 100 && constants[1] == 0 && constants[2] == 160 && constants[3] == 50 &&
                  std::fabs(constants[4] + 1.58f) < 1e-6f && constants[5] == -1.0f,
              what);
        context->PSSetShader(plainPs.Get(), nullptr, 0);
        check(bound_ps() == plainPs.Get(), "texture sharpening: a pixel shader without a twin: itself");
        context->PSSetShader(gamePs.Get(), nullptr, 0);
        check(bound_ps() == psTwin.Get(), "texture sharpening: the game's again: its twin");
        context->VSSetShader(b.Get(), nullptr, 0);
        check(bound_ps() == gamePs.Get() && twoViewsOn == 0, "texture sharpening: one view (no vertex twin): the game's pixel shader");
        context->VSSetShader(a.Get(), nullptr, 0);
        check(bound_ps() == psTwin.Get(), "texture sharpening: two views again: the twin");
        // The game unbinds all 14 pixel shader buffers (LMU does, once early in each eye pass): the bias buffer is bound
        // again as two views go on, and within 16 twin binds while they stay on.
        const auto bias_cb = [&] {
            ID3D11Buffer* b = nullptr;
            context->PSGetConstantBuffers(13, 1, &b);
            if (b)
                b->Release();
            return b;
        };
        ID3D11Buffer* const ours = bias_cb();
        ID3D11Buffer* const none[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        context->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, none);
        check(ours && bias_cb() == nullptr, "texture sharpening: the game unbinds all 14 pixel shader buffers");
        context->VSSetShader(b.Get(), nullptr, 0);
        context->VSSetShader(a.Get(), nullptr, 0);
        check(bias_cb() == ours, "texture sharpening: bound again as two views go on");
        context->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, none);
        const uint64_t lostBefore = lmu::mvr_pass_stats().biasCbLost;
        for (int i = 0; i < 16; ++i)
            context->PSSetShader(gamePs.Get(), nullptr, 0);
        check(bias_cb() == ours && lmu::mvr_pass_stats().biasCbLost == lostBefore + 1,
              "texture sharpening: unbound while two views stay on: found within 16 twin binds, bound again, counted");
        lmu::mvr_pass_end(0);
        check(bound_ps() == gamePs.Get(), "texture sharpening: pass end: the game's pixel shader");
        lmu::mvr_pass_begin(0, &view, false);  // no bias: two views, the game's pixel shader
        check(bound_ps() == gamePs.Get() && twoViewsOn == 1, "texture sharpening: a pass without a bias: the game's pixel shader");
        lmu::mvr_pass_end(0);
        const lmu::MvrPassStats ps = lmu::mvr_pass_stats();
        check(ps.biasPasses == 1 && ps.psTwinBinds >= 3, "texture sharpening: counted (one pass with a bias, its twin binds)");
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

    {  // Instancing (any GPU): the instanced twin bound with no stereo switch, the game's draws made with twice their
       // instances (pipeline statistics: vertices read), indirect draws into the eye alone; and its takeover when
       // single-pass stereo is refused.
        const char* src = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(id * 0.1, 0, 0.5, 1); }";
        ComPtr<ID3DBlob> code, errors;
        ComPtr<ID3D11VertexShader> game, gameTwin, instTwin;
        D3DCompile(src, std::strlen(src), "i", nullptr, nullptr, "main", "vs_4_0", 0, 0, &code, &errors);
        const simqvfr::dxbc::InstancedView iv = simqvfr::dxbc::add_instanced_view(
            static_cast<const uint8_t*>(code->GetBufferPointer()), code->GetBufferSize());
        device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &game);
        device->CreateVertexShader(iv.bytecode.data(), iv.bytecode.size(), nullptr, &instTwin);
        gameTwin = vertex_shader(device.Get(), "5");  // single-pass stereo's (a stand-in)
        check(game && instTwin && gameTwin, "instancing: a game shader with both twins");
        game->SetPrivateDataInterface(lmu::kMvrTwinGuid, gameTwin.Get());
        game->SetPrivateDataInterface(lmu::kMvrInstancedTwinGuid, instTwin.Get());
        lmu::mvr_twins_changed();
        const D3D11_QUERY_DESC qd = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
        ComPtr<ID3D11Query> query;
        device->CreateQuery(&qd, &query);
        const auto vertices = [&](auto&& draw) {  // the vertices the input assembler read for one draw
            context->Begin(query.Get());
            draw();
            context->End(query.Get());
            D3D11_QUERY_DATA_PIPELINE_STATISTICS st{};
            while (context->GetData(query.Get(), &st, sizeof(st), 0) == S_FALSE) {
            }
            return st.IAVertices;
        };
        const uint16_t indices[3] = {0, 1, 2};
        D3D11_BUFFER_DESC ibd = {sizeof(indices), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA ii = {indices, 0, 0};
        ComPtr<ID3D11Buffer> ib, args;
        device->CreateBuffer(&ibd, &ii, &ib);
        const UINT drawArgs[4] = {3, 1, 0, 0};
        D3D11_BUFFER_DESC ad = {sizeof(drawArgs), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0};
        const D3D11_SUBRESOURCE_DATA ai = {drawArgs, 0, 0};
        device->CreateBuffer(&ad, &ai, &args);
        context->IASetInputLayout(nullptr);
        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->RSSetViewports(1, &eye);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());

        lmu::mvr_use_instanced(true);
        const int stereoCalls = twoViewsCalls;
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        lmu::mvr_pass_begin(0, &view, false);  // its start hooks the draws instancing needs
        context->VSSetShader(game.Get(), nullptr, 0);
        n = viewports(context.Get(), vps);
        check(lmu::mvr_instanced() && bound_vs(context.Get()) == instTwin.Get() && twoViewsCalls == stereoCalls && n == 2 &&
                  same(vps[1], 100, 0, 60, 50),
              "instancing: the instanced twin bound, no stereo switch, the focus viewport beside the eye");
        const UINT64 draw = vertices([&] { context->Draw(3, 0); });
        const UINT64 indexed = vertices([&] { context->DrawIndexed(3, 0, 0); });
        const UINT64 inst = vertices([&] { context->DrawInstanced(3, 2, 0, 0); });
        const UINT64 indexedInst = vertices([&] { context->DrawIndexedInstanced(3, 2, 0, 0, 0); });
        std::snprintf(what, sizeof(what), "instancing: the game's draws doubled: Draw(3) read %llu vertices, DrawIndexed(3) "
                      "%llu, DrawInstanced(3, 2) %llu, DrawIndexedInstanced(3, 2) %llu",
                      static_cast<unsigned long long>(draw), static_cast<unsigned long long>(indexed),
                      static_cast<unsigned long long>(inst), static_cast<unsigned long long>(indexedInst));
        check(draw == 6 && indexed == 6 && inst == 12 && indexedInst == 12, what);
        const UINT64 indirect = vertices([&] { context->DrawInstancedIndirect(args.Get(), 0); });
        const lmu::MvrPassStats mid = lmu::mvr_pass_stats();
        std::snprintf(what, sizeof(what), "instancing: an indirect draw drawn by the game's shader into the eye alone (%llu "
                      "vertices), the twin bound again after",
                      static_cast<unsigned long long>(indirect));
        check(indirect == 3 && mid.eyeOnlyDraws == before.eyeOnlyDraws + 1 && bound_vs(context.Get()) == instTwin.Get(), what);
        ComPtr<ID3D11VertexShader> lone;  // no twin, no inputs either (no layout is bound)
        {
            const char* loneSrc = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, id, 0.5, 1); }";
            ComPtr<ID3DBlob> lc;
            D3DCompile(loneSrc, std::strlen(loneSrc), "l", nullptr, nullptr, "main", "vs_4_0", 0, 0, &lc, nullptr);
            device->CreateVertexShader(lc->GetBufferPointer(), lc->GetBufferSize(), nullptr, &lone);
        }
        context->VSSetShader(lone.Get(), nullptr, 0);
        const UINT64 plain = vertices([&] { context->Draw(3, 0); });
        check(plain == 3 && bound_vs(context.Get()) == lone.Get(), "instancing: a shader without a twin draws once");
        context->VSSetShader(game.Get(), nullptr, 0);
        lmu::mvr_pass_end(0);
        const UINT64 outside = vertices([&] { context->Draw(3, 0); });
        check(outside == 3 && bound_vs(context.Get()) == game.Get() && viewports(context.Get(), vps) == 1,
              "instancing: after the pass, the game's shader and one viewport, its draws once");
        check(lmu::mvr_pass_stats().doubledDraws == before.doubledDraws + 4, "instancing: four draws doubled");

        // End to end: a scene drawn in pass 0 through the hooks with the instanced twin. The eye's region must be what the
        // game's shader alone draws, pixel for pixel; the focus view's region beside it what the focus view's own
        // projection draws (the remap folded into the matrix, as passes 2-3 rendered it), but for edge pixels.
        {
            const char* sceneVs = R"(
cbuffer C : register(b0) { float4 r0, r1, r2, r3; }
struct O { float4 pos : SV_Position; float4 col : COLOR0; };
O main(float4 p : POSITION, float4 c : COLOR0) {
    O o; o.pos = float4(dot(r0, p), dot(r1, p), dot(r2, p), dot(r3, p)); o.col = c; return o; })";
            const char* scenePs = "float4 main(float4 pos : SV_Position, float4 col : COLOR0) : SV_Target { return col; }";
            ComPtr<ID3DBlob> vc, pc;
            D3DCompile(sceneVs, std::strlen(sceneVs), "sv", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vc, nullptr);
            D3DCompile(scenePs, std::strlen(scenePs), "sp", nullptr, nullptr, "main", "ps_4_0", 0, 0, &pc, nullptr);
            const simqvfr::dxbc::InstancedView sv = simqvfr::dxbc::add_instanced_view(
                static_cast<const uint8_t*>(vc->GetBufferPointer()), vc->GetBufferSize());
            ComPtr<ID3D11VertexShader> svs, stwin;
            ComPtr<ID3D11PixelShader> sps;
            ComPtr<ID3D11InputLayout> layout;
            const D3D11_INPUT_ELEMENT_DESC el[2] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            device->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, &svs);
            device->CreateVertexShader(sv.bytecode.data(), sv.bytecode.size(), nullptr, &stwin);
            device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &sps);
            device->CreateInputLayout(el, 2, vc->GetBufferPointer(), vc->GetBufferSize(), &layout);  // the game's bytecode
            svs->SetPrivateDataInterface(lmu::kMvrInstancedTwinGuid, stwin.Get());
            lmu::mvr_twins_changed();
            // A grid of 8 x 6 quads over the eye, each its own colour, depth and w (a perspective), as a triangle list.
            std::vector<float> verts;
            std::vector<uint16_t> idx;
            for (int j = 0; j < 6; ++j)
                for (int i = 0; i < 8; ++i) {
                    const float x0 = -1.1f + 2.2f * i / 8, x1 = -1.1f + 2.2f * (i + 1) / 8;
                    const float y0 = -1.1f + 2.2f * j / 6, y1 = -1.1f + 2.2f * (j + 1) / 6;
                    const float z = 0.2f + 0.07f * static_cast<float>((i * 5 + j * 3) % 9);
                    const float col[4] = {(i * 37 % 256) / 255.0f, (j * 53 % 256) / 255.0f, ((i + j) * 29 % 256) / 255.0f, 1};
                    const uint16_t base = static_cast<uint16_t>(verts.size() / 8);
                    for (const auto& [x, y] : {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x1, y1}, std::pair{x0, y1}}) {
                        verts.insert(verts.end(), {x, y, z, 1, col[0], col[1], col[2], col[3]});
                    }
                    // Clockwise on screen (the default rasterizer's front faces).
                    idx.insert(idx.end(), {base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 1), base,
                                           static_cast<uint16_t>(base + 3), static_cast<uint16_t>(base + 2)});
                }
            D3D11_BUFFER_DESC vbd = {static_cast<UINT>(verts.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
            D3D11_BUFFER_DESC sibd = {static_cast<UINT>(idx.size() * 2), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
            const D3D11_SUBRESOURCE_DATA vi = {verts.data(), 0, 0}, si = {idx.data(), 0, 0};
            ComPtr<ID3D11Buffer> svb, sib;
            device->CreateBuffer(&vbd, &vi, &svb);
            device->CreateBuffer(&sibd, &si, &sib);
            // The eye's camera rows (a mild perspective: w = 1 + 0.25 z) and the focus box's remap.
            const float rows[16] = {0.9f, 0.05f, 0, 0.02f, -0.03f, 0.95f, 0, -0.01f, 0, 0, 0.8f, 0.1f, 0, 0, 0.25f, 1};
            lmu::MvrPassView fv = view;  // eye 100 x 80, focus 60 x 50 beside it
            fv.remap[0] = 1 / 0.3f;
            fv.remap[1] = -0.1f / 0.3f;
            fv.remap[2] = 1 / 0.3f;
            fv.remap[3] = -0.2f / 0.3f;
            float focusRows[16];
            std::memcpy(focusRows, rows, sizeof(rows));
            for (int k = 0; k < 4; ++k) {
                focusRows[k] = fv.remap[0] * rows[k] + fv.remap[1] * rows[12 + k];
                focusRows[4 + k] = fv.remap[2] * rows[4 + k] + fv.remap[3] * rows[12 + k];
            }
            const auto cbuffer = [&](const float* data) {
                D3D11_BUFFER_DESC cd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
                const D3D11_SUBRESOURCE_DATA c = {data, 0, 0};
                ComPtr<ID3D11Buffer> b;
                device->CreateBuffer(&cd, &c, &b);
                return b;
            };
            const ComPtr<ID3D11Buffer> eyeCb = cbuffer(rows), focusCb = cbuffer(focusRows);
            const auto target = [&](UINT w, UINT h, ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& r,
                                    ComPtr<ID3D11DepthStencilView>& d) {
                D3D11_TEXTURE2D_DESC td2 = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_RENDER_TARGET, 0, 0};
                device->CreateTexture2D(&td2, nullptr, &t);
                device->CreateRenderTargetView(t.Get(), nullptr, &r);
                td2.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                td2.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                ComPtr<ID3D11Texture2D> dt;
                device->CreateTexture2D(&td2, nullptr, &dt);
                device->CreateDepthStencilView(dt.Get(), nullptr, &d);
                const float black[4] = {0, 0, 0, 0};
                context->ClearRenderTargetView(r.Get(), black);
                context->ClearDepthStencilView(d.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            };
            const auto read = [&](ID3D11Texture2D* t, UINT w, UINT h) {
                D3D11_TEXTURE2D_DESC sd2 = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                                            D3D11_CPU_ACCESS_READ, 0};
                ComPtr<ID3D11Texture2D> st;
                device->CreateTexture2D(&sd2, nullptr, &st);
                context->CopyResource(st.Get(), t);
                std::vector<uint32_t> px(static_cast<size_t>(w) * h);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(context->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                    for (UINT y = 0; y < h; ++y)
                        std::memcpy(px.data() + static_cast<size_t>(y) * w, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                                    w * 4);
                    context->Unmap(st.Get(), 0);
                }
                return px;
            };
            const UINT sceneStride = 32, zero = 0;
            const auto draw_scene = [&](ID3D11Buffer* cb) {
                context->IASetInputLayout(layout.Get());
                context->IASetVertexBuffers(0, 1, svb.GetAddressOf(), &sceneStride, &zero);
                context->IASetIndexBuffer(sib.Get(), DXGI_FORMAT_R16_UINT, 0);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetConstantBuffers(0, 1, &cb);
                context->PSSetShader(sps.Get(), nullptr, 0);
                context->DrawIndexed(static_cast<UINT>(idx.size()), 0, 0);
            };
            ComPtr<ID3D11Texture2D> mvT, eyeT, focusT;
            ComPtr<ID3D11RenderTargetView> mvR, eyeR, focusR;
            ComPtr<ID3D11DepthStencilView> mvD, eyeD, focusD;
            target(256, 128, mvT, mvR, mvD);
            target(256, 128, eyeT, eyeR, eyeD);
            target(60, 50, focusT, focusR, focusD);
            // The eye pass: both views through the hooks.
            context->RSSetViewports(1, &eye);
            context->OMSetRenderTargets(1, mvR.GetAddressOf(), mvD.Get());
            context->VSSetShader(svs.Get(), nullptr, 0);
            lmu::mvr_pass_begin(0, &fv, false);
            check(bound_vs(context.Get()) == stwin.Get(), "instancing end to end: the scene's instanced twin bound in pass 0");
            draw_scene(eyeCb.Get());
            lmu::mvr_pass_end(0);
            // The references, outside any pass: the eye with the game's shader; the focus view with its own projection.
            context->OMSetRenderTargets(1, eyeR.GetAddressOf(), eyeD.Get());
            context->RSSetViewports(1, &eye);
            context->VSSetShader(svs.Get(), nullptr, 0);
            draw_scene(eyeCb.Get());
            const D3D11_VIEWPORT focusVp = {0, 0, 60, 50, 0, 1};
            context->OMSetRenderTargets(1, focusR.GetAddressOf(), focusD.Get());
            context->RSSetViewports(1, &focusVp);
            draw_scene(focusCb.Get());
            const std::vector<uint32_t> mv = read(mvT.Get(), 256, 128), eyePx = read(eyeT.Get(), 256, 128),
                                        focusPx = read(focusT.Get(), 60, 50);
            int eyeDiff = 0, focusDiff = 0, lit = 0;
            for (UINT y = 0; y < 80; ++y)
                for (UINT x = 0; x < 100; ++x)
                    eyeDiff += mv[y * 256 + x] != eyePx[y * 256 + x] ? 1 : 0;
            for (UINT y = 0; y < 50; ++y)
                for (UINT x = 0; x < 60; ++x) {
                    focusDiff += mv[y * 256 + 100 + x] != focusPx[y * 60 + x] ? 1 : 0;
                    lit += focusPx[y * 60 + x] != 0 ? 1 : 0;
                }
            std::snprintf(what, sizeof(what), "instancing end to end: the eye's region as the game's shader draws it (%d of 8000 "
                          "pixels differ), the focus view's as its own projection draws it (%d of 3000 differ: edges; %d lit)",
                          eyeDiff, focusDiff, lit);
            check(eyeDiff == 0 && focusDiff <= 60 && lit > 2500, what);  // gaps where neighbours' w differ, in both
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
            context->IASetInputLayout(nullptr);
            context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        }

        // Single-pass stereo refused while instancing may take over: the instanced twin from that bind on.
        lmu::mvr_use_instanced(false, true);
        lmu::mvr_set_two_views(&refusing_two_views);
        lmu::mvr_pass_begin(0, &view, false);
        context->VSSetShader(game.Get(), nullptr, 0);
        const UINT64 takenOver = vertices([&] { context->Draw(3, 0); });
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(lmu::mvr_instanced() && !lmu::mvr_broken() && after.instancedTakeovers == 1 &&
                  bound_vs(context.Get()) == instTwin.Get() && takenOver == 6 && viewports(context.Get(), vps) == 2,
              "single-pass stereo refused: instancing took over (its twin, the draw doubled, two viewports), not broken");
        lmu::mvr_pass_end(0);
        lmu::mvr_use_instanced(false, false);
        lmu::mvr_set_two_views(&fake_two_views);
        context->VSSetShader(a.Get(), nullptr, 0);
    }

    const uint64_t errorsBefore = lmu::mvr_pass_stats().stereoErrors;
    lmu::mvr_set_two_views(&refusing_two_views);
    lmu::mvr_pass_begin(1, &view, false);
    const lmu::MvrPassStats s = lmu::mvr_pass_stats();
    check(bound_vs(context.Get()) == a.Get() && viewports(context.Get(), vps) == 1 && s.stereoErrors == errorsBefore + 1 &&
              lmu::mvr_broken(),
          "single-pass stereo refused with no instancing to take over: the game's state, off for the session");
    lmu::mvr_pass_end(1);

    const unsigned w = 160, h = 120;
    const std::vector<uint16_t> base = image(w, h, 0, 0, 1.0f);
    const lmu::MvrComparison same0 = lmu::mvr_compare(base, base, w, h);
    check(same0.ok && same0.shiftX == 0 && same0.shiftY == 0 && same0.meanDiff < 1e-6 &&
              std::fabs(same0.brightness - 1) < 1e-6 && same0.differingPct == 0,
          "compare: an image with itself: aligned, brightness 1, nothing differs");
    const lmu::MvrComparison moved = lmu::mvr_compare(base, image(w, h, 2, -1, 1.0f), w, h);
    std::snprintf(what, sizeof(what), "compare: moved by (2, -1): found at %+d,%+d, mean difference %.4f", moved.shiftX,
                  moved.shiftY, moved.meanDiff);
    check(moved.ok && moved.shiftX == 2 && moved.shiftY == -1 && moved.meanDiff < 0.002, what);
    const lmu::MvrComparison bright = lmu::mvr_compare(base, image(w, h, 0, 0, 1.1f), w, h);
    std::snprintf(what, sizeof(what), "compare: 10%% brighter: brightness %.4f, cell median %.4f", bright.brightness,
                  bright.cellMedian);
    check(bright.ok && std::fabs(bright.brightness - 1.1) < 0.005 && std::fabs(bright.cellMedian - 1.1) < 0.005, what);

    {  // The 8-bit comparison (the focus post's images against passes 2-3's: gamma-encoded BGRA).
        const auto bgra = [&](int shiftX, int shiftY, int add) {
            std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const double sx = static_cast<double>(x) - shiftX, sy = static_cast<double>(y) - shiftY;
                    const double v = 128 + 60 * std::sin(sx * 0.21) * std::cos(sy * 0.17) + 30 * std::sin((sx + sy) * 0.05);
                    for (int c = 0; c < 3; ++c)
                        px[(static_cast<size_t>(y) * w + x) * 4 + c] =
                            static_cast<uint8_t>(std::lround(std::min(255.0, std::max(0.0, v - 10 * c + add))));
                    px[(static_cast<size_t>(y) * w + x) * 4 + 3] = 255;
                }
            return px;
        };
        const std::vector<uint8_t> b8 = bgra(0, 0, 0);
        const lmu::MvrComparison s8 = lmu::mvr_compare_bgra8(b8, b8, w, h);
        check(s8.ok && s8.shiftX == 0 && s8.shiftY == 0 && s8.maxByte == 0 && s8.meanByte == 0 &&
                  std::fabs(s8.brightness - 1) < 1e-9,
              "compare 8-bit: an image with itself: aligned, brightness 1, no byte differs");
        const lmu::MvrComparison m8 = lmu::mvr_compare_bgra8(b8, bgra(-3, 2, 0), w, h);
        std::snprintf(what, sizeof(what), "compare 8-bit: moved by (-3, 2): found at %+d,%+d, largest byte difference %d",
                      m8.shiftX, m8.shiftY, m8.maxByte);
        check(m8.ok && m8.shiftX == -3 && m8.shiftY == 2 && m8.maxByte <= 1, what);
        const lmu::MvrComparison p8 = lmu::mvr_compare_bgra8(b8, bgra(0, 0, 3), w, h);
        std::snprintf(what, sizeof(what),
                      "compare 8-bit: 3 added to every channel: bytes largest %d, mean %.2f, %.1f%% over 4; brightness %.4f",
                      p8.maxByte, p8.meanByte, p8.bytesOverPct, p8.brightness);
        check(p8.ok && p8.maxByte == 3 && std::fabs(p8.meanByte - 3) < 0.05 && p8.bytesOverPct == 0 && p8.brightness > 1.01 &&
                  p8.brightness < 1.06,
              what);
        check(!lmu::mvr_compare_bgra8(b8, b8, w, h - 1).ok, "compare 8-bit: sizes that don't match the images: not ok");
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

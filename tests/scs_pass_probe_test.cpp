// Offline check of the SCS games' eye-pass probe (games/scssoftware/pass_probe.h) and the context hooks under it (engine/context_hooks.h).
// The tracker gets the bind sequence of the 100% capture (docs/recon-ats.md "Capture census at Scaling 100%"): shared
// work with 512-wide probe G-buffers, two eye passes back to back, the post from a quarter-size target. Then the hooks on
// a WARP device: binds and draws reach the observers, also after the context's multithread protection went on (d3d11
// rewrites the context's vtable then), and calls made from inside an observer go straight through.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include "engine/context_hooks.h"
#include "games/scssoftware/pass_probe.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;

namespace {

using scs::PassTracker;

PassTracker::Bind bind(unsigned targets, bool depth, unsigned w, unsigned h, int id) {
    PassTracker::Bind b;
    b.targets = targets;
    b.depth = depth;
    b.width = w;
    b.height = h;
    b.first = reinterpret_cast<const void*>(static_cast<uintptr_t>(id));
    return b;
}

void draws(PassTracker& t, int n, int instanced = 0) {
    for (int i = 0; i < n; ++i)
        t.draw(i < instanced);
}

// One frame shaped like ats_frame5585.rdc, `t0` its start in seconds; eye A's G-buffer target `a`, eye B's `b`.
PassTracker::Frame ats_frame(PassTracker& t, double t0, int a, int b) {
    t.bind(bind(0, true, 4096, 4096, 1), t0);  // sun shadow
    draws(t, 322);
    t.bind(bind(4, true, 512, 512, 2), t0 + 0.001);  // probe G-buffer: too small to be an eye
    draws(t, 413);
    t.bind(bind(4, true, 512, 1024, 3), t0 + 0.002);
    draws(t, 318);
    t.bind(bind(4, true, 5192, 4168, a), t0 + 0.004);  // eye A
    draws(t, 1968, 155);
    t.bind(bind(1, false, 2596, 2084, 10), t0 + 0.006);  // AO at half size: still eye A
    draws(t, 5);
    t.bind(bind(2, true, 5192, 4168, 11), t0 + 0.0062);  // light volumes
    draws(t, 24);
    t.bind(bind(1, true, 5192, 4168, 12), t0 + 0.0063);  // forward
    draws(t, 454, 2);
    t.bind(bind(4, true, 5192, 4168, b), t0 + 0.007);  // eye B
    draws(t, 1968, 155);
    t.bind(bind(4, true, 5192, 4168, b), t0 + 0.008);  // the same G-buffer again: the same pass
    t.bind(bind(1, true, 5192, 4168, 12), t0 + 0.009);
    draws(t, 478, 2);
    t.bind(bind(1, false, 1298, 1042, 20), t0 + 0.010);  // luminance: post
    draws(t, 30);
    return t.end_frame(t0 + 0.011);
}

void tracker_checks() {
    PassTracker t;
    const PassTracker::Frame f = ats_frame(t, 1.0, 100, 101);
    check(!PassTracker::gbuffer_shape(bind(1, true, 5192, 4168, 1)) && !PassTracker::gbuffer_shape(bind(4, true, 512, 1024, 1)) &&
              !PassTracker::gbuffer_shape(bind(4, false, 5192, 4168, 1)) && PassTracker::gbuffer_shape(bind(4, true, 5192, 4168, 1)),
          "the G-buffer's shape: 4 targets with depth, at least 1024 square");
    check(f.passes == 2 && f.eyeWidth == 5192 && f.eyeHeight == 4168, "two eye passes at the learned eye size; probes not eyes");
    check(f.shared.draws == 322 + 413 + 318 && f.pass[0].draws == 1968 + 5 + 24 + 454 &&
              f.pass[1].draws == 1968 + 478 && f.post.draws == 30,
          "draws counted into shared work, each eye pass and the post");
    check(f.pass[0].instanced == 157 && f.pass[1].instanced == 157, "instanced draws counted per pass");
    check(f.end[0] == PassTracker::End::nextGbuffer && f.end[1] == PassTracker::End::post,
          "eye A ended by eye B's G-buffer, eye B by the quarter-size post target");
    check(std::abs(f.shared.ms - 4.0) < 1e-6 && std::abs(f.pass[0].ms - 3.0) < 1e-6 && std::abs(f.pass[1].ms - 3.0) < 1e-6 &&
              std::abs(f.post.ms - 1.0) < 1e-6,
          "segment times on the render thread's clock");

    const PassTracker::Frame g = ats_frame(t, 2.0, 100, 101);
    check(g.passes == 2 && g.shared.draws == 1053, "the next frame starts clean");
    check(std::abs(g.shared.ms - (2.0 + 0.004 - 1.011) * 1000.0) < 1e-6,
          "the next frame's shared work starts at the previous frame's end");

    // A frame without the eye passes (a menu): no passes; one cut short by the frame's end.
    t.bind(bind(1, false, 2880, 1800, 30), 3.0);
    draws(t, 65);
    const PassTracker::Frame menu = t.end_frame(3.001);
    check(menu.passes == 0 && menu.shared.draws == 65, "a menu frame: no eye passes");
    t.bind(bind(4, true, 5192, 4168, 100), 4.0);
    draws(t, 10);
    const PassTracker::Frame cut = t.end_frame(4.002);
    check(cut.passes == 1 && cut.end[0] == PassTracker::End::frameEnd && cut.pass[0].draws == 10,
          "a pass still open at xrEndFrame is ended by it");

    scs::PassSummary s;
    s.add(f);
    s.add(g);
    s.add(menu);
    const std::string line = s.line();
    check(s.frames == 3 && s.stereo == 2 && line.find("eye A 3.00 ms") != std::string::npos &&
              line.find("ends A: next G-buffer 2, post 0") != std::string::npos,
          "the summary averages stereo frames and counts end reasons");
}

// ---- The context hooks on WARP ----

struct Seen {
    int binds = 0, draws = 0, instanced = 0, nested = 0;
    UINT lastCount = 0;
    bool lastDepth = false;
} seen;
ID3D11DeviceContext* hookedContext = nullptr;

void on_targets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const*, ID3D11DepthStencilView* dsv) {
    ++seen.binds;
    seen.lastCount = count;
    seen.lastDepth = dsv != nullptr;
    // A call into the context from inside an observer goes straight through (not observed again).
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv)
        rtv->Release();
}

void on_draw(ID3D11DeviceContext*, simqvfr::context_hooks::Entry entry, UINT, UINT instances) {
    ++seen.draws;
    if (entry == simqvfr::context_hooks::kDrawInstanced || instances != 1)
        ++seen.instanced;
}

void hook_checks() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        check(false, "a WARP device");
        return;
    }
    hookedContext = context.Get();
    D3D11_TEXTURE2D_DESC td = {64, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateTexture2D(&td, nullptr, &tex);
    device->CreateRenderTargetView(tex.Get(), nullptr, &rtv);

    namespace ch = simqvfr::context_hooks;
    ch::Observers o;
    o.targets = &on_targets;
    o.draw = &on_draw;
    const uint32_t wanted = (1u << ch::kOMSetRenderTargets) | (1u << ch::kDraw) | (1u << ch::kDrawInstanced);
    check(ch::install(context.Get(), wanted, o) && ch::installed(), "hooks installed on a WARP context");
    const ch::Stats st = ch::stats();
    check(st.functions >= 3 && st.failed == 0, "every function of the wanted entries hooked");

    ID3D11RenderTargetView* const views[] = {rtv.Get()};
    context->OMSetRenderTargets(1, views, nullptr);
    context->Draw(3, 0);
    context->DrawInstanced(3, 2, 0, 0);
    check(seen.binds == 1 && seen.lastCount == 1 && !seen.lastDepth && seen.draws == 2 && seen.instanced == 1,
          "a bind and two draws observed (one instanced)");

    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context.As(&mt))) {
        const BOOL was = mt->SetMultithreadProtected(TRUE);
        context->OMSetRenderTargets(1, views, nullptr);
        context->Draw(3, 0);
        check(seen.binds == 2 && seen.draws == 3, "multithread protection on: still observed (its functions hooked at install)");
        mt->SetMultithreadProtected(was);
    } else {
        check(false, "the context's multithread protection");
    }
    context->Flush();
    ch::rescan();
    context->OMSetRenderTargets(1, views, nullptr);
    check(seen.binds == 3, "after a flush and a rescan: still observed");

    ch::uninstall();
    context->OMSetRenderTargets(1, views, nullptr);
    context->Draw(3, 0);
    check(!ch::installed() && seen.binds == 3 && seen.draws == 3, "uninstalled: calls pass straight on, unobserved");
    check(SUCCEEDED(device->GetDeviceRemovedReason()), "the device is fine");
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    tracker_checks();
    hook_checks();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

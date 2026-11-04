#include "view_share.h"

#include <atomic>
#include <cstdint>
#include <mutex>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../log.h"
#include "../../quad/d3d11_view_resample.h"
#include "../../quad/focus_turn.h"
#include "views.h"

namespace acc {
namespace {

using Microsoft::WRL::ComPtr;

// ID3D11DeviceContext's vtable (d3d11.h order: IUnknown 0-2, ID3D11DeviceChild 3-6, then the context's own).
constexpr std::size_t kSlotDrawIndexed = 12, kSlotOMSetRenderTargets = 33, kSlotOMSetRenderTargetsAndUavs = 34,
                      kSlotDispatch = 41;
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using SetTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*);
using SetTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                     ID3D11DepthStencilView*, UINT, UINT,
                                                     ID3D11UnorderedAccessView* const*, const UINT*);
// Each vtable hooked, with what it held: ACC can recreate its session on a context of another class (a log: the
// second session's context had another vtable), and a hook must reach the original of the context it is called on.
struct Hooked {
    void** vtable;
    DrawIndexedFn drawIndexed;
    DispatchFn dispatch;
    SetTargetsFn setTargets;
    SetTargetsAndUavsFn setTargetsAndUavs;
};
constexpr int kMaxHooked = 8;
Hooked hooked[kMaxHooked] = {};
std::atomic<int> hookedCount{0};
const Hooked* hooked_for_vtable(void** vtable) {
    const int n = hookedCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (hooked[i].vtable == vtable)
            return &hooked[i];
    return nullptr;
}
const Hooked* hooked_for(ID3D11DeviceContext* ctx) { return hooked_for_vtable(*reinterpret_cast<void***>(ctx)); }

std::atomic<bool> shareSsr{true}, shareAo{true};
std::atomic<bool> active{false};
std::atomic<ID3D11DeviceContext*> immediate{nullptr};

// The render thread's (the only caller of ACC's immediate context), and view_share_stop's: under `mutex`.
std::mutex mutex;
aceqv::D3D11ViewResample resampler;
ComPtr<ID3D11Texture2D> eyeSsr[2];
aceqv::ViewRegion eyeSsrRegion[2];
bool eyeSsrFresh[2] = {}, eyeAoFresh[2] = {};
// Render thread only: the texture the last dispatch wrote, if it looked like the SSR denoiser's output; the bound
// render target 0, if it looks like the AO texture. Compared, never dereferenced.
ID3D11Resource* armed = nullptr;
ID3D11Resource* aoTarget = nullptr;

struct Counts {
    std::atomic<std::uint64_t> shared[2], eyes{0}, noEye{0}, failed{0}, unplaced{0};
    std::atomic<bool> unplacedLogged{false}, firstLogged{false};
};
Counts ssrCounts, aoCounts;
std::uint64_t frames = 0;

bool scene_texture(ID3D11Resource* r, DXGI_FORMAT a, DXGI_FORMAT b) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return false;
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    if (d.Format != a && d.Format != b)
        return false;
    int target[2];
    views_target(target);
    // The scene textures are the target's width (their height rounded up a little).
    return static_cast<int>(d.Width) == target[0] && static_cast<int>(d.Height) >= target[1];
}

bool on_immediate(ID3D11DeviceContext* ctx) {
    return ctx == immediate.load(std::memory_order_relaxed) && active.load(std::memory_order_relaxed);
}

// The view the bound viewport is (-1: none, logged once per effect), its rect in `region`.
int bound_view(ID3D11DeviceContext* ctx, Counts& counts, const char* what, aceqv::ViewRegion& region) {
    D3D11_VIEWPORT vp{};
    UINT n = 1;
    ctx->RSGetViewports(&n, &vp);
    int rects[4][4] = {};
    bool have[4] = {};
    for (int v = 0; v < 4; ++v)
        have[v] = views_view_rect(v, rects[v]);
    const int view = n ? view_at(rects, have, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height) : -1;
    if (view < 0) {
        counts.unplaced.fetch_add(1, std::memory_order_relaxed);
        if (!counts.unplacedLogged.exchange(true))
            aceqv::logf("acc view share: %s at viewport %.0f,%.0f %.0fx%.0f, none of the views' (%d,%d %dx%d / %d,%d "
                        "%dx%d / %d,%d %dx%d / %d,%d %dx%d): left as it is",
                        what, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, rects[0][0], rects[0][1], rects[0][2],
                        rects[0][3], rects[1][0], rects[1][1], rects[1][2], rects[1][3], rects[2][0], rects[2][1],
                        rects[2][2], rects[2][3], rects[3][0], rects[3][1], rects[3][2], rects[3][3]);
        return -1;
    }
    region = {rects[view][0], rects[view][1], rects[view][2], rects[view][3]};
    return view;
}

// Focus view e's mapping into its eye from this frame's record (what its views were rendered with).
bool focus_mapping(int e, aceqv::ViewMapping& m) {
    aceqv::Views4Frame frame{};
    if (!views_latest_frame(frame) || frame.focusTan[e][1] <= frame.focusTan[e][0] ||
        frame.outerTan[e][1] <= frame.outerTan[e][0])
        return false;
    for (int i = 0; i < 4; ++i) {
        m.focusTan[i] = frame.focusTan[e][i];
        m.eyeTan[i] = frame.outerTan[e][i];
    }
    aceqv::to_matrix(frame.turn[e], m.rotation);  // the focus camera is the eye's turned by turn[e]
    return true;
}

void note_shared(Counts& counts, const char* what, int view, const aceqv::ViewRegion& from, const aceqv::ViewRegion& to) {
    counts.shared[view - 2].fetch_add(1, std::memory_order_relaxed);
    if (!counts.firstLogged.exchange(true))
        aceqv::logf("acc view share: focus view %d's %s from its eye's (eye %d,%d %dx%d -> focus %d,%d %dx%d)", view, what,
                    from.x, from.y, from.width, from.height, to.x, to.y, to.width, to.height);
}

// ---- SSR ----

void STDMETHODCALLTYPE dispatch_hook(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    hooked_for(ctx)->dispatch(ctx, x, y, z);
    if (!on_immediate(ctx))
        return;
    armed = nullptr;
    if (!shareSsr.load(std::memory_order_relaxed))
        return;
    ID3D11UnorderedAccessView* uav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &uav);
    if (!uav)
        return;
    ID3D11Resource* r = nullptr;
    uav->GetResource(&r);
    uav->Release();
    if (r && scene_texture(r, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_TYPELESS))
        armed = r;
    if (r)
        r->Release();
}

// The draw right after an armed dispatch: if it reads that texture with a cube array bound, a view's reflection
// composite.
void on_composite(ID3D11DeviceContext* ctx, ID3D11Resource* written) {
    ID3D11ShaderResourceView* srvs[32] = {};
    ctx->PSGetShaderResources(0, 32, srvs);
    bool cubeArray = false;
    ComPtr<ID3D11Texture2D> ssr;
    for (ID3D11ShaderResourceView* srv : srvs) {
        if (!srv)
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC d;
        srv->GetDesc(&d);
        cubeArray = cubeArray || d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        ComPtr<ID3D11Resource> r;
        srv->GetResource(&r);
        if (r.Get() == written)
            r.As(&ssr);
        srv->Release();
    }
    if (!cubeArray || !ssr)
        return;
    aceqv::ViewRegion region;
    const int view = bound_view(ctx, ssrCounts, "a reflection composite", region);
    if (view < 0)
        return;
    if (view < 2) {
        eyeSsr[view] = ssr;
        eyeSsrRegion[view] = region;
        eyeSsrFresh[view] = true;
        ssrCounts.eyes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int e = view - 2;
    if (!eyeSsrFresh[e] || !eyeSsr[e]) {
        ssrCounts.noEye.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    eyeSsrFresh[e] = false;
    aceqv::ViewMapping m{};
    if (!focus_mapping(e, m) || !resampler.resample(eyeSsr[e].Get(), eyeSsrRegion[e], ssr.Get(), region, m)) {
        ssrCounts.failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    note_shared(ssrCounts, "reflections", view, eyeSsrRegion[e], region);
}

// ---- AO ----

void follow_target(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs) {
    if (!on_immediate(ctx))
        return;
    aoTarget = nullptr;
    if (!shareAo.load(std::memory_order_relaxed) || count == 0 || !rtvs || !rtvs[0])
        return;
    ID3D11Resource* r = nullptr;
    rtvs[0]->GetResource(&r);
    if (r && scene_texture(r, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_TYPELESS))
        aoTarget = r;
    if (r)
        r->Release();
}

void STDMETHODCALLTYPE set_targets_hook(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs,
                                        ID3D11DepthStencilView* dsv) {
    hooked_for(ctx)->setTargets(ctx, count, rtvs, dsv);
    follow_target(ctx, count, rtvs);
}

void STDMETHODCALLTYPE set_targets_and_uavs_hook(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs,
                                                 ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount,
                                                 ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts) {
    hooked_for(ctx)->setTargetsAndUavs(ctx, count, rtvs, dsv, uavStart, uavCount, uavs, initialCounts);
    if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        follow_target(ctx, count, rtvs);
}

// A draw into the AO texture (the view's AO, just drawn): after a focus view's, its region from its eye's.
void on_ao_drawn(ID3D11DeviceContext* ctx, ID3D11Resource* written) {
    aceqv::ViewRegion region;
    const int view = bound_view(ctx, aoCounts, "a draw into the AO texture", region);
    if (view < 0)
        return;
    if (view < 2) {
        eyeAoFresh[view] = true;
        aoCounts.eyes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int e = view - 2;
    int eyeRect[4];
    if (!eyeAoFresh[e] || !views_view_rect(e, eyeRect)) {
        aoCounts.noEye.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    eyeAoFresh[e] = false;
    const aceqv::ViewRegion eye{eyeRect[0], eyeRect[1], eyeRect[2], eyeRect[3]};
    auto* ao = static_cast<ID3D11Texture2D*>(written);  // scene_texture: a Texture2D
    aceqv::ViewMapping m{};
    if (!focus_mapping(e, m) || !resampler.resample_target(ao, eye, ao, region, m)) {
        aoCounts.failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    note_shared(aoCounts, "ambient occlusion", view, eye, region);
}

void STDMETHODCALLTYPE draw_indexed_hook(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base) {
    // Deferred contexts share the vtable: only the immediate one (the render thread's) touches `armed`, `aoTarget`.
    const bool mine = ctx == immediate.load(std::memory_order_relaxed);
    if (mine && armed) {
        ID3D11Resource* written = armed;
        armed = nullptr;
        std::lock_guard guard(mutex);
        if (active.load(std::memory_order_relaxed) && views_focused())
            on_composite(ctx, written);
    }
    hooked_for(ctx)->drawIndexed(ctx, count, start, base);
    if (mine && aoTarget) {
        std::lock_guard guard(mutex);
        if (active.load(std::memory_order_relaxed) && views_focused())
            on_ao_drawn(ctx, aoTarget);
    }
}

// The hooked slots of `vtable` (the entry is published before the slots point at the hooks).
bool hook_vtable(void** vtable) {
    if (hooked_for_vtable(vtable))
        return true;
    const int n = hookedCount.load();
    if (n >= kMaxHooked)
        return false;
    const std::size_t span = (kSlotDispatch - kSlotDrawIndexed + 1) * sizeof(void*);
    DWORD old = 0;
    if (!VirtualProtect(&vtable[kSlotDrawIndexed], span, PAGE_READWRITE, &old))
        return false;
    hooked[n] = {vtable, reinterpret_cast<DrawIndexedFn>(vtable[kSlotDrawIndexed]),
                 reinterpret_cast<DispatchFn>(vtable[kSlotDispatch]),
                 reinterpret_cast<SetTargetsFn>(vtable[kSlotOMSetRenderTargets]),
                 reinterpret_cast<SetTargetsAndUavsFn>(vtable[kSlotOMSetRenderTargetsAndUavs])};
    hookedCount.store(n + 1, std::memory_order_release);
    vtable[kSlotDrawIndexed] = reinterpret_cast<void*>(&draw_indexed_hook);
    vtable[kSlotDispatch] = reinterpret_cast<void*>(&dispatch_hook);
    vtable[kSlotOMSetRenderTargets] = reinterpret_cast<void*>(&set_targets_hook);
    vtable[kSlotOMSetRenderTargetsAndUavs] = reinterpret_cast<void*>(&set_targets_and_uavs_hook);
    VirtualProtect(&vtable[kSlotDrawIndexed], span, old, &old);
    return true;
}

void report(Counts& c, const char* what) {
    aceqv::logf("acc view share: %s over the last 900 frames: focus views given their eye's left %llu right %llu; eye "
                "passes %llu, focus passes without an eye's %llu, failed %llu, at no view's viewport %llu",
                what, static_cast<unsigned long long>(c.shared[0].exchange(0)),
                static_cast<unsigned long long>(c.shared[1].exchange(0)), static_cast<unsigned long long>(c.eyes.exchange(0)),
                static_cast<unsigned long long>(c.noEye.exchange(0)), static_cast<unsigned long long>(c.failed.exchange(0)),
                static_cast<unsigned long long>(c.unplaced.exchange(0)));
}

}  // namespace

void view_share_configure(bool ssr, bool ao) {
    shareSsr.store(ssr);
    shareAo.store(ao);
    if (!ssr)
        aceqv::log_line("acc view share: reflections not shared (acc_ssr_share=0): each focus view traces its own");
    if (!ao)
        aceqv::log_line("acc view share: ambient occlusion not shared (acc_ao_share=0): each focus view computes its own");
}

void view_share_start(ID3D11Device* device) {
    if ((!shareSsr.load() && !shareAo.load()) || !device)
        return;
    std::lock_guard guard(mutex);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    void** vtable = *reinterpret_cast<void***>(context.Get());
    if (!resampler.initialize(device)) {
        aceqv::log_line("acc view share: resampler unavailable; each focus view computes its own effects");
        return;
    }
    const int before = hookedCount.load();
    if (!hook_vtable(vtable)) {
        aceqv::log_line("acc view share: hooking the immediate context failed; each focus view computes its own effects");
        resampler.shutdown();
        return;
    }
    if (hookedCount.load() != before && before > 0)
        aceqv::logf("acc view share: this session's context has another vtable (%p): hooked too", static_cast<void*>(vtable));
    immediate.store(context.Get());  // the device holds it as long as the session's
    active.store(true);
    aceqv::logf("acc view share: on (the focus views take their eyes' %s)",
                shareSsr.load() && shareAo.load() ? "reflections and ambient occlusion"
                : shareSsr.load()                 ? "reflections"
                                                  : "ambient occlusion");
}

void view_share_stop() {
    active.store(false);
    std::lock_guard guard(mutex);
    immediate.store(nullptr);
    for (int e = 0; e < 2; ++e) {
        eyeSsr[e].Reset();
        eyeSsrFresh[e] = eyeAoFresh[e] = false;
    }
    resampler.shutdown();
}

void view_share_frame_end() {
    if (!active.load(std::memory_order_relaxed) || ++frames % 900 != 0)
        return;
    if (shareSsr.load(std::memory_order_relaxed))
        report(ssrCounts, "reflections");
    if (shareAo.load(std::memory_order_relaxed))
        report(aoCounts, "ambient occlusion");
}

}  // namespace acc

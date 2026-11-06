#include "view_share.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

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

// D3D11's own immediate context keeps its vtable per context, on the heap beside it, and rewrites its draw and
// dispatch slots itself as state changes (a log, 2026-11-04: those two hooks never ran without RenderDoc, whose
// wrapper has a fixed vtable; OMSetRenderTargets, not rewritten, ran 400k times per 900 frames). So the target-set
// hooks, which run before every pass, put those two back on the session's context, keeping what D3D11 last wrote
// as the original to call. Same thread as D3D11's rewrites (the immediate context's), so no race.
std::atomic<std::uint64_t> rearms{0};
template <typename Fn>
void rearm_slot(void** vtable, std::size_t slot, void* hook, Fn& original) {
    void* now = vtable[slot];
    if (now == hook)
        return;
    // D3D11's per-context vtable is heap memory it writes itself: writable, no VirtualProtect per rearm.
    MEMORY_BASIC_INFORMATION info{};
    const bool writable = VirtualQuery(&vtable[slot], &info, sizeof(info)) &&
                          (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
    DWORD old = 0;
    if (!writable && !VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old))
        return;
    original = reinterpret_cast<Fn>(now);
    vtable[slot] = hook;
    if (!writable)
        VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    rearms.fetch_add(1, std::memory_order_relaxed);
}
void STDMETHODCALLTYPE draw_indexed_hook(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base);
void STDMETHODCALLTYPE dispatch_hook(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z);
void rearm(ID3D11DeviceContext* ctx) {
    void** vtable = *reinterpret_cast<void***>(ctx);
    Hooked* h = const_cast<Hooked*>(hooked_for_vtable(vtable));
    if (!h)
        return;
    rearm_slot(vtable, kSlotDrawIndexed, reinterpret_cast<void*>(&draw_indexed_hook), h->drawIndexed);
    rearm_slot(vtable, kSlotDispatch, reinterpret_cast<void*>(&dispatch_hook), h->dispatch);
}

std::atomic<bool> shareSsr{true}, shareAo{true};
std::atomic<bool> active{false};
std::atomic<ID3D11DeviceContext*> immediate{nullptr};

// The render thread's (the only caller of ACC's immediate context), and view_share_stop's: under `mutex`.
std::mutex mutex;
simqvfr::D3D11ViewResample resampler;
ComPtr<ID3D11Texture2D> eyeSsr[2];
simqvfr::ViewRegion eyeSsrRegion[2];
bool eyeSsrFresh[2] = {};
// The texture each eye last drew its AO into this frame (compared, never dereferenced): a focus view's draw is
// refilled only from the same texture's eye region. Under DLSS several single-channel textures share the scene
// textures' size (2026-11-04), so a focus view's draw into one must not take an eye's draw into another.
ID3D11Resource* eyeAo[2] = {};
// A focus view's refill, waiting for ACC to leave the AO texture: on track each view draws its AO several times a
// frame into the same texture (2026-11-04: 3-4 draws per eye), so a refill after a focus view's first draw was drawn
// over by its next ones. Done when the render target moves off that texture (or at the frame's end), from its eye's
// finished region.
struct AoPending {
    ComPtr<ID3D11Texture2D> texture;
    simqvfr::ViewRegion eye, focus;
    bool set = false;
};
AoPending aoPending[2];  // under `mutex`

// One frame's AO-candidate draws now and then (the first few reports), to see which textures each view draws into.
struct AoTrace {
    bool recording = false;
    int reports = 0;
    struct Entry {
        int view;
        const void* texture;
        unsigned width, height;
        char action;  // e: an eye's; p: a focus view's, refill pending; r: refilled; o: into another texture than its
                      // eye's; n: no eye yet
    };
    Entry entries[64];
    int count = 0, dropped = 0, trackReports = 0;
};
AoTrace aoTrace;  // under `mutex`
// Render thread only: the texture the last dispatch wrote, if it looked like the SSR denoiser's output; the bound
// render target 0, if it looks like the AO texture. Compared, never dereferenced.
ID3D11Resource* armed = nullptr;
ID3D11Resource* aoTarget = nullptr;

struct Counts {
    std::atomic<std::uint64_t> shared[2], eyes{0}, noEye{0}, failed{0}, unplaced{0}, otherTexture{0};
    std::atomic<bool> unplacedLogged{false}, firstLogged{false};
};
Counts ssrCounts, aoCounts;
std::uint64_t frames = 0;

// Whether the hooks run at all, for the report: calls of each, and how many came on the session's context. The
// share worked only under RenderDoc (its own context wrapper) and saw nothing without it (2026-11-04).
struct HookCalls {
    std::atomic<std::uint64_t> draws{0}, drawsMine{0}, dispatches{0}, dispatchesMine{0}, targets{0}, targetsMine{0};
};
HookCalls hookCalls;
void** sessionVtable = nullptr;  // the session's context's vtable (view_share_start), under `mutex`

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
    // The scene textures span the views, rounded up to 4 (docs/recon-acc.md). The views can reach past the target: with
    // an odd eye width (1614) UE starts the right eye at an even x (1616), so its views end at 3230 and its
    // scene textures are 3232 wide against a 3228 target (2026-11-04: none matched, nothing was shared). Under
    // DLSS (or any screen percentage) the views render smaller than the target and the scene textures with them
    // (2026-11-04, Quality: views reach 1941x892, scene textures 1944x892 in a 2904x1336 target): so the views'
    // final rects alone size them, the target only before the renderer first reported them.
    int extent[2] = {0, 0};
    for (int v = 0; v < 4; ++v) {
        int rect[4];
        if (views_view_rect(v, rect)) {
            extent[0] = std::max(extent[0], rect[0] + rect[2]);
            extent[1] = std::max(extent[1], rect[1] + rect[3]);
        }
    }
    if (extent[0] <= 0 || extent[1] <= 0) {
        extent[0] = target[0];
        extent[1] = target[1];
    }
    // At least the views on both axes, but no bound above: UE keeps its scene textures at the largest size any
    // render asked for (on track a 2048x2048 one: 2904x2048 with DLSS off, 2048x2048 under DLSS, whose views reach
    // only 1941 across; 2026-11-04). The passes that use them are told apart by more than size: AO by its draws at
    // the views' viewports, the reflections by the composite that reads them with a cube array bound.
    const int w = static_cast<int>(d.Width), h = static_cast<int>(d.Height);
    const bool match = w >= extent[0] - 2 && h >= extent[1] - 2;
    if (!match) {  // each kind turned down once (format and size): what the scene textures really are
        static std::mutex loggedMutex;
        static std::vector<std::uint64_t> logged;
        const std::uint64_t key = (static_cast<std::uint64_t>(d.Format) << 40) | (static_cast<std::uint64_t>(w) << 20) |
                                  static_cast<std::uint64_t>(h);
        std::lock_guard guard(loggedMutex);
        if (logged.size() < 32 && std::find(logged.begin(), logged.end(), key) == logged.end()) {
            logged.push_back(key);
            simqvfr::logf("acc view share: format %d texture %dx%d (mips %u, array %u, bind 0x%x) is not a scene texture "
                        "(views reach %dx%d, target %dx%d)",
                        static_cast<int>(d.Format), w, h, d.MipLevels, d.ArraySize, d.BindFlags, extent[0], extent[1],
                        target[0], target[1]);
        }
    }
    return match;
}

bool on_immediate(ID3D11DeviceContext* ctx) {
    return ctx == immediate.load(std::memory_order_relaxed) && active.load(std::memory_order_relaxed);
}

// The view the bound viewport is (-1: none, logged once per effect), its rect in `region`.
int bound_view(ID3D11DeviceContext* ctx, Counts& counts, const char* what, simqvfr::ViewRegion& region) {
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
            simqvfr::logf("acc view share: %s at viewport %.0f,%.0f %.0fx%.0f, none of the views' (%d,%d %dx%d / %d,%d "
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
bool focus_mapping(int e, simqvfr::ViewMapping& m) {
    simqvfr::Views4Frame frame{};
    if (!views_latest_frame(frame) || frame.focusTan[e][1] <= frame.focusTan[e][0] ||
        frame.outerTan[e][1] <= frame.outerTan[e][0])
        return false;
    for (int i = 0; i < 4; ++i) {
        m.focusTan[i] = frame.focusTan[e][i];
        m.eyeTan[i] = frame.outerTan[e][i];
    }
    simqvfr::to_matrix(frame.turn[e], m.rotation);  // the focus camera is the eye's turned by turn[e]
    return true;
}

void note_shared(Counts& counts, const char* what, int view, const simqvfr::ViewRegion& from, const simqvfr::ViewRegion& to) {
    counts.shared[view - 2].fetch_add(1, std::memory_order_relaxed);
    if (!counts.firstLogged.exchange(true))
        simqvfr::logf("acc view share: focus view %d's %s from its eye's (eye %d,%d %dx%d -> focus %d,%d %dx%d)", view, what,
                    from.x, from.y, from.width, from.height, to.x, to.y, to.width, to.height);
}

// ---- SSR ----

void STDMETHODCALLTYPE dispatch_hook(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    hooked_for(ctx)->dispatch(ctx, x, y, z);
    hookCalls.dispatches.fetch_add(1, std::memory_order_relaxed);
    if (!on_immediate(ctx))
        return;
    hookCalls.dispatchesMine.fetch_add(1, std::memory_order_relaxed);
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
    simqvfr::ViewRegion region;
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
    simqvfr::ViewMapping m{};
    if (!focus_mapping(e, m) || !resampler.resample(eyeSsr[e].Get(), eyeSsrRegion[e], ssr.Get(), region, m)) {
        ssrCounts.failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    note_shared(ssrCounts, "reflections", view, eyeSsrRegion[e], region);
}

// ---- AO ----

void flush_ao();

void follow_target(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs) {
    hookCalls.targets.fetch_add(1, std::memory_order_relaxed);
    if (!on_immediate(ctx))
        return;
    hookCalls.targetsMine.fetch_add(1, std::memory_order_relaxed);
    rearm(ctx);
    if (aoTarget && (aoPending[0].set || aoPending[1].set)) {  // ACC leaves the AO texture: its views are drawn
        std::lock_guard guard(mutex);
        if (active.load(std::memory_order_relaxed))
            flush_ao();
    }
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

// A draw into the AO texture (the view's AO, just drawn): after a focus view's, its region from its eye's once ACC
// leaves the texture (flush_ao).
void on_ao_drawn(ID3D11DeviceContext* ctx, ID3D11Resource* written) {
    simqvfr::ViewRegion region;
    const int view = bound_view(ctx, aoCounts, "a draw into the AO texture", region);
    if (view < 0)
        return;
    const auto trace = [&](char action) {
        if (!aoTrace.recording)
            return;
        if (aoTrace.count == static_cast<int>(std::size(aoTrace.entries))) {
            ++aoTrace.dropped;
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(written)->GetDesc(&d);
        aoTrace.entries[aoTrace.count++] = {view, written, d.Width, d.Height, action};
    };
    if (view < 2) {
        eyeAo[view] = written;
        aoCounts.eyes.fetch_add(1, std::memory_order_relaxed);
        trace('e');
        return;
    }
    const int e = view - 2;
    int eyeRect[4];
    if (!eyeAo[e] || !views_view_rect(e, eyeRect)) {
        aoCounts.noEye.fetch_add(1, std::memory_order_relaxed);
        trace('n');
        return;
    }
    if (eyeAo[e] != written) {
        aoCounts.otherTexture.fetch_add(1, std::memory_order_relaxed);
        trace('o');
        return;
    }
    AoPending& pending = aoPending[e];
    pending.texture = static_cast<ID3D11Texture2D*>(written);  // scene_texture: a Texture2D
    pending.eye = {eyeRect[0], eyeRect[1], eyeRect[2], eyeRect[3]};
    pending.focus = region;
    pending.set = true;
    trace('p');
}

// The focus views' pending refills (under `mutex`), now that ACC has finished drawing into the AO texture.
void flush_ao() {
    for (int e = 0; e < 2; ++e) {
        AoPending& pending = aoPending[e];
        if (!pending.set)
            continue;
        pending.set = false;
        ComPtr<ID3D11Texture2D> ao = std::move(pending.texture);
        simqvfr::ViewMapping m{};
        if (!focus_mapping(e, m) || !resampler.resample_target(ao.Get(), pending.eye, ao.Get(), pending.focus, m)) {
            aoCounts.failed.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (aoTrace.recording && aoTrace.count < static_cast<int>(std::size(aoTrace.entries))) {
            D3D11_TEXTURE2D_DESC d{};
            ao->GetDesc(&d);
            aoTrace.entries[aoTrace.count++] = {e + 2, ao.Get(), d.Width, d.Height, 'r'};
        }
        note_shared(aoCounts, "ambient occlusion", e + 2, pending.eye, pending.focus);
    }
}

void STDMETHODCALLTYPE draw_indexed_hook(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base) {
    // Deferred contexts share the vtable: only the immediate one (the render thread's) touches `armed`, `aoTarget`.
    const bool mine = ctx == immediate.load(std::memory_order_relaxed);
    hookCalls.draws.fetch_add(1, std::memory_order_relaxed);
    if (mine)
        hookCalls.drawsMine.fetch_add(1, std::memory_order_relaxed);
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
    simqvfr::logf("acc view share: %s over the last 900 frames: focus views given their eye's left %llu right %llu; eye "
                "passes %llu, focus passes without an eye's %llu, into another texture than their eye's %llu, failed %llu, "
                "at no view's viewport %llu",
                what, static_cast<unsigned long long>(c.shared[0].exchange(0)),
                static_cast<unsigned long long>(c.shared[1].exchange(0)), static_cast<unsigned long long>(c.eyes.exchange(0)),
                static_cast<unsigned long long>(c.noEye.exchange(0)), static_cast<unsigned long long>(c.otherTexture.exchange(0)),
                static_cast<unsigned long long>(c.failed.exchange(0)), static_cast<unsigned long long>(c.unplaced.exchange(0)));
}

}  // namespace

void view_share_configure(bool ssr, bool ao) {
    shareSsr.store(ssr);
    shareAo.store(ao);
    if (!ssr)
        simqvfr::log_line("acc view share: reflections not shared (acc_ssr_share=0): each focus view traces its own");
    if (!ao)
        simqvfr::log_line("acc view share: ambient occlusion not shared (acc_ao_share=0): each focus view computes its own");
}

void view_share_start(ID3D11Device* device) {
    if ((!shareSsr.load() && !shareAo.load()) || !device)
        return;
    std::lock_guard guard(mutex);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    void** vtable = *reinterpret_cast<void***>(context.Get());
    if (!resampler.initialize(device)) {
        simqvfr::log_line("acc view share: resampler unavailable; each focus view computes its own effects");
        return;
    }
    const int before = hookedCount.load();
    if (!hook_vtable(vtable)) {
        simqvfr::log_line("acc view share: hooking the immediate context failed; each focus view computes its own effects");
        resampler.shutdown();
        return;
    }
    if (hookedCount.load() != before && before > 0)
        simqvfr::logf("acc view share: this session's context has another vtable (%p): hooked too", static_cast<void*>(vtable));
    immediate.store(context.Get());  // the device holds it as long as the session's
    sessionVtable = vtable;
    simqvfr::logf("acc view share: the session's immediate context %p, vtable %p (in %s)",
                static_cast<void*>(context.Get()), static_cast<void*>(vtable),
                [&] {
                    HMODULE owner = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(vtable), &owner);
                    wchar_t name[MAX_PATH] = {};
                    GetModuleFileNameW(owner, name, MAX_PATH);
                    static char narrow[MAX_PATH];
                    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, MAX_PATH, nullptr, nullptr);
                    return narrow;
                }());
    active.store(true);
    simqvfr::logf("acc view share: on (the focus views take their eyes' %s)",
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
        eyeSsrFresh[e] = false;
        eyeAo[e] = nullptr;
        aoPending[e] = AoPending{};
    }
    resampler.shutdown();
}

void view_share_frame_end() {
    if (!active.load(std::memory_order_relaxed))
        return;
    {
        std::lock_guard guard(mutex);
        flush_ao();  // any refill still pending (ACC never left the AO texture this frame)
        if (aoTrace.recording) {  // the frame just ended
            aoTrace.recording = false;
            char line[2048];
            int n = std::snprintf(line, sizeof(line), "acc view share: one frame's AO-candidate draws (view:texture "
                                                      "size action; e eye, p refill pending, r refilled, o other "
                                                      "texture, n no eye):");
            for (int i = 0; i < aoTrace.count && n < static_cast<int>(sizeof(line)) - 64; ++i) {
                const AoTrace::Entry& t = aoTrace.entries[i];
                n += std::snprintf(line + n, sizeof(line) - n, " %d:%p %ux%u %c", t.view, t.texture, t.width, t.height,
                                   t.action);
            }
            if (aoTrace.dropped)
                std::snprintf(line + n, sizeof(line) - n, " (+%d more)", aoTrace.dropped);
            simqvfr::log_line(line);
        }
        for (int e = 0; e < 2; ++e)
            eyeAo[e] = nullptr;  // an eye's AO is this frame's only
    }
    if (++frames % 900 != 0)
        return;
    {
        // The next frame, traced: at the first three reports, and at three where the views drew their AO more than once
        // a frame (on track).
        const bool many = aoCounts.eyes.load(std::memory_order_relaxed) > 2 * 900 + 200;
        std::lock_guard guard(mutex);
        if (aoTrace.reports < 3 || (many && aoTrace.trackReports < 3)) {
            ++(aoTrace.reports < 3 ? aoTrace.reports : aoTrace.trackReports);
            aoTrace.recording = true;
            aoTrace.count = aoTrace.dropped = 0;
        }
    }
    {
        std::lock_guard guard(mutex);
        void** vtable = nullptr;
        if (ID3D11DeviceContext* ctx = immediate.load(std::memory_order_relaxed))
            vtable = *reinterpret_cast<void***>(ctx);
        const bool ours = sessionVtable && sessionVtable[kSlotDrawIndexed] == reinterpret_cast<void*>(&draw_indexed_hook) &&
                          sessionVtable[kSlotDispatch] == reinterpret_cast<void*>(&dispatch_hook) &&
                          sessionVtable[kSlotOMSetRenderTargets] == reinterpret_cast<void*>(&set_targets_hook);
        simqvfr::logf("acc view share: hooks over the last 900 frames: draws %llu (on the session's context %llu), "
                    "dispatches %llu (%llu), target sets %llu (%llu), draw/dispatch slots put back %llu times; the "
                    "context's vtable %p %s the hooked one %p, whose slots are %s",
                    static_cast<unsigned long long>(hookCalls.draws.exchange(0)),
                    static_cast<unsigned long long>(hookCalls.drawsMine.exchange(0)),
                    static_cast<unsigned long long>(hookCalls.dispatches.exchange(0)),
                    static_cast<unsigned long long>(hookCalls.dispatchesMine.exchange(0)),
                    static_cast<unsigned long long>(hookCalls.targets.exchange(0)),
                    static_cast<unsigned long long>(hookCalls.targetsMine.exchange(0)),
                    static_cast<unsigned long long>(rearms.exchange(0)), static_cast<void*>(vtable),
                    vtable == sessionVtable ? "is" : "is NOT", static_cast<void*>(sessionVtable),
                    ours ? "still the hooks" : "NO LONGER the hooks (replaced)");
    }
    if (shareSsr.load(std::memory_order_relaxed))
        report(ssrCounts, "reflections");
    if (shareAo.load(std::memory_order_relaxed))
        report(aoCounts, "ambient occlusion");
}

}  // namespace acc

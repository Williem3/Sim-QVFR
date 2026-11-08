// LMU's multi-view rendering in the eye passes (mvr_pass.h). Render thread only: the hooks act on the game's
// immediate context while a pass runs and pass everything else straight through.
#include "mvr_pass.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include "../../engine/nvapi_stereo.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "mvr_probe.h"

namespace lmu {
namespace {

using Microsoft::WRL::ComPtr;

using VSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*,
                                               UINT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                      ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                             ID3D11DepthStencilView*, UINT, UINT,
                                                             ID3D11UnorderedAccessView* const*, const UINT*);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT,
                                                      DXGI_FORMAT);
template <typename Shader>
using SetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, Shader*, ID3D11ClassInstance* const*, UINT);
using SOSetTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11Buffer* const*, const UINT*);
// ID3D11DeviceContext's vtable (d3d11.h order: ID3D11DeviceChild's 7, then the context's own).
constexpr int kVSSetShader = 11, kGSSetShader = 23, kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUavs = 34,
              kSOSetTargets = 37, kRSSetViewports = 44, kRSSetScissorRects = 45, kResolveSubresource = 57,
              kHSSetShader = 60, kDSSetShader = 64;
constexpr unsigned kMaxRects = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

VSSetShaderFn realVSSetShader = nullptr;
OMSetRenderTargetsFn realOMSetRenderTargets = nullptr;
OMSetRenderTargetsAndUavsFn realOMSetRenderTargetsAndUavs = nullptr;
RSSetViewportsFn realRSSetViewports = nullptr;
RSSetScissorRectsFn realRSSetScissorRects = nullptr;
ResolveSubresourceFn realResolveSubresource = nullptr;
SetShaderFn<ID3D11GeometryShader> realGSSetShader = nullptr;
SetShaderFn<ID3D11HullShader> realHSSetShader = nullptr;
SetShaderFn<ID3D11DomainShader> realDSSetShader = nullptr;
SOSetTargetsFn realSOSetTargets = nullptr;

int nvapi_two_views(ID3D11DeviceContext* context, bool on) { return simqvfr::nvapi::set_two_views(context, on); }

// GPU time per pass, split at its multisample resolve: the scene (pass start to resolve) and the post-processing (resolve
// to pass end). Timestamps in a ring of frames, read back a few frames later without waiting.
struct GpuFrame {
    ComPtr<ID3D11Query> disjoint, stamp[4][3];  // pass start, resolve, pass end
    bool stamped[4][3] = {};
    bool pending = false;
    int mode = 0;  // mvr_frame_mode: 0 MVR, 1 the four passes alone (an A/B run)
};

// The twin of a game shader by its address, valid until the game creates another vertex shader (a new one may take a
// released one's address; twins are attached only at creation, mvr_probe.cpp): GetPrivateData once per shader instead of
// at every bind. Slots by a multiplicative hash of the address (allocations are aligned: its low bits alone collide;
// the 2026-11-08 10:08 run missed 30% of lookups with them, twice the shaders a frame binds).
struct TwinEntry {
    ID3D11VertexShader* vs = nullptr;
    ID3D11VertexShader* twin = nullptr;  // raw: the game's shader holds it (its private data) while it lives
    uint32_t epoch = 0;
};
constexpr unsigned kTwinCacheBits = 10, kTwinCache = 1u << kTwinCacheBits;

// CPU in this code's context hooks (the calls passed on included), in TSC ticks since the last log line.
struct HookTime {
    uint64_t hooks = 0, lookups = 0, switches = 0, calls = 0;
};

bool timeHooks = false;  // mvr_time_hooks

struct Timed {
    uint64_t* const sum;
    const uint64_t t0;
    explicit Timed(uint64_t& s) : sum(timeHooks ? &s : nullptr), t0(sum ? __rdtsc() : 0) {}
    ~Timed() {
        if (sum)
            *sum += __rdtsc() - t0;
    }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;
};
constexpr int kGpuFrames = 4;

struct State {
    ID3D11DeviceContext* context = nullptr;  // the game's immediate context (not referenced: the device keeps it)
    GpuFrame gpu[kGpuFrames];
    int gpuSlot = -1;            // this frame's, while it runs
    unsigned long long gpuFrameNo = 0;
    int mode = 0;                // this frame's (mvr_frame_mode)
    double sceneMs[2][4] = {}, postMs[2][4] = {};
    unsigned gpuCounts[2][4] = {};
    TwinEntry twins[kTwinCache];
    uint32_t twinEpoch = 1;
    uint64_t twinGeneration = 0;
    uint64_t loggedLookups = 0, loggedMisses = 0;  // the stats' counts at the last log line
    HookTime time;
    unsigned long long tscAtLog = 0;
    long long qpcAtLog = 0;
    ComPtr<ID3D11Buffer> remapCb;
    TwoViewsFn twoViews = &nvapi_two_views;
    bool broken = false;  // single-pass stereo refused once: off for the session
    // The pass.
    int pass = -1;
    bool drawFocus = false, suspended = false, sample = false, replaceResolve = false;
    uint64_t passTwinBinds = 0;  // this pass's draws that drew the focus view too
    MvrPassView view, viewOf[2];
    // What the game bound (raw: bound objects are kept alive by the pipeline).
    ID3D11VertexShader* gameVs = nullptr;
    ID3D11ClassInstance* instances[256] = {};
    UINT instanceCount = 0;
    bool depthBound = false;
    // A stage after the vertex shader (geometry, tessellation) or stream output: the twin's second position wouldn't
    // reach the rasterizer, so no two views then.
    bool gsBound = false, hsBound = false, dsBound = false, soBound = false;
    UINT vpCount = 0, scCount = 0;
    D3D11_VIEWPORT vps[kMaxRects] = {};
    D3D11_RECT scs[kMaxRects] = {};
    // What is applied.
    ID3D11VertexShader* boundVs = nullptr;
    bool on = false, stereo = false;
    // Each pass's multisample resolve target; the sampled regions.
    ComPtr<ID3D11Resource> resolved[4];
    ComPtr<ID3D11Texture2D> mvrImage[2], passImage[2];
    bool keepFocus = false;           // gate D
    ComPtr<ID3D11Texture2D> kept[2];  // each eye pass's resolved focus region
    bool keptOk[2] = {};
    bool have[4] = {};
    unsigned long long lastStatsFrame = 0;
    MvrPassStats stats;
} g;

bool ours(ID3D11DeviceContext* context) { return context == g.context && g.pass >= 0; }

void stamp(int pass, int which) {
    if (g.gpuSlot < 0 || pass < 0 || pass > 3)
        return;
    GpuFrame& f = g.gpu[g.gpuSlot];
    if (f.stamped[pass][which] || !f.stamp[pass][which])
        return;
    g.context->End(f.stamp[pass][which].Get());
    f.stamped[pass][which] = true;
}

// Frames whose timestamps are in: their scene and post times added up.
void gpu_collect() {
    for (GpuFrame& f : g.gpu) {
        if (!f.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (g.context->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        f.pending = false;
        if (dj.Disjoint || !dj.Frequency)
            continue;
        for (int p = 0; p < 4; ++p) {
            UINT64 t[3] = {};
            bool ok = f.stamped[p][0] && f.stamped[p][1] && f.stamped[p][2];
            for (int k = 0; k < 3 && ok; ++k)
                ok = g.context->GetData(f.stamp[p][k].Get(), &t[k], sizeof(t[k]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (!ok || t[1] < t[0] || t[2] < t[1])
                continue;
            g.sceneMs[f.mode][p] += 1000.0 * static_cast<double>(t[1] - t[0]) / static_cast<double>(dj.Frequency);
            g.postMs[f.mode][p] += 1000.0 * static_cast<double>(t[2] - t[1]) / static_cast<double>(dj.Frequency);
            ++g.gpuCounts[f.mode][p];
        }
    }
}

D3D11_VIEWPORT focus_viewport(const D3D11_VIEWPORT& v) {
    const float sx = g.view.eyeW ? static_cast<float>(g.view.focusW) / static_cast<float>(g.view.eyeW) : 1.0f;
    const float sy = g.view.eyeH ? static_cast<float>(g.view.focusH) / static_cast<float>(g.view.eyeH) : 1.0f;
    return {v.TopLeftX + v.Width, v.TopLeftY, v.Width * sx, v.Height * sy, v.MinDepth, v.MaxDepth};
}

// The game's scissor mapped like its viewport onto the focus viewport.
D3D11_RECT focus_scissor(const D3D11_RECT& r) {
    const D3D11_VIEWPORT& v = g.vps[0];
    const D3D11_VIEWPORT f = focus_viewport(v);
    const float sx = v.Width > 0 ? f.Width / v.Width : 1.0f, sy = v.Height > 0 ? f.Height / v.Height : 1.0f;
    const auto x = [&](LONG at) { return static_cast<LONG>(std::lround(f.TopLeftX + (static_cast<float>(at) - v.TopLeftX) * sx)); };
    const auto y = [&](LONG at) { return static_cast<LONG>(std::lround(f.TopLeftY + (static_cast<float>(at) - v.TopLeftY) * sy)); };
    return {x(r.left), y(r.top), x(r.right), y(r.bottom)};
}

void apply_viewports() {
    if (g.on && g.vpCount == 1) {
        const D3D11_VIEWPORT two[2] = {g.vps[0], focus_viewport(g.vps[0])};
        realRSSetViewports(g.context, 2, two);
        ++g.stats.viewportRewrites;
    } else {
        realRSSetViewports(g.context, g.vpCount, g.vps);
    }
}

void apply_scissors() {
    if (g.on && g.scCount == 1 && g.vpCount == 1) {
        const D3D11_RECT two[2] = {g.scs[0], focus_scissor(g.scs[0])};
        realRSSetScissorRects(g.context, 2, two);
        ++g.stats.scissorRewrites;
    } else if (g.scCount) {
        realRSSetScissorRects(g.context, g.scCount, g.scs);
    }
}

ID3D11VertexShader* twin_of(ID3D11VertexShader* vs) {
    if (!vs)
        return nullptr;
    Timed timed(g.time.lookups);
    ++g.stats.lookups;
    if (const uint64_t generation = mvr_shader_generation(); generation != g.twinGeneration) {
        g.twinGeneration = generation;
        ++g.twinEpoch;
    }
    TwinEntry& e = g.twins[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vs)) * 0x9E3779B97F4A7C15ull) >>
                           (64 - kTwinCacheBits)];
    if (e.epoch == g.twinEpoch && e.vs == vs)
        return e.twin;
    ++g.stats.lookupMisses;
    ID3D11VertexShader* twin = nullptr;
    IUnknown* unknown = nullptr;
    UINT size = sizeof(unknown);
    if (SUCCEEDED(vs->GetPrivateData(kMvrTwinGuid, &size, &unknown)) && unknown) {
        ComPtr<ID3D11VertexShader> t;
        unknown->QueryInterface(IID_PPV_ARGS(&t));
        unknown->Release();
        twin = t.Get();
    }
    e = {vs, twin, g.twinEpoch};
    return twin;
}

// Twin, two views and the second viewport exactly while a focus-drawing pass has a depth target and a shader with a twin.
void update() {
    ID3D11VertexShader* twin = nullptr;
    bool want = g.drawFocus && !g.broken && !g.suspended && g.depthBound && g.gameVs && !g.gsBound && !g.hsBound &&
                !g.dsBound && !g.soBound;
    if (want) {
        twin = twin_of(g.gameVs);
        want = twin != nullptr;
    }
    if (want != g.stereo) {
        Timed timed(g.time.switches);
        if (g.twoViews(g.context, want) != 0) {
            ++g.stats.stereoErrors;
            if (want) {
                g.broken = true;
                want = false;
                simqvfr::log_line("lmu mvr: single-pass stereo refused: the focus views are drawn by passes 2-3 only");
            }
        } else {
            g.stereo = want;
            ++g.stats.toggles;
        }
    }
    ID3D11VertexShader* bind = want ? twin : g.gameVs;
    if (bind != g.boundVs) {
        realVSSetShader(g.context, bind, g.instanceCount ? g.instances : nullptr, g.instanceCount);
        g.boundVs = bind;
        ++(want ? g.stats.twinBinds : g.stats.plainBinds);
        g.passTwinBinds += want ? 1 : 0;
    }
    if (want != g.on) {
        g.on = want;
        if (want)
            g.context->VSSetConstantBuffers(13, 1, g.remapCb.GetAddressOf());
        apply_viewports();
        apply_scissors();
    }
}

void STDMETHODCALLTYPE vs_set_shader(ID3D11DeviceContext* context, ID3D11VertexShader* vs,
                                     ID3D11ClassInstance* const* instances, UINT count) {
    if (!ours(context)) {
        realVSSetShader(context, vs, instances, count);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    g.gameVs = vs;
    g.instanceCount = std::min<UINT>(count, 256);
    for (UINT i = 0; i < g.instanceCount; ++i)
        g.instances[i] = instances ? instances[i] : nullptr;
    g.boundVs = reinterpret_cast<ID3D11VertexShader*>(~uintptr_t{0});  // the game's call always takes effect
    update();
}

template <typename Shader, SetShaderFn<Shader>* real, bool State::*bound>
void STDMETHODCALLTYPE later_stage(ID3D11DeviceContext* context, Shader* shader, ID3D11ClassInstance* const* instances,
                                   UINT count) {
    if (!ours(context)) {
        (*real)(context, shader, instances, count);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    (*real)(context, shader, instances, count);
    g.*bound = shader != nullptr;
    update();
}

void STDMETHODCALLTYPE so_set_targets(ID3D11DeviceContext* context, UINT n, ID3D11Buffer* const* buffers,
                                      const UINT* offsets) {
    if (!ours(context)) {
        realSOSetTargets(context, n, buffers, offsets);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realSOSetTargets(context, n, buffers, offsets);
    g.soBound = false;
    for (UINT i = 0; i < n && buffers; ++i)
        g.soBound = g.soBound || buffers[i] != nullptr;
    update();
}

void STDMETHODCALLTYPE om_set_render_targets(ID3D11DeviceContext* context, UINT n, ID3D11RenderTargetView* const* rtvs,
                                             ID3D11DepthStencilView* dsv) {
    if (!ours(context)) {
        realOMSetRenderTargets(context, n, rtvs, dsv);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realOMSetRenderTargets(context, n, rtvs, dsv);
    g.depthBound = dsv != nullptr;
    update();
}

void STDMETHODCALLTYPE om_set_render_targets_and_uavs(ID3D11DeviceContext* context, UINT n,
                                                      ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                                      UINT uavStart, UINT uavCount,
                                                      ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (!ours(context)) {
        realOMSetRenderTargetsAndUavs(context, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realOMSetRenderTargetsAndUavs(context, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        g.depthBound = dsv != nullptr;
        update();
    }
}

void STDMETHODCALLTYPE rs_set_viewports(ID3D11DeviceContext* context, UINT n, const D3D11_VIEWPORT* vps) {
    if (!ours(context)) {
        realRSSetViewports(context, n, vps);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    g.vpCount = std::min<UINT>(n, kMaxRects);
    if (vps)
        std::memcpy(g.vps, vps, g.vpCount * sizeof(D3D11_VIEWPORT));
    apply_viewports();
}

void STDMETHODCALLTYPE rs_set_scissor_rects(ID3D11DeviceContext* context, UINT n, const D3D11_RECT* rects) {
    if (!ours(context)) {
        realRSSetScissorRects(context, n, rects);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    g.scCount = std::min<UINT>(n, kMaxRects);
    if (rects)
        std::memcpy(g.scs, rects, g.scCount * sizeof(D3D11_RECT));
    if (!g.scCount)
        realRSSetScissorRects(context, 0, nullptr);
    else
        apply_scissors();
}

bool copy_region(ComPtr<ID3D11Texture2D>& dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w, unsigned h);

// The resolve target cleared (its view made once: it holds the resource, so the pointer stays its own).
ComPtr<ID3D11RenderTargetView> clearRtv;
ID3D11Resource* clearFor = nullptr;

void clear_target(ID3D11Resource* target) {
    if (clearFor != target) {
        clearRtv.Reset();
        clearFor = target;
        ComPtr<ID3D11Device> device;
        g.context->GetDevice(&device);
        if (FAILED(device->CreateRenderTargetView(target, nullptr, &clearRtv)))
            clearRtv.Reset();
    }
    if (clearRtv) {
        const float black[4] = {0, 0, 0, 0};
        g.context->ClearRenderTargetView(clearRtv.Get(), black);
    }
}

void STDMETHODCALLTYPE resolve_subresource(ID3D11DeviceContext* context, ID3D11Resource* dst, UINT dstSub,
                                           ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format) {
    bool multisample = false;
    if (ours(context) && g.pass <= 3 && src && dst) {
        ComPtr<ID3D11Texture2D> source;
        if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&source)))) {
            D3D11_TEXTURE2D_DESC d;
            source->GetDesc(&d);
            multisample = d.SampleDesc.Count > 1;
        }
    }
    const int eye = g.pass & 1;
    if (multisample)
        stamp(g.pass, 1);  // the scene ends, the post-processing starts
    if (multisample && g.replaceResolve && g.keptOk[eye]) {
        // Gate D: the scene this pass skipped is the focus view its eye's pass drew: into the top-left instead. Cleared
        // first: the post-processing covers the top-left at one scale, beyond the focus view where pass 1's images still
        // are (they would bloom into it).
        const MvrPassView& v = g.viewOf[eye];
        clear_target(dst);
        const D3D11_BOX box{0, 0, 0, v.focusW, v.focusH, 1};
        g.context->CopySubresourceRegion(dst, dstSub, 0, 0, 0, g.kept[eye].Get(), 0, &box);
        g.resolved[g.pass] = dst;
        ++g.stats.replacedResolves;
        return;
    }
    realResolveSubresource(context, dst, dstSub, src, srcSub, format);
    if (!multisample)
        return;
    g.resolved[g.pass] = dst;
    ++g.stats.resolves;
    if (g.keepFocus && g.pass < 2 && g.drawFocus && g.passTwinBinds > 0) {
        g.keptOk[eye] = copy_region(g.kept[eye], dst, g.view.eyeW, 0, g.view.focusW, g.view.focusH);
        g.stats.keptRegions += g.keptOk[eye] ? 1 : 0;
    }
}

bool hook(void* target, void* detour, void** original) {
    return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

// The game's bindings as the pass starts (the hooks only see later changes).
void read_bindings() {
    ID3D11VertexShader* vs = nullptr;
    UINT count = 256;
    g.context->VSGetShader(&vs, g.instances, &count);
    g.instanceCount = vs ? count : 0;
    for (UINT i = 0; i < g.instanceCount; ++i)
        if (g.instances[i])
            g.instances[i]->Release();  // raw, like the game's own bindings
    if (vs)
        vs->Release();
    g.gameVs = g.boundVs = vs;
    ID3D11DepthStencilView* dsv = nullptr;
    g.context->OMGetRenderTargets(0, nullptr, &dsv);
    g.depthBound = dsv != nullptr;
    if (dsv)
        dsv->Release();
    g.vpCount = kMaxRects;
    g.context->RSGetViewports(&g.vpCount, g.vps);
    g.scCount = kMaxRects;
    g.context->RSGetScissorRects(&g.scCount, g.scs);
    ID3D11GeometryShader* gs = nullptr;
    g.context->GSGetShader(&gs, nullptr, nullptr);
    g.gsBound = gs != nullptr;
    ID3D11HullShader* hs = nullptr;
    g.context->HSGetShader(&hs, nullptr, nullptr);
    g.hsBound = hs != nullptr;
    ID3D11DomainShader* ds = nullptr;
    g.context->DSGetShader(&ds, nullptr, nullptr);
    g.dsBound = ds != nullptr;
    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT] = {};
    g.context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
    g.soBound = false;
    for (ID3D11Buffer* b : so)
        if (b) {
            g.soBound = true;
            b->Release();
        }
    for (IUnknown* u : {static_cast<IUnknown*>(gs), static_cast<IUnknown*>(hs), static_cast<IUnknown*>(ds)})
        if (u)
            u->Release();
}

// `dst` (made or re-made at w x h in `src`'s format) = src's region at (x, y).
bool copy_region(ComPtr<ID3D11Texture2D>& dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w, unsigned h) {
    ComPtr<ID3D11Texture2D> source;
    if (!src || FAILED(src->QueryInterface(IID_PPV_ARGS(&source))) || !w || !h)
        return false;
    D3D11_TEXTURE2D_DESC d;
    source->GetDesc(&d);
    if (x + w > d.Width || y + h > d.Height || d.SampleDesc.Count != 1)
        return false;
    if (dst) {
        D3D11_TEXTURE2D_DESC have;
        dst->GetDesc(&have);
        if (have.Width != w || have.Height != h || have.Format != d.Format)
            dst.Reset();
    }
    if (!dst) {
        D3D11_TEXTURE2D_DESC c = d;
        c.Width = w;
        c.Height = h;
        c.MipLevels = 1;
        c.ArraySize = 1;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = 0;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        ComPtr<ID3D11Device> device;
        g.context->GetDevice(&device);
        if (FAILED(device->CreateTexture2D(&c, nullptr, &dst)))
            return false;
    }
    const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    g.context->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
    return true;
}

bool read_halves(ID3D11Texture2D* texture, std::vector<uint16_t>& out, unsigned& w, unsigned& h) {
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS)
        return false;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&s, nullptr, &staging)))
        return false;
    g.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    w = d.Width;
    h = d.Height;
    out.resize(static_cast<size_t>(w) * h * 4);
    for (unsigned y = 0; y < h; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, static_cast<const uint8_t*>(map.pData) + y * map.RowPitch,
                    static_cast<size_t>(w) * 8);
    g.context->Unmap(staging.Get(), 0);
    return true;
}

float half_to_float(uint16_t v) {
    const uint32_t sign = (v >> 15) & 1, exponent = (v >> 10) & 0x1f, mantissa = v & 0x3ff;
    float f;
    if (exponent == 0)
        f = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        f = mantissa ? 0.0f : 65504.0f;  // NaN as 0, infinity as the largest half
    else
        f = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -f : f;
}

std::vector<float> luminance(const std::vector<uint16_t>& px, unsigned w, unsigned h) {
    std::vector<float> l(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < l.size(); ++i)
        l[i] = std::max(0.0f, 0.2126f * half_to_float(px[i * 4]) + 0.7152f * half_to_float(px[i * 4 + 1]) +
                                  0.0722f * half_to_float(px[i * 4 + 2]));
    return l;
}

// For looking at: x / (1 + x), gamma 2.2, BGRA.
std::vector<uint8_t> tonemapped(const std::vector<uint16_t>& px, unsigned w, unsigned h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = std::max(0.0f, half_to_float(px[i * 4 + c]));
            out[i * 4 + 2 - c] = static_cast<uint8_t>(std::lround(255.0f * std::pow(v / (1.0f + v), 1.0f / 2.2f)));
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

void compare_sample(unsigned long long frame) {
    ++g.stats.samples;
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int eye = 0; eye < 2; ++eye) {
        std::vector<uint16_t> a, b;
        unsigned aw = 0, ah = 0, bw = 0, bh = 0;
        if (!g.have[eye] || !g.have[2 + eye] || !read_halves(g.mvrImage[eye].Get(), a, aw, ah) ||
            !read_halves(g.passImage[eye].Get(), b, bw, bh) || aw != bw || ah != bh) {
            simqvfr::logf("lmu mvr: frame %llu: eye %d's focus images not both copied (%d %d); not compared", frame, eye,
                          g.have[eye], g.have[2 + eye]);
            continue;
        }
        const MvrComparison c = mvr_compare(a, b, aw, ah);
        simqvfr::logf("lmu mvr: frame %llu: focus %d drawn in pass %d (MVR) vs pass %d, %ux%u: aligned at %+d,%+d px; "
                      "brightness %.4f (cell median %.4f); mean difference %.2f%%; %.2f%% of pixels differ >10%%",
                      frame, eye, eye, 2 + eye, aw, ah, c.shiftX, c.shiftY, c.brightness, c.cellMedian, 100 * c.meanDiff,
                      c.differingPct);
        if (g.stats.samples == 1) {
            wchar_t name[96];
            swprintf_s(name, L"lmu-mvr-frame%llu-focus%d-mvr.png", frame, eye);
            simqvfr::write_png_bgra((dir / name).wstring(), tonemapped(a, aw, ah), aw, ah);
            swprintf_s(name, L"lmu-mvr-frame%llu-focus%d-pass.png", frame, eye);
            simqvfr::write_png_bgra((dir / name).wstring(), tonemapped(b, bw, bh), bw, bh);
            simqvfr::logf("lmu mvr: images in %ls (lmu-mvr-frame%llu-focus*-mvr/pass.png)", dir.c_str(), frame);
        }
    }
}

}  // namespace

bool mvr_pass_install(ID3D11Device* device) {
    if (g.context)
        return true;
    if (!device)
        return false;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    const D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const float identity[4] = {1, 0, 1, 0};
    const D3D11_SUBRESOURCE_DATA init = {identity, 0, 0};
    if (FAILED(device->CreateBuffer(&cd, &init, &g.remapCb)))
        return false;
    const D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tq = {D3D11_QUERY_TIMESTAMP, 0};
    for (GpuFrame& f : g.gpu) {
        device->CreateQuery(&dq, &f.disjoint);
        for (auto& pass : f.stamp)
            for (auto& q : pass)
                device->CreateQuery(&tq, &q);
    }
    if (const MH_STATUS s = MH_Initialize(); s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    void** vtable = *reinterpret_cast<void***>(context.Get());
    const bool ok =
        hook(vtable[kVSSetShader], reinterpret_cast<void*>(&vs_set_shader), reinterpret_cast<void**>(&realVSSetShader)) &&
        hook(vtable[kOMSetRenderTargets], reinterpret_cast<void*>(&om_set_render_targets),
             reinterpret_cast<void**>(&realOMSetRenderTargets)) &&
        hook(vtable[kOMSetRenderTargetsAndUavs], reinterpret_cast<void*>(&om_set_render_targets_and_uavs),
             reinterpret_cast<void**>(&realOMSetRenderTargetsAndUavs)) &&
        hook(vtable[kRSSetViewports], reinterpret_cast<void*>(&rs_set_viewports),
             reinterpret_cast<void**>(&realRSSetViewports)) &&
        hook(vtable[kRSSetScissorRects], reinterpret_cast<void*>(&rs_set_scissor_rects),
             reinterpret_cast<void**>(&realRSSetScissorRects)) &&
        hook(vtable[kResolveSubresource], reinterpret_cast<void*>(&resolve_subresource),
             reinterpret_cast<void**>(&realResolveSubresource)) &&
        hook(vtable[kGSSetShader],
             reinterpret_cast<void*>(&later_stage<ID3D11GeometryShader, &realGSSetShader, &State::gsBound>),
             reinterpret_cast<void**>(&realGSSetShader)) &&
        hook(vtable[kHSSetShader], reinterpret_cast<void*>(&later_stage<ID3D11HullShader, &realHSSetShader, &State::hsBound>),
             reinterpret_cast<void**>(&realHSSetShader)) &&
        hook(vtable[kDSSetShader],
             reinterpret_cast<void*>(&later_stage<ID3D11DomainShader, &realDSSetShader, &State::dsBound>),
             reinterpret_cast<void**>(&realDSSetShader)) &&
        hook(vtable[kSOSetTargets], reinterpret_cast<void*>(&so_set_targets), reinterpret_cast<void**>(&realSOSetTargets));
    if (!ok) {
        simqvfr::log_line("lmu mvr: the context couldn't be hooked; off");
        return false;
    }
    g.context = context.Get();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g.qpcAtLog = now.QuadPart;
    g.tscAtLog = __rdtsc();
    simqvfr::log_line("lmu mvr: on: passes 0-1 draw their focus views too (single-pass stereo, beside the eye)");
    return true;
}

void mvr_pass_begin(int pass, const MvrPassView* view, bool sample, bool replaceResolve) {
    if (!g.context)
        return;
    Timed timed(g.time.hooks);
    g.pass = pass;
    g.drawFocus = view != nullptr && !g.broken;
    g.suspended = false;
    g.passTwinBinds = 0;
    if (pass == 0) {  // this frame's timestamps, in a slot whose last frame has been read
        if (g.gpuSlot >= 0) {  // the frame before stopped short of pass 3: its timing left out
            g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
            g.gpuSlot = -1;
        }
        gpu_collect();
        const int slot = static_cast<int>(g.gpuFrameNo++ % kGpuFrames);
        GpuFrame& f = g.gpu[slot];
        g.gpuSlot = -1;
        if (!f.pending && f.disjoint) {
            for (auto& p : f.stamped)
                for (bool& s : p)
                    s = false;
            f.mode = g.mode;
            g.context->Begin(f.disjoint.Get());
            g.gpuSlot = slot;
        }
    }
    stamp(pass, 0);
    g.replaceResolve = replaceResolve && pass >= 2 && g.keptOk[pass & 1];
    if (pass < 2)
        g.keptOk[pass] = false;
    if (pass == 0) {
        g.sample = sample;
        for (bool& h : g.have)
            h = false;
    }
    if (pass >= 0 && pass < 4)
        g.resolved[pass].Reset();
    if (view) {
        g.view = *view;
        g.viewOf[view->eye & 1] = *view;
        g.context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, view->remap, 0, 0);
        ++g.stats.passesWithFocus;
    }
    read_bindings();
    update();
}

void mvr_pass_end(int pass) {
    if (!g.context || g.pass < 0)
        return;
    Timed timed(g.time.hooks);
    g.drawFocus = false;
    update();  // the game's shader, one view, its own viewports
    stamp(pass, 2);
    if (pass == 3 && g.gpuSlot >= 0) {
        g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
        g.gpu[g.gpuSlot].pending = true;
        g.gpuSlot = -1;
    }
    if (g.sample && !g.keepFocus && pass >= 0 && pass < 4) {  // gate C's comparison (gate D's is four_passes.cpp's)
        const int eye = pass & 1;
        const MvrPassView& v = g.viewOf[eye];
        if (pass < 2)
            g.have[pass] = copy_region(g.mvrImage[eye], g.resolved[pass].Get(), v.eyeW, 0, v.focusW, v.focusH);
        else
            g.have[pass] = copy_region(g.passImage[eye], g.resolved[pass].Get(), 0, 0, v.focusW, v.focusH);
    }
    g.pass = -1;
    g.replaceResolve = false;
}

void mvr_keep_focus(bool on) { g.keepFocus = on; }

bool mvr_focus_ready(int eye) { return g.context && g.keepFocus && g.keptOk[eye & 1]; }

void mvr_suspend(bool on) {
    if (!g.context || g.pass < 0)
        return;
    g.suspended = on;
    update();
}

void mvr_frame_end(unsigned long long frame) {
    if (!g.context)
        return;
    if (g.sample) {
        g.sample = false;
        if (!g.keepFocus)
            compare_sample(frame);
    }
    if (frame >= g.lastStatsFrame + 900) {
        const double frames = static_cast<double>(frame - g.lastStatsFrame);
        g.lastStatsFrame = frame;
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        const unsigned long long tsc = __rdtsc();
        const double ms = 1000.0 * static_cast<double>(now.QuadPart - g.qpcAtLog) / static_cast<double>(f.QuadPart);
        if (timeHooks && ms > 0 && tsc > g.tscAtLog && frames > 0) {
            const double perMs = static_cast<double>(tsc - g.tscAtLog) / ms;
            const auto per = [&](uint64_t ticks) { return static_cast<double>(ticks) / perMs / frames; };
            simqvfr::logf("lmu mvr: this code's CPU in the context hooks %.3f ms per frame, the calls passed on included "
                          "(twin lookups %.3f: %llu, %llu not cached; stereo switches %.3f); %.0f hooked calls per frame",
                          per(g.time.hooks), per(g.time.lookups),
                          static_cast<unsigned long long>(g.stats.lookups - g.loggedLookups),
                          static_cast<unsigned long long>(g.stats.lookupMisses - g.loggedMisses), per(g.time.switches),
                          static_cast<double>(g.time.calls) / frames);
        }
        g.time = HookTime{};
        g.loggedLookups = g.stats.lookups;
        g.loggedMisses = g.stats.lookupMisses;
        g.qpcAtLog = now.QuadPart;
        g.tscAtLog = tsc;
        const MvrPassStats& s = g.stats;
        simqvfr::logf("lmu mvr: %llu passes drew their focus view; shader binds %llu twin / %llu plain, %llu stereo "
                      "switches (%llu refused), viewports %llu and scissors %llu doubled; focus regions kept %llu, "
                      "resolves replaced %llu; twin lookups %llu, %llu not cached",
                      static_cast<unsigned long long>(s.passesWithFocus), static_cast<unsigned long long>(s.twinBinds),
                      static_cast<unsigned long long>(s.plainBinds), static_cast<unsigned long long>(s.toggles),
                      static_cast<unsigned long long>(s.stereoErrors), static_cast<unsigned long long>(s.viewportRewrites),
                      static_cast<unsigned long long>(s.scissorRewrites), static_cast<unsigned long long>(s.keptRegions),
                      static_cast<unsigned long long>(s.replacedResolves), static_cast<unsigned long long>(s.lookups),
                      static_cast<unsigned long long>(s.lookupMisses));
        for (int m = 0; m < 2; ++m) {
            if (!g.gpuCounts[m][0])
                continue;
            const double* sc = g.sceneMs[m];
            const double* po = g.postMs[m];
            const unsigned* n = g.gpuCounts[m];
            const auto avg = [](double sum, unsigned k) { return k ? sum / k : 0.0; };
            double total = 0;
            for (int p = 0; p < 4; ++p)
                total += avg(sc[p] + po[p], n[p]);
            simqvfr::logf("lmu mvr: GPU per pass%s, scene / post-processing (to the resolve / after it): pass 0 %.2f / %.2f, "
                          "pass 1 %.2f / %.2f, pass 2 %.2f / %.2f, pass 3 %.2f / %.2f ms, %.2f in all (%u frames timed)",
                          m ? " without MVR (A/B)" : "", avg(sc[0], n[0]), avg(po[0], n[0]), avg(sc[1], n[1]), avg(po[1], n[1]),
                          avg(sc[2], n[2]), avg(po[2], n[2]), avg(sc[3], n[3]), avg(po[3], n[3]), total, n[0]);
            for (int p = 0; p < 4; ++p) {
                g.sceneMs[m][p] = g.postMs[m][p] = 0;
                g.gpuCounts[m][p] = 0;
            }
        }
    }
}

void mvr_set_two_views(TwoViewsFn fn) { g.twoViews = fn ? fn : &nvapi_two_views; }

void mvr_frame_mode(int mode) { g.mode = mode ? 1 : 0; }

void mvr_twins_changed() { ++g.twinEpoch; }

void mvr_time_hooks(bool on) { timeHooks = on; }

MvrComparison mvr_compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, unsigned w, unsigned h,
                          int range) {
    MvrComparison c;
    if (a.size() != static_cast<size_t>(w) * h * 4 || b.size() != a.size() || w < 64 || h < 64)
        return c;
    const std::vector<float> la = luminance(a, w, h), lb = luminance(b, w, h);
    const int border = range + 8;
    double best = -1, bestMeanA = 0;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            double diff = 0, sumA = 0;
            for (int y = border; y < static_cast<int>(h) - border; y += 4)
                for (int x = border; x < static_cast<int>(w) - border; x += 4) {
                    const float va = la[static_cast<size_t>(y) * w + x];
                    const float vb = lb[static_cast<size_t>(y + dy) * w + (x + dx)];
                    diff += std::fabs(va - vb);
                    sumA += va;
                }
            if (best < 0 || diff < best) {
                best = diff;
                bestMeanA = sumA;
                c.shiftX = dx;
                c.shiftY = dy;
            }
        }
    c.meanDiff = bestMeanA > 0 ? best / bestMeanA : 0;
    double sumA = 0, sumB = 0;
    size_t differing = 0, counted = 0;
    constexpr int kCells = 16;
    std::vector<double> cellA(kCells * kCells, 0), cellB(kCells * kCells, 0);
    for (int y = border; y < static_cast<int>(h) - border; ++y)
        for (int x = border; x < static_cast<int>(w) - border; ++x) {
            const float va = la[static_cast<size_t>(y) * w + x];
            const float vb = lb[static_cast<size_t>(y + c.shiftY) * w + (x + c.shiftX)];
            sumA += va;
            sumB += vb;
            ++counted;
            if (std::fabs(va - vb) > 0.1f * std::max(va, vb) && std::max(va, vb) > 1e-4f)
                ++differing;
            const int cell = (y * kCells / static_cast<int>(h)) * kCells + x * kCells / static_cast<int>(w);
            cellA[cell] += va;
            cellB[cell] += vb;
        }
    c.brightness = sumA > 0 ? sumB / sumA : 0;
    std::vector<double> ratios;
    for (int i = 0; i < kCells * kCells; ++i)
        if (cellA[i] > 1e-6)
            ratios.push_back(cellB[i] / cellA[i]);
    std::sort(ratios.begin(), ratios.end());
    c.cellMedian = ratios.empty() ? 0 : ratios[ratios.size() / 2];
    c.differingPct = counted ? 100.0 * static_cast<double>(differing) / static_cast<double>(counted) : 0;
    c.ok = true;
    return c;
}

MvrPassStats mvr_pass_stats() { return g.stats; }

}  // namespace lmu

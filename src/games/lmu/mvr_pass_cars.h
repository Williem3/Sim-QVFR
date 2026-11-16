// Part of mvr_pass.cpp's translation unit (included once, in order): car tracking (bindings followed, the per-draw
// buffer read) and car slots (slot target, slot twins, blend state twins, the car part draws). Included only by
// mvr_pass.cpp, inside its anonymous namespace (lmu::<anonymous>), after mvr_pass_twins.h; never include it anywhere
// else: the per-draw path stays in one translation unit (no whole-program optimization). No includes of its own:
// mvr_pass.cpp's.
#pragma once

// ---- Car tracking (mvr_car_track; car_motion.h): bindings followed, the per-draw buffer read as the game writes it ----

// Car tracking's sections are many (~7,000 a frame on track) and short: timing each would cost about as much as their
// work (a timed section ~17 ns, lmu_car_motion_test --bench), so 1 in 16 is timed and counted 16 times, each site with
// a counter of its own (the matrix read and the draw alternate in the eye passes: one counter for both timed one of
// them for long stretches, run 22), less the two timestamp reads' own ticks inside the section (timedEmptyTicks).
uint32_t carSampleRead = 0, carSampleDraw = 0, carSampleHook = 0;
uint64_t timedEmptyTicks = 0;  // the least a section with nothing in it measures (mvr_time_hooks)
struct SampledTimed {
    uint64_t* const sum;
    const uint64_t t0;
    SampledTimed(uint64_t& s, uint32_t& counter)
        : sum(timeHooks && (++counter & 15) == 0 ? &s : nullptr), t0(sum ? __rdtsc() : 0) {}
    ~SampledTimed() {
        if (sum) {
            const uint64_t d = __rdtsc() - t0;
            *sum += (d > timedEmptyTicks ? d - timedEmptyTicks : 0) * 16;
        }
    }
    SampledTimed(const SampledTimed&) = delete;
    SampledTimed& operator=(const SampledTimed&) = delete;
};

// The handlers get the original of the function the game called (LeanDetours, below) and call it themselves.
using VsCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using IaVbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
using IaIbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);

// Hooked whenever MVR is (not only for car tracking): the game's own b13 recorded, and our remap bound again over a
// game bind of b13 while two views are on (the twins read it there).
inline void vs_set_constant_buffers(VsCbFn real, ID3D11DeviceContext* context, UINT start, UINT n,
                                    ID3D11Buffer* const* buffers) {
    if (context != g.context || g.replaying) {
        real(context, start, n, buffers);
        return;
    }
    SampledTimed whole(g.time.carHooks, carSampleHook);
    if (g.carTrack) {
        if (timeHooks)
            ++g.time.carCalls;
        for (UINT i = 0; i < n && start + i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i)
            g.vsCb[start + i] = buffers ? buffers[i] : nullptr;
    }
    // (a call past the last slot is the runtime's to drop: not recorded)
    const bool covers13 = start <= 13 && n > 13 - start && start + n <= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    ID3D11Buffer* const b13 = covers13 && buffers ? buffers[13 - start] : nullptr;
    if (covers13 && b13 != g.remapCb.Get())  // our remap given back (a Get/Set round trip): not the game's
        hold(g.gameVsCb13, b13);
    real(context, start, n, buffers);
    if (covers13 && g.on && g.pass >= 0)
        real(context, 13, 1, g.remapCb.GetAddressOf());
}

// Map and Unmap: any resource but the per-draw buffer passes with one comparison.
inline HRESULT map_resource(MapFn real, ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, D3D11_MAP type,
                            UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    SampledTimed whole(g.time.carHooks, carSampleHook);
    const HRESULT hr = real(context, resource, sub, type, flags, mapped);
    if (timeHooks)
        ++g.time.carCalls;
    if (resource == g.worldRes && resource && context == g.context && SUCCEEDED(hr) && mapped)
        g.worldMapped = mapped->pData;
    return hr;
}

// The per-draw buffer's matrix, read as its update ends (its memory is still ours to read until the call passes on).
inline void unmap_resource(UnmapFn real, ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub) {
    SampledTimed whole(g.time.carHooks, carSampleHook);
    if (timeHooks)
        ++g.time.carCalls;
    if (resource == g.worldRes && g.worldMapped && g.carActive && context == g.context) {
        SampledTimed timed(g.time.carReads, carSampleRead);
        std::memcpy(g.matrix, static_cast<const uint8_t*>(g.worldMapped) + g.worldCbOffset, sizeof(g.matrix));
        g.worldMapped = nullptr;
        g.carMatrix = true;
        ++g.stats.carMatrixReads;
    }
    real(context, resource, sub);
}

inline void ia_set_vertex_buffers(IaVbFn real, ID3D11DeviceContext* context, UINT start, UINT n,
                                  ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
    SampledTimed whole(g.time.carHooks, carSampleHook);
    if (context == g.context && !g.replaying) {
        if (timeHooks)
            ++g.time.carCalls;
        if (start == 0 && n > 0) {
            g.iaVb = buffers ? buffers[0] : nullptr;
            g.iaVbOffset = offsets ? offsets[0] : 0;
        }
    }
    real(context, start, n, buffers, strides, offsets);
}

inline void ia_set_index_buffer(IaIbFn real, ID3D11DeviceContext* context, ID3D11Buffer* buffer, DXGI_FORMAT format,
                                UINT offset) {
    SampledTimed whole(g.time.carHooks, carSampleHook);
    if (context == g.context && !g.replaying) {
        if (timeHooks)
            ++g.time.carCalls;
        g.iaIb = buffer;
        g.iaIbOffset = offset;
    }
    real(context, buffer, format, offset);
}

// ---- Car slots (mvr_car_slots): the slot target beside the scene's, the slot twins, the blend states' twins ----

// The blend state twins, under the game's state as private data: [0] target 1 kept (the draw doesn't write depth: a
// transparent leaves the surface's slot), [1] target 1 written. Each: independent blend on, the game's targets as it set
// them (with independent blend off, target 0's for every target), target 1 unblended.
constexpr GUID kSlotBlendTwinGuid[2] = {
    {0x4f7d2a18, 0x93c5, 0x4e6b, {0xb0, 0x21, 0x7c, 0x5e, 0x08, 0xd4, 0x3a, 0x96}},
    {0xd18e6c03, 0x2b47, 0x4a9f, {0x85, 0x6d, 0x1e, 0xa3, 0x70, 0xc9, 0x5b, 0x24}}};

D3D11_BLEND_DESC blend_twin_desc(const D3D11_BLEND_DESC& game, bool write) {
    D3D11_BLEND_DESC t = game;
    if (!game.IndependentBlendEnable)
        for (int i = 1; i < 8; ++i)
            t.RenderTarget[i] = game.RenderTarget[0];
    t.IndependentBlendEnable = TRUE;
    t.RenderTarget[1] = {FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO,
                         D3D11_BLEND_OP_ADD, static_cast<UINT8>(write ? D3D11_COLOR_WRITE_ENABLE_RED : 0)};
    return t;
}

// The twin of the game's blend state `game` (null: D3D11's default), made the first time it's needed.
ID3D11BlendState* blend_twin_looked_up(ID3D11BlendState* game, bool write);

// blend_twin_looked_up, remembered for the last game state this frame (the game binds a few states over and over).
ID3D11BlendState* blend_twin(ID3D11BlendState* game, bool write) {
    if (!game)
        return g.defaultBlendTwin[write ? 1 : 0].Get();
    if (game != g.blendLook.game)
        g.blendLook = {game, {nullptr, nullptr}};
    ID3D11BlendState*& twin = g.blendLook.twin[write ? 1 : 0];
    if (!twin)
        twin = blend_twin_looked_up(game, write);
    return twin;
}

ID3D11BlendState* blend_twin_looked_up(ID3D11BlendState* game, bool write) {
    IUnknown* unknown = nullptr;
    UINT size = sizeof(unknown);
    if (SUCCEEDED(game->GetPrivateData(kSlotBlendTwinGuid[write ? 1 : 0], &size, &unknown)) && unknown) {
        ComPtr<ID3D11BlendState> t;
        unknown->QueryInterface(IID_PPV_ARGS(&t));
        unknown->Release();
        return t.Get();  // raw: the game's state holds it
    }
    D3D11_BLEND_DESC desc;
    game->GetDesc(&desc);
    const D3D11_BLEND_DESC twin = blend_twin_desc(desc, write);
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    ComPtr<ID3D11BlendState> t;
    if (FAILED(device->CreateBlendState(&twin, &t)))
        return game;  // as the game's: target 1 as it would be
    game->SetPrivateDataInterface(kSlotBlendTwinGuid[write ? 1 : 0], t.Get());
    ++g.stats.blendTwins;
    return t.Get();
}

void realOMSetBlendState(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT* factor, UINT mask) {
    call_real<kHookBlend>(c, bs, factor, mask);
}

// The blend state the draws should have now: the game's, or its twin while the slot target is bound.
void apply_blend() {
    if (g.slotsBound) {
        realOMSetBlendState(g.context, blend_twin(g.gameBlend, g.depthWrites), g.blendFactor, g.sampleMask);
        g.blendSwapped = true;
    } else if (g.blendSwapped) {
        realOMSetBlendState(g.context, g.gameBlend, g.blendFactor, g.sampleMask);
        g.blendSwapped = false;
    }
}

bool depth_writes(ID3D11DepthStencilState* ds) {
    if (!ds)
        return true;  // D3D11's default: depth on, written
    if (g.depthLook.known && ds == g.depthLook.ds)  // the last state looked at this frame
        return g.depthLook.writes;
    D3D11_DEPTH_STENCIL_DESC d;
    ds->GetDesc(&d);
    g.depthLook = {ds, d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL, true};
    return g.depthLook.writes;
}

using BlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT*, UINT);
using DepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);

inline void om_set_blend_state(BlendFn real, ID3D11DeviceContext* context, ID3D11BlendState* bs, const FLOAT* factor,
                               UINT mask) {
    if (context == g.context && !g.replaying) {
        hold(g.gameBlend, bs);
        for (int i = 0; i < 4; ++i)
            g.blendFactor[i] = factor ? factor[i] : 1.0f;
        g.sampleMask = mask;
        if (g.slotsBound) {
            Timed timed(g.time.slotState);
            real(context, blend_twin(bs, g.depthWrites), g.blendFactor, mask);
            g.blendSwapped = true;
            return;
        }
        g.blendSwapped = false;
    }
    real(context, bs, factor, mask);
}

inline void om_set_depth_stencil_state(DepthFn real, ID3D11DeviceContext* context, ID3D11DepthStencilState* ds,
                                       UINT ref) {
    real(context, ds, ref);
    if (context == g.context && !g.replaying) {
        const bool writes = depth_writes(ds);
        if (writes != g.depthWrites) {
            g.depthWrites = writes;
            if (g.slotsBound) {
                Timed timed(g.time.slotState);
                apply_blend();
            }
        }
    }
}

// Is render target view `rtv` of the scene's target? With MSAA off the tracker's learned single-sample target; with
// MSAA on the open pass's multisample target, until its resolve (the pass began at its bind with a depth target, and
// the game binds no other multisample target with one before the resolve). The last view asked is remembered with its
// texture's description (the slot target is made to it).
bool is_scene_rtv(ID3D11RenderTargetView* rtv) {
    if (rtv != g.sceneRtvChecked) {
        g.sceneRtvChecked = rtv;
        g.sceneRtvResource = nullptr;
        g.sceneRtvDesc = {};
        ComPtr<ID3D11Resource> r;
        rtv->GetResource(&r);
        ComPtr<ID3D11Texture2D> texture;
        if (r && SUCCEEDED(r.As(&texture))) {
            g.sceneRtvResource = r.Get();
            texture->GetDesc(&g.sceneRtvDesc);
        }
    }
    g.sceneCheckedAgainst = tr.scene;
    if (g.sceneRtvDesc.SampleDesc.Count > 1)  // (the caller is inside a pass: with the tracker driving, its own open one)
        g.sceneRtvIsScene = !g.afterResolve && !(tr.on && (!tr.open || tr.singleSample || tr.resolved));
    else
        g.sceneRtvIsScene = tr.scene && g.sceneRtvResource == tr.scene && tr.sceneDesc.SampleDesc.Count == 1;
    return g.sceneRtvIsScene;
}

// The slot target at the scene target's size and sample count (made again if they changed): render targets bound
// together must share both. With MSAA on it is multisample too (each sample takes the part's slot; DLSS's vectors read
// sample 0, as they do of the depth).
bool ensure_slot_target() {
    const UINT w = g.sceneRtvDesc.Width, h = g.sceneRtvDesc.Height;
    const UINT samples = (std::max)(g.sceneRtvDesc.SampleDesc.Count, 1u), quality = g.sceneRtvDesc.SampleDesc.Quality;
    if (g.slotTex && g.slotW == w && g.slotH == h && g.slotSamples == samples && g.slotQuality == quality)
        return true;
    g.slotTex.Reset();
    g.slotRtv.Reset();
    g.slotSrv.Reset();
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    UINT levels = 1;
    if (samples > 1 && (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R16_UINT, samples, &levels)) || quality >= levels)) {
        g.slotsOn = false;
        simqvfr::logf("lmu mvr: car slots: no R16_UINT target at the scene's %ux multisampling (quality %u of %u levels); off: the "
                      "other cars keep the world's vectors",
                      samples, quality, levels);
        return false;
    }
    const D3D11_TEXTURE2D_DESC t = {w, h, 1, 1, DXGI_FORMAT_R16_UINT, {samples, quality}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (!w || !h || FAILED(device->CreateTexture2D(&t, nullptr, &g.slotTex)) ||
        FAILED(device->CreateRenderTargetView(g.slotTex.Get(), nullptr, &g.slotRtv)) ||
        FAILED(device->CreateShaderResourceView(g.slotTex.Get(), nullptr, &g.slotSrv))) {
        g.slotTex.Reset();
        g.slotsOn = false;
        simqvfr::logf("lmu mvr: car slots: the %ux%u x%u slot target couldn't be made; off", w, h, samples);
        return false;
    }
    g.slotW = w;
    g.slotH = h;
    g.slotSamples = samples;
    g.slotQuality = quality;
    simqvfr::logf("lmu mvr: car slots: slot target %ux%u x%u (R16_UINT, %.0f MB)", w, h, samples, w * 2.0 * h * samples / 1048576.0);
    return true;
}

// The slot view for this draw at t127: slot `slot`'s (its word slot + 1), or none (-1: the draw isn't a car part with
// a frame before; unbound reads 0).
uint32_t carSampleView = 0;  // bind_slot_view's 1 in 16 (SampledTimed's counter)
void bind_slot_view(int slot) {
    ID3D11ShaderResourceView* view =
        slot >= 0 && static_cast<size_t>(slot) < g.slotViews.size() ? g.slotViews[slot].Get() : nullptr;
    if (view == g.slotViewBound)
        return;
    SampledTimed timed(g.time.slotViews, carSampleView);
    g.context->PSSetShaderResources(127, 1, &view);
    g.slotViewBound = view;
    if (view) {
        ++g.stats.slotViews;
        ++g.slotViewsThisPass;
    }
}

// After the game's bind of render targets in a pass: the scene's target alone with a depth target gets the slot target
// beside it (cleared over the pass's views the first time); anything else goes without.
void slot_bind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g.slotsOn)
        return;
    Timed timed(g.time.slotState);
    const bool was = g.slotsBound;
    g.slotsBound = false;
    if (n == 1 && rtvs && rtvs[0] && dsv && g.carActive && is_scene_rtv(rtvs[0]) && ensure_slot_target()) {
        if (!g.slotsCleared && g.context1) {
            const UINT fw = g.view.focusRenderW ? g.view.focusRenderW : g.view.focusW;
            const UINT fh = g.view.focusRenderH ? g.view.focusRenderH : g.view.focusH;
            const D3D11_RECT rects[2] = {{0, 0, static_cast<LONG>(g.view.eyeW), static_cast<LONG>(g.view.eyeH)},
                                         {static_cast<LONG>(g.view.eyeW), 0, static_cast<LONG>(g.view.eyeW + fw),
                                          static_cast<LONG>(fh)}};
            const FLOAT zero[4] = {};
            g.context1->ClearView(g.slotRtv.Get(), zero, rects, g.drawFocus && fw && fh ? 2 : 1);
            g.slotsCleared = true;
            ++g.stats.slotPasses;
        }
        if (g.slotsCleared) {
            ID3D11RenderTargetView* two[2] = {rtvs[0], g.slotRtv.Get()};
            realOMSetRenderTargets(g.context, 2, two, dsv);
            g.slotsBound = true;
            ++g.stats.slotBinds;
        }
    }
    if (was != g.slotsBound) {
        if (!g.slotsBound)
            bind_slot_view(-1);
        apply_blend();
        update_ps();
    }
}

// The slot target off the pipeline (before the post reads the scene, and at a pass's end): the game's targets as bound,
// without it.
void slot_unbind() {
    if (!g.slotsBound)
        return;
    Timed timed(g.time.slotState);
    ID3D11RenderTargetView* rtvs[2] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g.context->OMGetRenderTargets(2, rtvs, &dsv);
    realOMSetRenderTargets(g.context, 1, rtvs, dsv);
    for (ID3D11RenderTargetView* r : rtvs)
        if (r)
            r->Release();
    if (dsv)
        dsv->Release();
    g.slotsBound = false;
    bind_slot_view(-1);
    apply_blend();
    update_ps();
}

// Is this vertex buffer dynamic (the game's ring)? D3D is asked once a frame per buffer: an eye pass changes its vertex
// buffer at ~550 of its ~1,400 draws but has ~160 of them (lmu-ai_frame11731.rdc), and each GetDesc is a cache miss in
// the runtime's object. The answer is the frame's alone (a buffer freed since can't pass for one checked before at its
// address).
bool vb_dynamic(ID3D11Buffer* vb) {
    if (!vb)
        return false;
    const size_t i = static_cast<size_t>((reinterpret_cast<uintptr_t>(vb) * 0x9E3779B97F4A7C15ull) >> 55);  // 9 bits
    auto& seen = g.vbSeen[i & (g.vbSeen.size() - 1)];
    if (seen.vb == vb && seen.frame == g.carFrame)
        return seen.dynamic;
    D3D11_BUFFER_DESC d{};
    vb->GetDesc(&d);
    seen = {vb, g.carFrame, d.Usage == D3D11_USAGE_DYNAMIC};
    ++g.stats.carVbAsked;
    return seen.dynamic;
}

// A scene draw in an eye pass whose vertex shader reads a worldMatrix: its key and the matrix last written into the
// per-draw buffer go to the car parts' record. The per-draw buffer is learned from such a draw (the buffer at the
// shader's worldMatrix slot); the draw that teaches it has no matrix yet.
int car_draw(UINT count, UINT start, INT base) {
    if (!g.carTrack || g.vsWorldSlot < 0 || !g.depthBound)
        return -1;
    ID3D11Buffer* cb = g.vsCb[g.vsWorldSlot];
    if (!cb)
        return -1;
    if (cb != g.worldCb) {
        D3D11_BUFFER_DESC d{};
        cb->GetDesc(&d);
        if (d.ByteWidth < g.vsWorldOffset + sizeof(g.matrix)) {  // the matrix wouldn't fit in it: never read past its end
            ++g.stats.carNoMatrix;
            return -1;
        }
        g.worldCbHeld = cb;  // held: its address never another resource's while it's the one compared
        g.worldCb = cb;
        g.worldRes = cb;
        g.worldCbOffset = g.vsWorldOffset;
        g.worldMapped = nullptr;
        g.carMatrix = false;
        ++g.stats.carBufferLearned;
        return -1;
    }
    if (!g.carMatrix) {
        ++g.stats.carNoMatrix;
        return -1;
    }
    if (g.vsWorldOffset != g.worldCbOffset) {  // read at the learned offset only (one copy per Unmap)
        ++g.stats.carOtherOffset;
        return -1;
    }
    // A dynamic vertex buffer is a ring the game refills each frame (lmu-ai_frame11731.rdc: a 1.4 MB one, mapped 246
    // times in the frame, ~100 car-side draws): its draws' base vertex and offset move with the ring, so they're keyed by
    // the buffer, the index buffer and the index range alone (unique within a frame there; the index buffer's ranges
    // stay put). Checked once per vertex buffer bound (consecutive draws share one).
    if (g.iaVb != g.vbChecked) {
        g.vbChecked = g.iaVb;
        g.vbDynamic = vb_dynamic(g.iaVb);
    }
    CarDrawKey key{g.iaVb, g.iaIb, g.iaVbOffset, g.iaIbOffset, start, count, base};
    if (g.vbDynamic) {
        key.vbOffset = 0;
        key.base = 0;
        ++g.stats.carDynamicVb;
    }
    bool moved = false;
    const int slot = g.cars.draw(key, g.matrix, moved);
    return moved ? slot : -1;  // the slot to bind (car slots): a part with a frame before
}

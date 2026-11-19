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
    if (cb_track()) {
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
    if (context != g.context)
        return real(context, resource, sub, type, flags, mapped);
    SampledTimed whole(g.time.carHooks, carSampleHook);
    const HRESULT hr = real(context, resource, sub, type, flags, mapped);
    if (timeHooks)
        ++g.time.carCalls;
    if (resource == g.worldRes && resource && context == g.context && SUCCEEDED(hr) && mapped)
        g.worldMapped = mapped->pData;
    // Night car lighting: the floodlights' buffer, rewritten whole before each draw (WRITE_DISCARD), is written into
    // our copy, which goes to the driver's memory at the Unmap: the lights read from ordinary memory, not the driver's
    // write-combined memory (slow to read).
    if (resource == g.omniRes && resource && SUCCEEDED(hr) && mapped && mapped->pData && type == D3D11_MAP_WRITE_DISCARD &&
        sub == 0 && g.omniShadow.size() >= g.omniBytes && g.omniBytes) {
        g.omniReal = mapped->pData;
        mapped->pData = g.omniShadow.data();
    } else if (resource == g.omniRes && resource) {
        g.omniValid = false;  // written another way: our copy no longer what the draws read
    }
    return hr;
}

// The per-draw buffer's matrix, read as its update ends (its memory is still ours to read until the call passes on).
inline void unmap_resource(UnmapFn real, ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub) {
    if (context != g.context) {
        real(context, resource, sub);
        return;
    }
    SampledTimed whole(g.time.carHooks, carSampleHook);
    if (timeHooks)
        ++g.time.carCalls;
    if (resource == g.worldRes && g.worldMapped && (g.carActive || g.viewTrack) && context == g.context) {
        SampledTimed timed(g.time.carReads, carSampleRead);
        const auto* data = static_cast<const uint8_t*>(g.worldMapped);
        std::memcpy(g.matrix, data + g.worldCbOffset, sizeof(g.matrix));
        if (g.viewTrack && g.viewCbOffset != ~0u) {  // the draw's camera too (offsets checked against the buffer's size)
            std::memcpy(g.viewPair, data + g.viewCbOffset, 64);
            std::memcpy(g.viewPair + 16, data + g.viewProjCbOffset, 64);
            g.viewPairRead = true;
        }
        g.worldMapped = nullptr;
        g.carMatrix = true;
        ++g.stats.carMatrixReads;
    }
    if (resource == g.omniRes && g.omniReal && resource) {  // night car lighting: our copy to the driver's memory
        std::memcpy(g.omniReal, g.omniShadow.data(), g.omniBytes);
        g.omniReal = nullptr;
        ++g.omniWrites;
        g.omniValid = true;
    }
    real(context, resource, sub);
}

inline void ia_set_vertex_buffers(IaVbFn real, ID3D11DeviceContext* context, UINT start, UINT n,
                                  ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
    if (context != g.context) {
        real(context, start, n, buffers, strides, offsets);
        return;
    }
    SampledTimed whole(g.time.carHooks, carSampleHook);
    if (!g.replaying) {
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
    if (context != g.context) {
        real(context, buffer, format, offset);
        return;
    }
    SampledTimed whole(g.time.carHooks, carSampleHook);
    if (!g.replaying) {
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
    if (game != g.blendLook.game.Get()) {
        g.blendLook = {};
        g.blendLook.game = game;
    }
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

bool slot_writes();  // below

// The blend state the draws should have now: the game's, or its twin while the slot target is bound.
void apply_blend() {
    if (g.slotsBound) {
        realOMSetBlendState(g.context, blend_twin(g.gameBlend, slot_writes()), g.blendFactor, g.sampleMask);
        g.blendSwapped = true;
    } else if (g.blendSwapped) {
        realOMSetBlendState(g.context, g.gameBlend, g.blendFactor, g.sampleMask);
        g.blendSwapped = false;
    }
}

bool depth_writes(ID3D11DepthStencilState* ds) {
    if (!ds)
        return true;  // D3D11's default: depth on, written
    if (g.depthLook.known && ds == g.depthLook.ds.Get())  // the last state looked at this frame
        return g.depthLook.writes;
    D3D11_DEPTH_STENCIL_DESC d;
    ds->GetDesc(&d);
    g.depthLook.ds = ds;
    g.depthLook.writes = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
    g.depthLook.known = true;
    return g.depthLook.writes;
}

// Does blend state `bs` blend into target 0 (a transparent's; null, D3D11's default: no)? An "opaque" blend (one, zero)
// or a target it doesn't write isn't blending.
bool blends_colour(ID3D11BlendState* bs) {
    if (!bs)
        return false;
    D3D11_BLEND_DESC desc;
    bs->GetDesc(&desc);
    const D3D11_RENDER_TARGET_BLEND_DESC& t = desc.RenderTarget[0];
    if (!t.BlendEnable || !(t.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_ALL))
        return false;
    return !(t.SrcBlend == D3D11_BLEND_ONE && t.DestBlend == D3D11_BLEND_ZERO && t.BlendOp == D3D11_BLEND_OP_ADD);
}

// The car slots' target 1 is written where the draw writes depth, but not by a transparent while DLSS reads the depth
// from before the transparents: the slot stays the surface's behind the glass, as the depth does.
bool slot_writes() { return g.depthWrites && !(g.glass.mode && g.glass.read && g.glass.blends); }

// DLSS's depth before the transparents (mvr_pass_glass.h). After the game's bind of a depth target in an eye pass:
// whether it is the pass's scene depth (as bound at its begin).
void glass_note_bind(ID3D11DepthStencilView* dsv) {
    auto& gl = g.glass;
    gl.dsv = nullptr;
    gl.sceneBound = false;
    if (!dsv || g.pass < 0 || g.pass > 1)
        return;
    ComPtr<ID3D11Resource> depthTarget;
    dsv->GetResource(&depthTarget);
    gl.sceneBound = depthTarget && depthTarget.Get() == g.mask.sceneDepth[g.pass];
    if (gl.sceneBound)
        gl.dsv = dsv;  // raw: bound now, used before the next bind only
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
        if (g.glass.mode)  // DLSS's depth before the transparents (mvr_pass_glass.h)
            g.glass.blends = blends_colour(bs);
        if (g.slotsBound) {
            Timed timed(g.time.slotState);
            real(context, blend_twin(bs, slot_writes()), g.blendFactor, mask);
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
// View tracking: the pair last written into the per-draw buffer, kept for this eye pass when not one it has already (an
// eye pass draws its own camera and the mirrors' and side targets': view_pair.h tells them apart later).
void keep_view_pair() {
    if (!g.viewTrack || !g.viewPairRead || g.pass < 0 || g.pass > 1 || g.vsViewOffset != g.viewCbOffset ||
        g.vsViewProjOffset != g.viewProjCbOffset)
        return;
    State::PassPairs& p = g.passPairs[g.pass];
    if (p.frame != g.carFrame) {
        p.frame = g.carFrame;
        p.count = p.next = 0;
        p.last = -1;
    }
    const uint32_t world = car_matrix_identity(g.matrix) ? 1u : 0u;  // a static world draw
    // Most draws repeat the draw before's camera: that one first, the rest only when it changed.
    if (p.last >= 0 && std::memcmp(p.pair[p.last], g.viewPair, sizeof(g.viewPair)) == 0) {
        p.worldDraws[p.last] += world;
        return;
    }
    for (int i = 0; i < p.count; ++i)
        if (std::memcmp(p.pair[i], g.viewPair, sizeof(g.viewPair)) == 0) {
            p.last = i;
            p.worldDraws[i] += world;
            return;
        }
    const int at = p.count < State::kViewPairs ? p.count++ : p.next;
    p.next = (at + 1) % State::kViewPairs;
    std::memcpy(p.pair[at], g.viewPair, sizeof(g.viewPair));
    p.worldDraws[at] = world;
    p.last = at;
}

// Night car lighting (car_light.h): the floodlights this car part's draw was handed, kept for its slot; the
// floodlights' buffer learned from the first such draw (its next write is the first read).
uint32_t carLightSampleCounter = 0;
void car_light_record(unsigned slot) {
    if (g.vsOmniSlot < 0 || !g.vsOmniCount)
        return;
    const bool timed = (++carLightSampleCounter & 15) == 0;
    const uint64_t t0 = timed ? __rdtsc() : 0;
    ID3D11Buffer* cb = g.vsCb[g.vsOmniSlot];
    if (cb && cb != g.omniRes) {
        D3D11_BUFFER_DESC d{};
        cb->GetDesc(&d);
        const UINT need = (std::max)(g.vsOmniPos, g.vsOmniColour) + 16 * g.vsOmniCount;
        if (d.Usage == D3D11_USAGE_DYNAMIC && d.ByteWidth >= need && d.ByteWidth <= 65536) {
            g.omniHeld = cb;
            g.omniRes = cb;
            g.omniBytes = d.ByteWidth;
            g.omniShadow.assign(d.ByteWidth, 0);
            g.omniValid = false;
            g.omniReal = nullptr;
            simqvfr::logf("lmu car light: the floodlights' buffer learned (%u bytes, %u lights at %u and %u)", d.ByteWidth,
                          g.vsOmniCount, g.vsOmniPos, g.vsOmniColour);
        }
    }
    if (cb == g.omniRes && g.omniValid) {
        const float origin[3] = {g.matrix[12], g.matrix[13], g.matrix[14]};
        const OmniLayout layout{g.vsOmniPos, g.vsOmniColour, g.vsOmniCount};
        const bool paint = g.gamePs && ps_entry(g.gamePs).paint;
        g.carLights.draw(slot, origin, g.omniShadow.data(), g.omniShadow.size(), layout, g.omniWrites, paint);
        ++g.carLightRecords;
    } else {
        ++g.carLightMisses;
    }
    if (timed)
        g.carLightTicks += (__rdtsc() - t0) * 16;
}

int car_draw(UINT count, UINT start, INT base) {
    if (!cb_track() || g.vsWorldSlot < 0 || !g.depthBound)
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
        // The view matrices beside it, where this draw's shader reads them and the buffer holds them whole.
        const bool views = g.vsViewOffset != ~0u && g.vsViewProjOffset != ~0u && d.ByteWidth >= g.vsViewOffset + 64 &&
                           d.ByteWidth >= g.vsViewProjOffset + 64;
        g.viewCbOffset = views ? g.vsViewOffset : ~0u;
        g.viewProjCbOffset = views ? g.vsViewProjOffset : ~0u;
        g.viewPairRead = false;
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
    keep_view_pair();
    if (!g.carTrack || !g.carActive)  // view tracking alone (or the cars' A/B side without them): no car table
        return -1;
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
    if (moved && g.carLightOn)
        car_light_record(static_cast<unsigned>(slot));
    return moved ? slot : -1;  // the slot to bind (car slots): a part with a frame before
}

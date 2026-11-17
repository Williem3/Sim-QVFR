// Part of mvr_pass.cpp's translation unit (included once, in order): the detours per function, their installation
// (MinHook, every mode of d3d11's) and the bindings read as a pass starts. Included only by mvr_pass.cpp, inside its
// anonymous namespace (lmu::<anonymous>), after mvr_pass_hooks_entry.h; never include it anywhere else: the per-draw
// path stays in one translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

// The detours of hook H's functions, one per function (N): straight through inside the handling of one (depth), else
// the handler, which passes the call on through call_real.
template <int H, auto Handler, typename... A>
struct Detours {
    template <int N>
    static void STDMETHODCALLTYPE fn(ID3D11DeviceContext* context, A... a) {
        if (depth[H] > 0) {
            reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(functions[H].real[N])(context, a...);
            return;
        }
        ++depth[H];
        Handler(context, a...);
        --depth[H];
    }
    template <std::size_t... N>
    static void* get(int n, std::index_sequence<N...>) {
        void* const table[] = {reinterpret_cast<void*>(&fn<static_cast<int>(N)>)...};
        return table[n];
    }
    static void* get(int n) { return get(n, std::make_index_sequence<kFunctions>{}); }
};

// Car tracking's detours: its ~30,000 calls a frame on track (Map and Unmap ~90% of them, ~4 per draw, nearly all of
// other buffers: lmu-ai_frame11731.rdc) cost as little as they can. No depth count: their work is the same whether or
// not one function of an entry calls another (the matrix read once: the inner Unmap finds it taken; bindings stored
// twice), and none of them calls a hooked function. The handler gets the original of the function the game called
// (no lookup through the vtable) and calls it.
template <int H, auto Handler, typename R, typename... A>
struct LeanDetours {
    using Fn = R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...);
    template <int N>
    static R STDMETHODCALLTYPE fn(ID3D11DeviceContext* context, A... a) {
        return Handler(reinterpret_cast<Fn>(functions[H].real[N]), context, a...);
    }
    template <std::size_t... N>
    static void* get(int n, std::index_sequence<N...>) {
        void* const table[] = {reinterpret_cast<void*>(&fn<static_cast<int>(N)>)...};
        return table[n];
    }
    static void* get(int n) { return get(n, std::make_index_sequence<kFunctions>{}); }
};

// RSGetViewports' detours: the caller's return address (its image) tells the game's reads, answered with its own
// viewports, from ours (mvr_pass_binds.h rs_get_viewports).
struct ViewportReadDetours {
    template <int N>
    static void STDMETHODCALLTYPE fn(ID3D11DeviceContext* context, UINT* n, D3D11_VIEWPORT* vps) {
        rs_get_viewports(reinterpret_cast<RSGetViewportsFn>(functions[kHookVPGet].real[N]), _ReturnAddress(), context,
                         n, vps);
    }
    template <std::size_t... N>
    static void* get(int n, std::index_sequence<N...>) {
        void* const table[] = {reinterpret_cast<void*>(&fn<static_cast<int>(N)>)...};
        return table[n];
    }
    static void* get(int n) { return get(n, std::make_index_sequence<kFunctions>{}); }
};

void* detour_for(int h, int n) {
    using Instances = ID3D11ClassInstance* const*;
    switch (h) {
        case kHookVS: return Detours<kHookVS, &vs_set_shader, ID3D11VertexShader*, Instances, UINT>::get(n);
        case kHookGS:
            return Detours<kHookGS, &later_stage<kHookGS, ID3D11GeometryShader, &State::gsBound>, ID3D11GeometryShader*,
                           Instances, UINT>::get(n);
        case kHookOM:
            return Detours<kHookOM, &om_set_render_targets, UINT, ID3D11RenderTargetView* const*,
                           ID3D11DepthStencilView*>::get(n);
        case kHookOMUav:
            return Detours<kHookOMUav, &om_set_render_targets_and_uavs, UINT, ID3D11RenderTargetView* const*,
                           ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*>::get(n);
        case kHookSO: return Detours<kHookSO, &so_set_targets, UINT, ID3D11Buffer* const*, const UINT*>::get(n);
        case kHookVP: return Detours<kHookVP, &rs_set_viewports, UINT, const D3D11_VIEWPORT*>::get(n);
        case kHookSC: return Detours<kHookSC, &rs_set_scissor_rects, UINT, const D3D11_RECT*>::get(n);
        case kHookResolve:
            return Detours<kHookResolve, &resolve_subresource, ID3D11Resource*, UINT, ID3D11Resource*, UINT,
                           DXGI_FORMAT>::get(n);
        case kHookHS:
            return Detours<kHookHS, &later_stage<kHookHS, ID3D11HullShader, &State::hsBound>, ID3D11HullShader*,
                           Instances, UINT>::get(n);
        case kHookDS:
            return Detours<kHookDS, &later_stage<kHookDS, ID3D11DomainShader, &State::dsBound>, ID3D11DomainShader*,
                           Instances, UINT>::get(n);
        case kHookCopy:
            return Detours<kHookCopy, &copy_subresource_region, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                           const D3D11_BOX*>::get(n);
        case kHookDrawIndexed: return Detours<kHookDrawIndexed, &draw_indexed, UINT, UINT, INT>::get(n);
        case kHookDraw: return Detours<kHookDraw, &draw_traced<kHookDraw, UINT, UINT>, UINT, UINT>::get(n);
        case kHookDrawIndexedInstanced:
            return Detours<kHookDrawIndexedInstanced, &draw_traced<kHookDrawIndexedInstanced, UINT, UINT, UINT, INT, UINT>,
                           UINT, UINT, UINT, INT, UINT>::get(n);
        case kHookDrawInstanced:
            return Detours<kHookDrawInstanced, &draw_traced<kHookDrawInstanced, UINT, UINT, UINT, UINT>, UINT, UINT, UINT,
                           UINT>::get(n);
        case kHookDrawAuto: return Detours<kHookDrawAuto, &undoubled_draw<kHookDrawAuto>>::get(n);
        case kHookDrawIndexedIndirect:
            return Detours<kHookDrawIndexedIndirect, &undoubled_draw<kHookDrawIndexedIndirect, ID3D11Buffer*, UINT>,
                           ID3D11Buffer*, UINT>::get(n);
        case kHookDrawInstancedIndirect:
            return Detours<kHookDrawInstancedIndirect, &undoubled_draw<kHookDrawInstancedIndirect, ID3D11Buffer*, UINT>,
                           ID3D11Buffer*, UINT>::get(n);
        case kHookPS: return Detours<kHookPS, &ps_set_shader, ID3D11PixelShader*, Instances, UINT>::get(n);
        case kHookVSCB:
            return LeanDetours<kHookVSCB, &vs_set_constant_buffers, void, UINT, UINT, ID3D11Buffer* const*>::get(n);
        case kHookMap:
            return LeanDetours<kHookMap, &map_resource, HRESULT, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                               D3D11_MAPPED_SUBRESOURCE*>::get(n);
        case kHookUnmap: return LeanDetours<kHookUnmap, &unmap_resource, void, ID3D11Resource*, UINT>::get(n);
        case kHookIAVB:
            return LeanDetours<kHookIAVB, &ia_set_vertex_buffers, void, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                               const UINT*>::get(n);
        case kHookIAIB:
            return LeanDetours<kHookIAIB, &ia_set_index_buffer, void, ID3D11Buffer*, DXGI_FORMAT, UINT>::get(n);
        case kHookBlend:
            return LeanDetours<kHookBlend, &om_set_blend_state, void, ID3D11BlendState*, const FLOAT*, UINT>::get(n);
        case kHookDepth:
            return LeanDetours<kHookDepth, &om_set_depth_stencil_state, void, ID3D11DepthStencilState*, UINT>::get(n);
        case kHookExecute: return Detours<kHookExecute, &execute_command_list, ID3D11CommandList*, BOOL>::get(n);
        case kHookVPGet: return ViewportReadDetours::get(n);
        default: return nullptr;
    }
}

// The draws and CopySubresourceRegion only for the focus post (mvr_focus_replay), the draws also for instancing (doubled
// there), DrawAuto and the indirect draws only for instancing; both kinds of draw already where instancing may take over
// from single-pass stereo (it happens mid-pass, where d3d11's other modes can't be looked for): without either MVR's
// hooks are as before.
bool wanted(int h) {
    if (h == kHookPS)
        return g.biasHooks || g.slotsOn;
    if (h == kHookVSCB)  // always: the remap at b13 kept over the game's binds of it
        return true;
    if (is_car_hook(h))
        return g.carTrack;
    if (is_slot_hook(h))
        return g.slotsOn;
    if (is_undoubled_draw(h))
        return g.instanced || g.canInstance;
    if (h == kHookDrawIndexed && g.carTrack)  // car tracking's draws (LMU's scene draws are all DrawIndexed)
        return true;
    if (is_draw(h))
        return g.focusReplay || g.instanced || g.canInstance;
    return h != kHookCopy || g.focusReplay;
}

// The function the context's entry for hook `h` holds now, hooked in d3d11's code if not seen before (render thread).
// True: a new one.
bool hook_entry(int h) {
    if (!wanted(h))
        return false;
    void* fn = (*reinterpret_cast<void** const*>(g.context))[kSlotOf[h]];
    Functions& f = functions[h];
    for (int i = 0; i < f.count; ++i)
        if (f.target[i] == fn)
            return false;
    if (f.count == kFunctions) {
        if (!f.full)
            simqvfr::logf("lmu mvr: the context's %s entry holds a fifth function: calls through it aren't seen",
                          kHookName[h]);
        f.full = true;
        return false;
    }
    const int n = f.count;
    f.target[n] = fn;
    if (MH_CreateHook(fn, detour_for(h, n), &f.real[n]) == MH_OK && MH_EnableHook(fn) == MH_OK) {
        ++f.hooked;
        ++g.stats.contextFunctions;
        if (h == kHookResolve)
            ++g.stats.resolveFunctions;
    } else {
        f.real[n] = fn;  // passed on as it is, not tried again
        simqvfr::logf("lmu mvr: the context's %s function %p couldn't be hooked", kHookName[h], fn);
    }
    ++f.count;  // last: call_real reads target and real up to it
    return true;
}

int hook_entries() {
    int fresh = 0;
    for (int h = 0; h < kHooks; ++h)
        fresh += hook_entry(h) ? 1 : 0;
    return fresh;
}

// Every function the wanted entries hold: as they are; once idle (a flush) and busy (a command of ours); and the same
// with multithread protection on when it is off (on, then off again: it only adds locking meanwhile). With protection on
// already, turning it off here could race another thread: its plain functions are hooked when they first appear (a
// pass's start). At install, and when more entries are wanted later (instancing turned on between passes).
void hook_every_mode() {
    const float identity[4] = {1, 0, 1, 0};
    const auto idle_and_busy = [&] {
        hook_entries();
        g.context->Flush();
        hook_entries();
        g.context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, identity, 0, 0);
        hook_entries();
    };
    idle_and_busy();
    g.protectedNow = g.multithread && g.multithread->GetMultithreadProtected();
    if (g.multithread && !g.protectedNow) {
        g.multithread->SetMultithreadProtected(TRUE);
        idle_and_busy();
        g.multithread->SetMultithreadProtected(FALSE);
        hook_entries();
    }
}

void unhook_entries() {
    for (Functions& f : functions) {
        for (int i = 0; i < f.count; ++i)
            if (f.real[i] != f.target[i])
                MH_RemoveHook(f.target[i]);
        f = Functions{};
    }
}

// The pixel shader bound now and its class instances, held in g (the Get's references given back).
void read_ps_binding() {
    ID3D11PixelShader* ps = nullptr;
    ID3D11ClassInstance* got[256] = {};
    UINT count = 256;
    g.context->PSGetShader(&ps, got, &count);
    count = std::min<UINT>(count, 256);
    hold(g.gamePs, ps);
    hold_instances(g.psInstances, g.psInstanceCount, got, ps ? count : 0);
    for (UINT i = 0; i < count; ++i)
        if (got[i])
            got[i]->Release();
    if (ps)
        ps->Release();
    g.boundPs = ps;
}

// The game's bindings as the pass starts (the hooks only see later changes).
void read_bindings() {
    if (g.biasHooks || g.slotsOn)  // the pixel shader bound as the pass starts (set outside the passes, not seen)
        read_ps_binding();
    if (g.slotsOn) {  // car slots: the blend state (its twin goes in its place) and whether the depth state writes
        ID3D11BlendState* bs = nullptr;
        g.context->OMGetBlendState(&bs, g.blendFactor, &g.sampleMask);
        hold(g.gameBlend, bs);
        if (bs)
            bs->Release();
        ID3D11DepthStencilState* ds = nullptr;
        UINT ref = 0;
        g.context->OMGetDepthStencilState(&ds, &ref);
        g.depthWrites = depth_writes(ds);
        if (ds)
            ds->Release();
    }
    ID3D11VertexShader* vs = nullptr;
    {
        ID3D11ClassInstance* got[256] = {};
        UINT count = 256;
        g.context->VSGetShader(&vs, got, &count);
        count = std::min<UINT>(count, 256);
        hold(g.gameVs, vs);
        hold_instances(g.instances, g.instanceCount, got, vs ? count : 0);
        for (UINT i = 0; i < count; ++i)
            if (got[i])
                got[i]->Release();
        if (vs)
            vs->Release();  // g's own reference keeps it (vs is used below)
    }
    g.boundVs = vs;
    {
        const TwinEntry* e = vs ? &shader_entry(vs) : nullptr;
        g.vsRole = e ? e->role : VsRole::none;
        g.vsWorldSlot = e ? e->worldSlot : -1;
        g.vsWorldOffset = e ? e->worldOffset : 0;
        keep_game_entry(vs, e);
    }
    if (g.carTrack) {  // car tracking: the vertex shader's constant buffers and the input assembler's buffers as bound
        ID3D11Buffer* cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        g.context->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
        for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i) {
            g.vsCb[i] = cbs[i];  // raw, like the game's own bindings
            if (cbs[i])
                cbs[i]->Release();
        }
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, vbOffset = 0;
        g.context->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOffset);
        g.iaVb = vb;
        g.iaVbOffset = vbOffset;
        if (vb)
            vb->Release();
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT ibOffset = 0;
        g.context->IAGetIndexBuffer(&ib, &format, &ibOffset);
        g.iaIb = ib;
        g.iaIbOffset = ibOffset;
        if (ib)
            ib->Release();
    }
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    g.context->OMGetRenderTargets(1, &rtv, &dsv);
    g.depthBound = dsv != nullptr;
    if (rtv)
        rtv->Release();
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

// Sample images and their comparison (copy_region, read_halves, save_slot_image, compare_sample): mvr_pass_samples.cpp.

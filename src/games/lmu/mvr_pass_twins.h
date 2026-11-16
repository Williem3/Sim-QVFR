// Part of mvr_pass.cpp's translation unit (included once, in order): the vertex and pixel shader twins (twin lookup by
// address, the bound pixel shader's twin) and the PSSetShader hook. Included only by mvr_pass.cpp, inside its anonymous
// namespace (lmu::<anonymous>), after mvr_pass_viewports.h; never include it anywhere else: the per-draw path stays in
// one translation unit (no whole-program optimization). No includes of its own: mvr_pass.cpp's.
#pragma once

const TwinEntry& shader_entry(ID3D11VertexShader* vs) {
    Timed timed(g.time.lookups);
    ++g.stats.lookups;
    if (const uint64_t generation = mvr_shader_generation(); generation != g.twinGeneration) {
        g.twinGeneration = generation;
        ++g.twinEpoch;
    }
    TwinSet& set = g.twins[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vs)) * 0x9E3779B97F4A7C15ull) >>
                           (64 - kTwinSetBits)];
    for (uint8_t k = 0; k < 2; ++k)
        if (set.entry[k].epoch == g.twinEpoch && set.entry[k].vs == vs) {
            set.next = static_cast<uint8_t>(1 - k);
            return set.entry[k];
        }
    ++g.stats.lookupMisses;
    const uint8_t k = set.entry[0].epoch != g.twinEpoch ? 0 : set.entry[1].epoch != g.twinEpoch ? 1 : set.next;
    TwinEntry& e = set.entry[k];
    set.next = static_cast<uint8_t>(1 - k);
    const auto twin_under = [&](const GUID& guid) -> ID3D11VertexShader* {
        IUnknown* unknown = nullptr;
        UINT size = sizeof(unknown);
        if (FAILED(vs->GetPrivateData(guid, &size, &unknown)) || !unknown)
            return nullptr;
        ComPtr<ID3D11VertexShader> t;
        unknown->QueryInterface(IID_PPV_ARGS(&t));
        unknown->Release();
        return t.Get();  // raw: the game's shader holds it
    };
    UINT role = 0, size = sizeof(role);
    if (FAILED(vs->GetPrivateData(kVsRoleGuid, &size, &role)) || size != sizeof(role))
        role = 0;
    e = {vs, twin_under(kMvrTwinGuid), twin_under(kMvrInstancedTwinGuid), g.twinEpoch, static_cast<VsRole>(role)};
    if (g.carTrack) {  // where it reads its worldMatrix (the probe's reflection)
        UINT world[2] = {};
        size = sizeof(world);
        if (SUCCEEDED(vs->GetPrivateData(kVsWorldMatrixGuid, &size, world)) && size == sizeof(world) &&
            world[0] < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
            e.worldSlot = static_cast<int>(world[0]);
            e.worldOffset = world[1];
        }
    }
    return e;
}

// The twin for the way the passes draw now: single-pass stereo's or the instanced one.
ID3D11VertexShader* twin_of(ID3D11VertexShader* vs) {
    if (!vs)
        return nullptr;
    const TwinEntry& e = shader_entry(vs);
    return g.instanced ? e.instanced : e.twin;
}
VsRole role_of(ID3D11VertexShader* vs) { return vs ? shader_entry(vs).role : VsRole::none; }

// A game pixel shader's twins (null: none): its mip bias twin and its two slot twins, cached by its address like the
// vertex shaders' (shader_entry).
const PsTwinEntry& ps_entry(ID3D11PixelShader* ps) {
    Timed timed(g.time.lookups);
    ++g.stats.psLookups;
    if (const uint64_t generation = mvr_pixel_shader_generation(); generation != g.psGeneration) {
        g.psGeneration = generation;
        ++g.psEpoch;
    }
    PsTwinSet& set = g.psTwins[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ps)) * 0x9E3779B97F4A7C15ull) >>
                               (64 - kTwinSetBits)];
    for (uint8_t k = 0; k < 2; ++k)
        if (set.entry[k].epoch == g.psEpoch && set.entry[k].ps == ps) {
            set.next = static_cast<uint8_t>(1 - k);
            return set.entry[k];
        }
    ++g.stats.psLookupMisses;
    const uint8_t k = set.entry[0].epoch != g.psEpoch ? 0 : set.entry[1].epoch != g.psEpoch ? 1 : set.next;
    set.next = static_cast<uint8_t>(1 - k);
    const auto twin_under = [&](const GUID& guid) -> ID3D11PixelShader* {
        IUnknown* unknown = nullptr;
        UINT size = sizeof(unknown);
        if (FAILED(ps->GetPrivateData(guid, &size, &unknown)) || !unknown)
            return nullptr;
        ComPtr<ID3D11PixelShader> t;
        unknown->QueryInterface(IID_PPV_ARGS(&t));
        unknown->Release();
        return t.Get();  // raw: the game's shader holds it
    };
    set.entry[k] = {ps, g.biasHooks ? twin_under(kMvrMipBiasTwinGuid) : nullptr,
                    g.slotsOn ? twin_under(kMvrSlotTwinGuid) : nullptr,
                    g.slotsOn && g.biasHooks ? twin_under(kMvrSlotBiasTwinGuid) : nullptr, g.psEpoch};
    return set.entry[k];
}

// The game's pixel shader, its mip bias twin while this pass draws two views with a bias (the scene's draws; not the
// overlays drawn into the eye's final after the post, which DLSS never sees), its slot twin while the slot target is
// bound (car slots), or the slot twin of its mip bias twin with both.
void update_ps() {
    if (!g.biasHooks && !g.slotsOn)
        return;
    ID3D11PixelShader* bind = g.gamePs;
    const bool wantBias = g.biasHooks && g.on && g.bias && !g.overlayTarget && g.gamePs;
    const bool wantSlot = g.slotsBound && g.gamePs;
    bool biased = false, slotted = false;
    if (wantBias || wantSlot) {
        const PsTwinEntry& e = ps_entry(g.gamePs);
        if (wantSlot && wantBias && e.slotBias) {
            bind = e.slotBias;
            biased = slotted = true;
        } else if (wantSlot && e.slot) {
            bind = e.slot;
            slotted = true;
        } else if (wantBias && e.twin) {
            bind = e.twin;
            biased = true;
        }
        if (wantSlot && !slotted)
            ++g.stats.slotNoTwin;  // its draws don't write the slot target (the frame's earlier word stays)
        if (wantBias && !biased)
            ++g.stats.psNoTwin;
    }
    if (biased) {
        // The game's pixel shaders read buffers 0 and 2-7, but it unbinds all 14 now and then (LMU: once early in each
        // eye pass, PSSetConstantBuffers(0, 14) in lmu_frame10312.rdc), which left the twins reading zeros (no
        // rectangle, no bias): bound again as two views go on, and now and then checked.
        bool rebind = !g.biasCbBound;
        if (!rebind && (++g.biasChecks & 15) == 0) {
            ID3D11Buffer* bound = nullptr;
            g.context->PSGetConstantBuffers(13, 1, &bound);
            rebind = bound != g.biasCb.Get();
            if (bound)
                bound->Release();
            g.stats.biasCbLost += rebind ? 1 : 0;
        }
        if (rebind) {
            g.context->PSSetConstantBuffers(13, 1, g.biasCb.GetAddressOf());
            g.biasCbBound = g.biasCbUsed = true;  // (the game's b13 back at the pass end)
        }
    }
    if (bind != g.boundPs) {
        realPSSetShader(g.context, bind, g.psInstanceCount ? g.psInstances : nullptr, g.psInstanceCount);
        g.boundPs = bind;
        g.stats.psTwinBinds += biased ? 1 : 0;
        g.stats.slotTwinBinds += slotted ? 1 : 0;
    }
}

void STDMETHODCALLTYPE ps_set_shader(ID3D11DeviceContext* context, ID3D11PixelShader* ps,
                                     ID3D11ClassInstance* const* instances, UINT count) {
    if (!ours(context) || (!g.biasHooks && !g.slotsOn)) {
        realPSSetShader(context, ps, instances, count);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    if (ps && ps == g.boundPs && ps != g.gamePs)  // our twin given back (a Get/Set round trip): the game's own
        ps = g.gamePs;
    hold(g.gamePs, ps);
    hold_instances(g.psInstances, g.psInstanceCount, instances, std::min<UINT>(count, 256));
    g.boundPs = reinterpret_cast<ID3D11PixelShader*>(~uintptr_t{0});  // the game's call always takes effect
    update_ps();
}

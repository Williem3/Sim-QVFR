// The focus views' lighting (replay_internal.h has the file map): light tiles, sky, volumetric fog, deferred sun
// shadow and the atmosphere maps, each prepared and recorded at the Close of the game list with its pass.
#include "replay_internal.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../../log.h"
#include "replay_shaders.h"

namespace simqvfr::replay_detail {

// ---- R2b: the focus views' light tiles ------------------------------------------------------------------------------

D3D12_CPU_DESCRIPTOR_HANDLE game_cpu(UINT index) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = focus.gameHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * focus.increment;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE game_gpu(UINT index) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = focus.gameHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64{index} * focus.increment;
    return h;
}

namespace {
ComPtr<ID3D12Resource> make_buffer(UINT64 bytes) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                          D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                    IID_PPV_ARGS(&r));
    return r;
}
}  // namespace

void structured_views(ID3D12Resource* r, UINT elements, UINT stride, UINT uavSlot, UINT srvSlot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_UNKNOWN;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = elements;
    u.Buffer.StructureByteStride = stride;
    uavViewOriginal(device.Get(), r, nullptr, &u, game_cpu(uavSlot));
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.NumElements = elements;
    v.Buffer.StructureByteStride = stride;
    srvViewOriginal(device.Get(), r, &v, game_cpu(srvSlot));
}

// Under focus.lock: the twin of the stereo scene depth: single-sample R32_TYPELESS (what the tile and shadow passes
// read as R32_FLOAT), the largest, ACE's msaaDepth by name on a tie. Not just "the largest depth": flagsDepth (D32) is
// the same size, and hash order picked it in one run (tiles and shadows stopped, 2026-11-13 10:01).
const Twin* scene_depth_twin() {
    const Twin* best = nullptr;
    for (const auto& [game, tw] : focus.twins) {
        if (!tw.depth || tw.gameDesc.SampleDesc.Count != 1 || tw.gameDesc.Format != DXGI_FORMAT_R32_TYPELESS)
            continue;
        if (!best || tw.eyeW > best->eyeW || (tw.eyeW == best->eyeW && tw.name == "msaaDepth"))
            best = &tw;
    }
    if (!best && focus.resolvedDepth && focus.resolveReady)  // MSAA: our resolve of the multisampled depth twin
        best = &focus.resolvedDepthTwin;
    return best;
}

// Our single-sample resolves rest as shader resources; twins rest as their kind (`otherwise`).
D3D12_RESOURCE_STATES rest_of(ID3D12Resource* r, D3D12_RESOURCE_STATES otherwise) {
    if (r && (r == focus.resolvedDepth.Get() || r == focus.resolvedVelocity.Get() || r == focus.closestDepth.Get()))
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return otherwise;
}

namespace {
// Under focus.lock: our tile resources and descriptors for this frame's tile passes; the game's tile-list indices.
const Twin* focus_tiles_prepare(const Stream::TileDispatch& frustums) {
    if (focus.tilesBroken || !focus.bindlessBase.ptr || !frustums.heap)
        return nullptr;
    if (focus.gameHeap.Get() != frustums.heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC d = frustums.heap->GetDesc();
        if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
            return nullptr;
        retire(focus.gameHeap.Get());  // our lists recorded with it may still be ahead on the GPU
        focus.gameHeap = frustums.heap;
        focus.heapSize = d.NumDescriptors;
        focus.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const SIZE_T cpu = focus.gameHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        focus.heapCpuLo.store(0, std::memory_order_relaxed);
        focus.heapCpuHi.store(cpu + SIZE_T{focus.heapSize} * focus.increment, std::memory_order_relaxed);
        focus.heapCpuLo.store(cpu, std::memory_order_relaxed);
        focus.baseIndex = static_cast<UINT>((focus.bindlessBase.ptr - focus.gameHeap->GetGPUDescriptorHandleForHeapStart().ptr) /
                                            focus.increment);
        focus.slotFirst = focus.baseIndex + kBindlessRange - kTileSlots;
        if (focus.slotFirst + kTileSlots > focus.heapSize) {
            logf("replay focus: the bindless range (%u + %u) passes the heap's end (%u): no focus tiles", focus.baseIndex,
                 kBindlessRange, focus.heapSize);
            focus.tilesBroken = true;
            return nullptr;
        }
        logd("replay focus: tiles: the game's heap %u descriptors, bindless range from %u, ours %u..%u (the range's top)",
             focus.heapSize, focus.baseIndex, focus.slotFirst, focus.slotFirst + kTileSlots - 1);
        focus.tilesReady = false;
        focus.frustums[0].Reset();  // views written again below
    }
    // The focus depth twin: the stereo scene depth (scene_depth_twin).
    const Twin* depth = scene_depth_twin();
    if (!depth)
        return nullptr;
    float tileCount[2];
    std::memcpy(tileCount, frustums.constants + 18, sizeof(tileCount));
    UINT tx = static_cast<UINT>(tileCount[0]), ty = static_cast<UINT>(tileCount[1]);
    if (!tx || !ty || tx > 4096 || ty > 4096)
        return nullptr;
    // Our lists at the focus views' own grid (16-pixel tiles of the focus twin), when frameData can be given it.
    const UINT gameTx = tx, gameTy = ty;
    if (focus.framePso) {
        tx = (depth->focusW + 15) / 16;
        ty = (depth->focusH + 15) / 16;
    }
    if (!focus.frustums[0] || tx != focus.tilesX || ty != focus.tilesY) {
        for (int e = 0; e < 2; ++e)
            for (ComPtr<ID3D12Resource>* r : {std::addressof(focus.frustums[e]), std::addressof(focus.opaque[e]),
                                              std::addressof(focus.transparent[e])}) {  // ComPtr overloads operator&
                retire(r->Get());
                *r = make_buffer(UINT64{tx} * ty * kTileStride);
            }
        focus.tilesX = tx;
        focus.tilesY = ty;
        for (int e = 0; e < 2; ++e) {
            const UINT at = focus.slotFirst + e * 6;
            structured_views(focus.frustums[e].Get(), tx * ty, 64, at + 0, at + 1);
            structured_views(focus.opaque[e].Get(), tx * ty * 16, 4, at + 2, at + 3);
            structured_views(focus.transparent[e].Get(), tx * ty * 16, 4, at + 4, at + 5);
        }
        focus.tilesShaderResource = false;
        focus.depthForTiles = nullptr;
        for (int e = 0; e < 2; ++e) {  // the indices the focus pass constants carry: SRVs, relative to the base
            focus.ourTile[e] = focus.slotFirst + e * 6 + 3 - focus.baseIndex;      // opaque L, R
            focus.ourTile[2 + e] = focus.slotFirst + e * 6 + 5 - focus.baseIndex;  // transparent L, R
        }
        logf("replay focus: tiles: the game's grid %ux%u per eye; our tile lists made at %ux%u (the focus views' own)", gameTx,
             gameTy, tx, ty);
    }
    if (focus.depthForTiles != depth->focus.Get()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_FLOAT;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        srvViewOriginal(device.Get(), depth->focus.Get(), &v, game_cpu(focus.slotFirst + 12));
        focus.depthForTiles = depth->focus.Get();
    }
    // The game's tile-list SRVs: where it wrote them in its heap (directly, or copied from a staging heap).
    const SIZE_T start = focus.gameHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    for (const auto& [ptr, k] : focus.tileViews)
        if (ptr >= start && ptr < start + SIZE_T{focus.heapSize} * focus.increment) {
            const UINT abs = static_cast<UINT>((ptr - start) / focus.increment);
            if (abs < focus.baseIndex)
                continue;
            const uint32_t rel = abs - focus.baseIndex;
            if (k < 4)
                focus.gameTile[k] = rel;
            else  // post_process_color: every view of it (up to 4 distinct)
                for (uint32_t& g : focus.gamePrev)
                    if (g == rel || g == UINT32_MAX) {
                        g = rel;
                        break;
                    }
        }
    const bool known = focus.gameTile[0] != UINT32_MAX && focus.gameTile[1] != UINT32_MAX &&
                       focus.gameTile[2] != UINT32_MAX && focus.gameTile[3] != UINT32_MAX;
    if (known && !focus.tilesReady)
        logd("replay focus: tiles: the game's tile lists at bindless %u/%u (opaque) %u/%u (transparent), ours %u/%u %u/%u",
             focus.gameTile[0], focus.gameTile[1], focus.gameTile[2], focus.gameTile[3], focus.ourTile[0], focus.ourTile[1],
             focus.ourTile[2], focus.ourTile[3]);
    static int unknownFrames = 0;
    if (!known && ++unknownFrames == 300)
        logf("replay focus: tiles: the game's tile-list views not all seen after 300 tile passes (%u %u %u %u; %zu views "
             "recorded): they were made before the probe, or by a call it doesn't watch; no focus tiles",
             focus.gameTile[0], focus.gameTile[1], focus.gameTile[2], focus.gameTile[3], focus.tileViews.size());
    focus.tilesReady = known;
    return depth;
}
}  // namespace

// At the game's Close of a list with its tile passes: per eye, tileFrustums and tiledCulling again with the focus
// view's inverse projection, resolution and depth, into our tile lists. Executed after the game's batch, like the
// scene replays (before the lighting lists that read them).
void focus_tiles_record(List* gameList, const Stream& s) {
    std::vector<const Stream::TileDispatch*> fr, cu;
    for (const auto& d : s.tileDispatches)
        if (d.kind == 1 || d.kind == 2)
            (d.kind == 1 ? fr : cu).push_back(&d);
    if (fr.size() < 2 || cu.size() < 2) {
        static int epoch = 0, count = 0;
        if ((!fr.empty() || !cu.empty()) && miss_allowed(epoch, count, 3))
            logd("replay focus: tiles: not replayed (the list has %zu tileFrustums and %zu tiledCulling dispatches of %zu)",
                 fr.size(), cu.size(), s.tileDispatches.size());
        return;
    }
    Replay* r = nullptr;
    UINT fw = 0, fh = 0;
    ID3D12Resource* depth = nullptr;
    bool fromShaderResource = false;
    AcquireSRWLockExclusive(&focus.lock);
    const Twin* twin = focus_tiles_prepare(*fr[0]);
    if (twin && focus.tilesReady)
        r = acquire_replay();
    if (static int epoch = 0, count = 0; !r && miss_allowed(epoch, count, 4)) {
        float tileCount[2];
        std::memcpy(tileCount, fr[0]->constants + 18, sizeof(tileCount));
        const Twin* const d = scene_depth_twin();
        logd("replay focus: tiles: not replayed (prepared %d, ready %d, broken %d, bindless base %d, the list's heap %d (ours %d), "
             "depth twin %s, the game's tile count %.0fx%.0f, its lists at %u %u %u %u, our lists %ux%u)",
             twin ? 1 : 0, focus.tilesReady ? 1 : 0, focus.tilesBroken ? 1 : 0, focus.bindlessBase.ptr ? 1 : 0,
             fr[0]->heap ? 1 : 0, fr[0]->heap == focus.gameHeap.Get() ? 1 : 0, d ? d->name.c_str() : "none", tileCount[0],
             tileCount[1], focus.gameTile[0], focus.gameTile[1], focus.gameTile[2], focus.gameTile[3], focus.tilesX,
             focus.tilesY);
    }
    if (r) {
        fw = twin->focusW;
        fh = twin->focusH;
        depth = twin->focus.Get();
        fromShaderResource = focus.tilesShaderResource;
        focus.tilesShaderResource = true;
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    const UINT ftx = (fw + 15) / 16, fty = (fh + 15) / 16;  // the focus views' tiles (per eye)
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepTiles);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    barrier(l, depth, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (fromShaderResource)
        for (int e = 0; e < 2; ++e)
            for (ID3D12Resource* b : {focus.opaque[e].Get(), focus.transparent[e].Get()})
                barrier(l, b, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e) {
        const Stream::TileDispatch& f = *fr[e];
        const Stream::TileDispatch& c = *cu[e];
        float eyeInverse[16], focusInverse[16];
        std::memcpy(eyeInverse, f.constants, sizeof(eyeInverse));
        {  // the eye's NDC centre (-P02, -P12) for the atmosphere resample
            double q[4][4], pm[4][4];
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < 4; ++k)
                    q[i][k] = eyeInverse[k * 4 + i];
            if (invert(q, pm)) {
                AcquireSRWLockExclusive(&focus.lock);
                focus.eyeCentre[e][0] = static_cast<float>(-pm[0][2]);
                focus.eyeCentre[e][1] = static_cast<float>(-pm[1][2]);
                focus.centresKnown = true;
                ReleaseSRWLockExclusive(&focus.lock);
            }
        }
        if (!replay_focus_inverse_projection(eyeInverse, focus.settings.boxFraction, focusInverse, focus.gazeOffset[e][0],
                                             focus.gazeOffset[e][1], focus_box_y())) {
            ok = false;
            break;
        }
        // 1. tiledCulling's constants with the focus fields, into our slot.
        uint32_t cullParams[22];
        std::memcpy(cullParams, focusInverse, sizeof(focusInverse));
        const int32_t region[4] = {static_cast<int32_t>(fw), static_cast<int32_t>(fh), static_cast<int32_t>(e * fw), 0};
        std::memcpy(cullParams + 16, region, sizeof(region));
        const float tiles[2] = {static_cast<float>(focus.tilesX), static_cast<float>(focus.tilesY)};
        std::memcpy(cullParams + 20, tiles, sizeof(tiles));
        const uint32_t slot = focus.nextSlot.fetch_add(1) % kSlots;
        const D3D12_GPU_VIRTUAL_ADDRESS mine = focus.constants->GetGPUVirtualAddress() + UINT64{slot} * kSlotBytes;
        l->SetComputeRootSignature(focus.cullRoot.Get());
        l->SetPipelineState(focus.cullPso.Get());
        l->SetComputeRootConstantBufferView(0, c.cbv);
        l->SetComputeRoot32BitConstants(1, 22, cullParams, 0);
        l->SetComputeRootUnorderedAccessView(2, mine);
        l->Dispatch(1, 1, 1);
        // 2. tileFrustums for the focus view, at the focus grid (frameData's tileCount is given it in the scene replays).
        uint32_t frustumConstants[20];
        std::memcpy(frustumConstants, f.constants, sizeof(frustumConstants));
        std::memcpy(frustumConstants, focusInverse, sizeof(focusInverse));
        const float rcp[2] = {1.0f / static_cast<float>(fw), 1.0f / static_cast<float>(fh)};
        std::memcpy(frustumConstants + 16, rcp, sizeof(rcp));
        std::memcpy(frustumConstants + 18, tiles, sizeof(tiles));  // tileCount (floats at 72): our grid
        const UINT at = focus.slotFirst + e * 6;
        l->SetComputeRootSignature(f.root);
        l->SetPipelineState(f.pso);
        l->SetComputeRoot32BitConstants(0, 20, frustumConstants, 0);
        l->SetComputeRootDescriptorTable(1, game_gpu(at + 0));
        l->Dispatch((ftx + 15) / 16, (fty + 15) / 16, 1);
        barrier(l, focus.frustums[e].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        // 3. tiledCulling: our frustums, our focus depth, the game's entities and matrices, our tile lists.
        l->SetComputeRootSignature(c.root);
        l->SetPipelineState(c.pso);
        l->SetComputeRootConstantBufferView(0, mine);
        l->SetComputeRootDescriptorTable(1, game_gpu(at + 1));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 12));
        l->SetComputeRootDescriptorTable(3, c.tables[3]);
        l->SetComputeRootDescriptorTable(4, c.tables[4]);
        l->SetComputeRootDescriptorTable(5, game_gpu(at + 2));
        l->SetComputeRootDescriptorTable(6, game_gpu(at + 4));
        l->Dispatch(ftx, fty, 1);
        barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(l, focus.frustums[e].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    for (int e = 0; e < 2; ++e)
        for (ID3D12Resource* b : {focus.opaque[e].Get(), focus.transparent[e].Get()})
            barrier(l, b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    barrier(l, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    if (ok)
        add_pending(gameList, r);
    else
        r->busy = false;
    ReleaseSRWLockExclusive(&focus.lock);
    if (ok)
        focus.tileReplays.fetch_add(1, std::memory_order_relaxed);
}

// ---- the focus sky: our copy of the sky draw's CBV table (b3..b12) with cbSky (b4) patched -------------------------

// Under focus.lock (and cbvLock inside): for each sky table this list's sky draws use, a set of our descriptors (a ring of
// kSkyRing x kSkyTable at slots 80..) recreated from the game's CBVs, b4 a patched cbSky copy (written on the GPU by
// focus_sky_record). Returns the table map (game GPU handle -> ours) and, per table, (game cbSky address, ours).
std::vector<SkyPatch> focus_sky_prepare(const Stream& s) {
    std::vector<SkyPatch> out;
    if (!focus.settings.sky || focus.skyBroken || !focus.skyPso || !focus.gameHeap || !focus.slotFirst || focus.tilesBroken ||
        !focus.centresKnown || s.skyTables.empty())
        return out;
    if (!focus.skyCloudViews) {
        inReplay = true;
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        srvViewOriginal(device.Get(), focus.skyClouds.Get(), &v, game_cpu(focus.slotFirst + kSkyCloudSrv));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavViewOriginal(device.Get(), focus.skyClouds.Get(), nullptr, &u, game_cpu(focus.slotFirst + kSkyCloudSrv + 1));
        inReplay = false;
        focus.skyCloudViews = true;
    }
    const UINT64 heapGpu = focus.gameHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    const SIZE_T heapCpu = focus.gameHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    for (const UINT64 table : s.skyTables) {
        if (out.size() >= 1 || table < heapGpu)
            break;  // one sky table a list (ACE draws one sky)
        const UINT base = static_cast<UINT>((table - heapGpu) / focus.increment);
        D3D12_CONSTANT_BUFFER_VIEW_DESC views[kSkyTable];
        bool known = true;
        AcquireSRWLockShared(&focus.cbvLock);
        for (UINT k = 0; k < kSkyTable && known; ++k) {
            const auto it = focus.cbvInfo.find(heapCpu + SIZE_T{base + k} * focus.increment);
            known = it != focus.cbvInfo.end();
            if (known)
                views[k] = it->second;
        }
        ReleaseSRWLockShared(&focus.cbvLock);
        if (!known || !views[1].BufferLocation) {
            if (!focus.skyMissLogged) {
                focus.skyMissLogged = true;
                logf("replay focus: sky: the sky table's constant-buffer views (heap index %u..) weren't all seen; no focus "
                     "sky",
                     base);
            }
            break;
        }
        const UINT set = focus.skyRing++ % kSkyRing;
        const UINT first = focus.slotFirst + kSkySlotsFirst + set * kSkyTable;
        const UINT64 mine = focus.skyConstants->GetGPUVirtualAddress() + UINT64{focus.skySlot++ % kSkySlots} * 256;
        inReplay = true;
        for (UINT k = 0; k < kSkyTable; ++k) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC v = views[k];
            if (k == 1) {  // b4: cbSky -> our patched copy
                v.BufferLocation = mine;
                v.SizeInBytes = 256;
            }
            cbvViewOriginal(device.Get(), v.BufferLocation ? &v : nullptr, game_cpu(first + k));
        }
        inReplay = false;
        out.push_back({table, game_gpu(first).ptr, views[1].BufferLocation, mine});
    }
    return out;
}

// Into the focus replay `l` before its draws: our clouds (the cbSky-named texture through the box, eye 0's centre as
// ACE's stereo sky reads one for both eyes) and the cbSky copies with 4.x naming our clouds.
void focus_sky_record(List* l, const std::vector<SkyPatch>& patches, const float (&centre)[2]) {
    if (patches.empty())
        return;
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    for (const SkyPatch& sp : patches) {
        barrier(l, focus.skyClouds.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uint32_t p[4];
        const float f[3] = {box, centre[0], centre[1]};
        std::memcpy(p, f, sizeof(f));
        p[3] = kSkyCloudSize;
        l->SetComputeRootSignature(focus.skyRoot.Get());
        l->SetPipelineState(focus.skyPso.Get());
        l->SetComputeRootConstantBufferView(0, sp.gameCbSky);
        l->SetComputeRoot32BitConstants(1, 4, p, 0);
        l->SetComputeRoot32BitConstants(1, 1, &boxYBits, 4);
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.baseIndex));
        l->SetComputeRootDescriptorTable(3, game_gpu(focus.slotFirst + kSkyCloudSrv + 1));
        l->Dispatch((kSkyCloudSize + 7) / 8, (kSkyCloudSize + 7) / 8, 1);
        barrier(l, focus.skyClouds.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        // cbSky copy: register 4.x = our clouds' bindless index, unless ACE's is -1 (no screen clouds: the sky PS then
        // takes its clouds by view direction, right for a focus view as it is; frame-2000 16:26, clear weather)
        uint32_t q[8] = {16, 4, 1 | 16, 0};
        const uint32_t idx = focus.slotFirst + kSkyCloudSrv - focus.baseIndex;
        std::memcpy(&q[4], &idx, 4);
        l->SetComputeRootSignature(focus.cbPatchRoot.Get());
        l->SetPipelineState(focus.cbPatchPso.Get());
        l->SetComputeRootConstantBufferView(0, sp.gameCbSky);
        l->SetComputeRoot32BitConstants(1, 8, q, 0);
        l->SetComputeRootUnorderedAccessView(2, sp.ourCbSky);
        l->Dispatch(1, 1, 1);
    }
    barrier(l, focus.skyConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
}

namespace {
// ---- R2c: the focus views' volumetric fog ----------------------------------------------------------------------

// Under focus.lock: our grids and histories, like ACE's (its voxelGrid0 / voxelGridHistory_0_0 descriptions).
bool focus_fog_prepare(D3D12_GPU_DESCRIPTOR_HANDLE uavTable) {
    if (focus.fogBroken || focus.tilesBroken || !focus.gameHeap || !focus.slotFirst || !focus.fogPso || !uavTable.ptr)
        return false;
    const UINT64 heapGpu = focus.gameHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    if (uavTable.ptr < heapGpu || (uavTable.ptr - heapGpu) % focus.increment) {
        log_line("replay focus: fog: the UAV table isn't in the game's heap; no focus fog");
        focus.fogBroken = true;
        return false;
    }
    const UINT uavBase = static_cast<UINT>((uavTable.ptr - heapGpu) / focus.increment);
    if (focus.fogMade) {
        static int epoch = 0, count = 0;
        if (uavBase != focus.uavBaseIndex && miss_allowed(epoch, count, 3))
            logf("replay focus: fog: the game's UAV table is at %u now, ours were made for %u; no focus fog", uavBase,
                 focus.uavBaseIndex);
        return uavBase == focus.uavBaseIndex;
    }
    if (uavBase + kBindlessRange > focus.heapSize ||
        (uavBase + kBindlessRange > focus.slotFirst && uavBase < focus.slotFirst + kTileSlots)) {
        logf("replay focus: fog: the UAV range (%u + %u) doesn't leave room; no focus fog", uavBase, kBindlessRange);
        focus.fogBroken = true;
        return false;
    }
    focus.uavBaseIndex = uavBase;
    focus.fogUavFirst = uavBase + kBindlessRange - kFogUavSlots;
    if (focus.maxUavWritten >= focus.fogUavFirst) {
        logf("replay focus: fog: the game already wrote descriptor %llu in our UAV slots (%u..%u); no focus fog",
             static_cast<unsigned long long>(focus.maxUavWritten), focus.fogUavFirst, focus.fogUavFirst + kFogUavSlots - 1);
        focus.fogBroken = true;
        return false;
    }
    std::string names;
    for (const ComPtr<ID3D12Resource>& v : focus.views3d) {
        const std::string n = debug_name(v.Get());
        if (n == "voxelGrid0" || n == "voxelGrid1")
            focus.gameGrid[n.back() - '0'] = v;
        if (n == "voxelGrid0")
            focus.gameGridDesc = v->GetDesc();
        else if (n == "voxelGridHistory_0_0")
            focus.gameHistoryDesc = v->GetDesc();
        if (names.size() < 300)
            names += " " + (n.empty() ? std::string("?") : n);
    }
    const D3D12_RESOURCE_DESC grid = focus.gameGridDesc;
    const D3D12_RESOURCE_DESC history = focus.gameHistoryDesc.Dimension ? focus.gameHistoryDesc : focus.gameGridDesc;
    if (grid.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || history.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        if (!focus.fogMissLogged) {
            focus.fogMissLogged = true;
            logf("replay focus: fog: ACE's voxelGrid0 not among the %zu 3D textures it made views of (%s); no focus fog yet",
                 focus.views3d.size(), names.empty() ? "none" : names.c_str());
        }
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto make = [&](D3D12_RESOURCE_DESC d, ComPtr<ID3D12Resource>& r, UINT slot) {
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.MipLevels = 1;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                   nullptr, IID_PPV_ARGS(&r))))
            return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = d.Format;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        u.Texture3D.WSize = d.DepthOrArraySize;
        uavViewOriginal(device.Get(), r.Get(), nullptr, &u, game_cpu(focus.fogUavFirst + slot - focus.slotFirst - kFogFirst));
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = d.Format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture3D.MipLevels = 1;
        srvViewOriginal(device.Get(), r.Get(), &v, game_cpu(slot + 1));
        return true;
    };
    inReplay = true;
    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e) {
        const UINT at = focus.slotFirst + kFogFirst + 6 * e;
        ok = make(grid, focus.fogGrid[e], at) && make(history, focus.fogHistory[e][0], at + 2) &&
             make(history, focus.fogHistory[e][1], at + 4);
    }
    inReplay = false;
    if (!ok) {
        log_line("replay focus: fog: could not make our grids; no focus fog");
        focus.fogBroken = true;
        return false;
    }
    focus.fogMade = true;
    focus.fogHistoryValid = false;
    logf("replay focus: fog: our grids %llux%ux%u (format %d), histories format %d, SRVs at bindless %u..%u, UAVs at %u.. "
         "of the UAV table (base %u); the focus pass constants name eye 0's (volumetricFogMapIndex)",
         static_cast<unsigned long long>(grid.Width), grid.Height, static_cast<unsigned>(grid.DepthOrArraySize),
         static_cast<int>(grid.Format), static_cast<int>(history.Format), focus.slotFirst + kFogFirst - focus.baseIndex,
         focus.slotFirst + kFogFirst + 11 - focus.baseIndex, focus.fogUavFirst - focus.uavBaseIndex, focus.uavBaseIndex);
    return true;
}
}  // namespace

// At the game's Close of a list with its fog passes (per eye: light injection, then scatter): per eye both again with
// the focus view's constants, into our history and grid. Executed after the game's batch, before the lighting.
void focus_fog_record(List* gameList, const Stream& s) {
    std::vector<const Stream::TileDispatch*> inj, sc;
    for (const auto& d : s.tileDispatches)
        if (d.kind == 3)
            inj.push_back(&d);
        else if (d.kind == 4)
            sc.push_back(&d);
    // ACE's stereo lighting reads eye 0's grid for both eyes (volumetricFogMapIndex = voxelGrid0): eye 0's only.
    const size_t eyes = std::min<size_t>(1, std::min(inj.size(), sc.size()));
    if (!eyes)
        return;
    if (focus.settings.fog == 2) {  // ACE's grids resampled through each box: no ACE fog pass replayed
        Replay* rr = nullptr;
        float centre[2][2] = {};
        AcquireSRWLockExclusive(&focus.lock);
        if (inj[0]->tables[5].ptr == sc[0]->tables[2].ptr && focus_fog_prepare(inj[0]->tables[5]) && focus.atmoPso[1] &&
            focus.centresKnown && focus.gameGrid[0] && focus.gameGrid[1]) {
            if (!focus.fogResampleViews) {
                inReplay = true;
                for (int e = 0; e < 2; ++e) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = focus.gameGrid[e]->GetDesc().Format;
                    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    v.Texture3D.MipLevels = 1;
                    srvViewOriginal(device.Get(), focus.gameGrid[e].Get(), &v, game_cpu(focus.slotFirst + 67 + e));
                }
                inReplay = false;
                focus.fogResampleViews = true;
                log_line("replay focus: fog: resampling ACE's voxelGrid0 / 1 through each focus box (replay_fog=2)");
            }
            rr = acquire_replay();
            focus_centres(centre);
        }
        ReleaseSRWLockExclusive(&focus.lock);
        if (!rr)
            return;
        inReplay = true;
        rr->allocator->Reset();
        List* l = rr->list.Get();
        l->Reset(rr->allocator.Get(), nullptr);
        ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
        l->SetDescriptorHeaps(1, heaps);
        l->SetComputeRootSignature(focus.atmoRoot.Get());
        l->SetPipelineState(focus.atmoPso[1].Get());
        const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
        for (int e = 0; e < 2; ++e) {
            ID3D12Resource* grid = focus.fogGrid[e].Get();
            const D3D12_RESOURCE_DESC d = grid->GetDesc();
            uint32_t p[10] = {};
            const float f0[3] = {box, centre[e][0], centre[e][1]};
            std::memcpy(p, f0, sizeof(f0));
            p[3] = static_cast<uint32_t>(d.Width);
            p[4] = d.Height;
            p[5] = d.DepthOrArraySize;
            barrier(l, grid, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            l->SetComputeRoot32BitConstants(0, 10, p, 0);
        l->SetComputeRoot32BitConstants(0, 1, &boxYBits, 10);  // the box's height (kAtmoShader boxY)
        atmo_source_region(l, false);
            l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 67 + e));
            l->SetComputeRootDescriptorTable(2, game_gpu(focus.fogUavFirst + 6 * e));
            l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, p[5]);
            barrier(l, grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        l->Close();
        inReplay = false;
        AcquireSRWLockExclusive(&focus.lock);
        add_pending(gameList, rr);
        focus.fogReady = true;
        ReleaseSRWLockExclusive(&focus.lock);
        focus.fogReplays.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Replay* r = nullptr;
    int parity = 0;
    bool fresh = true;
    AcquireSRWLockExclusive(&focus.lock);
    // ACE's tables: injection 5 and scatter 2 are the UAV table (space 20), the same base.
    if (inj[0]->tables[5].ptr == sc[0]->tables[2].ptr && focus_fog_prepare(inj[0]->tables[5]))
        r = acquire_replay();
    if (r) {
        parity = focus.fogParity;
        fresh = !focus.fogHistoryValid;
        focus.fogParity ^= 1;
        focus.fogHistoryValid = true;
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    const auto rel = [](UINT slot) { return focus.slotFirst + slot - focus.baseIndex; };  // SRV: bindless-relative
    const auto relUav = [](UINT slot) { return focus.fogUavFirst + slot - kFogFirst - focus.uavBaseIndex; };
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepFog);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    for (size_t e = 0; e < eyes; ++e) {
        const Stream::TileDispatch& in = *inj[e];
        const Stream::TileDispatch& out = *sc[e];
        const UINT at = kFogFirst + 6 * static_cast<UINT>(e);
        ID3D12Resource* history = focus.fogHistory[e][parity].Get();
        ID3D12Resource* grid = focus.fogGrid[e].Get();
        // 1. Both constant copies with the focus fields.
        const uint32_t slotA = focus.fogSlot++ % kFogSlots, slotB = focus.fogSlot++ % kFogSlots;
        const D3D12_GPU_VIRTUAL_ADDRESS mineA = focus.fogConstants->GetGPUVirtualAddress() + UINT64{slotA} * kFogSlotBytes;
        const D3D12_GPU_VIRTUAL_ADDRESS mineB = focus.fogConstants->GetGPUVirtualAddress() + UINT64{slotB} * kFogSlotBytes;
        const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
        uint32_t params[9];
        std::memcpy(params, &box, 4);
        std::memcpy(params + 6, focus.gazeOffset[e], 8);  // gx, gy
        params[8] = boxYBits;
        params[1] = 0;
        params[2] = rel(at + 3 + 2 * (1 - parity));  // last frame's history, SRV
        params[3] = relUav(at + 2 + 2 * parity);     // this frame's history, UAV
        params[4] = 0;
        params[5] = fresh ? 1u : 0u;
        l->SetComputeRootSignature(focus.fogRoot.Get());
        l->SetPipelineState(focus.fogPso.Get());
        l->SetComputeRootConstantBufferView(0, in.cbv);
        l->SetComputeRoot32BitConstants(1, 9, params, 0);
        l->SetComputeRootUnorderedAccessView(2, mineA);
        l->Dispatch(16, 1, 1);
        params[1] = 1;
        params[3] = relUav(at + 0);            // our grid, UAV
        params[4] = rel(at + 3 + 2 * parity);  // this frame's history, SRV
        l->SetComputeRootConstantBufferView(0, out.cbv);
        l->SetComputeRoot32BitConstants(1, 9, params, 0);
        l->SetComputeRootUnorderedAccessView(2, mineB);
        l->Dispatch(1, 1, 1);
        barrier(l, focus.fogConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        // 2. ACE's light injection into our history (reading last frame's).
        barrier(l, history, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(in.root);
        l->SetPipelineState(in.pso);
        l->SetComputeRootConstantBufferView(0, mineA);
        for (UINT t = 1; t < 6; ++t)  // ACE's injection root signature: CBV + 5 tables
            if (in.tables[t].ptr)
                l->SetComputeRootDescriptorTable(t, in.tables[t]);
        l->Dispatch(in.dims[0], in.dims[1], in.dims[2]);
        barrier(l, history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        // 3. ACE's scatter into our grid.
        barrier(l, grid, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(out.root);
        l->SetPipelineState(out.pso);
        l->SetComputeRootConstantBufferView(0, mineB);
        for (UINT t = 1; t < 3; ++t)  // ACE's scatter root signature: CBV + 2 tables
            if (out.tables[t].ptr)
                l->SetComputeRootDescriptorTable(t, out.tables[t]);
        l->Dispatch(out.dims[0], out.dims[1], out.dims[2]);
        barrier(l, grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        barrier(l, focus.fogConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    add_pending(gameList, r);
    if (!focus.fogReady)
        logd("replay focus: fog: the first focus fog replay recorded; the lighting reads it from the next frame");
    focus.fogReady = true;  // the next scene replays name our grid (executed after this one: same queue, later batch)
    ReleaseSRWLockExclusive(&focus.lock);
    focus.fogReplays.fetch_add(1, std::memory_order_relaxed);
}

// ---- R2c: the focus views' deferred sun shadow --------------------------------------------------------------------

// At the game's Close of the list with normalReconstructionViewSpace (a compute pass over both eyes) and the two
// deferredShadows draws (one per eye, half resolution): both again for the focus views, from our focus depth into our
// normals and our deferred shadow (half the focus size). The focus pass constants name ours (deferredShadowMapIndex).
void focus_shadow_record(List* gameList, const Stream& s) {
    const Stream::TileDispatch* normalsPass = nullptr;
    for (const auto& d : s.tileDispatches)
        if (d.kind == 5)
            normalsPass = &d;
    if (!normalsPass || s.shadowDraws.size() < 2 || !normalsPass->cbv2)
        return;
    Replay* r = nullptr;
    ID3D12Resource* depth = nullptr;
    UINT fw = 0, fh = 0;
    AcquireSRWLockExclusive(&focus.lock);
    const Twin* twin = scene_depth_twin();
    const bool ok = !focus.shadowBroken && !focus.tilesBroken && focus.gameHeap && focus.slotFirst && focus.shadowPatchPso &&
                    twin && focus.depthForTiles == twin->focus.Get();  // our depth SRV (slot 12) names this twin
    if (static int epoch = 0, count = 0; !ok && focus.shadowPatchPso && miss_allowed(epoch, count, 3))
        logd("replay focus: shadow: not replayed (broken %d, tiles broken %d, depth twin %s %ux%u, the tiles' depth %s)",
             focus.shadowBroken ? 1 : 0, focus.tilesBroken ? 1 : 0, twin ? twin->name.c_str() : "none", twin ? twin->eyeW : 0,
             twin ? twin->eyeH : 0, twin && focus.depthForTiles == twin->focus.Get() ? "the same" : "another");
    if (ok) {
        fw = twin->focusW;
        fh = twin->focusH;
        depth = twin->focus.Get();
        if (!focus.normals || fw != focus.shadowFw || fh != focus.shadowFh) {
            retire(focus.normals.Get());
            retire(focus.shadow.Get());
            focus.normals.Reset();
            focus.shadow.Reset();
            if (focus.shadowRtvSlot < 0)
                focus.shadowRtv = view_slot(false, focus.shadowRtvSlot);
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            const D3D12_RESOURCE_DESC nd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, UINT64{2} * fw, fh, 1, 1,
                                         DXGI_FORMAT_R8G8B8A8_SNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            const D3D12_RESOURCE_DESC sd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, fw, (fh + 1) / 2, 1, 1,
                                         DXGI_FORMAT_R16G16_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
            D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R16G16_UNORM, {0, 0, 0, 0}};
            if (!focus.shadowRtv.ptr ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &nd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                       nullptr, IID_PPV_ARGS(&focus.normals))) ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                       &clear, IID_PPV_ARGS(&focus.shadow)))) {
                log_line("replay focus: shadow: could not make our normals / shadow; no focus shadow");
                focus.shadowBroken = true;
            } else {
                inReplay = true;
                D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                u.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
                u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavViewOriginal(device.Get(), focus.normals.Get(), nullptr, &u, game_cpu(focus.slotFirst + 28));
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Texture2D.MipLevels = 1;
                srvViewOriginal(device.Get(), focus.normals.Get(), &v, game_cpu(focus.slotFirst + 14));
                v.Format = DXGI_FORMAT_R16G16_UNORM;
                srvViewOriginal(device.Get(), focus.shadow.Get(), &v, game_cpu(focus.slotFirst + 15));
                D3D12_RENDER_TARGET_VIEW_DESC rv{};
                rv.Format = DXGI_FORMAT_R16G16_UNORM;
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvOriginal(device.Get(), focus.shadow.Get(), &rv, focus.shadowRtv);
                inReplay = false;
                focus.shadowFw = fw;
                focus.shadowFh = fh;
                logd("replay focus: shadow: our normals %ux%u, deferred shadow %ux%u at bindless %u", 2 * fw, fh, fw,
                     (fh + 1) / 2, focus.slotFirst + 15 - focus.baseIndex);
            }
        }
        if (!focus.shadowBroken)
            r = acquire_replay();
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepShadow);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    // 1. The constants: normals', then each eye's shadow.
    const D3D12_GPU_VIRTUAL_ADDRESS base = focus.shadowConstants->GetGPUVirtualAddress();
    D3D12_GPU_VIRTUAL_ADDRESS mine[3];
    for (D3D12_GPU_VIRTUAL_ADDRESS& m : mine)
        m = base + UINT64{focus.shadowSlot++ % kShadowSlots} * kShadowSlotBytes;
    const float box = static_cast<float>(focus.settings.boxFraction), ffw = static_cast<float>(fw), ffh = static_cast<float>(fh);
    const uint32_t boxYBits = float_bits(focus_box_y());
    l->SetComputeRootSignature(focus.shadowPatchRoot.Get());
    l->SetPipelineState(focus.shadowPatchPso.Get());
    l->SetComputeRootConstantBufferView(0, normalsPass->cbv2);
    for (int k = 0; k < 3; ++k) {
        uint32_t p[12] = {};
        std::memcpy(&p[0], &box, 4);
        p[1] = k == 0 ? 0u : 1u;
        p[2] = k == 0 ? 0u : static_cast<uint32_t>(k - 1);
        std::memcpy(&p[3], &ffw, 4);
        std::memcpy(&p[4], &ffh, 4);
        p[5] = boxYBits;
        std::memcpy(&p[8], focus.gazeOffset, sizeof(focus.gazeOffset));
        l->SetComputeRootConstantBufferView(1, k == 0 ? normalsPass->cbv2 : s.shadowDraws[k - 1].cbv);
        l->SetComputeRoot32BitConstants(2, 12, p, 0);
        l->SetComputeRootUnorderedAccessView(3, mine[k]);
        l->Dispatch(1, 1, 1);
    }
    barrier(l, focus.shadowConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    // 2. ACE's normal reconstruction from our focus depth into our normals.
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier(l, depth, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE), read);
    barrier(l, focus.normals.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(normalsPass->root);
    l->SetPipelineState(normalsPass->pso);
    l->SetComputeRootDescriptorTable(0, game_gpu(focus.slotFirst + 12));
    l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 28));
    l->SetComputeRootConstantBufferView(2, mine[0]);
    l->Dispatch((2 * fw + 7) / 8, (fh + 7) / 8, 1);
    barrier(l, focus.normals.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    // 3. ACE's deferredShadows, per eye into its half of our shadow.
    barrier(l, focus.shadow.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float zero[4] = {};
    l->ClearRenderTargetView(focus.shadowRtv, zero, 0, nullptr);
    l->OMSetRenderTargets(1, &focus.shadowRtv, FALSE, nullptr);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT hw = fw / 2, hh = (fh + 1) / 2;
    for (int e = 0; e < 2; ++e) {
        const Stream::ShadowDraw& d = s.shadowDraws[e];
        const D3D12_VIEWPORT vp{static_cast<float>(e * hw), 0, static_cast<float>(e == 0 ? hw : fw - hw),
                                static_cast<float>(hh), 0, 1};
        const D3D12_RECT rc{static_cast<LONG>(e * hw), 0, static_cast<LONG>(e == 0 ? hw : fw), static_cast<LONG>(hh)};
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &rc);
        l->SetGraphicsRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetGraphicsRootDescriptorTable(0, d.tables[0]);
        l->SetGraphicsRootDescriptorTable(1, game_gpu(focus.slotFirst + 12));
        l->SetGraphicsRootDescriptorTable(2, game_gpu(focus.slotFirst + 14));
        l->SetGraphicsRootDescriptorTable(3, d.tables[3]);
        l->SetGraphicsRootDescriptorTable(4, d.tables[4]);
        l->SetGraphicsRootConstantBufferView(5, mine[1 + e]);
        l->DrawInstanced(3, 1, 0, 0);
    }
    barrier(l, focus.shadow.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    barrier(l, depth, read, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    barrier(l, focus.shadowConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    add_pending(gameList, r);
    if (!focus.shadowReady)
        logd("replay focus: shadow: the first focus deferred-shadow replay recorded");
    focus.shadowReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.shadowReplays.fetch_add(1, std::memory_order_relaxed);
}

namespace {
// ---- R2c: the focus views' atmosphere maps ----------------------------------------------------------------------

// ACE's per-eye atmosphere maps the scene passes read by screen position, and the pass-constant field naming each:
// in-scatter (3D, 73.y), out-scatter (3D, 73.z), sky in-scatter (2D, 75.x), sky out-scatter (2D, 75.y); _0 / _1 the eye.
constexpr const char* kAtmoNames[4] = {"atmosphereInScatter_srv_", "atmosphereOutScatter_srv_",
                                       "atmosphereInScatterSkyUpscaled_srv_", "atmosphereOutScatterSkyUpscaled_srv_"};

// Under focus.lock: our focus copies of the 8 maps (4 per eye), found among the game's views by name at first use.
// Our slots: 32 + m the game map's SRV (input), 40 + m ours (SRV), 48 + m ours (UAV); m = eye * 4 + map.
bool focus_atmo_prepare() {
    if (focus.atmoBroken || focus.tilesBroken || !focus.gameHeap || !focus.slotFirst || !focus.atmoPso[0])
        return false;
    if (focus.atmoMade)
        return true;
    ID3D12Resource* game[8] = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC views[8] = {};
    for (const auto& [ptr, info] : focus.srvInfo) {
        const std::string n = debug_name(info.first.Get());
        for (int m = 0; m < 8; ++m)
            if (!game[m] && n == std::string(kAtmoNames[m % 4]) + std::to_string(m / 4)) {
                game[m] = info.first.Get();
                views[m] = info.second;
            }
    }
    for (int m = 0; m < 8; ++m)
        if (!game[m]) {
            if (!focus.atmoMissLogged) {
                focus.atmoMissLogged = true;
                logd("replay focus: atmosphere: %s%d not among the %zu views seen yet", kAtmoNames[m % 4], m / 4,
                     focus.srvInfo.size());
            }
            return false;
        }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    inReplay = true;
    bool ok = true;
    for (int m = 0; m < 8 && ok; ++m) {
        D3D12_RESOURCE_DESC d = game[m]->GetDesc();
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.MipLevels = 1;
        focus.atmoGame[m] = game[m];
        ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                       nullptr, IID_PPV_ARGS(&focus.atmo[m])));
        if (!ok)
            break;
        srvViewOriginal(device.Get(), game[m], &views[m], game_cpu(focus.slotFirst + 32 + m));
        D3D12_SHADER_RESOURCE_VIEW_DESC v = views[m];
        v.Format = d.Format;
        srvViewOriginal(device.Get(), focus.atmo[m].Get(), &v, game_cpu(focus.slotFirst + 40 + m));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = d.Format;
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            u.Texture3D.WSize = d.DepthOrArraySize;
        } else {
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        uavViewOriginal(device.Get(), focus.atmo[m].Get(), nullptr, &u, game_cpu(focus.slotFirst + 48 + m));
    }
    inReplay = false;
    if (!ok) {
        log_line("replay focus: atmosphere: could not make our maps; no focus atmosphere");
        focus.atmoBroken = true;
        return false;
    }
    const D3D12_RESOURCE_DESC a = game[0]->GetDesc(), b = game[2]->GetDesc();
    logd("replay focus: atmosphere: our maps made (3D %llux%ux%u, sky %llux%u), at bindless %u..%u",
         static_cast<unsigned long long>(a.Width), a.Height, static_cast<unsigned>(a.DepthOrArraySize),
         static_cast<unsigned long long>(b.Width), b.Height, focus.slotFirst + 40 - focus.baseIndex,
         focus.slotFirst + 47 - focus.baseIndex);
    focus.atmoMade = true;
    return true;
}

// Under focus.lock: replay_atmosphere=4's work textures (the game's low-res sky maps and upscaling intermediates, per
// eye), found by name like the maps.
bool focus_atmo_work_prepare() {
    if (focus.atmoWorkMade)
        return true;
    static const char* const kWork[4] = {"atmosphereInScatterSky_srv_", "atmosphereOutScatterSky_srv_",
                                         "atmosphereInScatterSkyUpscaledIntermediate_srv_",
                                         "atmosphereOutScatterSkyUpscaledIntermediate_srv_"};
    ID3D12Resource* game[2][4] = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC views[2][4] = {};
    for (const auto& [ptr, info] : focus.srvInfo) {
        const std::string n = debug_name(info.first.Get());
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < 4; ++k)
                if (!game[e][k] && n == std::string(kWork[k]) + std::to_string(e)) {
                    game[e][k] = info.first.Get();
                    views[e][k] = info.second;
                }
    }
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 4; ++k)
            if (!game[e][k])
                return false;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    inReplay = true;
    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e)
        for (int k = 0; k < 4 && ok; ++k) {
            D3D12_RESOURCE_DESC d = game[e][k]->GetDesc();
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            d.MipLevels = 1;
            d.Alignment = 0;
            ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                           D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                           IID_PPV_ARGS(&focus.atmoSkyWork[e][k])));
            if (!ok)
                break;
            D3D12_SHADER_RESOURCE_VIEW_DESC v = views[e][k];
            v.Format = d.Format;
            v.Texture2D.MostDetailedMip = 0;
            v.Texture2D.MipLevels = 1;
            srvViewOriginal(device.Get(), focus.atmoSkyWork[e][k].Get(), &v, game_cpu(focus.slotFirst + 112 + 8 * e + 2 * k));
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = d.Format;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavViewOriginal(device.Get(), focus.atmoSkyWork[e][k].Get(), nullptr, &u,
                                              game_cpu(focus.slotFirst + 112 + 8 * e + 2 * k + 1));
        }
    inReplay = false;
    if (!ok) {
        log_line("replay focus: atmosphere: could not make our sky work textures; the maps are resampled instead");
        focus.atmoGridPso.Reset();
        return false;
    }
    const D3D12_RESOURCE_DESC a = game[0][0]->GetDesc(), b = game[0][2]->GetDesc();
    logf("replay focus: atmosphere: replaying ACE's scattering per focus eye (low-res sky %llux%u, intermediate %llux%u)",
         static_cast<unsigned long long>(a.Width), a.Height, static_cast<unsigned long long>(b.Width), b.Height);
    focus.atmoWorkMade = true;
    return true;
}

// replay_atmosphere=4, into our open list `l` (our heap set): per focus eye, ACE's atmosphericScattering with the ray
// grid narrowed to the box into our 3D maps and low-res sky maps, then ACE's four upscaling dispatches (in, out
// horizontal into the intermediates, then vertical into our upscaled sky maps). True when both eyes were replayed.
bool focus_atmo_replay(List* l, const Stream& s, const float (&centre)[2][2]) {
    std::vector<const Stream::TileDispatch*> scatter, up;
    const D3D12_RESOURCE_DESC grid = focus.atmo[0]->GetDesc();  // the 3D in-scatter: the ray grid's W x H
    const UINT gx = (static_cast<UINT>(grid.Width) + 7) / 8, gy = (grid.Height + 7) / 8;
    for (const auto& d : s.tileDispatches) {
        if (d.kind == 17 && d.dims[0] == gx && d.dims[1] == gy && d.cbv && d.cbv1)
            scatter.push_back(&d);
        else if (d.kind == 8)
            up.push_back(&d);
    }
    if (scatter.size() < 2 || up.size() < 8) {
        if (!focus.atmoReplayLogged) {
            focus.atmoReplayLogged = true;
            logf("replay focus: atmosphere: ACE's scattering / upscaling not as expected (%zu eye scatterings of %ux%u "
                 "groups, %zu upscalings); the maps are resampled instead",
                 scatter.size(), gx, gy, up.size());
        }
        return false;
    }
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    const float W = static_cast<float>(grid.Width), H = static_cast<float>(grid.Height);
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for (int e = 0; e < 2; ++e) {
        // 1. Our scatteringSettings: the grid narrowed (half-cell centres: eye cell = W (1 + c - b) / 2 - 1/2 + b/2 + b x).
        const D3D12_GPU_VIRTUAL_ADDRESS cb = focus.atmoGridConstants->GetGPUVirtualAddress() + UINT64{focus.atmoGridSlot++ % 16} * 256;
        const float ox = W * (1 + centre[e][0] - box) / 2 - 0.5f + 0.5f * box;
        const float oy = H * (1 - centre[e][1] - boxY) / 2 - 0.5f + 0.5f * boxY;
        const float pr[6] = {box, ox, oy, boxY, 0, 0};
        l->SetComputeRootSignature(focus.aaPatchRoot.Get());
        l->SetPipelineState(focus.atmoGridPso.Get());
        l->SetComputeRootConstantBufferView(0, scatter[e]->cbv1);
        l->SetComputeRoot32BitConstants(1, 6, pr, 0);
        l->SetComputeRootUnorderedAccessView(2, cb);
        l->Dispatch(1, 1, 1);
        barrier(l, focus.atmoGridConstants.Get(), uav, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        // 2. ACE's scattering into ours: u0 in 3D, u1 in sky (low), u2 out 3D, u3 out sky (low).
        ID3D12Resource* outs[4] = {focus.atmo[e * 4 + 0].Get(), focus.atmoSkyWork[e][0].Get(), focus.atmo[e * 4 + 1].Get(),
                                   focus.atmoSkyWork[e][1].Get()};
        for (ID3D12Resource* r : outs)
            barrier(l, r, srv, uav);
        const Stream::TileDispatch& d = *scatter[e];
        l->SetComputeRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetComputeRootConstantBufferView(0, d.cbv);
        l->SetComputeRootConstantBufferView(1, cb);
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 48 + e * 4 + 0));
        l->SetComputeRootDescriptorTable(3, game_gpu(focus.slotFirst + 112 + 8 * e + 1));
        l->SetComputeRootDescriptorTable(4, game_gpu(focus.slotFirst + 48 + e * 4 + 1));
        l->SetComputeRootDescriptorTable(5, game_gpu(focus.slotFirst + 112 + 8 * e + 3));
        for (int t = 6; t <= 8; ++t)
            l->SetComputeRootDescriptorTable(t, d.tables[t]);
        l->Dispatch(d.dims[0], d.dims[1], d.dims[2]);
        for (ID3D12Resource* r : outs)
            barrier(l, r, uav, srv);
        barrier(l, focus.atmoGridConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, uav);
        // 3. ACE's upscaling, its order per eye: in -> in intermediate, out -> out intermediate, in intermediate -> in
        // upscaled, out intermediate -> out upscaled.
        const UINT inSrv[4] = {112 + 8u * e + 0, 112 + 8u * e + 2, 112 + 8u * e + 4, 112 + 8u * e + 6};
        const UINT outUav[4] = {112 + 8u * e + 5, 112 + 8u * e + 7, 48u + e * 4 + 2, 48u + e * 4 + 3};
        ID3D12Resource* target[4] = {focus.atmoSkyWork[e][2].Get(), focus.atmoSkyWork[e][3].Get(),
                                     focus.atmo[e * 4 + 2].Get(), focus.atmo[e * 4 + 3].Get()};
        for (int k = 0; k < 4; ++k) {
            const Stream::TileDispatch& u = *up[e * 4 + k];
            barrier(l, target[k], srv, uav);
            l->SetComputeRootSignature(u.root);
            l->SetPipelineState(u.pso);
            l->SetComputeRoot32BitConstants(0, 8, u.constants, 0);
            l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + inSrv[k]));
            l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + outUav[k]));
            l->Dispatch(u.dims[0], u.dims[1], u.dims[2]);
            barrier(l, target[k], uav, srv);
        }
    }
    return true;
}
}  // namespace

// At the game's Close of the list with atmosphericScatteringUpscaling: our 8 maps resampled from the game's
// (replay_atmosphere=4: computed by ACE's own passes for the focus camera, focus_atmo_replay).
void focus_atmo_record(List* gameList, const Stream& s) {
    Replay* r = nullptr;
    float centre[2][2] = {};
    bool replayMaps = false;
    AcquireSRWLockExclusive(&focus.lock);
    if (focus.centresKnown && focus_atmo_prepare())
        r = acquire_replay();
    if (r && focus.settings.atmosphere == 4 && focus.atmoGridPso)
        replayMaps = focus_atmo_work_prepare();
    focus_centres(centre);
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepAtmo);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    const bool replayed = replayMaps && focus_atmo_replay(l, s, centre);
    l->SetComputeRootSignature(focus.atmoRoot.Get());
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    for (int m = 0; m < 8 && !replayed; ++m) {
        const D3D12_RESOURCE_DESC d = focus.atmo[m]->GetDesc();
        const bool is3d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const UINT sz = is3d ? d.DepthOrArraySize : 1;
        uint32_t p[10] = {};
        const float f[3] = {box, centre[m / 4][0], centre[m / 4][1]};
        std::memcpy(p, f, sizeof(f));
        p[3] = static_cast<uint32_t>(d.Width);
        p[4] = d.Height;
        p[5] = sz;
        barrier(l, focus.atmo[m].Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetPipelineState(focus.atmoPso[is3d ? 1 : 0].Get());
        l->SetComputeRoot32BitConstants(0, 10, p, 0);
        l->SetComputeRoot32BitConstants(0, 1, &boxYBits, 10);  // the box's height (kAtmoShader boxY)
        atmo_source_region(l, false);
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 32 + m));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 48 + m));
        l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, sz);
        barrier(l, focus.atmo[m].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    add_pending(gameList, r);
    if (!focus.atmoReady)
        logd("replay focus: atmosphere: first resample recorded (eye centres %.4f,%.4f and %.4f,%.4f)", centre[0][0],
             centre[0][1], centre[1][0], centre[1][1]);
    focus.atmoReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.atmoReplays.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace simqvfr::replay_detail

// The focus views' transparents (replay_internal.h has the file map): the OIT composite, the refraction source
// (opaqueSceneBlur) and the previous-frame colour.
#include "replay_internal.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../../log.h"
#include "replay_shaders.h"

namespace simqvfr::replay_detail {

// ---- R2c: the order-independent-transparency composite --------------------------------------------------------------

// At the game's Close of the list with oitComposite (one full-target draw): again on our twins, with its three tables
// (accumulation, reveal, flags: fixed descriptors, not bindless) pointing at our views of our twins (slots 29-31).
void focus_oit_record(List* gameList, const Stream& s) {
    Replay* r = nullptr;
    std::vector<std::pair<SIZE_T, SIZE_T>> focusTargets;
    std::vector<std::pair<UINT64, UINT64>> tables;
    std::vector<ID3D12Resource*> reads;  // our twins the composite reads (render targets otherwise)
    UINT fw = 0, fh = 0;
    AcquireSRWLockExclusive(&focus.lock);
    static int missLogged = 0;
    if (focus.gameHeap && focus.slotFirst && !focus.tilesBroken && focus_prepare(s)) {
        bool ok = true;
        for (const SIZE_T h : s.targets) {
            const Twin& t = focus.twins.at(focus.views.at(h).resource.Get());
            focusTargets.emplace_back(h, t.focusView.ptr);
            fw = t.focusW;
            fh = t.focusH;
        }
        const UINT64 heapGpu = focus.gameHeap->GetGPUDescriptorHandleForHeapStart().ptr;
        std::string why;
        for (UINT k = 1; k <= 3 && ok; ++k) {
            const UINT64 g = s.oitTables[k].ptr;
            const auto info = g >= heapGpu ? focus.srvInfo.find(game_cpu(static_cast<UINT>((g - heapGpu) / focus.increment)).ptr)
                                           : focus.srvInfo.end();
            const auto twin = info == focus.srvInfo.end() ? focus.twins.end() : focus.twins.find(info->second.first.Get());
            if (twin == focus.twins.end()) {
                why = info == focus.srvInfo.end()
                          ? "table " + std::to_string(k) + "'s view (heap index " +
                                std::to_string((g - heapGpu) / focus.increment) + ") wasn't seen; " +
                                std::to_string(focus.srvInfo.size()) + " render-target views known"
                          : "no twin of " + debug_name(info->second.first.Get());
                ok = false;
                break;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC v = info->second.second;
            inReplay = true;
            srvViewOriginal(device.Get(), twin->second.focus.Get(), &v, game_cpu(focus.slotFirst + 28 + k));
            inReplay = false;
            tables.emplace_back(g, game_gpu(focus.slotFirst + 28 + k).ptr);
            reads.push_back(twin->second.focus.Get());
        }
        if (ok && fw)
            r = acquire_replay();
        else if (static int epoch = 0; focus.twins.size() > 8 && miss_allowed(epoch, missLogged, 3)) {  // not before the scene's twins exist
            logd("replay focus: OIT composite: not replayed (%s)", why.empty() ? "no target" : why.c_str());
        }
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepOit);
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (ID3D12Resource* t : reads)
        barrier(l, t, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    const D3D12_VIEWPORT view{0, 0, static_cast<float>(2 * fw), static_cast<float>(fh), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(2 * fw), static_cast<LONG>(fh)};
    Substitution sub{&view, &rect, 1, nullptr, nullptr, &focusTargets};
    sub.tables = &tables;
    l->RSSetViewports(1, &view);
    l->RSSetScissorRects(1, &rect);
    const int64_t draws = emit(l, s, &sub);
    for (ID3D12Resource* t : reads)
        barrier(l, t, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    if (draws < 0)
        r->busy = false;
    else
        add_pending(gameList, r);
    ReleaseSRWLockExclusive(&focus.lock);
    if (draws >= 0)
        focus.oitReplays.fetch_add(1, std::memory_order_relaxed);
}

// ---- P1: the focus views' refraction source (opaqueSceneBlur) -----------------------------------------------------

// At the game's Close of the list with its opaqueSceneBlur chain (gaussianBlur3x3Float4): our focus version, mip 0 a
// copy of our colour twin (the opaque focus scene: the transparents come later), mips 1-3 the game's blurred mips
// resampled through each eye's box (both halves). The focus pass constants name it (76.z refraction). Slots: 61 the
// game blur's SRV, 62 ours (all mips), 64-66 our mips 1-3 (UAV).
// MSAA: our multisampled colour twin resolved into our single-sample colour (both rest as render targets).
void resolve_focus_colour(List* l) {
    ID3D12Resource* ms = focus.resolvedColourTwin.eye.Get();
    ID3D12Resource* colour = focus.resolvedColour.Get();
    if (!ms || !colour)
        return;
    barrier(l, ms, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    l->ResolveSubresource(colour, 0, ms, 0, focus.resolvedColourTwin.viewFormat);
    barrier(l, colour, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, ms, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void focus_blur_record(List* gameList) {
    Replay* r = nullptr;
    ID3D12Resource* colour = nullptr;
    float centre[2][2] = {};
    AcquireSRWLockExclusive(&focus.lock);
    const Twin* twin = nullptr;
    for (const auto& [game, tw] : focus.twins)
        if (!tw.depth && tw.name == "msaaColor" && tw.gameDesc.SampleDesc.Count == 1)
            twin = &tw;
    // MSAA: our resolved colour (resolved again here: the opaque scene). replay_diag=2: only before samples 1, 3, ...
    // (an A/B of the MSAA refraction source)
    if (!twin && focus.resolvedColour && (focus.settings.diagMode != 2 || focus.samplesTaken.load() % 2 == 0))
        twin = &focus.resolvedColourTwin;
    if (!focus.blurBroken && !focus.tilesBroken && focus.gameHeap && focus.slotFirst && focus.atmoPso[0] &&
        focus.centresKnown && twin) {
        colour = twin->focus.Get();
        focus_centres(centre);
        if (!focus.blur || focus.blurColour != colour) {
            ID3D12Resource* game = nullptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            for (const auto& [ptr, info] : focus.srvInfo)
                if (!game && info.second.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
                    debug_name(info.first.Get()) == "opaqueSceneBlur") {
                    game = info.first.Get();
                    view = info.second;
                }
            const D3D12_RESOURCE_DESC cd = colour->GetDesc();
            if (game && game->GetDesc().MipLevels >= 2 && game->GetDesc().Format == cd.Format) {
                retire(focus.blur.Get());
                D3D12_RESOURCE_DESC d = cd;
                d.MipLevels = std::min<UINT16>(4, game->GetDesc().MipLevels);
                d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
                ComPtr<ID3D12Resource> b;
                if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                              D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                              IID_PPV_ARGS(&b)))) {
                    inReplay = true;
                    D3D12_SHADER_RESOURCE_VIEW_DESC gv = view;
                    gv.Texture2D.MostDetailedMip = 0;
                    gv.Texture2D.MipLevels = static_cast<UINT>(-1);
                    srvViewOriginal(device.Get(), game, &gv, game_cpu(focus.slotFirst + 61));
                    D3D12_SHADER_RESOURCE_VIEW_DESC ov{};
                    ov.Format = twin->viewFormat;
                    ov.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    ov.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    ov.Texture2D.MipLevels = d.MipLevels;
                    srvViewOriginal(device.Get(), b.Get(), &ov, game_cpu(focus.slotFirst + 62));
                    for (UINT m = 1; m < d.MipLevels; ++m) {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                        u.Format = twin->viewFormat;
                        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        u.Texture2D.MipSlice = m;
                        uavViewOriginal(device.Get(), b.Get(), nullptr, &u, game_cpu(focus.slotFirst + 63 + m));
                    }
                    inReplay = false;
                    focus.blur = b;
                    focus.blurColour = colour;
                    focus.blurMips = d.MipLevels;
                    logd("replay focus: refraction: our opaqueSceneBlur %llux%u, %u mips (mip 0 our colour, the rest the "
                         "eye's resampled), at bindless %u",
                         static_cast<unsigned long long>(d.Width), d.Height, static_cast<unsigned>(d.MipLevels),
                         focus.slotFirst + 62 - focus.baseIndex);
                } else {
                    log_line("replay focus: refraction: could not make our blur; none");
                    focus.blurBroken = true;
                }
            } else if (!focus.blurMissLogged) {
                focus.blurMissLogged = true;
                logd("replay focus: refraction: the game's opaqueSceneBlur not seen yet (or another format)");
            }
        }
        if (focus.blur && !focus.blurBroken)
            r = acquire_replay();
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepBlur);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    if (colour == focus.resolvedColour.Get())
        resolve_focus_colour(l);
    // mip 0: our opaque focus colour
    barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(l, focus.blur.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{focus.blur.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{colour, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(l, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, focus.blur.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // mips 1..: the game's mips through each box
    const D3D12_RESOURCE_DESC d = focus.blur->GetDesc();
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    l->SetComputeRootSignature(focus.atmoRoot.Get());
    l->SetPipelineState(focus.atmoPso[0].Get());
    for (UINT m = 1; m < focus.blurMips; ++m) {
        uint32_t p[10];
        const float f0[3] = {box, centre[0][0], centre[0][1]};
        std::memcpy(p, f0, sizeof(f0));
        p[3] = std::max<UINT>(1, static_cast<UINT>(d.Width >> m));
        p[4] = std::max<UINT>(1, d.Height >> m);
        p[5] = 1;
        p[6] = 1;
        std::memcpy(&p[7], &centre[1][0], 4);
        std::memcpy(&p[8], &centre[1][1], 4);
        const float mip = static_cast<float>(m);
        std::memcpy(&p[9], &mip, 4);
        l->SetComputeRoot32BitConstants(0, 10, p, 0);
        l->SetComputeRoot32BitConstants(0, 1, &boxYBits, 10);  // the box's height (kAtmoShader boxY)
        atmo_source_region(l, true);
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 61));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 63 + m));
        l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, 1);
    }
    barrier(l, focus.blur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    add_pending(gameList, r);
    focus.blurReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.blurReplays.fetch_add(1, std::memory_order_relaxed);
}

// ---- R2c: the focus views' previous-frame colour ----------------------------------------------------------------

// Under focus.lock, on the submit thread before a frame's clear: our msaaColor focus twin (last frame's lit focus
// views) copied into prevColour, which the focus pass constants name as previousFrameColor (ACE's SSR reads it).
void focus_prev_copy(ID3D12CommandQueue* queue) {
    if (!focus.settings.tiles || focus.tilesBroken || !focus.gameHeap || !focus.slotFirst)
        return;
    const Twin* colour = nullptr;
    for (const auto& [game, tw] : focus.twins)
        if (!tw.depth && tw.name == "msaaColor" && tw.gameDesc.SampleDesc.Count == 1)
            colour = &tw;
    if (!colour && focus.resolvedColour)  // MSAA: last frame's resolve (made in the post)
        colour = &focus.resolvedColourTwin;
    if (!colour)
        return;
    if (focus.prevSource != colour->focus.Get()) {
        retire(focus.prevColour.Get());
        retire(focus.prevList.Get());
        retire(focus.prevAllocator.Get());
        focus.prevColour.Reset();
        focus.prevList.Reset();
        focus.prevAllocator.Reset();
        focus.prevReady = false;
        D3D12_RESOURCE_DESC d = colour->focus->GetDesc();
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                   nullptr, IID_PPV_ARGS(&focus.prevColour))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&focus.prevAllocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, focus.prevAllocator.Get(), nullptr,
                                             IID_PPV_ARGS(&focus.prevList)))) {
            log_line("replay focus: previous-frame colour: could not make our copy");
            focus.prevSource = colour->focus.Get();
            return;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = colour->viewFormat;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        srvViewOriginal(device.Get(), focus.prevColour.Get(), &v, game_cpu(focus.slotFirst + 13));
        inReplay = true;
        List* l = focus.prevList.Get();
        barrier(l, colour->focus.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(l, focus.prevColour.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(focus.prevColour.Get(), colour->focus.Get());
        barrier(l, focus.prevColour.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        barrier(l, colour->focus.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        l->Close();
        inReplay = false;
        focus.prevSource = colour->focus.Get();
        focus.prevReady = true;
        logd("replay focus: previous-frame colour: ours at bindless %u (the game's post_process_color views %d %d %d %d)",
             focus.slotFirst + 13 - focus.baseIndex, static_cast<int>(focus.gamePrev[0]), static_cast<int>(focus.gamePrev[1]),
             static_cast<int>(focus.gamePrev[2]), static_cast<int>(focus.gamePrev[3]));
    }
    if (focus.prevReady) {
        ID3D12CommandList* lists[] = {focus.prevList.Get()};
        executeOriginal(queue, 1, lists);
    }
}

}  // namespace simqvfr::replay_detail

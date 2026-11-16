// Focus culling's record (replay_internal.h has the file map; FocusCull there says what it does): at the Close of
// ACE's instance visibility list, its culling and finalize dispatches again for the focus views, into our buffers.
#include "replay_internal.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "../../log.h"

namespace simqvfr::replay_detail {

// At the game's Close of its instance visibility list: the eyes' culling again for the focus views, into our buffers.
void focus_cull_record(List* gameList, const Stream& s) {
    static int epoch = 0, missed = 0;
    const auto miss = [&](const char* why) {
        if (miss_allowed(epoch, missed, 4))
            logd("replay cull: not this frame (%s)", why);
    };
    std::vector<const Stream::VisDispatch*> eyes;
    const Stream::VisDispatch* finalize = nullptr;
    for (const Stream::VisDispatch& d : s.visDispatches) {
        if (d.kind == 25 && d.consts[49] != 0)  // dualFrustumCull: the two eyes
            eyes.push_back(&d);
        else if (d.kind == 27)
            finalize = &d;
    }
    if (eyes.empty() || !finalize)
        return miss(eyes.empty() ? "no culling dispatch with two frustums in ACE's visibility list" : "no finalize dispatch");
    // The count and command templates ACE copied in from its upload buffer, by their targets' names.
    const Stream::BufferCopy* countsCopy = nullptr;
    const Stream::BufferCopy* commandsCopy = nullptr;
    for (const Stream::BufferCopy& c : s.bufferCopies) {
        const std::string name = debug_name(c.dst);
        if (name.rfind("instanceVisibilityPerLodCount", 0) == 0)
            countsCopy = &c;
        else if (name.rfind("instancesIndirectCommandBuffer", 0) == 0)
            commandsCopy = &c;
    }
    if (!countsCopy || !commandsCopy)
        return miss("the copies into ACE's count and command buffers were not seen in its visibility list");
    uint64_t gameVa = 0;
    std::memcpy(&gameVa, eyes[0]->consts + 20, 8);  // outputInstancesBufferAddress
    cull.gameInstancesVa.store(gameVa, std::memory_order_relaxed);
    const uint64_t gameBytes = cull.gameInstancesBytes.load(std::memory_order_relaxed);
    if (!gameVa || !gameBytes)
        return miss("the size of ACE's visible-instances buffer is not known yet (no vertex buffer view at its address seen)");
    Replay* r = nullptr;
    float centre[2][2];
    AcquireSRWLockExclusive(&focus.lock);
    const bool base = focus.gameHeap && focus.slotFirst && focus.gameHeap.Get() == finalize->heap && !cull.broken && focus.centresKnown;
    if (base) {
        const UINT64 countsBytes = countsCopy->dst->GetDesc().Width, commandsBytes = commandsCopy->dst->GetDesc().Width;
        if (!cull.instances || cull.instancesBytes != gameBytes || cull.countsBytes != countsBytes ||
            cull.commandsBytes != commandsBytes) {
            retire(cull.instances.Get());
            retire(cull.counts.Get());
            retire(cull.commands.Get());
            const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            const auto make = [&](UINT64 bytes, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
                const D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
                out.Reset();
                return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
            };
            // (their states between our lists: what the draws read them in, and the counts ready to be written)
            if (!make(gameBytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, cull.instances) ||
                !make(countsBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, cull.counts) ||
                !make(commandsBytes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, cull.commands)) {
                cull.broken = true;
                log_line("replay cull: our buffers could not be made; the focus replays stay whole");
            } else {
                cull.instancesBytes = gameBytes;
                cull.countsBytes = countsBytes;
                cull.commandsBytes = commandsBytes;
                cull.instancesVa = cull.instances->GetGPUVirtualAddress();
                D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};  // instanceOutputs: a RWByteAddressBuffer
                raw.Format = DXGI_FORMAT_R32_TYPELESS;
                raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                raw.Buffer.NumElements = static_cast<UINT>(gameBytes / 4);
                raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                uavViewOriginal(device.Get(), cull.instances.Get(), nullptr, &raw, game_cpu(focus.slotFirst + kCullSlot));
                structured_views(cull.counts.Get(), static_cast<UINT>(countsBytes / 4), 4, focus.slotFirst + kCullSlot + 1,
                                 focus.slotFirst + kCullSlot + 2);
                structured_views(cull.commands.Get(), static_cast<UINT>(commandsBytes / 32), 32, focus.slotFirst + kCullSlot + 3,
                                 focus.slotFirst + kCullSlot + 4);
                logf("replay cull: our buffers made: visible instances %llu B (ACE's at 0x%llx), counts %llu B, commands %llu B "
                     "(%llu draws); %zu eye dispatches a frame",
                     static_cast<unsigned long long>(gameBytes), static_cast<unsigned long long>(gameVa),
                     static_cast<unsigned long long>(countsBytes), static_cast<unsigned long long>(commandsBytes),
                     static_cast<unsigned long long>(commandsBytes / 32), eyes.size());
            }
        }
        cull.gameCommands = commandsCopy->dst;
        if (!cull.broken && countsCopy->bytes <= cull.countsBytes && commandsCopy->bytes <= cull.commandsBytes)
            r = acquire_replay();
        focus_centres(centre);
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return miss(base ? "no replay entry, or ACE's copies are larger than our buffers" : "the heap, our slots or the eyes' centres aren't known yet");
    // Each eye's frustum remapped to its focus box (a margin wider): x' = a (x - cx w), y' = b (y - cy w). In ACE's
    // visibilityGlobals each register holds one input coordinate's share of clip x, y, z, w (the shader builds its
    // planes as -(register.w + register.x) and so on, one component per register): x, y and w are components 0, 1, 3 of
    // every register.
    const float box = static_cast<float>(focus.settings.boxFraction) * 1.15f, boxY = focus_box_y() * 1.15f;
    const auto remap = [&](uint32_t* m, const float (&c)[2]) {
        float v[16];
        std::memcpy(v, m, sizeof(v));
        for (int k = 0; k < 4; ++k) {
            v[4 * k] = (v[4 * k] - c[0] * v[4 * k + 3]) / box;
            v[4 * k + 1] = (v[4 * k + 1] - c[1] * v[4 * k + 3]) / boxY;
        }
        std::memcpy(m, v, sizeof(v));
    };
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepCull);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    barrier(l, cull.counts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(l, cull.commands.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyBufferRegion(cull.counts.Get(), countsCopy->dstOffset, countsCopy->src, countsCopy->srcOffset, countsCopy->bytes);
    l->CopyBufferRegion(cull.commands.Get(), commandsCopy->dstOffset, commandsCopy->src, commandsCopy->srcOffset, commandsCopy->bytes);
    barrier(l, cull.counts.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(l, cull.commands.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(l, cull.instances.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint64_t mineVa = cull.instancesVa;
    for (const Stream::VisDispatch* d : eyes) {
        uint32_t c[52];
        std::memcpy(c, d->consts, sizeof(c));
        remap(c, centre[0]);
        remap(c + 28, centre[1]);
        std::memcpy(c + 20, &mineVa, 8);
        l->SetComputeRootSignature(d->root);
        l->SetPipelineState(d->pso);
        l->SetComputeRoot32BitConstants(0, 52, c, 0);
        for (UINT t = 1; t <= 4; ++t)
            l->SetComputeRootDescriptorTable(t, d->tables[t]);
        l->SetComputeRootDescriptorTable(5, game_gpu(focus.slotFirst + kCullSlot));
        l->SetComputeRootDescriptorTable(6, game_gpu(focus.slotFirst + kCullSlot + 1));
        l->SetComputeRootDescriptorTable(7, d->tables[7]);  // ACE's stats
        l->Dispatch(d->dims[0], d->dims[1], d->dims[2]);
    }
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    l->ResourceBarrier(1, &uav);
    barrier(l, cull.counts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l->SetComputeRootSignature(finalize->root);
    l->SetPipelineState(finalize->pso);
    l->SetComputeRootDescriptorTable(0, finalize->tables[0]);
    l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + kCullSlot + 2));
    l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + kCullSlot + 3));
    l->SetComputeRootDescriptorTable(3, finalize->tables[3]);
    l->Dispatch(finalize->dims[0], finalize->dims[1], finalize->dims[2]);
    barrier(l, cull.instances.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    barrier(l, cull.commands.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    barrier(l, cull.counts.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    add_pending(gameList, r);
    ReleaseSRWLockExclusive(&focus.lock);
    cull.readyFrame.store(focus.frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    cull.culls.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        logd("replay cull: the first focus culling recorded: %zu eye dispatches (the first %ux%ux%u), finalize %ux%ux%u; box "
             "%.3f x %.3f around %.3f,%.3f and %.3f,%.3f; counts copied %llu B, commands %llu B",
             eyes.size(), eyes[0]->dims[0], eyes[0]->dims[1], eyes[0]->dims[2], finalize->dims[0], finalize->dims[1],
             finalize->dims[2], box, boxY, centre[0][0], centre[0][1], centre[1][0], centre[1][1],
             static_cast<unsigned long long>(countsCopy->bytes), static_cast<unsigned long long>(commandsCopy->bytes));
}

}  // namespace simqvfr::replay_detail

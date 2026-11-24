// The live periphery's per-frame parts (replay_internal.h has the file map; the mechanism and the per-draw parts:
// replay_probe.cpp): the patch pipeline, the constants-copy lists recorded at a game list's Close, the region of
// a borrowed eye image.
#include "replay_internal.h"

#include <d3dcompiler.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "../../log.h"
#include "replay_shaders.h"

namespace simqvfr::replay_detail {

namespace {
constexpr char kEyeShader[] = R"(
cbuffer Source : register(b0) { float4 src[35]; };
cbuffer Params : register(b1) { uint mode; float fx; float fy; };
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    const uint rows = mode == 1 ? 15 : mode == 2 ? 18 : mode == 3 ? 35 : mode == 4 ? 22 : mode == 5 ? 2 : mode == 7 ? 17 : 3;
    if (i >= rows) return;
    float4 v = src[i];
    const float4 f = float4(fx, fy, fx, fy);
    if (mode == 1) {  // tile culling: int2 dimension, int2 pixelOffset
        if (i == 13) v = asfloat(int4(round(float4(asint(v)) * f)));
    } else if (mode == 2) {  // normal reconstruction: the inverse projections' columns in rows 4-7 and 12-15
        if (i == 5 || i == 13) v = v / fy;
        if (i == 7 || i == 15) v = v + (1 - 1 / fy) * src[i - 2];
        if (i == 17) { v.z = v.z * fx; v.w = v.w / fx; }
    } else if (mode == 3) {  // deferred shadows
        if (i == 32) v = v * f;
    } else if (mode == 5) {  // histogram: uint2 resolution (the colour is loaded by pixel below it)
        if (i == 0) { uint4 r = asuint(v); r.z = uint(round(r.z * fx)); r.w = uint(round(r.w * fy)); v = asfloat(r); }
    } else if (mode == 6) {  // histogramAverage: uint pixelCount
        if (i == 0) { uint4 r = asuint(v); r.x = uint(round(r.x * fx * fy)); v = asfloat(r); }
    } else if (mode == 7) {  // vrAntialiasing: float2 viewportOffset, float2 viewportSize (whole pixels)
        if (i == 16) v = round(v * f);
    } else {  // velocity and depth packing
        if (i == 12) v.xy = v.xy / f.xy;
        if (i == 15) { int4 r = asint(v); r.y = int(round(r.y * fx)); r.z = int(round(r.z * fy)); v = asfloat(r); }
        if (i == 17) v.xy = v.xy * f.xy;
        if (i == 18) v = v * f;
    }
    dst.Store4(i * 16, asuint(v));
}
)";
}  // namespace

// At a frame's end (one thread): the copy buffer and the pipeline, once.
bool eye_ready() {
    if (eyeLive.tried)
        return eyeLive.ready;
    eyeLive.tried = true;
    if (!focus.patchRoot || !focus.patchPso)
        return false;
    ComPtr<ID3DBlob> code, errors;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = focus.patchRoot.Get();
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kEyeSlots} * kSlotBytes, 1, 1, 1,
                                     DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(D3DCompile(kEyeShader, sizeof(kEyeShader) - 1, "eye-live", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                          &errors))) {
        logf("replay eye: the constants shader did not compile (%s); the periphery keeps its launch size",
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&eyeLive.pso))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr,
                                               IID_PPV_ARGS(&eyeLive.constants)))) {
        log_line("replay eye: the constants pipeline or buffer could not be made; the periphery keeps its launch size");
        return false;
    }
    eyeLive.ready = true;
    {  // Detailed logs: the dumps' buffers (none: no dumps)
        const D3D12_HEAP_PROPERTIES readHeap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC raw = buffer, read = buffer;
        raw.Width = UINT64{EyeLive::kDumps} * 2048;
        read.Width = UINT64{EyeLive::kDumps} * 4096;
        read.Flags = D3D12_RESOURCE_FLAG_NONE;
        void* mapped = nullptr;
        const D3D12_RANGE all{0, 0};
        if (log_debug() &&
            SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                      nullptr, IID_PPV_ARGS(&eyeLive.dumpRaw))) &&
            SUCCEEDED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &read, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, IID_PPV_ARGS(&eyeLive.dumpRead))) &&
            SUCCEEDED(eyeLive.dumpRead->Map(0, &all, &mapped)) && mapped) {
            eyeLive.dumpMapped = static_cast<const uint8_t*>(mapped);
        } else {
            eyeLive.dumpRaw.Reset();
            eyeLive.dumpRead.Reset();
        }
    }
    return true;
}

void eye_dump_log(int k, uint64_t at) {
    const auto rows = [&](int half, const char* which) {
        const float* f = reinterpret_cast<const float*>(eyeLive.dumpMapped + size_t(k) * 4096 + size_t(half) * 2048);
        static const int kRows[] = {84, 85, 112, 13, 17, 21, 25, 14, 18, 22, 26, 16, 95, 103, 107, 96, 104, 108, 98, 110};
        std::string line;
        char cell[96];
        for (const int r : kRows) {
            std::snprintf(cell, sizeof(cell), " %d:%.6g,%.6g,%.6g,%.6g", r, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
            line += cell;
        }
        logd("replay eye: constants dump frame %llu %s:%s", static_cast<unsigned long long>(at), which, line.c_str());
    };
    rows(0, "ACE's");
    rows(1, "ours");
}

// Under focus.lock: a list of ours for the copies (the pool grows as needed: a list that redirected its constants
// must get them).
Replay* eye_acquire() {
    if (!focus.fence)
        return nullptr;
    const uint64_t done = focus.fence->GetCompletedValue();
    for (auto& r : eyeLive.pool)
        if (!r->busy && r->fence <= done) {
            r->busy = true;
            return r.get();
        }
    auto r = std::make_unique<Replay>();
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r->allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&r->list))))
        return nullptr;
    r->list->Close();
    r->busy = true;
    eyeLive.pool.push_back(std::move(r));
    return eyeLive.pool.back().get();
}

// At the game's Close of a list that reads copies of ours: the list that makes them, executed ahead of its batch.
void eye_record(List* gameList, const Stream& s) {
    AcquireSRWLockExclusive(&focus.lock);
    Replay* r = eye_acquire();
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            log_line("replay eye: no command list for the constants copies: a list of ACE's runs with stale constants");
        return;
    }
    const EyeNow now = eye_now(s.eyeState);
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kStepEyeCopies);
    barrier(l, eyeLive.constants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(focus.patchRoot.Get());
    const Stream::EyePatch* dump = nullptr;
    int dumpSlot = 0;
    uint64_t dumpFrameAt = 0;
    for (const Stream::EyePatch& p : s.eyePatches) {
        uint32_t params[kPatchParams] = {};
        if (p.mode == 0) {
            const float floats[4] = {1.0f, 1.0f, p.w, p.h};  // the whole eye, at the list's live view size
            std::memcpy(params, floats, sizeof(floats));
            params[34] = 1;  // eyeMode
            std::memcpy(params + 35, &s.eyeBias, 4);  // mipBias
            params[22] = right_eye_fix_flags();  // flipRightY
            l->SetPipelineState(focus.patchPso.Get());
        } else {
            params[0] = static_cast<uint32_t>(p.mode);
            std::memcpy(params + 1, &now.fx, 4);
            std::memcpy(params + 2, &now.fy, 4);
            l->SetPipelineState(eyeLive.pso.Get());
        }
        l->SetComputeRoot32BitConstants(1, kPatchParams, params, 0);
        l->SetComputeRootConstantBufferView(0, p.from);
        l->SetComputeRootUnorderedAccessView(2, p.to);
        l->Dispatch(p.mode == 0 ? 3 : 1, 1, 1);
        // Detailed logs: this list's pass constants as ACE gave them and as ours, one list a frame
        if (p.mode == 0 && !dump && eyeLive.dumpMapped && eyeLive.dumpLeft.load(std::memory_order_relaxed) > 0) {
            const uint64_t f = focus.frame.load(std::memory_order_relaxed);
            uint64_t was = eyeLive.dumpFrame.load();
            if (was != f && eyeLive.dumpFrame.compare_exchange_strong(was, f)) {
                const int slot = eyeLive.dumpLeft.fetch_sub(1) - 1;
                if (slot >= 0) {
                    params[23] = 1;  // rawCopy
                    l->SetComputeRoot32BitConstants(1, kPatchParams, params, 0);
                    l->SetComputeRootUnorderedAccessView(2, eyeLive.dumpRaw->GetGPUVirtualAddress() + UINT64(slot) * 2048);
                    l->Dispatch(3, 1, 1);
                    dump = &p;
                    dumpSlot = slot;
                    dumpFrameAt = f;
                }
            }
        }
    }
    if (dump) {
        barrier(l, eyeLive.dumpRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(l, eyeLive.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const UINT64 at = UINT64(dumpSlot) * 4096;
        l->CopyBufferRegion(eyeLive.dumpRead.Get(), at, eyeLive.dumpRaw.Get(), UINT64(dumpSlot) * 2048, 2048);
        l->CopyBufferRegion(eyeLive.dumpRead.Get(), at + 2048, eyeLive.constants.Get(),
                            dump->to - eyeLive.constants->GetGPUVirtualAddress(), 2048);
        barrier(l, eyeLive.dumpRaw.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(l, eyeLive.constants.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    } else {
        barrier(l, eyeLive.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    l->Close();
    if (dump)
        eyeLive.dumpAt[dumpSlot].store(dumpFrameAt + 1);  // (+1: 0 is a free slot)
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    Replay*& slot = eyeLive.before[gameList];
    if (slot)
        slot->busy = false;  // closed again without having run
    slot = r;
    ReleaseSRWLockExclusive(&focus.lock);
    eyeLive.lists.fetch_add(1, std::memory_order_relaxed);
    eyeLive.patches.fetch_add(s.eyePatches.size(), std::memory_order_relaxed);
}

// The region of a borrowed eye image in use (kAtmoShader's ux, uy at constants 11-12; 0: all of it).
void atmo_source_region(List* l, bool eyeImage, bool output) {
    const EyeNow now = eye_now(eyeImage ? eyeLive.state.load(std::memory_order_relaxed) : 0);
    const EyeOut o = eye_out(now, now.on && output ? eyeLive.out.load(std::memory_order_relaxed) : 0);
    const float region[2] = {now.on ? o.lw / o.w : 0.0f, now.on ? o.lh / o.h : 0.0f};
    l->SetComputeRoot32BitConstants(0, 2, region, 11);
}

}  // namespace simqvfr::replay_detail

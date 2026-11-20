#include "replay_probe.h"

#include <d3dcompiler.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../../xr.h"
#include "../../probe/d3d11_capture.h"  // write_png_bgra
#include "../../probe/focus_ngx.h"
#include "replay_internal.h"
#include "replay_shaders.h"
#include "stereo_flag.h"

extern "C" size_t simqvfr_slot_SetPipelineState(void);
extern "C" size_t simqvfr_slot_CreateConstantBufferView(void);
extern "C" size_t simqvfr_slot_CreateGraphicsPipelineState(void);
extern "C" size_t simqvfr_slot_CreatePipelineState(void);
extern "C" size_t simqvfr_slot_CreatePipelineLibrary(void);
extern "C" size_t simqvfr_slot_LoadGraphicsPipeline(void);
extern "C" size_t simqvfr_slot_LoadPipeline(void);
extern "C" size_t simqvfr_slot_DrawInstanced(void);
extern "C" size_t simqvfr_slot_DrawIndexedInstanced(void);
extern "C" size_t simqvfr_slot_ExecuteIndirect(void);
extern "C" size_t simqvfr_slot_RSSetViewports(void);
extern "C" size_t simqvfr_slot_Close(void);
extern "C" size_t simqvfr_slot_Reset(void);
extern "C" size_t simqvfr_slot_SetGraphicsRootSignature(void);
extern "C" size_t simqvfr_slot_SetDescriptorHeaps(void);
extern "C" size_t simqvfr_slot_SetGraphicsRootDescriptorTable(void);
extern "C" size_t simqvfr_slot_SetGraphicsRoot32BitConstant(void);
extern "C" size_t simqvfr_slot_SetGraphicsRoot32BitConstants(void);
extern "C" size_t simqvfr_slot_SetGraphicsRootConstantBufferView(void);
extern "C" size_t simqvfr_slot_SetGraphicsRootShaderResourceView(void);
extern "C" size_t simqvfr_slot_IASetPrimitiveTopology(void);
extern "C" size_t simqvfr_slot_IASetIndexBuffer(void);
extern "C" size_t simqvfr_slot_IASetVertexBuffers(void);
extern "C" size_t simqvfr_slot_RSSetScissorRects(void);
extern "C" size_t simqvfr_slot_OMSetStencilRef(void);
extern "C" size_t simqvfr_slot_OMSetRenderTargets(void);
extern "C" size_t simqvfr_slot_CreateDepthStencilView(void);
extern "C" size_t simqvfr_slot_CreateRenderTargetView(void);
extern "C" size_t simqvfr_slot_ExecuteCommandLists(void);
extern "C" size_t simqvfr_slot_SetComputeRootSignature(void);
extern "C" size_t simqvfr_slot_SetComputeRoot32BitConstants(void);
extern "C" size_t simqvfr_slot_SetComputeRootConstantBufferView(void);
extern "C" size_t simqvfr_slot_SetComputeRootDescriptorTable(void);
extern "C" size_t simqvfr_slot_Dispatch(void);
extern "C" size_t simqvfr_slot_CopyBufferRegion(void);
extern "C" size_t simqvfr_slot_CreateShaderResourceView(void);
extern "C" size_t simqvfr_slot_CopyDescriptorsSimple(void);
extern "C" size_t simqvfr_slot_CreateUnorderedAccessView(void);

namespace simqvfr {

using namespace replay_detail;  // what this file shares with replay_samples.cpp, replay_cull.cpp

namespace {
using CloseFn = HRESULT(STDMETHODCALLTYPE*)(List*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using RootSigFn = void(STDMETHODCALLTYPE*)(List*, ID3D12RootSignature*);
using PsoFn = void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*);
using HeapsFn = void(STDMETHODCALLTYPE*)(List*, UINT, ID3D12DescriptorHeap* const*);
using TableFn = void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using Const1Fn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
using ConstNFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const void*, UINT);
using VaFn = void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using TopologyFn = void(STDMETHODCALLTYPE*)(List*, D3D12_PRIMITIVE_TOPOLOGY);
using IbFn = void(STDMETHODCALLTYPE*)(List*, const D3D12_INDEX_BUFFER_VIEW*);
using VbFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using ViewportsFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*);
using ScissorsFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RECT*);
using StencilFn = void(STDMETHODCALLTYPE*)(List*, UINT);
using DrawFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT);
using IndirectFn = void(STDMETHODCALLTYPE*)(List*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                            ID3D12Resource*, UINT64);
using OmFn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE*);
using DispatchFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
using CopySimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
                                              D3D12_DESCRIPTOR_HEAP_TYPE);

CloseFn closeOriginal;
ResetFn resetOriginal;
RootSigFn rootSigOriginal;
PsoFn psoOriginal;
HeapsFn heapsOriginal;
TableFn tableOriginal;
Const1Fn const1Original;
ConstNFn constNOriginal;
VaFn cbvOriginal, srvOriginal;
TopologyFn topologyOriginal;
IbFn ibOriginal;
VbFn vbOriginal;
ViewportsFn viewportsOriginal;
ScissorsFn scissorsOriginal;
StencilFn stencilOriginal;
DrawFn drawOriginal;
DrawIndexedFn drawIndexedOriginal;
IndirectFn indirectOriginal;
OmFn omOriginal;
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
DsvFn dsvOriginal;
RtvFn rtvOriginal;
ExecuteFn executeOriginal;
}  // namespace replay_detail
namespace {
RootSigFn computeRootSigOriginal;
ConstNFn computeConstNOriginal;
VaFn computeCbvOriginal;
TableFn computeTableOriginal;
DispatchFn dispatchOriginal;
using CopyRegionFn = void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
CopyRegionFn copyRegionOriginal;
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
SrvFn srvViewOriginal;
}  // namespace replay_detail
namespace {
using GfxPsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using StreamPsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
CbvFn cbvViewOriginal;
}  // namespace replay_detail
namespace {
GfxPsoFn gfxPsoOriginal;
StreamPsoFn streamPsoOriginal;
using LibGfxFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                            REFIID, void**);
using LibStreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*,
                                               REFIID, void**);
LibGfxFn libGfxOriginal;
LibStreamFn libStreamOriginal;
CopySimpleFn copySimpleOriginal;
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
UavFn uavViewOriginal;  // our own UAVs go straight to it (the hook takes focus.lock)
}  // namespace replay_detail
namespace {
using CopyRangesFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
CopyRangesFn copyRangesOriginal;
// ID3D12Device declares CopyDescriptors right before CopyDescriptorsSimple (d3d12.h).
size_t slot_CopyDescriptors() { return simqvfr_slot_CopyDescriptorsSimple() - 1; }

constexpr UINT kPassConstantsRoot = 1;  // ACE: root parameter 1 of scene draws is the 2 KB pass constants

}  // namespace
namespace replay_detail {  // declared in replay_internal.h
ComPtr<ID3D12Device> device;
}  // namespace replay_detail
namespace {
std::atomic<bool> installed{false};
int probeMode = 0;
std::atomic<bool> replayOn{true};
std::atomic<bool> focusActive{true};  // mode 3 cost A/B: the focus replays recorded at all
uint32_t minDraws = 16;
float minWidth = 512.0f;
double msPerTick = 0, msPerCycle = 0;

SRWLOCK streamsLock = SRWLOCK_INIT;
std::unordered_map<List*, std::unique_ptr<Stream>> streams;

struct Current {
    List* list = nullptr;
    Stream* stream = nullptr;
};
thread_local Current current;
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
thread_local bool inReplay = false;
}  // namespace replay_detail
namespace {
thread_local ComPtr<ID3D12CommandAllocator> replayAllocator;
thread_local ComPtr<List> replayList;
thread_local bool replayBroken = false;
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
std::atomic<uint64_t> listsSeen{0}, listsReplayed{0}, callsReplayed{0}, drawsReplayed{0}, focusReplays{0};
std::atomic<uint64_t> replayTicks{0}, hookCycles{0}, gameTicks{0}, frames{0};
RecordTiming recordTiming;
thread_local uint64_t skyCheckTicks = 0;  // emit's sky checks on this thread (read around one emit)
thread_local uint64_t emitOps = 0;
std::atomic<uint64_t> twinsMade{0}, twinsRemade{0};
}  // namespace replay_detail
namespace {
uint64_t windowFrames = 0, window = 0;
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
int64_t now_ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
}  // namespace replay_detail
namespace {
// The active stream of `list` on this thread, or null (not recording: no Reset seen, a bundle, inside a replay).
Stream* stream_of(List* list) {
    if (inReplay || !installed.load(std::memory_order_relaxed))  // hooks in, but the start not done (or failed)
        return nullptr;
    if (current.list == list)
        return current.stream && current.stream->active ? current.stream : nullptr;
    AcquireSRWLockShared(&streamsLock);
    const auto it = streams.find(list);
    Stream* s = it == streams.end() ? nullptr : it->second.get();
    ReleaseSRWLockShared(&streamsLock);
    current = {list, s};
    return s && s->active ? s : nullptr;
}

// Times the recording part of a hook in TSC cycles, charged to the list's stream.
struct HookTimer {
    Stream* s;
    uint64_t start;
    explicit HookTimer(Stream* stream) : s(stream), start(stream ? __rdtsc() : 0) {}
    ~HookTimer() {
        if (s)
            s->hookCycles += __rdtsc() - start;
    }
};
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
// replay_cull_probe: what this 900-frame window's focus scene replays leave out (0 nothing, 1 ExecuteIndirect, 2 the
// direct draws), and how many of each kind they emitted.
std::atomic<int> cullProbePhase{0};
}  // namespace replay_detail
namespace {
std::atomic<uint64_t> probeIndirect{0}, probeDirect{0};

SIZE_T map_target(const Substitution* sub, SIZE_T h) {
    if (sub && sub->targets)
        for (const auto& [from, to] : *sub->targets)
            if (from == h)
                return to;
    return 0;
}

bool is_sky_pso(ID3D12PipelineState* pso);  // the sky's pipeline (its pixel shader declares cbSky)
bool is_tyre_pso(ID3D12PipelineState* pso);  // ACE's tyre pipelines (no single-pass-stereo variant)
// A tyre draw again for the right eye (defined with the focus setup below).
void tyre_right_draw(List* l, std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS>>& made,
                     D3D12_GPU_VIRTUAL_ADDRESS constants, ID3D12PipelineState* pso, const D3D12_VIEWPORT* vp, UINT vpCount,
                     const D3D12_RECT* sc, UINT scCount, UINT indices, UINT start, INT base, UINT firstInstance);
std::atomic<uint64_t> skyPatchCount{0};    // focus sky draws given our cbSky
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
// Re-emits a stream into `l` (open). Returns the draws emitted, or -1 on a broken stream or a target without a twin.
int64_t emit(List* l, const Stream& s, const Substitution* sub) {
    const uint8_t* p = s.bytes.data();
    const uint8_t* const end = p + s.bytes.size();
    const auto get = [&p](auto& v) {
        std::memcpy(&v, p, sizeof(v));
        p += sizeof(v);
    };
    int64_t draws = 0;
    ID3D12PipelineState* curPso = nullptr;
    UINT64 curTable4 = 0;
    // ACE's tyres (one instance, the left eye only): drawn again for the right focus view
    bool curTyre = false;
    D3D12_GPU_VIRTUAL_ADDRESS curCbv1 = 0;
    std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS>> tyreConstants;
    // A sky draw: root 4 set to our table (cbSky patched) for the draw, the game's back after.
    const auto skyTable = [&]() -> UINT64 {
        if (!sub || !sub->skyTables || !curTable4 || !is_sky_pso(curPso))
            return 0;
        for (const auto& [from, to] : *sub->skyTables)
            if (from == curTable4)
                return to;
        return 0;
    };
    while (p < end) {
        const Op op = static_cast<Op>(*p++);
        UINT64 skyMine = 0;
        if (op == kDraw || op == kDrawIndexed || op == kIndirect) {
            if (recordTiming.on) {
                const int64_t t = now_ticks();
                skyMine = skyTable();
                skyCheckTicks += static_cast<uint64_t>(now_ticks() - t);
            } else {
                skyMine = skyTable();
            }
        }
        ++emitOps;
        if (skyMine) {
            l->SetGraphicsRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{skyMine});
            skyPatchCount.fetch_add(1, std::memory_order_relaxed);
        }
        switch (op) {
        case kRootSig: { ID3D12RootSignature* r; get(r); l->SetGraphicsRootSignature(r); break; }
        case kPso: {
            ID3D12PipelineState* r; get(r);
            curPso = r;
            curTyre = sub && sub->count == 2 && sub->viewports && sub->scissors && is_tyre_pso(r);
            l->SetPipelineState(r);
            break;
        }
        case kHeaps: {
            UINT n; get(n);
            ID3D12DescriptorHeap* heaps[2] = {};
            for (UINT i = 0; i < n; ++i) get(heaps[i]);
            l->SetDescriptorHeaps(n, heaps);
            break;
        }
        case kTable: {
            UINT i; D3D12_GPU_DESCRIPTOR_HANDLE h; get(i); get(h);
            if (sub && sub->tables)
                for (const auto& [from, to] : *sub->tables)
                    if (from == h.ptr) {
                        h.ptr = to;
                        break;
                    }
            l->SetGraphicsRootDescriptorTable(i, h);
            if (i == 4)
                curTable4 = h.ptr;
            break;
        }
        case kConst1: { UINT i, v, o; get(i); get(v); get(o); l->SetGraphicsRoot32BitConstant(i, v, o); break; }
        case kConstN: {
            UINT i, n, o; get(i); get(n); get(o);
            l->SetGraphicsRoot32BitConstants(i, n, p, o);
            p += size_t{n} * 4;
            break;
        }
        case kCbv: {
            UINT i; D3D12_GPU_VIRTUAL_ADDRESS va; get(i); get(va);
            if (sub && sub->from && (i == kPassConstantsRoot || i == 0))  // pass constants; frameData (root 0)
                for (size_t k = 0; k < sub->from->size(); ++k)
                    if ((*sub->from)[k] == va) {
                        va = (*sub->to)[k];
                        break;
                    }
            l->SetGraphicsRootConstantBufferView(i, va);
            if (i == kPassConstantsRoot)
                curCbv1 = va;
            break;
        }
        case kSrv: { UINT i; D3D12_GPU_VIRTUAL_ADDRESS va; get(i); get(va); l->SetGraphicsRootShaderResourceView(i, va); break; }
        case kTopology: { D3D12_PRIMITIVE_TOPOLOGY t; get(t); l->IASetPrimitiveTopology(t); break; }
        case kIb: {
            uint8_t has; get(has);
            D3D12_INDEX_BUFFER_VIEW v{};
            if (has) get(v);
            l->IASetIndexBuffer(has ? &v : nullptr);
            break;
        }
        case kVb: {
            UINT first, n; uint8_t has; get(first); get(n); get(has);
            const auto* views = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(p);
            D3D12_VERTEX_BUFFER_VIEW mine[16];
            if (has && sub && sub->vbBytes && n <= 16) {  // focus culling: our visible instances
                std::memcpy(mine, views, sizeof(D3D12_VERTEX_BUFFER_VIEW) * n);
                for (UINT i = 0; i < n; ++i)
                    if (mine[i].BufferLocation >= sub->vbFrom && mine[i].BufferLocation < sub->vbFrom + sub->vbBytes)
                        mine[i].BufferLocation = sub->vbTo + (mine[i].BufferLocation - sub->vbFrom);
                l->IASetVertexBuffers(first, n, mine);
            } else {
                l->IASetVertexBuffers(first, n, has ? views : nullptr);
            }
            if (has) p += sizeof(D3D12_VERTEX_BUFFER_VIEW) * n;
            break;
        }
        case kViewports: {
            UINT n; get(n);
            if (sub && sub->viewports)
                l->RSSetViewports(sub->count, sub->viewports);
            else
                l->RSSetViewports(n, reinterpret_cast<const D3D12_VIEWPORT*>(p));
            p += sizeof(D3D12_VIEWPORT) * n;
            break;
        }
        case kScissors: {
            UINT n; get(n);
            if (sub && sub->scissors)
                l->RSSetScissorRects(sub->count, sub->scissors);
            else
                l->RSSetScissorRects(n, reinterpret_cast<const D3D12_RECT*>(p));
            p += sizeof(D3D12_RECT) * n;
            break;
        }
        case kStencil: { UINT r; get(r); l->OMSetStencilRef(r); break; }
        case kOm: {  // recorded in mode 3 only; bound to our twins
            UINT n; get(n);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            for (UINT i = 0; i < n; ++i) {
                SIZE_T h; get(h);
                if (i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
                    rtvs[i].ptr = map_target(sub, h);
            }
            uint8_t hasDsv; get(hasDsv);
            SIZE_T d = 0;
            if (hasDsv) get(d);
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv{map_target(sub, d)};
            const UINT count = std::min<UINT>(n, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
            bool ok = !hasDsv || dsv.ptr;
            for (UINT i = 0; i < count; ++i) ok = ok && rtvs[i].ptr;
            if (!ok)
                return -1;  // a target without a twin: the replay would draw into the game's
            l->OMSetRenderTargets(count, count ? rtvs : nullptr, FALSE, hasDsv ? &dsv : nullptr);
            break;
        }
        case kDraw: {
            UINT a, b, c, d; get(a); get(b); get(c); get(d);
            if (sub && sub->skip == 2)
                break;
            l->DrawInstanced(a, b, c, d);
            ++draws;
            break;
        }
        case kDrawIndexed: {
            UINT a, b, c, e; INT d; get(a); get(b); get(c); get(d); get(e);
            if (sub && sub->skip == 2)
                break;
            if (sub && sub->targets)
                probeDirect.fetch_add(1, std::memory_order_relaxed);
            l->DrawIndexedInstanced(a, b, c, d, e);
            ++draws;
            if (curTyre && b == 1 && curCbv1)
                tyre_right_draw(l, tyreConstants, curCbv1, curPso, sub->viewports, 2, sub->scissors, 2, a, c, d, e);
            break;
        }
        case kIndirect: {
            ID3D12CommandSignature* sig; UINT max; ID3D12Resource* args; UINT64 argsOffset; ID3D12Resource* count;
            UINT64 countOffset;
            get(sig); get(max); get(args); get(argsOffset); get(count); get(countOffset);
            if (sub && sub->skip == 1)
                break;
            if (sub && sub->targets)
                probeIndirect.fetch_add(1, std::memory_order_relaxed);
            if (sub && sub->argsFrom && args == sub->argsFrom)
                args = sub->argsTo;  // focus culling: our draw arguments
            l->ExecuteIndirect(sig, max, args, argsOffset, count, countOffset);
            ++draws;
            break;
        }
        default:
            return -1;
        }
        if (skyMine)  // the game's table back for the draws after
            l->SetGraphicsRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{curTable4});
    }
    return draws;
}
}  // namespace replay_detail
namespace {
// Modes 1/2: re-emission into a list that is never executed.
void replay_unexecuted(const Stream& s) {
    if (replayBroken)
        return;
    if (!replayList) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replayAllocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replayAllocator.Get(), nullptr,
                                             IID_PPV_ARGS(&replayList)))) {
            replayBroken = true;
            log_line("replay probe: could not create a re-emission command list on this thread");
            return;
        }
        replayList->Close();
    }
    const int64_t start = now_ticks();
    if (FAILED(replayAllocator->Reset()) || FAILED(replayList->Reset(replayAllocator.Get(), nullptr))) {
        replayBroken = true;
        log_line("replay probe: re-emission list reset failed on this thread");
        return;
    }
    const int64_t draws = emit(replayList.Get(), s, nullptr);
    replayList->Close();
    if (draws < 0) {
        replayBroken = true;
        log_line("replay probe: unknown op in a stream; re-emission stopped on this thread");
        return;
    }
    replayTicks.fetch_add(static_cast<uint64_t>(now_ticks() - start), std::memory_order_relaxed);
    listsReplayed.fetch_add(1, std::memory_order_relaxed);
    callsReplayed.fetch_add(s.calls, std::memory_order_relaxed);
    drawsReplayed.fetch_add(static_cast<uint64_t>(draws), std::memory_order_relaxed);
}

// ---- Mode 3: replays that draw, into twins of the game's targets ---------------------------------------------------

// (the HLSL sources and our slot layout: replay_shaders.h; Replay, Twin, Focus: replay_internal.h)

}  // namespace
namespace replay_detail {  // declared in replay_internal.h
Focus focus;

bool invert(const double (&m)[4][4], double (&out)[4][4]) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = j < 4 ? m[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12)
            return false;
        for (int j = 0; j < 8; ++j)
            std::swap(a[c][j], a[pivot][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j)
            a[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j)
                    a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = a[i][j + 4];
    return true;
}
}  // namespace replay_detail
namespace {
int pso_kind(ID3D12PipelineState* pso) {
    if (!pso)
        return 0;
    AcquireSRWLockShared(&focus.psoLock);
    const auto it = focus.psoKinds.find(pso);
    const int known = it == focus.psoKinds.end() ? -1 : it->second;
    ReleaseSRWLockShared(&focus.psoLock);
    if (known >= 0)
        return known;
    wchar_t name[64] = {};
    UINT size = sizeof(name) - sizeof(wchar_t);
    int kind = 0;
    if (SUCCEEDED(pso->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        kind = std::wcscmp(name, L"tileFrustums") == 0                  ? 1
               : std::wcscmp(name, L"tiledCulling") == 0                ? 2
               : std::wcscmp(name, L"volumetricFogLightInjection") == 0 ? 3
               : std::wcscmp(name, L"volumetricFogScatter") == 0        ? 4
               : std::wcscmp(name, L"normalReconstructionViewSpace") == 0 ? 5
               : std::wcscmp(name, L"deferredShadows") == 0             ? 6
               : std::wcscmp(name, L"oitComposite") == 0                ? 7
               : std::wcscmp(name, L"atmosphericScatteringUpscaling") == 0 ? 8
               : std::wcscmp(name, L"tonemap") == 0                     ? 9
               : std::wcscmp(name, L"fxaa") == 0                        ? 10
               : std::wcscmp(name, L"gaussianBlur3x3Float4") == 0       ? 11
               : std::wcscmp(name, L"VelocityAndDepthPacking") == 0     ? 12
               : std::wcscmp(name, L"contrastAdaptiveSharpen") == 0     ? 13
               : std::wcscmp(name, L"resolveDepthCompute") == 0         ? 14
               : std::wcscmp(name, L"resolveDepthVelocityCompute") == 0 ? 15
               : std::wcsncmp(name, L"vrAntialiasing", 14) == 0         ? 16
               : std::wcscmp(name, L"atmosphericScattering") == 0       ? 17
               : std::wcscmp(name, L"histogram") == 0                   ? 18
               : std::wcscmp(name, L"histogramAverage") == 0            ? 19
               : std::wcscmp(name, L"gaussianBlur3x3Float3") == 0       ? 20
               : std::wcscmp(name, L"veilLumaDownsample") == 0          ? 21
               : std::wcscmp(name, L"veilDownsample") == 0              ? 22
               : std::wcscmp(name, L"veilUpsample") == 0                ? 23
               : std::wcscmp(name, L"hiZGeneration") == 0               ? 24
               : std::wcscmp(name, L"InstanceVisibilityInstanceCull") == 0 ? 25
               : std::wcscmp(name, L"InstanceVisibilityInstanceCullForDirectionalShadowDepth") == 0 ? 26
               : std::wcscmp(name, L"InstanceVisibilityFinalizeDrawCommands") == 0 ? 27
                                                                         : 0;
    AcquireSRWLockExclusive(&focus.psoLock);
    focus.psoKinds[pso] = kind;
    ReleaseSRWLockExclusive(&focus.psoLock);
    return kind;
}

}  // namespace

// The tile shaders multiply out = M v with M's columns in the 4 registers (tileFrustums' disassembly): eye[4k + i] is
// M[i][k]. inverse(R P) = inverse(P) inverse(R), R: x' = x / box + (ox / box) w, y' likewise.
bool replay_focus_inverse_projection(const float (&eye)[16], double box, float (&out)[16], double gazeX, double gazeY,
                                     double boxY) {
    if (boxY <= 0)
        boxY = box;
    double q[4][4], p[4][4];
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            q[i][k] = eye[k * 4 + i];
    if (!invert(q, p))
        return false;
    // P's x and y rows: (sx, 0, ox, 0), (0, sy, oy, 0); the box's centre is -ox, -oy plus the gaze offset
    const double ox = p[0][2] - gazeX, oy = p[1][2] - gazeY;
    for (int i = 0; i < 4; ++i) {
        out[0 * 4 + i] = static_cast<float>(q[i][0] * box);
        out[1 * 4 + i] = static_cast<float>(q[i][1] * boxY);
        out[2 * 4 + i] = static_cast<float>(q[i][2]);
        out[3 * 4 + i] = static_cast<float>(q[i][3] - q[i][0] * ox - q[i][1] * oy);
    }
    return true;
}

namespace replay_detail {  // declared in replay_internal.h
std::string debug_name(ID3D12Resource* r) {
    wchar_t name[96] = {};
    UINT size = sizeof(name) - sizeof(wchar_t);
    if (FAILED(r->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)) || !name[0])
        return "unnamed";
    std::string out;
    for (const wchar_t* c = name; *c; ++c)
        out += (std::iswalnum(*c) || *c == L'_') ? static_cast<char>(*c) : '_';
    return out;
}
}  // namespace replay_detail
namespace {
// ---- Focus culling (replay_cull) -----------------------------------------------------------------------------------
// What it does and its state: FocusCull in replay_internal.h; its record (focus_cull_record): replay_cull.cpp.
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
FocusCull cull;
}  // namespace replay_detail
namespace {
// ---- GPU time by step (replay_gpu_steps=1) --------------------------------------------------------------------------
// Where a frame's GPU time goes, from D3D12 timestamps on ACE's queue: one frame in sixteen, every list of ours stamps
// its start (the post list three times), and a one-command list of ours stamps the start of each batch of the game's
// (or of our compositor's) and the end of what we add after it. Sorted by time, each stamp owns the stretch up to the
// next: lists run one after another on the queue. Summed by kind and averaged over the samples of a 900-frame window.
// A list recorded in one frame and run in the next is not stamped: its time falls to the stamp before it.
constexpr UINT kStepQueries = 256;
// (the step kinds, kStepScene..kStepKinds: replay_internal.h)
struct GpuSteps {
    bool on = false;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;  // the resolve's
    ComPtr<List> list;
    std::atomic<uint64_t> frame{UINT64_MAX};  // the frame (focus.frame) whose lists are stamped
    std::atomic<uint32_t> next{0};
    uint8_t kinds[kStepQueries] = {};
    // the frame-end thread's
    int phase = 0;  // 0 idle, 1 stamping / waiting to resolve, 2 waiting to read
    uint64_t at = 0;
    uint32_t resolved = 0;
    double sum[kStepKinds] = {};
    uint32_t samples = 0, dropped = 0;
    uint64_t frequency = 0;
};
GpuSteps gpuSteps;
thread_local uint8_t gpuBatchKind = kStepGame;  // what the batches this thread executes are (replay_probe_gpu_scope)

}  // namespace
namespace replay_detail {  // declared in replay_internal.h
// In a list of ours being recorded: a timestamp where `kind` starts.
void gpu_stamp(List* l, uint8_t kind) {
    if (gpuSteps.frame.load(std::memory_order_relaxed) != focus.frame.load(std::memory_order_relaxed))
        return;
    const uint32_t i = gpuSteps.next.fetch_add(1);
    if (i >= kStepQueries)
        return;
    gpuSteps.kinds[i] = kind;
    l->EndQuery(gpuSteps.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
}
}  // namespace replay_detail
namespace {
// Under focus.lock: the game's list was Reset without being executed since its Close: our replays of it never run
// (they would hold pool entries for good, and a later execution of the list would run them stale).
void drop_pending(List* gameList) {
    const auto it = focus.pending.find(gameList);
    if (it == focus.pending.end())
        return;
    for (Replay* r : it->second)
        r->busy = false;  // never submitted: its fence is from its last run (the allocator is reset at its next use)
    focus.pending.erase(it);
    focus.pendingLists.store(focus.pending.size(), std::memory_order_relaxed);
}
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
void barrier(List* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to || !r)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    l->ResourceBarrier(1, &b);
}
}  // namespace replay_detail
namespace {
// ACE's tyres under single-pass stereo: after a tyre draw (one instance, viewport 0 = the left eye) the same draw into
// the second viewport with our right-eye copy of the pass constants `constants` (made in this list, once per address:
// `made`), then the viewports, scissors, pass constants and pipeline back. Calls go to `l` with inReplay set by the
// caller (our hooks pass them through). The compute root signature of `l` is ours afterwards.
void tyre_right_draw(List* l, std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS>>& made,
                     D3D12_GPU_VIRTUAL_ADDRESS constants, ID3D12PipelineState* pso, const D3D12_VIEWPORT* vp, UINT vpCount,
                     const D3D12_RECT* sc, UINT scCount, UINT indices, UINT start, INT base, UINT firstInstance) {
    if (!focus.tyrePso || !focus.tyreConstants || !pso || vpCount != 2)
        return;
    D3D12_GPU_VIRTUAL_ADDRESS mine = 0;
    for (const auto& [from, to] : made)
        if (from == constants)
            mine = to;
    if (!mine) {
        mine = focus.tyreConstants->GetGPUVirtualAddress() + UINT64{focus.tyreSlot.fetch_add(1) % kTyreSlots} * kTyreSlotBytes;
        made.emplace_back(constants, mine);
        barrier(l, focus.tyreConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(focus.tyreRoot.Get());
        l->SetPipelineState(focus.tyrePso.Get());
        l->SetComputeRootConstantBufferView(0, constants);
        l->SetComputeRootUnorderedAccessView(1, mine);
        l->Dispatch(2, 1, 1);
        barrier(l, focus.tyreConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        l->SetPipelineState(pso);
    }
    l->RSSetViewports(1, &vp[1]);
    if (scCount == 2)
        l->RSSetScissorRects(1, &sc[1]);
    l->SetGraphicsRootConstantBufferView(kPassConstantsRoot, mine);
    l->DrawIndexedInstanced(indices, 1, start, base, firstInstance);
    l->SetGraphicsRootConstantBufferView(kPassConstantsRoot, constants);
    l->RSSetViewports(vpCount, vp);
    if (scCount == 2)
        l->RSSetScissorRects(scCount, sc);
}

// ---- Samples (replay_samples.cpp) and the focus culling record (replay_cull.cpp) ---------------------------------

// ---- The live periphery: ACE's own eyes drawn into a smaller region of its launch-size targets ---------------------
//
// ACE sizes its eye targets once, from the size the layer offers at launch. To change the periphery's resolution while
// running, its own eye passes are made to draw into the top left of those targets: eye 0 at x 0, eye 1 right beside it,
// both lw x lh instead of the scene's ew x eh. In ACE's own lists (not copies): the viewports and scissors of its scene
// draws and deferred shadows are scaled, and the constants that name the eye's size are read from our patched copies
// (the list's CBV redirected; the copy is made on the GPU by a list of ours put ahead of the game's batch):
//   renderPassData (root 1): screenOffset / screenSizeDiv (row 84), eyeHalfWidthPixels (112.z), the jitter kept in
//     pixels (row 85 and the projections). screenSize (row 38) stays the target's: the shaders divide SV_Position by it
//     to sample the screen-space maps, which keep ACE's size.
//   tileFrustums / tiledCulling (cb0): dimension, pixelOffset (row 13).
//   normalReconstructionViewSpace (root 2): eyeSplitX, rcpEyeWidth (row 17), and the inverse projections' y (the shader
//     takes NDC y from the buffer's height, which stays the target's).
//   deferredShadows (root 5): uvScaleAndOffset (row 32).
//   VelocityAndDepthPacking (cb0): rcpRes (row 12), resolution (15.yz), pixelOffset (17.xy), uvScaleAndOffset (18).
//   histogram (root 4): resolution (0.zw); histogramAverage (root 3): pixelCount (0.x): the exposure meters the region.
//   vrAntialiasing (MSAA's resolve, root 6, one dispatch per eye): viewportOffset / viewportSize (row 16). Its pixel is
//     the offset plus the thread id, its NDC (id + 0.5) / viewportSize through the eye's own matrices, its history pixel
//     the offset plus last frame's uv x the size: all follow row 16. TextureSize (0.xy) is not read.
// contrastAdaptiveSharpen runs over the region only: over the unused area (exactly 0) it wrote invalid values, which
// ACE's bloom then spread into the picture (07:28 captures: white outside, pink fringes inside).
// The rest of ACE's whole-target work, for its cost: its by-pixel compute passes (the two gaussian blurs, the veil's
// downsamples and upsamples, hiZGeneration, the normals) are dispatched over the region's share of their groups plus
// one (each output pixel still reads its own place in the source: only fewer are made); its whole-target fullscreen
// draws (OIT composite, tonemap, fxaa, the copies) keep their viewport, which maps each pixel to itself, and get a
// scissor around the region.
// ACE's whole-target passes (OIT composite, sharpening, bloom, tonemap, fxaa) run unchanged over the whole target: each
// pixel maps to itself, so the smaller picture stays where it is. NGX is told the smaller regions (focus_ngx). The
// compositor and the focus views' borrowed inputs (bloom, refraction mips) read the region in use.
// ACE's MSAA depth resolves (resolveDepthCompute, resolveDepthVelocityCompute) have no constants: by pixel over the
// target, shrunk like the other by-pixel passes.
// Not handled yet: the histogram's flags lookup (by uv over the target).
}  // namespace
namespace replay_detail {  // declared in replay_internal.h
EyeLive eyeLive;
EyeNow eye_now(uint64_t state) {
    EyeNow n;
    if (!state)
        return n;
    n.on = true;
    n.ew = static_cast<float>((state >> 32) & 0xffff);
    n.eh = static_cast<float>(state >> 48);
    n.fx = static_cast<float>(state & 0xffff) / n.ew;
    n.fy = static_cast<float>((state >> 16) & 0xffff) / n.eh;
    n.lw = static_cast<float>(state & 0xffff);
    n.lh = static_cast<float>((state >> 16) & 0xffff);
    return n;
}
EyeOut eye_out(const EyeNow& now, uint64_t out) {
    if (!out)
        return {now.ew, now.eh, now.lw, now.lh};
    return {static_cast<float>((out >> 32) & 0xffff), static_cast<float>(out >> 48), static_cast<float>(out & 0xffff),
            static_cast<float>((out >> 16) & 0xffff)};
}
}  // namespace replay_detail
namespace {
// The thread groups of a by-pixel pass that cover `live` pixels, when `groups` cover `full`: the group's size in pixels
// is the power of two that fits. Exact, not rounded up from the scale: a group past the region works on pixels nothing
// drew (07:34 captures: the sharpening's extra 16 pixels were exactly 0, came out invalid, and ACE's bloom spread them
// into a white band along the region's edges).
UINT eye_groups(UINT groups, float full, float live) {
    if (!groups || full <= 0)
        return groups;
    const float per = full / static_cast<float>(groups);
    UINT size = 1;
    while (static_cast<float>(size) < per - 0.5f)
        size <<= 1;
    return std::clamp<UINT>(static_cast<UINT>(std::ceil(live / static_cast<float>(size))), 1, groups);
}

float float_from_bits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Our copy of the constants at `va` for this list (one per address, mode and view size).
D3D12_GPU_VIRTUAL_ADDRESS eye_slot(Stream* s, D3D12_GPU_VIRTUAL_ADDRESS va, int mode, float w, float h) {
    for (const Stream::EyePatch& p : s->eyePatches)
        if (p.from == va && p.mode == mode && p.w == w && p.h == h)
            return p.to;
    const D3D12_GPU_VIRTUAL_ADDRESS to =
        eyeLive.constants->GetGPUVirtualAddress() + UINT64{eyeLive.nextSlot.fetch_add(1) % kEyeSlots} * kSlotBytes;
    s->eyePatches.push_back({va, to, mode, w, h});
    return to;
}

// Before a draw of the game's, in its own list: when the draw is one of ACE's eye scene draws (its scene root signature
// with the pass constants bound, eye-shaped viewports) or its deferred shadows, the viewports and scissors scaled and
// the constants ours; otherwise the game's own back.
void eye_apply(List* l, Stream* s) {
    if (!s->eyeDirty)
        return;
    s->eyeDirty = false;
    const EyeNow now = eye_now(s->eyeState);
    const bool scene = s->gfxRoot && s->gfxRoot == s->sceneRoot && s->gfxRoot == s->cbv1Root && s->gfxCbvs[kPassConstantsRoot];
    const bool shadows = s->psoKind == 6;
    // Any other draw into one whole eye, or both side by side, follows too (ACE's hidden-area mask is one: drawn at the
    // launch size it covered most of a small eye, 07:17 run): `other`. A whole-target pass (twice the eye's width) is not
    // eye-shaped and stays.
    const bool other = !scene && !shadows;
    bool want = now.on && s->vpN >= 1 && s->vpN <= 2;
    for (UINT i = 0; want && i < s->vpN; ++i) {
        const D3D12_VIEWPORT& v = s->vpAll[i];
        if (shadows)  // known by its pipeline: whatever size ACE draws it at (07:28 run: none matched half the eye)
            break;
        const bool width = other ? std::fabs(v.Width - now.ew) <= 1
                                 : std::fabs(v.Width - now.ew) <= 1 || std::fabs(v.Width - now.ew * 0.5f) <= 1;
        want = v.TopLeftY == 0 && std::fabs(v.Height - now.eh) <= 1 && width &&
               (v.TopLeftX == 0 || std::fabs(v.TopLeftX - v.Width) <= 1);
    }
    if (want && other) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 12) {
            wchar_t name[64] = {};
            UINT size = sizeof(name) - sizeof(wchar_t);
            if (!s->pso || FAILED(s->pso->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
                name[0] = 0;
            logd("replay eye: an eye-shaped draw outside the scene passes follows the eye's size: pipeline '%ls', %u viewport(s), "
                 "the first %.1f,%.1f %.1fx%.1f",
                 name, s->vpN, s->vpAll[0].TopLeftX, s->vpAll[0].TopLeftY, s->vpAll[0].Width, s->vpAll[0].Height);
        }
    }
    if (now.on && scene && !want && s->vpN >= 1) {  // a scene-root draw left at its own size: which viewports
        eyeLive.did[7].fetch_add(1, std::memory_order_relaxed);
        const D3D12_VIEWPORT& v = s->vpAll[0];
        // (shadow maps and cubemaps are square powers of two: not worth a line)
        const bool square = v.Width == v.Height && (static_cast<UINT>(v.Width) & (static_cast<UINT>(v.Width) - 1)) == 0;
        if (!square && eyeLive.shapesSaid.fetch_add(1) < 16) {  // which pass: its pipeline and render target by name
            wchar_t name[64] = {};
            UINT size = sizeof(name) - sizeof(wchar_t);
            if (!s->pso || FAILED(s->pso->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
                name[0] = 0;
            std::string target = "?";
            AcquireSRWLockShared(&focus.lock);
            if (const auto t = focus.views.find(s->lastRtv); s->lastRtv && t != focus.views.end())
                target = debug_name(t->second.resource.Get());
            ReleaseSRWLockShared(&focus.lock);
            logd("replay eye: a scene-root draw left at its own size: %u viewport(s), the first %.1f,%.1f %.1fx%.1f (the eye "
                 "%.0fx%.0f); pipeline '%ls', render target '%s'",
                 s->vpN, v.TopLeftX, v.TopLeftY, v.Width, v.Height, now.ew, now.eh, name, target.c_str());
        }
    }
    if (want) {
        const int kind = shadows ? 2 : s->vpN == 2 ? 0 : 1;
        eyeLive.did[kind].fetch_add(1, std::memory_order_relaxed);
        if (!other && eyeLive.said[kind].fetch_add(1) < 3)
            logd("replay eye: %s: %u viewport(s), the first %.1f,%.1f %.1fx%.1f -> %.1f,%.1f %.1fx%.1f; %u scissor(s), the "
                 "first %ld,%ld..%ld,%ld; constants %s",
                 kind == 2 ? "deferred shadows" : kind == 0 ? "a stereo scene pass" : "a one-eye scene pass", s->vpN,
                 s->vpAll[0].TopLeftX, s->vpAll[0].TopLeftY, s->vpAll[0].Width, s->vpAll[0].Height,
                 s->vpAll[0].TopLeftX * now.fx, s->vpAll[0].TopLeftY * now.fy, s->vpAll[0].Width * now.fx,
                 s->vpAll[0].Height * now.fy, s->scN, s->scN ? s->scAll[0].left : 0, s->scN ? s->scAll[0].top : 0,
                 s->scN ? s->scAll[0].right : 0, s->scN ? s->scAll[0].bottom : 0,
                 scene ? "root 1 ours" : s->gfxCbvs[5] && s->gfxRoot == s->cbv5Root ? "root 5 ours" : "NOT redirected");
        D3D12_VIEWPORT vp[4];
        for (UINT i = 0; i < s->vpN; ++i) {
            vp[i] = s->vpAll[i];
            vp[i].TopLeftX *= now.fx;
            vp[i].Width *= now.fx;
            vp[i].TopLeftY *= now.fy;
            vp[i].Height *= now.fy;
        }
        viewportsOriginal(l, s->vpN, vp);
        if (s->scN) {
            D3D12_RECT sc[4];
            for (UINT i = 0; i < s->scN; ++i) {
                sc[i].left = static_cast<LONG>(std::floor(s->scAll[i].left * now.fx));
                sc[i].top = static_cast<LONG>(std::floor(s->scAll[i].top * now.fy));
                sc[i].right = static_cast<LONG>(std::ceil(s->scAll[i].right * now.fx));
                sc[i].bottom = static_cast<LONG>(std::ceil(s->scAll[i].bottom * now.fy));
            }
            scissorsOriginal(l, s->scN, sc);
        }
        s->eyeScaled = true;
    } else if (s->eyeScaled) {
        if (s->vpN)
            viewportsOriginal(l, s->vpN, s->vpAll);
        if (s->scN)
            scissorsOriginal(l, s->scN, s->scAll);
        s->eyeScaled = false;
    }
    if (want && scene) {
        cbvOriginal(l, kPassConstantsRoot,
                    eye_slot(s, s->gfxCbvs[kPassConstantsRoot], 0, s->vpAll[0].Width * now.fx, s->vpAll[0].Height * now.fy));
        s->eyeCbv1 = true;
    } else if (s->eyeCbv1) {
        if (s->gfxRoot == s->cbv1Root)
            cbvOriginal(l, kPassConstantsRoot, s->gfxCbvs[kPassConstantsRoot]);
        s->eyeCbv1 = false;
    }
    if (want && shadows && s->gfxCbvs[5] && s->gfxRoot == s->cbv5Root) {
        cbvOriginal(l, 5, eye_slot(s, s->gfxCbvs[5], 3, 0, 0));
        s->eyeCbv5 = true;
    } else if (s->eyeCbv5) {
        if (s->gfxRoot == s->cbv5Root)
            cbvOriginal(l, 5, s->gfxCbvs[5]);
        s->eyeCbv5 = false;
    }
    // A draw over the whole two-eye target (ACE's fullscreen post draws): the viewport stays (it maps each pixel to
    // itself), a scissor keeps the work to the region in use.
    // In an upscaling DLSS mode the draws after it (tonemap, fxaa) cover the output size: clipped to the region there.
    const EyeOut o = eye_out(now, s->eyeOut);
    const bool at = now.on && !want && s->vpN == 1 && s->vpAll[0].TopLeftX == 0 && s->vpAll[0].TopLeftY == 0;
    const bool wholeScene = at && std::fabs(s->vpAll[0].Width - 2 * now.ew) <= 2 && std::fabs(s->vpAll[0].Height - now.eh) <= 1;
    const bool wholeOut = at && !wholeScene && s->eyeOut && std::fabs(s->vpAll[0].Width - 2 * o.w) <= 2 &&
                          std::fabs(s->vpAll[0].Height - o.h) <= 1;
    const bool whole = wholeScene || wholeOut;
    if (whole) {
        D3D12_RECT r{0, 0, static_cast<LONG>(2 * (wholeOut ? o.lw : now.lw)), static_cast<LONG>(wholeOut ? o.lh : now.lh)};
        if (s->scN) {  // within the game's own
            r.left = std::max(r.left, s->scAll[0].left);
            r.top = std::max(r.top, s->scAll[0].top);
            r.right = std::max(r.left, std::min(r.right, s->scAll[0].right));
            r.bottom = std::max(r.top, std::min(r.bottom, s->scAll[0].bottom));
        }
        scissorsOriginal(l, 1, &r);
        s->eyeClipped = true;
        eyeLive.did[12].fetch_add(1, std::memory_order_relaxed);
        if (eyeLive.said[12].fetch_add(1) < 10) {
            wchar_t name[64] = {};
            UINT size = sizeof(name) - sizeof(wchar_t);
            if (!s->pso || FAILED(s->pso->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
                name[0] = 0;
            logd("replay eye: a whole-target draw clipped to the region: pipeline '%ls', viewport %.0fx%.0f, scissor %ld,%ld..%ld,%ld "
                 "(the game's %ld,%ld..%ld,%ld)",
                 name, s->vpAll[0].Width, s->vpAll[0].Height, r.left, r.top, r.right, r.bottom, s->scN ? s->scAll[0].left : 0,
                 s->scN ? s->scAll[0].top : 0, s->scN ? s->scAll[0].right : 0, s->scN ? s->scAll[0].bottom : 0);
        }
    } else if (s->eyeClipped) {
        if (!want && s->scN)  // (a scaled draw has just set its own)
            scissorsOriginal(l, s->scN, s->scAll);
        s->eyeClipped = false;
    }
}

// A one-command list of ours on the queue now: a timestamp where `kind` starts (GPU time by step).
void gpu_marker(ID3D12CommandQueue* queue, uint8_t kind) {
    AcquireSRWLockExclusive(&focus.lock);
    Replay* r = eye_acquire();
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    const bool was = inReplay;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    gpu_stamp(l, kind);
    l->Close();
    inReplay = was;
    ID3D12CommandList* one[] = {l};
    executeOriginal(queue, 1, one);
    const uint64_t value = focus.fenceValue.fetch_add(1) + 1;
    queue->Signal(focus.fence.Get(), value);
    AcquireSRWLockExclusive(&focus.lock);
    r->fence = value;
    r->busy = false;
    ReleaseSRWLockExclusive(&focus.lock);
}

void execute_batch(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists, bool stamped) {
    std::vector<Replay*> eyeFirst;  // the live periphery: our constants copies, ahead of the lists that read them
    if (probeMode == 3 && queue == focus.queue.Get() && eyeLive.lists.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&focus.lock);
        for (UINT i = 0; i < n && !eyeLive.before.empty(); ++i) {
            const auto it = eyeLive.before.find(reinterpret_cast<List*>(lists[i]));
            if (it != eyeLive.before.end()) {
                eyeFirst.push_back(it->second);
                eyeLive.before.erase(it);
            }
        }
        ReleaseSRWLockExclusive(&focus.lock);
        if (!eyeFirst.empty()) {
            std::vector<ID3D12CommandList*> ours;
            for (Replay* r : eyeFirst)
                ours.push_back(r->list.Get());
            executeOriginal(queue, static_cast<UINT>(ours.size()), ours.data());
        }
    }
    if (stamped)
        gpu_marker(queue, gpuBatchKind);  // the game's batch (or our compositor's) starts here
    executeOriginal(queue, n, lists);
    if (!eyeFirst.empty()) {
        const uint64_t value = focus.fenceValue.fetch_add(1) + 1;
        queue->Signal(focus.fence.Get(), value);
        AcquireSRWLockExclusive(&focus.lock);
        for (Replay* r : eyeFirst) {
            r->fence = value;
            r->busy = false;
        }
        ReleaseSRWLockExclusive(&focus.lock);
        eyeLive.executed.fetch_add(eyeFirst.size(), std::memory_order_relaxed);
    }
    if (probeMode != 3 || queue != focus.queue.Get())
        return;
    std::vector<Replay*> mine;
    AcquireSRWLockExclusive(&focus.lock);
    for (UINT i = 0; i < n; ++i) {
        // ID3D12GraphicsCommandList derives singly from ID3D12CommandList: the same object address.
        const auto it = focus.pending.find(reinterpret_cast<List*>(lists[i]));
        if (it != focus.pending.end()) {
            mine.insert(mine.end(), it->second.begin(), it->second.end());
            focus.pending.erase(it);
        }
    }
    focus.pendingLists.store(focus.pending.size(), std::memory_order_relaxed);
    if (!mine.empty()) {
        ++focus.submitting;  // until signalled below (slots_writable)
        const uint64_t frame = focus.frame.load();
        if (focus.clearedFrame.load() != frame && focus.ready) {
            if (focus.copyPending.load() && !focus.saving.load())
                focus_copy_sample();  // the sample frame's replays are all done: copy before this frame's clear
            focus_prev_copy(queue);   // last frame's focus colour, before this frame's clear
            focus_mask_refresh();     // the focus mask's settings changed: the clear lists recorded again
            ID3D12CommandList* clear[] = {focus_clear_list(frame), focus.eyeClearList.Get()};
            // The eye references are drawn only at sample frames: cleared only then.
            executeOriginal(queue, focus.sampleFrame.load() == frame ? 2u : 1u, clear);
            focus.clearedFrame = frame;
        }
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (mine.empty())
        return;
    // Our DLSS evaluations recorded before the game's last NGX shutdown name freed features: those replays are dropped
    // (the game's lists above ran as they were). The submission holds NGX's lifetime, so no shutdown slips in between.
    const uint64_t life = focus_ngx::lifetime();
    std::vector<Replay*> dropped;
    const auto split = [&](uint64_t now) {
        std::erase_if(mine, [&](Replay* r) {
            const bool stale = r->ngxLife && r->ngxLife != now;
            if (stale)
                dropped.push_back(r);
            return stale;
        });
    };
    split(life);
    const auto submit = [&] {
        std::vector<ID3D12CommandList*> ours;
        for (Replay* r : mine)
            ours.push_back(r->list.Get());
        if (!ours.empty())
            executeOriginal(queue, static_cast<UINT>(ours.size()), ours.data());
    };
    const bool holdsNgx = std::any_of(mine.begin(), mine.end(), [](const Replay* r) { return r->ngxLife != 0; });
    if (!holdsNgx) {
        submit();
    } else if (!focus_ngx::in_lifetime(life, submit)) {
        split(UINT64_MAX);  // shut down just now: every stamped one goes, the rest run
        submit();
    }
    uint64_t value = 0;
    if (!mine.empty()) {
        value = focus.fenceValue.fetch_add(1) + 1;
        queue->Signal(focus.fence.Get(), value);
    }
    AcquireSRWLockExclusive(&focus.lock);
    --focus.submitting;
    if (value && focus.slotsHeld.load(std::memory_order_relaxed))  // taken before a rebuild: our slots wait for it
        focus.slotsFence = std::max(focus.slotsFence, value);
    for (Replay* r : mine) {
        r->fence = value;
        r->busy = false;
    }
    for (Replay* r : dropped)
        r->busy = false;  // never submitted: the fence of its last run stands
    ReleaseSRWLockExclusive(&focus.lock);
    if (!dropped.empty()) {
        static std::atomic<int> droppedLogged{0};
        if (droppedLogged.fetch_add(1) < 4)
            logf("replay focus: %zu replay(s) with our DLSS evaluations from before the game's NGX shutdown not submitted",
                 dropped.size());
    }
    focusReplays.fetch_add(mine.size(), std::memory_order_relaxed);
}

void STDMETHODCALLTYPE executeHook(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists) {
    if (!installed.load(std::memory_order_relaxed)) {  // the start not done (or failed): focus.fence may not exist
        executeOriginal(queue, n, lists);
        return;
    }
    const bool stamped = gpuSteps.on && probeMode == 3 && queue == focus.queue.Get() &&
                         gpuSteps.frame.load(std::memory_order_relaxed) == focus.frame.load(std::memory_order_relaxed);
    execute_batch(queue, n, lists, stamped);
    if (stamped)
        gpu_marker(queue, kStepGap);  // what follows until the next batch is not this batch's
}

void note_view(ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle, bool depth) {
    if (!resource || inReplay)
        return;
    AcquireSRWLockExclusive(&focus.lock);
    ViewInfo& v = focus.views[handle.ptr];
    if (v.resource && v.resource.Get() != resource && focus.twins.count(v.resource.Get()))
        focus.viewsMoved = true;
    v = {resource, format, depth};
    ReleaseSRWLockExclusive(&focus.lock);
}
void STDMETHODCALLTYPE dsvHook(ID3D12Device* d, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                               D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    dsvOriginal(d, resource, desc, handle);
    note_view(resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, handle, true);
}
void STDMETHODCALLTYPE rtvHook(ID3D12Device* d, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                               D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    rtvOriginal(d, resource, desc, handle);
    note_view(resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, handle, false);
}

// ---- Recording hooks -----------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE resetHook(List* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    const HRESULT hr = resetOriginal(list, allocator, pso);
    if (inReplay || FAILED(hr) || !installed.load(std::memory_order_relaxed) ||
        list->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE)
        return hr;
    // Closed and Reset again without an execution (or executed on another queue): what we recorded for it never runs.
    // (an eye copy list left in eyeLive.before goes here too, or at the list's next Close in eye_record)
    if (probeMode == 3 && focus.pendingLists.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&focus.lock);
        drop_pending(list);
        if (const auto it = eyeLive.before.find(list); it != eyeLive.before.end()) {
            it->second->busy = false;  // as eye_record frees one closed again without having run
            eyeLive.before.erase(it);
        }
        ReleaseSRWLockExclusive(&focus.lock);
    }
    Stream* s = nullptr;
    AcquireSRWLockExclusive(&streamsLock);
    auto& slot = streams[list];
    if (!slot)
        slot = std::make_unique<Stream>();
    s = slot.get();
    ReleaseSRWLockExclusive(&streamsLock);
    current = {list, s};
    s->begin(now_ticks());
    if (probeMode == 3) {
        s->eyeState = eyeLive.state.load(std::memory_order_relaxed);
        s->eyeOut = eyeLive.out.load(std::memory_order_relaxed);
    }
    if (probeMode == 3 && !focusActive.load(std::memory_order_relaxed)) {
        s->active = false;  // cost A/B off: the hooks pass straight through (no recording)
        return hr;
    }
    if (pso) {
        s->op(kPso);
        s->put(pso);
    }
    return hr;
}

// ---- the sky draw: its pipelines (the pixel shader declares cbSky), the constant-buffer views the game makes -------

bool shader_declares(const D3D12_SHADER_BYTECODE& bc, const char* name) {
    if (!bc.pShaderBytecode || bc.BytecodeLength < 64 || bc.BytecodeLength > (64u << 20))
        return false;
    __try {
        const char* p = static_cast<const char*>(bc.pShaderBytecode);
        if (std::memcmp(p, "DXBC", 4) != 0)
            return false;
        const size_t n = std::strlen(name);
        for (size_t i = 0; i + n < bc.BytecodeLength; ++i)
            if (p[i] == name[0] && std::memcmp(p + i, name, n) == 0 && p[i + n] == 0)
                return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}
// DXIL keeps names inside its bitcode (not as plain text): the sky's pixel shader is known by its container digest
// (DXBC header bytes 4..19; ACE's sky material, from a PIX capture), the plain name only as a fallback.
bool is_sky_shader(const D3D12_SHADER_BYTECODE& bc) {
    static const uint8_t kSkyDigest[16] = {0x24, 0xec, 0x19, 0x36, 0x9f, 0xad, 0x25, 0x4a,
                                           0xf6, 0x76, 0x8c, 0xb8, 0x33, 0xf9, 0xce, 0xe2};
    if (bc.pShaderBytecode && bc.BytecodeLength >= 20) {
        __try {
            const uint8_t* p = static_cast<const uint8_t*>(bc.pShaderBytecode);
            if (std::memcmp(p, "DXBC", 4) == 0 && std::memcmp(p + 4, kSkyDigest, 16) == 0)
                return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return shader_declares(bc, "cbSky");
}
void count_pipeline() {
    static std::atomic<uint64_t> seen{0};
    const uint64_t n = seen.fetch_add(1) + 1;
    if (n == 1 || n == 100 || n == 1000 || n == 5000)
        logd("replay focus: sky: %llu pipelines seen being made or loaded", static_cast<unsigned long long>(n));
}
void note_sky_pso(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& ps) {
    if (!pso)
        return;
    AcquireSRWLockExclusive(&focus.psoLock);  // a pipeline just made: what an older one at this address was is forgotten
    focus.skyPsos.erase(pso);
    focus.psoKinds.erase(pso);
    ReleaseSRWLockExclusive(&focus.psoLock);
    if (!is_sky_shader(ps))
        return;
    AcquireSRWLockExclusive(&focus.psoLock);
    const bool first = focus.skyPsos.empty();
    focus.skyPsos.insert(pso);
    ReleaseSRWLockExclusive(&focus.psoLock);
    if (first)
        logd("replay focus: sky: the sky's pipeline seen (its pixel shader declares cbSky)");
}
// ACE's tyre pipelines, by the vertex shader's signatures (plain text in the container; the cbuffer name
// bindlessVehicleTyres sits in bitcode): a skinned scene vertex shader (BONEWEIGHTS, MATERIALID, SV_Position) that takes
// no SV_InstanceID and writes no SV_ViewportArrayIndex, where ACE's stereo vertex shaders do both.
void note_tyre_pso(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& vs) {
    if (!pso)
        return;
    AcquireSRWLockExclusive(&focus.psoLock);  // a pipeline just made: an older one's record at this address goes
    focus.tyrePsos.erase(pso);
    focus.vsSeen.erase(pso);
    focus.psoKinds.erase(pso);
    ReleaseSRWLockExclusive(&focus.psoLock);
    const uint8_t bits = static_cast<uint8_t>((shader_declares(vs, "BONEWEIGHTS") ? 1 : 0) | (shader_declares(vs, "MATERIALID") ? 2 : 0) |
                                              (shader_declares(vs, "SV_Position") ? 4 : 0) |
                                              (shader_declares(vs, "SV_InstanceID") ? 8 : 0) |
                                              (shader_declares(vs, "SV_ViewportArrayIndex") ? 16 : 0));
    if (bits & 4) {  // a real vertex shader container
        Focus::VsInfo info{static_cast<uint32_t>(vs.BytecodeLength), {}, bits};
        __try {
            std::memcpy(info.digest, static_cast<const uint8_t*>(vs.pShaderBytecode) + 4, 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        AcquireSRWLockExclusive(&focus.psoLock);
        focus.vsSeen[pso] = info;
        ReleaseSRWLockExclusive(&focus.psoLock);
    }
    if (bits != 7)
        return;
    AcquireSRWLockExclusive(&focus.psoLock);
    const bool added = focus.tyrePsos.insert(pso).second;
    const size_t count = focus.tyrePsos.size();
    ReleaseSRWLockExclusive(&focus.psoLock);
    if (added && count <= 8)
        logd("replay focus: tyres: ACE's tyre pipeline %zu seen (%p; no single-pass-stereo variant: one viewport)", count,
             static_cast<void*>(pso));
}
// True for ACE's tyre pipelines, and for a pipeline whose creation we never saw (its vertex shader unknown: in a
// stereo list a one-instance indexed draw is a tyre in every capture so far).
bool is_tyre_pso(ID3D12PipelineState* pso) {
    if (!pso || !focus.settings.tyres)
        return false;
    AcquireSRWLockShared(&focus.psoLock);
    const bool tyre = focus.tyrePsos.count(pso) != 0 || focus.vsSeen.count(pso) == 0;
    ReleaseSRWLockShared(&focus.psoLock);
    return tyre;
}
bool is_sky_pso(ID3D12PipelineState* pso) {
    if (!pso || !focus.settings.sky)
        return false;
    AcquireSRWLockShared(&focus.psoLock);
    const bool sky = focus.skyPsos.count(pso) != 0;
    ReleaseSRWLockShared(&focus.psoLock);
    return sky;
}
HRESULT STDMETHODCALLTYPE gfxPsoHook(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    const HRESULT hr = gfxPsoOriginal(d, desc, riid, out);
    count_pipeline();
    if (SUCCEEDED(hr) && desc && out && *out && riid == __uuidof(ID3D12PipelineState)) {
        note_sky_pso(static_cast<ID3D12PipelineState*>(*out), desc->PS);
        note_tyre_pso(static_cast<ID3D12PipelineState*>(*out), desc->VS);
    }
    return hr;
}
void note_stream_pso(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* pso);
HRESULT STDMETHODCALLTYPE streamPsoHook(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    const HRESULT hr = streamPsoOriginal(d, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && riid == __uuidof(ID3D12PipelineState))
        note_stream_pso(desc, *out);
    return hr;
}
// ACE loads its pipelines from its PSO cache (a pipeline library): the same check there.
HRESULT STDMETHODCALLTYPE libGfxHook(ID3D12PipelineLibrary* lib, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                     REFIID riid, void** out) {
    const HRESULT hr = libGfxOriginal(lib, name, desc, riid, out);
    count_pipeline();
    if (SUCCEEDED(hr) && desc && out && *out && riid == __uuidof(ID3D12PipelineState)) {
        note_sky_pso(static_cast<ID3D12PipelineState*>(*out), desc->PS);
        note_tyre_pso(static_cast<ID3D12PipelineState*>(*out), desc->VS);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE libStreamHook(ID3D12PipelineLibrary1* lib, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                        REFIID riid, void** out) {
    const HRESULT hr = libStreamOriginal(lib, name, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && riid == __uuidof(ID3D12PipelineState))
        note_stream_pso(desc, *out);
    return hr;
}
// A pipeline-state stream's subobjects, walked as the runtime reads them: each starts pointer-aligned with its type (a
// 32-bit enum), its struct follows at that struct's own alignment. (Testing every 8 bytes for the VS / PS type, as
// before, took other subobjects' data for shaders: the guarded faults in is_sky_shader, 13-22 a session.) An unknown
// type ends the walk: its size isn't known, so nothing after it can be found.
bool stream_shaders(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, D3D12_SHADER_BYTECODE& vs, D3D12_SHADER_BYTECODE& ps) {
    vs = ps = {};
    if (!desc || !desc->pPipelineStateSubobjectStream)
        return false;
    const auto* p = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    const size_t total = desc->SizeInBytes;
    const auto up = [](size_t v, size_t a) { return (v + a - 1) / a * a; };
    size_t at = 0;
    while (at + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= total) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, p + at, sizeof(type));
        size_t size = 0, align = 0;
        switch (type) {
#define SUBOBJECT(t, T)     case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##t: size = sizeof(T); align = alignof(T); break;
            SUBOBJECT(ROOT_SIGNATURE, ID3D12RootSignature*)
            SUBOBJECT(VS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(PS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(DS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(HS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(GS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(CS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC)
            SUBOBJECT(BLEND, D3D12_BLEND_DESC)
            SUBOBJECT(SAMPLE_MASK, UINT)
            SUBOBJECT(RASTERIZER, D3D12_RASTERIZER_DESC)
            SUBOBJECT(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC)
            SUBOBJECT(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC)
            SUBOBJECT(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)
            SUBOBJECT(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE)
            SUBOBJECT(RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY)
            SUBOBJECT(DEPTH_STENCIL_FORMAT, DXGI_FORMAT)
            SUBOBJECT(SAMPLE_DESC, DXGI_SAMPLE_DESC)
            SUBOBJECT(NODE_MASK, UINT)
            SUBOBJECT(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE)
            SUBOBJECT(FLAGS, D3D12_PIPELINE_STATE_FLAGS)
            SUBOBJECT(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1)
            SUBOBJECT(VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC)
            SUBOBJECT(AS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(MS, D3D12_SHADER_BYTECODE)
            SUBOBJECT(DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2)
            SUBOBJECT(RASTERIZER1, D3D12_RASTERIZER_DESC1)
            SUBOBJECT(RASTERIZER2, D3D12_RASTERIZER_DESC2)
#undef SUBOBJECT
        default:
            return vs.pShaderBytecode || ps.pShaderBytecode;
        }
        const size_t inner = up(at + sizeof(type), align);
        if (inner + size > total)
            break;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS)
            std::memcpy(&vs, p + inner, sizeof(vs));
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS)
            std::memcpy(&ps, p + inner, sizeof(ps));
        at = up(inner + size, sizeof(void*));
    }
    return vs.pShaderBytecode || ps.pShaderBytecode;
}
void note_stream_pso(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* pso) {
    count_pipeline();
    D3D12_SHADER_BYTECODE vs, ps;
    if (!stream_shaders(desc, vs, ps))
        return;
    auto* const state = static_cast<ID3D12PipelineState*>(pso);
    if (vs.pShaderBytecode)
        note_tyre_pso(state, vs);
    if (ps.pShaderBytecode)
        note_sky_pso(state, ps);
}
void note_game_write(SIZE_T dest, UINT count);
void STDMETHODCALLTYPE cbvViewHook(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    cbvViewOriginal(d, desc, dest);
    if (inReplay)
        return;
    AcquireSRWLockExclusive(&focus.cbvLock);
    focus.cbvInfo[dest.ptr] = desc ? *desc : D3D12_CONSTANT_BUFFER_VIEW_DESC{};
    ReleaseSRWLockExclusive(&focus.cbvLock);
    note_game_write(dest.ptr, 1);
}
// Without the sky and tyres: CBVs checked against our slots only.
void STDMETHODCALLTYPE cbvCheckHook(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    cbvViewOriginal(d, desc, dest);
    note_game_write(dest.ptr, 1);
}

// R2c: the transparents replayed now (replay_transparents=2: only before samples 1, 3, ...).
bool transparents_now() {
    return focus.settings.transparents == 1 || (focus.settings.transparents == 2 && focus.samplesTaken.load() % 2 == 0);
}

HRESULT STDMETHODCALLTYPE closeHook(List* list) {
    const HRESULT hr = closeOriginal(list);
    Stream* s = stream_of(list);
    if (!s)
        return hr;
    s->active = false;
    listsSeen.fetch_add(1, std::memory_order_relaxed);
    hookCycles.fetch_add(s->hookCycles, std::memory_order_relaxed);
    if (!s->eyePatches.empty())  // (whatever Close returned: the list's bindings name our copies)
        eye_record(list, *s);
    if (SUCCEEDED(hr) && probeMode == 3 && focusActive.load(std::memory_order_relaxed) && !s->visDispatches.empty() &&
        cull.mode.load(std::memory_order_relaxed))
        focus_cull_record(list, *s);
    if (probeMode == 3) {
        if (!focusActive.load(std::memory_order_relaxed))
            return hr;  // cost A/B: the game's own frame only
        if (focus.slotsHeld.load(std::memory_order_relaxed)) {  // after a rebuild: nothing until our slots are free
            AcquireSRWLockExclusive(&focus.lock);
            const bool free = slots_writable();
            ReleaseSRWLockExclusive(&focus.lock);
            if (!free)
                return hr;
        }
        if (SUCCEEDED(hr) && focus.settings.tiles && !s->tileDispatches.empty())
            focus_tiles_record(list, *s);
        if (SUCCEEDED(hr) && focus.settings.tiles && focus.settings.fog && !s->tileDispatches.empty())
            focus_fog_record(list, *s);
        if (SUCCEEDED(hr) && focus.settings.tiles && focus.settings.shadows && s->shadowDraws.size() >= 2)
            focus_shadow_record(list, *s);
        if (SUCCEEDED(hr) && focus.settings.tiles && focus.settings.transparents &&
            std::any_of(s->tileDispatches.begin(), s->tileDispatches.end(),
                        [](const Stream::TileDispatch& d) { return d.kind == 11; }))
            focus_blur_record(list);
        if (SUCCEEDED(hr) && focus.settings.tiles &&
            std::any_of(s->tileDispatches.begin(), s->tileDispatches.end(),
                        [](const Stream::TileDispatch& d) { return d.kind == 14; }))
            focus_resolve_record(list, *s);
        // ACE's menu, pause and loading frames tonemap straight into the final image, without the fxaa draw
        if (SUCCEEDED(hr) && focus.settings.tiles && focus.settings.post && s->hasTonemap)
            focus_post_record(list, *s);
        if (SUCCEEDED(hr) && focus.settings.tiles && focus.settings.atmosphere &&
            std::any_of(s->tileDispatches.begin(), s->tileDispatches.end(),
                        [](const Stream::TileDispatch& d) { return d.kind == 8; }))
            focus_atmo_record(list, *s);
        static std::atomic<int> monoLogged{0};
        static std::atomic<int> monoEpoch{0};
        if (const int e = remakeEpoch.load(); monoEpoch.exchange(e) != e)
            monoLogged = 8;  // after a rebuild: the next 16 candidates are described again
        if (SUCCEEDED(hr) && transparents_now() && !s->stereo && s->monoSeen && s->draws > 0 && monoLogged < 24) {
            const bool taken = !s->oitDraw && !s->monoMixed && !s->passConstants.empty() && (s->sawRtv || s->sawDsv) &&
                               std::fabs(s->monoW - focus.sceneEyeW.load()) <= 1 && s->monoH == focus.sceneEyeH.load();
            ++monoLogged;
            logd("replay focus: per-eye candidate: viewport x %.0f %.0fx%.0f (scene eye %.0fx%.0f), draws %u, pass "
                 "constants %zu, targets %zu, rtv %d dsv %d, mixed %d, oit %d: %s",
                 s->monoX, s->monoW, s->monoH, focus.sceneEyeW.load(), focus.sceneEyeH.load(), s->draws,
                 s->passConstants.size(), s->targets.size(), s->sawRtv ? 1 : 0, s->sawDsv ? 1 : 0, s->monoMixed ? 1 : 0,
                 s->oitDraw ? 1 : 0, taken ? "replayed" : "skipped");
        }
        if (SUCCEEDED(hr) && s->monoSeen && s->draws > 0 && std::fabs(s->monoW - focus.sceneEyeW.load()) <= 1 &&
            s->monoH == focus.sceneEyeH.load()) {  // an eye-size per-eye segment: which path it takes
            if (s->stereo)
                focus.monoInStereo[s->monoX > 0 ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
            else if (s->oitDraw)
                focus.monoOit.fetch_add(1, std::memory_order_relaxed);
            else if (s->monoMixed)
                focus.monoMixed.fetch_add(1, std::memory_order_relaxed);
            else if (s->passConstants.empty())
                focus.monoNoConst.fetch_add(1, std::memory_order_relaxed);
            else if (!s->sawRtv && !s->sawDsv)
                focus.monoNoTarget.fetch_add(1, std::memory_order_relaxed);
        }
        if (SUCCEEDED(hr) && s->replayable(focus.settings.allTargets)) {
            gameTicks.fetch_add(static_cast<uint64_t>(now_ticks() - s->resetTicks), std::memory_order_relaxed);
            focus_record(list, *s);
        } else if (SUCCEEDED(hr) && transparents_now() && s->oitDraw) {
            focus_oit_record(list, *s);
        } else if (SUCCEEDED(hr) && transparents_now() && !s->stereo && s->monoSeen && !s->monoMixed &&
                   s->draws > 0 && !s->passConstants.empty() && (s->sawRtv || s->sawDsv) &&
                   std::fabs(s->monoW - focus.sceneEyeW.load()) <= 1 && s->monoH == focus.sceneEyeH.load()) {  // one eye of the scene
            gameTicks.fetch_add(static_cast<uint64_t>(now_ticks() - s->resetTicks), std::memory_order_relaxed);
            focus_record(list, *s, true);
        }
        return hr;
    }
    if (s->draws >= minDraws && s->maxViewportWidth >= minWidth) {
        gameTicks.fetch_add(static_cast<uint64_t>(now_ticks() - s->resetTicks), std::memory_order_relaxed);
        if (replayOn.load(std::memory_order_relaxed)) {
            inReplay = true;
            replay_unexecuted(*s);
            inReplay = false;
        }
    }
    return hr;
}

void STDMETHODCALLTYPE rootSigHook(List* l, ID3D12RootSignature* r) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kRootSig); s->put(r); s->gfxRoot = r;
        s->eyeCbv1 = s->eyeCbv5 = false;  // a new root signature: every root argument is unset
        s->eyeDirty = true;
    }
    rootSigOriginal(l, r);
}
void STDMETHODCALLTYPE psoHook(List* l, ID3D12PipelineState* r) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kPso);
        s->put(r);
        s->pso = r;
        if (focus.settings.tiles)
            s->psoKind = pso_kind(r);
        s->skyBound = is_sky_pso(r);
        s->tyreBound = is_tyre_pso(r);
        s->eyeDirty = true;
    }
    psoOriginal(l, r);
}
void STDMETHODCALLTYPE heapsHook(List* l, UINT n, ID3D12DescriptorHeap* const* heaps) {
    if (Stream* s = stream_of(l); s && n <= 2) {
        HookTimer t(s);
        s->op(kHeaps);
        s->put(n);
        for (UINT i = 0; i < n; ++i) s->put(heaps[i]);
        if (focus.settings.tiles)
            for (UINT i = 0; i < n; ++i)
                if (heaps[i] && heaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                    s->heap = heaps[i];
    }
    heapsOriginal(l, n, heaps);
}
void STDMETHODCALLTYPE tableHook(List* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kTable);
        s->put(i);
        s->put(h);
        if (i == 18) {  // ACE's scene root signature: space 45, the bindless uint buffers (tile lists)
            s->bindlessUint = h;
            s->sceneRoot = s->gfxRoot;
            s->eyeDirty = true;
        }
        if (i < 8)
            s->gfxTables[i] = h;
    }
    tableOriginal(l, i, h);
}
void STDMETHODCALLTYPE const1Hook(List* l, UINT i, UINT v, UINT o) {
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kConst1); s->put(i); s->put(v); s->put(o); }
    const1Original(l, i, v, o);
}
void STDMETHODCALLTYPE constNHook(List* l, UINT i, UINT n, const void* data, UINT o) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kConstN); s->put(i); s->put(n); s->put(o);
        s->put_bytes(data, size_t{n} * 4);
        if (i == 0 && o + n <= 8 && data)
            std::memcpy(s->gfxConsts + o, data, size_t{n} * 4);
    }
    constNOriginal(l, i, n, data, o);
}
void STDMETHODCALLTYPE cbvHook(List* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kCbv); s->put(i); s->put(va);
        if (i < 8)
            s->gfxCbvs[i] = va;
        if (i == kPassConstantsRoot) {  // the game's own address is bound again
            s->cbv1Root = s->gfxRoot;
            s->eyeCbv1 = false;
            s->eyeDirty = true;
        } else if (i == 5) {
            s->cbv5Root = s->gfxRoot;
            s->eyeCbv5 = false;
            s->eyeDirty = true;
        }
        if (i == kPassConstantsRoot && std::find(s->passConstants.begin(), s->passConstants.end(), va) == s->passConstants.end())
            s->passConstants.push_back(va);
        if (i == 0 && std::find(s->frameConstants.begin(), s->frameConstants.end(), va) == s->frameConstants.end())
            s->frameConstants.push_back(va);
    }
    cbvOriginal(l, i, va);
}
void STDMETHODCALLTYPE srvHook(List* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kSrv); s->put(i); s->put(va); }
    srvOriginal(l, i, va);
}
void STDMETHODCALLTYPE topologyHook(List* l, D3D12_PRIMITIVE_TOPOLOGY t0) {
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kTopology); s->put(t0); }
    topologyOriginal(l, t0);
}
void STDMETHODCALLTYPE ibHook(List* l, const D3D12_INDEX_BUFFER_VIEW* v) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kIb);
        s->put(static_cast<uint8_t>(v != nullptr));
        if (v) s->put(*v);
    }
    ibOriginal(l, v);
}
void STDMETHODCALLTYPE vbHook(List* l, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* views) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kVb); s->put(start); s->put(n); s->put(static_cast<uint8_t>(views != nullptr));
        if (views) s->put_bytes(views, sizeof(D3D12_VERTEX_BUFFER_VIEW) * n);
        if (views)  // focus culling: the size of ACE's visible-instances buffer, from a view at its address
            if (const uint64_t va = cull.gameInstancesVa.load(std::memory_order_relaxed))
                for (UINT i = 0; i < n; ++i)
                    if (views[i].BufferLocation == va && views[i].SizeInBytes > cull.gameInstancesBytes.load(std::memory_order_relaxed))
                        cull.gameInstancesBytes.store(views[i].SizeInBytes, std::memory_order_relaxed);
    }
    vbOriginal(l, start, n, views);
}
void STDMETHODCALLTYPE viewportsHook(List* l, UINT n, const D3D12_VIEWPORT* v) {
    if (Stream* s = stream_of(l); s && v) {
        HookTimer t(s);
        s->op(kViewports); s->put(n); s->put_bytes(v, sizeof(D3D12_VIEWPORT) * n);
        for (UINT i = 0; i < n; ++i) s->maxViewportWidth = std::max(s->maxViewportWidth, v[i].Width);
        s->vpCount = n;
        if (n == 2) std::memcpy(s->vp, v, sizeof(s->vp));
        s->vpN = n <= 4 ? n : 0;  // (more than ACE's eye passes set: left alone)
        std::memcpy(s->vpAll, v, sizeof(D3D12_VIEWPORT) * s->vpN);
        s->eyeDirty = true;  // (eyeScaled stays: scissors of ours still in effect go back at the next draw)
        if (n == 2 && v[0].TopLeftX == 0 && v[0].TopLeftY == 0 && v[1].TopLeftY == 0 && v[0].Width == v[1].Width &&
            v[0].Height == v[1].Height && v[1].TopLeftX == v[0].Width && v[0].Width >= 64) {
            s->stereo = true;
            s->eyeW = v[0].Width;
            s->eyeH = v[0].Height;
        } else if (n == 1 && v[0].TopLeftY == 0 && v[0].Width >= 64) {
            if (!s->monoSeen) {
                s->monoSeen = true;
                s->monoX = v[0].TopLeftX;
                s->monoW = v[0].Width;
                s->monoH = v[0].Height;
            } else if (s->monoX != v[0].TopLeftX || s->monoW != v[0].Width || s->monoH != v[0].Height) {
                s->monoMixed = true;
            }
        }
    }
    viewportsOriginal(l, n, v);
}
void STDMETHODCALLTYPE scissorsHook(List* l, UINT n, const D3D12_RECT* r) {
    if (Stream* s = stream_of(l); s && r) {
        HookTimer t(s);
        s->op(kScissors); s->put(n); s->put_bytes(r, sizeof(D3D12_RECT) * n);
        s->scCount = n;
        if (n == 2) std::memcpy(s->sc, r, sizeof(s->sc));
        s->scN = n <= 4 ? n : 0;
        std::memcpy(s->scAll, r, sizeof(D3D12_RECT) * s->scN);
        if (s->eyeScaled || s->eyeClipped)  // ours go back at the next draw
            s->eyeDirty = true;
    }
    scissorsOriginal(l, n, r);
}
void STDMETHODCALLTYPE stencilHook(List* l, UINT r) {
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kStencil); s->put(r); }
    stencilOriginal(l, r);
}
void STDMETHODCALLTYPE omHook(List* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                              const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        if (n > 0) {
            s->sawRtv = true;
            s->lastRtv = rtvs ? rtvs[0].ptr : 0;
        }
        if (dsv) s->sawDsv = true;
        if (probeMode == 3) {  // re-emitted onto our twins
            const SIZE_T step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            s->op(kOm);
            s->put(n);
            for (UINT i = 0; i < n; ++i) {
                const SIZE_T h = single ? rtvs[0].ptr + SIZE_T{i} * step : rtvs[i].ptr;
                s->put(h);
                s->target(h);
            }
            s->put(static_cast<uint8_t>(dsv != nullptr));
            if (dsv) {
                s->put(dsv->ptr);
                s->target(dsv->ptr);
            }
        }
    }
    omOriginal(l, n, rtvs, single, dsv);
}
void STDMETHODCALLTYPE computeRootSigHook(List* l, ID3D12RootSignature* r) {
    if (Stream* s = stream_of(l)) s->computeRoot = r;
    computeRootSigOriginal(l, r);
}
void STDMETHODCALLTYPE computeConstNHook(List* l, UINT i, UINT n, const void* data, UINT o) {
    if (Stream* s = stream_of(l); s && i == 0 && o + n <= 20 && data)
        std::memcpy(s->constants + o, data, size_t{n} * 4);
    if (Stream* s = stream_of(l); s && i == 0 && o + n <= 64 && data && cull.mode.load(std::memory_order_relaxed))
        std::memcpy(s->visConsts + o, data, size_t{n} * 4);
    computeConstNOriginal(l, i, n, data, o);
}
void STDMETHODCALLTYPE copyRegionHook(List* l, ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset,
                                      UINT64 bytes) {
    if (Stream* s = stream_of(l); s && s->bufferCopies.size() < 64)
        s->bufferCopies.push_back({dst, dstOffset, src, srcOffset, bytes});
    copyRegionOriginal(l, dst, dstOffset, src, srcOffset, bytes);
}
void STDMETHODCALLTYPE computeCbvHook(List* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (Stream* s = stream_of(l); s && i < 8) {
        if (i == 0) s->computeCbv = va;
        s->computeCbvs[i] = va;
        s->computeCbvRoots[i] = s->computeRoot;
    }
    computeCbvOriginal(l, i, va);
}
void STDMETHODCALLTYPE computeTableHook(List* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (Stream* s = stream_of(l); s && i < 10) s->tables[i] = h;
    computeTableOriginal(l, i, h);
}
void STDMETHODCALLTYPE dispatchHook(List* l, UINT x, UINT y, UINT z) {
    if (Stream* s = stream_of(l); s && s->dispatchPsos.size() < 64)
        s->dispatchPsos.push_back(s->pso);
    if (Stream* s = stream_of(l); s && s->psoKind) {
        Stream::TileDispatch d{};
        d.kind = s->psoKind;
        d.pso = s->pso;
        d.root = s->computeRoot;
        d.heap = s->heap;
        std::memcpy(d.constants, s->constants, sizeof(d.constants));
        d.cbv = s->computeCbv;
        std::memcpy(d.tables, s->tables, sizeof(d.tables));
        d.dims[0] = x;
        d.dims[1] = y;
        d.dims[2] = z;
        d.cbv2 = s->computeCbvs[2];
        d.cbv6 = s->computeCbvs[6];
        d.cbv1 = s->computeCbvs[1];
        s->tileDispatches.push_back(d);
    }
    if (Stream* s = stream_of(l); s && s->psoKind >= 25 && s->psoKind <= 27 && cull.mode.load(std::memory_order_relaxed) &&
        s->visDispatches.size() < 64) {
        Stream::VisDispatch d{};
        d.kind = s->psoKind;
        d.pso = s->pso;
        d.root = s->computeRoot;
        d.heap = s->heap;
        std::memcpy(d.consts, s->visConsts, sizeof(d.consts));
        std::memcpy(d.tables, s->tables, sizeof(d.tables));
        d.dims[0] = x;
        d.dims[1] = y;
        d.dims[2] = z;
        s->visDispatches.push_back(d);
    }
    // The live periphery: ACE's by-pixel post passes over the region's share of their groups, plus one (the next
    // pass's taps just past the region read something made this frame).
    if (Stream* s = stream_of(l); s && s->eyeState && x && y &&
                                         (s->psoKind == 11 || s->psoKind == 14 || s->psoKind == 15 ||
                                          (s->psoKind >= 20 && s->psoKind <= 24))) {
        // (at the render or the output size: the larger share covers both; the output region is in whole blocks)
        const EyeNow now = eye_now(s->eyeState);
        const EyeOut o = eye_out(now, s->eyeOut);
        const float shareX = std::max(now.fx, o.lw / o.w), shareY = std::max(now.fy, o.lh / o.h);
        const UINT sx = std::min<UINT>(x, static_cast<UINT>(std::ceil(x * shareX)) + 1);
        const UINT sy = std::min<UINT>(y, static_cast<UINT>(std::ceil(y * shareY)) + 1);
        eyeLive.did[11].fetch_add(1, std::memory_order_relaxed);
        if (eyeLive.said[11].fetch_add(1) < 24)
            logd("replay eye: %s dispatch %ux%u -> %ux%u",
                 s->psoKind == 11 ? "gaussianBlur3x3Float4" : s->psoKind == 14 ? "resolveDepthCompute"
                 : s->psoKind == 15 ? "resolveDepthVelocityCompute" : s->psoKind == 20 ? "gaussianBlur3x3Float3"
                 : s->psoKind == 21 ? "veilLumaDownsample" : s->psoKind == 22 ? "veilDownsample"
                 : s->psoKind == 23 ? "veilUpsample" : "hiZGeneration",
                 x, y, sx, sy);
        dispatchOriginal(l, sx, sy, z);
        return;
    }
    // These passes' constants name the eye's size; ours for the dispatch, the game's own back after.
    if (Stream* s = stream_of(l); s && s->eyeState && s->psoKind == 1) {
        // tileFrustums takes its sizes as root constants (root 0: the inverse projection, 1 / the eye's size at 16-17,
        // tileCount at 18-19): the reciprocal of the size in use for the dispatch, the game's own back after.
        const EyeNow now = eye_now(s->eyeState);
        float rcp[2], mine[2];
        std::memcpy(rcp, s->constants + 16, sizeof(rcp));
        mine[0] = rcp[0] / now.fx;
        mine[1] = rcp[1] / now.fy;
        computeConstNOriginal(l, 0, 2, mine, 16);
        eyeLive.did[3].fetch_add(1, std::memory_order_relaxed);
        if (eyeLive.said[3].fetch_add(1) < 2)
            logd("replay eye: tileFrustums %ux%u: constants 16-17 %.6g, %.6g (1 / %.1f x %.1f) -> %.6g, %.6g; tile count %.0fx%.0f",
                 x, y, rcp[0], rcp[1], rcp[0] != 0 ? 1 / rcp[0] : 0, rcp[1] != 0 ? 1 / rcp[1] : 0, mine[0], mine[1],
                 float_from_bits(s->constants[18]), float_from_bits(s->constants[19]));
        dispatchOriginal(l, std::max<UINT>(1, static_cast<UINT>(std::ceil(x * now.fx))),
                         std::max<UINT>(1, static_cast<UINT>(std::ceil(y * now.fy))), z);
        computeConstNOriginal(l, 0, 2, rcp, 16);
        return;
    }
    if (Stream* s = stream_of(l); s && s->eyeState && s->psoKind == 13) {
        // contrastAdaptiveSharpen works by pixel over the whole target: over the region in use only.
        // It runs on the DLSS output: in an upscaling mode, at the output size (the region NGX was given there).
        const EyeNow now = eye_now(s->eyeState);
        const EyeOut o = eye_out(now, s->eyeOut);
        eyeLive.did[8].fetch_add(1, std::memory_order_relaxed);
        const UINT sx = eye_groups(x, 2 * o.w, 2 * o.lw), sy = eye_groups(y, o.h, o.lh);  // both eyes side by side
        if (eyeLive.said[8].fetch_add(1) < 2)
            logd("replay eye: contrastAdaptiveSharpen dispatch %ux%u -> %ux%u", x, y, sx, sy);
        dispatchOriginal(l, sx, sy, z);
        return;
    }
    if (Stream* s = stream_of(l); s && s->eyeState && (s->psoKind == 2 || s->psoKind == 5 || s->psoKind == 12 ||
                                                       s->psoKind == 16 || s->psoKind == 18 || s->psoKind == 19)) {
        const UINT rootIndex =
            s->psoKind == 5 ? 2u : s->psoKind == 16 ? 6u : s->psoKind == 18 ? 4u : s->psoKind == 19 ? 3u : 0u;
        const bool bound = s->computeCbvs[rootIndex] && s->computeCbvRoots[rootIndex] == s->computeRoot;
        if (!bound) {
            static std::atomic<int> missed{0};
            if (missed.fetch_add(1) < 8)
                logf("replay eye: a dispatch of pipeline kind %d with no root %u constants bound under its root signature: "
                     "left alone",
                     s->psoKind, rootIndex);
        }
        if (const D3D12_GPU_VIRTUAL_ADDRESS va = bound ? s->computeCbvs[rootIndex] : 0) {
            const EyeNow now = eye_now(s->eyeState);
            const int kind = s->psoKind == 2    ? 4
                             : s->psoKind == 5  ? 5
                             : s->psoKind == 12 ? 6
                             : s->psoKind == 16 ? 13
                             : s->psoKind == 18 ? 9
                                                : 10;
            const int mode = s->psoKind == 2    ? 1
                             : s->psoKind == 5  ? 2
                             : s->psoKind == 12 ? 4
                             : s->psoKind == 16 ? 7
                             : s->psoKind == 18 ? 5
                                                : 6;
            eyeLive.did[kind].fetch_add(1, std::memory_order_relaxed);
            if (eyeLive.said[kind].fetch_add(1) < 2)
                logd("replay eye: %s dispatch %ux%ux%u: root %u constants ours",
                     kind == 4 ? "tiledCulling" : kind == 5 ? "normalReconstructionViewSpace" : kind == 6 ? "VelocityAndDepthPacking"
                     : kind == 13 ? "vrAntialiasing" : kind == 9 ? "histogram" : "histogramAverage",
                     x, y, z, rootIndex);
            computeCbvOriginal(l, rootIndex, eye_slot(s, va, mode, 0, 0));
            if (s->psoKind == 2 || s->psoKind == 12 || s->psoKind == 16) {  // per-eye passes: only the region in use
                // (packing must not write past its eye; the resolve stops at its viewportSize anyway)
                x = eye_groups(x, now.ew, now.lw);  // one eye each
                y = eye_groups(y, now.eh, now.lh);
            } else if (s->psoKind == 5 || s->psoKind == 18) {  // by pixel over the whole target: the region's share, plus one
                const EyeOut o = eye_out(now, s->eyeOut);  // (the histogram may run at the output size)
                x = std::min<UINT>(x, static_cast<UINT>(std::ceil(x * std::max(now.fx, o.lw / o.w))) + 1);
                y = std::min<UINT>(y, static_cast<UINT>(std::ceil(y * std::max(now.fy, o.lh / o.h))) + 1);
            }
            dispatchOriginal(l, x, y, z);
            computeCbvOriginal(l, rootIndex, va);
            return;
        }
    }
    dispatchOriginal(l, x, y, z);
}
// The game's tile-list SRVs by resource name; and every descriptor it writes, against our reserved slots.
int tile_view_kind(ID3D12Resource* r) {
    const std::string n = debug_name(r);
    if (n == "entityTileOpaque_l_eye") return 0;
    if (n == "entityTileOpaque_r_eye") return 1;
    if (n == "entityTileTransparent_l_eye") return 2;
    if (n == "entityTileTransparent_r_eye") return 3;
    if (n == "post_process_color") return 4;
    return -1;
}
void check_reserved(SIZE_T dest, UINT count) {  // under focus.lock
    if (!focus.gameHeap || focus.collision)
        return;
    const SIZE_T start = focus.gameHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (dest < start || dest >= start + SIZE_T{focus.heapSize} * focus.increment)
        return;
    const UINT first = static_cast<UINT>((dest - start) / focus.increment), last = first + count - 1;
    if (last < focus.baseIndex + kBindlessRange)
        focus.maxWritten = std::max<SIZE_T>(focus.maxWritten, last);
    if (first < focus.baseIndex)
        focus.maxUavWritten = std::max<SIZE_T>(focus.maxUavWritten, std::min<UINT>(last, focus.baseIndex - 1));
    if (focus.fogUavFirst && !focus.fogBroken && first < focus.fogUavFirst + kFogUavSlots && last >= focus.fogUavFirst) {
        focus.fogBroken = true;
        logf("replay focus: fog: the game wrote descriptors %u..%u, inside our UAVs (%u..%u): focus fog off", first, last,
             focus.fogUavFirst, focus.fogUavFirst + kFogUavSlots - 1);
    }
    if (first < focus.slotFirst + kTileSlots && last >= focus.slotFirst) {
        focus.collision = true;
        focus.tilesBroken = true;
        logf("replay focus: tiles: the game wrote descriptors %u..%u, inside ours (%u..%u): focus tiles off", first, last,
             focus.slotFirst, focus.slotFirst + kTileSlots - 1);
    }
}
// The other ways the game writes descriptors (CBVs, UAVs, multi-range copies), against our slots: a range compare,
// the lock only for a write into the game's shader-visible heap.
void note_game_write(SIZE_T dest, UINT count) {
    if (inReplay || !count)
        return;
    const SIZE_T lo = focus.heapCpuLo.load(std::memory_order_relaxed), hi = focus.heapCpuHi.load(std::memory_order_relaxed);
    if (dest < lo || dest >= hi)
        return;
    AcquireSRWLockExclusive(&focus.lock);
    check_reserved(dest, count);
    ReleaseSRWLockExclusive(&focus.lock);
}
void STDMETHODCALLTYPE uavViewHook(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* counter,
                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    uavViewOriginal(d, r, counter, desc, dest);
    note_game_write(dest.ptr, 1);
}
void STDMETHODCALLTYPE copyRangesHook(ID3D12Device* d, UINT destRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* dest,
                                      const UINT* destSizes, UINT srcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* src,
                                      const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    copyRangesOriginal(d, destRanges, dest, destSizes, srcRanges, src, srcSizes, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !dest)
        return;
    for (UINT i = 0; i < destRanges; ++i)  // (no sizes: one descriptor a range)
        note_game_write(dest[i].ptr, destSizes ? destSizes[i] : 1);
}
void STDMETHODCALLTYPE srvViewHook(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    srvViewOriginal(d, r, desc, dest);
    if (inReplay)
        return;
    const int k = r && desc && (desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER ||
                                desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
                      ? tile_view_kind(r)
                      : -1;
    AcquireSRWLockExclusive(&focus.lock);
    if (r && desc && (focus.settings.transparents || focus.settings.atmosphere || focus.settings.post) &&
        (desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D || desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMS ||
         desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D) &&
        (r->GetDesc().Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) &&
        focus.srvInfo.size() < 4096)
        focus.srvInfo[dest.ptr] = {r, *desc};
    if (r && desc && desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D && focus.settings.fog &&
        focus.views3d.size() < 256 &&
        std::none_of(focus.views3d.begin(), focus.views3d.end(), [&](const ComPtr<ID3D12Resource>& v) { return v.Get() == r; }))
        focus.views3d.emplace_back(r);
    if (k >= 0 && k < 5)
        focus.tileViews.emplace_back(dest.ptr, k);
    check_reserved(dest.ptr, 1);
    ReleaseSRWLockExclusive(&focus.lock);
}
void STDMETHODCALLTYPE copySimpleHook(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                      D3D12_DESCRIPTOR_HEAP_TYPE type) {
    copySimpleOriginal(d, n, dest, src, type);
    if (inReplay || type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !n)
        return;
    const SIZE_T step = d->GetDescriptorHandleIncrementSize(type);
    AcquireSRWLockExclusive(&focus.lock);
    const size_t known = focus.tileViews.size();
    for (size_t i = 0; i < known; ++i) {
        const auto [ptr, k] = focus.tileViews[i];
        if (ptr >= src.ptr && ptr < src.ptr + SIZE_T{n} * step)
            focus.tileViews.emplace_back(dest.ptr + (ptr - src.ptr), k);
    }
    if (focus.settings.sky && n <= 4096) {  // CBVs copied (the sky table's)
        AcquireSRWLockExclusive(&focus.cbvLock);
        for (UINT i = 0; i < n; ++i) {
            const auto it = focus.cbvInfo.find(src.ptr + SIZE_T{i} * step);
            if (it != focus.cbvInfo.end()) {
                const D3D12_CONSTANT_BUFFER_VIEW_DESC v = it->second;
                focus.cbvInfo[dest.ptr + SIZE_T{i} * step] = v;
            }
        }
        ReleaseSRWLockExclusive(&focus.cbvLock);
    }
    if ((focus.settings.transparents || focus.settings.atmosphere || focus.settings.post) && !focus.srvInfo.empty() &&
        n <= 4096)
        for (UINT i = 0; i < n; ++i) {
            const auto it = focus.srvInfo.find(src.ptr + SIZE_T{i} * step);
            if (it != focus.srvInfo.end() && focus.srvInfo.size() < 8192) {
                const auto info = it->second;
                focus.srvInfo[dest.ptr + SIZE_T{i} * step] = info;
            }
        }
    check_reserved(dest.ptr, n);
    ReleaseSRWLockExclusive(&focus.lock);
}
// A draw with the sky's pipeline bound: its root-4 table (cbSky at b4) for the focus replay. Any draw kind (the sky is
// an indexed draw).
void note_sky_draw(Stream* s) {
    if (s->skyBound && s->gfxTables[4].ptr &&
        std::find(s->skyTables.begin(), s->skyTables.end(), s->gfxTables[4].ptr) == s->skyTables.end())
        s->skyTables.push_back(s->gfxTables[4].ptr);
}
void STDMETHODCALLTYPE drawHook(List* l, UINT a, UINT b, UINT c, UINT d) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kDraw); s->put(a); s->put(b); s->put(c); s->put(d);
        ++s->draws;
        note_sky_draw(s);
        if (s->psoKind == 9 || s->psoKind == 10) {  // P1: tonemap, fxaa
            Stream::PostDraw& pd = s->psoKind == 9 ? s->tonemapDraw : s->fxaaDraw;
            pd = {s->pso, s->gfxRoot, {}, s->gfxCbvs[6], {}, s->lastRtv};
            std::memcpy(pd.tables, s->gfxTables, sizeof(pd.tables));
            std::memcpy(pd.consts, s->gfxConsts, sizeof(pd.consts));
            (s->psoKind == 9 ? s->hasTonemap : s->hasFxaa) = true;
        }
        if (s->psoKind == 7) {
            s->oitDraw = true;
            std::memcpy(s->oitTables, s->gfxTables, sizeof(s->oitTables));
        }
        if (s->psoKind == 6) {
            Stream::ShadowDraw sd{s->pso, s->gfxRoot, {}, s->gfxCbvs[5]};
            std::memcpy(sd.tables, s->gfxTables, sizeof(sd.tables));
            s->shadowDraws.push_back(sd);
        }
        eye_apply(l, s);
    }
    drawOriginal(l, a, b, c, d);
}
void STDMETHODCALLTYPE drawIndexedHook(List* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kDrawIndexed); s->put(a); s->put(b); s->put(c); s->put(d); s->put(e);
        ++s->draws;
        note_sky_draw(s);
        eye_apply(l, s);
        // ACE's tyres in a stereo list (one instance: the left eye only): again for the right eye, in ACE's own list.
        if (b == 1 && s->vpCount == 2 && focus.settings.tyres) {  // diagnostics: what a one-instance stereo-list draw met
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 12) {
                Focus::VsInfo info{};
                AcquireSRWLockShared(&focus.psoLock);
                const auto it = focus.vsSeen.find(s->pso);
                const bool seen = it != focus.vsSeen.end();
                if (seen)
                    info = it->second;
                const size_t known = focus.tyrePsos.size(), all = focus.vsSeen.size();
                ReleaseSRWLockShared(&focus.psoLock);
                logd("replay focus: tyres: one-instance draw in a stereo list: pipeline %p, %u indices, drawn again %d, creation "
                     "seen %d (vertex shader %u B, digest %02x%02x%02x%02x, signature bits %u), pass constants %d, compute root "
                     "%d, our copy pipeline %d, tyre pipelines known %zu of %zu seen",
                     static_cast<void*>(s->pso), a, s->tyreBound ? 1 : 0, seen ? 1 : 0, info.length, info.digest[0],
                     info.digest[1], info.digest[2], info.digest[3], info.bits, s->gfxCbvs[kPassConstantsRoot] ? 1 : 0,
                     s->computeRoot ? 1 : 0, focus.tyrePso ? 1 : 0, known, all);
            }
        }
        if (s->tyreBound && b == 1 && s->vpCount == 2 && s->gfxCbvs[kPassConstantsRoot] && focus.tyrePso) {
            drawIndexedOriginal(l, a, b, c, d, e);
            if (s->computeRoot) {  // our copy would replace compute bindings the game set in this list
                if (!focus.tyreComputeLogged.exchange(true))
                    log_line("replay focus: tyres: a stereo list with compute bindings set; its tyres stay left-eye only");
                return;
            }
            inReplay = true;
            if (s->eyeScaled && s->eyeCbv1) {  // the live periphery: into the smaller right eye, from our copy of the constants
                const EyeNow now = eye_now(s->eyeState);
                D3D12_VIEWPORT vp[2] = {s->vp[0], s->vp[1]};
                D3D12_RECT sc[2] = {s->sc[0], s->sc[1]};
                for (int k = 0; k < 2; ++k) {
                    vp[k].TopLeftX *= now.fx;
                    vp[k].Width *= now.fx;
                    vp[k].TopLeftY *= now.fy;
                    vp[k].Height *= now.fy;
                    sc[k].left = static_cast<LONG>(std::floor(sc[k].left * now.fx));
                    sc[k].top = static_cast<LONG>(std::floor(sc[k].top * now.fy));
                    sc[k].right = static_cast<LONG>(std::ceil(sc[k].right * now.fx));
                    sc[k].bottom = static_cast<LONG>(std::ceil(sc[k].bottom * now.fy));
                }
                tyre_right_draw(l, s->tyreConstants,
                                eye_slot(s, s->gfxCbvs[kPassConstantsRoot], 0, s->vpAll[0].Width * now.fx,
                                         s->vpAll[0].Height * now.fy),
                                s->pso, vp, s->vpCount, sc, s->scCount, a, c, d, e);
            } else {
                tyre_right_draw(l, s->tyreConstants, s->gfxCbvs[kPassConstantsRoot], s->pso, s->vp, s->vpCount, s->sc,
                                s->scCount, a, c, d, e);
            }
            inReplay = false;
            if (!focus.tyreGameLogged.exchange(true))
                logd("replay focus: tyres: ACE's tyre draws drawn again for the right eye (viewport %.0f, %.0f %.0fx%.0f)",
                     s->vp[1].TopLeftX, s->vp[1].TopLeftY, s->vp[1].Width, s->vp[1].Height);
            return;
        }
    }
    drawIndexedOriginal(l, a, b, c, d, e);
}
void STDMETHODCALLTYPE indirectHook(List* l, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args, UINT64 argsOffset,
                                    ID3D12Resource* count, UINT64 countOffset) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kIndirect); s->put(sig); s->put(max); s->put(args); s->put(argsOffset); s->put(count); s->put(countOffset);
        ++s->draws;
        note_sky_draw(s);
        eye_apply(l, s);
    }
    indirectOriginal(l, sig, max, args, argsOffset, count, countOffset);
}

void calibrate() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    const int64_t t0 = now_ticks();
    const uint64_t c0 = __rdtsc();
    Sleep(20);
    const int64_t t1 = now_ticks();
    const uint64_t c1 = __rdtsc();
    msPerCycle = static_cast<double>(t1 - t0) * msPerTick / static_cast<double>(c1 - c0);
}

bool skyHooksEarly = false;
// The vtables were patched by a start that then failed: patched again, the originals saved would be our own hooks.
bool startPatched = false;
}

void replay_probe_early_sky() {
    if (skyHooksEarly || !ace_known_build())
        return;
    // The loaded one, else only System32's (never a d3d12.dll planted beside the game or on the search path).
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12)
        d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create = d3d12 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    ComPtr<ID3D12Device1> d;
    if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d)))) {
        log_line("replay focus: sky: no early D3D12 device; the sky's pipeline hooks wait for the session");
        return;
    }
    void** deviceTable = *reinterpret_cast<void***>(d.Get());
    bool ok = vtable_hook(deviceTable, simqvfr_slot_CreateConstantBufferView(), &cbvViewHook, &cbvViewOriginal) &&
              vtable_hook(deviceTable, simqvfr_slot_CreateGraphicsPipelineState(), &gfxPsoHook, &gfxPsoOriginal) &&
              vtable_hook(deviceTable, simqvfr_slot_CreatePipelineState(), &streamPsoHook, &streamPsoOriginal);
    ComPtr<ID3D12PipelineLibrary1> lib;
    if (ok && SUCCEEDED(d->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib)))) {
        void** libTable = *reinterpret_cast<void***>(lib.Get());
        ok = vtable_hook(libTable, simqvfr_slot_LoadGraphicsPipeline(), &libGfxHook, &libGfxOriginal) &&
             vtable_hook(libTable, simqvfr_slot_LoadPipeline(), &libStreamHook, &libStreamOriginal);
    }
    skyHooksEarly = ok;
    logf("replay focus: sky: early hooks (pipeline creation and cache loads, constant-buffer views) %s",
         ok ? "installed at instance creation" : "FAILED");
}

bool replay_probe_start(ID3D12Device* d, int mode, ID3D12CommandQueue* queue, const ReplayFocusSettings& settings) {
    if (installed || !d)
        return installed;
    if (!ace_known_build()) {  // ACE's root signatures are bound with one build's layouts
        log_line("replay probe: not started (not a known ACE build, see \"ace build:\"): the game renders as it is");
        return false;
    }
    if (startPatched) {
        log_line("replay probe: an earlier start failed with its hooks in (passing through); not started again");
        return false;
    }
    if (mode == 3 && !queue) {
        log_line("replay probe: mode 3 needs the game's queue");
        return false;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List> list;
    if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        log_line("replay probe: could not create a probe command list");
        return false;
    }
    list->Close();
    device = d;
    probeMode = mode;
    replayOn = true;
    calibrate();
    if (mode == 3)
        focus.settings = settings;  // before any hook reads it
    focus.renderScale = settings.dlss ? std::clamp(settings.dlssScale, 0.4, 1.0) : 1.0;
    focus.outK = settings.boxFraction * settings.pixelScale;
    focus.outKy = (settings.boxFractionY > 0 ? settings.boxFractionY : settings.boxFraction) *
                  (settings.pixelScaleY > 0 ? settings.pixelScaleY : settings.pixelScale);
    focus.sizeK = settings.boxFraction * settings.pixelScale * focus.renderScale;
    focus.sizeKy = (settings.boxFractionY > 0 ? settings.boxFractionY : settings.boxFraction) *
                   (settings.pixelScaleY > 0 ? settings.pixelScaleY : settings.pixelScale) * focus.renderScale;
    void** vtable = *reinterpret_cast<void***>(list.Get());
    // The hooks pass straight through until `installed` is set at the end (focus.fence and the rest exist then); on a
    // failure they stay in, passing through.
    startPatched = true;
    bool ok =
        vtable_hook(vtable, simqvfr_slot_Reset(), &resetHook, &resetOriginal) &&
        vtable_hook(vtable, simqvfr_slot_Close(), &closeHook, &closeOriginal) &&
        vtable_hook(vtable, simqvfr_slot_SetGraphicsRootSignature(), &rootSigHook, &rootSigOriginal) &&
        vtable_hook(vtable, simqvfr_slot_SetPipelineState(), &psoHook, &psoOriginal) &&
        vtable_hook(vtable, simqvfr_slot_SetDescriptorHeaps(), &heapsHook, &heapsOriginal) &&
        vtable_hook(vtable, simqvfr_slot_SetGraphicsRootDescriptorTable(), &tableHook, &tableOriginal) &&
        vtable_hook(vtable, simqvfr_slot_SetGraphicsRoot32BitConstant(), &const1Hook, &const1Original) &&
        vtable_hook(vtable, simqvfr_slot_SetGraphicsRoot32BitConstants(), &constNHook, &constNOriginal) &&
        vtable_hook(vtable, simqvfr_slot_SetGraphicsRootConstantBufferView(), &cbvHook, &cbvOriginal) &&
        vtable_hook(vtable, simqvfr_slot_SetGraphicsRootShaderResourceView(), &srvHook, &srvOriginal) &&
        vtable_hook(vtable, simqvfr_slot_IASetPrimitiveTopology(), &topologyHook, &topologyOriginal) &&
        vtable_hook(vtable, simqvfr_slot_IASetIndexBuffer(), &ibHook, &ibOriginal) &&
        vtable_hook(vtable, simqvfr_slot_IASetVertexBuffers(), &vbHook, &vbOriginal) &&
        vtable_hook(vtable, simqvfr_slot_RSSetViewports(), &viewportsHook, &viewportsOriginal) &&
        vtable_hook(vtable, simqvfr_slot_RSSetScissorRects(), &scissorsHook, &scissorsOriginal) &&
        vtable_hook(vtable, simqvfr_slot_OMSetStencilRef(), &stencilHook, &stencilOriginal) &&
        vtable_hook(vtable, simqvfr_slot_OMSetRenderTargets(), &omHook, &omOriginal) &&
        vtable_hook(vtable, simqvfr_slot_DrawInstanced(), &drawHook, &drawOriginal) &&
        vtable_hook(vtable, simqvfr_slot_DrawIndexedInstanced(), &drawIndexedHook, &drawIndexedOriginal) &&
        vtable_hook(vtable, simqvfr_slot_ExecuteIndirect(), &indirectHook, &indirectOriginal);
    if (ok && mode == 3) {
        focus.queue = queue;
        void** deviceTable = *reinterpret_cast<void***>(d);
        ok = vtable_hook(deviceTable, simqvfr_slot_CreateDepthStencilView(), &dsvHook, &dsvOriginal) &&
             vtable_hook(deviceTable, simqvfr_slot_CreateRenderTargetView(), &rtvHook, &rtvOriginal) &&
             vtable_hook(*reinterpret_cast<void***>(queue), simqvfr_slot_ExecuteCommandLists(), &executeHook,
                         &executeOriginal) &&
             focus_compile();
        if (ok && settings.cull) {  // focus culling: the buffer copies of ACE's visibility list
            cull.mode = settings.cull;
            cull.use = settings.cull == 2;
            ok = vtable_hook(vtable, simqvfr_slot_CopyBufferRegion(), &copyRegionHook, &copyRegionOriginal);
            logf("replay cull: focus culling %s (replay_cull=%d: 1 computed only, 2 used by the focus replays, 3 used in "
                 "alternate 900-frame windows)",
                 ok ? "on" : "could not hook", settings.cull);
        }
        recordTiming.on = ok && settings.recordTiming;
        if (recordTiming.on)
            logf("replay record: the focus replays' recording timed by phase (replay_record_timing=1; \"replay record:\" "
                 "lines every 900 frames)");
        if (ok && settings.gpuSteps) {  // GPU time by step: the timestamps' heap, their readback, the resolve's list
            const D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStepQueries, 0};
            const D3D12_HEAP_PROPERTIES rh{D3D12_HEAP_TYPE_READBACK};
            const D3D12_RESOURCE_DESC rb{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kStepQueries} * 8, 1, 1, 1,
                                         DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            gpuSteps.on = SUCCEEDED(d->CreateQueryHeap(&qh, IID_PPV_ARGS(&gpuSteps.heap))) &&
                          SUCCEEDED(d->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                                               nullptr, IID_PPV_ARGS(&gpuSteps.readback))) &&
                          SUCCEEDED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpuSteps.allocator))) &&
                          SUCCEEDED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpuSteps.allocator.Get(), nullptr,
                                                         IID_PPV_ARGS(&gpuSteps.list))) &&
                          SUCCEEDED(gpuSteps.list->Close()) && SUCCEEDED(queue->GetTimestampFrequency(&gpuSteps.frequency)) &&
                          gpuSteps.frequency > 0;
            logf("replay gpu: GPU time by step %s (replay_gpu_steps=1: one frame in sixteen stamped)",
                 gpuSteps.on ? "on" : "could not start");
        }
        if (ok && settings.tiles)
            ok = vtable_hook(vtable, simqvfr_slot_SetComputeRootSignature(), &computeRootSigHook,
                             &computeRootSigOriginal) &&
                 vtable_hook(vtable, simqvfr_slot_SetComputeRoot32BitConstants(), &computeConstNHook,
                             &computeConstNOriginal) &&
                 vtable_hook(vtable, simqvfr_slot_SetComputeRootConstantBufferView(), &computeCbvHook,
                             &computeCbvOriginal) &&
                 vtable_hook(vtable, simqvfr_slot_SetComputeRootDescriptorTable(), &computeTableHook,
                             &computeTableOriginal) &&
                 vtable_hook(vtable, simqvfr_slot_Dispatch(), &dispatchHook, &dispatchOriginal) &&
                 vtable_hook(deviceTable, simqvfr_slot_CreateShaderResourceView(), &srvViewHook, &srvViewOriginal) &&
                 vtable_hook(deviceTable, simqvfr_slot_CopyDescriptorsSimple(), &copySimpleHook, &copySimpleOriginal) &&
                 vtable_hook(deviceTable, simqvfr_slot_CreateUnorderedAccessView(), &uavViewHook, &uavViewOriginal) &&
                 vtable_hook(deviceTable, slot_CopyDescriptors(), &copyRangesHook, &copyRangesOriginal);
        else if (ok) {
            srvViewOriginal = reinterpret_cast<SrvFn>(deviceTable[simqvfr_slot_CreateShaderResourceView()]);
            uavViewOriginal = reinterpret_cast<UavFn>(deviceTable[simqvfr_slot_CreateUnorderedAccessView()]);
        }
        if (ok && ((settings.sky && settings.tiles) || settings.tyres) && !skyHooksEarly)
            ok = vtable_hook(deviceTable, simqvfr_slot_CreateConstantBufferView(), &cbvViewHook, &cbvViewOriginal) &&
                 vtable_hook(deviceTable, simqvfr_slot_CreateGraphicsPipelineState(), &gfxPsoHook, &gfxPsoOriginal) &&
                 vtable_hook(deviceTable, simqvfr_slot_CreatePipelineState(), &streamPsoHook, &streamPsoOriginal);
        if (ok && ((settings.sky && settings.tiles) || settings.tyres) && !skyHooksEarly) {
            ComPtr<ID3D12Device1> d1;
            ComPtr<ID3D12PipelineLibrary1> lib;
            if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d1))) &&
                SUCCEEDED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib)))) {
                void** libTable = *reinterpret_cast<void***>(lib.Get());
                if (!vtable_hook(libTable, simqvfr_slot_LoadGraphicsPipeline(), &libGfxHook, &libGfxOriginal) ||
                    !vtable_hook(libTable, simqvfr_slot_LoadPipeline(), &libStreamHook, &libStreamOriginal))
                    log_line("replay focus: sky: could not hook the pipeline library");
            } else {
                log_line("replay focus: sky: no pipeline library on this device (pipelines from a cache not seen)");
            }
        }
        else if (ok && !skyHooksEarly && settings.tiles)  // our slots: the game's CBVs checked against them
            ok = vtable_hook(deviceTable, simqvfr_slot_CreateConstantBufferView(), &cbvCheckHook, &cbvViewOriginal);
        else if (ok && !skyHooksEarly)
            cbvViewOriginal = reinterpret_cast<CbvFn>(deviceTable[simqvfr_slot_CreateConstantBufferView()]);
        focus.sampleFrame = settings.saveCount ? settings.saveFirst : UINT64_MAX;
    }
    installed = ok;
    if (ok && mode == 3 && settings.dlss && !focus_ngx::install())
        log_line("replay focus: dlss: NGX (_nvngx.dll) not loaded yet; its exports are detoured when it is");
    if (mode == 3)
        logf("replay probe: %s (mode 3: %s replayed for both focus views into twins of their targets, executed after the "
             "game's batch; box %.0f%%, %.2f focus pixels per eye pixel; samples from frame %llu every %llu, %u times)",
             ok ? "installed" : "FAILED", settings.allTargets ? "every stereo scene list" : "the stereo depth pass",
             settings.boxFraction * 100, settings.pixelScale, static_cast<unsigned long long>(settings.saveFirst),
             static_cast<unsigned long long>(settings.saveEvery), settings.saveCount);
    else
        logf("replay probe: %s (list vtable %p, mode %d: %s; a list qualifies with >= %u draws and a viewport >= %.0f wide; "
             "re-emitted lists are never executed)",
             ok ? "installed" : "FAILED", vtable, mode, mode == 2 ? "re-emission in alternate 900-frame windows" : "re-emission on",
             minDraws, minWidth);
    return ok;
}

bool replay_probe_installed() { return installed; }

void replay_probe_set_size(double k, double kY, double renderScale) {
    if (probeMode != 3)
        return;
    if (k > 0) {  // 0: that one stays as it is
        focus.outKWanted.store(k);
        focus.outKyWanted.store(kY > 0 ? kY : k);
    }
    if (renderScale > 0)
        focus.renderScaleWanted.store(renderScale);
}

void replay_probe_set_eye_scale(double scale) {
    eyeLive.wanted.store(static_cast<float>(std::clamp(scale, 0.25, 1.0)), std::memory_order_relaxed);
}
void replay_probe_eye_allow(bool composed) { eyeLive.allowed.store(composed, std::memory_order_relaxed); }
void replay_probe_gpu_scope(bool compositor) { gpuBatchKind = compositor ? kStepCompose : kStepGame; }
bool replay_probe_eye_region(float& fx, float& fy) {
    // The eye image the compositor reads is the game's final one: at its DLSS output size when it upscales.
    const EyeNow now = eye_now(eyeLive.state.load(std::memory_order_relaxed));
    const EyeOut o = eye_out(now, now.on ? eyeLive.out.load(std::memory_order_relaxed) : 0);
    fx = now.on ? o.lw / o.w : 1.0f;
    fy = now.on ? o.lh / o.h : 1.0f;
    return now.on;
}
void replay_probe_set_focus_mask(bool on, const FocusArea& area, unsigned marginTexels) {
    replay_detail::focus_mask_set(on, area, marginTexels);  // (stored before the start too: the launch's values)
}

void replay_probe_set_box(double box, double boxY) {
    if (probeMode != 3)
        return;
    focus.boxYWanted.store(std::clamp(boxY > 0 ? boxY : box, 0.05, 0.6));
    focus.boxWanted.store(std::clamp(box, 0.05, 0.6));
}

void replay_probe_set_gaze(const float (&ndc)[2][2]) {
    if (probeMode != 3)
        return;
    AcquireSRWLockExclusive(&focus.lock);
    std::memcpy(focus.gazeWanted, ndc, sizeof(focus.gazeWanted));
    ReleaseSRWLockExclusive(&focus.lock);
}

bool replay_probe_focus_image(ID3D12Resource*& image, D3D12_RESOURCE_STATES& state, double& box, float (&offset)[2][2],
                              double* boxY) {
    if (boxY)
        *boxY = focus_box_y();
    if (probeMode != 3)
        return false;
    AcquireSRWLockShared(&focus.lock);
    std::memcpy(offset, focus.gazeOffset, sizeof(focus.gazeOffset));
    // Only a focus image of this frame (or the one before: ACE records on other threads): in ACE's menus, pause and
    // loading screens no scene is drawn and the last focus image would sit black or stale over the menu.
    const uint64_t at = focus.postFrame.load(std::memory_order_relaxed), now = focus.frame.load(std::memory_order_relaxed);
    const bool fresh = at != UINT64_MAX && now >= at && now - at <= 1;
    const bool ok = focus.postReady && focus.postFinal && !focus.postBroken && fresh;
    image = ok ? focus.postFinal.Get() : nullptr;
    ReleaseSRWLockShared(&focus.lock);
    state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    box = focus.settings.boxFraction;
    return ok;
}

void replay_probe_set_thresholds(uint32_t draws, float width) {
    minDraws = draws;
    minWidth = width;
}

ReplayProbeStats replay_probe_take_stats() {
    ReplayProbeStats s;
    s.frames = frames.exchange(0);
    s.listsSeen = listsSeen.exchange(0);
    s.listsReplayed = listsReplayed.exchange(0);
    s.callsReplayed = callsReplayed.exchange(0);
    s.drawsReplayed = drawsReplayed.exchange(0);
    s.replayMs = static_cast<double>(replayTicks.exchange(0)) * msPerTick;
    s.hookMs = static_cast<double>(hookCycles.exchange(0)) * msPerCycle;
    s.gameMs = static_cast<double>(gameTicks.exchange(0)) * msPerTick;
    s.focusReplays = focusReplays.exchange(0);
    s.tileReplays = focus.tileReplays.exchange(0);
    s.fogReplays = focus.fogReplays.exchange(0);
    s.shadowReplays = focus.shadowReplays.exchange(0);
    s.monoReplays = focus.monoReplays.exchange(0);
    s.oitReplays = focus.oitReplays.exchange(0);
    s.atmoReplays = focus.atmoReplays.exchange(0);
    s.postReplays = focus.postReplays.exchange(0);
    s.blurReplays = focus.blurReplays.exchange(0);
    s.skyPatches = skyPatchCount.exchange(0);
    s.dlssEvaluations = focus.dlssEvaluations.exchange(0);
    s.twinsMade = twinsMade.load();
    s.twinsRemade = twinsRemade.load();
    return s;
}

ReplayFocusCompare replay_probe_focus_compare() {
    AcquireSRWLockShared(&focus.compareLock);
    const ReplayFocusCompare c = focus.compare;
    ReleaseSRWLockShared(&focus.compareLock);
    return c;
}

void replay_probe_frame_end() {
    if (!installed)
        return;
    frames.fetch_add(1, std::memory_order_relaxed);
    if (probeMode == 3 && focus.settings.dlss && !focus_ngx::installed() && frames.load() % 90 == 0)
        focus_ngx::install();
    if (probeMode == 3) {
        const uint64_t ended = focus.frame.fetch_add(1);  // the frame that just ended
        focus_fixation_frame_end(ended);
        if (focus.settings.cullProbe && ended % 900 == 0) {  // the window that just ended, then the next one's phase
            static const char* const kPhase[] = {"everything drawn", "the indirect draws left out", "the direct draws left out"};
            const int was = cullProbePhase.load();
            const int now = static_cast<int>((ended / 900) % 3);
            logf("replay cull probe: the window that ended had %s in the focus scene replays (%.0f indirect and %.0f direct draws "
                 "a frame emitted, both eyes' lists); the next: %s",
                 kPhase[was], probeIndirect.exchange(0) / 900.0, probeDirect.exchange(0) / 900.0, kPhase[now]);
            cullProbePhase.store(now);
        }
        if (cull.mode.load() && ended % 900 == 0) {
            const bool was = cull.use.load();
            if (cull.mode.load() == 3)
                cull.use.store((ended / 900) % 2 == 1);
            logd("replay cull: over 900 frames: %.2f cullings a frame; focus scene lists a frame drawn from ours %.2f, whole "
                 "%.2f; this window the replays read %s, the next %s",
                 cull.culls.exchange(0) / 900.0, cull.listsCulled.exchange(0) / 900.0, cull.listsWhole.exchange(0) / 900.0,
                 was ? "our culled buffers" : "ACE's", cull.use.load() ? "ours" : "ACE's");
        }
        if (gpuSteps.on) {  // GPU time by step
            GpuSteps& g = gpuSteps;
            if (g.phase == 0 && ended % 16 == 0) {
                g.next = 0;
                g.frame = ended + 1;  // the frame starting now
                g.at = ended + 3;
                g.phase = 1;
            } else if (g.phase == 1 && ended >= g.at) {
                g.frame = UINT64_MAX;
                g.resolved = std::min<uint32_t>(g.next.load(), kStepQueries);
                if (g.resolved >= 2 && SUCCEEDED(g.allocator->Reset()) && SUCCEEDED(g.list->Reset(g.allocator.Get(), nullptr))) {
                    g.list->ResolveQueryData(g.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, g.resolved, g.readback.Get(), 0);
                    g.list->Close();
                    ID3D12CommandList* one[] = {g.list.Get()};
                    executeOriginal(focus.queue.Get(), 1, one);
                    g.at = ended + 3;
                    g.phase = 2;
                } else {
                    g.phase = 0;
                }
            } else if (g.phase == 2 && ended >= g.at) {
                g.phase = 0;
                const D3D12_RANGE range{0, size_t{g.resolved} * 8};
                void* data = nullptr;
                if (SUCCEEDED(g.readback->Map(0, &range, &data)) && data) {
                    std::vector<std::pair<uint64_t, uint8_t>> marks;
                    uint64_t last = 0;
                    for (uint32_t i = 0; i < g.resolved; ++i) {
                        uint64_t t;
                        std::memcpy(&t, static_cast<const uint8_t*>(data) + size_t{i} * 8, 8);
                        marks.emplace_back(t, g.kinds[i]);
                        last = std::max(last, t);
                    }
                    const D3D12_RANGE none{0, 0};
                    g.readback->Unmap(0, &none);
                    // (a stamp whose list never ran this time holds an older sample's value)
                    std::erase_if(marks, [&](const auto& m) { return m.first == 0 || last - m.first > g.frequency / 10; });
                    std::sort(marks.begin(), marks.end());
                    double sum[kStepKinds] = {}, total = 0;
                    for (size_t i = 0; i + 1 < marks.size(); ++i) {
                        const double ms = static_cast<double>(marks[i + 1].first - marks[i].first) * 1000.0 /
                                          static_cast<double>(g.frequency);
                        sum[marks[i].second] += ms;
                        total += ms;
                    }
                    if (marks.size() >= 4 && total < 100.0) {
                        for (int k = 0; k < kStepKinds; ++k)
                            g.sum[k] += sum[k];
                        ++g.samples;
                    } else {
                        ++g.dropped;
                    }
                }
            }
            if (ended % 900 == 0 && g.samples) {
                const double n = g.samples;
                double ours = 0;
                for (int k = 0; k < kStepGame; ++k)
                    ours += g.sum[k];
                logf("replay gpu: ms a frame over %u stamped frames (%u dropped): ACE's own lists %.2f; ours %.2f = scene replays "
                     "%.2f, one-eye replays %.2f, tiles %.2f, fog %.2f, shadow %.2f, OIT %.2f, atmosphere %.2f, refraction %.2f, "
                     "resolve %.2f, post: bloom share %.2f, DLSS %.2f, tonemap and after %.2f, live periphery copies %.2f, focus "
                     "culling %.2f; our compositor %.2f; between batches %.2f",
                     g.samples, g.dropped, g.sum[kStepGame] / n, ours / n, g.sum[kStepScene] / n, g.sum[kStepMono] / n,
                     g.sum[kStepTiles] / n, g.sum[kStepFog] / n, g.sum[kStepShadow] / n, g.sum[kStepOit] / n, g.sum[kStepAtmo] / n,
                     g.sum[kStepBlur] / n, g.sum[kStepResolve] / n, g.sum[kStepPostVeil] / n, g.sum[kStepPostDlss] / n,
                     g.sum[kStepPostTone] / n, g.sum[kStepEyeCopies] / n, g.sum[kStepCull] / n, g.sum[kStepCompose] / n,
                     g.sum[kStepGap] / n);
                std::fill(std::begin(g.sum), std::end(g.sum), 0.0);
                g.samples = g.dropped = 0;
            }
        }
        focus_mask_window_log(ended);  // replay_mask_ab: which side the window just logged was
        {  // The live periphery: the size the next frame's lists take. A new value is taken once it has stood for a
           // third of a second (stepping a menu item is one change: each makes NGX features of that size).
            static float held = 1.0f;
            static int settle = 0;
            static uint64_t reported = 0;
            const float asked = eyeLive.wanted.load(std::memory_order_relaxed);
            if (asked != held) {
                held = asked;
                settle = 30;
            }
            if (settle > 0)
                --settle;
            const float ew = focus.sceneEyeW.load(), eh = focus.sceneEyeH.load();
            const uint64_t before = eyeLive.state.load(std::memory_order_relaxed);
            uint64_t next = before;
            int why = 0;
            if (held >= 0.995f)
                next = 0;
            else if (!eyeLive.allowed.load(std::memory_order_relaxed))
                next = 0, why = 1;
            else if (!eye_ready())
                next = 0, why = 2;
            else if (ew < 256 || eh < 256 || ew > 60000 || eh > 60000)
                why = 3;  // the scene's eye size not known (just rebuilt): the size before stays
            else if (settle == 0) {
                const uint64_t e = static_cast<uint64_t>(std::lround(ew)), h = static_cast<uint64_t>(std::lround(eh));
                const uint64_t lw = std::max<uint64_t>(64, static_cast<uint64_t>(ew * held / 16) * 16);
                const uint64_t lh = std::max<uint64_t>(64, static_cast<uint64_t>(eh * held / 16) * 16);
                next = lw < e && lh < h ? (lw | lh << 16 | e << 32 | h << 48) : 0;
            }
            // The game's DLSS upscale, when its feature was made from this eye size: the region at the output size, in
            // whole 16-pixel blocks (ACE's sharpening works in 16 x 16 groups with no bounds test: a part block ran past
            // the region onto pixels nothing wrote this frame, and bloom spread them as a white band, 09:52 run). NGX is
            // given this region (focus_ngx), the compositor and the borrowed bloom read it: DLSS's few rows of extra
            // vertical scale are undone where the region is mapped onto the eye's view.
            uint64_t outNext = 0;
            if (next) {
                const focus_ngx::CreateInfo c = focus_ngx::create_info();
                const uint64_t e = (next >> 32) & 0xffff, h = next >> 48;
                if (c.seen && (c.outWidth != c.width || c.outHeight != c.height) && c.width + 1 >= e && c.width <= e + 1 &&
                    c.height + 1 >= h && c.height <= h + 1 && c.outWidth < 0x10000 && c.outHeight < 0x10000) {
                    const auto blocks = [](double v, unsigned full) {
                        return std::clamp<uint64_t>(static_cast<uint64_t>(std::llround(v / 16)) * 16, 16, full);
                    };
                    const uint64_t ow = blocks(c.outWidth * static_cast<double>(next & 0xffff) / e, c.outWidth);
                    const uint64_t oh = blocks(c.outHeight * static_cast<double>((next >> 16) & 0xffff) / h, c.outHeight);
                    outNext = ow | oh << 16 | uint64_t{c.outWidth} << 32 | uint64_t{c.outHeight} << 48;
                }
            }
            focus_ngx::set_eye_live_out(outNext);
            if (const uint64_t outBefore = eyeLive.out.exchange(outNext, std::memory_order_relaxed);
                outNext != outBefore && outNext)
                logf("replay eye: live periphery after the game's DLSS upscale: the region %llux%llu of its %llux%llu output",
                     static_cast<unsigned long long>(outNext & 0xffff), static_cast<unsigned long long>((outNext >> 16) & 0xffff),
                     static_cast<unsigned long long>((outNext >> 32) & 0xffff), static_cast<unsigned long long>(outNext >> 48));
            if (next != before) {
                eyeLive.state.store(next, std::memory_order_relaxed);
                focus_ngx::set_eye_live(next);
            }
            if (next != reported || why != eyeLive.whyNot) {
                reported = next;
                eyeLive.whyNot = why;
                static const char* const kWhy[] = {"", "no composed frame yet", "our constants pipeline failed",
                                                   "the scene's eye size isn't known yet"};
                if (next)
                    logf("replay eye: live periphery: ACE's eyes drawn at %llux%llu of its %llux%llu (%.0f%% of the launch "
                         "size each way)%s%s",
                         static_cast<unsigned long long>(next & 0xffff), static_cast<unsigned long long>((next >> 16) & 0xffff),
                         static_cast<unsigned long long>((next >> 32) & 0xffff), static_cast<unsigned long long>(next >> 48),
                         100.0 * static_cast<double>(next & 0xffff) / static_cast<double>((next >> 32) & 0xffff),
                         why ? "; held: " : "", kWhy[why]);
                else if (held < 0.995f)
                    logf("replay eye: live periphery asked at %.0f%%, not in effect: %s", held * 100.0, kWhy[why]);
                else
                    log_line("replay eye: live periphery off: ACE's eyes at their launch size");
            }
            if (next && ended % 900 == 0)
                logd("replay eye: over 900 frames: %.2f of ACE's lists a frame read our constants (%.2f copies a frame), %.2f "
                     "copy lists run a frame; state applied a frame: stereo passes %.2f, one-eye passes %.2f, deferred shadows "
                     "%.2f, tileFrustums %.2f, tiledCulling %.2f, normals %.2f, packing %.2f, sharpening %.2f, histogram %.2f, "
                     "histogram average %.2f, MSAA resolve %.2f, other post dispatches shrunk %.2f, whole-target draws clipped %.2f; "
                     "scene-root draws "
                     "left at their own size %.2f",
                     eyeLive.lists.exchange(1) / 900.0, eyeLive.patches.exchange(0) / 900.0, eyeLive.executed.exchange(0) / 900.0,
                     eyeLive.did[0].exchange(0) / 900.0, eyeLive.did[1].exchange(0) / 900.0, eyeLive.did[2].exchange(0) / 900.0,
                     eyeLive.did[3].exchange(0) / 900.0, eyeLive.did[4].exchange(0) / 900.0, eyeLive.did[5].exchange(0) / 900.0,
                     eyeLive.did[6].exchange(0) / 900.0, eyeLive.did[8].exchange(0) / 900.0, eyeLive.did[9].exchange(0) / 900.0,
                     eyeLive.did[10].exchange(0) / 900.0, eyeLive.did[13].exchange(0) / 900.0, eyeLive.did[11].exchange(0) / 900.0,
                     eyeLive.did[12].exchange(0) / 900.0,
                     eyeLive.did[7].exchange(0) / 900.0);
        }
        {  // the next frame's box centre: the gaze asked for, clamped so the box stays inside the eye's image
            AcquireSRWLockExclusive(&focus.lock);
            std::memcpy(focus.gazePrev, focus.gazeOffset, sizeof(focus.gazePrev));
            if (focus.psoFlush.load() > 0) {  // after a rebuild: the pipelines' kinds read from their names again
                // and anything the game released since (it lets go of its old textures over several frames): the
                // steps that found a resource by name start over with the current ones
                bool used = false;
                if (focus.psoFlush.load() % 10 == 0 && sweep_released(&used) > 0 && used)
                    focus_rebuild("the game released a texture a focus step still read");
                focus.psoFlush.fetch_sub(1);
                AcquireSRWLockExclusive(&focus.psoLock);
                focus.psoKinds.clear();
                ReleaseSRWLockExclusive(&focus.psoLock);
            }
            {  // our sizes: the focus density and the Focus views mode, live (everything sized is made again)
                // A change is taken once the value has stood for half a second: stepping a menu item through ten values
                // is one rebuild, not ten (52 rebuilds in one session before).
                static double heldK = 0, heldKy = 0, heldScale = 0;
                static int settle = 0;
                if (const double k = focus.outKWanted.exchange(0), ky = focus.outKyWanted.exchange(0); k > 0) {
                    heldK = k;
                    heldKy = ky > 0 ? ky : k;
                    settle = 45;
                }
                if (const double scale = focus.renderScaleWanted.exchange(0); scale > 0) {
                    heldScale = scale;
                    settle = 45;
                }
                const bool take = settle > 0 && --settle == 0;
                const double newK = take && heldK > 0 ? heldK : focus.outK, newKy = take && heldKy > 0 ? heldKy : focus.outKy;
                const double newScale =
                    take && heldScale > 0 && focus.settings.dlss ? std::clamp(heldScale, 0.4, 1.0) : focus.renderScale;
                if (take)
                    heldK = heldKy = heldScale = 0;
                if (std::fabs(newK - focus.outK) > 1e-4 || std::fabs(newKy - focus.outKy) > 1e-4 ||
                    std::fabs(newScale - focus.renderScale) > 1e-4) {
                    focus.outK = newK;
                    focus.outKy = newKy;
                    focus.renderScale = newScale;
                    focus.sizeK = newK * newScale;
                    focus.sizeKy = newKy * newScale;
                    focus.settings.pixelScale = focus.sizeK / focus.settings.boxFraction;
                    focus.settings.pixelScaleY =
                        focus.sizeKy / (focus.settings.boxFractionY > 0 ? focus.settings.boxFractionY : focus.settings.boxFraction);
                    logf("replay focus: sizes now: focus views %.1f%% x %.1f%% of the eye's size, rendered at %.0f%% of that",
                         newK * 100, newKy * 100, newScale * 100);
                    focus_rebuild("our focus sizes changed");
                }
            }
            const double wanted = focus.boxWanted.exchange(0), wantedY = focus.boxYWanted.exchange(0);
            if (wanted > 0 && (wanted != focus.settings.boxFraction || (wantedY > 0 && wantedY != focus.settings.boxFractionY))) {
                focus.settings.boxFraction = wanted;  // the same focus pixels over another share of the eye
                focus.settings.pixelScale = focus.sizeK / wanted;
                focus.settings.boxFractionY = wantedY > 0 ? wantedY : wanted;
                focus.settings.pixelScaleY = focus.sizeKy / focus.settings.boxFractionY;
                logf("replay focus: the focus box now %.1f%% x %.1f%% of the eye's view: %.2f x %.2f focus pixels per eye pixel "
                     "(the focus views keep their pixels)",
                     wanted * 100, focus.settings.boxFractionY * 100, focus.settings.pixelScale, focus.settings.pixelScaleY);
            }
            const float limits[2] = {1.0f - static_cast<float>(focus.settings.boxFraction), 1.0f - focus_box_y()};
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k < 2; ++k) {
                    const float c = focus.eyeCentre[e][k] + focus.gazeWanted[e][k];
                    focus.gazeOffset[e][k] =
                        focus.centresKnown ? std::clamp(c, -limits[k], limits[k]) - focus.eyeCentre[e][k] : 0.0f;
                }
            ReleaseSRWLockExclusive(&focus.lock);
        }
        const uint32_t mt0 = focus.monoTaken[0].exchange(0), mt1 = focus.monoTaken[1].exchange(0);
        const uint32_t md0 = focus.monoDone[0].exchange(0), md1 = focus.monoDone[1].exchange(0);
        const uint32_t mu = focus.monoUnfit.exchange(0), mb = focus.monoBroken.exchange(0);
        const uint32_t mo = focus.monoOit.exchange(0), mm = focus.monoMixed.exchange(0);
        const uint32_t ms0 = focus.monoInStereo[0].exchange(0), ms1 = focus.monoInStereo[1].exchange(0);
        const uint32_t mc = focus.monoNoConst.exchange(0), mn = focus.monoNoTarget.exchange(0);
        if (ended == focus.sampleFrame.load() && focus.settings.diagMode == 3) {
            const uint32_t ph = focus.samplesTaken.load() % 4;
            logd("replay focus: sky diagnostic: sample frame %llu: sky maps %s, cbSky %s",
                 static_cast<unsigned long long>(ended), (ph & 1) ? "ours" : "ACE's", (ph & 2) ? "ours" : "ACE's");
        }
        if (ended == focus.sampleFrame.load())
            logd("replay focus: frame %llu per-eye lists: taken L %u R %u, replayed L %u R %u, unfit %u, broken %u; "
                 "eye-size segments not taken: inside a stereo list L %u R %u, with the OIT composite %u, mixed viewports "
                 "%u, no pass constants %u, no targets %u",
                 static_cast<unsigned long long>(ended), mt0, mt1, md0, md1, mu, mb, ms0, ms1, mo, mm, mc, mn);
        if (ended == focus.sampleFrame.load()) {
            AcquireSRWLockShared(&focus.lock);
            bool readable = false;
            for (const auto& [game, t] : focus.twins)
                readable = readable || t.gameDesc.SampleDesc.Count == 1;
            readable = readable || (focus.resolvedColour && focus.postReady);  // MSAA: the post samples still compare
            const bool ready = focus.ready;
            ReleaseSRWLockShared(&focus.lock);
            if (ready && readable) {
                focus.copyFrame = ended;
                focus.copyPending = true;  // copied at the next frame's first replay, before its clear
                const uint32_t taken = focus.samplesTaken.fetch_add(1) + 1;
                if (taken < focus.settings.saveCount)
                    focus.sampleFrame = ended + focus.settings.saveEvery;
            } else {
                // Nothing replayed yet, or only multisampled twins (not copyable to a buffer): postponed, not counted.
                if (ready)
                    logd("replay focus: the game's targets are multisampled; the sample waits for MSAA off");
                focus.sampleFrame = ended + focus.settings.saveEvery;
            }
        }
    }
    if (++windowFrames < 900)
        return;
    windowFrames = 0;
    const bool wasOn = replayOn.load();
    const ReplayProbeStats s = replay_probe_take_stats();
    const double n = s.frames ? static_cast<double>(s.frames) : 1.0;
    static uint64_t gameDlssBefore = 0;
    const uint64_t gameDlssNow = focus_ngx::game_evaluations();
    const double gameDlss = static_cast<double>(gameDlssNow - gameDlssBefore);
    gameDlssBefore = gameDlssNow;
    if (probeMode == 3) {
        logf("replay focus: window %llu, per frame: stereo lists replayed %.2f (executed %.2f), draws %.0f; tile replays "
             "%.2f, fog replays %.2f, shadow replays %.2f, per-eye lists %.2f, OIT composites %.2f, atmosphere %.2f, "
             "post %.2f, refraction %.2f, sky draws %.2f, focus DLSS %.2f (game's %.2f), focus MSAA AA %.2f; "
             "recording the replays "
             "%.3f ms, recording hooks %.3f ms, the game's own recording of those lists %.3f ms (CPU)",
             static_cast<unsigned long long>(window), s.listsReplayed / n, s.focusReplays / n, s.drawsReplayed / n,
             s.tileReplays / n, s.fogReplays / n, s.shadowReplays / n, s.monoReplays / n, s.oitReplays / n,
             s.atmoReplays / n, s.postReplays / n, s.blurReplays / n, s.skyPatches / n, s.dlssEvaluations / n,
             gameDlss / n, static_cast<double>(focus.aaReplays.exchange(0)) / n, s.replayMs / n,
             s.hookMs / n, s.gameMs / n);
        if (recordTiming.on) {
            RecordTiming& t = recordTiming;
            const double lists = static_cast<double>(std::max<uint64_t>(1, t.lists.load()));
            const auto ms = [&](std::atomic<uint64_t>& v) { return static_cast<double>(v.exchange(0)) * msPerTick / n; };
            const double listsN = static_cast<double>(t.lists.exchange(0)) / n;
            const double ops = static_cast<double>(t.ops.exchange(0)) / lists, draws = static_cast<double>(t.draws.exchange(0)) / lists;
            const double lockWait = ms(t.lockWait), underLock = ms(t.underLock), setup = ms(t.setup), emit = ms(t.emit),
                         sky = ms(t.skyChecks), close = ms(t.close), after = ms(t.after);
            logd("replay record: ms a frame over %.2f focus scene replays (summed over threads): waiting for the focus lock "
                 "%.3f, under it %.3f, setup (constants, sky) %.3f, re-emission %.3f (its sky checks %.3f), Close %.3f, the "
                 "lock again after %.3f; per replay %.0f ops, %.0f draws; the slowest replay %.3f ms",
                 listsN, lockWait, underLock, setup, emit, sky, close, after, ops, draws,
                 static_cast<double>(t.maxList.exchange(0)) * msPerTick);
        }
        ++window;
        if (focus.settings.costAb) {  // 2 windows on, 2 off: the first window after each switch settles
            const bool on = (window / 2) % 2 == 0;
            if (on != focusActive.load()) {
                focusActive = on;
                logf("replay focus: cost A/B: focus replays now %s (window %llu on)", on ? "ON" : "OFF",
                     static_cast<unsigned long long>(window));
            }
        }
        return;
    }
    logf("replay probe: window %llu (re-emission %s), per frame: lists %.1f (qualifying, re-emitted %.1f), calls "
         "re-emitted %.0f, draws re-emitted %.0f; re-emission %.3f ms, recording hooks %.3f ms, the game's own recording "
         "of those lists (Reset to Close) %.3f ms (CPU, summed over threads)",
         static_cast<unsigned long long>(window), wasOn ? "on" : "off", s.listsSeen / n, s.listsReplayed / n,
         s.callsReplayed / n, s.drawsReplayed / n, s.replayMs / n, s.hookMs / n, s.gameMs / n);
    ++window;
    if (probeMode == 2) {
        replayOn = !wasOn;
        logf("replay probe: re-emission now %s (A/B window %llu)", replayOn.load() ? "on" : "off",
             static_cast<unsigned long long>(window));
    }
}

bool replay_probe_stream_shaders(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, D3D12_SHADER_BYTECODE& vs,
                                 D3D12_SHADER_BYTECODE& ps) {
    return stream_shaders(desc, vs, ps);
}

}  // namespace simqvfr


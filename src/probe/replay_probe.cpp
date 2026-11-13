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
#include <utility>
#include <vector>

#include <wrl/client.h>

#include "../log.h"
#include "../xr.h"
#include "d3d11_capture.h"  // write_png_bgra

extern "C" size_t simqvfr_slot_SetPipelineState(void);
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
extern "C" size_t simqvfr_slot_CreateShaderResourceView(void);
extern "C" size_t simqvfr_slot_CopyDescriptorsSimple(void);

namespace simqvfr {
namespace {

using List = ID3D12GraphicsCommandList;
using Microsoft::WRL::ComPtr;

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
using DsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE);
using RtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using DispatchFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
using SrvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE);
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
DsvFn dsvOriginal;
RtvFn rtvOriginal;
ExecuteFn executeOriginal;
RootSigFn computeRootSigOriginal;
ConstNFn computeConstNOriginal;
VaFn computeCbvOriginal;
TableFn computeTableOriginal;
DispatchFn dispatchOriginal;
SrvFn srvViewOriginal;
CopySimpleFn copySimpleOriginal;

enum Op : uint8_t {
    kRootSig = 1, kPso, kHeaps, kTable, kConst1, kConstN, kCbv, kSrv, kTopology, kIb, kVb, kViewports, kScissors,
    kStencil, kDraw, kDrawIndexed, kIndirect, kOm
};

constexpr UINT kPassConstantsRoot = 1;  // ACE: root parameter 1 of scene draws is the 2 KB pass constants

// One command list's recorded calls since its Reset.
struct Stream {
    std::vector<uint8_t> bytes;
    bool active = false;
    uint32_t draws = 0, calls = 0;
    float maxViewportWidth = 0;
    int64_t resetTicks = 0;
    uint64_t hookCycles = 0;
    // mode 3: a list with two eye-sized viewports side by side, and the targets it binds
    bool sawRtv = false, sawDsv = false, stereo = false;
    float eyeW = 0, eyeH = 0;
    std::vector<SIZE_T> targets;                             // distinct RTV/DSV CPU handles it binds
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> passConstants;  // distinct root-1 addresses, in order
    // R2b: the compute bindings in effect and ACE's tile passes (found by pipeline name) as they were dispatched.
    int psoKind = 0;  // 1 tileFrustums, 2 tiledCulling
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* computeRoot = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;  // the CBV/SRV/UAV heap set
    uint32_t constants[20] = {};
    D3D12_GPU_VIRTUAL_ADDRESS computeCbv = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE tables[8] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE bindlessUint{};  // root table 18 (space 45 in ACE's scene root signature)
    struct TileDispatch {
        int kind;
        ID3D12PipelineState* pso;
        ID3D12RootSignature* root;
        ID3D12DescriptorHeap* heap;
        uint32_t constants[20];
        D3D12_GPU_VIRTUAL_ADDRESS cbv;
        D3D12_GPU_DESCRIPTOR_HANDLE tables[8];
        UINT dims[3];
    };
    std::vector<TileDispatch> tileDispatches;

    void begin(int64_t now) {
        bytes.clear();
        active = true;
        draws = calls = 0;
        maxViewportWidth = 0;
        resetTicks = now;
        hookCycles = 0;
        sawRtv = sawDsv = stereo = false;
        eyeW = eyeH = 0;
        targets.clear();
        passConstants.clear();
        psoKind = 0;
        pso = nullptr;
        computeRoot = nullptr;
        heap = nullptr;
        computeCbv = 0;
        bindlessUint = {};
        tileDispatches.clear();
    }
    template <class T> void put(const T& v) {
        const size_t at = bytes.size();
        bytes.resize(at + sizeof(T));
        std::memcpy(bytes.data() + at, &v, sizeof(T));
    }
    void put_bytes(const void* p, size_t n) {
        const size_t at = bytes.size();
        bytes.resize(at + n);
        if (n)
            std::memcpy(bytes.data() + at, p, n);
    }
    void op(Op o) {
        bytes.push_back(o);
        ++calls;
    }
    void target(SIZE_T h) {
        if (std::find(targets.begin(), targets.end(), h) == targets.end())
            targets.push_back(h);
    }
    bool replayable(bool all) const {
        return stereo && draws > 0 && !passConstants.empty() && (sawDsv || sawRtv) && (all || (sawDsv && !sawRtv));
    }
};

ComPtr<ID3D12Device> device;
std::atomic<bool> installed{false};
int probeMode = 0;
std::atomic<bool> replayOn{true};
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
thread_local bool inReplay = false;
thread_local ComPtr<ID3D12CommandAllocator> replayAllocator;
thread_local ComPtr<List> replayList;
thread_local bool replayBroken = false;

std::atomic<uint64_t> listsSeen{0}, listsReplayed{0}, callsReplayed{0}, drawsReplayed{0}, focusReplays{0};
std::atomic<uint64_t> replayTicks{0}, hookCycles{0}, gameTicks{0}, frames{0};
std::atomic<uint64_t> twinsMade{0}, twinsRemade{0};
uint64_t windowFrames = 0, window = 0;

int64_t now_ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// The active stream of `list` on this thread, or null (not recording: no Reset seen, a bundle, inside a replay).
Stream* stream_of(List* list) {
    if (inReplay)
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

// What a replay changes: its viewports and scissors (all of them), root-1 addresses (by the stream's order) and the
// targets it binds (the game's CPU handles -> ours).
struct Substitution {
    const D3D12_VIEWPORT* viewports = nullptr;
    const D3D12_RECT* scissors = nullptr;
    UINT count = 0;
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* from = nullptr;
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* to = nullptr;
    const std::vector<std::pair<SIZE_T, SIZE_T>>* targets = nullptr;
};

SIZE_T map_target(const Substitution* sub, SIZE_T h) {
    if (sub && sub->targets)
        for (const auto& [from, to] : *sub->targets)
            if (from == h)
                return to;
    return 0;
}

// Re-emits a stream into `l` (open). Returns the draws emitted, or -1 on a broken stream or a target without a twin.
int64_t emit(List* l, const Stream& s, const Substitution* sub) {
    const uint8_t* p = s.bytes.data();
    const uint8_t* const end = p + s.bytes.size();
    const auto get = [&p](auto& v) {
        std::memcpy(&v, p, sizeof(v));
        p += sizeof(v);
    };
    int64_t draws = 0;
    while (p < end) {
        const Op op = static_cast<Op>(*p++);
        switch (op) {
        case kRootSig: { ID3D12RootSignature* r; get(r); l->SetGraphicsRootSignature(r); break; }
        case kPso: { ID3D12PipelineState* r; get(r); l->SetPipelineState(r); break; }
        case kHeaps: {
            UINT n; get(n);
            ID3D12DescriptorHeap* heaps[2] = {};
            for (UINT i = 0; i < n; ++i) get(heaps[i]);
            l->SetDescriptorHeaps(n, heaps);
            break;
        }
        case kTable: { UINT i; D3D12_GPU_DESCRIPTOR_HANDLE h; get(i); get(h); l->SetGraphicsRootDescriptorTable(i, h); break; }
        case kConst1: { UINT i, v, o; get(i); get(v); get(o); l->SetGraphicsRoot32BitConstant(i, v, o); break; }
        case kConstN: {
            UINT i, n, o; get(i); get(n); get(o);
            l->SetGraphicsRoot32BitConstants(i, n, p, o);
            p += size_t{n} * 4;
            break;
        }
        case kCbv: {
            UINT i; D3D12_GPU_VIRTUAL_ADDRESS va; get(i); get(va);
            if (sub && sub->from && i == kPassConstantsRoot)
                for (size_t k = 0; k < sub->from->size(); ++k)
                    if ((*sub->from)[k] == va) {
                        va = (*sub->to)[k];
                        break;
                    }
            l->SetGraphicsRootConstantBufferView(i, va);
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
            l->IASetVertexBuffers(first, n, has ? views : nullptr);
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
        case kDraw: { UINT a, b, c, d; get(a); get(b); get(c); get(d); l->DrawInstanced(a, b, c, d); ++draws; break; }
        case kDrawIndexed: {
            UINT a, b, c, e; INT d; get(a); get(b); get(c); get(d); get(e);
            l->DrawIndexedInstanced(a, b, c, d, e);
            ++draws;
            break;
        }
        case kIndirect: {
            ID3D12CommandSignature* sig; UINT max; ID3D12Resource* args; UINT64 argsOffset; ID3D12Resource* count;
            UINT64 countOffset;
            get(sig); get(max); get(args); get(argsOffset); get(count); get(countOffset);
            l->ExecuteIndirect(sig, max, args, argsOffset, count, countOffset);
            ++draws;
            break;
        }
        default:
            return -1;
        }
    }
    return draws;
}

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

// The pass constants copied into our slot with the off-axis focus remap (ACE's layout, recon-ace.md "R1"): left
// projections at rows 13/17/21/25, right at 95/103/107, each 4 rows x, y, z, w with w = (0,0,-1,0); row 38.xy target
// size, row 84 the viewport rect, row 112.z eyeHalfWidthPixels. Each eye's centre comes from its unjittered projection
// (rows 21/22 and 103/104: ndc centre = -row.z). Rows 128-131 keep those unjittered x/y rows.
constexpr char kPatchShader[] = R"(
cbuffer Source : register(b0) { float4 src[128]; };
cbuffer Params : register(b1) { float2 halfExtent; float2 focusSize; uint4 from0; uint4 from1; uint4 to0; uint4 to1; uint fogSrv; };
RWByteAddressBuffer dst : register(u0);
bool is_in(uint r, uint a, uint b, uint c, uint d) { return r == a || r == b || r == c || r == d; }
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 132) return;
    if (i >= 128) {
        const uint from[4] = {17, 18, 103, 104};
        dst.Store4(i * 16, asuint(src[from[i - 128]]));
        return;
    }
    float4 v = src[i];
    const bool left = i < 64;
    const float ox = left ? src[17].z : src[103].z;  // projectionUnjittered: each eye's own centre
    const float oy = left ? src[18].z : src[104].z;
    const float ax = 1.0 / halfExtent.x, ay = 1.0 / halfExtent.y;
    if (is_in(i, 1, 13, 17, 21) || is_in(i, 25, 95, 103, 107))  // viewProjection and the projections
        v = ax * v + (ax * ox) * src[i + 3];          // x' = a x + b w, b = -a * centre = a * ox
    else if (is_in(i, 2, 14, 18, 22) || is_in(i, 26, 96, 104, 108))
        v = ay * v + (ay * oy) * src[i + 2];          // y' = c y + d w
    if (i == 38) v.xy = float2(focusSize.x * 2, focusSize.y);
    if (i == 84) v = float4(0, 0, focusSize.x, focusSize.y);
    if (i >= 29 && i <= 32)  // inverseViewProjection: inverse(R VP) = inverse(VP) inverse(R)
        v = float4(v.x * halfExtent.x, v.y * halfExtent.y, v.z, v.w - v.x * ox - v.y * oy);
    if (i == 112) v.z = focusSize.x;  // eyeHalfWidthPixels (a float, as ACE writes it)
    // Bindless indices the game's -> ours (R2b tile lists, R2c previous-frame colour): entityTilesIndex (row 0.x),
    // entityTilesIndexRight (112.y), the screen-space map indices (rows 76, 77). Unused pairs never match.
    const uint from[8] = {from0.x, from0.y, from0.z, from0.w, from1.x, from1.y, from1.z, from1.w};
    const uint to[8] = {to0.x, to0.y, to0.z, to0.w, to1.x, to1.y, to1.z, to1.w};
    [unroll] for (uint k = 0; k < 8; ++k) {
        if (i == 0 && asuint(v.x) == from[k]) v.x = asfloat(to[k]);
        if (i == 112 && asuint(v.y) == from[k]) v.y = asfloat(to[k]);
        if (i == 76 || i == 77) {
            if (asuint(v.x) == from[k]) v.x = asfloat(to[k]);
            if (asuint(v.y) == from[k]) v.y = asfloat(to[k]);
            if (asuint(v.z) == from[k]) v.z = asfloat(to[k]);
            if (asuint(v.w) == from[k]) v.w = asfloat(to[k]);
        }
    }
    // volumetricFogMapIndex (76.w): ACE's stereo lighting reads voxelGrid0 for both eyes; ours is eye 0's focus grid.
    if (i == 76 && fogSrv != 0) v.w = asfloat(fogSrv);
    dst.Store4(i * 16, asuint(v));
}
)";

constexpr UINT kSlotBytes = 4096;  // 2 KB constants + 4 kept rows, CBV-aligned
constexpr UINT kSlots = 512;
constexpr UINT kRtvSlots = 32, kDsvSlots = 16;

struct Replay {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List> list;
    uint64_t fence = 0;  // done when the fence reaches it
    bool busy = false;
};

// A view the game created: its resource and view format (RTV or DSV, by CPU handle).
struct ViewInfo {
    ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    bool depth = false;
};

// Our twins of one game target: focus size (both focus views side by side) and eye size (the reference, samples).
// Sized from the game target itself (two eyes side by side), so passes at other resolutions (ACE's half-width `flags`
// pass) have twins of their own size; per eye: eyeW x eyeH, focusW x focusH.
struct Twin {
    ComPtr<ID3D12Resource> focus, eye;
    D3D12_CPU_DESCRIPTOR_HANDLE focusView{}, eyeView{};
    D3D12_RESOURCE_DESC gameDesc{};
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    bool depth = false;
    std::string name;
    UINT eyeW = 0, eyeH = 0, focusW = 0, focusH = 0;
    int focusSlot = -1, eyeSlot = -1;  // view slots in our heaps
};

struct Focus {
    ReplayFocusSettings settings;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::atomic<uint64_t> fenceValue{0};
    ComPtr<ID3D12RootSignature> patchRoot;
    ComPtr<ID3D12PipelineState> patchPso;
    ComPtr<ID3D12Resource> constants;  // kSlots x kSlotBytes, UNORDERED_ACCESS
    std::atomic<uint32_t> nextSlot{0};
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    std::vector<int> freeRtv, freeDsv;  // view slots (OMSetRenderTargets copies a view at record time: reusable at once)
    bool ready = false;
    // Twins and lists of an earlier game setup, kept alive until the GPU has passed the stamped fence value.
    std::vector<std::pair<uint64_t, ComPtr<IUnknown>>> retired;
    ComPtr<ID3D12CommandAllocator> clearAllocator, eyeClearAllocator;
    ComPtr<List> clearList, eyeClearList;  // the focus twins every frame; the eye references at sample frames
    SRWLOCK lock = SRWLOCK_INIT;  // pool, pending, twins, views, clear list
    std::vector<std::unique_ptr<Replay>> pool;
    std::unordered_map<List*, std::vector<Replay*>> pending;  // the game's list -> our replays of it, in order
    std::unordered_map<SIZE_T, ViewInfo> views;  // the game's RTV/DSV CPU handles
    std::unordered_map<ID3D12Resource*, Twin> twins;
    std::vector<SIZE_T> refused;  // handles whose view we never saw (logged once)
    bool viewsMoved = false;      // a view handle now names another resource: twins of orphaned resources go
    // R2b: the focus views' light tiles. ACE's tile lists are bindless (space 45, root table 18 of its scene root
    // signature, a 131,072-descriptor range): our descriptors take the top kTileSlots of that range in its heap.
    ComPtr<ID3D12RootSignature> cullRoot;
    ComPtr<ID3D12PipelineState> cullPso;      // copies tiledCulling's constants with the focus fields
    ID3D12DescriptorHeap* gameHeap = nullptr;  // ACE's shader-visible CBV/SRV/UAV heap
    UINT heapSize = 0, increment = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE bindlessBase{};  // root table 18 as ACE's stereo scene lists set it
    UINT baseIndex = 0, slotFirst = 0;            // absolute heap indices
    UINT tilesX = 0, tilesY = 0;                  // the game's tile grid per eye (frameData.tileCount)
    ComPtr<ID3D12Resource> frustums[2], opaque[2], transparent[2];
    ID3D12Resource* depthForTiles = nullptr;      // the focus depth twin our depth SRV names
    bool tilesShaderResource = false;             // opaque/transparent: after a recorded tile replay
    bool tilesReady = false, tilesBroken = false;
    uint32_t gameTile[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};  // opaque L/R, transparent L/R (relative)
    uint32_t ourTile[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::vector<std::pair<SIZE_T, int>> tileViews;  // CPU handles the game wrote its named SRVs to (staging or not):
                                                    // 0-3 tile lists, 4 post_process_color (previous-frame colour)
    // R2c: the focus views' previous-frame colour (our colour twin copied before each frame's clear).
    uint32_t gamePrev[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};  // its SRVs (relative)
    ComPtr<ID3D12Resource> prevColour;
    ID3D12Resource* prevSource = nullptr;
    ComPtr<ID3D12CommandAllocator> prevAllocator;
    ComPtr<List> prevList;
    bool prevReady = false;
    SIZE_T maxWritten = 0;                          // the highest index the game wrote in the bindless range
    SIZE_T maxUavWritten = 0;                       // the highest it wrote below the bindless range (the UAV table)
    bool collision = false;
    std::unordered_map<ID3D12PipelineState*, int> psoKinds;  // 0 none, 1 tileFrustums, 2 tiledCulling, 3/4 fog
    // R2c: the focus views' volumetric fog. Per eye our grid and two histories (ping-pong), 3D like ACE's; their views
    // at our slots 16 + 6e: grid UAV/SRV, history 0 UAV/SRV, history 1 UAV/SRV. ACE's grid SRVs learned by name.
    ComPtr<ID3D12RootSignature> fogRoot;
    ComPtr<ID3D12PipelineState> fogPso;
    ComPtr<ID3D12Resource> fogConstants;  // kFogSlots x kFogSlotBytes, UNORDERED_ACCESS
    uint32_t fogSlot = 0;
    ComPtr<ID3D12Resource> fogGrid[2], fogHistory[2][2];
    D3D12_RESOURCE_DESC gameGridDesc{}, gameHistoryDesc{};  // from ACE's voxelGrid0 / voxelGridHistory_0_0
    std::vector<ComPtr<ID3D12Resource>> views3d;          // every 3D texture the game made an SRV of (ACE names its
                                                          // resources after their views: names read at the fog pass)
    bool fogMade = false, fogBroken = false, fogHistoryValid = false, fogReady = false, fogMissLogged = false;
    // The fog shaders' UAVs (space 20) are in another table: its base (ACE's 475,136) and our UAVs at its range's top
    // (kFogUavSlots, offset = our bindless fog slot - kFogFirst), guarded like the bindless block.
    UINT uavBaseIndex = 0, fogUavFirst = 0;
    int fogParity = 0;
    std::atomic<uint64_t> fogReplays{0};
    SRWLOCK psoLock = SRWLOCK_INIT;  // psoKinds only (SetPipelineState is hot)
    std::atomic<uint64_t> tileReplays{0};
    std::atomic<uint64_t> frame{0}, clearedFrame{UINT64_MAX};
    // samples: armed for a frame (references replayed too), copied before the next frame's clear, saved off-thread
    std::atomic<uint64_t> sampleFrame{UINT64_MAX};
    std::atomic<uint32_t> samplesTaken{0};
    std::atomic<uint32_t> sampleSlot{UINT32_MAX};  // the sample frame's first constants slot
    std::atomic<bool> copyPending{false};
    std::atomic<uint64_t> copyFrame{0};  // the sample frame being copied (its label)
    std::atomic<bool> saving{false};
    ReplayFocusCompare compare;
    SRWLOCK compareLock = SRWLOCK_INIT;
};
Focus focus;

// Our descriptors at the top of the bindless range: 0-11 tile lists, 12 focus depth, 13 previous colour, 16-27 fog.
constexpr UINT kTileSlots = 32, kBindlessRange = 131072, kTileStride = 64;  // 16 uints a tile
constexpr UINT kFogFirst = 16, kFogUavSlots = 32;
constexpr UINT kFogSlots = 32, kFogSlotBytes = 16384;  // the injection constants are 15,840 B

// The culling constants (tiledCulling cb0, 240 B: inverseView, view, inverseProjectionMatrix at 128, tileCount 192,
// entityCount, dimension 208, pixelOffset 216, aabbTolerance) copied with the focus inverse projection and its region
// of our focus depth twin.
constexpr char kCullShader[] = R"(
cbuffer Source : register(b0) { float4 src[15]; };
cbuffer Params : register(b1) { float4 inverseProjection[4]; int2 dimension; int2 pixelOffset; };
RWByteAddressBuffer dst : register(u0);
[numthreads(16, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 15) return;
    float4 v = src[i];
    if (i >= 8 && i < 12) v = inverseProjection[i - 8];
    if (i == 13) v = float4(asfloat(dimension), asfloat(pixelOffset));
    dst.Store4(i * 16, asuint(v));
}
)";

// R2c: ACE's fog constants copied for a focus view. Injection (mode 0, 15,840 B: all 990 registers, the tail holds
// voxelGridSize, cameraForward, shLumaFactor): its matrices' columns are in the
// registers (column_major, out = M v): viewProjection 4-7 and prevViewProjection 8-11 get x' = a x + a ox w,
// y' = a y + a oy w per register; invViewProjection 16-19 the inverse (columns 0/1 x box, column 3 - ox c0 - oy c1).
// The eye's centre (ox, oy) = P02, P12 of P = VP inverse(view): view is rigid, so inverse(view)'s column 2 is the
// view rotation's row 2 (registers 0-2 .z). temporalReproject (24.z) off for a fresh history; historyIndex and
// outputIndex (25.z/w) ours. Scatter (mode 1, 32 B): outputIndex and inputIndex (1.x/y) ours.
constexpr char kFogShader[] = R"(
cbuffer Source : register(b0) { float4 src[990]; };
cbuffer Params : register(b1) { float halfExtent; uint mode; uint historySrv; uint outputUav; uint inputSrv; uint fresh; };
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (mode == 1) {
        if (i >= 2) return;
        uint4 s = asuint(src[i]);
        if (i == 1) { s.x = outputUav; s.y = inputSrv; }
        dst.Store4(i * 16, s);
        return;
    }
    if (i >= 990) return;
    float4 v = src[i];
    const float ox = src[4].x * src[0].z + src[5].x * src[1].z + src[6].x * src[2].z;
    const float oy = src[4].y * src[0].z + src[5].y * src[1].z + src[6].y * src[2].z;
    const float a = 1.0 / halfExtent;
    if (i >= 4 && i <= 11) {
        v.x = a * v.x + a * ox * v.w;
        v.y = a * v.y + a * oy * v.w;
    }
    if (i == 16 || i == 17) v *= halfExtent;
    if (i == 19) v = v - ox * src[16] - oy * src[17];
    if (i == 24 && fresh != 0) v.z = asfloat(0u);
    if (i == 25) { v.z = asfloat(historySrv); v.w = asfloat(outputUav); }
    dst.Store4(i * 16, asuint(v));
}
)";

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
                                                                         : 0;
    AcquireSRWLockExclusive(&focus.psoLock);
    focus.psoKinds[pso] = kind;
    ReleaseSRWLockExclusive(&focus.psoLock);
    return kind;
}

}  // namespace

// The tile shaders multiply out = M v with M's columns in the 4 registers (tileFrustums' disassembly): eye[4k + i] is
// M[i][k]. inverse(R P) = inverse(P) inverse(R), R: x' = x / box + (ox / box) w, y' likewise.
bool replay_focus_inverse_projection(const float (&eye)[16], double box, float (&out)[16]) {
    double q[4][4], p[4][4];
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            q[i][k] = eye[k * 4 + i];
    if (!invert(q, p))
        return false;
    const double ox = p[0][2], oy = p[1][2];  // P's x and y rows: (sx, 0, ox, 0), (0, sy, oy, 0)
    for (int i = 0; i < 4; ++i) {
        out[0 * 4 + i] = static_cast<float>(q[i][0] * box);
        out[1 * 4 + i] = static_cast<float>(q[i][1] * box);
        out[2 * 4 + i] = static_cast<float>(q[i][2]);
        out[3 * 4 + i] = static_cast<float>(q[i][3] - q[i][0] * ox - q[i][1] * oy);
    }
    return true;
}

namespace {

bool focus_compile() {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kPatchShader, sizeof(kPatchShader) - 1, "focus-constants", nullptr, nullptr, "main", "cs_5_0", 0,
                          0, &code, &errors))) {
        logf("replay focus: the constants shader did not compile: %s",
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {1, 0, 21};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    const D3D12_ROOT_SIGNATURE_DESC desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&focus.patchRoot)))) {
        log_line("replay focus: root signature failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = focus.patchRoot.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&focus.patchPso)))) {
        log_line("replay focus: compute pipeline failed");
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kSlots} * kSlotBytes, 1, 1, 1,
                               DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&focus.constants))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&focus.fence)))) {
        log_line("replay focus: constants buffer or fence failed");
        return false;
    }
    for (int i = kRtvSlots - 1; i >= 0; --i) focus.freeRtv.push_back(i);
    for (int i = kDsvSlots - 1; i >= 0; --i) focus.freeDsv.push_back(i);
    {  // R2b: the culling-constants copy
        ComPtr<ID3DBlob> cullCode, cullErrors, cullBlob;
        D3D12_ROOT_PARAMETER cullParams[3] = {};
        cullParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cullParams[0].Descriptor = {0, 0};
        cullParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        cullParams[1].Constants = {1, 0, 20};
        cullParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        cullParams[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC cullDesc{3, cullParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC cullPso{};
        if (FAILED(D3DCompile(kCullShader, sizeof(kCullShader) - 1, "focus-culling", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                              &cullCode, &cullErrors)) ||
            FAILED(D3D12SerializeRootSignature(&cullDesc, D3D_ROOT_SIGNATURE_VERSION_1, &cullBlob, &cullErrors)) ||
            FAILED(device->CreateRootSignature(0, cullBlob->GetBufferPointer(), cullBlob->GetBufferSize(),
                                               IID_PPV_ARGS(&focus.cullRoot)))) {
            log_line("replay focus: the culling-constants shader failed; no focus tiles");
            focus.tilesBroken = true;
        } else {
            cullPso.pRootSignature = focus.cullRoot.Get();
            cullPso.CS = {cullCode->GetBufferPointer(), cullCode->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&cullPso, IID_PPV_ARGS(&focus.cullPso))))
                focus.tilesBroken = true;
        }
    }
    if (focus.settings.fog) {  // R2c: the fog-constants copy
        ComPtr<ID3DBlob> fogCode, fogErrors, fogBlob;
        D3D12_ROOT_PARAMETER fogParams[3] = {};
        fogParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        fogParams[0].Descriptor = {0, 0};
        fogParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        fogParams[1].Constants = {1, 0, 6};
        fogParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        fogParams[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC fogDesc{3, fogParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC fogPso{};
        D3D12_HEAP_PROPERTIES fogHeap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC fogBuffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kFogSlots} * kFogSlotBytes, 1, 1, 1,
                                            DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        if (FAILED(D3DCompile(kFogShader, sizeof(kFogShader) - 1, "focus-fog", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                              &fogCode, &fogErrors)) ||
            FAILED(D3D12SerializeRootSignature(&fogDesc, D3D_ROOT_SIGNATURE_VERSION_1, &fogBlob, &fogErrors)) ||
            FAILED(device->CreateRootSignature(0, fogBlob->GetBufferPointer(), fogBlob->GetBufferSize(),
                                               IID_PPV_ARGS(&focus.fogRoot)))) {
            logf("replay focus: fog: the constants shader failed (%s); no focus fog",
                 fogErrors ? static_cast<const char*>(fogErrors->GetBufferPointer()) : "?");
            focus.fogBroken = true;
        } else {
            fogPso.pRootSignature = focus.fogRoot.Get();
            fogPso.CS = {fogCode->GetBufferPointer(), fogCode->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&fogPso, IID_PPV_ARGS(&focus.fogPso))) ||
                FAILED(device->CreateCommittedResource(&fogHeap, D3D12_HEAP_FLAG_NONE, &fogBuffer,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&focus.fogConstants))))
                focus.fogBroken = true;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSlots};
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvSlots};
    return SUCCEEDED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&focus.rtvHeap))) &&
           SUCCEEDED(device->CreateDescriptorHeap(&dsvHeap, IID_PPV_ARGS(&focus.dsvHeap)));
}

D3D12_CPU_DESCRIPTOR_HANDLE view_slot(bool depth, int& index) {
    std::vector<int>& free = depth ? focus.freeDsv : focus.freeRtv;
    if (free.empty())
        return {0};
    index = free.back();
    free.pop_back();
    D3D12_CPU_DESCRIPTOR_HANDLE h = (depth ? focus.dsvHeap : focus.rtvHeap)->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) *
             device->GetDescriptorHandleIncrementSize(depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return h;
}
void free_slot(bool depth, int& index) {
    if (index >= 0)
        (depth ? focus.freeDsv : focus.freeRtv).push_back(index);
    index = -1;
}

// One twin resource and its view, `width` x `height`, the game target's format and samples.
ComPtr<ID3D12Resource> make_twin(const Twin& t, UINT width, UINT height, D3D12_CPU_DESCRIPTOR_HANDLE& view, int& slot) {
    view = view_slot(t.depth, slot);
    if (!view.ptr)
        return nullptr;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const UINT samples = t.gameDesc.SampleDesc.Count;
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, t.gameDesc.Format,
                             {samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             t.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = t.viewFormat;
    if (t.depth)
        clear.DepthStencil = {0.0f, 0};  // ACE: reverse-Z
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            t.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&r))))
        return nullptr;
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC v{};
        v.Format = t.viewFormat;
        v.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvOriginal(device.Get(), r.Get(), &v, view);
    } else {
        D3D12_RENDER_TARGET_VIEW_DESC v{};
        v.Format = t.viewFormat;
        v.ViewDimension = samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvOriginal(device.Get(), r.Get(), &v, view);
    }
    return r;
}

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

// Under focus.lock: kept alive until the GPU passes the next fence value (lists in flight may still use it).
void retire(IUnknown* object) {
    if (object)
        focus.retired.emplace_back(focus.fenceValue.load() + 1, object);
}
// Under focus.lock: releases what the GPU is done with.
void prune_retired() {
    const uint64_t done = focus.fence->GetCompletedValue();
    focus.retired.erase(std::remove_if(focus.retired.begin(), focus.retired.end(),
                                       [done](const auto& r) { return r.first <= done; }),
                        focus.retired.end());
}

// Under focus.lock: one clear list for the focus twins (every frame) and one for the eye references (sample frames):
// depth to 0 (reverse-Z), colour to 0.
bool record_clear() {
    retire(focus.clearAllocator.Get());
    retire(focus.eyeClearAllocator.Get());
    retire(focus.clearList.Get());
    retire(focus.eyeClearList.Get());
    focus.clearAllocator.Reset();
    focus.eyeClearAllocator.Reset();
    focus.clearList.Reset();
    focus.eyeClearList.Reset();
    for (int which = 0; which < 2; ++which) {
        ComPtr<ID3D12CommandAllocator>& a = which ? focus.eyeClearAllocator : focus.clearAllocator;
        ComPtr<List>& l = which ? focus.eyeClearList : focus.clearList;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l))))
            return false;
        inReplay = true;
        const float zero[4] = {};
        for (const auto& [game, t] : focus.twins) {
            const D3D12_CPU_DESCRIPTOR_HANDLE v = which ? t.eyeView : t.focusView;
            if (t.depth)
                l->ClearDepthStencilView(v, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            else
                l->ClearRenderTargetView(v, zero, 0, nullptr);
        }
        l->Close();
        inReplay = false;
    }
    return true;
}

// Under focus.lock: twins for every target the list binds, each sized from its own game target (two eyes side by
// side); a twin is made again when its game target's format, size or samples change (a resolution or MSAA change in
// the game's settings).
bool focus_prepare(const Stream& s) {
    prune_retired();
    bool changed = false;
    if (focus.viewsMoved) {  // the game made targets again: twins of resources no view names any more are retired
        focus.viewsMoved = false;
        for (auto t = focus.twins.begin(); t != focus.twins.end();) {
            bool named = false;
            for (const auto& [h, v] : focus.views)
                if (v.resource.Get() == t->first) {
                    named = true;
                    break;
                }
            if (named) {
                ++t;
                continue;
            }
            logf("replay focus: the game made its %s again: the old twins are retired", t->second.name.c_str());
            retire(t->second.focus.Get());
            retire(t->second.eye.Get());
            free_slot(t->second.depth, t->second.focusSlot);
            free_slot(t->second.depth, t->second.eyeSlot);
            t = focus.twins.erase(t);
            twinsRemade.fetch_add(1);
            changed = true;
        }
    }
    for (const SIZE_T h : s.targets) {
        const auto v = focus.views.find(h);
        if (v == focus.views.end()) {
            if (std::find(focus.refused.begin(), focus.refused.end(), h) == focus.refused.end()) {
                focus.refused.push_back(h);
                log_line("replay focus: a stereo list binds a target view created before the probe; that list is not replayed");
            }
            return false;
        }
        const auto t = focus.twins.find(v->second.resource.Get());
        if (t == focus.twins.end())
            continue;
        const D3D12_RESOURCE_DESC d = v->second.resource->GetDesc();
        if (d.Format != t->second.gameDesc.Format || d.SampleDesc.Count != t->second.gameDesc.SampleDesc.Count ||
            d.Width != t->second.gameDesc.Width || d.Height != t->second.gameDesc.Height) {
            logf("replay focus: the game's %s changed (%llux%u -> %llux%u, format %d -> %d, %u -> %u sample(s)): its twins "
                 "are made again",
                 t->second.name.c_str(), static_cast<unsigned long long>(t->second.gameDesc.Width), t->second.gameDesc.Height,
                 static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(t->second.gameDesc.Format),
                 static_cast<int>(d.Format), t->second.gameDesc.SampleDesc.Count, d.SampleDesc.Count);
            retire(t->second.focus.Get());
            retire(t->second.eye.Get());
            free_slot(t->second.depth, t->second.focusSlot);
            free_slot(t->second.depth, t->second.eyeSlot);
            focus.twins.erase(t);
            twinsRemade.fetch_add(1);
            changed = true;
        }
    }
    const double k = focus.settings.boxFraction * focus.settings.pixelScale;
    for (const SIZE_T h : s.targets) {
        const ViewInfo& v = focus.views[h];
        if (focus.twins.count(v.resource.Get()))
            continue;
        Twin t;
        t.gameDesc = v.resource->GetDesc();
        t.viewFormat = v.viewFormat != DXGI_FORMAT_UNKNOWN ? v.viewFormat : t.gameDesc.Format;
        t.depth = v.depth;
        t.name = debug_name(v.resource.Get());
        t.eyeW = static_cast<UINT>(t.gameDesc.Width / 2);
        t.eyeH = t.gameDesc.Height;
        t.focusW = std::max(16u, static_cast<UINT>(std::lround(t.eyeW * k)));
        t.focusH = std::max(16u, static_cast<UINT>(std::lround(t.eyeH * k)));
        t.focus = make_twin(t, t.focusW * 2, t.focusH, t.focusView, t.focusSlot);
        t.eye = make_twin(t, t.eyeW * 2, t.eyeH, t.eyeView, t.eyeSlot);
        if (!t.focus || !t.eye) {
            free_slot(t.depth, t.focusSlot);
            free_slot(t.depth, t.eyeSlot);
            logf("replay focus: could not make twins of %s", t.name.c_str());
            return false;
        }
        logf("replay focus: twin of %s (%s, format %d, %u sample(s)): focus %ux%u per eye (box %.0f%%, %.2f focus pixels "
             "per eye pixel), reference %ux%u per eye",
             t.name.c_str(), t.depth ? "depth" : "colour", static_cast<int>(t.gameDesc.Format), t.gameDesc.SampleDesc.Count,
             t.focusW, t.focusH, focus.settings.boxFraction * 100, focus.settings.pixelScale, t.eyeW, t.eyeH);
        focus.twins.emplace(v.resource.Get(), std::move(t));
        twinsMade.fetch_add(1);
        changed = true;
    }
    if ((changed || !focus.clearList) && !record_clear())
        return false;
    focus.ready = true;
    return true;
}

// Under focus.lock: a free replay entry (its last execution done).
Replay* acquire_replay() {
    const uint64_t done = focus.fence->GetCompletedValue();
    for (auto& r : focus.pool)
        if (!r->busy && r->fence <= done) {
            r->busy = true;
            return r.get();
        }
    if (focus.pool.size() >= 96)
        return nullptr;
    auto r = std::make_unique<Replay>();
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r->allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&r->list))))
        return nullptr;
    r->list->Close();
    r->busy = true;
    focus.pool.push_back(std::move(r));
    return focus.pool.back().get();
}

void barrier(List* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    l->ResourceBarrier(1, &b);
}

// At the game's Close of a replayable stereo list: our replay of it, recorded now, executed after the game's batch.
void focus_record(List* gameList, const Stream& s) {
    Replay* r = nullptr;
    std::vector<std::pair<SIZE_T, SIZE_T>> focusTargets, eyeTargets;
    AcquireSRWLockExclusive(&focus.lock);
    if (focus_prepare(s)) {
        r = acquire_replay();
        for (const SIZE_T h : s.targets) {
            const Twin& t = focus.twins.at(focus.views.at(h).resource.Get());
            focusTargets.emplace_back(h, t.focusView.ptr);
            eyeTargets.emplace_back(h, t.eyeView.ptr);
        }
    }
    // This list's own eye size (ACE's passes differ: the flags pass is half width) and its focus size.
    const double k = focus.settings.boxFraction * focus.settings.pixelScale;
    const UINT ewi = static_cast<UINT>(s.eyeW), ehi = static_cast<UINT>(s.eyeH);
    const UINT fwi = std::max(16u, static_cast<UINT>(std::lround(s.eyeW * k)));
    const UINT fhi = std::max(16u, static_cast<UINT>(std::lround(s.eyeH * k)));
    // R2b: the bindless base (root table 18) as the stereo scene lists set it; the tile lists' indices game -> ours.
    uint32_t tileFrom[8] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t tileTo[8] = {};
    if (s.bindlessUint.ptr && !focus.bindlessBase.ptr)
        focus.bindlessBase = s.bindlessUint;
    if (focus.tilesReady && !focus.tilesBroken)
        for (int i = 0; i < 4; ++i) {
            tileFrom[i] = focus.gameTile[i];
            tileTo[i] = focus.ourTile[i];
        }
    int pairs = 4;
    const uint32_t fogSrv = focus.fogMade && focus.fogReady && !focus.fogBroken && !focus.tilesBroken
                                ? focus.slotFirst + kFogFirst + 1 - focus.baseIndex
                                : 0;
    if (focus.prevReady && !focus.tilesBroken)
        for (int i = 0; i < 4; ++i)
            if (focus.gamePrev[i] != UINT32_MAX && pairs < 8) {
                tileFrom[pairs] = focus.gamePrev[i];
                tileTo[pairs++] = focus.slotFirst + 13 - focus.baseIndex;
            }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    const int64_t start = now_ticks();
    const bool sample = focus.sampleFrame.load() == focus.frame.load();
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    // 1. The focus constants: one slot per distinct root-1 address.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> slots;
    l->SetComputeRootSignature(focus.patchRoot.Get());
    l->SetPipelineState(focus.patchPso.Get());
    uint32_t params[21] = {};
    const float box = static_cast<float>(focus.settings.boxFraction);
    const float floats[4] = {box, box, static_cast<float>(fwi), static_cast<float>(fhi)};
    std::memcpy(params, floats, sizeof(floats));
    std::memcpy(params + 4, tileFrom, sizeof(tileFrom));
    std::memcpy(params + 8, tileTo, sizeof(tileTo));
    params[20] = fogSrv;
    l->SetComputeRoot32BitConstants(1, 21, params, 0);
    for (const D3D12_GPU_VIRTUAL_ADDRESS va : s.passConstants) {
        const uint32_t slot = focus.nextSlot.fetch_add(1) % kSlots;
        if (sample) {
            uint32_t none = UINT32_MAX;
            focus.sampleSlot.compare_exchange_strong(none, slot);
        }
        const D3D12_GPU_VIRTUAL_ADDRESS mine = focus.constants->GetGPUVirtualAddress() + UINT64{slot} * kSlotBytes;
        l->SetComputeRootConstantBufferView(0, va);
        l->SetComputeRootUnorderedAccessView(2, mine);
        l->Dispatch(3, 1, 1);
        slots.push_back(mine);
    }
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    // 2. Both focus views into the focus twins.
    const float fw = static_cast<float>(fwi), fh = static_cast<float>(fhi);
    const D3D12_VIEWPORT focusViews[2] = {{0, 0, fw, fh, 0, 1}, {fw, 0, fw, fh, 0, 1}};
    const D3D12_RECT focusRects[2] = {{0, 0, static_cast<LONG>(fwi), static_cast<LONG>(fhi)},
                                      {static_cast<LONG>(fwi), 0, static_cast<LONG>(2 * fwi), static_cast<LONG>(fhi)}};
    const Substitution sub{focusViews, focusRects, 2, &s.passConstants, &slots, &focusTargets};
    l->RSSetViewports(2, focusViews);  // in case the list sets none itself
    l->RSSetScissorRects(2, focusRects);
    int64_t draws = emit(l, s, &sub);
    // 3. At sample frames, the same list unchanged into the eye-size references.
    if (sample && draws >= 0) {
        const float ew = static_cast<float>(ewi), eh = static_cast<float>(ehi);
        const D3D12_VIEWPORT eyeViews[2] = {{0, 0, ew, eh, 0, 1}, {ew, 0, ew, eh, 0, 1}};
        const D3D12_RECT eyeRects[2] = {{0, 0, static_cast<LONG>(ewi), static_cast<LONG>(ehi)},
                                        {static_cast<LONG>(ewi), 0, static_cast<LONG>(2 * ewi), static_cast<LONG>(ehi)}};
        const Substitution eyeSub{eyeViews, eyeRects, 2, nullptr, nullptr, &eyeTargets};
        l->RSSetViewports(2, eyeViews);
        l->RSSetScissorRects(2, eyeRects);
        if (emit(l, s, &eyeSub) < 0)
            draws = -1;
    }
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->Close();
    inReplay = false;
    replayTicks.fetch_add(static_cast<uint64_t>(now_ticks() - start), std::memory_order_relaxed);
    AcquireSRWLockExclusive(&focus.lock);
    if (draws < 0)
        r->busy = false;  // recorded but never executed: the allocator is reset at its next use
    else
        focus.pending[gameList].push_back(r);
    ReleaseSRWLockExclusive(&focus.lock);
    if (draws < 0)
        return;
    listsReplayed.fetch_add(1, std::memory_order_relaxed);
    callsReplayed.fetch_add(s.calls, std::memory_order_relaxed);
    drawsReplayed.fetch_add(static_cast<uint64_t>(draws), std::memory_order_relaxed);
}

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
ComPtr<ID3D12Resource> make_buffer(UINT64 bytes) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                          D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                    IID_PPV_ARGS(&r));
    return r;
}
void structured_views(ID3D12Resource* r, UINT elements, UINT stride, UINT uavSlot, UINT srvSlot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_UNKNOWN;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = elements;
    u.Buffer.StructureByteStride = stride;
    device->CreateUnorderedAccessView(r, nullptr, &u, game_cpu(uavSlot));
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.NumElements = elements;
    v.Buffer.StructureByteStride = stride;
    srvViewOriginal(device.Get(), r, &v, game_cpu(srvSlot));
}

// Under focus.lock: our tile resources and descriptors for this frame's tile passes; the game's tile-list indices.
const Twin* focus_tiles_prepare(const Stream::TileDispatch& frustums) {
    if (focus.tilesBroken || !focus.bindlessBase.ptr || !frustums.heap)
        return nullptr;
    if (focus.gameHeap != frustums.heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC d = frustums.heap->GetDesc();
        if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
            return nullptr;
        focus.gameHeap = frustums.heap;
        focus.heapSize = d.NumDescriptors;
        focus.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        focus.baseIndex = static_cast<UINT>((focus.bindlessBase.ptr - focus.gameHeap->GetGPUDescriptorHandleForHeapStart().ptr) /
                                            focus.increment);
        focus.slotFirst = focus.baseIndex + kBindlessRange - kTileSlots;
        if (focus.slotFirst + kTileSlots > focus.heapSize) {
            logf("replay focus: the bindless range (%u + %u) passes the heap's end (%u): no focus tiles", focus.baseIndex,
                 kBindlessRange, focus.heapSize);
            focus.tilesBroken = true;
            return nullptr;
        }
        logf("replay focus: tiles: the game's heap %u descriptors, bindless range from %u, ours %u..%u (the range's top)",
             focus.heapSize, focus.baseIndex, focus.slotFirst, focus.slotFirst + kTileSlots - 1);
        focus.tilesReady = false;
        focus.frustums[0].Reset();  // views written again below
    }
    // The focus depth twin: the largest single-sample depth twin (the stereo scene depth).
    const Twin* depth = nullptr;
    for (const auto& [game, tw] : focus.twins)
        if (tw.depth && tw.gameDesc.SampleDesc.Count == 1 && (!depth || tw.eyeW > depth->eyeW))
            depth = &tw;
    if (!depth || depth->gameDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return nullptr;
    float tileCount[2];
    std::memcpy(tileCount, frustums.constants + 18, sizeof(tileCount));
    const UINT tx = static_cast<UINT>(tileCount[0]), ty = static_cast<UINT>(tileCount[1]);
    if (!tx || !ty || tx > 4096 || ty > 4096)
        return nullptr;
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
        logf("replay focus: tiles: the game's grid %ux%u per eye; our tile lists made", tx, ty);
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
        logf("replay focus: tiles: the game's tile lists at bindless %u/%u (opaque) %u/%u (transparent), ours %u/%u %u/%u",
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

// At the game's Close of a list with its tile passes: per eye, tileFrustums and tiledCulling again with the focus
// view's inverse projection, resolution and depth, into our tile lists. Executed after the game's batch, like the
// scene replays (before the lighting lists that read them).
void focus_tiles_record(List* gameList, const Stream& s) {
    std::vector<const Stream::TileDispatch*> fr, cu;
    for (const auto& d : s.tileDispatches)
        if (d.kind == 1 || d.kind == 2)
            (d.kind == 1 ? fr : cu).push_back(&d);
    if (fr.size() < 2 || cu.size() < 2)
        return;
    Replay* r = nullptr;
    UINT fw = 0, fh = 0;
    ID3D12Resource* depth = nullptr;
    bool fromShaderResource = false;
    AcquireSRWLockExclusive(&focus.lock);
    const Twin* twin = focus_tiles_prepare(*fr[0]);
    if (twin && focus.tilesReady)
        r = acquire_replay();
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
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
    l->SetDescriptorHeaps(1, heaps);
    barrier(l, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
        if (!replay_focus_inverse_projection(eyeInverse, focus.settings.boxFraction, focusInverse)) {
            ok = false;
            break;
        }
        // 1. tiledCulling's constants with the focus fields, into our slot.
        uint32_t cullParams[20];
        std::memcpy(cullParams, focusInverse, sizeof(focusInverse));
        const int32_t region[4] = {static_cast<int32_t>(fw), static_cast<int32_t>(fh), static_cast<int32_t>(e * fw), 0};
        std::memcpy(cullParams + 16, region, sizeof(region));
        const uint32_t slot = focus.nextSlot.fetch_add(1) % kSlots;
        const D3D12_GPU_VIRTUAL_ADDRESS mine = focus.constants->GetGPUVirtualAddress() + UINT64{slot} * kSlotBytes;
        l->SetComputeRootSignature(focus.cullRoot.Get());
        l->SetPipelineState(focus.cullPso.Get());
        l->SetComputeRootConstantBufferView(0, c.cbv);
        l->SetComputeRoot32BitConstants(1, 20, cullParams, 0);
        l->SetComputeRootUnorderedAccessView(2, mine);
        l->Dispatch(1, 1, 1);
        // 2. tileFrustums for the focus view (the game's tile grid kept: frameData.tileCount is the lighting's stride).
        uint32_t frustumConstants[20];
        std::memcpy(frustumConstants, f.constants, sizeof(frustumConstants));
        std::memcpy(frustumConstants, focusInverse, sizeof(focusInverse));
        const float rcp[2] = {1.0f / static_cast<float>(fw), 1.0f / static_cast<float>(fh)};
        std::memcpy(frustumConstants + 16, rcp, sizeof(rcp));
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
    barrier(l, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    if (ok)
        focus.pending[gameList].push_back(r);
    else
        r->busy = false;
    ReleaseSRWLockExclusive(&focus.lock);
    if (ok)
        focus.tileReplays.fetch_add(1, std::memory_order_relaxed);
}

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
    if (focus.fogMade)
        return uavBase == focus.uavBaseIndex;
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
        device->CreateUnorderedAccessView(r.Get(), nullptr, &u, game_cpu(focus.fogUavFirst + slot - focus.slotFirst - kFogFirst));
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
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
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
        const float box = static_cast<float>(focus.settings.boxFraction);
        uint32_t params[6];
        std::memcpy(params, &box, 4);
        params[1] = 0;
        params[2] = rel(at + 3 + 2 * (1 - parity));  // last frame's history, SRV
        params[3] = relUav(at + 2 + 2 * parity);     // this frame's history, UAV
        params[4] = 0;
        params[5] = fresh ? 1u : 0u;
        l->SetComputeRootSignature(focus.fogRoot.Get());
        l->SetPipelineState(focus.fogPso.Get());
        l->SetComputeRootConstantBufferView(0, in.cbv);
        l->SetComputeRoot32BitConstants(1, 6, params, 0);
        l->SetComputeRootUnorderedAccessView(2, mineA);
        l->Dispatch(16, 1, 1);
        params[1] = 1;
        params[3] = relUav(at + 0);            // our grid, UAV
        params[4] = rel(at + 3 + 2 * parity);  // this frame's history, SRV
        l->SetComputeRootConstantBufferView(0, out.cbv);
        l->SetComputeRoot32BitConstants(1, 6, params, 0);
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
    focus.pending[gameList].push_back(r);
    if (!focus.fogReady)
        log_line("replay focus: fog: the first focus fog replay recorded; the lighting reads it from the next frame");
    focus.fogReady = true;  // the next scene replays name our grid (executed after this one: same queue, later batch)
    ReleaseSRWLockExclusive(&focus.lock);
    focus.fogReplays.fetch_add(1, std::memory_order_relaxed);
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
        logf("replay focus: previous-frame colour: ours at bindless %u (the game's post_process_color views %d %d %d %d)",
             focus.slotFirst + 13 - focus.baseIndex, static_cast<int>(focus.gamePrev[0]), static_cast<int>(focus.gamePrev[1]),
             static_cast<int>(focus.gamePrev[2]), static_cast<int>(focus.gamePrev[3]));
    }
    if (focus.prevReady) {
        ID3D12CommandList* lists[] = {focus.prevList.Get()};
        executeOriginal(queue, 1, lists);
    }
}

// ---- Samples: readback, pictures, comparison -----------------------------------------------------------------------

// R11G11B10_FLOAT: 6-bit-mantissa 11-bit floats (red, green) and a 5-bit-mantissa 10-bit float (blue), 5-bit exponents.
float unpack_small_float(uint32_t bits, int mantissaBits) {
    const uint32_t mantissa = bits & ((1u << mantissaBits) - 1), exponent = bits >> mantissaBits;
    if (exponent == 0)
        return std::ldexp(static_cast<float>(mantissa), -14 - mantissaBits);
    if (exponent == 31)
        return 65504.0f;
    return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissaBits),
                      static_cast<int>(exponent) - 15);
}
float half_to_float(uint16_t h) {
    const uint32_t sign = h >> 15, exponent = (h >> 10) & 31, mantissa = h & 1023;
    const float v = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                    : exponent == 31 ? 65504.0f
                                     : std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
    return sign ? -v : v;
}

bool colour_supported(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM;
}
bool depth_supported(DXGI_FORMAT f) { return f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_FLOAT; }

// A readback as floats: 1 per pixel (depth) or 3 (colour, as stored).
std::vector<float> read_back(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f, DXGI_FORMAT format,
                             bool depth) {
    const UINT w = f.Footprint.Width, h = f.Footprint.Height;
    std::vector<float> out(size_t{w} * h * (depth ? 1 : 3));
    void* p = nullptr;
    if (FAILED(readback->Map(0, nullptr, &p)))
        return {};
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(p) + f.Offset + size_t{y} * f.Footprint.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            const size_t i = size_t{y} * w + x;
            if (depth) {
                std::memcpy(&out[i], row + size_t{x} * 4, 4);
            } else if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
                uint32_t v;
                std::memcpy(&v, row + size_t{x} * 4, 4);
                out[i * 3 + 0] = unpack_small_float(v & 0x7ff, 6);
                out[i * 3 + 1] = unpack_small_float((v >> 11) & 0x7ff, 6);
                out[i * 3 + 2] = unpack_small_float((v >> 22) & 0x3ff, 5);
            } else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                uint16_t v[4];
                std::memcpy(v, row + size_t{x} * 8, 8);
                for (int c = 0; c < 3; ++c) out[i * 3 + c] = half_to_float(v[c]);
            } else {
                const uint8_t* v = row + size_t{x} * 4;
                const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
                for (int c = 0; c < 3; ++c) out[i * 3 + c] = v[bgra ? 2 - c : c] / 255.0f;
            }
        }
    }
    readback->Unmap(0, nullptr);
    return out;
}

void save_picture(const std::wstring& path, const std::vector<float>& v, UINT w, UINT h, bool depth, float scale) {
    std::vector<uint8_t> bgra(size_t{w} * h * 4);
    for (size_t i = 0; i < size_t{w} * h; ++i) {
        uint8_t rgb[3];
        if (depth) {
            const float d = v[i];
            rgb[0] = rgb[1] = rgb[2] =
                static_cast<uint8_t>(std::lround((d <= 0 ? 0.0f : std::sqrt(std::min(1.0f, d * scale))) * 255));
        } else {
            for (int c = 0; c < 3; ++c) {
                const float x = std::max(0.0f, v[i * 3 + c] * scale);
                rgb[c] = static_cast<uint8_t>(std::lround(std::pow(x / (1 + x), 1 / 2.2f) * 255));
            }
        }
        bgra[i * 4 + 0] = rgb[2];
        bgra[i * 4 + 1] = rgb[1];
        bgra[i * 4 + 2] = rgb[0];
        bgra[i * 4 + 3] = 255;
    }
    write_png_bgra(path, bgra, w, h);
}

struct SampleTarget {
    std::string name;
    bool depth = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> focusRead, eyeRead;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT focusFoot{}, eyeFoot{};
    UINT fw = 0, fh = 0, ew = 0, eh = 0;  // per eye
};
struct SampleJob {
    uint64_t fenceValue = 0, frame = 0;
    double box = 0;
    ComPtr<ID3D12Resource> constantsRead;
    std::vector<SampleTarget> targets;
    std::vector<ComPtr<IUnknown>> keep;  // the copy list and twins until the copy is done
};

// The focus pixel (u, v) of eye `eye`, as a fractional pixel position in the eye reference.
struct Mapper {
    double cx[2], cy[2], box;
    UINT fw, fh, ew, eh;
    void eye_pixel(int eye, double u, double v, double& x, double& y) const {
        const double nx = (u + 0.5) / fw * 2 - 1, ny = 1 - (v + 0.5) / fh * 2;
        x = (cx[eye] + nx * box + 1) * 0.5 * ew - 0.5;
        y = (1 - (cy[eye] + ny * box)) * 0.5 * eh - 0.5;
    }
};

// Bilinear in the eye reference, `channels` floats per pixel; false at the image edge or across sky / no sky.
bool bilinear(const std::vector<float>& img, UINT stride, int eye, UINT ew, UINT eh, double x, double y, int channels,
              const std::vector<float>* depthForSky, float* out) {
    const long px = static_cast<long>(std::floor(x)), py = static_cast<long>(std::floor(y));
    if (px < 1 || py < 1 || px + 2 >= static_cast<long>(ew) || py + 2 >= static_cast<long>(eh))
        return false;
    const auto idx = [&](long xx, long yy) {
        return static_cast<size_t>(yy) * stride + static_cast<size_t>(eye) * ew + static_cast<size_t>(xx);
    };
    if (depthForSky) {
        const auto& d = *depthForSky;
        const bool s00 = d[idx(px, py)] <= 0, s10 = d[idx(px + 1, py)] <= 0, s01 = d[idx(px, py + 1)] <= 0,
                   s11 = d[idx(px + 1, py + 1)] <= 0;
        if (s00 != s11 || s10 != s01 || s00 != s10)
            return false;  // an edge against the sky: no bilinear value
    }
    const double tx = x - px, ty = y - py;
    for (int c = 0; c < channels; ++c) {
        const float a = img[idx(px, py) * channels + c], b = img[idx(px + 1, py) * channels + c];
        const float d0 = img[idx(px, py + 1) * channels + c], d1 = img[idx(px + 1, py + 1) * channels + c];
        out[c] = static_cast<float>((a * (1 - tx) + b * tx) * (1 - ty) + (d0 * (1 - tx) + d1 * tx) * ty);
    }
    return true;
}

// Off the game's threads: waits for the copy, saves every twin pair and compares them through the remap.
void focus_save(std::shared_ptr<SampleJob> job) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    focus.fence->SetEventOnCompletion(job->fenceValue, event);
    WaitForSingleObject(event, 10000);
    CloseHandle(event);
    float rows[16] = {};
    void* p = nullptr;
    if (job->constantsRead && SUCCEEDED(job->constantsRead->Map(0, nullptr, &p))) {
        std::memcpy(rows, static_cast<const uint8_t*>(p) + 128 * 16, sizeof(rows));
        job->constantsRead->Unmap(0, nullptr);
    }
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"replay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ReplayFocusCompare result;
    std::vector<float> eyeDepth;
    UINT eyeDepthW = 0;
    double bestShare = -1;
    double cx[2] = {-rows[2], -rows[10]}, cy[2] = {-rows[6], -rows[14]};
    for (const SampleTarget& t : job->targets) {
        const Mapper m{{cx[0], cx[1]}, {cy[0], cy[1]}, job->box, t.fw, t.fh, t.ew, t.eh};
        const std::vector<float> f = read_back(t.focusRead.Get(), t.focusFoot, t.format, t.depth);
        const std::vector<float> e = read_back(t.eyeRead.Get(), t.eyeFoot, t.format, t.depth);
        if (f.empty() || e.empty())
            continue;
        // Picture scale: depth by the reference's 99th percentile; colour by its median (to ~0.18).
        std::vector<float> sorted;
        for (size_t i = 0; i < e.size(); i += t.depth ? 7 : 21)
            if (e[i] > 0) sorted.push_back(e[i]);
        std::sort(sorted.begin(), sorted.end());
        float scale = 1;
        if (!sorted.empty())
            scale = t.depth ? 1.0f / std::max(1e-6f, sorted[sorted.size() * 99 / 100])
                            : 0.18f / std::max(1e-4f, sorted[sorted.size() / 2]);
        wchar_t name[160];
        std::swprintf(name, 160, L"frame-%llu-%hs-focus.png", static_cast<unsigned long long>(job->frame), t.name.c_str());
        save_picture((dir / name).wstring(), f, t.fw * 2, t.fh, t.depth, scale);
        std::swprintf(name, 160, L"frame-%llu-%hs-eye.png", static_cast<unsigned long long>(job->frame), t.name.c_str());
        save_picture((dir / name).wstring(), e, t.ew * 2, t.eh, t.depth, scale);
        const UINT fStride = t.fw * 2, eStride = t.ew * 2;
        if (t.depth && bestShare < 0) {  // the largest depth twin (sorted first)
            eyeDepth = e;
            eyeDepthW = t.ew;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    uint64_t compared = 0, agree = 0;
                    for (int eye = 0; eye < 2; ++eye)
                        for (UINT v = 2; v + 2 < t.fh; v += 2)
                            for (UINT u = 2; u + 2 < t.fw; u += 2) {
                                double x, y;
                                m.eye_pixel(eye, u, v, x, y);
                                float b;
                                const float a = f[size_t{v} * fStride + static_cast<size_t>(eye) * t.fw + u];
                                if (!bilinear(e, eStride, eye, t.ew, t.eh, x + dx, y + dy, 1, &e, &b))
                                    continue;
                                if (a <= 0 && b <= 0)
                                    continue;  // sky in both
                                ++compared;
                                if (std::fabs(a - b) <= 0.01f * std::max(a, b))
                                    ++agree;
                            }
                    const double share = compared ? static_cast<double>(agree) / static_cast<double>(compared) : 0;
                    if (share > bestShare) {
                        bestShare = share;
                        result.pixels = compared;
                        result.agree = share;
                        result.shiftX = dx;
                        result.shiftY = dy;
                    }
                }
        } else if (!t.depth && result.colourPixels == 0) {  // the largest colour twin
            const bool skyMask = eyeDepthW == t.ew && !eyeDepth.empty();
            uint64_t compared = 0, agree = 0;
            for (int eye = 0; eye < 2; ++eye)
                for (UINT v = 2; v + 2 < t.fh; v += 2)
                    for (UINT u = 2; u + 2 < t.fw; u += 2) {
                        double x, y;
                        m.eye_pixel(eye, u, v, x, y);
                        float b[3];
                        if (!bilinear(e, eStride, eye, t.ew, t.eh, x + result.shiftX, y + result.shiftY, 3,
                                      skyMask ? &eyeDepth : nullptr, b))
                            continue;
                        const float* a = &f[(size_t{v} * fStride + static_cast<size_t>(eye) * t.fw + u) * 3];
                        ++compared;
                        bool same = true;
                        for (int c = 0; c < 3; ++c)
                            same = same && std::fabs(a[c] - b[c]) <= 0.03f * std::max(std::fabs(a[c]), std::fabs(b[c])) + 0.003f;
                        if (same)
                            ++agree;
                    }
            result.colourPixels = compared;
            result.colourAgree = compared ? static_cast<double>(agree) / static_cast<double>(compared) : 0;
            // At the eye's pixel size: k x k focus pixels (one eye pixel's footprint) averaged, compared with the eye
            // at the block's centre.
            const UINT k = std::max(1u, static_cast<UINT>(std::lround(t.fw / std::max(1.0, t.ew * job->box))));
            uint64_t eyeCompared = 0, eyeAgree = 0;
            for (int eye = 0; eye < 2; ++eye)
                for (UINT v0 = k; v0 + 2 * k <= t.fh; v0 += k)
                    for (UINT u0 = k; u0 + 2 * k <= t.fw; u0 += k) {
                        double x, y;
                        m.eye_pixel(eye, u0 + (k - 1) * 0.5, v0 + (k - 1) * 0.5, x, y);
                        float b[3];
                        if (!bilinear(e, eStride, eye, t.ew, t.eh, x + result.shiftX, y + result.shiftY, 3,
                                      skyMask ? &eyeDepth : nullptr, b))
                            continue;
                        float a[3] = {};
                        for (UINT v = v0; v < v0 + k; ++v)
                            for (UINT u = u0; u < u0 + k; ++u)
                                for (int c = 0; c < 3; ++c)
                                    a[c] += f[(size_t{v} * fStride + static_cast<size_t>(eye) * t.fw + u) * 3 + c];
                        ++eyeCompared;
                        bool same = true;
                        for (int c = 0; c < 3; ++c) {
                            a[c] /= static_cast<float>(k * k);
                            same = same && std::fabs(a[c] - b[c]) <= 0.03f * std::max(std::fabs(a[c]), std::fabs(b[c])) + 0.003f;
                        }
                        if (same)
                            ++eyeAgree;
                    }
            result.colourEyePixels = eyeCompared;
            result.colourEyeAgree = eyeCompared ? static_cast<double>(eyeAgree) / static_cast<double>(eyeCompared) : 0;
            logf("replay focus: sample frame %llu: colour %s at the eye's pixel size (%ux%u focus pixels each): %llu "
                 "compared, %.1f%% agree",
                 static_cast<unsigned long long>(job->frame), t.name.c_str(), k, k,
                 static_cast<unsigned long long>(eyeCompared), result.colourEyeAgree * 100);
            logf("replay focus: sample frame %llu: colour %s: %llu pixels compared, %.1f%% agree (every channel within 3%%)",
                 static_cast<unsigned long long>(job->frame), t.name.c_str(), static_cast<unsigned long long>(compared),
                 result.colourAgree * 100);
        }
    }
    AcquireSRWLockExclusive(&focus.compareLock);
    result.sample = focus.compare.sample + 1;
    focus.compare = result;
    ReleaseSRWLockExclusive(&focus.compareLock);
    logf("replay focus: sample frame %llu: depth %llu pixels compared, %.1f%% agree within 1%% at the best shift (%d, %d) "
         "eye pixels; eye centres (%.4f, %.4f) and (%.4f, %.4f); images in logs\\replay",
         static_cast<unsigned long long>(job->frame), static_cast<unsigned long long>(result.pixels), result.agree * 100,
         result.shiftX, result.shiftY, cx[0], cy[0], cx[1], cy[1]);
    focus.saving = false;
}

// Under focus.lock, on the submit thread before the next frame's clear: every twin of a readable format and the
// sample's constants copied to readback, then saved off-thread.
void focus_copy_sample() {
    focus.copyPending = false;
    auto job = std::make_shared<SampleJob>();
    job->frame = focus.copyFrame.load();
    job->box = focus.settings.boxFraction;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List> l;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&l))))
        return;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    const auto buffer = [&](UINT64 bytes) {
        ComPtr<ID3D12Resource> out;
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&out));
        return out;
    };
    inReplay = true;
    // Depth twins first (the colour comparison skips sky edges by the depth).
    std::vector<const Twin*> order;
    for (const auto& [game, t] : focus.twins)
        if (t.gameDesc.SampleDesc.Count == 1 &&
            (t.depth ? depth_supported(t.gameDesc.Format) : colour_supported(t.viewFormat)))
            order.push_back(&t);
    std::stable_sort(order.begin(), order.end(), [](const Twin* a, const Twin* b) {
        return a->depth != b->depth ? a->depth : a->eyeW > b->eyeW;  // depth first, then the largest
    });
    for (const Twin* t : order) {
        SampleTarget s;
        s.name = t->name;
        s.depth = t->depth;
        s.format = t->depth ? DXGI_FORMAT_R32_FLOAT : t->viewFormat;
        s.fw = t->focusW;
        s.fh = t->focusH;
        s.ew = t->eyeW;
        s.eh = t->eyeH;
        const D3D12_RESOURCE_DESC fd = t->focus->GetDesc(), ed = t->eye->GetDesc();
        UINT64 fb = 0, eb = 0;
        device->GetCopyableFootprints(&fd, 0, 1, 0, &s.focusFoot, nullptr, nullptr, &fb);
        device->GetCopyableFootprints(&ed, 0, 1, 0, &s.eyeFoot, nullptr, nullptr, &eb);
        s.focusRead = buffer(fb);
        s.eyeRead = buffer(eb);
        if (!s.focusRead || !s.eyeRead)
            continue;
        const D3D12_RESOURCE_STATES state = t->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        const std::tuple<ID3D12Resource*, ID3D12Resource*, D3D12_PLACED_SUBRESOURCE_FOOTPRINT> pairs[2] = {
            {t->focus.Get(), s.focusRead.Get(), s.focusFoot}, {t->eye.Get(), s.eyeRead.Get(), s.eyeFoot}};
        for (const auto& [twin, read, foot] : pairs) {
            barrier(l.Get(), twin, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{read, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = foot;
            D3D12_TEXTURE_COPY_LOCATION src{twin, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), twin, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            job->keep.emplace_back(twin);
        }
        job->targets.push_back(std::move(s));
    }
    const uint32_t slot = focus.sampleSlot.load();
    if (slot != UINT32_MAX) {
        job->constantsRead = buffer(kSlotBytes);
        barrier(l.Get(), focus.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(job->constantsRead.Get(), 0, focus.constants.Get(), UINT64{slot} * kSlotBytes, kSlotBytes);
        barrier(l.Get(), focus.constants.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    l->Close();
    inReplay = false;
    if (job->targets.empty()) {
        log_line("replay focus: no twin of a format the sample can read (multisampled, or not depth / colour)");
        return;
    }
    ID3D12CommandList* lists[] = {l.Get()};
    executeOriginal(focus.queue.Get(), 1, lists);
    job->fenceValue = focus.fenceValue.fetch_add(1) + 1;
    focus.queue->Signal(focus.fence.Get(), job->fenceValue);
    job->keep.emplace_back(l.Get());
    job->keep.emplace_back(allocator.Get());
    focus.saving = true;
    std::thread(focus_save, job).detach();
}

void STDMETHODCALLTYPE executeHook(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists) {
    executeOriginal(queue, n, lists);
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
    if (!mine.empty()) {
        const uint64_t frame = focus.frame.load();
        if (focus.clearedFrame.load() != frame && focus.ready) {
            if (focus.copyPending.load() && !focus.saving.load())
                focus_copy_sample();  // the sample frame's replays are all done: copy before this frame's clear
            focus_prev_copy(queue);   // last frame's focus colour, before this frame's clear
            ID3D12CommandList* clear[] = {focus.clearList.Get(), focus.eyeClearList.Get()};
            // The eye references are drawn only at sample frames: cleared only then.
            executeOriginal(queue, focus.sampleFrame.load() == frame ? 2u : 1u, clear);
            focus.clearedFrame = frame;
        }
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (mine.empty())
        return;
    std::vector<ID3D12CommandList*> ours;
    for (Replay* r : mine)
        ours.push_back(r->list.Get());
    executeOriginal(queue, static_cast<UINT>(ours.size()), ours.data());
    const uint64_t value = focus.fenceValue.fetch_add(1) + 1;
    queue->Signal(focus.fence.Get(), value);
    AcquireSRWLockExclusive(&focus.lock);
    for (Replay* r : mine) {
        r->fence = value;
        r->busy = false;
    }
    ReleaseSRWLockExclusive(&focus.lock);
    focusReplays.fetch_add(mine.size(), std::memory_order_relaxed);
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
    if (inReplay || FAILED(hr) || list->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE)
        return hr;
    Stream* s = nullptr;
    AcquireSRWLockExclusive(&streamsLock);
    auto& slot = streams[list];
    if (!slot)
        slot = std::make_unique<Stream>();
    s = slot.get();
    ReleaseSRWLockExclusive(&streamsLock);
    current = {list, s};
    s->begin(now_ticks());
    if (pso) {
        s->op(kPso);
        s->put(pso);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE closeHook(List* list) {
    const HRESULT hr = closeOriginal(list);
    Stream* s = stream_of(list);
    if (!s)
        return hr;
    s->active = false;
    listsSeen.fetch_add(1, std::memory_order_relaxed);
    hookCycles.fetch_add(s->hookCycles, std::memory_order_relaxed);
    if (probeMode == 3) {
        if (SUCCEEDED(hr) && focus.settings.tiles && !s->tileDispatches.empty())
            focus_tiles_record(list, *s);
        if (SUCCEEDED(hr) && focus.settings.tiles && focus.settings.fog && !s->tileDispatches.empty())
            focus_fog_record(list, *s);
        if (SUCCEEDED(hr) && s->replayable(focus.settings.allTargets)) {
            gameTicks.fetch_add(static_cast<uint64_t>(now_ticks() - s->resetTicks), std::memory_order_relaxed);
            focus_record(list, *s);
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
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kRootSig); s->put(r); }
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
        if (i == 18)  // ACE's scene root signature: space 45, the bindless uint buffers (tile lists)
            s->bindlessUint = h;
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
    }
    constNOriginal(l, i, n, data, o);
}
void STDMETHODCALLTYPE cbvHook(List* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kCbv); s->put(i); s->put(va);
        if (i == kPassConstantsRoot && std::find(s->passConstants.begin(), s->passConstants.end(), va) == s->passConstants.end())
            s->passConstants.push_back(va);
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
    }
    vbOriginal(l, start, n, views);
}
void STDMETHODCALLTYPE viewportsHook(List* l, UINT n, const D3D12_VIEWPORT* v) {
    if (Stream* s = stream_of(l); s && v) {
        HookTimer t(s);
        s->op(kViewports); s->put(n); s->put_bytes(v, sizeof(D3D12_VIEWPORT) * n);
        for (UINT i = 0; i < n; ++i) s->maxViewportWidth = std::max(s->maxViewportWidth, v[i].Width);
        if (n == 2 && v[0].TopLeftX == 0 && v[0].TopLeftY == 0 && v[1].TopLeftY == 0 && v[0].Width == v[1].Width &&
            v[0].Height == v[1].Height && v[1].TopLeftX == v[0].Width && v[0].Width >= 64) {
            s->stereo = true;
            s->eyeW = v[0].Width;
            s->eyeH = v[0].Height;
        }
    }
    viewportsOriginal(l, n, v);
}
void STDMETHODCALLTYPE scissorsHook(List* l, UINT n, const D3D12_RECT* r) {
    if (Stream* s = stream_of(l); s && r) { HookTimer t(s); s->op(kScissors); s->put(n); s->put_bytes(r, sizeof(D3D12_RECT) * n); }
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
        if (n > 0) s->sawRtv = true;
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
    computeConstNOriginal(l, i, n, data, o);
}
void STDMETHODCALLTYPE computeCbvHook(List* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (Stream* s = stream_of(l); s && i == 0) s->computeCbv = va;
    computeCbvOriginal(l, i, va);
}
void STDMETHODCALLTYPE computeTableHook(List* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (Stream* s = stream_of(l); s && i < 8) s->tables[i] = h;
    computeTableOriginal(l, i, h);
}
void STDMETHODCALLTYPE dispatchHook(List* l, UINT x, UINT y, UINT z) {
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
        s->tileDispatches.push_back(d);
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
    check_reserved(dest.ptr, n);
    ReleaseSRWLockExclusive(&focus.lock);
}
void STDMETHODCALLTYPE drawHook(List* l, UINT a, UINT b, UINT c, UINT d) {
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kDraw); s->put(a); s->put(b); s->put(c); s->put(d); ++s->draws; }
    drawOriginal(l, a, b, c, d);
}
void STDMETHODCALLTYPE drawIndexedHook(List* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kDrawIndexed); s->put(a); s->put(b); s->put(c); s->put(d); s->put(e);
        ++s->draws;
    }
    drawIndexedOriginal(l, a, b, c, d, e);
}
void STDMETHODCALLTYPE indirectHook(List* l, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args, UINT64 argsOffset,
                                    ID3D12Resource* count, UINT64 countOffset) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kIndirect); s->put(sig); s->put(max); s->put(args); s->put(argsOffset); s->put(count); s->put(countOffset);
        ++s->draws;
    }
    indirectOriginal(l, sig, max, args, argsOffset, count, countOffset);
}

bool patch(void** vtable, size_t slot, void* hook, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}
template <class Fn> bool hook(void** vtable, size_t slot, Fn detour, Fn* original) {
    return patch(vtable, slot, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
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

}  // namespace

bool replay_probe_start(ID3D12Device* d, int mode, ID3D12CommandQueue* queue, const ReplayFocusSettings& settings) {
    if (installed || !d)
        return installed;
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
    void** vtable = *reinterpret_cast<void***>(list.Get());
    bool ok =
        hook(vtable, simqvfr_slot_Reset(), &resetHook, &resetOriginal) &&
        hook(vtable, simqvfr_slot_Close(), &closeHook, &closeOriginal) &&
        hook(vtable, simqvfr_slot_SetGraphicsRootSignature(), &rootSigHook, &rootSigOriginal) &&
        hook(vtable, simqvfr_slot_SetPipelineState(), &psoHook, &psoOriginal) &&
        hook(vtable, simqvfr_slot_SetDescriptorHeaps(), &heapsHook, &heapsOriginal) &&
        hook(vtable, simqvfr_slot_SetGraphicsRootDescriptorTable(), &tableHook, &tableOriginal) &&
        hook(vtable, simqvfr_slot_SetGraphicsRoot32BitConstant(), &const1Hook, &const1Original) &&
        hook(vtable, simqvfr_slot_SetGraphicsRoot32BitConstants(), &constNHook, &constNOriginal) &&
        hook(vtable, simqvfr_slot_SetGraphicsRootConstantBufferView(), &cbvHook, &cbvOriginal) &&
        hook(vtable, simqvfr_slot_SetGraphicsRootShaderResourceView(), &srvHook, &srvOriginal) &&
        hook(vtable, simqvfr_slot_IASetPrimitiveTopology(), &topologyHook, &topologyOriginal) &&
        hook(vtable, simqvfr_slot_IASetIndexBuffer(), &ibHook, &ibOriginal) &&
        hook(vtable, simqvfr_slot_IASetVertexBuffers(), &vbHook, &vbOriginal) &&
        hook(vtable, simqvfr_slot_RSSetViewports(), &viewportsHook, &viewportsOriginal) &&
        hook(vtable, simqvfr_slot_RSSetScissorRects(), &scissorsHook, &scissorsOriginal) &&
        hook(vtable, simqvfr_slot_OMSetStencilRef(), &stencilHook, &stencilOriginal) &&
        hook(vtable, simqvfr_slot_OMSetRenderTargets(), &omHook, &omOriginal) &&
        hook(vtable, simqvfr_slot_DrawInstanced(), &drawHook, &drawOriginal) &&
        hook(vtable, simqvfr_slot_DrawIndexedInstanced(), &drawIndexedHook, &drawIndexedOriginal) &&
        hook(vtable, simqvfr_slot_ExecuteIndirect(), &indirectHook, &indirectOriginal);
    if (ok && mode == 3) {
        focus.queue = queue;
        void** deviceTable = *reinterpret_cast<void***>(d);
        ok = hook(deviceTable, simqvfr_slot_CreateDepthStencilView(), &dsvHook, &dsvOriginal) &&
             hook(deviceTable, simqvfr_slot_CreateRenderTargetView(), &rtvHook, &rtvOriginal) &&
             hook(*reinterpret_cast<void***>(queue), simqvfr_slot_ExecuteCommandLists(), &executeHook, &executeOriginal) &&
             focus_compile();
        if (ok && settings.tiles)
            ok = hook(vtable, simqvfr_slot_SetComputeRootSignature(), &computeRootSigHook, &computeRootSigOriginal) &&
                 hook(vtable, simqvfr_slot_SetComputeRoot32BitConstants(), &computeConstNHook, &computeConstNOriginal) &&
                 hook(vtable, simqvfr_slot_SetComputeRootConstantBufferView(), &computeCbvHook, &computeCbvOriginal) &&
                 hook(vtable, simqvfr_slot_SetComputeRootDescriptorTable(), &computeTableHook, &computeTableOriginal) &&
                 hook(vtable, simqvfr_slot_Dispatch(), &dispatchHook, &dispatchOriginal) &&
                 hook(deviceTable, simqvfr_slot_CreateShaderResourceView(), &srvViewHook, &srvViewOriginal) &&
                 hook(deviceTable, simqvfr_slot_CopyDescriptorsSimple(), &copySimpleHook, &copySimpleOriginal);
        else if (ok)
            srvViewOriginal = reinterpret_cast<SrvFn>(deviceTable[simqvfr_slot_CreateShaderResourceView()]);
        focus.sampleFrame = settings.saveCount ? settings.saveFirst : UINT64_MAX;
    }
    installed = ok;
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
    if (probeMode == 3) {
        const uint64_t ended = focus.frame.fetch_add(1);  // the frame that just ended
        if (ended == focus.sampleFrame.load()) {
            AcquireSRWLockShared(&focus.lock);
            bool readable = false;
            for (const auto& [game, t] : focus.twins)
                readable = readable || t.gameDesc.SampleDesc.Count == 1;
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
                    log_line("replay focus: the game's targets are multisampled; the sample waits for MSAA off");
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
    if (probeMode == 3) {
        logf("replay focus: window %llu, per frame: stereo lists replayed %.2f (executed %.2f), draws %.0f; tile replays "
             "%.2f, fog replays %.2f; recording the replays %.3f ms, recording hooks %.3f ms, the game's own recording of "
             "those lists %.3f ms (CPU)",
             static_cast<unsigned long long>(window), s.listsReplayed / n, s.focusReplays / n, s.drawsReplayed / n,
             s.tileReplays / n, s.fogReplays / n, s.replayMs / n, s.hookMs / n, s.gameMs / n);
        ++window;
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

}  // namespace simqvfr

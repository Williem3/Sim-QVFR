#include "replay_probe.h"

#include <d3dcompiler.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <unordered_map>
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
extern "C" size_t simqvfr_slot_ExecuteCommandLists(void);

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
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

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
ExecuteFn executeOriginal;

enum Op : uint8_t {
    kRootSig = 1, kPso, kHeaps, kTable, kConst1, kConstN, kCbv, kSrv, kTopology, kIb, kVb, kViewports, kScissors,
    kStencil, kDraw, kDrawIndexed, kIndirect
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
    // mode 3: a depth-only list with two eye-sized viewports side by side
    bool sawRtv = false, sawDsv = false, stereo = false;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    float eyeW = 0, eyeH = 0;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> passConstants;  // distinct root-1 addresses, in order

    void begin(int64_t now) {
        bytes.clear();
        active = true;
        draws = calls = 0;
        maxViewportWidth = 0;
        resetTicks = now;
        hookCycles = 0;
        sawRtv = sawDsv = stereo = false;
        dsv = {};
        eyeW = eyeH = 0;
        passConstants.clear();
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
    bool focus_depth() const { return sawDsv && !sawRtv && stereo && draws > 0 && !passConstants.empty(); }
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

// What a replay changes: its viewports and scissors (all of them), and root-1 addresses (by the stream's order).
struct Substitution {
    const D3D12_VIEWPORT* viewports = nullptr;
    const D3D12_RECT* scissors = nullptr;
    UINT count = 0;
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* from = nullptr;
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* to = nullptr;
};

// Re-emits a stream into `l` (open). Returns the draws emitted, or -1 on a broken stream.
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

// ---- Mode 3: the focus depth replay --------------------------------------------------------------------------------

// The pass constants copied into our slot with the off-axis focus remap (ACE's layout, recon-ace.md "First drawn
// replay"): left projections at rows 13/17/21/25, right at 95/103/107, each 4 rows x, y, z, w with w = (0,0,-1,0);
// row 38.xy target size, row 84 the viewport rect, row 112.z eyeHalfWidthPixels. Each eye's centre comes from its
// unjittered projection (rows 21/22 and 103/104: ndc centre = -row.z). Rows 128-131 keep the unjittered x/y rows.
constexpr char kPatchShader[] = R"(
cbuffer Source : register(b0) { float4 src[128]; };
cbuffer Params : register(b1) { float2 halfExtent; float2 focusSize; };
RWByteAddressBuffer dst : register(u0);
bool is_in(uint r, uint a, uint b, uint c, uint d) { return r == a || r == b || r == c || r == d; }
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 132) return;
    if (i >= 128) {
        const uint from[4] = {21, 22, 103, 104};
        dst.Store4(i * 16, asuint(src[from[i - 128]]));
        return;
    }
    float4 v = src[i];
    const bool left = i < 64;
    const float ox = left ? src[21].z : src[103].z;
    const float oy = left ? src[22].z : src[104].z;
    const float ax = 1.0 / halfExtent.x, ay = 1.0 / halfExtent.y;
    if (is_in(i, 13, 17, 21, 25) || is_in(i, 95, 103, 107, 107))
        v = ax * v + (ax * ox) * src[i + 3];          // x' = a x + b w, b = -a * centre = a * ox
    else if (is_in(i, 14, 18, 22, 26) || is_in(i, 96, 104, 108, 108))
        v = ay * v + (ay * oy) * src[i + 2];          // y' = c y + d w
    if (i == 38) v.xy = float2(focusSize.x * 2, focusSize.y);
    if (i == 84) v = float4(0, 0, focusSize.x, focusSize.y);
    if (i == 112) v.z = focusSize.x;  // eyeHalfWidthPixels (a float, as ACE writes it)
    dst.Store4(i * 16, asuint(v));
}
)";

constexpr UINT kSlotBytes = 4096;  // 2 KB constants + 4 kept rows, CBV-aligned
constexpr UINT kSlots = 512;

struct Replay {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List> list;
    uint64_t fence = 0;  // done when the fence reaches it
    bool busy = false;
};

struct DsvInfo {
    ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
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
    ComPtr<ID3D12DescriptorHeap> dsvHeap;  // 0: focus, 1: eye reference
    ComPtr<ID3D12Resource> focusDepth, eyeDepth;
    UINT fw = 0, fh = 0, eyeW = 0, eyeH = 0;
    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN, viewFormat = DXGI_FORMAT_UNKNOWN;
    UINT samples = 1;
    bool ready = false, failed = false;
    std::vector<ComPtr<IUnknown>> retired;  // targets and lists of an earlier game depth: kept alive (in flight)
    ComPtr<ID3D12CommandAllocator> clearAllocator;
    ComPtr<List> clearList;
    SRWLOCK lock = SRWLOCK_INIT;  // pool, pending, targets, dsvs
    std::vector<std::unique_ptr<Replay>> pool;
    std::unordered_map<List*, Replay*> pending;  // the game's list -> our replay of it
    std::unordered_map<SIZE_T, DsvInfo> dsvs;    // DSV CPU handle -> its resource
    std::atomic<uint64_t> frame{0}, clearedFrame{UINT64_MAX};
    // samples: armed for a frame (both targets replayed), copied before the next frame's clear, saved off-thread
    std::atomic<uint64_t> sampleFrame{UINT64_MAX};
    std::atomic<uint32_t> samplesTaken{0};
    std::atomic<uint32_t> sampleSlot{UINT32_MAX};  // the sample frame's first constants slot
    std::atomic<bool> copyPending{false};
    std::atomic<uint64_t> copyFrame{0};  // the sample frame being copied (its label)
    ComPtr<ID3D12CommandAllocator> copyAllocator;
    ComPtr<List> copyList;
    ComPtr<ID3D12Resource> readFocus, readEye, readConstants;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footFocus{}, footEye{};
    std::atomic<bool> saving{false};
    ReplayFocusCompare compare;
    SRWLOCK compareLock = SRWLOCK_INIT;
};
Focus focus;

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
    params[1].Constants = {1, 0, 4};
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
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
    return SUCCEEDED(device->CreateDescriptorHeap(&dsvHeap, IID_PPV_ARGS(&focus.dsvHeap)));
}

D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = focus.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return h;
}

ComPtr<ID3D12Resource> make_depth(UINT width, UINT height, int slot) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, focus.resourceFormat,
                             {focus.samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = focus.viewFormat;
    clear.DepthStencil = {0.0f, 0};  // ACE: reverse-Z
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                               IID_PPV_ARGS(&r))))
        return nullptr;
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = focus.viewFormat;
    view.ViewDimension = focus.samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvOriginal(device.Get(), r.Get(), &view, dsv_handle(slot));
    return r;
}

// Under focus.lock: our targets, from the game's depth (format, samples) and eye size; made again when those change
// (the player switches MSAA or the resolution in the game's settings: its pipelines then expect the new depth).
bool focus_targets(const Stream& s) {
    if (focus.failed)
        return false;
    const auto it = focus.dsvs.find(s.dsv.ptr);
    if (it == focus.dsvs.end()) {
        if (focus.ready)
            return true;  // a view we never saw: keep the targets we have
        focus.failed = true;
        log_line("replay focus: the stereo depth pass binds a depth view created before the probe; no replay");
        return false;
    }
    const D3D12_RESOURCE_DESC desc = it->second.resource->GetDesc();
    const DXGI_FORMAT view = it->second.viewFormat != DXGI_FORMAT_UNKNOWN ? it->second.viewFormat : desc.Format;
    if (focus.ready) {
        if (desc.Format == focus.resourceFormat && view == focus.viewFormat && desc.SampleDesc.Count == focus.samples &&
            static_cast<UINT>(s.eyeW) == focus.eyeW && static_cast<UINT>(s.eyeH) == focus.eyeH)
            return true;
        logf("replay focus: the game's depth changed (format %d -> %d, %u -> %u sample(s), eye %ux%u -> %.0fx%.0f): our "
             "targets are made again",
             static_cast<int>(focus.resourceFormat), static_cast<int>(desc.Format), focus.samples, desc.SampleDesc.Count,
             focus.eyeW, focus.eyeH, s.eyeW, s.eyeH);
        for (IUnknown* old : {static_cast<IUnknown*>(focus.focusDepth.Get()), static_cast<IUnknown*>(focus.eyeDepth.Get()),
                              static_cast<IUnknown*>(focus.clearList.Get()), static_cast<IUnknown*>(focus.clearAllocator.Get()),
                              static_cast<IUnknown*>(focus.copyList.Get()), static_cast<IUnknown*>(focus.copyAllocator.Get()),
                              static_cast<IUnknown*>(focus.readFocus.Get()), static_cast<IUnknown*>(focus.readEye.Get())})
            if (old)
                focus.retired.emplace_back(old);
        focus.focusDepth.Reset();
        focus.eyeDepth.Reset();
        focus.clearList.Reset();
        focus.clearAllocator.Reset();
        if (!focus.saving.load()) {  // a sample being saved still reads these; its own references keep them
            focus.copyList.Reset();
            focus.copyAllocator.Reset();
            focus.readFocus.Reset();
            focus.readEye.Reset();
        }
        focus.ready = false;
    }
    focus.resourceFormat = desc.Format;
    focus.viewFormat = view;
    focus.samples = desc.SampleDesc.Count;
    focus.eyeW = static_cast<UINT>(s.eyeW);
    focus.eyeH = static_cast<UINT>(s.eyeH);
    const double k = focus.settings.boxFraction * focus.settings.pixelScale;
    focus.fw = std::max(16u, static_cast<UINT>(std::lround(s.eyeW * k)));
    focus.fh = std::max(16u, static_cast<UINT>(std::lround(s.eyeH * k)));
    focus.focusDepth = make_depth(focus.fw * 2, focus.fh, 0);
    focus.eyeDepth = make_depth(focus.eyeW * 2, focus.eyeH, 1);
    if (!focus.focusDepth || !focus.eyeDepth ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&focus.clearAllocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, focus.clearAllocator.Get(), nullptr,
                                         IID_PPV_ARGS(&focus.clearList)))) {
        focus.failed = true;
        log_line("replay focus: could not make our depth targets");
        return false;
    }
    inReplay = true;  // our own lists are not recorded
    focus.clearList->ClearDepthStencilView(dsv_handle(0), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    focus.clearList->ClearDepthStencilView(dsv_handle(1), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    focus.clearList->Close();
    inReplay = false;
    focus.ready = true;
    logf("replay focus: our depth targets: focus %ux%u per eye (box %.0f%% of each eye's extent, %.2f focus pixels per eye "
         "pixel), eye reference %ux%u per eye; game depth format %d (view %d), %u sample(s)",
         focus.fw, focus.fh, focus.settings.boxFraction * 100, focus.settings.pixelScale, focus.eyeW, focus.eyeH,
         static_cast<int>(focus.resourceFormat), static_cast<int>(focus.viewFormat), focus.samples);
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
    if (focus.pool.size() >= 64)
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

// At the game's Close of a stereo depth list: our replay of it, recorded now, executed after the game's batch.
void focus_record(List* gameList, const Stream& s) {
    Replay* r = nullptr;
    AcquireSRWLockExclusive(&focus.lock);
    if (focus_targets(s))
        r = acquire_replay();
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
    const float params[4] = {static_cast<float>(focus.settings.boxFraction), static_cast<float>(focus.settings.boxFraction),
                             static_cast<float>(focus.fw), static_cast<float>(focus.fh)};
    l->SetComputeRoot32BitConstants(1, 4, params, 0);
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
    // 2. Both focus views into our focus target.
    const float fw = static_cast<float>(focus.fw), fh = static_cast<float>(focus.fh);
    const D3D12_VIEWPORT focusViews[2] = {{0, 0, fw, fh, 0, 1}, {fw, 0, fw, fh, 0, 1}};
    const D3D12_RECT focusRects[2] = {{0, 0, static_cast<LONG>(focus.fw), static_cast<LONG>(focus.fh)},
                                      {static_cast<LONG>(focus.fw), 0, static_cast<LONG>(2 * focus.fw), static_cast<LONG>(focus.fh)}};
    const Substitution sub{focusViews, focusRects, 2, &s.passConstants, &slots};
    const D3D12_CPU_DESCRIPTOR_HANDLE focusDsv = dsv_handle(0);
    l->OMSetRenderTargets(0, nullptr, FALSE, &focusDsv);
    l->RSSetViewports(2, focusViews);  // in case the list sets none itself
    l->RSSetScissorRects(2, focusRects);
    int64_t draws = emit(l, s, &sub);
    // 3. At sample frames, the same list unchanged into the eye reference.
    if (sample && draws >= 0) {
        const float ew = static_cast<float>(focus.eyeW), eh = static_cast<float>(focus.eyeH);
        const D3D12_VIEWPORT eyeViews[2] = {{0, 0, ew, eh, 0, 1}, {ew, 0, ew, eh, 0, 1}};
        const D3D12_RECT eyeRects[2] = {{0, 0, static_cast<LONG>(focus.eyeW), static_cast<LONG>(focus.eyeH)},
                                        {static_cast<LONG>(focus.eyeW), 0, static_cast<LONG>(2 * focus.eyeW), static_cast<LONG>(focus.eyeH)}};
        const Substitution eyeSub{eyeViews, eyeRects, 2, nullptr, nullptr};
        const D3D12_CPU_DESCRIPTOR_HANDLE eyeDsv = dsv_handle(1);
        l->OMSetRenderTargets(0, nullptr, FALSE, &eyeDsv);
        l->RSSetViewports(2, eyeViews);
        l->RSSetScissorRects(2, eyeRects);
        emit(l, s, &eyeSub);
    }
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->Close();
    inReplay = false;
    replayTicks.fetch_add(static_cast<uint64_t>(now_ticks() - start), std::memory_order_relaxed);
    if (draws < 0) {
        AcquireSRWLockExclusive(&focus.lock);
        r->busy = false;
        ReleaseSRWLockExclusive(&focus.lock);
        return;
    }
    listsReplayed.fetch_add(1, std::memory_order_relaxed);
    callsReplayed.fetch_add(s.calls, std::memory_order_relaxed);
    drawsReplayed.fetch_add(static_cast<uint64_t>(draws), std::memory_order_relaxed);
    AcquireSRWLockExclusive(&focus.lock);
    focus.pending[gameList] = r;
    ReleaseSRWLockExclusive(&focus.lock);
}

uint8_t depth_gray(float d, float scale) {
    const float v = d <= 0 ? 0.0f : std::sqrt(std::min(1.0f, d * scale));
    return static_cast<uint8_t>(std::lround(v * 255.0f));
}

std::vector<float> read_depth(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f) {
    std::vector<float> out(size_t{f.Footprint.Width} * f.Footprint.Height);
    void* p = nullptr;
    if (FAILED(readback->Map(0, nullptr, &p)))
        return {};
    for (UINT y = 0; y < f.Footprint.Height; ++y)
        std::memcpy(&out[size_t{y} * f.Footprint.Width], static_cast<const uint8_t*>(p) + f.Offset + size_t{y} * f.Footprint.RowPitch,
                    size_t{f.Footprint.Width} * 4);
    readback->Unmap(0, nullptr);
    return out;
}

void save_gray(const std::wstring& path, const std::vector<float>& depth, UINT w, UINT h, float scale) {
    std::vector<uint8_t> bgra(size_t{w} * h * 4);
    for (size_t i = 0; i < depth.size(); ++i) {
        const uint8_t g = depth_gray(depth[i], scale);
        bgra[i * 4 + 0] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = g;
        bgra[i * 4 + 3] = 255;
    }
    write_png_bgra(path, bgra, w, h);
}

// Off the game's threads: waits for the copy, saves both depths and compares the focus views with the eye reference.
void focus_save(uint64_t fenceValue, uint64_t frame) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    focus.fence->SetEventOnCompletion(fenceValue, event);
    WaitForSingleObject(event, 10000);
    CloseHandle(event);
    const std::vector<float> fd = read_depth(focus.readFocus.Get(), focus.footFocus);
    const std::vector<float> ed = read_depth(focus.readEye.Get(), focus.footEye);
    float rows[16] = {};
    void* p = nullptr;
    if (SUCCEEDED(focus.readConstants->Map(0, nullptr, &p))) {
        std::memcpy(rows, static_cast<const uint8_t*>(p) + 128 * 16, sizeof(rows));
        focus.readConstants->Unmap(0, nullptr);
    }
    if (fd.empty() || ed.empty()) {
        log_line("replay focus: sample readback failed");
        focus.saving = false;
        return;
    }
    // Scale for the pictures: the 99th percentile of the eye reference's depth.
    std::vector<float> sorted;
    for (size_t i = 0; i < ed.size(); i += 7)
        if (ed[i] > 0) sorted.push_back(ed[i]);
    std::sort(sorted.begin(), sorted.end());
    const float top = sorted.empty() ? 1.0f : sorted[sorted.size() * 99 / 100];
    const float scale = top > 0 ? 1.0f / top : 1.0f;
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"replay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wchar_t name[64];
    std::swprintf(name, 64, L"frame-%llu-focus-depth.png", static_cast<unsigned long long>(frame));
    save_gray((dir / name).wstring(), fd, focus.fw * 2, focus.fh, scale);
    std::swprintf(name, 64, L"frame-%llu-eye-depth.png", static_cast<unsigned long long>(frame));
    save_gray((dir / name).wstring(), ed, focus.eyeW * 2, focus.eyeH, scale);
    // Compare: each focus pixel maps to eye NDC = centre + focus NDC * half extent (centre = -ox, -oy).
    const double hw = focus.settings.boxFraction;
    const UINT fw = focus.fw, fh = focus.fh, ew = focus.eyeW, eh = focus.eyeH;
    const size_t fStride = size_t{fw} * 2, eStride = size_t{ew} * 2;
    ReplayFocusCompare best;
    double bestShare = -1;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            uint64_t compared = 0, agree = 0;
            for (int eye = 0; eye < 2; ++eye) {
                const double cx = -rows[eye * 8 + 2], cy = -rows[eye * 8 + 6];  // rows 128/129 left, 130/131 right
                for (UINT v = 2; v + 2 < fh; v += 2)
                    for (UINT u = 2; u + 2 < fw; u += 2) {
                        const double nx = (u + 0.5) / fw * 2 - 1, ny = 1 - (v + 0.5) / fh * 2;
                        const double ex = cx + nx * hw, ey = cy + ny * hw;
                        // Bilinear in the eye reference (depth is linear in screen space on a plane), shifted.
                        const double fx = (ex + 1) * 0.5 * ew - 0.5 + dx, fy = (1 - ey) * 0.5 * eh - 0.5 + dy;
                        const long px = static_cast<long>(std::floor(fx)), py = static_cast<long>(std::floor(fy));
                        if (px < 1 || py < 1 || px + 2 >= static_cast<long>(ew) || py + 2 >= static_cast<long>(eh))
                            continue;
                        const auto at = [&](long x, long y) {
                            return ed[static_cast<size_t>(y) * eStride + static_cast<size_t>(eye) * ew + static_cast<size_t>(x)];
                        };
                        const float b00 = at(px, py), b10 = at(px + 1, py), b01 = at(px, py + 1), b11 = at(px + 1, py + 1);
                        const float a = fd[size_t{v} * fStride + static_cast<size_t>(eye) * fw + u];
                        const bool skyA = a <= 0, skyB = b00 <= 0 && b10 <= 0 && b01 <= 0 && b11 <= 0;
                        if (skyA && skyB)
                            continue;  // sky in both
                        if ((b00 <= 0) != (b11 <= 0) || (b10 <= 0) != (b01 <= 0))
                            continue;  // an edge against the sky: no bilinear value
                        const double tx = fx - px, ty = fy - py;
                        const float b = static_cast<float>((b00 * (1 - tx) + b10 * tx) * (1 - ty) + (b01 * (1 - tx) + b11 * tx) * ty);
                        ++compared;
                        if (std::fabs(a - b) <= 0.01f * std::max(a, b))
                            ++agree;
                    }
            }
            const double share = compared ? static_cast<double>(agree) / static_cast<double>(compared) : 0;
            if (share > bestShare) {
                bestShare = share;
                best.pixels = compared;
                best.agree = share;
                best.shiftX = dx;
                best.shiftY = dy;
            }
        }
    AcquireSRWLockExclusive(&focus.compareLock);
    best.sample = focus.compare.sample + 1;
    focus.compare = best;
    ReleaseSRWLockExclusive(&focus.compareLock);
    logf("replay focus: sample frame %llu: %llu pixels compared, %.1f%% agree within 1%% of depth at the best shift (%d, %d) "
         "eye pixels; eye centres (%.4f, %.4f) and (%.4f, %.4f); images in logs\\replay",
         static_cast<unsigned long long>(frame), static_cast<unsigned long long>(best.pixels), best.agree * 100, best.shiftX,
         best.shiftY, -rows[2], -rows[6], -rows[10], -rows[14]);
    focus.saving = false;
}

// On the submit thread, before the next frame's clear: copy both depths and the sample's constants to readback.
void focus_copy_sample() {
    if (focus.samples != 1) {
        log_line("replay focus: multisampled depth; samples are not saved (run with MSAA off)");
        focus.copyPending = false;
        return;
    }
    if (!focus.copyList) {
        D3D12_RESOURCE_DESC fdesc = focus.focusDepth->GetDesc(), edesc = focus.eyeDepth->GetDesc();
        UINT64 fbytes = 0, ebytes = 0;
        device->GetCopyableFootprints(&fdesc, 0, 1, 0, &focus.footFocus, nullptr, nullptr, &fbytes);
        device->GetCopyableFootprints(&edesc, 0, 1, 0, &focus.footEye, nullptr, nullptr, &ebytes);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        const auto buffer = [&](UINT64 bytes, ComPtr<ID3D12Resource>& out) {
            D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                             nullptr, IID_PPV_ARGS(&out)));
        };
        if (!buffer(fbytes, focus.readFocus) || !buffer(ebytes, focus.readEye) || !buffer(kSlotBytes, focus.readConstants) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&focus.copyAllocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, focus.copyAllocator.Get(), nullptr,
                                             IID_PPV_ARGS(&focus.copyList)))) {
            log_line("replay focus: readback resources failed; no samples");
            focus.copyPending = false;
            focus.samplesTaken = focus.settings.saveCount;
            return;
        }
        focus.copyList->Close();
    }
    const uint32_t slot = focus.sampleSlot.load();
    inReplay = true;
    focus.copyAllocator->Reset();
    List* l = focus.copyList.Get();
    l->Reset(focus.copyAllocator.Get(), nullptr);
    barrier(l, focus.focusDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(l, focus.eyeDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{focus.readFocus.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = focus.footFocus;
    D3D12_TEXTURE_COPY_LOCATION src{focus.focusDepth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    dst.pResource = focus.readEye.Get();
    dst.PlacedFootprint = focus.footEye;
    src.pResource = focus.eyeDepth.Get();
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (slot != UINT32_MAX)
        l->CopyBufferRegion(focus.readConstants.Get(), 0, focus.constants.Get(), UINT64{slot} * kSlotBytes, kSlotBytes);
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(l, focus.eyeDepth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    barrier(l, focus.focusDepth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    l->Close();
    inReplay = false;
    ID3D12CommandList* lists[] = {l};
    executeOriginal(focus.queue.Get(), 1, lists);
    const uint64_t value = focus.fenceValue.fetch_add(1) + 1;
    focus.queue->Signal(focus.fence.Get(), value);
    focus.copyPending = false;
    focus.saving = true;
    std::thread(focus_save, value, focus.copyFrame.load()).detach();
}

void STDMETHODCALLTYPE executeHook(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists) {
    executeOriginal(queue, n, lists);
    if (probeMode != 3 || queue != focus.queue.Get() || !focus.ready)
        return;
    std::vector<Replay*> mine;
    AcquireSRWLockExclusive(&focus.lock);
    for (UINT i = 0; i < n; ++i) {
        // ID3D12GraphicsCommandList derives singly from ID3D12CommandList: the same object address.
        const auto it = focus.pending.find(reinterpret_cast<List*>(lists[i]));
        if (it != focus.pending.end()) {
            mine.push_back(it->second);
            focus.pending.erase(it);
        }
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (mine.empty())
        return;
    const uint64_t frame = focus.frame.load();
    if (focus.clearedFrame.load() != frame) {
        AcquireSRWLockExclusive(&focus.lock);  // the targets and clear list may be made again on a worker thread
        if (focus.ready) {
            if (focus.copyPending.load() && !focus.saving.load())
                focus_copy_sample();  // the sample frame's replays are all done: copy before this frame's clear
            ID3D12CommandList* clear[] = {focus.clearList.Get()};
            executeOriginal(queue, 1, clear);
        }
        ReleaseSRWLockExclusive(&focus.lock);
        focus.clearedFrame = frame;
    }
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

void STDMETHODCALLTYPE dsvHook(ID3D12Device* d, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                               D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    dsvOriginal(d, resource, desc, handle);
    if (!resource || inReplay)
        return;
    AcquireSRWLockExclusive(&focus.lock);
    focus.dsvs[handle.ptr] = {resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN};
    ReleaseSRWLockExclusive(&focus.lock);
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
        if (SUCCEEDED(hr) && s->focus_depth()) {
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
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kPso); s->put(r); }
    psoOriginal(l, r);
}
void STDMETHODCALLTYPE heapsHook(List* l, UINT n, ID3D12DescriptorHeap* const* heaps) {
    if (Stream* s = stream_of(l); s && n <= 2) {
        HookTimer t(s);
        s->op(kHeaps);
        s->put(n);
        for (UINT i = 0; i < n; ++i) s->put(heaps[i]);
    }
    heapsOriginal(l, n, heaps);
}
void STDMETHODCALLTYPE tableHook(List* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kTable); s->put(i); s->put(h); }
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
            v[0].Height == v[1].Height && v[1].TopLeftX == v[0].Width && v[0].Width >= 256) {
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
    if (Stream* s = stream_of(l)) {  // not re-emitted: a replay binds its own target
        if (n > 0) s->sawRtv = true;
        if (dsv) {
            s->sawDsv = true;
            s->dsv = *dsv;
        }
    }
    omOriginal(l, n, rtvs, single, dsv);
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
        focus.settings = settings;
        focus.queue = queue;
        ok = hook(*reinterpret_cast<void***>(d), simqvfr_slot_CreateDepthStencilView(), &dsvHook, &dsvOriginal) &&
             hook(*reinterpret_cast<void***>(queue), simqvfr_slot_ExecuteCommandLists(), &executeHook, &executeOriginal) &&
             focus_compile();
        focus.sampleFrame = settings.saveCount ? settings.saveFirst : UINT64_MAX;
    }
    installed = ok;
    if (mode == 3)
        logf("replay probe: %s (mode 3: the stereo depth pass replayed for both focus views into a target of ours, executed "
             "after the game's batch; box %.0f%%, %.2f focus pixels per eye pixel; samples from frame %llu every %llu, %u "
             "times)",
             ok ? "installed" : "FAILED", settings.boxFraction * 100, settings.pixelScale,
             static_cast<unsigned long long>(settings.saveFirst), static_cast<unsigned long long>(settings.saveEvery),
             settings.saveCount);
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
        if (ended == focus.sampleFrame.load() && focus.ready && focus.samples == 1) {
            focus.copyFrame = ended;
            focus.copyPending = true;  // copied at the next frame's first replay, before its clear
            const uint32_t taken = focus.samplesTaken.fetch_add(1) + 1;
            if (taken < focus.settings.saveCount)
                focus.sampleFrame = ended + focus.settings.saveEvery;
        } else if (ended == focus.sampleFrame.load()) {
            // Nothing replayed yet, or multisampled depth (not copyable to a buffer): postponed, not counted.
            if (focus.ready && focus.samples != 1)
                log_line("replay focus: the game's depth is multisampled; the sample waits for MSAA off");
            focus.sampleFrame = ended + focus.settings.saveEvery;
        }
    }
    if (++windowFrames < 900)
        return;
    windowFrames = 0;
    const bool wasOn = replayOn.load();
    const ReplayProbeStats s = replay_probe_take_stats();
    const double n = s.frames ? static_cast<double>(s.frames) : 1.0;
    if (probeMode == 3) {
        logf("replay focus: window %llu, per frame: stereo depth lists replayed %.2f (executed %.2f), draws %.0f; recording "
             "the replays %.3f ms, recording hooks %.3f ms, the game's own recording of those lists %.3f ms (CPU)",
             static_cast<unsigned long long>(window), s.listsReplayed / n, s.focusReplays / n, s.drawsReplayed / n,
             s.replayMs / n, s.hookMs / n, s.gameMs / n);
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

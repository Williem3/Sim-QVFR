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

#include "../log.h"
#include "../xr.h"
#include "d3d11_capture.h"  // write_png_bgra
#include "focus_ngx.h"

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
using CbvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using GfxPsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using StreamPsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
CbvFn cbvViewOriginal;
GfxPsoFn gfxPsoOriginal;
StreamPsoFn streamPsoOriginal;
using LibGfxFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                            REFIID, void**);
using LibStreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*,
                                               REFIID, void**);
LibGfxFn libGfxOriginal;
LibStreamFn libStreamOriginal;
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
    D3D12_GPU_DESCRIPTOR_HANDLE tables[10] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE bindlessUint{};  // root table 18 (space 45 in ACE's scene root signature)
    D3D12_GPU_VIRTUAL_ADDRESS computeCbvs[8] = {};
    // R2c: graphics state for ACE's deferredShadows draws (root tables / CBVs 0-7 as last set)
    ID3D12RootSignature* gfxRoot = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gfxTables[8] = {};
    D3D12_GPU_VIRTUAL_ADDRESS gfxCbvs[8] = {};
    struct ShadowDraw {
        ID3D12PipelineState* pso;
        ID3D12RootSignature* root;
        D3D12_GPU_DESCRIPTOR_HANDLE tables[8];
        D3D12_GPU_VIRTUAL_ADDRESS cbv;  // root 5: the eye's 560-B constants
    };
    std::vector<ShadowDraw> shadowDraws;
    // R2c: a list with one eye-size viewport (ACE's per-eye transparents), and the OIT composite's tables
    bool monoSeen = false, monoMixed = false;
    float monoX = 0, monoW = 0, monoH = 0;
    bool oitDraw = false;
    D3D12_GPU_DESCRIPTOR_HANDLE oitTables[4] = {};
    // P1: graphics root constants (root 0) and the first render target as last set; the post draws
    uint32_t gfxConsts[8] = {};
    SIZE_T lastRtv = 0;
    struct PostDraw {
        ID3D12PipelineState* pso;
        ID3D12RootSignature* root;
        D3D12_GPU_DESCRIPTOR_HANDLE tables[8];
        D3D12_GPU_VIRTUAL_ADDRESS cbv;  // root 6 (tonemap's constants)
        uint32_t consts[8];             // root 0 (fxaa's)
        SIZE_T rtv;
    };
    PostDraw tonemapDraw{}, fxaaDraw{};
    bool skyBound = false;              // the bound PSO is the sky's
    // ACE's tyres under single-pass stereo: the bound PSO is the tyres', the viewports and scissors as last set, our
    // right-eye constants per root-1 address (this list's)
    bool tyreBound = false;
    UINT vpCount = 0, scCount = 0;
    D3D12_VIEWPORT vp[2] = {};
    D3D12_RECT sc[2] = {};
    std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS>> tyreConstants;
    std::vector<UINT64> skyTables;      // root-4 tables (b3..b12, cbSky at b4) the sky draws used
    bool hasTonemap = false, hasFxaa = false;
    struct TileDispatch {
        int kind;
        ID3D12PipelineState* pso;
        ID3D12RootSignature* root;
        ID3D12DescriptorHeap* heap;
        uint32_t constants[20];
        D3D12_GPU_VIRTUAL_ADDRESS cbv;
        D3D12_GPU_DESCRIPTOR_HANDLE tables[10];
        UINT dims[3];
        D3D12_GPU_VIRTUAL_ADDRESS cbv2;  // root 2 (normalReconstructionViewSpace's constants)
        D3D12_GPU_VIRTUAL_ADDRESS cbv6;  // root 6 (vrAntialiasing's constants)
        D3D12_GPU_VIRTUAL_ADDRESS cbv1;  // root 1 (atmosphericScattering's scatteringSettings)
    };
    std::vector<TileDispatch> tileDispatches;
    std::vector<ID3D12PipelineState*> dispatchPsos;  // every compute dispatch's pipeline (diagnostics)

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
        dispatchPsos.clear();
        std::memset(computeCbvs, 0, sizeof(computeCbvs));
        gfxRoot = nullptr;
        std::memset(gfxTables, 0, sizeof(gfxTables));
        std::memset(gfxCbvs, 0, sizeof(gfxCbvs));
        shadowDraws.clear();
        monoSeen = monoMixed = oitDraw = false;
        std::memset(gfxConsts, 0, sizeof(gfxConsts));
        lastRtv = 0;
        hasTonemap = hasFxaa = false;
        skyBound = false;
        skyTables.clear();
        tyreBound = false;
        vpCount = scCount = 0;
        tyreConstants.clear();
        monoX = monoW = monoH = 0;
        std::memset(oitTables, 0, sizeof(oitTables));
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
    const std::vector<std::pair<UINT64, UINT64>>* tables = nullptr;  // descriptor tables (GPU handles) -> ours
    const std::vector<std::pair<UINT64, UINT64>>* skyTables = nullptr;  // root 4 -> ours, only around sky draws
};

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
        const UINT64 skyMine = (op == kDraw || op == kDrawIndexed || op == kIndirect) ? skyTable() : 0;
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
            if (sub && sub->from && i == kPassConstantsRoot)
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
            if (curTyre && b == 1 && curCbv1)
                tyre_right_draw(l, tyreConstants, curCbv1, curPso, sub->viewports, 2, sub->scissors, 2, a, c, d, e);
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
        if (skyMine)  // the game's table back for the draws after
            l->SetGraphicsRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{curTable4});
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
cbuffer Params : register(b1) { float2 halfExtent; float2 focusSize; uint4 from0; uint4 from1; uint4 to0; uint4 to1; uint fogSrv; uint shadowSrv; uint4 atmo0; uint4 atmo1; uint blurSrv; uint fogSrv1; float4 gaze; float4 gazePrev; };
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
    // The box's centre off the eye's forward axis (the gaze; NDC, per eye: gaze.xy left, .zw right; a right-eye list
    // holds its eye in the left rows). The previous frame's projections take the previous frame's centre, so velocities
    // and reprojection stay right while the box moves.
    const bool rightEye = !left || src[84].x > 0;
    const bool prevRow = (i >= 21 && i <= 28) || (i >= 107 && i <= 110);
    const float2 g = prevRow ? (rightEye ? gazePrev.zw : gazePrev.xy) : (rightEye ? gaze.zw : gaze.xy);
    const float ox = (left ? src[17].z : src[103].z) - g.x;  // projectionUnjittered: each eye's own centre
    const float oy = (left ? src[18].z : src[104].z) - g.y;
    const float ax = 1.0 / halfExtent.x, ay = 1.0 / halfExtent.y;
    if (is_in(i, 1, 13, 17, 21) || is_in(i, 25, 95, 103, 107))  // viewProjection and the projections
        v = ax * v + (ax * ox) * src[i + 3];          // x' = a x + b w, b = -a * centre = a * ox
    else if (is_in(i, 2, 14, 18, 22) || is_in(i, 26, 96, 104, 108))
        v = ay * v + (ay * oy) * src[i + 2];          // y' = c y + d w
    if (i == 38) v.xy = float2(focusSize.x * 2, focusSize.y);
    if (i == 84) v = float4(v.x > 0 ? focusSize.x : 0, 0, focusSize.x, focusSize.y);  // a right-eye list: its offset
    if (i >= 29 && i <= 32)  // inverseViewProjection: inverse(R VP) = inverse(VP) inverse(R)
        v = float4(v.x * halfExtent.x, v.y * halfExtent.y, v.z, v.w - v.x * ox - v.y * oy);
    if (i == 112 && v.z != 0) v.z = focusSize.x;  // eyeHalfWidthPixels (a float, as ACE writes it; 0 in per-eye lists)
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
    if (i == 76 && fogSrv != 0) v.w = asfloat(src[84].x > 0 && fogSrv1 != 0 ? fogSrv1 : fogSrv);  // right-eye list: grid 1
    // deferredShadowMapIndex (76.x): ours, from the focus depth (R2c).
    if (i == 76 && shadowSrv != 0) v.x = asfloat(shadowSrv);
    // refractionMapIndex (76.z): our opaqueSceneBlur (P1).
    if (i == 76 && blurSrv != 0) v.z = asfloat(blurSrv);
    // The atmosphere maps (73.y in-scatter, 73.z out-scatter, 75.x / 75.y sky): our focus copies, the right eye's set
    // for a right-eye list (its own offset, row 84.x > 0), eye 0's otherwise (ACE's stereo lists read _0 for both).
    if (atmo0.x != 0 && (i == 73 || i == 75)) {
        const uint4 a = src[84].x > 0 ? atmo1 : atmo0;
        if (i == 73) { v.y = asfloat(a.x); v.z = asfloat(a.y); }
        else if (a.z != 0) { v.x = asfloat(a.z); v.y = asfloat(a.w); }  // sky maps: 0 = keep ACE's
    }
    dst.Store4(i * 16, asuint(v));
}
)";

// kPatchShader's Params as 32-bit values, at HLSL's packing (a vector never straddles a 16-byte register): 0-3 the
// floats, 4-11 from, 12-19 to, 20 fogSrv, 21 shadowSrv, (22-23 padding), 24-27 atmo0, 28-31 atmo1, 32 blurSrv, 33
// fogSrv1, (34-35 padding), 36-39 gaze, 40-43 gazePrev.
constexpr UINT kPatchParams = 44;
// ACE's tyre shaders have no single-pass-stereo variant (the vertex shader takes no instance id and writes no viewport
// index; 16:43 capture): the pass constants copied with the right eye's matrices in the left eye's rows, for a second
// draw into the right eye's viewport. The tyre VS reads view (5-8), prevView (9-12), projection (13-16) and
// prevProjection (21-24); the right eye's are viewRight (91-94), prevViewRight (99-102), projectionRight (95-98) and
// prevProjectionUnjitteredRight (107-110; the previous jitter = the left eye's 21-24 minus its unjittered 25-28). The
// pixel shader takes its eye from the pixel position, as in ACE's stereo draws: the rest stays.
constexpr char kTyreRightShader[] = R"(
cbuffer Src : register(b0) { float4 src[113]; };
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 113) return;
    float4 v = src[i];
    if (i >= 5 && i <= 8) v = src[i + 86];
    else if (i >= 9 && i <= 12) v = src[i + 90];
    else if (i >= 13 && i <= 16) v = src[i + 82];
    else if (i >= 17 && i <= 20) v = src[i + 86];
    else if (i >= 21 && i <= 24) v = src[i + 86] + (src[i] - src[i + 4]);
    else if (i >= 25 && i <= 28) v = src[i + 82];
    dst.Store4(i * 16, asuint(v));
}
)";
constexpr UINT kTyreSlots = 64, kTyreSlotBytes = 2048;

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
    bool fogResampleViews = false;
    // The fog shaders' UAVs (space 20) are in another table: its base (ACE's 475,136) and our UAVs at its range's top
    // (kFogUavSlots, offset = our bindless fog slot - kFogFirst), guarded like the bindless block.
    UINT uavBaseIndex = 0, fogUavFirst = 0;
    int fogParity = 0;
    std::atomic<uint64_t> fogReplays{0};
    ComPtr<ID3D12Resource> gameGrid[2];  // ACE's voxelGrid0 / 1 (replay_fog=2: resampled into ours)
    // R2c: the focus views' deferred sun shadow: our normals (UAV slot 28, SRV 14) and shadow (SRV 15, an RTV of ours).
    ComPtr<ID3D12RootSignature> shadowPatchRoot;
    ComPtr<ID3D12PipelineState> shadowPatchPso;
    ComPtr<ID3D12Resource> shadowConstants;
    uint32_t shadowSlot = 0;
    ComPtr<ID3D12Resource> normals, shadow;
    UINT shadowFw = 0, shadowFh = 0;
    int shadowRtvSlot = -1;
    D3D12_CPU_DESCRIPTOR_HANDLE shadowRtv{};
    bool shadowBroken = false, shadowReady = false;
    std::atomic<uint64_t> shadowReplays{0};
    // R2c: SRVs the game made of its render targets (CPU handle -> resource, view), for the OIT composite's tables
    std::unordered_map<SIZE_T, std::pair<ComPtr<ID3D12Resource>, D3D12_SHADER_RESOURCE_VIEW_DESC>> srvInfo;
    std::atomic<uint64_t> monoReplays{0}, oitReplays{0};
    // per frame (logged at sample frames): per-eye lists taken by eye, replayed by eye, unfit, broken (draws < 0)
    std::atomic<uint32_t> monoTaken[2]{}, monoDone[2]{}, monoUnfit{0}, monoBroken{0}, monoOit{0}, monoMixed{0}, monoInStereo[2]{},
        monoNoConst{0}, monoNoTarget{0};
    std::atomic<float> sceneEyeW{0}, sceneEyeH{0};  // the stereo scene lists' eye size: per-eye lists must match it
    // R2c: the atmosphere maps' focus copies (m = eye * 4 + map), the eyes' NDC centres (from the tile passes)
    ComPtr<ID3D12RootSignature> atmoRoot;
    ComPtr<ID3D12PipelineState> atmoPso[2];  // 2D, 3D
    ComPtr<ID3D12Resource> atmo[8];
    ID3D12Resource* atmoGame[8] = {};
    float eyeCentre[2][2] = {};
    // The focus box's centre off each eye's forward axis, in the eye's NDC (the gaze): asked for by the layer
    // (replay_probe_set_gaze), taken at the frame's end for the next frame's lists (every pass of a frame uses one
    // centre), clamped so the box stays inside the eye's image; last frame's for the previous-frame projections.
    float gazeWanted[2][2] = {}, gazeOffset[2][2] = {}, gazePrev[2][2] = {};
    // The focus views' size as a share of the eye's (box x pixels per eye pixel), fixed at the start: the box's size
    // can change while running (replay_probe_set_box, taken at a frame's end), the focus pixels stay, so the density
    // follows. 0: no change asked.
    double sizeK = 0, sizeKy = 0;
    std::atomic<double> boxYWanted{0};
    std::atomic<double> boxWanted{0};
    // replay_atmosphere=4: ACE's atmosphericScattering + upscaling replayed per focus eye (ray grid narrowed to the box):
    // our low-res sky maps and the upscaling intermediates, per eye [in low, out low, in intermediate, out
    // intermediate] (SRV at slot 112 + 8 e + 2 k, UAV + 1); our grid constants.
    ComPtr<ID3D12Resource> atmoSkyWork[2][4];
    ComPtr<ID3D12PipelineState> atmoGridPso;
    ComPtr<ID3D12Resource> atmoGridConstants;
    UINT atmoGridSlot = 0;
    bool atmoWorkMade = false, atmoReplayLogged = false;
    bool centresKnown = false, atmoMade = false, atmoReady = false, atmoBroken = false, atmoMissLogged = false;
    std::atomic<uint64_t> atmoReplays{0};
    // P1: the focus post
    ComPtr<ID3D12RootSignature> cbPatchRoot;
    ComPtr<ID3D12PipelineState> cbPatchPso;
    ComPtr<ID3D12Resource> postConstants, postVeil, postLdr, postFinal, postGameFxaa;
    DXGI_FORMAT postGameFxaaFormat = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* postColour = nullptr;
    UINT postW = 0, postH = 0, postSlot = 0;
    int postLdrRtvSlot = -1, postFinalRtvSlot = -1;
    bool postFxaa = true;  // the post targets were made for a list with ACE's fxaa draw (false: tonemap only, its menus)
    // replay_dlss=2: the same frame's tonemap also from our colour (no DLSS) and from DLSS without CAS, compared too
    ComPtr<ID3D12Resource> postAlt[2];
    int postAltRtvSlot[2] = {-1, -1};
    D3D12_CPU_DESCRIPTOR_HANDLE postAltRtv[2]{};
    DXGI_FORMAT postLdrRtvFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_CPU_DESCRIPTOR_HANDLE postLdrRtv{}, postFinalRtv{};
    bool postBroken = false, postReady = false;
    std::atomic<uint64_t> postFrame{UINT64_MAX};  // the frame whose focus post replay was recorded last (focus.frame)
    std::atomic<uint64_t> postReplays{0};
    // P2a: our DLSS per focus view: ACE's velocity packing replayed (our four outputs, UAV offsets 24-27; our
    // velocity / flags twin SRVs at slots 69 / 70), our NGX features (one per eye), our DLSS image (SRV slot 71)
    ComPtr<ID3D12RootSignature> packRoot;
    ComPtr<ID3D12PipelineState> packPso;
    ComPtr<ID3D12Resource> packConstants, packAa, packMb, packVelocity, packFlags, dlssOut, dlssOutAuto, casOut;
    bool casDone = false;
    // P2c (MSAA without DLSS): ACE's vrAntialiasing replayed per focus eye: our multisampled colour twin (SRV slot 76),
    // our scene depth (slot 12), our history (slot 77) -> aaOut (UAV block 32, SRV slot 78), copied into aaHistory.
    ComPtr<ID3D12RootSignature> aaPatchRoot;
    ComPtr<ID3D12PipelineState> aaPatchPso;
    ComPtr<ID3D12Resource> aaConstants, aaOut, aaHistory;
    ID3D12Resource* aaSource = nullptr;
    UINT aaSlot = 0;
    bool aaBroken = false, aaReady = false, aaFresh = true;
    std::atomic<uint64_t> aaReplays{0};
    // MSAA: single-sample focus copies made from our multisampled twins: depth and velocity by ACE's own resolve passes
    // replayed (SRVs of the twins at slots 74/75, our velocity's SRV 76; UAVs at block offsets 29-31), colour by a
    // ResolveSubresource before the post. Each has a stand-in Twin (not in `twins`) for the code that takes one.
    ComPtr<ID3D12Resource> resolvedDepth, closestDepth, resolvedVelocity, resolvedColour;
    Twin resolvedDepthTwin, resolvedVelocityTwin, resolvedColourTwin;
    ID3D12Resource *resolveDepthSource = nullptr, *resolveVelocitySource = nullptr;
    bool resolveBroken = false, resolveReady = false;
    std::atomic<uint64_t> resolveReplays{0};  // this frame's DLSS image sharpened (ACE's CAS replayed: our casOut, SRV slot 73)
    ID3D12Resource *packColour = nullptr, *packVelocityTwin = nullptr, *packFlagsTwin = nullptr;
    UINT packW = 0, packH = 0, packSlot = 0;
    focus_ngx::Feature* ngx[2] = {};
    focus_ngx::Feature* ngxAuto[2] = {};  // replay_dlss=2: DLSS's own auto-exposure, no exposure texture (SRV slot 72)
    int dlssPhase = -1;
    UINT ngxW = 0, ngxH = 0;
    uint64_t ngxUsed[2] = {};
    bool dlssBroken = false, dlssReady = false, dlssFresh = true;
    std::atomic<uint64_t> dlssEvaluations{0};
    // P1: the focus refraction source
    ComPtr<ID3D12Resource> blur;
    ID3D12Resource* blurColour = nullptr;
    UINT blurMips = 0;
    bool blurBroken = false, blurReady = false, blurMissLogged = false;
    std::atomic<uint64_t> blurReplays{0};
    // the focus sky (clouds through the box): its PSOs, the game's CBVs, our cbSky copies and table sets
    std::unordered_set<ID3D12PipelineState*> skyPsos;
    // ACE's tyres under single-pass stereo: its tyre pipelines, our right-eye constants (kTyreSlots x 2048 B, at rest
    // VERTEX_AND_CONSTANT_BUFFER) and the copy shader
    std::unordered_set<ID3D12PipelineState*> tyrePsos;
    struct VsInfo {
        uint32_t length;
        uint8_t digest[4];  // the container digest's first bytes
        uint8_t bits;       // 1 BONEWEIGHTS, 2 MATERIALID, 4 SV_Position, 8 SV_InstanceID, 16 SV_ViewportArrayIndex
    };
    std::unordered_map<ID3D12PipelineState*, VsInfo> vsSeen;  // every pipeline whose creation we saw (psoLock)
    ComPtr<ID3D12RootSignature> tyreRoot;
    ComPtr<ID3D12PipelineState> tyrePso;
    ComPtr<ID3D12Resource> tyreConstants;
    std::atomic<uint32_t> tyreSlot{0};
    std::atomic<bool> tyreGameLogged{false}, tyreComputeLogged{false};
    std::unordered_map<SIZE_T, D3D12_CONSTANT_BUFFER_VIEW_DESC> cbvInfo;
    SRWLOCK cbvLock = SRWLOCK_INIT;
    ComPtr<ID3D12RootSignature> skyRoot;
    ComPtr<ID3D12PipelineState> skyPso;
    ComPtr<ID3D12Resource> skyConstants, skyClouds;
    UINT skyRing = 0, skySlot = 0;
    bool skyBroken = false, skyCloudViews = false, skyMissLogged = false;
    std::atomic<uint64_t> skyPatches{0};
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

// Our descriptors at the top of the bindless range: 0-11 tile lists, 12 focus depth, 13 previous colour, 14-15 shadow,
// 16-27 fog, 28 normals UAV, 29-31 OIT views, 32-55 atmosphere (game SRVs, our SRVs, our UAVs), 56-60 post, 61-66
// refraction blur.
constexpr UINT kTileSlots = 160, kBindlessRange = 131072, kTileStride = 64;  // 16 uints a tile
// the sky: 80.. our CBV table sets (kSkyRing x kSkyTable), 110/111 our clouds' SRV / UAV
constexpr UINT kSkyTable = 10, kSkyRing = 3, kSkySlotsFirst = 80, kSkyCloudSrv = 110, kSkySlots = 16, kSkyCloudSize = 512;
constexpr UINT kFogFirst = 16, kFogUavSlots = 40;  // 0-11 fog, 24-27 packing, 28 CAS, 29-31 resolves, 32 MSAA AA
constexpr UINT kShadowSlots = 48, kShadowSlotBytes = 1024;
constexpr UINT kPostSlots = 16;  // 256 B each
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
cbuffer Params : register(b1) { float halfExtent; uint mode; uint historySrv; uint outputUav; uint inputSrv; uint fresh; float gx; float gy; float halfY; };
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
    const float ox = src[4].x * src[0].z + src[5].x * src[1].z + src[6].x * src[2].z - gx;  // less the gaze offset
    const float oy = src[4].y * src[0].z + src[5].y * src[1].z + src[6].y * src[2].z - gy;
    const float a = 1.0 / halfExtent, ay = 1.0 / halfY;
    if (i >= 4 && i <= 11) {
        v.x = a * v.x + a * ox * v.w;
        v.y = ay * v.y + ay * oy * v.w;
    }
    if (i == 16) v *= halfExtent;
    if (i == 17) v *= halfY;
    if (i == 19) v = v - ox * src[16] - oy * src[17];
    if (i == 24 && fresh != 0) v.z = asfloat(0u);
    if (i == 25) { v.z = asfloat(historySrv); v.w = asfloat(outputUav); }
    dst.Store4(i * 16, asuint(v));
}
)";

// R2c: ACE's deferred-shadow constants for the focus views. Mode 0: normalReconstructionViewSpace's (288 B): the
// inverse projections (left 4-7, right 12-15; columns in the registers) remapped like the fog's (columns 0/1 x box,
// column 3 - ox c0 - oy c1; ox = P02 = c3.x / c0.x, oy = c3.y / c1.y), bufferSize = both focus views, eyeSplitX and
// rcpEyeWidth the focus width. Mode 1: deferredShadows' (560 B) for eye `eye`: inverseViewProjection (0-3) the same
// way, with that eye's centre from the normals constants (b0).
constexpr char kShadowShader[] = R"(
cbuffer Normals : register(b0) { float4 nrm[18]; };
cbuffer Shadow : register(b1) { float4 shd[35]; };
cbuffer Params : register(b2) { float box; uint mode; uint eye; float fw; float fh; float boxY; float4 gaze; };  // gaze: value 8
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (mode == 0) {
        if (i >= 18) return;
        float4 v = nrm[i];
        if ((i >= 4 && i <= 7) || (i >= 12 && i <= 15)) {
            const uint b = i < 8 ? 4 : 12;
            const float2 g = i < 8 ? gaze.xy : gaze.zw;
            const float ox = nrm[b + 3].x / nrm[b].x - g.x, oy = nrm[b + 3].y / nrm[b + 1].y - g.y;
            if (i == b) v *= box;
            if (i == b + 1) v *= boxY;
            if (i == b + 3) v = v - ox * nrm[b] - oy * nrm[b + 1];
        }
        if (i == 16) v = float4(2 * fw, fh, 1.0 / (2 * fw), 1.0 / fh);
        if (i == 17) { v.z = fw; v.w = 1.0 / fw; }
        dst.Store4(i * 16, asuint(v));
        return;
    }
    if (i >= 35) return;
    float4 v = shd[i];
    const uint b = eye == 0 ? 4 : 12;
    const float2 g = eye == 0 ? gaze.xy : gaze.zw;
    const float ox = nrm[b + 3].x / nrm[b].x - g.x, oy = nrm[b + 3].y / nrm[b + 1].y - g.y;
    if (i == 0) v *= box;
    if (i == 1) v *= boxY;
    if (i == 3) v = v - ox * shd[0] - oy * shd[1];
    dst.Store4(i * 16, asuint(v));
}
)";

// R2c: an atmosphere map resampled for a focus view. ACE's maps are screen-shaped (looked up at (SV_Position - offset) /
// size, row 84; 3D ones with a log-distance slice in z): the focus texel at focus uv takes the eye map at the eye uv
// the remap gives (eye NDC = centre + box * focus NDC); z slices kept. Built twice: 2D (ATMO_3D 0) and 3D.
constexpr char kAtmoShader[] = R"(
cbuffer P : register(b0) { float box; float cx; float cy; uint sx; uint sy; uint sz; uint stereo; float cx1; float cy1; float mipLevel; float boxY; };
SamplerState lin : register(s0);
#if ATMO_3D
Texture3D<float4> src : register(t0);
RWTexture3D<float4> dst : register(u0);
#else
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
#endif
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= sx || id.y >= sy || id.z >= sz) return;
    float2 uf = (float2(id.xy) + 0.5) / float2(sx, sy);
    float2 c = float2(cx, cy);
    float eye = 0;
    if (stereo != 0) {  // two eyes side by side (the veil): each half through its own eye's box
        eye = uf.x >= 0.5 ? 1 : 0;
        uf.x = uf.x * 2 - eye;
        if (eye > 0) c = float2(cx1, cy1);
    }
    const float2 ndc = c + float2(box, boxY) * float2(uf.x * 2 - 1, 1 - uf.y * 2);
    float2 ue = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (stereo != 0) ue.x = (ue.x + eye) * 0.5;
#if ATMO_3D
    dst[id] = src.SampleLevel(lin, float3(ue, (id.z + 0.5) / sz), 0);
#else
    dst[id.xy] = src.SampleLevel(lin, ue, mipLevel);
#endif
}
)";

// P1: a constant buffer copied with one register replaced (the tonemap's target size).
constexpr char kCbPatchShader[] = R"(
cbuffer Src : register(b0) { float4 src[16]; };
cbuffer P : register(b1) { uint count; uint reg; uint mask; uint pad; float4 value; };  // mask 0: all of the register
RWByteAddressBuffer dst : register(u0);
[numthreads(16, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= count) return;
    float4 v = src[i];
    if (i == reg && !((mask & 16) && asint(v.x) < 0)) {  // mask 16: an index of -1 (none) stays
        if (mask == 0 || (mask & 1)) v.x = value.x;
        if (mask == 0 || (mask & 2)) v.y = value.y;
        if (mask == 0 || (mask & 4)) v.z = value.z;
        if (mask == 0 || (mask & 8)) v.w = value.w;
    }
    dst.Store4(i * 16, asuint(v));
}
)";

// P2a: ACE's VelocityAndDepthPacking constants (352 B, 22 registers) for a focus eye: inverseViewProj (0-3),
// viewProj (4-7), prevViewProj (8-11), columns in the registers: x' = a x + a ox w per column, the inverse columns 0/1 x
// box and column 3 - ox c0 - oy c1 (ox = -the eye's NDC centre); rcpRes and resolution the focus size; the jitters (pixels)
// x k; pixelOffset the focus eye's; the texture indices ours (SRVs: velocity, depth, flags; UAVs: MB, velocity only,
// flags output, AA).
constexpr char kPackShader[] = R"(
cbuffer Src : register(b0) { float4 src[22]; };
cbuffer P : register(b1) { float box; float ox; float oy; float k; float fw; float fh; float offX; uint velSrv;
                           uint depthSrv; uint flagsSrv; uint mbUav; uint velOnlyUav; uint flagsUav; uint aaUav; float pox; float poy; float boxY; float ky; };
RWByteAddressBuffer dst : register(u0);
[numthreads(32, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 22) return;
    float4 v = src[i];
    const float a = 1.0 / box, ay = 1.0 / boxY;
    if (i == 0) v *= box;
    if (i == 1) v *= boxY;
    if (i == 3) v = v - ox * src[0] - oy * src[1];
    if (i >= 4 && i <= 7) { v.x = a * v.x + a * ox * v.w; v.y = ay * v.y + ay * oy * v.w; }
    if (i >= 8 && i <= 11) { v.x = a * v.x + a * pox * v.w; v.y = ay * v.y + ay * poy * v.w; }  // last frame's box
    if (i == 12) v = float4(1.0 / fw, 1.0 / fh, v.z * k, v.w * ky);
    if (i == 13) { v.x = asfloat(velSrv); v.y = asfloat(depthSrv); }
    if (i == 14) { v.x *= k; v.y *= ky; }
    if (i == 15) { v.x = asfloat(mbUav); v.y = asfloat(uint(fw)); v.z = asfloat(uint(fh)); }
    if (i == 16) { v.y = asfloat(velOnlyUav); v.w = asfloat(flagsSrv); }
    if (i == 17) v.x = offX;
    if (i == 20) v.w = asfloat(flagsUav);
    if (i == 21) v.w = asfloat(aaUav);
    dst.Store4(i * 16, asuint(v));
}
)";

// P2c: ACE's vrAntialiasing constants (272 B, 17 registers) for a focus eye: TextureSize (0.xy) the focus target;
// inverseViewProj (3-6), viewProj (7-10), prevViewProj (11-14) remapped like the packing's (columns in the registers);
// viewportOffset / Size (16) the focus eye's.
constexpr char kAaShader[] = R"(
cbuffer Src : register(b0) { float4 src[17]; };
cbuffer P : register(b1) { float box; float ox; float oy; float fw; float fh; float offX; float pox; float poy; float boxY; };
RWByteAddressBuffer dst : register(u0);
[numthreads(32, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 17) return;
    float4 v = src[i];
    const float a = 1.0 / box, ay = 1.0 / boxY;
    if (i == 0) v.xy = float2(2 * fw, fh);
    if (i == 3) v *= box;
    if (i == 4) v *= boxY;
    if (i == 6) v = v - ox * src[3] - oy * src[4];
    if (i >= 7 && i <= 10) { v.x = a * v.x + a * ox * v.w; v.y = ay * v.y + ay * oy * v.w; }
    if (i >= 11 && i <= 14) { v.x = a * v.x + a * pox * v.w; v.y = ay * v.y + ay * poy * v.w; }  // last frame's box
    if (i == 16) v = float4(offX, 0, fw, fh);
    dst.Store4(i * 16, asuint(v));
}
)";

// replay_atmosphere=4: ACE's scatteringSettings (96 B, 6 registers) for a focus eye: the ray grid narrowed to the box.
// Per cell ray = eyeDirection + dx x + dy y over the W x H grid; the focus cell (x, y) is the eye's cell (ox + b x,
// oy + b y): eyeDirection' = eyeDirection + ox dx + oy dy, dx' = b dx, dy' = b dy (w components kept).
constexpr char kAtmoGridShader[] = R"(
cbuffer Src : register(b0) { float4 src[6]; };
cbuffer P : register(b1) { float box; float ox; float oy; float p3; float p4; float p5; };
RWByteAddressBuffer dst : register(u0);
[numthreads(8, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 6) return;
    float4 v = src[i];
    if (i == 0) v.xyz = src[0].xyz + ox * src[2].xyz + oy * src[3].xyz;
    if (i == 2) v.xyz = box * src[2].xyz;
    if (i == 3) v.xyz = p3 * src[3].xyz;  // p3: the box's height
    dst.Store4(i * 16, asuint(v));
}
)";

// The sky's screen-space clouds for a focus view: the cloud texture the sky draw's cbSky names (CloudsScreenBindless
// Index, register 4.x; read on the GPU) sampled through the box (eye uv = centre + box x focus uv) into ours.
constexpr char kSkyCloudShader[] = R"(
cbuffer Sky : register(b0) { float4 sky[5]; };
cbuffer P : register(b1) { float box; float cx; float cy; uint size; float boxY; };
Texture2D<float4> bindless[] : register(t0, space1);
RWTexture2D<float4> dst : register(u0);
SamplerState lin : register(s0);
[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
    if (id.x >= size || id.y >= size) return;
    // ACE's own rule for this lookup (the sky PS's screen-clouds sample: index < range ? index : 0; other lookups
    // fall back to 1): no clouds (clear sky) is -1, which reads descriptor 0.
    uint idx = asuint(sky[4].x);
    if (idx >= 131072) idx = 0;
    const float2 uf = (float2(id) + 0.5) / size;
    const float2 ndc = float2(cx, cy) + float2(box, boxY) * float2(uf.x * 2 - 1, 1 - uf.y * 2);
    const float2 ue = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    dst[id] = bindless[NonUniformResourceIndex(idx)].SampleLevel(lin, ue, 0);
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
    params[1].Constants = {1, 0, kPatchParams};
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
    if (focus.settings.tyres) {  // the right eye's tyres: the constants copy
        ComPtr<ID3DBlob> tyreCode, tyreErrors, tyreBlob;
        D3D12_ROOT_PARAMETER tyreParams[2] = {};
        tyreParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        tyreParams[0].Descriptor = {0, 0};
        tyreParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        tyreParams[1].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC tyreDesc{2, tyreParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_RESOURCE_DESC tyreBuffer = buffer;
        tyreBuffer.Width = UINT64{kTyreSlots} * kTyreSlotBytes;
        bool okTyre = SUCCEEDED(D3DCompile(kTyreRightShader, sizeof(kTyreRightShader) - 1, "tyre-right", nullptr, nullptr,
                                           "main", "cs_5_0", 0, 0, &tyreCode, &tyreErrors)) &&
                      SUCCEEDED(D3D12SerializeRootSignature(&tyreDesc, D3D_ROOT_SIGNATURE_VERSION_1, &tyreBlob, &tyreErrors)) &&
                      SUCCEEDED(device->CreateRootSignature(0, tyreBlob->GetBufferPointer(), tyreBlob->GetBufferSize(),
                                                            IID_PPV_ARGS(&focus.tyreRoot)));
        if (okTyre) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC tyrePso{};
            tyrePso.pRootSignature = focus.tyreRoot.Get();
            tyrePso.CS = {tyreCode->GetBufferPointer(), tyreCode->GetBufferSize()};
            okTyre = SUCCEEDED(device->CreateComputePipelineState(&tyrePso, IID_PPV_ARGS(&focus.tyrePso))) &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tyreBuffer,
                                                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr,
                                                               IID_PPV_ARGS(&focus.tyreConstants)));
        }
        if (!okTyre) {
            log_line("replay focus: tyres: the right-eye constants copy could not be made; ACE's tyres stay left-eye only");
            focus.tyrePso.Reset();
        }
    }
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
        fogParams[1].Constants = {1, 0, 9};
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
    if (focus.settings.shadows) {  // R2c: the deferred-shadow constants copy
        ComPtr<ID3DBlob> code2, errors2, blob2;
        D3D12_ROOT_PARAMETER p2[4] = {};
        p2[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p2[0].Descriptor = {0, 0};
        p2[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p2[1].Descriptor = {1, 0};
        p2[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p2[2].Constants = {2, 0, 12};
        p2[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p2[3].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d2{4, p2, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso2{};
        D3D12_HEAP_PROPERTIES heap2{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf2{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kShadowSlots} * kShadowSlotBytes, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        if (FAILED(D3DCompile(kShadowShader, sizeof(kShadowShader) - 1, "focus-shadow", nullptr, nullptr, "main", "cs_5_0", 0,
                              0, &code2, &errors2)) ||
            FAILED(D3D12SerializeRootSignature(&d2, D3D_ROOT_SIGNATURE_VERSION_1, &blob2, &errors2)) ||
            FAILED(device->CreateRootSignature(0, blob2->GetBufferPointer(), blob2->GetBufferSize(),
                                               IID_PPV_ARGS(&focus.shadowPatchRoot)))) {
            logf("replay focus: shadow: the constants shader failed (%s); no focus shadow",
                 errors2 ? static_cast<const char*>(errors2->GetBufferPointer()) : "?");
            focus.shadowBroken = true;
        } else {
            pso2.pRootSignature = focus.shadowPatchRoot.Get();
            pso2.CS = {code2->GetBufferPointer(), code2->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&pso2, IID_PPV_ARGS(&focus.shadowPatchPso))) ||
                FAILED(device->CreateCommittedResource(&heap2, D3D12_HEAP_FLAG_NONE, &buf2,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&focus.shadowConstants))))
                focus.shadowBroken = true;
        }
    }
    if (focus.settings.atmosphere || focus.settings.post || focus.settings.transparents) {  // atmosphere, veil, blur
        ComPtr<ID3DBlob> blob3, errors3;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p3[3] = {};
        p3[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p3[0].Constants = {0, 0, 11};
        p3[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p3[1].DescriptorTable = {1, &ranges[0]};
        p3[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p3[2].DescriptorTable = {1, &ranges[1]};
        D3D12_STATIC_SAMPLER_DESC lin{};
        lin.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lin.AddressU = lin.AddressV = lin.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lin.MaxLOD = D3D12_FLOAT32_MAX;
        lin.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC d3{3, p3, 1, &lin, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        bool ok3 = SUCCEEDED(D3D12SerializeRootSignature(&d3, D3D_ROOT_SIGNATURE_VERSION_1, &blob3, &errors3)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob3->GetBufferPointer(), blob3->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.atmoRoot)));
        for (int dim = 0; dim < 2 && ok3; ++dim) {
            const D3D_SHADER_MACRO defines[] = {{"ATMO_3D", dim ? "1" : "0"}, {nullptr, nullptr}};
            ComPtr<ID3DBlob> code3;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso3{};
            ok3 = SUCCEEDED(D3DCompile(kAtmoShader, sizeof(kAtmoShader) - 1, "focus-atmosphere", defines, nullptr, "main",
                                       "cs_5_0", 0, 0, &code3, &errors3));
            if (ok3) {
                pso3.pRootSignature = focus.atmoRoot.Get();
                pso3.CS = {code3->GetBufferPointer(), code3->GetBufferSize()};
                ok3 = SUCCEEDED(device->CreateComputePipelineState(&pso3, IID_PPV_ARGS(&focus.atmoPso[dim])));
            }
        }
        if (!ok3) {
            logf("replay focus: atmosphere: the resample shader failed (%s); no focus atmosphere",
                 errors3 ? static_cast<const char*>(errors3->GetBufferPointer()) : "?");
            focus.atmoBroken = true;
        }
    }
    if (focus.settings.post || focus.settings.sky) {  // P1: the constant-buffer patch (tonemap; the sky's cbSky)
        ComPtr<ID3DBlob> code4, errors4, blob4;
        D3D12_ROOT_PARAMETER p4[3] = {};
        p4[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p4[0].Descriptor = {0, 0};
        p4[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p4[1].Constants = {1, 0, 8};
        p4[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p4[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d4{3, p4, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso4{};
        D3D12_HEAP_PROPERTIES heap4{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf4{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kPostSlots} * 256, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok4 = SUCCEEDED(D3DCompile(kCbPatchShader, sizeof(kCbPatchShader) - 1, "focus-cb-patch", nullptr, nullptr,
                                        "main", "cs_5_0", 0, 0, &code4, &errors4)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d4, D3D_ROOT_SIGNATURE_VERSION_1, &blob4, &errors4)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob4->GetBufferPointer(), blob4->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.cbPatchRoot)));
        if (ok4) {
            pso4.pRootSignature = focus.cbPatchRoot.Get();
            pso4.CS = {code4->GetBufferPointer(), code4->GetBufferSize()};
            ok4 = SUCCEEDED(device->CreateComputePipelineState(&pso4, IID_PPV_ARGS(&focus.cbPatchPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap4, D3D12_HEAP_FLAG_NONE, &buf4,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.postConstants)));
        }
        if (!ok4) {
            logf("replay focus: post: the constants patch failed (%s); no focus post",
                 errors4 ? static_cast<const char*>(errors4->GetBufferPointer()) : "?");
            focus.postBroken = true;
        }
    }
    if (focus.settings.dlss) {  // P2a: the packing constants
        ComPtr<ID3DBlob> code6, errors6, blob6;
        D3D12_ROOT_PARAMETER p6[3] = {};
        p6[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p6[0].Descriptor = {0, 0};
        p6[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p6[1].Constants = {1, 0, 18};
        p6[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p6[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d6{3, p6, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES heap6{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf6{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kPostSlots} * 512, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok6 = SUCCEEDED(D3DCompile(kPackShader, sizeof(kPackShader) - 1, "focus-pack", nullptr, nullptr, "main",
                                        "cs_5_0", 0, 0, &code6, &errors6)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d6, D3D_ROOT_SIGNATURE_VERSION_1, &blob6, &errors6)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob6->GetBufferPointer(), blob6->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.packRoot)));
        if (ok6) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso6{};
            pso6.pRootSignature = focus.packRoot.Get();
            pso6.CS = {code6->GetBufferPointer(), code6->GetBufferSize()};
            ok6 = SUCCEEDED(device->CreateComputePipelineState(&pso6, IID_PPV_ARGS(&focus.packPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap6, D3D12_HEAP_FLAG_NONE, &buf6,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.packConstants)));
        }
        if (!ok6) {
            logf("replay focus: dlss: the packing constants shader failed (%s); no focus DLSS",
                 errors6 ? static_cast<const char*>(errors6->GetBufferPointer()) : "?");
            focus.dlssBroken = true;
        }
    }
    if (focus.settings.post) {  // P2c: the vrAntialiasing constants
        ComPtr<ID3DBlob> code7, errors7, blob7;
        D3D12_ROOT_PARAMETER p7[3] = {};
        p7[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p7[0].Descriptor = {0, 0};
        p7[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p7[1].Constants = {1, 0, 9};
        p7[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p7[2].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC d7{3, p7, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES heap7{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf7{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kPostSlots} * 512, 1, 1, 1,
                                       DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok7 = SUCCEEDED(D3DCompile(kAaShader, sizeof(kAaShader) - 1, "focus-aa", nullptr, nullptr, "main", "cs_5_0",
                                        0, 0, &code7, &errors7)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d7, D3D_ROOT_SIGNATURE_VERSION_1, &blob7, &errors7)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob7->GetBufferPointer(), blob7->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.aaPatchRoot)));
        if (ok7) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso7{};
            pso7.pRootSignature = focus.aaPatchRoot.Get();
            pso7.CS = {code7->GetBufferPointer(), code7->GetBufferSize()};
            ok7 = SUCCEEDED(device->CreateComputePipelineState(&pso7, IID_PPV_ARGS(&focus.aaPatchPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap7, D3D12_HEAP_FLAG_NONE, &buf7,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.aaConstants)));
        }
        if (ok7 && focus.settings.atmosphere == 4) {  // the focus ray grid (same root signature)
            ComPtr<ID3DBlob> code8, errors8;
            const D3D12_RESOURCE_DESC buf8{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 16 * 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                           {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso8{};
            pso8.pRootSignature = focus.aaPatchRoot.Get();
            if (SUCCEEDED(D3DCompile(kAtmoGridShader, sizeof(kAtmoGridShader) - 1, "focus-atmo-grid", nullptr, nullptr,
                                     "main", "cs_5_0", 0, 0, &code8, &errors8))) {
                pso8.CS = {code8->GetBufferPointer(), code8->GetBufferSize()};
                if (FAILED(device->CreateComputePipelineState(&pso8, IID_PPV_ARGS(&focus.atmoGridPso))) ||
                    FAILED(device->CreateCommittedResource(&heap7, D3D12_HEAP_FLAG_NONE, &buf8,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&focus.atmoGridConstants))))
                    focus.atmoGridPso.Reset();
            }
            if (!focus.atmoGridPso)
                log_line("replay focus: atmosphere: the ray-grid shader failed; the maps are resampled instead");
        }
        if (!ok7) {
            logf("replay focus: msaa: the antialiasing constants shader failed (%s); no focus MSAA resolve",
                 errors7 ? static_cast<const char*>(errors7->GetBufferPointer()) : "?");
            focus.aaBroken = true;
        }
    }
    if (focus.settings.sky) {  // the sky's clouds resample
        ComPtr<ID3DBlob> code5, errors5, blob5;
        D3D12_DESCRIPTOR_RANGE r5[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kBindlessRange, 0, 1, 0},
                                        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p5[4] = {};
        p5[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p5[0].Descriptor = {0, 0};
        p5[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p5[1].Constants = {1, 0, 5};
        p5[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p5[2].DescriptorTable = {1, &r5[0]};
        p5[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p5[3].DescriptorTable = {1, &r5[1]};
        D3D12_STATIC_SAMPLER_DESC lin5{};
        lin5.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lin5.AddressU = lin5.AddressV = lin5.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lin5.MaxLOD = D3D12_FLOAT32_MAX;
        lin5.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC d5{4, p5, 1, &lin5, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso5{};
        D3D12_HEAP_PROPERTIES heap5{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC buf5{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64{kSkySlots} * 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                       {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const D3D12_RESOURCE_DESC tex5{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kSkyCloudSize, kSkyCloudSize, 1, 1,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        bool ok5 = SUCCEEDED(D3DCompile(kSkyCloudShader, sizeof(kSkyCloudShader) - 1, "focus-sky-clouds", nullptr, nullptr,
                                        "main", "cs_5_1", D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code5,
                                        &errors5)) &&
                   SUCCEEDED(D3D12SerializeRootSignature(&d5, D3D_ROOT_SIGNATURE_VERSION_1, &blob5, &errors5)) &&
                   SUCCEEDED(device->CreateRootSignature(0, blob5->GetBufferPointer(), blob5->GetBufferSize(),
                                                         IID_PPV_ARGS(&focus.skyRoot)));
        if (ok5) {
            pso5.pRootSignature = focus.skyRoot.Get();
            pso5.CS = {code5->GetBufferPointer(), code5->GetBufferSize()};
            ok5 = SUCCEEDED(device->CreateComputePipelineState(&pso5, IID_PPV_ARGS(&focus.skyPso))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap5, D3D12_HEAP_FLAG_NONE, &buf5,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&focus.skyConstants))) &&
                  SUCCEEDED(device->CreateCommittedResource(&heap5, D3D12_HEAP_FLAG_NONE, &tex5,
                                                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                            IID_PPV_ARGS(&focus.skyClouds)));
        }
        if (!ok5) {
            logf("replay focus: sky: the clouds shader failed (%s); no focus sky",
                 errors5 ? static_cast<const char*>(errors5->GetBufferPointer()) : "?");
            focus.skyBroken = true;
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
        const float zero[4] = {}, one[4] = {1, 1, 1, 1};
        for (const auto& [game, t] : focus.twins) {
            const D3D12_CPU_DESCRIPTOR_HANDLE v = which ? t.eyeView : t.focusView;
            if (t.depth)
                l->ClearDepthStencilView(v, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            else  // ACE clears its OIT reveal target to 1 (nothing in front), everything else to 0
                l->ClearRenderTargetView(v, t.name == "oitReveal" ? one : zero, 0, nullptr);
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
    const double k = focus.sizeK;
    for (const SIZE_T h : s.targets) {
        const ViewInfo& v = focus.views[h];
        if (focus.twins.count(v.resource.Get()))
            continue;
        // A new game target with the name of an older one of another sample count (MSAA switched live): the older
        // twins go, so lookups by name find the current ones.
        const std::string newName = debug_name(v.resource.Get());
        const D3D12_RESOURCE_DESC nd = v.resource->GetDesc();
        for (auto it = focus.twins.begin(); it != focus.twins.end();) {
            Twin& o = it->second;
            // (only another sample count: the game keeps same-named targets of different sizes side by side)
            if (o.name == newName && o.depth == v.depth && o.gameDesc.SampleDesc.Count != nd.SampleDesc.Count) {
                logf("replay focus: the game made %s again (%u -> %u sample(s), %llux%u -> %llux%u): its older twins go",
                     newName.c_str(), o.gameDesc.SampleDesc.Count, nd.SampleDesc.Count,
                     static_cast<unsigned long long>(o.gameDesc.Width), o.gameDesc.Height,
                     static_cast<unsigned long long>(nd.Width), nd.Height);
                retire(o.focus.Get());
                retire(o.eye.Get());
                free_slot(o.depth, o.focusSlot);
                free_slot(o.depth, o.eyeSlot);
                it = focus.twins.erase(it);
                twinsRemade.fetch_add(1);
                changed = true;
            } else {
                ++it;
            }
        }
        Twin t;
        t.gameDesc = v.resource->GetDesc();
        t.viewFormat = v.viewFormat != DXGI_FORMAT_UNKNOWN ? v.viewFormat : t.gameDesc.Format;
        t.depth = v.depth;
        t.name = debug_name(v.resource.Get());
        t.eyeW = static_cast<UINT>(t.gameDesc.Width / 2);
        t.eyeH = t.gameDesc.Height;
        t.focusW = std::max(16u, static_cast<UINT>(std::lround(t.eyeW * k)));
        t.focusH = std::max(16u, static_cast<UINT>(std::lround(t.eyeH * focus.sizeKy)));
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

// The box's half extent in NDC y (x's unless a crop or a focus height of its own makes it differ).
float focus_box_y() {
    return static_cast<float>(focus.settings.boxFractionY > 0 ? focus.settings.boxFractionY : focus.settings.boxFraction);
}
uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Each eye's box centre in its NDC: the eye's forward axis plus the gaze offset of this frame (under focus.lock).
void focus_centres(float (&centre)[2][2]) {
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 2; ++k)
            centre[e][k] = focus.eyeCentre[e][k] + focus.gazeOffset[e][k];
}

void barrier(List* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to || !r)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    l->ResourceBarrier(1, &b);
}

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

// At the game's Close of a replayable stereo list: our replay of it, recorded now, executed after the game's batch.
struct SkyPatch {
    UINT64 gameTable = 0, ourTable = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gameCbSky = 0, ourCbSky = 0;
};
std::vector<SkyPatch> focus_sky_prepare(const Stream& s);
void focus_sky_record(List* l, const std::vector<SkyPatch>& patches, const float (&centre)[2]);

void focus_record(List* gameList, const Stream& s, bool mono = false) {
    Replay* r = nullptr;
    std::vector<std::pair<SIZE_T, SIZE_T>> focusTargets, eyeTargets;
    std::vector<SkyPatch> skyPatches;
    // A per-eye list (mono): one eye-size viewport at x 0 (eye 0) or at the eye width (eye 1) of a two-eye target.
    const int monoEye = mono ? (s.monoX > 0 ? 1 : 0) : -1;
    if (mono)
        focus.monoTaken[monoEye].fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&focus.lock);
    if (focus_prepare(s)) {
        bool fits = true;
        for (const SIZE_T h : s.targets) {
            const Twin& t = focus.twins.at(focus.views.at(h).resource.Get());
            focusTargets.emplace_back(h, t.focusView.ptr);
            eyeTargets.emplace_back(h, t.eyeView.ptr);
            if (mono && (static_cast<float>(t.eyeW) != s.monoW || static_cast<float>(t.eyeH) != s.monoH ||
                         (s.monoX != 0 && s.monoX != s.monoW)))
                fits = false;  // not one eye of this target (a full-target pass, or another size)
        }
        if (fits)
            r = acquire_replay();
        if (r && !mono && (focus.settings.diagMode != 3 || (focus.samplesTaken.load() % 4) & 2))
            skyPatches = focus_sky_prepare(s);
        else if (!r && mono && !fits) {
            focus.monoUnfit.fetch_add(1, std::memory_order_relaxed);
            static int unfitLogged = 0;
            if (unfitLogged++ < 6)
                logf("replay focus: per-eye list x %.0f %.0fx%.0f doesn't fit its targets' eye size; skipped", s.monoX,
                     s.monoW, s.monoH);
        }
        if (!mono) {
            // The scene's eye size (what a per-eye list must match): only from lists that write the scene depth
            // (msaaDepth). ACE's half-width flags pass (flagsDepth) is stereo too; taking its size let per-eye lists
            // closing on other threads right after it fail the match and drop out (intermittent missing transparents).
            bool scene = false;
            for (const SIZE_T h : s.targets) {
                const Twin& t = focus.twins.at(focus.views.at(h).resource.Get());
                scene = scene || (t.depth && t.name == "msaaDepth");
            }
            if (scene) {
                focus.sceneEyeW = s.eyeW;
                focus.sceneEyeH = s.eyeH;
            }
        }
    }
    // This list's own eye size (ACE's passes differ: the flags pass is half width) and its focus size.
    const double k = focus.sizeK;
    const float sEyeW = mono ? s.monoW : s.eyeW, sEyeH = mono ? s.monoH : s.eyeH;
    const UINT ewi = static_cast<UINT>(sEyeW), ehi = static_cast<UINT>(sEyeH);
    const UINT fwi = std::max(16u, static_cast<UINT>(std::lround(sEyeW * k)));
    const UINT fhi = std::max(16u, static_cast<UINT>(std::lround(sEyeH * focus.sizeKy)));
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
    // replay_diag: sample 1 as set, 2 our fog off, 3 our 3D atmosphere maps on, 4 the sky maps too
    const uint32_t diagPhase = focus.settings.diag ? focus.samplesTaken.load() : UINT32_MAX;
    const bool atmoOn = focus.atmoReady && !focus.atmoBroken && !focus.tilesBroken &&
                        (focus.settings.diag ? diagPhase >= 2
                                             : focus.settings.atmosphere != 2 || focus.samplesTaken.load() % 2 == 0);
    // replay_diag=3 (the sky): by sample, bit 0 our sky maps, bit 1 our cbSky copy: 0 ACE's both, 1 our maps, 2 our
    // cbSky, 3 both ours
    const uint32_t skyPhase = focus.settings.diagMode == 3 ? focus.samplesTaken.load() % 4 : 3;
    const bool atmoSky = ((focus.settings.diag && diagPhase >= 3) || focus.settings.atmosphere >= 3) && (skyPhase & 1);
    const bool shadowPhase = focus.settings.shadows != 2 || focus.samplesTaken.load() % 2 == 0;  // A/B: on, off, ...
    const uint32_t shadowSrv = shadowPhase && focus.shadowReady && !focus.shadowBroken && !focus.tilesBroken
                                   ? focus.slotFirst + 15 - focus.baseIndex
                                   : 0;
    float gazeNow[4], gazeBefore[4];  // this frame's box centres off the eyes' axes, and last frame's
    std::memcpy(gazeNow, focus.gazeOffset, sizeof(gazeNow));
    std::memcpy(gazeBefore, focus.gazePrev, sizeof(gazeBefore));
    const uint32_t fogSrv = focus.fogMade && focus.fogReady && !focus.fogBroken && !focus.tilesBroken && diagPhase != 1
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
    if (!skyPatches.empty()) {  // the sky's clouds and cbSky copies, before the draws
        const float c0[2] = {focus.eyeCentre[0][0] + focus.gazeOffset[0][0], focus.eyeCentre[0][1] + focus.gazeOffset[0][1]};
        focus_sky_record(l, skyPatches, c0);
    }
    std::vector<std::pair<UINT64, UINT64>> skyMap;
    for (const SkyPatch& sp : skyPatches)
        skyMap.emplace_back(sp.gameTable, sp.ourTable);
    // 1. The focus constants: one slot per distinct root-1 address.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> slots;
    l->SetComputeRootSignature(focus.patchRoot.Get());
    l->SetPipelineState(focus.patchPso.Get());
    uint32_t params[kPatchParams] = {};
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    const float floats[4] = {box, boxY, static_cast<float>(fwi), static_cast<float>(fhi)};
    std::memcpy(params, floats, sizeof(floats));
    std::memcpy(params + 4, tileFrom, sizeof(tileFrom));
    std::memcpy(params + 12, tileTo, sizeof(tileTo));
    params[20] = fogSrv;
    params[21] = shadowSrv;
    // The sky maps (m % 4 = 2, 3) stay ACE's: the sky shader doesn't read them at the eye's screen position (10:23 run:
    // resampled, the focus sky came out uniformly darker).
    for (int m = 0; m < 8; ++m)
        params[24 + m] = atmoOn && (m % 4 < 2 || atmoSky) ? focus.slotFirst + 40 + static_cast<UINT>(m) - focus.baseIndex : 0;
    params[32] = focus.blurReady && !focus.blurBroken && !focus.tilesBroken
                     ? focus.slotFirst + 62 - focus.baseIndex
                     : 0;
    std::memcpy(params + 36, gazeNow, sizeof(gazeNow));
    std::memcpy(params + 40, gazeBefore, sizeof(gazeBefore));
    params[33] = fogSrv && focus.settings.fog == 2 ? focus.slotFirst + kFogFirst + 6 + 1 - focus.baseIndex : 0;
    l->SetComputeRoot32BitConstants(1, kPatchParams, params, 0);
    for (const D3D12_GPU_VIRTUAL_ADDRESS va : s.passConstants) {
        const uint32_t slot = focus.nextSlot.fetch_add(1) % kSlots;
        if (sample && !mono) {  // the sample's eye centres come from a stereo list's constants (per-eye lists differ)
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
    const UINT views = mono ? 1u : 2u;
    const D3D12_VIEWPORT* fv = mono ? &focusViews[monoEye] : focusViews;
    const D3D12_RECT* fr = mono ? &focusRects[monoEye] : focusRects;
    Substitution sub{fv, fr, views, &s.passConstants, &slots, &focusTargets};
    if (!skyMap.empty())
        sub.skyTables = &skyMap;
    l->RSSetViewports(views, fv);  // in case the list sets none itself
    l->RSSetScissorRects(views, fr);
    int64_t draws = emit(l, s, &sub);
    // 3. At sample frames, the same list unchanged into the eye-size references.
    if (sample && draws >= 0) {
        const float ew = static_cast<float>(ewi), eh = static_cast<float>(ehi);
        const D3D12_VIEWPORT eyeViews[2] = {{0, 0, ew, eh, 0, 1}, {ew, 0, ew, eh, 0, 1}};
        const D3D12_RECT eyeRects[2] = {{0, 0, static_cast<LONG>(ewi), static_cast<LONG>(ehi)},
                                        {static_cast<LONG>(ewi), 0, static_cast<LONG>(2 * ewi), static_cast<LONG>(ehi)}};
        const D3D12_VIEWPORT* ev = mono ? &eyeViews[monoEye] : eyeViews;
        const D3D12_RECT* er = mono ? &eyeRects[monoEye] : eyeRects;
        const Substitution eyeSub{ev, er, views, nullptr, nullptr, &eyeTargets};
        l->RSSetViewports(views, ev);
        l->RSSetScissorRects(views, er);
        if (emit(l, s, &eyeSub) < 0)
            draws = -1;
    }
    barrier(l, focus.constants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!skyPatches.empty())
        barrier(l, focus.skyConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->Close();
    inReplay = false;
    replayTicks.fetch_add(static_cast<uint64_t>(now_ticks() - start), std::memory_order_relaxed);
    AcquireSRWLockExclusive(&focus.lock);
    if (draws < 0)
        r->busy = false;  // recorded but never executed: the allocator is reset at its next use
    else
        focus.pending[gameList].push_back(r);
    ReleaseSRWLockExclusive(&focus.lock);
    if (draws < 0) {
        if (mono)
            focus.monoBroken.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    listsReplayed.fetch_add(1, std::memory_order_relaxed);
    if (mono) {
        focus.monoReplays.fetch_add(1, std::memory_order_relaxed);
        focus.monoDone[monoEye].fetch_add(1, std::memory_order_relaxed);
    }
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

// Under focus.lock: the twin of the stereo scene depth: single-sample R32_TYPELESS (what the tile and shadow passes
// read as R32_FLOAT), the largest, ACE's msaaDepth by name on a tie. Not just "the largest depth": flagsDepth (D32) is
// the same size, and hash order picked it in one run (tiles and shadows stopped, 2026-11-13 10:01).
D3D12_RESOURCE_STATES rest_of(ID3D12Resource* r, D3D12_RESOURCE_STATES otherwise);

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
    // The focus depth twin: the stereo scene depth (scene_depth_twin).
    const Twin* depth = scene_depth_twin();
    if (!depth)
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
    barrier(l, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE));
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
        device->CreateUnorderedAccessView(focus.skyClouds.Get(), nullptr, &u, game_cpu(focus.slotFirst + kSkyCloudSrv + 1));
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
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
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
        ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
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
            l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 67 + e));
            l->SetComputeRootDescriptorTable(2, game_gpu(focus.fogUavFirst + 6 * e));
            l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, p[5]);
            barrier(l, grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        l->Close();
        inReplay = false;
        AcquireSRWLockExclusive(&focus.lock);
        focus.pending[gameList].push_back(rr);
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
    focus.pending[gameList].push_back(r);
    if (!focus.fogReady)
        log_line("replay focus: fog: the first focus fog replay recorded; the lighting reads it from the next frame");
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
                device->CreateUnorderedAccessView(focus.normals.Get(), nullptr, &u, game_cpu(focus.slotFirst + 28));
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
                logf("replay focus: shadow: our normals %ux%u, deferred shadow %ux%u at bindless %u", 2 * fw, fh, fw,
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
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
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
    focus.pending[gameList].push_back(r);
    if (!focus.shadowReady)
        log_line("replay focus: shadow: the first focus deferred-shadow replay recorded");
    focus.shadowReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.shadowReplays.fetch_add(1, std::memory_order_relaxed);
}

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
        else if (missLogged < 3 && focus.twins.size() > 8) {  // not before the scene's twins exist
            ++missLogged;
            logf("replay focus: OIT composite: not replayed (%s)", why.empty() ? "no target" : why.c_str());
        }
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
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
        focus.pending[gameList].push_back(r);
    ReleaseSRWLockExclusive(&focus.lock);
    if (draws >= 0)
        focus.oitReplays.fetch_add(1, std::memory_order_relaxed);
}

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
                logf("replay focus: atmosphere: %s%d not among the %zu views seen yet", kAtmoNames[m % 4], m / 4,
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
        device->CreateUnorderedAccessView(focus.atmo[m].Get(), nullptr, &u, game_cpu(focus.slotFirst + 48 + m));
    }
    inReplay = false;
    if (!ok) {
        log_line("replay focus: atmosphere: could not make our maps; no focus atmosphere");
        focus.atmoBroken = true;
        return false;
    }
    const D3D12_RESOURCE_DESC a = game[0]->GetDesc(), b = game[2]->GetDesc();
    logf("replay focus: atmosphere: our maps made (3D %llux%ux%u, sky %llux%u), at bindless %u..%u",
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
            device->CreateUnorderedAccessView(focus.atmoSkyWork[e][k].Get(), nullptr, &u,
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
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
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
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 32 + m));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 48 + m));
        l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, sz);
        barrier(l, focus.atmo[m].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    focus.pending[gameList].push_back(r);
    if (!focus.atmoReady)
        logf("replay focus: atmosphere: first resample recorded (eye centres %.4f,%.4f and %.4f,%.4f)", centre[0][0],
             centre[0][1], centre[1][0], centre[1][1]);
    focus.atmoReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.atmoReplays.fetch_add(1, std::memory_order_relaxed);
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
                        device->CreateUnorderedAccessView(b.Get(), nullptr, &u, game_cpu(focus.slotFirst + 63 + m));
                    }
                    inReplay = false;
                    focus.blur = b;
                    focus.blurColour = colour;
                    focus.blurMips = d.MipLevels;
                    logf("replay focus: refraction: our opaqueSceneBlur %llux%u, %u mips (mip 0 our colour, the rest the "
                         "eye's resampled), at bindless %u",
                         static_cast<unsigned long long>(d.Width), d.Height, static_cast<unsigned>(d.MipLevels),
                         focus.slotFirst + 62 - focus.baseIndex);
                } else {
                    log_line("replay focus: refraction: could not make our blur; none");
                    focus.blurBroken = true;
                }
            } else if (!focus.blurMissLogged) {
                focus.blurMissLogged = true;
                log_line("replay focus: refraction: the game's opaqueSceneBlur not seen yet (or another format)");
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
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
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
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 61));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 63 + m));
        l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, 1);
    }
    barrier(l, focus.blur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    focus.pending[gameList].push_back(r);
    focus.blurReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.blurReplays.fetch_add(1, std::memory_order_relaxed);
}

// ---- MSAA: the focus views' depth and velocity resolves ---------------------------------------------------------

// At the game's Close of its resolve list (resolveDepthCompute, resolveDepthVelocityCompute: per pixel, no constants,
// single-descriptor tables): ACE's two pipelines replayed on our multisampled depth and velocity twins into our
// single-sample focus depth (stand-in for depthResolveTarget), closest depth and velocity (velocity_n_reactive_mask).
void focus_resolve_record(List* gameList, const Stream& s) {
    const Stream::TileDispatch *rd = nullptr, *rdv = nullptr;
    for (const auto& d : s.tileDispatches) {
        if (d.kind == 14 && !rd)
            rd = &d;
        if (d.kind == 15 && !rdv)
            rdv = &d;
    }
    if (!rd)
        return;
    Replay* r = nullptr;
    std::string why;
    UINT w = 0, h = 0;
    ID3D12Resource *msDepth = nullptr, *msVelocity = nullptr;
    AcquireSRWLockExclusive(&focus.lock);
    const Twin *depthTwin = nullptr, *velocityTwin = nullptr;
    for (const auto& [g, tw] : focus.twins) {
        if (tw.gameDesc.SampleDesc.Count < 2)
            continue;
        if (tw.depth && tw.name == "msaaDepth")
            depthTwin = &tw;
        else if (!tw.depth && tw.name.find("velocity") != std::string::npos)
            velocityTwin = &tw;
    }
    if (focus.resolveBroken || !focus.gameHeap || !focus.slotFirst || !focus.fogUavFirst || !depthTwin) {
        why = "the multisampled depth twin, our bindless block or our UAV block (fog) not there";
    } else {
        w = 2 * depthTwin->focusW;
        h = depthTwin->focusH;
        msDepth = depthTwin->focus.Get();
        msVelocity = velocityTwin ? velocityTwin->focus.Get() : nullptr;
        if (focus.resolveDepthSource != msDepth || focus.resolveVelocitySource != msVelocity) {
            for (ID3D12Resource* old : {focus.resolvedDepth.Get(), focus.closestDepth.Get(), focus.resolvedVelocity.Get()})
                retire(old);
            focus.resolvedDepth.Reset();
            focus.closestDepth.Reset();
            focus.resolvedVelocity.Reset();
            focus.resolveReady = false;
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            const D3D12_RESOURCE_STATES read =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            const auto make = [&](DXGI_FORMAT f, ComPtr<ID3D12Resource>& out) {
                d.Format = f;
                return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, read, nullptr,
                                                                 IID_PPV_ARGS(&out)));
            };
            const DXGI_FORMAT velFormat = velocityTwin ? velocityTwin->viewFormat : DXGI_FORMAT_R16G16_FLOAT;
            if (!make(DXGI_FORMAT_R32_FLOAT, focus.resolvedDepth) || !make(DXGI_FORMAT_R32_FLOAT, focus.closestDepth) ||
                (velocityTwin && !make(velFormat, focus.resolvedVelocity))) {
                log_line("replay focus: msaa: could not make our resolved depth / velocity; no focus resolves");
                focus.resolveBroken = true;
            } else {
                inReplay = true;
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Format = DXGI_FORMAT_R32_FLOAT;
                srvViewOriginal(device.Get(), msDepth, &v, game_cpu(focus.slotFirst + 74));
                if (msVelocity) {
                    v.Format = velFormat;
                    srvViewOriginal(device.Get(), msVelocity, &v, game_cpu(focus.slotFirst + 75));
                }
                const auto uav = [&](ID3D12Resource* res, UINT offset) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                    u.Format = res->GetDesc().Format;
                    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    device->CreateUnorderedAccessView(res, nullptr, &u, game_cpu(focus.fogUavFirst + offset));
                };
                uav(focus.resolvedDepth.Get(), 29);
                uav(focus.closestDepth.Get(), 30);
                if (focus.resolvedVelocity)
                    uav(focus.resolvedVelocity.Get(), 31);
                inReplay = false;
                focus.resolvedDepthTwin = *depthTwin;
                focus.resolvedDepthTwin.focus = focus.resolvedDepth;
                focus.resolvedDepthTwin.gameDesc.SampleDesc = {1, 0};
                focus.resolvedDepthTwin.gameDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                focus.resolvedDepthTwin.viewFormat = DXGI_FORMAT_R32_FLOAT;
                focus.resolvedDepthTwin.name = "depthResolveTarget";
                if (velocityTwin) {
                    focus.resolvedVelocityTwin = *velocityTwin;
                    focus.resolvedVelocityTwin.focus = focus.resolvedVelocity;
                    focus.resolvedVelocityTwin.gameDesc.SampleDesc = {1, 0};
                    focus.resolvedVelocityTwin.name = "velocity_n_reactive_mask";
                }
                focus.resolveDepthSource = msDepth;
                focus.resolveVelocitySource = msVelocity;
                logf("replay focus: msaa: our resolved focus depth / closest depth%s %ux%u (from the %u-sample twins%s)",
                     velocityTwin ? " / velocity" : "", w, h, depthTwin->gameDesc.SampleDesc.Count,
                     velocityTwin ? (", velocity " + velocityTwin->name).c_str() : ", no velocity twin");
            }
        }
        if (!focus.resolveBroken && focus.resolvedDepth)
            r = acquire_replay();
    }
    static int missLogged = 0;
    if (!r && !why.empty() && missLogged < 3) {
        ++missLogged;
        logf("replay focus: msaa: resolves not replayed (%s)", why.c_str());
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
    l->SetDescriptorHeaps(1, heaps);
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier(l, msDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, msVelocity, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (ID3D12Resource* res : {focus.resolvedDepth.Get(), focus.closestDepth.Get(), focus.resolvedVelocity.Get()})
        barrier(l, res, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(rd->root);
    l->SetPipelineState(rd->pso);
    l->SetComputeRootDescriptorTable(0, game_gpu(focus.slotFirst + 74));
    l->SetComputeRootDescriptorTable(1, game_gpu(focus.fogUavFirst + 29));
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    if (rdv && msVelocity && focus.resolvedVelocity) {
        l->SetComputeRootSignature(rdv->root);
        l->SetPipelineState(rdv->pso);
        l->SetComputeRootDescriptorTable(0, game_gpu(focus.slotFirst + 74));
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 75));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.fogUavFirst + 30));
        l->SetComputeRootDescriptorTable(3, game_gpu(focus.fogUavFirst + 31));
        l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    for (ID3D12Resource* res : {focus.resolvedDepth.Get(), focus.closestDepth.Get(), focus.resolvedVelocity.Get()})
        barrier(l, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    barrier(l, msVelocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, msDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    focus.pending[gameList].push_back(r);
    if (!focus.resolveReady)
        log_line("replay focus: msaa: the first focus depth / velocity resolve recorded");
    focus.resolveReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.resolveReplays.fetch_add(1, std::memory_order_relaxed);
}

// ---- P2a: our DLSS per focus view ---------------------------------------------------------------------------------

// The focus twin of a resource ACE's DLSS evaluation names, by its name (none: nullptr).
ID3D12Resource* twin_named(const std::string& name, bool depth) {  // under focus.lock
    if (depth) {
        const Twin* d = scene_depth_twin();
        for (const auto& [game, tw] : focus.twins)
            if (tw.depth && tw.name == name && tw.gameDesc.SampleDesc.Count == 1)
                return tw.focus.Get();
        return d ? d->focus.Get() : nullptr;
    }
    for (const auto& [game, tw] : focus.twins)
        if (!tw.depth && tw.name == name && tw.gameDesc.SampleDesc.Count == 1)
            return tw.focus.Get();
    return nullptr;
}

// In our post list (open, our heap set), after the veil: per focus eye, ACE's VelocityAndDepthPacking replayed with
// focus constants into our four outputs, then our NGX DLSS feature for that eye over our colour twin, depth twin and
// packed motion vectors (each input the counterpart of what ACE's own evaluation named) into our DLSS image (slot 71).
// True when both eyes were evaluated (the tonemap then reads our DLSS image).
bool focus_dlss_record(List* l, const Stream& s, ID3D12Resource* colour, UINT fw2, UINT fh, const float (&centre)[2][2]) {
    if (focus.dlssBroken || !focus.packPso || !focus_ngx::installed())
        return false;
    const Stream::TileDispatch* pack[2] = {};
    for (const auto& d : s.tileDispatches)
        if (d.kind == 12) {
            if (!pack[0])
                pack[0] = &d;
            else if (!pack[1])
                pack[1] = &d;
        }
    focus_ngx::EyeEval game[2];
    static int missLogged = 0;
    const auto miss = [&](const char* why) {
        if (missLogged < 4) {
            ++missLogged;
            logf("replay focus: dlss: not this frame (%s)", why);
        }
        return false;
    };
    if (!pack[0] || !pack[1])
        return miss("ACE's two VelocityAndDepthPacking dispatches not in its post list");
    if (!focus_ngx::latest(0, game[0]) || !focus_ngx::latest(1, game[1]))
        return miss("ACE's DLSS evaluations of both eyes not seen yet");
    if (game[0].sequence <= focus.ngxUsed[0] || game[1].sequence <= focus.ngxUsed[1])
        return miss("ACE's DLSS evaluations not this frame's");
    const UINT fw = fw2 / 2;
    ID3D12Resource *depth = nullptr, *colourIn = nullptr, *motionIn = nullptr;
    const Twin* velocityTwin = nullptr;
    const Twin* flagsTwin = nullptr;
    UINT ew = 0, eh = 0;
    std::string why;
    AcquireSRWLockExclusive(&focus.lock);
    for (const auto& [g, tw] : focus.twins) {
        if (tw.depth || tw.gameDesc.SampleDesc.Count != 1)
            continue;
        if (tw.name == "velocity")
            velocityTwin = &tw;
        else if (tw.name == "flags")
            flagsTwin = &tw;
        else if (tw.name == "msaaColor") {
            ew = tw.eyeW;
            eh = tw.eyeH;
        }
    }
    if (!ew && focus.resolvedColour) {  // MSAA: the eye size from our resolved colour's stand-in
        ew = focus.resolvedColourTwin.eyeW;
        eh = focus.resolvedColourTwin.eyeH;
    }
    depth = focus.depthForTiles;  // the scene depth twin (slot 12's view)
    if (!velocityTwin && focus.resolvedVelocity && focus.resolveReady)  // MSAA: our resolve of the velocity twin
        velocityTwin = &focus.resolvedVelocityTwin;
    if (!velocityTwin || !flagsTwin || !depth || !ew || !focus.fogUavFirst || !focus.uavBaseIndex) {
        why = "the velocity / flags / depth twins or our UAV block (fog) not there";
    } else if (focus.packW != fw2 || focus.packH != fh || focus.packColour != colour ||
               focus.packVelocityTwin != velocityTwin->focus.Get() || focus.packFlagsTwin != flagsTwin->focus.Get()) {
        for (ID3D12Resource* old : {focus.packAa.Get(), focus.packMb.Get(), focus.packVelocity.Get(), focus.packFlags.Get(),
                                    focus.dlssOut.Get(), focus.dlssOutAuto.Get(), focus.casOut.Get()})
            retire(old);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, fw2, fh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const auto make = [&](DXGI_FORMAT f, ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES state) {
            d.Format = f;
            return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
        };
        // ACE's DLSS output format (post_process_color) for our DLSS image
        const DXGI_FORMAT outFormat = game[0].output ? game[0].output->GetDesc().Format : DXGI_FORMAT_R11G11B10_FLOAT;
        const bool ok = make(DXGI_FORMAT_R16G16B16A16_FLOAT, focus.packAa, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                        make(DXGI_FORMAT_R16G16B16A16_FLOAT, focus.packMb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                        make(DXGI_FORMAT_R16G16_FLOAT, focus.packVelocity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                        make(DXGI_FORMAT_R16_FLOAT, focus.packFlags, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                        make(outFormat, focus.dlssOut, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) &&
                        make(outFormat, focus.dlssOutAuto, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) &&
                        make(outFormat, focus.casOut, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        if (!ok) {
            log_line("replay focus: dlss: could not make our packing outputs / DLSS image; no focus DLSS");
            focus.dlssBroken = true;
        } else {
            inReplay = true;
            const auto uav = [&](ID3D12Resource* r, UINT offset) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                u.Format = r->GetDesc().Format;
                u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                device->CreateUnorderedAccessView(r, nullptr, &u, game_cpu(focus.fogUavFirst + offset));
            };
            uav(focus.packMb.Get(), 24);
            uav(focus.packVelocity.Get(), 25);
            uav(focus.packFlags.Get(), 26);
            uav(focus.packAa.Get(), 27);
            const auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.Format = f;
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                v.Texture2D.MipLevels = 1;
                srvViewOriginal(device.Get(), r, &v, game_cpu(focus.slotFirst + slot));
            };
            srv(velocityTwin->focus.Get(), velocityTwin->viewFormat, 69);
            srv(flagsTwin->focus.Get(), flagsTwin->viewFormat, 70);
            srv(focus.dlssOut.Get(), outFormat, 71);
            srv(focus.dlssOutAuto.Get(), outFormat, 72);
            srv(focus.casOut.Get(), outFormat, 73);
            uav(focus.casOut.Get(), 28);
            inReplay = false;
            focus.packW = fw2;
            focus.packH = fh;
            focus.packColour = colour;
            focus.packVelocityTwin = velocityTwin->focus.Get();
            focus.packFlagsTwin = flagsTwin->focus.Get();
            logf("replay focus: dlss: our packing outputs and DLSS image %ux%u (output format %d); velocity twin format %d, "
                 "flags %d",
                 fw2, fh, static_cast<int>(outFormat), static_cast<int>(velocityTwin->viewFormat),
                 static_cast<int>(flagsTwin->viewFormat));
        }
    }
    // ACE's inputs -> ours, by name: the colour and depth twins, our packing outputs for the motion vectors.
    if (why.empty() && !focus.dlssBroken) {
        colourIn = (game[0].colorName == "msaaColor" || game[0].colorName == "upscaling") ? colour
                                                                                            : twin_named(game[0].colorName, false);
        const std::string& mv = game[0].motionName;
        motionIn = mv == "velocityAndDepthAA" ? focus.packAa.Get() : mv == "velocity" ? focus.packVelocity.Get() : nullptr;
        ID3D12Resource* depthNamed = !game[0].depth                                           ? nullptr
                                     : game[0].depthName == "closestDepth" && focus.closestDepth ? focus.closestDepth.Get()
                                                                                                  : twin_named(game[0].depthName, true);
        if (depthNamed)
            depth = depthNamed;
        if (!colourIn || !motionIn)
            why = "ACE's DLSS colour '" + game[0].colorName + "' or motion '" + mv + "' has no focus counterpart";
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (focus.dlssBroken)
        return false;
    if (!why.empty())
        return miss(why.c_str());
    if (pack[0]->tables[2].ptr != game_gpu(focus.uavBaseIndex).ptr)
        return miss("ACE's packing UAV table isn't the bindless UAV range our outputs are in");
    // Our NGX features at the focus eye size (made again on a size change: the old ones are left to NGX's shutdown).
    if (!focus.ngx[0] || !focus.ngx[1] || focus.ngxW != fw || focus.ngxH != fh) {
        focus.ngx[0] = focus_ngx::make(l, fw, fh);
        focus.ngx[1] = focus.ngx[0] ? focus_ngx::make(l, fw, fh) : nullptr;
        if (focus.settings.dlss == 3 && focus.ngx[1]) {  // A/B: a pair with DLSS's own auto-exposure (flag 0x40)
            focus.ngxAuto[0] = focus_ngx::make(l, fw, fh, 0x40);
            focus.ngxAuto[1] = focus.ngxAuto[0] ? focus_ngx::make(l, fw, fh, 0x40) : nullptr;
        }
        focus.ngxW = fw;
        focus.ngxH = fh;
        focus.dlssFresh = true;
        if (!focus.ngx[0] || !focus.ngx[1]) {
            log_line("replay focus: dlss: NGX refused our focus features; no focus DLSS");
            focus.dlssBroken = true;
            return false;
        }
        ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};  // NGX may set its own heaps
        l->SetDescriptorHeaps(1, heaps);
    }
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    const float k = static_cast<float>(fw) / (static_cast<float>(ew) * box);  // focus pixels per eye pixel (eyeW: per eye)
    const float ky = static_cast<float>(focus.sizeKy / std::max(1e-6f, boxY));  // and vertically
    const UINT eyeW = ew;
    // 1. The packing, per eye: its constants patched, then ACE's dispatch over our focus eye.
    barrier(l, focus.packVelocityTwin, rest_of(focus.packVelocityTwin, D3D12_RESOURCE_STATE_RENDER_TARGET), read);
    barrier(l, focus.packFlagsTwin, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    barrier(l, depth, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE), read);
    D3D12_GPU_VIRTUAL_ADDRESS cb[2];
    for (int e = 0; e < 2; ++e) {
        cb[e] = focus.packConstants->GetGPUVirtualAddress() + UINT64{focus.packSlot++ % kPostSlots} * 512;
        uint32_t p[18] = {};
        p[16] = boxYBits;
        std::memcpy(p + 17, &ky, 4);
        const float f[7] = {box, -centre[e][0], -centre[e][1], k, static_cast<float>(fw), static_cast<float>(fh),
                            static_cast<float>(e * fw)};
        std::memcpy(p, f, sizeof(f));
        const float before[2] = {-(centre[e][0] - focus.gazeOffset[e][0] + focus.gazePrev[e][0]),
                                 -(centre[e][1] - focus.gazeOffset[e][1] + focus.gazePrev[e][1])};
        std::memcpy(p + 14, before, sizeof(before));
        const UINT rel = focus.slotFirst - focus.baseIndex, urel = focus.fogUavFirst - focus.uavBaseIndex;
        p[7] = rel + 69;   // velocity
        p[8] = rel + 12;   // depth
        p[9] = rel + 70;   // flags
        p[10] = urel + 24; // MB
        p[11] = urel + 25; // velocity only
        p[12] = urel + 26; // flags output
        p[13] = urel + 27; // AA
        l->SetComputeRootSignature(focus.packRoot.Get());
        l->SetPipelineState(focus.packPso.Get());
        l->SetComputeRootConstantBufferView(0, pack[e]->cbv);
        l->SetComputeRoot32BitConstants(1, 14, p, 0);
        l->SetComputeRootUnorderedAccessView(2, cb[e]);
        l->Dispatch(1, 1, 1);
    }
    barrier(l, focus.packConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    for (int e = 0; e < 2; ++e) {
        const Stream::TileDispatch& d = *pack[e];
        l->SetComputeRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetComputeRootConstantBufferView(0, cb[e]);
        for (int t = 1; t <= 4; ++t)
            l->SetComputeRootDescriptorTable(t, d.tables[t]);
        l->Dispatch((fw + 7) / 8, (fh + 7) / 8, 1);
    }
    barrier(l, focus.packConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(l, focus.packVelocityTwin, read, rest_of(focus.packVelocityTwin, D3D12_RESOURCE_STATE_RENDER_TARGET));
    barrier(l, focus.packFlagsTwin, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // 2. Our DLSS, per eye (its own feature and history).
    for (ID3D12Resource* r : {focus.packAa.Get(), focus.packVelocity.Get()})
        barrier(l, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    barrier(l, focus.dlssOut.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool autoPair = focus.ngxAuto[0] && focus.ngxAuto[1];
    if (autoPair)
        barrier(l, focus.dlssOutAuto.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bool ok = true;
    for (int e = 0; e < 2; ++e) {
        focus_ngx::Inputs in;
        in.color = colourIn;
        in.depth = depth;
        in.motion = motionIn;
        in.output = focus.dlssOut.Get();
        in.exposure = game[e].exposure;  // ACE's own (its 1x1 luminance or none)
        in.base[0] = e * fw;
        in.base[1] = 0;
        in.width = fw;
        in.height = fh;
        in.jitter[0] = game[e].jitter[0] * k;
        in.jitter[1] = game[e].jitter[1] * k;
        for (int a = 0; a < 2; ++a) {  // a scale that is a size (|s| > 4): the focus size's; else as ACE's
            const float sz = a == 0 ? static_cast<float>(fw) / static_cast<float>(eyeW)
                                    : static_cast<float>(fh) / static_cast<float>(eh);
            in.motionScale[a] = std::fabs(game[e].motionScale[a]) > 4 ? game[e].motionScale[a] * sz : game[e].motionScale[a];
        }
        in.game = &game[e];
        in.reset = focus.dlssFresh || game[e].reset != 0;
        ok = focus_ngx::evaluate(l, focus.ngx[e], in) && ok;
        if (autoPair) {
            in.output = focus.dlssOutAuto.Get();
            in.exposure = nullptr;
            focus_ngx::evaluate(l, focus.ngxAuto[e], in);
        }
        focus.ngxUsed[e] = game[e].sequence;
    }
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};  // NGX sets its own heaps
    l->SetDescriptorHeaps(1, heaps);
    barrier(l, focus.dlssOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (autoPair)
        barrier(l, focus.dlssOutAuto.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    barrier(l, colour, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, depth, read, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    for (ID3D12Resource* r : {focus.packAa.Get(), focus.packVelocity.Get()})
        barrier(l, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!ok)
        return false;
    // 3. ACE's CAS (FidelityFX contrast-adaptive sharpening, its only constant the sharpness: size-independent) over our
    // DLSS image into casOut, as ACE sharpens its eyes' DLSS output.
    focus.casDone = false;
    for (const auto& d : s.tileDispatches) {
        if (d.kind != 13)
            continue;
        barrier(l, focus.casOut.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetComputeRoot32BitConstants(0, 1, d.constants, 0);
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 71));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.fogUavFirst + 28));
        l->Dispatch((fw2 + 15) / 16, (fh + 15) / 16, 1);
        barrier(l, focus.casOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        focus.casDone = true;
        static bool casLogged = false;
        if (!casLogged) {
            casLogged = true;
            float sharp;
            std::memcpy(&sharp, d.constants, 4);
            logf("replay focus: dlss: ACE's CAS replayed on our DLSS image (its constant 0x%08x / %.4g)", d.constants[0], sharp);
        }
        break;
    }
    if (!focus.dlssReady)
        logf("replay focus: dlss: the first focus DLSS recorded (colour %s, depth %s, motion %s -> ours; jitter x %.3f, "
             "motion scale %.4g,%.4g -> ours)",
             game[0].colorName.c_str(), game[0].depthName.c_str(), game[0].motionName.c_str(), k, game[0].motionScale[0],
             game[0].motionScale[1]);
    focus.dlssReady = true;
    focus.dlssFresh = false;
    focus.dlssEvaluations.fetch_add(2, std::memory_order_relaxed);
    return true;
}

// ---- P2c: the focus views' MSAA antialiasing (DLSS off) -------------------------------------------------------------

// In our post list (open, our heap set): ACE's vrAntialiasing (one dispatch per eye in its post list: per-view constants
// at root 6; tables t0 colour MS, t1 unused, t2 depthResolveTarget, t3 prev_frame_color, u0 post_process_color, u1
// unused) replayed per focus eye with its constants patched, from our multisampled colour twin, our scene depth and
// our history into aaOut, which is then copied into our history. True when done (the tonemap reads aaOut, slot 78).
bool focus_aa_record(List* l, const Stream& s, UINT fw2, UINT fh, const float (&centre)[2][2]) {
    if (focus.aaBroken || !focus.aaPatchPso || !focus.fogUavFirst)
        return false;
    const Stream::TileDispatch* aa[2] = {};
    for (const auto& d : s.tileDispatches)
        if (d.kind == 16 && d.cbv6) {
            if (!aa[0])
                aa[0] = &d;
            else if (!aa[1])
                aa[1] = &d;
        }
    if (!aa[0] || !aa[1]) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::string names;
            for (ID3D12PipelineState* pso : s.dispatchPsos) {
                wchar_t n[64] = {};
                UINT size = sizeof(n) - sizeof(wchar_t);
                if (pso && SUCCEEDED(pso->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, n))) {
                    names += ' ';
                    for (const wchar_t* c = n; *c; ++c)
                        names += static_cast<char>(*c);
                } else {
                    names += " ?";
                }
            }
            size_t k16 = 0, cb = 0;
            for (const auto& d : s.tileDispatches)
                if (d.kind == 16) {
                    ++k16;
                    cb += d.cbv6 != 0;
                }
            logf("replay focus: msaa: ACE's antialiasing not found as two dispatches in its post list (kind-16 dispatches %zu, "
                 "with a root-6 CBV %zu); the list's compute dispatches:%s",
                 k16, cb, names.c_str());
        }
        return false;
    }
    ID3D12Resource* ms = focus.resolvedColourTwin.eye.Get();  // the multisampled colour twin (see focus_post_record)
    ID3D12Resource* colour = focus.resolvedColour.Get();
    if (!ms || !colour)
        return false;
    const UINT fw = fw2 / 2;
    AcquireSRWLockExclusive(&focus.lock);
    if (focus.aaSource != ms || !focus.aaOut || focus.aaOut->GetDesc().Width != fw2) {
        retire(focus.aaOut.Get());
        retire(focus.aaHistory.Get());
        focus.aaOut.Reset();
        focus.aaHistory.Reset();
        D3D12_RESOURCE_DESC d = colour->GetDesc();
        d.Alignment = 0;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, read, nullptr, IID_PPV_ARGS(&focus.aaOut))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, read, nullptr,
                                                   IID_PPV_ARGS(&focus.aaHistory)))) {
            log_line("replay focus: msaa: could not make our antialiasing targets; no focus MSAA resolve");
            focus.aaBroken = true;
        } else {
            inReplay = true;
            const DXGI_FORMAT f = focus.resolvedColourTwin.viewFormat;
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = f;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            srvViewOriginal(device.Get(), ms, &v, game_cpu(focus.slotFirst + 76));
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            srvViewOriginal(device.Get(), focus.aaHistory.Get(), &v, game_cpu(focus.slotFirst + 77));
            srvViewOriginal(device.Get(), focus.aaOut.Get(), &v, game_cpu(focus.slotFirst + 78));
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = f;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(focus.aaOut.Get(), nullptr, &u, game_cpu(focus.fogUavFirst + 32));
            inReplay = false;
            focus.aaSource = ms;
            focus.aaFresh = true;
            logf("replay focus: msaa: our antialiased focus colour and history %ux%u (ACE's vrAntialiasing replayed)", fw2, fh);
        }
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (focus.aaBroken || !focus.aaOut)
        return false;
    // 1. Per eye: the constants patched.
    D3D12_GPU_VIRTUAL_ADDRESS cb[2];
    const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
    l->SetComputeRootSignature(focus.aaPatchRoot.Get());
    l->SetPipelineState(focus.aaPatchPso.Get());
    for (int e = 0; e < 2; ++e) {
        cb[e] = focus.aaConstants->GetGPUVirtualAddress() + UINT64{focus.aaSlot++ % kPostSlots} * 512;
        const float p[9] = {box, -centre[e][0], -centre[e][1], static_cast<float>(fw), static_cast<float>(fh),
                            static_cast<float>(e * fw), -(centre[e][0] - focus.gazeOffset[e][0] + focus.gazePrev[e][0]),
                            -(centre[e][1] - focus.gazeOffset[e][1] + focus.gazePrev[e][1]), boxY};
        l->SetComputeRootConstantBufferView(0, aa[e]->cbv6);
        l->SetComputeRoot32BitConstants(1, 9, p, 0);
        l->SetComputeRootUnorderedAccessView(2, cb[e]);
        l->Dispatch(1, 1, 1);
    }
    barrier(l, focus.aaConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    // 2. ACE's pipeline per eye. A fresh history (first frame) starts from the current resolve.
    ID3D12Resource* depth = focus.depthForTiles;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (focus.aaFresh) {
        barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(focus.aaHistory.Get(), colour);
        barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        barrier(l, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        focus.aaFresh = false;
    }
    barrier(l, ms, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, depth, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE), read);
    barrier(l, focus.aaOut.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int e = 0; e < 2; ++e) {
        const Stream::TileDispatch& d = *aa[e];
        l->SetComputeRootSignature(d.root);
        l->SetPipelineState(d.pso);
        l->SetComputeRootDescriptorTable(0, game_gpu(focus.slotFirst + 76));
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 77));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 12));
        l->SetComputeRootDescriptorTable(3, game_gpu(focus.slotFirst + 77));
        l->SetComputeRootDescriptorTable(4, game_gpu(focus.fogUavFirst + 32));
        l->SetComputeRootDescriptorTable(5, game_gpu(focus.fogUavFirst + 32));
        l->SetComputeRootConstantBufferView(6, cb[e]);
        l->Dispatch((fw + 7) / 8, (fh + 7) / 8, 1);
    }
    barrier(l, focus.aaOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(l, depth, read, rest_of(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    barrier(l, ms, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(l, focus.aaConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // 3. Our history (ACE copies post_process_color into prev_frame_color the same way).
    barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(focus.aaHistory.Get(), focus.aaOut.Get());
    barrier(l, focus.aaHistory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    barrier(l, focus.aaOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (!focus.aaReady)
        log_line("replay focus: msaa: the first focus antialiasing recorded (ACE's vrAntialiasing per eye)");
    focus.aaReady = true;
    focus.aaReplays.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- P1: the focus views' post: bloom, tonemap, fxaa -------------------------------------------------------------

// The game's view (srvInfo) a descriptor table points at: its resource and view, or false.
bool table_view(D3D12_GPU_DESCRIPTOR_HANDLE table, ComPtr<ID3D12Resource>& r, D3D12_SHADER_RESOURCE_VIEW_DESC& v) {
    const UINT64 heapGpu = focus.gameHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    if (table.ptr < heapGpu)
        return false;
    const auto it = focus.srvInfo.find(game_cpu(static_cast<UINT>((table.ptr - heapGpu) / focus.increment)).ptr);
    if (it == focus.srvInfo.end())
        return false;
    r = it->second.first;
    v = it->second.second;
    return true;
}

// At the game's Close of its post list (tonemap and fxaa drawn): per frame, the eye's veil (bloom) resampled through
// each focus box into our veil; the game's tonemap replayed into our focus image (our colour and veil; the game's
// exposure, grading LUT; its constants with our size); its fxaa into our final focus image. Our slots: 56 our colour
// twin's SRV, 57 the game veil's SRV, 58/59 our veil UAV/SRV, 60 our tonemapped image's SRV.
bool focus_dlss_record(List* l, const Stream& s, ID3D12Resource* colour, UINT fw2, UINT fh, const float (&centre)[2][2]);
bool focus_aa_record(List* l, const Stream& s, UINT fw2, UINT fh, const float (&centre)[2][2]);

void focus_post_record(List* gameList, const Stream& s) {
    Replay* r = nullptr;
    UINT fw2 = 0, fh = 0;
    ID3D12Resource* colour = nullptr;
    float centre[2][2] = {};
    std::string why;
    AcquireSRWLockExclusive(&focus.lock);
    const Twin* twin = nullptr;
    const Twin* colourMs = nullptr;  // MSAA: the multisampled colour twin, resolved into ours first
    for (const auto& [game, tw] : focus.twins) {
        if (!tw.depth && tw.name == "msaaColor" && tw.gameDesc.SampleDesc.Count == 1)
            twin = &tw;
        else if (!tw.depth && tw.name == "msaaColor")
            colourMs = &tw;
    }
    if (!twin && colourMs) {
        if (!focus.resolvedColour || focus.resolvedColourTwin.eye.Get() != colourMs->focus.Get()) {
            retire(focus.resolvedColour.Get());
            focus.resolvedColour.Reset();
            D3D12_RESOURCE_DESC d = colourMs->focus->GetDesc();
            d.SampleDesc = {1, 0};
            d.Alignment = 0;  // the multisampled twin's 4 MB placement alignment is refused for one sample
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                          nullptr, IID_PPV_ARGS(&focus.resolvedColour)))) {
                focus.resolvedColourTwin = *colourMs;
                focus.resolvedColourTwin.focus = focus.resolvedColour;
                focus.resolvedColourTwin.eye = colourMs->focus;  // here: the multisampled source
                focus.resolvedColourTwin.gameDesc.SampleDesc = {1, 0};
                logf("replay focus: msaa: our resolved focus colour %llux%u (from the %u-sample twin)",
                     static_cast<unsigned long long>(d.Width), d.Height, colourMs->gameDesc.SampleDesc.Count);
            } else {
                static bool failLogged = false;
                if (!failLogged) {
                    failLogged = true;
                    log_line("replay focus: msaa: could not make our resolved focus colour");
                }
            }
        }
        if (focus.resolvedColour)
            twin = &focus.resolvedColourTwin;
    }
    if (focus.postBroken || focus.tilesBroken || !focus.gameHeap || !focus.slotFirst || !focus.cbPatchPso ||
        !focus.atmoPso[0] || !focus.centresKnown || !twin) {
        why = "not ready (tiles, centres or the colour twin)";
    } else {
        fw2 = 2 * twin->focusW;
        fh = twin->focusH;
        colour = twin->focus.Get();
        focus_centres(centre);
        if (!focus.postLdr || fw2 != focus.postW || fh != focus.postH || focus.postColour != colour ||
            focus.postFxaa != s.hasFxaa) {
            ComPtr<ID3D12Resource> veil, fxaaTex;
            D3D12_SHADER_RESOURCE_VIEW_DESC veilView{}, fxaaView{};
            // Without ACE's fxaa draw (its menus; frame-600 17:19 capture) the tonemap's target is the final image: our
            // targets take its format, and the tonemap is replayed straight into our final focus image.
            const auto tonemapRt = focus.views.find(s.tonemapDraw.rtv);
            const auto fxaaRt = s.hasFxaa ? focus.views.find(s.fxaaDraw.rtv) : tonemapRt;
            bool fxaaSeen = tonemapRt != focus.views.end() && fxaaRt != focus.views.end();
            if (fxaaSeen && s.hasFxaa) {
                fxaaSeen = table_view(s.fxaaDraw.tables[1], fxaaTex, fxaaView);
            } else if (fxaaSeen) {
                fxaaTex = tonemapRt->second.resource;
                fxaaView.Format = tonemapRt->second.viewFormat;
                fxaaView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                fxaaView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                fxaaView.Texture2D.MipLevels = 1;
                fxaaSeen = fxaaTex != nullptr;
            }
            if (!fxaaSeen || !table_view(s.tonemapDraw.tables[3], veil, veilView)) {
                why = "the game's veil / fxaa_texture views or render targets not seen";
            } else {
                for (ID3D12Resource* old : {focus.postLdr.Get(), focus.postFinal.Get(), focus.postVeil.Get()})
                    retire(old);
                if (focus.postLdrRtvSlot < 0)
                    focus.postLdrRtv = view_slot(false, focus.postLdrRtvSlot);
                if (focus.postFinalRtvSlot < 0)
                    focus.postFinalRtv = view_slot(false, focus.postFinalRtvSlot);
                D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
                D3D12_RESOURCE_DESC vd = veil->GetDesc();
                vd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                vd.MipLevels = 1;
                D3D12_RESOURCE_DESC ld = fxaaTex->GetDesc();
                ld.Width = fw2;
                ld.Height = fh;
                ld.MipLevels = 1;
                ld.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_RESOURCE_DESC fd = ld;
                fd.Format = fxaaRt->second.viewFormat;
                // ACE's fxaa_texture is typed UNORM with _SRGB views (format casting set up by the game): ours are
                // typeless, which allows both.
                const auto typeless = [](DXGI_FORMAT f) {
                    switch (f) {
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
                    default:
                        return f;
                    }
                };
                ld.Format = typeless(ld.Format);
                fd.Format = typeless(fd.Format);
                bool ok = focus.postLdrRtv.ptr && focus.postFinalRtv.ptr;
                ComPtr<ID3D12Resource> v2, l2, f2;
                ok = ok &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &vd,
                                                               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                               IID_PPV_ARGS(&v2))) &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ld,
                                                               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                               IID_PPV_ARGS(&l2))) &&
                     SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &fd,
                                                               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                               IID_PPV_ARGS(&f2)));
                if (!ok) {
                    log_line("replay focus: post: could not make our post targets; no focus post");
                    focus.postBroken = true;
                } else {
                    inReplay = true;
                    D3D12_SHADER_RESOURCE_VIEW_DESC cv{};
                    cv.Format = twin->viewFormat;
                    cv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    cv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    cv.Texture2D.MipLevels = 1;
                    srvViewOriginal(device.Get(), colour, &cv, game_cpu(focus.slotFirst + 56));
                    srvViewOriginal(device.Get(), veil.Get(), &veilView, game_cpu(focus.slotFirst + 57));
                    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
                    u.Format = vd.Format;
                    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    device->CreateUnorderedAccessView(v2.Get(), nullptr, &u, game_cpu(focus.slotFirst + 58));
                    D3D12_SHADER_RESOURCE_VIEW_DESC ov = veilView;
                    ov.Texture2D.MostDetailedMip = 0;
                    ov.Texture2D.MipLevels = 1;
                    srvViewOriginal(device.Get(), v2.Get(), &ov, game_cpu(focus.slotFirst + 59));
                    D3D12_SHADER_RESOURCE_VIEW_DESC lv = fxaaView;
                    lv.Texture2D.MostDetailedMip = 0;
                    lv.Texture2D.MipLevels = 1;
                    srvViewOriginal(device.Get(), l2.Get(), &lv, game_cpu(focus.slotFirst + 60));
                    D3D12_RENDER_TARGET_VIEW_DESC rv{};
                    rv.Format = tonemapRt->second.viewFormat;
                    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    rtvOriginal(device.Get(), l2.Get(), &rv, focus.postLdrRtv);
                    rv.Format = fxaaRt->second.viewFormat;
                    rtvOriginal(device.Get(), f2.Get(), &rv, focus.postFinalRtv);
                    inReplay = false;
                    focus.postVeil = v2;
                    focus.postLdr = l2;
                    focus.postLdrRtvFormat = rv.Format == fxaaRt->second.viewFormat ? tonemapRt->second.viewFormat : rv.Format;
                    for (ComPtr<ID3D12Resource>& a : focus.postAlt) {
                        retire(a.Get());
                        a.Reset();
                    }
                    focus.postFinal = f2;
                    focus.postW = fw2;
                    focus.postH = fh;
                    focus.postColour = colour;
                    focus.postFxaa = s.hasFxaa;
                    focus.postGameFxaa = s.hasFxaa ? fxaaTex : nullptr;  // the samples' comparison: only ACE's fxaa_texture
                    focus.postGameFxaaFormat = fxaaView.Format;
                    if (!s.hasFxaa)
                        log_line("replay focus: post: ACE's post has no fxaa draw (its menus): the tonemap goes straight "
                                 "into our final focus image");
                    logf("replay focus: post: our focus post targets %ux%u (tonemap format %d, final %d), veil %llux%u",
                         fw2, fh, static_cast<int>(rv.Format == fxaaRt->second.viewFormat ? tonemapRt->second.viewFormat
                                                                                          : rv.Format),
                         static_cast<int>(fxaaRt->second.viewFormat), static_cast<unsigned long long>(vd.Width), vd.Height);
                }
            }
        }
        if (focus.postLdr && !focus.postBroken && focus.settings.dlss >= 2 && !focus.postAlt[0]) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            const D3D12_RESOURCE_DESC ld = focus.postLdr->GetDesc();
            inReplay = true;
            for (int a = 0; a < 2; ++a) {
                if (focus.postAltRtvSlot[a] < 0)
                    focus.postAltRtv[a] = view_slot(false, focus.postAltRtvSlot[a]);
                if (!focus.postAltRtv[a].ptr ||
                    FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ld,
                                                           D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                           IID_PPV_ARGS(&focus.postAlt[a])))) {
                    focus.postAlt[a].Reset();
                    continue;
                }
                D3D12_RENDER_TARGET_VIEW_DESC rv{};
                rv.Format = focus.postLdrRtvFormat;
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvOriginal(device.Get(), focus.postAlt[a].Get(), &rv, focus.postAltRtv[a]);
            }
            inReplay = false;
            if (focus.postAlt[0] && focus.postAlt[1])
                log_line("replay focus: dlss A/B: the same frame's tonemap also from our colour and from DLSS without CAS");
        }
        if (focus.postLdr && !focus.postBroken)
            r = acquire_replay();
    }
    static int missLogged = 0;
    if (!r && !why.empty() && missLogged < 3 && focus.twins.size() > 4) {
        ++missLogged;
        logf("replay focus: post: not replayed (%s)", why.c_str());
    }
    ReleaseSRWLockExclusive(&focus.lock);
    if (!r)
        return;
    inReplay = true;
    r->allocator->Reset();
    List* l = r->list.Get();
    l->Reset(r->allocator.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = {focus.gameHeap};
    l->SetDescriptorHeaps(1, heaps);
    // 0. MSAA: our multisampled colour twin resolved into our single-sample colour (as ACE resolves msaaColor into
    // upscaling before its DLSS).
    if (colour == focus.resolvedColour.Get())
        resolve_focus_colour(l);
    // 1. The veil (bloom) through each focus box, both halves.
    const D3D12_RESOURCE_DESC vd = focus.postVeil->GetDesc();
    {
        const float box = static_cast<float>(focus.settings.boxFraction), boxY = focus_box_y();
    const uint32_t boxYBits = float_bits(boxY);
    (void)boxYBits;
        uint32_t p[10] = {};
        const float f0[3] = {box, centre[0][0], centre[0][1]};
        std::memcpy(p, f0, sizeof(f0));
        p[3] = static_cast<uint32_t>(vd.Width);
        p[4] = vd.Height;
        p[5] = 1;
        p[6] = 1;  // stereo: two halves
        std::memcpy(&p[7], &centre[1][0], 4);
        std::memcpy(&p[8], &centre[1][1], 4);
        barrier(l, focus.postVeil.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetComputeRootSignature(focus.atmoRoot.Get());
        l->SetPipelineState(focus.atmoPso[0].Get());
        l->SetComputeRoot32BitConstants(0, 10, p, 0);
        l->SetComputeRoot32BitConstants(0, 1, &boxYBits, 10);  // the box's height (kAtmoShader boxY)
        l->SetComputeRootDescriptorTable(1, game_gpu(focus.slotFirst + 57));
        l->SetComputeRootDescriptorTable(2, game_gpu(focus.slotFirst + 58));
        l->Dispatch((p[3] + 7) / 8, (p[4] + 7) / 8, 1);
        barrier(l, focus.postVeil.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    // 1b. P2a: our DLSS per focus eye (ACE's packing replayed for our motion vectors), read by the tonemap below.
    const bool dlssUsed = focus.settings.dlss && focus_dlss_record(l, s, colour, fw2, fh, centre);
    // 1c. P2c: MSAA without DLSS: ACE's own antialiasing replayed (our colour twin multisampled, ACE's pass present)
    const bool aaUsed = !dlssUsed && colour == focus.resolvedColour.Get() && focus_aa_record(l, s, fw2, fh, centre);
    // 2. The tonemap constants with our size (register 1: height, width, 1/height, 1/width).
    const D3D12_GPU_VIRTUAL_ADDRESS mine = focus.postConstants->GetGPUVirtualAddress() + UINT64{focus.postSlot++ % kPostSlots} * 256;
    {
        uint32_t p[8] = {6, 1, 0, 0};
        const float size[4] = {static_cast<float>(fh), static_cast<float>(fw2), 1.0f / static_cast<float>(fh),
                               1.0f / static_cast<float>(fw2)};
        std::memcpy(&p[4], size, sizeof(size));
        l->SetComputeRootSignature(focus.cbPatchRoot.Get());
        l->SetPipelineState(focus.cbPatchPso.Get());
        l->SetComputeRootConstantBufferView(0, s.tonemapDraw.cbv);
        l->SetComputeRoot32BitConstants(1, 8, p, 0);
        l->SetComputeRootUnorderedAccessView(2, mine);
        l->Dispatch(1, 1, 1);
        barrier(l, focus.postConstants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_VIEWPORT view{0, 0, static_cast<float>(fw2), static_cast<float>(fh), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(fw2), static_cast<LONG>(fh)};
    // 3. The game's tonemap into our focus image.
    barrier(l, colour, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    ID3D12Resource* const tonemapTarget = s.hasFxaa ? focus.postLdr.Get() : focus.postFinal.Get();
    barrier(l, tonemapTarget, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    l->OMSetRenderTargets(1, s.hasFxaa ? &focus.postLdrRtv : &focus.postFinalRtv, FALSE, nullptr);
    l->RSSetViewports(1, &view);
    l->RSSetScissorRects(1, &rect);
    l->SetGraphicsRootSignature(s.tonemapDraw.root);
    l->SetPipelineState(s.tonemapDraw.pso);
    const UINT postSource = aaUsed ? 78 : !dlssUsed ? 56 : focus.casDone ? 73 : 71;
    l->SetGraphicsRootDescriptorTable(0, game_gpu(focus.slotFirst + postSource));
    l->SetGraphicsRootDescriptorTable(1, s.tonemapDraw.tables[1]);
    l->SetGraphicsRootDescriptorTable(2, s.tonemapDraw.tables[2]);
    l->SetGraphicsRootDescriptorTable(3, game_gpu(focus.slotFirst + 59));
    l->SetGraphicsRootDescriptorTable(4, s.tonemapDraw.tables[4]);
    l->SetGraphicsRootDescriptorTable(5, s.tonemapDraw.tables[5]);
    l->SetGraphicsRootConstantBufferView(6, mine);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);
    barrier(l, tonemapTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    // replay_dlss=2: the same tonemap from our colour (no DLSS) and from DLSS without CAS (auto-exposure pair: 3)
    if (dlssUsed && focus.postAlt[0] && focus.postAlt[1]) {
        const UINT sources[2] = {56, focus.ngxAuto[0] ? 72u : 71u};
        for (int a = 0; a < 2; ++a) {
            barrier(l, focus.postAlt[a].Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            l->OMSetRenderTargets(1, &focus.postAltRtv[a], FALSE, nullptr);
            l->SetGraphicsRootDescriptorTable(0, game_gpu(focus.slotFirst + sources[a]));
            l->DrawInstanced(3, 1, 0, 0);
            barrier(l, focus.postAlt[a].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
    }
    barrier(l, colour, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // 4. The game's fxaa into our final focus image (rcpFrame ours); not in its menus (no fxaa draw: step 3 wrote it).
    if (s.hasFxaa) {
        barrier(l, focus.postFinal.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        l->OMSetRenderTargets(1, &focus.postFinalRtv, FALSE, nullptr);
        l->SetGraphicsRootSignature(s.fxaaDraw.root);
        l->SetPipelineState(s.fxaaDraw.pso);
        uint32_t consts[8];
        std::memcpy(consts, s.fxaaDraw.consts, sizeof(consts));
        const float rcp[2] = {1.0f / static_cast<float>(fw2), 1.0f / static_cast<float>(fh)};
        std::memcpy(consts, rcp, sizeof(rcp));
        l->SetGraphicsRoot32BitConstants(0, 8, consts, 0);
        l->SetGraphicsRootDescriptorTable(1, game_gpu(focus.slotFirst + 60));
        l->DrawInstanced(3, 1, 0, 0);
        barrier(l, focus.postFinal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    barrier(l, focus.postConstants.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->Close();
    inReplay = false;
    AcquireSRWLockExclusive(&focus.lock);
    focus.pending[gameList].push_back(r);
    if (!focus.postReady)
        log_line("replay focus: post: the first focus post replay recorded (bloom, tonemap, fxaa)");
    focus.postReady = true;
    ReleaseSRWLockExclusive(&focus.lock);
    focus.postFrame.store(focus.frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    focus.postReplays.fetch_add(1, std::memory_order_relaxed);
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
                if (scale < 0) {  // already display values (8-bit, tonemapped): written as they are
                    rgb[c] = static_cast<uint8_t>(std::lround(std::clamp(v[i * 3 + c], 0.0f, 1.0f) * 255));
                    continue;
                }
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
    bool shadowOn = true;  // R2c shadow A/B: whether this sample frame's focus lighting used our deferred shadow
    uint32_t phase = 0;    // replay_diag: the sample's index (0-based)
    ComPtr<ID3D12Resource> constantsRead;
    std::vector<SampleTarget> targets;
    std::vector<ComPtr<IUnknown>> keep;  // the copy list and twins until the copy is done
    struct Extra {  // whole textures saved as pictures only (the atmosphere sky maps: ours vs the game's)
        std::string name;
        ComPtr<ID3D12Resource> read;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    std::vector<Extra> extras;
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
        if (t.name.rfind("post", 0) == 0)
            scale = -1;  // display values: raw
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
        } else if (!t.depth && (result.colourPixels == 0 || t.name.rfind("post", 0) == 0)) {  // the largest colour twin; P1 post
            const ReplayFocusCompare kept = result;  // the post comparison logs only
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
            uint64_t eyeCompared = 0, eyeAgree = 0, hoodAgree = 0;
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
                        // Within the eye's 3x3 neighbourhood around the mapped position (bilinear() checked the edges).
                        const long px = static_cast<long>(std::floor(x + result.shiftX + 0.5));
                        const long py = static_cast<long>(std::floor(y + result.shiftY + 0.5));
                        bool inHood = true;
                        for (int c = 0; c < 3 && inHood; ++c) {
                            float lo = 1e30f, hi = -1e30f;
                            for (long dy = -1; dy <= 1; ++dy)
                                for (long dx = -1; dx <= 1; ++dx) {
                                    const long qx = std::clamp(px + dx, 0L, static_cast<long>(t.ew) - 1);
                                    const long qy = std::clamp(py + dy, 0L, static_cast<long>(t.eh) - 1);
                                    const float q = e[(static_cast<size_t>(qy) * eStride + static_cast<size_t>(eye) * t.ew +
                                                       static_cast<size_t>(qx)) * 3 + c];
                                    lo = std::min(lo, q);
                                    hi = std::max(hi, q);
                                }
                            const float tol = 0.03f * std::max(std::fabs(lo), std::fabs(hi)) + 0.003f;
                            inHood = a[c] >= lo - tol && a[c] <= hi + tol;
                        }
                        if (inHood)
                            ++hoodAgree;
                    }
            if (focus.settings.diag) {
                static const char* const kPhase[4] = {"as set", "our fog OFF", "our 3D atmosphere ON",
                                                      "our 3D atmosphere + sky maps ON"};
                logf("replay focus: sample frame %llu: diagnostic phase: %s", static_cast<unsigned long long>(job->frame),
                     kPhase[std::min<uint32_t>(3, job->phase)]);
            }
            if (focus.settings.atmosphere == 2)
                logf("replay focus: sample frame %llu: atmosphere A/B: the focus views %s our atmosphere maps",
                     static_cast<unsigned long long>(job->frame), job->shadowOn ? "used" : "did NOT use");
            if (focus.settings.transparents == 2)
                logf("replay focus: sample frame %llu: transparents A/B: %s",
                     static_cast<unsigned long long>(job->frame), job->shadowOn ? "replayed" : "NOT replayed");
            if (focus.settings.shadows == 2)
                logf("replay focus: sample frame %llu: shadow A/B: the focus lighting %s our deferred shadow",
                     static_cast<unsigned long long>(job->frame), job->shadowOn ? "used" : "did NOT use");
            result.colourEyePixels = eyeCompared;
            result.colourEyeAgree = eyeCompared ? static_cast<double>(eyeAgree) / static_cast<double>(eyeCompared) : 0;
            result.colourHoodAgree = eyeCompared ? static_cast<double>(hoodAgree) / static_cast<double>(eyeCompared) : 0;
            logf("replay focus: sample frame %llu: colour %s at the eye's pixel size (%ux%u focus pixels each): %llu "
                 "compared, %.1f%% agree; %.1f%% within the eye's 3x3 neighbourhood (edge-tolerant)",
                 static_cast<unsigned long long>(job->frame), t.name.c_str(), k, k,
                 static_cast<unsigned long long>(eyeCompared), result.colourEyeAgree * 100, result.colourHoodAgree * 100);
            logf("replay focus: sample frame %llu: colour %s: %llu pixels compared, %.1f%% agree (every channel within 3%%)",
                 static_cast<unsigned long long>(job->frame), t.name.c_str(), static_cast<unsigned long long>(compared),
                 result.colourAgree * 100);
            if (t.name.rfind("post", 0) == 0)
                result = kept;  // the scene colour's result stays the one reported
        }
    }
    for (const SampleJob::Extra& x : job->extras) {  // pictures only, each at its own median exposure
        const std::vector<float> v = read_back(x.read.Get(), x.foot, x.format, false);
        if (v.empty())
            continue;
        std::vector<float> sorted;
        for (size_t i = 0; i < v.size(); i += 7)
            if (v[i] > 0) sorted.push_back(v[i]);
        std::sort(sorted.begin(), sorted.end());
        const float scale = sorted.empty() ? 1.0f : 0.18f / std::max(1e-6f, sorted[sorted.size() / 2]);
        double mean[3] = {};
        for (size_t i = 0; i < v.size() / 3; ++i)
            for (int c = 0; c < 3; ++c) mean[c] += v[i * 3 + c];
        const double n = static_cast<double>(std::max<size_t>(1, v.size() / 3));
        double alpha = -1;
        if (x.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {  // the alpha (cloud coverage / transmittance)
            void* m = nullptr;
            if (SUCCEEDED(x.read->Map(0, nullptr, &m))) {
                double sum = 0;
                for (UINT y = 0; y < x.foot.Footprint.Height; ++y)
                    for (UINT xx = 0; xx < x.foot.Footprint.Width; ++xx) {
                        uint16_t h;
                        std::memcpy(&h, static_cast<const uint8_t*>(m) + x.foot.Offset + size_t{y} * x.foot.Footprint.RowPitch +
                                            size_t{xx} * 8 + 6,
                                    2);
                        sum += half_to_float(h);
                    }
                x.read->Unmap(0, nullptr);
                alpha = sum / (static_cast<double>(x.foot.Footprint.Width) * x.foot.Footprint.Height);
            }
        }
        if (alpha >= 0)
            logf("replay focus: sample frame %llu: %s alpha mean %.4f", static_cast<unsigned long long>(job->frame),
                 x.name.c_str(), alpha);
        logf("replay focus: sample frame %llu: %s %ux%u mean (%.4f, %.4f, %.4f), median %.4f",
             static_cast<unsigned long long>(job->frame), x.name.c_str(), x.foot.Footprint.Width, x.foot.Footprint.Height,
             mean[0] / n, mean[1] / n, mean[2] / n, sorted.empty() ? 0.0f : sorted[sorted.size() / 2]);
        wchar_t name[160];
        std::swprintf(name, 160, L"frame-%llu-%hs.png", static_cast<unsigned long long>(job->frame), x.name.c_str());
        save_picture((dir / name).wstring(), v, x.foot.Footprint.Width, x.foot.Footprint.Height, false, scale);
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
    job->shadowOn = (focus.samplesTaken.load() + 1) % 2 == 0;  // counted at the sample frame's end: k + 1 here
    job->phase = focus.samplesTaken.load() - 1;
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
        // depth first, then the largest, then ACE's scene targets by name (same-size flagsDepth, oit*, velocity)
        if (a->depth != b->depth)
            return a->depth;
        if (a->eyeW != b->eyeW)
            return a->eyeW > b->eyeW;
        const auto scene = [](const Twin* t) { return t->name == "msaaDepth" || t->name == "msaaColor"; };
        return scene(a) && !scene(b);
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
    if (focus.skyCloudViews && focus.skyClouds) {  // our focus clouds (the sky patch), as a picture with its alpha logged
        SampleJob::Extra x;
        x.name = "clouds-ours";
        const D3D12_RESOURCE_DESC d = focus.skyClouds->GetDesc();
        x.format = d.Format;
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
        x.read = buffer(bytes);
        if (x.read) {
            barrier(l.Get(), focus.skyClouds.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = x.foot;
            D3D12_TEXTURE_COPY_LOCATION src{focus.skyClouds.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), focus.skyClouds.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            job->keep.emplace_back(focus.skyClouds.Get());
            job->extras.push_back(std::move(x));
        }
    }
    if (focus.atmoMade) {  // the sky maps of eye 0 (ours resampled, the game's): pictures for the sky investigation
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        for (int m : {2, 3}) {
            for (int which = 0; which < 2; ++which) {
                ID3D12Resource* res = which ? focus.atmoGame[m] : focus.atmo[m].Get();
                if (!res)
                    continue;
                SampleJob::Extra x;
                x.name = std::string(m == 2 ? "skyIn" : "skyOut") + (which ? "-game" : "-ours");
                const D3D12_RESOURCE_DESC d = res->GetDesc();
                x.format = d.Format;
                UINT64 bytes = 0;
                device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
                x.read = buffer(bytes);
                if (!x.read)
                    continue;
                const D3D12_RESOURCE_STATES state = which ? read : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint = x.foot;
                D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                src.SubresourceIndex = 0;
                l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
                job->keep.emplace_back(res);
                job->extras.push_back(std::move(x));
            }
        }
    }
    {  // MSAA: our refraction source's mip 0 (the opaque scene, resolved at the blur) and our resolved colour (the post's)
        const std::tuple<ID3D12Resource*, const char*, D3D12_RESOURCE_STATES> extra[2] = {
            {focus.blur.Get(), "blur0-ours", D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
            {focus.resolvedColour.Get(), "colour-resolved", D3D12_RESOURCE_STATE_RENDER_TARGET}};
        for (const auto& [res, nm, state] : extra) {
            if (!res || !focus.resolvedColour)
                continue;
            SampleJob::Extra x;
            x.name = nm;
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            x.format = d.Format;
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
            x.read = buffer(bytes);
            if (!x.read)
                continue;
            barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = x.foot;
            D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            job->keep.emplace_back(res);
            job->extras.push_back(std::move(x));
        }
    }
    if (focus.dlssReady && focus.dlssOut && focus.packAa) {  // P2a: our DLSS image and packed motion vectors
        for (int which = 0; which < 2; ++which) {
            ID3D12Resource* res = which ? focus.packAa.Get() : focus.dlssOut.Get();
            if (which == 1 && focus.ngxAuto[0])
                res = focus.dlssOutAuto.Get();
            else if (which == 1 && focus.casDone)
                res = focus.casOut.Get();
            const D3D12_RESOURCE_STATES state =
                which && !focus.ngxAuto[0] && !focus.casDone ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                             : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            SampleJob::Extra x;
            x.name = which ? (focus.ngxAuto[0] ? "dlss-auto" : focus.casDone ? "dlss-cas" : "motion-ours") : "dlss-ours";
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            x.format = d.Format;
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
            x.read = buffer(bytes);
            if (!x.read)
                continue;
            barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = x.foot;
            D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            job->keep.emplace_back(res);
            job->extras.push_back(std::move(x));
        }
    }
    if (focus.postReady && focus.postLdr && focus.postGameFxaa) {  // P1: our tonemapped focus vs the game's eyes
        const Twin* colourTwin = nullptr;
        for (const auto& [game, t] : focus.twins)
            if (!t.depth && t.name == "msaaColor" && t.gameDesc.SampleDesc.Count == 1)
                colourTwin = &t;
        if (!colourTwin && focus.resolvedColour)
            colourTwin = &focus.resolvedColourTwin;
        const std::pair<ID3D12Resource*, const char*> posts[3] = {
            {focus.postLdr.Get(), "post"}, {focus.postAlt[0].Get(), "post-nodlss"},
            {focus.postAlt[1].Get(), focus.ngxAuto[0] ? "post-auto" : "post-nocas"}};
        for (const auto& [postRes, postName] : posts) {
        if (colourTwin && postRes) {
            SampleTarget s;
            s.name = postName;
            s.format = focus.postGameFxaaFormat;
            s.fw = focus.postW / 2;
            s.fh = focus.postH;
            s.ew = colourTwin->eyeW;
            s.eh = colourTwin->eyeH;
            const D3D12_RESOURCE_DESC fd = postRes->GetDesc(), ed = focus.postGameFxaa->GetDesc();
            UINT64 fb = 0, eb = 0;
            device->GetCopyableFootprints(&fd, 0, 1, 0, &s.focusFoot, nullptr, nullptr, &fb);
            device->GetCopyableFootprints(&ed, 0, 1, 0, &s.eyeFoot, nullptr, nullptr, &eb);
            s.focusRead = buffer(fb);
            s.eyeRead = buffer(eb);
            if (s.focusRead && s.eyeRead && ed.Width == UINT64{2} * s.ew && ed.Height == s.eh) {
                // ours rests as a shader resource, the game's fxaa_texture as a render target (end of its post list)
                const std::tuple<ID3D12Resource*, ID3D12Resource*, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_RESOURCE_STATES>
                    pairs[2] = {{postRes, s.focusRead.Get(), s.focusFoot, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
                                {focus.postGameFxaa.Get(), s.eyeRead.Get(), s.eyeFoot, D3D12_RESOURCE_STATE_RENDER_TARGET}};
                for (const auto& [res, read, foot, state] : pairs) {
                    barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION dst{read, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                    dst.PlacedFootprint = foot;
                    D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                    src.SubresourceIndex = 0;
                    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
                    job->keep.emplace_back(res);
                }
                job->targets.push_back(std::move(s));
            }
        }
        }
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
        logf("replay focus: sky: %llu pipelines seen being made or loaded", static_cast<unsigned long long>(n));
}
void note_sky_pso(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& ps) {
    if (!pso || !is_sky_shader(ps))
        return;
    AcquireSRWLockExclusive(&focus.psoLock);
    const bool first = focus.skyPsos.empty();
    focus.skyPsos.insert(pso);
    ReleaseSRWLockExclusive(&focus.psoLock);
    if (first)
        log_line("replay focus: sky: the sky's pipeline seen (its pixel shader declares cbSky)");
}
// ACE's tyre pipelines, by the vertex shader's signatures (plain text in the container; the cbuffer name
// bindlessVehicleTyres sits in bitcode): a skinned scene vertex shader (BONEWEIGHTS, MATERIALID, SV_Position) that takes
// no SV_InstanceID and writes no SV_ViewportArrayIndex, where ACE's stereo vertex shaders do both.
void note_tyre_pso(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& vs) {
    if (!pso)
        return;
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
        logf("replay focus: tyres: ACE's tyre pipeline %zu seen (%p; no single-pass-stereo variant: one viewport)", count,
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
void note_stream_pso(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* pso) {
    count_pipeline();
    if (desc && desc->pPipelineStateSubobjectStream) {
        // Each subobject: its type (a 32-bit enum at a pointer-aligned offset), then its struct, pointer-aligned.
        const uint8_t* p = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        for (size_t i = 0; i + 8 + sizeof(D3D12_SHADER_BYTECODE) <= desc->SizeInBytes; i += 8) {
            uint32_t type;
            std::memcpy(&type, p + i, 4);
            if (type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS && type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS)
                continue;
            D3D12_SHADER_BYTECODE bc;
            std::memcpy(&bc, p + i + 8, sizeof(bc));
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                note_tyre_pso(static_cast<ID3D12PipelineState*>(pso), bc);
                continue;
            }
            if (is_sky_shader(bc)) {
                note_sky_pso(static_cast<ID3D12PipelineState*>(pso), bc);
                break;
            }
        }
    }
}
void STDMETHODCALLTYPE cbvViewHook(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    cbvViewOriginal(d, desc, dest);
    if (inReplay)
        return;
    AcquireSRWLockExclusive(&focus.cbvLock);
    focus.cbvInfo[dest.ptr] = desc ? *desc : D3D12_CONSTANT_BUFFER_VIEW_DESC{};
    ReleaseSRWLockExclusive(&focus.cbvLock);
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
    if (probeMode == 3) {
        if (!focusActive.load(std::memory_order_relaxed))
            return hr;  // cost A/B: the game's own frame only
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
        if (SUCCEEDED(hr) && transparents_now() && !s->stereo && s->monoSeen && s->draws > 0 && monoLogged < 24) {
            const bool taken = !s->oitDraw && !s->monoMixed && !s->passConstants.empty() && (s->sawRtv || s->sawDsv) &&
                               s->monoW == focus.sceneEyeW.load() && s->monoH == focus.sceneEyeH.load();
            ++monoLogged;
            logf("replay focus: per-eye candidate: viewport x %.0f %.0fx%.0f (scene eye %.0fx%.0f), draws %u, pass "
                 "constants %zu, targets %zu, rtv %d dsv %d, mixed %d, oit %d: %s",
                 s->monoX, s->monoW, s->monoH, focus.sceneEyeW.load(), focus.sceneEyeH.load(), s->draws,
                 s->passConstants.size(), s->targets.size(), s->sawRtv ? 1 : 0, s->sawDsv ? 1 : 0, s->monoMixed ? 1 : 0,
                 s->oitDraw ? 1 : 0, taken ? "replayed" : "skipped");
        }
        if (SUCCEEDED(hr) && s->monoSeen && s->draws > 0 && s->monoW == focus.sceneEyeW.load() &&
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
                   s->monoW == focus.sceneEyeW.load() && s->monoH == focus.sceneEyeH.load()) {  // one eye of the scene
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
    if (Stream* s = stream_of(l)) { HookTimer t(s); s->op(kRootSig); s->put(r); s->gfxRoot = r; }
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
        s->vpCount = n;
        if (n == 2) std::memcpy(s->vp, v, sizeof(s->vp));
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
    computeConstNOriginal(l, i, n, data, o);
}
void STDMETHODCALLTYPE computeCbvHook(List* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (Stream* s = stream_of(l); s && i < 8) {
        if (i == 0) s->computeCbv = va;
        s->computeCbvs[i] = va;
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
    }
    drawOriginal(l, a, b, c, d);
}
void STDMETHODCALLTYPE drawIndexedHook(List* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (Stream* s = stream_of(l)) {
        HookTimer t(s);
        s->op(kDrawIndexed); s->put(a); s->put(b); s->put(c); s->put(d); s->put(e);
        ++s->draws;
        note_sky_draw(s);
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
                logf("replay focus: tyres: one-instance draw in a stereo list: pipeline %p, %u indices, drawn again %d, creation "
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
            tyre_right_draw(l, s->tyreConstants, s->gfxCbvs[kPassConstantsRoot], s->pso, s->vp, s->vpCount, s->sc, s->scCount,
                            a, c, d, e);
            inReplay = false;
            if (!focus.tyreGameLogged.exchange(true))
                logf("replay focus: tyres: ACE's tyre draws drawn again for the right eye (viewport %.0f, %.0f %.0fx%.0f)",
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

namespace {
bool skyHooksEarly = false;
}

void replay_probe_early_sky() {
    if (skyHooksEarly)
        return;
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create = d3d12 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    ComPtr<ID3D12Device1> d;
    if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d)))) {
        log_line("replay focus: sky: no early D3D12 device; the sky's pipeline hooks wait for the session");
        return;
    }
    void** deviceTable = *reinterpret_cast<void***>(d.Get());
    bool ok = hook(deviceTable, simqvfr_slot_CreateConstantBufferView(), &cbvViewHook, &cbvViewOriginal) &&
              hook(deviceTable, simqvfr_slot_CreateGraphicsPipelineState(), &gfxPsoHook, &gfxPsoOriginal) &&
              hook(deviceTable, simqvfr_slot_CreatePipelineState(), &streamPsoHook, &streamPsoOriginal);
    ComPtr<ID3D12PipelineLibrary1> lib;
    if (ok && SUCCEEDED(d->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib)))) {
        void** libTable = *reinterpret_cast<void***>(lib.Get());
        ok = hook(libTable, simqvfr_slot_LoadGraphicsPipeline(), &libGfxHook, &libGfxOriginal) &&
             hook(libTable, simqvfr_slot_LoadPipeline(), &libStreamHook, &libStreamOriginal);
    }
    skyHooksEarly = ok;
    logf("replay focus: sky: early hooks (pipeline creation and cache loads, constant-buffer views) %s",
         ok ? "installed at instance creation" : "FAILED");
}

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
        focus.sizeK = settings.boxFraction * settings.pixelScale;
        focus.sizeKy = (settings.boxFractionY > 0 ? settings.boxFractionY : settings.boxFraction) *
                       (settings.pixelScaleY > 0 ? settings.pixelScaleY : settings.pixelScale);
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
        if (ok && ((settings.sky && settings.tiles) || settings.tyres) && !skyHooksEarly)
            ok = hook(deviceTable, simqvfr_slot_CreateConstantBufferView(), &cbvViewHook, &cbvViewOriginal) &&
                 hook(deviceTable, simqvfr_slot_CreateGraphicsPipelineState(), &gfxPsoHook, &gfxPsoOriginal) &&
                 hook(deviceTable, simqvfr_slot_CreatePipelineState(), &streamPsoHook, &streamPsoOriginal);
        if (ok && ((settings.sky && settings.tiles) || settings.tyres) && !skyHooksEarly) {
            ComPtr<ID3D12Device1> d1;
            ComPtr<ID3D12PipelineLibrary1> lib;
            if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d1))) &&
                SUCCEEDED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib)))) {
                void** libTable = *reinterpret_cast<void***>(lib.Get());
                if (!hook(libTable, simqvfr_slot_LoadGraphicsPipeline(), &libGfxHook, &libGfxOriginal) ||
                    !hook(libTable, simqvfr_slot_LoadPipeline(), &libStreamHook, &libStreamOriginal))
                    log_line("replay focus: sky: could not hook the pipeline library");
            } else {
                log_line("replay focus: sky: no pipeline library on this device (pipelines from a cache not seen)");
            }
        }
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
        {  // the next frame's box centre: the gaze asked for, clamped so the box stays inside the eye's image
            AcquireSRWLockExclusive(&focus.lock);
            std::memcpy(focus.gazePrev, focus.gazeOffset, sizeof(focus.gazePrev));
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
            logf("replay focus: sky diagnostic: sample frame %llu: sky maps %s, cbSky %s",
                 static_cast<unsigned long long>(ended), (ph & 1) ? "ours" : "ACE's", (ph & 2) ? "ours" : "ACE's");
        }
        if (ended == focus.sampleFrame.load())
            logf("replay focus: frame %llu per-eye lists: taken L %u R %u, replayed L %u R %u, unfit %u, broken %u; "
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

}  // namespace simqvfr

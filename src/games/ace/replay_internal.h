// replay_probe.h's internals, shared by its files (replay_detail: not for other code). ACE's API path: our hooks
// record the game's command lists (Stream), the focus replays are recorded from them into lists of ours (Replay)
// and run with the game's; one state for the focus views, `focus` (focus.lock where its members say so).
//   replay_probe.cpp        the recording hooks, stream recording and emit, the live periphery's per-draw parts, queue
//                           execute and GPU markers, the start, the frame's end, stats (everything called per
//                           recorded command or per draw stays here: the build has no LTCG)
//   replay_focus.cpp        the focus core: compile, twins and view slots, retire / prune, rebuild, prepare, the focus
//                           scene replay (focus_record)
//   replay_lighting.cpp     per pass: light tiles, sky, volumetric fog, deferred sun shadow, atmosphere maps
//   replay_transparents.cpp per pass: the OIT composite, the refraction source (blur), the previous-frame colour
//   replay_post.cpp         per pass: MSAA resolves, our DLSS, MSAA antialiasing, bloom / tonemap / fxaa
//   replay_eye_live.cpp     the live periphery's per-frame parts: its pipeline, the constants-copy lists
//   replay_cull.cpp         focus culling's record: ACE's instance visibility pass again for the focus views
//   replay_samples.cpp      samples: twins and their eye references read back, saved as pictures, compared
//   replay_shaders.h        the HLSL sources of our compute passes and our slot / descriptor layout constants
#pragma once

#include "replay_probe.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../probe/focus_ngx.h"

namespace simqvfr::replay_detail {

using List = ID3D12GraphicsCommandList;
using Microsoft::WRL::ComPtr;

using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using UavFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE);

enum Op : uint8_t {
    kRootSig = 1, kPso, kHeaps, kTable, kConst1, kConstN, kCbv, kSrv, kTopology, kIb, kVb, kViewports, kScissors,
    kStencil, kDraw, kDrawIndexed, kIndirect, kOm
};

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
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> frameConstants;  // distinct root-0 addresses (frameData: tileCount)
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
    ID3D12RootSignature* computeCbvRoots[8] = {};  // the compute root signature each was set under
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
    // ACE's instance visibility pass (focus culling): root 0's constants as last set (visibilityGlobals, 52 values), its
    // culling and finalize dispatches, and the buffer copies of the list (the count and command templates).
    uint32_t visConsts[64] = {};
    struct VisDispatch {
        int kind;  // 25 cull, 26 its directional-shadow variant, 27 finalize
        ID3D12PipelineState* pso;
        ID3D12RootSignature* root;
        ID3D12DescriptorHeap* heap;
        uint32_t consts[52];
        D3D12_GPU_DESCRIPTOR_HANDLE tables[10];
        UINT dims[3];
    };
    std::vector<VisDispatch> visDispatches;
    struct BufferCopy {
        ID3D12Resource* dst;
        UINT64 dstOffset;
        ID3D12Resource* src;
        UINT64 srcOffset, bytes;
    };
    std::vector<BufferCopy> bufferCopies;
    // The live periphery (eye_apply): every viewport and scissor as the game last set them, what of ours is in effect
    // in the game's own list, and the constants this list needs patched before it runs.
    UINT vpN = 0, scN = 0;
    D3D12_VIEWPORT vpAll[4] = {};
    D3D12_RECT scAll[4] = {};
    bool eyeDirty = false, eyeScaled = false, eyeCbv1 = false, eyeCbv5 = false, eyeClipped = false;
    ID3D12RootSignature* cbv1Root = nullptr;   // the root signature root 1's address was set under
    ID3D12RootSignature* cbv5Root = nullptr;   // and root 5's (deferredShadows' constants)
    ID3D12RootSignature* sceneRoot = nullptr;  // ACE's scene root signature (its table 18 was set under it)
    uint64_t eyeState = 0;                     // the live size this list records with (0: off), taken at its Reset
    uint64_t eyeOut = 0;                       // and the region after the game's DLSS upscale (eye_out), taken with it
    float eyeBias = 0;                         // the periphery texture bias this list records with (0: off), taken with it
    bool eyeFix = false;                       // the right eye's previous projection corrected (fixNow), taken with it
    struct EyePatch {
        D3D12_GPU_VIRTUAL_ADDRESS from, to;
        int mode;    // 0 pass constants, 1 tile culling, 2 normal reconstruction, 3 deferred shadows, 4 velocity packing
        float w, h;  // mode 0: the list's live view size
    };
    std::vector<EyePatch> eyePatches;
    // The draw census (replay_probe_census, a development build's capture hotkey): this list's draws by pipeline,
    // logged at its Close.
    struct CensusPso {
        ID3D12PipelineState* pso;
        uint32_t draws, indexed, indirect, instMin, instMax, oneViewport, twoViewports, again;
        uint64_t elements;
    };
    bool census = false;
    std::vector<CensusPso> censusPsos;
    // ACE's flags pass: its half-width viewport pair was widened (widen_half_stereo). The list draws the left eye only
    // and ACE draws the right eye in a list of its own, so nothing of ours draws its objects again for the right eye.
    bool halfStereo = false;

    void begin(int64_t now) {
        bytes.clear();
        census = false;
        halfStereo = false;
        censusPsos.clear();
        active = true;
        draws = calls = 0;
        maxViewportWidth = 0;
        resetTicks = now;
        hookCycles = 0;
        sawRtv = sawDsv = stereo = false;
        eyeW = eyeH = 0;
        targets.clear();
        passConstants.clear();
        frameConstants.clear();
        psoKind = 0;
        pso = nullptr;
        computeRoot = nullptr;
        heap = nullptr;
        computeCbv = 0;
        bindlessUint = {};
        tileDispatches.clear();
        dispatchPsos.clear();
        visDispatches.clear();
        bufferCopies.clear();
        std::memset(computeCbvs, 0, sizeof(computeCbvs));
        std::memset(computeCbvRoots, 0, sizeof(computeCbvRoots));
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
        vpN = scN = 0;
        eyeDirty = eyeScaled = eyeCbv1 = eyeCbv5 = eyeClipped = false;
        cbv1Root = cbv5Root = sceneRoot = nullptr;
        eyeState = eyeOut = 0;
        eyeBias = 0;
        eyeFix = false;
        eyePatches.clear();
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

struct Replay {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List> list;
    uint64_t fence = 0;  // done when the fence reaches it
    bool busy = false;
    // NGX's lifetime (focus_ngx::lifetime) when it recorded our DLSS evaluations; 0: none in it. Not submitted once
    // the game has shut NGX down since: the features and bags it names are freed.
    uint64_t ngxLife = 0;
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
    // (see retire(): freed only once every list recorded up to a few frames later has run)
    struct Retired {
        uint64_t frame = 0;   // the frame it was retired in
        uint64_t fence = 0;   // once aged: the last fence value signalled by then (0: not aged yet)
        ComPtr<IUnknown> object;
        focus_ngx::Feature* feature = nullptr;  // or one of our NGX features
    };
    std::vector<Retired> retired;
    ComPtr<ID3D12CommandAllocator> clearAllocator, eyeClearAllocator;
    ComPtr<List> clearList, eyeClearList;  // the focus twins every frame; the eye references at sample frames
    // The focus mask (replay_probe_set_focus_mask; quad/focus_mask.h): right after the clear, the focus views' depth
    // twins get the nearest depth (1: reverse-Z) outside the focus shape, so the replays' depth-tested draws skip those
    // pixels. clearList draws it while it is on; plainClearList is the clear alone (the A/B's other side, replay_mask_ab).
    ComPtr<ID3D12CommandAllocator> plainClearAllocator;
    ComPtr<List> plainClearList;
    ComPtr<ID3D12RootSignature> maskRoot;
    ComPtr<ID3D12Resource> maskVb;  // upload heap: every depth twin's mesh, one after another
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> maskPsos;  // by DSV format and sample count
    bool maskFailed = false;        // its shader or root signature couldn't be made: off for the session
    uint64_t maskRecorded = 0;      // the mask settings' epoch the lists were recorded with
    uint32_t maskTwins = 0, maskVertices = 0;  // what the last recording masked (the log)
    double maskShare = 0;           // the share of the scene twin's focus pixels it covers (the log)
    SRWLOCK lock = SRWLOCK_INIT;  // pool, pending, twins, views, clear list
    std::vector<std::unique_ptr<Replay>> pool;
    std::unordered_map<List*, std::vector<Replay*>> pending;  // the game's list -> our replays of it, in order
    std::atomic<size_t> pendingLists{0};  // pending.size(), read without the lock (the game's Reset skips it when 0)
    // After a rebuild (slots_writable): held until the GPU is past slotsFence, from frame slotsHeldFrame; submitting:
    // execute_batch's replays taken from pending and not yet signalled.
    std::atomic<bool> slotsHeld{false};
    uint64_t slotsFence = 0, slotsHeldFrame = 0;
    int submitting = 0;
    std::unordered_map<SIZE_T, ViewInfo> views;  // the game's RTV/DSV CPU handles
    std::unordered_map<ID3D12Resource*, Twin> twins;
    std::vector<SIZE_T> refused;  // handles whose view we never saw (logged once)
    bool viewsMoved = false;      // a view handle now names another resource: twins of orphaned resources go
    uint64_t releaseChecked = UINT64_MAX;  // the frame focus_prepare last looked for game targets only we still hold
    // R2b: the focus views' light tiles. ACE's tile lists are bindless (space 45, root table 18 of its scene root
    // signature, a 131,072-descriptor range): our descriptors take the top kTileSlots of that range in its heap.
    ComPtr<ID3D12RootSignature> cullRoot;
    ComPtr<ID3D12PipelineState> cullPso;      // copies tiledCulling's constants with the focus fields
    // ACE's shader-visible CBV/SRV/UAV heap: held (our lists set it, and the descriptor hooks read its start), the one
    // before retired on a change
    ComPtr<ID3D12DescriptorHeap> gameHeap;
    UINT heapSize = 0, increment = 0;
    // gameHeap's CPU range, for the descriptor hooks' check without the lock (0, 0: none yet)
    std::atomic<SIZE_T> heapCpuLo{0}, heapCpuHi{0};
    D3D12_GPU_DESCRIPTOR_HANDLE bindlessBase{};  // root table 18 as ACE's stereo scene lists set it
    UINT baseIndex = 0, slotFirst = 0;            // absolute heap indices
    UINT tilesX = 0, tilesY = 0;                  // the focus views' tile grid per eye (our lists' size and stride)
    ComPtr<ID3D12PipelineState> framePso;         // kFrameShader on patchRoot
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
    // psoKinds is keyed by address and read from the pipeline's name once. The game makes its pipelines again on a
    // DLSS mode change, and a new pipeline at a freed one's address took the old answer (05:26 run: tiledCulling no
    // longer known after the switch, so no tile replay and nothing after it). After a rebuild the answers are thrown
    // away for this many frames (names read again: one lookup per pipeline a frame), covering pipelines made around it.
    std::atomic<int> psoFlush{0};
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
    // Our DLSS upscaling the focus views (settings.dlssScale < 1): sizeK / sizeKy are the render size (the twins'), and
    // the output size (our DLSS image, the post targets, the image composed) is the render size over this.
    double renderScale = 1.0;
    std::atomic<double> boxYWanted{0};
    std::atomic<double> boxWanted{0};
    // Our sizes asked while running (the headset menu; taken at a frame's end, then focus_rebuild): the focus views'
    // output size as a share of the eye's each way (focus density), and the render share of it (Focus views mode).
    std::atomic<double> outKWanted{0}, outKyWanted{0}, renderScaleWanted{0};
    double outK = 0, outKy = 0;  // as set (sizeK = outK x renderScale)
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
    UINT ngxW = 0, ngxH = 0, ngxOutW = 0, ngxOutH = 0, packOutW = 0, packOutH = 0;
    int ngxPreset = 0;  // focus_ngx::focus_preset() the features were made with
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

// ---- Focus culling (replay_cull) -----------------------------------------------------------------------------------
// The focus replays draw each eye's whole draw list into a view covering a small part of it: ~2.4 ms of GPU a frame in
// geometry, whatever the focus views' size (2026-11-14: 9x fewer focus pixels saved 0.3 ms). About 1.4 ms of it is
// ExecuteIndirect draws, whose instances and draw arguments come from ACE's instance visibility pass. That pass is
// replayed for the focus views: the eyes' culling dispatches again (the ones with dualFrustumCull set: left eye's
// frustum from viewProjectionTransform, right eye's from mainViewProjectionTransform), both matrices remapped to the
// focus boxes, writing our own visible-instances and count buffers; then ACE's finalize into our own command buffer.
// The focus replays' ExecuteIndirect calls and instance vertex buffer views are pointed at ours. Draws ACE issues
// directly are not culled.
struct FocusCull {
    std::atomic<int> mode{0};  // 0 off, 1 computed only, 2 used by the focus replays, 3 used in alternate 900-frame windows
    std::atomic<bool> use{false};  // this window's replays read ours
    std::atomic<uint64_t> gameInstancesVa{0}, gameInstancesBytes{0};  // ACE's visible-instances buffer
    std::atomic<uint64_t> readyFrame{UINT64_MAX};  // the frame ours hold the culling of
    // under focus.lock
    ComPtr<ID3D12Resource> instances, counts, commands;
    ID3D12Resource* gameCommands = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS instancesVa = 0;
    UINT64 instancesBytes = 0, countsBytes = 0, commandsBytes = 0;
    bool broken = false;
    std::atomic<uint64_t> culls{0}, listsCulled{0}, listsWhole{0};
};
constexpr UINT kCullSlot = 130;  // of our descriptors: instances UAV, counts UAV, counts SRV, commands UAV, (its SRV)

// GPU time by step (replay_gpu_steps=1, replay_probe.cpp): what a timestamp starts.
enum : uint8_t {
    kStepScene, kStepMono, kStepTiles, kStepFog, kStepShadow, kStepOit, kStepAtmo, kStepBlur, kStepResolve, kStepPostVeil,
    kStepPostDlss, kStepPostTone, kStepEyeCopies, kStepCull, kStepGame, kStepCompose, kStepGap, kStepKinds
};

// ---- Types shared by the files (defined in replay_probe.cpp before the split)
using DsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE);
using RtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE);
using SrvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE);
using CbvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
// replay_record_timing (diagnostic): focus_record's CPU time by phase, in QPC ticks, summed over threads.
struct RecordTiming {
    bool on = false;
    std::atomic<uint64_t> lists{0}, lockWait{0}, underLock{0}, setup{0}, emit{0}, skyChecks{0}, close{0}, after{0},
        maxList{0}, ops{0}, draws{0};
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
    int skip = 0;  // replay_cull_probe (diagnostic): 1 the indirect draws left out, 2 the direct draws
    // Focus culling: the game's indirect command buffer -> ours, and vertex buffer views inside its visible-instances
    // buffer -> the same place in ours.
    ID3D12Resource* argsFrom = nullptr;
    ID3D12Resource* argsTo = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS vbFrom = 0, vbTo = 0;
    UINT64 vbBytes = 0;
};
struct SkyPatch {
    UINT64 gameTable = 0, ourTable = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gameCbSky = 0, ourCbSky = 0;
};
constexpr UINT kEyeSlots = 512;

struct EyeLive {
    std::atomic<float> wanted{1.0f};   // the periphery's share of its launch size (1: off)
    std::atomic<bool> allowed{false};  // the compositor reads the region in use (a composed frame)
    std::atomic<uint64_t> state{0};    // taken at a frame's end: 0 off, else lw | lh << 16 | ew << 32 | eh << 48
    // with it, when the game's DLSS upscales its eyes (Quality, Balanced, Performance): the region at the output size,
    // as NGX is told it (focus_ngx: the output x fx, rounded), ow | oh << 16 | outW << 32 | outH << 48; 0: no upscale
    std::atomic<uint64_t> out{0};
    // Periphery texture match (replay_probe_set_eye_texture_match): ACE's own eye passes read a copy of their pass
    // constants with globalMaterialMipBias moved by this many mip levels (negative: sharper). ACE's alpha-to-coverage
    // surfaces (fences, lattices) average to a light solid shade at the periphery's low resolution and are see-through
    // in the sharper focus views; a sharper periphery makes the two agree. `biasNow`: taken at a frame's end.
    std::atomic<float> match{0}, mipBias{0}, biasNow{0};
    // The right eye's previous projection corrected (kPatchShader's fixPrev): asked (replay_dlss_right_prev_fix), and
    // as a frame's end took it (with DLSS on and the copies ready). With it ACE's stereo scene draws read our copy.
    std::atomic<bool> rightPrevFix{false}, fixNow{false};
    std::atomic<uint64_t> fixDraws{0};  // stereo scene draws that read our copy for it alone (eye_apply)
    // Detailed logs: ACE's pass constants as given and as our copy has them, for one list a frame over kDumps frames
    // (eye_record copies them to dumpRead; a frame's end logs each once its frame is kDumpAge old).
    static constexpr int kDumps = 8, kDumpAge = 8;
    ComPtr<ID3D12Resource> dumpRaw, dumpRead;  // kDumps x 2 KB (UAV), kDumps x 4 KB (readback, mapped)
    const uint8_t* dumpMapped = nullptr;
    std::atomic<int> dumpLeft{0}, dumpBursts{0};
    std::atomic<uint64_t> dumpFrame{UINT64_MAX}, dumpAt[kDumps] = {};
    ComPtr<ID3D12Resource> constants;  // our copies: kEyeSlots x kSlotBytes
    ComPtr<ID3D12PipelineState> pso;   // kEyeShader
    std::atomic<uint32_t> nextSlot{0};
    std::vector<std::unique_ptr<Replay>> pool;     // under focus.lock
    std::unordered_map<List*, Replay*> before;     // under focus.lock: the patch list each closed game list needs first
    std::atomic<uint64_t> lists{0}, patches{0}, executed{0};
    // what was changed, by kind (the 900-frame line): scene state applied (stereo, one eye), deferred shadows, the
    // tile-frustum constants, tile culling, normals, packing; scene-root draws left alone for their viewport's shape
    // (8 sharpening, 9 histogram, 10 histogramAverage, 11 by-pixel post dispatches shrunk, 12 whole-target draws clipped,
    // 13 MSAA's resolve)
    std::atomic<uint64_t> did[14] = {};
    std::atomic<int> said[14] = {};  // first uses described
    std::atomic<int> shapesSaid{0};
    bool tried = false, ready = false;
    int whyNot = -1;
};

struct EyeNow {
    bool on = false;
    float fx = 1, fy = 1, ew = 0, eh = 0, lw = 0, lh = 0;
};

// The passes after the game's DLSS work at its output size: the region there (the scene's own when it doesn't upscale).
struct EyeOut {
    float w = 0, h = 0, lw = 0, lh = 0;  // the eye at the output size, and the region in use of it
};

// ---- Defined in replay_probe.cpp
extern ComPtr<ID3D12Device> device;
extern thread_local bool inReplay;  // our own calls into a list: the hooks pass them through
extern ExecuteFn executeOriginal;
extern UavFn uavViewOriginal;  // our own UAVs go straight to it (the hook takes focus.lock)
extern Focus focus;
extern FocusCull cull;

extern DsvFn dsvOriginal;
extern RtvFn rtvOriginal;
extern SrvFn srvViewOriginal;
extern CbvFn cbvViewOriginal;
extern std::atomic<uint64_t> listsReplayed, callsReplayed, drawsReplayed, replayTicks, twinsMade, twinsRemade;
// Development builds: live diagnostic switches ([experiment] replay_live_diag, read with the live settings). Bits:
// 1 the depth-fade remap off, 2 the OIT composite not replayed, 4 per-eye lists not replayed, 8 per-eye lists of at
// most 4 draws (the particles' OIT lists) not replayed, 16 the flags pass's viewports not widened.
extern std::atomic<unsigned> liveDiag;
extern RecordTiming recordTiming;
extern thread_local uint64_t skyCheckTicks, emitOps;
extern std::atomic<int> cullProbePhase;
extern EyeLive eyeLive;

std::string debug_name(ID3D12Resource* r);  // here: called per draw (eye_apply) and per SRV write (tile_view_kind)
void gpu_stamp(List* l, uint8_t kind);
void barrier(List* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);  // here: per tyre draw
int64_t now_ticks();
int64_t emit(List* l, const Stream& s, const Substitution* sub);
bool invert(const double (&m)[4][4], double (&out)[4][4]);
EyeNow eye_now(uint64_t state);
EyeOut eye_out(const EyeNow& now, uint64_t out);  // eye_now / eye_out: here, per draw and per dispatch

// ---- replay_focus.cpp
extern std::atomic<int> remakeEpoch;
bool focus_compile();
// The focus mask: under focus.lock, the clear lists recorded again when its settings changed since; the list to run this
// frame (the plain clear on the A/B's other side).
void focus_mask_set(bool on, const FocusArea& area, unsigned marginTexels);  // any thread
void focus_mask_refresh();
List* focus_clear_list(uint64_t frame);
// The A/B (replay_mask_ab): the side of a frame, 0 with the mask, 1 without; and the frame end's log of the windows.
int focus_mask_side(uint64_t frame);
void focus_mask_window_log(uint64_t ended);
D3D12_CPU_DESCRIPTOR_HANDLE view_slot(bool depth, int& index);
void retire(IUnknown* object);  // under focus.lock: kept alive until no list can still use it
void retire_feature(focus_ngx::Feature*& feature);
bool miss_allowed(int& epoch, int& count, int limit);  // the rate-limited "why not" lines
size_t sweep_released(bool* used);
bool slots_writable();
void focus_rebuild(const char* why);
bool focus_prepare(const Stream& s);
Replay* acquire_replay();                         // under focus.lock
void add_pending(List* gameList, Replay* r);      // under focus.lock
float focus_box_y();
uint32_t float_bits(float f);
void focus_centres(float (&centre)[2][2]);        // under focus.lock
void focus_record(List* gameList, const Stream& s, bool mono = false);

// ---- replay_lighting.cpp
D3D12_CPU_DESCRIPTOR_HANDLE game_cpu(UINT index);
D3D12_GPU_DESCRIPTOR_HANDLE game_gpu(UINT index);
void structured_views(ID3D12Resource* r, UINT elements, UINT stride, UINT uavSlot, UINT srvSlot);
const Twin* scene_depth_twin();
D3D12_RESOURCE_STATES rest_of(ID3D12Resource* r, D3D12_RESOURCE_STATES otherwise);
void focus_tiles_record(List* gameList, const Stream& s);
std::vector<SkyPatch> focus_sky_prepare(const Stream& s);
void focus_sky_record(List* l, const std::vector<SkyPatch>& patches, const float (&centre)[2]);
void focus_fog_record(List* gameList, const Stream& s);
void focus_shadow_record(List* gameList, const Stream& s);
void focus_atmo_record(List* gameList, const Stream& s);

// ---- replay_transparents.cpp
void focus_oit_record(List* gameList, const Stream& s);
void resolve_focus_colour(List* l);
void focus_blur_record(List* gameList);
void focus_prev_copy(ID3D12CommandQueue* queue);

// ---- replay_post.cpp
void focus_resolve_record(List* gameList, const Stream& s);
void focus_post_record(List* gameList, const Stream& s);

// ---- replay_eye_live.cpp
bool eye_ready();
// A frame's end: one dump of ACE's pass constants (EyeLive::dumpRead slot k, from frame `at`) logged.
void eye_dump_log(int k, uint64_t at);
Replay* eye_acquire();
void eye_record(List* gameList, const Stream& s);
// The region of a borrowed eye image in use (the live periphery); `output`: an image after the game's DLSS (its output size).
void atmo_source_region(List* l, bool eyeImage, bool output = false);

// ---- replay_samples.cpp
void focus_copy_sample();

// ---- replay_fixation.cpp
// In the focus tile list, the focus depth a shader resource behind `depthSrv` (both eyes side by side, fw x fh each):
// once a frame, the left view's centre texels queued for reading.
void focus_fixation_record(List* l, D3D12_GPU_DESCRIPTOR_HANDLE depthSrv, UINT fw, UINT fh, const float (&inverse)[16]);
void focus_fixation_frame_end(uint64_t ended);
double focus_fixation_m();

// ---- replay_cull.cpp
void focus_cull_record(List* gameList, const Stream& s);

}  // namespace simqvfr::replay_detail

// LMU's multi-view rendering in the eye passes (mvr_pass.h). Render thread only: the hooks act on the game's
// immediate context while a pass runs and pass everything else straight through. They are on every function the
// context's entries hold in d3d11's modes (MinHook; "How the hooks are reached" below).
#include "mvr_pass.h"

#include <windows.h>
#include <d3d11_4.h>
#include <intrin.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../engine/nvapi_stereo.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "../../engine/dlss_views.h"
#include "car_motion.h"
#include "focus_post.h"
#include "glare_math.h"
#include "mvr_probe.h"
#include "tag_viewport.h"
#include "abi.h"
#include "yebis_shaders.h"

namespace lmu {
namespace {

using Microsoft::WRL::ComPtr;

using namespace context_slot;
constexpr unsigned kMaxRects = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

// How the hooks are reached (mvr_pass.h context_slot): d3d11 rewrites the context's vtable entries as it runs, and a
// hook on one function an entry held misses the calls through the others (nvapi_stereo_check 6: once protection has
// been on, the plain functions are other ones, which the locking ones don't call), so each function an entry is seen
// holding is hooked in d3d11's code (MinHook), a detour of its own each: those found at install (the entries as they
// are, with multithread protection on if it was off, each after a flush and after a command) and any new one at a
// pass's start. Our own calls and the calls passed on go to the original of the function the entry holds now.
enum Hook : int {
    kHookVS, kHookGS, kHookOM, kHookOMUav, kHookSO, kHookVP, kHookSC, kHookResolve, kHookHS, kHookDS, kHookDrawIndexed,
    kHookDraw, kHookDrawIndexedInstanced, kHookDrawInstanced, kHookCopy, kHookDrawAuto, kHookDrawIndexedIndirect,
    kHookDrawInstancedIndirect, kHookPS, kHookVSCB, kHookMap, kHookUnmap, kHookIAVB, kHookIAIB, kHookBlend, kHookDepth,
    kHooks
};
constexpr unsigned kSlotOf[kHooks] = {kVSSetShader,   kGSSetShader,    kOMSetRenderTargets, kOMSetRenderTargetsAndUavs,
                                      kSOSetTargets,  kRSSetViewports, kRSSetScissorRects,  kResolveSubresource,
                                      kHSSetShader,   kDSSetShader,    kDrawIndexed,        kDraw,
                                      kDrawIndexedInstanced,           kDrawInstanced,      kCopySubresourceRegion,
                                      kDrawAuto,      kDrawIndexedInstancedIndirect,        kDrawInstancedIndirect,
                                      kPSSetShader,   kVSSetConstantBuffers, kMap, kUnmap,  kIASetVertexBuffers,
                                      kIASetIndexBuffer, kOMSetBlendState, kOMSetDepthStencilState};
constexpr const char* kHookName[kHooks] = {"VSSetShader",    "GSSetShader",       "OMSetRenderTargets",
                                           "OMSetRenderTargetsAndUnorderedAccessViews", "SOSetTargets",
                                           "RSSetViewports", "RSSetScissorRects", "ResolveSubresource",
                                           "HSSetShader",    "DSSetShader",       "DrawIndexed",
                                           "Draw",           "DrawIndexedInstanced", "DrawInstanced",
                                           "CopySubresourceRegion", "DrawAuto", "DrawIndexedInstancedIndirect",
                                           "DrawInstancedIndirect", "PSSetShader", "VSSetConstantBuffers", "Map",
                                           "Unmap", "IASetVertexBuffers", "IASetIndexBuffer", "OMSetBlendState",
                                           "OMSetDepthStencilState"};
// Car tracking's hooks (mvr_car_track).
constexpr bool is_car_hook(int h) {
    return h == kHookVSCB || h == kHookMap || h == kHookUnmap || h == kHookIAVB || h == kHookIAIB;
}
// Car slots' (mvr_car_slots).
constexpr bool is_slot_hook(int h) { return h == kHookBlend || h == kHookDepth; }
// The draws: hooked only with the focus post (DrawIndexed: its copy and composite; all four: the overlay trace) or
// with instancing (they're doubled).
constexpr bool is_draw(int h) {
    return h == kHookDrawIndexed || h == kHookDraw || h == kHookDrawIndexedInstanced || h == kHookDrawInstanced;
}
// The draws instancing can't double (no instance count to set): only with it.
constexpr bool is_undoubled_draw(int h) {
    return h == kHookDrawAuto || h == kHookDrawIndexedIndirect || h == kHookDrawInstancedIndirect;
}
// Per entry: 4 seen for each on WARP and on an RTX 5070 Ti (2026-11-09: idle and busy, multithread protection off, on,
// and off again after it was on), room for more.
constexpr int kFunctions = 8;
struct Functions {
    void* target[kFunctions] = {};  // d3d11's
    void* real[kFunctions] = {};    // its original (MinHook's trampoline; the function itself if it couldn't be hooked)
    int count = 0, hooked = 0;
    bool full = false;  // a fifth seen (logged once)
};
Functions functions[kHooks];
// Calls of a hooked function inside the handling of one: ours, the one passed on, and one function of an entry calling
// another (a locking one calling the plain one) go straight through.
thread_local int depth[kHooks] = {};

template <int H, typename... A>
void call_real(ID3D11DeviceContext* context, A... a) {
    void* fn = (*reinterpret_cast<void** const*>(context))[kSlotOf[H]];
    void* real = fn;  // a function not hooked (yet): d3d11's own
    const Functions& f = functions[H];
    for (int i = 0; i < f.count; ++i)
        if (f.target[i] == fn) {
            real = f.real[i];
            break;
        }
    ++depth[H];
    reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(real)(context, a...);
    --depth[H];
}

void realVSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* i, UINT n) {
    call_real<kHookVS>(c, vs, i, n);
}
void realPSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* i, UINT n) {
    call_real<kHookPS>(c, ps, i, n);
}
void realOMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                            ID3D11DepthStencilView* dsv) {
    call_real<kHookOM>(c, n, rtvs, dsv);
}
void realOMSetRenderTargetsAndUavs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                   ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount,
                                   ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    call_real<kHookOMUav>(c, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
}
void realRSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps) { call_real<kHookVP>(c, n, vps); }
void realRSSetScissorRects(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* rects) { call_real<kHookSC>(c, n, rects); }
void realSOSetTargets(ID3D11DeviceContext* c, UINT n, ID3D11Buffer* const* buffers, const UINT* offsets) {
    call_real<kHookSO>(c, n, buffers, offsets);
}
void realResolveSubresource(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub,
                            DXGI_FORMAT format) {
    call_real<kHookResolve>(c, dst, dstSub, src, srcSub, format);
}

int nvapi_two_views(ID3D11DeviceContext* context, bool on) { return simqvfr::nvapi::set_two_views(context, on); }

// GPU time per pass, split at its multisample resolve: the scene (pass start to resolve) and the post-processing (resolve
// to pass end). Timestamps in a ring of frames, read back a few frames later without waiting.
struct GpuFrame {
    ComPtr<ID3D11Query> disjoint, stamp[4][3];  // pass start, resolve, pass end
    bool stamped[4][3] = {};
    bool pending = false;
    int mode = 0;  // mvr_frame_mode: 0 MVR, 1 the four passes alone (an A/B run)
};

// The twin of a game shader by its address, valid until the game creates another vertex shader (a new one may take a
// released one's address; twins are attached only at creation, mvr_probe.cpp): GetPrivateData once per shader instead of
// at every bind. Sets of two by a multiplicative hash of the address (allocations are aligned: its low bits alone
// collide, 30% missed in the 10:08 run); two per set, as one per slot still missed 7.8% at 10:34 (two shaders sharing a
// slot, bound in turn, threw each other out).
struct TwinEntry {
    ID3D11VertexShader* vs = nullptr;
    ID3D11VertexShader* twin = nullptr;       // raw: the game's shader holds it (its private data) while it lives
    ID3D11VertexShader* instanced = nullptr;  // its instanced twin, likewise
    uint32_t epoch = 0;
    VsRole role = VsRole::none;  // the probe's marking (yebis_shaders.h)
    int worldSlot = -1;          // where it reads its worldMatrix (car tracking; mvr_probe.h kVsWorldMatrixGuid)
    UINT worldOffset = 0;
};
struct TwinSet {
    TwinEntry entry[2];
    uint8_t next = 0;  // the entry to replace when both hold another shader (the one not looked up last)
};
constexpr unsigned kTwinSetBits = 10, kTwinSets = 1u << kTwinSetBits;
constexpr unsigned kCarSlots = 4096;  // car_motion's table (CarMotion's default), the slot views and motions
// The same for the pixel shaders' mip bias twins (texture sharpening).
struct PsTwinEntry {
    ID3D11PixelShader* ps = nullptr;
    ID3D11PixelShader* twin = nullptr;  // raw: the game's shader holds it
    ID3D11PixelShader* slot = nullptr;  // its slot twin, and the slot twin of its mip bias twin (car slots)
    ID3D11PixelShader* slotBias = nullptr;
    uint32_t epoch = 0;
};
struct PsTwinSet {
    PsTwinEntry entry[2];
    uint8_t next = 0;
};

// CPU in this code's context hooks (the calls passed on included), in TSC ticks since the last log line.
struct HookTime {
    uint64_t hooks = 0, lookups = 0, switches = 0, calls = 0;
    uint64_t cars = 0, carCalls = 0;  // car tracking's own work (not the calls passed on); its hooks' calls
    uint64_t carReads = 0, carDraws = 0;  // of `cars`: the matrix reads at Unmap, the draws (keys, the table)
    // Car slots' (the calls they make included): the slot views bound per draw (1 in 16 timed), the target's binds and
    // the blend states' twins, the motions computed and uploaded.
    uint64_t slotViews = 0, slotState = 0, motion = 0;
};

bool timeHooks = false;  // mvr_time_hooks

struct Timed {
    uint64_t* const sum;
    const uint64_t t0;
    explicit Timed(uint64_t& s) : sum(timeHooks ? &s : nullptr), t0(sum ? __rdtsc() : 0) {}
    ~Timed() {
        if (sum)
            *sum += __rdtsc() - t0;
    }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;
};
constexpr int kGpuFrames = 4;

struct State {
    ID3D11DeviceContext* context = nullptr;  // the game's immediate context (not referenced: the device keeps it)
    ComPtr<ID3D11Multithread> multithread;   // its multithread protection (logged when it changes)
    bool protectedNow = false;
    GpuFrame gpu[kGpuFrames];
    int gpuSlot = -1;            // this frame's, while it runs
    unsigned long long gpuFrameNo = 0;
    int mode = 0;                // this frame's (mvr_frame_mode)
    double sceneMs[2][4] = {}, postMs[2][4] = {};
    unsigned gpuCounts[2][4] = {};
    TwinSet twins[kTwinSets];
    uint32_t twinEpoch = 1;
    uint64_t twinGeneration = 0;
    uint64_t loggedLookups = 0, loggedMisses = 0;  // the stats' counts at the last log line
    HookTime time;
    unsigned long long tscAtLog = 0;
    long long qpcAtLog = 0;
    ComPtr<ID3D11Buffer> remapCb;
    TwoViewsFn twoViews = &nvapi_two_views;
    bool broken = false;  // single-pass stereo refused once and instancing couldn't take over: off for the session
    // Instancing instead of single-pass stereo (mvr_use_instanced): the instanced twins bound, the game's draws doubled,
    // no stereo switch. canInstance: it may take over when single-pass stereo is refused.
    bool instanced = false, canInstance = false;
    bool undoubledLogged = false;
    // The pass.
    int pass = -1;
    bool drawFocus = false, sample = false, replaceResolve = false;
    uint64_t passTwinBinds = 0;  // this pass's draws that drew the focus view too
    MvrPassView view, viewOf[2];
    // What the game bound (raw: bound objects are kept alive by the pipeline).
    ID3D11VertexShader* gameVs = nullptr;
    VsRole vsRole = VsRole::none;  // the game's bound vertex shader's role, in and outside the passes (R6)
    ID3D11ClassInstance* instances[256] = {};
    UINT instanceCount = 0;
    bool depthBound = false;
    // A stage after the vertex shader (geometry, tessellation) or stream output: the twin's second position wouldn't
    // reach the rasterizer, so no two views then.
    bool gsBound = false, hsBound = false, dsBound = false, soBound = false;
    UINT vpCount = 0, scCount = 0;
    D3D11_VIEWPORT vps[kMaxRects] = {};
    D3D11_RECT scs[kMaxRects] = {};
    // What is applied.
    ID3D11VertexShader* boundVs = nullptr;
    bool on = false, stereo = false;
    // Texture sharpening (mvr_mip_bias_hooks, MvrPassView::mipBias): the game's pixel shader as it bound it, what is
    // bound, this pass's bias (from its view), and the constants (focus viewport, focus and eye bias) as uploaded.
    bool biasHooks = false, bias = false, biasCbBound = false;
    uint32_t biasChecks = 0;  // twin binds, every 16th checking the buffer is still bound
    ID3D11PixelShader* gamePs = nullptr;
    ID3D11PixelShader* boundPs = nullptr;
    ID3D11ClassInstance* psInstances[256] = {};
    UINT psInstanceCount = 0;
    PsTwinSet psTwins[kTwinSets];
    uint32_t psEpoch = 1;
    uint64_t psGeneration = 0;
    ComPtr<ID3D11Buffer> biasCb;
    float biasConstants[8] = {};
    // Car tracking (mvr_car_track; car_motion.h): the vertex shader's constant buffers and the input assembler's buffers
    // as the game bound them (raw, compared and used as keys only), the bound vertex shader's worldMatrix place, the
    // per-draw buffer (learned from a draw), its mapped memory while mapped, the matrix last written into it.
    bool carTrack = false, carMatrix = false;
    ID3D11Buffer* vsCb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11Buffer* iaVb = nullptr;
    ID3D11Buffer* iaIb = nullptr;
    UINT iaVbOffset = 0, iaIbOffset = 0;
    int vsWorldSlot = -1;
    UINT vsWorldOffset = 0;
    ID3D11Buffer* worldCb = nullptr;
    ID3D11Resource* worldRes = nullptr;  // the same, as Map and Unmap name it (compared with every one of theirs)
    ComPtr<ID3D11Buffer> worldCbHeld;
    UINT worldCbOffset = 0;
    ID3D11Buffer* vbChecked = nullptr;  // the vertex buffer last checked for dynamic usage (reset each frame)
    bool vbDynamic = false;
    struct VbSeen {  // a vertex buffer's usage as D3D gave it this frame (vb_dynamic)
        ID3D11Buffer* vb = nullptr;
        uint64_t frame = 0;
        bool dynamic = false;
    };
    std::array<VbSeen, 512> vbSeen{};
    void* worldMapped = nullptr;
    float matrix[16] = {};
    CarMotion cars;
    uint64_t carFrame = 0;
    CarMotionStats carLogged;     // the counts at the last log line
    // Car slots (mvr_car_slots): the level (0 off); the slot target (the scene target's size) and its views; per slot a
    // view of its word (slot + 1) for t127, the one bound; the target bound now (the scene's binds in a pass) and this
    // pass's views cleared; the game's blend state as bound (its factor and mask), whether a twin is bound in its place,
    // the depth state's writing; the default blend state's twins ([0] target 1 kept, [1] written); each slot's motion.
    int slotsLevel = 0;
    bool carActive = true;  // mvr_car_active (an A/B's side)
    bool slotsOn = false, slotsBound = false, slotsCleared = false;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11Texture2D> slotTex;
    ComPtr<ID3D11RenderTargetView> slotRtv;
    ComPtr<ID3D11ShaderResourceView> slotSrv;
    UINT slotW = 0, slotH = 0;
    UINT slotSamples = 1, slotQuality = 0;  // as the scene's target: render targets bound together share them
    ComPtr<ID3D11Buffer> slotWords;
    std::vector<ComPtr<ID3D11ShaderResourceView>> slotViews;
    ID3D11ShaderResourceView* slotViewBound = nullptr;
    ID3D11RenderTargetView* sceneRtvChecked = nullptr;  // the last render target view checked for the scene's (raw)
    bool sceneRtvIsScene = false;
    ID3D11Resource* sceneCheckedAgainst = nullptr;  // the tracker's scene target it was checked against (raw)
    ID3D11Resource* sceneRtvResource = nullptr;     // that view's texture (raw, compared only) and its description
    D3D11_TEXTURE2D_DESC sceneRtvDesc{};
    ID3D11BlendState* gameBlend = nullptr;  // raw: bound, so alive
    float blendFactor[4] = {1, 1, 1, 1};
    UINT sampleMask = 0xffffffff;
    bool blendSwapped = false, depthWrites = true;
    ComPtr<ID3D11BlendState> defaultBlendTwin[2];
    ComPtr<ID3D11Buffer> motionBuffer;
    ComPtr<ID3D11ShaderResourceView> motionSrv;
    std::vector<float> motion;
    uint64_t slotViewsThisPass = 0;
    uint64_t slotImageFrame = 0, slotImageAfter = 900;  // the gate image's frame (both eyes), the earliest next one
    bool slotImages = false;                             // mvr_car_slot_images
    MvrPassStats carStatsLogged;  // (only its car counts are read)
    // Each pass's multisample resolve target; the sampled regions.
    ComPtr<ID3D11Resource> resolved[4];
    ComPtr<ID3D11Texture2D> mvrImage[2], passImage[2];
    bool keepFocus = false;           // gate D
    ComPtr<ID3D11Texture2D> kept[2];  // each eye pass's resolved focus region
    bool keptOk[2] = {};
    // The focus post (mvr_focus_replay): on; this pass's post-processing has begun (its multisample resolve is done);
    // its copy and composite replayed.
    bool focusReplay = false, postWindow = false, focusCopied = false, focusComposited = false;
    bool compositeOk = false;  // this pass's focus composite made (the blit's input)
    // R3 (docs/recon-lmu.md "R3b/R3c design"): the focus view's final beside the eye in the eye's final target. This
    // pass's blit replayed; draws into the eye's final after it are overlays MVR doubles (the driver tags); the eye
    // finals of this frame (the VR UI after the passes is doubled into them too) and their sizes.
    bool focusBlitted = false, overlayTarget = false;
    ComPtr<ID3D11Resource> eyeFinal[2];
    bool finalThisFrame[2] = {};
    unsigned finalW[2] = {}, finalH[2] = {};
    bool blitRefusalLogged = false;
    int maskDrawsLogged = 0;  // the first lens mask draws logged with their state
    const std::atomic<bool>* maskStale = nullptr;  // mvr_mask_stale
    int maskDroppedSince = 0;                       // mvr_mask_dropped
    PanelFn panelFn = nullptr;                      // mvr_panel_draws
    // The game's latest CopySubresourceRegions (mvr_copy_source): raw, compared only; the frame count when seen.
    struct CopySeen {
        ID3D11Resource* dst = nullptr;
        ID3D11Resource* src = nullptr;
        unsigned long long frame = 0;
    };
    static constexpr unsigned kCopiesKept = 16;
    CopySeen copies[kCopiesKept];
    unsigned copyNext = 0;
    // DLAA (engine/dlss_views.h): the latest depth target bound with each render target of this pass (the resolve hook finds
    // the scene's by the render target it resolves).
    struct SceneBind {
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
    };
    SceneBind sceneBinds[4];
    unsigned sceneBindNext = 0;
    FocusPost focusPost;
    bool focusPostFailed = false;  // couldn't be made: off for the session
    // mvr_ui_mips: the VR UI canvas's copy with mips, made once a frame for the panel's doubled draws.
    struct UiMips {
        bool on = true, failed = false, logged = false;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        ID3D11Resource* source = nullptr;  // raw, compared only: the canvas it was made from
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        unsigned long long frame = ~0ull;  // framesSinceInstall when its mips were last made
    } uiMips;
    std::string postTrace;         // this pass's draws after the copy (trace_post_draw)
    int tracesLogged = 0;
    uint32_t unknown[16][4] = {};  // pixel shaders seen drawing into the resolve's target instead of the composite
    int unknownCount = 0;
    ComPtr<ID3D11Resource> copyTarget;  // this pass's YEBIS copy's output (the composite's t0)
    uint32_t byBindings[8][4] = {};     // composites taken by their bindings (logged once each)
    int bindingCount = 0;
    // The overlay trace (with the focus post; docs/recon-lmu.md "R3 plan"): every draw of a few frames from each pass's
    // multisample resolve to its end, during the lens mask and outside the passes.
    bool tracing = false, afterResolve = false;
    // The focus post executing its command list: d3d11 may replay it through the immediate context's own functions
    // (WARP does), so its state and draws reach these hooks as if they were the game's; ignored meanwhile.
    bool replaying = false;
    int framesTraced = 0, traceDraws = 0, traceDropped = 0;
    unsigned long long framesSinceInstall = 0;
    bool have[4] = {};
    unsigned long long lastStatsFrame = 0;
    MvrPassStats stats;
} g;

bool ours(ID3D11DeviceContext* context) { return context == g.context && g.pass >= 0 && !g.replaying; }

// R4b's pass tracker (mvr_track_passes): the eye passes as the d3d11 calls show them, compared with the game's bounds.
struct Tracker {
    bool on = false;
    bool open = false, resolved = false, blitSeen = false;
    int pass = -1, passes = 0;            // the open tracked pass; tracked passes begun this frame (at most two)
    uint64_t draws = 0;                   // draws on the immediate context this frame (the four draw calls)
    uint64_t begin[2] = {}, end[2] = {};  // the draw count at each tracked pass's begin and end
    const char* endWhy[2] = {};
    int actual = 0;                                   // the game's passes 0-1 begun this frame (mvr_pass_begin)
    uint64_t actualBegin[2] = {}, actualEnd[2] = {};  // the draw count at their begin and end
    std::string firstBind;  // a frame without the game's passes: the first tracked pass's target (the log)
    // MSAA off (docs/recon-lmu.md "MSAA off"): no multisample target, no resolve. The scene's target is learned from
    // YEBIS's copy, which then reads it directly: a single-sample texture the copy reads that
    // was drawn with a depth target, in a frame with no multisample scene. A pass then begins at its bind with a depth
    // target, and its post-processing at the copy (post_begins) instead of at the resolve.
    // The learned target: raw, compared only (with its description, so a texture made later at the same address isn't
    // taken for it); no reference held, as below.
    ID3D11Resource* scene = nullptr;
    D3D11_TEXTURE2D_DESC sceneDesc{};
    // This frame's single-sample textures bound as render target 0 with a depth target outside the tracked passes:
    // raw, compared only. No reference is held: one of them is the desktop mirror's swapchain back buffer, which the
    // game's ResizeBuffers needs every reference to gone (a reference kept here failed it at the session's start:
    // "Resize swap chain failed", 2026-11-10 19:13).
    ID3D11Resource* drawnWithDepth[8] = {};
    unsigned drawnNext = 0;
    bool learnedThisFrame = false;  // no pass begins in the frame the target was learned in (it may be mid-frame)
    bool msaaSinceScene = false;    // a multisample scene seen since the target was learned (MSAA on again)
    // A single-sample pass's own final blit (YEBIS's blit reading the scene's target, seen whether or not the focus post
    // ran) and its target (raw): the pass ends at another target after it. Without it a frame whose focus views
    // didn't fit (the first after loading, at the game's own size) found pass 0 alone, never pass 1, after which the
    // live resolution is applied: so it never was, and the focus views never fitted (2026-11-10 19:19).
    ID3D11Resource* finalTarget = nullptr;
    int blitsBeforePostLogged = 0, passesWithoutPostLogged = 0;
    bool singleSample = false;  // the open pass began on the learned scene target
    bool msaaFrame = false;     // a multisample scene was bound this frame
    bool sceneLogged = false;
    bool smallLogged = false;  // a too-small candidate for the scene's target has been said
    MvrTrackStats stats;
    TrackFn driveBegin = nullptr, driveEnd = nullptr;  // mvr_track_drive: the passes' work on these bounds
    uint64_t loggedFrames = 0;
    int detailsLogged = 0;
    bool menuLogged = false;
} tr;

// The pass trace (log only): while the tracker drives the passes, three frames' render target binds with depth or a
// multisample target, resolves, CopySubresourceRegions, YEBIS's draws, the focus post's window and the tracked passes'
// bounds, in order, to see what the tracker takes for a pass's begin and end. The frames with tracked passes numbered
// kPassTraceAt (the first renders at the game's own size); a frame without passes is dropped.
// With [experiment] lmu_pass_trace=1 (mvr_pass_trace) every frame is recorded instead, and a frame is logged whenever
// its shape is one not seen before this launch: the order of its passes' bounds, resolves, what is bound after a
// resolve, YEBIS's draws and the focus post's window, with the textures' numbers left out. With it the game's graphics
// settings as its own files hold them (UserData\Config_DX11_VR.ini, UserData\player\Settings.JSON) and the renderer's
// post-processing bits, so a run that steps through the game's settings shows which one changes the stream.
struct PassTrace {
    bool recording = false;
    int framesWithPasses = 0, logged = 0, dropped = 0;
    std::vector<std::string> events;
    std::vector<const void*> ids;  // the textures mentioned this frame (raw: compared only)
    int level = 0;                 // mvr_pass_trace
    std::vector<uint64_t> shapes;  // the shapes logged so far (their hashes)
    int lastShape = -1;
    unsigned long long lastSwitchFrame = 0, settingsCheckedFrame = 0;
    std::string settings;          // the game's settings as last logged
} pt;
constexpr int kPassTraceAt[] = {2, 3, 45};
constexpr std::size_t kPassTraceEvents = 400;
constexpr std::size_t kPassTraceShapes = 40;

// The game's graphics settings, read from its own files (not its memory): one line.
std::string pt_game_settings() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n == MAX_PATH)
        return "?";
    const std::filesystem::path user = std::filesystem::path(exe).parent_path() / L"UserData";
    std::string line;
    const std::wstring config = (user / L"Config_DX11_VR.ini").wstring();
    for (const wchar_t* key : {L"EPostProcessingSettings", L"MSAA", L"UseFXAA", L"UpscaleMethod", L"RenderScale", L"ShaderLevel"}) {
        wchar_t value[64] = {};
        GetPrivateProfileStringW(L"COMPONENTS", key, L"?", value, 64, config.c_str());
        char text[160];
        std::snprintf(text, sizeof(text), "%s%ls=%ls", line.empty() ? "" : " ", key, value);
        line += text;
    }
    std::string json;
    if (HANDLE f = CreateFileW((user / L"player" / L"Settings.JSON").wstring().c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 22)) {
            json.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD got = 0;
            if (!ReadFile(f, json.data(), static_cast<DWORD>(json.size()), &got, nullptr))
                got = 0;
            json.resize(got);
        }
        CloseHandle(f);
    }
    line += ";";
    for (const char* key : {"Environment Reflections", "Road Reflections", "Shadows", "Shadow Blur", "Soft Particles", "Motion blur",
                            "Rain FX Quality", "Raindrop Flow", "Rainspray Flow", "Opponent Detail", "Player Detail", "Texture Detail",
                            "Special FX", "Circuit Detail", "Sun Occlusion", "Rearview Driving", "Max Visible Vehicles",
                            "Heat FX Fade Speed", "Live TV Displays"}) {
        const std::string quoted = std::string("\"") + key + "\":";
        const std::size_t at = json.find(quoted);
        if (at == std::string::npos)
            continue;
        std::size_t from = at + quoted.size(), to = json.find_first_of(",\r\n}", from);
        while (from < json.size() && json[from] == ' ')
            ++from;
        line += " " + std::string(key) + " " + json.substr(from, to == std::string::npos ? 8 : to - from) + ",";
    }
    // The renderer's own state (a read of the game's memory, this diagnostic's alone): the post-processing bits and the
    // flags a static reading of its post-processing function names. Their offsets are from that reading, not verified.
    const std::uintptr_t base = simqvfr::module_base();
    if (const auto* renderer = *reinterpret_cast<const std::uint8_t* const*>(base + kRenderer)) {
        std::uint32_t post = 0, msaa = 0;
        std::memcpy(&post, renderer + 0x7770c, sizeof(post));
        std::memcpy(&msaa, renderer + 0x1a1c, sizeof(msaa));
        char text[160];
        std::snprintf(text, sizeof(text), " renderer: post bits 0x%08x (bits 24-26: %u%u%u), +0x770fa %u, +0x1a1c %u", post,
                      (post >> 24) & 1, (post >> 25) & 1, (post >> 26) & 1, static_cast<unsigned>(renderer[0x770fa]), msaa);
        line += text;
    }
    return line;
}

void pt_event(const char* format, ...) {
    if (pt.events.size() >= kPassTraceEvents) {
        ++pt.dropped;
        return;
    }
    char head[32];
    std::snprintf(head, sizeof(head), "@%llu ", static_cast<unsigned long long>(tr.draws));
    char body[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    pt.events.push_back(std::string(head) + body);
}

// A texture as "#n", with its size, format and sample count at its first mention this frame; "-" for none.
std::string pt_tex(ID3D11Resource* resource) {
    if (!resource)
        return "-";
    for (std::size_t i = 0; i < pt.ids.size(); ++i)
        if (pt.ids[i] == resource)
            return "#" + std::to_string(i);
    pt.ids.push_back(resource);
    std::string s = "#" + std::to_string(pt.ids.size() - 1);
    ComPtr<ID3D11Texture2D> t;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&t)))) {
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        char desc[64];
        std::snprintf(desc, sizeof(desc), "(%ux%u f%d x%u%s)", d.Width, d.Height, static_cast<int>(d.Format),
                      d.SampleDesc.Count, d.ArraySize > 1 ? " array" : "");
        s += desc;
    } else {
        s += "(not 2D)";
    }
    return s;
}

std::string pt_view(ID3D11View* view) {
    if (!view)
        return "-";
    ComPtr<ID3D11Resource> r;
    view->GetResource(&r);
    return pt_tex(r.Get());
}

// A draw on the game's context with one of YEBIS's pixel shaders (copy, composite, final blit): its input and target.
void pt_draw(ID3D11DeviceContext* context, const char* call) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    UINT role = 0, size = sizeof(role);
    if (!ps || FAILED(ps->GetPrivateData(kYebisRoleGuid, &size, &role)) || !role)
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    const char* name = role == static_cast<UINT>(YebisRole::copy)        ? "copy"
                       : role == static_cast<UINT>(YebisRole::composite) ? "composite"
                       : role == static_cast<UINT>(YebisRole::blit)      ? "final blit"
                                                                         : "?";
    const std::string in = pt_view(srv.Get()), out = pt_view(rtv.Get());
    pt_event("YEBIS %s (%s) t0 %s > %s, %s", name, call, in.c_str(), out.c_str(),
             tr.open ? ("in tracked pass " + std::to_string(tr.pass)).c_str() : "outside the tracked passes");
}

// A frame's events written out, a few to a line.
void pt_log_events() {
    std::string line;
    for (const std::string& e : pt.events) {
        if (!line.empty() && line.size() + e.size() > 1300) {
            simqvfr::logf("lmu mvr: pass trace: %s", line.c_str());
            line.clear();
        }
        line += line.empty() ? e : "; " + e;
    }
    if (!line.empty())
        simqvfr::logf("lmu mvr: pass trace: %s", line.c_str());
}

// The frame's shape (PassTrace): what orders the passes and their post-processing, without the textures' numbers, the
// draw counts and the sizes. `yebis`: a draw with one of YEBIS's shaders is in it.
std::string pt_shape(bool& yebis) {
    std::string shape;
    yebis = false;
    for (const std::string& e : pt.events) {
        const std::size_t space = e.find(' ');
        std::string k = space == std::string::npos ? e : e.substr(space + 1);
        const auto starts = [&k](const char* with) { return k.rfind(with, 0) == 0; };
        yebis = yebis || starts("YEBIS");
        if (!(starts("P+") || starts("P-") || starts("YEBIS") || starts("R ") || starts("W ") ||
              k.find("(after a resolve in the pass)") != std::string::npos))
            continue;
        if (starts("P+"))
            k = k.substr(0, k.find(';'));
        if (starts("W "))
            k = k.substr(0, k.find(':'));
        std::string plain;
        for (std::size_t i = 0; i < k.size(); ++i) {
            plain += k[i];
            if (k[i] == '#')
                while (i + 1 < k.size() && k[i + 1] >= '0' && k[i + 1] <= '9')
                    ++i;
        }
        shape += plain + "|";
    }
    return shape;
}

// The frame end (after the tracker's): the trace logged if this frame is one of kPassTraceAt, or with lmu_pass_trace
// if its shape is new; the next frame's decided.
void pt_frame_end(unsigned long long frame, int passes) {
    if (passes > 0)
        ++pt.framesWithPasses;
    const char* const key = "@N = the game's draws this frame so far, #n = a texture (size, format, samples at its first "
                            "mention); B = a render target bind (target 0, depth), R = a resolve, C = a "
                            "CopySubresourceRegion, P+/P- = a tracked pass's begin/end, W = the focus post's window opened";
    if (pt.level && pt.recording) {
        // The game's settings, when its files change (looked at every two seconds or so).
        if (frame - pt.settingsCheckedFrame >= 180 || pt.settings.empty()) {
            pt.settingsCheckedFrame = frame;
            std::string now = pt_game_settings();
            if (now != pt.settings) {
                simqvfr::logf("lmu mvr: pass trace: frame %llu: the game's settings%s: %s", frame, pt.settings.empty() ? "" : " CHANGED",
                              now.c_str());
                pt.settings = std::move(now);
            }
        }
        bool yebis = false;
        const std::string shape = pt_shape(yebis);
        if (passes > 0 || yebis) {
            uint64_t hash = 1469598103934665603ull;
            for (const char c : shape)
                hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
            int index = -1;
            for (std::size_t i = 0; i < pt.shapes.size(); ++i)
                if (pt.shapes[i] == hash)
                    index = static_cast<int>(i);
            if (index < 0 && pt.shapes.size() < kPassTraceShapes) {
                pt.shapes.push_back(hash);
                index = static_cast<int>(pt.shapes.size()) - 1;
                simqvfr::logf("lmu mvr: pass trace: frame %llu: SHAPE %d, new (%d tracked passes): %zu events, %d not logged (over "
                              "%zu); %s",
                              frame, index, passes, pt.events.size(), pt.dropped, kPassTraceEvents, key);
                simqvfr::logf("lmu mvr: pass trace: shape %d with the game's settings: %s", index, pt_game_settings().c_str());
                pt_log_events();
                simqvfr::logf("lmu mvr: pass trace: shape %d is: %s", index, shape.c_str());
                pt.lastShape = index;
                pt.lastSwitchFrame = frame;
            } else if (index >= 0 && index != pt.lastShape && frame - pt.lastSwitchFrame >= 45) {
                simqvfr::logf("lmu mvr: pass trace: frame %llu: shape %d again (was %d; %d tracked passes)", frame, index, pt.lastShape,
                              passes);
                pt.lastShape = index;
                pt.lastSwitchFrame = frame;
            }
        }
    } else if (pt.recording && passes > 0 && pt.logged < static_cast<int>(std::size(kPassTraceAt)) &&
               pt.framesWithPasses == kPassTraceAt[pt.logged]) {
        ++pt.logged;
        simqvfr::logf("lmu mvr: pass trace: frame %llu (number %d with tracked passes, %d of them): %zu events, %d not "
                      "logged (over %zu); %s",
                      frame, pt.framesWithPasses, passes, pt.events.size(), pt.dropped, kPassTraceEvents, key);
        pt_log_events();
    }
    pt.events.clear();
    pt.ids.clear();
    pt.dropped = 0;
    pt.recording = tr.on && tr.driveBegin &&
                   (pt.level ? true
                             : pt.logged < static_cast<int>(std::size(kPassTraceAt)) &&
                                   pt.framesWithPasses + 1 == kPassTraceAt[pt.logged]);
}

void track_begin(const char* what) {
    tr.open = true;
    tr.pass = tr.passes++;
    tr.resolved = tr.blitSeen = false;
    tr.finalTarget = nullptr;
    tr.begin[tr.pass] = tr.draws;
    if (tr.actual == 0 && tr.firstBind.empty())
        tr.firstBind = what;
    if (tr.driveBegin)
        tr.driveBegin(tr.pass);  // R4b: the pass's work (mvr_pass_begin among it) now, before its draws
    if (pt.recording)
        pt_event("P+%d on %s; its focus view %s (eye %ux%u, focus %ux%u)", tr.pass, what,
                 g.drawFocus ? "drawn" : "not drawn", g.view.eyeW, g.view.eyeH, g.view.focusW, g.view.focusH);
}

void track_end(const char* why) {
    if (!tr.open)
        return;
    if (tr.singleSample && !tr.resolved) {  // MSAA off: a pass that never reached YEBIS's copy
        ++tr.stats.passesWithoutPost;
        if (tr.passesWithoutPostLogged < 3) {
            ++tr.passesWithoutPostLogged;
            simqvfr::logf("lmu mvr: pass tracker, MSAA off: frame %llu: pass %d ended (%s) %llu draws in, before its "
                          "post-processing began",
                          static_cast<unsigned long long>(g.framesSinceInstall), tr.pass, why,
                          static_cast<unsigned long long>(tr.draws - tr.begin[tr.pass]));
        }
    }
    if (pt.recording)
        pt_event("P-%d at %s", tr.pass, why);
    tr.open = false;
    tr.end[tr.pass] = tr.draws;
    tr.endWhy[tr.pass] = why;
    if (tr.driveEnd)
        tr.driveEnd(tr.pass);  // R4b: after the pass's draws (mvr_pass_end among it)
}

// A render target bind on the game's immediate context: a pass begins at the first multisample target bound with a
// depth target, or with MSAA off the learned scene target bound with one (after the frame's start or the previous
// pass's end); the open one ends at another target bound after its final blit, or at the next pass's scene (a
// multisample bind after its resolve; with MSAA off only the final blit or the frame end: YEBIS draws into the scene's
// own target then).
void track_bind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    ComPtr<ID3D11Resource> r0;
    if (n && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && rtvs && rtvs[0])
        rtvs[0]->GetResource(&r0);
    bool msaa = false;
    D3D11_TEXTURE2D_DESC d{};
    if (dsv && r0) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(r0.As(&texture))) {
            texture->GetDesc(&d);
            msaa = d.SampleDesc.Count > 1;
        }
    }
    tr.msaaFrame = tr.msaaFrame || msaa;
    tr.msaaSinceScene = tr.msaaSinceScene || (msaa && tr.scene);
    const bool learned = !msaa && !tr.msaaFrame && !tr.learnedThisFrame && dsv && r0 && tr.scene && r0.Get() == tr.scene &&
                         d.Width == tr.sceneDesc.Width && d.Height == tr.sceneDesc.Height &&
                         d.Format == tr.sceneDesc.Format && d.SampleDesc.Count == tr.sceneDesc.SampleDesc.Count;
    bool multisampleTarget = msaa;  // also without depth (the tracker's own test needs both)
    if (pt.recording && r0 && !dsv) {
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC td{};
        if (SUCCEEDED(r0.As(&texture))) {
            texture->GetDesc(&td);
            multisampleTarget = td.SampleDesc.Count > 1;
        }
    }
    if (pt.recording && (dsv || multisampleTarget)) {
        ComPtr<ID3D11Resource> depthTarget;
        if (dsv)
            dsv->GetResource(&depthTarget);
        const std::string rt = pt_tex(r0.Get()), ds = pt_tex(depthTarget.Get());
        pt_event("B %s depth %s%s", rt.c_str(), ds.c_str(), tr.open && tr.resolved ? " (after a resolve in the pass)" : "");
    }
    if (tr.open) {
        const ID3D11Resource* finalOf = tr.singleSample && tr.finalTarget ? tr.finalTarget : g.eyeFinal[tr.pass].Get();
        if (tr.blitSeen && r0 && r0.Get() != finalOf)
            track_end("another target after its final blit");
        else if (msaa && tr.resolved)
            track_end("the next pass's scene");
    }
    if (!tr.open && (msaa || learned) && tr.passes < 2) {
        char what[96];
        std::snprintf(what, sizeof(what), "%ux%u format %d x%u", d.Width, d.Height, static_cast<int>(d.Format),
                      d.SampleDesc.Count);
        track_begin(what);
        tr.singleSample = learned;
        tr.stats.singleSamplePasses += learned ? 1 : 0;
    }
}

// The frame end: the open pass ends; this frame's tracked passes against the game's.
void track_frame_end(unsigned long long frame) {
    track_end("the frame end");
    MvrTrackStats& s = tr.stats;
    ++s.frames;
    if (tr.actual == 0) {
        if (tr.passes) {
            ++s.framesWithoutGame;
            s.passesWithoutGame += static_cast<uint64_t>(tr.passes);
            if (!tr.menuLogged) {
                tr.menuLogged = true;
                simqvfr::logf("lmu mvr: pass tracker: frame %llu has no eye passes of the game's, yet %d found (the first on %s)",
                              frame, tr.passes, tr.firstBind.c_str());
            }
        }
    } else if (tr.passes != tr.actual) {
        ++s.countDiffers;
        if (tr.detailsLogged < 6) {
            ++tr.detailsLogged;
            simqvfr::logf("lmu mvr: pass tracker: frame %llu: %d passes found, the game's %d", frame, tr.passes, tr.actual);
        }
    } else {
        bool ok = true;
        std::string detail;
        for (int q = 0; q < tr.passes; ++q) {
            const long long startGap = static_cast<long long>(tr.begin[q]) - static_cast<long long>(tr.actualBegin[q]);
            const long long endGap = static_cast<long long>(tr.actualEnd[q]) - static_cast<long long>(tr.end[q]);
            if (startGap < 0) {
                ++s.beganBefore;
                ok = false;
            } else {
                s.startGapSum += static_cast<uint64_t>(startGap);
                s.startGapMax = (std::max)(s.startGapMax, static_cast<uint64_t>(startGap));
            }
            if (endGap < 0) {
                ++s.endedAfter;
                ok = false;
            } else {
                s.endGapSum += static_cast<uint64_t>(endGap);
                s.endGapMax = (std::max)(s.endGapMax, static_cast<uint64_t>(endGap));
            }
            ++s.passesCompared;
            char part[200];
            std::snprintf(part, sizeof(part), "%spass %d: began %lld draws into the game's, ended %lld draws before its end (%s)",
                          q ? "; " : "", q, startGap, endGap, tr.endWhy[q] ? tr.endWhy[q] : "?");
            detail += part;
        }
        if (ok)
            ++s.matched;
        if (tr.detailsLogged < 6) {
            ++tr.detailsLogged;
            simqvfr::logf("lmu mvr: pass tracker: frame %llu: %s", frame, detail.c_str());
        }
    }
    tr.passes = tr.actual = 0;
    tr.draws = 0;
    tr.firstBind.clear();
    tr.msaaFrame = tr.singleSample = tr.learnedThisFrame = false;
    std::fill(std::begin(tr.drawnWithDepth), std::end(tr.drawnWithDepth), nullptr);
    tr.drawnNext = 0;
}

template <class Shader>
std::string shader_name(Shader* shader);
std::string describe(ID3D11Resource* resource);

// MSAA off: after a Draw in a pass begun on the scene's single-sample target, YEBIS's final blit (its pixel shader's
// role, reading the scene's target) marks the pass's end-to-be, whether the focus post ran or not. Only once the
// pass's post-processing has begun (at the copy): the real one comes after it, and a draw like it before the copy
// ended a pass early, splitting an eye's pass in two and leaving the other eye untracked (2026-11-10 19:30, after eye
// tracking went on; logged here to learn what it is).
void note_final_blit(ID3D11DeviceContext* context) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    UINT role = 0, size = sizeof(role);
    if (!ps || FAILED(ps->GetPrivateData(kYebisRoleGuid, &size, &role)) || size != sizeof(role) ||
        role != static_cast<UINT>(YebisRole::blit))
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> source;
    if (srv)
        srv->GetResource(&source);
    if (!source || source.Get() != tr.scene)
        return;
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    if (!target)
        return;
    if (!tr.resolved) {
        ++tr.stats.blitsBeforePost;
        if (tr.blitsBeforePostLogged < 3) {
            ++tr.blitsBeforePostLogged;
            ComPtr<ID3D11VertexShader> vs;
            context->VSGetShader(&vs, nullptr, nullptr);
            simqvfr::logf("lmu mvr: pass tracker, MSAA off: frame %llu pass %d: a blit (vs %s, ps %s) reads the scene's "
                          "target into %s %llu draws into the pass, before its post-processing began: not its final blit",
                          static_cast<unsigned long long>(g.framesSinceInstall), tr.pass, shader_name(vs.Get()).c_str(),
                          shader_name(ps.Get()).c_str(), describe(target.Get()).c_str(),
                          static_cast<unsigned long long>(tr.draws - tr.begin[tr.pass]));
        }
        return;
    }
    tr.finalTarget = target.Get();  // raw, compared only
    tr.blitSeen = true;
    ++tr.stats.singleSampleBlits;
}

// Outside the tracked passes (the tracker on): render target 0's texture noted when bound with a depth target.
void note_drawn_with_depth(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!dsv || !n || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL || !rtvs || !rtvs[0])
        return;
    ID3D11Resource* r = nullptr;
    rtvs[0]->GetResource(&r);
    if (!r)
        return;
    r->Release();  // raw from here on: compared, never used
    for (ID3D11Resource* seen : tr.drawnWithDepth)
        if (seen == r)
            return;
    tr.drawnWithDepth[tr.drawnNext++ % std::size(tr.drawnWithDepth)] = r;
}

bool drawn_with_depth(ID3D11Resource* r) {
    for (ID3D11Resource* seen : tr.drawnWithDepth)
        if (r && seen == r)
            return true;
    return false;
}

void stamp(int pass, int which) {
    if (g.gpuSlot < 0 || pass < 0 || pass > 3)
        return;
    GpuFrame& f = g.gpu[g.gpuSlot];
    if (f.stamped[pass][which] || !f.stamp[pass][which])
        return;
    g.context->End(f.stamp[pass][which].Get());
    f.stamped[pass][which] = true;
}

// Frames whose timestamps are in: their scene and post times added up.
void gpu_collect() {
    for (GpuFrame& f : g.gpu) {
        if (!f.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (g.context->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        f.pending = false;
        if (dj.Disjoint || !dj.Frequency)
            continue;
        for (int p = 0; p < 4; ++p) {
            UINT64 t[3] = {};
            bool ok = f.stamped[p][0] && f.stamped[p][1] && f.stamped[p][2];
            for (int k = 0; k < 3 && ok; ++k)
                ok = g.context->GetData(f.stamp[p][k].Get(), &t[k], sizeof(t[k]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (!ok || t[1] < t[0] || t[2] < t[1])
                continue;
            g.sceneMs[f.mode][p] += 1000.0 * static_cast<double>(t[1] - t[0]) / static_cast<double>(dj.Frequency);
            g.postMs[f.mode][p] += 1000.0 * static_cast<double>(t[2] - t[1]) / static_cast<double>(dj.Frequency);
            ++g.gpuCounts[f.mode][p];
        }
    }
}

// The eye's corner of its final (the overlays' and the focus final's space): the rendered corner stretched by the game's
// final blit when its render scale isn't 1 (MvrPassView::finalEyeW).
unsigned final_eye_w(const MvrPassView& view) { return view.finalEyeW ? view.finalEyeW : view.eyeW; }
unsigned final_eye_h(const MvrPassView& view) { return view.finalEyeH ? view.finalEyeH : view.eyeH; }

// The focus view's viewport beside the eye's `v`: in the scene the eye's rendered size scaled to the focus view's (to
// what it's drawn at before the post, `scene`: smaller under DLSS super resolution); for an overlay into the eye's final
// (`overlay`), the eye's corner of the final to the focus final's (the focus size, 1:1).
D3D11_VIEWPORT focus_viewport_of(const MvrPassView& view, const D3D11_VIEWPORT& v, bool overlay = false, bool scene = false) {
    const unsigned ew = overlay ? final_eye_w(view) : view.eyeW, eh = overlay ? final_eye_h(view) : view.eyeH;
    const unsigned fw = !overlay && scene && view.focusRenderW ? view.focusRenderW : view.focusW;
    const unsigned fh = !overlay && scene && view.focusRenderH ? view.focusRenderH : view.focusH;
    const float sx = ew ? static_cast<float>(fw) / static_cast<float>(ew) : 1.0f;
    const float sy = eh ? static_cast<float>(fh) / static_cast<float>(eh) : 1.0f;
    return {v.TopLeftX + v.Width, v.TopLeftY, v.Width * sx, v.Height * sy, v.MinDepth, v.MaxDepth};
}

// The pass's focus viewport now: the scene's draws until its post begins (the resolve, or YEBIS's copy with MSAA off),
// the post's region after it.
D3D11_VIEWPORT focus_viewport(const D3D11_VIEWPORT& v) {
    return focus_viewport_of(g.view, v, g.overlayTarget, !g.afterResolve);
}

// An overlay's viewport into eye `eye`'s final: the whole target cut to the eye's corner of it (as the driver tags'
// under live resolution: tag_viewport.h), so its second viewport lands on the focus view beside it.
D3D11_VIEWPORT overlay_viewport(int eye, const MvrPassView& view, const D3D11_VIEWPORT& v) {
    TagViewport cut{};
    if (!tag_viewport_cut({v.TopLeftX, v.TopLeftY, v.Width, v.Height}, g.finalW[eye], g.finalH[eye], final_eye_w(view),
                          final_eye_h(view), cut))
        return v;
    return {cut.x, cut.y, cut.width, cut.height, v.MinDepth, v.MaxDepth};
}

// The eye's viewport as applied with two views: the game's, or its cut for an overlay (R3c).
D3D11_VIEWPORT first_viewport() {
    return g.overlayTarget ? overlay_viewport(g.pass & 1, g.view, g.vps[0]) : g.vps[0];
}

// DLAA (engine/dlss_views.h): an overlay's viewport moved back by the clip shift its view's draws carry (a shift of s moves
// the drawing s[0] x width / 2 pixels right and s[1] x height / 2 up), so the overlay lands where the post's image,
// which DLAA left without the jitter, has the world.
D3D11_VIEWPORT unshifted(D3D11_VIEWPORT v, const double (&shift)[2]) {
    v.TopLeftX -= static_cast<float>(shift[0] * v.Width / 2);
    v.TopLeftY += static_cast<float>(shift[1] * v.Height / 2);
    return v;
}

// The game's scissor mapped like its viewport onto the focus viewport.
D3D11_RECT focus_scissor(const D3D11_RECT& r) {
    const D3D11_VIEWPORT v = first_viewport();
    const D3D11_VIEWPORT f = focus_viewport(v);
    const float sx = v.Width > 0 ? f.Width / v.Width : 1.0f, sy = v.Height > 0 ? f.Height / v.Height : 1.0f;
    const auto x = [&](LONG at) { return static_cast<LONG>(std::lround(f.TopLeftX + (static_cast<float>(at) - v.TopLeftX) * sx)); };
    const auto y = [&](LONG at) { return static_cast<LONG>(std::lround(f.TopLeftY + (static_cast<float>(at) - v.TopLeftY) * sy)); };
    return {x(r.left), y(r.top), x(r.right), y(r.bottom)};
}

// Texture sharpening's constants for the pixel shader twins (pixel shader buffer 13): the focus view's viewport as
// apply_viewports gives it (the focus bias inside it, the eye's outside: dxbc::add_focus_mip_bias), uploaded when they
// change. Without two viewports, no rectangle: the eye's bias everywhere.
void update_bias_constants(const D3D11_VIEWPORT* focus) {
    if (!g.bias || !g.biasCb)
        return;
    float c[8] = {0, 0, 0, 0, g.view.mipBias[1], g.view.mipBias[0], 0, 0};
    if (focus) {
        c[0] = focus->TopLeftX;
        c[1] = focus->TopLeftY;
        c[2] = focus->TopLeftX + focus->Width;
        c[3] = focus->TopLeftY + focus->Height;
    }
    if (std::memcmp(c, g.biasConstants, sizeof(c)) != 0) {
        std::memcpy(g.biasConstants, c, sizeof(c));
        g.context->UpdateSubresource(g.biasCb.Get(), 0, nullptr, c, 0, 0);
    }
}

void apply_viewports() {
    if (g.on && g.vpCount == 1) {
        const D3D11_VIEWPORT first = first_viewport();
        D3D11_VIEWPORT two[2] = {first, focus_viewport(first)};
        update_bias_constants(&two[1]);
        if (g.overlayTarget) {  // after the post: drawn moved back by the jitter DLAA took out
            two[0] = unshifted(two[0], g.view.overlayShift[0]);
            two[1] = unshifted(two[1], g.view.overlayShift[1]);
        }
        realRSSetViewports(g.context, 2, two);
        ++g.stats.viewportRewrites;
    } else {
        if (g.on)
            update_bias_constants(nullptr);
        realRSSetViewports(g.context, g.vpCount, g.vps);
    }
}

void apply_scissors() {
    if (g.on && g.scCount == 1 && g.vpCount == 1) {
        const D3D11_RECT two[2] = {g.scs[0], focus_scissor(g.scs[0])};
        realRSSetScissorRects(g.context, 2, two);
        ++g.stats.scissorRewrites;
    } else if (g.scCount) {
        realRSSetScissorRects(g.context, g.scCount, g.scs);
    }
}

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
            g.biasCbBound = true;
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
    g.gamePs = ps;
    g.psInstanceCount = std::min<UINT>(count, 256);
    for (UINT i = 0; i < g.psInstanceCount; ++i)
        g.psInstances[i] = instances ? instances[i] : nullptr;
    g.boundPs = reinterpret_cast<ID3D11PixelShader*>(~uintptr_t{0});  // the game's call always takes effect
    update_ps();
}

// ---- Car tracking (mvr_car_track; car_motion.h): bindings followed, the per-draw buffer read as the game writes it ----

// Car tracking's sections are many (~7,000 a frame on track) and short: timing each would cost about as much as their
// work (a timed section ~17 ns, lmu_car_motion_test --bench), so 1 in 16 is timed and counted 16 times, each site with
// a counter of its own (the matrix read and the draw alternate in the eye passes: one counter for both timed one of
// them for long stretches, run 22), less the two timestamp reads' own ticks inside the section (timedEmptyTicks).
uint32_t carSampleRead = 0, carSampleDraw = 0;
uint64_t timedEmptyTicks = 0;  // the least a section with nothing in it measures (mvr_time_hooks)
struct SampledTimed {
    uint64_t* const sum;
    const uint64_t t0;
    SampledTimed(uint64_t& s, uint32_t& counter)
        : sum(timeHooks && (++counter & 15) == 0 ? &s : nullptr), t0(sum ? __rdtsc() : 0) {}
    ~SampledTimed() {
        if (sum) {
            const uint64_t d = __rdtsc() - t0;
            *sum += (d > timedEmptyTicks ? d - timedEmptyTicks : 0) * 16;
        }
    }
    SampledTimed(const SampledTimed&) = delete;
    SampledTimed& operator=(const SampledTimed&) = delete;
};

// The handlers get the original of the function the game called (LeanDetours, below) and call it themselves.
using VsCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using IaVbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
using IaIbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);

inline void vs_set_constant_buffers(VsCbFn real, ID3D11DeviceContext* context, UINT start, UINT n,
                                    ID3D11Buffer* const* buffers) {
    if (context == g.context && !g.replaying) {
        if (timeHooks)
            ++g.time.carCalls;
        for (UINT i = 0; i < n && start + i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i)
            g.vsCb[start + i] = buffers ? buffers[i] : nullptr;
    }
    real(context, start, n, buffers);
}

// Map and Unmap: any resource but the per-draw buffer passes with one comparison.
inline HRESULT map_resource(MapFn real, ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, D3D11_MAP type,
                            UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    const HRESULT hr = real(context, resource, sub, type, flags, mapped);
    if (timeHooks)
        ++g.time.carCalls;
    if (resource == g.worldRes && resource && context == g.context && SUCCEEDED(hr) && mapped)
        g.worldMapped = mapped->pData;
    return hr;
}

// The per-draw buffer's matrix, read as its update ends (its memory is still ours to read until the call passes on).
inline void unmap_resource(UnmapFn real, ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub) {
    if (timeHooks)
        ++g.time.carCalls;
    if (resource == g.worldRes && g.worldMapped && g.carActive && context == g.context) {
        SampledTimed timed(g.time.carReads, carSampleRead);
        std::memcpy(g.matrix, static_cast<const uint8_t*>(g.worldMapped) + g.worldCbOffset, sizeof(g.matrix));
        g.worldMapped = nullptr;
        g.carMatrix = true;
        ++g.stats.carMatrixReads;
    }
    real(context, resource, sub);
}

inline void ia_set_vertex_buffers(IaVbFn real, ID3D11DeviceContext* context, UINT start, UINT n,
                                  ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
    if (context == g.context && !g.replaying) {
        if (timeHooks)
            ++g.time.carCalls;
        if (start == 0 && n > 0) {
            g.iaVb = buffers ? buffers[0] : nullptr;
            g.iaVbOffset = offsets ? offsets[0] : 0;
        }
    }
    real(context, start, n, buffers, strides, offsets);
}

inline void ia_set_index_buffer(IaIbFn real, ID3D11DeviceContext* context, ID3D11Buffer* buffer, DXGI_FORMAT format,
                                UINT offset) {
    if (context == g.context && !g.replaying) {
        if (timeHooks)
            ++g.time.carCalls;
        g.iaIb = buffer;
        g.iaIbOffset = offset;
    }
    real(context, buffer, format, offset);
}

// ---- Car slots (mvr_car_slots): the slot target beside the scene's, the slot twins, the blend states' twins ----

// The blend state twins, under the game's state as private data: [0] target 1 kept (the draw doesn't write depth: a
// transparent leaves the surface's slot), [1] target 1 written. Each: independent blend on, the game's targets as it set
// them (with independent blend off, target 0's for every target), target 1 unblended.
constexpr GUID kSlotBlendTwinGuid[2] = {
    {0x4f7d2a18, 0x93c5, 0x4e6b, {0xb0, 0x21, 0x7c, 0x5e, 0x08, 0xd4, 0x3a, 0x96}},
    {0xd18e6c03, 0x2b47, 0x4a9f, {0x85, 0x6d, 0x1e, 0xa3, 0x70, 0xc9, 0x5b, 0x24}}};

D3D11_BLEND_DESC blend_twin_desc(const D3D11_BLEND_DESC& game, bool write) {
    D3D11_BLEND_DESC t = game;
    if (!game.IndependentBlendEnable)
        for (int i = 1; i < 8; ++i)
            t.RenderTarget[i] = game.RenderTarget[0];
    t.IndependentBlendEnable = TRUE;
    t.RenderTarget[1] = {FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO,
                         D3D11_BLEND_OP_ADD, static_cast<UINT8>(write ? D3D11_COLOR_WRITE_ENABLE_RED : 0)};
    return t;
}

// The twin of the game's blend state `game` (null: D3D11's default), made the first time it's needed.
ID3D11BlendState* blend_twin(ID3D11BlendState* game, bool write) {
    if (!game)
        return g.defaultBlendTwin[write ? 1 : 0].Get();
    IUnknown* unknown = nullptr;
    UINT size = sizeof(unknown);
    if (SUCCEEDED(game->GetPrivateData(kSlotBlendTwinGuid[write ? 1 : 0], &size, &unknown)) && unknown) {
        ComPtr<ID3D11BlendState> t;
        unknown->QueryInterface(IID_PPV_ARGS(&t));
        unknown->Release();
        return t.Get();  // raw: the game's state holds it
    }
    D3D11_BLEND_DESC desc;
    game->GetDesc(&desc);
    const D3D11_BLEND_DESC twin = blend_twin_desc(desc, write);
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    ComPtr<ID3D11BlendState> t;
    if (FAILED(device->CreateBlendState(&twin, &t)))
        return game;  // as the game's: target 1 as it would be
    game->SetPrivateDataInterface(kSlotBlendTwinGuid[write ? 1 : 0], t.Get());
    ++g.stats.blendTwins;
    return t.Get();
}

void realOMSetBlendState(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT* factor, UINT mask) {
    call_real<kHookBlend>(c, bs, factor, mask);
}

// The blend state the draws should have now: the game's, or its twin while the slot target is bound.
void apply_blend() {
    if (g.slotsBound) {
        realOMSetBlendState(g.context, blend_twin(g.gameBlend, g.depthWrites), g.blendFactor, g.sampleMask);
        g.blendSwapped = true;
    } else if (g.blendSwapped) {
        realOMSetBlendState(g.context, g.gameBlend, g.blendFactor, g.sampleMask);
        g.blendSwapped = false;
    }
}

bool depth_writes(ID3D11DepthStencilState* ds) {
    if (!ds)
        return true;  // D3D11's default: depth on, written
    D3D11_DEPTH_STENCIL_DESC d;
    ds->GetDesc(&d);
    return d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
}

using BlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT*, UINT);
using DepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);

inline void om_set_blend_state(BlendFn real, ID3D11DeviceContext* context, ID3D11BlendState* bs, const FLOAT* factor,
                               UINT mask) {
    if (context == g.context && !g.replaying) {
        g.gameBlend = bs;
        for (int i = 0; i < 4; ++i)
            g.blendFactor[i] = factor ? factor[i] : 1.0f;
        g.sampleMask = mask;
        if (g.slotsBound) {
            Timed timed(g.time.slotState);
            real(context, blend_twin(bs, g.depthWrites), g.blendFactor, mask);
            g.blendSwapped = true;
            return;
        }
        g.blendSwapped = false;
    }
    real(context, bs, factor, mask);
}

inline void om_set_depth_stencil_state(DepthFn real, ID3D11DeviceContext* context, ID3D11DepthStencilState* ds,
                                       UINT ref) {
    real(context, ds, ref);
    if (context == g.context && !g.replaying) {
        const bool writes = depth_writes(ds);
        if (writes != g.depthWrites) {
            g.depthWrites = writes;
            if (g.slotsBound) {
                Timed timed(g.time.slotState);
                apply_blend();
            }
        }
    }
}

// Is render target view `rtv` of the scene's target? With MSAA off the tracker's learned single-sample target; with
// MSAA on the open pass's multisample target, until its resolve (the pass began at its bind with a depth target, and
// the game binds no other multisample target with one before the resolve). The last view asked is remembered with its
// texture's description (the slot target is made to it).
bool is_scene_rtv(ID3D11RenderTargetView* rtv) {
    if (rtv != g.sceneRtvChecked) {
        g.sceneRtvChecked = rtv;
        g.sceneRtvResource = nullptr;
        g.sceneRtvDesc = {};
        ComPtr<ID3D11Resource> r;
        rtv->GetResource(&r);
        ComPtr<ID3D11Texture2D> texture;
        if (r && SUCCEEDED(r.As(&texture))) {
            g.sceneRtvResource = r.Get();
            texture->GetDesc(&g.sceneRtvDesc);
        }
    }
    g.sceneCheckedAgainst = tr.scene;
    if (g.sceneRtvDesc.SampleDesc.Count > 1)  // (the caller is inside a pass: with the tracker driving, its own open one)
        g.sceneRtvIsScene = !g.afterResolve && !(tr.on && (!tr.open || tr.singleSample || tr.resolved));
    else
        g.sceneRtvIsScene = tr.scene && g.sceneRtvResource == tr.scene && tr.sceneDesc.SampleDesc.Count == 1;
    return g.sceneRtvIsScene;
}

// The slot target at the scene target's size and sample count (made again if they changed): render targets bound
// together must share both. With MSAA on it is multisample too (each sample takes the part's slot; DLSS's vectors read
// sample 0, as they do of the depth).
bool ensure_slot_target() {
    const UINT w = g.sceneRtvDesc.Width, h = g.sceneRtvDesc.Height;
    const UINT samples = (std::max)(g.sceneRtvDesc.SampleDesc.Count, 1u), quality = g.sceneRtvDesc.SampleDesc.Quality;
    if (g.slotTex && g.slotW == w && g.slotH == h && g.slotSamples == samples && g.slotQuality == quality)
        return true;
    g.slotTex.Reset();
    g.slotRtv.Reset();
    g.slotSrv.Reset();
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    UINT levels = 1;
    if (samples > 1 && (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R16_UINT, samples, &levels)) || quality >= levels)) {
        g.slotsOn = false;
        simqvfr::logf("lmu mvr: car slots: no R16_UINT target at the scene's %ux multisampling (quality %u of %u levels); off: the "
                      "other cars keep the world's vectors",
                      samples, quality, levels);
        return false;
    }
    const D3D11_TEXTURE2D_DESC t = {w, h, 1, 1, DXGI_FORMAT_R16_UINT, {samples, quality}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (!w || !h || FAILED(device->CreateTexture2D(&t, nullptr, &g.slotTex)) ||
        FAILED(device->CreateRenderTargetView(g.slotTex.Get(), nullptr, &g.slotRtv)) ||
        FAILED(device->CreateShaderResourceView(g.slotTex.Get(), nullptr, &g.slotSrv))) {
        g.slotTex.Reset();
        g.slotsOn = false;
        simqvfr::logf("lmu mvr: car slots: the %ux%u x%u slot target couldn't be made; off", w, h, samples);
        return false;
    }
    g.slotW = w;
    g.slotH = h;
    g.slotSamples = samples;
    g.slotQuality = quality;
    simqvfr::logf("lmu mvr: car slots: slot target %ux%u x%u (R16_UINT, %.0f MB)", w, h, samples, w * 2.0 * h * samples / 1048576.0);
    return true;
}

// The slot view for this draw at t127: slot `slot`'s (its word slot + 1), or none (-1: the draw isn't a car part with
// a frame before; unbound reads 0).
uint32_t carSampleView = 0;  // bind_slot_view's 1 in 16 (SampledTimed's counter)
void bind_slot_view(int slot) {
    ID3D11ShaderResourceView* view =
        slot >= 0 && static_cast<size_t>(slot) < g.slotViews.size() ? g.slotViews[slot].Get() : nullptr;
    if (view == g.slotViewBound)
        return;
    SampledTimed timed(g.time.slotViews, carSampleView);
    g.context->PSSetShaderResources(127, 1, &view);
    g.slotViewBound = view;
    if (view) {
        ++g.stats.slotViews;
        ++g.slotViewsThisPass;
    }
}

// After the game's bind of render targets in a pass: the scene's target alone with a depth target gets the slot target
// beside it (cleared over the pass's views the first time); anything else goes without.
void slot_bind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g.slotsOn)
        return;
    Timed timed(g.time.slotState);
    const bool was = g.slotsBound;
    g.slotsBound = false;
    if (n == 1 && rtvs && rtvs[0] && dsv && g.carActive && is_scene_rtv(rtvs[0]) && ensure_slot_target()) {
        if (!g.slotsCleared && g.context1) {
            const UINT fw = g.view.focusRenderW ? g.view.focusRenderW : g.view.focusW;
            const UINT fh = g.view.focusRenderH ? g.view.focusRenderH : g.view.focusH;
            const D3D11_RECT rects[2] = {{0, 0, static_cast<LONG>(g.view.eyeW), static_cast<LONG>(g.view.eyeH)},
                                         {static_cast<LONG>(g.view.eyeW), 0, static_cast<LONG>(g.view.eyeW + fw),
                                          static_cast<LONG>(fh)}};
            const FLOAT zero[4] = {};
            g.context1->ClearView(g.slotRtv.Get(), zero, rects, g.drawFocus && fw && fh ? 2 : 1);
            g.slotsCleared = true;
            ++g.stats.slotPasses;
        }
        if (g.slotsCleared) {
            ID3D11RenderTargetView* two[2] = {rtvs[0], g.slotRtv.Get()};
            realOMSetRenderTargets(g.context, 2, two, dsv);
            g.slotsBound = true;
            ++g.stats.slotBinds;
        }
    }
    if (was != g.slotsBound) {
        if (!g.slotsBound)
            bind_slot_view(-1);
        apply_blend();
        update_ps();
    }
}

// The slot target off the pipeline (before the post reads the scene, and at a pass's end): the game's targets as bound,
// without it.
void slot_unbind() {
    if (!g.slotsBound)
        return;
    Timed timed(g.time.slotState);
    ID3D11RenderTargetView* rtvs[2] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g.context->OMGetRenderTargets(2, rtvs, &dsv);
    realOMSetRenderTargets(g.context, 1, rtvs, dsv);
    for (ID3D11RenderTargetView* r : rtvs)
        if (r)
            r->Release();
    if (dsv)
        dsv->Release();
    g.slotsBound = false;
    bind_slot_view(-1);
    apply_blend();
    update_ps();
}

// Is this vertex buffer dynamic (the game's ring)? D3D is asked once a frame per buffer: an eye pass changes its vertex
// buffer at ~550 of its ~1,400 draws but has ~160 of them (lmu-ai_frame11731.rdc), and each GetDesc is a cache miss in
// the runtime's object. The answer is the frame's alone (a buffer freed since can't pass for one checked before at its
// address).
bool vb_dynamic(ID3D11Buffer* vb) {
    if (!vb)
        return false;
    const size_t i = static_cast<size_t>((reinterpret_cast<uintptr_t>(vb) * 0x9E3779B97F4A7C15ull) >> 55);  // 9 bits
    auto& seen = g.vbSeen[i & (g.vbSeen.size() - 1)];
    if (seen.vb == vb && seen.frame == g.carFrame)
        return seen.dynamic;
    D3D11_BUFFER_DESC d{};
    vb->GetDesc(&d);
    seen = {vb, g.carFrame, d.Usage == D3D11_USAGE_DYNAMIC};
    ++g.stats.carVbAsked;
    return seen.dynamic;
}

// A scene draw in an eye pass whose vertex shader reads a worldMatrix: its key and the matrix last written into the
// per-draw buffer go to the car parts' record. The per-draw buffer is learned from such a draw (the buffer at the
// shader's worldMatrix slot); the draw that teaches it has no matrix yet.
int car_draw(UINT count, UINT start, INT base) {
    if (!g.carTrack || g.vsWorldSlot < 0 || !g.depthBound)
        return -1;
    ID3D11Buffer* cb = g.vsCb[g.vsWorldSlot];
    if (!cb)
        return -1;
    if (cb != g.worldCb) {
        D3D11_BUFFER_DESC d{};
        cb->GetDesc(&d);
        if (d.ByteWidth < g.vsWorldOffset + sizeof(g.matrix)) {  // the matrix wouldn't fit in it: never read past its end
            ++g.stats.carNoMatrix;
            return -1;
        }
        g.worldCbHeld = cb;  // held: its address never another resource's while it's the one compared
        g.worldCb = cb;
        g.worldRes = cb;
        g.worldCbOffset = g.vsWorldOffset;
        g.worldMapped = nullptr;
        g.carMatrix = false;
        ++g.stats.carBufferLearned;
        return -1;
    }
    if (!g.carMatrix) {
        ++g.stats.carNoMatrix;
        return -1;
    }
    if (g.vsWorldOffset != g.worldCbOffset) {  // read at the learned offset only (one copy per Unmap)
        ++g.stats.carOtherOffset;
        return -1;
    }
    // A dynamic vertex buffer is a ring the game refills each frame (lmu-ai_frame11731.rdc: a 1.4 MB one, mapped 246
    // times in the frame, ~100 car-side draws): its draws' base vertex and offset move with the ring, so they're keyed by
    // the buffer, the index buffer and the index range alone (unique within a frame there; the index buffer's ranges
    // stay put). Checked once per vertex buffer bound (consecutive draws share one).
    if (g.iaVb != g.vbChecked) {
        g.vbChecked = g.iaVb;
        g.vbDynamic = vb_dynamic(g.iaVb);
    }
    CarDrawKey key{g.iaVb, g.iaIb, g.iaVbOffset, g.iaIbOffset, start, count, base};
    if (g.vbDynamic) {
        key.vbOffset = 0;
        key.base = 0;
        ++g.stats.carDynamicVb;
    }
    bool moved = false;
    const int slot = g.cars.draw(key, g.matrix, moved);
    return moved ? slot : -1;  // the slot to bind (car slots): a part with a frame before
}

// Single-pass stereo refused: instancing for the rest of the session if its twins are there (the next pass's start
// hooks the draws it needs), else no focus views from the eye passes. True: instancing took over.
bool take_over_with_instancing() {
    if (!g.canInstance || g.instanced) {
        g.broken = true;
        simqvfr::log_line("lmu mvr: NVIDIA's multi-view rendering refused: only passes 2-3 draw the focus views");
        return false;
    }
    g.instanced = true;
    ++g.stats.instancedTakeovers;
    simqvfr::log_line("lmu mvr: NVIDIA's multi-view rendering refused: instancing draws the focus views from now on "
                      "(each draw twice, the vertex shader picking the viewport)");
    return true;
}

// Twin, two views and the second viewport exactly while a focus-drawing pass has a depth target and a shader with a twin.
void update() {
    ID3D11VertexShader* twin = nullptr;
    // The scene's draws (a depth target bound) and, after the focus view's final blit, the overlays into the eye's final.
    const bool can = g.drawFocus && !g.broken && (g.depthBound || g.overlayTarget) && g.gameVs && !g.gsBound &&
                     !g.hsBound && !g.dsBound && !g.soBound;
    bool want = can;
    if (want) {
        twin = twin_of(g.gameVs);
        want = twin != nullptr;
    }
    if (want != g.stereo && g.instanced) {  // no mode to switch: the draws are doubled while the twin is bound
        g.stereo = want;
        ++g.stats.toggles;
    } else if (want != g.stereo) {
        Timed timed(g.time.switches);
        if (g.twoViews(g.context, want) != 0) {
            ++g.stats.stereoErrors;
            if (want) {
                want = false;
                twin = nullptr;
                if (take_over_with_instancing() && can) {
                    twin = twin_of(g.gameVs);
                    want = twin != nullptr;
                    g.stereo = want;
                }
            }
        } else {
            g.stereo = want;
            ++g.stats.toggles;
        }
    }
    ID3D11VertexShader* bind = want ? twin : g.gameVs;
    if (bind != g.boundVs) {
        realVSSetShader(g.context, bind, g.instanceCount ? g.instances : nullptr, g.instanceCount);
        g.boundVs = bind;
        ++(want ? g.stats.twinBinds : g.stats.plainBinds);
        g.passTwinBinds += want ? 1 : 0;
    }
    if (want != g.on) {
        g.on = want;
        if (want) {
            g.context->VSSetConstantBuffers(13, 1, g.remapCb.GetAddressOf());
            g.biasCbBound = false;  // texture sharpening's buffer bound again with the next twin (the game unbinds it)
        }
        apply_viewports();
        apply_scissors();
        update_ps();  // texture sharpening: the twin exactly while two views are on
    }
}

void STDMETHODCALLTYPE vs_set_shader(ID3D11DeviceContext* context, ID3D11VertexShader* vs,
                                     ID3D11ClassInstance* const* instances, UINT count) {
    if (context == g.context && !g.replaying) {  // every game bind, so a draw anywhere knows its shader's role (R6)
        const TwinEntry* e = vs ? &shader_entry(vs) : nullptr;
        g.vsRole = e ? e->role : VsRole::none;
        g.vsWorldSlot = e ? e->worldSlot : -1;  // car tracking: where it reads its worldMatrix
        g.vsWorldOffset = e ? e->worldOffset : 0;
    }
    if (!ours(context)) {
        realVSSetShader(context, vs, instances, count);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    g.gameVs = vs;
    g.instanceCount = std::min<UINT>(count, 256);
    for (UINT i = 0; i < g.instanceCount; ++i)
        g.instances[i] = instances ? instances[i] : nullptr;
    g.boundVs = reinterpret_cast<ID3D11VertexShader*>(~uintptr_t{0});  // the game's call always takes effect
    update();
}

template <int H, typename Shader, bool State::*bound>
void STDMETHODCALLTYPE later_stage(ID3D11DeviceContext* context, Shader* shader, ID3D11ClassInstance* const* instances,
                                   UINT count) {
    if (!ours(context)) {
        call_real<H>(context, shader, instances, count);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    call_real<H>(context, shader, instances, count);
    g.*bound = shader != nullptr;
    update();
}

void STDMETHODCALLTYPE so_set_targets(ID3D11DeviceContext* context, UINT n, ID3D11Buffer* const* buffers,
                                      const UINT* offsets) {
    if (!ours(context)) {
        realSOSetTargets(context, n, buffers, offsets);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realSOSetTargets(context, n, buffers, offsets);
    g.soBound = false;
    for (UINT i = 0; i < n && buffers; ++i)
        g.soBound = g.soBound || buffers[i] != nullptr;
    update();
}

// After this pass's focus final blit (R3c): whether the first render target is the eye's final (an overlay's draw).
bool overlay_target(UINT n, ID3D11RenderTargetView* const* rtvs) {
    if (!g.focusBlitted || !n || !rtvs || !rtvs[0])
        return false;
    ComPtr<ID3D11Resource> r;
    rtvs[0]->GetResource(&r);
    return r && r.Get() == g.eyeFinal[g.pass & 1].Get();
}

// DLAA: which depth target a pass's render targets are drawn with (inside the tracked passes only: the views are held).
void note_scene_bind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!dsv || !n || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL || !rtvs || !rtvs[0] || !simqvfr::dlss_wanted())
        return;
    for (State::SceneBind& b : g.sceneBinds)
        if (b.rtv.Get() == rtvs[0]) {
            if (b.dsv.Get() != dsv)
                b.dsv = dsv;
            return;
        }
    State::SceneBind& b = g.sceneBinds[g.sceneBindNext++ % std::size(g.sceneBinds)];
    b.rtv = rtvs[0];
    b.dsv = dsv;
}

// The depth target last bound with a render target on `colour` (the scene's multisample target at its resolve).
ID3D11DepthStencilView* scene_depth(ID3D11Resource* colour) {
    for (State::SceneBind& b : g.sceneBinds) {
        if (!b.rtv || !b.dsv)
            continue;
        ComPtr<ID3D11Resource> r;
        b.rtv->GetResource(&r);
        if (r.Get() == colour)
            return b.dsv.Get();
    }
    return nullptr;
}

void STDMETHODCALLTYPE om_set_render_targets(ID3D11DeviceContext* context, UINT n, ID3D11RenderTargetView* const* rtvs,
                                             ID3D11DepthStencilView* dsv) {
    if (!ours(context)) {
        realOMSetRenderTargets(context, n, rtvs, dsv);
        if (tr.on && context == g.context && !g.replaying)
            track_bind(n, rtvs, dsv);
        if (ours(context)) {  // the bind that began a tracked pass: its scene's targets
            note_scene_bind(n, rtvs, dsv);
            slot_bind(n, rtvs, dsv);
        } else if (tr.on && context == g.context && !g.replaying) {  // MSAA off: what the scene's target may be
            note_drawn_with_depth(n, rtvs, dsv);
        }
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realOMSetRenderTargets(context, n, rtvs, dsv);
    if (tr.on)
        track_bind(n, rtvs, dsv);
    note_scene_bind(n, rtvs, dsv);
    if (ours(context))  // (a bind the tracker ended the pass at isn't the pass's)
        slot_bind(n, rtvs, dsv);
    g.depthBound = dsv != nullptr;
    g.overlayTarget = overlay_target(n, rtvs);
    update();
}

void STDMETHODCALLTYPE om_set_render_targets_and_uavs(ID3D11DeviceContext* context, UINT n,
                                                      ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                                      UINT uavStart, UINT uavCount,
                                                      ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (!ours(context)) {
        realOMSetRenderTargetsAndUavs(context, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
        if (tr.on && context == g.context && !g.replaying && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            track_bind(n, rtvs, dsv);
        if (ours(context))
            note_scene_bind(n, rtvs, dsv);
        else if (tr.on && context == g.context && !g.replaying)
            note_drawn_with_depth(n, rtvs, dsv);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    realOMSetRenderTargetsAndUavs(context, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
    if (tr.on && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        track_bind(n, rtvs, dsv);
    note_scene_bind(n, rtvs, dsv);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && ours(context))
        slot_bind(0, nullptr, nullptr);  // car slots: not beside unordered access views
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        g.depthBound = dsv != nullptr;
        g.overlayTarget = overlay_target(n, rtvs);
        update();
    }
}

void STDMETHODCALLTYPE rs_set_viewports(ID3D11DeviceContext* context, UINT n, const D3D11_VIEWPORT* vps) {
    if (!ours(context)) {
        realRSSetViewports(context, n, vps);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    g.vpCount = std::min<UINT>(n, kMaxRects);
    if (vps)
        std::memcpy(g.vps, vps, g.vpCount * sizeof(D3D11_VIEWPORT));
    apply_viewports();
}

void STDMETHODCALLTYPE rs_set_scissor_rects(ID3D11DeviceContext* context, UINT n, const D3D11_RECT* rects) {
    if (!ours(context)) {
        realRSSetScissorRects(context, n, rects);
        return;
    }
    Timed timed(g.time.hooks);
    ++g.time.calls;
    g.scCount = std::min<UINT>(n, kMaxRects);
    if (rects)
        std::memcpy(g.scs, rects, g.scCount * sizeof(D3D11_RECT));
    if (!g.scCount)
        realRSSetScissorRects(context, 0, nullptr);
    else
        apply_scissors();
}

bool copy_region(ComPtr<ID3D11Texture2D>& dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w, unsigned h);

// The resolve target cleared (its view made once: it holds the resource, so the pointer stays its own).
ComPtr<ID3D11RenderTargetView> clearRtv;
ID3D11Resource* clearFor = nullptr;

void clear_target(ID3D11Resource* target) {
    if (clearFor != target) {
        clearRtv.Reset();
        clearFor = target;
        ComPtr<ID3D11Device> device;
        g.context->GetDevice(&device);
        if (FAILED(device->CreateRenderTargetView(target, nullptr, &clearRtv)))
            clearRtv.Reset();
    }
    if (clearRtv) {
        const float black[4] = {0, 0, 0, 0};
        g.context->ClearRenderTargetView(clearRtv.Get(), black);
    }
}

void post_begins(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* drawn);

void STDMETHODCALLTYPE resolve_subresource(ID3D11DeviceContext* context, ID3D11Resource* dst, UINT dstSub,
                                           ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format) {
    if (pt.recording && context == g.context && !g.replaying) {
        const std::string from = pt_tex(src), to = pt_tex(dst);
        pt_event("R %s > %s, %s", from.c_str(), to.c_str(),
                 tr.open ? ("in tracked pass " + std::to_string(tr.pass)).c_str() : "outside the tracked passes");
    }
    if (tr.on && tr.open && context == g.context && !g.replaying && src) {
        ComPtr<ID3D11Texture2D> source;
        D3D11_TEXTURE2D_DESC sd{};
        if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&source)))) {
            source->GetDesc(&sd);
            tr.resolved = tr.resolved || sd.SampleDesc.Count > 1;
        }
    }
    bool multisample = false;
    if (ours(context) && g.pass <= 3 && src && dst) {
        ComPtr<ID3D11Texture2D> source;
        if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&source)))) {
            D3D11_TEXTURE2D_DESC d;
            source->GetDesc(&d);
            multisample = d.SampleDesc.Count > 1;
        }
    }
    const int eye = g.pass & 1;
    if (multisample) {
        stamp(g.pass, 1);  // the scene ends, the post-processing starts
        g.afterResolve = true;
    }
    if (multisample && g.replaceResolve && g.keptOk[eye]) {
        // Gate D: the scene this pass skipped is the focus view its eye's pass drew: into the top-left instead. Cleared
        // first: the post-processing covers the top-left at one scale, beyond the focus view where pass 1's images still
        // are (they would bloom into it).
        const MvrPassView& v = g.viewOf[eye];
        clear_target(dst);
        const D3D11_BOX box{0, 0, 0, v.focusW, v.focusH, 1};
        g.context->CopySubresourceRegion(dst, dstSub, 0, 0, 0, g.kept[eye].Get(), 0, &box);
        g.resolved[g.pass] = dst;
        ++g.stats.replacedResolves;
        return;
    }
    realResolveSubresource(context, dst, dstSub, src, srcSub, format);
    if (!multisample)
        return;
    ++g.stats.resolves;
    post_begins(context, dst, src);
}

// The pass's post-processing begins on `scene` (the resolve's target; with MSAA off the scene's own target, at YEBIS's
// copy, before it reads it), which was drawn into `drawn` (the multisample target, or the same): DLAA on the eye and its
// focus view, gate D's kept region, the focus post's window.
void save_slot_image(int eye, ID3D11Resource* scene);
void post_begins(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* drawn) {
    ID3D11Resource* const dst = scene;
    const int eye = g.pass & 1;
    g.resolved[g.pass] = dst;
    // Car slots: the slot target off the pipeline (the post doesn't draw into it; DLSS reads it), each part's motion
    // uploaded for this pass's vectors, now and then the gate image (level 1 and up).
    bool slotsDrawn = false;
    if (g.slotsOn) {
        slot_unbind();
        slotsDrawn = g.slotsCleared && g.pass < 2;
        if (slotsDrawn && g.motionBuffer) {
            {
                // Only the parts first drawn since the last upload (pass 0: the frame's; pass 1: the few its eye alone
                // drew), only their range of slots: run 25's A/B found the cars' motion dearer than its tracking alone.
                Timed timed(g.time.motion);
                size_t first = 0, last = 0;
                const size_t moving = g.cars.update_motions(g.motion, first, last);
                if (moving && first <= last && g.motion.size() * sizeof(float) == size_t{kCarSlots} * 48) {
                    const D3D11_BOX box = {static_cast<UINT>(first * 48), 0, 0, static_cast<UINT>((last + 1) * 48), 1,
                                           1};
                    g.context->UpdateSubresource(g.motionBuffer.Get(), 0, &box, g.motion.data() + first * 12, 0, 0);
                    ++g.stats.motionUploads;
                    g.stats.motionSlots += moving;
                }
            }
            // The gate image (diagnostics runs only: run 26b's two froze the game ~250 ms each, max frame 508 ms).
            const bool armed = g.slotImages && g.pass == 0 && g.stats.slotImages < 4 && g.slotViewsThisPass >= 50 &&
                               g.framesSinceInstall >= g.slotImageAfter;
            if (armed) {
                g.slotImageFrame = g.framesSinceInstall;
                g.slotImageAfter = g.framesSinceInstall + 2700;  // the next a while later
            }
            if (g.slotImageFrame == g.framesSinceInstall && g.slotImageFrame)
                save_slot_image(eye, dst);  // both eyes of the frame
        }
    }
    // A focus view drawn smaller (DLSS super resolution) goes through the module even if DLSS went off since the pass
    // was set up: it stretches what DLSS can't upscale.
    const bool smaller = (g.view.focusRenderW && g.view.focusRenderW < g.view.focusW) ||
                         (g.view.focusRenderH && g.view.focusRenderH < g.view.focusH);
    if (g.pass < 2 && (simqvfr::dlss_wanted() || smaller) && g.view.eyeW && g.view.eyeH) {
        // DLAA on the eye (and its focus view beside it) before anything reads the resolved scene. Its draws and copies
        // pass these hooks untouched; single-pass stereo is off meanwhile (the scene's last shader may have left it on).
        simqvfr::DlssPass dp;
        dp.eye = eye;
        dp.eyeWidth = g.view.eyeW;
        dp.eyeHeight = g.view.eyeH;
        dp.focus = g.drawFocus && g.passTwinBinds > 0;
        dp.focusWidth = g.view.focusW;
        dp.focusHeight = g.view.focusH;
        dp.focusRenderWidth = g.view.focusRenderW;
        dp.focusRenderHeight = g.view.focusRenderH;
        std::memcpy(dp.remap, g.view.remap, sizeof(dp.remap));
        if (slotsDrawn && g.slotsLevel >= 2 && g.slotSrv && g.motionSrv) {  // DLSS's vectors take the parts' motion
            dp.objectSlots = g.slotSrv.Get();
            dp.objectMotion = g.motionSrv.Get();
            dp.objectCount = kCarSlots;
        }
        const bool stereo = g.stereo && !g.instanced;
        if (stereo)
            g.twoViews(g.context, false);
        const bool was = g.replaying;
        g.replaying = true;
        simqvfr::dlss_after_resolve(context, dst, scene_depth(drawn), dp);
        g.replaying = was;
        if (stereo)
            g.twoViews(g.context, true);
    }
    if (g.keepFocus && g.pass < 2 && g.drawFocus && g.passTwinBinds > 0) {
        g.keptOk[eye] = copy_region(g.kept[eye], dst, g.view.eyeW, 0, g.view.focusW, g.view.focusH);
        g.stats.keptRegions += g.keptOk[eye] ? 1 : 0;
    }
    if (g.focusReplay && g.pass < 2 && g.drawFocus && g.passTwinBinds > 0 && g.view.box.valid && !g.focusPostFailed) {
        if (!g.focusPost.ready()) {
            ComPtr<ID3D11Device> device;
            g.context->GetDevice(&device);
            if (!g.focusPost.initialize(device.Get())) {
                g.focusPostFailed = true;
                simqvfr::log_line("lmu mvr: the focus post couldn't be made (shaders, states, deferred context); off");
            }
        }
        g.postWindow = g.focusPost.ready();
    }
    if (pt.recording)
        pt_event("W pass %d: the focus post's window %s (focus view drawn %d, twin binds %llu, box %d)", g.pass,
                 g.postWindow ? "open" : "not opened", g.drawFocus ? 1 : 0,
                 static_cast<unsigned long long>(g.passTwinBinds), g.view.box.valid ? 1 : 0);
}

// The focus post at one of the pass's post-processing draws (the post window open): after YEBIS's copy of the resolved
// scene (the copy's pixel shader reading the resolve's target; its downsamples read other textures) the focus view's part
// copied likewise; after its composite (with the eye's glare and exposure bound) the focus view's composite.
// A texture's size and format for the log ("2076x1667 f10"), "-" for none.
std::string describe(ID3D11Resource* resource) {
    ComPtr<ID3D11Texture2D> t;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&t))))
        return "-";
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    char s[48];
    std::snprintf(s, sizeof(s), "%ux%u f%d", d.Width, d.Height, static_cast<int>(d.Format));
    return s;
}

// After the copy and before a composite is seen: each draw's pixel shader (its checksum, from the probe) and target,
// for the log when the pass ends without one ("r" marks the resolve's target); a draw into the resolve's target with
// a shader that isn't the composite we know logged with its inputs, once per shader.
void trace_post_draw(ID3D11DeviceContext* context, ID3D11PixelShader* ps) {
    uint32_t c[4] = {};
    UINT size = sizeof(c);
    const bool named = ps && SUCCEEDED(ps->GetPrivateData(kShaderChecksumGuid, &size, c)) && size == sizeof(c);
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    const bool intoResolved = target && target.Get() == g.resolved[g.pass].Get();
    if (g.postTrace.size() < 3000) {
        char s[96];
        std::snprintf(s, sizeof(s), " %08x%s>%s%s", named ? c[0] : 0, named ? "" : "?", describe(target.Get()).c_str(),
                      intoResolved ? " r" : "");
        g.postTrace += s;
    }
    if (!intoResolved)
        return;
    ++g.stats.focusUnknownComposites;
    for (int i = 0; i < g.unknownCount; ++i)
        if (std::memcmp(g.unknown[i], c, sizeof(c)) == 0)
            return;
    if (g.unknownCount == static_cast<int>(std::size(g.unknown)))
        return;
    std::memcpy(g.unknown[g.unknownCount++], c, sizeof(c));
    std::string inputs;
    ID3D11ShaderResourceView* srvs[8] = {};
    context->PSGetShaderResources(0, 8, srvs);
    for (int i = 0; i < 8; ++i) {
        ComPtr<ID3D11Resource> r;
        if (srvs[i]) {
            srvs[i]->GetResource(&r);
            srvs[i]->Release();
        }
        if (r)
            inputs += " t" + std::to_string(i) + " " + describe(r.Get()) + (r.Get() == g.resolved[g.pass].Get() ? " r" : "");
    }
    ID3D11SamplerState* samplers[8] = {};
    context->PSGetSamplers(0, 8, samplers);
    int samplerCount = 0;
    for (ID3D11SamplerState* s : samplers)
        if (s) {
            ++samplerCount;
            s->Release();
        }
    simqvfr::logf("lmu mvr: focus post: pass %d: after the copy, pixel shader %08x-%08x-%08x-%08x%s draws into the "
                  "resolve's target (%s), not the composite we know; inputs:%s; %d samplers%s",
                  g.pass, c[0], c[1], c[2], c[3], named ? "" : " (not named: created before the probe?)",
                  describe(target.Get()).c_str(), inputs.c_str(), samplerCount,
                  g.unknownCount == static_cast<int>(std::size(g.unknown)) ? "; no more of these logged" : "");
}

// After the copy: YEBIS's composite by what it does, whatever its tone curve (LMU switches between variants, e.g. in the
// pits and on track: 60678228, b7eb8877, 2026-11-09): a draw into the resolve's target reading the copy's output (t0),
// a glare (t1) and the 1x1 exposure (t2).
bool composite_by_bindings(ID3D11DeviceContext* context) {
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    if (!target || target.Get() != g.resolved[g.pass].Get() || !g.copyTarget)
        return false;
    ID3D11ShaderResourceView* srvs[3] = {};
    context->PSGetShaderResources(0, 3, srvs);
    ComPtr<ID3D11Resource> r[3];
    for (int i = 0; i < 3; ++i)
        if (srvs[i]) {
            srvs[i]->GetResource(&r[i]);
            srvs[i]->Release();
        }
    ComPtr<ID3D11Texture2D> exposure;
    D3D11_TEXTURE2D_DESC ed{};
    if (!r[0] || r[0].Get() != g.copyTarget.Get() || !r[1] || !r[2] || FAILED(r[2].As(&exposure)))
        return false;
    exposure->GetDesc(&ed);
    return ed.Width == 1 && ed.Height == 1;
}

void focus_post_draw(ID3D11DeviceContext* context) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    UINT role = 0, size = sizeof(role);
    if (!ps || FAILED(ps->GetPrivateData(kYebisRoleGuid, &size, &role)) || size != sizeof(role))
        role = 0;
    if (g.focusCopied && !g.focusComposited && role != static_cast<UINT>(YebisRole::composite)) {
        if (composite_by_bindings(context)) {
            role = static_cast<UINT>(YebisRole::composite);
            ++g.stats.focusCompositesByBindings;
            uint32_t c[4] = {};
            UINT cs = sizeof(c);
            if (ps && SUCCEEDED(ps->GetPrivateData(kShaderChecksumGuid, &cs, c)) && cs == sizeof(c)) {
                bool seen = false;
                for (int i = 0; i < g.bindingCount && !seen; ++i)
                    seen = std::memcmp(g.byBindings[i], c, sizeof(c)) == 0;
                if (!seen && g.bindingCount < static_cast<int>(std::size(g.byBindings))) {
                    std::memcpy(g.byBindings[g.bindingCount++], c, sizeof(c));
                    simqvfr::logf("lmu mvr: focus post: pass %d: YEBIS's composite by its bindings (the copy's output, a "
                                  "glare, the 1x1 exposure into the resolve's target): pixel shader "
                                  "%08x-%08x-%08x-%08x",
                                  g.pass, c[0], c[1], c[2], c[3]);
                }
            }
        } else {
            trace_post_draw(context, ps.Get());
        }
    }
    if (!role)
        return;
    const int eye = g.pass & 1;
    if (role == static_cast<UINT>(YebisRole::copy) && !g.focusCopied) {
        ComPtr<ID3D11ShaderResourceView> srv;
        context->PSGetShaderResources(0, 1, &srv);
        ComPtr<ID3D11Resource> resource;
        if (srv)
            srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (!resource || resource.Get() != g.resolved[g.pass].Get() || FAILED(resource.As(&texture)))
            return;
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        ComPtr<ID3D11RenderTargetView> copyRtv;
        context->OMGetRenderTargets(1, &copyRtv, nullptr);
        g.copyTarget.Reset();
        if (copyRtv)
            copyRtv->GetResource(&g.copyTarget);  // the composite reads it (composite_by_bindings)
        g.replaying = true;
        g.focusCopied = g.focusPost.after_copy(context, eye, srv.Get(), d.Width, d.Height,
                                               {g.view.eyeW, 0, g.view.focusW, g.view.focusH});
        g.replaying = false;
        if (g.focusCopied) {
            ++g.stats.focusCopies;
        } else {
            ++g.stats.focusFailures;
            g.postWindow = false;
        }
    } else if (role == static_cast<UINT>(YebisRole::composite) && g.focusCopied && !g.focusComposited) {
        g.focusComposited = true;
        g.postWindow = false;  // nothing more this pass
        ComPtr<ID3D11ShaderResourceView> glare;
        context->PSGetShaderResources(1, 1, &glare);
        ComPtr<ID3D11Resource> resource;
        if (glare)
            glare->GetResource(&resource);
        ComPtr<ID3D11Texture2D> glareTexture, scene;
        D3D11_TEXTURE2D_DESC gd{}, sd{};
        bool ok = resource && SUCCEEDED(resource.As(&glareTexture)) && g.resolved[g.pass] &&
                  SUCCEEDED(g.resolved[g.pass].As(&scene));
        if (ok) {
            glareTexture->GetDesc(&gd);
            scene->GetDesc(&sd);
            // The eye's glare is the eye's share of the glare texture (YEBIS builds it at the scene's UV: glare_math.h).
            const simqvfr::ViewRegion region = yebis_pass_region(
                {0, 0, static_cast<int>(gd.Width), static_cast<int>(gd.Height)},
                static_cast<double>(g.view.eyeW) / sd.Width, static_cast<double>(g.view.eyeH) / sd.Height);
            g.replaying = true;
            ok = g.focusPost.after_composite(context, eye, region, glare_mapping(g.view.box));
            g.replaying = false;
        }
        g.compositeOk = ok;
        ++(ok ? g.stats.focusComposites : g.stats.focusFailures);
    }
}

// R3b: right after the game's final blit in a pass that made its focus composite (its pixel shader known by checksum,
// reading the resolve's target into the eye's final): the same quad once more over the focus view's region beside the
// eye (FocusPost::blit_final), with the game's vertex colour. From then on the pass's draws into the eye's final are
// overlays, doubled by MVR (update(), overlay_target).
void focus_blit(ID3D11DeviceContext* context, UINT count, UINT start) {
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    UINT role = 0, size = sizeof(role);
    if (!ps || FAILED(ps->GetPrivateData(kYebisRoleGuid, &size, &role)) || role != static_cast<UINT>(YebisRole::blit))
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> source;
    if (srv)
        srv->GetResource(&source);
    if (!source || source.Get() != g.resolved[g.pass].Get())
        return;  // the blit of something else (the desktop mirror's is outside the passes)
    g.focusBlitted = true;  // tried once a pass
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    ComPtr<ID3D11Texture2D> texture;
    if (rtv)
        rtv->GetResource(&target);
    D3D11_TEXTURE2D_DESC d{};
    if (target && SUCCEEDED(target.As(&texture)))
        texture->GetDesc(&d);
    const MvrPassView& v = g.view;
    const int eye = g.pass & 1;  // as the focus post's (after_copy, after_composite)
    // The eye's corner of its final: its rendered corner, stretched when the game's render scale isn't 1 (the blit maps
    // the whole main target over the whole final). The focus final beside it at the focus image's own size.
    const unsigned ew = final_eye_w(v), eh = final_eye_h(v);
    if (!texture || ew + v.focusW > d.Width || v.focusH > d.Height) {
        ++g.stats.focusFailures;
        if (!g.blitRefusalLogged) {
            g.blitRefusalLogged = true;
            simqvfr::logf("lmu mvr: focus post: pass %d: the eye's final (%ux%u) has no room for the focus view %ux%u beside "
                          "the eye's %u: no focus final there",
                          g.pass, d.Width, d.Height, v.focusW, v.focusH, ew);
        }
        return;
    }
    g.replaying = true;
    const bool ok = g.focusPost.blit_final(context, eye, count, start, {0, 0, ew, eh}, {ew, 0, v.focusW, v.focusH});
    g.replaying = false;
    if (!ok) {
        ++g.stats.focusFailures;
        return;
    }
    ++g.stats.focusBlits;
    if (tr.open)
        tr.blitSeen = true;
    g.eyeFinal[eye] = target;
    g.finalW[eye] = d.Width;
    g.finalH[eye] = d.Height;
    g.finalThisFrame[eye] = true;
    g.overlayTarget = true;  // the eye's final is bound
    update();
}

// Instancing: draw H made with twice its instances (the instanced twin bound: each instance's even copy into the eye's
// viewport, its odd one into the focus view's); with input layouts that read per-instance data, the bound one's twin
// (step rates doubled, mvr_probe.cpp) around it, so a pair reads its instance's data.
template <int H, typename... A>
void doubled_draw(ID3D11DeviceContext* context, A... a) {
    const std::tuple<A...> t(a...);
    ID3D11InputLayout* layout = nullptr;
    ComPtr<ID3D11InputLayout> twinLayout;
    if (mvr_probe_instanced_layouts() > 0) {
        context->IAGetInputLayout(&layout);
        IUnknown* unknown = nullptr;
        UINT size = sizeof(unknown);
        if (layout && SUCCEEDED(layout->GetPrivateData(kMvrInstancedLayoutGuid, &size, &unknown)) && unknown) {
            unknown->QueryInterface(IID_PPV_ARGS(&twinLayout));
            unknown->Release();
        }
        if (twinLayout) {
            context->IASetInputLayout(twinLayout.Get());
            ++g.stats.layoutSwaps;
        }
    }
    ++g.stats.doubledDraws;
    if constexpr (H == kHookDrawIndexed)
        call_real<kHookDrawIndexedInstanced>(context, std::get<0>(t), UINT{2}, std::get<1>(t), std::get<2>(t), UINT{0});
    else if constexpr (H == kHookDraw)
        call_real<kHookDrawInstanced>(context, std::get<0>(t), UINT{2}, std::get<1>(t), UINT{0});
    else if constexpr (H == kHookDrawIndexedInstanced)
        call_real<H>(context, std::get<0>(t), std::get<1>(t) * 2, std::get<2>(t), std::get<3>(t), std::get<4>(t));
    else if constexpr (H == kHookDrawInstanced)
        call_real<H>(context, std::get<0>(t), std::get<1>(t) * 2, std::get<2>(t), std::get<3>(t));
    else
        static_assert(H == kHookDraw, "a draw with an instance count");
    if (twinLayout)
        context->IASetInputLayout(layout);
    if (layout)
        layout->Release();
}

// The game's draw H in a pass as it reaches d3d11: doubled with instancing while two views are on, else as it was.
template <int H, typename... A>
void game_draw(ID3D11DeviceContext* context, A... a) {
    if (g.instanced && g.on && ours(context))
        doubled_draw<H>(context, a...);
    else
        call_real<H>(context, a...);
}

// mvr_ui_mips: for the panel's canvas `canvas` (the draw's pixel shader input 0), a copy of it with its mips, made once a
// frame (the canvas is drawn before the panels); null when it has its own mips, is small, or the copy can't be made.
ID3D11ShaderResourceView* ui_canvas_with_mips(ID3D11DeviceContext* context, ID3D11ShaderResourceView* canvas) {
    State::UiMips& m = g.uiMips;
    if (!m.on || m.failed || !canvas)
        return nullptr;
    ComPtr<ID3D11Resource> resource;
    canvas->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    canvas->GetDesc(&view);
    if (!resource || FAILED(resource.As(&texture)) || view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return nullptr;
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.Width < 512 || d.Height < 256)
        return nullptr;
    if (m.source == resource.Get() && m.frame == g.framesSinceInstall && m.srv)
        return m.srv.Get();
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC have{};
    if (m.texture)
        m.texture->GetDesc(&have);
    if (!m.texture || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format ||
        m.viewFormat != view.Format) {
        m.texture.Reset();
        m.srv.Reset();
        UINT support = 0;
        D3D11_TEXTURE2D_DESC td = d;
        td.MipLevels = 0;  // the whole chain
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.CPUAccessFlags = 0;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = view.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = static_cast<UINT>(-1);
        const bool ok = SUCCEEDED(device->CheckFormatSupport(view.Format, &support)) &&
                        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) &&
                        SUCCEEDED(device->CreateTexture2D(&td, nullptr, &m.texture)) &&
                        SUCCEEDED(device->CreateShaderResourceView(m.texture.Get(), &sd, &m.srv));
        if (!ok) {
            m.failed = true;
            m.texture.Reset();
            m.srv.Reset();
            simqvfr::logf("lmu mvr: the VR UI canvas (%ux%u format %d) can't have a copy with mips made: the panel "
                          "samples it as it is",
                          d.Width, d.Height, static_cast<int>(view.Format));
            return nullptr;
        }
        m.viewFormat = view.Format;
        m.texture->GetDesc(&have);
        if (!m.logged) {
            m.logged = true;
            simqvfr::logf("lmu mvr: the VR UI canvas is %ux%u with one mip level: the panel's doubled draws sample a copy "
                          "with %u (lmu_ui_mips=0: as it is)",
                          d.Width, d.Height, have.MipLevels);
        }
    }
    const bool was = g.replaying;  // our copy, not one of the game's the copy hook records
    g.replaying = true;
    context->CopySubresourceRegion(m.texture.Get(), 0, 0, 0, 0, resource.Get(), 0, nullptr);
    g.replaying = was;
    context->GenerateMips(m.srv.Get());
    m.source = resource.Get();
    m.frame = g.framesSinceInstall;
    ++g.stats.uiMipFrames;
    return m.srv.Get();
}

// R3c: after the passes, a draw into an eye's final whose focus final was made this frame, with a vertex shader that has
// a twin (the VR UI's panel): drawn with two views, the eye's remap and the focus viewport beside its own (cut to the
// part the eye rendered when it is the whole target), its canvas with mips (mvr_ui_mips), the state put back. False:
// not doubled (the caller draws it).
template <int H, typename... A>
bool double_outside(ID3D11DeviceContext* context, A... a) {
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    int eye = -1;
    for (int e = 0; e < 2 && target; ++e)
        if (g.finalThisFrame[e] && target.Get() == g.eyeFinal[e].Get())
            eye = e;
    if (eye < 0)
        return false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11ClassInstance* instances[256] = {};
    UINT instanceCount = 256;
    context->VSGetShader(&vs, instances, &instanceCount);
    const auto release = [&] {
        if (vs)
            vs->Release();
        for (UINT i = 0; i < instanceCount && i < 256; ++i)
            if (instances[i])
                instances[i]->Release();
    };
    ID3D11VertexShader* twin = twin_of(vs);
    D3D11_VIEWPORT vps[kMaxRects];
    UINT n = kMaxRects;
    context->RSGetViewports(&n, vps);
    ComPtr<ID3D11GeometryShader> gs;
    context->GSGetShader(&gs, nullptr, nullptr);
    if (!twin || gs || n != 1 || g.broken) {
        release();
        return false;
    }
    const MvrPassView& v = g.viewOf[eye];
    ID3D11Buffer* kept = nullptr;
    context->VSGetConstantBuffers(13, 1, &kept);
    context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, v.remap, 0, 0);
    context->VSSetConstantBuffers(13, 1, g.remapCb.GetAddressOf());
    bool doubled = false;
    if (g.instanced || g.twoViews(context, true) == 0) {
        const D3D11_VIEWPORT first = overlay_viewport(eye, v, vps[0]);
        const D3D11_VIEWPORT two[2] = {unshifted(first, v.overlayShift[0]),
                                       unshifted(focus_viewport_of(v, first, true), v.overlayShift[1])};
        ID3D11ShaderResourceView* canvas = nullptr;
        context->PSGetShaderResources(0, 1, &canvas);
        ID3D11ShaderResourceView* mipped = ui_canvas_with_mips(context, canvas);
        if (mipped)
            context->PSSetShaderResources(0, 1, &mipped);
        realVSSetShader(context, twin, instanceCount ? instances : nullptr, instanceCount);
        realRSSetViewports(context, 2, two);
        if (g.instanced) {
            doubled_draw<H>(context, a...);
        } else {
            call_real<H>(context, a...);
            g.twoViews(context, false);
        }
        realVSSetShader(context, vs, instanceCount ? instances : nullptr, instanceCount);
        realRSSetViewports(context, n, vps);
        if (mipped)
            context->PSSetShaderResources(0, 1, &canvas);
        if (canvas)
            canvas->Release();
        doubled = true;
        ++g.stats.overlaysOutside;
    }
    context->VSSetConstantBuffers(13, 1, &kept);
    if (kept)
        kept->Release();
    release();
    return doubled;
}

// A shader's name for the trace: its checksum's first word (the probe's private data), "*" for a vertex shader with a
// twin, "-" for none, "?" for one not named.
template <class Shader>
std::string shader_name(Shader* shader) {
    if (!shader)
        return "-";
    uint32_t c[4] = {};
    UINT size = sizeof(c);
    char s[16];
    if (FAILED(shader->GetPrivateData(kShaderChecksumGuid, &size, c)) || size != sizeof(c))
        std::snprintf(s, sizeof(s), "?%04x", static_cast<unsigned>(reinterpret_cast<uintptr_t>(shader) & 0xffff));
    else
        std::snprintf(s, sizeof(s), "%08x", c[0]);
    std::string name = s;
    if constexpr (std::is_same_v<Shader, ID3D11VertexShader>) {
        UINT twinSize = 0;
        if (shader->GetPrivateData(g.instanced ? kMvrInstancedTwinGuid : kMvrTwinGuid, &twinSize, nullptr) == S_OK &&
            twinSize)
            name += "*";
    }
    return name;
}

// The overlay trace's line for one draw (the state it ran with).
void trace_draw(ID3D11DeviceContext* context, const char* call, long long a, long long b) {
    const bool inPass = g.pass >= 0;
    const char* phase = !inPass ? "outside" : g.vsRole == VsRole::lensMask ? "mask" : g.afterResolve ? "post" : nullptr;
    if (!phase)
        return;  // a pass's scene: not traced
    if (g.traceDraws >= 800) {
        ++g.traceDropped;
        return;
    }
    ++g.traceDraws;
    ComPtr<ID3D11VertexShader> boundVs;
    context->VSGetShader(&boundVs, nullptr, nullptr);
    ID3D11VertexShader* vs = inPass ? g.gameVs : boundVs.Get();  // in a pass the twin may be bound: the game's named
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    ComPtr<ID3D11Resource> target;
    if (rtv)
        rtv->GetResource(&target);
    D3D11_VIEWPORT vps[kMaxRects];
    UINT n = kMaxRects;
    context->RSGetViewports(&n, vps);
    const bool resolvedTarget = inPass && target && target.Get() == g.resolved[g.pass].Get();
    simqvfr::logf("lmu mvr trace: f%llu p%d %s %s(%lld,%lld) vs %s ps %s > %s #%06x%s depth %s vp %u: %.0f,%.0f %.0fx%.0f%s",
                  g.gpuFrameNo, g.pass, phase, call, a, b, shader_name(vs).c_str(), shader_name(ps.Get()).c_str(),
                  describe(target.Get()).c_str(),
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(target.Get()) & 0xffffff),
                  resolvedTarget ? " (resolved)" : "", dsv ? "yes" : "no", n, n ? vps[0].TopLeftX : 0.0f,
                  n ? vps[0].TopLeftY : 0.0f, n ? vps[0].Width : 0.0f, n ? vps[0].Height : 0.0f,
                  inPass && g.on ? " two views" : "");
}

// R6c: a draw with the lens mask's vertex shader (its role): counted, drawn like the scene's (with two views in a pass
// that draws its focus view: the twin puts the hidden area where the focus view sees it, R6a), or dropped while the mask
// is stale (mvr_mask_stale). The first two logged with the state they ran with. True: dropped (the caller skips it).
bool lens_mask_draw(ID3D11DeviceContext* context, const char* call, long long a, long long b) {
    const bool drop = g.maskStale && g.maskStale->load(std::memory_order_relaxed);
    if (drop) {
        ++g.stats.maskDropped;
        ++g.maskDroppedSince;
    } else {
        ++g.stats.maskDraws;
        if (g.on)
            ++g.stats.maskDrawsTwo;
    }
    if (g.maskDrawsLogged >= 2)
        return drop;
    ++g.maskDrawsLogged;
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    ComPtr<ID3D11Resource> target, depthTarget;
    if (rtv)
        rtv->GetResource(&target);
    if (dsv)
        dsv->GetResource(&depthTarget);
    D3D11_VIEWPORT vps[kMaxRects];
    UINT n = kMaxRects;
    context->RSGetViewports(&n, vps);
    simqvfr::logf("lmu mvr: lens mask draw: f%llu p%d %s(%lld,%lld) ps %s > %s depth %s vp %u: %.0f,%.0f %.0fx%.0f; %s",
                  g.gpuFrameNo, g.pass, call, a, b, shader_name(ps.Get()).c_str(),
                  target ? describe(target.Get()).c_str() : "none",
                  depthTarget ? describe(depthTarget.Get()).c_str() : "none", n, n ? vps[0].TopLeftX : 0.0f,
                  n ? vps[0].TopLeftY : 0.0f, n ? vps[0].Width : 0.0f, n ? vps[0].Height : 0.0f,
                  drop ? "dropped (the mask is stale: a live FOV crop change)"
                  : g.on ? "two views (into the focus view too)"
                         : "the eye only");
    return drop;
}

// R6b: the VR UI panel's draw outside the passes in a frame without focus finals (the menus): the PanelFn around it.
bool panel_draw_here(ID3D11DeviceContext* context) {
    return g.panelFn && context == g.context && !g.replaying && g.pass < 0 && g.vsRole == VsRole::vruiPanel &&
           !g.finalThisFrame[0] && !g.finalThisFrame[1];
}
void call_panel_fn(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start) {
    g.replaying = true;  // its work (a resample executed on this context, the redraw) passes the hooks
    g.panelFn(context, after, count, start);
    g.replaying = false;
}

// The game's CopySubresourceRegion: passed on, remembered (mvr_copy_source).
void STDMETHODCALLTYPE copy_subresource_region(ID3D11DeviceContext* context, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z,
                                              ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box) {
    call_real<kHookCopy>(context, dst, dstSub, x, y, z, src, srcSub, box);
    if (context != g.context || g.replaying)
        return;
    if (pt.recording) {
        const std::string from = pt_tex(src), to = pt_tex(dst);
        pt_event("C %s > %s at %u,%u%s", from.c_str(), to.c_str(), x, y, box ? "" : " (whole)");
    }
    State::CopySeen& c = g.copies[g.copyNext++ % State::kCopiesKept];
    c.dst = dst;
    c.src = src;
    c.frame = g.framesSinceInstall;
}

// MSAA off (Tracker::scene): YEBIS's copy, before it draws. Outside the tracked passes, in a frame without a
// multisample scene: the single-sample texture it reads, if it was drawn with a depth target, is the scene's target,
// learned. In a pass begun on that target: the pass's post-processing begins here, before the copy reads the scene
// (post_begins: DLAA in place, the focus post's window), as it does at the resolve with MSAA on.
void copy_before(ID3D11DeviceContext* context) {
    if (tr.msaaFrame || g.afterResolve)
        return;
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    UINT role = 0, size = sizeof(role);
    if (!ps || FAILED(ps->GetPrivateData(kYebisRoleGuid, &size, &role)) || size != sizeof(role) ||
        role != static_cast<UINT>(YebisRole::copy))
        return;
    ComPtr<ID3D11ShaderResourceView> srv;
    context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11Resource> resource;
    if (srv)
        srv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)))
        return;
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.SampleDesc.Count != 1)
        return;
    if (tr.open && tr.singleSample && ours(context) && g.pass < 2) {
        if (resource.Get() != tr.scene)
            return;
        stamp(g.pass, 1);  // the scene ends, the post-processing starts
        g.afterResolve = true;
        tr.resolved = true;
        ++g.stats.singleSamplePosts;
        post_begins(context, resource.Get(), resource.Get());
        return;
    }
    if (tr.open || (resource.Get() == tr.scene && std::memcmp(&d, &tr.sceneDesc, sizeof(d)) == 0) ||
        !drawn_with_depth(resource.Get()))
        return;
    // Not every such texture is the scene's: LMU post-processes smaller views of the world the same way (a 2560x160
    // one, its mirror's by its shape, was learned in place of the 3120x2505 scene when a race session began on
    // 2026-11-11 22:28; no eye pass was found from then on, so no focus views, no DLSS). The scene's target is of the
    // eye targets' order: nothing under a quarter of an eye's final a side, nor of the scene learned before, takes
    // its place.
    {
        UINT fw = 0, fh = 0;
        ComPtr<ID3D11Texture2D> final0;
        if (g.eyeFinal[0] && SUCCEEDED(g.eyeFinal[0].As(&final0))) {
            D3D11_TEXTURE2D_DESC fd;
            final0->GetDesc(&fd);
            fw = fd.Width;
            fh = fd.Height;
        }
        const bool tooSmall = (fw && (d.Width * 4 < fw || d.Height * 4 < fh)) ||
                           (tr.sceneDesc.Width && (d.Width * 4 < tr.sceneDesc.Width || d.Height * 4 < tr.sceneDesc.Height));
        if (tooSmall) {
            ++tr.stats.sceneCandidatesRefused;
            if (!tr.smallLogged) {
                tr.smallLogged = true;
                simqvfr::logf("lmu mvr: pass tracker: a %ux%u target YEBIS's copy reads (drawn with a depth target) isn't taken "
                              "for the scene's: too small beside the %ux%u scene learned before and the eyes' %ux%u finals",
                              d.Width, d.Height, tr.sceneDesc.Width, tr.sceneDesc.Height, fw, fh);
            }
            return;
        }
    }
    tr.scene = resource.Get();
    tr.sceneDesc = d;
    tr.learnedThisFrame = true;
    tr.msaaSinceScene = false;
    if (!tr.sceneLogged) {
        tr.sceneLogged = true;
        simqvfr::logf("lmu mvr: pass tracker: no multisample scene (MSAA off): the scene's target is %ux%u format %d (what "
                      "YEBIS's copy reads, drawn with a depth target): the eye passes begin at its binds with depth, their "
                      "post-processing at the copy",
                      d.Width, d.Height, static_cast<int>(d.Format));
    } else {
        ++tr.stats.sceneRelearned;
    }
}

void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    if (g.slotsBound && g.vsRole == VsRole::lensMask && ours(context))
        bind_slot_view(-1);  // car slots: the lens mask is no car part (it may return before the parts' step below)
    if (context == g.context && !g.replaying) {
        ++tr.draws;
        if (g.vsRole == VsRole::lensMask && lens_mask_draw(context, "DrawIndexed", count, start))
            return;
        if (tr.on)
            copy_before(context);
    }
    const bool outside = context == g.context && !g.replaying && g.pass < 0 && (g.finalThisFrame[0] || g.finalThisFrame[1]);
    if (g.carTrack && g.carActive && ours(context)) {
        int slot = -1;
        {
            SampledTimed timed(g.time.carDraws, carSampleDraw);
            slot = car_draw(count, start, base);
        }
        if (g.slotsBound)
            bind_slot_view(slot);
    }
    if (!outside || !double_outside<kHookDrawIndexed>(context, count, start, base))
        game_draw<kHookDrawIndexed>(context, count, start, base);
    if (g.tracing && context == g.context && !g.replaying)
        trace_draw(context, "DrawIndexed", count, start);
    if (pt.recording && context == g.context && !g.replaying)
        pt_draw(context, "DrawIndexed");
    if (ours(context) && g.overlayTarget && g.on)
        ++g.stats.overlayDraws;
    if (!g.postWindow || !ours(context))
        return;
    Timed timed(g.time.hooks);
    focus_post_draw(context);
}

// Draw and the instanced draws: the real call (an overlay after the passes doubled: double_outside), the trace's line;
// Draw also the final blit's replay (focus_blit).
template <int H, typename... A>
void STDMETHODCALLTYPE draw_traced(ID3D11DeviceContext* context, A... a) {
    if (g.slotsBound && ours(context))
        bind_slot_view(-1);  // car slots: these draws are no car part's
    if (context == g.context && !g.replaying) {
        ++tr.draws;
        const long long args[] = {static_cast<long long>(a)...};
        if (g.vsRole == VsRole::lensMask && lens_mask_draw(context, kHookName[H], args[0], args[1]))
            return;
    }
    const bool outside = context == g.context && !g.replaying && g.pass < 0 && (g.finalThisFrame[0] || g.finalThisFrame[1]);
    bool panel = false;
    if constexpr (H == kHookDraw) {  // R6b: the menus' panel (Draw: vertex count, start)
        panel = panel_draw_here(context);
        if (panel) {
            const long long args[] = {static_cast<long long>(a)...};
            call_panel_fn(context, false, static_cast<unsigned>(args[0]), static_cast<unsigned>(args[1]));
        }
    }
    if (!outside || !double_outside<H>(context, a...))
        game_draw<H>(context, a...);
    if constexpr (H == kHookDraw) {
        if (panel) {
            const long long args[] = {static_cast<long long>(a)...};
            call_panel_fn(context, true, static_cast<unsigned>(args[0]), static_cast<unsigned>(args[1]));
        }
    }
    if (g.tracing && context == g.context && !g.replaying) {
        const long long args[] = {static_cast<long long>(a)...};
        trace_draw(context, kHookName[H], args[0], args[1]);
    }
    if (pt.recording && context == g.context && !g.replaying)
        pt_draw(context, kHookName[H]);
    if (ours(context) && g.overlayTarget && g.on)
        ++g.stats.overlayDraws;
    if constexpr (H == kHookDraw) {
        if (ours(context) && tr.open && tr.singleSample && !tr.blitSeen)  // MSAA off: the pass's final blit
            note_final_blit(context);
        if (ours(context) && g.compositeOk && !g.focusBlitted && g.pass < 2 && g.drawFocus) {
            Timed timed(g.time.hooks);
            const long long args[] = {static_cast<long long>(a)...};
            focus_blit(context, static_cast<UINT>(args[0]), static_cast<UINT>(args[1]));
        }
    }
}

// DrawAuto and the indirect draws with instancing: no instance count to double (it's in a buffer, or there is none), so
// while two views are on the game's own shader draws it, into the eye's viewport alone; logged once.
template <int H, typename... A>
void STDMETHODCALLTYPE undoubled_draw(ID3D11DeviceContext* context, A... a) {
    if (context == g.context && !g.replaying)
        ++tr.draws;
    if (!(g.instanced && g.on && ours(context))) {
        call_real<H>(context, a...);
        return;
    }
    ++g.stats.eyeOnlyDraws;
    if (!g.undoubledLogged) {
        g.undoubledLogged = true;
        simqvfr::logf("lmu mvr: pass %d: a %s with two views on: drawn into the eye alone (instancing can't double it; "
                      "counted from now on)",
                      g.pass, kHookName[H]);
    }
    realVSSetShader(context, g.gameVs, g.instanceCount ? g.instances : nullptr, g.instanceCount);
    call_real<H>(context, a...);
    realVSSetShader(context, g.boundVs, g.instanceCount ? g.instances : nullptr, g.instanceCount);
}

// The detours of hook H's functions, one per function (N): straight through inside the handling of one (depth), else
// the handler, which passes the call on through call_real.
template <int H, auto Handler, typename... A>
struct Detours {
    template <int N>
    static void STDMETHODCALLTYPE fn(ID3D11DeviceContext* context, A... a) {
        if (depth[H] > 0) {
            reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(functions[H].real[N])(context, a...);
            return;
        }
        ++depth[H];
        Handler(context, a...);
        --depth[H];
    }
    template <std::size_t... N>
    static void* get(int n, std::index_sequence<N...>) {
        void* const table[] = {reinterpret_cast<void*>(&fn<static_cast<int>(N)>)...};
        return table[n];
    }
    static void* get(int n) { return get(n, std::make_index_sequence<kFunctions>{}); }
};

// Car tracking's detours: its ~30,000 calls a frame on track (Map and Unmap ~90% of them, ~4 per draw, nearly all of
// other buffers: lmu-ai_frame11731.rdc) cost as little as they can. No depth count: their work is the same whether or
// not one function of an entry calls another (the matrix read once: the inner Unmap finds it taken; bindings stored
// twice), and none of them calls a hooked function. The handler gets the original of the function the game called
// (no lookup through the vtable) and calls it.
template <int H, auto Handler, typename R, typename... A>
struct LeanDetours {
    using Fn = R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...);
    template <int N>
    static R STDMETHODCALLTYPE fn(ID3D11DeviceContext* context, A... a) {
        return Handler(reinterpret_cast<Fn>(functions[H].real[N]), context, a...);
    }
    template <std::size_t... N>
    static void* get(int n, std::index_sequence<N...>) {
        void* const table[] = {reinterpret_cast<void*>(&fn<static_cast<int>(N)>)...};
        return table[n];
    }
    static void* get(int n) { return get(n, std::make_index_sequence<kFunctions>{}); }
};

void* detour_for(int h, int n) {
    using Instances = ID3D11ClassInstance* const*;
    switch (h) {
        case kHookVS: return Detours<kHookVS, &vs_set_shader, ID3D11VertexShader*, Instances, UINT>::get(n);
        case kHookGS:
            return Detours<kHookGS, &later_stage<kHookGS, ID3D11GeometryShader, &State::gsBound>, ID3D11GeometryShader*,
                           Instances, UINT>::get(n);
        case kHookOM:
            return Detours<kHookOM, &om_set_render_targets, UINT, ID3D11RenderTargetView* const*,
                           ID3D11DepthStencilView*>::get(n);
        case kHookOMUav:
            return Detours<kHookOMUav, &om_set_render_targets_and_uavs, UINT, ID3D11RenderTargetView* const*,
                           ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*>::get(n);
        case kHookSO: return Detours<kHookSO, &so_set_targets, UINT, ID3D11Buffer* const*, const UINT*>::get(n);
        case kHookVP: return Detours<kHookVP, &rs_set_viewports, UINT, const D3D11_VIEWPORT*>::get(n);
        case kHookSC: return Detours<kHookSC, &rs_set_scissor_rects, UINT, const D3D11_RECT*>::get(n);
        case kHookResolve:
            return Detours<kHookResolve, &resolve_subresource, ID3D11Resource*, UINT, ID3D11Resource*, UINT,
                           DXGI_FORMAT>::get(n);
        case kHookHS:
            return Detours<kHookHS, &later_stage<kHookHS, ID3D11HullShader, &State::hsBound>, ID3D11HullShader*,
                           Instances, UINT>::get(n);
        case kHookDS:
            return Detours<kHookDS, &later_stage<kHookDS, ID3D11DomainShader, &State::dsBound>, ID3D11DomainShader*,
                           Instances, UINT>::get(n);
        case kHookCopy:
            return Detours<kHookCopy, &copy_subresource_region, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                           const D3D11_BOX*>::get(n);
        case kHookDrawIndexed: return Detours<kHookDrawIndexed, &draw_indexed, UINT, UINT, INT>::get(n);
        case kHookDraw: return Detours<kHookDraw, &draw_traced<kHookDraw, UINT, UINT>, UINT, UINT>::get(n);
        case kHookDrawIndexedInstanced:
            return Detours<kHookDrawIndexedInstanced, &draw_traced<kHookDrawIndexedInstanced, UINT, UINT, UINT, INT, UINT>,
                           UINT, UINT, UINT, INT, UINT>::get(n);
        case kHookDrawInstanced:
            return Detours<kHookDrawInstanced, &draw_traced<kHookDrawInstanced, UINT, UINT, UINT, UINT>, UINT, UINT, UINT,
                           UINT>::get(n);
        case kHookDrawAuto: return Detours<kHookDrawAuto, &undoubled_draw<kHookDrawAuto>>::get(n);
        case kHookDrawIndexedIndirect:
            return Detours<kHookDrawIndexedIndirect, &undoubled_draw<kHookDrawIndexedIndirect, ID3D11Buffer*, UINT>,
                           ID3D11Buffer*, UINT>::get(n);
        case kHookDrawInstancedIndirect:
            return Detours<kHookDrawInstancedIndirect, &undoubled_draw<kHookDrawInstancedIndirect, ID3D11Buffer*, UINT>,
                           ID3D11Buffer*, UINT>::get(n);
        case kHookPS: return Detours<kHookPS, &ps_set_shader, ID3D11PixelShader*, Instances, UINT>::get(n);
        case kHookVSCB:
            return LeanDetours<kHookVSCB, &vs_set_constant_buffers, void, UINT, UINT, ID3D11Buffer* const*>::get(n);
        case kHookMap:
            return LeanDetours<kHookMap, &map_resource, HRESULT, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                               D3D11_MAPPED_SUBRESOURCE*>::get(n);
        case kHookUnmap: return LeanDetours<kHookUnmap, &unmap_resource, void, ID3D11Resource*, UINT>::get(n);
        case kHookIAVB:
            return LeanDetours<kHookIAVB, &ia_set_vertex_buffers, void, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                               const UINT*>::get(n);
        case kHookIAIB:
            return LeanDetours<kHookIAIB, &ia_set_index_buffer, void, ID3D11Buffer*, DXGI_FORMAT, UINT>::get(n);
        case kHookBlend:
            return LeanDetours<kHookBlend, &om_set_blend_state, void, ID3D11BlendState*, const FLOAT*, UINT>::get(n);
        case kHookDepth:
            return LeanDetours<kHookDepth, &om_set_depth_stencil_state, void, ID3D11DepthStencilState*, UINT>::get(n);
        default: return nullptr;
    }
}

// The draws and CopySubresourceRegion only for the focus post (mvr_focus_replay), the draws also for instancing (doubled
// there), DrawAuto and the indirect draws only for instancing; both kinds of draw already where instancing may take over
// from single-pass stereo (it happens mid-pass, where d3d11's other modes can't be looked for): without either MVR's
// hooks are as before.
bool wanted(int h) {
    if (h == kHookPS)
        return g.biasHooks || g.slotsOn;
    if (is_car_hook(h))
        return g.carTrack;
    if (is_slot_hook(h))
        return g.slotsOn;
    if (is_undoubled_draw(h))
        return g.instanced || g.canInstance;
    if (h == kHookDrawIndexed && g.carTrack)  // car tracking's draws (LMU's scene draws are all DrawIndexed)
        return true;
    if (is_draw(h))
        return g.focusReplay || g.instanced || g.canInstance;
    return h != kHookCopy || g.focusReplay;
}

// The function the context's entry for hook `h` holds now, hooked in d3d11's code if not seen before (render thread).
// True: a new one.
bool hook_entry(int h) {
    if (!wanted(h))
        return false;
    void* fn = (*reinterpret_cast<void** const*>(g.context))[kSlotOf[h]];
    Functions& f = functions[h];
    for (int i = 0; i < f.count; ++i)
        if (f.target[i] == fn)
            return false;
    if (f.count == kFunctions) {
        if (!f.full)
            simqvfr::logf("lmu mvr: the context's %s entry holds a fifth function: calls through it aren't seen",
                          kHookName[h]);
        f.full = true;
        return false;
    }
    const int n = f.count;
    f.target[n] = fn;
    if (MH_CreateHook(fn, detour_for(h, n), &f.real[n]) == MH_OK && MH_EnableHook(fn) == MH_OK) {
        ++f.hooked;
        ++g.stats.contextFunctions;
        if (h == kHookResolve)
            ++g.stats.resolveFunctions;
    } else {
        f.real[n] = fn;  // passed on as it is, not tried again
        simqvfr::logf("lmu mvr: the context's %s function %p couldn't be hooked", kHookName[h], fn);
    }
    ++f.count;  // last: call_real reads target and real up to it
    return true;
}

int hook_entries() {
    int fresh = 0;
    for (int h = 0; h < kHooks; ++h)
        fresh += hook_entry(h) ? 1 : 0;
    return fresh;
}

// Every function the wanted entries hold: as they are; once idle (a flush) and busy (a command of ours); and the same
// with multithread protection on when it is off (on, then off again: it only adds locking meanwhile). With protection on
// already, turning it off here could race another thread: its plain functions are hooked when they first appear (a
// pass's start). At install, and when more entries are wanted later (instancing turned on between passes).
void hook_every_mode() {
    const float identity[4] = {1, 0, 1, 0};
    const auto idle_and_busy = [&] {
        hook_entries();
        g.context->Flush();
        hook_entries();
        g.context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, identity, 0, 0);
        hook_entries();
    };
    idle_and_busy();
    g.protectedNow = g.multithread && g.multithread->GetMultithreadProtected();
    if (g.multithread && !g.protectedNow) {
        g.multithread->SetMultithreadProtected(TRUE);
        idle_and_busy();
        g.multithread->SetMultithreadProtected(FALSE);
        hook_entries();
    }
}

void unhook_entries() {
    for (Functions& f : functions) {
        for (int i = 0; i < f.count; ++i)
            if (f.real[i] != f.target[i])
                MH_RemoveHook(f.target[i]);
        f = Functions{};
    }
}

// The game's bindings as the pass starts (the hooks only see later changes).
void read_bindings() {
    if (g.biasHooks || g.slotsOn) {  // the pixel shader bound as the pass starts (set outside the passes, not seen)
        ID3D11PixelShader* ps = nullptr;
        UINT count = 256;
        g.context->PSGetShader(&ps, g.psInstances, &count);
        g.psInstanceCount = ps ? count : 0;
        for (UINT i = 0; i < g.psInstanceCount; ++i)
            if (g.psInstances[i])
                g.psInstances[i]->Release();  // raw, like the game's own bindings
        if (ps)
            ps->Release();
        g.gamePs = g.boundPs = ps;
    }
    if (g.slotsOn) {  // car slots: the blend state (its twin goes in its place) and whether the depth state writes
        ID3D11BlendState* bs = nullptr;
        g.context->OMGetBlendState(&bs, g.blendFactor, &g.sampleMask);
        g.gameBlend = bs;  // raw, like the game's own bindings
        if (bs)
            bs->Release();
        ID3D11DepthStencilState* ds = nullptr;
        UINT ref = 0;
        g.context->OMGetDepthStencilState(&ds, &ref);
        g.depthWrites = depth_writes(ds);
        if (ds)
            ds->Release();
    }
    ID3D11VertexShader* vs = nullptr;
    UINT count = 256;
    g.context->VSGetShader(&vs, g.instances, &count);
    g.instanceCount = vs ? count : 0;
    for (UINT i = 0; i < g.instanceCount; ++i)
        if (g.instances[i])
            g.instances[i]->Release();  // raw, like the game's own bindings
    if (vs)
        vs->Release();
    g.gameVs = g.boundVs = vs;
    {
        const TwinEntry* e = vs ? &shader_entry(vs) : nullptr;
        g.vsRole = e ? e->role : VsRole::none;
        g.vsWorldSlot = e ? e->worldSlot : -1;
        g.vsWorldOffset = e ? e->worldOffset : 0;
    }
    if (g.carTrack) {  // car tracking: the vertex shader's constant buffers and the input assembler's buffers as bound
        ID3D11Buffer* cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        g.context->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
        for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i) {
            g.vsCb[i] = cbs[i];  // raw, like the game's own bindings
            if (cbs[i])
                cbs[i]->Release();
        }
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, vbOffset = 0;
        g.context->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOffset);
        g.iaVb = vb;
        g.iaVbOffset = vbOffset;
        if (vb)
            vb->Release();
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT ibOffset = 0;
        g.context->IAGetIndexBuffer(&ib, &format, &ibOffset);
        g.iaIb = ib;
        g.iaIbOffset = ibOffset;
        if (ib)
            ib->Release();
    }
    if (g.biasHooks) {  // texture sharpening: the pixel shader the pass starts with
        ID3D11PixelShader* ps = nullptr;
        UINT psCount = 256;
        g.context->PSGetShader(&ps, g.psInstances, &psCount);
        g.psInstanceCount = ps ? psCount : 0;
        for (UINT i = 0; i < g.psInstanceCount; ++i)
            if (g.psInstances[i])
                g.psInstances[i]->Release();
        if (ps)
            ps->Release();
        g.gamePs = g.boundPs = ps;
    }
    ID3D11DepthStencilView* dsv = nullptr;
    g.context->OMGetRenderTargets(0, nullptr, &dsv);
    g.depthBound = dsv != nullptr;
    if (dsv)
        dsv->Release();
    g.vpCount = kMaxRects;
    g.context->RSGetViewports(&g.vpCount, g.vps);
    g.scCount = kMaxRects;
    g.context->RSGetScissorRects(&g.scCount, g.scs);
    ID3D11GeometryShader* gs = nullptr;
    g.context->GSGetShader(&gs, nullptr, nullptr);
    g.gsBound = gs != nullptr;
    ID3D11HullShader* hs = nullptr;
    g.context->HSGetShader(&hs, nullptr, nullptr);
    g.hsBound = hs != nullptr;
    ID3D11DomainShader* ds = nullptr;
    g.context->DSGetShader(&ds, nullptr, nullptr);
    g.dsBound = ds != nullptr;
    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT] = {};
    g.context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
    g.soBound = false;
    for (ID3D11Buffer* b : so)
        if (b) {
            g.soBound = true;
            b->Release();
        }
    for (IUnknown* u : {static_cast<IUnknown*>(gs), static_cast<IUnknown*>(hs), static_cast<IUnknown*>(ds)})
        if (u)
            u->Release();
}

// `dst` (made or re-made at w x h in `src`'s format) = src's region at (x, y).
bool copy_region(ComPtr<ID3D11Texture2D>& dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w, unsigned h) {
    ComPtr<ID3D11Texture2D> source;
    if (!src || FAILED(src->QueryInterface(IID_PPV_ARGS(&source))) || !w || !h)
        return false;
    D3D11_TEXTURE2D_DESC d;
    source->GetDesc(&d);
    if (x + w > d.Width || y + h > d.Height || d.SampleDesc.Count != 1)
        return false;
    if (dst) {
        D3D11_TEXTURE2D_DESC have;
        dst->GetDesc(&have);
        if (have.Width != w || have.Height != h || have.Format != d.Format)
            dst.Reset();
    }
    if (!dst) {
        D3D11_TEXTURE2D_DESC c = d;
        c.Width = w;
        c.Height = h;
        c.MipLevels = 1;
        c.ArraySize = 1;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = 0;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        ComPtr<ID3D11Device> device;
        g.context->GetDevice(&device);
        if (FAILED(device->CreateTexture2D(&c, nullptr, &dst)))
            return false;
    }
    const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    g.context->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
    return true;
}

bool read_halves(ID3D11Texture2D* texture, std::vector<uint16_t>& out, unsigned& w, unsigned& h) {
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS)
        return false;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&s, nullptr, &staging)))
        return false;
    g.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    w = d.Width;
    h = d.Height;
    out.resize(static_cast<size_t>(w) * h * 4);
    for (unsigned y = 0; y < h; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, static_cast<const uint8_t*>(map.pData) + y * map.RowPitch,
                    static_cast<size_t>(w) * 8);
    g.context->Unmap(staging.Get(), 0);
    return true;
}

float half_to_float(uint16_t v) {
    const uint32_t sign = (v >> 15) & 1, exponent = (v >> 10) & 0x1f, mantissa = v & 0x3ff;
    float f;
    if (exponent == 0)
        f = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        f = mantissa ? 0.0f : 65504.0f;  // NaN as 0, infinity as the largest half
    else
        f = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -f : f;
}

std::vector<float> luminance(const std::vector<uint16_t>& px, unsigned w, unsigned h) {
    std::vector<float> l(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < l.size(); ++i)
        l[i] = std::max(0.0f, 0.2126f * half_to_float(px[i * 4]) + 0.7152f * half_to_float(px[i * 4 + 1]) +
                                  0.0722f * half_to_float(px[i * 4 + 2]));
    return l;
}

// For looking at: x / (1 + x), gamma 2.2, BGRA.
std::vector<uint8_t> tonemapped(const std::vector<uint16_t>& px, unsigned w, unsigned h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = std::max(0.0f, half_to_float(px[i * 4 + c]));
            out[i * 4 + 2 - c] = static_cast<uint8_t>(std::lround(255.0f * std::pow(v / (1.0f + v), 1.0f / 2.2f)));
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

// Car slots' gate image: the pass's views (the eye, its focus view beside it) as drawn, each car part's pixels in a colour
// of its slot over the scene darkened; the share of the views' pixels that are car parts and how many parts, logged.
void save_slot_image(int eye, ID3D11Resource* scene) {
    const UINT fw = g.drawFocus ? (g.view.focusRenderW ? g.view.focusRenderW : g.view.focusW) : 0;
    const UINT fh = g.drawFocus ? (g.view.focusRenderH ? g.view.focusRenderH : g.view.focusH) : 0;
    const UINT w = std::min(g.view.eyeW + fw, g.slotW), h = std::min(std::max(g.view.eyeH, fh), g.slotH);
    if (!w || !h || !scene || !g.slotTex || g.slotSamples > 1 || tr.sceneDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return;  // (the gate image is of a single-sample slot target: MSAA off)
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    const auto staged = [&](ID3D11Resource* src, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& out) {
        const D3D11_TEXTURE2D_DESC t = {w, h, 1, 1, format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        if (FAILED(device->CreateTexture2D(&t, nullptr, &out)))
            return false;
        const D3D11_BOX box = {0, 0, 0, w, h, 1};
        g.context->CopySubresourceRegion(out.Get(), 0, 0, 0, 0, src, 0, &box);
        return true;
    };
    ComPtr<ID3D11Texture2D> slots, colour;
    if (!staged(g.slotTex.Get(), DXGI_FORMAT_R16_UINT, slots) || !staged(scene, DXGI_FORMAT_R16G16B16A16_FLOAT, colour))
        return;
    D3D11_MAPPED_SUBRESOURCE ms{}, mc{};
    if (FAILED(g.context->Map(slots.Get(), 0, D3D11_MAP_READ, 0, &ms)))
        return;
    if (FAILED(g.context->Map(colour.Get(), 0, D3D11_MAP_READ, 0, &mc))) {
        g.context->Unmap(slots.Get(), 0);
        return;
    }
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    std::vector<uint8_t> seen(kCarSlots + 1, 0);
    size_t carPixels = 0, viewPixels = 0, parts = 0;
    for (UINT y = 0; y < h; ++y) {
        const auto* sr = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(ms.pData) + y * ms.RowPitch);
        const auto* cr = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mc.pData) + y * mc.RowPitch);
        for (UINT x = 0; x < w; ++x) {
            const bool inView = (x < g.view.eyeW && y < g.view.eyeH) || (x >= g.view.eyeW && y < fh);
            uint8_t* o = bgra.data() + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float v = std::max(0.0f, half_to_float(cr[x * 4 + c]));
                o[2 - c] = static_cast<uint8_t>(std::lround(110.0f * std::pow(v / (1.0f + v), 1.0f / 2.2f)));
            }
            o[3] = 255;
            if (!inView)
                continue;
            ++viewPixels;
            const uint16_t word = sr[x];
            if (word == 0 || word > kCarSlots)
                continue;
            ++carPixels;
            if (!seen[word]) {
                seen[word] = 1;
                ++parts;
            }
            const uint32_t hash = word * 2654435761u;  // a colour per slot
            o[0] = static_cast<uint8_t>(96 + (hash >> 8) % 160);
            o[1] = static_cast<uint8_t>(96 + (hash >> 16) % 160);
            o[2] = static_cast<uint8_t>(96 + (hash >> 24) % 160);
        }
    }
    g.context->Unmap(colour.Get(), 0);
    g.context->Unmap(slots.Get(), 0);
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wchar_t name[96];
    swprintf_s(name, L"lmu-car-slots-frame%llu-eye%d.png", g.framesSinceInstall, eye);
    simqvfr::write_png_bgra((dir / name).wstring(), bgra, w, h);
    ++g.stats.slotImages;
    simqvfr::logf("lmu mvr: car slots: gate image %ls (%ux%u, eye %d%s): %.2f%% of the views' pixels car parts, %zu "
                  "parts; %llu slot views bound in the pass",
                  (dir / name).c_str(), w, h, eye, fw ? " and its focus view" : "",
                  viewPixels ? 100.0 * static_cast<double>(carPixels) / static_cast<double>(viewPixels) : 0.0, parts,
                  static_cast<unsigned long long>(g.slotViewsThisPass));
}

void compare_sample(unsigned long long frame) {
    ++g.stats.samples;
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int eye = 0; eye < 2; ++eye) {
        std::vector<uint16_t> a, b;
        unsigned aw = 0, ah = 0, bw = 0, bh = 0;
        if (!g.have[eye] || !g.have[2 + eye] || !read_halves(g.mvrImage[eye].Get(), a, aw, ah) ||
            !read_halves(g.passImage[eye].Get(), b, bw, bh) || aw != bw || ah != bh) {
            simqvfr::logf("lmu mvr: frame %llu: eye %d's focus images not both copied (%d %d); not compared", frame, eye,
                          g.have[eye], g.have[2 + eye]);
            continue;
        }
        const MvrComparison c = mvr_compare(a, b, aw, ah);
        simqvfr::logf("lmu mvr: frame %llu: focus %d drawn in pass %d (MVR) vs pass %d, %ux%u: aligned at %+d,%+d px; "
                      "brightness %.4f (cell median %.4f); mean difference %.2f%%; %.2f%% of pixels differ >10%%",
                      frame, eye, eye, 2 + eye, aw, ah, c.shiftX, c.shiftY, c.brightness, c.cellMedian, 100 * c.meanDiff,
                      c.differingPct);
        if (g.stats.samples == 1) {
            wchar_t name[96];
            swprintf_s(name, L"lmu-mvr-frame%llu-focus%d-mvr.png", frame, eye);
            simqvfr::write_png_bgra((dir / name).wstring(), tonemapped(a, aw, ah), aw, ah);
            swprintf_s(name, L"lmu-mvr-frame%llu-focus%d-pass.png", frame, eye);
            simqvfr::write_png_bgra((dir / name).wstring(), tonemapped(b, bw, bh), bw, bh);
            simqvfr::logf("lmu mvr: images in %ls (lmu-mvr-frame%llu-focus*-mvr/pass.png)", dir.c_str(), frame);
        }
    }
}

}  // namespace

bool mvr_pass_install(ID3D11Device* device) {
    if (g.context)
        return true;
    if (!device)
        return false;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    const D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const float identity[4] = {1, 0, 1, 0};
    const D3D11_SUBRESOURCE_DATA init = {identity, 0, 0};
    if (FAILED(device->CreateBuffer(&cd, &init, &g.remapCb)))
        return false;
    if (g.biasHooks) {  // texture sharpening's constants (no rectangle, no bias until a pass sets them)
        const D3D11_BUFFER_DESC bd = {sizeof(g.biasConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA zero = {g.biasConstants, 0, 0};
        if (FAILED(device->CreateBuffer(&bd, &zero, &g.biasCb)))
            g.biasHooks = false;
    }
    if (g.slotsOn) {  // car slots: the slot words and a view of each, the motions, the default blend state's twins
        std::vector<uint32_t> words(kCarSlots + 1);
        for (uint32_t i = 0; i <= kCarSlots; ++i)
            words[i] = i;  // slot s's view is element s + 1: its word s + 1 (0: none)
        const D3D11_BUFFER_DESC wd = {static_cast<UINT>(words.size() * 4), D3D11_USAGE_IMMUTABLE,
                                      D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA wi = {words.data(), 0, 0};
        const D3D11_BUFFER_DESC md = {kCarSlots * 48, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                      D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 48};
        std::vector<float> motionInit(size_t{kCarSlots} * 12, 0.0f);
        for (size_t i = 0; i < kCarSlots; ++i)
            motionInit[i * 12] = motionInit[i * 12 + 4] = motionInit[i * 12 + 8] = 1.0f;
        const D3D11_SUBRESOURCE_DATA mi = {motionInit.data(), 0, 0};
        bool ok = SUCCEEDED(device->CreateBuffer(&wd, &wi, &g.slotWords)) &&
                  SUCCEEDED(device->CreateBuffer(&md, &mi, &g.motionBuffer)) &&
                  SUCCEEDED(device->CreateShaderResourceView(g.motionBuffer.Get(), nullptr, &g.motionSrv));
        g.slotViews.resize(kCarSlots);
        for (UINT i = 0; ok && i < kCarSlots; ++i) {
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R32_UINT;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.FirstElement = i + 1;
            v.Buffer.NumElements = 1;
            ok = SUCCEEDED(device->CreateShaderResourceView(g.slotWords.Get(), &v, &g.slotViews[i]));
        }
        for (int w = 0; ok && w < 2; ++w) {
            const D3D11_BLEND_DESC plain = {FALSE, FALSE,
                                            {{FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                                              D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL}}};
            const D3D11_BLEND_DESC twin = blend_twin_desc(plain, w == 1);
            ok = SUCCEEDED(device->CreateBlendState(&twin, &g.defaultBlendTwin[w]));
        }
        ok = ok && SUCCEEDED(context.As(&g.context1));
        if (!ok) {
            g.slotsOn = false;
            g.slotViews.clear();
            simqvfr::log_line("lmu mvr: car slots: their buffers, views or blend states couldn't be made; off");
        }
    }
    const D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, tq = {D3D11_QUERY_TIMESTAMP, 0};
    for (GpuFrame& f : g.gpu) {
        device->CreateQuery(&dq, &f.disjoint);
        for (auto& pass : f.stamp)
            for (auto& q : pass)
                device->CreateQuery(&tq, &q);
    }
    if (const MH_STATUS s = MH_Initialize(); s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    // The probe's way to the two views (tests set it with mvr_use_instanced): instancing hooks the draws too.
    if (mvr_probe_backend() == MvrBackend::instanced)
        g.instanced = true;
    if (mvr_probe_instanced())
        g.canInstance = true;
    g.context = context.Get();
    context.As(&g.multithread);
    hook_every_mode();
    for (int h = 0; h < kHooks; ++h)
        if (wanted(h) && !functions[h].hooked) {
            unhook_entries();
            g.context = nullptr;
            g.multithread.Reset();
            simqvfr::logf("lmu mvr: the context's %s couldn't be hooked; off", kHookName[h]);
            return false;
        }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g.qpcAtLog = now.QuadPart;
    g.tscAtLog = __rdtsc();
    std::string counts;
    for (int h = 0; h < kHooks; ++h)
        if (wanted(h)) {
            counts += (h ? ", " : "") + std::string(kHookName[h]) + " " + std::to_string(functions[h].hooked);
            ++g.stats.contextEntries;
        }
    simqvfr::logf("lmu mvr: on: passes 0-1 draw their focus views too (%s, beside the eye%s); the context's hooks on %llu "
                  "functions in d3d11's code (%s); multithread protection %s",
                  g.instanced ? mvr_backend_name(MvrBackend::instanced) : mvr_backend_name(MvrBackend::nvapi),
                  !g.instanced && g.canInstance ? "; instancing if the driver refuses it" : "",
                  static_cast<unsigned long long>(g.stats.contextFunctions), counts.c_str(),
                  !g.multithread ? "not known" : g.protectedNow ? "on" : "off");
    if (g.carTrack)
        simqvfr::logf("lmu mvr: car tracking on (log only): each eye-pass draw whose vertex shader reads a worldMatrix "
                      "keyed by its buffers and index range, its matrix read as the game writes the per-draw buffer");
    if (g.slotsOn)
        simqvfr::logf("lmu mvr: car slots on (level %d): the scene's binds get a slot target at render target 1, the "
                      "pixel shaders their slot twins, the blend states twins writing it where depth is written; %s",
                      g.slotsLevel, g.slotsLevel >= 2 ? "DLSS's vectors take the parts' motion" : "DLSS as before");
    return true;
}

void mvr_pass_begin(int pass, const MvrPassView* view, bool sample, bool replaceResolve) {
    if (!g.context)
        return;
    Timed timed(g.time.hooks);
    // A function an entry holds that wasn't seen at install (a mode of d3d11's not met there): hooked from this pass on.
    for (int h = 0; h < kHooks; ++h)
        if (hook_entry(h))
            simqvfr::logf("lmu mvr: the context's %s entry holds a function not seen before: hooked (%d for it now)",
                          kHookName[h], functions[h].count);
    if (pass == 0 && g.multithread) {  // who turns multithread protection on or off, and when (once per frame)
        const bool on = g.multithread->GetMultithreadProtected() != FALSE;
        if (on != g.protectedNow) {
            g.protectedNow = on;
            simqvfr::logf("lmu mvr: the immediate context's multithread protection went %s (game frame %llu)",
                          on ? "on" : "off", g.gpuFrameNo);
        }
    }
    g.pass = pass;
    if (pass < 2) {  // the tracker's comparison
        tr.actualBegin[pass] = tr.draws;
        tr.actual = (std::max)(tr.actual, pass + 1);
    }
    g.drawFocus = view != nullptr && !g.broken;
    g.passTwinBinds = 0;
    g.postWindow = g.focusCopied = g.focusComposited = false;
    g.postTrace.clear();
    g.copyTarget.Reset();
    g.compositeOk = g.focusBlitted = g.overlayTarget = false;
    if (pass == 0)
        g.finalThisFrame[0] = g.finalThisFrame[1] = false;  // a new frame: its own focus finals
    if (pass == 0 && g.carTrack) {  // car tracking: the matrices drawn from now on are this frame's
        Timed carTimed(g.time.cars);
        g.cars.frame_begin(++g.carFrame);
        g.vbChecked = nullptr;  // a buffer freed since can't pass for one checked before at its address
    }
    if (g.carTrack) {  // the pass's draws matched by pass 0's order first (car_motion.h)
        Timed carTimed(g.time.cars);
        g.cars.pass_begin(pass);
    }
    g.afterResolve = false;
    if (pass == 0) {  // this frame's timestamps, in a slot whose last frame has been read
        if (g.gpuSlot >= 0) {  // the frame before stopped short of pass 3: its timing left out
            g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
            g.gpuSlot = -1;
        }
        gpu_collect();
        const int slot = static_cast<int>(g.gpuFrameNo++ % kGpuFrames);
        GpuFrame& f = g.gpu[slot];
        g.gpuSlot = -1;
        if (!f.pending && f.disjoint) {
            for (auto& p : f.stamped)
                for (bool& s : p)
                    s = false;
            f.mode = g.mode;
            g.context->Begin(f.disjoint.Get());
            g.gpuSlot = slot;
        }
    }
    stamp(pass, 0);
    g.replaceResolve = replaceResolve && pass >= 2 && g.keptOk[pass & 1];
    if (pass < 2)
        g.keptOk[pass] = false;
    if (pass == 0) {
        g.sample = sample;
        for (bool& h : g.have)
            h = false;
    }
    if (pass >= 0 && pass < 4)
        g.resolved[pass].Reset();
    if (view) {
        g.view = *view;
        g.viewOf[view->eye & 1] = *view;
        g.context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, view->remap, 0, 0);
        ++g.stats.passesWithFocus;
    }
    // Texture sharpening: this pass's biases (its view's), the twins bound with two views on.
    g.bias = g.biasHooks && g.biasCb && view && (view->mipBias[0] != 0 || view->mipBias[1] != 0);
    g.biasCbBound = false;
    // Car slots: this pass's views not cleared yet, nothing bound (pass end unbound them).
    g.slotsBound = g.slotsCleared = false;
    g.slotViewBound = nullptr;
    g.slotViewsThisPass = 0;
    if (g.blendSwapped)  // a pass that didn't end: the game's blend state back before it is read
        apply_blend();
    g.stats.biasPasses += g.bias ? 1 : 0;
    read_bindings();
    update();
}

void mvr_pass_end(int pass) {
    if (!g.context || g.pass < 0)
        return;
    Timed timed(g.time.hooks);
    slot_unbind();  // car slots: the game's targets and blend state as it bound them
    g.drawFocus = false;
    g.overlayTarget = false;
    update();  // the game's shader, one view, its own viewports
    g.bias = false;
    update_ps();  // the game's pixel shader (update() put it back if two views were on)
    stamp(pass, 2);
    if (pass == 3 && g.gpuSlot >= 0) {
        g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
        g.gpu[g.gpuSlot].pending = true;
        g.gpuSlot = -1;
    }
    if (g.sample && !g.keepFocus && pass >= 0 && pass < 4) {  // gate C's comparison (gate D's is four_passes.cpp's)
        const int eye = pass & 1;
        const MvrPassView& v = g.viewOf[eye];
        if (pass < 2)
            g.have[pass] = copy_region(g.mvrImage[eye], g.resolved[pass].Get(), v.eyeW, 0, v.focusW, v.focusH);
        else
            g.have[pass] = copy_region(g.passImage[eye], g.resolved[pass].Get(), 0, 0, v.focusW, v.focusH);
    }
    if (g.focusCopied && !g.focusComposited) {  // the focus post's copy ran, its composite didn't (no composite seen)
        ++g.stats.focusNoComposite;
        if (g.tracesLogged < 4) {
            ++g.tracesLogged;
            simqvfr::logf("lmu mvr: focus post: pass %d ended without the composite; its draws (DrawIndexed) after the copy, "
                          "pixel shader > target:%s",
                          pass, g.postTrace.empty() ? " none" : g.postTrace.c_str());
        }
    }
    g.focusCopied = false;
    if (pass >= 0 && pass < 2)
        tr.actualEnd[pass] = tr.draws;
    g.pass = -1;
    g.replaceResolve = false;
    g.postWindow = false;
    g.afterResolve = false;
}

void mvr_keep_focus(bool on) { g.keepFocus = on; }

void mvr_ui_mips(bool on) { g.uiMips.on = on; }

void mvr_mip_bias_hooks(bool on) {
    if (!g.context)  // before the install only: its hooks are chosen there
        g.biasHooks = on;
}

bool mvr_mip_bias_ready() { return g.context && g.biasHooks; }

void mvr_car_track(bool on) {
    if (!g.context)  // before the install only: its hooks are chosen there
        g.carTrack = on;
}

void mvr_test_scene_target(ID3D11Resource* scene) {
    tr.scene = scene;
    tr.sceneDesc = {};
    ComPtr<ID3D11Texture2D> t;
    if (scene && SUCCEEDED(scene->QueryInterface(IID_PPV_ARGS(&t))))
        t->GetDesc(&tr.sceneDesc);
}

ID3D11Texture2D* mvr_car_slot_texture() { return g.slotTex.Get(); }

void mvr_car_active(bool on) { g.carActive = on; }

void mvr_car_slot_images(bool on) { g.slotImages = on; }

void mvr_car_slots(int level) {
    if (!g.context) {  // before the install only (and after mvr_car_track: the slots need the tracking)
        g.slotsLevel = level;
        g.slotsOn = level > 0 && g.carTrack;
    }
}

void mvr_focus_replay(bool on) {
    g.focusReplay = on;
    if (!on) {  // nothing doubled after the passes any more (R5's fallback switches it off mid-session)
        g.finalThisFrame[0] = g.finalThisFrame[1] = false;
        g.postWindow = false;
    }
}

ID3D11Texture2D* mvr_focus_image(int eye, unsigned long long& composites) {
    composites = g.focusPost.composites(eye);
    return g.focusPost.image(eye);
}

bool mvr_focus_final(int eye, unsigned& x, unsigned& y, unsigned& width, unsigned& height) {
    if (eye < 0 || eye > 1 || !g.finalThisFrame[eye])
        return false;
    const MvrPassView& v = g.viewOf[eye];
    x = final_eye_w(v);  // as focus_blit put it
    y = 0;
    width = v.focusW;
    height = v.focusH;
    return true;
}

bool mvr_focus_ready(int eye) { return g.context && g.keepFocus && g.keptOk[eye & 1]; }

void mvr_mask_stale(const std::atomic<bool>* stale) { g.maskStale = stale; }

int mvr_mask_dropped() {
    const int n = g.maskDroppedSince;
    g.maskDroppedSince = 0;
    return n;
}

void mvr_panel_draws(PanelFn fn) { g.panelFn = fn; }

ID3D11Resource* mvr_copy_source(ID3D11Resource* dst) {
    if (!dst)
        return nullptr;
    for (unsigned i = 1; i <= State::kCopiesKept; ++i) {
        const State::CopySeen& c = g.copies[(g.copyNext - i) % State::kCopiesKept];
        if (c.dst == dst && c.frame + 1 >= g.framesSinceInstall)  // this frame's or the one before's (a frame end since)
            return c.src;
    }
    return nullptr;
}

ID3D11Texture2D* mvr_eye_final(int eye) {
    if (eye < 0 || eye > 1 || !g.eyeFinal[eye])
        return nullptr;
    ComPtr<ID3D11Texture2D> t;
    return SUCCEEDED(g.eyeFinal[eye].As(&t)) ? t.Get() : nullptr;  // raw: eyeFinal holds it
}

void mvr_redraw_panel(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
                      unsigned vertexCount, unsigned startVertex) {
    if (!context || !rtv || !width || !height)
        return;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    D3D11_VIEWPORT vps[kMaxRects];
    UINT n = kMaxRects;
    context->RSGetViewports(&n, vps);
    D3D11_RECT scs[kMaxRects];
    UINT sn = kMaxRects;
    context->RSGetScissorRects(&sn, scs);
    // The game's target's size: its viewport and scissors scaled from that to the image's.
    float sx = 1, sy = 1;
    if (rtvs[0]) {
        ComPtr<ID3D11Resource> resource;
        rtvs[0]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC d{};
        if (resource && SUCCEEDED(resource.As(&texture))) {
            texture->GetDesc(&d);
            sx = d.Width ? static_cast<float>(width) / static_cast<float>(d.Width) : 1.0f;
            sy = d.Height ? static_cast<float>(height) / static_cast<float>(d.Height) : 1.0f;
        }
    }
    D3D11_VIEWPORT vp = n ? vps[0] : D3D11_VIEWPORT{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    if (n) {
        vp.TopLeftX *= sx;
        vp.TopLeftY *= sy;
        vp.Width *= sx;
        vp.Height *= sy;
    }
    D3D11_RECT sc = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (sn)
        sc = {static_cast<LONG>(std::lround(scs[0].left * sx)), static_cast<LONG>(std::lround(scs[0].top * sy)),
              static_cast<LONG>(std::lround(scs[0].right * sx)), static_cast<LONG>(std::lround(scs[0].bottom * sy))};
    const bool was = g.replaying;
    g.replaying = true;
    realOMSetRenderTargets(context, 1, &rtv, nullptr);
    realRSSetViewports(context, 1, &vp);
    realRSSetScissorRects(context, 1, &sc);
    call_real<kHookDraw>(context, static_cast<UINT>(vertexCount), static_cast<UINT>(startVertex));
    realOMSetRenderTargets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    realRSSetViewports(context, n, vps);
    realRSSetScissorRects(context, sn, scs);
    g.replaying = was;
    for (ID3D11RenderTargetView* r : rtvs)
        if (r)
            r->Release();
    if (dsv)
        dsv->Release();
    ++g.stats.panelReplays;
}

void mvr_frame_end(unsigned long long frame) {
    if (!g.context)
        return;
    if (g.gpuSlot >= 0) {  // a frame of fewer than four passes (two, with the focus finals): its timing complete now
        g.context->End(g.gpu[g.gpuSlot].disjoint.Get());
        g.gpu[g.gpuSlot].pending = true;
        g.gpuSlot = -1;
    }
    if (tr.on) {
        const int passes = tr.passes;
        track_frame_end(frame);  // a pass the tracker ends here reads its focus final (mvr_focus_final) first
        pt_frame_end(frame, passes);
    } else {  // the counts start afresh each frame either way
        tr.passes = tr.actual = 0;
        tr.draws = 0;
    }
    g.finalThisFrame[0] = g.finalThisFrame[1] = false;  // the next frame's are its own (a menu frame has none)
    for (State::SceneBind& b : g.sceneBinds)  // DLAA's: nothing of the game's held between frames
        b = State::SceneBind{};
    // The overlay trace: the frames 600, 2400 and 4200 frame ends after the install, each from the frame end before.
    ++g.framesSinceInstall;
    if (g.tracing) {
        g.tracing = false;
        simqvfr::logf("lmu mvr trace: frame %llu traced (from the frame end before): %d draws logged, %d not (over 800); "
                      "the layer's own draws on the game's context (composition, overlay) may be among the outside ones",
                      g.gpuFrameNo, g.traceDraws, g.traceDropped);
    }
    constexpr unsigned long long kTraceAt[] = {600, 2400, 4200};
    if (g.focusReplay && g.framesTraced < static_cast<int>(std::size(kTraceAt)) &&
        g.framesSinceInstall + 1 == kTraceAt[g.framesTraced]) {
        g.tracing = true;
        ++g.framesTraced;
        g.traceDraws = g.traceDropped = 0;
    }
    if (g.sample) {
        g.sample = false;
        if (!g.keepFocus)
            compare_sample(frame);
    }
    if (frame >= g.lastStatsFrame + 900) {
        const double frames = static_cast<double>(frame - g.lastStatsFrame);
        g.lastStatsFrame = frame;
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        const unsigned long long tsc = __rdtsc();
        const double ms = 1000.0 * static_cast<double>(now.QuadPart - g.qpcAtLog) / static_cast<double>(f.QuadPart);
        if (timeHooks && ms > 0 && tsc > g.tscAtLog && frames > 0) {
            const double perMs = static_cast<double>(tsc - g.tscAtLog) / ms;
            const auto per = [&](uint64_t ticks) { return static_cast<double>(ticks) / perMs / frames; };
            simqvfr::logf("lmu mvr: this code's CPU in the context hooks %.3f ms per frame, the calls passed on included "
                          "(twin lookups %.3f: %llu, %llu not cached; stereo switches %.3f); %.0f hooked calls per frame",
                          per(g.time.hooks), per(g.time.lookups),
                          static_cast<unsigned long long>(g.stats.lookups - g.loggedLookups),
                          static_cast<unsigned long long>(g.stats.lookupMisses - g.loggedMisses), per(g.time.switches),
                          static_cast<double>(g.time.calls) / frames);
            if (g.carTrack)
                simqvfr::logf("lmu mvr: car tracking's own CPU %.3f ms per frame (its matrix reads %.3f, its draws %.3f, "
                              "the frame's start %.3f; 1 in 16 of its draws and of its matrix reads timed, each less the "
                              "timing's own %llu ticks; not the calls passed on, nor the detours' own cost per call); %.0f "
                              "calls of its hooks per frame (Map, Unmap, VSSetConstantBuffers, the input assembler's "
                              "buffers)",
                              per(g.time.cars + g.time.carReads + g.time.carDraws), per(g.time.carReads),
                              per(g.time.carDraws), per(g.time.cars), static_cast<unsigned long long>(timedEmptyTicks),
                              static_cast<double>(g.time.carCalls) / frames);
            if (g.slotsOn)
                simqvfr::logf("lmu mvr: car slots' CPU per frame, the calls they make included: slot views %.3f ms (1 in "
                              "16 timed), the target's binds and the blend states' twins %.3f, the motions computed and "
                              "uploaded %.3f",
                              per(g.time.slotViews), per(g.time.slotState), per(g.time.motion));
        }
        g.time = HookTime{};
        g.loggedLookups = g.stats.lookups;
        g.loggedMisses = g.stats.lookupMisses;
        g.qpcAtLog = now.QuadPart;
        g.tscAtLog = tsc;
        const MvrPassStats& s = g.stats;
        simqvfr::logf("lmu mvr: %llu passes drew their focus view; shader binds %llu twin / %llu plain, %llu stereo "
                      "switches (%llu refused), viewports %llu and scissors %llu doubled; focus regions kept %llu, "
                      "resolves replaced %llu; twin lookups %llu, %llu not cached",
                      static_cast<unsigned long long>(s.passesWithFocus), static_cast<unsigned long long>(s.twinBinds),
                      static_cast<unsigned long long>(s.plainBinds), static_cast<unsigned long long>(s.toggles),
                      static_cast<unsigned long long>(s.stereoErrors), static_cast<unsigned long long>(s.viewportRewrites),
                      static_cast<unsigned long long>(s.scissorRewrites), static_cast<unsigned long long>(s.keptRegions),
                      static_cast<unsigned long long>(s.replacedResolves), static_cast<unsigned long long>(s.lookups),
                      static_cast<unsigned long long>(s.lookupMisses));
        if (g.biasHooks)
            simqvfr::logf("lmu mvr: texture sharpening: %llu passes with a mip bias; pixel shader twin binds %llu, binds of "
                          "shaders without one %llu; twin lookups %llu, %llu not cached; its buffer found unbound %llu "
                          "times (rebound); focus rectangle %.0f,%.0f-%.0f,%.0f, bias focus %.2f, eye %.2f now",
                          static_cast<unsigned long long>(s.biasPasses), static_cast<unsigned long long>(s.psTwinBinds),
                          static_cast<unsigned long long>(s.psNoTwin), static_cast<unsigned long long>(s.psLookups),
                          static_cast<unsigned long long>(s.psLookupMisses), static_cast<unsigned long long>(s.biasCbLost),
                          g.biasConstants[0], g.biasConstants[1], g.biasConstants[2], g.biasConstants[3],
                          g.biasConstants[4], g.biasConstants[5]);
        if (g.carTrack) {  // since the line before, per frame; the keys held now
            const CarMotionStats c = g.cars.stats();
            const CarMotionStats& b = g.carLogged;
            const double n = c.frames > b.frames ? static_cast<double>(c.frames - b.frames) : 1.0;
            const auto pf = [&](uint64_t now, uint64_t before) { return static_cast<double>(now - before) / n; };
            simqvfr::logf("lmu mvr: car tracking: %llu frames; per frame %.1f draws with a world matrix (%.1f the identity, "
                          "%.1f of objects: %.1f matched to the frame before, %.1f first seen or after a gap, %.2f without "
                          "a free slot); %llu keys held, %.1f freed per frame; per frame the per-draw buffer learned %.2f "
                          "times, matrices read %.1f, draws without one %.2f, at another offset %.2f, from a dynamic vertex "
                          "buffer %.1f (keyed without their base vertex); vertex buffers' usage asked %.1f; part draws "
                          "matched by pass 0's order %.1f%% (looked up %.1f out of order, %.1f without an order)",
                          static_cast<unsigned long long>(c.frames - b.frames), pf(c.draws, b.draws),
                          pf(c.identityDraws, b.identityDraws), pf(c.partDraws, b.partDraws), pf(c.matched, b.matched),
                          pf(c.newKeys, b.newKeys), pf(c.full, b.full), static_cast<unsigned long long>(c.live),
                          pf(c.evicted, b.evicted), pf(s.carBufferLearned, g.carStatsLogged.carBufferLearned),
                          pf(s.carMatrixReads, g.carStatsLogged.carMatrixReads),
                          pf(s.carNoMatrix, g.carStatsLogged.carNoMatrix),
                          pf(s.carOtherOffset, g.carStatsLogged.carOtherOffset),
                          pf(s.carDynamicVb, g.carStatsLogged.carDynamicVb),
                          pf(s.carVbAsked, g.carStatsLogged.carVbAsked),
                          c.partDraws > b.partDraws ? 100.0 * static_cast<double>(c.inOrder - b.inOrder) /
                                                          static_cast<double>(c.partDraws - b.partDraws)
                                                    : 0.0,
                          pf(c.outOfOrder, b.outOfOrder), pf(c.noOrder, b.noOrder));
            g.carLogged = c;
            g.carStatsLogged = s;
        }
        if (g.slotsOn)
            simqvfr::logf("lmu mvr: car slots: %llu passes with the slot target, %llu binds of it, %llu slot views bound, "
                          "%llu slot twin binds, %llu binds of shaders without one while it was bound; %llu blend twins; "
                          "%llu motion uploads (%llu part motions); %llu gate images",
                          static_cast<unsigned long long>(s.slotPasses), static_cast<unsigned long long>(s.slotBinds),
                          static_cast<unsigned long long>(s.slotViews), static_cast<unsigned long long>(s.slotTwinBinds),
                          static_cast<unsigned long long>(s.slotNoTwin), static_cast<unsigned long long>(s.blendTwins),
                          static_cast<unsigned long long>(s.motionUploads), static_cast<unsigned long long>(s.motionSlots),
                          static_cast<unsigned long long>(s.slotImages));
        if (tr.on) {
            const MvrTrackStats& ts = tr.stats;
            const uint64_t compared = ts.passesCompared ? ts.passesCompared : 1;
            simqvfr::logf("lmu mvr: pass tracker: %llu frames: the game's two passes found as its own in %llu (began %llu "
                          "times before the game's, ended %llu after; draws of the game's pass left out at the start %.1f "
                          "on average, %llu at most, at the end %.1f, %llu at most); %llu frames with another count; frames "
                          "without the game's passes: %llu with %llu found",
                          static_cast<unsigned long long>(ts.frames), static_cast<unsigned long long>(ts.matched),
                          static_cast<unsigned long long>(ts.beganBefore), static_cast<unsigned long long>(ts.endedAfter),
                          static_cast<double>(ts.startGapSum) / compared, static_cast<unsigned long long>(ts.startGapMax),
                          static_cast<double>(ts.endGapSum) / compared, static_cast<unsigned long long>(ts.endGapMax),
                          static_cast<unsigned long long>(ts.countDiffers),
                          static_cast<unsigned long long>(ts.framesWithoutGame),
                          static_cast<unsigned long long>(ts.passesWithoutGame));
            if (tr.scene)
                simqvfr::logf("lmu mvr: pass tracker, MSAA off: %llu passes begun on the scene's single-sample target, "
                              "%llu post-processings begun at YEBIS's copy (%llu at a resolve), %llu final blits seen "
                              "(%llu blits before a post not taken); %llu passes ended before their post; the target "
                              "learned again %llu times",
                              static_cast<unsigned long long>(ts.singleSamplePasses),
                              static_cast<unsigned long long>(s.singleSamplePosts),
                              static_cast<unsigned long long>(s.resolves),
                              static_cast<unsigned long long>(ts.singleSampleBlits),
                              static_cast<unsigned long long>(ts.blitsBeforePost),
                              static_cast<unsigned long long>(ts.passesWithoutPost),
                              static_cast<unsigned long long>(ts.sceneRelearned));
        }
        if (g.instanced)
            simqvfr::logf("lmu mvr: instancing: %llu draws made with twice their instances (%llu with a per-instance "
                          "layout's twin), %llu drawn into the eye alone (DrawAuto, indirect)%s",
                          static_cast<unsigned long long>(s.doubledDraws), static_cast<unsigned long long>(s.layoutSwaps),
                          static_cast<unsigned long long>(s.eyeOnlyDraws),
                          s.instancedTakeovers ? "; took over from NVIDIA's MVR, which the driver refused" : "");
        if (s.maskDraws || s.maskDropped || s.panelReplays || s.uiMipFrames)
            simqvfr::logf("lmu mvr: lens mask: %llu draws (%llu with two views), %llu dropped while stale; VR UI panel "
                          "draws replayed at the menus' native size: %llu; frames whose panel sampled its canvas with "
                          "mips: %llu",
                          static_cast<unsigned long long>(s.maskDraws), static_cast<unsigned long long>(s.maskDrawsTwo),
                          static_cast<unsigned long long>(s.maskDropped),
                          static_cast<unsigned long long>(s.panelReplays),
                          static_cast<unsigned long long>(s.uiMipFrames));
        if (g.focusReplay) {
            const MvrProbeStats probe = mvr_probe_stats();
            simqvfr::logf("lmu mvr: focus post: %llu copies, %llu composites (%llu by their bindings), %llu final blits; "
                          "overlays doubled: %llu draws in the passes, %llu after them; %llu steps failed; %llu passes "
                          "without the composite after the copy, %llu draws into the resolve's target by another shader "
                          "(YEBIS's copy and composite pixel shaders marked %llu and %llu times)",
                          static_cast<unsigned long long>(s.focusCopies),
                          static_cast<unsigned long long>(s.focusComposites),
                          static_cast<unsigned long long>(s.focusCompositesByBindings),
                          static_cast<unsigned long long>(s.focusBlits),
                          static_cast<unsigned long long>(s.overlayDraws),
                          static_cast<unsigned long long>(s.overlaysOutside),
                          static_cast<unsigned long long>(s.focusFailures),
                          static_cast<unsigned long long>(s.focusNoComposite),
                          static_cast<unsigned long long>(s.focusUnknownComposites),
                          static_cast<unsigned long long>(probe.yebisCopies),
                          static_cast<unsigned long long>(probe.yebisComposites));
        }
        for (int m = 0; m < 2; ++m) {
            if (!g.gpuCounts[m][0])
                continue;
            const double* sc = g.sceneMs[m];
            const double* po = g.postMs[m];
            const unsigned* n = g.gpuCounts[m];
            const auto avg = [](double sum, unsigned k) { return k ? sum / k : 0.0; };
            double total = 0;
            for (int p = 0; p < 4; ++p)
                total += avg(sc[p] + po[p], n[p]);
            simqvfr::logf("lmu mvr: GPU per pass%s, scene / post-processing (to the resolve / after it): pass 0 %.2f / %.2f, "
                          "pass 1 %.2f / %.2f, pass 2 %.2f / %.2f, pass 3 %.2f / %.2f ms, %.2f in all (%u frames timed)",
                          m ? " on the A/B's other side (without MVR, the four passes, instancing, or without the other "
                              "cars' motion)"
                            : "", avg(sc[0], n[0]), avg(po[0], n[0]), avg(sc[1], n[1]), avg(po[1], n[1]),
                          avg(sc[2], n[2]), avg(po[2], n[2]), avg(sc[3], n[3]), avg(po[3], n[3]), total, n[0]);
            for (int p = 0; p < 4; ++p) {
                g.sceneMs[m][p] = g.postMs[m][p] = 0;
                g.gpuCounts[m][p] = 0;
            }
        }
    }
}

void mvr_set_two_views(TwoViewsFn fn) { g.twoViews = fn ? fn : &nvapi_two_views; }

void mvr_use_instanced(bool on, bool fallback) {
    g.instanced = on;
    g.canInstance = on || fallback;
    if (!on)
        g.stereo = false;  // the next update() switches single-pass stereo as needed
    bool missing = false;  // an entry now wanted with none of its functions hooked (installed before it was wanted)
    for (int h = 0; h < kHooks; ++h)
        missing = missing || (wanted(h) && functions[h].count == 0);
    if (g.context && g.pass < 0 && missing)  // the draws it needs hooked in every mode of d3d11's (between passes)
        hook_every_mode();
}

bool mvr_instanced() { return g.instanced; }

void mvr_frame_mode(int mode) { g.mode = mode ? 1 : 0; }

void mvr_twins_changed() { ++g.twinEpoch; }

void mvr_time_hooks(bool on) {
    if (on && !timeHooks) {  // the timestamp reads' own share of a sampled section: the least of many empty ones
        uint64_t least = ~uint64_t{0};
        for (int i = 0; i < 4096; ++i) {
            const uint64_t t0 = __rdtsc();
            least = (std::min)(least, __rdtsc() - t0);
        }
        timedEmptyTicks = least;
    }
    timeHooks = on;
}

namespace {

// mvr_compare's measures on two luminance images.
MvrComparison compare_luminance(const std::vector<float>& la, const std::vector<float>& lb, unsigned w, unsigned h,
                                int range) {
    MvrComparison c;
    const int border = range + 8;
    double best = -1, bestMeanA = 0;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            double diff = 0, sumA = 0;
            for (int y = border; y < static_cast<int>(h) - border; y += 4)
                for (int x = border; x < static_cast<int>(w) - border; x += 4) {
                    const float va = la[static_cast<size_t>(y) * w + x];
                    const float vb = lb[static_cast<size_t>(y + dy) * w + (x + dx)];
                    diff += std::fabs(va - vb);
                    sumA += va;
                }
            if (best < 0 || diff < best) {
                best = diff;
                bestMeanA = sumA;
                c.shiftX = dx;
                c.shiftY = dy;
            }
        }
    c.meanDiff = bestMeanA > 0 ? best / bestMeanA : 0;
    double sumA = 0, sumB = 0;
    size_t differing = 0, counted = 0;
    constexpr int kCells = 16;
    std::vector<double> cellA(kCells * kCells, 0), cellB(kCells * kCells, 0);
    for (int y = border; y < static_cast<int>(h) - border; ++y)
        for (int x = border; x < static_cast<int>(w) - border; ++x) {
            const float va = la[static_cast<size_t>(y) * w + x];
            const float vb = lb[static_cast<size_t>(y + c.shiftY) * w + (x + c.shiftX)];
            sumA += va;
            sumB += vb;
            ++counted;
            if (std::fabs(va - vb) > 0.1f * std::max(va, vb) && std::max(va, vb) > 1e-4f)
                ++differing;
            const int cell = (y * kCells / static_cast<int>(h)) * kCells + x * kCells / static_cast<int>(w);
            cellA[cell] += va;
            cellB[cell] += vb;
        }
    c.brightness = sumA > 0 ? sumB / sumA : 0;
    std::vector<double> ratios;
    for (int i = 0; i < kCells * kCells; ++i)
        if (cellA[i] > 1e-6)
            ratios.push_back(cellB[i] / cellA[i]);
    std::sort(ratios.begin(), ratios.end());
    c.cellMedian = ratios.empty() ? 0 : ratios[ratios.size() / 2];
    c.differingPct = counted ? 100.0 * static_cast<double>(differing) / static_cast<double>(counted) : 0;
    c.ok = true;
    return c;
}

float srgb_to_linear(uint8_t v) {
    const float x = static_cast<float>(v) / 255.0f;
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

MvrComparison mvr_compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, unsigned w, unsigned h,
                          int range) {
    if (a.size() != static_cast<size_t>(w) * h * 4 || b.size() != a.size() || w < 64 || h < 64)
        return {};
    return compare_luminance(luminance(a, w, h), luminance(b, w, h), w, h, range);
}

MvrComparison mvr_compare_bgra8(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, unsigned w, unsigned h,
                                int range) {
    if (a.size() != static_cast<size_t>(w) * h * 4 || b.size() != a.size() || w < 64 || h < 64)
        return {};
    float table[256];
    for (int i = 0; i < 256; ++i)
        table[i] = srgb_to_linear(static_cast<uint8_t>(i));
    const auto lum = [&](const std::vector<uint8_t>& px) {
        std::vector<float> l(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < l.size(); ++i)
            l[i] = 0.0722f * table[px[i * 4]] + 0.7152f * table[px[i * 4 + 1]] + 0.2126f * table[px[i * 4 + 2]];
        return l;
    };
    MvrComparison c = compare_luminance(lum(a), lum(b), w, h, range);
    // The bytes themselves at that offset (colour channels; alpha left out).
    const int border = range + 8;
    double sum = 0;
    size_t counted = 0, over = 0;
    for (int y = border; y < static_cast<int>(h) - border; ++y)
        for (int x = border; x < static_cast<int>(w) - border; ++x) {
            const size_t ia = (static_cast<size_t>(y) * w + x) * 4;
            const size_t ib = (static_cast<size_t>(y + c.shiftY) * w + (x + c.shiftX)) * 4;
            int most = 0;
            for (int k = 0; k < 3; ++k) {
                const int d = std::abs(static_cast<int>(a[ia + k]) - static_cast<int>(b[ib + k]));
                most = std::max(most, d);
                sum += d;
            }
            c.maxByte = std::max(c.maxByte, most);
            over += most > 4 ? 1 : 0;
            ++counted;
        }
    c.meanByte = counted ? sum / (3.0 * static_cast<double>(counted)) : 0;
    c.bytesOverPct = counted ? 100.0 * static_cast<double>(over) / static_cast<double>(counted) : 0;
    return c;
}

MvrPassStats mvr_pass_stats() { return g.stats; }

CarMotionStats mvr_car_stats() { return g.cars.stats(); }

bool mvr_broken() { return g.context && g.broken; }

void mvr_track_drive(TrackFn begin, TrackFn end) {
    tr.driveBegin = begin;
    tr.driveEnd = end;
}

void mvr_pass_trace(int level) {
    pt.level = level;
    pt.shapes.clear();
    pt.lastShape = -1;
    pt.settings.clear();
    if (level)
        simqvfr::log_line("lmu mvr: pass trace on (lmu_pass_trace): every frame recorded, a frame logged when its shape is new, "
                          "with the game's settings as its files hold them");
}

void mvr_track_passes(bool on) {
    tr.on = on;
    tr.open = false;
    tr.passes = tr.actual = 0;
    tr.draws = 0;
}

MvrTrackStats mvr_track_stats() { return tr.stats; }

bool mvr_single_sample_passes() { return tr.on && tr.scene && !tr.msaaSinceScene; }

}  // namespace lmu

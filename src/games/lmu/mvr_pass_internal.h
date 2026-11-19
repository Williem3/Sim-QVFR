// mvr_pass.h's internals, shared by its files (detail: not for other code). One state for the whole path, `g`, with
// the pass tracker `tr`, the live resolution's `sizeCut` and the diagnostics' `pt` and `census`: render thread only
// (mvr_pass.cpp's opening comment), defined once in mvr_pass.cpp. The draw hooks and everything else per draw stay in
// mvr_pass.cpp's translation unit (no whole-program optimization: a call into another file isn't inlined), split by
// cluster into headers it alone includes, in order; what they check of the diagnostics is inline here (pt.recording,
// census_here), so with those off a draw makes no call into the .cpp files below.
//   mvr_pass.cpp          the spine: includes, the state's definitions, the cluster headers in order:
//     _hook_table.h       the hooks' table, call_real, the real calls, the section timer, held objects
//     _tracker.h          the pass tracker, GPU timestamps
//     _viewports.h        focus viewport mapping, the mip bias constants, the live resolution's size cut
//     _twins.h            VS/PS twins and their lookup, the PSSetShader hook
//     _cars.h             car tracking and car slots
//     _two_views.h        update() (the two views switched), the VS/GS/HS/DS/SO set hooks
//     _binds.h            the OM and RS hooks, FSR, the resolve, the post's beginning
//     _post.h             the focus post's replay, the final blit's replay
//     _doubling.h         draw doubling, the overlay trace, lens mask and panel draws, the copy hook
//     _scene_learn.h      the single-sample scene target learned (MSAA off)
//     _post_roles.h       post-role learning by bindings, the frame's shape
//     _hooks_entry.h      the draw hook entry points, ExecuteCommandList
//     _hook_install.h     the detours, their installation, the bindings read at a pass's start
//     _api.h              install, the passes' begin and end, the options
//     _frame_end.h        the frame's end, the rest of the public API
//   mvr_pass_trace.cpp    the pass trace (lmu_pass_trace; diagnostics, log only)
//   mvr_pass_census.cpp   the post census (lmu_post_census; diagnostics, log only)
//   mvr_pass_samples.cpp  sample images: regions copied, read back and compared (gates C and D, mvr_compare), the car
//                         slots' gate image
#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../engine/d3d11_state.h"
#include "../../quad/focus_mask.h"
#include "car_motion.h"
#include "focus_post.h"
#include "mvr_pass.h"
#include "yebis_shaders.h"

namespace lmu::mvr_detail {

using Microsoft::WRL::ComPtr;

constexpr unsigned kMaxRects = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

// NVIDIA's single-pass stereo switch (State::twoViews' default; mvr_set_two_views replaces it in tests).
int nvapi_two_views(ID3D11DeviceContext* context, bool on);

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
    UINT viewOffset = ~0u, viewProjOffset = ~0u;  // its viewMatrix and viewProjMatrix there (kVsViewMatricesGuid)
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
// A pixel shader's YEBIS role (the probe's marking, yebis_shaders.h) by its address, likewise valid until the game
// creates another pixel shader (mvr_pixel_shader_generation: a new one may take a released one's address): the pass
// tracker looks at the pixel shader of nearly every draw (copy_before at each DrawIndexed until the frame's first
// multisample bind, and all through the passes with MSAA off), and GetPrivateData at each cost more than its own work.
struct PsRoleEntry {
    ID3D11PixelShader* ps = nullptr;
    uint32_t epoch = 0;
    UINT role = 0;
};
struct PsRoleSet {
    PsRoleEntry entry[2];
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
    // The pass tracker's and the frame's shape's looks at the draws (copy_before, observe_post_draw and the like), the
    // calls they make included; not the draws passed on, nor the post-processing's start (`post`).
    uint64_t tracker = 0;
    // The post-processing's start (post_begins: DLSS, the slot unbind, the motions' upload, the focus post's window),
    // taken out of the hooks' and the tracker's figures it sits in.
    uint64_t post = 0;
    // The car hooks' calls in all, the calls passed on included (1 in 16 timed): the most their detours can cost.
    uint64_t carHooks = 0;
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
    uint64_t loggedRoleLookups = 0, loggedRoleMisses = 0, loggedShapeSkipped = 0, loggedRolesByBind = 0;
    HookTime time;
    unsigned long long tscAtLog = 0;
    long long qpcAtLog = 0;
    ComPtr<ID3D11Buffer> remapCb;
    // The game's own VS b13 (held: our remap sits in its place while bound) and whether the remap is bound now; the
    // game's PS b13 as a pass began (held) and whether texture sharpening's buffer went there this pass.
    ID3D11Buffer* gameVsCb13 = nullptr;
    bool remapBound = false;
    ID3D11Buffer* gamePsCb13 = nullptr;
    bool biasCbUsed = false;
    TwoViewsFn twoViews = &nvapi_two_views;
    bool broken = false;  // single-pass stereo refused once and instancing couldn't take over: off for the session
    bool stereoOffLogged = false;  // a failed turn-off of single-pass stereo said (it breaks MVR for the session)
    // Instancing instead of single-pass stereo (mvr_use_instanced): the instanced twins bound, the game's draws doubled,
    // no stereo switch. canInstance: it may take over when single-pass stereo is refused.
    bool instanced = false, canInstance = false;
    bool undoubledLogged = false;
    // The pass.
    int pass = -1;
    bool drawFocus = false, sample = false, replaceResolve = false;
    uint64_t passTwinBinds = 0;  // this pass's draws that drew the focus view too
    MvrPassView view, viewOf[2];
    // What the game bound. gameVs, gamePs, gameBlend and their class instances are held (one reference each, taken when
    // recorded, given back when replaced: hold / hold_instances): while a twin is bound in their place the pipeline
    // holds the twin, not them, and LMU releases its shaders at any time; the twins live as their private data.
    ID3D11VertexShader* gameVs = nullptr;
    // gameVs's twins as its bind looked them up (gameVsLooked == gameVs, the twins' epoch then): update() reuses them.
    ID3D11VertexShader* gameVsLooked = nullptr;
    ID3D11VertexShader* gameVsTwin = nullptr;
    ID3D11VertexShader* gameVsInstanced = nullptr;
    uint32_t gameVsEpoch = 0;
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
    bool manyViewportsLogged = false;  // a depth-bound draw with more than one viewport, eye only (said once)
    // What is applied.
    ID3D11VertexShader* boundVs = nullptr;
    // The two viewports / scissors last applied with the focus rect appended (a game's Get/Set round trip gives them
    // back: mapped to its own one).
    D3D11_VIEWPORT appliedVps[2] = {};
    D3D11_RECT appliedScs[2] = {};
    bool twoVpsApplied = false, twoScsApplied = false;
    // Instancing: the input layout last checked by a doubled draw (raw, while bound), its kind (0 unknown, 1 per-vertex,
    // 2 per-instance) and twin, valid for the probe's layout generation.
    ID3D11InputLayout* layoutChecked = nullptr;
    ID3D11InputLayout* layoutTwin = nullptr;
    uint64_t layoutGeneration = ~0ull;
    UINT layoutKind = 0;
    bool layoutEyeOnlyLogged = false;
    bool on = false, stereo = false;
    // Texture sharpening (mvr_mip_bias_hooks, MvrPassView::mipBias): the game's pixel shader as it bound it, what is
    // bound, this pass's bias (from its view), and the constants (focus viewport, focus and eye bias) as uploaded.
    bool biasHooks = false, bias = false, biasCbBound = false;
    uint32_t biasChecks = 0;  // twin binds, every 16th checking the buffer is still bound
    ID3D11PixelShader* gamePs = nullptr;  // held, like gameVs
    ID3D11PixelShader* boundPs = nullptr;
    ID3D11ClassInstance* psInstances[256] = {};
    UINT psInstanceCount = 0;
    PsTwinSet psTwins[kTwinSets];
    uint32_t psEpoch = 1;
    uint64_t psGeneration = 0;
    PsRoleSet psRoles[kTwinSets];  // role_of_ps: the pixel shaders' YEBIS roles by address
    uint32_t roleEpoch = 1;
    uint64_t roleGeneration = 0;
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
    // View tracking (mvr_view_track; DLSS's camera on any build, view_pair.h): car tracking's buffer reads without its
    // table. The bound shader's viewMatrix and viewProjMatrix places, the learned buffer's (from the draw that taught
    // it), the pair last written into it, and each eye pass's distinct pairs as drawn (the eye's own among the mirrors').
    bool viewTrack = false, viewPairRead = false;
    UINT vsViewOffset = ~0u, vsViewProjOffset = ~0u;
    UINT viewCbOffset = ~0u, viewProjCbOffset = ~0u;
    float viewPair[32] = {};  // view, then viewProj (row-major, as the buffer holds them)
    // Room for the shadow cascades' and cube maps' cameras that pass 0 draws before its scene (the eye's own comes
    // after them: when full, the oldest goes).
    static constexpr int kViewPairs = 16;
    struct PassPairs {
        float pair[kViewPairs][32] = {};
        // Per pair, its draws with an identity worldMatrix (the static world's: its camera is the world's; the cockpit
        // and the sky may be drawn with the same FOV from another space).
        uint32_t worldDraws[kViewPairs] = {};
        int count = 0, next = 0, last = -1;  // pairs held, the one to replace when full, the one kept last
        uint64_t frame = 0;                  // carFrame they were drawn in
    } passPairs[2];
    CarMotion cars;
    uint64_t carFrame = 0;
    CarMotionStats carLogged;     // the counts at the last log line
    // Car slots (mvr_car_slots): the level (0 off); the slot target (the scene target's size) and its views; per slot a
    // view of its word (slot + 1) for t127, the one bound; the target bound now (the scene's binds in a pass) and this
    // pass's views cleared; the game's blend state as bound (its factor and mask), whether a twin is bound in its place,
    // the depth state's writing; the default blend state's twins ([0] target 1 kept, [1] written); each slot's motion.
    int slotsLevel = 0;
    bool carActive = true;  // mvr_car_active (an A/B's side)
    int eyeOff = -1;        // mvr_eye_off: the eye pass (0-1) whose DLSS is left out; -1 none
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
    // The last blend state's twin (per write mask) and depth state's writes. Each holds a reference to the state it
    // caches, so no other state can take its address while it is cached (the twin lives as the held state's private
    // data); reset at each frame end. (Not render target views: a reference held on a swapchain's back buffer makes
    // the game's ResizeBuffers fail, lmu_focus_post_roles_test.)
    struct BlendLook {
        ComPtr<ID3D11BlendState> game;
        ID3D11BlendState* twin[2] = {};
    } blendLook;
    struct DepthLook {
        ComPtr<ID3D11DepthStencilState> ds;
        bool writes = true, known = false;
    } depthLook;
    ID3D11RenderTargetView* sceneRtvChecked = nullptr;  // the last render target view checked for the scene's (raw)
    bool sceneRtvIsScene = false;
    ID3D11Resource* sceneCheckedAgainst = nullptr;  // the tracker's scene target it was checked against (raw)
    ID3D11Resource* sceneRtvResource = nullptr;     // that view's texture (raw, compared only) and its description
    D3D11_TEXTURE2D_DESC sceneRtvDesc{};
    ID3D11BlendState* gameBlend = nullptr;  // held, like gameVs (its twin is bound in its place)
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
    // In the post window before the focus copy: the last draw into a single-sample texture of the resolve's size other
    // than the resolve's target (a stage between the resolve and YEBIS, whose copy then starts from it: reflections
    // high, 2026-11-11), raw, compared only; whether its viewports covered the focus view beside the eye; its pixel
    // shader's checksum (the log). Logged once each: the focus copy from that image, from the resolve instead.
    ID3D11Resource* stageImage = nullptr;
    bool stageCovers = false;
    uint32_t stagePs[4] = {};
    bool stageImageLogged = false, stageResolveLogged = false;
    // R3 (docs/recon-lmu.md "R3b/R3c design"): the focus view's final beside the eye in the eye's final target. This
    // pass's blit replayed; draws into the eye's final after it are overlays MVR doubles (the driver tags); the eye
    // finals of this frame (the VR UI after the passes is doubled into them too) and their sizes.
    bool focusBlitted = false, overlayTarget = false;
    // mvr_fsr: FSR on; the eyes' images (raw, compared only); said once.
    bool fsr = false, fsrLogged = false;
    // The probe knows no copy / no final blit shader by its checksum (as of this pass's begin): found by bindings.
    bool copyUnknown = false, blitUnknown = false, byBindingsLogged = false;
    int copyKnownForTests = -1, blitKnownForTests = -1;
    // mvr_shape_ready: this pass's steps as observe_post_draw saw them; each eye pass's outcome; the last frame's.
    bool obsCopied = false, obsComposited = false, obsFinal = false;
    // Every scene-sized image a draw made from the resolve's target this pass (raw, compared only): the composite
    // reads one of them, not always the latest (other steps read the resolve too: 2026-11-12 13:20 run, post high).
    ID3D11Resource* obsCopyTargets[6] = {};
    unsigned obsCopyCount = 0;
    bool shapeOk[2] = {}, shapeReady = false, shapeForceBad = false;
    bool readsAsGameForTests = false;  // mvr_test_reads_as_game: this image's RSGetViewports taken as the game's
    // mvr_viewport_reads ([experiment] lmu_viewport_reads): 1 any caller but this image given the game's own viewports,
    // 2 only the game's exe (YEBIS's), 0 the hook not installed. Reads by other images seen (another DLL: NGX's, a tool's).
    int viewportReads = 1;
    bool otherReadLogged = false;
    bool gameReadLogged[2] = {};       // the game's viewport read given its own: under the cut, beside the focus view
    bool extraResolveLogged = false;   // a second multisample resolve in a pass (logged once)
    // observe_post_draw this pass: not where it drew focus views after a frame with both (the recovery reads the shape
    // only while the eyes render alone, or once 45 frames in a row lacked them: every frame then is looked at).
    bool observeShape = true, lastFrameFinals = false;
    int shapeMissing[2] = {}, shapeMissingLast[2] = {};  // mvr_shape_missing: each eye pass's first step not seen
    ID3D11Resource* fsrEye[2] = {};
    // mvr_fsr_watch: CSSetUnorderedAccessViews hooked, and whether an eye's image was bound there since it was last read.
    bool fsrWatch = false, fsrUavSeen = false;
    ComPtr<ID3D11Resource> eyeFinal[2];
    bool finalThisFrame[2] = {};
    unsigned finalW[2] = {}, finalH[2] = {};
    bool blitRefusalLogged = false;
    int maskDrawsLogged = 0;  // the first lens mask draws logged with their state
    const std::atomic<bool>* maskStale = nullptr;  // mvr_mask_stale
    int maskDroppedSince = 0;                       // mvr_mask_dropped
    bool maskAbDrop = false;                        // mvr_mask_ab_drop
    PanelFn panelFn = nullptr;                      // mvr_panel_draws
    // DLSS's depth before the transparents (mvr_glass_depth, mvr_pass_glass.h).
    struct Glass {
        int mode = 0;            // mvr_glass_depth
        bool read = true;        // mvr_glass_read: DLSS reads the copy this frame
        bool blends = false;     // the game's bound blend state blends into target 0 (at its bind, with mode)
        bool sceneBound = false; // the bound depth target is this eye pass's scene depth
        ID3D11DepthStencilView* dsv = nullptr;  // its view (raw: bound now)
        bool opaqueSince = true; // an unblended draw wrote the depth since the pass began or its last copy
        int copies = 0;          // this pass's
        uint64_t opaqueAfterCopy = 0;  // unblended depth-writing draws since its last copy
        bool copied = false;     // this pass has a copy for DLSS
    } glass;
    // The game's latest CopySubresourceRegions (mvr_copy_source): dst raw, compared only; src held (hold: the caller
    // queries it after the game may have released it; given back two frame ends later); the frame count when seen.
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
    bool inList = false;  // the game's command list executing (its replayed calls counted)
    std::atomic<bool> listsAsBefore{false};  // mvr_replay_lists_plain(false): the lists executed as before the fix
    int framesTraced = 0, traceDraws = 0, traceDropped = 0;
    unsigned long long framesSinceInstall = 0;
    bool have[4] = {};
    unsigned long long lastStatsFrame = 0;
    unsigned long long timedFrames = 0;  // frames timed (mvr_time_hooks: 1 in kTimedEvery) since the last log
    // The focus mask (mvr_focus_mask; quad/focus_mask.h): its depth drawn into the focus viewport before the scene.
    struct FocusMask {
        bool on = false;
        simqvfr::FocusArea area{};  // the shape asked for (its aspect set from the focus viewport's at the draw)
        unsigned margin = 16;       // texels
        // The mesh as last made, for this area, margin and focus viewport size; empty: nothing to mask there.
        simqvfr::FocusArea meshArea{};
        unsigned meshMargin = 0, meshW = 0, meshH = 0;
        bool meshMade = false;
        UINT vertices = 0;
        uint64_t epoch = 0;  // the mesh's (a new one records the list again)
        ComPtr<ID3D11Buffer> vb;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11DeviceContext> deferred;
        // The draw recorded once (its state restored as it runs) for these targets and viewport.
        ComPtr<ID3D11CommandList> list;
        ComPtr<ID3D11DepthStencilView> listDsv;
        D3D11_VIEWPORT listVp{};
        uint64_t listEpoch = 0;
        bool failed = false;                   // its shader or states couldn't be made: off for the session
        ID3D11Resource* sceneDepth[2] = {};    // each eye pass's depth target as it began (raw, compared only)
        bool done[2] = {};                     // drawn (or decided against) in this eye pass
        bool otherTestLogged = false;
    } mask;
    MvrPassStats stats;
    // No destructor gives the held game objects back: g lives until the process ends, and calling into d3d11 from
    // DLL_PROCESS_DETACH risks its locks (held by threads ExitProcess already ended); the references go with the process.
};
extern State g;

// The per-draw buffer read (car tracking, or view tracking alone).
inline bool cb_track() { return g.carTrack || g.viewTrack; }

inline bool ours(ID3D11DeviceContext* context) {
    if (g.inList && context == g.context)
        ++g.stats.callsInLists;
    return context == g.context && g.pass >= 0 && !g.replaying;
}

// R4b's pass tracker (mvr_track_passes): the eye passes as the d3d11 calls show them, compared with the game's bounds.
struct Tracker {
    bool on = false;
    bool open = false, resolved = false, blitSeen = false;
    // The open pass's multisample scene target (raw, compared only) and whether a YEBIS draw (any role) came after its
    // resolve. A stage between the resolve and YEBIS (reflections high: a single-sample depth for YEBIS made, then the
    // scene target bound again, 2026-11-11 pass trace) rebinds the pass's own scene: still that pass, not the next's.
    ID3D11Resource* sceneTarget = nullptr;
    bool postSeen = false, rebindLogged = false;
    bool fsrEndLogged = false;  // FSR with MSAA off: a pass ended at its eye image without our focus step (logged once)
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
    ID3D11Resource* drawnWithDepth[32] = {};
    // MSAA off past a stage (post effects medium and up, 2026-11-12 pass trace): YEBIS's copy reads the stage's image
    // of the scene, not the scene's target, which YEBIS's composite then draws into. Outside the passes: the copy's
    // source this frame when it wasn't drawn with depth (raw, compared only) and its size; a draw reading it into a
    // target of that size drawn with depth teaches the scene's target. In an open single-sample pass: render target
    // 0 is another texture, bound without depth (a draw into it reading the scene begins the post-processing).
    ID3D11Resource* stageCandidate = nullptr;
    UINT candidateW = 0, candidateH = 0;
    bool offScene = false, stagePostLogged = false;
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
    // The size of the scene target the last tracked pass began on: the game's main targets (mvr_scene_size), set before
    // its driveBegin.
    unsigned sceneSizeW = 0, sceneSizeH = 0;
    TrackFn driveBegin = nullptr, driveEnd = nullptr;  // mvr_track_drive: the passes' work on these bounds
    uint64_t loggedFrames = 0;
    int detailsLogged = 0;
    bool menuLogged = false;
};
extern Tracker tr;

// The pass trace (log only): while the tracker drives the passes, three frames' (one without debug logs: pt_first)
// render target binds with depth or a
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
};
extern PassTrace pt;

// The pass trace (mvr_pass_trace.cpp), called only while pt.recording: an event of this frame, a texture's number
// (its size at its first mention), a YEBIS draw's input and target, and the frame's end (after the tracker's).
void pt_event(const char* format, ...);
std::string pt_tex(ID3D11Resource* resource);
void pt_draw(ID3D11DeviceContext* context, const char* call);
void pt_frame_end(unsigned long long frame, int passes);

// The live resolution (mvr_size_cut): mvr_pass.cpp "Live resolution".
struct SizeCut {
    bool on = false;
    unsigned fullW = 0, fullH = 0, eyeW = 0, eyeH = 0;
    unsigned long long viewports = 0, scissors = 0, left = 0;  // since the install: cut, cut, left as they were
    bool leftLogged = false;
    // The post's depth copy was bound in this pass (MSAA off: the scene's last step before its post): no more cuts in it.
    // Every bind after it is the post's (YEBIS's), and YEBIS reads the viewport bound as it starts (its device's
    // GetViewport, the only RSGetViewports in it) to size the region it processes: it must see the game's own (full)
    // viewport there, as with the render scale written, or it stretches the eye's corner over its whole target.
    bool endedThisPass = false;
    unsigned cutLoggedW = 0, cutLoggedH = 0;  // the render size the last "cut" line was for (logged again at a new one)
    // Viewports at 0,0 larger than the eye on both axes that aren't the game's render size: left as they are, logged
    // (the first 12 sizes): a game size the cut doesn't know shows here.
    float otherSizes[12][2] = {};
    int otherCount = 0;
    // The game's own rects as it set them (g.vps / g.scs hold them cut): the cut is decided again from these at each
    // render target bind, since the game may set a viewport before binding the target it is for (its depth copy does).
    UINT vpCount = 0, scCount = 0;
    D3D11_VIEWPORT vps[kMaxRects] = {};
    D3D11_RECT scs[kMaxRects] = {};
};
extern SizeCut sizeCut;

// ---- Post census (mvr_post_census; diagnostics, log only): what the eye's post-processing draws with ----
// Zero writes (docs/recon-lmu.md "Zero writes"): YEBIS's region follows the game's render scale by itself, so before its
// draws can be scaled at the d3d11 level, every draw of tracked pass 0 from its resolve (or post start) to its end is
// logged at a sampled frame: shaders, target, the game's viewport and scissor, the inputs, the first vertices and the
// first constants of each bound buffer. Two censuses at two live resolutions in one run show what follows the scale.
struct PostCensus {
    int level = 0;
    bool active = false;
    int draws = 0;
    unsigned long long next = 0;  // framesSinceInstall of the next census
    unsigned long long taken = 0;
    unsigned lastEye[2] = {};     // the eye size of the last census (a new size: a census at the next pass 0)
    // Level 2: each bound constant buffer's whole bound range saved (logs\census\c<N>_d<draw>_<stage><slot>.bin) for
    // the first kCensusDumps censuses at a new eye size; with lmu_mvr_dump=1's shaders, read offline.
    bool dumpThis = false;
    int dumps = 0;
    // Level 4: also the draws between a frame's end and the next pass 0 (outside the tracked passes), as level 3 logs them.
    bool pre = false;
};
extern PostCensus census;
constexpr int kCensusDumps = 4;
constexpr int kCensusDraws = 120;
constexpr unsigned long long kCensusEvery = 900;
extern std::unordered_set<std::uint64_t> censusSaved;  // this census's saved contents (levels 3 and up)

// A draw the census logs (per draw: inline).
inline bool census_here(ID3D11DeviceContext* context) {
    if (census.pre)
        return context == g.context && !g.replaying && g.pass < 0;
    return census.active && g.pass == 0 && (census.level >= 3 || g.afterResolve) && ours(context);
}
// The census's line for a draw (before it) and, at level 5, its target saved (after it): mvr_pass_census.cpp.
void census_any(ID3D11DeviceContext* context, const char* call, long long a, long long b);
void census_after(ID3D11DeviceContext* context);

// mvr_pass.cpp's, used by the diagnostics too: a pixel shader's YEBIS role (0: none), a texture's size and format
// for the log.
UINT role_of_ps(ID3D11PixelShader* ps);
std::string describe(ID3D11Resource* resource);

// mvr_pass_samples.cpp: `dst` (made or re-made at w x h in `src`'s format) = src's region at (x, y); a
// half-float RGBA texture read back; its pixels tonemapped for looking at (BGRA); the car slots' gate image; gate C's
// comparison at a sample frame.
bool copy_region(ComPtr<ID3D11Texture2D>& dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w, unsigned h);
bool read_halves(ID3D11Texture2D* texture, std::vector<uint16_t>& out, unsigned& w, unsigned& h);
std::vector<uint8_t> tonemapped(const std::vector<uint16_t>& px, unsigned w, unsigned h);
void save_slot_image(int eye, ID3D11Resource* scene);
void compare_sample(unsigned long long frame);

}  // namespace lmu::mvr_detail

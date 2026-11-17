// corner.h's internals, shared by its files (detail: not for other code). One state for the whole path, `g`, used
// from the game's render thread (the observers) and, where its members say so, other threads (atomics, frameMutex).
//   corner.cpp          the D3D11 observers, the frame's end, install and session end
//   corner_math.cpp     sizes and rectangles: the corner, the focus box, twins
//   corner_mirrors.cpp  the game's large constant buffers mirrored and rewritten; a draw's constant range rewritten
//   corner_eye_order.cpp which eye a pass belongs to
//   corner_focus.cpp    the focus view: two views, its scene draws, the full-screen passes replayed, twins
//   corner_present.cpp  the headset images: the corner scaled back, the focus view composed; the swapchain calls
//   corner_burst.cpp    the burst recorder (dev builds' Ctrl+Alt+B)
//   corner_diag.cpp     performance lines and the dev builds' hotkeys
#pragma once

#include "corner.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../engine/context_hooks.h"
#include "../../engine/d3d11_state.h"

namespace scs::detail {


using Microsoft::WRL::ComPtr;
namespace ch = simqvfr::context_hooks;
namespace d3s = simqvfr::d3d11_state;

constexpr UINT kMaxVectors = 4096;  // a constant buffer's most (64 KB)
constexpr UINT kMaxViews = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

// The game's state one of our blits changes, kept and put back (d3d11_state.h): `parts` with pixel t0 and s0, the
// shaders without class instances, a saved count of 0 viewports or scissors left unset (as the blits always did).
inline d3s::Spec blit_state(uint32_t parts) {
    d3s::Spec s;
    s.parts = parts;
    s.psSrv = s.psSamplers = {0, 1};
    s.instances = false;
    s.emptyRects = false;
    return s;
}

// A large constant buffer of the game's (each written once at the frame's start: ats_frame5585.rdc) and our copy with
// every eye-shaped pair rewritten, bound in its place for the rest of the frame.
constexpr UINT kMirrorMin = 256 * 1024;
struct Mirror {
    ComPtr<ID3D11Buffer> game, copy;
    UINT bytes = 0;
    uint64_t refreshedFrame = ~0ull;
    uint64_t usedFrame = 0;  // the last frame a bind used it (evict_mirrors)
    int id = -1;             // its slot in State::mirrorIds (every mirror has one: learn mirrors nothing without)
    uint32_t seenGen = 0;    // State::mirrorGen[id] as of the copy
};

struct Range {
    ComPtr<ID3D11Buffer> game;  // the game's buffer as bound (a reference while we hold the draw)
    UINT first = 0, count = 0;  // in constants (16 bytes)
    bool ours = false;          // ours is bound in its place for this draw
};

struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;  // the game's immediate context
    float percent = 0;
    float fx = 1, fy = 1;  // the corner's share of each axis, whole pixels of the eye
    unsigned eyeW = 0, eyeH = 0;
    DWORD thread = 0;
    bool active = false;  // between an eye pass's first G-buffer bind and the frame's end
    bool cut = false;     // the bound target has the eye's shape
    bool smallTarget = false;
    unsigned boundW = 0, boundH = 0;
    std::atomic<uint64_t> endFrames{0};
    uint64_t seenEndFrames = 0;
    std::atomic<bool> cornerNow{false};  // set as a frame's eyes go into the corner, taken by its xrEndFrame
    bool thisFrameCornered = false;
    // The game's own viewports and scissors, as last set.
    D3D11_VIEWPORT viewports[kMaxViews] = {};
    UINT viewportCount = 0;
    D3D11_RECT scissors[kMaxViews] = {};
    UINT scissorCount = 0;
    // The rewrite: the game's range copied in, rewritten out, then copied into the constant buffer bound.
    ComPtr<ID3D11Buffer> rawIn, rawOut, params, vsCb, psCb, vsCbFocus, psCbFocus;
    ComPtr<ID3D11ShaderResourceView> rawInView;
    ComPtr<ID3D11UnorderedAccessView> rawOutView;
    ComPtr<ID3D11ComputeShader> patchCs, mirrorCs;
    std::vector<Mirror> mirrors;
    ComPtr<ID3D11Buffer> bigIn, bigOut;  // the mirrors' scratch, as large as the largest
    ComPtr<ID3D11ShaderResourceView> bigInView;
    ComPtr<ID3D11UnorderedAccessView> bigOutView;
    UINT bigBytes = 0;
    uint64_t mirrorRefreshes = 0, mirrorSwaps = 0, midFrameWrites = 0;
    // Writes (Unmap) to a mirrored buffer from a thread other than the one drawing the eyes: our copy is made on the
    // drawing thread, so another thread's write would leave the rest of the frame reading the copy's older contents. Such
    // a write bumps the buffer's generation, and its next bind copies it again (refresh). Counted too (ETS2 2026-11-15:
    // markers flashing in one eye for single frames; ATS: 8,629 such writes, none with the eyes in the corner). The
    // buffers' identities are kept here for the other threads to compare with. A slot is null while free (an evicted
    // mirror's, reused by the next); with none free a buffer isn't mirrored at all (mirrorNoId counts those binds).
    static constexpr int kMirrorIds = 16;
    std::atomic<ID3D11Resource*> mirrorIds[kMirrorIds] = {};
    std::atomic<uint32_t> mirrorGen[kMirrorIds] = {};
    std::atomic<int> mirrorIdCount{0};  // the slots ever used (the other threads compare those)
    uint64_t mirrorsEvicted = 0, mirrorNoId = 0;
    bool mirrorNoIdLogged = false;
    std::atomic<bool> activeNow{false};  // the eyes in the corner (g.active, for the other threads)
    // A session start or end asked for and not done yet (service_lifecycle): the swapchain calls leave the state alone.
    std::atomic<bool> lifecyclePending{false};
    // The game's state put back after session end (session_ending): 0 nothing asked, 1 asked, 2 being done, 3 done
    // (observing stopped). A fallback after restoreDeadline (GetTickCount64) stops observing without it.
    std::atomic<int> restore{0};
    std::atomic<uint64_t> restoreDeadline{0};
    std::atomic<uint64_t> otherWrites{0}, otherWritesActive{0};
    std::atomic<DWORD> otherWriter{0};
    bool otherWriterLogged = false;
    // Ctrl+Alt+N: the scene draws read the game's own buffers (no mirror copies), live; the full-screen passes keep their
    // per-draw rewrite (it reads the game's buffer itself).
    bool mirrorsOff = false, mirrorsKeyDown = false;
    Range vs, ps;
    CornerStats stats;
    uint64_t loggedDraws = 0;
    std::mutex frameMutex;  // corner_frame_end's copies
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<XrCompositionLayerProjectionView> views;
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrFrameEndInfo info{};
    // The swapchains' images and the one the game wrote last this frame (seen at its render-target binds).
    struct Chain {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<ID3D11Texture2D*> images;  // raw: the runtime's, alive while the swapchain is
        // The images the game holds, in the order it acquired them: OpenXR releases the oldest first, and ATS acquires
        // its next image before releasing the one it drew (S1 run 6: scaling "the last acquired" scaled the next
        // image and released this one unscaled: the small copy popping in).
        std::deque<uint32_t> held;
        bool scaled = false;                   // scaled back at this frame's release
        int eye = -1;                          // the view it is submitted for (xrEndFrame)
    };
    std::vector<Chain> chains;
    // The upscale: the corner copied into `scratch`, drawn back over the image.
    ComPtr<ID3D11Texture2D> scratch;
    ComPtr<ID3D11ShaderResourceView> scratchView;
    D3D11_TEXTURE2D_DESC scratchDesc{};
    // Stage 4: the focus region of a released image, drawn over the box of the scaled-up eye. Dev builds' Ctrl+Alt+K:
    // on / off.
    ComPtr<ID3D11Texture2D> focusScratch;
    ComPtr<ID3D11ShaderResourceView> focusScratchView;
    D3D11_TEXTURE2D_DESC focusScratchDesc{};
    bool composeFocus = true, composeKeyDown = false;
    uint64_t composed = 0;
    ComPtr<ID3D11VertexShader> blitVs;
    ComPtr<ID3D11PixelShader> blitPs, featherPs, squashPs;
    bool shootSquash = false;  // the next shot through squashPs
    ComPtr<ID3D11BlendState> featherBlend;
    struct Stretch {  // a scratch per size and format (stretch_into_twin)
        D3D11_TEXTURE2D_DESC desc{};
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> view;
    };
    std::vector<Stretch> stretches;
    uint64_t stretched = 0;
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11RasterizerState> raster;
    struct Target {
        ID3D11Texture2D* image = nullptr;
        ComPtr<ID3D11RenderTargetView> view;
    };
    std::vector<Target> targets;
    uint64_t upscaled = 0, upscaleMisses = 0, otherThreadReleases = 0, heldTwo = 0;
    // Stage 3: the full-screen passes drawn again for the focus view. A game texture's focus region is written by
    // the two-view scene draws and by these replays; a replay reads its inputs from "twins": textures of ours, as large
    // as the game's, with the region copied to their top-left (Prism's passes address a region from the origin only).
    struct Tex {
        uint64_t writeSerial = 0, writeFrame = ~0ull;  // the focus region's last write
        // The game's, held by every entry (with or without a twin): its address, the key, can't be another texture's
        // while the entry lives (else a new texture there would inherit the write serial or the failed flag).
        ComPtr<ID3D11Texture2D> game, twin;
        ComPtr<ID3D11RenderTargetView> twinRtv;
        ComPtr<ID3D11ShaderResourceView> twinAll;  // every mip (GenerateMips)
        uint64_t copiedSerial = ~0ull;
        bool zero = true, failed = false;
        UINT mips = 1;
        uint64_t usedFrame = 0;  // the last frame it was read or written (evict_unused)
        uint64_t bytes = 0;      // its twin's share of twinBytes
    };
    std::unordered_map<ID3D11Texture2D*, Tex> texes;
    struct TwinView {
        ComPtr<ID3D11ShaderResourceView> game, twin;  // the game's view (and so its texture) kept while this lives
        uint64_t usedFrame = 0;
    };
    std::unordered_map<ID3D11ShaderResourceView*, TwinView> twinViews;
    ID3D11Texture2D* boundRt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};  // raw, compared only
    bool boundMarked = false, boundSwapchain = false;
    bool mvr = false;  // NVIDIA's two views there: the scene draws fill the focus region
    Range vsFocus, psFocus;
    uint64_t replays = 0, twinCopies = 0, twinBytes = 0, finalShots = 0;
    uint64_t undeclared = 0;  // rewrites for a shader whose declared constants we don't know (the whole range read)
    int finalPasses = 0;
    // The focus boxes: each eye's top-left on its screen (x right, y down), this frame's and the frame before's (the
    // reprojections land in the last frame's box). From the gaze (corner_set_gaze), taken at the frame's start.
    std::atomic<float> gaze[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    std::atomic<int> livePercent{0}, liveFocusPercent{0};  // corner_set_live's, 0: none yet
    std::atomic<int> liveFocusPercentY{0}, liveDensity{100};
    // The box's height as a share of the eye's screen (focusPercent: its width) and the focus view's density: its
    // region in the targets is box x density of them.
    int focusPercentY = 0;
    float density = 1;
    // corner_set_look's: the composed box's edge.
    std::atomic<float> lookFeather{0.12f}, lookRadius{0}, lookColor[3] = {0, 1, 0};
    std::atomic<bool> lookBorder{false};
    ComPtr<ID3D11Buffer> lookCb;
    std::atomic<int> way{0};  // corner_way's, set at each frame's start
    float box[2][2] = {{0.35f, 0.35f}, {0.35f, 0.35f}}, prevBox[2][2] = {{0.35f, 0.35f}, {0.35f, 0.35f}};
    bool postPhase = false;  // past this eye's scene: the eye is the one whose final blit comes next
    int blits = 0;           // final blits into the headset's images this frame
    int boundChainEye = -1;  // the eye of the swapchain image bound
    // The game's Scaling setting isn't 100%: its eye targets aren't the size of the headset's images, so its final blit
    // resamples (ETS2 at 75%: 3896x3128 into 5192x4168, 2026-11-15). The path doesn't handle that and stays off for the
    // session's frames: the game draws as without Sim-QVFR. Checked whenever the eye's size is (re)learned.
    bool scaleMismatch = false;
    int remapEye = -1;       // the eye whose remap the scene draws' constant buffer holds
    // The eye of the frame's first and second pass: set at the first scene's G-buffer bind from the order learned for
    // the headset image being drawn (below); where that isn't known yet, the order of the frame before's final blits
    // (ATS draws the right eye first; nothing here assumes an order).
    int passEye[2] = {0, 1};
    int passEyeLogs = 0;  // the first wrong frames logged, the rest counted
    // ETS2 at 100% Scaling: the eye order follows the headset image index (bursts 2026-11-15 04:47 and 05:07, two
    // sessions: images 0 and 1 right eye first, both scenes then both post chains; image 2 left eye first, each eye's
    // scene, post and final blit in turn; Prism's render graph orders the eyes, their images among its resources). The
    // frame before's order was wrong two frames in three: the focus view drawn at the other eye's box, blinking. (Each
    // eye's depth buffer as the key was measured at 05:06: the eyes share it.) So the first final blit's eye is learned
    // per image index, and looked up at the next frame with that index. An index seen with both orders counts a conflict
    // and keeps the latest.
    static constexpr int kOrderImages = 8;
    std::atomic<int> acquiredIndex{-1};  // the left eye's swapchain's last acquired image (corner_acquired)
    int frameImage = -1;                 // this frame's, taken at its first scene
    int firstEyeByImage[kOrderImages] = {-1, -1, -1, -1, -1, -1, -1, -1};
    bool orderLogged = false;
    uint64_t orderHits = 0, orderMisses = 0, orderConflicts = 0, orderChanges = 0;
    uint64_t boxMoves = 0;
    // Diagnostics for what shows only while the box jumps (S5 run 2: far terrain gone for a moment as the gaze arrives):
    // Ctrl+Alt+J arms a burst at each of the next three jumps of more than 8% of the screen; Ctrl+Alt+H switches the
    // reprojection matrices between the frame before's box (0) and this frame's (1), live.
    int jumpBursts = 0;
    bool jumpKeyDown = false, historyKeyDown = false, historyCurrentBox = false;
    // Where our own time goes (the submission side: what the calls cost on the render thread), per bucket, and the frame
    // rate, logged every 450 frames; Ctrl+Alt+G switches the focus view's work off and on, live, for an A/B.
    enum { kTimeFocusDraws, kTimeReplays, kTimeRewrites, kTimeMirrors, kTimeRelease, kTimes };
    int64_t ticks[kTimes] = {};
    uint64_t timed[kTimes] = {};
    int64_t perfSince = 0;
    uint64_t perfFrame = 0;
    bool focusOff = false, focusKeyDown = false;
    // The launch's bisection switches (corner_set_diag; [experiment] scs_focus_draws, scs_focus_replays,
    // scs_replay_skip): the focus view's scene draws off, its replayed passes off, or single replays (bit n-1: the
    // frame's nth replay) skipped. Set before the session, kept across sessions.
    bool diagNoDraws = false, diagNoReplays = false;
    uint64_t replaySkip = 0;
    unsigned frameReplay = 0;     // this frame's replays so far (take_frame_end resets it)
    uint64_t replayLogFrame = 0;  // the frame whose replays are each logged once (300 cornered frames in)
    bool occlusionKeyDown = false, ownOcclusion = false;  // Ctrl+Alt+A: the half-size passes replayed for the focus view
    // Stage 2: the focus view beside the corner.
    int focusPercent = 0;
    bool depthBound = false, fullSize = false;  // the bound targets: a depth target, the eye's full size
    bool twoViews = false;                      // NVAPI's two views on
    bool viewsDual = false;                     // two viewports (corner, focus) set by us
    ComPtr<ID3D11Buffer> remapCb;
    // The focus view's scene draws without NVIDIA's: Sim-QVFR's own multi-view rendering by instancing. The game
    // shader's instanced twin (view = SV_InstanceID & 1, SV_ViewportArrayIndex = view), every such draw with twice its
    // instances, a layout with per-instance data swapped for its twin (step rates doubled) around the draw. Where the
    // device lets a vertex shader pick the viewport (D3D11_OPTIONS3). The way in use: NVIDIA's where it is there,
    // unless [experiment] multi_view_software=1; Ctrl+Alt+I switches between the two, live.
    bool instanced = false, preferInstanced = false, instancingKeyDown = false;
    bool instancedDraw = false;  // focus_draw bound an instanced twin: this draw goes out doubled
    uint64_t doubled = 0, doubledLayouts = 0;
    ID3D11VertexShader* gameVs = nullptr;  // the game's shader while its twin draws (a reference held)
    ComPtr<ID3D11Buffer> gameCb13;         // the game's vertex b13 while the remap is bound there (gameVs set)
    uint64_t focusDraws = 0, noTwin = 0;
    ComPtr<ID3D11Texture2D> gbufferTex;  // the G-buffer's first target while its pass runs
    int gbufferPasses = 0;               // this frame
    // The burst recorder (dev builds' Ctrl+Alt+B): each image released to the runtime over the next kBurstFrames frames,
    // at a quarter of its size, before and after the scale-up, read back after the burst and written as PNGs.
    struct Shot {
        std::wstring tag;
        unsigned w = 0, h = 0;
        ComPtr<ID3D11Texture2D> staging;
        uint64_t frame = 0;
        int chain = 0;
        bool after = false, cornered = false, scaled = false;
        uint32_t index = 0;
    };
    std::vector<Shot> shots;
    // A burst also keeps every replay's output (the focus region of its first target), in order: which pass loses what.
    struct Crop {
        D3D11_TEXTURE2D_DESC desc{};
        ComPtr<ID3D11Texture2D> tex;
    };
    std::vector<Crop> crops;
    int stage = 0;
    int burstFramesLeft = 0;
    uint64_t burstStartFrame = 0;
    bool hotkeyDown = false;
    ComPtr<ID3D11Texture2D> burstTex;
    ComPtr<ID3D11RenderTargetView> burstRtv;
    D3D11_TEXTURE2D_DESC burstDesc{};
    struct Source {
        ID3D11Texture2D* image = nullptr;
        ComPtr<ID3D11ShaderResourceView> view;
    };
    std::vector<Source> sources;
};
extern State g;

constexpr int kBurstFrames = 20;
constexpr uint64_t kEvictFrames = 900;  // twins unused this long are let go (evict_unused)

struct Timed {  // a bucket's time, for as long as it lives
    int bucket;
    LARGE_INTEGER start;
    explicit Timed(int b) : bucket(b) { QueryPerformanceCounter(&start); }
    ~Timed() {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        g.ticks[bucket] += end.QuadPart - start.QuadPart;
        ++g.timed[bucket];
    }
};

// corner.cpp: the observers, the frame's end, setup
void apply_views();
void take_frame_end();
void on_targets(ID3D11DeviceContext*, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
bool on_viewports(ID3D11DeviceContext*, UINT count, const D3D11_VIEWPORT* in, D3D11_VIEWPORT* out);
bool on_scissors(ID3D11DeviceContext*, UINT count, const D3D11_RECT* in, D3D11_RECT* out);
void on_draw(ID3D11DeviceContext*, ch::Entry entry, UINT count, UINT);
void on_draw_after(ID3D11DeviceContext*, ch::Entry entry, UINT count, UINT, UINT start);
bool make_resources();
void release_all();
void service_lifecycle();  // the session start and end the main thread asked for, done at an xrEndFrame
void restore_game_state();
bool session_ending();  // at each observer's start: true, return at once (the session ended)

// corner_math.cpp
bool focus_on();
bool eye_shaped(unsigned w, unsigned h);
void size_of(ID3D11View* view, unsigned& w, unsigned& h);
void scale_viewports(UINT count, const D3D11_VIEWPORT* in, D3D11_VIEWPORT* out);
void scale_scissors(UINT count, const D3D11_RECT* in, D3D11_RECT* out);
void exact_fraction();
void box_remap(const float (&box)[2], float (&out)[4]);
float beta_x();
float beta_y();
float focus_fx();
float focus_fy();
unsigned twin_full(unsigned eye, float focus);
unsigned twin_w(unsigned w);
unsigned twin_h(unsigned h);
float twin_fx();
float twin_fy();
D3D11_RECT focus_rect_in(unsigned w, unsigned h);
D3D11_RECT focus_rect();

// corner_mirrors.cpp
Mirror* mirror_of_game(ID3D11Buffer* b);
ID3D11Buffer* game_of(ID3D11Buffer* b);
bool big_scratch(UINT bytes);
void run_cs(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out, const void* params,
            UINT groups);
void refresh(Mirror& m);
Mirror* learn(ID3D11Buffer* b);
void evict_mirrors();
void swap_bound(bool on);
void on_unmap(ID3D11DeviceContext*, ID3D11Resource* resource, UINT);
bool on_constant_buffers(ID3D11DeviceContext*, bool, UINT, UINT count, ID3D11Buffer* const* in, ID3D11Buffer** out);
bool rewrite(Range& r, ID3D11Buffer* ours, bool pixel, bool focus = false);
void put_back(Range& r, bool pixel);

// corner_eye_order.cpp
void scene_eye();
void learn_order(int eye);
int current_eye();

// corner_focus.cpp
void evict_unused();
void mark_bound_written();
bool stretch_into_twin(State::Tex& t, ID3D11Texture2D* game, const D3D11_TEXTURE2D_DESC& d, float extent = 1);
State::Tex* twin_of(ID3D11Texture2D* game, const D3D11_TEXTURE2D_DESC& d);
ID3D11ShaderResourceView* twin_view(ID3D11ShaderResourceView* gameView, State::Tex& t);
void replay(UINT vertices, UINT start);
void two_views(bool on);
void dual_views();
void focus_draw();
bool on_draw_replace(ID3D11DeviceContext*, ch::Entry entry, const ch::DrawCall& d);

// corner_present.cpp
DXGI_FORMAT srgb_view(DXGI_FORMAT f);
bool blit_resources();
ID3D11RenderTargetView* target_view(ID3D11Texture2D* image, const D3D11_TEXTURE2D_DESC& d);
bool upscale(ID3D11Texture2D* image, UINT cw, UINT ch, int eye);
State::Chain* chain_of(XrSwapchain handle, PFN_xrEnumerateSwapchainImages enumerate);

// corner_burst.cpp
void shoot_stage(const D3D11_RECT& f, const char* inputs);
ID3D11ShaderResourceView* source_view(ID3D11Texture2D* image, const D3D11_TEXTURE2D_DESC& d);
void shoot(ID3D11Texture2D* image, int chain, uint32_t index, bool after, bool scaled, const wchar_t* tag = nullptr);
void write_burst();

// corner_diag.cpp
void perf_line();
// Every xrEndFrame: a removed GPU device logged once, with its reason and the VRAM (read every 90 frames).
void device_watch();
void device_watch_reset();  // release_all: the next install reads its own adapter and peak
void poll_diag_keys();
void poll_diag_hotkeys();

}  // namespace scs::detail

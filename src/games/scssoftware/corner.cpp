#include "corner.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "../../dev_build.h"
#include "../../engine/context_hooks.h"
#include "../../engine/nvapi_stereo.h"
#include "../lmu/mvr_probe.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "corner_rules.h"
#include "pass_probe.h"

namespace scs {
namespace {

using Microsoft::WRL::ComPtr;
namespace ch = simqvfr::context_hooks;

constexpr UINT kMaxVectors = 4096;  // a constant buffer's most (64 KB)
constexpr UINT kMaxViews = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

// A large constant buffer of the game's (each written once at the frame's start: ats_frame5585.rdc) and our copy with
// every eye-shaped pair rewritten, bound in its place for the rest of the frame.
constexpr UINT kMirrorMin = 256 * 1024;
struct Mirror {
    ComPtr<ID3D11Buffer> game, copy;
    UINT bytes = 0;
    uint64_t refreshedFrame = ~0ull;
    int id = -1;           // its slot in State::mirrorIds (-1: none free: another thread's writes go unseen)
    uint32_t seenGen = 0;  // State::mirrorGen[id] as of the copy
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
    // buffers' identities are kept here for the other threads to compare with.
    static constexpr int kMirrorIds = 16;
    std::atomic<ID3D11Resource*> mirrorIds[kMirrorIds] = {};
    std::atomic<uint32_t> mirrorGen[kMirrorIds] = {};
    std::atomic<int> mirrorIdCount{0};
    std::atomic<bool> activeNow{false};  // the eyes in the corner (g.active, for the other threads)
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
        ComPtr<ID3D11Texture2D> game, twin;            // the game's (kept alive with its twin)
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
} g;

constexpr int kBurstFrames = 20;

constexpr char kBlit[] = R"(
Texture2D src : register(t0);
SamplerState s : register(s0);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(O i) : SV_Target { return src.SampleLevel(s, i.uv, 0); }
// A float target for a burst's 8-bit shot: x / (1 + x), so values over 1 keep their differences.
float4 squash(O i) : SV_Target {
    float3 c = max(src.SampleLevel(s, i.uv, 0).rgb, 0);
    return float4(c / (1 + c), 1);
}
// The focus view over the box: a rounded box, its edge blended into the eye's picture, an outline if asked for.
// `look`: the blend's width and the corner radius in units of half the box's shorter side, outline on, the box's
// width over its height in pixels.
cbuffer Look : register(b1) { float4 look; float4 lookColor; };
float4 feather(O i) : SV_Target {
    float2 half = look.w >= 1 ? float2(look.w, 1) : float2(1, 1 / look.w);
    float2 q = abs(i.uv * 2 - 1) * half;
    float r = saturate(look.y);
    float2 dd = q - (half - r);
    float inside = r - (length(max(dd, 0)) + min(max(dd.x, dd.y), 0));  // from the edge inwards
    float3 c = src.SampleLevel(s, i.uv, 0).rgb;
    if (look.z > 0.5 && inside < 0.012)
        return float4(lookColor.rgb, inside >= 0 ? 1 : 0);
    return float4(c, saturate(inside / max(look.x, 0.002)));
}
)";

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
bool focus_on() { return g.focusPercent && !g.focusOff; }

void perf_line() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const uint64_t frames = g.stats.frames - g.perfFrame;
    if (g.perfSince && frames) {
        const double seconds = static_cast<double>(now.QuadPart - g.perfSince) / freq.QuadPart;
        const auto ms = [&](int b) { return g.ticks[b] * 1000.0 / freq.QuadPart / frames; };
        const auto per = [&](int b) { return static_cast<double>(g.timed[b]) / frames; };
        simqvfr::logf("scs perf: %.1f fps over %.1f s, the focus view %s; per frame on the render thread: two-view draws "
                      "%.0f in %.2f ms, replays %.0f in %.2f ms, eye rewrites %.0f in %.2f ms, mirrors %.1f in %.2f ms, "
                      "release %.1f in %.2f ms",
                      frames / seconds, seconds,
                      g.focusOff ? "OFF (Ctrl+Alt+G)"
                                 : !g.focusPercent ? "not set"
                                                   : g.instanced && (g.preferInstanced || !g.mvr) ? "on (instancing)"
                                                                                                  : g.mvr ? "on (NVIDIA's)" : "on (passes only)",
                      per(State::kTimeFocusDraws), ms(State::kTimeFocusDraws), per(State::kTimeReplays), ms(State::kTimeReplays),
                      per(State::kTimeRewrites), ms(State::kTimeRewrites), per(State::kTimeMirrors), ms(State::kTimeMirrors),
                      per(State::kTimeRelease), ms(State::kTimeRelease));
    }
    g.perfSince = now.QuadPart;
    g.perfFrame = g.stats.frames;
    for (int b = 0; b < State::kTimes; ++b)
        g.ticks[b] = 0, g.timed[b] = 0;
}

void swap_bound(bool on);
void two_views(bool on);
float beta_x();
float beta_y();
float focus_fx();
float focus_fy();
bool blit_resources();
DXGI_FORMAT srgb_view(DXGI_FORMAT f);
void shoot(ID3D11Texture2D* image, int chain, uint32_t index, bool after, bool scaled, const wchar_t* tag = nullptr);

bool eye_shaped(unsigned w, unsigned h) {
    if (!g.eyeW || w < 16 || w > g.eyeW || h > g.eyeH)
        return false;
    const double a = static_cast<double>(w) * g.eyeH, b = static_cast<double>(h) * g.eyeW;
    return std::abs(a - b) <= std::max(g.eyeW, g.eyeH);  // the same shape within a pixel of rounding
}

void size_of(ID3D11View* view, unsigned& w, unsigned& h) {
    w = h = 0;
    if (!view)
        return;
    ComPtr<ID3D11Resource> res;
    view->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (res && SUCCEEDED(res.As(&tex))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        w = d.Width;
        h = d.Height;
    }
}

void scale_viewports(UINT count, const D3D11_VIEWPORT* in, D3D11_VIEWPORT* out) {
    for (UINT i = 0; i < count; ++i) {
        out[i] = in[i];
        out[i].TopLeftX *= g.fx;
        out[i].TopLeftY *= g.fy;
        out[i].Width *= g.fx;
        out[i].Height *= g.fy;
    }
}

void scale_scissors(UINT count, const D3D11_RECT* in, D3D11_RECT* out) {
    for (UINT i = 0; i < count; ++i) {
        out[i].left = static_cast<LONG>(std::floor(in[i].left * g.fx));
        out[i].top = static_cast<LONG>(std::floor(in[i].top * g.fy));
        out[i].right = static_cast<LONG>(std::ceil(in[i].right * g.fx));
        out[i].bottom = static_cast<LONG>(std::ceil(in[i].bottom * g.fy));
    }
}

// The viewports and scissors as they should be now: cut or the game's own (inside an observer: straight through).
void apply_views() {
    D3D11_VIEWPORT vp[kMaxViews];
    D3D11_RECT sc[kMaxViews];
    const bool cutNow = g.active && g.cut;
    if (g.viewportCount) {
        if (cutNow)
            scale_viewports(g.viewportCount, g.viewports, vp);
        g.context->RSSetViewports(g.viewportCount, cutNow ? vp : g.viewports);
    }
    if (g.scissorCount) {
        if (cutNow)
            scale_scissors(g.scissorCount, g.scissors, sc);
        g.context->RSSetScissorRects(g.scissorCount, cutNow ? sc : g.scissors);
    }
}

void exact_fraction() {
    // Whole pixels of the eye's own targets on each axis.
    g.fx = std::max(1.0f, std::round(g.percent / 100.0f * g.eyeW)) / static_cast<float>(g.eyeW);
    g.fy = std::max(1.0f, std::round(g.percent / 100.0f * g.eyeH)) / static_cast<float>(g.eyeH);
    simqvfr::logf("scs corner: eye targets %ux%u, the eye drawn into their top-left %ux%u (%.4f x %.4f)", g.eyeW, g.eyeH,
                  static_cast<unsigned>(std::lround(g.fx * g.eyeW)), static_cast<unsigned>(std::lround(g.fy * g.eyeH)),
                  g.fx, g.fy);
}

// Twins and twin views no draw has used for kEvictFrames frames, let go: the game made new targets (a menu, a loading
// pause: ETS2 2026-11-15, 11 twins and 112 MB grew to 33 and 337 MB), and a twin view holds the game's old texture
// alive with it. Used ones are touched every frame (twin_of, twin_view, mark_bound_written).
constexpr uint64_t kEvictFrames = 900;
void evict_unused() {
    const uint64_t now = g.stats.frames;
    const auto stale = [now](uint64_t used) { return now - used > kEvictFrames; };
    size_t views = 0, twins = 0;
    for (auto it = g.twinViews.begin(); it != g.twinViews.end();)
        if (stale(it->second.usedFrame)) {
            it = g.twinViews.erase(it);
            ++views;
        } else {
            ++it;
        }
    for (auto it = g.texes.begin(); it != g.texes.end();)
        if (stale(it->second.usedFrame)) {
            g.twinBytes -= std::min(g.twinBytes, it->second.bytes);
            twins += it->second.twin ? 1 : 0;
            it = g.texes.erase(it);
        } else {
            ++it;
        }
    if (views || twins)
        simqvfr::logf("scs focus: %zu twins and %zu twin views unused for %llu frames let go (%zu twins left, %.0f MB at "
                      "most)",
                      twins, views, static_cast<unsigned long long>(kEvictFrames), g.texes.size(),
                      g.twinBytes / 1048576.0);
}

void take_frame_end() {
    const uint64_t ends = g.endFrames.load(std::memory_order_acquire);
    if (ends == g.seenEndFrames)
        return;
    g.seenEndFrames = ends;
    ++g.stats.frames;
    if (g.stats.frames % kEvictFrames == 0)
        evict_unused();
    g.stats.cornerFrames += g.thisFrameCornered ? 1 : 0;
    g.thisFrameCornered = false;
    g.gbufferPasses = 0;
    g.finalPasses = 0;
    g.stage = 0;
    g.blits = 0;
    g.postPhase = false;
    g.remapEye = -1;
    g.frameImage = -1;
    if (const int want = g.livePercent.load(std::memory_order_relaxed); want > 0 && g.eyeW) {  // the menu's sizes
        const int wantFocus = g.focusPercent ? std::clamp(g.liveFocusPercent.load(std::memory_order_relaxed), 10, 60) : 0;
        int wantY = g.liveFocusPercentY.load(std::memory_order_relaxed);
        wantY = wantFocus ? std::clamp(wantY > 0 ? wantY : wantFocus, 10, 80) : 0;
        // The focus region is box x density of the targets: it keeps what it asks for first, up to the room beside a
        // 25% corner and the targets' height; the corner takes what the region leaves.
        float density = std::clamp(g.liveDensity.load(std::memory_order_relaxed), 25, 400) / 100.0f;
        if (wantFocus)
            density = std::min({density, 75.0f / wantFocus, 100.0f / wantY});
        const int room = wantFocus ? static_cast<int>(std::floor(100 - wantFocus * density)) : 99;
        const int corner = std::clamp(want, 25, std::max(25, room));
        if (corner != static_cast<int>(std::lround(g.percent)) || wantFocus != g.focusPercent || wantY != g.focusPercentY ||
            density != g.density) {
            simqvfr::logf("scs corner: live: the eye's corner %d%% (was %.0f%%), the focus box %d%% x %d%% at %.0f%% density "
                          "(was %d%% x %d%% at %.0f%%)%s",
                          corner, g.percent, wantFocus, wantY, density * 100, g.focusPercent, g.focusPercentY, g.density * 100,
                          corner != want ? ": the corner cut to what fits beside the focus region" : "");
            g.percent = static_cast<float>(corner);
            const float oldFx = g.focusPercent ? focus_fx() : 0, oldFy = g.focusPercent ? focus_fy() : 0;
            g.focusPercent = wantFocus;
            g.focusPercentY = wantY;
            g.density = density;
            if (wantFocus && (focus_fx() != oldFx || focus_fy() != oldFy)) {  // the twins are the old region's size
                g.texes.clear();
                g.twinViews.clear();
                g.twinBytes = 0;
                g.stretches.clear();  // and the scratches sized for them (else each slider step adds a set)
                g.crops.clear();
            }
            exact_fraction();
        }
    }
    if (g.focusPercent) {  // this frame's boxes
        for (int e = 0; e < 2; ++e)
            for (int a = 0; a < 2; ++a) {
                const float beta = a ? beta_y() : beta_x();
                g.prevBox[e][a] = g.box[e][a];
                const float centre = g.gaze[e][a].load(std::memory_order_relaxed);
                const float edge = std::min(std::max(centre - beta / 2, 0.0f), 1 - beta);
                // Whole texels of the full-size targets, so the box's edges fall between the same texels in every pass.
                const float size = static_cast<float>(a ? g.eyeH : g.eyeW);
                const float snapped = size > 0 ? std::round(edge * size) / size : edge;
                g.boxMoves += snapped != g.box[e][a] ? 1 : 0;
                g.box[e][a] = snapped;
            }
        float jump = 0;
        for (int e = 0; e < 2; ++e)
            for (int a = 0; a < 2; ++a)
                jump = std::max(jump, std::fabs(g.box[e][a] - g.prevBox[e][a]));
        if (jump > 0.08f && g.jumpBursts > 0 && g.burstFramesLeft == 0 && g.shots.empty()) {
            --g.jumpBursts;
            g.burstFramesLeft = kBurstFrames;
            g.burstStartFrame = g.stats.frames;
            simqvfr::logf("scs focus: the box jumped %.2f of the screen (view 0 %.3f,%.3f -> %.3f,%.3f): recording the next "
                          "%d frames (%d more armed)", jump, g.prevBox[0][0], g.prevBox[0][1], g.box[0][0], g.box[0][1],
                          kBurstFrames, g.jumpBursts);
        }
    }
    g.gbufferTex.Reset();
    if (g.active) {
        g.active = false;
        g.activeNow.store(false, std::memory_order_relaxed);
        two_views(false);
        apply_views();      // the next frame's shared work at the game's own sizes
        swap_bound(false);  // and the game's own constant buffers
    }
    g.way.store(!g.focusPercent || (!g.mvr && !g.instanced) || g.scaleMismatch ? 0
                : g.focusOff ? 3
                : g.instanced && (g.preferInstanced || !g.mvr) ? 2 : 1,
                std::memory_order_relaxed);
    if (g.stats.frames % 450 == 0)
        perf_line();
    if (g.stats.frames % 900 == 0 && g.doubled)
        simqvfr::logf("scs focus: instancing: %llu draws doubled so far, %llu of them with a per-instance layout's twin",
                      static_cast<unsigned long long>(g.doubled), static_cast<unsigned long long>(g.doubledLayouts));
    if (g.stats.frames % 900 == 0)
        simqvfr::logf("scs corner: %llu frames, %llu with the eyes in the corner; draws rewritten %llu, nothing to rewrite "
                      "%llu; other targets skipped %llu; mirrors %zu, refreshed %llu times, written after their copy %llu, bound sets swapped %llu; eye images scaled back %llu, cut instead %llu, images acquired ahead %llu; focus draws %llu, without a twin %llu, passes replayed %llu, twins %zu (%.0f MB at most), region copies %llu, taken from the eye %llu, eye images composed %llu; boxes at %.3f,%.3f and %.3f,%.3f (moved %llu times); rewrites without the shader's declared constants %llu",
                      static_cast<unsigned long long>(g.stats.frames), static_cast<unsigned long long>(g.stats.cornerFrames),
                      static_cast<unsigned long long>(g.stats.rewritten), static_cast<unsigned long long>(g.stats.notRewritten),
                      static_cast<unsigned long long>(g.stats.skipped),
                      g.mirrors.size(), static_cast<unsigned long long>(g.mirrorRefreshes),
                      static_cast<unsigned long long>(g.midFrameWrites), static_cast<unsigned long long>(g.mirrorSwaps),
                      static_cast<unsigned long long>(g.upscaled), static_cast<unsigned long long>(g.upscaleMisses),
                      static_cast<unsigned long long>(g.heldTwo), static_cast<unsigned long long>(g.focusDraws),
                      static_cast<unsigned long long>(g.noTwin), static_cast<unsigned long long>(g.replays), g.twinViews.size(),
                      g.twinBytes / 1048576.0, static_cast<unsigned long long>(g.twinCopies),
                      static_cast<unsigned long long>(g.stretched), static_cast<unsigned long long>(g.composed), g.box[0][0], g.box[0][1],
                      g.box[1][0], g.box[1][1], static_cast<unsigned long long>(g.boxMoves),
                      static_cast<unsigned long long>(g.undeclared));
    if (g.stats.frames % 900 == 0 && g.focusPercent) {
        char table[64] = "";
        for (int i = 0; i < State::kOrderImages; ++i)
            if (g.firstEyeByImage[i] >= 0) {
                const size_t n = strlen(table);
                snprintf(table + n, sizeof(table) - n, " %d:%s", i, g.firstEyeByImage[i] ? "R" : "L");
            }
        simqvfr::logf("scs focus: the eye order: from the headset image %llu frames, from the frame before's %llu; first "
                      "eye by image index%s; conflicts %llu; frames whose first scene was drawn for the other eye %llu",
                      static_cast<unsigned long long>(g.orderHits), static_cast<unsigned long long>(g.orderMisses),
                      table[0] ? table : " (none yet)", static_cast<unsigned long long>(g.orderConflicts),
                      static_cast<unsigned long long>(g.orderChanges));
    }
    if (const DWORD w = g.otherWriter.load(std::memory_order_relaxed); w && !g.otherWriterLogged) {
        g.otherWriterLogged = true;
        simqvfr::logf("scs corner: thread %lu writes a mirrored constant buffer (the eyes are drawn on %lu): our copy can be "
                      "older than the game's buffer for the rest of that frame",
                      static_cast<unsigned long>(w), static_cast<unsigned long>(g.thread));
    }
    if (g.stats.frames % 900 == 0 && !g.mirrors.empty())
        simqvfr::logf("scs corner: mirrors %s (Ctrl+Alt+N); writes to them from other threads %llu, %llu with the eyes in "
                      "the corner",
                      g.mirrorsOff ? "OFF" : "on", static_cast<unsigned long long>(g.otherWrites.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g.otherWritesActive.load(std::memory_order_relaxed)));
}

// The frame's first scene pass begins (its G-buffer bind): the order learned for the headset image being drawn.
void scene_eye() {
    if (g.gbufferPasses != 1)
        return;
    const int image = g.acquiredIndex.load(std::memory_order_acquire);
    g.frameImage = image >= 0 && image < State::kOrderImages ? image : -1;
    const int first = g.frameImage >= 0 ? g.firstEyeByImage[g.frameImage] : -1;
    if (first < 0) {
        ++g.orderMisses;
        return;
    }
    ++g.orderHits;
    g.passEye[0] = first;
    g.passEye[1] = 1 - first;
}

// The frame's first final blit went to `eye`: the order of this frame's image index.
void learn_order(int eye) {
    if (g.frameImage < 0 || eye < 0)
        return;
    int& known = g.firstEyeByImage[g.frameImage];
    if (known >= 0 && known != eye && ++g.orderConflicts <= 8)
        simqvfr::logf("scs focus: headset image %d drawn with the %s eye first, before with the %s (%llu such)",
                      g.frameImage, eye ? "right" : "left", known ? "right" : "left",
                      static_cast<unsigned long long>(g.orderConflicts));
    known = eye;
    if (!g.orderLogged) {
        int learned = 0;
        for (int i : g.firstEyeByImage)
            learned += i >= 0 ? 1 : 0;
        if (learned >= 3) {
            g.orderLogged = true;
            simqvfr::log_line("scs focus: each frame's eye order now from the headset image it draws (learned from the "
                              "final blits)");
        }
    }
}

void on_targets(ID3D11DeviceContext*, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    const DWORD self = GetCurrentThreadId();
    if (g.thread && self != g.thread)
        return;
    PassTracker::Bind b;
    b.depth = dsv != nullptr;
    for (UINT i = 0; i < count && rtvs; ++i)
        b.targets += rtvs[i] ? 1 : 0;
    unsigned w = 0, h = 0;
    size_of(count && rtvs && rtvs[0] ? static_cast<ID3D11View*>(rtvs[0]) : dsv, w, h);
    b.width = w;
    b.height = h;
    const bool gbuffer = PassTracker::gbuffer_shape(b);
    if (!g.thread) {
        if (!gbuffer)
            return;
        g.thread = self;
        g.seenEndFrames = g.endFrames.load(std::memory_order_acquire);
        simqvfr::logf("scs corner: following thread %lu (its first G-buffer bind)", static_cast<unsigned long>(self));
    }
    take_frame_end();
    if (gbuffer && w * h >= g.eyeW * g.eyeH && (w != g.eyeW || h != g.eyeH)) {
        g.eyeW = w;
        g.eyeH = h;
        exact_fraction();
        unsigned imageW = 0, imageH = 0;
        {
            std::lock_guard lock(g.frameMutex);
            for (const State::Chain& c : g.chains)
                if (!c.images.empty() && c.images[0]) {
                    D3D11_TEXTURE2D_DESC d;
                    c.images[0]->GetDesc(&d);
                    imageW = d.Width;
                    imageH = d.Height;
                    break;
                }
        }
        // Within a few texels (ATS: 5192 against the recommended 5190).
        const bool mismatch = imageW && (imageW > w + 8 || w > imageW + 8 || imageH > h + 8 || h > imageH + 8);
        if (mismatch != g.scaleMismatch || mismatch)
            simqvfr::logf("scs corner: the eye's targets are %ux%u, the headset's images %ux%u: %s", w, h, imageW, imageH,
                          mismatch ? "the game's Scaling isn't 100%; QVFR stays OFF until it is (set Scaling to 100% in the "
                                     "game's graphics settings)"
                                   : "the same size");
        g.scaleMismatch = mismatch;
    }
    const bool eyeGbuffer = gbuffer && w == g.eyeW && h == g.eyeH;
    if (g.gbufferTex) {  // the G-buffer pass ends at this bind (unless it binds the same targets again)
        ComPtr<ID3D11Resource> res;
        if (eyeGbuffer)
            rtvs[0]->GetResource(&res);
        if (res.Get() != g.gbufferTex.Get()) {
            if (g.burstFramesLeft > 0) {
                wchar_t tag[32];
                swprintf_s(tag, L"gbuffer-pass%d", g.gbufferPasses);
                shoot(g.gbufferTex.Get(), 0, 0, false, false, tag);
            }
            g.gbufferTex.Reset();
        }
    }
    if (eyeGbuffer && !g.gbufferTex && g.focusPercent) {
        ComPtr<ID3D11Resource> res;
        rtvs[0]->GetResource(&res);
        res.As(&g.gbufferTex);
        ++g.gbufferPasses;
        g.postPhase = false;
        scene_eye();
    }
    g.boundMarked = false;
    g.boundSwapchain = false;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        g.boundRt[i] = nullptr;
        if (g.focusPercent && rtvs && i < count && rtvs[i]) {
            ComPtr<ID3D11Resource> res;
            rtvs[i]->GetResource(&res);
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            res->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)  // a 2D texture's (compared, and its desc read by the burst)
                g.boundRt[i] = static_cast<ID3D11Texture2D*>(res.Get());
        }
    }
    g.boundChainEye = -1;
    if (g.focusPercent && g.boundRt[0]) {
        std::lock_guard lock(g.frameMutex);
        for (size_t i = 0; i < g.chains.size(); ++i)
            for (ID3D11Texture2D* t : g.chains[i].images)
                if (t == g.boundRt[0]) {
                    g.boundSwapchain = true;
                    g.boundChainEye = g.chains[i].eye >= 0 ? g.chains[i].eye : static_cast<int>(i & 1);
                }
    }
    if (g.active && w > 0 && w * 3 < g.eyeW)
        g.postPhase = true;  // a quarter-size target or smaller: the exposure and bloom chain, past the eye's scene
    g.depthBound = dsv != nullptr;
    g.fullSize = g.eyeW && w == g.eyeW && h == g.eyeH;
    g.viewsDual = false;
    if (!(g.depthBound && g.fullSize))
        two_views(false);
    const bool wasCut = g.active && g.cut;
    if (gbuffer && w == g.eyeW && h == g.eyeH && !g.active && !g.scaleMismatch) {
        g.active = true;
        g.thisFrameCornered = true;
        g.cornerNow.store(true, std::memory_order_release);
        g.activeNow.store(true, std::memory_order_relaxed);
        if (!g.mirrorsOff)
            swap_bound(true);  // the binds made before this one, to our rewritten copies
    }
    g.boundW = w;
    g.boundH = h;
    g.cut = eye_shaped(w, h);
    g.smallTarget = w > 0 && w <= 64 && h <= 64;
    if ((g.active && g.cut) != wasCut || (g.active && g.cut))
        apply_views();
}

bool on_viewports(ID3D11DeviceContext*, UINT count, const D3D11_VIEWPORT* in, D3D11_VIEWPORT* out) {
    if (g.thread && GetCurrentThreadId() != g.thread)
        return false;
    g.viewportCount = std::min(count, kMaxViews);
    g.viewsDual = false;
    if (in)
        std::memcpy(g.viewports, in, g.viewportCount * sizeof(D3D11_VIEWPORT));
    if (!(g.active && g.cut) || !in)
        return false;
    scale_viewports(g.viewportCount, in, out);
    return true;
}

bool on_scissors(ID3D11DeviceContext*, UINT count, const D3D11_RECT* in, D3D11_RECT* out) {
    if (g.thread && GetCurrentThreadId() != g.thread)
        return false;
    g.scissorCount = std::min(count, kMaxViews);
    g.viewsDual = false;
    if (in)
        std::memcpy(g.scissors, in, g.scissorCount * sizeof(D3D11_RECT));
    if (!(g.active && g.cut) || !in)
        return false;
    scale_scissors(g.scissorCount, in, out);
    return true;
}

Mirror* mirror_of_game(ID3D11Buffer* b) {
    for (Mirror& m : g.mirrors)
        if (m.game.Get() == b)
            return &m;
    return nullptr;
}
// The game's buffer behind one of our copies (or `b` itself).
ID3D11Buffer* game_of(ID3D11Buffer* b) {
    for (Mirror& m : g.mirrors)
        if (m.copy.Get() == b)
            return m.game.Get();
    return b;
}

bool big_scratch(UINT bytes) {
    if (bytes <= g.bigBytes)
        return true;
    g.bigIn.Reset(), g.bigOut.Reset(), g.bigInView.Reset(), g.bigOutView.Reset();
    const D3D11_BUFFER_DESC in = {bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                  D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    D3D11_BUFFER_DESC out = in;
    out.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx = {0, bytes / 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer = {0, bytes / 4, D3D11_BUFFER_UAV_FLAG_RAW};
    if (FAILED(g.device->CreateBuffer(&in, nullptr, &g.bigIn)) || FAILED(g.device->CreateBuffer(&out, nullptr, &g.bigOut)) ||
        FAILED(g.device->CreateShaderResourceView(g.bigIn.Get(), &sv, &g.bigInView)) ||
        FAILED(g.device->CreateUnorderedAccessView(g.bigOut.Get(), &uv, &g.bigOutView)))
        return false;
    g.bigBytes = bytes;
    return true;
}

// Runs `cs` from `in` to `out` with `params` (64 bytes), the game's compute state kept and put back.
void run_cs(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out, const void* params,
            UINT groups) {
    ComPtr<ID3D11ComputeShader> gameCs;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> cb;
    g.context->CSGetShader(&gameCs, nullptr, nullptr);
    g.context->CSGetShaderResources(0, 1, &srv);
    g.context->CSGetUnorderedAccessViews(0, 1, &uav);
    g.context->CSGetConstantBuffers(0, 1, &cb);
    g.context->UpdateSubresource(g.params.Get(), 0, nullptr, params, 0, 0);
    ID3D11ShaderResourceView* const ins[] = {in};
    ID3D11UnorderedAccessView* const outs[] = {out};
    ID3D11Buffer* const ps[] = {g.params.Get()};
    g.context->CSSetShader(cs, nullptr, 0);
    g.context->CSSetShaderResources(0, 1, ins);
    g.context->CSSetUnorderedAccessViews(0, 1, outs, nullptr);
    g.context->CSSetConstantBuffers(0, 1, ps);
    g.context->Dispatch(groups, 1, 1);
    ID3D11ShaderResourceView* const s0[] = {srv.Get()};
    ID3D11UnorderedAccessView* const u0[] = {uav.Get()};
    ID3D11Buffer* const c0[] = {cb.Get()};
    g.context->CSSetShader(gameCs.Get(), nullptr, 0);
    g.context->CSSetShaderResources(0, 1, s0);
    g.context->CSSetUnorderedAccessViews(0, 1, u0, nullptr);
    g.context->CSSetConstantBuffers(0, 1, c0);
}

// This frame's copy of a mirrored buffer: the game's data copied and every eye-shaped pair rewritten. Copied again
// after the game writes it: on the drawing thread (on_unmap clears refreshedFrame) or on another (its generation).
void refresh(Mirror& m) {
    const uint32_t gen = m.id >= 0 ? g.mirrorGen[m.id].load(std::memory_order_acquire) : 0;
    if ((m.refreshedFrame == g.stats.frames && gen == m.seenGen) || !big_scratch(m.bytes))
        return;
    m.refreshedFrame = g.stats.frames;
    m.seenGen = gen;
    const Timed timed(State::kTimeMirrors);
    const D3D11_BOX box = {0, 0, 0, m.bytes, 1, 1};
    g.context->CopySubresourceRegion(g.bigIn.Get(), 0, 0, 0, 0, m.game.Get(), 0, &box);
    const struct {
        float fx, fy;
        UINT count, pad;
        float w, h, p0, p1;
        float unused[8];
    } p = {g.fx, g.fy, m.bytes / 16, 0, static_cast<float>(g.eyeW), static_cast<float>(g.eyeH), 0, 0, {}};
    static_assert(sizeof(p) == 64, "run_cs's params are 64 bytes");
    run_cs(g.mirrorCs.Get(), g.bigInView.Get(), g.bigOutView.Get(), &p, (m.bytes / 16 + 63) / 64);
    g.context->CopySubresourceRegion(m.copy.Get(), 0, 0, 0, 0, g.bigOut.Get(), 0, &box);
    ++g.mirrorRefreshes;
}

// A large constant buffer seen for the first time: our copy made.
Mirror* learn(ID3D11Buffer* b) {
    if (!b)
        return nullptr;
    if (Mirror* m = mirror_of_game(b))
        return m;
    if (game_of(b) != b)
        return nullptr;  // one of ours
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    if (d.ByteWidth < kMirrorMin || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
        return nullptr;
    Mirror m;
    m.game = b;
    m.bytes = d.ByteWidth & ~15u;
    const D3D11_BUFFER_DESC cd = {m.bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(g.device->CreateBuffer(&cd, nullptr, &m.copy))) {
        simqvfr::logf("scs corner: a copy of the game's %u-byte constant buffer couldn't be made", d.ByteWidth);
        return nullptr;
    }
    if (const int n = g.mirrorIdCount.load(std::memory_order_relaxed); n < State::kMirrorIds) {
        m.id = n;
        g.mirrorGen[n].store(0, std::memory_order_relaxed);
        g.mirrorIds[n].store(b, std::memory_order_relaxed);
        g.mirrorIdCount.store(n + 1, std::memory_order_release);
    }
    g.mirrors.push_back(m);
    simqvfr::logf("scs corner: the game's constant buffer %p (%u bytes, usage %u) mirrored (%zu so far)",
                  static_cast<void*>(b), d.ByteWidth, static_cast<unsigned>(d.Usage), g.mirrors.size());
    return &g.mirrors.back();
}

// The vertex and pixel stages' bound constant buffers swapped to our copies (on) or back to the game's (off), ranges kept.
void swap_bound(bool on) {
    constexpr UINT n = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    for (int stage = 0; stage < 2; ++stage) {
        ID3D11Buffer* bufs[n] = {};
        UINT first[n] = {}, num[n] = {};
        if (stage)
            g.context->PSGetConstantBuffers1(0, n, bufs, first, num);
        else
            g.context->VSGetConstantBuffers1(0, n, bufs, first, num);
        bool changed = false;
        ID3D11Buffer* next[n];
        for (UINT i = 0; i < n; ++i) {
            next[i] = bufs[i];
            if (!bufs[i])
                continue;
            if (on) {
                if (Mirror* m = learn(bufs[i])) {
                    refresh(*m);
                    next[i] = m->copy.Get();
                }
            } else {
                next[i] = game_of(bufs[i]);
            }
            changed |= next[i] != bufs[i];
        }
        if (changed) {
            if (stage)
                g.context->PSSetConstantBuffers1(0, n, next, first, num);
            else
                g.context->VSSetConstantBuffers1(0, n, next, first, num);
            ++g.mirrorSwaps;
        }
        for (ID3D11Buffer* b : bufs)
            if (b)
                b->Release();
    }
}

// The game wrote a buffer: a mirrored one is copied again at its next bind (its pool's buffers can be filled mid-frame,
// after this frame's copy was made: the stale copy flickered on the cab roof, S1 run 2). From another thread: through
// the buffer's generation (refresh), since the mirror list is the drawing thread's.
void on_unmap(ID3D11DeviceContext*, ID3D11Resource* resource, UINT) {
    if (const DWORD self = GetCurrentThreadId(); self != g.thread) {
        const int n = g.mirrorIdCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (g.mirrorIds[i].load(std::memory_order_relaxed) == resource) {
                g.mirrorGen[i].fetch_add(1, std::memory_order_release);
                g.otherWrites.fetch_add(1, std::memory_order_relaxed);
                if (g.activeNow.load(std::memory_order_relaxed))
                    g.otherWritesActive.fetch_add(1, std::memory_order_relaxed);
                g.otherWriter.store(self, std::memory_order_relaxed);
                break;
            }
        return;
    }
    for (Mirror& m : g.mirrors)
        if (static_cast<ID3D11Resource*>(m.game.Get()) == resource) {
            if (g.active && m.refreshedFrame == g.stats.frames)
                ++g.midFrameWrites;
            m.refreshedFrame = ~0ull;
            return;
        }
}

bool on_constant_buffers(ID3D11DeviceContext*, bool, UINT, UINT count, ID3D11Buffer* const* in, ID3D11Buffer** out) {
    if (!g.active || g.mirrorsOff || GetCurrentThreadId() != g.thread)
        return false;
    bool changed = false;
    for (UINT i = 0; i < count; ++i) {
        out[i] = in[i];
        if (Mirror* m = learn(in[i])) {
            refresh(*m);
            out[i] = m->copy.Get();
            changed = true;
        }
    }
    return changed;
}

// The eye the pass being drawn belongs to: a headset image's own; in an eye's scene the pass's (scene_eye: the order
// learned for this frame's headset image); past it (the post-processing) the eye whose final blit is next.
int current_eye() {
    if (g.boundSwapchain && g.boundChainEye >= 0)
        return g.boundChainEye;
    return g.passEye[std::min(std::max(g.postPhase ? g.blits : g.gbufferPasses - 1, 0), 1)];
}

// The clip remap of a box: the focus view's clip position from the eye's (x' = a x + b w, y' = c y + d w).
void box_remap(const float (&box)[2], float (&out)[4]) {
    const float bx = beta_x(), by = beta_y();
    out[0] = 1 / bx;
    out[1] = (1 - 2 * box[0] - bx) / bx;
    out[2] = 1 / by;
    out[3] = (2 * box[1] + by - 1) / by;
}

// The box's shares of the eye's screen.
float beta_x() { return g.focusPercent / 100.0f; }
float beta_y() { return (g.focusPercentY > 0 ? g.focusPercentY : g.focusPercent) / 100.0f; }
// The focus region's shares of a full-size target: the box at the focus view's density, in whole texels.
float focus_fx() { return std::max(1.0f, std::round(beta_x() * g.density * g.eyeW)) / static_cast<float>(g.eyeW); }
float focus_fy() { return std::max(1.0f, std::round(beta_y() * g.density * g.eyeH)) / static_cast<float>(g.eyeH); }

// A twin holds the focus region at its origin and a little of the eye's picture around it, no more: 4% past the region
// on each axis, in multiples of 8 texels at full size so the half- and eighth-size textures' twins keep the same share.
// (At the game's own size the 34 twins of a 5192x4168 eye took up to 4.3 GB.)
unsigned twin_full(unsigned eye, float focus) {
    const unsigned n = (static_cast<unsigned>(std::ceil(focus * eye * 1.04f)) + 7) / 8 * 8;
    return std::min(eye, std::max(8u, n));
}
unsigned twin_w(unsigned w) {
    return std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(twin_full(g.eyeW, focus_fx())) * w / g.eyeW)));
}
unsigned twin_h(unsigned h) {
    return std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(twin_full(g.eyeH, focus_fy())) * h / g.eyeH)));
}
// The focus region's share of a twin (what focus_fx is of a texture of the game's).
float twin_fx() { return focus_fx() * g.eyeW / twin_full(g.eyeW, focus_fx()); }
float twin_fy() { return focus_fy() * g.eyeH / twin_full(g.eyeH, focus_fy()); }

// The game's slot-0 range of one stage rewritten into `ours` (the GPU's copy) and bound in its place for this draw
// (put_back undoes it). False: left as the game bound it.
bool rewrite(Range& r, ID3D11Buffer* ours, bool pixel, bool focus = false) {
    ID3D11Buffer* buf = nullptr;
    UINT first = 0, count = 0;
    if (pixel)
        g.context->PSGetConstantBuffers1(0, 1, &buf, &first, &count);
    else
        g.context->VSGetConstantBuffers1(0, 1, &buf, &first, &count);
    r.game.Attach(buf);
    r.first = first;
    r.count = count;
    r.ours = false;
    if (!buf)
        return false;
    D3D11_BUFFER_DESC d;
    buf->GetDesc(&d);
    const UINT total = d.ByteWidth / 16;
    if (count == 0) {  // a plain bind: the whole buffer
        first = 0, count = total;
    } else if (first + count > total) {  // a range bound past the buffer's end (it reads zeros there): what is in it
        if (first >= total) {
            r = Range{};
            return false;
        }
        count = total - first;
    }
    count = std::min(count, kMaxVectors);
    if (!count)
        return false;
    const D3D11_BOX box = {first * 16, 0, 0, (first + count) * 16, 1, 1};
    g.context->CopySubresourceRegion(g.rawIn.Get(), 0, 0, 0, 0, game_of(buf), 0, &box);  // the game's own data
    // The rules read only what the shader declares: a range is bound in blocks of 16 vectors, and past the shader's own
    // lie the pooled buffer's leftovers (S5 run 4: a leftover matrix pair there had the focus rule rewrite the depth of
    // field's weights: far terrain black for single frames).
    UINT declared = count;
    {
        ComPtr<ID3D11DeviceChild> shader;
        if (pixel) {
            ID3D11PixelShader* s = nullptr;
            g.context->PSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        } else {
            ID3D11VertexShader* s = nullptr;
            g.context->VSGetShader(&s, nullptr, nullptr);
            shader.Attach(s);
        }
        UINT vectors = 0, size = sizeof(vectors);
        if (shader && SUCCEEDED(shader->GetPrivateData(lmu::kCb0VectorsGuid, &size, &vectors))) {
            if (!vectors) {  // the shader reads no constants from slot 0: nothing of its to rewrite
                r = Range{};
                return false;
            }
            declared = std::min(count, vectors);
        } else {
            ++g.undeclared;
        }
    }
    // The focus view: the middle focusPercent of the eye's screen on each axis; its clip is the eye's scaled about the
    // centre (the remap the scene draws' twins use).
    const int eye = current_eye();
    struct {
        float fx, fy;
        UINT count, flags;
        float box[4], remap[4];
        float px, py, pad[2];
    } p = {focus ? twin_fx() : g.fx, focus ? twin_fy() : g.fy, declared, (pixel ? 0u : 1u) | (focus ? 2u : 0u),
           {g.box[eye][0], g.box[eye][1], beta_x(), beta_y()}, {1, 0, 1, 0}, focus ? focus_fx() : g.fx,
           focus ? focus_fy() : g.fy, {}};
    static_assert(sizeof(p) == 64, "run_cs's params are 64 bytes");
    if (focus)  // the reprojections: into the frame before's box (Ctrl+Alt+H: this frame's)
        box_remap(g.historyCurrentBox ? g.box[eye] : g.prevBox[eye], p.remap);
    run_cs(g.patchCs.Get(), g.rawInView.Get(), g.rawOutView.Get(), &p, 1);
    const D3D11_BOX outBox = {0, 0, 0, count * 16, 1, 1};
    g.context->CopySubresourceRegion(ours, 0, 0, 0, 0, g.rawOut.Get(), 0, &outBox);
    const UINT zero = 0, n = ((count + 15) / 16) * 16;  // ranged binds: multiples of 16 constants
    if (pixel)
        g.context->PSSetConstantBuffers1(0, 1, &ours, &zero, &n);
    else
        g.context->VSSetConstantBuffers1(0, 1, &ours, &zero, &n);
    r.ours = true;
    return true;
}

void put_back(Range& r, bool pixel) {
    if (r.ours) {
        ID3D11Buffer* const b[] = {r.game.Get()};
        if (r.count)
            pixel ? g.context->PSSetConstantBuffers1(0, 1, b, &r.first, &r.count)
                  : g.context->VSSetConstantBuffers1(0, 1, b, &r.first, &r.count);
        else
            pixel ? g.context->PSSetConstantBuffers(0, 1, b) : g.context->VSSetConstantBuffers(0, 1, b);
    }
    r = Range{};
}

// The bound targets' focus regions are being written (a two-view scene draw, a replay).
void mark_bound_written() {
    if (g.boundMarked)
        return;
    g.boundMarked = true;
    for (ID3D11Texture2D* t : g.boundRt)
        if (t) {
            State::Tex& x = g.texes[t];
            ++x.writeSerial;
            x.writeFrame = g.stats.frames;
            x.usedFrame = g.stats.frames;
        }
}

// The focus view's rectangle in a w x h texture of the eye's shape.
D3D11_RECT focus_rect_in(unsigned w, unsigned h) {
    const LONG x = static_cast<LONG>(std::lround(g.fx * w));
    const LONG fw = std::min<LONG>(std::lround(focus_fx() * w), static_cast<LONG>(w) - x);
    const LONG fh = std::min<LONG>(std::lround(focus_fy() * h), static_cast<LONG>(h));
    return {x, 0, x + fw, fh};
}

// A texture with no focus content of its own (the bloom: one chain for the whole eye): its twin's region filled from the
// eye's image of the box, magnified, so the focus view takes the eye's (bloom, anything else of the eye's alone).
// Float and normalised formats only. The game's state is put back.
// `extent`: how far the fill goes, in focus views (1: the focus region alone; more: the eye's picture of what lies right
// of and below the box, so a pass that reaches past the region's edge finds the scene there, not nothing).
bool stretch_into_twin(State::Tex& t, ID3D11Texture2D* game, const D3D11_TEXTURE2D_DESC& d, float extent = 1) {
    switch (d.Format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: break;
        default: return false;
    }
    if (!blit_resources())
        return false;
    const float bx = beta_x(), by = beta_y();
    const float* b = g.box[current_eye()];
    const UINT x = static_cast<UINT>(std::lround(g.fx * d.Width * b[0])), y = static_cast<UINT>(std::lround(g.fy * d.Height * b[1]));
    extent = std::min({extent, (1 - b[0]) / bx, (1 - b[1]) / by});  // no further than the eye's own edge
    {
        const D3D11_RECT r = focus_rect_in(d.Width, d.Height);  // nor than the twin's
        extent = std::min(extent, std::min(static_cast<float>(twin_w(d.Width)) / std::max<LONG>(1, r.right - r.left),
                                           static_cast<float>(twin_h(d.Height)) / std::max<LONG>(1, r.bottom)));
    }
    extent = std::max(1.0f, extent);
    const UINT w = std::max<UINT>(1, static_cast<UINT>(std::lround(g.fx * d.Width * bx * extent)));
    const UINT h = std::max<UINT>(1, static_cast<UINT>(std::lround(g.fy * d.Height * by * extent)));
    if (x + w > d.Width || y + h > d.Height)
        return false;
    State::Stretch* st = nullptr;
    for (State::Stretch& e : g.stretches)
        if (e.desc.Width == w && e.desc.Height == h && e.desc.Format == d.Format)
            st = &e;
    if (!st) {
        State::Stretch e;
        e.desc = {w, h, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(g.device->CreateTexture2D(&e.desc, nullptr, &e.tex)) ||
            FAILED(g.device->CreateShaderResourceView(e.tex.Get(), nullptr, &e.view)))
            return false;
        g.stretches.push_back(e);
        st = &g.stretches.back();
    }
    ID3D11DeviceContext1* c = g.context.Get();
    const D3D11_BOX box = {x, y, 0, x + w, y + h, 1};
    c->CopySubresourceRegion(st->tex.Get(), 0, 0, 0, 0, game, 0, &box);
    // The game's state this draw touches.
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> samp;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4];
    UINT sampleMask, stencilRef, vpCount = kMaxViews, scCount = kMaxViews;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11RasterizerState> raster;
    D3D11_VIEWPORT vp[kMaxViews];
    D3D11_RECT sc[kMaxViews];
    c->IAGetInputLayout(&layout);
    c->IAGetPrimitiveTopology(&topology);
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &srv);
    c->PSGetSamplers(0, 1, &samp);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    c->OMGetBlendState(&blend, blendFactor, &sampleMask);
    c->OMGetDepthStencilState(&depthState, &stencilRef);
    c->RSGetState(&raster);
    c->RSGetViewports(&vpCount, vp);
    c->RSGetScissorRects(&scCount, sc);

    const D3D11_RECT f = focus_rect_in(d.Width, d.Height);
    const float ow = (f.right - f.left) * extent, oh = f.bottom * extent;
    const D3D11_VIEWPORT out = {0, 0, ow, oh, 0, 1};
    const D3D11_RECT outRect = {0, 0, static_cast<LONG>(std::ceil(ow)), static_cast<LONG>(std::ceil(oh))};
    ID3D11RenderTargetView* target[] = {t.twinRtv.Get()};
    ID3D11ShaderResourceView* src[] = {st->view.Get()};
    ID3D11SamplerState* linear[] = {g.linear.Get()};
    c->OMSetRenderTargets(1, target, nullptr);
    c->RSSetViewports(1, &out);
    c->RSSetScissorRects(1, &outRect);
    c->RSSetState(g.raster.Get());
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.blitVs.Get(), nullptr, 0);
    c->PSSetShader(g.blitPs.Get(), nullptr, 0);
    c->PSSetShaderResources(0, 1, src);
    c->PSSetSamplers(0, 1, linear);
    c->Draw(3, 0);

    ID3D11ShaderResourceView* putSrv[] = {srv.Get()};
    ID3D11SamplerState* putSamp[] = {samp.Get()};
    c->PSSetShaderResources(0, 1, putSrv);
    c->PSSetSamplers(0, 1, putSamp);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    for (ID3D11RenderTargetView* r : rtvs)
        if (r)
            r->Release();
    if (dsv)
        dsv->Release();
    c->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
    c->OMSetDepthStencilState(depthState.Get(), stencilRef);
    c->RSSetState(raster.Get());
    if (vpCount)
        c->RSSetViewports(vpCount, vp);
    if (scCount)
        c->RSSetScissorRects(scCount, sc);
    c->IASetInputLayout(layout.Get());
    c->IASetPrimitiveTopology(topology);
    c->VSSetShader(vs.Get(), nullptr, 0);
    c->PSSetShader(ps.Get(), nullptr, 0);
    ++g.stretched;
    return true;
}

// A game texture's twin, its top-left holding the texture's focus region as last written (nothing where the region
// has no focus content: zero). Null: no twin.
State::Tex* twin_of(ID3D11Texture2D* game, const D3D11_TEXTURE2D_DESC& d) {
    State::Tex& t = g.texes[game];
    t.usedFrame = g.stats.frames;
    if (t.failed)
        return nullptr;
    if (!t.twin) {
        D3D11_TEXTURE2D_DESC td = d;
        td.Width = std::max(twin_w(d.Width), static_cast<unsigned>(focus_rect_in(d.Width, d.Height).right - focus_rect_in(d.Width, d.Height).left));
        td.Height = std::max(twin_h(d.Height), static_cast<unsigned>(focus_rect_in(d.Width, d.Height).bottom));
        for (UINT most = 1 + static_cast<UINT>(std::floor(std::log2(static_cast<double>(std::max(td.Width, td.Height)))));
             td.MipLevels > most;)
            td.MipLevels = most;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = d.MipLevels != 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = srgb_view(d.Format);
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateTexture2D(&td, nullptr, &t.twin)) ||
            FAILED(g.device->CreateRenderTargetView(t.twin.Get(), &rv, &t.twinRtv))) {
            t.twin.Reset();
            t.failed = true;
            simqvfr::logf("scs focus: no twin for a %ux%u texture of format %d (%d mips): its replays read the game's own",
                          d.Width, d.Height, static_cast<int>(d.Format), static_cast<int>(d.MipLevels));
            return nullptr;
        }
        if (td.MiscFlags)
            g.device->CreateShaderResourceView(t.twin.Get(), nullptr, &t.twinAll);
        t.game = game;
        t.mips = d.MipLevels;
        const float none[4] = {};
        g.context->ClearRenderTargetView(t.twinRtv.Get(), none);
        t.bytes = static_cast<uint64_t>(td.Width) * td.Height * 8;  // an upper estimate (16-bit RGBA)
        g.twinBytes += t.bytes;
        simqvfr::logf("scs focus: twin %zu: %ux%u for a %ux%u texture of format %d, %d mips", g.texes.size(), td.Width,
                      td.Height, d.Width, d.Height, static_cast<int>(d.Format), static_cast<int>(d.MipLevels));
    }
    const bool content = t.writeFrame != ~0ull && g.stats.frames - t.writeFrame <= 1;
    if (content && t.copiedSerial != t.writeSerial) {
        const D3D11_RECT f = focus_rect_in(d.Width, d.Height);
        const D3D11_BOX box = {static_cast<UINT>(f.left), 0, 0, static_cast<UINT>(f.right), static_cast<UINT>(f.bottom), 1};
        // First the eye's picture around the box (the AO reaches a quarter of a half-size texture past the region's
        // right and bottom edges; the other passes a few texels), then the focus region itself over its part.
        const float p = focus_fx();
        stretch_into_twin(t, game, d, ((d.Width * 2 <= g.eyeW + 2 ? 0.25f : 0.02f) + p) / p);
        g.context->CopySubresourceRegion(t.twin.Get(), 0, 0, 0, 0, game, 0, &box);
        if (t.twinAll)
            g.context->GenerateMips(t.twinAll.Get());
        t.copiedSerial = t.writeSerial;
        t.zero = false;
        ++g.twinCopies;
    } else if (!content) {
        if (stretch_into_twin(t, game, d)) {  // the eye's own, through the box (every time: the eye's changes)
            t.zero = false;
            t.copiedSerial = ~0ull;
        } else if (!t.zero) {
            const float none[4] = {};
            g.context->ClearRenderTargetView(t.twinRtv.Get(), none);
            t.zero = true;
            t.copiedSerial = ~0ull;
        }
    }
    return &t;
}

// The twin's view for a game view (the same format and mips).
ID3D11ShaderResourceView* twin_view(ID3D11ShaderResourceView* gameView, State::Tex& t) {
    State::TwinView& v = g.twinViews[gameView];
    v.usedFrame = g.stats.frames;
    if (!v.twin) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        gameView->GetDesc(&sd);
        if (FAILED(g.device->CreateShaderResourceView(t.twin.Get(), &sd, &v.twin)))
            return nullptr;
        v.game = gameView;  // kept, so the address isn't another view's later
    }
    return v.twin.Get();
}

// A burst's record of a replay: the focus region of its first target, and (the burst's first frame) a log line naming
// the pass: its target, shader and inputs.
void shoot_stage(const D3D11_RECT& f, const char* inputs) {
    const int stage = g.stage++;
    ID3D11Texture2D* target = g.boundRt[0];
    if (!target)
        return;
    D3D11_TEXTURE2D_DESC d;
    target->GetDesc(&d);
    const int eye = current_eye();
    if (g.stats.frames == g.burstStartFrame) {
        ID3D11PixelShader* ps = nullptr;
        g.context->PSGetShader(&ps, nullptr, nullptr);
        simqvfr::logf("scs focus: burst stage %02d eye %d: target %ux%u format %d, shader %p, inputs%s", stage, eye, d.Width,
                      d.Height, static_cast<int>(d.Format), static_cast<void*>(ps), inputs);
        if (ps)
            ps->Release();
    }
    if (d.SampleDesc.Count != 1)
        return;
    // The focus region, and the eye's own picture of the box (the game's draw just before): the same pass, both views.
    const float* b = g.box[std::min(std::max(eye, 0), 1)];
    const LONG ex = std::lround(g.fx * d.Width * b[0]), ey = std::lround(g.fy * d.Height * b[1]);
    const D3D11_RECT rects[2] = {f, {ex, ey, ex + std::lround(g.fx * d.Width * beta_x()), ey + std::lround(g.fy * d.Height * beta_y())}};
    const bool isFloat = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                         d.Format == DXGI_FORMAT_R16_FLOAT || d.Format == DXGI_FORMAT_R32_FLOAT ||
                         d.Format == DXGI_FORMAT_R16G16_FLOAT || d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    for (int k = 0; k < 2; ++k) {
        const D3D11_RECT& r = rects[k];
        const UINT w = static_cast<UINT>(r.right - r.left), h = static_cast<UINT>(r.bottom - r.top);
        if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top || static_cast<UINT>(r.right) > d.Width ||
            static_cast<UINT>(r.bottom) > d.Height)
            continue;
        State::Crop* crop = nullptr;
        for (State::Crop& e : g.crops)
            if (e.desc.Width == w && e.desc.Height == h && e.desc.Format == d.Format)
                crop = &e;
        if (!crop) {
            State::Crop e;
            e.desc = {w, h, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
            if (FAILED(g.device->CreateTexture2D(&e.desc, nullptr, &e.tex)))
                continue;
            g.crops.push_back(e);
            crop = &g.crops.back();
        }
        const D3D11_BOX box = {static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0, static_cast<UINT>(r.right),
                               static_cast<UINT>(r.bottom), 1};
        g.context->CopySubresourceRegion(crop->tex.Get(), 0, 0, 0, 0, target, 0, &box);
        wchar_t tag[64];
        swprintf_s(tag, L"stage-eye%d-s%02d-%ux%u-fmt%d%ls-%ls", eye, stage, d.Width, d.Height, static_cast<int>(d.Format),
                   isFloat ? L"-squashed" : L"", k ? L"eyebox" : L"focus");
        g.shootSquash = isFloat;
        shoot(crop->tex.Get(), 0, 0, false, false, tag);
        g.shootSquash = false;
    }
}

// A full-screen pass the game just drew for the eye, drawn again for the focus view: its eye-shaped inputs from their
// twins, its constants rewritten for a region at the twins' origin with the focus camera, its output on the focus
// region of the game's own targets (with the game's depth and stencil there).
void replay(UINT vertices, UINT start) {
    constexpr UINT kSlots = 16;
    ID3D11ShaderResourceView* game[kSlots] = {};
    ID3D11ShaderResourceView* bind[kSlots] = {};
    g.context->PSGetShaderResources(0, kSlots, game);
    bool swapped = false;
    const bool record = g.burstFramesLeft > 0;
    char inputs[512] = "";
    for (UINT i = 0; i < kSlots; ++i) {
        bind[i] = game[i];
        if (!game[i])
            continue;
        ComPtr<ID3D11Resource> res;
        game[i]->GetResource(&res);
        ComPtr<ID3D11Texture2D> tex;
        if (!res || FAILED(res.As(&tex)))
            continue;
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        const bool shaped = d.SampleDesc.Count == 1 && d.ArraySize == 1 && eye_shaped(d.Width, d.Height);
        const char* how = "game's";
        if (shaped)
            if (State::Tex* t = twin_of(tex.Get(), d))
                if (ID3D11ShaderResourceView* v = twin_view(game[i], *t)) {
                    bind[i] = v;
                    swapped = true;
                    how = t->zero ? "twin, zero" : t->copiedSerial == ~0ull ? "twin, the eye's stretched" : "twin, focus";
                }
        if (record && g.stats.frames == g.burstStartFrame) {
            const size_t n = strlen(inputs);
            snprintf(inputs + n, sizeof(inputs) - n, " [%u: %ux%u format %d, %s]", i, d.Width, d.Height,
                     static_cast<int>(d.Format), how);
        }
    }
    if (swapped)
        g.context->PSSetShaderResources(0, kSlots, bind);
    rewrite(g.vsFocus, g.vsCbFocus.Get(), false, true);
    rewrite(g.psFocus, g.psCbFocus.Get(), true, true);
    const D3D11_RECT f = focus_rect_in(g.boundW, g.boundH);
    const D3D11_VIEWPORT vp = {static_cast<float>(f.left), 0, static_cast<float>(f.right - f.left),
                               static_cast<float>(f.bottom), g.viewportCount ? g.viewports[0].MinDepth : 0,
                               g.viewportCount ? g.viewports[0].MaxDepth : 1};
    g.context->RSSetViewports(1, &vp);
    if (g.scissorCount)
        g.context->RSSetScissorRects(1, &f);
    g.context->Draw(vertices, start);
    if (record)
        shoot_stage(f, inputs);
    put_back(g.vsFocus, false);
    put_back(g.psFocus, true);
    if (swapped)
        g.context->PSSetShaderResources(0, kSlots, game);
    for (ID3D11ShaderResourceView* v : game)
        if (v)
            v->Release();
    g.viewsDual = false;
    apply_views();
    mark_bound_written();
    ++g.replays;
}

// NVAPI's two views on or off (the render thread). A refusal turns the focus view off for the session.
void two_views(bool on) {
    if (g.twoViews == on || !g.context)
        return;
    const int status = simqvfr::nvapi::set_two_views(g.context.Get(), on);
    if (status != 0 && on) {
        simqvfr::logf("scs focus: NVIDIA's two views refused (%s): the focus view's scene draws are off for this session",
                      simqvfr::nvapi::status_name(status));
        g.mvr = false;
        return;
    }
    g.twoViews = on;
}

// The focus view's rectangle in a full-size target: right of the corner, from the top.
D3D11_RECT focus_rect() {
    const LONG x = static_cast<LONG>(std::lround(g.fx * g.eyeW));
    const LONG w = std::min<LONG>(std::lround(focus_fx() * g.eyeW), static_cast<LONG>(g.eyeW) - x);
    const LONG h = std::min<LONG>(std::lround(focus_fy() * g.eyeH), static_cast<LONG>(g.eyeH));
    return {x, 0, x + w, h};
}

// Two viewports and scissors: the game's own cut to the corner, and the focus view's.
void dual_views() {
    if (g.viewsDual || !g.viewportCount)
        return;
    const D3D11_RECT f = focus_rect();
    D3D11_VIEWPORT vp[2];
    scale_viewports(1, g.viewports, vp);
    vp[1] = {static_cast<float>(f.left), 0, static_cast<float>(f.right - f.left), static_cast<float>(f.bottom),
             vp[0].MinDepth, vp[0].MaxDepth};
    g.context->RSSetViewports(2, vp);
    if (g.scissorCount) {
        D3D11_RECT sc[2];
        scale_scissors(1, g.scissors, sc);
        sc[1] = f;
        g.context->RSSetScissorRects(2, sc);
    }
    g.viewsDual = true;
}

// An indexed draw of an eye pass's scene: the game shader's twin draws the eye and its focus view.
void focus_draw() {
    ID3D11VertexShader* vs = nullptr;
    g.context->VSGetShader(&vs, nullptr, nullptr);
    if (!vs)
        return;
    const bool instancing = g.instanced && (g.preferInstanced || !g.mvr);
    ID3D11VertexShader* twin = nullptr;
    UINT size = sizeof(twin);
    if (FAILED(vs->GetPrivateData(instancing ? lmu::kMvrInstancedTwinGuid : lmu::kMvrTwinGuid, &size, &twin)) || !twin) {
        vs->Release();
        ++g.noTwin;
        two_views(false);
        if (g.viewsDual) {  // this draw into the eye alone
            g.viewsDual = false;
            apply_views();
        }
        return;
    }
    two_views(!instancing);
    if (!instancing && !g.twoViews) {
        twin->Release();
        vs->Release();
        return;
    }
    g.instancedDraw = instancing;
    dual_views();
    if (const int eye = current_eye(); eye != g.remapEye) {  // this eye's box
        float r[4];
        box_remap(g.box[eye], r);
        g.context->UpdateSubresource(g.remapCb.Get(), 0, nullptr, r, 0, 0);
        g.remapEye = eye;
    }
    ID3D11Buffer* const remap[] = {g.remapCb.Get()};
    g.gameCb13.Reset();
    g.context->VSGetConstantBuffers(13, 1, g.gameCb13.GetAddressOf());  // put back after the draw, with the shader
    g.context->VSSetConstantBuffers(13, 1, remap);
    g.context->VSSetShader(twin, nullptr, 0);
    twin->Release();
    g.gameVs = vs;  // put back after the draw
    mark_bound_written();
    ++g.focusDraws;
}

// A draw whose shader focus_draw swapped for its instanced twin: drawn here with twice its instances, in the game's place.
bool on_draw_replace(ID3D11DeviceContext*, ch::Entry entry, const ch::DrawCall& d) {
    if (!g.instancedDraw)
        return false;
    g.instancedDraw = false;
    ID3D11DeviceContext1* c = g.context.Get();
    // Per-instance vertex data: the layout's twin, each step rate doubled, so a pair reads one instance's.
    ComPtr<ID3D11InputLayout> layout, twin;
    c->IAGetInputLayout(&layout);
    if (layout) {
        UINT size = sizeof(void*);
        IUnknown* stored = nullptr;  // SetPrivateDataInterface's: an IUnknown, asked for the layout (as LMU's doubled_draw)
        if (SUCCEEDED(layout->GetPrivateData(lmu::kMvrInstancedLayoutGuid, &size, &stored)) && stored) {
            stored->QueryInterface(IID_PPV_ARGS(twin.GetAddressOf()));
            stored->Release();
        }
        if (twin) {
            c->IASetInputLayout(twin.Get());
            ++g.doubledLayouts;
        }
    }
    switch (entry) {
        case ch::kDrawIndexed: c->DrawIndexedInstanced(d.count, 2, d.start, d.baseVertex, 0); break;
        case ch::kDrawIndexedInstanced:
            c->DrawIndexedInstanced(d.count, d.instances * 2, d.start, d.baseVertex, d.startInstance);
            break;
        case ch::kDraw: c->DrawInstanced(d.count, 2, d.start, 0); break;
        default: c->DrawInstanced(d.count, d.instances * 2, d.start, d.startInstance); break;
    }
    if (twin)
        c->IASetInputLayout(layout.Get());
    ++g.doubled;
    return true;
}

void on_draw(ID3D11DeviceContext*, ch::Entry entry, UINT count, UINT) {
    g.instancedDraw = false;
    if (!g.active || GetCurrentThreadId() != g.thread)
        return;
    if (entry == ch::kDrawIndexed || entry == ch::kDrawIndexedInstanced) {
        if (focus_on() && (g.mvr || g.instanced) && g.depthBound && g.fullSize) {
            const Timed t(State::kTimeFocusDraws);
            focus_draw();
        } else if (g.twoViews) {  // switched off mid-frame: one view again
            two_views(false);
            g.viewsDual = false;
            apply_views();
        }
        return;
    }
    if (entry != ch::kDraw && entry != ch::kDrawInstanced)
        return;
    two_views(false);  // the full-screen passes: one view
    if (g.focusPercent && g.boundSwapchain && g.burstFramesLeft > 0 && count == 3) {  // the eye's final image, whole
        ID3D11ShaderResourceView* v = nullptr;
        g.context->PSGetShaderResources(0, 1, &v);
        ComPtr<ID3D11Resource> res;
        if (v) {
            v->GetResource(&res);
            v->Release();
        }
        ComPtr<ID3D11Texture2D> tex;
        if (res && SUCCEEDED(res.As(&tex))) {
            wchar_t tag[32];
            swprintf_s(tag, L"final-pass%d", ++g.finalPasses);
            shoot(tex.Get(), 0, 0, false, false, tag);
        }
    }
    if (!g.cut && !g.smallTarget) {
        ++g.stats.skipped;
        return;
    }
    bool vs, ps;
    {
        const Timed t(State::kTimeRewrites);
        vs = rewrite(g.vs, g.vsCb.Get(), false);
        ps = rewrite(g.ps, g.psCb.Get(), true);
    }
    // The UI panels drawn onto the headset's image (quads placed in the world): with two views, like the scene.
    if (focus_on() && (g.mvr || g.instanced) && g.boundSwapchain && count != 3 && entry == ch::kDraw) {
        const Timed t(State::kTimeFocusDraws);
        focus_draw();
    }
    ++(vs || ps ? g.stats.rewritten : g.stats.notRewritten);  // the rule itself is the GPU's (the first frames' log names the draws)
    if (g.loggedDraws < 60) {
        ++g.loggedDraws;
        simqvfr::logf("scs corner: draw %u vertices into %ux%u (%s): vertex range %u+%u, pixel range %u+%u rewritten",
                      count, g.boundW, g.boundH, g.cut ? "eye-shaped, cut" : "small", g.vs.first, g.vs.count, g.ps.first,
                      g.ps.count);
    }
}

void on_draw_after(ID3D11DeviceContext*, ch::Entry entry, UINT count, UINT, UINT start) {
    if (g.gameVs) {
        g.context->VSSetShader(g.gameVs, nullptr, 0);
        g.gameVs->Release();
        g.gameVs = nullptr;
        ID3D11Buffer* const cb13[] = {g.gameCb13.Get()};  // the game's b13 again (null if it had none)
        g.context->VSSetConstantBuffers(13, 1, cb13);
        g.gameCb13.Reset();
    }
    if (g.vs.game || g.ps.game) {
        put_back(g.vs, false);
        put_back(g.ps, true);
    }
    // The focus view's turn: a full-screen triangle into a full-size target of the eye's. The quarter-size exposure and
    // bloom chain stays the eye's alone, and so does the half-size ambient occlusion chain (S6 run 2): its search
    // reaches up to a quarter of the eye's screen, most of a focus view's width, and the shader drops every sample
    // past its view's edge, so a focus view's own occlusion came out lighter and skewed towards its edges, differently
    // with every scene. The full-size upsample that follows is replayed, reading the eye's half-size occlusion and
    // depth through the box (twin_of fills a twin from the eye where the region has no content), against the focus
    // view's own full-size depth. Ctrl+Alt+A: the focus view's own occlusion again, live.
    if (entry == ch::kDraw && count == 3 && focus_on() && g.active && g.cut &&
        (g.ownOcclusion ? g.boundW * 2 + 2 >= g.eyeW : g.boundW == g.eyeW) && GetCurrentThreadId() == g.thread) {
        const Timed t(State::kTimeReplays);
        replay(count, start);  // into the headset's image too: its final blit
    }
    if (entry == ch::kDraw && count == 3 && g.boundSwapchain && g.active) {
        if (g.blits < 2 && g.boundChainEye >= 0) {
            // The first scene was drawn for the other eye's box: a frame whose focus view is the wrong one.
            if (g.blits == 0 && g.passEye[0] != g.boundChainEye) {
                ++g.orderChanges;
                if (g.passEyeLogs < 8) {
                    ++g.passEyeLogs;
                    simqvfr::logf("scs focus: the frame's first final blit is view %d's (%s eye), its scene was drawn as "
                                  "view %d's (headset image %d)%s",
                                  g.boundChainEye, g.boundChainEye == 0 ? "left" : "right", g.passEye[0], g.frameImage,
                                  g.passEyeLogs == 8 ? " (further ones counted in the summary)" : "");
                }
            }
            if (g.blits == 0)
                learn_order(g.boundChainEye);
            g.passEye[g.blits] = g.boundChainEye;
            g.passEye[1 - g.blits] = 1 - g.boundChainEye;
        }
        ++g.blits;  // an eye's final blit: the next post-processing is the other eye's
    }
}

bool make_resources() {
    const D3D11_BUFFER_DESC raw = {kMaxVectors * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                   D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    D3D11_BUFFER_DESC rawOut = raw;
    rawOut.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    const D3D11_BUFFER_DESC cb = {kMaxVectors * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_BUFFER_DESC params = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};  // run_cs's
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx = {0, kMaxVectors * 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer = {0, kMaxVectors * 4, D3D11_BUFFER_UAV_FLAG_RAW};
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kCornerPatchCs, sizeof(kCornerPatchCs) - 1, "scs-corner", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        simqvfr::logf("scs corner: the rewrite shader didn't compile: %s",
                      errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    ComPtr<ID3DBlob> mirrorCode;
    errors.Reset();
    if (FAILED(D3DCompile(kCornerMirrorCs, sizeof(kCornerMirrorCs) - 1, "scs-corner-mirror", nullptr, nullptr, "main",
                          "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &mirrorCode, &errors))) {
        simqvfr::logf("scs corner: the mirror shader didn't compile: %s",
                      errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(g.device->CreateComputeShader(mirrorCode->GetBufferPointer(), mirrorCode->GetBufferSize(), nullptr,
                                             &g.mirrorCs)))
        return false;
    return SUCCEEDED(g.device->CreateBuffer(&raw, nullptr, &g.rawIn)) &&
           SUCCEEDED(g.device->CreateBuffer(&rawOut, nullptr, &g.rawOut)) &&
           SUCCEEDED(g.device->CreateBuffer(&cb, nullptr, &g.vsCb)) &&
           SUCCEEDED(g.device->CreateBuffer(&cb, nullptr, &g.psCb)) &&
           SUCCEEDED(g.device->CreateBuffer(&cb, nullptr, &g.vsCbFocus)) &&
           SUCCEEDED(g.device->CreateBuffer(&cb, nullptr, &g.psCbFocus)) &&
           SUCCEEDED(g.device->CreateBuffer(&params, nullptr, &g.params)) &&
           SUCCEEDED(g.device->CreateShaderResourceView(g.rawIn.Get(), &sv, &g.rawInView)) &&
           SUCCEEDED(g.device->CreateUnorderedAccessView(g.rawOut.Get(), &uv, &g.rawOutView)) &&
           SUCCEEDED(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.patchCs));
}


DXGI_FORMAT srgb_view(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return f;
    }
}

bool blit_resources() {
    if (g.blitPs)
        return true;
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors)) ||
        FAILED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors)))
        return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3DBlob> fps, sps;
    if (SUCCEEDED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "squash", "ps_5_0", 0, 0, &sps, &errors)))
        g.device->CreatePixelShader(sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, &g.squashPs);
    if (FAILED(D3DCompile(kBlit, sizeof(kBlit) - 1, "scs-corner-blit", nullptr, nullptr, "feather", "ps_5_0", 0, 0, &fps, &errors)))
        return false;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                          D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_COLOR_WRITE_ENABLE_ALL};
    return SUCCEEDED(g.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.blitVs)) &&
           SUCCEEDED(g.device->CreatePixelShader(fps->GetBufferPointer(), fps->GetBufferSize(), nullptr, &g.featherPs)) &&
           SUCCEEDED(g.device->CreateBlendState(&bd, &g.featherBlend)) &&
           SUCCEEDED(g.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.blitPs)) &&
           SUCCEEDED(g.device->CreateSamplerState(&sd, &g.linear)) &&
           SUCCEEDED(g.device->CreateRasterizerState(&rd, &g.raster));
}

ID3D11RenderTargetView* target_view(ID3D11Texture2D* image, const D3D11_TEXTURE2D_DESC& d) {
    for (State::Target& t : g.targets)
        if (t.image == image)
            return t.view.Get();
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = srgb_view(d.Format);
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    State::Target t;
    t.image = image;
    if (FAILED(g.device->CreateRenderTargetView(image, &rv, &t.view)))
        return nullptr;
    g.targets.push_back(t);
    return g.targets.back().view.Get();
}

// The corner of `image` (cw x ch) drawn back over the whole image; the game's state put back. False: nothing changed.
bool upscale(ID3D11Texture2D* image, UINT cw, UINT ch, int eye) {
    D3D11_TEXTURE2D_DESC d;
    image->GetDesc(&d);
    if (!blit_resources() || d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_RENDER_TARGET) || cw > d.Width ||
        ch > d.Height)
        return false;
    if (!g.scratch || g.scratchDesc.Width != cw || g.scratchDesc.Height != ch || g.scratchDesc.Format != d.Format) {
        g.scratch.Reset();
        g.scratchView.Reset();
        D3D11_TEXTURE2D_DESC sd = {cw, ch, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srgb_view(d.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.scratch)) ||
            FAILED(g.device->CreateShaderResourceView(g.scratch.Get(), &sv, &g.scratchView)))
            return false;
        g.scratchDesc = sd;
    }
    ID3D11RenderTargetView* rtv = target_view(image, d);
    if (!rtv)
        return false;
    // The focus view, if this image's focus region was drawn this frame: kept aside before the eye covers it.
    const D3D11_RECT fr = focus_rect_in(d.Width, d.Height);
    const UINT fw = static_cast<UINT>(fr.right - fr.left), fh = static_cast<UINT>(fr.bottom);
    bool focus = false;
    if (g.focusPercent && g.composeFocus && fw && fh) {
        const auto it = g.texes.find(image);
        focus = it != g.texes.end() && it->second.writeFrame == g.stats.frames;
    }
    if (focus && (!g.focusScratch || g.focusScratchDesc.Width != fw || g.focusScratchDesc.Height != fh ||
                  g.focusScratchDesc.Format != d.Format)) {
        g.focusScratch.Reset();
        g.focusScratchView.Reset();
        D3D11_TEXTURE2D_DESC sd = {fw, fh, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srgb_view(d.Format);
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.focusScratch)) ||
            FAILED(g.device->CreateShaderResourceView(g.focusScratch.Get(), &sv, &g.focusScratchView)))
            focus = false;
        else
            g.focusScratchDesc = sd;
    }
    two_views(false);
    ID3D11DeviceContext1* c = g.context.Get();
    const ch::Bypass bypass;
    // The game's state this touches.
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> samp;
    ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4];
    UINT sampleMask;
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilRef;
    ComPtr<ID3D11RasterizerState> raster;
    D3D11_VIEWPORT vp[kMaxViews];
    UINT vpCount = kMaxViews;
    c->IAGetInputLayout(&layout);
    c->IAGetPrimitiveTopology(&topology);
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->GSGetShader(&gs, nullptr, nullptr);
    c->HSGetShader(&hs, nullptr, nullptr);
    c->DSGetShader(&ds, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &srv);
    c->PSGetSamplers(0, 1, &samp);
    ID3D11RenderTargetView* rawRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* rawDsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawRtvs, &rawDsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        rtvs[i].Attach(rawRtvs[i]);
    dsv.Attach(rawDsv);
    c->OMGetBlendState(&blend, blendFactor, &sampleMask);
    c->OMGetDepthStencilState(&depthState, &stencilRef);
    c->RSGetState(&raster);
    c->RSGetViewports(&vpCount, vp);

    const D3D11_BOX box = {0, 0, 0, cw, ch, 1};
    c->CopySubresourceRegion(g.scratch.Get(), 0, 0, 0, 0, image, 0, &box);
    if (focus) {
        const D3D11_BOX fbox = {static_cast<UINT>(fr.left), 0, 0, static_cast<UINT>(fr.right), fh, 1};
        c->CopySubresourceRegion(g.focusScratch.Get(), 0, 0, 0, 0, image, 0, &fbox);
    }
    ID3D11ShaderResourceView* noSrv[] = {nullptr};
    c->PSSetShaderResources(0, 1, noSrv);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT full = {0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height), 0, 1};
    c->RSSetViewports(1, &full);
    c->RSSetState(g.raster.Get());
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.blitVs.Get(), nullptr, 0);
    c->PSSetShader(g.blitPs.Get(), nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* src[] = {g.scratchView.Get()};
    ID3D11SamplerState* s[] = {g.linear.Get()};
    c->PSSetShaderResources(0, 1, src);
    c->PSSetSamplers(0, 1, s);
    c->Draw(3, 0);
    if (focus) {  // over the box: the focus region stretched (or shrunk) to its share of the image
        const float* b = g.box[std::min(std::max(eye, 0), 1)];
        const D3D11_VIEWPORT bv = {b[0] * d.Width, b[1] * d.Height, beta_x() * d.Width, beta_y() * d.Height, 0, 1};
        c->RSSetViewports(1, &bv);
        ID3D11ShaderResourceView* fsrc[] = {g.focusScratchView.Get()};
        c->PSSetShaderResources(0, 1, fsrc);
        c->PSSetShader(g.featherPs.Get(), nullptr, 0);
        c->OMSetBlendState(g.featherBlend.Get(), nullptr, 0xffffffff);
        // The edge: the game's own buffer in pixel slot 1 kept and put back.
        ID3D11Buffer* gameCb = nullptr;
        UINT gameFirst = 0, gameCount = 0;
        c->PSGetConstantBuffers1(1, 1, &gameCb, &gameFirst, &gameCount);
        if (g.lookCb) {
            const float look[8] = {std::max(0.0f, g.lookFeather.load(std::memory_order_relaxed)),
                                   g.lookRadius.load(std::memory_order_relaxed),
                                   g.lookBorder.load(std::memory_order_relaxed) ? 1.0f : 0.0f,
                                   bv.Height > 0 ? bv.Width / bv.Height : 1.0f,
                                   g.lookColor[0].load(std::memory_order_relaxed), g.lookColor[1].load(std::memory_order_relaxed),
                                   g.lookColor[2].load(std::memory_order_relaxed), 1};
            c->UpdateSubresource(g.lookCb.Get(), 0, nullptr, look, 0, 0);
            ID3D11Buffer* const cb[] = {g.lookCb.Get()};
            c->PSSetConstantBuffers(1, 1, cb);
        }
        c->Draw(3, 0);
        ID3D11Buffer* const put[] = {gameCb};
        if (gameCb && gameCount)
            c->PSSetConstantBuffers1(1, 1, put, &gameFirst, &gameCount);
        else
            c->PSSetConstantBuffers(1, 1, put);
        if (gameCb)
            gameCb->Release();
        ++g.composed;
    }

    c->PSSetShaderResources(0, 1, noSrv);
    ID3D11RenderTargetView* putRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        putRtvs[i] = rtvs[i].Get();
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, putRtvs, dsv.Get());
    c->IASetInputLayout(layout.Get());
    c->IASetPrimitiveTopology(topology);
    c->VSSetShader(vs.Get(), nullptr, 0);
    c->PSSetShader(ps.Get(), nullptr, 0);
    c->GSSetShader(gs.Get(), nullptr, 0);
    c->HSSetShader(hs.Get(), nullptr, 0);
    c->DSSetShader(ds.Get(), nullptr, 0);
    ID3D11ShaderResourceView* putSrv[] = {srv.Get()};
    ID3D11SamplerState* putSamp[] = {samp.Get()};
    c->PSSetShaderResources(0, 1, putSrv);
    c->PSSetSamplers(0, 1, putSamp);
    c->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
    c->OMSetDepthStencilState(depthState.Get(), stencilRef);
    c->RSSetState(raster.Get());
    if (vpCount)
        c->RSSetViewports(vpCount, vp);
    return true;
}


ID3D11ShaderResourceView* source_view(ID3D11Texture2D* image, const D3D11_TEXTURE2D_DESC& d) {
    for (State::Source& s : g.sources)
        if (s.image == image)
            return s.view.Get();
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = srgb_view(d.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    State::Source s;
    s.image = image;
    if (FAILED(g.device->CreateShaderResourceView(image, &sv, &s.view)))
        return nullptr;
    g.sources.push_back(s);
    return g.sources.back().view.Get();
}

// The whole image drawn at a quarter of its size into `burstTex`, copied into a new staging texture: one shot.
void shoot(ID3D11Texture2D* image, int chain, uint32_t index, bool after, bool scaled, const wchar_t* tag) {
    two_views(false);
    D3D11_TEXTURE2D_DESC d;
    image->GetDesc(&d);
    ID3D11ShaderResourceView* srv = source_view(image, d);
    if (!srv || !blit_resources())
        return;
    const UINT w = std::max(1u, d.Width / 4), h = std::max(1u, d.Height / 4);
    if (!g.burstTex || g.burstDesc.Width != w || g.burstDesc.Height != h) {
        g.burstTex.Reset();
        g.burstRtv.Reset();
        D3D11_TEXTURE2D_DESC sd = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.burstTex)) ||
            FAILED(g.device->CreateRenderTargetView(g.burstTex.Get(), &rv, &g.burstRtv)))
            return;
        g.burstDesc = sd;
    }
    State::Shot shot;
    D3D11_TEXTURE2D_DESC st = g.burstDesc;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.device->CreateTexture2D(&st, nullptr, &shot.staging)))
        return;
    ID3D11DeviceContext1* c = g.context.Get();
    const ch::Bypass bypass;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> psSrv;
    ComPtr<ID3D11SamplerState> samp;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[kMaxViews];
    UINT vpCount = kMaxViews;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    FLOAT bf[4];
    UINT mask;
    ComPtr<ID3D11DepthStencilState> depth;
    UINT ref;
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &psSrv);
    c->PSGetSamplers(0, 1, &samp);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    c->RSGetViewports(&vpCount, vp);
    c->RSGetState(&raster);
    c->OMGetBlendState(&blend, bf, &mask);
    c->OMGetDepthStencilState(&depth, &ref);
    c->IAGetInputLayout(&layout);
    c->IAGetPrimitiveTopology(&topo);
    ID3D11RenderTargetView* out[] = {g.burstRtv.Get()};
    c->OMSetRenderTargets(1, out, nullptr);
    const D3D11_VIEWPORT v = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    c->RSSetViewports(1, &v);
    c->RSSetState(g.raster.Get());
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.blitVs.Get(), nullptr, 0);
    c->PSSetShader(g.shootSquash && g.squashPs ? g.squashPs.Get() : g.blitPs.Get(), nullptr, 0);
    ID3D11SamplerState* s[] = {g.linear.Get()};
    c->PSSetShaderResources(0, 1, &srv);
    c->PSSetSamplers(0, 1, s);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none[] = {nullptr};
    c->PSSetShaderResources(0, 1, none);
    c->CopyResource(shot.staging.Get(), g.burstTex.Get());
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    for (ID3D11RenderTargetView* r : rtvs)
        if (r)
            r->Release();
    if (dsv)
        dsv->Release();
    if (vpCount)
        c->RSSetViewports(vpCount, vp);
    c->RSSetState(raster.Get());
    c->OMSetBlendState(blend.Get(), bf, mask);
    c->OMSetDepthStencilState(depth.Get(), ref);
    c->IASetInputLayout(layout.Get());
    c->IASetPrimitiveTopology(topo);
    c->VSSetShader(vs.Get(), nullptr, 0);
    c->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* put[] = {psSrv.Get()};
    ID3D11SamplerState* putS[] = {samp.Get()};
    c->PSSetShaderResources(0, 1, put);
    c->PSSetSamplers(0, 1, putS);
    shot.frame = g.stats.frames;
    shot.tag = tag ? tag : L"";
    shot.w = w;
    shot.h = h;
    shot.chain = chain;
    shot.index = index;
    shot.after = after;
    shot.cornered = g.active;
    shot.scaled = scaled;
    g.shots.push_back(shot);
}

// After the burst: every shot read back (render thread), written as PNG on a thread of its own.
void write_burst() {
    struct Pixels {
        std::vector<uint8_t> bgra;
        unsigned w = 0, h = 0;
        std::wstring name;
    };
    std::vector<Pixels> all;
    for (State::Shot& s : g.shots) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
            continue;
        Pixels p;
        p.w = s.w;
        p.h = s.h;
        p.bgra.resize(static_cast<size_t>(p.w) * p.h * 4);
        for (unsigned y = 0; y < p.h; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
            uint8_t* o = p.bgra.data() + static_cast<size_t>(y) * p.w * 4;
            for (unsigned x = 0; x < p.w; ++x) {
                o[x * 4 + 0] = row[x * 4 + 2];
                o[x * 4 + 1] = row[x * 4 + 1];
                o[x * 4 + 2] = row[x * 4 + 0];
                o[x * 4 + 3] = 255;
            }
        }
        g.context->Unmap(s.staging.Get(), 0);
        wchar_t name[96];
        swprintf_s(name, L"f%03llu-eye%d-%ls-img%u%ls%ls.png", static_cast<unsigned long long>(s.frame - g.burstStartFrame),
                   s.chain, s.after ? L"2after" : L"1before", s.index, s.cornered ? L"" : L"-nocorner",
                   s.after && !s.scaled ? L"-NOTSCALED" : L"");
        if (!s.tag.empty())
            swprintf_s(name, L"f%03llu-%ls.png", static_cast<unsigned long long>(s.frame - g.burstStartFrame), s.tag.c_str());
        p.name = name;
        all.push_back(std::move(p));
    }
    g.shots.clear();
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t folder[64];
    swprintf_s(folder, L"scs-burst-%02u%02u%02u", t.wHour, t.wMinute, t.wSecond);
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures" / folder;
    simqvfr::logf("scs corner: burst of %zu images read back; writing them to %ls", all.size(), dir.c_str());
    std::thread([all = std::move(all), dir]() {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        size_t written = 0;
        for (const auto& p : all)
            written += simqvfr::write_png_bgra((dir / p.name).wstring(), p.bgra, p.w, p.h) ? 1 : 0;
        simqvfr::logf("scs corner: burst written: %zu of %zu PNGs in %ls", written, all.size(), dir.c_str());
    }).detach();
}

// The diagnostic hotkeys, Ctrl+Alt with: B a burst from the next release; K the focus view composed or not; J bursts at
// the next three box jumps; H the reprojections' box; G the focus view's work off; I NVIDIA's multi-view or instancing;
// A the focus view's own occlusion; N the mirrors off. (Not F: the player's frame capture hotkey.)
[[maybe_unused]] void poll_diag_keys() {  // unused in a release (poll_diag_hotkeys)
    const bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    // `key` pressed with the chord since the last poll (`held`: its state then).
    const auto pressed = [chord](int key, bool& held) {
        const bool down = chord && (GetAsyncKeyState(key) & 0x8000);
        const bool edge = down && !held;
        held = down;
        return edge;
    };
    if (pressed('B', g.hotkeyDown) && g.burstFramesLeft == 0 && g.shots.empty()) {
        g.burstFramesLeft = kBurstFrames;
        g.burstStartFrame = g.stats.frames;
        simqvfr::logf("scs corner: Ctrl+Alt+B: recording the next %d frames' released images (before and after the "
                      "scale-up, a quarter size)", kBurstFrames);
    }
    if (pressed('K', g.composeKeyDown) && g.focusPercent) {
        g.composeFocus = !g.composeFocus;
        simqvfr::logf("scs focus: Ctrl+Alt+K: the focus view %s", g.composeFocus ? "composed over the box" : "not composed");
    }
    if (pressed('J', g.jumpKeyDown) && g.focusPercent) {
        g.jumpBursts = 3;
        simqvfr::log_line("scs focus: Ctrl+Alt+J: the next three jumps of the box are recorded");
    }
    if (pressed('H', g.historyKeyDown) && g.focusPercent) {
        g.historyCurrentBox = !g.historyCurrentBox;
        simqvfr::logf("scs focus: Ctrl+Alt+H: the reprojections use %s", g.historyCurrentBox ? "this frame's box" : "the frame before's box");
    }
    if (pressed('G', g.focusKeyDown) && g.focusPercent) {
        perf_line();  // what the state so far cost, then the other
        g.focusOff = !g.focusOff;
        simqvfr::logf("scs focus: Ctrl+Alt+G: the focus view's draws and passes are %s", g.focusOff ? "OFF" : "on");
    }
    if (pressed('I', g.instancingKeyDown) && g.focusPercent) {
        if (g.mvr && g.instanced) {
            perf_line();  // what the way so far cost, then the other
            g.preferInstanced = !g.preferInstanced;
            simqvfr::logf("scs focus: Ctrl+Alt+I: the focus view's scene draws by %s",
                          g.preferInstanced ? "Sim-QVFR's own multi-view rendering (instancing)" : "NVIDIA's multi-view rendering");
        } else {
            simqvfr::logf("scs focus: Ctrl+Alt+I: only one way here (NVIDIA's %d, instancing %d)", g.mvr ? 1 : 0, g.instanced ? 1 : 0);
        }
    }
    if (pressed('A', g.occlusionKeyDown) && g.focusPercent) {
        g.ownOcclusion = !g.ownOcclusion;
        simqvfr::logf("scs focus: Ctrl+Alt+A: the focus view's ambient occlusion is %s",
                      g.ownOcclusion ? "its own (the half-size passes replayed)" : "the eye's, through the box");
    }
    if (pressed('N', g.mirrorsKeyDown)) {
        g.mirrorsOff = !g.mirrorsOff;
        if (g.active) {
            const ch::Bypass bypass;  // our own binds (at a release: outside an observer), not seen by on_constant_buffers
            swap_bound(!g.mirrorsOff);  // what is bound now: the game's buffers back, or our copies
        }
        simqvfr::logf("scs corner: Ctrl+Alt+N: the scene draws read %s (writes to them from other threads so far: %llu, "
                      "%llu with the eyes in the corner)",
                      g.mirrorsOff ? "the game's own constant buffers (mirrors OFF)" : "our copies (mirrors on)",
                      static_cast<unsigned long long>(g.otherWrites.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g.otherWritesActive.load(std::memory_order_relaxed)));
    }
}

// The diagnostic hotkeys exist in development builds only (dev_build.h): a release reads no key here.
void poll_diag_hotkeys() {
    if constexpr (simqvfr::kDevBuild)
        poll_diag_keys();
}

State::Chain* chain_of(XrSwapchain handle, PFN_xrEnumerateSwapchainImages enumerate) {
    for (State::Chain& c : g.chains)
        if (c.handle == handle)
            return &c;
    if (!enumerate)
        return nullptr;
    uint32_t count = 0;
    if (XR_FAILED(enumerate(handle, 0, &count, nullptr)) || !count)
        return nullptr;
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(enumerate(handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
        return nullptr;
    State::Chain c;
    c.handle = handle;
    for (const auto& i : images)
        c.images.push_back(i.texture);
    g.chains.push_back(c);
    simqvfr::logf("scs corner: swapchain %p: %u images known (the corner is scaled back over them at xrEndFrame)",
                  static_cast<void*>(handle), count);
    return &g.chains.back();
}

// Everything tied to the device and the session, released and reset: at session end, and on every way out of
// corner_install that leaves QVFR off (a second session may come on another thread or another device; every
// resource of ours keeps the device alive). What other threads set (the gaze, the live sizes, the look), the player's
// toggles and the running counts stay. The context's hooks are gone before this runs (or never were).
void release_all() {
    if (g.context)
        two_views(false);
    if (g.gameVs) {
        g.gameVs->Release();
        g.gameVs = nullptr;
    }
    g.gameCb13.Reset();
    g.thread = 0;
    g.active = false;
    g.activeNow.store(false, std::memory_order_relaxed);
    g.cornerNow.store(false, std::memory_order_relaxed);
    g.seenEndFrames = g.endFrames.load(std::memory_order_acquire);
    g.thisFrameCornered = false;
    g.cut = g.smallTarget = false;
    g.eyeW = g.eyeH = 0;  // learned again at the next session's first G-buffer bind
    g.scaleMismatch = false;
    g.viewportCount = g.scissorCount = 0;
    g.viewsDual = g.twoViews = false;
    g.instancedDraw = false;
    g.mvr = g.instanced = false;
    g.focusPercent = 0;
    g.boundSwapchain = g.boundMarked = false;
    g.boundChainEye = -1;
    std::fill(std::begin(g.boundRt), std::end(g.boundRt), nullptr);
    g.gbufferTex.Reset();
    g.gbufferPasses = g.blits = 0;
    g.postPhase = false;
    g.remapEye = -1;
    // The eye order: new swapchains, learned again.
    g.acquiredIndex.store(-1, std::memory_order_relaxed);
    g.frameImage = -1;
    std::fill(std::begin(g.firstEyeByImage), std::end(g.firstEyeByImage), -1);
    g.orderLogged = false;
    // The rewrite's and the mirrors' resources.
    g.vs = Range{};
    g.ps = Range{};
    g.vsFocus = Range{};
    g.psFocus = Range{};
    g.rawIn.Reset(), g.rawOut.Reset(), g.params.Reset(), g.vsCb.Reset(), g.psCb.Reset(), g.vsCbFocus.Reset(),
        g.psCbFocus.Reset();
    g.rawInView.Reset();
    g.rawOutView.Reset();
    g.patchCs.Reset();
    g.mirrorCs.Reset();
    g.mirrorIdCount.store(0, std::memory_order_release);  // first: the other threads stop comparing
    for (auto& id : g.mirrorIds)
        id.store(nullptr, std::memory_order_relaxed);
    g.mirrors.clear();
    g.bigIn.Reset(), g.bigOut.Reset(), g.bigInView.Reset(), g.bigOutView.Reset();
    g.bigBytes = 0;
    // The focus view's.
    g.remapCb.Reset();
    g.lookCb.Reset();
    g.twinViews.clear();
    g.texes.clear();
    g.twinBytes = 0;
    g.stretches.clear();
    // The headset images' and the blits'.
    {
        std::lock_guard lock(g.frameMutex);
        g.chains.clear();
        g.targets.clear();
    }
    g.scratch.Reset(), g.scratchView.Reset();
    g.scratchDesc = {};
    g.focusScratch.Reset(), g.focusScratchView.Reset();
    g.focusScratchDesc = {};
    g.blitVs.Reset();
    g.blitPs.Reset(), g.featherPs.Reset(), g.squashPs.Reset();
    g.featherBlend.Reset();
    g.linear.Reset();
    g.raster.Reset();
    // The burst recorder's.
    g.shots.clear();
    g.crops.clear();
    g.sources.clear();
    g.burstTex.Reset(), g.burstRtv.Reset();
    g.burstDesc = {};
    g.burstFramesLeft = 0;
    g.context.Reset();
    g.device.Reset();
}

}  // namespace

bool corner_install(ID3D11Device* device, int percent, int focusPercent) {
    if (!device || percent <= 0 || percent >= 100 || g.context)
        return g.context != nullptr;
    g.device = device;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (FAILED(context.As(&g.context))) {
        simqvfr::log_line("scs corner: no ID3D11DeviceContext1 (ranged constant buffers): off");
        release_all();
        return false;
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt{};
    device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt));
    if (!opt.ConstantBufferOffsetting || !opt.ConstantBufferPartialUpdate) {
        simqvfr::log_line("scs corner: the device can't bind or update part of a constant buffer: off");
        release_all();
        return false;
    }
    if (!make_resources()) {
        simqvfr::log_line("scs corner: its buffers or shader couldn't be made: off");
        release_all();
        return false;
    }
    g.percent = static_cast<float>(percent);
    ch::Observers o;
    o.targets = &on_targets;
    o.draw = &on_draw;
    o.draw_after = &on_draw_after;
    o.draw_replace = &on_draw_replace;
    o.viewports = &on_viewports;
    o.scissors = &on_scissors;
    o.constant_buffers = &on_constant_buffers;
    o.unmap = &on_unmap;
    uint32_t wanted = (1u << ch::kOMSetRenderTargets) | (1u << ch::kOMSetRenderTargetsAndUavs) |
                      (1u << ch::kDraw) | (1u << ch::kDrawInstanced) | (1u << ch::kRSSetViewports) |
                      (1u << ch::kRSSetScissorRects) | (1u << ch::kVSSetConstantBuffers1) |
                      (1u << ch::kPSSetConstantBuffers1) | (1u << ch::kUnmap);
    g.focusPercent = 0;
    if (focusPercent > 0 && focusPercent + percent <= 100) {
        const simqvfr::nvapi::Support support = simqvfr::nvapi::query(device);
        // The focus view shares the eye's camera: its clip position is the eye's, x and y scaled about the centre.
        const float remap[4] = {100.0f / focusPercent, 0, 100.0f / focusPercent, 0};
        const D3D11_BUFFER_DESC rd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA ri = {remap, 0, 0};
        g.focusPercent = focusPercent;  // the full-screen passes' replays need neither way
        g.focusPercentY = focusPercent;
        g.density = 1;
        const D3D11_BUFFER_DESC ld = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        // The composed box's edge (the feather shader's b1): without it the shader would read whatever the game bound.
        const bool lookCb = SUCCEEDED(device->CreateBuffer(&ld, nullptr, &g.lookCb));
        const bool remapCb = lookCb && SUCCEEDED(device->CreateBuffer(&rd, &ri, &g.remapCb));
        // The two ways to the scene draws' second view: NVIDIA's (the probe attached its twins unless
        // multi_view_software=1), and instancing where a vertex shader may pick the viewport.
        const lmu::MvrBackend probed = lmu::mvr_probe_backend();
        D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
        g.instanced = remapCb && SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3))) &&
                      o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
        g.mvr = remapCb && support.xyzw && probed != lmu::MvrBackend::instanced;
        g.preferInstanced = g.instanced && !g.mvr;
        if (g.mvr || g.instanced) {
            wanted |= (1u << ch::kDrawIndexed) | (1u << ch::kDrawIndexedInstanced);
            simqvfr::logf("scs focus: on: each eye pass's indexed scene draws also draw the middle %d%% of the view beside "
                          "the corner, by %s (NVIDIA's %s, instancing %s%s)",
                          focusPercent, g.mvr ? "NVIDIA's multi-view rendering" : "Sim-QVFR's own multi-view rendering (instancing)",
                          g.mvr ? "there" : support.xyzw ? "not used (multi_view_software=1)" : "not on this device",
                          g.instanced ? "there" : "not on this device",
                          simqvfr::kDevBuild ? "; Ctrl+Alt+I switches where both are; Ctrl+Alt+B saves the G-buffers" : "");
        } else {  // no way to a second view on this GPU: the game is left as it is
            simqvfr::logf("scs focus: neither NVIDIA's two views (loaded %d, xyzw %d, %s) nor a viewport index from the "
                          "vertex shader on this device: QVFR stays off, the game draws as without Sim-QVFR",
                          support.loaded, support.xyzw, simqvfr::nvapi::status_name(support.status));
            release_all();
            return false;
        }
    } else if (focusPercent > 0) {
        simqvfr::logf("scs focus: off: %d%% doesn't fit beside a %d%% corner", focusPercent, percent);
    }
    if (!ch::install(g.context.Get(), wanted, o)) {
        simqvfr::log_line("scs corner: the context couldn't be hooked: off");
        release_all();
        return false;
    }
    simqvfr::logf("scs corner: on: each eye drawn into the top-left %d%% of its targets on each axis (log of the first "
                  "rewritten draws follows; a summary every 900 frames)",
                  percent);
    return true;
}

bool corner_on() { return g.context != nullptr; }

const XrFrameEndInfo* corner_frame_end(const XrFrameEndInfo* info, PFN_xrEnumerateSwapchainImages enumerate) {
    if (!g.context)
        return info;
    g.endFrames.fetch_add(1, std::memory_order_release);
    // The context's entries hooked again if the game's mode changed them: on its own thread, between its commands
    // (context_hooks.h), never inside an observer.
    if (g.thread && GetCurrentThreadId() == g.thread)
        ch::rescan();
    // The frame being submitted: whether its eyes went into the corner (ATS draws a frame, then ends it).
    const bool cornered = g.cornerNow.exchange(false, std::memory_order_acq_rel);
    if (g.burstFramesLeft > 0 && g.thread && GetCurrentThreadId() == g.thread && --g.burstFramesLeft == 0)
        write_burst();
    if (!cornered || !info)
        return info;
    std::lock_guard lock(g.frameMutex);
    g.projections.clear();
    g.views.clear();
    g.layers.assign(info->layers, info->layers + info->layerCount);
    size_t viewTotal = 0;
    for (uint32_t i = 0; i < info->layerCount; ++i)
        if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            viewTotal += reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i])->viewCount;
    g.projections.reserve(info->layerCount);
    g.views.reserve(viewTotal);
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        const size_t at = g.views.size();
        for (uint32_t v = 0; v < proj->viewCount; ++v) {
            XrCompositionLayerProjectionView view = proj->views[v];
            XrRect2Di& r = view.subImage.imageRect;
            const UINT cw = static_cast<UINT>(std::max(1, static_cast<int32_t>(std::lround(r.extent.width * g.fx))));
            const UINT ch = static_cast<UINT>(std::max(1, static_cast<int32_t>(std::lround(r.extent.height * g.fy))));
            State::Chain* chain = chain_of(view.subImage.swapchain, enumerate);
            if (chain && chain->scaled && r.offset.x == 0 && r.offset.y == 0 && view.subImage.imageArrayIndex == 0) {
                ++g.upscaled;  // scaled back at its release: its rectangle as the game gave it
            } else {
                ++g.upscaleMisses;
                r.extent.width = static_cast<int32_t>(cw);
                r.extent.height = static_cast<int32_t>(ch);
            }
            if (chain) {
                chain->scaled = false;
                chain->eye = static_cast<int>(v);
            }
            g.views.push_back(view);
        }
        g.projections.push_back(*proj);
        g.projections.back().views = g.views.data() + at;
        g.layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g.projections.back());
    }
    g.info = *info;
    g.info.layers = g.layers.data();
    return &g.info;
}

void corner_acquired(XrSwapchain chain, uint32_t index, PFN_xrEnumerateSwapchainImages enumerate) {
    if (!g.context)
        return;
    std::lock_guard lock(g.frameMutex);
    if (State::Chain* c = chain_of(chain, enumerate)) {
        c->held.push_back(index);
        const int eye = c->eye >= 0 ? c->eye : static_cast<int>((c - g.chains.data()) & 1);
        if (eye == 0)  // the key of the frame's eye order (scene_eye)
            g.acquiredIndex.store(static_cast<int>(index), std::memory_order_release);
        if (c->held.size() > 1 && g.heldTwo++ == 0)
            simqvfr::logf("scs corner: the game acquires its next swapchain image before releasing the last one (%zu held): "
                          "each release scales the oldest", c->held.size());
    }
}

void corner_releasing(XrSwapchain chain) {
    if (!g.context)
        return;
    // The image this release hands back: the oldest the game holds (taken under the lock: corner_acquired may grow the
    // list from another thread meanwhile).
    int chainIdx = -1;
    uint32_t released = ~0u;
    ID3D11Texture2D* image = nullptr;
    {
        std::lock_guard lock(g.frameMutex);
        for (size_t i = 0; i < g.chains.size(); ++i)
            if (g.chains[i].handle == chain && !g.chains[i].held.empty()) {
                chainIdx = static_cast<int>(i);
                released = g.chains[i].held.front();
                g.chains[i].held.pop_front();
                if (released < g.chains[i].images.size())
                    image = g.chains[i].images[released];
                break;
            }
    }
    if (!image)
        return;
    // The drawing thread's own state only on it; elsewhere the published copy.
    const bool onThread = g.thread && GetCurrentThreadId() == g.thread;
    const bool active = onThread ? g.active : g.activeNow.load(std::memory_order_relaxed);
    if (onThread)
        poll_diag_hotkeys();
    const bool burst = onThread && g.burstFramesLeft > 0;
    if (burst && !active) {  // a frame without its eyes in the corner, recorded too
        std::lock_guard lock(g.frameMutex);
        shoot(image, chainIdx, released, true, false);
    }
    if (!active)
        return;  // a frame without its eyes in the corner: the image as the game drew it
    if (!onThread) {
        if (g.otherThreadReleases++ == 0)
            simqvfr::log_line("scs corner: a swapchain image released off the render thread: not scaled (the corner is cut "
                              "at xrEndFrame instead)");
        return;
    }
    std::lock_guard lock(g.frameMutex);
    State::Chain* found = nullptr;  // found again: the list may have changed since
    for (State::Chain& x : g.chains)
        if (x.handle == chain)
            found = &x;
    if (!found)
        return;
    State::Chain& c = *found;
    D3D11_TEXTURE2D_DESC d;
    image->GetDesc(&d);
    const UINT cw = static_cast<UINT>(std::max(1L, std::lround(d.Width * g.fx)));
    const UINT ch = static_cast<UINT>(std::max(1L, std::lround(d.Height * g.fy)));
    if (burst)
        shoot(image, chainIdx, released, false, false);
    {
        const Timed t(State::kTimeRelease);
        c.scaled = upscale(image, cw, ch, c.eye >= 0 ? c.eye : chainIdx & 1);
    }
    if (burst)
        shoot(image, chainIdx, released, true, c.scaled);
}

void corner_set_gaze(const float (&screen)[2][2]) {
    for (int e = 0; e < 2; ++e)
        for (int a = 0; a < 2; ++a)
            if (screen[e][a] == screen[e][a])  // not NaN
                g.gaze[e][a].store(screen[e][a], std::memory_order_relaxed);
}

void corner_set_live(int percent, int focusPercent, int focusPercentY, int densityPercent) {
    g.liveFocusPercent.store(focusPercent, std::memory_order_relaxed);
    g.liveFocusPercentY.store(focusPercentY, std::memory_order_relaxed);
    g.liveDensity.store(densityPercent, std::memory_order_relaxed);
    g.livePercent.store(percent, std::memory_order_relaxed);
}

void corner_set_look(float feather, float cornerRadius, bool border, const float (&color)[4]) {
    g.lookFeather.store(feather, std::memory_order_relaxed);
    g.lookRadius.store(cornerRadius, std::memory_order_relaxed);
    g.lookBorder.store(border, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i)
        g.lookColor[i].store(color[i], std::memory_order_relaxed);
}

int corner_way() { return g.context ? g.way.load(std::memory_order_relaxed) : 0; }

CornerStats corner_stats() { return g.stats; }

void corner_session_end() {
    if (!g.context)
        return;
    ch::uninstall();
    simqvfr::logf("scs corner: at session end: %llu frames, %llu with the eyes in the corner",
                  static_cast<unsigned long long>(g.stats.frames), static_cast<unsigned long long>(g.stats.cornerFrames));
    release_all();  // the session's swapchains go with it (ATS makes new ones when its AA setting changes)
}

}  // namespace scs

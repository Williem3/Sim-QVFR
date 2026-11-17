// QVFR for SCS Software's games at the D3D11 level (corner.h): the D3D11 observers that put each eye in the corner,
// the frame's end, install and session end. The parts: corner_internal.h.
#include "corner_internal.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../dev_build.h"
#include "../../engine/nvapi_stereo.h"
#include "../lmu/mvr_probe.h"
#include "../../log.h"
#include "../../probe/aftermath.h"
#include "../../probe/hang_watch.h"
#include "corner_rules.h"
#include "pass_probe.h"

namespace scs {
namespace detail {

State g;

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

void take_frame_end() {
    const uint64_t ends = g.endFrames.load(std::memory_order_acquire);
    if (ends == g.seenEndFrames)
        return;
    g.seenEndFrames = ends;
    if (g.gpuTimes)
        gpu_times_frame_end();  // before the frame count moves on: the slot is this frame's
    mark(kMarkFrameEnd, 0);
    g.frameDraw = 0;
    ++g.stats.frames;
    if (g.stats.frames % kEvictFrames == 0)
        evict_unused();
    g.stats.cornerFrames += g.thisFrameCornered ? 1 : 0;
    if (!g.replayLogFrame && g.stats.cornerFrames == 300)
        g.replayLogFrame = g.stats.frames + 1;  // a frame well into driving: its replays named once
    g.frameReplay = 0;
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
    if (g.stats.frames % kEvictFrames == 0)
        evict_mirrors();  // after our copies were swapped back out (above)
    release_orphans();    // every frame: what the game let go since the last
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

void on_targets(ID3D11DeviceContext*, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (session_ending())
        return;
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
        if (g.gpuTimes)
            gpu_pass_begin();
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
    if (session_ending() || (g.thread && GetCurrentThreadId() != g.thread))
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
    if (session_ending() || (g.thread && GetCurrentThreadId() != g.thread))
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

void on_draw(ID3D11DeviceContext*, ch::Entry entry, UINT count, UINT) {
    g.instancedDraw = false;
    if (session_ending() || !g.active || GetCurrentThreadId() != g.thread)
        return;
    if (entry == ch::kDrawIndexed || entry == ch::kDrawIndexedInstanced) {
        const bool focusDraw = focus_on() && !g.diagNoDraws && (g.mvr || g.instanced) && g.depthBound && g.fullSize;
        mark(kMarkDraw, (++g.frameDraw & 0xFFFFFF) | 1u << 24 | (focusDraw ? 1u << 25 : 0));
        if (focusDraw) {
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
    mark(kMarkDraw, ++g.frameDraw & 0xFFFFFF);
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
    if (focus_on() && !g.diagNoDraws && (g.mvr || g.instanced) && g.boundSwapchain && count != 3 && entry == ch::kDraw) {
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
    // The ranges on_draw rewrote, for the replay's log and timing (the put-backs clear them).
    const UINT vsFirst = g.vs.first, vsCount = g.vs.count, psFirst = g.ps.first, psCount = g.ps.count;
    if (g.vs.game || g.ps.game) {
        put_back(g.vs, false);
        put_back(g.ps, true);
    }
    // After the put-backs: what this draw's on_draw changed goes back even if the session ended in between.
    if (session_ending())
        return;
    // The focus view's turn: a full-screen triangle into a full-size target of the eye's. The quarter-size exposure and
    // bloom chain stays the eye's alone, and so does the half-size ambient occlusion chain (S6 run 2): its search
    // reaches up to a quarter of the eye's screen, most of a focus view's width, and the shader drops every sample
    // past its view's edge, so a focus view's own occlusion came out lighter and skewed towards its edges, differently
    // with every scene. The full-size upsample that follows is replayed, reading the eye's half-size occlusion and
    // depth through the box (twin_of fills a twin from the eye where the region has no content), against the focus
    // view's own full-size depth. Ctrl+Alt+A: the focus view's own occlusion again, live.
    if (entry == ch::kDraw && count == 3 && focus_on() && g.active && g.cut &&
        (g.ownOcclusion ? g.boundW * 2 + 2 >= g.eyeW : g.boundW == g.eyeW) && GetCurrentThreadId() == g.thread) {
        const unsigned n = ++g.frameReplay;
        const bool large = psCount > kLargeRange;
        const bool skip = g.diagNoReplays || (n <= 64 && ((g.replaySkip >> (n - 1)) & 1)) || (large && g.diagSkipLarge);
        if (large) {
            if (g.diagDumpLarge)
                dump_large_range(n);
            ++g.largeReplays;
            g.largeSkipped += skip ? 1 : 0;
            if (g.largeLogged < 12) {
                ++g.largeLogged;
                simqvfr::logf("scs focus: a large replay at frame %llu: replay %u of the frame into %ux%u, vertex range %u+%u, "
                              "pixel range %u+%u%s", static_cast<unsigned long long>(g.stats.frames), n, g.boundW, g.boundH,
                              vsFirst, vsCount, psFirst, psCount, skip ? ": SKIPPED" : "");
            }
        }
        if (g.stats.frames == g.replayLogFrame)
            simqvfr::logf("scs focus: replay %u of the frame: into %ux%u%s, vertex range %u+%u, pixel range %u+%u%s", n,
                          g.boundW, g.boundH, g.boundSwapchain ? " (the headset's image)" : "", vsFirst, vsCount, psFirst,
                          psCount, skip ? ": SKIPPED (scs_focus_replays / scs_replay_skip)" : "");
        if (!skip) {
            const Timed t(State::kTimeReplays);
            if (g.gpuTimes)
                gpu_replay_begin(psFirst, psCount);
            mark(kMarkReplay, n << 16 | std::min<UINT>(psCount, 0xFFFF));
            replay(count, start);  // into the headset's image too: its final blit
            mark(kMarkReplayDone, n << 16);
            if (g.gpuTimes)
                gpu_replay_end();
        }
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

// The game's own state back, on the render thread between its calls (inside an observer, or under a ch::Bypass):
// NVAPI's one view, its viewports and scissors as it set them, its constant buffers in our copies' place. Session end
// can come mid-frame, and no xrEndFrame may follow (flat mode, quit) to do it at a frame's end.
void restore_game_state() {
    g.active = false;
    g.activeNow.store(false, std::memory_order_relaxed);
    two_views(false);
    g.viewsDual = false;
    apply_views();  // not active: the game's own
    swap_bound(false);
}

constexpr uint64_t kRestoreMs = 2000;  // the render thread's next observed call, waited for this long at most

// At each observer's start. False: the session goes on. True: it ended (corner_session_end) and the observer returns
// at once (the game's call passes on as it is). The render thread's first call after it puts the game's state back
// and stops observing. Past kRestoreMs without one (the render thread no longer drawing), a call on another thread
// stops observing without it; end_now settles one still asked for.
bool session_ending() {
    const int s = g.restore.load(std::memory_order_acquire);
    if (s == 0)
        return false;
    if (s != 1)
        return true;
    const bool render = g.thread && GetCurrentThreadId() == g.thread;
    if (!render && GetTickCount64() < g.restoreDeadline.load(std::memory_order_relaxed))
        return true;
    int asked = 1;
    if (!g.restore.compare_exchange_strong(asked, 2, std::memory_order_acq_rel))
        return true;  // another thread took it
    if (render)
        restore_game_state();  // our calls pass straight through (inside an observer)
    ch::uninstall();
    simqvfr::logf("scs corner: after the session's end: %s, observing stopped (thread %lu)",
                  render ? "the game's views, viewports and constant buffers put back" : "no render-thread call came: "
                  "nothing put back", GetCurrentThreadId());
    g.restore.store(3, std::memory_order_release);
    return true;
}

// Everything tied to the device and the session, released and reset: at session end, and on every way out of
// install_now that leaves QVFR off (a second session may come on another thread or another device; every resource of
// ours keeps the device alive). What other threads set (the gaze, the live sizes, the look), the player's toggles and
// the running counts stay. On the thread that ends the frames (service_lifecycle); the context's hooks are off first.
void release_all() {
    if (g.context)
        two_views(false);
    gpu_times_reset();
    simqvfr::aftermath::detach();
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
    device_watch_reset();
    g.device.Reset();
    g.restore.store(0, std::memory_order_release);  // the next session's end asks again
}

}  // namespace detail

using namespace detail;

namespace {

// Session start and end come on the game's main thread (xrCreateSession, xrDestroySession) while its render thread
// keeps drawing on the same immediate context, which isn't thread-safe: the install's probing (a Flush, a command of
// ours, multithread protection switched on and off) and the end's release of everything the observers use would run
// beside the render thread's own commands (ATS 2026-11-15 06:57: three session restarts in 13 s, then a freeze inside
// the runtime's xrEndFrame). So both are requests, done by the thread that ends the frames, at its next xrEndFrame
// (service_lifecycle): between its commands, with none of our code running beside it.
struct Lifecycle {
    std::mutex mutex;
    bool end = false;      // a session ended: release everything
    bool install = false;  // a session started: install on `device`
    ComPtr<ID3D11Device> device;
    int percent = 0, focusPercent = 0;
} lifecycle;

void end_now() {
    // The restore session end asked for, if no observed call took it: here (on the render thread, the game's state put
    // back between its calls; elsewhere not touched), and observing stopped before anything is released.
    int asked = 1;
    if (g.restore.compare_exchange_strong(asked, 2, std::memory_order_acq_rel)) {
        if (g.thread && GetCurrentThreadId() == g.thread) {
            const ch::Bypass bypass;  // our calls straight on, past the still-installed observers
            restore_game_state();
        }
        ch::uninstall();
        g.restore.store(3, std::memory_order_release);
    }
    // One being done inside an observer on another thread: a few calls; waited for (bounded) so the release doesn't
    // pull the context away under it.
    for (int i = 0; i < 500 && g.restore.load(std::memory_order_acquire) == 2; ++i)
        Sleep(1);
    ch::uninstall();  // (again: a no-op unless that wait ran out)
    simqvfr::logf("scs corner: at session end: %llu frames, %llu with the eyes in the corner",
                  static_cast<unsigned long long>(g.stats.frames), static_cast<unsigned long long>(g.stats.cornerFrames));
    release_all();  // the session's swapchains go with it (ATS makes new ones when its AA setting changes)
}

bool install_now(ID3D11Device* device, int percent, int focusPercent) {
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
    simqvfr::hang_watch_on_fire(&hang_check);  // a frame stuck inside the runtime still logs the device's state
    if (simqvfr::aftermath::attach(device, g.context.Get()))  // [experiment] aftermath=1 (dev builds with the SDK)
        simqvfr::aftermath::set_marker_names(&marker_name);
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

}  // namespace

void detail::service_lifecycle() {
    bool end = false, install = false;
    ComPtr<ID3D11Device> device;
    int percent = 0, focusPercent = 0;
    {
        std::lock_guard lock(lifecycle.mutex);
        if (!lifecycle.end && !lifecycle.install)
            return;
        end = lifecycle.end;
        install = lifecycle.install;
        device = std::move(lifecycle.device);
        percent = lifecycle.percent;
        focusPercent = lifecycle.focusPercent;
        lifecycle.end = lifecycle.install = false;
        g.lifecyclePending.store(false, std::memory_order_release);
    }
    if (end && g.context) {
        end_now();
        simqvfr::logf("scs corner: the session's end done on thread %lu, at its first xrEndFrame after", GetCurrentThreadId());
    }
    if (install && device && !g.context && install_now(device.Get(), percent, focusPercent))
        simqvfr::logf("scs corner: installed on thread %lu, at the session's first xrEndFrame", GetCurrentThreadId());
}

bool corner_install(ID3D11Device* device, int percent, int focusPercent) {
    if (!device || percent <= 0 || percent >= 100)
        return false;
    std::lock_guard lock(lifecycle.mutex);
    lifecycle.install = true;
    lifecycle.device = device;
    lifecycle.percent = percent;
    lifecycle.focusPercent = focusPercent;
    g.lifecyclePending.store(true, std::memory_order_release);
    return true;
}

void corner_set_diag(bool noDraws, bool noReplays, uint64_t replaySkip, bool gpuTimes, bool skipLarge, bool dumpLarge) {
    g.gpuTimes = gpuTimes;
    g.diagSkipLarge = skipLarge;
    g.diagDumpLarge = dumpLarge;
    g.diagNoDraws = noDraws;
    g.diagNoReplays = noReplays;
    g.replaySkip = replaySkip;
}

bool corner_on() { return g.context != nullptr; }

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

// At xrDestroySession, usually on the game's main thread. The session can end mid-frame (the views doubled, the
// viewports cut, our copies bound) with no xrEndFrame after it (flat mode, quit), and the main thread mustn't touch the
// render thread's context: so the game's state is put back by the render thread's next observed call (session_ending),
// which then stops observing; until then every observer passes calls straight on. On the render thread itself: here,
// at once. The release still waits for the next xrEndFrame (service_lifecycle). An install not done yet is dropped.
void corner_session_end() {
    std::lock_guard lock(lifecycle.mutex);
    lifecycle.install = false;
    lifecycle.device.Reset();
    if (!g.context)
        return;
    lifecycle.end = true;
    g.restoreDeadline.store(GetTickCount64() + kRestoreMs, std::memory_order_relaxed);
    int none = 0;
    g.restore.compare_exchange_strong(none, 1, std::memory_order_acq_rel);  // once per session
    if (g.thread && GetCurrentThreadId() == g.thread) {  // between the render thread's own calls: no wait
        int asked = 1;
        if (g.restore.compare_exchange_strong(asked, 2, std::memory_order_acq_rel)) {
            {
                const ch::Bypass bypass;
                restore_game_state();
            }
            ch::uninstall();
            g.restore.store(3, std::memory_order_release);
            simqvfr::log_line("scs corner: session end on the render thread: the game's state put back, observing stopped");
        }
    }
    g.lifecyclePending.store(true, std::memory_order_release);
}

}  // namespace scs

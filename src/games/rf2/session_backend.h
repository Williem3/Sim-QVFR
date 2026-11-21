#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <d3d11.h>

#include "../../log.h"
#include "backend_stats.h"
#include "compositor.h"
#include "draw_backend.h"
#include "gpu_timing.h"
#include "lifecycle_backend.h"
#include "observer_backend.h"
#include "pass_state.h"
#include "regulator.h"
#include "render_backend.h"
#include "resolve_backend.h"
#include "target_backend.h"

namespace simqvfr::games::rf2 {

// The hook layer's switch for NVIDIA's two-view mode on a context (single-pass stereo through NVAPI): 0 on success.
using TwoViewsFn = int (*)(ID3D11DeviceContext*, bool);

// Owns the RF2-specific state and is the one entry surface for the generic D3D11 hook layer
// (src/games/lmu/mvr_pass.cpp). Each method is the body of one thin call site: target bind,
// draw, resolve, frame end, OpenXR FOV. Every hook-side input (tracked context, replay flag,
// stage bindings, twin lookup, real-call wrappers) is passed in, so this header never touches
// the hook layer's state. Member construction order preserves the previous namespace-scope
// object order.
struct SessionBackend {
    Observer observer;
    PassState pass;
    Compositor compositor;
    BackendStats stats;
    RemapBuffer remap;
    GpuTiming gpu;  // log-only GPU time of the eye passes, the resolves and the compositions (gpu_timing.h)
    AtlasLayout atlas;  // in use this frame (render thread): where the outer and focus views sit in the scene target

    // The GPU regulator takes each timed frame's GPU time from the timing (gpu_timing.h) on the render thread.
    SessionBackend() {
        gpu.set_sink([this](double ms) { on_gpu_sample(ms); });
    }

    // OMSetRenderTargets / OMSetRenderTargetsAndUnorderedAccessViews, before the real call.
    void note_target_bind(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool replaying,
                          UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
        if (context != tracked || replaying || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
        const bool scene = n && rtvs && rtvs[0] && is_full_scene_target(rtvs[0]);
        const bool wasOpen = pass.fullSceneOpen;
        if (scene && dsv && !pass.fullSceneOpen && pass.fullScenePassInFrame == 0) {
            latch_atlas();  // one layout a frame
            gpu.frame_open(context);
        }
        pass.note_full_scene_bind(scene && dsv);
        if (!wasOpen && pass.fullSceneOpen) gpu.pass_open(context, pass.fullScenePassInFrame);
        else if (wasOpen && !pass.fullSceneOpen) gpu.pass_close(context, pass.fullScenePassInFrame);
        if (!pass.real_eye_pass()) two_views_off(context);  // the draws from here are not an eye pass's
        observer.note_target_bind(context, scene, dsv != nullptr);
    }

    // Every draw-family hook, before the draw is handled.
    template <class HasTwin>
    void note_draw(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool replaying,
                   const StageBindings& stage, const GUID& twinGuid, HasTwin&& hasTwin) {
        if (context != tracked || replaying) return;
        observer.note_draw(context, stage, twinGuid, hasTwin);
    }

    // A draw the hook layer cannot double (DrawAuto, indirect draws).
    void note_unsupported_draw(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool replaying) {
        if (context != tracked || replaying) return;
        two_views_off(context);  // drawn as the game made it: one view
        if (pass.real_eye_pass()) stats.note_unsupported();
    }

    // A game draw in an eye pass, drawn into the outer + focus atlas: by NVIDIA's single-pass stereo (the two-view mode
    // on, the NVAPI twin bound, one draw: `drawOnce`) when use_single_pass_stereo() was called, else by instancing (the
    // instanced twin, the draw with twice its instances: `drawDoubled`). `onRefused()`: single-pass stereo was refused
    // by the driver; true if instancing can take over (the rest of the session, this draw too).
    // True: handled. False: the caller makes the game's own draw.
    template <class SetShader, class SetViewports, class SetScissors, class DrawDoubled, class DrawOnce, class OnRefused>
    bool draw_eye(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool replaying,
                  const GUID& instancedGuid, const GUID& singlePassGuid, SetShader&& setShader,
                  SetViewports&& setViewports, SetScissors&& setScissors, DrawDoubled&& drawDoubled,
                  DrawOnce&& drawOnce, OnRefused&& onRefused) {
        if (context != tracked || replaying || !pass.real_eye_pass()) return false;
        stats.note_draw();
        const unsigned passIndex = pass.fullScenePassInFrame;
        const int eye = pass.eye_for_current_pass();
        const float* eyeRemap = observer.fovRemap[eye];
        stats.log_cb13(passIndex, eye, eyeRemap, observer.focusRect[eye]);

        if (single_pass_stereo()) {
            const DrawResult result =
                try_draw(context, singlePassGuid, eyeRemap, remap, atlas, setShader, setViewports, setScissors,
                         drawOnce, [&] { return set_two_views(context, true); });
            if (result == DrawResult::handled) {
                stats.note_doubled();
                return true;
            }
            if (result == DrawResult::no_twin) {
                set_two_views(context, false);  // the game's own draw: one view
                stats.note_no_twin();
                return false;
            }
            // Refused: the driver would not switch to two views. Instancing takes over, or the draw stays plain.
            if (!spsBroken_ || !onRefused()) {
                stats.note_no_twin();
                return false;
            }
        }

        const DrawResult result = try_draw(context, instancedGuid, eyeRemap, remap, atlas, setShader, setViewports,
                                           setScissors, drawDoubled, [] { return true; });
        if (result == DrawResult::handled) {
            stats.note_doubled();
            return true;
        }
        stats.note_no_twin();
        return false;
    }

    // NVIDIA's multi-view rendering (single-pass stereo with a full second position, through NVAPI): the eye passes'
    // draws are made once, with the game shader's NVAPI twin bound and the two-view mode on, instead of twice as
    // instances. The hook layer passes its mode switch; without this call (or null) instancing draws the views.
    void use_single_pass_stereo(TwoViewsFn twoViews) { twoViews_ = twoViews; }
    bool single_pass_stereo() const { return twoViews_ != nullptr && !spsBroken_; }

    // The mode is on only while an eye pass's draws are made: off when the pass is left, before the scene resolves
    // (the composition draws after it), around a command list and a draw the hooks cannot double.
    void note_execute(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool replaying) {
        if (context == tracked && !replaying) two_views_off(context);
    }

    // ResolveSubresource, before the real call: the scene's resolve is counted for the observer.
    void note_resolve(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool replaying,
                      ID3D11Resource* src) {
        if (context == tracked && !replaying) two_views_off(context);  // before the resolve and the composition after it
        if (gpu.enabled() && context == tracked && !replaying && is_scene_resource(src)) gpu.resolve_begin(context);
        if (context != tracked || replaying || !observer.candidate796 || !is_scene_resource(src)) return;
        observer.note_scene_resolve();
    }

    // ResolveSubresource, after the real call: compose the finished eye (outer + focus).
    // `replaying` must stay a reference: Compositor::compose() sets it to shield its own draws
    // from the hooks.
    void note_resolved(ID3D11DeviceContext* context, ID3D11DeviceContext* tracked, bool& replaying,
                       ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src) {
        const bool scene = gpu.enabled() && context == tracked && !replaying && is_scene_resource(src);
        if (scene) gpu.resolve_done(context);
        const ResolveResult result = handle_resolve(pass, compositor, stats, context, tracked, replaying, dst, dstSub, src,
                                                    observer.frame, observer.focusRect, atlas);
        if (scene && result.attempted) gpu.compose_done(context);
    }

    // [experiment] gpu_timing (layer side: adapter): 0 no timing log (default; the regulator still times when it is on), 1 the GPU
    // timing log, 2 also the passes' pipeline statistics.
    void set_gpu_timing(int mode) { gpu.set_mode(mode); }

    // Frame end.
    void end_frame(unsigned long long frame, uint64_t stereoErrors) {
        observer.end_candidate();
        frame_end(pass, stats, compositor, frame);
        observer.frame_end(frame, stereoErrors);
        if (frame > 0 && frame % 300 == 0)
            simqvfr::logf("rf2 multi-view: %s; two-view mode switches=%llu refused=%llu",
                          single_pass_stereo() ? "NVIDIA's single-pass stereo" : "instancing",
                          static_cast<unsigned long long>(spsSwitches_), static_cast<unsigned long long>(spsRefused_));
    }

    // The sizes of the two views (the ini's periphery_scale and focus density, as shares of the native eye; layer side:
    // qvfr_set_density), live and at the start. Takes effect with the next frame's first scene pass, so one frame never
    // mixes two layouts. The focus view's size sets the pixel density in the focus area, the outer view's the periphery's.
    void set_density(double outerScale, double focusW, double focusH) {
        std::lock_guard<std::mutex> lock(atlasMutex_);
        baseOuter_ = outerScale;
        baseFocusW_ = focusW;
        baseFocusH_ = focusH;
        baseSet_ = true;
        refresh_limits_locked();
        apply_atlas_locked(nullptr);
    }

    // The GPU regulator's settings ([regulator], layer side: qvfr_set_regulator), live and at the start, with the set focus
    // density as a share (1.0 = 100%): the density the regulator works down from. The set periphery and density are the
    // ceiling; the regulator only lowers the views below them and brings them back, and writes nothing.
    void set_regulator(const GpuRegulatorSettings& settings, double density) {
        std::lock_guard<std::mutex> lock(atlasMutex_);
        const GpuRegulatorSettings s = clamp_regulator(settings);
        const bool changed = !regSet_ || !(s == reg_.settings());
        baseDensity_ = density * 100.0;
        reg_.set_settings(s);
        refresh_limits_locked();
        gpu.set_need(reg_.active());
        regSet_ = true;
        if (changed)
            simqvfr::logf("rf2 regulator: focus density %s, periphery %s; steps down above %.1f ms and up below %.1f ms of GPU time, "
                          "1%% down after %d frames over, 1%% up after %d frames under (floors: periphery %d%%, density %d%%)",
                          s.focus ? "on" : "off", s.periphery ? "on" : "off", s.decreaseAbove / 10.0, s.increaseBelow / 10.0,
                          s.lowerFrames, s.raiseFrames, kRegulatorPeripheryFloor, kRegulatorDensityFloor);
        apply_atlas_locked(nullptr);
    }

    // The regulator as it stands (any thread; layer side: qvfr_regulator_status, the overlay's row): false while none runs. The
    // views' sizes are the set ones less the cuts, as apply_atlas_locked makes them.
    bool regulator_status(bool& focus, bool& periphery, double& peripheryPercent, double& densityPercent, int& cutPeriphery,
                          int& cutDensity) {
        std::lock_guard<std::mutex> lock(atlasMutex_);
        if (!baseSet_ || !reg_.active()) return false;
        focus = reg_.settings().focus;
        periphery = reg_.settings().periphery;
        cutPeriphery = reg_.periphery_cut();
        cutDensity = reg_.density_cut();
        peripheryPercent = std::max(10.0, baseOuter_ * 100.0 - cutPeriphery);
        densityPercent = std::max(0.0, baseDensity_ - cutDensity);
        return true;
    }

    // The focus area's look and place, live and at the start (layer side: qvfr_set_focus_look): the feather band, corner
    // radius and outline are the composition's (compositor.h); `offsetDegrees` moves the focus box up (+) or down (-)
    // from the eye's forward axis (the ini's fixed_offset_y), which recomputes the rect and the clip remap at once.
    void set_focus_look(double feather, double cornerRadius, bool outline, const float (&outlineColor)[4],
                        double offsetDegrees) {
        compositor.set_look(feather, cornerRadius, outline, outlineColor);
        offsetDegrees = std::clamp(offsetDegrees, -80.0, 80.0);
        std::lock_guard<std::mutex> lock(focusMutex_);
        if (offsetSet_ && offsetDegrees == offsetY_) return;
        simqvfr::logf("rf2 focus: offset %.0f deg %s (was %.0f)", offsetDegrees < 0 ? -offsetDegrees : offsetDegrees,
                      offsetDegrees > 0 ? "up" : offsetDegrees < 0 ? "down" : "(on the forward axis)", offsetY_);
        offsetSet_ = true;
        offsetY_ = offsetDegrees;
        fovLogCounter_ = 0;  // the mapping below is logged at once
        if (haveTan_) apply_focus_locked();
    }

    // OpenXR FOV tangents (left, right, down, up per eye) -> per-eye focus rect and clip remap.
    void note_fov(const float (&tanFov)[2][4]) {
        std::lock_guard<std::mutex> lock(focusMutex_);
        std::memcpy(lastTan_, tanFov, sizeof(lastTan_));
        haveTan_ = true;
        apply_focus_locked();
    }

    // The focus box's share of the eye's field of view per axis (the ini's focus_percent, live and at the start;
    // layer side: qvfr_set_live). The focus view's image stays the atlas tile's size, so the share also sets the
    // pixel density in the focus area (a smaller share is denser). Recomputes the rect and the clip remap at once.
    void set_focus_fraction(double fractionX, double fractionY) {
        fractionX = std::clamp(fractionX, 0.10, 1.0);
        fractionY = std::clamp(fractionY, 0.10, 1.0);
        std::lock_guard<std::mutex> lock(focusMutex_);
        if (focusSet_ && fractionX == fx_ && fractionY == fy_) return;
        simqvfr::logf("rf2 focus: box %.1f%% x %.1f%% of the eye's field of view (was %.1f%% x %.1f%%)",
                      fractionX * 100, fractionY * 100, fx_ * 100, fy_ * 100);
        focusSet_ = true;
        fx_ = fractionX;
        fy_ = fractionY;
        fovLogCounter_ = 0;  // the mapping below is logged at once
        if (haveTan_) {
            apply_focus_locked();
            return;
        }
        // No FOV yet: the centred box and its remap until the first one arrives.
        for (int eye = 0; eye < 2; ++eye) {
            const float remap[4] = {static_cast<float>(1.0 / fx_), 0.0f, static_cast<float>(1.0 / fy_), 0.0f};
            const float rect[4] = {static_cast<float>(0.5 - fx_ / 2), static_cast<float>(0.5 - fy_ / 2),
                                   static_cast<float>(0.5 + fx_ / 2), static_cast<float>(0.5 + fy_ / 2)};
            std::memcpy(observer.fovRemap[eye], remap, sizeof(remap));
            std::memcpy(observer.focusRect[eye], rect, sizeof(rect));
        }
    }

private:
    // The first scene pass of a frame (render thread): the layout set since the last frame comes into use.
    void latch_atlas() {
        std::lock_guard<std::mutex> lock(atlasMutex_);
        if (!atlasDirty_) return;
        atlas = pendingAtlas_;
        atlasDirty_ = false;
    }

    // NVIDIA's two-view mode on the tracked context (render thread only). Switched only when it changes.
    bool set_two_views(ID3D11DeviceContext* context, bool on) {
        if (!twoViews_) return !on;
        if (spsOn_ == on) return true;
        if (twoViews_(context, on) != 0) {
            ++spsRefused_;
            if (on) {
                spsBroken_ = true;  // instancing from now on, if it can
                simqvfr::log_line("rf2 multi-view: NVIDIA's single-pass stereo was refused by the driver");
            } else {
                simqvfr::log_line("rf2 multi-view: the two-view mode would not switch off");
            }
            return false;
        }
        spsOn_ = on;
        ++spsSwitches_;
        return true;
    }
    void two_views_off(ID3D11DeviceContext* context) {
        if (spsOn_) set_two_views(context, false);
    }

    TwoViewsFn twoViews_ = nullptr;
    bool spsOn_ = false, spsBroken_ = false;
    uint64_t spsSwitches_ = 0, spsRefused_ = 0;

    // One timed frame's GPU time (render thread). A regulator step changes the pending layout, taken up at the next frame's
    // first scene pass like any other change.
    void on_gpu_sample(double ms) {
        std::lock_guard<std::mutex> lock(atlasMutex_);
        if (!reg_.feed(ms)) return;
        char why[96];
        snprintf(why, sizeof(why), "GPU %.1f ms %s %.1f for %d frames", reg_.last_mean_ms(),
                 reg_.last_direction() > 0 ? "over" : "under",
                 (reg_.last_direction() > 0 ? reg_.settings().decreaseAbove : reg_.settings().increaseBelow) / 10.0,
                 reg_.last_direction() > 0 ? reg_.settings().lowerFrames : reg_.settings().raiseFrames);
        apply_atlas_locked(why);
    }

    // Under atlasMutex_: how far each axis can go down from its set value, from the set values and the floors.
    void refresh_limits_locked() {
        reg_.set_limits(static_cast<int>(baseOuter_ * 100.0 + 0.5) - kRegulatorPeripheryFloor,
                        static_cast<int>(baseDensity_ + 0.5) - kRegulatorDensityFloor);
    }

    // Under atlasMutex_: the layout of the set sizes less the regulator's cuts, pending for the next frame. `why`: what made the
    // regulator step (logged), null otherwise.
    void apply_atlas_locked(const char* why) {
        if (!baseSet_) return;
        const int cutP = reg_.periphery_cut(), cutD = reg_.density_cut();
        double outer = baseOuter_, focusW = baseFocusW_, focusH = baseFocusH_;
        if (cutP) outer = std::max(0.10, outer - cutP / 100.0);
        if (cutD && baseDensity_ > 0) {
            const double factor = std::max(0.0, baseDensity_ - cutD) / baseDensity_;
            focusW *= factor;
            focusH *= factor;
        }
        const AtlasLayout next = make_atlas_layout(outer, focusW, focusH);
        if (atlasSet_ && next == pendingAtlas_) return;
        const bool limited = next.focusW != static_cast<UINT>(focusW * kSceneWidth + 0.5) ||
                             next.outerW + 1 < static_cast<UINT>(outer * kSceneWidth);
        char regulated[160] = "";
        if (cutP || cutD || why)
            snprintf(regulated, sizeof(regulated), " [regulator%s%s: periphery -%d, density -%d]", why ? ": " : "", why ? why : "",
                     cutP, cutD);
        // A regulator step is logged every fifth percent (and when back at the set values), not each: it steps up to three times a second.
        const bool quiet = why && (cutP % 5 || cutD % 5);
        if (!quiet)
            simqvfr::logf("rf2 atlas: outer %ux%u (periphery %.0f%% of the native eye), focus %ux%u at x=%u (%.0f%% x %.0f%% "
                          "of it)%s%s",
                          next.outerW, next.outerH, outer * 100, next.focusW, next.focusH, next.focus_x(), focusW * 100,
                          focusH * 100, limited ? " (limited to fit the target)" : "", regulated);
        atlasSet_ = true;
        pendingAtlas_ = next;
        atlasDirty_ = true;
    }

    std::mutex atlasMutex_;
    AtlasLayout pendingAtlas_;
    bool atlasSet_ = false, atlasDirty_ = false;
    // Under atlasMutex_: the set sizes (set_density; the density in percent from set_regulator) and the regulator.
    double baseOuter_ = 0, baseFocusW_ = 0, baseFocusH_ = 0, baseDensity_ = 0;
    bool baseSet_ = false, regSet_ = false;
    Regulator reg_;

    // Under focusMutex_: the FOV and the box's share -> the rect and the remap the draws and the composition use.
    void apply_focus_locked() {
        FocusMapping mapping{};
        if (!calculate_focus_mapping(lastTan_, fx_, fy_, mapping, offsetY_)) return;
        std::memcpy(observer.fovRemap, mapping.remap, sizeof(mapping.remap));
        std::memcpy(observer.focusRect, mapping.rect, sizeof(mapping.rect));
        if (fovLogCounter_++ % 600 != 0) return;
        const auto& r = observer.focusRect;
        const auto& m = observer.fovRemap;
        simqvfr::logf("rf2 OFFAXIS FOCUS: L rect %.6f %.6f %.6f %.6f remap %.6f %.6f %.6f %.6f; "
                      "R rect %.6f %.6f %.6f %.6f remap %.6f %.6f %.6f %.6f",
                      r[0][0], r[0][1], r[0][2], r[0][3], m[0][0], m[0][1], m[0][2], m[0][3],
                      r[1][0], r[1][1], r[1][2], r[1][3], m[1][0], m[1][1], m[1][2], m[1][3]);
    }

    std::mutex focusMutex_;
    float lastTan_[2][4]{};
    bool haveTan_ = false, focusSet_ = false, offsetSet_ = false;
    double offsetY_ = 0;  // degrees up of the forward axis (set_focus_look)
    double fx_ = kDefaultFocusFraction, fy_ = kDefaultFocusFraction;
    unsigned fovLogCounter_ = 0;
};

}  // namespace simqvfr::games::rf2

#pragma once

// RF2's QVFR at the D3D11 level, fed by the engine's context hooks (engine/context_hooks.h; adapter.cpp installs them):
// each eye pass's draws draw the eye's outer view and its focus view side by side in the eye's own scene target, with
// NVIDIA's multi-view rendering (the game shader's NVAPI twin, one draw) or by instancing (the instanced twin, twice the
// instances); at the eye's resolve the two are composed into the whole resolved target (compositor.h), so the game's
// post-processing sees a normal eye image. Shader twins: LMU's shader probe (lmu/mvr_probe.h), as ATS and ETS2 use it.
//
// Threads: the observers (targets ... resolved) run on the game's render thread. The ini's sizes, focus box and look,
// the FOV and the regulator's settings arrive from the layer's threads under the two locks; the render thread takes
// them up at the first scene pass of each frame, so one frame never mixes two layouts or two focus boxes.

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../../engine/context_hooks.h"
#include "../../engine/nvapi_stereo.h"
#include "../../log.h"
#include "../../probe/gpu_load.h"
#include "../lmu/mvr_probe.h"
#include "backend_stats.h"
#include "census.h"
#include "compositor.h"
#include "draw_backend.h"
#include "gpu_timing.h"
#include "pass_state.h"
#include "../../quad/gpu_regulator.h"
#include "render_backend.h"

namespace simqvfr::games::rf2 {

namespace ch = simqvfr::context_hooks;

// How the eye passes draw their two views.
enum class MultiView { none, nvidia, instancing };

class Renderer {
public:
    // The GPU regulator takes each timed frame's GPU time from the timing (gpu_timing.h) on the render thread.
    Renderer() { gpu_.set_sink([this](double ms) { on_gpu_sample(ms); }); }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ---- Set-up (the layer's threads) ----

    // The backend the device offers (lmu::mvr_probe_backend; [experiment] multi_view_software=1 asks for instancing),
    // and whether instancing can take over if NVIDIA's two views are refused at a draw.
    void set_backend(MultiView backend, bool canInstance) {
        backend_ = backend;
        canInstance_ = canInstance;
        simqvfr::logf("rf2 multi-view: %s (instancing %s)",
                      backend == MultiView::nvidia       ? "NVIDIA's multi-view rendering"
                      : backend == MultiView::instancing ? "instancing"
                                                         : "none: QVFR off",
                      canInstance ? "available" : "not available");
    }
    // The eye size the game was offered (xrEnumerateViewConfigurationViews): its scene target is looked for at it.
    void set_eye_size(uint32_t width, uint32_t height) {
        eyeW_.store(width);
        eyeH_.store(height);
    }
    void set_census(bool on) { census_.set_on(on); }
    // [experiment] gpu_timing: 0 no timing log (default; the regulator still times when it is on), 1 the GPU timing log,
    // 2 also the passes' pipeline statistics.
    void set_gpu_timing(int mode) { gpu_.set_mode(mode); }

    // The sizes of the two views (the ini's periphery_scale and focus density, as shares of the native eye; layer side:
    // qvfr_set_density), live and at the start. The focus view's size sets the pixel density in the focus area, the
    // outer view's the periphery's.
    void set_density(double outerScale, double focusW, double focusH) {
        std::lock_guard<std::mutex> lock(atlasMutex_);
        baseOuter_ = outerScale;
        baseFocusW_ = focusW;
        baseFocusH_ = focusH;
        baseSet_ = true;
        refresh_limits_locked();
        atlasDirty_ = true;
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
        gpu_.set_need(reg_.active());
        regSet_ = true;
        atlasDirty_ = true;
        if (changed)
            simqvfr::logf("rf2 regulator: focus density %s, periphery %s; steps down above %.1f ms of GPU time while the GPU "
                          "is busy, up below %.1f ms, 1%% down after %d frames over, 1%% up after %d frames under (floors: "
                          "periphery %d%%, density %d%%)",
                          s.focus ? "on" : "off", s.periphery ? "on" : "off", s.decreaseAbove / 10.0, s.increaseBelow / 10.0,
                          s.lowerFrames, s.raiseFrames, kRegulatorPeripheryFloor, kRegulatorDensityFloor);
    }

    // The regulator as it stands (any thread; layer side: qvfr_regulator_status, the overlay's row): false while none runs.
    bool regulator_status(bool& focus, bool& periphery, double& peripheryPercent, double& densityPercent, int& cutPeriphery,
                          int& cutDensity) {
        std::lock_guard<std::mutex> lock(atlasMutex_);
        if (!baseSet_ || !reg_.active())
            return false;
        focus = reg_.settings().focus;
        periphery = reg_.settings().periphery;
        cutPeriphery = reg_.periphery_cut();
        cutDensity = reg_.density_cut();
        peripheryPercent = std::max(10.0, baseOuter_ * 100.0 - cutPeriphery);
        densityPercent = std::max(0.0, baseDensity_ - cutDensity);
        return true;
    }

    // The focus area's look and place, live and at the start (layer side: qvfr_set_focus_look): the feather band, corner
    // radius and outline are the composition's; `offsetDegrees` moves the focus box up (+) or down (-) from the eye's
    // forward axis (the ini's fixed_offset_y).
    void set_focus_look(double feather, double cornerRadius, bool outline, const float (&outlineColor)[4],
                        double offsetDegrees) {
        compositor_.set_look(feather, cornerRadius, outline, outlineColor);
        offsetDegrees = std::clamp(offsetDegrees, -80.0, 80.0);
        std::lock_guard<std::mutex> lock(focusMutex_);
        if (offsetSet_ && offsetDegrees == offsetY_)
            return;
        simqvfr::logf("rf2 focus: offset %.0f deg %s (was %.0f)", std::fabs(offsetDegrees),
                      offsetDegrees > 0 ? "up" : offsetDegrees < 0 ? "down" : "(on the forward axis)", offsetY_);
        offsetSet_ = true;
        offsetY_ = offsetDegrees;
        focusLogs_ = 0;
        update_focus_locked();
    }

    // The focus box's share of the eye's field of view per axis (the ini's focus_percent, live and at the start; layer
    // side: qvfr_set_live). The focus view keeps its tile's size, so the share also sets the pixel density in the focus
    // area (a smaller share is denser).
    void set_focus(double fractionX, double fractionY) {
        fractionX = std::clamp(fractionX, 0.10, 1.0);
        fractionY = std::clamp(fractionY, 0.10, 1.0);
        std::lock_guard<std::mutex> lock(focusMutex_);
        if (focusSet_ && fractionX == fx_ && fractionY == fy_)
            return;
        simqvfr::logf("rf2 focus: box %.1f%% x %.1f%% of the eye's field of view (was %.1f%% x %.1f%%)", fractionX * 100,
                      fractionY * 100, fx_ * 100, fy_ * 100);
        focusSet_ = true;
        fx_ = fractionX;
        fy_ = fractionY;
        focusLogs_ = 0;
        update_focus_locked();
    }

    // OpenXR FOV tangents (left, right, down, up per eye), at every stereo xrLocateViews.
    void note_fov(const float (&tanFov)[2][4]) {
        std::lock_guard<std::mutex> lock(focusMutex_);
        if (haveTan_ && std::memcmp(lastTan_, tanFov, sizeof(lastTan_)) == 0)
            return;
        std::memcpy(lastTan_, tanFov, sizeof(lastTan_));
        haveTan_ = true;
        update_focus_locked();
    }

    // ---- The context hooks' observers (the game's render thread) ----

    void targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
        const bool scene = n && rtvs && rtvs[0] && dsv && scene_target(rtvs[0], true);
        const bool wasOpen = pass_.fullSceneOpen;
        if (scene && !pass_.fullSceneOpen && pass_.fullScenePassInFrame == 0) {
            latch();  // one layout and one focus box a frame
            gpu_.frame_open(c);
        }
        pass_.note_full_scene_bind(scene);
        if (!wasOpen && pass_.fullSceneOpen)
            gpu_.pass_open(c, pass_.fullScenePassInFrame);
        else if (wasOpen && !pass_.fullSceneOpen)
            gpu_.pass_close(c, pass_.fullScenePassInFrame);
        if (!pass_.real_eye_pass())
            two_views(c, false);  // the draws from here are not an eye pass's
        census_.note_target(c, n, rtvs, dsv, pass_.fullScenePassInFrame, pass_.real_eye_pass() ? pass_.eye_for_current_pass() : -1,
                            scene);
    }

    // Before a draw: an eye pass's draw gets its twin, the two viewports and the remap (put back in draw_after). The two
    // views are placed in the target whatever the game's own viewport (its depth range kept), as first measured; a draw
    // without a twin is the game's own.
    void draw(ID3D11DeviceContext* c, ch::Entry entry) {
        census_.note_draw();
        restore_ = Restore::none;
        if (!pass_.real_eye_pass() || !atlas_.valid() || backend_ == MultiView::none)
            return;
        stats_.note_draw();
        if (entry == ch::kDrawIndexedInstancedIndirect || entry == ch::kDrawInstancedIndirect) {
            two_views(c, false);  // drawn as the game made it: into the whole target (rare; counted)
            stats_.note_unsupported();
            return;
        }
        saved_.viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetViewports(&saved_.viewports, saved_.viewport);
        saved_.scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetScissorRects(&saved_.scissors, saved_.scissor);
        const D3D11_VIEWPORT v = saved_.viewports ? saved_.viewport[0] : D3D11_VIEWPORT{0, 0, float(atlas_.width), float(atlas_.height), 0, 1};
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        saved_.instanceCount = kMaxInstances;
        c->VSGetShader(&vs, saved_.instances, &saved_.instanceCount);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> twin;
        bool instancing = backend_ == MultiView::instancing;
        if (vs) {
            twin = twin_of(vs.Get(), instancing ? lmu::kMvrInstancedTwinGuid : lmu::kMvrTwinGuid);
            if (twin && !instancing && !two_views(c, true)) {  // refused: instancing for the rest of the session, if it can
                twin.Reset();
                if (canInstance_) {
                    backend_ = MultiView::instancing;
                    instancing = true;
                    simqvfr::log_line("rf2 multi-view: NVIDIA's two views refused by the driver: instancing from now on");
                    twin = twin_of(vs.Get(), lmu::kMvrInstancedTwinGuid);
                } else {
                    backend_ = MultiView::none;
                    simqvfr::log_line("rf2 multi-view: NVIDIA's two views refused by the driver and no instancing: QVFR off");
                }
            }
        }
        if (!twin) {  // the game's own draw, one view (counted: the census and the 300-frame line say how many)
            two_views(c, false);
            release_instances();
            stats_.note_no_twin();
            return;
        }
        saved_.cb13.Reset();
        c->VSGetConstantBuffers(13, 1, saved_.cb13.GetAddressOf());
        saved_.vs = vs;
        const DrawAtlas atlas = make_draw_atlas(v, atlas_);
        c->RSSetViewports(2, atlas.viewport);
        c->RSSetScissorRects(2, atlas.scissor);
        c->VSSetShader(twin.Get(), saved_.instanceCount ? saved_.instances : nullptr, saved_.instanceCount);
        const int eye = pass_.eye_for_current_pass();
        if (eye != remapEye_) {
            remap_.upload_and_bind(c, mapping_.remap[eye]);
            remapEye_ = eye;
        } else {
            remap_.bind(c);
        }
        instancedDraw_ = instancing;
        restore_ = Restore::all;
        stats_.note_doubled();
    }

    // An instancing draw (draw() bound the instanced twin): drawn here with twice its instances, each instance's
    // per-instance data read by both views (the input layout's twin, each step rate doubled: lmu/mvr_probe.h).
    bool draw_replace(ID3D11DeviceContext* c, ch::Entry entry, const ch::DrawCall& d) {
        if (!instancedDraw_)
            return false;
        instancedDraw_ = false;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout, twin;
        c->IAGetInputLayout(&layout);
        if (layout) {
            UINT size = sizeof(void*);
            IUnknown* stored = nullptr;
            if (SUCCEEDED(layout->GetPrivateData(lmu::kMvrInstancedLayoutGuid, &size, &stored)) && stored) {
                stored->QueryInterface(IID_PPV_ARGS(twin.GetAddressOf()));
                stored->Release();
            }
            if (twin)
                c->IASetInputLayout(twin.Get());
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
        return true;
    }

    // After a draw: what draw() changed put back.
    void draw_after(ID3D11DeviceContext* c) {
        instancedDraw_ = false;
        if (restore_ == Restore::none)
            return;
        if (restore_ == Restore::all) {
            c->VSSetShader(saved_.vs.Get(), saved_.instanceCount ? saved_.instances : nullptr, saved_.instanceCount);
            ID3D11Buffer* cb = saved_.cb13.Get();
            c->VSSetConstantBuffers(13, 1, &cb);
            saved_.vs.Reset();
            saved_.cb13.Reset();
            release_instances();
        }
        if (saved_.viewports)
            c->RSSetViewports(saved_.viewports, saved_.viewport);
        if (saved_.scissors)
            c->RSSetScissorRects(saved_.scissors, saved_.scissor);
        restore_ = Restore::none;
    }

    void execute(ID3D11DeviceContext* c) {
        two_views(c, false);  // the mode is on only around an eye pass's own draws
        remapEye_ = -1;       // the list may have bound its own b13
        census_.note_execute();
    }

    void resolve(ID3D11DeviceContext* c, ID3D11Resource* src) {
        two_views(c, false);  // before the resolve and the composition after it
        if (scene_resource(src))
            gpu_.resolve_begin(c);
    }

    // After a resolve: an eye's scene resolved is composed into the destination.
    void resolved(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, DXGI_FORMAT format) {
        const bool scene = scene_resource(src);
        int composed = -1;
        if (scene) {
            gpu_.resolve_done(c);
            const int eye = pass_.consume_composite_eye(frame_.load());
            if (eye >= 0 && atlas_.valid()) {
                const bool ok = compositor_.compose(c, dst, dstSub, format, eye, mapping_.rect[eye], atlas_);
                stats_.note_composite(ok);
                gpu_.compose_done(c);
                composed = ok ? eye : -1;
            }
        }
        census_.note_resolve(dst, src, scene, composed);
    }

    // A target bind on another immediate context (context_hooks::Observers::other_targets): the game's renderer is there
    // when it binds the eye's scene target, and the hooks follow it.
    void other_targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
        if (!n || !rtvs || !rtvs[0] || !dsv || !scene_target(rtvs[0], false))
            return;
        if (ch::retarget(c)) {
            spsOn_ = false;  // the two-view mode was the other context's
            remapEye_ = -1;
            simqvfr::logf("rf2: the eye's scene target is drawn on context %p: QVFR follows it", static_cast<void*>(c));
        }
    }

    // Every xrEndFrame (the layer's frame_end).
    void end_frame() {
        const unsigned long long frame = frame_.fetch_add(1) + 1;
        pass_.frame_end();
        stats_.log_frame(frame, compositor_.frames(0), compositor_.frames(1));
        census_.frame_end(frame);
        if (!target_.valid() && frame == 900 && eyeW_.load())
            simqvfr::logf("rf2: no multisampled scene target of the eye's %ux%u (or its aspect) was bound in 900 frames: "
                          "QVFR stays off. It needs the game's anti-aliasing (MSAA) on.",
                          eyeW_.load(), eyeH_.load());
        if (frame % 300 == 0 && target_.valid())
            simqvfr::logf("rf2 multi-view: %s; two-view mode switches=%llu; regulator: %llu frames over the limit while the "
                          "GPU wasn't busy",
                          backend_ == MultiView::nvidia ? "NVIDIA's" : backend_ == MultiView::instancing ? "instancing" : "off",
                          static_cast<unsigned long long>(spsSwitches_), heldNotBound_.load());
    }

private:
    static constexpr UINT kMaxInstances = 256;
    enum class Restore { none, all };
    struct Saved {
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        ID3D11ClassInstance* instances[kMaxInstances]{};
        UINT instanceCount = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> cb13;
        D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewports = 0;
        D3D11_RECT scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT scissors = 0;
    };
    void release_instances() {
        for (UINT i = 0; i < saved_.instanceCount && i < kMaxInstances; ++i)
            if (saved_.instances[i]) {
                saved_.instances[i]->Release();
                saved_.instances[i] = nullptr;
            }
        saved_.instanceCount = 0;
    }

    static bool describe(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& d) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
        if (!r || FAILED(r->QueryInterface(IID_PPV_ARGS(&t))))
            return false;
        t->GetDesc(&d);
        return true;
    }
    bool scene_resource(ID3D11Resource* r) const {
        D3D11_TEXTURE2D_DESC d{};
        return target_.valid() && describe(r, d) && target_.matches(d);
    }
    // The eye's scene target: once locked, the same size, format and samples; until then a multisampled 16-bit float
    // target of the eye's size, or of its aspect at half its size or more (a game resolution scale), bound with depth,
    // locked on (`lock`) the first time one is bound. Targets that are close but not it are logged, a few, once each.
    bool scene_target(ID3D11RenderTargetView* rtv, bool lock) {
        Microsoft::WRL::ComPtr<ID3D11Resource> r;
        rtv->GetResource(&r);
        D3D11_TEXTURE2D_DESC d{};
        if (!describe(r.Get(), d))
            return false;
        if (target_.valid())
            return target_.matches(d);
        if (d.SampleDesc.Count < 2)
            return false;
        const uint32_t w = eyeW_.load(), h = eyeH_.load();
        const bool fp16 = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
        const bool exact = w && d.Width == w && d.Height == h;
        const bool aspect = w && h && d.Width * 2 >= w &&
                            std::fabs(static_cast<double>(d.Width) / d.Height - static_cast<double>(w) / h) < 0.01;
        if (!fp16 || !(exact || aspect)) {
            note_candidate(d);
            return false;
        }
        if (!lock)
            return true;
        target_ = {d.Width, d.Height, d.SampleDesc.Count, d.Format};
        simqvfr::logf("rf2: the eye's scene target is %ux%u format %u x%u MSAA (%s the offered eye %ux%u)", d.Width, d.Height,
                      static_cast<unsigned>(d.Format), d.SampleDesc.Count, exact ? "the size of" : "the aspect of", w, h);
        {
            std::lock_guard<std::mutex> l(atlasMutex_);
            atlasDirty_ = true;
        }
        return true;
    }
    void note_candidate(const D3D11_TEXTURE2D_DESC& d) {
        if (candidates_ >= 8)
            return;
        for (unsigned i = 0; i < candidates_; ++i)
            if (seen_[i].Width == d.Width && seen_[i].Height == d.Height && seen_[i].Format == d.Format &&
                seen_[i].SampleDesc.Count == d.SampleDesc.Count)
                return;
        seen_[candidates_++] = d;
        simqvfr::logf("rf2: multisampled target %ux%u format %u x%u bound with depth: not the eye's scene target (eye %ux%u)",
                      d.Width, d.Height, static_cast<unsigned>(d.Format), d.SampleDesc.Count, eyeW_.load(), eyeH_.load());
    }

    // NVIDIA's two-view mode on the observed context (render thread). Switched only when it changes. False: refused.
    bool two_views(ID3D11DeviceContext* c, bool on) {
        if (spsOn_ == on)
            return true;
        if (on && backend_ != MultiView::nvidia)
            return false;
        const int status = simqvfr::nvapi::set_two_views(c, on);
        if (status != 0) {
            simqvfr::logf("rf2 multi-view: NVIDIA's two views would not switch %s (%s)", on ? "on" : "off",
                          simqvfr::nvapi::status_name(status));
            if (!on)
                spsOn_ = false;
            return false;
        }
        spsOn_ = on;
        ++spsSwitches_;
        return true;
    }

    // The first scene pass of a frame (render thread): the sizes, cuts and focus box set since the last frame come into use.
    void latch() {
        {
            std::lock_guard<std::mutex> lock(focusMutex_);
            if (focusDirty_) {
                mapping_ = pendingMapping_;
                focusDirty_ = false;
                remapEye_ = -1;
            }
        }
        std::lock_guard<std::mutex> lock(atlasMutex_);
        if (!atlasDirty_ || !baseSet_ || !target_.valid())
            return;
        atlasDirty_ = false;
        const int cutP = reg_.periphery_cut(), cutD = reg_.density_cut();
        double outer = baseOuter_, focusW = baseFocusW_, focusH = baseFocusH_;
        if (cutP)
            outer = std::max(0.10, outer - cutP / 100.0);
        if (cutD && baseDensity_ > 0) {
            const double factor = std::max(0.0, baseDensity_ - cutD) / baseDensity_;
            focusW *= factor;
            focusH *= factor;
        }
        const AtlasLayout next = make_atlas_layout(target_, outer, focusW, focusH);
        if (next == atlas_)
            return;
        const bool limited = next.focusW != static_cast<UINT>(focusW * next.width + 0.5) ||
                             next.outerW + 1 < static_cast<UINT>(outer * next.width);
        // A regulator step is logged every fifth percent (and when back at the set values), not each.
        if (!stepped_ || (cutP % 5 == 0 && cutD % 5 == 0))
            simqvfr::logf("rf2 atlas: outer %ux%u (periphery %.0f%% of the native eye), focus %ux%u at x=%u (%.0f%% x %.0f%% "
                          "of it)%s%s",
                          next.outerW, next.outerH, outer * 100, next.focusW, next.focusH, next.focus_x(), focusW * 100,
                          focusH * 100, limited ? " (limited to fit the target)" : "",
                          cutP || cutD ? " [regulator: periphery and density below the set ones]" : "");
        stepped_ = false;
        atlas_ = next;
    }

    // One timed frame's GPU time (render thread, gpu_timing.h's sink): a regulator step changes the layout, taken up at
    // the next frame's first scene pass like any other change.
    void on_gpu_sample(double ms) {
        const GpuLoad load = simqvfr::gpu_load_latest();
        const bool gpuBound = !load.valid || load.busy >= kRegulatorBusyPercent;
        std::lock_guard<std::mutex> lock(atlasMutex_);
        if (reg_.feed(ms, gpuBound)) {
            atlasDirty_ = true;
            stepped_ = true;
        }
        heldNotBound_.store(reg_.held_not_gpu_bound());
    }

    // Under atlasMutex_: how far each axis can go down from its set value, from the set values and the floors.
    void refresh_limits_locked() {
        reg_.set_limits(static_cast<int>(baseOuter_ * 100.0 + 0.5) - kRegulatorPeripheryFloor,
                        static_cast<int>(baseDensity_ + 0.5) - kRegulatorDensityFloor);
    }

    // Under focusMutex_: the box from the FOV (or centred before one is known), for the next frame.
    void update_focus_locked() {
        FocusMapping m = centred_focus_mapping(fx_, fy_);
        if (haveTan_ && !calculate_focus_mapping(lastTan_, fx_, fy_, m, offsetY_))
            return;
        pendingMapping_ = m;
        focusDirty_ = true;
        if (focusLogs_++ % 600 != 0)
            return;
        const auto& r = m.rect;
        const auto& p = m.remap;
        simqvfr::logf("rf2 focus box: L rect %.4f %.4f %.4f %.4f remap %.4f %.4f %.4f %.4f; R rect %.4f %.4f %.4f %.4f remap "
                      "%.4f %.4f %.4f %.4f",
                      r[0][0], r[0][1], r[0][2], r[0][3], p[0][0], p[0][1], p[0][2], p[0][3], r[1][0], r[1][1], r[1][2],
                      r[1][3], p[1][0], p[1][1], p[1][2], p[1][3]);
    }

    // Render thread.
    PassState pass_;
    Compositor compositor_;
    BackendStats stats_;
    RemapBuffer remap_;
    GpuTiming gpu_;
    Census census_;
    SceneTarget target_;
    D3D11_TEXTURE2D_DESC seen_[8]{};
    unsigned candidates_ = 0;
    AtlasLayout atlas_;
    FocusMapping mapping_ = centred_focus_mapping(kDefaultFocusFraction, kDefaultFocusFraction);
    int remapEye_ = -1;
    Saved saved_;
    Restore restore_ = Restore::none;
    bool instancedDraw_ = false;
    bool spsOn_ = false;
    uint64_t spsSwitches_ = 0;
    std::atomic<MultiView> backend_{MultiView::none};
    bool canInstance_ = false;
    std::atomic<uint32_t> eyeW_{0}, eyeH_{0};
    std::atomic<unsigned long long> frame_{0};
    std::atomic<unsigned long long> heldNotBound_{0};

    // Under atlasMutex_: the set sizes (set_density; the density in percent from set_regulator) and the regulator.
    std::mutex atlasMutex_;
    double baseOuter_ = 0, baseFocusW_ = 0, baseFocusH_ = 0, baseDensity_ = 0;
    bool baseSet_ = false, regSet_ = false, atlasDirty_ = false, stepped_ = false;
    Regulator reg_;

    // Under focusMutex_: the FOV, the box's share and offset -> the mapping the next frame takes up.
    std::mutex focusMutex_;
    float lastTan_[2][4]{};
    bool haveTan_ = false, focusSet_ = false, offsetSet_ = false, focusDirty_ = true;
    double offsetY_ = 0;
    double fx_ = kDefaultFocusFraction, fy_ = kDefaultFocusFraction;
    FocusMapping pendingMapping_ = centred_focus_mapping(kDefaultFocusFraction, kDefaultFocusFraction);
    unsigned focusLogs_ = 0;
};

}  // namespace simqvfr::games::rf2

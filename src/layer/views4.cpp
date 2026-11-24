// views4: composing the game's four views into the eyes' images, on D3D12 (D3D11: views4_d3d11.cpp), and submitting them.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <dxgi1_4.h>

#include "games/adapter.h"
#include "log.h"
#include "probe/d3d11_compose_timer.h"
#include "probe/d3d11_capture.h"
#include "probe/gpu_frame_timer.h"
#include "quad/compositor.h"
#include "quad/focus_shape.h"
#include "quad/focus_quad.h"
#include "quad/focus_turn.h"
#include "quad/helmet.h"
#include "quad/native_quad.h"
#include "quad/output_density.h"
#include "xr.h"
#include "layer/layer.h"
#include "layer/views4_internal.h"

namespace simqvfr::layer {

// The composed eyes' size in the output (under quad.mutex), from the top left of each eye's half: the native eye at
// this frame's output scale (quad/output_density.h), or with the FOV crop its kept share (the same density over the
// cropped FOV). The crop ACE located its views with, so a live change follows ACE's frames (at most a frame apart).
XrExtent2Di views4_output_extent(const OutputDensity& d) {
    const double scale = std::max(1.0, d.scale);
    return {output_density_pixels(quad.nativeW, scale, quad.v4Crop.kept_width(), d.eyeW),
            output_density_pixels(quad.nativeH, scale, quad.v4Crop.kept_height(), d.eyeH)};
}

void views4_output_chain(OutputDensity& d) {
    d.cap = output_density_cap(quad.nativeW, quad.nativeH, quad.maxW, quad.maxH,
                               experiment.outputDensityMax.load(std::memory_order_relaxed));
    // The launch focus density: the focus views' size against the native eye over the box's share of the FOV.
    const Views4Layout& launch = experiment.views4Layout;
    double density = 1;
    if (experiment.focusFraction > 0)
        density = std::max(density, launch.focus / experiment.focusFraction);
    if (experiment.focus_y() > 0)
        density = std::max(density, launch.focusHeight / experiment.focus_y());
    const double want = std::clamp(std::max(density, d.target), 1.0, d.cap);
    d.chainScale = output_chain_scale(want, d.cap);
    d.eyeW = static_cast<uint32_t>(output_density_pixels(quad.nativeW, d.chainScale, 1, 0));
    d.eyeH = static_cast<uint32_t>(output_density_pixels(quad.nativeH, d.chainScale, 1, 0));
    // Both eyes side by side in the chain: each at most half its largest width (quad/output_density.h).
    d.eyeW = std::min(d.eyeW, output_eye_max_width(quad.maxW));
    d.eyeH = std::min(d.eyeH, output_eye_max_height(quad.maxH));
    if (d.wanted <= 0)  // the first chain: the launch density until a composition measures it
        d.target = output_density_next(want, 1, d.cap);
    d.scale = std::min(d.target, d.chainScale);
}

void views4_output_density(OutputDensity& d, const char* who) {
    d.cap = output_density_cap(quad.nativeW, quad.nativeH, quad.maxW, quad.maxH,
                               experiment.outputDensityMax.load(std::memory_order_relaxed));
    d.wanted = d.wanting;
    d.wanting = 0;
    if (d.wanted > 0)
        d.target = output_density_next(d.wanted, d.target, d.cap);
    else
        d.target = std::min(d.target, d.cap);
    d.scale = std::min(d.target, d.chainScale);
    if (std::abs(d.scale - d.logged) > 1e-6) {
        logf("%s: output %.0f%% of the native eye (%ux%u per eye of a %ux%u chain eye; the focus views at %.2f texels "
             "per native pixel, up to %.0f%%)%s",
             who, d.scale * 100, output_density_pixels(quad.nativeW, d.scale, 1, d.eyeW),
             output_density_pixels(quad.nativeH, d.scale, 1, d.eyeH), d.eyeW, d.eyeH, d.wanted, d.cap * 100,
             d.target > d.chainScale + 1e-9 ? " (more wanted than its chain holds)" : "");
        d.logged = d.scale;
    }
}

namespace {

// views4: ACE's eye image holds the outer views as views 0-1 and the focus views as views 2-3 of the
// layout (the focus cameras: focus_percent of each eye's tangent extent around its forward axis). Composes
// both eyes into the output chain at native size, on ACE's queue, before ACE releases the image.
void views4_compose() {
    quad.v4Composed = false;
    // What this frame's views were rendered with (ACE's own eye projections, the focus boxes and
    // turns, views4.h); before the first focus cameras are filled, the located FOV, unturned.
    // Taken before anything can skip this frame: a frame filled but not taken would leave every later
    // composition a frame behind, its focus placed by an older gaze (focus lagging the eyes, 2026-11-02).
    Views4Frame frame{};
    const bool recorded = game->qvfr_take_frame(frame);
    if (!quad.outChain || quad.appAcquired >= quad.appImages.size() || !quad.v4FovValid)
        return;
    if (!recorded)
        frame = views4_unrecorded_frame(quad.v4Fov);
    ID3D12Resource* app = quad.appImages[quad.appAcquired];
    EyeComposition eyes[2] = {};
    const HelmetSettings& helmet = helmetSettings;
    V4Helmet helmetFrame = views4_helmet_start(helmet);
    // Outer source = view e, focus source = view e + 2, wherever the layout puts them, at the sizes this frame was
    // rendered with (live resolution); launch sizes until the first record.
    const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
    const int w = static_cast<int>(quad.appW), h = static_cast<int>(quad.appH);
    const auto source = [&](int view, float (&uv)[4]) { views4_source(layout, view, false, w, h, {0, 0}, w, h, uv); };
    for (int e = 0; e < 2; ++e) {
        views4_eye(frame, e, source, eyes[e]);
        views4_helmet_eye(helmet, helmetFrame, e, eyes[e], quad.v4SubmitFov[e], quad.v4Extent[e], quad.outDensity,
                          static_cast<double>(eyes[e].focusSource[2]) * w);
    }
    views4_output_density(quad.outDensity, "views4");
    if (helmet.on)
        log_helmet_light();
    uint32_t outIndex = 0;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    // Bounded (inside ACE's xrReleaseSwapchainImage, under quad.mutex): timed out, ACE's own eyes go out this frame and
    // the image, still acquired, is waited on again at the next.
    const XrResult waited =
        acquire_wait(quad.outChain, quad.outPending, kImageWaitNs, kImageWaitTries, outIndex, "views4: output image wait");
    if (waited != XR_SUCCESS) {
        quad_fail(waited == XR_TIMEOUT_EXPIRED ? "views4 output wait timed out" : "views4 output acquire failed");
        return;
    }
    if (outIndex >= quad.outImages.size()) {
        next.ReleaseSwapchainImage(quad.outChain, &release);
        quad_fail("views4 output acquire failed");
        return;
    }
    ID3D12Resource* out = quad.outImages[outIndex];
    bool ok = quad.compositor.capture_outer(app) && quad.compositor.compose(app, out, eyes);
    if (ok && quad_capture_due())
        views4_capture(app, out);
    next.ReleaseSwapchainImage(quad.outChain, &release);
    if (!ok) {
        quad_fail("views4 compose failed");
        return;
    }
    quad.v4Composed = true;
    quad.fallbackRun = 0;
    views4_helmet_layer(helmet, helmetFrame, eyes, nullptr);
    if (quad.composed++ == 0)
        logf("views4: first composed frame: eye 0 outer tangents %.3f/%.3f/%.3f/%.3f, focus +-%.3f x +-%.3f, output "
             "%ux%u per eye",
             eyes[0].outerTan[0], eyes[0].outerTan[1], eyes[0].outerTan[2], eyes[0].outerTan[3], eyes[0].focusTan[1],
             eyes[0].focusTan[2], quad.v4Extent[0].width, quad.v4Extent[0].height);
}

// P3, the API-level path (replay_compose): ACE renders only its two eyes (single-pass stereo); the replay probe has
// drawn both focus views into its own image. Each eye: the outer view is ACE's eye (its half of the image, over the
// FOV it was located with), the focus view the probe's image (its half), over the box centred on the eye's forward
// axis, unturned (the probe's replay renders it there). Composed into the output chain at native size, on ACE's queue.
void replay_compose() {
    quad.v4Composed = false;
    if (!quad.outChain || quad.appAcquired >= quad.appImages.size() || !quad.v4FovValid)
        return;
    FocusImages* images = focus_images();
    if (!images)
        return;
    images->eye_allow(false);  // until this frame is composed: the uncomposed fallback shows ACE's whole image
    FocusImage focus;
    if (!images->focus_image(focus))
        return;  // no focus image yet: ACE's eyes go to the headset as they are
    ID3D12Resource* focusImage = focus.image;
    const D3D12_RESOURCE_STATES focusState = static_cast<D3D12_RESOURCE_STATES>(focus.state);
    const double box = focus.box, boxY = focus.boxY;
    const float (&gazeNdc)[2][2] = focus.offset;  // each eye's box centre off its forward axis, as this frame was drawn
    // The live periphery: the share of ACE's eye image this frame was drawn into (its top left).
    float eyeX = 1, eyeY = 1;
    images->eye_region(eyeX, eyeY);
    ID3D12Resource* app = quad.appImages[quad.appAcquired];
    EyeComposition eyes[2] = {};
    const HelmetSettings& helmet = helmetSettings;
    V4Helmet helmetFrame = views4_helmet_start(helmet);
    const XrExtent2Di extent = views4_output_extent(quad.outDensity);
    const double focusTexels = 0.5 * static_cast<double>(focusImage->GetDesc().Width);  // each eye's half
    for (int e = 0; e < 2; ++e) {
        EyeComposition& c = eyes[e];
        fov_tangents(quad.v4Fov[e], c.outerTan);
        double hw, hh;
        focus_extents(c.outerTan, box, boxY, hw, hh);
        // The box's centre: the gaze offset (the eye's NDC) in tangents of the FOV ACE rendered.
        const float cx = gazeNdc[e][0] * 0.5f * (c.outerTan[1] - c.outerTan[0]);
        const float cy = gazeNdc[e][1] * 0.5f * (c.outerTan[2] - c.outerTan[3]);
        const float focusTan[4] = {cx - static_cast<float>(hw), cx + static_cast<float>(hw), cy + static_cast<float>(hh),
                                   cy - static_cast<float>(hh)};
        std::memcpy(c.focusTan, focusTan, sizeof(focusTan));
        const XrQuaternionf none{0, 0, 0, 1};
        to_matrix(none, c.rotation);
        const float half[4] = {0.5f * static_cast<float>(e), 0, 0.5f, 1};
        const float outer[4] = {0.5f * static_cast<float>(e) * eyeX, 0, 0.5f * eyeX, eyeY};
        std::memcpy(c.outerSource, outer, sizeof(outer));
        std::memcpy(c.focusSource, half, sizeof(half));
        c.feather = static_cast<float>(experiment.feather);
        c.border = experiment.border ? 0.008f : 0.0f;
        std::memcpy(c.borderColor, experiment.borderColor, sizeof(c.borderColor));
        c.cornerRadius = static_cast<float>(experiment.cornerRadius);
        c.shape = experiment.focusShape.load();
        c.sharpness = static_cast<float>(experiment.sharpen.load(std::memory_order_relaxed)) / 100.0f;
        color_grade_constants(color_grade_from_bits(experiment.colorGrade.load(std::memory_order_relaxed)), c.color);
        quad.v4Extent[e] = extent;
        views4_helmet_eye(helmet, helmetFrame, e, c, quad.v4SubmitFov[e], quad.v4Extent[e], quad.outDensity, focusTexels);
    }
    views4_output_density(quad.outDensity, "replay compose");
    uint32_t outIndex = 0;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    // Bounded (inside ACE's xrReleaseSwapchainImage, under quad.mutex): timed out, ACE's eyes go out uncomposed this
    // frame and the image, still acquired, is waited on again at the next.
    const XrResult waited =
        acquire_wait(quad.outChain, quad.outPending, kImageWaitNs, kImageWaitTries, outIndex, "replay compose: output image wait");
    if (waited != XR_SUCCESS) {
        quad_fail(waited == XR_TIMEOUT_EXPIRED ? "replay compose: output wait timed out"
                                               : "replay compose: output acquire failed");
        return;
    }
    if (outIndex >= quad.outImages.size()) {
        next.ReleaseSwapchainImage(quad.outChain, &release);
        quad_fail("replay compose: output acquire failed");
        return;
    }
    ID3D12Resource* out = quad.outImages[outIndex];
    images->gpu_scope(true);
    const bool ok = quad.compositor.capture_outer(app) && quad.compositor.compose(focusImage, out, eyes, focusState);
    images->gpu_scope(false);
    int burstIndex = 0;
    if (frame_census_take_capture(burstIndex) && ok)  // the draw census hotkey: ACE's eyes, the focus views, the output
        views4_census_capture(app, focusImage, focusState, out, burstIndex);
    if (experiment.replayEyeCapture) {  // diagnostic: ACE's whole eye image and the composed left eye, about two seconds
                                        // after each change of the live periphery (up to 12 a session)
        static float lastX = -1;
        static int due = 0, taken = 0;
        if (eyeX != lastX) {
            lastX = eyeX;
            due = 150;
        }
        if (due > 0 && --due == 0 && taken < 12 && ok) {
            ++taken;
            logf("replay eye: capture with the periphery at %.0f%% x %.0f%% of its launch size", eyeX * 100, eyeY * 100);
            views4_capture(app, out);
        }
    }
    next.ReleaseSwapchainImage(quad.outChain, &release);
    if (!ok) {
        quad_fail("replay compose failed");
        return;
    }
    quad.v4Composed = true;
    quad.fallbackRun = 0;
    images->eye_allow(true);
    views4_helmet_layer(helmet, helmetFrame, eyes, nullptr);
    if (quad.composed++ == 0)
        logf("replay compose: first composed frame: eye 0 outer tangents %.3f/%.3f/%.3f/%.3f, focus +-%.3f x +-%.3f "
             "(box %.0f%%), output %dx%d per eye",
             eyes[0].outerTan[0], eyes[0].outerTan[1], eyes[0].outerTan[2], eyes[0].outerTan[3], eyes[0].focusTan[1],
             eyes[0].focusTan[2], box * 100, extent.width, extent.height);
}

}  // namespace

// Frame A's image is copied before the runtime gets it back; frame B's stays acquired (so this
// layer may still read it) until B's xrEndFrame has composed it.
XrResult XRAPI_CALL layerReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
    game->swapchain_releasing(swapchain);  // the game's last change before the runtime reads the image (ATS's corner)
    if (experiment.d3d11CaptureFrame)
        d3d11_capture_release(swapchain, endFrames);
    if (views4_outline_on())
        views4_outline_draw(swapchain);  // on ACE's queue, after its frame, before the runtime reads it
    if (!experiment.views4 && swapchain == measuredEyeChain.load(std::memory_order_relaxed))
        gpu_frame_timer_mark(GpuMark::kRendered);  // baseline with gpu_frame_timing: the game's own frame
    if (experiment.replayCompose) {
        std::lock_guard guard(quad.mutex);
        if (swapchain == quad.appChain)
            replay_compose();  // reads ACE's image on its queue before the runtime gets it back
    }
    if (experiment.views4Compose) {
        std::lock_guard guard(quad.mutex);
        if (swapchain == quad.appChain) {
            gpu_frame_timer_mark(GpuMark::kRendered);
            const int64_t t0 = qpc_now();
            views4_compose();  // reads the image on ACE's queue before the runtime gets it back
            cpu.compose.add(qpc_now() - t0);
            gpu_frame_timer_mark(GpuMark::kComposed);
        }
    }
    if (experiment.quad) {
        bool active;
        {
            std::lock_guard guard(loops.mutex);
            active = loops.active;
        }
        std::lock_guard guard(quad.mutex);
        if (active && swapchain == quad.appChain && quad.appAcquired < quad.appImages.size()) {
            if (loops.inB) {
                quad.releaseHeld = true;
                return XR_SUCCESS;
            }
            quad.outerCaptured = quad.compositor.capture_outer(quad.appImages[quad.appAcquired]);
        }
    }
    return next.ReleaseSwapchainImage(swapchain, info);
}

namespace {

// focus_layer=2: each focus view (`views`: its eye's pose, its own FOV and image) as a quad seen by that eye only,
// placed by focus_quad (quad/focus_quad.h).
void views4_focus_quads(const XrCompositionLayerProjectionView (&views)[2], XrCompositionLayerFlags flags, XrSpace space,
                        XrCompositionLayerQuad (&quads)[2]) {
    const float distance = static_cast<float>(experiment.focusQuadDistance.load(std::memory_order_relaxed));
    for (int v = 0; v < 2; ++v) {
        const FocusQuad placed = focus_quad(views[v].pose, views[v].fov, distance);
        XrCompositionLayerQuad& q = quads[v];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = flags;
        q.space = space;
        q.eyeVisibility = v == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
        q.subImage = views[v].subImage;
        q.pose = placed.pose;
        q.size = placed.size;
    }
    static float logged = 0;
    if (logged != distance) {
        logged = distance;
        logf("views4 d3d11: focus_layer=2: eye 0's quad %.2f x %.2f m, %.0f m out (its image %dx%d)", quads[0].size.width,
             quads[0].size.height, distance, quads[0].subImage.imageRect.extent.width,
             quads[0].subImage.imageRect.extent.height);
    }
}

}  // namespace

XrResult views4_submit(XrSession session, const XrFrameEndInfo* info) {
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    XrCompositionLayerQuad helmetLayers[kHelmetQuadsMax] = {};  // the helmet's own layer (its bands), over the composed scene
    int helmetCount = 0, helmetAfter = -1;
    // focus_layer: the focus views' projection layer, right over the periphery's (`focusAfter`), under the helmet.
    XrCompositionLayerProjection focusLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView focusViews[2] = {};
    XrCompositionLayerQuad focusQuads[2] = {};  // focus_layer=2: the same images as a quad per eye
    int focusAfter = -1;
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<std::vector<XrCompositionLayerProjectionView>> views;
    projections.reserve(info->layerCount);
    views.reserve(info->layerCount);
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layers[i]);
        if (views4_outline_on() && proj->viewCount > 0) {
            std::lock_guard guard(v4outline.mutex);
            if (!v4outline.chain)
                v4outline.chain = proj->views[0].subImage.swapchain;  // ACE's eye chain
        }
        auto& copy = views.emplace_back(proj->views, proj->views + proj->viewCount);
        if ((experiment.views4Compose || experiment.replayCompose) && proj->viewCount == 2) {
            std::lock_guard guard(quad.mutex);
            if (quad.v4Composed && proj->views[0].subImage.swapchain == quad.appChain) {
                // The composed output (at its density's size), with ACE's own pose and FOV for each eye.
                quad.v4Composed = false;
                for (uint32_t v = 0; v < 2; ++v) {
                    log_submitted_fov(v, copy[v].fov, quad.v4SubmitFov[v]);
                    // The FOV the output was composed over (ACE's rendered projection), which is what
                    // the runtime must display it with, whatever ACE's own projection view says.
                    copy[v].fov = quad.v4SubmitFov[v];
                    copy[v].next = nullptr;  // depth info would describe ACE's quarters, not the output
                    copy[v].subImage.swapchain = quad.outChain;
                    const uint32_t eyeW = quad.outDensity.eyeW ? quad.outDensity.eyeW : quad.nativeW;
                    copy[v].subImage.imageRect = {{static_cast<int32_t>(v * eyeW), 0}, quad.v4Extent[v]};
                    copy[v].subImage.imageArrayIndex = 0;
                }
                projections.push_back(*proj);
                projections.back().views = copy.data();
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
                views4_helmet_submit(session, static_cast<int>(i), helmetLayers, helmetCount, helmetAfter);
                continue;
            }
        }
        if (experiment.replayCompose)
            continue;  // the API-level path: not composed this frame (no focus image yet): ACE's eyes as they are
        // Separate views (qvfr_separate_views, LMU): each eye an image of its own, whole; composed from the adapter's atlas,
        // or (no four views this frame: menus, loading) submitted as it is. None of the shared-target rects below apply.
        if (experiment.views4ComposeD3D11 && proj->viewCount == 2 && game->qvfr_separate_views()) {
            bool layered = false;
            d3d11_compose_timer_begin();  // the overlay's layer GPU time (probe/d3d11_compose_timer.h)
            const bool composed = v4d3d11_compose(session, proj, copy, focusAfter < 0 ? &focusViews : nullptr, &layered,
                                                  info->displayTime);
            d3d11_compose_timer_end();
            if (composed) {
                // A quad view session (native_quad.cpp): the focus views are this layer's views 2-3, which the runtime
                // composes itself (frames without them get copies of the eyes there, at the layer's bottom edge).
                const bool quadViews = layered && nativeQuad.active;
                if (quadViews)
                    copy.insert(copy.end(), {focusViews[0], focusViews[1]});
                projections.push_back(*proj);
                projections.back().views = copy.data();
                projections.back().viewCount = static_cast<uint32_t>(copy.size());
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
                if (layered && !quadViews) {
                    // The same space and flags as the game's layer, blended over it by its premultiplied alpha.
                    focusLayer.layerFlags = proj->layerFlags | XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    focusLayer.layerFlags &= ~static_cast<XrCompositionLayerFlags>(XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
                    focusLayer.space = proj->space;
                    focusLayer.viewCount = 2;
                    focusLayer.views = focusViews;
                    focusAfter = static_cast<int>(i);
                    if (experiment.focusLayer == 2)
                        views4_focus_quads(focusViews, focusLayer.layerFlags, proj->space, focusQuads);
                }
                std::lock_guard guard(quad.mutex);
                views4_helmet_submit(session, static_cast<int>(i), helmetLayers, helmetCount, helmetAfter);
            } else {
                // No four views this frame, yet the game rendered its eyes into the top-left of their images (the focus
                // views didn't come): that part of each submitted, over the eye's whole field of view, where the whole
                // image would show the eye small in a corner.
                copy.assign(proj->views, proj->views + proj->viewCount);  // as the game gave them
                bool changed = false;
                unsigned ew[2] = {}, eh[2] = {};
                if (game->qvfr_eye_extent(ew, eh)) {
                    bool cut = true;
                    for (uint32_t v = 0; v < 2 && cut; ++v) {
                        const XrRect2Di& r = proj->views[v].subImage.imageRect;
                        cut = ew[v] > 0 && eh[v] > 0 && static_cast<int32_t>(ew[v]) <= r.extent.width &&
                              static_cast<int32_t>(eh[v]) <= r.extent.height;
                    }
                    if (cut)
                        for (uint32_t v = 0; v < 2; ++v)
                            copy[v].subImage.imageRect.extent = {static_cast<int32_t>(ew[v]), static_cast<int32_t>(eh[v])};
                    changed = cut;
                }
                // Native quad views with the FOV crop: the eyes padded out to the runtime's (issue 29: Pimax stretched
                // the cropped eyes over its whole eye in menus, and in race frames without focus views).
                if (copy.size() == 2 && v4d3d11_pad_eyes(session, copy, info->displayTime))
                    changed = true;
                if (changed) {
                    projections.push_back(*proj);
                    projections.back().views = copy.data();
                    layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
                }
            }
            continue;
        }
        Views4Frame latest{};
        game->qvfr_latest_frame(latest);  // the rendered sizes (live resolution)
        // The game drew only its eyes this frame, each over its whole half (X-Plane's menu): submitted as it is.
        if (latest.eyesOnly) {
            if (!v4EyesOnlyLogged) {
                logf("views4: the game drew its eyes only (no four views): submitted as it is, uncomposed");
                v4EyesOnlyLogged = true;
            }
            continue;
        }
        v4EyesOnlyLogged = false;
        const Views4Layout layout = views4_with_live(experiment.views4Layout, latest.live, latest.width, latest.height);
        bool gameLayout = true;
        for (uint32_t v = 0; v < proj->viewCount; ++v)
            gameLayout &= copy[v].subImage.imageRect.offset.x == static_cast<int32_t>(v) * copy[v].subImage.imageRect.extent.width;
        if (!gameLayout)
            continue;
        if (proj->viewCount == 2 && game->qvfr_eye_columns() && game->qvfr_wants_game_image())
            v4d3d11_game_image(proj, layout);
        bool composedColumns = false;
        if (experiment.views4ComposeD3D11 && proj->viewCount == 2 && game->qvfr_eye_columns()) {
            d3d11_compose_timer_begin();
            composedColumns = v4d3d11_compose(session, proj, copy);
            d3d11_compose_timer_end();
        }
        if (composedColumns) {
            projections.push_back(*proj);
            projections.back().views = copy.data();
            layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
            std::lock_guard guard(quad.mutex);
            views4_helmet_submit(session, static_cast<int>(i), helmetLayers, helmetCount, helmetAfter);
            continue;
        }
        for (uint32_t v = 0; v < proj->viewCount; ++v) {
            // ACE submits halves of its target (W/2 x H each): point them at view v (or v + 2). A game in eye
            // columns (ACC) submits the whole texture in each eye's image: keep the offset, move and cut to the view.
            XrRect2Di& rect = copy[v].subImage.imageRect;
            const int shown = static_cast<int>(v + (experiment.views4ShowCopies ? 2 : 0));
            const ViewRect view = game->qvfr_eye_columns()
                                      ? views4_rect_eye_columns(layout, shown, 2 * rect.extent.width, rect.extent.height)
                                      : views4_rect(layout, shown, 2 * rect.extent.width, rect.extent.height);
            if (game->qvfr_eye_columns()) {
                rect.offset.x += view.left - static_cast<int>(v) * rect.extent.width;
                rect.offset.y += view.top;
            } else {
                rect.offset.x = view.left;
            }
            rect.extent.width = view.width;
            rect.extent.height = view.height;
            if (!views4RectsLogged)
                logf("views4: projection view %u submitted as rect=%d,%d %dx%d", v, rect.offset.x, rect.offset.y,
                     rect.extent.width, rect.extent.height);
        }
        views4RectsLogged = true;
        projections.push_back(*proj);
        projections.back().views = copy.data();
        layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
    }
    for (int b = helmetCount - 1; b >= 0; --b)  // over the scene, under the overlay and the menu
        layers.insert(layers.begin() + helmetAfter + 1, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&helmetLayers[b]));
    // Inserted after the helmet's, so they land directly over the periphery, under them.
    if (focusAfter >= 0 && experiment.focusLayer == 2) {
        layers.insert(layers.begin() + focusAfter + 1, {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&focusQuads[0]),
                                                         reinterpret_cast<const XrCompositionLayerBaseHeader*>(&focusQuads[1])});
    } else if (focusAfter >= 0) {
        layers.insert(layers.begin() + focusAfter + 1, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&focusLayer));
    }
    XrFrameEndInfo submitted = *info;
    submitted.layers = layers.data();
    submitted.layerCount = static_cast<uint32_t>(layers.size());
    return next.EndFrame(session, &submitted);
}

}  // namespace simqvfr::layer

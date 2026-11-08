// views4: composing the game's four views into the eyes' images, on D3D12 or D3D11, and submitting them.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <dxgi1_4.h>

#include "games/adapter.h"
#include "log.h"
#include "probe/d3d11_capture.h"
#include "probe/gpu_frame_timer.h"
#include "quad/compositor.h"
#include "quad/focus_shape.h"
#include "quad/focus_turn.h"
#include "quad/helmet.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

// The composed eyes' size in the output (under quad.mutex), from the top left of each eye's half:
// native, or with the FOV crop its kept share (the same density over the cropped FOV). The crop ACE
// located its views with, so a live change follows ACE's frames (at most a frame apart).
XrExtent2Di views4_output_extent() {
    const auto kept = [](uint32_t native, double share) {
        return static_cast<int32_t>(std::clamp<long>(std::lround(native * share), 1, static_cast<long>(native)));
    };
    return {kept(quad.nativeW, quad.v4Crop.kept_width()), kept(quad.nativeH, quad.v4Crop.kept_height())};
}

namespace {

// A frame record for a frame the game recorded nothing for (before its first, or its projection unknown): the
// eyes' FOVs, the focus box centred on each, unturned.
Views4Frame views4_unrecorded_frame(const XrFovf (&fov)[2]) {
    Views4Frame frame{};
    for (int e = 0; e < 2; ++e) {
        fov_tangents(fov[e], frame.outerTan[e]);
        double hw, hh;
        focus_extents(frame.outerTan[e], experiment.box_x(), experiment.box_y(), hw, hh);
        const float focusTan[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                   static_cast<float>(-hh)};
        std::memcpy(frame.focusTan[e], focusTan, sizeof(focusTan));
        frame.turn[e] = {0, 0, 0, 1};
    }
    return frame;
}

// Where view `view` sits in the game's image: its rect in the game's target (targetW x targetH, at `origin` in a
// texture of textureW x textureH), as a texture region (u0, v0, du, dv).
void views4_source(const Views4Layout& layout, int view, bool eyeColumns, int targetW, int targetH, XrOffset2Di origin,
                   int textureW, int textureH, float (&uv)[4]) {
    const ViewRect r = eyeColumns ? views4_rect_eye_columns(layout, view, targetW, targetH)
                                  : views4_rect(layout, view, targetW, targetH);
    uv[0] = static_cast<float>(origin.x + r.left) / static_cast<float>(textureW);
    uv[1] = static_cast<float>(origin.y + r.top) / static_cast<float>(textureH);
    uv[2] = static_cast<float>(r.width) / static_cast<float>(textureW);
    uv[3] = static_cast<float>(r.height) / static_cast<float>(textureH);
}

// One eye's composition, for either compositor: what its views were rendered with (the frame's record: outer and
// focus tangents, the focus turn), where they sit in the game's image (`source`: the outer view e, the focus view
// e + 2), and the look from the live settings (feather, outline, corners, shape).
template <class Source>
void views4_eye(const Views4Frame& frame, int e, const Source& source, EyeComposition& c) {
    std::memcpy(c.outerTan, frame.outerTan[e], sizeof(c.outerTan));
    std::memcpy(c.focusTan, frame.focusTan[e], sizeof(c.focusTan));
    // The focus camera is the eye's turned by turn[e] (in the eye's frame): a direction in the outer view's
    // frame is in the focus view's frame after the inverse turn.
    to_matrix(qconj(frame.turn[e]), c.rotation);
    source(e, c.outerSource);
    source(e + 2, c.focusSource);
    c.feather = static_cast<float>(experiment.feather);
    // The outline (debug_border) is drawn by the compositor along the focus area's exact edge, turned and
    // rounded as it is.
    c.border = experiment.border ? 0.008f : 0.0f;
    std::memcpy(c.borderColor, experiment.borderColor, sizeof(c.borderColor));
    c.cornerRadius = static_cast<float>(experiment.cornerRadius);
    focus_shape_insets(experiment.focusShape.load(std::memory_order_relaxed), c.cornerInset);
}

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
        views4_helmet_eye(helmet, helmetFrame, e, eyes[e], quad.v4SubmitFov[e], quad.v4Extent[e]);
    }
    if (helmet.on)
        log_helmet_light();
    uint32_t outIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(quad.outChain, &acquire, &outIndex)) || outIndex >= quad.outImages.size()) {
        quad_fail("views4 output acquire failed");
        return;
    }
    ID3D12Resource* out = quad.outImages[outIndex];
    bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(quad.outChain, &wait)) && quad.compositor.capture_outer(app) &&
              quad.compositor.compose(app, out, eyes);
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
             eyes[0].focusTan[2], quad.nativeW, quad.nativeH);
}

}  // namespace

// Frame A's image is copied before the runtime gets it back; frame B's stays acquired (so this
// layer may still read it) until B's xrEndFrame has composed it.
XrResult XRAPI_CALL layerReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
    if (experiment.d3d11CaptureFrame)
        d3d11_capture_release(swapchain, endFrames);
    if (views4_outline_on())
        views4_outline_draw(swapchain);  // on ACE's queue, after its frame, before the runtime reads it
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

// The D3D11 output chain and compositor, made at the first composition (the game's format, the native eye size,
// double wide). False: v4d3d11.failed, with the reason logged once.
bool v4d3d11_resources(XrSession session, int64_t format) {
    if (v4d3d11.outChain)
        return true;
    if (v4d3d11.failed)
        return false;
    v4d3d11.failed = true;  // until everything below is made
    uint32_t nativeW, nativeH;
    ID3D11Device* device;
    {
        std::lock_guard guard(quad.mutex);
        nativeW = quad.nativeW;
        nativeH = quad.nativeH;
        device = sessionGraphics.d3d11;
    }
    if (!device || !nativeW || !nativeH) {
        log_line("views4 d3d11: no D3D11 device or native eye size; the headset shows views 0-1");
        return false;
    }
    if (!v4d3d11.compositor.initialize(device, static_cast<DXGI_FORMAT>(format))) {
        log_line("views4 d3d11: compositor unavailable; the headset shows views 0-1");
        return false;
    }
    XrSwapchainCreateInfo out{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    out.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    out.format = format;
    out.sampleCount = out.faceCount = out.arraySize = out.mipCount = 1;
    out.width = 2 * nativeW;
    out.height = nativeH;
    XrSwapchain chain = XR_NULL_HANDLE;
    XrResult r = next.CreateSwapchain(session, &out, &chain);
    uint32_t count = 0;
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r)) {
        logf("views4 d3d11: output swapchain %ux%u failed (%d); the headset shows views 0-1", out.width, out.height, r);
        if (chain)
            next.DestroySwapchain(chain);
        v4d3d11.compositor.shutdown();
        return false;
    }
    v4d3d11.outChain = chain;
    for (const auto& image : images)
        v4d3d11.outImages.push_back(image.texture);
    d3d11_capture_images(chain, images.data(), count);  // d3d11_capture_frame saves the composed output too
    v4d3d11.outW = nativeW;
    v4d3d11.outH = nativeH;
    v4d3d11.failed = false;
    logf("views4 d3d11: output chain %p %ux%u (%u images, format %lld), focus box %.0f%% x %.0f%%, feather %.0f%%",
         static_cast<void*>(chain), out.width, out.height, count, static_cast<long long>(format),
         experiment.box_x() * 100, experiment.box_y() * 100, experiment.feather * 100);
    return true;
}

// Composes both eyes of `proj` (the game's two eye views, eye columns of `layout`) into the output chain and
// points `copy` (its views) at it. False: nothing changed, the caller submits views 0-1 as they are.
bool v4d3d11_compose(XrSession session, const XrCompositionLayerProjection* proj,
                     std::vector<XrCompositionLayerProjectionView>& copy) {
    // What this frame's views were rendered with (the game's record, frame_records.h), taken before anything can
    // skip the frame, so later compositions stay in step with the frames rendered.
    Views4Frame frame{};
    const bool recorded = game->qvfr_take_frame(frame);
    std::lock_guard guard(v4d3d11.mutex);
    const auto fallback = [](const char* why) {
        ++v4d3d11.fallbacks;
        if (v4d3d11.lastFallback != why)
            logf("views4 d3d11: not composed (%s); the headset shows views 0-1", why);
        v4d3d11.lastFallback = why;
        return false;
    };
    const auto it = v4d3d11.chains.find(proj->views[0].subImage.swapchain);
    if (it == v4d3d11.chains.end() || it->second.acquired >= it->second.images.size())
        return fallback("the game's eye image is unknown");
    if (!v4d3d11_resources(session, it->second.format))
        return false;
    ID3D11Texture2D* views = it->second.images[it->second.acquired];
    D3D11_TEXTURE2D_DESC desc;
    views->GetDesc(&desc);
    const float texW = static_cast<float>(desc.Width), texH = static_cast<float>(desc.Height);
    // Before the game's first record (or with no projection in it), the submitted FOVs, centred and unturned.
    if (!recorded || !(frame.outerTan[0][1] > frame.outerTan[0][0]) || !(frame.outerTan[1][1] > frame.outerTan[1][0]))
        frame = views4_unrecorded_frame({proj->views[0].fov, proj->views[1].fov});
    // The game's target as the eyes' rects give it (eye e: the half from e x W), as views4_submit reads it.
    const XrExtent2Di eye = proj->views[0].subImage.imageRect.extent;
    const XrOffset2Di origin = proj->views[0].subImage.imageRect.offset;
    const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
    const auto source = [&](int view, float (&uv)[4]) {
        views4_source(layout, view, true, 2 * eye.width, eye.height, origin, static_cast<int>(desc.Width),
                      static_cast<int>(desc.Height), uv);
    };
    // The helmet and the FOV crop, as the D3D12 path (views4_helmet_eye): each eye's submitted FOV and its size in
    // the output, from the top left of its half.
    std::lock_guard quadGuard(quad.mutex);
    const HelmetSettings& helmet = helmetSettings;
    V4Helmet helmetFrame = views4_helmet_start(helmet);
    EyeComposition eyes[2] = {};
    XrFovf submitFov[2];
    XrExtent2Di extent[2];
    for (int e = 0; e < 2; ++e) {
        views4_eye(frame, e, source, eyes[e]);
        views4_helmet_eye(helmet, helmetFrame, e, eyes[e], submitFov[e], extent[e]);
    }
    if (helmet.on)
        log_helmet_light();
    uint32_t outIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(v4d3d11.outChain, &acquire, &outIndex)) ||
        outIndex >= v4d3d11.outImages.size())
        return fallback("output acquire failed");
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(v4d3d11.outChain, &wait)) &&
                    v4d3d11.compositor.compose(views, v4d3d11.outImages[outIndex], eyes);
    if (ok && experiment.d3d11CaptureFrame) {
        d3d11_capture_acquired(v4d3d11.outChain, outIndex);
        d3d11_capture_release(v4d3d11.outChain, endFrames);
    }
    next.ReleaseSwapchainImage(v4d3d11.outChain, &release);
    if (!ok)
        return fallback("compose failed");
    views4_helmet_layer(helmet, helmetFrame, eyes, views);
    if (v4d3d11.composed++ == 0 || v4d3d11.lastFallback) {
        logf("views4 d3d11: composing (eye 0: outer %.0f,%.0f %.0fx%.0f, focus %.0f,%.0f %.0fx%.0f of the game's "
             "%ux%u image; focus tangents +-%.3f x +-%.3f)",
             eyes[0].outerSource[0] * texW, eyes[0].outerSource[1] * texH, eyes[0].outerSource[2] * texW,
             eyes[0].outerSource[3] * texH, eyes[0].focusSource[0] * texW, eyes[0].focusSource[1] * texH,
             eyes[0].focusSource[2] * texW, eyes[0].focusSource[3] * texH, desc.Width, desc.Height, eyes[0].focusTan[1],
             eyes[0].focusTan[2]);
        v4d3d11.lastFallback = nullptr;
    }
    for (uint32_t v = 0; v < 2; ++v) {
        // The game's own pose; the FOV the output was composed over (the game's rendered projection, or with the
        // helmet's pieces in the scene the uncropped eye), which the runtime must display it with.
        copy[v].fov = submitFov[v];
        copy[v].next = nullptr;  // depth info would describe the game's views, not the output
        copy[v].subImage.swapchain = v4d3d11.outChain;
        copy[v].subImage.imageRect = {{static_cast<int32_t>(v * v4d3d11.outW), 0}, extent[v]};
        copy[v].subImage.imageArrayIndex = 0;
    }
    return true;
}

}  // namespace

XrResult views4_submit(XrSession session, const XrFrameEndInfo* info) {
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    XrCompositionLayerQuad helmetLayers[2] = {};  // the helmet's own layer (its bands), over the composed scene
    int helmetCount = 0, helmetAfter = -1;
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
        if (experiment.views4Compose && proj->viewCount == 2) {
            std::lock_guard guard(quad.mutex);
            if (quad.v4Composed && proj->views[0].subImage.swapchain == quad.appChain) {
                // The composed output, at native size, with ACE's own pose and FOV for each eye.
                quad.v4Composed = false;
                for (uint32_t v = 0; v < 2; ++v) {
                    log_submitted_fov(v, copy[v].fov, quad.v4SubmitFov[v]);
                    // The FOV the output was composed over (ACE's rendered projection), which is what
                    // the runtime must display it with, whatever ACE's own projection view says.
                    copy[v].fov = quad.v4SubmitFov[v];
                    copy[v].next = nullptr;  // depth info would describe ACE's quarters, not the output
                    copy[v].subImage.swapchain = quad.outChain;
                    copy[v].subImage.imageRect = {{static_cast<int32_t>(v * quad.nativeW), 0}, quad.v4Extent[v]};
                    copy[v].subImage.imageArrayIndex = 0;
                }
                projections.push_back(*proj);
                projections.back().views = copy.data();
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
                views4_helmet_submit(session, static_cast<int>(i), helmetLayers, helmetCount, helmetAfter);
                continue;
            }
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
        if (experiment.views4ComposeD3D11 && proj->viewCount == 2 && game->qvfr_eye_columns() &&
            v4d3d11_compose(session, proj, copy)) {
            projections.push_back(*proj);
            projections.back().views = copy.data();
            layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
            std::lock_guard guard(quad.mutex);
            views4_helmet_submit(session, static_cast<int>(i), helmetLayers, helmetCount, helmetAfter);
            continue;
        }
        for (uint32_t v = 0; v < proj->viewCount; ++v) {
            // ACE submits halves of its target (W/2 x H each): point them at view v (or v + 2). A game in eye
            // columns (ACC) has eye v's views in its half: keep the offset, move and cut to the view.
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
    XrFrameEndInfo submitted = *info;
    submitted.layers = layers.data();
    submitted.layerCount = static_cast<uint32_t>(layers.size());
    return next.EndFrame(session, &submitted);
}

}  // namespace simqvfr::layer

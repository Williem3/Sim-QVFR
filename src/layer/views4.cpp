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
#include "quad/focus_quad.h"
#include "quad/focus_turn.h"
#include "quad/helmet.h"
#include "quad/native_quad.h"
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
// e + 2), and the look from the live settings (feather, outline, corners, shape, sharpening).
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
    c.sharpness = static_cast<float>(experiment.sharpen.load(std::memory_order_relaxed)) / 100.0f;
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
    if (!v4d3d11.compositor.ready() && !v4d3d11.compositor.initialize(device, static_cast<DXGI_FORMAT>(format))) {
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

// focus_layer: the compositor (no output chain: nothing is composed at native size). False: v4d3d11.failed.
bool v4d3d11_compositor_ready(int64_t format) {
    if (v4d3d11.compositor.ready())
        return true;
    if (v4d3d11.failed)
        return false;
    ID3D11Device* device;
    {
        std::lock_guard guard(quad.mutex);
        device = sessionGraphics.d3d11;
    }
    if (!device || !v4d3d11.compositor.initialize(device, static_cast<DXGI_FORMAT>(format))) {
        log_line("views4 d3d11: compositor unavailable; the headset shows views 0-1");
        v4d3d11.failed = true;
        return false;
    }
    return true;
}

// focus_layer: the focus views' swapchain, both eyes side by side, each `w` x `h` (the game's view images: no focus
// view is larger). False: v4d3d11.focusFailed (then composed as without focus_layer), the reason logged once.
bool v4d3d11_focus_resources(XrSession session, uint32_t w, uint32_t h) {
    if (v4d3d11.focusChain)
        return true;
    if (v4d3d11.focusFailed)
        return false;
    if (!w || !h)
        return false;  // not known yet: composed this frame, tried again at the next
    v4d3d11.focusFailed = true;  // until it is made
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = kHelmetLayerFormat;  // RGBA8 sRGB: alpha for the feather, whatever the game's format
    info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
    info.width = 2 * w;
    info.height = h;
    XrSwapchain chain = XR_NULL_HANDLE;
    XrResult r = next.CreateSwapchain(session, &info, &chain);
    uint32_t count = 0;
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r) && count)
        r = next.EnumerateSwapchainImages(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r) || !count) {
        logf("views4 d3d11: focus_layer: its swapchain %ux%u failed (%d); composed as without it", info.width,
             info.height, r);
        if (chain)
            next.DestroySwapchain(chain);
        return false;
    }
    v4d3d11.focusChain = chain;
    v4d3d11.focusImages.clear();
    for (const auto& image : images)
        v4d3d11.focusImages.push_back(image.texture);
    v4d3d11.focusW = w;
    v4d3d11.focusH = h;
    v4d3d11.focusFailed = false;
    logf("views4 d3d11: focus_layer: focus views' swapchain %p %ux%u (%u images, RGBA8 sRGB, premultiplied alpha); "
         "no %ux%u output chain",
         static_cast<void*>(chain), info.width, info.height, count, 2 * quad.nativeW, quad.nativeH);
    return true;
}

// Sharpening with focus_layer (native quad views): the periphery's own swapchain, the focus chain's size in the game's
// `format`, made the first time sharpening is on. False: v4d3d11.peripheryFailed (the periphery then goes unsharpened,
// the game's own image), the reason logged once.
bool v4d3d11_periphery_resources(XrSession session, int64_t format) {
    if (v4d3d11.peripheryChain)
        return true;
    if (v4d3d11.peripheryFailed || !v4d3d11.focusW || !v4d3d11.focusH)
        return false;
    v4d3d11.peripheryFailed = true;  // until it is made
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
    info.width = 2 * v4d3d11.focusW;
    info.height = v4d3d11.focusH;
    XrSwapchain chain = XR_NULL_HANDLE;
    XrResult r = next.CreateSwapchain(session, &info, &chain);
    uint32_t count = 0;
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r) && count)
        r = next.EnumerateSwapchainImages(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r) || !count) {
        logf("views4 d3d11: sharpening: the periphery's swapchain %ux%u failed (%d); the periphery goes unsharpened",
             info.width, info.height, r);
        if (chain)
            next.DestroySwapchain(chain);
        return false;
    }
    v4d3d11.peripheryChain = chain;
    v4d3d11.peripheryImages.clear();
    for (const auto& image : images)
        v4d3d11.peripheryImages.push_back(image.texture);
    v4d3d11.peripheryFailed = false;
    logf("views4 d3d11: sharpening: the periphery's swapchain %p %ux%u (%u images, format %lld)", static_cast<void*>(chain),
         info.width, info.height, count, static_cast<long long>(format));
    return true;
}

// Sharpening with focus_layer (native quad views): each eye's outer view (its region of `views`) copied texel for texel
// into the periphery's chain, sharpened, at x = e x focusW, `outer[e]` in size; `copy` pointed at it. False: nothing
// changed (the periphery stays the game's image), the reason logged once.
bool v4d3d11_sharpened_periphery(XrSession session, int64_t format, ID3D11Texture2D* views, const EyeComposition (&eyes)[2],
                                 const XrExtent2Di (&outer)[2], std::vector<XrCompositionLayerProjectionView>& copy) {
    if (!v4d3d11_periphery_resources(session, format))
        return false;
    int rects[2][4];
    for (int e = 0; e < 2; ++e) {
        rects[e][0] = e * static_cast<int>(v4d3d11.focusW);
        rects[e][1] = 0;
        rects[e][2] = std::min(outer[e].width, static_cast<int32_t>(v4d3d11.focusW));
        rects[e][3] = std::min(outer[e].height, static_cast<int32_t>(v4d3d11.focusH));
    }
    const float sources[2][4] = {{eyes[0].outerSource[0], eyes[0].outerSource[1], eyes[0].outerSource[2], eyes[0].outerSource[3]},
                                 {eyes[1].outerSource[0], eyes[1].outerSource[1], eyes[1].outerSource[2], eyes[1].outerSource[3]}};
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(v4d3d11.peripheryChain, &acquire, &index)) ||
        index >= v4d3d11.peripheryImages.size())
        return false;
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(v4d3d11.peripheryChain, &wait)) &&
                    v4d3d11.compositor.draw_view_copies(views, v4d3d11.peripheryImages[index], sources, rects,
                                                        eyes[0].sharpness);
    next.ReleaseSwapchainImage(v4d3d11.peripheryChain, &release);
    if (!ok) {
        if (!v4d3d11.peripheryDrawFailed)
            log_line("views4 d3d11: sharpening: the periphery's copy failed; it goes unsharpened this frame");
        v4d3d11.peripheryDrawFailed = true;
        return false;
    }
    v4d3d11.peripheryDrawFailed = false;
    for (int v = 0; v < 2; ++v) {
        copy[v].subImage.swapchain = v4d3d11.peripheryChain;
        copy[v].subImage.imageRect = {{rects[v][0], rects[v][1]}, {rects[v][2], rects[v][3]}};
        copy[v].subImage.imageArrayIndex = 0;
    }
    return true;
}

}  // namespace

bool v4d3d11_tracks_chains() {
    return experiment.views4ComposeD3D11 || (experiment.views4 && game->qvfr_wants_game_image());
}

namespace {

// The game's image (eye columns: the whole target in the first eye's swapchain) to the game's adapter, with each
// view's rect in it as views4_source places it (GameAdapter::qvfr_game_image), before anything composes it.
void v4d3d11_game_image(const XrCompositionLayerProjection* proj, const Views4Layout& layout) {
    ID3D11Texture2D* image = nullptr;
    {
        std::lock_guard guard(v4d3d11.mutex);
        const auto it = v4d3d11.chains.find(proj->views[0].subImage.swapchain);
        if (it == v4d3d11.chains.end() || it->second.acquired >= it->second.images.size())
            return;
        image = it->second.images[it->second.acquired];
    }
    D3D11_TEXTURE2D_DESC desc;
    image->GetDesc(&desc);
    const XrExtent2Di eye = proj->views[0].subImage.imageRect.extent;
    const XrOffset2Di origin = proj->views[0].subImage.imageRect.offset;
    int rects[4][4];
    for (int v = 0; v < 4; ++v) {
        const ViewRect r = views4_rect_eye_columns(layout, v, 2 * eye.width, eye.height);
        rects[v][0] = origin.x + r.left;
        rects[v][1] = origin.y + r.top;
        rects[v][2] = r.width;
        rects[v][3] = r.height;
    }
    game->qvfr_game_image(image, rects);
}

// Native quad views: the outline this far in from the focus box's edge (its share of the box, inside_box), past the
// strip the runtime fades into the eyes (Pimax: the outermost lines hardly showed, 2026-11-09).
constexpr float kNativeOutlineInset = 0.05f;

// Native quad views: each eye's focus image over the runtime's focus view `region` (tangents, eye frame) for a box of
// `boxW` x `boxH` texels (the focus view as rendered): on the box's own texel grid when the focus camera is the eye's
// unturned (copied texel for texel, focus_region_image), else the region at the box's density. False: no image.
bool v4d3d11_region_image(const EyeComposition& eye, int boxW, int boxH, const float (&region)[4], RegionImage& out,
                          bool& aligned) {
    aligned = true;
    for (int i = 0; i < 9; ++i)
        aligned &= std::fabs(eye.rotation[i] - (i % 4 == 0 ? 1.0f : 0.0f)) < 1e-5f;
    if (aligned)
        return focus_region_image(eye.focusTan, boxW, boxH, region, out);
    const double sx = boxW / std::max(1e-6, static_cast<double>(eye.focusTan[1]) - eye.focusTan[0]);
    const double sy = boxH / std::max(1e-6, static_cast<double>(eye.focusTan[2]) - eye.focusTan[3]);
    out = {};
    std::memcpy(out.tan, region, sizeof(out.tan));
    out.width = static_cast<int>(std::ceil((region[1] - region[0]) * sx));
    out.height = static_cast<int>(std::ceil((region[2] - region[3]) * sy));
    return out.width >= 1 && out.height >= 1 && out.width <= 65536 && out.height <= 65536;
}

// Native quad views, every 900 race frames: how the focus views sat in the runtime's (v4d3d11.region*).
void v4d3d11_region_note() {
    const uint64_t frames = v4d3d11.regionFrames + v4d3d11.regionNone;
    if (frames < 900)
        return;
    if (v4d3d11.regionFrames)
        logf("native quad views: %llu race frames over the runtime's focus views: the focus box %.1f%% inside them on "
             "average, %.1f%% at the least (the rest shows at the periphery's density); %llu drawn smaller (larger "
             "than the focus chain), %llu not texel for texel (a turned focus camera); %llu without the runtime's "
             "focus views (the box alone)",
             static_cast<unsigned long long>(v4d3d11.regionFrames),
             100.0 * v4d3d11.regionShareSum / static_cast<double>(v4d3d11.regionFrames), 100.0 * v4d3d11.regionShareMin,
             static_cast<unsigned long long>(v4d3d11.regionScaled),
             static_cast<unsigned long long>(v4d3d11.regionUnaligned),
             static_cast<unsigned long long>(v4d3d11.regionNone));
    else
        logf("native quad views: %llu race frames without the runtime's focus views located: the box alone, at the FOV "
             "of the focus camera (a runtime that draws views 2-3 into its own focus region shows it misplaced)",
             static_cast<unsigned long long>(v4d3d11.regionNone));
    v4d3d11.regionFrames = v4d3d11.regionScaled = v4d3d11.regionUnaligned = v4d3d11.regionNone = 0;
    v4d3d11.regionShareSum = 0;
    v4d3d11.regionShareMin = 1;
}

// focus_layer, for a frame with four views (under v4d3d11.mutex and quad.mutex): each focus view drawn texel for
// texel into the focus chain, its feather in alpha, and `focus` pointed at them with the focus cameras' poses and
// FOVs; `copy` (the periphery) keeps the game's own images, cut to the outer views as rendered, with their FOV (with
// sharpening on, a sharpened copy of them in the game's `format`: v4d3d11_sharpened_periphery).
// Native quad views: views 2-3 cover exactly the runtime's focus views located for `displayTime` (a runtime may draw
// them into its own focus region whatever FOV they carry): the focus box drawn inside, the periphery around it.
// False: nothing changed (the caller composes as without focus_layer).
bool v4d3d11_layered(XrSession session, const XrCompositionLayerProjection* proj, const Views4Frame& frame,
                     const EyeComposition (&eyes)[2], ID3D11Texture2D* views, float texW, float texH, int64_t format,
                     const float (&texelsPerPixel)[2], XrTime displayTime,
                     std::vector<XrCompositionLayerProjectionView>& copy, XrCompositionLayerProjectionView (&focus)[2]) {
    int render[2] = {}, output[2] = {};
    game->qvfr_target_sizes(render, output);  // the game's view images (LMU: one size for all four)
    // Each view's size as rendered (live resolution), from its region of the views' image.
    const auto pixels = [](float share, float size, uint32_t most) {
        return static_cast<int32_t>(std::clamp<long>(std::lround(share * size), 1, static_cast<long>(most)));
    };
    const uint32_t most[2] = {static_cast<uint32_t>(std::max(render[0], 1)), static_cast<uint32_t>(std::max(render[1], 1))};
    int boxPx[2][2];
    for (int e = 0; e < 2; ++e) {
        boxPx[e][0] = pixels(eyes[e].focusSource[2], texW, most[0]);
        boxPx[e][1] = pixels(eyes[e].focusSource[3], texH, most[1]);
    }
    // Native quad views: the image over the runtime's focus views, each eye's.
    float regions[2][4];
    RegionImage regionImage[2];
    bool aligned[2] = {true, true};
    bool overRegion = nativeQuad.active && runtime_focus_regions(displayTime, regions);
    for (int e = 0; overRegion && e < 2; ++e)
        overRegion = v4d3d11_region_image(eyes[e], boxPx[e][0], boxPx[e][1], regions[e], regionImage[e], aligned[e]);
    // The focus chain, made at the first such frame: the game's view images, or the runtime's focus views at the
    // focus density (the launch density: live changes stay at or under it) and a few texels more, within 8192 per eye
    // across. Not made while the game's view size is unknown.
    uint32_t chainW = render[0] > 0 && render[1] > 0 ? most[0] : 0, chainH = chainW ? most[1] : 0;
    if (overRegion && chainW)
        for (const RegionImage& r : regionImage) {
            chainW = std::max(chainW, static_cast<uint32_t>(std::min(8192, r.width + 16)));
            chainH = std::max(chainH, static_cast<uint32_t>(std::min(16384, r.height + 16)));
        }
    if (!v4d3d11_focus_resources(session, chainW, chainH))
        return false;
    int rects[2][4];
    XrExtent2Di outer[2];
    bool scaled = false;
    for (int e = 0; e < 2; ++e) {
        rects[e][0] = e * static_cast<int>(v4d3d11.focusW);
        rects[e][1] = 0;
        rects[e][2] = std::min(overRegion ? regionImage[e].width : boxPx[e][0], static_cast<int>(v4d3d11.focusW));
        rects[e][3] = std::min(overRegion ? regionImage[e].height : boxPx[e][1], static_cast<int>(v4d3d11.focusH));
        scaled |= overRegion && (rects[e][2] < regionImage[e].width || rects[e][3] < regionImage[e].height);
        const XrExtent2Di gameRect = proj->views[e].subImage.imageRect.extent;
        outer[e] = {pixels(eyes[e].outerSource[2], texW, static_cast<uint32_t>(std::max(gameRect.width, 1))),
                    pixels(eyes[e].outerSource[3], texH, static_cast<uint32_t>(std::max(gameRect.height, 1)))};
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(v4d3d11.focusChain, &acquire, &index)) ||
        index >= v4d3d11.focusImages.size())
        return false;
    // A quad view session: opaque focus views (the runtime blends them into the eyes itself, and takes no alpha from
    // a projection layer's views 2-3), the periphery blended into their edge by the feather and the outline drawn
    // kNativeOutlineInset in from it (FocusLayerPS); otherwise the feather in alpha.
    EyeComposition drawn[2] = {eyes[0], eyes[1]};
    if (nativeQuad.active)
        for (int e = 0; e < 2; ++e) {
            EyeComposition& d = drawn[e];
            d.viewTan[0] = 1;
            d.viewTan[1] = kNativeOutlineInset;
            d.viewTan[2] = d.viewTan[3] = 0;
            if (overRegion)
                std::memcpy(d.regionTan, regionImage[e].tan, sizeof(d.regionTan));
        }
    // Sharpening: the focus views' texels per display pixel (FocusLayerPS sharpens at the display's scale above 1.25).
    for (int e = 0; e < 2; ++e)
        drawn[e].viewTan[2] = texelsPerPixel[e];
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(v4d3d11.focusChain, &wait)) &&
                    v4d3d11.compositor.draw_focus_layer(views, v4d3d11.focusImages[index], drawn, rects);
    next.ReleaseSwapchainImage(v4d3d11.focusChain, &release);
    if (!ok)
        return false;
    for (uint32_t v = 0; v < 2; ++v) {
        // The periphery: the game's own image (its outer view at the top left), the FOV it was rendered over. The
        // game's depth info, if any, is dropped as when composed (the proof's: its rect would need cutting too).
        copy[v].fov = tangents_fov(eyes[v].outerTan);
        copy[v].next = nullptr;
        copy[v].subImage.imageRect.extent = outer[v];
        // The focus view: the eye's position, the focus camera's turn (a direction in the eye's frame is in the
        // focus view's after the inverse turn, views4_eye), its own FOV and pixels.
        // Over the runtime's focus view: the eye's own pose and the region image's FOV (the runtime's, out to whole
        // texels of the box's grid).
        focus[v] = copy[v];
        focus[v].pose.orientation = overRegion ? copy[v].pose.orientation : qmul(copy[v].pose.orientation, frame.turn[v]);
        focus[v].fov = overRegion ? tangents_fov(regionImage[v].tan) : tangents_fov(eyes[v].focusTan);
        focus[v].subImage.swapchain = v4d3d11.focusChain;
        focus[v].subImage.imageRect = {{rects[v][0], rects[v][1]}, {rects[v][2], rects[v][3]}};
        focus[v].subImage.imageArrayIndex = 0;
    }
    if (nativeQuad.active) {
        if (overRegion) {
            ++v4d3d11.regionFrames;
            v4d3d11.regionScaled += scaled;
            v4d3d11.regionUnaligned += !(aligned[0] && aligned[1]);
            for (int e = 0; e < 2; ++e) {
                const double share = box_share_in_region(eyes[e].focusTan, regions[e]);
                v4d3d11.regionShareSum += 0.5 * share;
                v4d3d11.regionShareMin = std::min(v4d3d11.regionShareMin, share);
            }
            if (!v4d3d11.regionLogged) {
                v4d3d11.regionLogged = true;
                logf("native quad views: the first race frame over the runtime's focus views (eye 0: the runtime's "
                     "%.3f %.3f %.3f %.3f, the box %.3f %.3f %.3f %.3f in tangents; the image %dx%d, the box's %dx%d "
                     "texels at %d,%d in it%s; the focus chain %ux%u per eye)",
                     regions[0][0], regions[0][1], regions[0][2], regions[0][3], eyes[0].focusTan[0], eyes[0].focusTan[1],
                     eyes[0].focusTan[2], eyes[0].focusTan[3], regionImage[0].width, regionImage[0].height,
                     boxPx[0][0], boxPx[0][1], regionImage[0].boxX, regionImage[0].boxY,
                     aligned[0] ? ", texel for texel" : ", resampled (a turned focus camera)", v4d3d11.focusW,
                     v4d3d11.focusH);
            }
        } else {
            ++v4d3d11.regionNone;
        }
        v4d3d11_region_note();
    }
    // Sharpening: the focus views were drawn sharpened (FocusLayerPS); the periphery a sharpened copy of its own.
    const bool sharpened = eyes[0].sharpness > 0 && v4d3d11_sharpened_periphery(session, format, views, eyes, outer, copy);
    if (sharpened != v4d3d11.peripherySharpened) {
        logf("views4 d3d11: sharpening %.0f%%: the periphery %s", eyes[0].sharpness * 100,
             sharpened ? "a sharpened copy of the game's image" : "the game's own image");
        v4d3d11.peripherySharpened = sharpened;
    }
    if (v4d3d11.layered++ == 0 || v4d3d11.lastFallback) {
        logf("views4 d3d11: %s (eye 0: periphery %dx%d of the game's image at %d,%d, FOV tangents %.3f %.3f %.3f %.3f; "
             "focus %dx%d over tangents %.3f %.3f %.3f %.3f, feather %.0f%%)",
             nativeQuad.active ? "native quad views: the eyes and the focus views as one four-view layer"
                               : "focus_layer: two layers",
             outer[0].width, outer[0].height, copy[0].subImage.imageRect.offset.x, copy[0].subImage.imageRect.offset.y,
             eyes[0].outerTan[0], eyes[0].outerTan[1], eyes[0].outerTan[2], eyes[0].outerTan[3], rects[0][2],
             rects[0][3], eyes[0].focusTan[0], eyes[0].focusTan[1], eyes[0].focusTan[2], eyes[0].focusTan[3],
             experiment.feather * 100);
        v4d3d11.lastFallback = nullptr;
    }
    return true;
}

// Composes both eyes of `proj` (the game's two eye views, eye columns of `layout`) into the output chain and
// points `copy` (its views) at it. False: nothing changed, the caller submits views 0-1 as they are.
// With focus_layer (`focus` given, separate views, the helmet off): nothing composed; `copy` the periphery and
// `focus` the focus views' layer (v4d3d11_layered, over the runtime's focus views for `displayTime` under native quad
// views), `*layered` true.
bool v4d3d11_compose(XrSession session, const XrCompositionLayerProjection* proj,
                     std::vector<XrCompositionLayerProjectionView>& copy,
                     XrCompositionLayerProjectionView (*focus)[2] = nullptr, bool* layered = nullptr,
                     XrTime displayTime = 0) {
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
    // focus_layer needs only the compositor; whether this frame is layered is known once its views are (below). The
    // output chain is made there too, before quad.mutex is taken (v4d3d11_resources takes it).
    const bool tryLayered =
        focus && layered && (experiment.focusLayer || nativeQuad.active) && !v4d3d11.focusFailed;
    if (layered)
        *layered = false;
    if (!v4d3d11_compositor_ready(it->second.format))
        return false;
    ID3D11Texture2D* views = it->second.images[it->second.acquired];
    // Separate views (qvfr_separate_views, LMU): each eye's image holds only its outer view; the adapter's atlas holds all
    // four, the submitted eyes copied in, with each view's region.
    const bool separate = game->qvfr_separate_views();
    float atlasUv[4][4] = {};
    if (separate) {
        const auto right = v4d3d11.chains.find(proj->views[1].subImage.swapchain);
        if (right == v4d3d11.chains.end() || right->second.acquired >= right->second.images.size())
            return fallback("the game's right eye image is unknown");
        ID3D11Texture2D* const eyeImages[2] = {views, right->second.images[right->second.acquired]};
        views = game->qvfr_views_atlas(eyeImages, atlasUv);
        if (!views)
            return fallback("no four views this frame");
    }
    D3D11_TEXTURE2D_DESC desc;
    views->GetDesc(&desc);
    const float texW = static_cast<float>(desc.Width), texH = static_cast<float>(desc.Height);
    // focus_layer only for a scene frame (the game's atlas of its four views: two of its view images wide and high)
    // with the helmet off. The menus (a native-size image of their own) and the helmet are composed as without it.
    bool useLayered = false;
    if (tryLayered) {
        int render[2] = {}, output[2] = {};
        game->qvfr_target_sizes(render, output);
        bool helmetOn;
        {
            std::lock_guard helmetGuard(quad.mutex);
            helmetOn = helmetSettings.on;
        }
        const bool sceneAtlas = separate && render[0] > 0 && render[1] > 0 &&
                                desc.Width == 2u * static_cast<UINT>(render[0]) &&
                                desc.Height == 2u * static_cast<UINT>(render[1]);
        useLayered = sceneAtlas && !helmetOn;
        if (sceneAtlas && helmetOn && !v4d3d11.helmetComposedLogged) {
            log_line("views4 d3d11: focus_layer: the helmet is on: composed as without focus_layer while it is");
            v4d3d11.helmetComposedLogged = true;
        }
    }
    if (!useLayered && !v4d3d11_resources(session, it->second.format))
        return false;
    // Before the game's first record (or with no projection in it), the submitted FOVs, centred and unturned.
    if (!recorded || !(frame.outerTan[0][1] > frame.outerTan[0][0]) || !(frame.outerTan[1][1] > frame.outerTan[1][0]))
        frame = views4_unrecorded_frame({proj->views[0].fov, proj->views[1].fov});
    // The game's target as the eyes' rects give it (eye e: the half from e x W), as views4_submit reads it.
    const XrExtent2Di eye = proj->views[0].subImage.imageRect.extent;
    const XrOffset2Di origin = proj->views[0].subImage.imageRect.offset;
    const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
    const auto source = [&](int view, float (&uv)[4]) {
        if (separate) {
            std::memcpy(uv, atlasUv[view], sizeof(uv));
            return;
        }
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
    if (useLayered) {
        // The focus views' texels per display pixel across: their pixels per tangent over the native eye's (the
        // output's size over the FOV it covers, as composed), for the runtime that shows them (FocusLayerPS).
        float texelsPerPixel[2] = {};
        for (int e = 0; e < 2; ++e) {
            float fov[4];
            fov_tangents(submitFov[e], fov);
            const float focusPerTan = eyes[e].focusSource[2] * texW / std::max(eyes[e].focusTan[1] - eyes[e].focusTan[0], 1e-6f);
            const float nativePerTan = static_cast<float>(extent[e].width) / std::max(fov[1] - fov[0], 1e-6f);
            texelsPerPixel[e] = nativePerTan > 0 ? focusPerTan / nativePerTan : 0.0f;
        }
        if (eyes[0].sharpness > 0 && std::abs(texelsPerPixel[0] - v4d3d11.loggedTexelsPerPixel) > 0.05f) {
            logf("views4 d3d11: sharpening: the focus views at %.2f texels per display pixel%s", texelsPerPixel[0],
                 texelsPerPixel[0] > 1.25f ? " (sharpened at the display's scale)" : "");
            v4d3d11.loggedTexelsPerPixel = texelsPerPixel[0];
        }
        if (v4d3d11_layered(session, proj, frame, eyes, views, texW, texH, it->second.format, texelsPerPixel,
                            displayTime, copy, *focus)) {
            *layered = true;
            return true;
        }
        // Composed from now on: the output chain can't be made here (under quad.mutex), so this frame only if a menu
        // made it already; otherwise the next frame makes it.
        if (!v4d3d11.focusFailed)
            log_line("views4 d3d11: focus_layer: the focus layer failed; composed as without focus_layer from now on");
        v4d3d11.focusFailed = true;
        if (!v4d3d11.outChain)
            return fallback("focus_layer failed, the output chain is made at the next frame");
    }
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
        // Separate views (qvfr_separate_views, LMU): each eye an image of its own, whole; composed from the adapter's atlas,
        // or (no four views this frame: menus, loading) submitted as it is. None of the shared-target rects below apply.
        if (experiment.views4ComposeD3D11 && proj->viewCount == 2 && game->qvfr_separate_views()) {
            bool layered = false;
            if (v4d3d11_compose(session, proj, copy, focusAfter < 0 ? &focusViews : nullptr, &layered,
                                info->displayTime)) {
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

// views4 on D3D11: composing the game's four views into the eyes' output chain, or, with focus_layer and native quad
// views, the focus views' own layer and the periphery (the state: v4d3d11, state.h). Called from views4_submit.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "probe/frame_parts.h"
#include "games/adapter.h"
#include "log.h"
#include "probe/d3d11_capture.h"
#include "quad/compositor.h"
#include "quad/focus_turn.h"
#include "quad/helmet.h"
#include "quad/native_quad.h"
#include "quad/output_density.h"
#include "quad/showcase.h"
#include "xr.h"
#include "layer/layer.h"
#include "layer/views4_internal.h"

namespace simqvfr::layer {

namespace {

// The D3D11 output chain and compositor, made at the first composition (the game's format, double wide, each eye the
// native eye at the focus density's scale: quad/output_density.h). Made again larger when a live change wants more
// than it holds; the old chain goes a few compositions later. False: v4d3d11.failed, with the reason logged once.
bool v4d3d11_resources(XrSession session, int64_t format) {
    if (v4d3d11.retiredChain && --v4d3d11.retiredFrames <= 0) {
        next.DestroySwapchain(v4d3d11.retiredChain);
        v4d3d11.retiredChain = XR_NULL_HANDLE;
    }
    const bool growing = v4d3d11.outChain != XR_NULL_HANDLE;
    if (growing) {
        bool grow;
        {
            std::lock_guard guard(quad.mutex);
            const OutputDensity& d = v4d3d11.outDensity;
            grow = !v4d3d11.growFailed && d.target > d.chainScale + 1e-9 && d.cap > d.chainScale + 1e-9;
        }
        // Not while its image is left acquired (a timed-out wait) or the last old chain is still waiting to go.
        if (!grow || v4d3d11.outPending.acquired || v4d3d11.retiredChain)
            return true;
    }
    if (v4d3d11.failed)
        return false;
    if (!growing)
        v4d3d11.failed = true;  // until everything below is made
    uint32_t nativeW, nativeH;
    ID3D11Device* device;
    OutputDensity density;
    {
        std::lock_guard guard(quad.mutex);
        nativeW = quad.nativeW;
        nativeH = quad.nativeH;
        device = sessionGraphics.d3d11;
        density = v4d3d11.outDensity;
        if (nativeW && nativeH)
            views4_output_chain(density);
    }
    if (!device || !nativeW || !nativeH) {
        log_line("views4 d3d11: no D3D11 device or native eye size; the headset shows views 0-1");
        return growing;
    }
    if (!v4d3d11.compositor.ready() && !v4d3d11.compositor.initialize(device, static_cast<DXGI_FORMAT>(format))) {
        log_line("views4 d3d11: compositor unavailable; the headset shows views 0-1");
        return false;
    }
    XrSwapchainCreateInfo out{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    out.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    out.format = format;
    out.sampleCount = out.faceCount = out.arraySize = out.mipCount = 1;
    out.width = 2 * density.eyeW;
    out.height = density.eyeH;
    XrSwapchain chain = XR_NULL_HANDLE;
    XrResult r = next.CreateSwapchain(session, &out, &chain);
    uint32_t count = 0;
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r)) {
        if (chain)
            next.DestroySwapchain(chain);
        if (growing) {  // the chain it has stays, at its own size
            logf("views4 d3d11: a larger output chain %ux%u failed (%d); the output stays %ux%u", out.width, out.height,
                 r, 2 * v4d3d11.outW, v4d3d11.outH);
            v4d3d11.growFailed = true;
            return true;
        }
        logf("views4 d3d11: output swapchain %ux%u failed (%d); the headset shows views 0-1", out.width, out.height, r);
        v4d3d11.compositor.shutdown();
        return false;
    }
    if (growing) {
        // The old chain's last image went out with an earlier frame; the runtime may still read it for a few.
        v4d3d11.retiredChain = v4d3d11.outChain;
        v4d3d11.retiredFrames = 8;
        v4d3d11.outImages.clear();
    }
    v4d3d11.outChain = chain;
    for (const auto& image : images)
        v4d3d11.outImages.push_back(image.texture);
    d3d11_capture_images(chain, images.data(), count);  // d3d11_capture_frame saves the composed output too
    {
        std::lock_guard guard(quad.mutex);
        v4d3d11.outDensity = density;
        v4d3d11.outDensity.logged = 0;  // its scale logged again at the next composition
    }
    v4d3d11.outW = density.eyeW;
    v4d3d11.outH = density.eyeH;
    v4d3d11.failed = false;
    logf("views4 d3d11: %soutput chain %p %ux%u (%u images, format %lld, ~%.0f MB at 4 bytes a pixel; %.0f%% of the %ux%u "
         "native eye, up to %.0f%%), focus box %.0f%% x %.0f%%, feather %.0f%%",
         growing ? "larger " : "", static_cast<void*>(chain), out.width, out.height, count,
         static_cast<long long>(format), 4.0 * out.width * out.height * count / (1024.0 * 1024.0),
         density.chainScale * 100, nativeW, nativeH, density.cap * 100,
         experiment.box_x() * 100, experiment.box_y() * 100, experiment.feather * 100);
    return true;
}

// The showcase's Stock (quad/showcase.h): after kStockIdleFrames frames of the eyes submitted as they are, the output
// chain is let go, and v4d3d11_resources makes it again at the first composition back. Left alone while Stock shows,
// Windows pages it out to make room for the native-size eyes and back in at the switch: a 5 s freeze on a 16 GB card at
// native-size targets (2026-11-17).
constexpr int kStockIdleFrames = 8;  // the runtime is surely done with the chain's last image
int stockIdleFrames = 0;
void v4d3d11_release_for_stock() {
    if (v4d3d11.retiredChain) {
        next.DestroySwapchain(v4d3d11.retiredChain);
        v4d3d11.retiredChain = XR_NULL_HANDLE;
        v4d3d11.retiredFrames = 0;
    }
    if (!v4d3d11.outChain || v4d3d11.outPending.acquired)
        return;
    const double mb = 4.0 * 2 * v4d3d11.outW * v4d3d11.outH * static_cast<double>(v4d3d11.outImages.size()) /
                      (1024.0 * 1024.0);
    next.DestroySwapchain(v4d3d11.outChain);
    v4d3d11.outChain = XR_NULL_HANDLE;
    v4d3d11.outImages.clear();
    v4d3d11.outW = v4d3d11.outH = 0;
    v4d3d11.growFailed = false;
    logf("views4 d3d11: showcase: Stock: the output chain let go (~%.0f MB), made again at the switch back", mb);
    log_vram("Stock, the output chain let go");
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

}  // namespace

bool v4d3d11_tracks_chains() {
    return experiment.views4ComposeD3D11 || (experiment.views4 && game->qvfr_wants_game_image());
}

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

namespace {

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

// Native quad views with the FOV crop, a race frame (under v4d3d11.mutex): each eye's periphery as rendered (`copy`,
// the game's own image) copied texel for texel into the pad chain over the runtime's whole located eye, black around
// it, and `copy` pointed there with that FOV. The runtime may draw views 0-1 over its eye whatever FOV they carry
// (issue 29: Pimax stretched the cropped periphery over the whole eye, a picture in the picture). False: `copy` as it
// was (no crop, the runtime's eyes unknown, or a chain, view or copy that failed or wasn't ready this frame).
// `device` the session's (sessionGraphics.d3d11): read by the caller, which may hold quad.mutex (the race path does), so
// this takes no lock but v4d3d11.mutex's, held by the caller too.
// `grade` (colour correction, color_grade_constants'; a race frame's): the periphery drawn graded into the pad chain
// instead of copied (compose_shader.h GradePS), padded or not (without the runtime's eyes too: each eye as rendered), so
// it matches the graded focus views; `*graded` says whether it was, the focus views graded only then.
bool v4d3d11_pad_periphery(XrSession session, XrTime displayTime, std::vector<XrCompositionLayerProjectionView>& copy,
                           ID3D11Device* device, const float (*grade)[3] = nullptr, bool* graded = nullptr) {
    if (graded)
        *graded = false;
    if (v4d3d11.padRetired && --v4d3d11.padRetiredFrames <= 0) {
        next.DestroySwapchain(v4d3d11.padRetired);
        v4d3d11.padRetired = XR_NULL_HANDLE;
    }
    if (grade && v4d3d11.padGradeRetry > 0) {  // a graded copy failed lately: ungraded (and the focus views) a while
        --v4d3d11.padGradeRetry;
        grade = nullptr;
    }
    if (grade && v4d3d11.padGradeFailed)
        grade = nullptr;
    float runtimeEyes[2][4];
    const bool eyesKnown = runtime_eye_tangents(displayTime, runtimeEyes);
    if (!device || v4d3d11.padFailed || copy.size() < 2 || (!eyesKnown && !grade))
        return false;
    RegionImage pad[2];
    bool any = false;
    for (int e = 0; e < 2; ++e) {
        const XrExtent2Di ext = copy[e].subImage.imageRect.extent;
        float tan[4];
        fov_tangents(copy[e].fov, tan);  // the FOV the view was rendered over
        if (eyesKnown && narrower_than_eye(tan, runtimeEyes[e]) &&
            eye_pad_image(tan, ext.width, ext.height, runtimeEyes[e], pad[e])) {
            any = true;
            continue;
        }
        std::memcpy(pad[e].tan, tan, sizeof(pad[e].tan));  // this eye as it is, in the pad chain too
        pad[e].width = ext.width;
        pad[e].height = ext.height;
        pad[e].boxX = pad[e].boxY = 0;
    }
    if (!any && !grade)
        return false;
    // Off for the session: the padding when the crop needed it (with colour correction, both), only colour correction
    // when the copy was for it alone, so a crop turned on later is still padded (issue 29).
    const auto fail = [any](const char* why) {
        if (any) {
            logf("native quad views: the FOV crop's periphery can't be padded out to the runtime's eyes (%s): views 0-1 "
                 "as rendered (the runtime may stretch them)",
                 why);
            v4d3d11.padFailed = true;
        } else {
            logf("native quad views: colour correction off: the periphery can't be copied to grade it (%s)", why);
            v4d3d11.padGradeFailed = true;
        }
        return false;
    };
    ID3D11Texture2D* source[2] = {};
    int64_t format = 0;
    for (int e = 0; e < 2; ++e) {
        const auto it = v4d3d11.chains.find(copy[e].subImage.swapchain);
        if (it == v4d3d11.chains.end() || it->second.acquired >= it->second.images.size())
            return false;  // not this frame
        source[e] = it->second.images[it->second.acquired];
        if (e == 1 && it->second.format != format)
            return fail("the eyes' images in different formats");
        format = it->second.format;
    }
    D3D11_TEXTURE2D_DESC sourceDesc;
    source[0]->GetDesc(&sourceDesc);
    if (sourceDesc.SampleDesc.Count > 1)
        return fail("multisampled eye images");
    const uint32_t needW = static_cast<uint32_t>(std::max(pad[0].width, pad[1].width));
    const uint32_t needH = static_cast<uint32_t>(std::max(pad[0].height, pad[1].height));
    if (!v4d3d11.padChain || needW > v4d3d11.padW || needH > v4d3d11.padH || format != v4d3d11.padFormat) {
        // Not while its image is left acquired (a timed-out wait) or the last old chain is still waiting to go.
        if (v4d3d11.padChain && (v4d3d11.padPending.acquired || v4d3d11.padRetired))
            return false;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        // A few texels to spare, for live resolution's small changes.
        const uint32_t eyeW = std::min<uint32_t>(8192, needW + 16), eyeH = std::min<uint32_t>(16384, needH + 16);
        info.width = 2 * eyeW;
        info.height = eyeH;
        XrSwapchain chain = XR_NULL_HANDLE;
        XrResult r = next.CreateSwapchain(session, &info, &chain);
        uint32_t count = 0;
        if (XR_SUCCEEDED(r))
            r = next.EnumerateSwapchainImages(chain, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_SUCCEEDED(r) && count)
            r = next.EnumerateSwapchainImages(chain, count, &count,
                                              reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(r) || !count) {
            if (chain)
                next.DestroySwapchain(chain);
            logf("native quad views: the periphery's pad chain %ux%u failed (%d)", info.width, info.height, r);
            return fail("its swapchain failed");
        }
        if (v4d3d11.padChain) {
            // The old chain's last image went out with an earlier frame; the runtime may still read it for a few.
            v4d3d11.padRetired = v4d3d11.padChain;
            v4d3d11.padRetiredFrames = 8;
        }
        for (ID3D11RenderTargetView* t : v4d3d11.padTargets)  // the old images' views (the runtime never sees them)
            if (t)
                t->Release();
        v4d3d11.padTargets.assign(count, nullptr);
        v4d3d11.padChain = chain;
        v4d3d11.padImages.clear();
        for (const auto& image : images)
            v4d3d11.padImages.push_back(image.texture);
        v4d3d11.padFormat = format;
        v4d3d11.padW = eyeW;
        v4d3d11.padH = eyeH;
        v4d3d11.padPending = {};
        logf("native quad views: the periphery padded out to the runtime's eyes in a chain of its own %ux%u (%u images, "
             "format %lld; eye 0: its %dx%d texels at %d,%d of %dx%d over tangents %.3f %.3f %.3f %.3f, black around)",
             info.width, info.height, count, static_cast<long long>(format), copy[0].subImage.imageRect.extent.width,
             copy[0].subImage.imageRect.extent.height, pad[0].boxX, pad[0].boxY, pad[0].width, pad[0].height,
             pad[0].tan[0], pad[0].tan[1], pad[0].tan[2], pad[0].tan[3]);
    }
    uint32_t index = 0;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult waited = acquire_wait(v4d3d11.padChain, v4d3d11.padPending, kImageWaitNs, kImageWaitTries, index,
                                         "native quad views: pad image wait");
    if (waited != XR_SUCCESS)
        return false;  // timed out: waited on again at the next frame
    if (index >= v4d3d11.padImages.size() || index >= v4d3d11.padTargets.size() || !device) {
        next.ReleaseSwapchainImage(v4d3d11.padChain, &release);
        return false;
    }
    ID3D11Texture2D* target = v4d3d11.padImages[index];
    if (!v4d3d11.padTargets[index]) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = static_cast<DXGI_FORMAT>(format);
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateRenderTargetView(target, &rtv, &v4d3d11.padTargets[index]))) {
            v4d3d11.padTargets[index] = nullptr;
            next.ReleaseSwapchainImage(v4d3d11.padChain, &release);
            return fail("no render target view of its image");
        }
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const float black[4] = {0, 0, 0, 1};
    context->ClearRenderTargetView(v4d3d11.padTargets[index], black);
    // Colour correction: each eye drawn graded, texel for texel, where it would be copied. A source the compositor's
    // views can't read (an array image) turns colour correction off here for the session (the focus views with it).
    bool drewGraded = false;
    if (grade) {
        D3D11_TEXTURE2D_DESC d;
        source[0]->GetDesc(&d);
        if (d.ArraySize > 1 || copy[0].subImage.imageArrayIndex || copy[1].subImage.imageArrayIndex) {
            v4d3d11.padGradeFailed = true;  // the images stay arrays: for the session
            log_line("native quad views: colour correction off: the game's eye images are arrays (the periphery's "
                     "copy can't be graded)");
        } else {
            int from[2][4], to[2][2];
            for (int e = 0; e < 2; ++e) {
                const XrRect2Di rect = copy[e].subImage.imageRect;
                from[e][0] = rect.offset.x;
                from[e][1] = rect.offset.y;
                from[e][2] = rect.extent.width;
                from[e][3] = rect.extent.height;
                to[e][0] = static_cast<int>(e * v4d3d11.padW) + pad[e].boxX;
                to[e][1] = pad[e].boxY;
            }
            drewGraded = v4d3d11.compositor.draw_graded(source, from, v4d3d11.padTargets[index], to, *grade);
            if (!drewGraded) {  // tried again in a second or so, the focus views ungraded meanwhile
                v4d3d11.padGradeRetry = 90;
                if (!v4d3d11.padGradeLogged)
                    log_line("native quad views: the periphery's graded copy failed: colour correction paused, tried "
                             "again in a second");
                v4d3d11.padGradeLogged = true;
            }
        }
    }
    if (graded)
        *graded = drewGraded;
    for (int e = 0; !drewGraded && e < 2; ++e) {
        D3D11_TEXTURE2D_DESC d;
        source[e]->GetDesc(&d);
        const XrRect2Di rect = copy[e].subImage.imageRect;
        const D3D11_BOX box{static_cast<UINT>(rect.offset.x), static_cast<UINT>(rect.offset.y), 0,
                            static_cast<UINT>(rect.offset.x + rect.extent.width),
                            static_cast<UINT>(rect.offset.y + rect.extent.height), 1};
        context->CopySubresourceRegion(target, 0, static_cast<UINT>(e * v4d3d11.padW + pad[e].boxX),
                                       static_cast<UINT>(pad[e].boxY), 0, source[e],
                                       D3D11CalcSubresource(0, copy[e].subImage.imageArrayIndex, d.MipLevels), &box);
    }
    context->Release();
    next.ReleaseSwapchainImage(v4d3d11.padChain, &release);
    for (int e = 0; e < 2; ++e) {
        copy[e].fov = tangents_fov(pad[e].tan);
        copy[e].subImage.swapchain = v4d3d11.padChain;
        copy[e].subImage.imageRect = {{static_cast<int32_t>(e * v4d3d11.padW), 0}, {pad[e].width, pad[e].height}};
        copy[e].subImage.imageArrayIndex = 0;
    }
    ++v4d3d11.padded;
    return true;
}

// focus_layer, for a frame with four views (under v4d3d11.mutex and quad.mutex): each focus view drawn texel for
// texel into the focus chain, its feather in alpha, and `focus` pointed at them with the focus cameras' poses and
// FOVs; `copy` (the periphery) keeps the game's own images, cut to the outer views as rendered, with their FOV (never
// sharpened: only the focus views are).
// Native quad views: views 2-3 cover exactly the runtime's focus views located for `displayTime` (a runtime may draw
// them into its own focus region whatever FOV they carry): the focus box drawn inside, the periphery around it.
// False: nothing changed (the caller composes as without focus_layer), or, `*focusTimedOut` set, the focus chain's
// image wasn't ready in time (this frame only: the caller submits the game's views as they are).
bool v4d3d11_layered(XrSession session, const XrCompositionLayerProjection* proj, const Views4Frame& frame,
                     const EyeComposition (&eyes)[2], ID3D11Texture2D* views, float texW, float texH,
                     const float (&texelsPerPixel)[2], XrTime displayTime,
                     std::vector<XrCompositionLayerProjectionView>& copy, XrCompositionLayerProjectionView (&focus)[2],
                     bool* focusTimedOut) {
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
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    // Bounded (in the game's xrEndFrame, under v4d3d11.mutex and quad.mutex). Timed out: the image stays acquired (not
    // released), waited on again at the next frame; this one goes out without its focus layer.
    const XrResult waited = acquire_wait(v4d3d11.focusChain, v4d3d11.focusPending, kImageWaitNs, kImageWaitTries, index,
                                         "views4 d3d11: focus layer image wait");
    if (waited == XR_TIMEOUT_EXPIRED) {
        *focusTimedOut = true;
        return false;
    }
    if (waited != XR_SUCCESS)
        return false;
    if (index >= v4d3d11.focusImages.size()) {
        next.ReleaseSwapchainImage(v4d3d11.focusChain, &release);
        return false;
    }
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
    // The periphery: the game's own image (its outer view at the top left), the FOV it was rendered over. The game's
    // depth info, if any, is dropped as when composed (the proof's: its rect would need cutting too). Made in a copy of
    // `copy`, which stays as it was if the focus layer fails below.
    std::vector<XrCompositionLayerProjectionView> periphery = copy;
    for (uint32_t v = 0; v < 2; ++v) {
        periphery[v].fov = tangents_fov(eyes[v].outerTan);
        periphery[v].next = nullptr;
        periphery[v].subImage.imageRect.extent = outer[v];
    }
    // Native quad views: the periphery padded out to the runtime's eyes with the FOV crop (issue 29), and drawn graded
    // with colour correction. First, so the focus views are graded only when the periphery was: the two always match.
    // With focus_layer alone the runtime shows the game's own periphery, so neither is graded.
    // Under quad.mutex (v4d3d11_compose): the device read here, not inside (a second lock of it threw: issue 29).
    const bool wantGrade = nativeQuad.active && drawn[0].color[0] > 0;
    bool graded = false;
    const bool padded = nativeQuad.active && v4d3d11_pad_periphery(session, displayTime, periphery, sessionGraphics.d3d11,
                                                                   wantGrade ? &drawn[0].color : nullptr, &graded);
    if (!graded)
        for (EyeComposition& d : drawn)
            std::memset(d.color, 0, sizeof(d.color));
    v4d3d11.gradeState = wantGrade ? (graded ? 0 : 1) : eyes[0].color[0] > 0 && !nativeQuad.active ? 2 : 0;
    if (eyes[0].color[0] > 0 && !nativeQuad.active && !v4d3d11.gradeLayerLogged) {
        v4d3d11.gradeLayerLogged = true;
        log_line("views4 d3d11: focus_layer: colour correction isn't applied (the runtime shows the game's own periphery)");
    }
    const bool ok = v4d3d11.compositor.draw_focus_layer(views, v4d3d11.focusImages[index], drawn, rects);
    next.ReleaseSwapchainImage(v4d3d11.focusChain, &release);
    if (!ok)
        return false;
    for (uint32_t v = 0; v < 2; ++v) {
        // The focus view: the eye's position, the focus camera's turn (a direction in the eye's frame is in the
        // focus view's after the inverse turn, views4_eye), its own FOV and pixels.
        // Over the runtime's focus view: the eye's own pose and the region image's FOV (the runtime's, out to whole
        // texels of the box's grid).
        focus[v] = periphery[v];
        focus[v].pose.orientation = overRegion ? periphery[v].pose.orientation : qmul(periphery[v].pose.orientation, frame.turn[v]);
        focus[v].fov = overRegion ? tangents_fov(regionImage[v].tan) : tangents_fov(eyes[v].focusTan);
        focus[v].subImage.swapchain = v4d3d11.focusChain;
        focus[v].subImage.imageRect = {{rects[v][0], rects[v][1]}, {rects[v][2], rects[v][3]}};
        focus[v].subImage.imageArrayIndex = 0;
    }
    copy.swap(periphery);
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
    if (v4d3d11.layered++ == 0 || v4d3d11.lastFallback) {
        logf("views4 d3d11: %s (eye 0: periphery %dx%d of the game's image at %d,%d, FOV tangents %.3f %.3f %.3f %.3f; "
             "focus %dx%d over tangents %.3f %.3f %.3f %.3f, feather %.0f%%)%s",
             nativeQuad.active ? "native quad views: the eyes and the focus views as one four-view layer"
                               : "focus_layer: two layers",
             outer[0].width, outer[0].height, copy[0].subImage.imageRect.offset.x, copy[0].subImage.imageRect.offset.y,
             eyes[0].outerTan[0], eyes[0].outerTan[1], eyes[0].outerTan[2], eyes[0].outerTan[3], rects[0][2],
             rects[0][3], eyes[0].focusTan[0], eyes[0].focusTan[1], eyes[0].focusTan[2], eyes[0].focusTan[3],
             experiment.feather * 100, padded ? "; the periphery padded out to the runtime's eyes" : "");
        v4d3d11.lastFallback = nullptr;
    }
    return true;
}

}  // namespace

// A frame that goes out with the game's own views 0-1 (no four views: menus, loading, or multi-view without its focus
// views) under native quad views with the FOV crop: padded out to the runtime's eyes as a race frame's periphery is.
bool v4d3d11_pad_eyes(XrSession session, std::vector<XrCompositionLayerProjectionView>& copy, XrTime displayTime) {
    if (!nativeQuad.active)
        return false;
    std::lock_guard guard(v4d3d11.mutex);
    ID3D11Device* device;
    {
        std::lock_guard quadGuard(quad.mutex);  // after v4d3d11.mutex, as v4d3d11_compose takes them
        device = sessionGraphics.d3d11;
    }
    if (!v4d3d11_pad_periphery(session, displayTime, copy, device))
        return false;
    for (int e = 0; e < 2; ++e)
        copy[e].next = nullptr;  // the game's depth info would describe its own image, not the pad
    ++v4d3d11.paddedEyesOnly;
    return true;
}

// Composes both eyes of `proj` (the game's two eye views, eye columns of `layout`) into the output chain and
// points `copy` (its views) at it. False: nothing changed, the caller submits views 0-1 as they are.
// With focus_layer (`focus` given, separate views, the helmet off): nothing composed; `copy` the periphery and
// `focus` the focus views' layer (v4d3d11_layered, over the runtime's focus views for `displayTime` under native quad
// views), `*layered` true.
bool v4d3d11_compose(XrSession session, const XrCompositionLayerProjection* proj,
                     std::vector<XrCompositionLayerProjectionView>& copy,
                     XrCompositionLayerProjectionView (*focus)[2], bool* layered,
                     XrTime displayTime) {
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
        {
            FramePartTimer part(FramePart::atlas);
            views = game->qvfr_views_atlas(eyeImages, atlasUv);
        }
        if (!views) {
            if (stock_active() && ++stockIdleFrames >= kStockIdleFrames && (v4d3d11.outChain || v4d3d11.retiredChain))
                v4d3d11_release_for_stock();
            return fallback("no four views this frame");
        }
    }
    stockIdleFrames = 0;
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
    // The output's density only for a composed frame: with focus_layer the runtime samples the focus views itself, and
    // the eyes' sizes stay the native eye's (the display's pixels, for the sharpening's footprint below).
    OutputDensity layeredDensity;  // scale 1, the native eye
    for (int e = 0; e < 2; ++e) {
        views4_eye(frame, e, source, eyes[e]);
        views4_helmet_eye(helmet, helmetFrame, e, eyes[e], submitFov[e], extent[e],
                          useLayered ? layeredDensity : v4d3d11.outDensity,
                          useLayered ? 0.0 : static_cast<double>(eyes[e].focusSource[2]) * texW);
    }
    if (!useLayered)
        views4_output_density(v4d3d11.outDensity, "views4 d3d11");
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
        bool focusTimedOut = false;
        v4d3d11.gradeState = 0;  // v4d3d11_layered says otherwise; composed frames are always graded
        if (v4d3d11_layered(session, proj, frame, eyes, views, texW, texH, texelsPerPixel,
                            displayTime, copy, *focus, &focusTimedOut)) {
            *layered = true;
            return true;
        }
        if (focusTimedOut)  // a slow image, not a broken layer: focus_layer stays on, this frame goes out as it is
            return fallback("the focus layer's image wasn't ready in time");
        // Composed from now on: the output chain can't be made here (under quad.mutex), so this frame only if a menu
        // made it already; otherwise the next frame makes it.
        if (!v4d3d11.focusFailed)
            log_line("views4 d3d11: focus_layer: the focus layer failed; composed as without focus_layer from now on");
        v4d3d11.focusFailed = true;
        if (!v4d3d11.outChain)
            return fallback("focus_layer failed, the output chain is made at the next frame");
    }
    uint32_t outIndex = 0;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    // Bounded (in the game's xrEndFrame, under v4d3d11.mutex and quad.mutex). Timed out: views 0-1 this frame, the
    // image (still acquired, not released) waited on again at the next.
    XrResult waited;
    {
        FramePartTimer part(FramePart::chain);
        waited = acquire_wait(v4d3d11.outChain, v4d3d11.outPending, kImageWaitNs, kImageWaitTries, outIndex,
                              "views4 d3d11: output image wait");
    }
    if (waited == XR_TIMEOUT_EXPIRED)
        return fallback("output wait timed out");
    if (waited != XR_SUCCESS)
        return fallback("output acquire failed");
    if (outIndex >= v4d3d11.outImages.size()) {
        next.ReleaseSwapchainImage(v4d3d11.outChain, &release);
        return fallback("output acquire failed");
    }
    bool ok;
    {
        FramePartTimer part(FramePart::compose);
        ok = v4d3d11.compositor.compose(views, v4d3d11.outImages[outIndex], eyes);
    }
    if (ok && experiment.d3d11CaptureFrame) {
        d3d11_capture_acquired(v4d3d11.outChain, outIndex);
        d3d11_capture_release(v4d3d11.outChain, endFrames);
    }
    {
        FramePartTimer part(FramePart::chain);
        next.ReleaseSwapchainImage(v4d3d11.outChain, &release);
    }
    if (!ok)
        return fallback("compose failed");
    {
        FramePartTimer part(FramePart::helmet);
        views4_helmet_layer(helmet, helmetFrame, eyes, views);
    }
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

}  // namespace simqvfr::layer

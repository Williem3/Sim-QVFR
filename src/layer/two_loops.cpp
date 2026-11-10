// Two-loop Foveated Interlaced: pairing frames A and B, captures, and submitting B; and the visibility
// mask hook.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "log.h"
#include "quad/compositor.h"
#include "quad/focus_shape.h"
#include "quad/focus_turn.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

double ms_between(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(loops.frequency.QuadPart);
}

// Narrows [lo, hi] (angles) to `fraction` of its tangent-space width, centred on the tangent
// `centre` (0 = straight ahead) and shifted inwards where the view ends.
void narrow(float& loAngle, float& hiAngle, double fraction, double centre) {
    const double lo = std::tan(loAngle), hi = std::tan(hiAngle);
    const double half = (hi - lo) * fraction / 2;
    centre = std::clamp(centre, lo + half, hi - half);
    loAngle = static_cast<float>(std::atan(centre - half));
    hiAngle = static_cast<float>(std::atan(centre + half));
}

// Frame A's xrEndFrame is swallowed: keep the views ACE rendered it with.
void quad_record_a(const XrFrameEndInfo* info) {
    std::lock_guard guard(quad.mutex);
    quad.aViewsValid = false;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        if (proj->viewCount != 2 || proj->views[0].subImage.swapchain != quad.appChain)
            continue;
        for (int e = 0; e < 2; ++e)
            quad.aViews[e] = {proj->views[e].pose, proj->views[e].fov, proj->views[e].subImage.imageRect};
        quad.aViewsValid = true;
        return;
    }
}

namespace {

constexpr int kMaxCaptures = 8;

}  // namespace

bool quad_capture_due() {
    if (!experiment.captureEverySeconds || quad.captures >= kMaxCaptures)
        return false;
    const ULONGLONG now = GetTickCount64();
    if (quad.lastCapture == 0) {  // first capture one interval after the pipeline starts
        quad.lastCapture = now;
        return false;
    }
    return now - quad.lastCapture >= experiment.captureEverySeconds * 1000ull;
}

namespace {

// Called with quad.mutex held, after compose and before the output image is released.
void quad_capture(ID3D12Resource* focus, ID3D12Resource* output, const EyeComposition& eye) {
    quad.lastCapture = GetTickCount64();
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"cap%02d-%02u%02u%02u", ++quad.captures, t.wHour, t.wMinute, t.wSecond);
    const bool ok = quad.compositor.save_captures(focus, output, (dir / name).wstring());
    float cx, cy;
    focus_centre(eye, cx, cy);
    logf("quad: capture %ls %s; eye 0 frame B faces A tangent %.3f,%.3f, gaze %.3f,%.3f,%.3f", name,
         ok ? "saved" : "FAILED", cx, cy, gaze.direction.x, gaze.direction.y, gaze.direction.z);
}

}  // namespace

// Called with quad.mutex held, after compose and before the output image is released.
void views4_capture(ID3D12Resource* views, ID3D12Resource* output) {
    quad.lastCapture = GetTickCount64();
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"v4cap%02d-%02u%02u%02u", ++quad.captures, t.wHour, t.wMinute, t.wSecond);
    const bool ok = quad.compositor.save_views4_captures(views, output, (dir / name).wstring());
    logf("views4: capture %ls %s (all four views, and the composed left eye)", name, ok ? "saved" : "FAILED");
}

// Frame B's xrEndFrame: composes A (outer) and B (focus) into the output swapchain and submits it in
// place of ACE's projection layer. Any missing piece shows ACE's frame B as it is (the focus box
// with black around it) rather than dropping the frame.
XrResult quad_submit_b(XrSession session, const XrFrameEndInfo* info) {
    std::unique_lock lock(quad.mutex);
    auto releaseHeld = [&] {
        if (quad.releaseHeld) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            next.ReleaseSwapchainImage(quad.appChain, &release);
            quad.releaseHeld = false;
        }
    };
    auto fallback = [&](const char* why) {
        quad_fail(why);
        releaseHeld();
        lock.unlock();
        return next.EndFrame(session, info);
    };
    int projIndex = -1;
    for (uint32_t i = 0; i < info->layerCount && projIndex < 0; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            continue;
        auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        if (proj->viewCount == 2 && proj->views[0].subImage.swapchain == quad.appChain)
            projIndex = static_cast<int>(i);
    }
    if (projIndex < 0) {  // ACE submitted no eye view this frame (shouldRender false, loading): nothing to compose
        releaseHeld();
        lock.unlock();
        return next.EndFrame(session, info);
    }
    if (!quad.outChain)
        return fallback("no output swapchain");
    if (!quad.releaseHeld || quad.appAcquired >= quad.appImages.size())
        return fallback("frame B's image was not held");
    if (!quad.outerCaptured || !quad.aViewsValid)
        return fallback("frame A missing");

    auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[projIndex]);
    EyeComposition eyes[2] = {};
    const float appW = static_cast<float>(quad.appW), appH = static_cast<float>(quad.appH);
    for (int e = 0; e < 2; ++e) {
        const EyeView& a = quad.aViews[e];
        const XrCompositionLayerProjectionView& b = proj->views[e];
        const float outerTan[4] = {std::tan(a.fov.angleLeft), std::tan(a.fov.angleRight), std::tan(a.fov.angleUp),
                                   std::tan(a.fov.angleDown)};
        const float focusTan[4] = {std::tan(b.fov.angleLeft), std::tan(b.fov.angleRight), std::tan(b.fov.angleUp),
                                   std::tan(b.fov.angleDown)};
        std::memcpy(eyes[e].outerTan, outerTan, sizeof(outerTan));
        std::memcpy(eyes[e].focusTan, focusTan, sizeof(focusTan));
        // A direction in A's eye frame, to world, to B's eye frame. Also absorbs any head turn
        // between the two frames.
        to_matrix(qmul(qconj(b.pose.orientation), a.pose.orientation), eyes[e].rotation);
        const XrRect2Di& ar = a.rect;
        const XrRect2Di& br = b.subImage.imageRect;
        const float outer[4] = {ar.offset.x / appW, ar.offset.y / appH, ar.extent.width / appW, ar.extent.height / appH};
        const float focus[4] = {br.offset.x / appW, br.offset.y / appH, br.extent.width / appW, br.extent.height / appH};
        std::memcpy(eyes[e].outerSource, outer, sizeof(outer));
        std::memcpy(eyes[e].focusSource, focus, sizeof(focus));
        eyes[e].feather = static_cast<float>(experiment.feather);
        eyes[e].border = experiment.border ? 0.008f : 0.0f;
        std::memcpy(eyes[e].borderColor, experiment.borderColor, sizeof(eyes[e].borderColor));
        eyes[e].cornerRadius = static_cast<float>(experiment.cornerRadius);
        focus_shape_insets(experiment.focusShape.load(std::memory_order_relaxed), eyes[e].cornerInset);
    }

    uint32_t outIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(next.AcquireSwapchainImage(quad.outChain, &acquire, &outIndex)) || outIndex >= quad.outImages.size())
        return fallback("output acquire failed");
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(quad.outChain, &wait)) &&
                    quad.compositor.compose(quad.appImages[quad.appAcquired], quad.outImages[outIndex], eyes);
    if (ok && quad_capture_due())
        quad_capture(quad.appImages[quad.appAcquired], quad.outImages[outIndex], eyes[0]);
    next.ReleaseSwapchainImage(quad.outChain, &release);
    if (!ok)
        return fallback("compose failed");
    releaseHeld();

    XrCompositionLayerProjectionView views[2];
    for (int e = 0; e < 2; ++e) {
        views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[e].pose = quad.aViews[e].pose;
        views[e].fov = quad.aViews[e].fov;
        views[e].subImage.swapchain = quad.outChain;
        views[e].subImage.imageRect = {{static_cast<int32_t>(e * quad.nativeW), 0},
                                       {static_cast<int32_t>(quad.nativeW), static_cast<int32_t>(quad.nativeH)}};
    }
    XrCompositionLayerProjection layer = *proj;
    layer.next = nullptr;
    layer.viewCount = 2;
    layer.views = views;
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    layers[projIndex] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    XrFrameEndInfo end = *info;
    end.layers = layers.data();
    quad.fallbackRun = 0;
    if (quad.composed++ == 0) {
        float cx, cy;
        focus_centre(eyes[0], cx, cy);
        logf("quad: first composed frame: eye 0 frame B faces A tangent %.3f,%.3f with tangents %.3f/%.3f/%.3f/%.3f, "
             "outer src %.3f,%.3f %.3fx%.3f, focus src %.3f,%.3f %.3fx%.3f",
             cx, cy, eyes[0].focusTan[0], eyes[0].focusTan[1], eyes[0].focusTan[2], eyes[0].focusTan[3],
             eyes[0].outerSource[0], eyes[0].outerSource[1], eyes[0].outerSource[2], eyes[0].outerSource[3],
             eyes[0].focusSource[0], eyes[0].focusSource[1], eyes[0].focusSource[2], eyes[0].focusSource[3]);
    }
    lock.unlock();
    return next.EndFrame(session, &end);
}

// views4 composing: the game draws each eye's hidden-area mesh into every view, so the focus views got the
// lens mask's black corners in their own frame (ACE: composition run 2; ACC through OpenComposite: a dark rim
// round the focus area, 2026-11-04). views4_mask=0 answers the hidden
// mesh with one zero-area triangle: nothing is masked in any view (the lenses hide those pixels anyway).
// views4_test_visible_mask=1 answers the visible mesh with the whole view, as runtimes that offer one (VDXR) do, to
// reproduce their path on one that doesn't (SteamVR with the Pimax).
// views4_test_hidden_mask=1 answers the hidden mesh with one of ours, so a game's lens mask can be seen where the runtime
// gives none (SteamVR on the user's headset): each corner of the view's located FOV cut off by a triangle a quarter of
// each side long, and a small triangle straight ahead (inside the focus box when the eyes look at it).
namespace {
std::atomic<bool> runtimeMaskLogged[3];  // per XrVisibilityMaskTypeKHR (1-3), the runtime's answer logged once
XrResult test_visible_mask(XrVisibilityMaskKHR* mask) {
    mask->vertexCountOutput = 4;
    mask->indexCountOutput = 6;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < 4 || mask->indexCapacityInput < 6 || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    // Tangents well past any FOV, counter-clockwise: the whole view is visible.
    const XrVector2f corners[4] = {{-4.0f, -4.0f}, {4.0f, -4.0f}, {4.0f, 4.0f}, {-4.0f, 4.0f}};
    const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    std::memcpy(mask->vertices, corners, sizeof(corners));
    std::memcpy(mask->indices, indices, sizeof(indices));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        log_line("views4: visible-area mesh replaced by the whole view (views4_test_visible_mask=1)");
    return XR_SUCCESS;
}

XrResult test_hidden_mask(uint32_t viewIndex, XrVisibilityMaskKHR* mask) {
    constexpr uint32_t kCount = 15;  // five triangles, each its own three vertices
    mask->vertexCountOutput = kCount;
    mask->indexCountOutput = kCount;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < kCount || mask->indexCapacityInput < kCount || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    // The view's FOV as the game located it (cropped, if a crop is set); tangents.
    float l = -1, r = 1, d = -1, u = 1;
    {
        std::lock_guard guard(quad.mutex);
        if (viewIndex < 2) {
            const XrFovf& fov = quad.v4Fov[viewIndex];
            if (fov.angleRight > fov.angleLeft && fov.angleUp > fov.angleDown) {
                l = std::tan(fov.angleLeft);
                r = std::tan(fov.angleRight);
                d = std::tan(fov.angleDown);
                u = std::tan(fov.angleUp);
            }
        }
    }
    const float a = 0.25f * (r - l), b = 0.25f * (u - d), s = 0.04f;
    // Counter-clockwise (x right, y up).
    const XrVector2f v[kCount] = {{l, d},  {l + a, d}, {l, d + b},    // bottom left
                                  {r, d},  {r, d + b}, {r - a, d},    // bottom right
                                  {r, u},  {r - a, u}, {r, u - b},    // top right
                                  {l, u},  {l, u - b}, {l + a, u},    // top left
                                  {-s, -s}, {s, -s},   {0.0f, s}};    // straight ahead
    std::memcpy(mask->vertices, v, sizeof(v));
    for (uint32_t i = 0; i < kCount; ++i)
        mask->indices[i] = i;
    logf("views4: hidden-area mesh of view %u replaced by a test one (views4_test_hidden_mask=1): the corners of its FOV "
         "(tangents %.3f/%.3f/%.3f/%.3f) cut a quarter of each side, a triangle %.2f wide straight ahead",
         viewIndex, l, r, d, u, 2 * s);
    return XR_SUCCESS;
}
}  // namespace
XrResult XRAPI_CALL layerGetVisibilityMaskKHR(XrSession session, XrViewConfigurationType type, uint32_t viewIndex,
                                              XrVisibilityMaskTypeKHR maskType, XrVisibilityMaskKHR* mask) {
    if (experiment.views4 && maskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR && mask &&
        mask->vertexCapacityInput > 0)
        game->visibility_mask_taken(viewIndex);  // the mesh itself, not the size query
    if (experiment.views4TestHiddenMask && maskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR && mask)
        return test_hidden_mask(viewIndex, mask);
    if (experiment.views4TestVisibleMask && maskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR && mask)
        return test_visible_mask(mask);
    if (!experiment.views4NoMask || maskType != XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR || !mask) {
        const XrResult r = next.GetVisibilityMaskKHR(session, type, viewIndex, maskType, mask);
        // What the runtime offers, once per type (the size query already has the counts; a game builds its mesh from it).
        const int t = static_cast<int>(maskType) - 1;
        if (mask && t >= 0 && t < 3 && !runtimeMaskLogged[t].exchange(true))
            logf("visibility mask from the runtime: %s, view %u: %u vertices, %u indices, result=%d",
                 maskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR    ? "hidden triangles"
                 : maskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR ? "visible triangles"
                                                                                 : "line loop",
                 viewIndex, mask->vertexCountOutput, mask->indexCountOutput, r);
        return r;
    }
    mask->vertexCountOutput = 3;
    mask->indexCountOutput = 3;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < 3 || mask->indexCapacityInput < 3 || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < 3; ++i) {
        mask->vertices[i] = {0.0f, 0.0f};
        mask->indices[i] = i;
    }
    if (!visibilityMaskLogged) {
        visibilityMaskLogged = true;
        log_line("views4: hidden-area mesh replaced by an empty one (views4_mask=0)");
    }
    return XR_SUCCESS;
}

}  // namespace simqvfr::layer

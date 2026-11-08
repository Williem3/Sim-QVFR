// XR_APILAYER_SIM_qvfr: logs what the game asks for (views, swapchains, frame submission) and starts the
// log-only engine probes (probe/probe.cpp) at xrCreateSession. With mode=off nothing alters rendering; the
// experiments and the quad pipeline are selected in sim-qvfr.ini. This file has the loader side:
// GetInstanceProcAddr, the layer order and gaze extension checks, CreateApiLayerInstance and the exported
// negotiation; the rest is in src/layer/ (layer.h).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dxgi1_4.h>
#include <intrin.h>

#include "cheeky/cheeky_host.h"
#include "cheeky/cheeky_settings.h"
#include "games/adapter.h"
#include "games/game.h"
#include "ini_put.h"
#include "input/hotkeys.h"
#include "input/pad_input.h"
#include "layer_chain.h"
#include "log.h"
#include "probe/cpu_load.h"
#include "probe/cpu_profile.h"
#include "probe/d3d11_capture.h"
#include "probe/d3d12_trace.h"
#include "probe/dred.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "probe/hang_watch.h"
#include "probe/pass_census.h"
#include "probe/pix_capture.h"
#include "probe/probe.h"
#include "profile/profiles.h"
#include "quad/compositor.h"
#include "quad/focus_offset.h"
#include "quad/focus_shape.h"
#include "quad/focus_turn.h"
#include "quad/fov_crop.h"
#include "quad/helmet.h"
#include "quad/menu_panel.h"
#include "quad/osd.h"
#include "quad/qvfr_optimizer.h"
#include "quad/session_graphics.h"
#include "quad/settings_menu.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr uint64_t kMaxFallbackRun = 90;  // ~1-2 s of display frames

double ms_between(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(loops.frequency.QuadPart);
}

// Narrows [lo, hi] (angles) to `fraction` of its tangent-space width, centred on the tangent
// `centre` (0 = straight ahead) and shifted inwards where the view ends.
void narrow(float& loAngle, float& hiAngle, double fraction, double centre = 0.0) {
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

namespace {

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

}  // namespace

// views4 composing: the game draws each eye's hidden-area mesh into every view, so the focus views got the
// lens mask's black corners in their own frame (ACE: composition run 2; ACC through OpenComposite: a dark rim
// round the focus area, 2026-11-04). views4_mask=0 answers the hidden
// mesh with one zero-area triangle: nothing is masked in any view (the lenses hide those pixels anyway).
XrResult XRAPI_CALL layerGetVisibilityMaskKHR(XrSession session, XrViewConfigurationType type, uint32_t viewIndex,
                                              XrVisibilityMaskTypeKHR maskType, XrVisibilityMaskKHR* mask) {
    if (!experiment.views4NoMask || maskType != XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR || !mask)
        return next.GetVisibilityMaskKHR(session, type, viewIndex, maskType, mask);
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

namespace {

XrResult locate_views(XrSession session, const XrViewLocateInfo* info, XrViewState* state, uint32_t capacity,
                      uint32_t* countOutput, XrView* views) {
    XrResult r = next.LocateViews(session, info, state, capacity, countOutput, views);
    // The FOV crop: ACE renders, and submits, the cropped FOV (everything below follows it).
    const FovCrop crop = experiment.crop();
    const bool stereo = XR_SUCCEEDED(r) && views && countOutput && std::min(*countOutput, capacity) >= 2 &&
                        info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (stereo)
        for (int i = 0; i < 2; ++i)
            views[i].fov = crop_fov(views[i].fov, crop, i);
    if (experiment.views4 && XR_SUCCEEDED(r) && views && countOutput) {
        game->qvfr_note_views(views, std::min(*countOutput, capacity));
        if (views4_outline_on() && *countOutput >= 2 && capacity >= 2) {
            std::lock_guard guard(v4outline.mutex);
            v4outline.fov[0] = views[0].fov;
            v4outline.fov[1] = views[1].fov;
            v4outline.fovValid = true;
        }
        if (experiment.composing() && *countOutput >= 2 && capacity >= 2) {
            {
                std::lock_guard guard(quad.mutex);
                quad.v4Fov[0] = views[0].fov;
                quad.v4Fov[1] = views[1].fov;
                if (stereo) {
                    quad.v4Crop = crop;
                    // Each eye against the head (the eyes' middle orientation): canted displays turn the eyes.
                    XrQuaternionf a = views[0].pose.orientation, b = views[1].pose.orientation;
                    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0)
                        b = {-b.x, -b.y, -b.z, -b.w};
                    XrQuaternionf head{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
                    const float n = std::sqrt(head.x * head.x + head.y * head.y + head.z * head.z + head.w * head.w);
                    if (n > 1e-6f) {
                        head = {head.x / n, head.y / n, head.z / n, head.w / n};
                        quad.v4EyeTurn[0] = qmul(qconj(head), views[0].pose.orientation);
                        quad.v4EyeTurn[1] = qmul(qconj(head), views[1].pose.orientation);
                        // Each eye from the eyes' middle (the helmet's depth): half the IPD, in the head's frame.
                        const XrVector3f& p0 = views[0].pose.position;
                        const XrVector3f& p1 = views[1].pose.position;
                        const XrVector3f half{(p0.x - p1.x) / 2, (p0.y - p1.y) / 2, (p0.z - p1.z) / 2};
                        quad.v4EyeOffset[0] = rotate(qconj(head), half);
                        quad.v4EyeOffset[1] = {-quad.v4EyeOffset[0].x, -quad.v4EyeOffset[0].y, -quad.v4EyeOffset[0].z};
                    }
                }
                quad.v4FovValid = true;
            }
            // Gaze: where the gaze ray points in each eye's frame (canted displays rotate each eye), as
            // tangents; the focus cameras turn towards it, clamped against the FOV ACE renders with
            // (fill_focus_cameras). Untracked gaze recentres after kGazeHoldNs. The vertical offset
            // (focus_offset.h) moves the tracked gaze, or the fixed box off each eye's forward axis.
            // A head-locked call (OpenComposite's eyes-to-head query) leaves the gaze to the scene-space one
            // (gaze_skips_space).
            if (!(experiment.gaze && gaze_skips_space(info->space, info->displayTime))) {
                float tan[2][2] = {};
                bool valid[2] = {};
                XrVector3f direction{0, 0, -1};
                const bool gazing = experiment.gaze && gaze_now(session, info->space, info->displayTime, direction);
                steady_gaze(gazing, direction);
                const int offset = gazing ? experiment.gazeOffsetY : experiment.fixedOffsetY;
                for (int e = 0; e < 2; ++e) {
                    if (gazing) {
                        const XrVector3f d = rotate(qconj(views[e].pose.orientation), direction);
                        valid[e] = d.z < -0.1f;
                        if (valid[e]) {
                            tan[e][0] = d.x / -d.z;
                            tan[e][1] = d.y / -d.z;
                        }
                    } else {
                        valid[e] = offset != 0;
                    }
                    if (valid[e] && offset != 0)
                        tan[e][1] = static_cast<float>(offset_tan_y(tan[e][1], offset));
                }
                game->qvfr_set_gaze(tan, valid, gazing);
            }
        }
    }
    const bool narrowThisFrame = experiment.focusOnly || (experiment.twoLoops && experiment.focusInB && loops.inB);
    if (narrowThisFrame && capacity > 0 && XR_SUCCEEDED(r)) {
        XrVector3f direction{0, 0, -1};
        const bool gazing = experiment.quad && experiment.gaze &&
                            gaze_direction(session, info->space, info->displayTime, direction);
        if (experiment.quad)
            steady_gaze(gazing, direction);
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i) {
            const XrFovf before = views[i].fov;
            double tanX = 0, tanY = 0;
            if (gazing) {  // gaze ray direction in this eye's frame (canted displays rotate each eye)
                const XrQuaternionf& q = views[i].pose.orientation;
                const XrVector3f d = rotate({-q.x, -q.y, -q.z, q.w}, direction);
                if (d.z < -0.1f) {
                    tanX = d.x / -d.z;
                    tanY = d.y / -d.z;
                }
            }
            if (experiment.quad)  // the vertical offset (focus_offset.h), on the gaze or the fixed box
                tanY = offset_tan_y(tanY, gazing ? experiment.gazeOffsetY : experiment.fixedOffsetY);
            if (experiment.quad) {
                turn_view(views[i], experiment.focusFraction, experiment.focus_y(), tanX, tanY);
            } else {
                narrow(views[i].fov.angleLeft, views[i].fov.angleRight, experiment.focusFraction, tanX);
                narrow(views[i].fov.angleDown, views[i].fov.angleUp, experiment.focus_y(), tanY);
            }
            if (!focusLogged)
                logf("experiment focus_only view=%u fov %.4f/%.4f/%.4f/%.4f -> %.4f/%.4f/%.4f/%.4f", i,
                     before.angleLeft, before.angleRight, before.angleUp, before.angleDown, views[i].fov.angleLeft,
                     views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown);
        }
        focusLogged = true;
    }
    if (capacity > 0 && sampled(++locates) && XR_SUCCEEDED(r))
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
            logf("xrLocateViews call=%llu config=%d time=%lld view=%u fov=%.4f/%.4f/%.4f/%.4f pos=%.4f,%.4f,%.4f",
                 static_cast<unsigned long long>(locates), info->viewConfigurationType,
                 static_cast<long long>(info->displayTime), i, views[i].fov.angleLeft, views[i].fov.angleRight,
                 views[i].fov.angleUp, views[i].fov.angleDown, views[i].pose.position.x, views[i].pose.position.y,
                 views[i].pose.position.z);
    return r;
}

}  // namespace

// views4: ACE's two "eyes" are each an outer plus a focus view wide and hold views 0+1 and 2+3 side
// by side (views4_layout.h). Submit views 0 and 1, the real eyes, as the projection views.
// Logs ACE's own submitted FOV next to the one the output is submitted with, once and on a change.
void log_submitted_fov(uint32_t eye, const XrFovf& submitted, const XrFovf& composed) {
    XrFovf& last = quad.v4LoggedGameFov[eye];
    const auto same = [](const XrFovf& a, const XrFovf& b, float eps) {
        return std::fabs(a.angleLeft - b.angleLeft) < eps && std::fabs(a.angleRight - b.angleRight) < eps &&
               std::fabs(a.angleUp - b.angleUp) < eps && std::fabs(a.angleDown - b.angleDown) < eps;
    };
    if (same(submitted, last, 0.002f))
        return;
    last = submitted;
    logf("views4: eye %u ACE submits FOV %.4f/%.4f/%.4f/%.4f, output submitted with %.4f/%.4f/%.4f/%.4f (its "
         "rendered projection)%s",
         eye, submitted.angleLeft, submitted.angleRight, submitted.angleUp, submitted.angleDown, composed.angleLeft, composed.angleRight,
         composed.angleUp, composed.angleDown, same(submitted, composed, 0.002f) ? "" : "  <-- DIFFERENT");
}

XrResult XRAPI_CALL layerLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                     uint32_t capacity, uint32_t* countOutput, XrView* views) {
    if (!cpu.realLocateViews)
        return locate_views(session, info, state, capacity, countOutput, views);
    const int64_t t0 = qpc_now();
    const XrResult r = locate_views(session, info, state, capacity, countOutput, views);
    cpu.locate.add(qpc_now() - t0);
    return r;
}

namespace {

XrResult end_frame(XrSession session, const XrFrameEndInfo* info);

// Live resolution (views4): periphery_scale and focus_density resize the views inside ACE's target,
// up to their launch size (views4_live). A larger focus box at the same density needs more pixels;
// beyond the launch size the density gives.
void live_resolution(const LiveSettings& l) {
    const Views4Layout& launch = experiment.views4Layout;
    const Views4Sizes wanted = views4_sizes(live.ini.c_str(), l.focusFraction, l.focusFractionY);
    const double cw = l.crop.kept_width(), ch = l.crop.kept_height();  // the outer views take the live crop
    // One view size (X-Plane): periphery and focus each sized on its own inside the launch layers. Eye columns (ACC):
    // one width for all four views, the outer and focus rows sharing the height.
    const bool columns = game->qvfr_eye_columns(), oneSize = columns && game->qvfr_one_view_size();
    game->qvfr_set_live_layout(oneSize   ? views4_live_one_size(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
                               : columns ? views4_live_eye_columns(launch, wanted.outer, wanted.focusHeight, cw, ch)
                                         : views4_live(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch));
    const Views4Fit fit = oneSize   ? views4_fit_one_size(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch)
                          : columns ? views4_fit_eye_columns(launch, wanted.outer, wanted.focusHeight, cw, ch)
                                    : views4_fit(launch, wanted.outer, wanted.focus, wanted.focusHeight, cw, ch);
    logf("live: periphery %.0f%% of native, focus density %.0f%% x %.0f%% (focus views %.0f%% x %.0f%% of the native "
         "eye; launch %.0f%%, %.0f%% x %.0f%%)%s%s",
         fit.outer * 100, fit.focus / l.focusFraction * 100, fit.focusHeight / experiment.focus_y() * 100,
         fit.focus * 100, fit.focusHeight * 100, launch.outer * 100, launch.focus * 100, launch.focusHeight * 100,
         fit.focusCapped ? " (focus capped: larger than the launch target allows)" : "",
         fit.outerCapped ? (fit.outerByRoom ? " (periphery capped: the focus views use the width)"
                                            : " (periphery capped: larger than the launch target allows)")
                         : "");
}

// Live settings: applies the ini's live keys (LiveSettings). Called between display frames.
void live_apply(const LiveSettings& l) {
    if (experiment.composing()) {  // the helmet: either compositor
        std::lock_guard guard(quad.mutex);
        if (!(helmetSettings == l.helmet))
            logf("live: helmet %d, rim %d%%, corners %d%%, light %d%%, size %d deg, height %d deg, depth %d cm, visor %ls%s",
                 l.helmet.on ? 1 : 0, l.helmet.rim, l.helmet.corners, l.helmet.light, l.helmet.size, l.helmet.height,
                 l.helmet.depth,
                 kVisorPositionNames[static_cast<int>(l.helmet.visor)], l.helmet.debugLight ? ", showing its light" : "");
        helmetSettings = l.helmet;
    }
    if (experiment.views4) {
        const FovCrop before = experiment.crop();
        experiment.set_crop(l.crop);
        if (before.top != l.crop.top || before.bottom != l.crop.bottom || before.outer != l.crop.outer ||
            before.inner != l.crop.inner)
            logf("live: FOV crop top %.0f%%, bottom %.0f%%, outer %.0f%%, inner %.0f%%%s", l.crop.top * 100,
                 l.crop.bottom * 100, l.crop.outer * 100, l.crop.inner * 100,
                 game->qvfr_dlss_held() ? " (DLSS is on: the views keep their launch sizes until the next launch)" : "");
    }
    experiment.focusFraction = l.focusFraction;
    experiment.focusFractionY = l.focusFractionY;
    experiment.feather = l.feather;
    experiment.cornerRadius = l.cornerRadius;
    experiment.focusShape = focus_shape_bits(l.shape);
    std::memcpy(experiment.borderColor, l.borderColor, sizeof(experiment.borderColor));
    experiment.border = l.border;
    experiment.gaze = l.gaze;
    experiment.fixedOffsetY = l.offset.fixed;
    experiment.gazeOffsetY = l.offset.gaze;
    experiment.gazeDeadzone = l.gazeDeadzone;
    bool gazeAvailable;
    {
        std::lock_guard guard(gaze.mutex);
        gazeAvailable = gaze.set != XR_NULL_HANDLE;
    }
    if (experiment.views4 && game_features().objectDetail)
        game->qvfr_set_object_detail(l.objectDetail);  // the adapter logs a change
    if (experiment.views4 && !experiment.views4EyeCamera)
        game->qvfr_set_live(experiment.box_x(l.focusFraction),
                        experiment.box_y(l.focusFractionY > 0 ? l.focusFractionY : l.focusFraction),
                        1.0 + l.feather + experiment.views4FocusCullMargin,
                        l.gaze && experiment.composing());
    logf("live: focus box %.0f%% x %.0f%%, feather %.0f%%, corners %.0f%%, outline %d, gaze %d, offset fixed %d deg, "
         "gaze %d deg, gaze dead-zone %.1f deg",
         l.focusFraction * 100, experiment.focus_y() * 100, l.feather * 100, l.cornerRadius * 100, l.border ? 1 : 0,
         l.gaze ? 1 : 0, l.offset.fixed, l.offset.gaze, l.gazeDeadzone);
    logf("live: focus shape %ls", focus_shape_text(l.shape).c_str());
    if (experiment.views4 && game_features().liveResolution)
        live_resolution(l);
    else if (experiment.views4)
        logf("live: %ls resizes its views at the next launch (no live resolution yet)", game->info()->shortName);
    if (l.gaze && !gazeAvailable && (experiment.quad || experiment.composing()))
        log_line("live: gaze turned on, but eye tracking was off at launch; it starts at the next launch");
    if (l.border && !experiment.borderAtLaunch && experiment.views4 && !experiment.composing())
        log_line("live: debug_border without views4_compose starts at the next launch");
}

// Cheeky's runtime gets [cheeky]'s live settings and, in QVFR, the quad view roles (cheeky_settings.h).
void cheeky_apply_settings(const CheekySettings& settings, bool gazeOn) {
    if (!cheeky_running())
        return;
    CheekySettings c = settings;
    c.fg = c.fg && game_features().frameGeneration;  // a game without it: off, whatever the ini says
    cheeky_apply(cheeky_set_lines(c, experiment.views4, gazeOn, experiment.views4ComposeD3D11));
}

void cheeky_apply_ini() {
    if (!live.ini.empty())
        cheeky_apply_settings(read_cheeky_settings(live.ini.c_str()), read_live(live.ini.c_str()).gaze);
}

void menu_placement_changed(const MenuPlacement& placement);  // the launcher moved the settings menu's panel (below)

void osd_placement_changed(const OsdPlacement& placement, const CheekySettings& cheeky);  // the overlay (below)

void osd_placement_changed();

constexpr int64_t kLiveCheckMs = 250;

IniSnapshot read_snapshot(const wchar_t* ini) {
    IniSnapshot s;
    s.live = read_live(ini);
    s.cheeky = read_cheeky_settings(ini);
    s.menu = read_menu_values(ini);
    s.menuPlacement = read_menu_placement(ini);
    s.osd = read_osd_placement(ini);
    s.hotkeys = read_hotkeys(ini);
    s.restartKeys = restart_keys(ini, experiment.views4);
    return s;
}

void ini_worker_loop() {
    std::unique_lock lock(iniWorker.mutex);
    for (;;) {
        iniWorker.wake.wait_for(lock, std::chrono::milliseconds(kLiveCheckMs),
                                [] { return iniWorker.poke || !iniWorker.writes.empty(); });
        iniWorker.poke = false;
        std::deque<QueuedIniWrite> writes;
        writes.swap(iniWorker.writes);
        const std::wstring ini = iniWorker.ini;
        const FILETIME seen = iniWorker.written;
        const uint64_t pass = iniWorker.requested;
        // Numbered in order; a menu step that stood in for an older one skips that one's number.
        const uint64_t through = writes.empty() ? iniWorker.writesDone : writes.back().number;
        lock.unlock();
        for (auto& w : writes)
            w.write();
        // After its own writes it reads the ini whatever its write time says (a write can leave it unchanged).
        std::optional<IniSnapshot> snapshot;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const bool stamped = !ini.empty() && GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &attributes);
        const bool changed = stamped && (!writes.empty() || CompareFileTime(&attributes.ftLastWriteTime, &seen) != 0);
        if (changed) {
            snapshot = read_snapshot(ini.c_str());
            snapshot->writesBefore = through;
        }
        lock.lock();
        iniWorker.writesDone = through;
        if (snapshot && ini == iniWorker.ini) {
            iniWorker.ready = std::move(snapshot);
            iniWorker.written = attributes.ftLastWriteTime;
        }
        iniWorker.done = std::max(iniWorker.done, pass);
        iniWorker.settled.notify_all();
    }
}

// Starts the worker (once) and points it at `ini`, read from `written` on (a new path drops what it read).
// Under iniWorker.mutex.
void ini_worker_watch(const std::wstring& ini, const FILETIME& written) {
    if (!iniWorker.started) {
        iniWorker.started = true;
        std::thread(ini_worker_loop).detach();  // ends with the process (ExitProcess stops it before teardown)
    }
    if (ini != iniWorker.ini) {
        iniWorker.ini = ini;
        iniWorker.written = written;
        iniWorker.ready.reset();
    }
}

}  // namespace

// Queues a write to the ini (or the launcher's) and wakes the worker; the live reload follows it. Returns
// the write's number: snapshots read after it carry writesBefore >= it. A menu step queued right behind another
// one not yet written replaces it: it writes every value the older one did (only the last in the queue, so a
// write of another kind between them keeps its order).
uint64_t ini_write_later(std::function<void()> write, IniWrite kind) {
    std::lock_guard guard(iniWorker.mutex);
    ini_worker_watch(live.ini, live.written);
    const uint64_t number = ++iniWorker.writesQueued;
    if (kind != IniWrite::Once && !iniWorker.writes.empty() && iniWorker.writes.back().kind == kind)
        iniWorker.writes.back() = {number, kind, std::move(write)};
    else
        iniWorker.writes.push_back({number, kind, std::move(write)});
    iniWorker.wake.notify_one();
    return number;
}

// Waits until the writes queued so far are done and the ini has been read after them (tests; the
// session's end, so no change is lost).
void ini_worker_sync() {
    std::unique_lock lock(iniWorker.mutex);
    if (!iniWorker.started)
        return;
    const uint64_t want = ++iniWorker.requested;
    iniWorker.poke = true;
    iniWorker.wake.notify_one();
    iniWorker.settled.wait(lock, [want] { return iniWorker.done >= want; });
}

// Applies the ini's live keys when the worker read a change. Only between display frames: with
// two_loops, never between frame A and frame B. No file access here.
void live_reload() {
    if (live.ini.empty())
        return;
    {
        std::lock_guard guard(iniWorker.mutex);
        ini_worker_watch(live.ini, live.written);
    }
    {
        std::lock_guard guard(loops.mutex);
        if (loops.active && loops.pairedEnds % 2 != 0)
            return;  // A ended, B not yet: apply after B
    }
    std::optional<IniSnapshot> s;
    {
        std::lock_guard guard(iniWorker.mutex);
        s.swap(iniWorker.ready);
    }
    if (!s)
        return;
    live_apply(s->live);
    cheeky_apply_settings(s->cheeky, s->live.gaze);
    menu_values_changed(*s);
    menu_placement_changed(s->menuPlacement);
    osd_placement_changed(s->osd, s->cheeky);
    hotkeys_changed(s->hotkeys);
    if (s->restartKeys != live.restartKeys) {
        logf("live: %ls-> %ls(applies at the next launch)", live.restartKeys.c_str(), s->restartKeys.c_str());
        live.restartKeys = s->restartKeys;
    }
}

namespace {

constexpr int64_t kOptimizeWindowMs = 500;

// ---- Hotkeys ---------------------------------------------------------------------------------
// [hotkeys] (input/hotkeys.h), reloaded live: each action's keyboard combination, held while ACE has focus,
// and its wheel or controller button, read by a thread of its own through DirectInput (input/pad_input.h)
// whenever one is bound. The thread polls every kPadPollMs and publishes the actions held as bits.
constexpr int kPadPollMs = 8, kPadRescanMs = 3000;

bool pad_bound(const Hotkeys& h) {
    for (const Hotkey& k : h)
        if (k.pad.set())
            return true;
    return false;
}

void pad_watch_run() {
    // DirectInput wants a top-level window of this process; a hidden one of the thread's own.
    HWND window = CreateWindowExW(0, L"STATIC", L"Sim-QVFR input", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    PadInput pads;
    if (!window || !pads.open(window)) {
        log_line("hotkeys: DirectInput not available; wheel and controller buttons do nothing");
        if (window)
            DestroyWindow(window);
        return;
    }
    {
        std::wstring found;
        for (const auto& [id, name] : pads.devices())
            found += (found.empty() ? L"" : L", ") + name + L" (" + id + L")";
        logf("hotkeys: game controllers: %ls", found.empty() ? L"none" : found.c_str());
    }
    int64_t nextRescan = qpc_now() + qpc_ms(kPadRescanMs);
    std::unique_lock lock(padWatch.mutex);
    while (!padWatch.stop) {
        const Hotkeys bindings = padWatch.bindings;
        lock.unlock();
        const std::vector<PadHeld> held = pads.poll();
        const auto devices = pads.devices();
        uint32_t bits = 0;
        bool missing = false;
        for (int a = 0; a < kHotkeyActions; ++a) {
            const PadControl& want = bindings[a].pad;
            if (!want.set())
                continue;
            bool connected = false;
            for (const PadHeld& h : held)
                if (_wcsicmp(h.device.c_str(), want.device.c_str()) == 0 && h.control == want.control)
                    bits |= 1u << a;
            for (const auto& [id, name] : devices)
                connected |= _wcsicmp(id.c_str(), want.device.c_str()) == 0;
            missing |= !connected;
        }
        padWatch.held.store(bits, std::memory_order_relaxed);
        if (missing && qpc_now() >= nextRescan) {  // a bound device plugged in since
            pads.rescan();
            nextRescan = qpc_now() + qpc_ms(kPadRescanMs);
        }
        lock.lock();
        padWatch.wake.wait_for(lock, std::chrono::milliseconds(kPadPollMs), [] { return padWatch.stop; });
    }
    lock.unlock();
    pads.close();
    DestroyWindow(window);
}

// Starts the thread once a wheel or controller button is bound (from the frame thread, each frame).
void pad_watch_ensure() {
    if (!pad_bound(hotkeys))
        return;
    std::lock_guard guard(padWatch.mutex);
    if (!padWatch.thread.joinable()) {
        padWatch.stop = false;
        padWatch.bindings = hotkeys;
        padWatch.thread = std::thread(pad_watch_run);
    }
}

}  // namespace

void pad_watch_stop() {
    std::thread thread;
    {
        std::lock_guard guard(padWatch.mutex);
        padWatch.stop = true;
        thread.swap(padWatch.thread);
    }
    padWatch.wake.notify_all();
    if (thread.joinable())
        thread.join();
    padWatch.held.store(0);
}

void hotkeys_changed(const Hotkeys& h) {
    if (h == hotkeys)
        return;
    hotkeys = h;
    {
        std::lock_guard guard(padWatch.mutex);
        padWatch.bindings = h;
    }
    std::wstring line;
    for (int a = 0; a < kHotkeyActions; ++a) {
        const std::wstring key = key_text(h[a].key), pad = control_text(h[a].pad.control);
        line += std::wstring(a ? L", " : L"") + hotkey_ini_key(static_cast<HotkeyAction>(a)) + L"=" + (key.empty() ? L"-" : key) +
                (pad.empty() ? L"" : L" / " + pad);
    }
    logf("hotkeys: %ls", line.c_str());
}

namespace {

bool game_focused();

// Exactly `key`'s modifiers and its key held, while ACE has focus.
bool key_held(const KeyCombo& key) {
    if (!key.set() || !game_focused())
        return false;
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return down(key.vk) && down(VK_CONTROL) == key.ctrl && down(VK_MENU) == key.alt && down(VK_SHIFT) == key.shift;
}

bool pad_held(HotkeyAction action) {
    return (padWatch.held.load(std::memory_order_relaxed) >> static_cast<int>(action)) & 1;
}

bool hotkey_held(HotkeyAction action) { return key_held(hotkeys[static_cast<int>(action)].key) || pad_held(action); }

// The settings menu's hint: how to select and change, and how to close.
std::wstring menu_hint() {
    const Hotkeys defaults = default_hotkeys();
    bool defaultKeys = true, padKeys = false;
    for (int a = static_cast<int>(HotkeyAction::MenuUp); a <= static_cast<int>(HotkeyAction::MenuRight); ++a) {
        defaultKeys &= hotkeys[a].key == defaults[a].key;
        padKeys |= hotkeys[a].pad.set();
    }
    const Hotkey& toggle = hotkeys[static_cast<int>(HotkeyAction::MenuToggle)];
    std::wstring hint = defaultKeys ? L"Ctrl+Alt+arrows" : L"Your menu hotkeys";
    hint += padKeys ? L", wheel buttons or right stick: select and change" : L", or right stick: select and change";
    hint += toggle.key.set() || toggle.pad.set() ? L" · open/close hotkey or left stick click: close" : L" · left stick click: close";
    return hint;
}

// The menu's hotkeys footer ([menu] hotkey_footer): the in-game actions' keys and wheel buttons, those that apply
// (the visor with the helmet on, the CPU capture with cpu_profile, the frame capture with d3d11_capture_hotkey); empty
// when none is bound.
std::wstring hotkey_footer(bool helmetOn) {
    struct Entry {
        HotkeyAction action;
        const wchar_t* name;
        bool shown;
    };
    const Entry entries[] = {{HotkeyAction::Overlay, L"Overlay", true},
                             {HotkeyAction::Visor, L"Visor", helmetOn},
                             {HotkeyAction::CpuCapture, L"CPU capture", experiment.cpuProfile},
                             {HotkeyAction::FrameCapture, L"Frame capture", experiment.d3d11CaptureHotkey}};
    std::wstring out;
    for (const Entry& e : entries) {
        const Hotkey& h = hotkeys[static_cast<int>(e.action)];
        if (!e.shown || (!h.key.set() && !h.pad.set()))
            continue;
        std::wstring binding = key_text(h.key);
        if (h.pad.set())
            binding += (binding.empty() ? L"" : L" / ") + std::wstring(L"wheel ") + control_text(h.pad.control);
        out += (out.empty() ? L"" : L"  \u00b7  ") + std::wstring(e.name) + L" " + binding;
    }
    return out;
}

constexpr int64_t kMenuHideMs = 6000, kRepeatDelayMs = 400, kRepeatMs = 90;

constexpr MenuKey kMenuKeyOf[] = {MenuKey::Up, MenuKey::Down, MenuKey::Left, MenuKey::Right};

}  // namespace

int64_t qpc_ms(int64_t ms) {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency * ms / 1000;
}

namespace {

bool game_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

double native_center_ppd();  // below

void menu_place(const MenuPlacement& placement) {
    const MenuPose pose = menu_pose(placement, static_cast<float>(kPanelHeight) / kPanelWidth);
    menu.layer.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    menu.layer.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    menu.layer.size = {pose.width, pose.height};
}

}  // namespace

// The launcher changed the ini: an open menu shows the new values.
// A snapshot read before the latest step was written would put the menu back a step (and the next step's
// write would carry the old value: two quick steps lost the second), so the menu waits for a newer one.
void menu_values_changed(const IniSnapshot& s) {
    if (!menu.visible || s.writesBefore < menu.lastWrite)
        return;
    menu.model.values = s.menu;
    menu.model.cheeky.values = s.cheeky;
    menu.dirty = true;
}

namespace {

void menu_open(int64_t now);

// Moved from the launcher (its Headset menu pane): the panel goes there and shows, so you can see it.
void menu_placement_changed(const MenuPlacement& placement) {
    if (placement == live.menuPlacement)
        return;
    live.menuPlacement = placement;
    menu_place(placement);
    logf("settings menu: moved to %d deg right, %d deg up, %d deg wide", placement.yaw, placement.pitch, placement.width);
    if (experiment.views4 || experiment.quad)
        menu_open(qpc_now());
}

// Profiles (profile/profiles.h) in the launcher's folder (SIM_QVFR_SETTINGS), else next to the ini the layer reads.
std::wstring menu_profiles_ini() { return menu.settingsIni.empty() ? live.ini : menu.settingsIni; }

// Where this game's per-game keys go in the launcher's folder (profile/profiles.h game_settings_target): the
// launcher's ini while it shows this game, else this game's own copy, so a change made in the headset never lands in
// the settings of the game the launcher shows. Reads the launcher's ini: on the ini worker.
std::wstring game_settings_ini(const std::wstring& settings) {
    const games::GameInfo* g = games::current_game();
    if (settings.empty() || !g)
        return settings;
    return game_settings_target(settings, std::wstring(g->id, g->id + std::strlen(g->id)));
}

void menu_profiles_read() {
    const std::wstring ini = menu_profiles_ini();
    menu.model.profiles = profile_names(profiles_folder(ini));
    // The profile in use from the ini this game runs with: the launcher's may be showing another game's.
    const std::wstring current = current_profile((live.ini.empty() ? ini : live.ini).c_str());
    menu.model.profile = -1;
    for (size_t i = 0; i < menu.model.profiles.size(); ++i)
        if (_wcsicmp(menu.model.profiles[i].c_str(), current.c_str()) == 0)
            menu.model.profile = static_cast<int>(i);
    Profile p;
    menu.model.profileChanged = menu.model.profile >= 0 && load_profile(profiles_folder(ini), current, p, menu.model.values.eyeTracking) &&
                                !profile_matches(live.ini.c_str(), p);
}

// The profile the Profile line stepped to: its keys written to the ini the layer reloads and the launcher's, as
// the menu's own steps are; the menu shows its values once the live reload has them.
void menu_profile_use() {
    const int request = std::exchange(menu.model.profileRequest, -1);
    if (request < 0 || request >= static_cast<int>(menu.model.profiles.size()))
        return;
    const std::wstring ini = menu_profiles_ini();
    Profile p;
    if (!load_profile(profiles_folder(ini), menu.model.profiles[request], p, menu.model.values.eyeTracking)) {
        logf("settings menu: profile %ls not found", menu.model.profiles[request].c_str());
        return;
    }
    if (optimize.optimizer.running()) {
        optimize.optimizer.cancel();
        log_line("optimize: stopped (a profile was used)");
    }
    menu.lastWrite = ini_write_later([live = live.ini, settings = menu.settingsIni, p] {
        apply_profile(live.c_str(), p);
        set_current_profile(live.c_str(), p.name);
        if (!settings.empty()) {
            const std::wstring target = game_settings_ini(settings);
            apply_profile(target.c_str(), p);
            set_current_profile(target.c_str(), p.name);
        }
    });
    logf("settings menu: profile %ls (%zu keys)", p.name.c_str(), p.keys.size());
}

void menu_open(int64_t now) {
    if (menu.settingsIni.empty()) {  // the launcher's own ini, where the next launch reads them
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            menu.settingsIni = path;
    }
    if (!menu.visible) {
        bool stepPending;  // reopened before its last step reached the ini: keep the values it shows
        {
            std::lock_guard guard(iniWorker.mutex);
            stepPending = iniWorker.writesDone < menu.lastWrite;
        }
        if (!stepPending)
            menu.model.values = read_menu_values(live.ini.c_str());
        menu.model.cheeky = {cheeky_running(), cheeky_version(),
                             stepPending ? menu.model.cheeky.values : read_cheeky_settings(live.ini.c_str())};
        const Views4Layout& l = experiment.views4Layout;
        {
            std::lock_guard guard(quad.mutex);  // the native eye, for the notes' pixel sizes
            menu.model.launch = {experiment.views4, l.outer * 100, l.focus * 100, l.focusHeight * 100,
                                 static_cast<int>(quad.nativeW), static_cast<int>(quad.nativeH), 0,
                                 experiment.eyeScale * 100};
        }
        menu.model.launch.cropWidth = l.cropWidth * 100;  // the launch crop
        menu.model.launch.cropHeight = l.cropHeight * 100;
        const games::GameFeatures features = game_features();  // what the game has: the rest is left out
        menu.model.launch.cropPage = features.fovCrop;
        menu.model.launch.helmetPage = features.helmet;
        menu.model.launch.fgPage = features.frameGeneration;
        menu.model.launch.objectDetail = features.objectDetail;  // X-Plane only
        menu.model.launch.liveResolution = features.liveResolution;
        {
            std::lock_guard guard(quad.mutex);
            menu.model.launch.helmetPieces = quad.helmetPiecesLoaded;
        }
        menu.model.launch.nativePpd = native_center_ppd();  // takes quad.mutex itself
        if (!optimize.optimizer.running()) {  // a finished run's detail is not kept over a reopen
            optimize.shown = false;
            optimize.note.clear();
        }
        if (!stepPending)  // the profiles, and the one in use unless a step of the menu's is on its way
            menu_profiles_read();
        menu.visible = menu.dirty = true;
        log_line("settings menu: open");
    }
    menu.hideAt = now + qpc_ms(kMenuHideMs);
}

}  // namespace

void menu_press(MenuKey key, int64_t now) {
    if (!menu.visible) {  // the first press only opens it
        menu_open(now);
        return;
    }
    menu.hideAt = now + qpc_ms(kMenuHideMs);
    menu.dirty = true;
    if (!menu_key(menu.model, key)) {
        menu_profile_use();
        return;
    }
    menu.model.profileChanged = menu.model.profile >= 0;
    if (optimize.optimizer.running()) {  // a value changed by hand: the optimizer's measurements no longer hold
        optimize.optimizer.cancel();
        log_line("optimize: stopped (a setting was changed)");
    }
    {  // what was set, to check against what applies ("live:", "cheeky: settings applied")
        const std::vector<MenuLine> lines = menu_lines(menu.model);
        const int i = menu.model.selected;
        if (i >= 0 && i < static_cast<int>(lines.size()))
            logf("settings menu: %ls: %ls -> %ls", menu_title(menu.model).c_str(), lines[i].label.c_str(),
                 lines[i].value.c_str());
    }
    // Written on the ini worker (off the render thread), to the ini the layer reloads and the launcher's
    // (the next launch starts from them); the live reload follows within a frame or two. Steps quicker than the
    // worker are written as one (IniWrite::MenuStep).
    menu.lastWrite = ini_write_later([ini = live.ini, settings = menu.settingsIni, values = menu.model.values,
                     cheeky = menu.model.cheeky, qvfr = menu.model.launch.qvfr, detail = menu.model.launch.objectDetail] {
        write_menu_values(ini.c_str(), values, qvfr, detail);
        if (cheeky.available)
            write_cheeky_settings(ini.c_str(), cheeky.values, false);
        if (!settings.empty()) {
            const std::wstring target = game_settings_ini(settings);
            write_menu_values(target.c_str(), values, qvfr, detail);
            if (cheeky.available)
                write_cheeky_settings(target.c_str(), cheeky.values, false);
            if (target != settings) {  // the panels' places and the overlay are every game's: the launcher's ini
                write_osd_placement(settings.c_str(), values.overlay);
                ini_put(settings.c_str(), L"menu", L"yaw", values.placement.yaw);
                ini_put(settings.c_str(), L"menu", L"pitch", values.placement.pitch);
                ini_put(settings.c_str(), L"menu", L"width", values.placement.width);
                ini_put(settings.c_str(), L"menu", L"hotkey_footer", values.hotkeyFooter ? 1 : 0);
            }
        }
    }, IniWrite::MenuStep);
}

namespace {

// Held keys and stick directions repeat after kRepeatDelayMs, every kRepeatMs.
bool repeat(int& held, int64_t& nextAt, int direction, int64_t now) {
    if (direction < 0) {
        held = -1;
        return false;
    }
    if (held != direction) {
        held = direction;
        nextAt = now + qpc_ms(kRepeatDelayMs);
        return true;
    }
    if (now < nextAt)
        return false;
    nextAt = now + qpc_ms(kRepeatMs);
    return true;
}

// The menu's hotkeys: the open/close one, and the four directions. A key's first press opens the menu; a wheel's
// directions act only while it shows (the game may use the same switch).
void menu_keyboard(int64_t now) {
    pad_watch_ensure();
    const bool toggle = hotkey_held(HotkeyAction::MenuToggle);
    if (toggle && !menu.hotkeyTogglePrevious) {
        if (menu.visible) {
            menu.visible = false;
            log_line("settings menu: closed");
        } else {
            menu_open(now);
        }
    }
    menu.hotkeyTogglePrevious = toggle;
    int down = -1;
    for (int i = 0; i < 4 && down < 0; ++i) {
        const HotkeyAction a = static_cast<HotkeyAction>(static_cast<int>(HotkeyAction::MenuUp) + i);
        if (key_held(hotkeys[static_cast<int>(a)].key) || (menu.visible && pad_held(a)))
            down = i;
    }
    if (repeat(menu.heldKey, menu.keyRepeat, down, now))
        menu_press(kMenuKeyOf[down], now);
}

void menu_controllers(XrSession session, int64_t now) {
    bool toggle = false;
    XrVector2f stick{};
    {
        std::lock_guard guard(gaze.mutex);
        if (!gaze.attached || !gaze.menuToggle)
            return;
        if (gaze.appSyncs == 0) {  // ACE never syncs actions: sync the layer's set alone
            const XrActiveActionSet active{gaze.set, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            next.SyncActions(session, &sync);
        }
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = gaze.menuToggle;
        XrActionStateBoolean pressed{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(next.GetActionStateBoolean(session, &get, &pressed)) && pressed.isActive)
            toggle = pressed.currentState;
        get.action = gaze.menuNavigate;
        XrActionStateVector2f axes{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(next.GetActionStateVector2f(session, &get, &axes)) && axes.isActive)
            stick = axes.currentState;
        if (!menu.profileLogged && (toggle || stick.x != 0 || stick.y != 0)) {
            menu.profileLogged = true;
            XrPath hand = XR_NULL_PATH;
            XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
            const bool known = XR_SUCCEEDED(next.StringToPath(gaze.instance, "/user/hand/right", &hand)) &&
                               XR_SUCCEEDED(next.GetCurrentInteractionProfile(session, hand, &state));
            logf("settings menu: controller input from %s",
                 known ? path_name(gaze.instance, state.interactionProfile).c_str() : "?");
        }
    }
    if (toggle && !menu.togglePrevious) {
        if (menu.visible) {
            menu.visible = false;
            log_line("settings menu: closed");
        } else {
            menu_open(now);
        }
    }
    menu.togglePrevious = toggle;
    // A clear push in one direction; back below kRelease ends it.
    constexpr float kPush = 0.6f, kRelease = 0.3f;
    int dir = menu.stickDir;
    const float ax = std::fabs(stick.x), ay = std::fabs(stick.y);
    if (std::max(ax, ay) < kRelease)
        dir = -1;
    else if (ay >= kPush && ay > ax)
        dir = stick.y > 0 ? 0 : 1;
    else if (ax >= kPush && ax > ay)
        dir = stick.x < 0 ? 2 : 3;
    if (menu.visible && repeat(menu.stickDir, menu.stickRepeat, dir, now))
        menu_press(kMenuKeyOf[dir], now);
    else if (!menu.visible)
        menu.stickDir = dir;  // a push held while opening does not step
}

bool menu_resources(XrSession session) {
    if (!game->has_qvfr())  // log only: nothing of ours in the headset
        return false;
    if (menu.chain)
        return true;
    if (menu.failed)
        return false;
    const SessionGraphics graphics = session_graphics_now();
    if (!graphics.valid())
        return false;  // no D3D11 or D3D12 session (yet)
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    chainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    chainInfo.sampleCount = 1;
    chainInfo.width = kPanelWidth;
    chainInfo.height = kPanelHeight;
    chainInfo.faceCount = 1;
    chainInfo.arraySize = 1;
    chainInfo.mipCount = 1;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &menu.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &menu.chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, menu.chain, menu.images);
    if (XR_SUCCEEDED(r) && !menu.images.empty())
        menu.upload = make_panel_upload(graphics);
    if (XR_FAILED(r) || menu.images.empty() || !menu.upload) {
        logf("settings menu: panel setup failed (%d, %s); the menu still changes settings", r, graphics.name());
        menu.failed = true;
        return false;
    }
    XrCompositionLayerQuad& l = menu.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    l.space = menu.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = menu.chain;
    l.subImage.imageRect = {{0, 0}, {kPanelWidth, kPanelHeight}};
    const MenuPlacement& placement = live.menuPlacement;
    menu_place(placement);
    logf("settings menu: panel ready, %d deg right, %d deg up, %d deg wide", placement.yaw, placement.pitch, placement.width);
    return true;
}

// Pixels per degree at the centre of the view at the runtime's recommended size (100% density), over
// the FOV ACE renders with: views4 reads it back from ACE's projection (the focus density is against
// it), Two-loop uses frame A's. 0 until known.
double native_center_ppd() {
    float tan[4] = {};
    Views4Frame frame{};
    if (experiment.views4 && game->qvfr_latest_frame(frame))
        std::memcpy(tan, frame.outerTan[0], sizeof(tan));
    std::lock_guard guard(quad.mutex);
    if (!(tan[1] > tan[0])) {
        if (experiment.views4 && quad.v4FovValid)
            fov_tangents(quad.v4Fov[0], tan);
        else if (quad.aViewsValid)
            fov_tangents(quad.aViews[0].fov, tan);
    }
    // The native size spans the uncropped FOV (the crop keeps the density).
    const double width = (static_cast<double>(tan[1]) - tan[0]) / experiment.crop().kept_width();
    return quad.nativeW && width > 0.02 ? quad.nativeW / width * 3.14159265358979 / 180 : 0;
}

void menu_render() {
    const std::wstring title = menu_title(menu.model);
    const std::wstring hint = menu_hint();
    if (menu.model.launch.nativePpd <= 0)  // known once ACE has rendered a frame
        menu.model.launch.nativePpd = native_center_ppd();
    menu.model.launch.dlssHeld = experiment.views4 && game->qvfr_dlss_held();
    const std::vector<uint32_t> pixels = draw_menu_panel(title, menu_resolution(menu.model), menu_stats(menu.model),
                                                         menu_lines(menu.model),
                                                         menu_notes(menu.model), hint, menu_subtitle(menu.model),
                                                         menu.model.values.hotkeyFooter
                                                             ? hotkey_footer(menu.model.values.helmet.on && experiment.composing())
                                                             : L"");
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100'000'000;
    if (XR_FAILED(next.AcquireSwapchainImage(menu.chain, nullptr, &index)))
        return;
    const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(menu.chain, &wait)) && index < menu.images.size() &&
                    menu.upload && menu.upload->upload(menu.images[index], pixels);
    next.ReleaseSwapchainImage(menu.chain, nullptr);
    if (ok) {
        menu.uploaded = true;
        menu.dirty = false;
    }
}

const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now);  // the overlay's card (below)

void optimize_tick(int64_t now);  // the QVFR optimizer (below)

bool notice_pending();            // a notice for the start of the session (below)

// This frame's layers with the performance overlay and the menu panel on top while they are shown;
// `info` itself otherwise.
const XrFrameEndInfo* menu_frame(XrSession session, const XrFrameEndInfo* info, XrFrameEndInfo& storage,
                                 std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    // The performance overlay also while measuring without QVFR (baseline=1), to compare the two; the settings
    // menu only with a method of the layer's (its settings would change nothing in mode=off).
    const bool method = experiment.views4 || experiment.quad;
    if (live.ini.empty() || (!method && !experiment.measure && !notice_pending()))
        return info;
    // For the launcher's Center PPD: once, when ACE has rendered.
    static bool ppdReported = false;
    if (method && !ppdReported && endFrames % 90 == 0) {
        if (const double ppd = native_center_ppd(); ppd > 0) {
            wchar_t text[32];
            swprintf_s(text, L"%.2f", ppd);
            runtime_info(L"native_center_ppd", text);
            logf("settings menu: %.2f px per degree at the centre at 100%% density", ppd);
            ppdReported = true;
        }
    }
    const int64_t now = qpc_now();
    if (method) {
        menu_keyboard(now);
        menu_controllers(session, now);
        optimize_tick(now);
    }
    if (menu.visible && now >= menu.hideAt) {
        menu.visible = false;
        log_line("settings menu: hidden");
    }
    bool showMenu = menu.visible && menu_resources(session);
    if (showMenu && menu.dirty)
        menu_render();
    showMenu = showMenu && menu.uploaded;
    const XrCompositionLayerBaseHeader* card = osd_frame(session, now);
    if (!showMenu && !card)
        return info;
    layers.assign(info->layers, info->layers + info->layerCount);
    if (card)
        layers.push_back(card);
    if (showMenu)  // the menu over the card
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu.layer));
    storage = *info;
    storage.layerCount = static_cast<uint32_t>(layers.size());
    storage.layers = layers.data();
    return &storage;
}

}  // namespace

void menu_shutdown() {
    menu.upload.reset();
    if (menu.chain)
        next.DestroySwapchain(menu.chain);
    if (menu.view)
        next.DestroySpace(menu.view);
    menu.chain = XR_NULL_HANDLE;
    menu.view = XR_NULL_HANDLE;
    menu.images.clear();
    menu.visible = menu.uploaded = menu.failed = false;
    menu.dirty = true;
}

namespace {

constexpr int64_t kOsdDrawMs = 500, kOsdCheekyMs = 1000;

constexpr int64_t kNoticeMs = 30000;

constexpr OsdPlacement kNoticePlacement{true, 0, -6, 34};

void notice_set(std::wstring title, std::vector<std::wstring> lines) {
    notice.title = std::move(title);
    notice.lines = std::move(lines);
}

bool notice_pending() { return !notice.title.empty(); }

constexpr size_t kOsdHistory = 150;

}  // namespace

void osd_waited(XrDuration period) { osd.periodNs.store(period, std::memory_order_relaxed); }

bool frame_generation_on() { return cheeky_running() && osd.cheeky.processing && osd.cheeky.fg; }

bool osd_shown(OsdSummary& out) {
    if (!osd.placement.enabled || !osd.shownValid)
        return false;
    out = osd.shown;
    return true;
}

void osd_wait_returned(double waitMs) {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    osd.waitReturnQpc = qpc_now();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    osd.waitReturnCycles = cycles;
    osd.waitThread = GetCurrentThreadId();
    osd.waitMs = waitMs;
}

namespace {

void osd_measure() {
    if (!osd.placement.enabled && !optimize.optimizer.running())
        return;
    const int64_t now = qpc_now();
    const DWORD thread = GetCurrentThreadId();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    if (!osd.tscStartQpc) {
        osd.tscStartQpc = now;
        osd.tscStart = __rdtsc();
    } else if (osd.cyclesPerMs == 0 && now - osd.tscStartQpc > qpc_ms(500)) {
        osd.cyclesPerMs = static_cast<double>(__rdtsc() - osd.tscStart) /
                          (static_cast<double>(now - osd.tscStartQpc) * 1000.0 / static_cast<double>(qpc_ms(1000)));
    }
    if (osd.lastEnd) {
        OsdFrame frame;
        frame.intervalMs = static_cast<double>(now - osd.lastEnd) * 1000.0 / static_cast<double>(qpc_ms(1000));
        if (thread == osd.waitThread && osd.waitReturnQpc > osd.lastEnd && osd.cyclesPerMs > 0) {
            // The render thread's work for the frame: from xrWaitFrame's return, its wall time and what of it ran.
            frame.workMs = static_cast<double>(now - osd.waitReturnQpc) * 1000.0 / static_cast<double>(qpc_ms(1000));
            frame.cpuMs = static_cast<double>(cycles - osd.waitReturnCycles) / osd.cyclesPerMs;
            frame.waitMs = osd.waitMs;
        } else if (thread == osd.thread && osd.cyclesPerMs > 0) {
            frame.cpuMs = static_cast<double>(cycles - osd.lastCycles) / osd.cyclesPerMs;
        }
        if (osd.placement.enabled) {
            osd.frames.push_back(frame);
            osd.history.push_back(frame.intervalMs);
            if (osd.history.size() > kOsdHistory)
                osd.history.pop_front();
        }
        if (optimize.optimizer.running())
            optimize.frames.push_back(frame);
    }
    osd.lastEnd = now;
    osd.thread = thread;
    osd.lastCycles = cycles;
}

void osd_place() {
    const OsdPlacement& p = notice_pending() ? kNoticePlacement : osd.placement;
    const MenuPose pose = menu_pose({p.yaw, p.pitch, p.width}, static_cast<float>(kOsdHeight) / kOsdWidth);
    osd.layer.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
    osd.layer.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    osd.layer.size = {pose.width, pose.height};
}

void osd_placement_changed() {
    osd_placement_changed(read_osd_placement(live.ini.c_str()), read_cheeky_settings(live.ini.c_str()));
}

void osd_placement_changed(const OsdPlacement& placement, const CheekySettings& cheeky) {
    osd.cheeky = cheeky;  // frame generation on or off, its multiplier
    if (osd.read && placement == osd.placement)
        return;
    if (osd.read || placement.enabled)
        logf("performance overlay: %s, %d deg right, %d deg up, %d deg wide", placement.enabled ? "on" : "off",
             placement.yaw, placement.pitch, placement.width);
    if (placement.enabled && !osd.placement.enabled) {  // a fresh window
        osd.frames.clear();
        osd.history.clear();
        osd.lastEnd = 0;
        osd.nextDraw = 0;
    }
    osd.placement = placement;
    osd.read = true;
    osd_place();
}

// The visor hotkey (Ctrl+Alt+V by default) steps the helmet's visor (closed, half, open, closed), written to the
// ini the layer reloads from and to the launcher's, as the headset menu writes it.
void visor_hotkey() {
    const bool held = hotkey_held(HotkeyAction::Visor);
    if (held && !visorHotkeyHeld && experiment.composing()) {
        HelmetSettings h;
        {
            std::lock_guard guard(quad.mutex);  // at once; the ini on the worker
            helmetSettings.visor = static_cast<VisorPosition>((static_cast<int>(helmetSettings.visor) + 1) % 3);
            h = helmetSettings;
        }
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        const std::wstring settings = n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
        ini_write_later([ini = live.ini, settings, h] {
            write_helmet_settings(ini.c_str(), h);
            if (!settings.empty())
                write_helmet_settings(game_settings_ini(settings).c_str(), h);
        });
        logf("helmet: visor %ls (hotkey)", kVisorPositionNames[static_cast<int>(h.visor)]);
    }
    visorHotkeyHeld = held;
}

void osd_hotkey() {
    pad_watch_ensure();  // also without a method of the layer's (baseline=1)
    const bool held = hotkey_held(HotkeyAction::Overlay);
    if (held && !osd.hotkeyHeld) {
        OsdPlacement placement = osd.placement;
        placement.enabled = !placement.enabled;
        wchar_t path[MAX_PATH] = {};  // the launcher's own ini: the next launch starts with it
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_SETTINGS", path, MAX_PATH);
        const std::wstring settings = n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
        ini_write_later([ini = live.ini, settings, placement] {
            write_osd_placement(ini.c_str(), placement);
            if (!settings.empty())
                write_osd_placement(settings.c_str(), placement);
        });
        osd_placement_changed(placement, osd.cheeky);  // at once; the ini on the worker
    }
    osd.hotkeyHeld = held;
}

bool osd_resources(XrSession session) {
    if (!game->has_qvfr())  // log only: nothing of ours in the headset
        return false;
    if (osd.chain)
        return true;
    if (osd.failed)
        return false;
    const SessionGraphics graphics = session_graphics_now();
    if (!graphics.valid())
        return false;
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1;
    XrSwapchainCreateInfo chainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    chainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    chainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    chainInfo.sampleCount = 1;
    chainInfo.width = kOsdWidth;
    chainInfo.height = kOsdHeight;
    chainInfo.faceCount = 1;
    chainInfo.arraySize = 1;
    chainInfo.mipCount = 1;
    XrResult r = next.CreateReferenceSpace(session, &spaceInfo, &osd.view);
    if (XR_SUCCEEDED(r))
        r = next.CreateSwapchain(session, &chainInfo, &osd.chain);
    if (XR_SUCCEEDED(r))
        r = swapchain_textures(graphics, next.EnumerateSwapchainImages, osd.chain, osd.images);
    if (XR_SUCCEEDED(r) && !osd.images.empty())
        osd.upload = make_panel_upload(graphics, kOsdWidth, kOsdHeight);
    if (XR_FAILED(r) || osd.images.empty() || !osd.upload) {
        logf("performance overlay: card setup failed (%d, %s)", r, graphics.name());
        osd.failed = true;
        return false;
    }
    XrCompositionLayerQuad& l = osd.layer;
    l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    l.space = osd.view;
    l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    l.subImage.swapchain = osd.chain;
    l.subImage.imageRect = {{0, 0}, {kOsdWidth, kOsdHeight}};
    osd_place();
    if (experiment.views4 && graphics.d3d12)  // the GPU frame timer reads D3D12 timestamps (D3D11: no GPU times)
        gpu_frame_timer_start(graphics.d3d12, graphics.queue);
    // VRAM: this process's use of the adapter's local memory against the budget Windows gives it.
    IDXGIFactory4* factory = nullptr;
    LUID luid{};
    if (graphics.adapter_luid(luid) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&osd.adapter))))
            osd.adapter = nullptr;
        factory->Release();
    }
    // D3D11 (ACC, X-Plane): no queue timestamps, so the GPU tile is the whole GPU's load from Windows' engine
    // counters (probe/gpu_load.h). A D3D12 session started it at xrCreateSession; it stops with the session.
    if (!graphics.d3d12 && graphics.adapter_luid(luid))
        gpu_load_start(static_cast<uint32_t>(luid.HighPart), luid.LowPart);
    log_line(osd.adapter ? "performance overlay: card ready" : "performance overlay: card ready (no VRAM numbers)");
    return true;
}

OsdInput osd_input(int64_t now) {
    OsdInput in;
    in.frames.swap(osd.frames);
    in.periodMs = static_cast<double>(osd.periodNs.load(std::memory_order_relaxed)) / 1e6;
    if (experiment.views4) {
        const GpuFrameStats gpu = gpu_frame_timer_collect();
        if (gpu.frames) {
            in.gpuMs = gpu.renderAvg;
            if (experiment.views4Compose)
                in.composeMs = gpu.composeAvg;
        }
    }
    if (osd.adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (SUCCEEDED(osd.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            in.vramUsed = memory.CurrentUsage;
            in.vramBudget = memory.Budget;
        }
    }
    {
        std::lock_guard guard(gaze.mutex);
        const uint64_t queries = gaze.queries - osd.gazeQueries, tracked = gaze.tracked - osd.gazeTracked;
        osd.gazeQueries = gaze.queries;
        osd.gazeTracked = gaze.tracked;
        if (!experiment.gaze || queries == 0)  // off, or not asked (the focus does not follow the eyes here)
            in.gaze = gaze.action || !experiment.gaze ? OsdGaze::Off : OsdGaze::Unavailable;
        else
            in.gaze = tracked * 10 >= queries * 9 ? OsdGaze::Tracking : tracked * 2 >= queries ? OsdGaze::Partial : OsdGaze::Lost;
    }
    // Frame generation: Cheeky's, when the layer runs it with [cheeky] fg on.
    const CheekySettings& cheeky = osd.cheeky;
    if (cheeky_running() && cheeky.processing && cheeky.fg) {
        if (now >= osd.nextCheeky) {
            osd.cheekySnapshot = cheeky_snapshot();
            osd.nextCheeky = now + qpc_ms(kOsdCheekyMs);
        }
        const OsdCheeky c = osd_cheeky(osd.cheekySnapshot);
        in.fg = c.active ? OsdFg::On : OsdFg::Starting;
        in.fgMultiplier = cheeky.fgMultiplier;
        in.fgOutputFps = c.outputFps;
        in.fgMs = c.fgMs;
        in.fgState = c.state;
        in.gpuBusyPercent = c.gpuBusyPercent;
        if (c.refreshHz > 0)
            in.refreshMs = 1000.0 / c.refreshHz;  // the headset's own refresh (ACE's period may be multiplied)
    }
    // The whole GPU and ACE's threads, from the layer's own samplers (Cheeky's GPU share only without them),
    // averaged over the card's window (since its last draw).
    int samples = 0;
    if (const GpuLoad gpu = gpu_load_window(samples, 0); gpu.valid) {
        in.gpuBusyPercent = gpu.busy;
        in.gpuGamePercent = gpu.game;
    }
    if (const CpuLoad threads = cpu_load_window(samples, 0); threads.valid) {
        in.cpuBusiestPercent = threads.busiestPercent;
        in.cpuBusiestName = threads.busiestName;
        in.cpuBusiestIsRender = threads.busiestThread == osd.thread;
    }
    return in;
}

// The notice's card while it lasts (null once it is over, or if the card cannot be made); the overlay's card
// afterwards, redrawn before it shows again.
const XrCompositionLayerBaseHeader* notice_frame(XrSession session, int64_t now) {
    if (!notice_pending())
        return nullptr;
    if (!notice.until)
        notice.until = now + qpc_ms(kNoticeMs);
    if (now >= notice.until || !osd_resources(session)) {
        notice = {};
        osd.uploaded = false;  // the card holds the notice's pixels
        osd.nextDraw = 0;
        osd_place();
        log_line("notice: hidden");
        return nullptr;
    }
    if (!notice.uploaded) {
        osd_place();
        const std::vector<uint32_t> pixels = draw_notice_panel(notice.title, notice.lines);  // once: a few ms
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;
        if (XR_SUCCEEDED(next.AcquireSwapchainImage(osd.chain, nullptr, &index))) {
            notice.uploaded = XR_SUCCEEDED(next.WaitSwapchainImage(osd.chain, &wait)) && index < osd.images.size() &&
                              osd.upload && osd.upload->upload(osd.images[index], pixels);
            next.ReleaseSwapchainImage(osd.chain, nullptr);
        }
        if (notice.uploaded)
            log_line("notice: shown");
    }
    return notice.uploaded ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&osd.layer) : nullptr;
}

const XrCompositionLayerBaseHeader* osd_frame(XrSession session, int64_t now) {
    if (const XrCompositionLayerBaseHeader* card = notice_frame(session, now))
        return card;
    if (!osd.read)
        osd_placement_changed();
    osd_hotkey();
    visor_hotkey();
    if (!osd.placement.enabled || !osd_resources(session))
        return nullptr;
    if (now >= osd.nextDraw && !osd.drawing.valid()) {
        osd.nextDraw = now + qpc_ms(kOsdDrawMs);
        OsdInput in = osd_input(now);
        osd.shown = osd_summary(in);  // also for the log
        osd.shownValid = true;
        osd.drawing = std::async(std::launch::async, [in = std::move(in), summary = osd.shown,
                                                      history = std::vector<double>(osd.history.begin(), osd.history.end())] {
            return draw_osd_panel(summary, history, osd_tiles(in, summary), osd_rows(in, summary));
        });
    }
    if (osd.drawing.valid() && osd.drawing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::vector<uint32_t> pixels = osd.drawing.get();
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;
        if (XR_SUCCEEDED(next.AcquireSwapchainImage(osd.chain, nullptr, &index))) {
            const bool ok = XR_SUCCEEDED(next.WaitSwapchainImage(osd.chain, &wait)) && index < osd.images.size() &&
                            osd.upload && osd.upload->upload(osd.images[index], pixels);
            next.ReleaseSwapchainImage(osd.chain, nullptr);
            osd.uploaded = osd.uploaded || ok;
        }
    }
    return osd.uploaded ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&osd.layer) : nullptr;
}

}  // namespace

void osd_shutdown() {
    if (osd.drawing.valid())
        osd.drawing.wait();
    osd.drawing = {};
    osd.upload.reset();
    if (osd.chain)
        next.DestroySwapchain(osd.chain);
    if (osd.view)
        next.DestroySpace(osd.view);
    if (osd.adapter)
        osd.adapter->Release();
    osd.chain = XR_NULL_HANDLE;
    osd.view = XR_NULL_HANDLE;
    osd.adapter = nullptr;
    osd.images.clear();
    osd.uploaded = osd.failed = false;
    osd.frames.clear();
    osd.history.clear();
    osd.lastEnd = 0;
}

namespace {

// ---- QVFR optimizer --------------------------------------------------------------------------
// The settings menu's Optimize (quad/qvfr_optimizer.h): Right runs it (or stops a run), Left undoes it.
// Every kOptimizeWindowMs a window of ACE's frames goes to the optimizer: the GPU timer's own samples
// (90th percentile) and the render thread's work, which the overlay's rule (osd_summary) also uses to say
// whether the CPU limits the frame. The values it asks for are written as a menu step is, so the live
// reload resizes the views (or, with DLSS holding them, the next launch does). [optimize] in the ini:
// target_percent (of the frame budget), min_periphery, min_density, min_focus (the floors it lowers to).
void optimize_apply(const QvfrValues& q) {
    MenuValues& v = menu.model.values;
    v.periphery = q.periphery;
    v.density = q.density;
    v.width = q.width;
    v.height = q.height;
    menu.dirty = true;
    menu.lastWrite = ini_write_later([ini = live.ini, settings = menu.settingsIni, values = v] {
        write_menu_values(ini.c_str(), values, true);
        if (!settings.empty())
            write_menu_values(game_settings_ini(settings).c_str(), values, true);
    });
    logf("optimize: periphery %d%%, focus density %d%%, focus box %d%% x %d%%", q.periphery, q.density, q.width,
         q.height);
}

void optimize_start(int64_t now) {
    optimize.note.clear();
    // It measures what each step does, so the views must resize live (with DLSS: views4_dlss_live).
    if (game->qvfr_dlss_held()) {
        optimize.note = L"DLSS holds the view sizes until the next launch (DLAA, or views4_dlss_live=0): it can't measure live";
        return;
    }
    int nativeW = 0;
    {
        std::lock_guard guard(quad.mutex);
        if (!gpu_frame_timer_start(quad.device, quad.queue)) {
            optimize.note = L"No GPU frame timing on this device";
            return;
        }
        nativeW = static_cast<int>(quad.nativeW);
    }
    gpu_frame_timer_collect_optimizer();  // drop what the windows gathered before the run
    int samples = 0;
    gpu_load_window(samples, 2);
    cpu_load_window(samples, 2);
    optimize.frames.clear();
    optimize.nextWindow = now + qpc_ms(kOptimizeWindowMs);
    const wchar_t* ini = live.ini.c_str();
    const auto read = [ini](const wchar_t* key, int fallback, int lo, int hi) {
        return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"optimize", key, fallback, ini)), lo, hi);
    };
    const MenuValues& v = menu.model.values;
    const MenuLaunch& l = menu.model.launch;
    QvfrLimits limits;
    const int minFocus = read(L"min_focus", limits.floor.width, 10, 100);
    limits.floor = {read(L"min_periphery", limits.floor.periphery, 10, 100), read(L"min_density", limits.floor.density, 50, 400),
                    minFocus, minFocus};
    limits.cropWidth = v.crop_width();
    limits.cropHeight = v.crop_height();
    limits.nativeWidth = nativeW;
    // The views fit in the launch target (views4_live), at most their launch sizes.
    limits.ceiling.periphery = static_cast<int>(l.outer);
    limits.ceiling.density = static_cast<int>(std::min(l.focusWidth * 100 / v.width, l.focusHeight * 100 / v.height));
    if (nativeW > 0)
        limits.maxTargetWidth = static_cast<int>(2.0 * nativeW * (l.outer * l.cropWidth / 1e4 + l.focusWidth / 100));
    // From what is in effect (a value above the launch size renders at what fits).
    const MenuFit fit = menu_fit(menu.model);
    const QvfrValues current{static_cast<int>(std::lround(fit.periphery)), static_cast<int>(std::lround(fit.densityX)),
                             v.width, v.height};
    optimize.optimizer.start(current, limits, read(L"target_percent", 88, 50, 98) / 100.0);
    logf("optimize: start from periphery %d%%, focus density %d%%, box %d%% x %d%%; floors %d%%, %d%%, %d%%; "
         "ceilings %d%%, %d%%",
         current.periphery, current.density, current.width, current.height, limits.floor.periphery,
         limits.floor.density, limits.floor.width, limits.ceiling.periphery, limits.ceiling.density);
}

// The budget is the period xrWaitFrame gives ACE: with Cheeky's frame generation already the multiplied
// one, so `in` leaves frame generation off. The GPU time is the whole GPU's per ACE frame (frame
// generation, the composition and the runtime's compositor too) when the load sampler has it, else ACE's
// own queue (90th percentile).
QvfrWindow optimize_window() {
    const GpuFrameStats gpu = gpu_frame_timer_collect_optimizer();
    OsdInput in;
    in.frames.swap(optimize.frames);
    in.periodMs = static_cast<double>(osd.periodNs.load(std::memory_order_relaxed)) / 1e6;
    in.gpuMs = gpu.frames ? gpu.renderP90 : -1;
    int samples = 0;
    if (const GpuLoad load = gpu_load_window(samples, 2); load.valid) {
        in.gpuBusyPercent = load.busy;
        in.gpuGamePercent = load.game;
    }
    if (const CpuLoad threads = cpu_load_window(samples, 2); threads.valid) {
        in.cpuBusiestPercent = threads.busiestPercent;
        in.cpuBusiestIsRender = threads.busiestThread == osd.thread;
    }
    const OsdSummary s = osd_summary(in);
    QvfrWindow w;
    w.gpuMs = s.gpuMs;
    w.cpuMs = s.cpuMs;
    w.budgetMs = s.budgetMs;
    w.cpuBound = s.bound == OsdBound::Cpu;
    w.frames = gpu.frames;
    return w;
}

void optimize_tick(int64_t now) {
    QvfrOptimizer& o = optimize.optimizer;
    if (const int request = std::exchange(menu.model.optimizeRequest, 0); request != 0 && experiment.views4) {
        optimize.shown = true;
        if (request > 0 && o.running()) {
            o.cancel();
            log_line("optimize: stopped");
        } else if (request > 0) {
            optimize_start(now);
        } else {
            o.cancel();
            optimize.note.clear();
            if (const QvfrAction back = o.undo(); back.apply)
                optimize_apply(back.values);
        }
    }
    if (o.running()) {
        if (menu.visible)  // shown until it is done
            menu.hideAt = std::max(menu.hideAt, now + qpc_ms(kMenuHideMs));
        if (now >= optimize.nextWindow) {
            optimize.nextWindow = now + qpc_ms(kOptimizeWindowMs);
            if (const QvfrAction a = o.feed(optimize_window()); a.apply)
                optimize_apply(a.values);
        }
    } else {
        optimize.frames.clear();
    }
    // The menu's line and detail.
    std::wstring line, detail;
    if (optimize.shown) {
        detail = optimize.note.empty() ? o.status() : optimize.note;
        switch (o.state()) {
        case QvfrState::Measuring:
        case QvfrState::Settling: line = L"Running…"; break;
        case QvfrState::Done: line = o.can_undo() ? L"Done (Left: undo)" : L"Done"; break;
        case QvfrState::Failed: line = L"Not possible"; break;
        default: line = optimize.note.empty() ? L"" : L"Not possible"; break;
        }
    }
    if (line != menu.model.optimizeStatus || detail != menu.model.optimizeDetail) {
        if (detail != menu.model.optimizeDetail && !detail.empty())
            logf("optimize: %ls", detail.c_str());
        menu.model.optimizeStatus = line;
        menu.model.optimizeDetail = detail;
        menu.dirty = true;
    }
}

// cpu_profile=1: a capture of the busiest threads (and this one, the render thread) at cpu_profile_frame, and
// whenever Ctrl+Alt+C is pressed while ACE has focus and none is running.
void profile_notice(const std::wstring& text, int64_t ms) {
    if (!experiment.views4 && !experiment.quad)
        return;  // no settings menu to show it in (baseline): the log has it
    const int64_t now = qpc_now();
    menu.model.notice = text;
    menu.dirty = true;
    menu_open(now);
    menu.hideAt = now + qpc_ms(ms);
    profileNotice.until = menu.hideAt;
}

// The frame capture hotkey (d3d11_capture_hotkey, probe/d3d11_capture.h): each press saves the next frame's swapchain
// images, the game's and the composed output: what the headset gets at that moment.
void frame_capture_poll() {
    if (!experiment.d3d11CaptureHotkey)
        return;
    const bool held = hotkey_held(HotkeyAction::FrameCapture);
    const bool pressed = held && !frameCaptureHotkeyHeld;
    frameCaptureHotkeyHeld = held;
    if (pressed && d3d11_capture_arm(endFrames + 1))
        profile_notice(L"Frame capture: saved to the logs folder (captures)", 2500);
}

void cpu_profile_poll() {
    if (!experiment.cpuProfile)
        return;
    const bool running = cpu_profile_running();
    if (profileNotice.wasRunning && !running)
        profile_notice(L"CPU capture done: the report is in the logs folder", 5000);
    profileNotice.wasRunning = running;
    if (profileNotice.until && qpc_now() >= profileNotice.until) {
        profileNotice.until = 0;
        menu.model.notice.clear();
        menu.dirty = true;
    }
    const bool held = hotkey_held(HotkeyAction::CpuCapture);
    const bool pressed = held && !cpuProfileHotkeyHeld;
    cpuProfileHotkeyHeld = held;
    // cpu_profile_frame=0 is no capture by itself, not one at the session's first frame.
    if (!pressed && (experiment.cpuProfileFrame == 0 || endFrames != experiment.cpuProfileFrame))
        return;
    if (cpu_profile_running()) {
        log_line("cpu profile: a capture is running already");
        return;
    }
    // The render thread (this one), ACE's game and physics threads by name (busy or not at this moment: a
    // capture started on a loading screen still gets them), then the busiest others.
    CpuProfileRequest request;
    request.seconds = experiment.cpuProfileSeconds;
    request.slowFrameMs = experiment.cpuProfileSlowMs;
    request.threads.push_back(GetCurrentThreadId());
    const std::vector<ThreadLoad> all = cpu_load_latest_threads(256);
    for (const ThreadLoad& t : all)
        if (t.name == L"GameThread" || t.name == L"Physics")
            request.threads.push_back(t.id);
    for (size_t i = 0, added = 0; i < all.size() && added < static_cast<size_t>(experiment.cpuProfileThreads); ++i)
        if (std::find(request.threads.begin(), request.threads.end(), all[i].id) == request.threads.end()) {
            request.threads.push_back(all[i].id);
            ++added;
        }
    if (cpu_profile_start(request)) {
        profileNotice.wasRunning = true;
        profile_notice(L"CPU capture: " + std::to_wstring(experiment.cpuProfileSeconds) + L" s, keep driving", 4000);
    }
}

}  // namespace

XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info) {
    hang_watch_frame();
    cpu_profile_mark_frame();  // the game's frame ends here: a running capture's samples go to their frames
    cpu_load_render_thread(GetCurrentThreadId());
    cpu_profile_poll();
    frame_capture_poll();
    if ((experiment.views4 || experiment.measure) && cpu.msPerTick > 0) {
        const int64_t now = qpc_now();
        if (pacing.lastEnd && pacing.interval.size() < 4 * kCpuReportFrames)
            pacing.interval.push_back(static_cast<float>((now - pacing.lastEnd) * cpu.msPerTick));
        pacing.lastEnd = now;
    }
    osd_measure();   // before the layer's own work in the frame
    cheeky_frame();  // Cheeky's frame callback, after ACE's frame is recorded
    XrResult r;
    if (!cpu.realEndFrame) {
        r = end_frame(session, info);
    } else {
        const int64_t t0 = qpc_now();
        r = end_frame(session, info);
        cpu.endFrame.add(qpc_now() - t0);
        if (endFrames % kCpuReportFrames == 0)
            report_cpu_times();
    }
    live_reload();
    return r;
}

namespace {

XrResult end_frame(XrSession session, const XrFrameEndInfo* frameInfo) {
    XrFrameEndInfo withMenu{};
    std::vector<const XrCompositionLayerBaseHeader*> menuLayers;
    const XrFrameEndInfo* info = menu_frame(session, frameInfo, withMenu, menuLayers);
    ++endFrames;
    if (experiment.pixCaptureFrame)
        pix_capture_frame(endFrames);
    if (sampled(endFrames)) {
        logf("xrEndFrame frame=%llu time=%lld layers=%u", static_cast<unsigned long long>(endFrames),
             static_cast<long long>(info->displayTime), info->layerCount);
        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* layer = info->layers[i];
            if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                logf("  layer %u type=%d flags=0x%llx", i, layer->type,
                     static_cast<unsigned long long>(layer->layerFlags));
                continue;
            }
            auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            for (uint32_t v = 0; v < proj->viewCount; ++v) {
                const XrSwapchainSubImage& s = proj->views[v].subImage;
                const auto* chained = static_cast<const XrBaseInStructure*>(proj->views[v].next);
                logf("  projection view %u swapchain=%p rect=%d,%d %dx%d array=%u next=%d", v,
                     static_cast<void*>(s.swapchain), s.imageRect.offset.x, s.imageRect.offset.y,
                     s.imageRect.extent.width, s.imageRect.extent.height, s.imageArrayIndex,
                     chained ? chained->type : 0);
            }
        }
    }
    if (experiment.probes) {
        probe_frame_end();
        d3d12_trace_frame_end(probe_sequence_active());
    }
    if (experiment.passCensus)
        pass_census_frame_end();
    game->frame_end();
    if (experiment.views4) {
        game->qvfr_frame_end();
        return views4_submit(session, info);
    }
    if (!experiment.twoLoops)
        return next.EndFrame(session, info);

    std::unique_lock lock(loops.mutex);
    if (!loops.active) {
        ++loops.ends;
        if (loops.ends > loops.begins && !loops.broken) {
            loops.broken = true;
            log_line("two_loops: refused: xrEndFrame calls outnumber xrBeginFrame (a frame generator above this "
                     "layer, e.g. VectorXR Frame Generation?); turn it off for the game");
        }
        lock.unlock();
        return next.EndFrame(session, info);
    }
    LARGE_INTEGER now;
    const uint64_t index = loops.pairedEnds++;
    if (index % 2 == 0) {  // A: rendered for the display time, never submitted
        QueryPerformanceCounter(&now);
        loops.aMs += ms_between(loops.aWaitReturned, now);
        if (experiment.quad)
            quad_record_a(info);
        return XR_SUCCESS;
    }
    loops.inB = false;
    lock.unlock();
    const XrResult r = experiment.quad ? quad_submit_b(session, info) : next.EndFrame(session, info);
    lock.lock();
    // A fallback shows only frame B's focus region (black around it). Brief ones are harmless; a run of
    // them means composition does not work here, so stop pairing frames and let ACE render normally.
    if (experiment.quad) {
        std::lock_guard guard(quad.mutex);
        if (quad.fallbackRun >= kMaxFallbackRun) {
            loops.active = false;
            loops.broken = true;
            loops.inB = false;
            logf("quad: %llu frames in a row could not be composed; quad off, ACE runs unchanged",
                 static_cast<unsigned long long>(quad.fallbackRun));
        }
    }
    QueryPerformanceCounter(&now);
    loops.bMs += ms_between(loops.bWaitReturned, now);
    ++loops.pairs;
    const double sinceReport = ms_between(loops.lastReport, now);
    if (sinceReport >= 2000.0) {
        logf("two_loops: %.1f display frames/s, ACE frame A %.2f ms, frame B %.2f ms (wait return to end), "
             "result=%d, quad composed %llu fallbacks %llu, gaze tracked %llu/%llu (%.2f,%.2f,%.2f)",
             loops.pairs * 1000.0 / sinceReport, loops.aMs / loops.pairs, loops.bMs / loops.pairs, r,
             static_cast<unsigned long long>(quad.composed), static_cast<unsigned long long>(quad.fallbacks),
             static_cast<unsigned long long>(gaze.tracked), static_cast<unsigned long long>(gaze.queries),
             gaze.direction.x, gaze.direction.y, gaze.direction.z);
        loops.pairs = 0;
        loops.aMs = loops.bMs = 0;
        loops.lastReport = now;
    }
    return r;
}

}  // namespace

#define SIMQVFR_INTERCEPT(fn)                                              \
    if (std::strcmp(name, "xr" #fn) == 0) {                              \
        *function = reinterpret_cast<PFN_xrVoidFunction>(&layer##fn);    \
        return XR_SUCCESS;                                               \
    }

XrResult XRAPI_CALL layerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    SIMQVFR_INTERCEPT(GetInstanceProcAddr)
    if (next.EndFrame) {
        SIMQVFR_INTERCEPT(EnumerateViewConfigurationViews)
        SIMQVFR_INTERCEPT(CreateSession)
        SIMQVFR_INTERCEPT(DestroySession)
        SIMQVFR_INTERCEPT(BeginSession)
        SIMQVFR_INTERCEPT(WaitFrame)
        SIMQVFR_INTERCEPT(BeginFrame)
        SIMQVFR_INTERCEPT(CreateSwapchain)
        SIMQVFR_INTERCEPT(EnumerateSwapchainImages)
        SIMQVFR_INTERCEPT(LocateViews)
        SIMQVFR_INTERCEPT(CreateReferenceSpace)
        SIMQVFR_INTERCEPT(DestroySpace)
        SIMQVFR_INTERCEPT(EndFrame)
        SIMQVFR_INTERCEPT(AcquireSwapchainImage)
        SIMQVFR_INTERCEPT(ReleaseSwapchainImage)
        SIMQVFR_INTERCEPT(SuggestInteractionProfileBindings)
        SIMQVFR_INTERCEPT(AttachSessionActionSets)
        SIMQVFR_INTERCEPT(SyncActions)
        SIMQVFR_INTERCEPT(PollEvent)
        if (next.GetVisibilityMaskKHR)
            SIMQVFR_INTERCEPT(GetVisibilityMaskKHR)
    }
    return next.GetInstanceProcAddr ? next.GetInstanceProcAddr(instance, name, function) : XR_ERROR_HANDLE_INVALID;
}

namespace {

// The layers between this one and the runtime. VectorXR's frame generation has to be among them:
// above this layer it would get ACE's four-view image instead of the composed stereo frame.
void log_layer_order(const XrApiLayerNextInfo* below) {
    std::string names;
    bool vectorxrBelow = false;
    for (const XrApiLayerNextInfo* n = below; n; n = n->next) {
        names += names.empty() ? "" : ", ";
        names += n->layerName;
        vectorxrBelow |= std::strstr(n->layerName, "VECTORXR") != nullptr;
    }
    logf("layers below this one: [%s]", names.c_str());
    layersBelow = names;
    if (!vectorxrBelow && GetModuleHandleW(L"XR_APILAYER_DIENERTECH_VECTORXR.dll"))
        log_line("VectorXR is loaded above this layer: its frame generation would see the game's four-view image; move "
                 "this layer above VectorXR (machine-wide registration, then VectorXR's layer order)");
}

// Whether the layer adds XR_EXT_eye_gaze_interaction to the game's instance (layerCreateApiLayerInstance).
enum class GazeExtension {
    Requested,  // the game enables it itself
    Add,        // the runtime (through the layers below) lists it: add it
    Try,        // the list didn't answer: add it, and create the instance without it if that fails
    Absent,     // the runtime doesn't list it: leave the request as it is
};

GazeExtension gaze_extension(const XrInstanceCreateInfo& info, PFN_xrGetInstanceProcAddr gipa) {
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        if (std::strcmp(info.enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Requested;
    PFN_xrVoidFunction function = nullptr;
    if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function)) || !function)
        return GazeExtension::Try;
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    uint32_t count = 0;
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)))
        return GazeExtension::Try;
    std::vector<XrExtensionProperties> properties(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (count && XR_FAILED(enumerate(nullptr, count, &count, properties.data())))
        return GazeExtension::Try;
    for (const XrExtensionProperties& p : properties)
        if (std::strcmp(p.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            return GazeExtension::Add;
    return GazeExtension::Absent;
}

// Why a game can start flat with this layer loaded: the OpenXR loader negotiates every layer, then can still
// fail xrCreateInstance before the chain reaches this one (a layer named in XR_ENABLE_API_LAYERS whose manifest
// it refuses, an extension nobody provides). The loader prints its reason on the game's stderr, which the launcher
// keeps in the run folder's openxr-loader.log; this log names the layers and says when that happened.
void note_loader_negotiation() {
    const int n = ++loaderNegotiations;
    if (n == 1) {
        char enabled[2048] = {};
        GetEnvironmentVariableA("XR_ENABLE_API_LAYERS", enabled, static_cast<DWORD>(std::size(enabled)));
        logf("OpenXR layers named for this launch: %s", enabled[0] ? enabled : "(none: implicit layers only)");
    } else if (!instanceReachedLayer) {
        log_line("the game's previous xrCreateInstance failed before reaching this layer (no VR: the game starts "
                 "flat). The OpenXR loader's reason is in this launch's run folder, openxr-loader.log");
    }
}

// The layers named in SIM_QVFR_CHAIN, loaded from their manifests in XR_API_LAYER_PATH and linked below
// `self` (layer_chain.h: the launcher does this for a game whose OpenXR loader can't take a list of layers).
// A layer that fails to load is left out and logged, as the loader would. `self` when there is none.
const XrApiLayerNextInfo* chain_layers_below(const XrApiLayerNextInfo* self) {
    static std::vector<XrApiLayerNextInfo> storage;  // outlives the create call down the chain
    wchar_t value[4096] = {}, folder[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(chain::kChainVariable, value, static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value))
        return self;
    GetEnvironmentVariableW(L"XR_API_LAYER_PATH", folder, static_cast<DWORD>(std::size(folder)));
    std::vector<chain::Layer> layers;
    char names[4096] = {};  // layer names are ASCII
    WideCharToMultiByte(CP_UTF8, 0, value, -1, names, static_cast<int>(std::size(names)), nullptr, nullptr);
    for (const std::string& name : chain::split_names(names)) {
        chain::Layer layer;
        std::string why;
        if (chain::load(folder, name, layer, why)) {
            layers.push_back(layer);
            logf("chain: %s loaded below this layer", name.c_str());
        } else {
            logf("chain: %s left out: %s", name.c_str(), why.c_str());
        }
    }
    return chain::link(layers, self, storage);
}

}  // namespace

XrResult XRAPI_CALL layerCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!info || !layerInfo || !layerInfo->nextInfo || !instance ||
        std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    instanceReachedLayer = true;
    // This layer's node, with the layers the launcher asked it to chain (SIM_QVFR_CHAIN) linked below it.
    const XrApiLayerNextInfo* own = chain_layers_below(layerInfo->nextInfo);
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = own->next;
    PFN_xrGetInstanceProcAddr gipa = own->nextGetInstanceProcAddr;
    // Registered as an implicit layer, so it loads into every OpenXR app: stay silent and
    // pass-through everywhere except a supported game (src/games).
    const games::GameInfo* thisGame = games::current_game();
    if (!thisGame) {
        next = Dispatch{gipa};
        return own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    }
    // Eye tracking needs XR_EXT_eye_gaze_interaction on the instance. ACE and X-Plane ask for it themselves; ACC
    // (through OpenComposite) doesn't, so its gaze worked only with a layer below that adds it (Cheeky, OpenXR Toolkit,
    // VectorXR), and failed with none (the eye gaze profile's bindings rejected, XR_ERROR_PATH_UNSUPPORTED). So the
    // layer adds it when the game hasn't and the runtime offers it; with the offer unknown it tries, and creates the
    // instance without it if the runtime refuses.
    const GazeExtension gazeExtension = gaze_extension(*info, gipa);
    XrResult r = XR_ERROR_EXTENSION_NOT_PRESENT;
    if (gazeExtension == GazeExtension::Add || gazeExtension == GazeExtension::Try) {
        std::vector<const char*> names(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        names.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        XrInstanceCreateInfo withGaze = *info;
        withGaze.enabledExtensionCount = static_cast<uint32_t>(names.size());
        withGaze.enabledExtensionNames = names.data();
        r = own->nextCreateApiLayerInstance(&withGaze, &nextLayerInfo, instance);
    }
    const bool gazeAdded = XR_SUCCEEDED(r);
    if (!gazeAdded)
        r = own->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    game = &games::adapter_for(thisGame);
    load_experiment();
    // Where the launcher writes live settings while this ACE runs (live_reload).
    runtime_info(L"live_ini", live.ini);
    runtime_info(L"live_pid", std::to_wstring(GetCurrentProcessId()));
    log_layer_order(own->next);
    logf("xrCreateApiLayerInstance app=%s engine=%s extensions=%u result=%d", info->applicationInfo.applicationName,
         info->applicationInfo.engineName, info->enabledExtensionCount, r);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        logf("  extension %s", info->enabledExtensionNames[i]);
    switch (gazeExtension) {
    case GazeExtension::Requested: log_line("gaze: the game enables XR_EXT_eye_gaze_interaction itself"); break;
    case GazeExtension::Absent: log_line("gaze: the runtime doesn't offer XR_EXT_eye_gaze_interaction (no eye tracking)"); break;
    case GazeExtension::Add:
    case GazeExtension::Try:
        logf("gaze: XR_EXT_eye_gaze_interaction %s (%s)",
             gazeAdded ? "added for the game" : "NOT added: the runtime refused the instance with it",
             gazeExtension == GazeExtension::Add ? "the runtime offers it" : "the runtime's offer unknown, tried");
        break;
    }
    if (XR_FAILED(r))
        return r;
    // Seconds before ACE initialises NGX, so Cheeky's hooks see its DLSS features created. Only in a game
    // it's made for.
    if (game->loads_cheeky()) {
        cheeky_start(cheeky_config(live.ini.c_str(), layer_directory()));
        {  // for the launcher's Foveated DLSS pane
            static const wchar_t* const kStatus[] = {L"off", L"missing", L"refused", L"running"};
            runtime_info(L"cheeky_status", kStatus[static_cast<int>(cheeky_status())]);
            if (cheeky_running())
                runtime_info(L"cheeky_version", cheeky_version());
        }
        cheeky_apply_ini();
    }
    // Before ACE creates its D3D12 device and renderer, so renderer init sees the patched mode.
    if (experiment.dred)
        dred_enable();
    if (experiment.d3d12Debug)
        d3d12_debug_enable(experiment.gpuValidation);
    if (experiment.pixCaptureFrame)  // before ACE creates its D3D12 device
        pix_capture_load(experiment.pixCaptureFrame);
    game->instance_created(live.ini.c_str(), experiment.mode);
    if (experiment.views4) {
        games::QvfrStart start;
        start.layout = experiment.views4Layout;
        start.focusFraction = experiment.views4EyeCamera ? 0.0 : experiment.box_x();
        start.focusFractionY = experiment.box_y();
        start.dlssLive = experiment.views4DlssLive;
        // The cull box: the focus box plus its feather band plus a gaze-latency allowance.
        start.focusCull = experiment.views4FocusCull;
        start.focusCullScale = 1.0 + experiment.feather + experiment.views4FocusCullMargin;
        start.focusCullGaze = experiment.gaze && experiment.composing();  // gaze turns only when composing
        start.ini = live.ini.c_str();
        if (!game->qvfr_start(start)) {
            experiment.views4 = false;  // no doubled eye width or rect rewrite without the engine side
            notice_set(L"Sim-QVFR is off",
                       {L"This " + std::wstring(thisGame->name) +
                            L" build isn’t one Sim-QVFR supports, so the game runs as normal, "
                            L"without foveated rendering. The game has probably been updated.",
                        L"Look for an Sim-QVFR release for the new build. The launcher’s Launch checks say the same "
                        L"before a launch."});
        }
    }
    Dispatch d;
    d.GetInstanceProcAddr = gipa;
    bool ok = true;
#define SIMQVFR_LOAD(fn) ok &= XR_SUCCEEDED(gipa(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction*>(&d.fn))) && d.fn;
    SIMQVFR_LOAD(EnumerateViewConfigurationViews)
    SIMQVFR_LOAD(CreateSession)
    SIMQVFR_LOAD(DestroySession)
    SIMQVFR_LOAD(BeginSession)
    SIMQVFR_LOAD(WaitFrame)
    SIMQVFR_LOAD(BeginFrame)
    SIMQVFR_LOAD(CreateSwapchain)
    SIMQVFR_LOAD(EnumerateSwapchainImages)
    SIMQVFR_LOAD(LocateViews)
    SIMQVFR_LOAD(EndFrame)
    SIMQVFR_LOAD(AcquireSwapchainImage)
    SIMQVFR_LOAD(WaitSwapchainImage)
    SIMQVFR_LOAD(ReleaseSwapchainImage)
    SIMQVFR_LOAD(StringToPath)
    SIMQVFR_LOAD(CreateActionSet)
    SIMQVFR_LOAD(CreateAction)
    SIMQVFR_LOAD(SuggestInteractionProfileBindings)
    SIMQVFR_LOAD(AttachSessionActionSets)
    SIMQVFR_LOAD(SyncActions)
    SIMQVFR_LOAD(PollEvent)
    SIMQVFR_LOAD(CreateActionSpace)
    SIMQVFR_LOAD(LocateSpace)
    SIMQVFR_LOAD(CreateReferenceSpace)
    SIMQVFR_LOAD(DestroySpace)
    SIMQVFR_LOAD(DestroySwapchain)
    SIMQVFR_LOAD(GetActionStateBoolean)
    SIMQVFR_LOAD(GetActionStateVector2f)
    SIMQVFR_LOAD(GetActionStatePose)
    SIMQVFR_LOAD(GetCurrentInteractionProfile)
    SIMQVFR_LOAD(PathToString)
#undef SIMQVFR_LOAD
    gipa(*instance, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&d.GetVisibilityMaskKHR));
    if (ok && (experiment.views4 || experiment.measure)) {  // time what runs below this layer (report_cpu_times)
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        cpu.msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
        cpu.realEndFrame = d.EndFrame;
        cpu.realLocateViews = d.LocateViews;
        d.EndFrame = timedEndFrameBelow;
        d.LocateViews = timedLocateViewsBelow;
    }
    next = ok ? d : Dispatch{gipa};
    if (!ok)
        log_line("missing downstream functions; pass-through only");
    else
        gaze_init(*instance);
    return r;
}

}  // namespace simqvfr::layer

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !layerName || !request || std::strcmp(layerName, simqvfr::layer::kLayerName) != 0 ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_API_VERSION_1_0;
    request->getInstanceProcAddr = simqvfr::layer::layerGetInstanceProcAddr;
    request->createApiLayerInstance = simqvfr::layer::layerCreateApiLayerInstance;
    if (simqvfr::layer::is_game_process()) {
        simqvfr::log_line("Loader negotiated API layer interface v1 (Sim-QVFR probe build)");
        simqvfr::layer::note_loader_negotiation();
    }
    return XR_SUCCESS;
}

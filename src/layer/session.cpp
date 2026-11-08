// The session hooks: view configuration, create, destroy, begin, wait and begin frame, and the swapchain
// create, enumerate and acquire hooks.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <dxgi1_4.h>

#include "cheeky/cheeky_host.h"
#include "log.h"
#include "probe/cpu_load.h"
#include "probe/d3d11_capture.h"
#include "probe/d3d12_trace.h"
#include "probe/dred.h"
#include "probe/gpu_frame_timer.h"
#include "probe/gpu_load.h"
#include "probe/hang_watch.h"
#include "probe/pass_census.h"
#include "probe/probe.h"
#include "probe/vram_census.h"
#include "quad/session_graphics.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

XrResult XRAPI_CALL layerEnumerateViewConfigurationViews(XrInstance instance, XrSystemId system,
                                                         XrViewConfigurationType type, uint32_t capacity,
                                                         uint32_t* countOutput, XrViewConfigurationView* views) {
    XrResult r = next.EnumerateViewConfigurationViews(instance, system, type, capacity, countOutput, views);
    const bool stereo = XR_SUCCEEDED(r) && views && countOutput && *countOutput > 0 && capacity > 0 &&
                        type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (stereo) {
        std::lock_guard guard(quad.mutex);
        quad.nativeW = views[0].recommendedImageRectWidth;
        quad.nativeH = views[0].recommendedImageRectHeight;
        runtime_info(L"recommended_width", std::to_wstring(views[0].recommendedImageRectWidth));
        runtime_info(L"recommended_height", std::to_wstring(views[0].recommendedImageRectHeight));
        runtime_info(L"max_width", std::to_wstring(views[0].maxImageRectWidth));
        runtime_info(L"max_height", std::to_wstring(views[0].maxImageRectHeight));
    }
    // views4: ACE's "eye" holds one outer and one focus view side by side (views4_layout.h), so its
    // double-wide target holds all four.
    const Views4Layout& layout = experiment.views4Layout;
    const bool columns = experiment.views4 && game->qvfr_eye_columns();  // eye columns: views4_rect_eye_columns
    // One view size (qvfr_one_view_size): every view the larger of the outer and focus sizes, the column two high.
    const bool oneSize = columns && game->qvfr_one_view_size();
    const double viewWidth = std::max(layout.outer_width(), layout.focus);
    const double viewHeight = std::max(layout.outer_height(), layout.focusHeight);
    const double widthScale = experiment.views4 ? (oneSize ? viewWidth : columns ? layout.column_eye_width() : layout.eye_width())
                                                : experiment.eyeScale;
    const double heightScale = experiment.views4
                                   ? (oneSize ? 2 * viewHeight : columns ? layout.column_eye_height() : layout.eye_height())
                                   : experiment.eyeScale;
    if (oneSize && stereo && (layout.outer_width() != layout.focus || layout.outer_height() != layout.focusHeight))
        logf("views4: the game draws every view at one size: %.2f x %.2f of the native eye (outer asked %.2f x %.2f, "
             "focus %.2f x %.2f)", viewWidth, viewHeight, layout.outer_width(), layout.outer_height(), layout.focus,
             layout.focusHeight);
    if (XR_SUCCEEDED(r) && views && countOutput && (experiment.eyeScale != 1.0 || experiment.views4))
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i) {
            // ACE's swapchain is double-wide, so twice the width must stay within the runtime's maximum.
            const uint32_t maxW = views[i].maxImageRectWidth ? std::max(64u, views[i].maxImageRectWidth / 2) : UINT32_MAX;
            const uint32_t maxH = views[i].maxImageRectHeight ? std::max(64u, views[i].maxImageRectHeight) : UINT32_MAX;
            const auto wanted = static_cast<uint32_t>(views[i].recommendedImageRectWidth * widthScale);
            if (experiment.views4 && wanted > maxW)
                logf("views4: eye width %u exceeds the runtime maximum %u; every view loses resolution, lower "
                     "periphery_scale or focus_density", wanted, maxW);
            views[i].recommendedImageRectWidth = std::clamp(wanted, 64u, maxW);
            views[i].recommendedImageRectHeight = std::clamp(
                static_cast<uint32_t>(views[i].recommendedImageRectHeight * heightScale), 64u, maxH);
        }
    if (stereo) {
        std::lock_guard guard(quad.mutex);
        quad.reportedW = views[0].recommendedImageRectWidth;
        quad.reportedH = views[0].recommendedImageRectHeight;
    }
    if (XR_SUCCEEDED(r) && views && countOutput)
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
            logf("xrEnumerateViewConfigurationViews type=%d view=%u recommended=%ux%u max=%ux%u samples=%u", type, i,
                 views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight, views[i].maxImageRectWidth,
                 views[i].maxImageRectHeight, views[i].recommendedSwapchainSampleCount);
    return r;
}

XrResult XRAPI_CALL layerCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
    const auto* binding = static_cast<const XrBaseInStructure*>(info->next);
    game->session_running(false);  // no views until it begins
    XrResult r = next.CreateSession(instance, info, session);
    logf("xrCreateSession result=%d bindingType=%d", r, binding ? binding->type : 0);
    if (XR_SUCCEEDED(r)) {
        {
            std::lock_guard guard(quad.mutex);
            sessionGraphics = session_graphics(binding);
        }
        hang_watch_arm(experiment.hangWatchSeconds);
        game->session_created(experiment.probes && probe_start());
        if (!experiment.probes)
            log_line(experiment.views3 ? "engine probes and D3D12 trace off (probes=0); views3 cannot report the view mode"
                                       : "engine probes and D3D12 trace off (probes=0)");
        if (binding && binding->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            ID3D11Device* device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(binding)->device;
            d3d11_capture_start(device, experiment.d3d11CaptureFrame);
            if (experiment.views4ComposeD3D11)
                game->d3d11_session(device);
            cheeky_session_d3d11(device);  // when Cheeky runs in this game (loads_cheeky)
        }
        if (binding && binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto* d3d12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding);
            if (experiment.probes)
                d3d12_trace_start(d3d12->device);
            if (experiment.views4)  // before ACE makes its renderer: what VRAM follows the resolution
                vram_census_start(d3d12->device);
            if (experiment.dred)
                dred_watch(d3d12->device);
            if ((experiment.passCensus || experiment.uavBarriers) && pass_census_start(d3d12->device)) {
                pass_census_set_uav_barriers(experiment.uavBarriers);
                if (experiment.uavBarriers)
                    log_line("pass census: a UAV barrier follows every dispatch (pass_census_uav_barriers)");
                if (experiment.passCensus) {
                    pass_census_watch(experiment.passCensusWatch);
                    pass_census_arm(experiment.passCensusFrame, 10);
                }
            }
            if (experiment.d3d12Debug)
                d3d12_debug_attach(d3d12->device);
            std::lock_guard guard(quad.mutex);
            quad.device = d3d12->device;
            quad.queue = d3d12->queue;
            if (experiment.views4 && experiment.gpuFrameTiming)
                gpu_frame_timer_start(d3d12->device, d3d12->queue);
            if (experiment.views4 || experiment.measure) {  // the whole GPU's and ACE's threads' load (the overlay, the log)
                const LUID adapter = d3d12->device->GetAdapterLuid();
                gpu_load_start(static_cast<uint32_t>(adapter.HighPart), adapter.LowPart);
                cpu_load_start();
            }
            cheeky_session(d3d12->device, d3d12->queue);
        } else if (experiment.quad) {
            log_line("quad: session is not D3D12 (a layer above converted it?); quad pipeline off");
        }
    }
    return r;
}

XrResult XRAPI_CALL layerDestroySession(XrSession session) {
    game->session_running(false);
    hang_watch_disarm();
    ini_worker_sync();  // the menu's and hotkeys' last changes reach the ini
    menu_shutdown();  // its swapchain and space belong to the session
    osd_shutdown();
    probe_stop();
    game->session_destroyed();
    dred_stop();
    d3d11_capture_stop();
    {
        std::lock_guard guard(v4d3d11.mutex);
        v4d3d11.compositor.shutdown();
        v4d3d11.chains.clear();
        v4d3d11.outChain = XR_NULL_HANDLE;  // the session's: destroyed with it
        v4d3d11.outImages.clear();
        v4d3d11.failed = false;
    }
    {
        std::lock_guard guard(gaze.mutex);
        if (gaze.queries > 900 && gaze.tracked == 0)  // asked for ~10 s, never tracked
            runtime_info(L"eye_tracking", L"0");
    }
    {
        std::lock_guard guard(quad.mutex);
        quad.v4Composed = quad.v4FovValid = false;
    }
    {
        std::lock_guard guard(v4outline.mutex);
        v4outline.outline.shutdown();
        v4outline.ready = v4outline.failed = v4outline.fovValid = false;
        v4outline.chain = XR_NULL_HANDLE;
        v4outline.images.clear();
        v4outline.acquired.clear();
    }
    {
        std::lock_guard guard(quad.mutex);
        quad.compositor.shutdown();
        quad.appChain = quad.outChain = XR_NULL_HANDLE;
        quad.appImages.clear();
        quad.outImages.clear();
        quad.outerCaptured = quad.aViewsValid = quad.releaseHeld = false;
        quad.device = nullptr;
        quad.queue = nullptr;
        sessionGraphics = {};
        // The helmet's layer goes with the session; its pieces load again into the next one's compositor.
        helmet_layer_release();
        quad.helmetLayerFailed = false;
        quad.helmetBandsLogged.clear();
        for (int p = 0; p < 3; ++p) {
            quad.helmetPiecesTried[p].clear();
            quad.helmetPiecesWritten[p] = {};
        }
        quad.helmetHinted.clear();
        quad.helmetPiecesLoaded = false;
    }
    gpu_frame_timer_stop();
    gpu_load_stop();
    cpu_load_stop();
    pad_watch_stop();
    gaze_thread_stop();  // before the spaces it locates go
    {
        std::lock_guard guard(gaze.mutex);  // the session's spaces and attachment go with it
        gaze.space = gaze.gameSpace = XR_NULL_HANDLE;
        gaze.attached = false;
        gaze.appSyncs = 0;
    }
    cheeky_session(nullptr, nullptr);
    log_line("xrDestroySession");
    return next.DestroySession(session);
}

XrResult XRAPI_CALL layerWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    if (!experiment.twoLoops) {
        const int64_t t0 = qpc_now();
        const XrResult r = next.WaitFrame(session, info, state);
        if (XR_SUCCEEDED(r)) {
            osd_waited(state->predictedDisplayPeriod);
            const double waitMs = (qpc_now() - t0) * cpu.msPerTick;
            if ((experiment.views4 || experiment.measure) && pacing.wait.size() < 4 * kCpuReportFrames) {
                pacing.wait.push_back(static_cast<float>(waitMs));
                pacing.periodMs = state->predictedDisplayPeriod / 1e6;
            }
            osd_wait_returned(waitMs);
        }
        return r;
    }
    std::unique_lock lock(loops.mutex);
    if (!loops.active && !loops.broken && loops.waits >= kArmAfterFrames && loops.waits == loops.begins &&
        loops.begins == loops.ends && (!experiment.quad || quad_ready())) {
        loops.active = true;
        loops.pairedWaits = loops.pairedBegins = loops.pairedEnds = 0;
        QueryPerformanceFrequency(&loops.frequency);
        QueryPerformanceCounter(&loops.lastReport);
        logf("two_loops: ARMED after %llu frames; ACE now runs two frames (A, B) per display frame",
             static_cast<unsigned long long>(loops.waits));
    }
    if (!loops.active) {
        lock.unlock();
        const XrResult r = next.WaitFrame(session, info, state);
        lock.lock();
        if (++loops.waits == 20 * kArmAfterFrames && !loops.broken)
            log_line(experiment.quad && !quad_ready()
                         ? "two_loops: not armed: ACE's eye swapchain was not adopted (see the quad: lines above); "
                           "ACE runs unchanged"
                         : "two_loops: not armed yet: no idle point between frames (is ACE's option to overlap the "
                           "OpenXR frame wait with the previous frame on?)");
        return r;
    }
    const uint64_t index = loops.pairedWaits++;
    if (index % 2 == 1) {  // B: same display time and pose as A, no real wait
        loops.inB = true;
        state->predictedDisplayTime = loops.pendingState.predictedDisplayTime;
        state->predictedDisplayPeriod = loops.pendingState.predictedDisplayPeriod;
        state->shouldRender = loops.pendingState.shouldRender;
        QueryPerformanceCounter(&loops.bWaitReturned);
        return XR_SUCCESS;
    }
    lock.unlock();  // never hold the lock across the runtime's blocking wait
    const XrResult r = next.WaitFrame(session, info, state);
    lock.lock();
    loops.pendingState.predictedDisplayTime = state->predictedDisplayTime;
    loops.pendingState.predictedDisplayPeriod = state->predictedDisplayPeriod;
    loops.pendingState.shouldRender = state->shouldRender;
    QueryPerformanceCounter(&loops.aWaitReturned);
    if (XR_SUCCEEDED(r))
        osd_waited(state->predictedDisplayPeriod / 2);  // two ACE frames per display frame
    return r;
}

XrResult XRAPI_CALL layerBeginFrame(XrSession session, const XrFrameBeginInfo* info) {
    if (!experiment.twoLoops) {
        const XrResult r = next.BeginFrame(session, info);
        if (XR_SUCCEEDED(r))
            gpu_frame_timer_mark(GpuMark::kBegin);
        return r;
    }
    std::unique_lock lock(loops.mutex);
    if (!loops.active) {
        ++loops.begins;
        lock.unlock();
        return next.BeginFrame(session, info);
    }
    const uint64_t index = loops.pairedBegins++;
    lock.unlock();
    return index % 2 == 0 ? next.BeginFrame(session, info) : XR_SUCCESS;  // B's frame is already begun
}

XrResult XRAPI_CALL layerBeginSession(XrSession session, const XrSessionBeginInfo* info) {
    XrResult r = next.BeginSession(session, info);
    logf("xrBeginSession viewConfig=%d result=%d", info->primaryViewConfigurationType, r);
    if (XR_SUCCEEDED(r))
        game->session_running(true);
    return r;
}

namespace {

// Takes ACE's double-wide eye swapchain as the quad input and creates the output swapchain.
void quad_adopt_chain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain chain) {
    std::lock_guard guard(quad.mutex);
    if (quad.appChain || !quad.device || !quad.nativeW || !quad.reportedH || !info->height)
        return;
    // The double-wide eye target, matched by shape rather than exact width: ACE's own VR resolution
    // setting scales it from what it was offered. The compositor reads the image rects ACE submits.
    const double wanted = 2.0 * quad.reportedW / quad.reportedH;
    const double aspect = static_cast<double>(info->width) / info->height;
    if (std::abs(aspect / wanted - 1) > 0.03) {
        logf("quad: %ux%u is not ACE's double-wide eye target (offered %ux%u per eye); not adopted", info->width,
             info->height, quad.reportedW, quad.reportedH);
        return;
    }
    if (info->arraySize != 1 || info->sampleCount != 1) {
        logf("quad: eye target %ux%u has array=%u samples=%u, which the compositor cannot read; ACE runs unchanged",
             info->width, info->height, info->arraySize, info->sampleCount);
        return;
    }
    if (!quad.compositor.initialize(quad.device, quad.queue, static_cast<DXGI_FORMAT>(info->format))) {
        log_line("quad: compositor unavailable; ACE runs unchanged");
        return;
    }
    XrSwapchainCreateInfo out = *info;
    out.next = nullptr;
    out.width = 2 * quad.nativeW;
    out.height = quad.nativeH;
    out.mipCount = 1;
    out.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchain outChain = XR_NULL_HANDLE;
    XrResult r = next.CreateSwapchain(session, &out, &outChain);
    uint32_t count = 0;
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(outChain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_SUCCEEDED(r))
        r = next.EnumerateSwapchainImages(outChain, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(r)) {
        logf("quad: output swapchain %ux%u failed (%d); quad pipeline off", out.width, out.height, r);
        return;
    }
    quad.appChain = chain;
    quad.appW = info->width;
    quad.appH = info->height;
    quad.format = static_cast<DXGI_FORMAT>(info->format);
    quad.outChain = outChain;
    for (const auto& image : images)
        quad.outImages.push_back(image.texture);
    logf("quad: ACE eye chain %p %ux%u (%.2fx the %ux%u offered: ACE's own resolution setting) -> output chain %p "
         "%ux%u (%u images), focus %.0f%% of the FOV, feather %.0f%%",
         static_cast<void*>(chain), info->width, info->height, info->height / static_cast<double>(quad.reportedH),
         quad.reportedW, quad.reportedH, static_cast<void*>(outChain), out.width, out.height, count,
         experiment.focusFraction * 100, experiment.feather * 100);
}

}  // namespace

XrResult XRAPI_CALL layerCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    XrResult r = next.CreateSwapchain(session, info, swapchain);
    logf("xrCreateSwapchain %ux%u array=%u faces=%u mips=%u samples=%u format=%lld usage=0x%llx flags=0x%llx "
         "result=%d handle=%p",
         info->width, info->height, info->arraySize, info->faceCount, info->mipCount, info->sampleCount,
         static_cast<long long>(info->format), static_cast<unsigned long long>(info->usageFlags),
         static_cast<unsigned long long>(info->createFlags), r, XR_SUCCEEDED(r) ? static_cast<void*>(*swapchain) : nullptr);
    if (XR_SUCCEEDED(r)) {
        std::lock_guard guard(chainMutex);
        chainSizes[*swapchain] = {info->width, info->height};
    }
    if (XR_SUCCEEDED(r) && views4_outline_on()) {
        std::lock_guard guard(v4outline.mutex);
        v4outline.formats[*swapchain] = info->format;
    }
    if (XR_SUCCEEDED(r) && experiment.views4ComposeD3D11) {
        std::lock_guard guard(v4d3d11.mutex);
        v4d3d11.chains[*swapchain].format = info->format;
    }
    if (XR_SUCCEEDED(r) && (experiment.quad || experiment.views4Compose) &&
        (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
        quad_adopt_chain(session, info, *swapchain);
    return r;
}

XrResult XRAPI_CALL layerEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* countOutput,
                                                  XrSwapchainImageBaseHeader* images) {
    XrResult r = next.EnumerateSwapchainImages(swapchain, capacity, countOutput, images);
    if (XR_SUCCEEDED(r) && capacity > 0 && images && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR &&
        experiment.d3d11CaptureFrame)
        d3d11_capture_images(swapchain, reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images),
                             std::min(*countOutput, capacity));
    if (XR_SUCCEEDED(r) && capacity > 0 && images && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR &&
        experiment.views4ComposeD3D11) {
        std::lock_guard guard(v4d3d11.mutex);
        if (auto it = v4d3d11.chains.find(swapchain); it != v4d3d11.chains.end()) {
            const auto* d3d11Images = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
            it->second.images.clear();
            for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
                it->second.images.push_back(d3d11Images[i].texture);
        }
    }
    if (XR_FAILED(r) || capacity == 0 || !images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)
        return r;
    std::pair<uint32_t, uint32_t> size{0, 0};
    {
        std::lock_guard guard(chainMutex);
        if (auto it = chainSizes.find(swapchain); it != chainSizes.end())
            size = it->second;
    }
    auto* d3d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
        d3d12_trace_register_image(d3d12Images[i].texture, size.first, size.second, i);
    {
        std::lock_guard guard(quad.mutex);
        if (swapchain == quad.appChain) {
            quad.appImages.clear();
            for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
                quad.appImages.push_back(d3d12Images[i].texture);
        }
    }
    if (views4_outline_on()) {
        std::lock_guard guard(v4outline.mutex);
        auto& list = v4outline.images[swapchain];
        list.clear();
        for (uint32_t i = 0; i < *countOutput && i < capacity; ++i)
            list.push_back(d3d12Images[i].texture);
    }
    logf("xrEnumerateSwapchainImages chain=%p %ux%u images=%u registered for D3D12 tracing",
         static_cast<void*>(swapchain), size.first, size.second, *countOutput);
    return r;
}

XrResult XRAPI_CALL layerAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info,
                                               uint32_t* index) {
    const XrResult r = next.AcquireSwapchainImage(swapchain, info, index);
    if (XR_SUCCEEDED(r) && experiment.d3d11CaptureFrame)
        d3d11_capture_acquired(swapchain, *index);
    if (XR_SUCCEEDED(r) && experiment.views4ComposeD3D11) {
        std::lock_guard guard(v4d3d11.mutex);
        if (auto it = v4d3d11.chains.find(swapchain); it != v4d3d11.chains.end())
            it->second.acquired = *index;
    }
    if (XR_SUCCEEDED(r) && views4_outline_on()) {
        std::lock_guard guard(v4outline.mutex);
        v4outline.acquired[swapchain] = *index;
    }
    if (XR_SUCCEEDED(r) && (experiment.quad || experiment.views4Compose)) {
        std::lock_guard guard(quad.mutex);
        if (swapchain == quad.appChain)
            quad.appAcquired = *index;
    }
    return r;
}

}  // namespace simqvfr::layer

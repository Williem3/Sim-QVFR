// xrEndFrame: the hook and end_frame, which hands each frame to the pipeline in use.
#include <mutex>
#include <vector>

#include "cheeky/cheeky_host.h"
#include "log.h"
#include "probe/cpu_load.h"
#include "probe/cpu_placement.h"
#include "probe/cpu_isolation.h"
#include "probe/cpu_profile.h"
#include "probe/frame_parts.h"
#include "probe/d3d12_trace.h"
#include "probe/hang_watch.h"
#include "probe/pass_census.h"
#include "games/ace/replay_probe.h"
#include "probe/pix_capture.h"
#include "probe/probe.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

XrResult XRAPI_CALL layerEndFrame(XrSession session, const XrFrameEndInfo* info) {
    turbo_frame_ended();
    hang_watch_frame();
    cpu_profile_mark_frame();  // the game's frame ends here: a running capture's samples go to their frames
    cpu_load_render_thread(GetCurrentThreadId());
    cpu_placement_frame();  // which kind of core the render thread is on (read only)
    cpu_isolation_frame();  // [experiment] cpu_isolate: the render thread's core to itself, or the A/B's side
    cpu_profile_poll();
    frame_capture_poll();
    if ((experiment.views4 || experiment.measure) && cpu.msPerTick > 0) {
        const int64_t now = qpc_now();
        if (pacing.lastEnd && pacing.interval.size() < 4 * kCpuReportFrames)
            pacing.interval.push_back(static_cast<float>((now - pacing.lastEnd) * cpu.msPerTick));
        pacing.lastEnd = now;
    }
    osd_measure(info ? info->displayTime : 0);  // before the layer's own work in the frame
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

XrResult end_frame(XrSession session, const XrFrameEndInfo* frameInfo) {
    XrFrameEndInfo withMenu{};
    std::vector<const XrCompositionLayerBaseHeader*> menuLayers;
    hang_watch_stage("overlay and menu: before");
    const XrFrameEndInfo* info;
    {
        FramePartTimer part(FramePart::menu);
        info = menu_frame(session, frameInfo, withMenu, menuLayers);
    }
    hang_watch_stage("overlay and menu: done");
    ++endFrames;
    // The API path's gaze: ACE attaches its action sets (and the layer's gaze with them) at start-up, but not on every
    // launch (the 18:38 run: none in over two minutes); five seconds in, the layer attaches its own.
    if (experiment.replayCompose && endFrames == 450)
        gaze_attach_alone(session);
    if (experiment.pixCaptureFrame || experiment.pixCaptureHotkey)
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
    if (experiment.replayProbe)
        replay_probe_frame_end();
    {
        FramePartTimer part(FramePart::gameEnd);
        game->frame_end();
    }
    if (experiment.views4) {
        {
            FramePartTimer part(FramePart::qvfrEnd);
            game->qvfr_frame_end();
        }
        FramePartTimer part(FramePart::submit);
        return views4_submit(session, info);
    }
    if (experiment.replayCompose)  // P3: the composed output in place of ACE's eyes (views4's submission)
        return views4_submit(session, info);
    if (experiment.measure && experiment.gpuFrameTiming && !measuredEyeChain.load(std::memory_order_relaxed))
        for (uint32_t i = 0; i < info->layerCount; ++i)
            if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
                if (proj->viewCount && proj->views[0].subImage.swapchain) {
                    measuredEyeChain = proj->views[0].subImage.swapchain;
                    logf("views4 gpu: baseline: ACE's frame ends at the release of swapchain %p (its projection layer)",
                         static_cast<void*>(proj->views[0].subImage.swapchain));
                }
                break;
            }
    const XrFrameEndInfo* adjusted = game->adjust_frame_end(info, next.EnumerateSwapchainImages);
    hang_watch_stage("the runtime's xrEndFrame");
    const XrResult result = next.EndFrame(session, adjusted);
    hang_watch_stage("after xrEndFrame");
    return result;
}

}  // namespace simqvfr::layer

// ACC's mesh passes per view (docs/recon-acc.md "Engine map: mesh passes"): a probe of UE's mesh pass setup
// (FParallelMeshDrawCommandPass::DispatchPassSetup) and dispatch (DispatchDraw) per view and pass type, the render
// thread's scene render (RenderViewFamily_RenderThread) and InitViews, for the draw replay's measure (MVR track M0
// for an instanced-stereo engine): what the focus pair's mesh passes cost the render thread.
//  - acc_pass_probe=1: log only. Every 900 frames: per view (stereo pass) and pass type, draws set up and the render
//    thread's time in DispatchDraw; the scene render and InitViews per frame; ComputeViewVisibility with each view's
//    steps (precomputed visibility, the culling loops, occlusion, relevance, mesh pass setup) and the dynamic mesh
//    gather (abi.h [Visibility]).
//  - acc_focus_skip_ab=N: an interleaved A/B, N main-family frames each way: B gives the focus pair's non-translucent
//    mesh passes no draws (their setup gets nothing to build, their dispatch nothing to draw), so the focus views show
//    no opaque scene in B frames (diagnostic only). The report compares the render thread's scene render per state:
//    the ceiling of what drawing the focus pair from the eye pair's draws can save.
//  - acc_gather_skip_ab=N: the same A/B for GatherDynamicMeshElements: B clears the focus views' bits (views 2-3:
//    0x4, 0x8) in its per-primitive view masks, so the focus views get no dynamic meshes (cars, hands, the wheel
//    vanish from the box in B frames: diagnostic only). The report compares the gather per state: its focus share.
//    Either A/B, not both (acc_focus_skip_ab wins).
// Hooks (MinHook) only when a key is on; their anchors are abi.h [Mesh passes] and [Visibility].
#pragma once

namespace acc {

struct PassProbeConfig {
    bool probe = false;
    int skipAbFrames = 0;
    int gatherAbFrames = 0;
    bool on() const { return probe || skipAbFrames > 0 || gatherAbFrames > 0; }
    int ab_frames() const { return skipAbFrames > 0 ? skipAbFrames : gatherAbFrames; }
    bool gather_ab() const { return skipAbFrames <= 0 && gatherAbFrames > 0; }
};
// The focus views' bits in a primitive's view mask (ComputeViewVisibility's ViewBit: 1 << view index).
inline constexpr unsigned char kFocusViewBits = 0x4 | 0x8;
PassProbeConfig pass_probe_config(const wchar_t* ini);
// Hooks the four functions on this build (the anchors checked); false and a log line otherwise.
bool pass_probe_start(const PassProbeConfig& config);
// Every xrEndFrame: the report every 900 frames.
void pass_probe_frame_end();

// Whether an EMeshPass is one of the translucency passes (ACC's DispatchPassSetup maps 9-12 and 17 to its
// translucency pass index): those keep their own draws in the A/B (their pixel shaders read the view's own depth).
inline bool translucency_pass(int type) { return (type >= 9 && type <= 12) || type == 17; }

}  // namespace acc

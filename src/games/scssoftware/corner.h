// QVFR for SCS Software's games (American Truck Simulator, Euro Truck Simulator 2) at the D3D11 and OpenXR level only: no
// game code hooked, no game memory written. Each eye is drawn into a corner of the game's own targets ([experiment]
// scs_corner, the corner's share of each axis in percent), its focus view into the room beside it in the same pass (by
// NVIDIA's multi-view rendering or by instancing), the full-screen passes replayed for the focus region, and the eye
// scaled back over each headset image at its release. Design and runs: the local recon notes (docs/recon-ats.md).
//
// From an eye pass's first G-buffer bind to the frame's end, on the render thread (the one that bound it):
//   - viewports and scissors are cut to the corner whenever the bound target has the eye's shape (the eye's targets, its
//     half and quarter sizes, the swapchain image), the game's own values kept to put back;
//   - every non-indexed draw (the full-screen passes) into such a target, or into a small one (the histogram's 4x2, the
//     exposure's 1x1), gets its vertex and pixel shaders' constant ranges (slot 0) rewritten for the corner: the game's
//     range copied into a buffer of ours on the GPU, rewritten there by corner_rules.h's compute shader, bound for that
//     draw alone. The game's data never comes back to the CPU.
// As the game releases each swapchain image (xrReleaseSwapchainImage, before the runtime may read it: done at
// xrEndFrame instead, the runtime sometimes read the image first and showed the small corner, "a smaller copy of the
// scene popping in", S1 run 4) it is filled again from its corner, scaled up (a copy of the corner,
// drawn back over the whole image with linear filtering), so the runtime gets a whole image every frame: with only the
// corner submitted (imageRect cut), the Pimax runtime showed the whole image on some frames, the eye shrunk into its
// top-left (S1 runs 2-3: a strobe at the top of the view, "an image popping in and out"). The imageRect cut stays as
// the fallback when an image can't be found. Frames without an eye pass (the menus) are left as the game drew them.
#pragma once

#include "../../xr.h"

#include <cstdint>
#include <vector>

struct ID3D11Device;

namespace scs {

struct CornerStats {
    uint64_t frames = 0, cornerFrames = 0;
    // Non-indexed draws into the eye's targets: constant ranges rewritten by us (which rule applied is the GPU's), or
    // left as the game bound them (nothing of ours to rewrite).
    uint64_t rewritten = 0, notRewritten = 0;
    uint64_t skipped = 0;  // non-indexed draws into other targets (the UI canvases, the desktop mirror)
};

// xrCreateSession: the corner set up on the game's device ([experiment] scs_corner percent), at the session's first
// xrEndFrame (corner_frame_end, on the thread that ends the frames: the install issues commands on the game's
// immediate context, which the game's main thread, where sessions start, mustn't). False: the request refused (no
// device, a share out of range); the install's own outcome is in the log (corner_on() after that xrEndFrame).
// `focusPercent` ([experiment] scs_focus; 0: off): every indexed scene draw of an eye pass (a full-size target with
// depth) also draws that eye's focus view, the middle `focusPercent`% of its field of view on each axis, into the room
// right of the corner: by NVIDIA's multi-view rendering or Sim-QVFR's own (instancing), whichever the device has (the
// game shader's twins: lmu::mvr_probe attaches them at creation), two viewports, the clip remap in vertex constant
// buffer 13 (the game's own put back after each draw). The focus view is composed over the eye's box at each release.
bool corner_install(ID3D11Device* device, int percent, int focusPercent = 0);
// Bisection switches for a GPU hang (before the session; [experiment] keys, all off by default): the focus view's
// scene draws off (`noDraws`), its replayed full-screen passes off (`noReplays`), or the frame's nth replay skipped for
// each bit n-1 of `replaySkip`. The log names each replay of one frame, 300 frames into driving. `gpuTimes`: the replays and
// eye passes timed on the GPU (corner_internal.h's gpu_replay_begin).
// `skipLarge`: the replays with a pixel range over 128 vectors skipped (a light list's).
void corner_set_diag(bool noDraws, bool noReplays, uint64_t replaySkip, bool gpuTimes = false, bool skipLarge = false,
                     bool dumpLarge = false,  // `dumpLarge`: their ranges saved (dev diagnostics)
                     bool rewriteRing = true,  // false: one set of rewrite buffers, reused (the old way, for an A/B)
                     const std::vector<uint64_t>& skipPs = {},  // the replays of these pixel shaders (their ids) skipped
                     const std::vector<uint64_t>& dumpPs = {});  // these pixel shaders' constant ranges saved (diagnostics)
// Dev test of the UI panels' focus redraw (redraw_focus): every draw onto the headset's image takes it, twin or not.
void corner_set_force_ui_redraw(bool on);
// Dev builds' mirror diagnostic ([experiment] scs_mirror_diag=1, corner_mirror_diag.cpp): before the game's shaders.
void corner_mirror_diag(bool on);
// The scene draws' own-pixel-loads twins (lmu::mvr_probe_own_pixel_loads): bound for their focus draws. Before the install.
void corner_own_pixel_loads(bool on);
bool corner_on();
// Every xrEndFrame (any thread): `info` with each projection view's image rectangle cut to the corner when the frame
// just drawn had its eyes in it; `info` itself otherwise. The copies live until the next call.
// `enumerate`: the next layer's xrEnumerateSwapchainImages (null: the imageRect cut alone).
const XrFrameEndInfo* corner_frame_end(const XrFrameEndInfo* info, PFN_xrEnumerateSwapchainImages enumerate = nullptr);
// xrAcquireSwapchainImage: which image of `chain` the game draws into (its images from `enumerate`, once).
void corner_acquired(XrSwapchain chain, uint32_t index, PFN_xrEnumerateSwapchainImages enumerate);
// xrReleaseSwapchainImage, before it goes on: the corner scaled back over the image (the render thread, a frame with
// its eyes in the corner).
void corner_releasing(XrSwapchain chain);
// xrDestroySwapchain, before it goes on (any thread): `chain`'s images forgotten (their raw pointers), learned again if
// the handle comes back.
void corner_swapchain_destroyed(XrSwapchain chain);
// Where each eye's focus box should be centred on that eye's screen (x right, y down, 0..1; any thread). Taken at each
// frame's start; the box is kept inside the screen.
void corner_set_gaze(const float (&screen)[2][2]);
// The headset menu's sizes, live (any thread; taken at the next frame's start), in percent: the eye's corner (its
// density), the focus box's width and height as shares of the eye's screen, and the focus view's density (its region
// in the targets is box x density of them). Kept to what fits: box 10..60 x 10..80, the density to the room there is
// (beside a 25% corner, and the targets' height), the corner 25..what the focus region leaves.
void corner_set_live(int percent, int focusPercent, int focusPercentY, int densityPercent);
// How the box sits on the eye (any thread): its blended edge and rounded corners as shares of half its shorter side,
// the outline and its colour (linear RGB).
void corner_set_look(float feather, float cornerRadius, bool border, const float (&color)[4]);
// How the focus view's scene draws go out this frame (any thread; the overlay's badge): 0 no focus view, 1 NVIDIA's
// multi-view rendering, 2 Sim-QVFR's own (instancing), 3 the focus view switched off (dev builds' Ctrl+Alt+G).
int corner_way();
CornerStats corner_stats();
// xrDestroySession (the game's main thread): observing stops at once; everything is released at the next xrEndFrame.
void corner_session_end();

}  // namespace scs

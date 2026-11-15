// ATS's eyes drawn into a corner of the game's own targets, at the D3D11 level only (stage 1 of its API-level QVFR:
// docs/recon-ats.md "Stage 1 design notes"; [experiment] ats_corner = the corner's share of each axis in percent, 0 off).
// The room this leaves beside each eye is where its focus view goes at stage 2.
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

namespace ats {

struct CornerStats {
    uint64_t frames = 0, cornerFrames = 0;
    uint64_t patched[4] = {};  // draws by CornerRule (none, pair, rect, pixelRect), vertex and pixel ranges together
    uint64_t skipped = 0;      // non-indexed draws into other targets (the UI canvases, the desktop mirror)
    uint64_t copyFailures = 0;
};

// xrCreateSession: the corner set up on the game's device ([experiment] ats_corner percent). False: off.
// `focusPercent` ([experiment] ats_focus, stage 2; 0: off): every indexed scene draw of an eye pass (a full-size target
// with depth) also draws that eye's focus view, the middle `focusPercent`% of its field of view on each axis at the
// targets' own density, into the room right of the corner, through NVIDIA's multi-view rendering: the game shader's
// twin (lmu::mvr_probe attaches it at creation), two viewports, the clip remap in vertex constant buffer 13. Nothing
// reads that region yet: Ctrl+Alt+B also saves each eye's first G-buffer target whole, to compare it with the eye.
bool corner_install(ID3D11Device* device, int percent, int focusPercent = 0);
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
// Where each eye's focus box should be centred on that eye's screen (x right, y down, 0..1; any thread). Taken at each
// frame's start; the box is kept inside the screen.
void corner_set_gaze(const float (&screen)[2][2]);
CornerStats corner_stats();
void corner_session_end();

}  // namespace ats

// LMU QVFR: the focus views take their eye's glare (docs/recon-lmu.md "Glare"). YEBIS sizes its glare (bloom, star,
// ghosts) in each view's own pixels: its pyramid runs at 1/5, 1/10 and 1/20 of the view, so in a focus view the halo
// around a light is the box's share of the eye's in angle, and stops at the box edge in the composed image. Here each
// eye pass's glare result is kept, and each focus pass's is replaced by its eye's, resampled through the focus box
// (each focus texel takes the eye's glare in its direction), before YEBIS's tonemap adds it to the scene: the halo is
// the periphery's on both sides of the box edge. [experiment] lmu_glare_share=0 turns it off; with lmu_diag=1,
// Ctrl+Alt+1 switches it live (an A/B in one run).
#pragma once

#include <string>

#include "focus_math.h"

namespace lmu {

// Hooks YEBIS's glare composite on this build (the anchor checked); false and a log line otherwise.
bool glare_share_start();
// On or off from the next pass (any thread). The hook stays; off, it only passes through.
void glare_share_set(bool on);
bool glare_share_on();
// The render thread, around each pass: the pass about to run (0-1 eyes, 2-3 focus views of eyes 0-1) and, for a focus
// pass, its box (null: the pass isn't a focus view this frame, its glare stays its own); -1 after the pass.
// `passW` x `passH` of its `viewW` x `viewH` target: what the pass renders (live resolution; MVR's side by side per axis;
// 0 not known yet), its view's share of YEBIS's glare too (glare_math.h yebis_pass_region). `yebisScale`: the render
// scale its post-processing runs at, over the one the targets were made at (glare_math.h yebis_covers; logged).
void glare_share_pass(int pass, const FocusBox* box, unsigned passW = 0, unsigned passH = 0, unsigned viewW = 0,
                      unsigned viewH = 0, double yebisScale = 0);
// Every frame (counts; a summary line every 900 frames while it works).
void glare_share_frame_end(unsigned long long frame);

// The frame capture hotkey, before pass 0 (render thread): this frame's glare results are read back pass by pass, as
// YEBIS's tonemap reads them (a focus pass's after the share; with the share on, its own before it too), share on or off.
void glare_share_capture_frame();
// After that frame (its views saved as `viewPaths`, eye 0, eye 1, focus 0, focus 1): writes them as
// <dir>\<stem>-glare<pass>.dds (and -glare<pass>-own.dds), and logs each with its region and the
// tools/glare_place.py line that checks where the view's glare is. Nothing if this frame read none.
void glare_share_save_capture(const std::wstring& dir, const std::wstring& stem, const std::wstring (&viewPaths)[4]);

}  // namespace lmu

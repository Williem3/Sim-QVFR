// LMU's four passes (docs/recon-lmu.md "Design notes"): the game's VR eye loop runs four times instead of two. Stage one
// is the copies gate ([experiment] lmu_copies=1): passes 2 and 3 render eyes 0 and 1 again, with the same cameras and
// targets, and their images are compared with the eyes' pixel for pixel. Every difference is per-eye state that a
// focus view would get wrong. It changes what the game renders (four passes, the headset shows passes 2-3's images),
// so it only runs when asked.
#pragma once

namespace lmu {

// A gate run's settings ([experiment] keys, lmu/adapter.cpp).
struct FourPassesRun {
    bool focus = false;          // passes 2-3 are focus views (else copies of the eyes)
    bool holdExposure = true;    // passes 2-3 render with YEBIS's frame time held near zero
    double fraction = 0.3;       // the focus box: share of each eye's tangent span, centred
    int first = 300;             // scene frames before the first sample
    int every = 300;             // scene frames between samples
    int count = 3;               // samples
    int warmup = 60;             // four-pass frames before each sample (else two: full frame rate)
    bool exitWhenDone = false;   // ask the game to close after the last sample (probe/run_exit.h)
};

// Installs the four passes on this build (hash and anchors checked); false and a log line otherwise. Focus views render
// into the eye targets and are set aside before the game submits, so the headset shows the eyes.
bool four_passes_start(const FourPassesRun& run);
// Every xrEndFrame: counts the passes, and at a few frames on track compares passes 2-3 with eyes 0-1 (copies: pixel
// for pixel; focus: each focus view with its eye's crop).
void four_passes_frame_end();

}  // namespace lmu

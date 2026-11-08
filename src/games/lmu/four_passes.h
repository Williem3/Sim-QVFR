// LMU's four passes (docs/recon-lmu.md "Design notes"): the game's VR eye loop runs four times instead of two. Stage one
// is the copies gate ([experiment] lmu_copies=1): passes 2 and 3 render eyes 0 and 1 again, with the same cameras and
// targets, and their images are compared with the eyes' pixel for pixel. Every difference is per-eye state that a
// focus view would get wrong. It changes what the game renders (four passes, the headset shows passes 2-3's images),
// so it only runs when asked.
#pragma once

namespace lmu {

// Installs the four passes as copies on this build (hash and anchors checked); false and a log line otherwise.
// `holdExposure`: passes 2-3 render with YEBIS's frame time held near zero, so they don't advance auto exposure again.
bool four_passes_start_copies(bool holdExposure);
// Every xrEndFrame: counts the passes, and at a few frames on track compares passes 2-3 with eyes 0-1.
void four_passes_frame_end();

}  // namespace lmu

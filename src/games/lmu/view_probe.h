// LMU's view probe ([experiment] lmu_view_probe=1): log only, nothing the game renders changes. It records which
// pipeline views the renderer draws each frame, their sub-views, and every view-params call with its eye index, to
// tell whether the two eyes are two pipeline views or two sub-views of one (docs/recon-lmu.md "Engine map").
#pragma once

namespace lmu {

// Installs the probe on this build (hashes and anchors checked); false and a log line if anything doesn't match.
bool view_probe_start();
// Every xrEndFrame: closes the frame's record; logs a few sample frames in full and a summary every 1800 frames.
void view_probe_frame_end();

}  // namespace lmu

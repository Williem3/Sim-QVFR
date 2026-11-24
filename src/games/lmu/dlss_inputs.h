// LMU's inputs to the engine's DLAA (engine/dlss_views.h): each eye pass's view matrix and projection, from what the
// pass's draws upload (view_pair.h), on every build; nothing is read in the game. The rest the engine takes from the
// D3D11 and OpenXR calls; mvr_pass.cpp calls it at the eye passes.
#pragma once

namespace lmu {

// The engine's matrices callback set to LMU's (simqvfr::dlss_set_matrices), before simqvfr::dlss_start.
void dlss_use_lmu_inputs();
// The eye pass's projection as DLSS takes it (the pass's draws'; the ordinary depth convention, depth terms included),
// for the focus boxes' convergence (fixation.h). During the pass.
bool lmu_pass_projection(int eye, float (&projection)[16]);

// Eye pass `eye`'s camera for night car lighting: its view and view x projection (row vectors, reversed depth: as the
// draws upload them and the depth target holds it, not the eye slot's ordinary projection), the drawn pairs' as DLSS
// takes them, strictly: none when several have the eye's FOV and none the world's draws (DLSS's own choice unchanged;
// the drawn pairs alone took another camera with the eye's FOV in ~1 pass in 10 in replay, 2026-11-19); none found:
// the eye's last within 50 ms. False: none at all.
bool lmu_pass_camera(int eye, float (&view)[16], float (&viewProj)[16]);

}  // namespace lmu

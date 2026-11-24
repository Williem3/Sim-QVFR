// LMU's inputs to the engine's DLAA (engine/dlss_views.h): each eye pass's view matrix and projection, from what the
// pass's draws upload (view_pair.h; the default on every build), or read from the game's renderer on the known build
// with lmu_build_free=0 (abi.h; docs/recon-lmu.md "DLSS for LMU at the API level"). The rest the engine takes from the
// D3D11 and OpenXR calls; mvr_pass.cpp calls it at the eye passes.
#pragma once

namespace lmu {

// The engine's matrices callback set to LMU's (simqvfr::dlss_set_matrices), before simqvfr::dlss_start.
void dlss_use_lmu_inputs();
// The eye pass's projection as DLSS takes it (the pass's draws'; the renderer's on a known build with lmu_build_free=0;
// the ordinary depth convention, depth terms included), for the focus boxes' convergence (fixation.h). During the pass.
bool lmu_pass_projection(int eye, float (&projection)[16]);

// Eye pass `eye`'s camera for night car lighting: its view and view x projection (row vectors, reversed depth: as the
// draws upload them and the depth target holds it, not the eye slot's ordinary projection). A known build with
// lmu_build_free=0: the renderer's view params (read only) while they are that eye's, both eyes of a frame from the
// source the first eye took; else (the default, every build) the drawn pairs' as DLSS takes them (none when several
// have the eye's FOV and none the world's draws; DLSS's own choice unchanged); none found: the eye's last within 50 ms.
// False: none at all. The drawn pairs alone took another camera with the eye's FOV in ~1 pass in 10 in replay
// (2026-11-19: up to 16 such, none with world draws), so one eye was relit from elsewhere.
bool lmu_pass_camera(int eye, float (&view)[16], float (&viewProj)[16]);

}  // namespace lmu

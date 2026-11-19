// LMU's inputs to the engine's DLAA (engine/dlss_views.h): each eye pass's view matrix and projection, read from the
// game's renderer (abi.h; docs/recon-lmu.md "DLSS for LMU at the API level"). The rest the engine takes from the D3D11
// and OpenXR calls; mvr_pass.cpp calls it at the eye passes.
#pragma once

namespace lmu {

// The engine's matrices callback set to LMU's (simqvfr::dlss_set_matrices), before simqvfr::dlss_start.
void dlss_use_lmu_inputs();

// Eye pass `eye`'s camera for night car lighting: its view and view x projection (row vectors). A known build: the
// renderer's view params (read only) while they are that eye's; else, and on any build, the drawn pairs' as DLSS takes
// them (none when several have the eye's FOV and none the world's draws); none found: the eye's from up to 8 calls
// back. False: none at all. The drawn pairs alone took another camera with the eye's FOV in ~1 pass in 10 in replay
// (2026-11-19: up to 16 such, none with world draws), so one eye was relit from elsewhere.
bool lmu_pass_camera(int eye, float (&view)[16], float (&viewProj)[16]);

}  // namespace lmu

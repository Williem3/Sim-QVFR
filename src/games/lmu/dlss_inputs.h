// LMU's inputs to the engine's DLAA (engine/dlss_views.h): each eye pass's view matrix and projection, read from the
// game's renderer (abi.h; docs/recon-lmu.md "DLSS for LMU at the API level"). The rest the engine takes from the D3D11
// and OpenXR calls; mvr_pass.cpp calls it at the eye passes.
#pragma once

namespace lmu {

// The engine's matrices callback set to LMU's (simqvfr::dlss_set_matrices), before simqvfr::dlss_start.
void dlss_use_lmu_inputs();

}  // namespace lmu

// NVIDIA multi-view rendering for this run: each eye's pass draws its focus view too (LMU: games/lmu/mvr_pass.h), so
// the game prepares its scene for two passes instead of four. The launcher's [experiment] multi_view=1, for a game that
// has it (GameFeatures::multiView), on a machine with an NVIDIA GPU of the RTX 20 series or newer (engine/nvapi_gpus.h).
// The game's own device is checked again when it's made (a GPU without it there: the four passes as before).
#pragma once

#include <string>

#include "game.h"

namespace simqvfr::games {

// On for this run; `why`: the GPU it runs on ("NVIDIA GeForce RTX 5070 Ti"), or why not.
bool multi_view_on(const GameInfo* game, const wchar_t* ini, std::string* why = nullptr);
// [experiment] lmu_mvr (LMU's MVR gates, 1 or 2) on a GPU that offers multi-view rendering; 0 on any other GPU.
unsigned lmu_mvr_gate(const wchar_t* ini);

}  // namespace simqvfr::games

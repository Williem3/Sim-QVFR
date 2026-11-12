// Multi-view rendering for this run: each eye's pass draws its focus view too (LMU: games/lmu/mvr_pass.h), so the game
// runs only its own two eye passes. Always on for a game that has it (GameFeatures::multiView; no option since
// 2026-11-12, when LMU's four passes went), on any GPU: NVIDIA's multi-view rendering on an RTX 20 series or newer
// (engine/nvapi_gpus.h), Sim-QVFR's own (instancing) elsewhere. The game's own device decides when it's made (a GPU
// that can do neither: the eyes alone, without focus views).
#pragma once

#include <string>

#include "game.h"

namespace simqvfr::games {

// On for this run; `why`: the GPU and the way it is expected to draw ("NVIDIA GeForce RTX 5070 Ti: NVIDIA's multi-view
// rendering"), or why not.
bool multi_view_on(const GameInfo* game, const wchar_t* ini, std::string* why = nullptr);

}  // namespace simqvfr::games

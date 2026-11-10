// Multi-view rendering for this run: each eye's pass draws its focus view too (LMU: games/lmu/mvr_pass.h), so the game
// prepares its scene for two passes instead of four. The launcher's [experiment] multi_view (on unless 0; on by default
// since 2026-11-10), for a game that has it (GameFeatures::multiView), on any GPU: NVIDIA's single-pass stereo on an RTX
// 20 series or newer (engine/nvapi_gpus.h), Sim-QVFR's own (instancing) elsewhere. The game's own device decides when
// it's made (a GPU that can do neither: the four passes as before).
#pragma once

#include <string>

#include "game.h"

namespace simqvfr::games {

// On for this run; `why`: the GPU and the way it is expected to draw ("NVIDIA GeForce RTX 5070 Ti: NVIDIA's single-pass
// stereo"), or why not.
bool multi_view_on(const GameInfo* game, const wchar_t* ini, std::string* why = nullptr);
// [experiment] lmu_mvr (LMU's MVR gates, 1 or 2) on a GPU that offers NVIDIA's multi-view rendering; 0 on any other GPU.
unsigned lmu_mvr_gate(const wchar_t* ini);

}  // namespace simqvfr::games

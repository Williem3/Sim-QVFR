// Reading sim-qvfr.ini at the start of a session: the experiment, live keys, crop and views4 sizes (config.cpp).
#pragma once

#include <string>

#include "games/game.h"
#include "layer/state.h"

namespace simqvfr::layer {

// config.cpp
games::GameFeatures game_features();
LiveSettings read_live(const wchar_t* ini);
std::wstring restart_keys(const wchar_t* ini, bool views4);
Views4Sizes views4_sizes(const wchar_t* ini, double focusFraction, double focusFractionY);
void runtime_info(const wchar_t* key, const std::wstring& value);
std::wstring layer_directory();
// The ini this launch reads: SIM_QVFR_INI from a launcher, else the one next to the DLL.
std::wstring experiment_ini();
// [experiment] native_quad_views (1, auto, the default; 0 off) in it, and nothing that rules it out (Cheeky's frame
// generation): read before the instance exists, to ask for XR_VARJO_quad_views. `why`: the reason, for the log.
bool native_quad_wanted(std::string* why);
void load_experiment();

}  // namespace simqvfr::layer

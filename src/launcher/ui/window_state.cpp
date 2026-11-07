// The window's state (window_state.h) and the small helpers every module uses: the chosen game, colours.
#include <string>

#include <windows.h>

#include "../../games/game.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

// The window.
App app;

// The game the launcher starts, by name (games/game.h).
std::wstring game_name() { return games::launcher_game().name; }

// Its short name ("ACE", "ACC"), for short text.
std::wstring short_name() { return games::launcher_game().shortName; }

// The chosen game has `feature` (games::GameFeatures); without it the window greys it out.
bool game_has(bool games::GameFeatures::*feature) { return games::launcher_game().features.*feature; }

std::wstring not_yet() { return L"Not available for " + short_name() + L" yet."; }

Color col(uint32_t rgb, float alpha) { return {rgb, alpha}; }

// The OpenXR layers pane: Windows' list has changes not applied yet.
bool layers_dirty() { return app.layersDraft != app.layersWindows || app.layersLockMoved; }

}  // namespace simqvfr::launcher::ui::win

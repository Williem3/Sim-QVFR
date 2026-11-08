// Diagnostic runs that end themselves ([experiment] exit_when_done=1): once a run has its samples, the game is asked to
// close the way its window's close button asks (WM_CLOSE to this process's visible top-level windows), so it shuts down
// normally and saves its settings, and the tester doesn't have to find the quit menu in the headset. Never forced: a
// game that ignores it keeps running.
#pragma once

namespace simqvfr {

// Posts WM_CLOSE to the game's visible, unowned top-level windows; how many were asked (0: none found). Logs it.
int request_game_exit(const char* why);

}  // namespace simqvfr

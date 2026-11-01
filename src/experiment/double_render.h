// Experiment 6: render every frame twice with identical cameras, to learn whether ACE survives
// a second render per frame and what it costs. Requires probe_start() (MinHook initialised and the
// exe verified) to have succeeded first.
#pragma once

namespace aceqv {

bool double_render_start();

}  // namespace aceqv

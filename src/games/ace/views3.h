// Single-frame quad views, step 1: run ACE's renderer in its 3-view mode (mode 2, most likely triple
// screens) while in VR, to learn what an extra view does: where its camera comes from, how the
// atlas grows, how the final pass maps it, and the GPU cost. Nothing overflows: the renderer's fixed
// per-view storage has 3 slots.
#pragma once

namespace aceqv {

// Patches renderer init so VR selects view mode 2. Must run before ACE creates its renderer
// (xrCreateInstance is early enough; views3_watch confirms it). Verifies the code bytes first.
bool views3_patch();
// Logs the renderer's view mode and per-view vector sizes from renderer begin. Requires
// probe_start() to have succeeded.
void views3_watch();

}  // namespace aceqv

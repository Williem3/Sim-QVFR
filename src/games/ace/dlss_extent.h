// views4 with DLSS: ACE makes its four DLSS features (the periphery's and the focus views' sizes) through one NGX
// parameter bag and evaluates them without setting OutWidth/OutHeight, so at every evaluation the bag holds the
// last-made (focus) view's output size. NGX itself uses each feature's own size; a layer between ACE and NGX that
// reads the bag (Cheeky Foveated DLSS builds without its created-extent fix, e.g. 0.7.0-standalone) composes the
// periphery at the focus size and the periphery no longer matches the focus box. These hooks on NGX's create and
// evaluate (linked into ACE, abi.h [dlss-extent]) record each feature's output and put it in the bag before each
// evaluation (ngx_extent.h). [experiment] views4_dlss_extent=0 turns them off.
#pragma once

namespace simqvfr {

// Verifies the anchors and hooks NGX's create and evaluate; logs and returns false (nothing hooked) otherwise. Call
// before ACE makes its renderer (and so its DLSS features), after views4_start (MinHook initialised).
bool dlss_extent_start();
// Once per xrEndFrame: the periodic summary.
void dlss_extent_frame_end();

}  // namespace simqvfr

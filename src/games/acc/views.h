// ACC's four views: FSteamVRHMD (abi.h) asks UE 4.26 for four views per VR frame, laid out as views4_layout.h has
// them in the game's target; views 2-3 are the focus views: each its eye's pose, with the eye's projection narrowed
// to the focus box (views_map.h focus_projection), centred, not yet turned to the gaze.
// ACC's shaders need instanced stereo, so the focus views are a second instanced pair (the left one primary).
// Vtable slots only, no code stubs. The layer widens the eye the runtime reports (OpenComposite passes it on
// as SteamVR's recommended size), so the target holds all four, and points the submitted eyes at views 0-1.
#pragma once

#include "../../quad/views4_layout.h"

namespace acc {

// Before UE's first frame in VR (xrCreateInstance). Checks every slot first: all or nothing. Again later:
// true, nothing changes. `focusX` / `focusY`: the focus box as a share of each eye's tangent width / height
// (focusX <= 0: the focus views copy the eyes; focusY <= 0: as focusX).
bool views_start(const aceqv::Views4Layout& layout, double focusX, double focusY);
// A live focus box, from the next projection UE asks for.
void views_set_focus(double focusX, double focusY);
// The game's target (width, height) as AdjustViewRect last saw it (0 before the first frame).
void views_target(int (&size)[2]);
// Every xrEndFrame: which passes UE asked for, logged every 900 frames.
void views_frame_end();
// Whether OpenXR's session runs (xrBeginSession until it is destroyed or another is created). Until it does,
// projections and eye poses come from the last ones SteamVR gave: OpenComposite aborts when asked without
// views (README.md).
void views_session_running(bool running);

}  // namespace acc

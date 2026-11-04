// ACC's four views: FSteamVRHMD (abi.h) asks UE 4.26 for four views per VR frame, laid out as views4_layout.h has
// them in the game's target; views 2-3 are the focus views: each its eye's projection narrowed to the focus box
// (views_map.h focus_projection), centred, and its eye's pose turned towards the gaze (views_set_gaze).
// ACC's shaders need instanced stereo, so the focus views are a second instanced pair (the left one primary).
// Vtable slots only, no code stubs. The layer widens the eye the runtime reports (OpenComposite passes it on
// as SteamVR's recommended size), so the target holds all four, and points the submitted eyes at views 0-1.
#pragma once

#include "../../quad/views4_layout.h"
#include "../adapter.h"

namespace acc {

// Before UE's first frame in VR (xrCreateInstance). Checks every slot first: all or nothing. Again later:
// true, nothing changes. `focusX` / `focusY`: the focus box as a share of each eye's tangent width / height
// (focusX <= 0: the focus views copy the eyes; focusY <= 0: as focusX).
bool views_start(const aceqv::Views4Layout& layout, double focusX, double focusY);
// A live focus box, from the next projection UE asks for.
void views_set_focus(double focusX, double focusY);
// Live resolution: the views laid out again inside the launch target (views4_live_eye_columns), from the next frame
// UE sets up. Each frame's record carries the layout it was rendered with.
void views_set_live_layout(const aceqv::Views4Layout& layout);
// Where each eye looks, as tangents of its view (`valid[e]` false: no turn), at every xrLocateViews. The focus
// views turn towards it (focus_turn.h, clamped so the box stays in the eye's view), from the next frame UE sets up.
void views_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]);
// The record of the next frame to compose (what its views were rendered with), in rendering order; false before
// the first. Latest: the newest, without taking it.
bool views_take_frame(aceqv::Views4Frame& frame);
bool views_latest_frame(aceqv::Views4Frame& frame);
// The game's target (width, height) as AdjustViewRect last saw it (0 before the first frame).
void views_target(int (&size)[2]);
// View v's rect in the target (x, y, width, height) as the renderer last reported it (SetFinalViewRect); false
// before it did.
bool views_view_rect(int view, int (&rect)[4]);
// The four views are patched in and views 2-3 narrowed to a focus box (not copies of the eyes).
bool views_focused();
// Every xrEndFrame: which passes UE asked for, logged every 900 frames.
void views_frame_end();
// Whether OpenXR's session runs (xrBeginSession until it is destroyed or another is created). Until it does,
// projections and eye poses come from the last ones SteamVR gave: OpenComposite aborts when asked without
// views (README.md).
void views_session_running(bool running);

}  // namespace acc

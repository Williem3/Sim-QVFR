// ACC's four views: FSteamVRHMD (abi.h) asks UE 4.26 for four views per VR frame, laid out as views4_layout.h has
// them in the game's target; views 2-3 are the focus views: each keeps its eye's camera, its eye's projection narrowed
// to the focus box where the gaze puts it (quad/offaxis_focus.h, as LMU's; views_set_gaze).
// ACC's shaders need instanced stereo, so the focus views are a second instanced pair (the left one primary).
// Vtable slots and code patches checked against this build first (abi.h). The layer widens the eye the runtime
// reports (OpenComposite passes it on as SteamVR's recommended size), so the target holds all four, and points the
// submitted eyes at views 0-1.
#pragma once

#include "../../quad/views4_layout.h"
#include "../adapter.h"

namespace acc {

// What views_start puts in besides the four views themselves (docs/recon-acc.md "Playbook audit"): each fix for a
// focus/periphery difference (the artefact catalogue's rows) on unless its [experiment] key is 0, so a gate run can
// tell what each one does. Read at xrCreateInstance (views_fixes).
struct ViewsFixes {
    bool exposure = true;     // acc_exposure_share: every view the left eye's auto exposure and pre-exposure
    bool vignette = true;     // acc_vignette_off: no tonemapper vignette in any view
    bool shadows = true;      // acc_shadow_share: the left eye's sun cascades, projected in every view
    bool foliageCull = true;  // acc_foliage_pairs: foliage culled per instanced pair (its own left, its partner's right)
    bool foliageLod = true;   // acc_foliage_lod: the focus views take the left eye's foliage LODs (CPU and shader)
    // acc_lens_effects_off=1: bloom and lens flares held off in every view. Off by default since the focus views take
    // their eye's bloom, flares and dirt mask (view_share.h, acc_bloom_share); before that each view blurred its own.
    bool lensEffects = false;
    // acc_temporal_share: each focus view's state takes its eye's frame index and TAA sample index every frame, so
    // its jitter and temporal noise run in step with its eye's (the copies gate's first run: 0.5% of pixels off by
    // more than 16 on edges, the tonemapper's dither a step apart everywhere).
    bool temporal = true;
    // acc_contact_shadows_off: UE's contact shadows held off (r.ContactShadows 0) while QVFR runs. They march each view's
    // own depth buffer, so the denser focus view finds other occluders than its eye: on the Spa lap of 2026-11-08 the
    // sunlit band below the windscreen base was lit in one view and shadowed in the other, both ways (docs/recon-acc.md
    // "The dash shadow mismatch"). Off, the gate's A/B had no such flips. Through the diagnostic hold (views_set_diag 3),
    // so Ctrl+Alt+5 and the gate's A/B switch it live.
    bool contactShadowsOff = true;
    // acc_focus_turn=1: the focus cameras turned towards the gaze with a centred box (the way before 2026-11-08), not
    // the eye's camera with the box off-axis.
    bool turnFocus = false;
};
ViewsFixes views_fixes(const wchar_t* ini);

// Before UE's first frame in VR (xrCreateInstance). Checks every slot first: all or nothing. Again later:
// true, nothing changes. `focusX` / `focusY`: the focus box as a share of each eye's tangent width / height
// (focusX <= 0: the focus views copy the eyes; focusY <= 0: as focusX).
bool views_start(const simqvfr::Views4Layout& layout, double focusX, double focusY, const ViewsFixes& fixes);
// Frames whose four views UE rendered (the render thread recorded them), since the start.
unsigned long long views_rendered_frames();
// Contact shadows are held off by ViewsFixes::contactShadowsOff (false: not asked, or not this build's).
bool views_contact_shadows_held();
// Eye e's projection as UE last had it (row-major, UE's: [14] the near plane in centimetres); false before the first.
bool views_eye_projection(int eye, float (&m)[16]);
// Where eye e looks, as tangents of its view (views_set_gaze's; its forward axis while none is valid).
bool views_eye_gaze(int eye, float (&tan)[2]);
// A live focus box, from the next projection UE asks for.
void views_set_focus(double focusX, double focusY);
// Live resolution: the views laid out again inside the launch target (views4_live_eye_columns), from the next frame
// UE sets up. Each frame's record carries the layout it was rendered with.
void views_set_live_layout(const simqvfr::Views4Layout& layout);
// Where each eye looks, as tangents of its view (`valid[e]` false: its forward axis), at every xrLocateViews. Each
// focus box is centred there (kept inside the eye), from the next frame UE sets up; acc_focus_turn=1: the focus
// camera turns towards it instead (focus_turn.h).
void views_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]);
// The record of the next frame to compose (what its views were rendered with), in rendering order; false before
// the first. Latest: the newest, without taking it.
bool views_take_frame(simqvfr::Views4Frame& frame);
bool views_latest_frame(simqvfr::Views4Frame& frame);
// The game's target (width, height) as AdjustViewRect last saw it (0 before the first frame).
void views_target(int (&size)[2]);
// View v's rect in the target (x, y, width, height) as the renderer last reported it (SetFinalViewRect); false
// before it did.
bool views_view_rect(int view, int (&rect)[4]);
// View v's output rect in the target (x, y, width, height) as AdjustViewRect last placed it: where post-processing
// after an upscaler (ACC's DLSS, Cheeky's) draws it; the same as views_view_rect without one. False before the first.
bool views_output_rect(int view, int (&rect)[4]);
// The four views are patched in and views 2-3 narrowed to a focus box (not copies of the eyes).
bool views_focused();
// Every xrEndFrame: which passes UE asked for, logged every 900 frames.
void views_frame_end();
// Diagnostics (acc_diag=1; games/diag_switches.h): abi.h kDiagCvars (0 SSR, 1 ambient occlusion, 2 the sun's shadows,
// 3 contact shadows: also ViewsFixes::contactShadowsOff and the gate's A/B)
// held at 0 while switched off, from the next xrEndFrame; switched on again, back to the game's own value. Start: false
// if their code isn't this build's (the switches then do nothing).
bool views_diag_start();
void views_set_diag(int which, bool off);
// Whether OpenXR's session runs (xrBeginSession until it is destroyed or another is created). Until it does,
// projections and eye poses come from the last ones SteamVR gave: OpenComposite aborts when asked without
// views (docs/recon-acc.md).
void views_session_running(bool running);

}  // namespace acc

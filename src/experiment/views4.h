// Single-frame quad views, stage gate: ACE renders 4 views in one VR frame, views 2 and 3 being
// copies of the two eyes (recon "The VR view path, mapped"). VR keeps view mode 1, so every
// VR-only branch runs as usual; what changes:
//  - viewCount() returns 4 to an allowlist of call sites (logged: every caller seen, once),
//  - the viewport function lays the 4 views out in the render target (views4_layout.h: equal
//    quarters, or outer and focus views each at their own size),
//  - per-view records and the 3-slot camera block fold views 2 and 3 onto 0 and 1,
//  - the layer doubles the eye width ACE sees, so its double-wide target holds 4 eye-sized views,
//    and points the submitted projection views at the first two quarters.
#pragma once

#include "../quad/focus_cull.h"
#include "../quad/views4_layout.h"
#include "../xr.h"

namespace aceqv {

// Where the four views sit in ACE's target (views4_layout.h): equal quarters unless set. Call
// before views4_start.
void views4_set_layout(const Views4Layout& layout);
// `callers`: "init" (renderer init's per-view vector sizing only), "all", or "init" plus
// comma-separated call-site RVAs (hex) of `call viewCount` instructions. Must run before ACE
// creates its renderer (xrCreateInstance). Verifies every site first; all or nothing.
// `eyesOnlySlotAdvance`: views 2-3 skip the history-slot advance (keeps the eyes' buffers, but
// breaks command recording; runs 7-10). Off = ACE's own advance for every view (runs 5-6: stable,
// all 4 views render, the eyes read the copies' buffer and show black).
// `focusFraction` / `focusFractionY`: with "cameras", views 2-3 cover this fraction of each eye's
// tangent width / height, centred on the eye's forward axis (`focusFractionY` <= 0: the same).
bool views4_start(const wchar_t* callers, bool eyesOnlySlotAdvance, double focusFraction, double focusFractionY = 0);
// Gaze ("cameras"): where each eye looks, as tangents (x, y) of that eye's view, set at every
// xrLocateViews (`valid[e]` false: no turn). The next focus cameras the builder fills turn towards
// the latest (focus_turn.h), clamped against the FOV ACE actually rendered. `gazeSample`: from a
// gaze sample (not the untracked default).
void views4_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool gazeSample);
// What one ACE frame's views were rendered with, recorded when the builder fills the focus cameras:
// per eye the outer view's tangents (left, right, up, down; read back from ACE's own eye projection,
// so they hold whatever FOV ACE used), the focus view's tangents, and the focus camera's turn (in the
// eye's frame). The composition maps the views with exactly these, on any headset.
struct Views4Frame {
    float outerTan[2][4];
    float focusTan[2][4];
    XrQuaternionf turn[2];
    double liveX[4] = {1, 1, 1, 1}, liveY[4] = {1, 1, 1, 1};  // the views' live sizes (views4_layout.h)
};
// Queues a frame record for the composition (fill_focus_cameras; tests feed it directly).
void views4_push_frame(const Views4Frame& frame);
// Focus cull (views4_focus_cull, needs "cameras"): every filled frame also builds the focus views'
// world-space frusta (focus_cull.h), their extents widened by `scale`. With `gaze` on, none are
// built before the first gaze sample. Call before views4_start.
void views4_set_focus_cull(bool on, double scale, bool gaze);
// Live resolution: the live sizes of `live` (views4_live) replace the layout's from the next focus
// cameras filled on, so one ACE frame renders with one set; its Views4Frame records them.
void views4_set_live_layout(const Views4Layout& live);
// Live settings (between frames, any time after views4_start): the focus box (as views4_start) and
// the focus cull's scale and gaze. The focus views keep their pixel size, so the box's density
// changes with it. The next focus cameras filled use them.
void views4_set_live(double focusFraction, double focusFractionY, double cullScale, bool gaze);
// The latest focus frustum of view 2 or 3; false when focus cull is off or none is built yet.
bool views4_focus_frustum(int view, FocusFrustum& out);
// The record of the next frame to be composed, in rendering order. False until the first focus
// cameras are filled (then the composition falls back to the located FOV, unturned).
bool views4_take_frame(Views4Frame& frame);
// The latest record, without taking it (the uncomposed debug outline).
bool views4_latest_frame(Views4Frame& frame);
// Diagnostic (views4_shadow_view1_plain): the deferredShadows loop's view-1 shortcut is skipped, so
// view 1 computes its shadow matrices like every other view.
bool views4_shadow_view1_plain();
// Diagnostic: comma-separated hex offsets of lighting-pass dwords that views 2-3 copy from their
// eye's pass (views4_borrow). Call before views4_start.
void views4_set_borrow(const wchar_t* offsets);
// Per-frame counters into the log, from xrEndFrame. Twice a session it also dumps the eyes' camera
// slots and per-view vectors next to the located views (to identify what each matrix holds).
void views4_frame_end();
// The views ACE just located (FOV and pose), for that dump.
void views4_note_views(const XrView* views, uint32_t count);

}  // namespace aceqv

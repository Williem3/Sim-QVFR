// X-Plane drawing four views (docs/recon-xplane.md): the instance multiplier 4, render-state slots 2-3 filled, and the
// final resolve's shader edited at runtime. Stage gate so far: slots 2-3 are copies of the eyes and the final resolve
// shows layers 2-3, so a normal headset image means X-Plane rendered four views.
#pragma once

#include <cstdint>

namespace xplane {

// At xrCreateInstance, after the layer counts are 4 (adapter.cpp). False: nothing was changed (a site isn't this
// build's), or some patches failed (logged).
bool four_views_copies_start();

// The focus stage: views 2-3 render the focus boxes (their projections narrowed to `fraction` x `fractionY` of each
// eye's tangent span, centred on the gaze, or on its forward direction) instead of copies of the eyes. Any time: it
// takes effect at the next four_views_latch_frame (at once before the first).
void four_views_set_focus(bool on, double fraction, double fractionY);

// The focus boxes follow the gaze (docs/recon-xplane.md "Gaze: the focus box off the eye's axis"): their projections
// shift off the eye's axis. Set before four_views_copies_start (default on); off keeps them on the eyes' axes.
// Either way the shader edits that place a box in its eye read it (centre and size) from push constants that follow
// every pipeline bind (docs/recon-xplane.md "Live focus size").
void four_views_set_gaze_follow(bool on);
// The push constants are patched in (four_views_copies_start) and the boxes follow the gaze.
bool four_views_gaze_follows();
// Where each eye looks (tangents x, y of its view; `valid` false: its forward axis), from xrLocateViews.
void four_views_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]);
// The settings (focus size, view rects) and the last gaze become the boxes of every draw and pipeline bind until the
// next latch: once a frame (xrEndFrame), so a frame's projections, push constants, LOD and composition record agree.
void four_views_latch_frame();
// Logs the latched boxes, the values pushed with them and the push counts (diagnostics).
void four_views_gaze_log();

// QVFR composition in eye columns (docs/recon-xplane.md "Composition"): the final resolve writes each eye's view over
// its focus view (instead of showing layers 2-3). Set before X-Plane creates its shaders (early load).
void four_views_set_eye_columns(bool on);
// Per-view sizes (docs/recon-xplane.md "Design: true per-view sizes"), stage 1: one viewport per view (every rect the
// whole layer unless four_views_set_view_sizes). Before four_views_copies_start.
void four_views_set_view_rects(bool on);
// Stage 2: each kind of view in a rect of its own size inside its layers, the layer being the larger of the two per
// axis (the size the layer offers X-Plane). Sizes in native eye widths and heights (Views4Layout's outer_width(),
// outer_height(), focus, focusHeight). Needs view rects; off: every rect the whole layer. From the next frame.
void four_views_set_view_sizes(bool on, double outerWidth, double outerHeight, double focusWidth, double focusHeight);
// Live resolution: the rects' shares of their layers (outer views x, y, focus views x, y; each (0, 1], the layers keep
// their launch size), from the next frame (the latch). Only with per-view sizes on: false, and nothing changed, when off.
bool four_views_set_view_shares(double outerX, double outerY, double focusX, double focusY);
// Diagnostics (docs/recon-xplane.md "Measured: the wing after the SSR edit"): the switch edits go into the SSR and CACAO
// and deferred-shading shaders (before X-Plane makes its shaders), and SSR's reflections, CACAO's occlusion or the sun's
// cascaded shadows are off from the next frame.
void four_views_set_diag_edits(bool on);
void four_views_set_diagnostics(bool ssrOff, bool aoOff, bool shadowOff);
// set viewports calls given four rects so far. Focus views take their eye's bloom (docs/recon-xplane.md "Measured:
// bloom per view"). Before X-Plane makes its shaders.
void four_views_set_bloom_from_eye(bool on);
// Alpha-tested materials in views 0-1 test the alpha of the focus view's mip (docs/recon-xplane.md "Measured: a 'wall'
// missing in the focus view"), so fences and foliage cut out alike in both views. Before X-Plane makes its shaders.
void four_views_set_periphery_alpha_lod(bool on);
// SSR in the focus views: marched in their eye's screen (true, the default) or in their own. Before X-Plane makes its
// shaders.
void four_views_set_ssr_from_eye(bool on);
// AO at the focus views' edges from their eye's (true, the default) or their own (docs/recon-xplane.md "Measured: a
// dark rim at the top of the focus views"). Before X-Plane makes its shaders.
void four_views_set_ao_edges_from_eye(bool on);
// The object and tree LOD from the focus views' pixel density (true, the default) or the eye view's, X-Plane's own
// (docs/recon-xplane.md "Measured: trees low poly in the focus views"). Any time; follows four_views_set_focus's size.
void four_views_set_focus_lod(bool on);
// The focus views shaded at the full rate (VRS writes rate 1x1 into layers 2-3). Before X-Plane makes its shaders.
void four_views_set_focus_full_rate(bool on);
int four_views_view_rect_sets();
// The views' height = half the stereo image's (five sites; abi.h). After four_views_copies_start. False: not this
// build's code, or a patch failed (logged).
bool four_views_eye_columns_start();
// How many viewports the eye-columns patch gave the whole column (the final resolve's, one a frame).
int four_views_column_viewports();
// Draws made with four views so far (a frame without any drew its eyes only: X-Plane's menu).
std::uint64_t four_views_draws();
// The final resolve's shader was made (X-Plane makes it when the flight loads).
bool four_views_final_resolve_made();
// Logs the viewports X-Plane set lately (the setter's arguments) and the VR context's size (diagnostics).
void four_views_viewport_log();
// Logs the stereo vegetation culls given u_render_layers 4, by the view count their setup computed (diagnostics).
void four_views_vegetation_log();

// What the views were last rendered with, per eye (left, right, up, down tangents): the eye's own projection and the
// focus view's. False before the first four-view draw. A fresh record is taken at the next draw after each call.
bool four_views_tangents(float (&outer)[2][4], float (&focus)[2][4]);

}  // namespace xplane

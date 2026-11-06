// X-Plane drawing four views (README.md): the instance multiplier 4, render-state slots 2-3 filled, and the final
// resolve's shader edited at runtime. Stage gate so far: slots 2-3 are copies of the eyes and the final resolve shows
// layers 2-3, so a normal headset image means X-Plane rendered four views.
#pragma once

#include <cstdint>

namespace xplane {

// At xrCreateInstance, after the layer counts are 4 (adapter.cpp). False: nothing was changed (a site isn't this
// build's), or some patches failed (logged).
bool four_views_copies_start();

// The focus stage: views 2-3 render the focus boxes (their projections narrowed to `fraction` x `fractionY` of each
// eye's tangent span, centred on its forward direction) instead of copies of the eyes.
void four_views_set_focus(bool on, double fraction, double fractionY);

// QVFR composition in eye columns (README.md "Composition"): the final resolve writes each eye's view over its focus
// view (instead of showing layers 2-3). Set before X-Plane creates its shaders (early load).
void four_views_set_eye_columns(bool on);
// Per-view sizes (README.md "Design: true per-view sizes"): one viewport per view, the outer views' `outerScale` of
// their layers from the top-left (1: the whole layer). Before four_views_copies_start.
void four_views_set_view_rects(bool on, double outerScale);
// set viewports calls given four rects so far.
// Focus views take their eye's bloom (README.md "Measured: bloom per view"). Before X-Plane makes its shaders.
void four_views_set_bloom_from_eye(bool on);
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

// What the views were last rendered with, per eye (left, right, up, down tangents): the eye's own projection and the
// focus view's. False before the first four-view draw. A fresh record is taken at the next draw after each call.
bool four_views_tangents(float (&outer)[2][4], float (&focus)[2][4]);

}  // namespace xplane

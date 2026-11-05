// X-Plane drawing four views (README.md): the instance multiplier 4, render-state slots 2-3 filled, and the final
// resolve's shader edited at runtime. Stage gate so far: slots 2-3 are copies of the eyes and the final resolve shows
// layers 2-3, so a normal headset image means X-Plane rendered four views.
#pragma once

namespace xplane {

// At xrCreateInstance, after the layer counts are 4 (adapter.cpp). False: nothing was changed (a site isn't this
// build's), or some patches failed (logged).
bool four_views_copies_start();

// The focus stage: views 2-3 render the focus boxes (their projections narrowed to `fraction` x `fractionY` of each
// eye's tangent span, centred on its forward direction) instead of copies of the eyes.
void four_views_set_focus(bool on, double fraction, double fractionY);

}  // namespace xplane

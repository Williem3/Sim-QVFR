// Where the eyes look, how far: LMU's scene depth at the left eye's box centre, so the layer converges both eyes' focus
// boxes there (quad/focus_converge.h), as ACC's (games/acc/fixation.h). At the left eye pass's post (the scene drawn,
// its depth target the one DLSS reads: mvr_pass_binds.h post_begins) a few texels round the box centre are read without
// stalling (quad/d3d11_fixation.h), turned into metres through the pass camera's depth terms (dlss_inputs.h; reversed
// as DLSS takes it, lmu_dlss_depth_reversed) and smoothed. [experiment] views4_converge=0 turns the convergence off in
// the layer; lmu_fixation=0 turns this read off (the boxes parallel, as before).
#pragma once

#include "../../quad/offaxis_focus.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;

namespace lmu {

// The session's device (d3d11_session); `reversed` LMU's depth convention. Stop at the session's end.
void fixation_start(ID3D11Device* device, bool reversed);
void fixation_stop();
// The left eye pass's scene is drawn into `dsv`'s texture: queue a read round `box`'s centre (the eye at the target's
// top-left, eyeW x eyeH), once a pass. First at the pass's first transparent draw writing depth (mvr_pass_glass.h: the
// car's glass writes depth, and the eyes look through it), else at the pass's post (`passEnd`, which ends the pass's
// turn). On the render thread, inside the hooks (the caller marks the work as ours).
void fixation_pass(ID3D11DepthStencilView* dsv, const simqvfr::FocusBox& box, unsigned eyeW, unsigned eyeH,
                   bool passEnd);
// Every xrEndFrame on the render thread: take a finished read.
void fixation_frame_end();
// The smoothed fixation distance in metres; 0 before the first read (or off). Any thread.
double fixation_m();
// The scene depth's terms as last looked up (depth = a + b / z, the depth target's own convention: LMU's reversed);
// false before the first.
bool fixation_depth_terms(double& a, double& b);

}  // namespace lmu

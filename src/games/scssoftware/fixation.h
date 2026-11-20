// Where the eyes look, how far: the SCS games' scene depth at the left eye's box centre, so the layer converges both eyes'
// focus boxes there (quad/focus_converge.h), as ACC's and LMU's. The left eye's G-buffer depth is read as its pass ends
// (the eyes share one depth buffer: the right eye's pass clears it), a few texels round the box centre, without stalling
// (quad/d3d11_fixation.h). The depth's convention is never assumed: a scene draw's world-view-projection (its vertex
// range's rows 4-7, the scene shaders' cb0[4..7]) is read back now and then, and its z row is a w row times a plus
// b (0,0,0,1): depth = a + b / w, w the view distance (fixation_depth_terms). ATS's depth target has no shader binding:
// copied whole every 4th frame. The cab's first few draws have their own projection (0.1-15 m) into the same depth; the
// world's terms are taken (headset A/B 2026-11-19: they put both boxes together on the dash and the road alike).
// [experiment] scs_fixation=0: no reads; scs_fixation_converge=0: reads only, the boxes parallel (Ctrl+Alt+V toggles it
// in dev builds); views4_converge=0 turns the convergence off in the layer.
#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11DepthStencilView;

namespace scs {

// The depth terms of a clip matrix given as the four float4 a shader dots the position with (`rows`: clip x, y, z, w),
// or the same transposed (`rows` false: columns). False: not a perspective matrix whose z is an affine function of its
// w (a and b unchanged). Pure, for tests/scs_fixation_test.cpp.
bool fixation_depth_terms(const float (&m)[4][4], bool rows, double& a, double& b);

// [experiment] scs_fixation (the reads) and scs_fixation_converge (the boxes aimed by them): the adapter, at launch.
void fixation_wanted(bool on, bool aim);
// The session's device; on the render thread (the corner's first G-buffer bind). Again: nothing new.
void fixation_start(ID3D11Device* device);
// At the session's end (release_all).
void fixation_stop();
// Each eye's field of view as tangents left, right, down, up (the adapter's qvfr_note_views). Any thread.
void fixation_note_fov(const float (&fov)[2][4]);
// An indexed scene draw in the left eye's G-buffer pass, its vertex shader's constants bound: now and then their rows
// 4-7 copied for the depth terms.
void fixation_scene_draw(ID3D11DeviceContext1* context);
// The left eye's G-buffer pass begins with `dsv` bound, and ends: its depth read round the box centre `centre` (fractions
// of the eye's screen, y down), the eye drawn into the top-left fx x fy share of eyeW x eyeH targets.
void fixation_gbuffer_begin(ID3D11DepthStencilView* dsv);
// The depth target without a shader binding is copied whole into one of ours first, every 4th left pass.
void fixation_gbuffer_end(ID3D11DeviceContext* context, const float (&centre)[2], float fx, float fy, unsigned eyeW,
                          unsigned eyeH);
// The frame's start on the render thread: finished reads taken.
void fixation_frame_end();
// The smoothed fixation distance in metres; 0 before the first read (or off). Any thread.
double fixation_m();

}  // namespace scs

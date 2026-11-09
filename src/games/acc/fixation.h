// Where the eyes look, how far: ACC's scene depth at the left eye's gaze point, so the layer converges both eyes' focus
// boxes there (quad/focus_converge.h; docs/recon-acc.md "Box convergence"). The scene depth is the depth target ACC's
// base pass draws with (bound beside three or more colour targets, the G-buffer: view_share.h's target hooks pass it
// on); at each xrEndFrame a few texels round the gaze point of the left eye's view are read without stalling
// (quad/d3d11_depth_probe.h), turned into metres (UE's reversed infinite depth: view z = near / depth, in centimetres)
// and smoothed (FixationFilter). [experiment] views4_converge=0 turns the convergence off in the layer.
#pragma once

struct ID3D11Device;
struct ID3D11DepthStencilView;

namespace acc {

// The session's device (xrCreateSession); stop at its end.
void fixation_start(ID3D11Device* device);
void fixation_stop();
// Every render target set on ACC's immediate context (view_share's hooks): `colourTargets` bound with `dsv`.
void fixation_targets(unsigned colourTargets, ID3D11DepthStencilView* dsv);
// Every xrEndFrame, on ACC's render thread: queue this frame's read, take a finished one.
void fixation_frame_end();
// The smoothed fixation distance in metres; 0 before the first read.
double fixation_m();

}  // namespace acc

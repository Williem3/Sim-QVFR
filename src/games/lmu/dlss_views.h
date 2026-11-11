// DLAA for LMU's views at the API level ([experiment] lmu_dlss; docs/recon-lmu.md "DLSS for LMU at the API level").
// LMU ships no DLSS and makes no motion vectors or jitter, so the layer supplies all of it without touching the game's
// code:
//   - jitter: each frame the FOV the game locates (xrLocateViews) is moved by a fraction of a pixel; the game builds its
//     projection from it. The focus views (drawn by their eye's pass through a clip remap, mvr_pass.h) get their own
//     through the remap. The layer composes with the FOV as located by the runtime, so the headset sees none of it.
//   - depth: the pass's multisample depth target, sample 0.
//   - motion vectors: depth reprojected through this frame's and the last frame's view x projection, which the game's
//     renderer holds for the pass (a read). Right for whatever stands still in the world; the player's car and the
//     others get wrong vectors until they have their own (the next step).
//   - evaluation: right after the pass's multisample resolve (mvr_pass.cpp's ResolveSubresource hook), or with MSAA off
//     right before YEBIS's copy reads the scene's own target (mvr_pass.cpp copy_before), each view's part of the HDR
//     scene through NGX (engine/ngx_dlss.h) and back into place, before the game's post-processing reads it.
// Needs the multi-view path (the eye passes found at the d3d11 level) and a build with the NGX SDK.
#pragma once

#include <cstdint>
#include <string>

#include <openxr/openxr.h>

struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace lmu {

struct DlssRun {
    int views = 0;             // 0 off, 1 the eyes, 2 the eyes and the focus views
    std::wstring dllFolder;    // where nvngx_dlss.dll is (empty: beside the game's exe)
    int preset = 0;            // NGX's render preset number (0: its default)
    bool jitter = true;        // off: no jitter (the inputs' plumbing alone; DLAA then only smooths over time)
    bool viewInverse = false;  // the matrix the renderer holds is the camera's world matrix, not the view matrix
    // The scene's depth is 1 at the near plane and 0 far away (measured: its depth test is "greater", its draws'
    // projection the eye's with the depth turned round). Off: 0 near, as the eye's projection slot has it.
    bool depthReversed = true;
    bool flipX = false, flipY = false;  // the jitter handed to NGX negated on that axis
    // The player's car and cockpit: what lies in a box around the head in the tracking space (metres to the left,
    // right, down, up, front and back) moves on screen with the head alone (the eyes' located poses), not with the
    // car's travel through the world. Off: everything is the world (the cockpit then smears at speed).
    bool car = true;
    double carBox[6] = {1.5, 1.5, 1.5, 0.6, 3.0, 3.0};
    bool gate = false;         // every 300 frames: the vectors checked against the frames themselves, logged
    bool vectors = true;       // off: zero vectors (a comparison)
};

void dlss_start(const DlssRun& run);
bool dlss_wanted();
// The end of xrLocateViews for the game's stereo views: this frame's jitter put into their FOV.
void dlss_jitter_views(XrView* views, uint32_t count);
// Eye `eye`'s focus view's remap for this frame (x' = a x + b w, y' = c y + d w): its jitter added to b and d.
void dlss_focus_jitter(int eye, unsigned focusWidth, unsigned focusHeight, float& b, float& d);
// The clip shift (x += cx w, y += cy w) this frame's jitter put into eye `eye`'s projection, as the projection carries
// it (the log's sign check): what the eye's image no longer has once DLAA is through it. What is measured on the
// projection (the focus box's eye tangents, which the layer composes the periphery with) is moved back by it; what is
// drawn with it after the post (the VR UI, the driver tags) is drawn moved back by it. False: no jitter this frame.
bool dlss_eye_clip_shift(int eye, double& cx, double& cy);

struct DlssPass {
    int eye = 0;
    unsigned eyeWidth = 0, eyeHeight = 0;      // the eye's part of the resolved scene, from its top-left
    bool focus = false;                        // its focus view is right of it
    unsigned focusWidth = 0, focusHeight = 0;
    float remap[4] = {1, 0, 1, 0};             // the focus view's clip remap as drawn (its jitter included)
};
// Right after an eye pass's multisample resolve into `resolved`, on the game's immediate context; `depth` the scene's
// depth target. The caller keeps its own hooks out of the way (the work draws and copies on `context`); the context's
// state is put back call by call before it returns.
void dlss_after_resolve(ID3D11DeviceContext* context, ID3D11Resource* resolved, ID3D11DepthStencilView* depth,
                        const DlssPass& pass);
void dlss_frame_end(unsigned long long frame);
// The session's end (or a check's): every feature released, NGX shut down, nothing of the device held. A later pass
// starts it again.
void dlss_shutdown();

// Checks outside the game (tests/lmu_dlss_check.cpp): the pass's view and projection given instead of read from the
// game's renderer (null: read); the evaluations that succeeded since the start.
void dlss_test_matrices(const float* view, const float* projection);
unsigned long long dlss_evaluations_total();

}  // namespace lmu

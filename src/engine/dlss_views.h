// DLAA through NGX for a game that ships none, at the API level: the layer supplies every input without touching the
// game's code. The engine's part, game-independent; a game gives it its pass's matrices (dlss_set_matrices) and calls
// it at its eye passes (LMU: games/lmu/dlss_inputs.h for the matrices, mvr_pass.cpp for the calls; docs/recon-lmu.md
// "DLSS for LMU at the API level").
//   - jitter: each frame the FOV the game locates (xrLocateViews) is moved by a fraction of a pixel; the game builds its
//     projection from it. The focus views (drawn by their eye's pass through a clip remap) get their own through the
//     remap. The layer composes with the FOV as located by the runtime, so the headset sees none of it.
//   - depth: the pass's depth target (multisample: sample 0).
//   - motion vectors: depth reprojected through this frame's and the last frame's view x projection (the game's, from
//     its callback). Right for whatever stands still in the world; what lies in a box about the head (a cockpit) moves
//     with the head alone, from the eyes' located poses; other moving things get the world's vectors (they smear).
//   - evaluation: the caller's, right after an eye pass's scene is complete and before its post-processing reads it
//     (a multisample resolve, or the first read of the scene's own target), each view's part of the HDR scene through
//     NGX (ngx_dlss.h) and back into place.
// Needs a build with the NGX SDK (CMake: NGX_SDK_DIR).
#pragma once

#include <cstdint>
#include <string>

#include <openxr/openxr.h>

struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace simqvfr {

struct DlssRun {
    const char* tag = "dlss";  // the log lines' prefix (LMU: "lmu dlss")
    int views = 0;             // 0 off, 1 the eyes, 2 the eyes and the focus views
    std::wstring dllFolder;    // where nvngx_dlss.dll is (empty: beside the game's exe)
    int preset = 0;            // NGX's render preset number (0: its default)
    bool jitter = true;        // off: no jitter (the inputs' plumbing alone; DLAA then only smooths over time)
    bool viewInverse = false;  // the matrix the game gives is the camera's world matrix, not the view matrix
    // The scene's depth is 1 at the near plane and 0 far away while the projection the game gives is the ordinary one
    // (LMU: its depth test is "greater", its draws' projection the eye's with the depth turned round). Off: 0 near.
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

// The game's side: eye `eye`'s pass's view matrix (world -> view, row vectors) and projection as rendered (jitter
// included, the ordinary depth convention; see DlssRun::depthReversed), each 4x4 row-major. False: not available.
using PassMatricesFn = bool (*)(int eye, float (&view)[16], float (&projection)[16]);
void dlss_set_matrices(PassMatricesFn fn);

void dlss_start(const DlssRun& run);
bool dlss_wanted();
// The end of xrLocateViews for the game's stereo views: this frame's jitter put into their FOV.
void dlss_jitter_views(XrView* views, uint32_t count);
// Eye `eye`'s focus view's remap for this frame (x' = a x + b w, y' = c y + d w): its jitter added to b and d.
void dlss_focus_jitter(int eye, unsigned focusWidth, unsigned focusHeight, float& b, float& d);
// The clip shift (x += cx w, y += cy w) this frame's jitter put into eye `eye`'s projection, as the projection carries
// it (the log's sign check): what the eye's image no longer has once DLAA is through it. What is measured on the
// projection (the focus box's eye tangents, which the layer composes the periphery with) is moved back by it; what is
// drawn with it after the post (a VR UI, labels) is drawn moved back by it. False: no jitter this frame.
bool dlss_eye_clip_shift(int eye, double& cx, double& cy);

struct DlssPass {
    int eye = 0;
    unsigned eyeWidth = 0, eyeHeight = 0;      // the eye's part of the scene, from its top-left
    bool focus = false;                        // its focus view is right of it
    unsigned focusWidth = 0, focusHeight = 0;
    float remap[4] = {1, 0, 1, 0};             // the focus view's clip remap as drawn (its jitter included)
};
// Right after an eye pass's scene is complete in `resolved` (a resolve's target, or the scene's own single-sample
// target), on the game's immediate context; `depth` the scene's depth target. The caller keeps its own hooks out of
// the way (the work draws and copies on `context`); the context's state is put back call by call before it returns.
void dlss_after_resolve(ID3D11DeviceContext* context, ID3D11Resource* resolved, ID3D11DepthStencilView* depth,
                        const DlssPass& pass);
void dlss_frame_end(unsigned long long frame);
// The session's end (or a check's): every feature released, NGX shut down, nothing of the device held. A later pass
// starts it again.
void dlss_shutdown();

// The evaluations that succeeded since the start (checks outside the game).
unsigned long long dlss_evaluations_total();

}  // namespace simqvfr

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
//   - the focus views' mode (DlssRun::focusScale): DLAA, or drawn smaller by the game's pass (the caller sizes them by
//     dlss_focus_scale) and upscaled into their full region by DLSS super resolution before the post reads it.
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
    std::wstring dllFolder;    // the player's nvngx_dlss.dll folder, looked in first (empty: Sim-QVFR's own DLL)
    // NGX's render presets (dlss_math.h preset_name: 0 NGX's default for the mode, 6 F, 11 K, 13 M): the focus views',
    // and the eyes' (the periphery's) unless eyePreset is set (>= 0). NGX's default is K for DLAA, quality and
    // balanced, M for performance; F (an older, cheaper model) costs the eyes about half of K.
    int preset = 0;
    int eyePreset = -1;
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
    // The focus views' DLSS mode: drawn at this percent of their size each way and upscaled to it by DLSS super resolution
    // (77 ultra quality, 67 quality, 58 balanced, 50 performance; 33..100), 100 DLAA. The eyes stay DLAA.
    int focusScale = 100;
    // Textures sampled sharper (dlss_mip_bias): 0 off, 1 the focus views, 2 the focus views and the eyes.
    int textureSharpening = 0;
};

// The game's side: eye `eye`'s pass's view matrix (world -> view, row vectors) and projection as rendered (jitter
// included, the ordinary depth convention; see DlssRun::depthReversed), each 4x4 row-major. False: not available.
using PassMatricesFn = bool (*)(int eye, float (&view)[16], float (&projection)[16]);
void dlss_set_matrices(PassMatricesFn fn);

void dlss_start(const DlssRun& run);
bool dlss_wanted();

// The player's settings changed while the game runs (engine/dlss_settings.h), from any thread: taken at the next
// dlss_frame_end, on the render thread, between frames. On or off (off releases the features; NGX stays started until
// the session ends; on again starts NGX if it never did); the focus views' mode; the focus views' and the eyes' presets.
// A changed mode or preset makes each view's feature again at its next pass: a short hitch.
struct DlssLive {
    bool on = false;      // the eyes and the focus views (DlssRun::views 2), or none
    int focusScale = 100; // DlssRun::focusScale
    int preset = 0;       // DlssRun::preset (the focus views')
    int eyePreset = -1;   // DlssRun::eyePreset
    int textureSharpening = 0;  // DlssRun::textureSharpening (no new feature: taken at once)
};
void dlss_live(const DlssLive& live);
// The end of xrLocateViews for the game's stereo views: this frame's jitter put into their FOV.
void dlss_jitter_views(XrView* views, uint32_t count);
// The focus views' render scale for the frame being set up: a focus view w x h is drawn at dlss::render_size(w, s) x
// dlss::render_size(h, s) and DLSS upscales it to w x h before anything reads it (DlssPass::focusRenderWidth). 1 unless
// DlssRun::focusScale asks for less and DLSS works (NGX started, no focus feature refused at its sizes): until then,
// and after it gives up, the focus views are drawn at their size.
double dlss_focus_scale();
// Eye `eye`'s focus view's remap for this frame (x' = a x + b w, y' = c y + d w): its jitter added to b and d. The
// size is what the focus view is drawn at (its render size under super resolution).
void dlss_focus_jitter(int eye, unsigned focusWidth, unsigned focusHeight, float& b, float& d);
// The clip shift (x += cx w, y += cy w) this frame's jitter put into eye `eye`'s projection, as the projection carries
// it (the log's sign check): what the eye's image no longer has once DLAA is through it. What is measured on the
// projection (the focus box's eye tangents, which the layer composes the periphery with) is moved back by it; what is
// drawn with it after the post (a VR UI, labels) is drawn moved back by it. False: no jitter this frame.
bool dlss_eye_clip_shift(int eye, double& cx, double& cy);
// The mip bias eye `eye`'s pass samples its textures with (DlssRun::textureSharpening), NVIDIA's (DLSS programming
// guide 3.5): log2(render width / output width) - 1, so -1 under DLAA and -2 at performance. Only for a view DLSS
// processed last frame (as the jitter): a view left as drawn keeps the game's sampling. `focus` the focus view's, `eye`
// the eye's (0 under "focus only"). False: both 0 (off, or nothing processed).
bool dlss_mip_bias(int eye, float& focus, float& eyeBias);

struct DlssPass {
    int eye = 0;
    unsigned eyeWidth = 0, eyeHeight = 0;      // the eye's part of the scene, from its top-left
    bool focus = false;                        // its focus view is right of it
    unsigned focusWidth = 0, focusHeight = 0;  // its size (what the post reads, from the eye's top-right)
    // What it was drawn at, from the same corner (dlss_focus_scale; 0: its size): upscaled to its size in place.
    unsigned focusRenderWidth = 0, focusRenderHeight = 0;
    float remap[4] = {1, 0, 1, 0};             // the focus view's clip remap as drawn (its jitter included)
};
// Right after an eye pass's scene is complete in `resolved` (a resolve's target, or the scene's own single-sample
// target), on the game's immediate context; `depth` the scene's depth target. The caller keeps its own hooks out of
// the way (the work draws and copies on `context`); the context's state is put back call by call before it returns.
// A focus view drawn smaller that DLSS couldn't upscale this frame is stretched to its size (soft, but in place).
void dlss_after_resolve(ID3D11DeviceContext* context, ID3D11Resource* resolved, ID3D11DepthStencilView* depth,
                        const DlssPass& pass);
void dlss_frame_end(unsigned long long frame);
// The session's end (or a check's): every feature released, NGX shut down, nothing of the device held. A later pass
// starts it again.
void dlss_shutdown();

// The evaluations that succeeded since the start, and the focus views stretched instead (checks outside the game).
unsigned long long dlss_evaluations_total();
unsigned long long dlss_stretched_total();

}  // namespace simqvfr

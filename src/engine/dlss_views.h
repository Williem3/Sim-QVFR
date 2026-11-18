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
#include <vector>

#include <openxr/openxr.h>

struct ID3D11CommandList;
struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;

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
    // The upscaler that runs the views (the same inputs, jitter and modes either way): 0 NVIDIA DLSS (NGX on the game's
    // D3D11 device), 1 AMD FSR (engine/fsr_upscale.h: our D3D12 device beside the game's; FSR 4 where AMD's runtime
    // offers it, else 3.1; its "native AA" for DLAA; NGX's presets don't apply). fsrWhereNoDlss: where DLSS can't run (a
    // GPU not NVIDIA's by its DXGI vendor id, or NGX refusing to start: an older NVIDIA card), FSR whatever `upscaler`
    // says.
    // 0x1002), where NGX can't run, FSR whatever `upscaler` says.
    int upscaler = 0;
    bool fsrWhereNoDlss = false;
    std::wstring fsrFolder;  // AMD's two DLLs' folder, looked in first (empty: beside the layer, then the game's exe)
    int fsrProvider = 0;     // 0 the newest AMD's runtime offers on this GPU, 3 the newest FSR 3.x (fsr::Provider)
    // With the depth copied before the transparents (dlss_depth_copy_list): every ~2 s, one eye's copy compared with the
    // scene's depth at the pass's end (every 8th texel), logged: how much of the view transparents writing depth (a
    // car's glass) drew nearer, at what distance, and what lies behind them.
    bool depthMeasure = false;
    // The copy taken only where what was drawn over it lies nearer than this (metres; 0: everywhere): the player's car's
    // glass, not another car drawn blended with its depth written (whose surface the depth is).
    double depthCopyNear = 3.0;
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
    int upscaler = 0;           // DlssRun::upscaler (every view's feature made again by the other)
};
void dlss_live(const DlssLive& live);
// Checks: how long the settings must hold before a change that makes features again is taken (500 ms in the game).
void dlss_live_settle_for_tests(int ms);
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
    // Moving objects (LMU's other cars, games/lmu/mvr_pass.h mvr_car_slots): a uint texture the scene target's size whose
    // texel is an object's slot + 1 where that object is the surface (0: the world), and a structured buffer of float4x3,
    // per slot the object's motion in world space (a point this frame times it: where it was the frame before; row
    // vectors). Null: none (the world's reprojection alone).
    ID3D11ShaderResourceView* objectSlots = nullptr;
    ID3D11ShaderResourceView* objectMotion = nullptr;
    unsigned objectCount = 0;
    // The scene's depth was copied in this pass before its transparents wrote theirs (dlss_depth_copy_list, executed):
    // with depthCopyRead the vectors and DLSS take that copy in place of the depth target (what lies behind a car's
    // glass, not the glass); without, it is only measured (DlssRun::depthMeasure; an A/B's other side).
    bool depthCopied = false;
    bool depthCopyRead = true;
    // DLAA on the focus view over a band of its rows alone (focusBandHeight rows from focusBandTop; 0: all of it), a
    // feature of its own at that size; the rows outside it take the eye's image as DLSS made it, resampled (a game that
    // draws only part of its focus view at full quality: LMU's corridor). DLAA alone (not super resolution), NGX alone.
    // The band may move from frame to frame (its vectors carry the move); its height should hold (a new height makes
    // its feature again).
    unsigned focusBandTop = 0, focusBandHeight = 0;
    // What of the focus view the game drew at full quality (LMU's corridor; null: all of it): after DLSS, the rest
    // takes the eye's image as DLSS made it, so what the game filled in from the eye before DLSS (its raw, jittered
    // image) never reaches the headset. focusKeep: a texture over the focus view (its texel nearest each pixel) whose
    // value above 0 is kept, blended to the eye's image across focusKeepFeather of its units (0: a hard edge);
    // focusStamps: a uint texture the focus view's size whose texels equal to focusStamp take the eye's image too.
    ID3D11ShaderResourceView* focusKeep = nullptr;
    unsigned focusKeepWidth = 0, focusKeepHeight = 0;
    float focusKeepFeather = 0;
    ID3D11ShaderResourceView* focusStamps = nullptr;
    uint32_t focusStamp = 0;
    bool focusKeepTint = false;  // what takes the eye's image tinted blue (the game's coverage view)
};
// Transparents that write depth (LMU's car glass: blended, depth written): through them DLSS would see the glass's
// distance for the world behind it, and every pixel there would get the vector of a point half a metre away (the road
// ahead smears; with the car's box, it moves with the head alone). The game's side, at the first such draw of an eye
// pass after its opaque depth: a command list (the scene's depth `depth` as it stands, its first sample, the top-left
// `width` x `height`, into the module's own copy) to execute on the immediate context with its state restored
// (ExecuteCommandList(list, TRUE)); then DlssPass::depthCopied at the pass's dlss_after_resolve. Null: not now (the
// module not started yet, or the depth target unreadable). Recorded again only for another depth target or size.
ID3D11CommandList* dlss_depth_copy_list(ID3D11DepthStencilView* depth, unsigned width, unsigned height);
// Right after an eye pass's scene is complete in `resolved` (a resolve's target, or the scene's own single-sample
// target), on the game's immediate context; `depth` the scene's depth target. The caller keeps its own hooks out of
// the way (the work draws and copies on `context`); the context's state is put back call by call before it returns.
// A focus view drawn smaller that DLSS couldn't upscale this frame is stretched to its size (soft, but in place).
void dlss_after_resolve(ID3D11DeviceContext* context, ID3D11Resource* resolved, ID3D11DepthStencilView* depth,
                        const DlssPass& pass);
void dlss_frame_end(unsigned long long frame);
// Checks: view `index`'s motion vectors as its last evaluation got them (0-1 the eyes, 2-3 their focus views), x and y in
// pixels per texel, row by row. False: none yet.
bool dlss_vectors_for_checks(ID3D11DeviceContext* context, int index, std::vector<float>& xy, unsigned& width,
                             unsigned& height);
// Every view's feature and its textures released, NGX (or FSR) left started: made again at each view's next pass (the
// showcase's Stock, which runs none). On the render thread, between frames. The features released.
int dlss_release_views();
// The session's end (or a check's): every feature released, NGX shut down, nothing of the device held. A later pass
// starts it again.
void dlss_shutdown();

// The upscaler running the views now (DlssRun::upscaler as resolved for the GPU): 0 DLSS, 1 FSR; -1 none started yet.
int dlss_upscaler_in_use();
// Its name for the overlay, from any thread: "DLSS", "FSR4", "FSR3.1" (the provider AMD's runtime took); "DLSS" until
// one is known.
const wchar_t* dlss_upscaler_badge();

// The evaluations that succeeded since the start, and the focus views stretched instead (checks outside the game).
unsigned long long dlss_evaluations_total();
unsigned long long dlss_stretched_total();

}  // namespace simqvfr

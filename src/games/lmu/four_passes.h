// LMU's four passes (docs/recon-lmu.md "Design notes"): the game's VR eye loop runs four times instead of two. Stage one
// is the copies gate ([experiment] lmu_copies=1): passes 2 and 3 render eyes 0 and 1 again, with the same cameras and
// targets, and their images are compared with the eyes' pixel for pixel. Every difference is per-eye state that a
// focus view would get wrong. The focus gate (lmu_focus=1) renders passes 2-3 as focus views and compares them with
// their eye's crop. QVFR (lmu_qvfr=1 with mode=views4) renders them every frame and hands the layer all four views in
// one texture (the atlas) to compose. The gates change what the game renders, so they only run when asked.
#pragma once

#include "../adapter.h"

struct ID3D11Texture2D;

namespace lmu {

// A run's settings ([experiment] keys, lmu/adapter.cpp).
struct FourPassesRun {
    bool qvfr = false;           // QVFR: four passes every frame, composed by the layer (no samples, no exit)
    bool focus = false;          // passes 2-3 are focus views (else copies of the eyes); QVFR: always
    bool holdExposure = true;    // passes 2-3 render with YEBIS's frame time held near zero
    double fraction = 0.3;       // the focus box: share of each eye's tangent width
    double fractionY = 0;        // and height (<= 0: as `fraction`)
    int first = 300;             // gates: scene frames before the first sample
    int every = 300;             // gates: scene frames between samples
    int count = 3;               // gates: samples
    int warmup = 60;             // gates: four-pass frames before each sample (else two: full frame rate)
    bool exitWhenDone = false;   // gates: ask the game to close after the last sample (probe/run_exit.h)
    // QVFR diagnostic ([experiment] lmu_render_scale_probe, percent; 0 off): 600 scene frames in, the render scale is
    // set to this share for 120 frames and then restored, the eye and focus images saved at its middle: what LMU renders
    // when only the scale changes (the live resolution lever, docs/recon-lmu.md).
    int renderScaleProbe = 0;
    // QVFR: frames without the eye loop (menus) composed at the native eye with the UI drawn at that size
    // ([experiment] lmu_menu_native, default on); the views' layer in native eye widths and heights (the native size
    // is the size offered over it).
    bool menuNative = true;
    double layerWidth = 1, layerHeight = 1;
    // QVFR gate C of multi-view rendering ([experiment] lmu_mvr=1, mvr_pass.h): passes 0-1 draw their focus views too,
    // beside the eye; passes 2-3 as before; compared at the gate samples (first, every, count).
    bool mvr = false;
    // Gate D (lmu_mvr=2): passes 2-3 skip the scene (its draws, sorting and culling) when their eye's pass drew the focus
    // view, their post-processing run on it; at the gate samples the focus view is compared with its eye's crop.
    bool mvrSkip = false;
    // The focus views beside their eyes in images of the outer views' shape (Views4Layout::secondViewBeside): the eyes render at
    // one scale (the larger of their shares: YEBIS follows the render scale on both axes), the focus views at their own
    // width and height (four_passes_set_live_axes), drawn by their eye's pass, post-processed at the scale that covers them.
    bool perAxis = false;
    // An A/B run of MVR ([experiment] lmu_mvr_ab=N): N frames with it, then N with the four passes alone (passes 2-3
    // render their scene), in turn; each side's CPU and GPU per pass logged apart. 0: MVR every frame.
    int mvrAb = 0;
    // The gates' samples (lmu_mvr: first, every, count): the focus views compared, their images saved. Off in a player's
    // run (multi_view): no read-backs.
    bool mvrCompare = false;
    // The CPU in the context hooks timed and logged (lmu_mvr_timing, any A/B run).
    bool mvrTiming = false;
    // QVFR: the focus views take their eye's glare (glare_share.h; [experiment] lmu_glare_share, default on).
    bool glareShare = true;
    // MVR ([experiment] lmu_focus_replay, default off): each eye pass also post-processes its focus view (focus_post.h:
    // YEBIS's copy and composite once more, with the eye's glare and exposure), its final blit beside the eye and the
    // overlays doubled into it (R3).
    bool focusReplay = false;
    // R4a ([experiment] lmu_focus_final, with focusReplay): the game's two eye passes only; the focus views are those
    // focus finals beside the eyes. lmu_focus_final_ab=N: N frames like that, then N with the four passes (gate D), in
    // turn, each side's CPU and GPU per pass logged apart.
    bool focusFinal = false;
    int focusFinalAb = 0;
    // R5c ([experiment] lmu_focus_final_ab_plain=1, with focusFinalAb): the A/B's four-pass side is plain QVFR (no
    // multi-view: passes 2-3 render their scene, no focus post), as a player without multi-view has it, instead of gate D.
    bool focusFinalAbPlain = false;
    // R4b's pass tracker, observe only ([experiment] lmu_pass_tracker, with focusReplay): the eye passes found at the
    // d3d11 level compared with the game's in the log (mvr_pass.h mvr_track_passes); MVR's hooks from the session's
    // first frame end, so the menus are seen too.
    bool passTracker = false;
    // R4b-2 ([experiment] lmu_pass_tracker=2, with focusFinal): the tracker drives the passes; no game code hooked (R6:
    // the lens mask and the VR UI from their d3d11 draws; no scene render, eye loop, post-effect time, scene skip, glare
    // or driver tags hooks; no A/B: the game's own two passes).
    bool trackerDrives = false;
    // R5: the scene frame at which the multi-view path is made to fall back to the four passes ([experiment]
    // lmu_mvr_fallback_at, a test of the fallback). 0: only when it fails.
    int fallbackAt = 0;
    // The two ways to draw the focus views, compared in one run ([experiment] lmu_mvr_backend_ab=N, with trackerDrives on
    // an NVIDIA RTX 20+ card, where both twins are attached): N frames with NVIDIA's single-pass stereo, then N with
    // instancing, in turn; each side's CPU per pass logged apart, its GPU per pass by mvr_pass's per-mode lines.
    int backendAb = 0;
};

// Installs the four passes on this build (hash and anchors checked); false and a log line otherwise. Focus views render
// into the eye targets and are set aside before the game submits, so the headset shows the eyes (gates) or, composed,
// all four (QVFR).
bool four_passes_start(const FourPassesRun& run);
// Every xrEndFrame: counts the passes; gates: at a few frames on track compares passes 2-3 with eyes 0-1 (copies: pixel
// for pixel; focus: each focus view with its eye's crop).
void four_passes_frame_end();
// xrCreateSession with a D3D11 binding: the game's device (the pass tracker installs MVR's hooks on it at a frame end).
void four_passes_device(ID3D11Device* device);

// ---- QVFR (run.qvfr) ----
// Where each eye's focus box is centred, as tangents of that eye's view (`valid[e]` false: its forward axis), from the
// next frame; kept inside the eye. Any thread.
void four_passes_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]);
// The box's share of each eye's tangent width and height (fractionY <= 0: as fractionX), from the next frame. Any thread.
void four_passes_set_box(double fractionX, double fractionY);
// The latest composed frame's record (the eyes' and focus views' tangents, unturned); false before the first.
bool four_passes_record(simqvfr::Views4Frame& frame);
// The frame being submitted, all four views in one texture (adapter.h qvfr_views_atlas): the game's submitted eye
// images (`eyes`) copied into its top row, the focus views below. Null when this frame has no four views (menus,
// loading, a focus pass refused). xrEndFrame, on the render thread.
ID3D11Texture2D* four_passes_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]);
// The size every view renders at (the eye targets'), 0 before the first four-pass frame.
void four_passes_view_size(int (&size)[2]);
// Live resolution: the outer and focus views at these shares (0.1-1) of the launch-size targets, per axis, from the
// next frame. LMU's render scale is set per pass, so each pass renders into the top-left of its target; the copies
// and the atlas regions follow the sizes rendered. Any thread.
void four_passes_set_live_scales(double outer, double focus);
// The same per axis (FourPassesRun::perAxis): the outer and focus views' shares of the target's width and height.
void four_passes_set_live_axes(double outerWidth, double outerHeight, double focusWidth, double focusHeight);
// Every xrLocateViews: the FOV the game just located (with the layer's FOV crop). The game builds its lens mask in
// screen space with the FOV located when it fetches the mesh (VR_Init: four_passes_mask_taken), so while the located
// FOV differs from that one (a live crop change) the mask draw is skipped: it would black out what the lens shows.
void four_passes_note_views(const XrView* views, uint32_t count);
void four_passes_mask_taken(uint32_t viewIndex);

}  // namespace lmu

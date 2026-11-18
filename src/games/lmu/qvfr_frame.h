// LMU's QVFR frame (until 2026-11-12 four_passes, the eye loop run four times; that path is gone): the game's
// own two eye passes draw their focus views too (multi-view rendering, mvr_pass.h), found and driven at the d3d11 level
// (the pass tracker), and the layer composes all four views from the eyes' images (the focus finals beside the eyes).
// No game code is hooked or patched and nothing is written into the game: the live resolution (the eyes at the
// periphery's size inside their targets) is the eye passes' viewports and scissors cut at the d3d11 level (mvr_size_cut).
#pragma once

#include <cstdint>

#include "../../quad/focus_mask.h"
#include "../adapter.h"

struct ID3D11Texture2D;

namespace lmu {

// A run's settings ([experiment] keys, lmu/adapter.cpp).
struct QvfrFrameRun {
    bool qvfr = false;           // QVFR: composed by the layer
    double fraction = 0.3;       // the focus box: share of each eye's tangent width
    double fractionY = 0;        // and height (<= 0: as `fraction`)
    // QVFR: frames without the eye loop (menus) composed at the native eye with the UI drawn at that size
    // ([experiment] lmu_menu_native, default on); the views' layer in native eye widths and heights (the native size
    // is the size offered over it).
    bool menuNative = true;
    double layerWidth = 1, layerHeight = 1;
    // Multi-view rendering (mvr_pass.h): the eye passes draw their focus views too, beside the eye.
    bool mvr = false;
    // The focus views beside their eyes in images of the outer views' shape (Views4Layout::secondViewBeside): the eyes at
    // one share on both axes, the focus views at their own width and height (qvfr_frame_set_live_axes), drawn by their
    // eye's pass.
    bool perAxis = false;
    // The CPU in the context hooks timed and logged (lmu_mvr_timing, any A/B run).
    bool mvrTiming = false;
    int passTrace = 0;  // [experiment] lmu_pass_trace (mvr_pass.h mvr_pass_trace)
    int postCensus = 0;  // [experiment] lmu_post_census (mvr_pass.h mvr_post_census)
    // MVR ([experiment] lmu_focus_replay, default off): each eye pass also post-processes its focus view (focus_post.h:
    // YEBIS's copy and composite once more, with the eye's glare and exposure), its final blit beside the eye and the
    // overlays doubled into it (R3).
    bool focusReplay = false;
    // [experiment] lmu_ui_mips (default 1): the VR UI panel's doubled draws sample a copy of its canvas with mips
    // (mvr_pass.h mvr_ui_mips); 0: the game's canvas (one mip) as it is.
    bool uiMips = true;
    // The focus views are the focus finals beside the eyes (the game's two eye passes only).
    bool focusFinal = false;
    // R4b's pass tracker, observe only ([experiment] lmu_pass_tracker, with focusReplay): the eye passes found at the
    // d3d11 level compared with the game's in the log (mvr_pass.h mvr_track_passes); MVR's hooks from the session's
    // first frame end, so the menus are seen too.
    bool passTracker = false;
    // R4b-2 ([experiment] lmu_pass_tracker=2, with focusFinal): the tracker drives the passes; no game code hooked (R6:
    // the lens mask and the VR UI from their d3d11 draws; no scene render, eye loop, post-effect time, scene skip, glare
    // or driver tags hooks; no A/B: the game's own two passes).
    bool trackerDrives = false;
    // Texture sharpening ([dlss] texture_sharpening) on at launch: its pixel shader hook is installed (mvr_pass.h
    // mvr_mip_bias_hooks), and the probe makes the twins. Off at launch: neither, at no cost; turned on, the next launch.
    bool textureSharpening = false;
    // Other cars' motion vectors, stage 2 ([experiment] lmu_car_track=1, log only): each car part's world matrix tracked
    // frame to frame from the eye passes' draws (mvr_pass.h mvr_car_track); nothing drawn changes.
    bool carTrack = false;
    // Stage 3 ([experiment] lmu_car_vectors, with carTrack): 1 the parts' slots written beside the scene and a gate
    // image; 2 DLSS's vectors take the parts' motion too (mvr_pass.h mvr_car_slots).
    int carSlots = 0;
    // The player's [dlss] car_vectors go on only where DLSS can run (a build with NGX, an RTX card, the tracker's
    // passes); the [experiment] keys (carForced) as they say. carAb (lmu_car_vectors_ab=N): N frames with the cars'
    // motion, N without (mvr_pass.h mvr_car_active), in turn; each side's CPU per pass logged, GPU per mvr_pass's modes.
    bool carForced = false;
    int carAb = 0;
    // The recovery's test ([experiment] lmu_mvr_lose_at=N): from scene frame N the focus post is off for 1800 scene
    // frames (no focus finals, as with a pipeline it doesn't handle), then on again. 0: never.
    int loseAt = 0;
    // With loseAt ([experiment] lmu_mvr_lose_shape=1): the frame's shape reported not ready through that window, as
    // with a pipeline that lacks a step: no try, so no blink, until the window ends.
    bool loseShape = false;
    // The two ways to draw the focus views, compared in one run ([experiment] lmu_mvr_backend_ab=N, with trackerDrives on
    // an NVIDIA RTX 20+ card, where both twins are attached): N frames with NVIDIA's single-pass stereo, then N with
    // instancing, in turn; each side's CPU per pass logged apart, its GPU per pass by mvr_pass's per-mode lines.
    int backendAb = 0;
    // What the lens mask saves ([experiment] lmu_mask_ab=N, a measurement; with views4_test_hidden_mask=2 the mask is the
    // focus box's cut corners): N frames with the game's mask draw, N without, in turn; GPU per pass by mvr_pass's modes.
    int maskAb = 0;
    // [experiment] lmu_build_free=1: the game's facts (offered size, render scale, FSR, eye projections, eye of a pass)
    // from OpenXR and Direct3D on a build abi.h knows too, as on any other build (qvfr_frame.cpp "any build").
    bool buildFree = false;
};

// The game's exe is a build abi.h's addresses are from (game.h kBuilds; hashed once). Any other build runs QVFR from
// what OpenXR and Direct3D show, with nothing read in the game (DLSS's camera: dlss_inputs.cpp, view_pair.h).
bool qvfr_frame_known_build();
// This run takes the game's facts from OpenXR and Direct3D (an unknown build, or lmu_build_free=1); set by
// qvfr_frame_start.
bool qvfr_frame_build_free();
// Eye `eye`'s projection from the FOV the game was last handed (focus_math.h projection_from_tangents: its x and y
// terms only). False before the first.
bool qvfr_frame_eye_projection(int eye, float (&out)[16]);
// The stereo views' size the layer offered the game (xrEnumerateViewConfigurationViews, after the layer's own change):
// the eye targets' size on any build.
void qvfr_frame_offered(std::uint32_t width, std::uint32_t height);
// The FOV the game was handed at xrLocateViews, after everything the layer does to it (the crop, DLSS's jitter): its
// eye projections on any build.
void qvfr_frame_game_views(const XrView* views, uint32_t count);

// Starts QVFR when the run has multi-view's room (run.trackerDrives), on any build; false and a log line otherwise. The focus views are drawn beside the eyes by their eye passes and composed with them by the layer.
bool qvfr_frame_start(const QvfrFrameRun& run);
// Every xrEndFrame: counts the passes, multi-view's health and recovery, the frame's eye extent and the overlay's badge.
void qvfr_frame_frame_end();
// How the focus views were drawn as of the last frame end (the overlay's badge): NVIDIA's multi-view rendering, the
// software multi-view (instancing, or instancing taken over from NVIDIA's), the eyes alone, or none (multi-view off at
// launch, or not running yet). Any thread.
simqvfr::games::QvfrMultiView qvfr_frame_multi_view();
// xrCreateSession with a D3D11 binding: the game's device (the pass tracker installs MVR's hooks on it at a frame end).
void qvfr_frame_device(ID3D11Device* device);

// ---- QVFR (run.qvfr) ----
// Where each eye's focus box is centred, as tangents of that eye's view (`valid[e]` false: its forward axis), from the
// next frame; kept inside the eye. Any thread.
void qvfr_frame_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]);
// The box's share of each eye's tangent width and height (fractionY <= 0: as fractionX), from the next frame. Any thread.
void qvfr_frame_set_box(double fractionX, double fractionY);
// The focus mask (mvr_pass.h mvr_focus_mask), from the next frame. Any thread.
void qvfr_frame_set_focus_mask(bool on, const simqvfr::FocusArea& area, unsigned marginTexels);
// The latest composed frame's record (the eyes' and focus views' tangents, unturned); false before the first.
bool qvfr_frame_record(simqvfr::Views4Frame& frame);
// A frame with eye passes but no four views (the focus views didn't come): the part of each eye's image its pass
// rendered, from the top-left, in pixels. The layer submits that part in place of the whole image, where the eye would
// show small in a corner. False: nothing to cut (four views this frame, a frame without passes, the eyes whole). Once a
// frame, at xrEndFrame after qvfr_frame_frame_end.
bool qvfr_frame_eye_extent(unsigned (&width)[2], unsigned (&height)[2]);
// The frame being submitted, all four views in one texture (adapter.h qvfr_views_atlas): the game's submitted eye
// images (`eyes`) copied into its top row, the focus views below. Null when this frame has no four views (menus,
// loading, a focus pass refused). xrEndFrame, on the render thread.
ID3D11Texture2D* qvfr_frame_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]);
// The size every view renders at (the eye targets'), 0 before the first four-pass frame.
void qvfr_frame_view_size(int (&size)[2]);
// Live resolution: the outer and focus views at these shares (0.1-1) of the launch-size targets, per axis, from the
// next frame. LMU's render scale is set per pass, so each pass renders into the top-left of its target; the copies
// and the atlas regions follow the sizes rendered. Any thread.
void qvfr_frame_set_live_scales(double outer, double focus);
// The player's DLSS (or FSR) and its other cars' motion as they are now (live, any thread): with either off the cars'
// tracking pauses at the next frame (its hooks, installed at launch, stay in until the session ends), and resumes when
// both are on again. Not while lmu_car_vectors_ab alternates it.
void qvfr_frame_cars_wanted(bool wanted);
// The same per axis (QvfrFrameRun::perAxis): the outer and focus views' shares of the target's width and height.
void qvfr_frame_set_live_axes(double outerWidth, double outerHeight, double focusWidth, double focusHeight);
// Every xrLocateViews: the FOV the game just located (with the layer's FOV crop). The game builds its lens mask in
// screen space with the FOV located when it fetches the mesh (VR_Init: qvfr_frame_mask_taken), so while the located
// FOV differs from that one (a live crop change) the mask draw is skipped: it would black out what the lens shows.
void qvfr_frame_note_views(const XrView* views, uint32_t count);
void qvfr_frame_mask_taken(uint32_t viewIndex);

}  // namespace lmu

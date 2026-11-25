// What the layer asks of a game's engine code (src/games/README.md): QVFR, the game rendering two outer
// and two focus views in one frame, and the game's own experiments. Layer only (OpenXR types); the
// launcher needs just game.h. The layer holds the adapter for the game it runs in (adapter_for); in any
// other process it holds one that does nothing and whose QVFR never starts.
#pragma once

#include <cstdint>

#include "../engine/dlss_settings.h"
#include "../engine/nr_settings.h"
#include "../quad/focus_mask.h"
#include "../quad/focus_images.h"
#include "../quad/views4_layout.h"
#include "../xr.h"
#include "game.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace simqvfr {

// What one game frame's views were rendered with, recorded when the game fills the focus cameras: per
// eye the outer view's tangents (left, right, up, down; read back from the game's own eye projection, so
// they hold whatever FOV it used), the focus view's tangents, and the focus camera's turn (in the eye's
// frame). The composition maps the views with exactly these, on any headset.
struct Views4Frame {
    float outerTan[2][4];
    float focusTan[2][4];
    XrQuaternionf turn[2];
    bool live = false;  // `width`/`height`: the view layout in the game's output (live resolution, views4_layout.h)
    double width[4] = {}, height[4] = {};  // what the composition reads; with DLSS, its output layout
    double renderWidth[4] = {}, renderHeight[4] = {};  // the views rendered (DLSS's input; else the same)
    // The game drew only its two eyes this frame, each over its whole half of the target (X-Plane's menu): nothing to
    // compose, the layer submits it as it is.
    bool eyesOnly = false;
};

namespace games {

// QVFR's launch settings, all known at xrCreateInstance.
struct QvfrStart {
    Views4Layout layout;           // where the four views sit in the game's target (views4_layout.h)
    double focusFraction = 0;      // the focus box: share of each eye's tangent width (0: the eyes' own cameras)
    double focusFractionY = 0;     // and height (<= 0: as focusFraction)
    bool dlssLive = true;          // live resolution resizes the views below DLSS (DLSS dynamic resolution)
    bool focusCull = false;        // the focus views skip draws outside their frustum (focus_cull.h)
    double focusCullScale = 1;     // the cull box's extent, a multiple of the focus box
    bool focusCullGaze = false;    // the cull box follows the gaze
    const wchar_t* ini = nullptr;  // the game's own [experiment] diagnostics are read from here
};

// How a game's focus views are drawn, for the performance overlay's badge: no multi-view path (a game without one, or
// it off at launch), NVIDIA's multi-view rendering (each focus view drawn in its eye's pass), Sim-QVFR's software
// multi-view (the same by instancing: any GPU), or multi-view on but given way to the focus views' own passes (it fell
// back: the driver refused, LMU's FSR on, a device that can't).
// eyesOnly: the multi-view path is on but no focus views are being made (the eyes alone, at full resolution).
enum class QvfrMultiView { none, nvidia, software, fallback, eyesOnly };

class GameAdapter {
public:
    virtual ~GameAdapter() = default;
    virtual const GameInfo* info() const = 0;  // null: not a supported game
    // False for a game known by its identity alone (being reverse engineered): the layer only logs there,
    // as mode=off, and puts nothing of its own into the game or the headset.
    virtual bool has_qvfr() const { return true; }
    // xrEnumerateViewConfigurationViews (stereo): the size each eye's view was offered to the game, after the layer's
    // own change to it (LMU: its eye targets' size, on any game build).
    virtual void views_offered(uint32_t width, uint32_t height) {
        (void)width;
        (void)height;
    }
    // True: a game without the layer's views4 QVFR (has_qvfr false) that runs QVFR on a path of its own below the layer
    // (the SCS games' D3D11 corner), decided from `ini` at load, before the adapter's instance_created. The layer then
    // still runs as mode=off, but its reports say QVFR and the CPU settings apply.
    virtual bool own_qvfr(const wchar_t* ini) const {
        (void)ini;
        return false;
    }
    // False while the layer can't compose this game's views (its graphics API isn't supported yet): QVFR then
    // runs uncomposed, the headset showing views 0-1.
    virtual bool qvfr_composable() const { return has_qvfr(); }
    // The layer composes this game's views with its D3D11 compositor instead (d3d11_compositor.h): one image
    // holds all four views in eye columns (qvfr_eye_columns), each focus view placed by its record (turned or off-centre).
    virtual bool qvfr_compose_d3d11() const { return false; }
    // Cheeky Foveated DLSS runs in this game (GameInfo::features.foveatedDlss).
    virtual bool loads_cheeky() const { return has_qvfr() && info() && info()->features.foveatedDlss; }
    // The game lays its views out in eye columns (views4_rect_eye_columns: outer views over focus views, each pair
    // side by side from x = 0) and submits each eye as its half of the target: the layer offers that layout's eye
    // size, and keeps each eye's offset, moved and cut to its outer view.
    virtual bool qvfr_eye_columns() const { return false; }
    // The game draws all four views at one size (instanced layers in one viewport): each eye column is one view wide
    // and two high, a view being the larger of the outer and focus sizes asked for (the layer offers that).
    virtual bool qvfr_one_view_size() const { return false; }
    // The game renders every view as an image of its own and submits each eye's in a swapchain of its own (LMU: the eye
    // loop runs four times through targets of one size): the layer offers that one view size as each eye's (the larger of
    // the outer and focus sizes asked for) and composes from the adapter's atlas (qvfr_views_atlas).
    virtual bool qvfr_separate_views() const { return false; }
    // With qvfr_separate_views: the four views of the frame being submitted in one texture, with the game's two submitted
    // eye images (`eyes`, as the game left them after its eye loop) copied into it, and each view's region in it
    // (u0, v0, du, dv; view e the outer view of eye e, e + 2 its focus view). Called in xrEndFrame on the game's render
    // thread (its immediate context). Null: nothing to compose this frame (the eyes are submitted as they are).
    // With qvfr_separate_views, a frame qvfr_views_atlas has no four views for: the part of each eye's image the game
    // rendered this frame, from its top-left, in pixels; the layer submits that part. False: the eyes whole.
    virtual bool qvfr_eye_extent(unsigned (&width)[2], unsigned (&height)[2]) {
        (void)width;
        (void)height;
        return false;
    }
    virtual ID3D11Texture2D* qvfr_views_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) {
        (void)eyes;
        (void)uv;
        return nullptr;
    }
    // False: the game uses no OpenXR input (no xrAttachSessionActionSets, LMU), so the layer attaches its own gaze and
    // menu actions alone at xrBeginSession; otherwise they ride along with the game's (one attach per session).
    virtual bool attaches_action_sets() const { return true; }
    // [experiment] hang_watch_s when the ini doesn't set it (probe/hang_watch.h): the longest the game goes
    // without a frame on its own, when loading, plus a margin.
    virtual unsigned hang_watch_s() const { return 30; }

    // ---- QVFR ----
    // At xrCreateInstance, before the game makes its renderer. False: the game's code isn't the build the
    // adapter knows, nothing was changed, and the layer stays off (the game runs as normal).
    virtual bool qvfr_start(const QvfrStart& start) = 0;
    // Where each eye looks, as tangents (x, y) of that eye's view, at every xrLocateViews (`valid[e]` false:
    // no turn). `gazeSample`: from a gaze sample, not the untracked default.
    virtual void qvfr_set_gaze(const float (&tan)[2][2], const bool (&valid)[2], bool gazeSample) = 0;
    // Live resolution: `live` (views4_live) from the next frame the game fills on.
    virtual void qvfr_set_live_layout(const Views4Layout& live) = 0;
    // Live resolution works this run (GameInfo::features.liveResolution, unless the adapter knows better once QVFR
    // started: a game whose views resize live only with an experiment on).
    virtual bool qvfr_live_resolution() const { return info() && info()->features.liveResolution; }
    // Live focus box and focus cull, from the next frame the game fills on.
    virtual void qvfr_set_live(double focusFraction, double focusFractionY, double cullScale, bool gaze) = 0;
    // The focus mask ([quad] focus_mask; quad/focus_mask.h), live: the focus shape as the compositor composes it (the
    // free-form outline's points, or the box's corner radius; its aspect is the game's to set per view) and the margin
    // in focus view texels. A game that can mask its focus views skips their texels outside it.
    virtual void qvfr_set_focus_mask(bool on, const FocusArea& area, int marginTexels) {
        (void)on;
        (void)area;
        (void)marginTexels;
    }
    // Road focus ([quad] focus_corridor; GameFeatures::focusCorridor), at launch and live: the focus views kept to a
    // corridor along the road (widthM metres either side of its centre: focus_corridor_width; heightDm tenths of a metre
    // above it: focus_corridor_height; opening by spreadDeg degrees either side with distance: focus_corridor_spread)
    // and the player's car, its edges blended over blendDm tenths of a metre near
    // (focus_corridor_blend), what it takes from the periphery tinted with `show` (focus_corridor_show). `on`
    // is the player's switch with fixed foveation (the layer passes off while eye tracking is on).
    // The part of the focus box the headset shows (shares of the box: left, top, right, bottom; all of it: 0 0 1 1), live:
    // under native quad views the runtime's own focus region. A game that masks its focus views skips the rest too.
    virtual void qvfr_set_focus_visible(const float (&visible)[4]) { (void)visible; }
    virtual void qvfr_set_focus_corridor(bool on, int widthM, int heightDm, int spreadDeg, int blendDm, bool show) {
        (void)on;
        (void)widthM;
        (void)heightDm;
        (void)spreadDeg;
        (void)blendDm;
        (void)show;
    }
    // QVFR: object and tree detail, percent (GameFeatures::objectDetail; [experiment] object_detail), at launch and
    // live. 100: the LOD the focus views' pixel density needs, 0: the game's own for its render size.
    virtual void qvfr_set_object_detail(int percent) { (void)percent; }
    // QVFR: Sim-QVFR's own DLSS ([dlss], GameFeatures::sideLoadedDlss; engine/dlss_settings.h), live: called with the
    // ini's at every reload; the adapter applies what changed (the DLL's folder at the next launch).
    virtual void qvfr_set_dlss(const DlssSettings& settings) { (void)settings; }
    // QVFR: DLSS-NR on Sim-QVFR's own DLSS views ([dlss_nr], engine/nr_settings.h), live: called as qvfr_set_dlss is.
    virtual void qvfr_set_nr(const NrSettings& settings) { (void)settings; }
    // The game upscales with DLSS and the views keep their launch sizes (live resolution: next launch).
    virtual bool qvfr_dlss_held() = 0;
    // The game's render and output target sizes (width, height) as last laid out (0 before the first frame).
    virtual void qvfr_target_sizes(int (&render)[2], int (&output)[2]) = 0;
    // The record of the next frame to compose, in rendering order; false before the first.
    virtual bool qvfr_take_frame(Views4Frame& frame) = 0;
    // The latest record, without taking it.
    virtual bool qvfr_latest_frame(Views4Frame& frame) = 0;
    // A copies gate this launch (the game's [experiment] keys in `ini`, read before instance_created): the layer lays
    // views 2-3 out at the outer views' size, so each can be compared with its eye pixel for pixel (ACC's gates.h).
    virtual bool qvfr_copies_layout(const wchar_t* ini) const {
        (void)ini;
        return false;
    }
    // How far the eyes look, in metres, from the game's depth at the gaze point (0: unknown): the layer aims both eyes'
    // focus boxes at that point so they converge where the eyes do (quad/focus_converge.h). Any thread.
    virtual double qvfr_fixation_m() const { return 0; }
    // The game's own image at every xrEndFrame (D3D11, eye columns: ACC's gates): the layer keeps track of the game's
    // swapchain images and hands over the one its projection layer submits, with each view's rect in it (x, y, width,
    // height) as the layer submits and composes them, before composing. On the game's render thread.
    virtual bool qvfr_wants_game_image() const { return false; }
    virtual void qvfr_game_image(ID3D11Texture2D* image, const int (&rects)[4][4]) {
        (void)image;
        (void)rects;
    }
    // The views the game just located (diagnostics; LMU: the FOV its lens mask is built with).
    virtual void qvfr_note_views(const XrView* views, uint32_t count) = 0;
    // The game's stereo views as it is about to get them, after everything the layer reads from them: a game whose
    // temporal anti-aliasing is ours (LMU's DLAA) moves their FOV by its sub-pixel jitter here. The layer composes with
    // the FOV as it was before.
    virtual void qvfr_jitter_views(XrView* views, uint32_t count) {
        (void)views;
        (void)count;
    }
    // The game fetched view `viewIndex`'s hidden-area mesh (xrGetVisibilityMaskKHR with room for it): a game that turns
    // it into screen space with the FOV it just located (LMU) has a mask for that FOV only.
    virtual void visibility_mask_taken(uint32_t viewIndex) { (void)viewIndex; }
    // Every xrEndFrame while QVFR runs (its counters into the log).
    virtual void qvfr_frame_end() = 0;
    // How the focus views were drawn as of the last frame (the overlay's badge). Any thread.
    virtual QvfrMultiView qvfr_multi_view() const { return QvfrMultiView::none; }
    // Night car lighting's cost per frame (GameFeatures::carLighting; the overlay): GPU and CPU ms, averaged over a short
    // window. False: not running, or no average yet. Any thread.
    virtual bool qvfr_car_light_costs(double& gpuMs, double& cpuMs) const {
        (void)gpuMs;
        (void)cpuMs;
        return false;
    }
    // The game's own upscaler as the overlay shows it (engine/dlss_views.h: "DLSS", or AMD FSR's version), and how many
    // views it has evaluated this session (the card's DLSS mark when it changed). False: the game has none of ours.
    // `badge` lives for the process. Any thread.
    virtual bool qvfr_upscaler(const wchar_t*& badge, unsigned long long& evaluations) const {
        (void)badge;
        (void)evaluations;
        return false;
    }

    // ---- The game's own experiments and watches, each reading its own ini keys ----
    // The layer was loaded at the game's first vkCreateInstance (GameInfo::earlyLoad, SIM_QVFR_EARLY=1): from DllMain,
    // under the loader lock, after the game's static initialisers and before it makes any shader. `ini`: SIM_QVFR_INI.
    virtual void early_start(const wchar_t* ini) { (void)ini; }
    // xrCreateApiLayerInstance, before qvfr_start. `mode`: [experiment] mode in effect (baseline=1: "off").
    virtual void instance_created(const wchar_t* ini, const wchar_t* mode) {
        (void)ini;
        (void)mode;
    }
    // xrCreateSession; `probed`: the engine probes started (probe.h).
    virtual void session_created(bool probed) { (void)probed; }
    // xrCreateSession with a D3D11 binding: the game's device. Called when the layer composes D3D11 views, or when
    // observes_d3d11() asks for it (a game's log-only D3D11 probes, ATS).
    virtual void d3d11_session(ID3D11Device* device) { (void)device; }
    virtual bool observes_d3d11() const { return false; }
    // A game without the layer's QVFR pipeline that follows the gaze itself (ATS's API-level focus view): the layer
    // sets its gaze actions up and calls qvfr_note_views and qvfr_set_gaze at every stereo xrLocateViews.
    virtual bool wants_gaze() const { return false; }
    // A game whose QVFR runs through none of the layer's own methods (no compositor of ours: the game's own layers go
    // on) but still shows the overlay and the headset menu, and takes the menu's sizes live: the periphery's and the
    // focus box's shares of the eye (0..1) and eye tracking on or off, at every live read of the ini.
    virtual bool shows_overlay() const { return false; }
    struct LiveView {
        double periphery = 1;           // the eye's own density, a share of native (0..1)
        double focusX = 0.3, focusY = 0.3;  // the focus box's shares of the eye's screen
        double density = 1;             // the focus view's density, a share of native
        double feather = 0.1;           // the box's blended edge, a share of half its shorter side
        double cornerRadius = 0;        // rounded corners, a share of half its shorter side
        bool border = false;            // the outline
        float borderColor[4] = {0, 1, 0, 1};
        bool gaze = true;               // eye tracking
    };
    virtual void live_view(const LiveView& view) { (void)view; }
    // The focus box's look at launch (the ini's feather, corner radius, outline and its colour), before any live_view: a
    // game composing its own box (the SCS corner) starts from it, not from defaults until the ini first changes.
    virtual void qvfr_set_look(double feather, double cornerRadius, bool border, const float (&color)[4]) {
        (void)feather;
        (void)cornerRadius;
        (void)border;
        (void)color;
    }
    // The OpenXR session began (true: xrBeginSession succeeded) or is being created or destroyed (false). Until
    // it begins, the runtime may have no views to answer with.
    virtual void session_running(bool running) { (void)running; }
    virtual void session_destroyed() {}
    // Every xrEndFrame.
    virtual void frame_end() {}
    // Every xrEndFrame of a game without the layer's own submission (identity games): the frame to submit, `info` itself
    // or a copy the adapter keeps until its next call (ATS's corner: each eye image's rectangle cut).
    // xrAcquireSwapchainImage succeeded for one of the game's swapchains (`enumerate`: the next layer's
    // xrEnumerateSwapchainImages), and xrReleaseSwapchainImage is about to hand an image back to the runtime: the last
    // moment to change it (ATS's corner scales its eye back over the whole image there).
    virtual void swapchain_acquired(XrSwapchain chain, uint32_t index, PFN_xrEnumerateSwapchainImages enumerate) {
        (void)chain;
        (void)index;
        (void)enumerate;
    }
    virtual void swapchain_releasing(XrSwapchain chain) { (void)chain; }
    // xrDestroySwapchain for one of the game's swapchains, before it goes on (any thread the game destroys it on):
    // whatever the adapter keeps for `chain` (its images' raw pointers) must go now; the handle may come back for another.
    virtual void swapchain_destroyed(XrSwapchain chain) { (void)chain; }
    // `enumerate`: the next layer's xrEnumerateSwapchainImages (the swapchains' images, if the adapter draws into them).
    virtual const XrFrameEndInfo* adjust_frame_end(const XrFrameEndInfo* info, PFN_xrEnumerateSwapchainImages enumerate) {
        (void)enumerate;
        return info;
    }
    // The focus views drawn by the adapter at the D3D12 level (quad/focus_images.h: ACE's replay); null: not this way.
    virtual FocusImages* focus_images() { return nullptr; }
};

// The adapter for `game` (null: the one that does nothing). Lives for the process.
GameAdapter& adapter_for(const GameInfo* game);

// The layer DLL was loaded early (SIM_QVFR_EARLY=1): the current game's adapter's early_start. From DllMain.
void early_load();

}  // namespace games
}  // namespace simqvfr

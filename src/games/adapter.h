// What the layer asks of a game's engine code (src/games/README.md): QVFR, the game rendering two outer
// and two focus views in one frame, and the game's own experiments. Layer only (OpenXR types); the
// launcher needs just game.h. The layer holds the adapter for the game it runs in (adapter_for); in any
// other process it holds one that does nothing and whose QVFR never starts.
#pragma once

#include <cstdint>

#include "../quad/views4_layout.h"
#include "../xr.h"
#include "game.h"

struct ID3D11Device;

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

class GameAdapter {
public:
    virtual ~GameAdapter() = default;
    virtual const GameInfo* info() const = 0;  // null: not a supported game
    // False for a game known by its identity alone (being reverse engineered): the layer only logs there,
    // as mode=off, and puts nothing of its own into the game or the headset.
    virtual bool has_qvfr() const { return true; }
    // False while the layer can't compose this game's views (its graphics API isn't supported yet): QVFR then
    // runs uncomposed, the headset showing views 0-1.
    virtual bool qvfr_composable() const { return has_qvfr(); }
    // The layer composes this game's views with its D3D11 compositor instead (d3d11_compositor.h): one image
    // holds all four views in eye columns (qvfr_eye_columns), each focus view placed by its record (turned or off-centre).
    virtual bool qvfr_compose_d3d11() const { return false; }
    // Cheeky Foveated DLSS runs in this game (GameInfo::features.foveatedDlss).
    virtual bool loads_cheeky() const { return has_qvfr() && info() && info()->features.foveatedDlss; }
    // The game lays its views out in eye columns (views4_rect_eye_columns) and submits each eye as its half of
    // the target: the layer offers that layout's eye size, and keeps each eye's offset, cut to its outer view.
    virtual bool qvfr_eye_columns() const { return false; }
    // The game draws all four views at one size (instanced layers in one viewport): each eye column is one view wide
    // and two high, a view being the larger of the outer and focus sizes asked for (the layer offers that).
    virtual bool qvfr_one_view_size() const { return false; }
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
    // QVFR: object and tree detail, percent (GameFeatures::objectDetail; [experiment] object_detail), at launch and
    // live. 100: the LOD the focus views' pixel density needs, 0: the game's own for its render size.
    virtual void qvfr_set_object_detail(int percent) { (void)percent; }
    // The game upscales with DLSS and the views keep their launch sizes (live resolution: next launch).
    virtual bool qvfr_dlss_held() = 0;
    // The game's render and output target sizes (width, height) as last laid out (0 before the first frame).
    virtual void qvfr_target_sizes(int (&render)[2], int (&output)[2]) = 0;
    // The record of the next frame to compose, in rendering order; false before the first.
    virtual bool qvfr_take_frame(Views4Frame& frame) = 0;
    // The latest record, without taking it.
    virtual bool qvfr_latest_frame(Views4Frame& frame) = 0;
    // The views the game just located (diagnostics).
    virtual void qvfr_note_views(const XrView* views, uint32_t count) = 0;
    // Every xrEndFrame while QVFR runs (its counters into the log).
    virtual void qvfr_frame_end() = 0;

    // ---- The game's own experiments and watches, each reading its own ini keys ----
    // The layer was loaded into the game before it ran (GameInfo::earlyLoad, SIM_QVFR_EARLY=1): from DllMain, under the
    // loader lock, with the game's main thread not yet started. `ini`: SIM_QVFR_INI.
    virtual void early_start(const wchar_t* ini) { (void)ini; }
    // xrCreateApiLayerInstance, before qvfr_start. `mode`: [experiment] mode in effect (baseline=1: "off").
    virtual void instance_created(const wchar_t* ini, const wchar_t* mode) {
        (void)ini;
        (void)mode;
    }
    // xrCreateSession; `probed`: the engine probes started (probe.h).
    virtual void session_created(bool probed) { (void)probed; }
    // xrCreateSession with a D3D11 binding: the game's device.
    virtual void d3d11_session(ID3D11Device* device) { (void)device; }
    // The OpenXR session began (true: xrBeginSession succeeded) or is being created or destroyed (false). Until
    // it begins, the runtime may have no views to answer with.
    virtual void session_running(bool running) { (void)running; }
    virtual void session_destroyed() {}
    // Every xrEndFrame.
    virtual void frame_end() {}
};

// The adapter for `game` (null: the one that does nothing). Lives for the process.
GameAdapter& adapter_for(const GameInfo* game);

// The layer DLL was loaded early (SIM_QVFR_EARLY=1): the current game's adapter's early_start. From DllMain.
void early_load();

}  // namespace games
}  // namespace simqvfr

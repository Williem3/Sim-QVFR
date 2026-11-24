// A game whose adapter renders the focus views itself at the D3D12 level (ACE's replay, games/ace/replay_*): the game
// draws only its own two eyes, the adapter draws the focus views and hands the layer their image, and the layer
// composes the two (layer/views4.cpp replay_compose). The adapter offers it through GameAdapter::focus_images; a game
// mod through its table's focus_images entry (mod_api.h SqvfrFocusImages). One per process.
#pragma once

#include <cstdint>

#include "focus_mask.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace simqvfr {

// What the layer settles at the session's start ([quad] and [dlss] against the eye the game renders).
struct FocusImagesStart {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;  // the game's queue that presents: the focus views execute on it
    const wchar_t* ini = L"";             // the launch's ini (the adapter reads its own switches there)
    int mode = 3;                         // [experiment] replay_probe: 1/2 cost probes, 3 the focus views
    bool compose = false;                 // the layer composes the focus image with the game's eyes this launch
    bool apiPath = false;                 // the game's QVFR by this path (its own switches default on)
    double boxFraction = 0.2, boxFractionY = 0;  // the focus box: half extent in each eye's NDC (Y 0: as X)
    double pixelScale = 2, pixelScaleY = 0;      // focus pixels per eye pixel (Y 0: as X)
    bool post = false;                    // the focus views' own post (tonemap, bloom, fxaa): needed to compose
    int dlss = 0;                         // the focus views through our DLSS ([experiment] replay_dlss)
    double dlssScale = 1;                 // their render share (1: DLAA)
};

// The focus views' image as the layer composes it.
struct FocusImage {
    ID3D12Resource* image = nullptr;      // both eyes side by side, after the post (the swapchain's format, typeless)
    int state = 0;                        // its D3D12_RESOURCE_STATES
    double box = 0, boxY = 0;             // the focus box's half extent in each eye's NDC
    float offset[2][2] = {};              // each eye's box centre off its forward axis (the gaze this frame used)
};

// What DLSS runs for the eyes and the focus views (the headset menu's note). Not seen until the game made its feature.
struct FocusDlssState {
    bool seen = false, forcedDlaa = false;
    int gameQuality = 0;        // the in-game mode as the game asked (NGX's PerfQuality value)
    unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;  // the eyes' feature as made
    int eyePreset = 0, focusPreset = 0;  // as used (the game's own preset when not overridden)
    bool focusRunning = false;
};

class FocusImages {
public:
    virtual ~FocusImages() = default;
    // xrCreateInstance, before the game makes its device, swap chain or renderer: `start` without the device and queue
    // (its mode, compose and apiPath as the session will start with).
    virtual void early(const FocusImagesStart& start) = 0;
    virtual bool start(const FocusImagesStart& start) = 0;
    virtual void frame_end() = 0;
    // Where each eye's focus box sits, off its forward axis in the eye's NDC (x right, y up), for the next frame.
    virtual void set_gaze(const float (&ndc)[2][2]) = 0;
    // Live: the focus views' output size as a share of the eye each way and their render share (0: unchanged).
    virtual void set_size(double k, double kY, double renderScale) = 0;
    virtual void set_box(double box, double boxY) = 0;
    // The live periphery: the game's eyes drawn at `scale` of their launch size (1: off).
    virtual void set_eye_scale(double scale) = 0;
    // The showcase's Stock: no focus views, the game's eyes at their launch size, uncomposed.
    virtual void set_stock(bool on) = 0;
    // Periphery texture match: the eyes' material textures sharper by `share` of the focus views' mip levels.
    virtual void set_eye_texture_match(double share, double mips) = 0;
    // False before the first focus image.
    virtual bool focus_image(FocusImage& out) = 0;
    // The share of each eye's launch-size image in use (true while the live periphery is in effect).
    virtual bool eye_region(float& fx, float& fy) = 0;
    // Whether this frame was composed (the game's eyes may only shrink while the compositor reads the region in use).
    virtual void eye_allow(bool composed) = 0;
    // GPU time by step: the batches this thread executes from now are the compositor's (true) or the game's.
    virtual void gpu_scope(bool compositor) = 0;
    // Development builds: the draw census (the next `frames` frames into the log) and live diagnostic switches.
    virtual bool census(unsigned frames) = 0;
    virtual void set_live_diag(unsigned bits) = 0;
    // DLSS (FocusImagesStart::dlss): the eyes' and the focus views' presets (0: the game's own), at launch before
    // `early` and live from the headset menu; and what runs.
    virtual void set_dlss_presets(int eye, int focus) = 0;
    virtual bool dlss_state(FocusDlssState& out) = 0;
};

}  // namespace simqvfr

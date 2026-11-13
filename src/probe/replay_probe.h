// Probes for an API-level QVFR (ACE): the game renders only its own two eyes; the layer replays the eyes' command lists
// for the focus views at the D3D12 level.
// Hooks on the shared ID3D12GraphicsCommandList vtable record each list's state and draw calls into a compact stream.
// mode 1/2 (gate G5): at Close, a list that looks like a scene pass (enough draws, a large viewport) is re-emitted on the
//   same thread into a private command list that is closed and never executed: the CPU cost of re-emission. Mode 2
//   re-emits in alternate 900-frame windows (an A/B). Every 900 frames: lists, calls and draws re-emitted per frame, the
//   re-emission's CPU time, the recording hooks' own time, the game's Reset-to-Close time of the same lists.
// mode 3 (R1/R2, replays that draw): a list with two eye-sized viewports side by side (the game's single-pass-stereo
//   scene lists; R1: depth-only ones, R2 `allTargets`: also velocity and colour) is replayed for both focus views at
//   once into twins of its targets: every render or depth target the list binds gets a focus-size twin of ours (same
//   format and samples), and each OMSetRenderTargets of the list is redirected to the twins. The pass constants (root
//   parameter 1) are copied on the GPU into our buffer with an off-axis focus remap of every projection (x' = a x + b w,
//   y' = c y + d w around each eye's own centre); the viewports and scissors are our focus rects. The replay executes on
//   the game's queue right after the batch that holds the game's list. At sample frames the same lists are also replayed
//   unchanged into eye-size reference twins; depth and colour twins are saved (logs\replay) and compared pixel by pixel
//   through the remap. Nothing of the game's is written or transitioned.
#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace simqvfr {

struct ReplayProbeStats {
    uint64_t frames = 0;
    uint64_t listsSeen = 0;       // lists recorded (any)
    uint64_t listsReplayed = 0;   // lists that qualified and were re-emitted
    uint64_t callsReplayed = 0;   // command-list calls re-emitted
    uint64_t drawsReplayed = 0;   // draws and ExecuteIndirects re-emitted
    double replayMs = 0;          // CPU time re-emitting (sum over threads)
    double hookMs = 0;            // CPU time inside the recording hooks (sum over threads)
    double gameMs = 0;            // the game's Reset-to-Close time of the qualifying lists (sum over threads)
    uint64_t focusReplays = 0;    // mode 3: focus replays executed
    uint64_t twinsMade = 0, twinsRemade = 0;  // mode 3, since start (not zeroed): twins made, and made again
    uint64_t tileReplays = 0;     // R2b: focus tile-culling replays executed
    uint64_t fogReplays = 0;      // R2c: focus fog replays executed (both eyes each)
    uint64_t shadowReplays = 0;   // R2c: focus deferred-shadow replays (normals + both eyes' shadow)
    uint64_t monoReplays = 0;     // R2c: per-eye lists replayed into their eye's focus view
    uint64_t oitReplays = 0;      // R2c: OIT composites replayed
    uint64_t atmoReplays = 0;     // R2c: atmosphere resamples (8 maps each)
    uint64_t postReplays = 0;     // P1: focus post replays (bloom, tonemap, fxaa)
    uint64_t blurReplays = 0;
    uint64_t dlssEvaluations = 0;  // P2a: our focus DLSS evaluations (both eyes)
    uint64_t skyPatches = 0;      // focus sky draws given our cbSky (clouds through the box)     // P1: focus refraction blurs (opaqueSceneBlur: our colour + the eye's mips resampled)
};

// The focus replay's settings (mode 3).
struct ReplayFocusSettings {
    double boxFraction = 0.20;    // the focus box's half extent in each eye's NDC (0.2: 20% of the eye's tangent extent)
    double pixelScale = 2.0;      // focus pixels per eye pixel
    uint64_t saveFirst = 1800;    // the first sample frame (xrEndFrame count), then every `saveEvery`, `saveCount` times
    uint64_t saveEvery = 900;
    uint32_t saveCount = 3;
    bool allTargets = false;      // R2: replay every stereo scene list (depth, velocity, colour), not only depth-only ones
    bool tiles = false;           // R2b: the focus views' own light tiles (ACE's tileFrustums + tiledCulling replayed)
    int fog = 0;                  // R2c: the focus views' own volumetric fog grids (needs `tiles`: its descriptors):
                                  // 1 ACE's fog passes replayed with focus constants; 2 ACE's grids resampled through
                                  // each focus box (its values, far scatter included, at the focus positions)
    int shadows = 0;              // R2c: the focus views' own deferred sun shadow (needs `tiles`: its depth view); 2: A/B,
                                  // the lighting uses it at samples 1, 3, ... and not at 2, 4, ... (replayed either way)
    int transparents = 0;         // R2c: the per-eye lists (transparents: one eye-size viewport) into that eye's focus
                                  // view, and the order-independent-transparency composite into the focus colour;
                                  // 2: A/B, replayed before samples 1, 3, ... and not before 2, 4, ...
    bool post = false;            // P1: the focus views' post: the eye's bloom through the box, the game's tonemap and
                                  // fxaa replayed on the focus colour (exposure, grading from the game's own)
    int dlss = 0;                 // P2a: our NVIDIA DLSS per focus view (NGX's exports, inside the game's NGX session),
                                  // fed by ACE's velocity packing replayed with focus constants (needs `post`)
    bool sky = false;             // the sky draw's screen-space clouds: its cbSky (a CBV table entry) patched to name our
                                  // resampled clouds in the focus replay (needs `tiles`)
    bool costAb = false;
    int diagMode = 0;             // replay_diag value (2: our MSAA refraction source only before samples 1, 3, ...)
    bool diag = false;            // a per-sample schedule: sample 1 as set, 2 our fog off, 3 our 3D atmosphere maps on,
                                  // 4 also the sky maps (needs `atmosphere` for the maps to exist)          // cost A/B: every focus replay on for 2 windows (1800 frames), off for 2, ...
    int atmosphere = 0;           // R2c: the atmospheric scattering maps (per eye, screen-shaped) resampled through each
                                  // focus box into focus copies the focus pass constants name (needs `tiles`); 2: A/B,
                                  // named at samples 1, 3, ... and not at 2, 4, ... (resampled either way); 3: the
                                  // sky maps too (the sky draw reads them at screen position)
};

// R2b: the focus inverse projection from an eye's (4 rows as the shader uses them, view = rows . clip): the eye's
// projection inverted, its x/y rows remapped around the eye's own centre (half extent `box` in NDC), inverted back.
// Returns false when the matrix doesn't invert. Exposed for the offline test.
bool replay_focus_inverse_projection(const float (&eye)[16], double box, float (&focus)[16]);

// mode 1/2: re-emission cost (never executed); mode 3: focus depth replay (executed on `queue`).
bool replay_probe_start(ID3D12Device* device, int mode, ID3D12CommandQueue* queue = nullptr,
                        const ReplayFocusSettings& focus = {});
void replay_probe_frame_end();
// At instance creation (before the game makes its D3D12 device and pipelines): the sky patch's hooks (pipeline creation,
// pipeline-library loads, constant-buffer views) on D3D12's shared device vtable, through a throwaway device. The game's
// pipelines come from its PSO cache before the session; hooks installed at the session see none of them.
void replay_probe_early_sky();
bool replay_probe_installed();
// The counters since the last call, then zeroed (tests; the frame-end report uses them too).
ReplayProbeStats replay_probe_take_stats();
// Qualifying thresholds for modes 1/2 (tests use small ones).
void replay_probe_set_thresholds(uint32_t minDraws, float minViewportWidth);
// Mode 3: the last sample's comparison (tests): depth pixels compared and the share that agree within 1% (best shift);
// colour (the first colour twin of a supported format) at that shift.
struct ReplayFocusCompare {
    uint64_t sample = 0;          // how many samples were compared so far
    uint64_t pixels = 0;
    double agree = 0;             // 0..1
    int shiftX = 0, shiftY = 0;
    uint64_t colourPixels = 0;
    double colourAgree = 0;       // 0..1: every channel within 3% (+0.003)
    uint64_t colourEyePixels = 0;  // the same at the eye's pixel size: focus blocks averaged to one eye pixel each
    double colourEyeAgree = 0;     // (detail the eye can't hold doesn't count against the focus view)
    double colourHoodAgree = 0;    // the same blocks within the eye's 3x3 neighbourhood (min..max, + the 3% tolerance):
                                   // edges a pixel apart in sharpness don't count
};
ReplayFocusCompare replay_probe_focus_compare();

}  // namespace simqvfr

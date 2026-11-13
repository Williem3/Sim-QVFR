// Probes for an API-level QVFR (ACE): the game renders only its own two eyes; the layer replays the eyes' command lists
// for the focus views at the D3D12 level.
// Hooks on the shared ID3D12GraphicsCommandList vtable record each list's state and draw calls into a compact stream.
// mode 1/2 (gate G5): at Close, a list that looks like a scene pass (enough draws, a large viewport) is re-emitted on the
//   same thread into a private command list that is closed and never executed: the CPU cost of re-emission. Mode 2
//   re-emits in alternate 900-frame windows (an A/B). Every 900 frames: lists, calls and draws re-emitted per frame, the
//   re-emission's CPU time, the recording hooks' own time, the game's Reset-to-Close time of the same lists.
// mode 3 (R1, the first replay that draws): a depth-only list with two eye-sized viewports side by side (the game's
//   single-pass-stereo Z-prepass) is replayed into a depth target of our own, both focus views at once: the pass
//   constants (root parameter 1) are copied on the GPU into our buffer with an off-axis focus remap of every projection
//   (x' = a x + b w, y' = c y + d w around each eye's own centre), the viewports and scissors are our focus rects. The
//   replay executes on the game's queue right after the batch that holds the game's list. At sample frames the same
//   lists are also replayed unchanged into an eye-sized reference target of ours; both depths are saved (logs\replay)
//   and compared pixel by pixel through the remap. Nothing of the game's is written or transitioned.
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
    uint64_t focusReplays = 0;    // mode 3: focus depth replays executed
};

// The focus replay's settings (mode 3).
struct ReplayFocusSettings {
    double boxFraction = 0.20;    // the focus box's half extent in each eye's NDC (0.2: 20% of the eye's tangent extent)
    double pixelScale = 2.0;      // focus pixels per eye pixel
    uint64_t saveFirst = 1800;    // the first sample frame (xrEndFrame count), then every `saveEvery`, `saveCount` times
    uint64_t saveEvery = 900;
    uint32_t saveCount = 3;
};

// mode 1/2: re-emission cost (never executed); mode 3: focus depth replay (executed on `queue`).
bool replay_probe_start(ID3D12Device* device, int mode, ID3D12CommandQueue* queue = nullptr,
                        const ReplayFocusSettings& focus = {});
void replay_probe_frame_end();
bool replay_probe_installed();
// The counters since the last call, then zeroed (tests; the frame-end report uses them too).
ReplayProbeStats replay_probe_take_stats();
// Qualifying thresholds for modes 1/2 (tests use small ones).
void replay_probe_set_thresholds(uint32_t minDraws, float minViewportWidth);
// Mode 3: the last sample's comparison (tests): pixels compared and the share that agree within 1% (best shift).
struct ReplayFocusCompare {
    uint64_t sample = 0;          // how many samples were compared so far
    uint64_t pixels = 0;
    double agree = 0;             // 0..1
    int shiftX = 0, shiftY = 0;
};
ReplayFocusCompare replay_probe_focus_compare();

}  // namespace simqvfr

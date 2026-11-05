// The in-headset performance overlay (OSD, [osd]): a wide card in the view with ACE's
// frame rate and pacing, its CPU and GPU time against the frame budget (headroom, and which of the
// two limits it), VRAM, frame generation and eye tracking. This file is the pure part: the numbers
// from the layer's measurements, and what each row says. The layer measures (xr_layer.cpp) and draws
// the card with draw_osd_panel (osd_panel.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace simqvfr {

// Where the card floats ([osd], reloaded live), placed as the settings menu's panel is (settings_menu.h's
// MenuPlacement, menu_pose): degrees right (yaw) and up (pitch) of straight ahead, and its width in
// degrees. The default sits above the view's centre, clear of the menu.
struct OsdPlacement {
    bool enabled = false;
    int yaw = 0, pitch = 18, width = 26;
    bool operator==(const OsdPlacement&) const = default;
};
inline constexpr int kOsdYawMax = 35, kOsdPitchMax = 30, kOsdWidthMin = 12, kOsdWidthMax = 45;
OsdPlacement read_osd_placement(const wchar_t* ini);
bool write_osd_placement(const wchar_t* ini, const OsdPlacement& placement);

// One of ACE's frames, as the layer sees it: from the previous xrEndFrame to this one; the render
// thread's span from xrWaitFrame's return to xrEndFrame (its work for the frame: the frame's critical
// path on the CPU) and the CPU time it actually ran in it (the rest it was blocked, on ACE's other threads
// or the GPU); and how long xrWaitFrame held it before (the runtime pacing it to the display).
struct OsdFrame {
    double intervalMs = 0;
    double cpuMs = -1;   // running, ms (-1: unknown); without workMs, the render thread's CPU over the interval
    double workMs = -1;  // xrWaitFrame's return to xrEndFrame, wall time
    double waitMs = -1;  // in xrWaitFrame
};

// Frame generation as Cheeky reports it (its snapshot's "fg").
enum class OsdFg { Off, Starting, On };
// Eye tracking: off in the settings, not offered by the runtime, delivering samples or not.
enum class OsdGaze { Off, Unavailable, Tracking, Partial, Lost };

// Everything one card shows, gathered over the last window (about half a second).
struct OsdInput {
    std::vector<OsdFrame> frames;  // this window's frames, oldest first
    double periodMs = 0;           // the frame period xrWaitFrame gives ACE, 0: unknown (with frame generation
                                   // below the layer, Cheeky's layer may already give the multiplied period)
    double refreshMs = -1;         // the headset's own refresh period (Cheeky's cadence), -1: unknown
    double gpuMs = -1;             // ACE's GPU time per frame (D3D12 timestamps), -1: unknown
    double composeMs = -1;         // the layer's own GPU work per frame (QVFR composition), -1: unknown
    double gpuBusyPercent = -1;    // the whole GPU: its busiest render engine, every process (probe/gpu_load.h), -1: unknown
    double gpuGamePercent = -1;     // ACE's own share of it
    double cpuBusiestPercent = -1; // ACE's busiest thread, percent of one core (probe/cpu_load.h), -1: unknown
    std::wstring cpuBusiestName;   // its description, if any
    bool cpuBusiestIsRender = false;  // it is the render thread
    OsdFg fg = OsdFg::Off;
    int fgMultiplier = 1;          // displayed frames per ACE frame while generating
    double fgOutputFps = 0;        // frames shown per second with generated ones (0: unknown)
    double fgMs = -1;              // frame generation's GPU time per generated frame, -1: unknown
    std::wstring fgState;          // Cheeky's own words while not generating ("Waiting for guides")
    OsdGaze gaze = OsdGaze::Off;
    uint64_t vramUsed = 0, vramBudget = 0;  // bytes, 0: unknown
};

// What the card takes from Cheeky's runtime snapshot (CheekyRuntime_Snapshot's JSON): whether frame
// generation is generating ("fg" "active") or what it is doing instead ("state"), the frames shown
// per second ("cadence" "unique_fps"), its GPU time ("performance" "total_ms") and the GPU's busy
// share ("system_load" "gpu_busy_percent"). `valid` false when the snapshot has no "fg".
struct OsdCheeky {
    bool valid = false, active = false;
    std::wstring state;
    double outputFps = 0, fgMs = -1, gpuBusyPercent = -1;
    double refreshHz = 0;  // "cadence" "refresh_hz": the headset's refresh (0: unknown)
};
OsdCheeky osd_cheeky(const std::string& snapshot);

// Pacing: frames over budget with neither processor full (the runtime or frame generation holds ACE back).
enum class OsdBound { Unknown, None, Cpu, Gpu, Pacing };

// The numbers: ACE's frame rate, frame time and pacing against its budget (the headset's refresh period
// times the frame generation multiplier: what ACE has to deliver), and each processor's headroom in it.
struct OsdSummary {
    double fps = 0, frameMs = 0;  // ACE's frames
    double shownFps = 0;          // frames the headset shows: with frame generation, generated ones too
    double refreshHz = 0;         // 0: unknown
    double budgetMs = 0;        // 0: unknown
    double latePercent = 0;     // frames over the budget by more than 5% (each costs frame rate)
    double jitterMs = 0;        // standard deviation of the frame times
    double frameP90Ms = 0;      // the 90th percentile frame time
    double low1Fps = 0;         // the 1% low: 1000 / the 99th percentile frame time
    // CPU: the render thread's work per frame (wall time), what of it ran and what was blocked, the busiest
    // ACE thread's time per frame (its share of a core times the frame time), and xrWaitFrame's hold.
    double cpuMs = -1, cpuBusyMs = -1, cpuBlockedMs = -1, cpuThreadMs = -1, waitMs = -1;
    // GPU: the whole GPU's time per ACE frame (its busy share times the frame time; ACE's queue time when
    // the busy share is unknown), and its parts: ACE's queue, frame generation, the layer's composition, the rest
    // (the runtime's compositor, other apps). -1: unknown.
    double gpuMs = -1, gpuGameMs = -1, gpuFgMs = -1, gpuLayerMs = -1, gpuRestMs = -1;
    bool gpuWhole = false;      // gpuMs is the whole GPU's
    double cpuHeadroom = 0, gpuHeadroom = 0;  // percent of the budget left (negative: over)
    OsdBound bound = OsdBound::Unknown;
};
OsdSummary osd_summary(const OsdInput& input);

// The colours of the card's state: macOS's system green, yellow and red, and grey.
enum class OsdTone { Neutral, Good, Warn, Bad };

// The card's top row: frame rate, frame time, CPU and GPU, each a large number and unit with a
// line under it (the target, the headroom), coloured by `tone`.
struct OsdTile {
    std::wstring label, value, unit, detail;
    OsdTone tone = OsdTone::Neutral;
};
std::vector<OsdTile> osd_tiles(const OsdInput& input, const OsdSummary& summary);

// The cells under the graph: a small grey label over a value.
struct OsdRow {
    std::wstring label, value;
    OsdTone tone = OsdTone::Neutral;
    bool pill = false;  // the value as a coloured badge (states, not numbers)
    double meter = -1;  // 0..1: a bar under the value (VRAM), -1: none
};
// Pacing, limited by, frame p90 and 1% low, the GPU's parts, the busiest thread, the render thread, VRAM,
// frame generation, eye tracking: three rows of three.
std::vector<OsdRow> osd_rows(const OsdInput& input, const OsdSummary& summary);

// RGBA8 pixels (sRGB, straight alpha), kOsdWidth x kOsdHeight: a rounded translucent card with the
// `tiles` across the top, a strip graph of the recent frame times (`history`, ms, oldest first)
// against the budget, then `rows` as a grid of three columns.
inline constexpr int kOsdWidth = 1024, kOsdHeight = 496;
std::vector<uint32_t> draw_osd_panel(const OsdSummary& summary, const std::vector<double>& history,
                                     const std::vector<OsdTile>& tiles, const std::vector<OsdRow>& rows);

// The same card with a notice instead of the numbers: `title` in yellow, then `lines`, each a paragraph
// (wrapped; the first in white, the rest grey). The layer shows it for a while at the session's start when
// it could not do what it was asked (xr_layer.cpp's notice).
std::vector<uint32_t> draw_notice_panel(const std::wstring& title, const std::vector<std::wstring>& lines);

}  // namespace simqvfr

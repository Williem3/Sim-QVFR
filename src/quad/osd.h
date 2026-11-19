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
// degrees. The default sits straight ahead, low, beside the menu (its default place, settings_menu.h).
struct OsdPlacement {
    bool enabled = false;
    int yaw = 0, pitch = -25, width = 19;
    bool operator==(const OsdPlacement&) const = default;
};
inline constexpr int kOsdYawMax = 35, kOsdPitchMax = 30, kOsdWidthMin = 12, kOsdWidthMax = 45;
OsdPlacement read_osd_placement(const wchar_t* ini);
bool write_osd_placement(const wchar_t* ini, const OsdPlacement& placement);

// The clock ([clock], reloaded live): the local time alone on a small card of its own, placed as the overlay's card
// (degrees right, up, wide). Off by default; the default place is low on the right, across from the menu's.
struct ClockPlacement {
    bool enabled = false;
    int yaw = 22, pitch = -23, width = 8;
    bool operator==(const ClockPlacement&) const = default;
};
inline constexpr int kClockYawMax = kOsdYawMax, kClockPitchMax = kOsdPitchMax, kClockWidthMin = 4, kClockWidthMax = 20;
ClockPlacement read_clock_placement(const wchar_t* ini);
bool write_clock_placement(const wchar_t* ini, const ClockPlacement& placement);

// One of ACE's frames, as the layer sees it: from the previous xrEndFrame to this one; the render
// thread's span from xrWaitFrame's return to xrEndFrame (its work for the frame: the frame's critical
// path on the CPU) and the CPU time it actually ran in it (the rest it was blocked, on ACE's other threads
// or the GPU); and how long xrWaitFrame held it before (the runtime pacing it to the display).
struct OsdFrame {
    double intervalMs = 0;
    double cpuMs = -1;   // running, ms (-1: unknown); without workMs, the render thread's CPU over the interval
    double workMs = -1;  // xrWaitFrame's return to xrEndFrame, wall time
    double waitMs = -1;  // in xrWaitFrame
    // The display refreshes from the previous frame's display time to this one's (osd_refreshes_between): 1 shown on
    // its refresh, 2 or more the runtime showed a frame again in between. 0: unknown.
    int refreshes = 0;
    // A setting that changes the picture or its cost changed just before this frame (the headset menu or the launcher):
    // the frame graph marks it, so a step's effect lines up with the step.
    bool changed = false;
};

// The display refreshes between two frames' display times (xrEndFrame's displayTime, ns) at the display's period
// (ns): their gap in whole periods. What the headset showed, which the CPU's interval between xrEndFrame calls
// is not: the runtime takes a frame handed in a little late for its refresh all the same (the next one comes
// early). 0: unknown (no previous frame, no period, or the same display time again).
int osd_refreshes_between(int64_t previousNs, int64_t displayNs, int64_t periodNs);

// A frame's refreshes: from the display times when known, else its interval in whole budgets (at least 1).
int osd_refreshes(const OsdFrame& frame, double budgetMs);

// Frame generation as Cheeky reports it (its snapshot's "fg").
enum class OsdFg { Off, Starting, On };
// Eye tracking: off in the settings, not offered by the runtime, delivering samples or not.
// Runtime / RuntimeStill: the runtime composes quad views and its own foveated rendering puts the focus views where the
// eyes look (Pimax's, Varjo's): it moved them in the last few seconds, or it has held them still that long.
enum class OsdGaze { Off, Unavailable, Tracking, Partial, Lost, Runtime, RuntimeStill };

// How the game's focus views are drawn (games/adapter.h QvfrMultiView): the card's badge, by the CPU tile's label.
// None: no badge.
enum class OsdMultiView { None, Mvr, Smvr, Fallback, EyesOnly };

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
    OsdMultiView multiView = OsdMultiView::None;
    bool dlss = false;  // DLSS evaluated views since the card was last drawn (said beside the multi-view badge)
    std::wstring upscaler = L"DLSS";  // what evaluated them: "DLSS", "FSR4", "FSR3.1" (engine/dlss_views.h)
    // Night car lighting's cost per frame (games/adapter.h qvfr_car_light_costs), -1 unknown: shown in frame
    // generation's cell while frame generation is off (a game with it has no Cheeky: LMU).
    double carLightGpuMs = -1, carLightCpuMs = -1;
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
    double missedPercent = 0;   // frames that missed a refresh (osd_refreshes 2 or more: each costs frame rate)
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

// The headset menu's frame line, from what the overlay last showed: "GPU 9.8 ms · CPU 7.1 ms · 1.3 ms left · 0% missed"
// ("0.8 ms over" past the budget). `bad`: over the budget, or frames missing the headset (the line in red).
std::wstring osd_menu_line(const OsdSummary& summary, bool& bad);

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

// The badge beside the CPU tile's label, a pill (an empty value: none): MVR (NVIDIA's multi-view rendering) and
// SMVR (Sim-QVFR's software multi-view) green, Fallback (the focus views in passes of their own again) yellow.
OsdRow osd_badge(const OsdInput& input);

// One panel pixel for a quad layer: GDI's BGRx `gdi` (sRGB) with `alpha`, as RGBA premultiplied in linear light and
// encoded sRGB again (the swapchain is _SRGB). Transparent is black: a runtime that blends the layer as premultiplied
// (OpenXR's default, the layers carry no UNPREMULTIPLIED flag) shows nothing there. Straight alpha with the card's
// grey left in the clear corners showed that grey on runtimes that blend as premultiplied: square corners over a dark
// background, rounded over a bright one (2026-11-16).
uint32_t panel_rgba(uint32_t gdi, uint8_t alpha);
// A rounded rectangle's coverage (0..1) at pixel (x, y) of a width x height panel with corners of `radius`, antialiased.
double panel_corner_coverage(int x, int y, int width, int height, int radius);

// The local time on a 12-hour clock ("9:05 PM"), from the hour (0-23) and minute: the card's clock.
std::wstring osd_clock(int hour, int minute);

// RGBA8 pixels (sRGB, premultiplied alpha: panel_rgba), kOsdWidth x kOsdHeight: a rounded translucent card with the
// `tiles` across the top, a strip graph of the recent frames (`history`, oldest first: each bar its frame time against
// the budget, coloured by its refreshes: green shown on its refresh, yellow one missed, red more), then `rows` as a
// grid of three columns; `badge` (osd_badge) at the right of the CPU tile's label;
// `clock` (osd_clock, empty: none) at the graph's right end, which then leaves room for it.
inline constexpr int kOsdWidth = 1024, kOsdHeight = 496;
std::vector<uint32_t> draw_osd_panel(const OsdSummary& summary, const std::vector<OsdFrame>& history,
                                     const std::vector<OsdTile>& tiles, const std::vector<OsdRow>& rows,
                                     const OsdRow& badge = {}, const std::wstring& clock = {});

// RGBA8 pixels (sRGB, premultiplied alpha), kClockWidth x kClockHeight: the clock's card, rounded all round, `time` (osd_clock) large and
// white on the overlay card's translucent dark.
inline constexpr int kClockWidth = 320, kClockHeight = 112;
std::vector<uint32_t> draw_clock_panel(const std::wstring& time);

// The same card with a notice instead of the numbers: `title` in yellow, then `lines`, each a paragraph
// (wrapped; the first in white, the rest grey). The layer shows it for a while at the session's start when
// it could not do what it was asked (xr_layer.cpp's notice).
std::vector<uint32_t> draw_notice_panel(const std::wstring& title, const std::vector<std::wstring>& lines);

}  // namespace simqvfr

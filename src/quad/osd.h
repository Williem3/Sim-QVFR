// The in-headset performance overlay (OSD, [osd]): a wide card in the view with ACE's
// frame rate and pacing, its CPU and GPU time against the frame budget (headroom, and which of the
// two limits it), VRAM, frame generation and eye tracking. This file is the pure part: the numbers
// from the layer's measurements, and what each row says. The layer measures (xr_layer.cpp) and draws
// the card with draw_osd_panel (osd_panel.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aceqv {

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

// One of ACE's frames, as the layer sees it: from the previous xrEndFrame to this one, and the CPU
// time from xrWaitFrame's return to xrEndFrame (the render thread's work for the frame).
struct OsdFrame {
    double intervalMs = 0;
    double cpuMs = -1;  // -1: unknown
};

// Frame generation as Cheeky reports it (its snapshot's "fg").
enum class OsdFg { Off, Starting, On };
// Eye tracking: off in the settings, not offered by the runtime, delivering samples or not.
enum class OsdGaze { Off, Unavailable, Tracking, Partial, Lost };

// Everything one card shows, gathered over the last window (about half a second).
struct OsdInput {
    std::vector<OsdFrame> frames;  // this window's frames, oldest first
    double periodMs = 0;           // the display's refresh period (xrWaitFrame), 0: unknown
    double gpuMs = -1;             // ACE's GPU time per frame (D3D12 timestamps), -1: unknown
    double composeMs = -1;         // the layer's own GPU work per frame (QVFR composition), -1: unknown
    double gpuBusyPercent = -1;    // the GPU's 3D engine busy time (Cheeky's system load), -1: unknown
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
};
OsdCheeky osd_cheeky(const std::string& snapshot);

enum class OsdBound { Unknown, None, Cpu, Gpu };

// The numbers: ACE's frame rate, frame time and pacing against its budget (the refresh period times
// the frame generation multiplier: what ACE has to deliver), and each processor's headroom in it.
struct OsdSummary {
    double fps = 0, frameMs = 0;  // ACE's frames
    double shownFps = 0;          // frames the headset shows: with frame generation, generated ones too
    double refreshHz = 0;         // 0: unknown
    double budgetMs = 0;        // 0: unknown
    double latePercent = 0;     // frames that took more than 1.5 budgets (a skipped or repeated frame)
    double jitterMs = 0;        // standard deviation of the frame times
    double cpuMs = -1, gpuMs = -1;
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
// Pacing, limited by, GPU overhead, VRAM, frame generation, eye tracking.
std::vector<OsdRow> osd_rows(const OsdInput& input, const OsdSummary& summary);

// RGBA8 pixels (sRGB, straight alpha), kOsdWidth x kOsdHeight: a rounded translucent card with the
// `tiles` across the top, a strip graph of the recent frame times (`history`, ms, oldest first)
// against the budget, then `rows` as a grid of three columns.
inline constexpr int kOsdWidth = 1024, kOsdHeight = 420;
std::vector<uint32_t> draw_osd_panel(const OsdSummary& summary, const std::vector<double>& history,
                                     const std::vector<OsdTile>& tiles, const std::vector<OsdRow>& rows);

}  // namespace aceqv

// The launcher window's settings (the kit's LauncherSettings), read from and written to
// ace-quadview.ini: only the keys the layer reads, everything else in the file is left as it is.
//
// Quad View Foveated Rendering (mode=views4): one ACE frame renders the outer views at `outer` of the
// headset's recommended size and the focus views at `inner` density over the foveated size, each
// independent. Two-loop Foveated Interlaced (mode=quad): two ACE frames per headset frame, both at
// `outer`; frame B covers the foveated size, so its density follows from the two and is not set.
#pragma once

#include <string>

#include "../cheeky/cheeky_settings.h"
#include "../input/hotkeys.h"
#include "../quad/focus_offset.h"
#include "../quad/focus_shape.h"
#include "../quad/gaze_deadzone.h"
#include "../quad/helmet.h"
#include "../quad/osd.h"

namespace aceqv::launcher {

enum class RenderMode { Qvfr, TwoLoop };
enum class OutlineColor { Teal, Magenta, Green };

struct Settings {
    RenderMode mode = RenderMode::Qvfr;
    bool eyeTracking = true;  // [quad] gaze: Eye tracked, or Fixed (centre)
    int outer = 50;           // periphery_scale (QVFR) / eye_scale (Two-loop): percent of recommended
    int inner = 100;          // focus_density (QVFR): focus pixel density, percent of recommended
    int width = 39;           // focus_percent: foveated width, percent of the outer view
    int height = 39;          // focus_height_percent: foveated height
    bool outline = false;     // [quad] debug_border
    OutlineColor color = OutlineColor::Teal;
    int feather = 20;         // [quad] feather_percent: blend band, percent of the focus region
    int corner = 0;           // [quad] corner_radius_percent: corner radius, percent of half the shorter side
    FocusShape shape;         // [quad] focus_shape: the box (rounded by corner), or each corner pulled in on its own
    FocusOffset offset;       // [quad] fixed_offset_y, gaze_offset_y: the focus region's vertical offset, degrees up
    int gazeDeadzone = kGazeDeadzoneDefault;  // [quad] gaze_deadzone: tenths of a degree the gaze moves before the region follows
    // The in-headset settings menu's place (the layer's MenuPlacement, src/quad/settings_menu.h): degrees
    // right and up of straight ahead, and the panel's width in degrees.
    int menuYaw = 0;          // [menu] yaw
    int menuPitch = -4;       // [menu] pitch
    int menuWidth = 33;       // [menu] width
    OsdPlacement osd;         // [osd]: the performance overlay, its corner and size (live)
    // [crop] (QVFR): shares of each eye's field of view ACE does not render, each side on its own (the
    // layer's fov_crop.h): outer is the temple side, inner the nose side. Applies at the next launch.
    bool crop = false;        // [crop] enabled
    int cropTop = 0, cropBottom = 0, cropOuter = 0, cropInner = 0;  // [crop] top, bottom, outer, inner: percent
    HelmetSettings helmet;    // [helmet] (QVFR): the helmet overlay round the view (live)
    // The outer views' kept share of the eye's width and height (1 without a crop or in Two-loop).
    double crop_width() const;
    double crop_height() const;
    // Cheeky Foveated DLSS ([cheeky], cheeky_settings.h); its keys but `load` apply live.
    CheekySettings cheeky;
    bool cpuProfile = false;  // [experiment] cpu_profile: the CPU capture hotkey records one (Diagnostics pane)
    Hotkeys hotkeys = default_hotkeys();  // [hotkeys] (input/hotkeys.h; the Hotkeys pane): live
    // [launcher] baseline_next: the next launch runs without QVFR (ACE's own two views, measured), once.
    bool baselineNext = false;
};

struct Range {
    int min, max;
};
inline constexpr Range kOuterRange{10, 100}, kInnerRange{50, 200}, kSizeRange{10, 100}, kFeatherRange{0, 50},
    kCornerRange{0, 100}, kCropRange{0, 40},  // kCropRange: the layer's kCropMax
    kFocusOffsetRange{-kFocusOffsetMax, kFocusOffsetMax}, kGazeDeadzoneRange{0, kGazeDeadzoneMax},
    kFocusCornerRange{0, kFocusCornerMax};
// The layer's kMenuYawMax, kMenuPitchMax, kMenuWidthMin and kMenuWidthMax.
inline constexpr Range kMenuYawRange{-35, 35}, kMenuPitchRange{-30, 30}, kMenuWidthRange{20, 60};

Settings load_settings(const std::wstring& ini);
// Writes the settings' keys (and the mode's fixed keys). False if the file cannot be written.
bool save_settings(const std::wstring& ini, const Settings& settings);
// The keys a running game reloads (the layer's live_reload): the foveated size, feather, corners,
// outline, eye tracking, the headset menu's place, the hotkeys and, for QVFR, the resolution (up to its launch size)
// and the FOV crop. The method and
// Two-loop's resolution apply at the next launch.
bool save_live_settings(const std::wstring& ini, const Settings& settings);
// The ini a running ACE reads (runtime.ini's live_ini, while its live_pid is still ACE), or empty.
std::wstring running_live_ini(const std::wstring& runtimeInfo);

// What the running game launched with (runtime.ini, written by the layer at launch): its method and
// view sizes, percent of the native eye (QVFR: the outer and focus views; Two-loop: eye_scale).
struct LaunchSizes {
    bool known = false;
    RenderMode mode = RenderMode::Qvfr;
    int outer = 0, focusWidth = 0, focusHeight = 0;
    std::wstring crop;  // the FOV crop launched with, "top,bottom,outer,inner" percent (crop_key)
};
// A crop as the layer reports it at launch: "top,bottom,outer,inner" percent, all 0 without one.
std::wstring crop_key(const Settings& settings);
LaunchSizes load_launch_sizes(const std::wstring& runtimeInfo);
// Whether `settings` applies live in full to a game launched with `launch`: the same method and, for
// QVFR, no view larger than at launch (live sizes can only shrink; the FOV crop shapes the outer
// views); Two-loop's resolution unchanged.
bool fits_launch(const Settings& settings, const LaunchSizes& launch);

// A size in pixels.
struct Size {
    int width = 0, height = 0;
};

// The last session's measurements (runtime.ini's session_* keys, the layer's write_session_summary, updated
// every ~10 s while ACE runs): for the Diagnostics pane and a summary users can paste. -1: not measured.
struct SessionInfo {
    std::wstring time, mode, limited, layersBelow, log, cpuProfile;
    double fps = -1, frameP50 = -1, frameP90 = -1, latePercent = -1, refreshHz = -1;
    double cpuMs = -1, gpuMs = -1, gpuBusy = -1, cores = -1, gazeMs = -1, layerMs = -1;
    bool frameGeneration = false;
    std::vector<std::pair<std::wstring, double>> threads;  // name, share of one core (percent), busiest first
    bool known() const { return fps > 0; }
};

// What the layer reported from the last session (runtime.ini): the headset's recommended per-eye
// size and runtime maximum (0 = not known yet), and whether eye tracking delivered samples.
enum class EyeTracker { Unknown, Detected, Missing };
struct RuntimeInfo {
    int width = 0, height = 0, maxWidth = 0, maxHeight = 0;
    double centerPpd = 0;  // pixels per degree at the view's centre at 100% density (the layer's native_center_ppd)
    EyeTracker eyeTracker = EyeTracker::Unknown;
    // Cheeky at the last launch: off, missing, refused or running (empty: no launch yet), and its version.
    std::wstring cheekyStatus, cheekyVersion;
    // VRAM at the last QVFR launch (the layer's vram census): this process's use and Windows' budget for it,
    // MB; the part of it in render targets that follow ACE's target, and that target's size (0: none yet).
    double vramUsedMb = 0, vramBudgetMb = 0, vramScaledMb = 0;
    Size vramTarget;
    SessionInfo session;
    bool known() const { return width > 0 && height > 0; }
};

// Cheeky's runtime as the layer will look for it: [cheeky] runtime, or CheekyFoveatedDLSSRuntime.dll in
// [cheeky] config_dir (default CheekyFoveatedDLSS), relative paths against the layer's folder.
std::wstring cheeky_runtime_path(const std::wstring& ini, const std::wstring& layerFolder);
// Sets [cheeky] runtime (an absolute path). False if the file cannot be written.
bool save_cheeky_runtime(const std::wstring& ini, const std::wstring& runtime);
// Cheeky's OpenXR layer (CheekyOpenXRSetup.exe): registered as an implicit layer, and enabled.
enum class CheekyLayer { Missing, Disabled, Enabled };
CheekyLayer cheeky_layer_state();
RuntimeInfo load_runtime_info(const std::wstring& path);

// The kit's readouts for a recommended per-eye size: view sizes in px, ACE's double-wide target and
// the pixels rendered per headset frame (Two-loop: both frames) against the runtime's own.
struct Readouts {
    Size runtime;              // the runtime's recommended eye, before the layer changes it
    Size outer, focus, target;
    double pixels = 0;         // per headset frame
    double runtimePixels = 0;  // both eyes at the runtime's recommended size, as ACE renders without the layer
    double saved = 0;          // 1 - pixels / runtimePixels; negative when the layer renders more
    double focusDensity = 0;   // focus pixel density against recommended (horizontal)
    double centerPpd = 0;      // the focus area's pixels per degree at its centre (0: not known yet)
    bool overRuntimeLimit = false;  // the offered eye is wider or taller than the runtime's maximum
};
Readouts readouts(const Settings& settings, const RuntimeInfo& runtime);

// VRAM at `settings` (QVFR), from the last launch's measurement: ACE's render targets that follow its
// target scale with the target's area, the rest of its use stays. `mb` 0: not known (no QVFR launch yet,
// or Two-loop). Measured on whatever track and cars that launch loaded, so a heavier one adds to it.
struct VramEstimate {
    double mb = 0, budgetMb = 0;
};
VramEstimate vram_estimate(const Settings& settings, const Readouts& readouts, const RuntimeInfo& runtime);

// The last session as plain text, one item a line, for pasting into a message (the Diagnostics pane's Copy).
std::wstring session_summary_text(const RuntimeInfo& runtime);

const wchar_t* color_name(OutlineColor color);

}  // namespace aceqv::launcher

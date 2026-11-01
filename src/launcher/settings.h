// The launcher window's settings (the kit's LauncherSettings), read from and written to
// ace-quadview.ini: only the keys the layer reads, everything else in the file is left as it is.
//
// Quad View Foveated Rendering (mode=views4): one ACE frame renders the outer views at `outer` of the
// headset's recommended size and the focus views at `inner` density over the foveated size, each
// independent. Two-loop Foveated Interlaced (mode=quad): two ACE frames per headset frame, both at
// `outer`; frame B covers the foveated size, so its density follows from the two and is not set.
#pragma once

#include <string>

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
};

struct Range {
    int min, max;
};
inline constexpr Range kOuterRange{10, 100}, kInnerRange{50, 200}, kSizeRange{10, 100}, kFeatherRange{0, 50},
    kCornerRange{0, 100};

Settings load_settings(const std::wstring& ini);
// Writes the settings' keys (and the mode's fixed keys). False if the file cannot be written.
bool save_settings(const std::wstring& ini, const Settings& settings);
// The keys a running game reloads (the layer's live_reload): the foveated size, feather, corners,
// outline, eye tracking and, for QVFR, the resolution (up to its launch size). The method and
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
};
LaunchSizes load_launch_sizes(const std::wstring& runtimeInfo);
// Whether `settings` applies live in full to a game launched with `launch`: the same method and, for
// QVFR, no view larger than at launch (live sizes can only shrink); Two-loop's resolution unchanged.
bool fits_launch(const Settings& settings, const LaunchSizes& launch);

// What the layer reported from the last session (runtime.ini): the headset's recommended per-eye
// size and runtime maximum (0 = not known yet), and whether eye tracking delivered samples.
enum class EyeTracker { Unknown, Detected, Missing };
struct RuntimeInfo {
    int width = 0, height = 0, maxWidth = 0, maxHeight = 0;
    EyeTracker eyeTracker = EyeTracker::Unknown;
    bool known() const { return width > 0 && height > 0; }
};
RuntimeInfo load_runtime_info(const std::wstring& path);

// The kit's readouts for a recommended per-eye size: view sizes in px, ACE's double-wide target and
// the pixels rendered per headset frame (Two-loop: both frames) against the runtime's own.
struct Size {
    int width = 0, height = 0;
};
struct Readouts {
    Size runtime;              // the runtime's recommended eye, before the layer changes it
    Size outer, focus, target;
    double pixels = 0;         // per headset frame
    double runtimePixels = 0;  // both eyes at the runtime's recommended size, as ACE renders without the layer
    double saved = 0;          // 1 - pixels / runtimePixels; negative when the layer renders more
    double focusDensity = 0;   // focus pixel density against recommended (horizontal)
    bool overRuntimeLimit = false;  // the offered eye is wider or taller than the runtime's maximum
};
Readouts readouts(const Settings& settings, const RuntimeInfo& runtime);

const wchar_t* color_name(OutlineColor color);

}  // namespace aceqv::launcher

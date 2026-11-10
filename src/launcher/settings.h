// The launcher window's settings (the kit's LauncherSettings), read from and written to
// sim-qvfr.ini: only the keys the layer reads, everything else in the file is left as it is.
//
// Quad View Foveated Rendering (mode=views4): one ACE frame renders the outer views at `outer` of the
// headset's recommended size and the focus views at `inner` density over the foveated size, each
// independent. Two-loop Foveated Interlaced (mode=quad): two ACE frames per headset frame, both at
// `outer`; frame B covers the foveated size, so its density follows from the two and is not set.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "../cheeky/cheeky_settings.h"
#include "../input/hotkeys.h"
#include "../profile/profiles.h"
#include "../quad/focus_offset.h"
#include "../quad/focus_shape.h"
#include "../quad/gaze_deadzone.h"
#include "../quad/helmet.h"
#include "../quad/osd.h"

namespace simqvfr::launcher {

enum class RenderMode { Qvfr, TwoLoop };
enum class OutlineColor { Teal, Magenta, Green };

struct Settings {
    RenderMode mode = RenderMode::Qvfr;
    bool eyeTracking = true;  // [quad] gaze: Eye tracked, or Fixed (centre)
    int outer = 50;           // periphery_scale (QVFR) / eye_scale (Two-loop): percent of recommended
    int inner = 100;          // focus_density (QVFR): focus pixel density, percent of recommended
    int objectDetail = 100;   // object_detail (QVFR, X-Plane): object and tree LOD, percent of the focus views' (live)
    // [quad] sharpen_percent (QVFR, a game with GameFeatures::sharpening: LMU): CAS sharpening as the views are composed,
    // 0 off (the default) to 100 the strongest (live).
    int sharpen = 0;
    // [experiment] multi_view (QVFR, a game with GameFeatures::multiView: LMU): multi-view rendering, each eye's pass
    // drawing its focus view too (games/multi_view.h): NVIDIA's on an RTX 20 series or newer, Sim-QVFR's own (instancing)
    // on any other GPU. On by default (2026-11-10; migrate_settings switches older settings over once). Next launch;
    // the layer checks the game's device itself.
    bool multiView = true;
    // [experiment] native_quad_views (QVFR, a game with GameFeatures::nativeQuadViews: LMU): on a Pimax or Varjo runtime,
    // the runtime composes the four views (layer/native_quad.cpp). 1 auto (the default), 0 off, 2 asked for even with
    // another OpenXR layer between (the runtime's offer unknown). Next launch.
    int nativeQuad = 1;
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
    bool hotkeyFooter = true; // [menu] hotkey_footer: the in-game hotkeys under the headset menu
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
    // [experiment] d3d11_capture_hotkey: the frame capture hotkey saves the next frame's swapchain images (Diagnostics
    // pane; games composed by the D3D11 compositor).
    bool frameCapture = false;
    Hotkeys hotkeys = default_hotkeys();  // [hotkeys] (input/hotkeys.h; the Hotkeys pane): live
    // [launcher] baseline_next: the next launch runs without QVFR (ACE's own two views, measured), once.
    bool baselineNext = false;
    // [launcher] anticheat (Games pane, a game with GameInfo::antiCheatStart: LMU): start the game through its
    // anti-cheat's bootstrapper and run Sim-QVFR there too, at the user's risk. Next launch.
    bool antiCheat = false;
    // [launcher] update_check: ask GitHub for the newest release when the launcher starts (About pane). Off until the
    // user says otherwise: the first start asks once, and [launcher] update_asked keeps the answer from being asked again.
    bool updateCheck = false;
    bool updateAsked = false;
    // The game tabs (Games pane; game_tabs.h), every game's, not per-game settings. An installed game has a tab unless
    // [launcher] hidden_games lists it; one that isn't installed has none unless [launcher] shown_games lists it (the
    // user gave it one). [launcher] color_<id>: a game's own colour (RRGGBB) in place of its built-in accent.
    std::wstring hiddenGames, shownGames;
    std::map<std::wstring, uint32_t> gameColors;  // game id -> 0xRRGGBB; a game without one keeps its own
};

struct Range {
    int min, max;
};
inline constexpr Range kOuterRange{10, 100}, kInnerRange{50, 400}, kSizeRange{10, 100}, kFeatherRange{0, 50},
    kCornerRange{0, 100}, kCropRange{0, 40}, kObjectDetailRange{0, 100},  // kCropRange: the layer's kCropMax
    kSharpenRange{0, 100},
    kFocusOffsetRange{-kFocusOffsetMax, kFocusOffsetMax}, kGazeDeadzoneRange{0, kGazeDeadzoneMax},
    kFocusCornerRange{0, kFocusCornerMax};
// Dragging Inner resolution moves it in steps of this many percent, as the headset menu's Focus density does: its
// 350 values span fewer pixels than that, so a plain drag skipped round values such as 100% (99, then 101).
inline constexpr int kInnerDragStep = 5;
// A slider's value at `share` (0..1) of its travel: on a grid of `step` from range.min, never past range.max.
int slider_value_at(Range range, double share, int step = 1);
// The layer's kMenuYawMax, kMenuPitchMax, kMenuWidthMin and kMenuWidthMax.
inline constexpr Range kMenuYawRange{-35, 35}, kMenuPitchRange{-30, 30}, kMenuWidthRange{20, 60};

Settings load_settings(const std::wstring& ini);
// Restore Defaults: the defaults for the game's settings, keeping what is the launcher's or the user's own beyond
// them: the hotkeys, the update check, the game tabs and their colours.
Settings restored_defaults(const Settings& current);
// Writes the settings' keys (and the mode's fixed keys). False if the file cannot be written.
bool save_settings(const std::wstring& ini, const Settings& settings);
// The keys a running game reloads (the layer's live_reload): the foveated size, feather, corners,
// outline, eye tracking, the headset menu's place, the hotkeys and, for QVFR, the resolution (up to its launch size)
// and the FOV crop. The method and
// Two-loop's resolution apply at the next launch.
bool save_live_settings(const std::wstring& ini, const Settings& settings);
// A profile of `settings` (profile/profiles.h): the keys save_settings writes for the picture, without the
// panels' places ([menu], [osd]), the hotkeys, the launcher's own keys and the CPU capture switch.
Profile settings_profile(const Settings& settings, const std::wstring& name);
// Whether an ini key is one a profile (and a game's own settings) holds: not [menu], [osd], [hotkeys], [launcher],
// [profiles], the CPU and frame capture switches or Cheeky's install keys.
bool per_game_key(const std::wstring& section, const std::wstring& key);

// Per-game settings (profile/profiles.h game_settings_file): each game remembers its own resolution, focus area,
// object detail, crop, helmet, Cheeky settings and profile in use, so a value set for one never changes another.
// The ini's per-game keys and its [profiles] current, as `gameId`'s copy (written afresh).
bool store_game_settings(const std::wstring& ini, const std::wstring& gameId);
// `gameId`'s copy into the ini: its per-game keys and [profiles] current only. False, the ini unchanged, without one.
bool restore_game_settings(const std::wstring& ini, const std::wstring& gameId);
// Every game without a copy gets one of the ini's settings: after an update, each game starts from the settings the
// user had, and only changes made for a game from then on are its own. Returns how many were made.
int seed_game_settings(const std::wstring& ini);
// Leaving `fromId` for `toId`: the ini stored as `fromId`'s copy, then `toId`'s loaded (a copy of the ini first if
// it has none). False if a step failed.
bool switch_game_settings(const std::wstring& ini, const std::wstring& fromId, const std::wstring& toId);
// One-time switches of settings written before a default changed, at the launcher's start and launch (once each:
// [launcher] marks it done). Multi-view rendering became the default on 2026-11-10 (LMU, any GPU); the launcher had
// written multi_view=0 for everyone, so the ini, every game's copy and every saved profile with multi_view=0 get 1.
// A choice made after it is kept. Returns the files changed.
inline constexpr wchar_t kMultiViewDefaultOn[] = L"multi_view_default_on";
int migrate_settings(const std::wstring& ini);
// A profile as text to share: only its keys that differ from the defaults' profile.
std::string share_profile(const Profile& profile);
// A shared profile back: the defaults' profile with the text's keys over it, only keys a profile has (and the
// method only as views4 or quad), so a shared text can't set anything else. False if it isn't one.
bool shared_profile(const std::string& text, Profile& profile);

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
    double halfRatePercent = -1;  // the session's frames the runtime ran the game at half rate (or slower)
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
    // Quad views at the last launch: the runtime offered them ("offered", "none", "unknown": a layer between; empty: no
    // launch yet), and whether the session ran as a quad view one (the runtime composing).
    std::wstring quadViews;
    bool quadViewsUsed = false;
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

}  // namespace simqvfr::launcher

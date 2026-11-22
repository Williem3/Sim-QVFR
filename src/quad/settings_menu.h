// The in-headset settings menu (Ctrl+Alt+arrows while ACE has focus): the live keys as the launcher
// shows them, stepped from hotkeys and written to the ini the layer reloads from (and the
// launcher's), so a change applies like one made in the launcher window. Pure: no D3D12 or OpenXR.
#pragma once

#include <string>
#include <vector>

#include "../cheeky/cheeky_settings.h"
#include "../engine/dlss_settings.h"
#include "../engine/nr_settings.h"
#include "car_lighting.h"
#include "road_focus_shape.h"
#include "color_grade.h"
#include "focus_offset.h"
#include "focus_quad_distance.h"
#include "focus_shape.h"
#include "gaze_deadzone.h"
#include "helmet.h"
#include "osd.h"
#include "qvfr_settings.h"

namespace simqvfr {

enum class MenuKey { Up, Down, Left, Right };
enum class MenuItem {
    Periphery, Density, Width, Height, Feather, Corners, Outline, EyeTracking,
    FocusOffset,   // the vertical offset of the mode in use (eye tracked or fixed; focus_offset.h)
    FocusOffsetX,  // the horizontal offset of the mode in use
    GazeDeadzone,  // how far the gaze moves before the focus box follows (gaze_deadzone.h)
    // The Focus shape (focus_shape.h): box or custom, then which of the outline's points to adjust
    // (SettingsMenu::shapePoint) and that point's place, up/down and left/right in percent of the box from its centre.
    Shape, ShapePoint, ShapePointY, ShapePointX,
    // The Headset page: the performance overlay ([osd], osd.h), the clock ([clock]; ClockWidth its size) and the
    // panels' places.
    Overlay, OverlayYaw, OverlayPitch, OverlayWidth, Clock, ClockYaw, ClockPitch, MenuYaw, MenuPitch, MenuWidth,
    // The page switch, and with Cheeky running the Foveated DLSS page ([cheeky], cheeky_settings.h).
    Page, CheekyProcessing, CheekyOuterDlaa, CheekyOuterScale, CheekyOuterPreset, CheekyFocusPreset,
    CheekySupersampling, CheekyMotionFix, CheekyNr, CheekyNrPreset, CheekyNrPasses,
    // The Frame generation page (Cheeky's DLSS-G).
    FgOn, FgMultiplier, FgSize, FgScale, FgPeriphery, FgRelease, FgBorder,
    // The View crop page (QVFR, [crop], fov_crop.h), and the Helmet page ([helmet], helmet.h).
    Crop, CropTop, CropBottom, CropOuter, CropInner, Helmet, HelmetRim, HelmetCorners, HelmetLight, HelmetDebug,
    Visor, HelmetSize, HelmetHeight, HelmetDepth, HelmetDetail,
    // QVFR: finds the resolution that holds the frame rate (qvfr_optimizer.h). Right runs (or stops) it, Left undoes.
    Optimize,
    // The settings page's first line: the profiles (profile/profiles.h), stepped through and used at once.
    Profile,
    // The Headset page: the hotkeys' line under the panel ([menu] hotkey_footer).
    HotkeyFooter,
    // The settings page, after Focus density, for a game with it (MenuLaunch::objectDetail: X-Plane): object detail.
    ObjectDetail,
    // The settings page, after Gaze dead-zone, with focus_layer=2 (MenuLaunch::focusQuads): the quads' distance.
    FocusQuadDistance,
    // The settings page, after Focus density (and Object detail), for a game with it (MenuLaunch::sharpening).
    Sharpening,
    // The DLSS page, Sim-QVFR's own ([dlss], engine/dlss_settings.h), for a game with it (MenuLaunch::ownDlss: LMU): on,
    // the focus views' mode and preset, the eyes' preset, texture sharpening; with MenuLaunch::fsr the upscaler (DLSS or
    // AMD FSR, an experiment).
    DlssOn, DlssUpscaler, DlssFocusMode, DlssFocusPreset, DlssEyePreset, DlssSharpening,
    // The DLSS-NR page ([dlss_nr], engine/nr_settings.h), for a launch that can run it (MenuLaunch::ownNr): on, the
    // model's settings, where it runs relative to DLSS, the focus views' working size, the edge blend, the periphery
    // and how much of it at what working size.
    NrOn, NrIntensity, NrStyle, NrPreset, NrLocalTone, NrLocalStructure, NrAutoMask, NrOrder, NrFocusScale, NrEdgeBlend,
    NrPeriphery, NrPeripheryScale, NrPeripheryCoverage,
    NrLimit,  // the most a view's model works on (NrSettings::workingLimit), after the focus working size
    // The Picture page's Vision section, for a game with it (MenuLaunch::eyeOff): one eye off ([quad] eye_off).
    EyeOff,
    // The CPU page: the main thread's core ([cpu] isolate_main_thread, probe/cpu_isolation.h).
    CpuIsolateMain,
    // The clock's size ([clock] width, degrees), as the launcher's Clock card.
    ClockWidth,
    // The Colour page (quad/color_grade.h; a game with it, MenuLaunch::sharpening): the switch, then its four values.
    ColorOn, ColorExposure, ColorBrightness, ColorContrast, ColorSaturation,
    // The Colour page's Night cars section (quad/car_lighting.h; a game with it, MenuLaunch::carLighting): the switch,
    // then the game's light kept, ambient occlusion, the highlights and the gloss.
    CarLightOn, CarLightQuality, CarLightFill, CarLightOcclusion, CarLightHighlights, CarLightGloss, CarLightSmooth,
    CarLightShadows, CarLightDetail, CarLightReflections, CarLightHeadlights, CarLightGround, CarLightNightVision,
    // The Foveation section's road focus ([quad] focus_corridor; MenuLaunch::focusCorridor, fixed foveation), its width.
    FocusCorridor, FocusCorridorPreset, FocusCorridorWidth, FocusCorridorHeight, FocusCorridorBlend, FocusCorridorShow,
    // The pages' section headers: the launcher's card names, drawn as small headings, never selected (menu_key skips
    // them). Each page is a launcher tab, its sections that tab's cards.
    HeadResolution, HeadFoveation, HeadView, HeadDlss, HeadNr, HeadCheeky, HeadFg, HeadMenu, HeadOverlay, HeadClock,
    HeadVision, HeadCpu, HeadColor, HeadCarLight
};
// A section header (HeadResolution .. HeadCarLight): drawn as a heading, skipped by Up and Down.
inline bool menu_item_is_header(MenuItem item) {
    return static_cast<int>(item) >= static_cast<int>(MenuItem::HeadResolution) &&
           static_cast<int>(item) <= static_cast<int>(MenuItem::HeadCarLight);
}

// Where the panel floats ([menu], set in the launcher's Headset menu pane or from the menu itself,
// reloaded live): degrees right (yaw) and up (pitch) of straight ahead, and the panel's width in
// degrees, kMenuDistance in front of the head. The defaults: low on the left, 20 degrees wide (0.28 m at 0.8 m),
// clear of the overlay's card below the centre and the clock's on the right (osd.h).
struct MenuPlacement {
    int yaw = -22, pitch = -23, width = 20;
    bool operator==(const MenuPlacement&) const = default;
};
inline constexpr int kMenuYawMax = 35, kMenuPitchMax = 30, kMenuWidthMin = 20, kMenuWidthMax = 60;
inline constexpr float kMenuDistance = 0.8f;

// Percent values, as the launcher's Settings (src/launcher/settings.h), with its ranges; the panel's
// place in degrees.
struct MenuValues {
    int periphery = kPeripheryScale.def;   // periphery_scale (qvfr_settings.h, as every key here it has)
    int density = kFocusDensity.def;       // focus_density
    int width = kFocusPercent.def;         // focus_percent
    int height = kFocusHeightPercent.def;  // focus_height_percent
    int feather = kFeatherPercent.def;     // [quad] feather_percent
    int corners = kCornerRadiusPercent.def;  // [quad] corner_radius_percent
    bool outline = false;     // [quad] debug_border
    bool eyeTracking = true;  // [quad] gaze
    FocusOffset offset;       // [quad] fixed_offset_y, gaze_offset_y, fixed_offset_x, gaze_offset_x
    int gazeDeadzone = kGazeDeadzoneDefault;  // [quad] gaze_deadzone, tenths of a degree
    FocusShape shape;         // [quad] focus_shape and shape_points
    bool focusMask = kFocusMask.def != 0;        // [quad] focus_mask (read only: the pixels saved)
    bool focusCorridor = kFocusCorridor.def != 0;          // [quad] focus_corridor (road focus)
    int focusCorridorWidth = kFocusCorridorWidth.def;      // [quad] focus_corridor_width, metres either side
    int focusCorridorHeight = kFocusCorridorHeight.def;    // [quad] focus_corridor_height, tenths of a metre
    int focusCorridorPreset = kFocusCorridorPreset.def;    // [quad] focus_corridor_preset: 0 A, 1 B
    int focusCorridorBlend = kFocusCorridorBlend.def;      // [quad] focus_corridor_blend, tenths of a metre
    bool focusCorridorShow = kFocusCorridorShow.def != 0;  // [quad] focus_corridor_show
    RoadFocusSaved roadSaved;  // the player's own shape and size while Road focus's cockpit shape is set
    int focusMaskMargin = kFocusMaskMargin.def;  // [experiment] focus_mask_margin (likewise)
    OsdPlacement overlay;     // [osd]: the performance overlay, on or off, and its place
    ClockPlacement clock;     // [clock]: the clock, on or off, and its place
    MenuPlacement placement;  // [menu]
    bool hotkeyFooter = true; // [menu] hotkey_footer: the in-game hotkeys listed under the panel
    bool cpuIsolateMain = kCpuIsolateMain.def != 0;  // [cpu] isolate_main_thread: the main thread's core to itself
    bool crop = false;        // [crop] enabled (QVFR)
    int cropTop = 0, cropBottom = 0, cropOuter = 0, cropInner = 0;  // [crop] top, bottom, outer, inner: percent
    HelmetSettings helmet;    // [helmet] (QVFR)
    int objectDetail = kObjectDetail.def;  // [experiment] object_detail (QVFR, a game with it: X-Plane), percent
    int sharpen = 0;          // [quad] sharpen_percent (QVFR, a game with it), percent; 0 off
    ColorGrade color;         // [quad] color_correction and its values (QVFR, a game with sharpening; the Colour page)
    CarLighting carLight;     // [quad] car_light and its values (QVFR, a game with it; the Colour page's Night cars)
    int eyeOff = 0;           // [quad] eye_off (a game with it): 0 both eyes on, 1 the left off, 2 the right (eye_off.h)
    int focusQuadStep = kFocusQuadSteps - 1;  // [experiment] focus_quad_distance, as a step of kFocusQuadDistances
    DlssSettings dlss;        // [dlss]: Sim-QVFR's own DLSS (a game with it: LMU; the DLSS page)
    NrSettings nr;            // [dlss_nr]: DLSS-NR on those views (the DLSS-NR page)
    // The outer views' kept share of the eye (percent; 100 without a crop).
    double crop_width() const { return crop ? 100.0 - cropOuter - cropInner : 100.0; }
    double crop_height() const { return crop ? 100.0 - cropTop - cropBottom : 100.0; }
    bool operator==(const MenuValues&) const = default;
};

// What the running game launched with: the outer and focus views' sizes, percent of the native eye
// (views4_layout.h), which live resolution cannot exceed, and the native eye in pixels (0: unknown).
// Two-loop: no live resolution.
struct MenuLaunch {
    bool qvfr = true;
    double outer = 100, focusWidth = 100, focusHeight = 100;
    int nativeWidth = 0, nativeHeight = 0;
    double nativePpd = 0;       // pixels per degree at the centre at 100% density (0: unknown)
    double twoLoopScale = 100;  // Two-loop: eye_scale, percent (both frames' resolution, fixed at launch)
    bool dlssHeld = false;      // QVFR with DLSS on: the views keep their launch sizes (resolution: next launch)
    double cropWidth = 100, cropHeight = 100;  // the FOV crop launched with: the outer views' kept share of the eye, percent
    bool helmetPieces = false;  // the helmet draws its images (helmet.h): visor, size and height instead of rim and corners
    // What the game has (games/game.h GameFeatures): the View crop and Helmet pages, and resolution changes applied
    // while it runs (otherwise at the next launch).
    bool cropPage = true, helmetPage = true, liveResolution = true;
    bool fgPage = true;  // Cheeky's frame generation page (GameFeatures::frameGeneration)
    // The Focus shape page and the optimizer: off for a game whose path has neither (ATS).
    bool shapePage = true, optimize = true;
    // The sizes follow the corner path's rule (quad/corner_fit.h: the SCS games), not the four views' fit.
    bool cornerFit = false;
    // Object detail on the settings page (GameFeatures::objectDetail): off unless the layer says the game has it, so
    // no other game ever shows it.
    bool objectDetail = false;
    // Sharpening on the settings page (GameFeatures::sharpening), as objectDetail; with it (QVFR) the Colour page too:
    // every game whose compositor runs.
    bool sharpening = false;
    // One eye off on the Picture page (GameFeatures::eyeOff), as objectDetail.
    bool eyeOff = false;
    // Night car lighting on the Colour page (GameFeatures::carLighting, QVFR), and whether it was on at launch (its
    // hooks in: its values live; off at launch, turning it on applies at the next launch, the page says so).
    bool carLighting = false, carLightReady = false;
    // The game masks its focus views (GameFeatures::focusMask): the pixels saved leave out what the mask skips.
    bool focusMask = false;
    // Road focus on the Foveation page (GameFeatures::focusCorridor, QVFR), as objectDetail.
    bool focusCorridor = false;
    // focus_layer=2 in this run: the focus quads' distance on the settings page (focus_quad_distance.h).
    bool focusQuads = false;
    // The runtime composes the views (native quad views: Pimax, Varjo): the settings page says its own sliders don't apply.
    bool runtimeComposes = false;  // the runtime composes the views: the focus area inside its own focus region
    int runtimeFocusWidth = 0, runtimeFocusHeight = 0;  // that region, percent of the eye (0: not known yet)
    // Sim-QVFR's own DLSS can run in this launch (GameFeatures::sideLoadedDlss, QVFR with the multi-view path, a build
    // with the NGX SDK): its DLSS page ([dlss]), live.
    bool ownDlss = false;
    // AMD FSR may run those views instead (the game's experiment key, LMU: [experiment] lmu_fsr=1): the page's Upscaler.
    bool fsr = false;
    // AMD FSR runs them: no NVIDIA RTX card on this PC (experimental). The page is FSR's, without NGX's presets.
    bool fsrPc = false;
    // DLSS-NR can run in this launch: ownDlss and the player's nvngx_dlssnr.dll in [dlss_nr] folder. Its page, live.
    bool ownNr = false;
    // Texture sharpening was on at launch, so its hook and shader twins were made: its settings are live. Off at launch,
    // turning it on applies at the next launch (the page says so).
    bool sharpeningReady = false;
    // The API-level QVFR test path (ACE: replay_compose): QVFR without the game hooks; the page's name says so (its
    // items are still the plain set until the path has settings of its own).
    bool apiPath = false;
    int apiFocusScale = 100;  // the focus views' mode this launch runs ([dlss] focus_scale at launch)
    // Composed by the layer: the most focus density the headset gets, percent of the native eye (the output's largest
    // size, quad/output_density.h); above it the density only antialiases. 0: no such cap (the runtime composes).
    double outputCap = 0;
    bool apiDlss = false;  // the API path with DLSS: the two presets on the settings page ([dlss] eye_preset, focus_preset)
    // The game's adapter makes QVFR at the D3D11 level (GameAdapter::adapter_qvfr: RF2) and the menu steers it: periphery,
    // focus density, the focus area's size, feather, corners, outline and its up/down place; no eye tracking, shape, crop,
    // helmet or optimizer (the layer has no views of its own there).
    bool adapterQvfr = false;
};

// Cheeky Foveated DLSS, when the layer runs it: its version (for the credit line) and [cheeky]'s live keys.
struct MenuCheeky {
    bool available = false;
    std::wstring version;
    CheekySettings values;
};

struct SettingsMenu {
    MenuLaunch launch;
    MenuValues values;
    MenuCheeky cheeky;
    int page = 0;  // index into menu_pages
    int selected = 0;
    // What Adjust point picks (ShapePoint): kFocusWholeShape (its points moved together), or a point; its place:
    // ShapePointY, ShapePointX.
    int shapePoint = kFocusWholeShape;
    int scroll = 0;  // the first line drawn after the page switch (menu_panel.h menu_first_visible), the panel's to keep
    std::wstring optimizeStatus;  // the optimizer's line, short (empty: not run yet)
    std::wstring optimizeDetail;  // what it measured and did, under the title of the settings page (empty: none)
    // Colour correction in the layer's last race frame (the Color page's note): 0 applied (or off), 1 paused this frame
    // (the periphery under native quad views couldn't be graded), 2 not applied in this mode (focus_layer).
    int colorState = 0;
    std::wstring dlssState;  // the API path: what DLSS runs on the periphery and the focus views (empty: nothing to say)
    std::wstring notice;  // a passing message under the title of any page (a CPU capture running or done; empty: none)
    // The frame times beside the title (osd_menu_line; with the overlay off, how to see them), and whether they're over
    // the budget (in red).
    std::wstring frameLine;
    bool frameBad = false;
    int optimizeRequest = 0;      // set by menu_key on Optimize: 1 run or stop, -1 undo; the layer takes it
    // The profiles' names (the presets first), the one in use (-1: none) and whether a value changed since, and
    // the one Left/Right stepped to, for the layer to use (-1: none; it takes it, as optimizeRequest).
    std::vector<std::wstring> profiles;
    int profile = -1;
    bool profileChanged = false;
    int profileRequest = -1;
};

// The menu's pages, as tabs, their sections the launcher's cards by the launcher's names: Picture (Resolution, the
// game's upscaler: DLSS or FSR and DLSS-NR, or Foveated DLSS and Frame generation; Vision), Foveation, Colour (QVFR, a
// game with it: colour correction), View (View & helmet; a game without either, no page), Headset (Headset menu,
// Performance overlay, Clock) and Game (CPU). The user's layout (2026-11-16); before it ten pages of their own names,
// some of one line, the page shown only by its title. Colour: its own page after Foveation at the user's ask (2026-11-18).
enum class MenuPage { Picture, Foveation, Color, View, Headset, Game };
std::vector<MenuPage> menu_pages(const SettingsMenu& menu);
MenuPage menu_page(const SettingsMenu& menu);
// The pages' names for the tab strip under the title, in order (menu_pages'); the current one is menu.page.
std::vector<std::wstring> menu_tabs(const SettingsMenu& menu);
// The section the selected line is in: its header (MenuItem::Head*), or the page's first one for the page switch and
// the profile. The notes and the line under the title follow it.
MenuItem menu_section(const SettingsMenu& menu);

// The items for the method: QVFR also resizes its views live (and the settings page ends with Optimize).
std::vector<MenuItem> menu_items(bool qvfr);
// The current page's items: the page switch first, then the page's.
std::vector<MenuItem> menu_page_items(const SettingsMenu& menu);
// The current page's name, as its tab shows it ("Picture", "View & DLSS", "Headset", "Game"): in the player's language
// (lang.h), or with `shown` false in English (the log's "settings menu: ..." lines).
std::wstring menu_page_name(const SettingsMenu& menu, bool shown = true);
// The panel's title ("Headset menu", the method added for Two-loop and the API path), and its line under it: Cheeky's pages their credit ("CheekyFoveatedDLSS - v0.6.3"),
// the settings page the optimizer's detail while it has one; else empty.
std::wstring menu_title(const SettingsMenu& menu);
std::wstring menu_subtitle(const SettingsMenu& menu);
// One key: Up/Down select (wrapping), Left/Right step the value. True when a value changed.
bool menu_key(SettingsMenu& menu, MenuKey key);

// The values in an ini, read as the launcher reads them (unset periphery and density follow eye_scale).
MenuValues read_menu_values(const wchar_t* ini);
// Writes the menu's keys (resolution, the crop and the helmet only for QVFR; object detail only with `objectDetail`,
// sharpening and colour correction only with `sharpening`, a game that has it; `adapterQvfr` (RF2) also writes the two views'
// resolution). False if the file cannot be written.
bool write_menu_values(const wchar_t* ini, const MenuValues& values, bool qvfr, bool objectDetail = false,
                       bool focusQuads = false, bool sharpening = false, bool eyeOff = false, bool carLighting = false,
                       bool focusCorridor = false, bool adapterQvfr = false);

// What the values ask for against the launch (QVFR), by the layer's rule (views4_layout.h's
// views4_fit): the views share ACE's launch target, so periphery and focus trade width; the focus
// views keep what they ask for first; the outer views take the values' crop. Sizes in percent of the
// native eye (periphery: its density).
struct MenuFit {
    bool peripheryCapped = false, focusCapped = false;
    bool peripheryByRoom = false;  // the periphery is held because the focus views use the width
    bool peripheryRaised = false;  // asked below the path's least periphery (cornerFit): raised to it
    double periphery = 0, focusWidth = 0, focusHeight = 0;  // in effect
    double densityX = 0, densityY = 0;                      // in effect, percent of native
    double wantFocusWidth = 0, wantFocusHeight = 0;         // density x box
};
MenuFit menu_fit(const SettingsMenu& menu);
// Whether a value asks for more than the launch size (the excess applies at the next launch).
bool menu_over_launch(const SettingsMenu& menu);

struct MenuLine {
    std::wstring label, value;
    bool selected = false;
    bool capped = false;  // asked above the launch size: `value` also says what is in effect
    bool header = false;  // a section's heading (the launcher's card name): no value, never selected
};
// The page's lines, labels and values in the player's language (lang.h); `shown` false: in English, for the log
// (layer/menu.cpp's "settings menu: <page> / <section>: <label> -> <value>" stays English).
std::vector<MenuLine> menu_lines(const SettingsMenu& menu, bool shown = true);
// Why a value did not take and what to lower (empty when everything fits): at most two lines.
std::vector<std::wstring> menu_notes(const SettingsMenu& menu);

// ACE's render target (QVFR): the most the launch allows, what renders now and, when a value did not
// fit, what the values ask for ("4775x1250 (6.0 MP)"). `shown` false for Two-loop.
struct MenuResolution {
    bool shown = false, capped = false;
    std::wstring max, current, asked;
};
MenuResolution menu_resolution(const SettingsMenu& menu);

// What the values give now: the focus area's pixels per degree at its centre (whole, 0: unknown) and
// the pixels saved against both eyes at the runtime's recommended size, as the launcher reports it
// (percent; negative: that much more).
struct MenuStats {
    int centerPpd = 0;
    int savedPercent = 0;
};
MenuStats menu_stats(const SettingsMenu& menu);

MenuPlacement read_menu_placement(const wchar_t* ini);

// The quad layer in view space (OpenXR: -Z ahead, +X right, +Y up): its centre on the sphere at
// kMenuDistance, turned to face the eye, and its size in metres for an image `aspect` high per wide.
struct MenuPose {
    float position[3] = {};
    float orientation[4] = {0, 0, 0, 1};  // x, y, z, w
    float width = 0, height = 0;
};
MenuPose menu_pose(const MenuPlacement& placement, float aspect);

}  // namespace simqvfr

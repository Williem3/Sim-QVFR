// The in-headset settings menu (Ctrl+Alt+arrows while ACE has focus): the live keys as the launcher
// shows them, stepped from hotkeys and written to the ini the layer reloads from (and the
// launcher's), so a change applies like one made in the launcher window. Pure: no D3D12 or OpenXR.
#pragma once

#include <string>
#include <vector>

#include "../cheeky/cheeky_settings.h"
#include "focus_offset.h"
#include "helmet.h"
#include "osd.h"

namespace aceqv {

enum class MenuKey { Up, Down, Left, Right };
enum class MenuItem {
    Periphery, Density, Width, Height, Feather, Corners, Outline, EyeTracking,
    FocusOffset,  // the vertical offset of the mode in use (eye tracked or fixed; focus_offset.h)
    // The Overlay and menu page: the performance overlay ([osd], osd.h) and the panels' places.
    Overlay, OverlayYaw, OverlayPitch, OverlayWidth, MenuYaw, MenuPitch, MenuWidth,
    // The page switch, and with Cheeky running the Foveated DLSS page ([cheeky], cheeky_settings.h).
    Page, CheekyProcessing, CheekyOuterDlaa, CheekyOuterScale, CheekyOuterPreset, CheekyFocusPreset,
    CheekySupersampling, CheekyMotionFix, CheekyNr, CheekyNrPreset, CheekyNrPasses,
    // The Frame generation page (Cheeky's DLSS-G).
    FgOn, FgMultiplier, FgSize, FgScale, FgPeriphery, FgRelease, FgBorder,
    // The View crop page (QVFR, [crop], fov_crop.h), and the Helmet page ([helmet], helmet.h).
    Crop, CropTop, CropBottom, CropOuter, CropInner, Helmet, HelmetRim, HelmetCorners, HelmetLight, HelmetDebug,
    Visor, HelmetSize, HelmetHeight, HelmetDepth, HelmetDetail,
    // QVFR: finds the resolution that holds the frame rate (qvfr_optimizer.h). Right runs (or stops) it, Left undoes.
    Optimize
};

// Where the panel floats ([menu], set in the launcher's Headset menu pane or from the menu itself,
// reloaded live): degrees right (yaw) and up (pitch) of straight ahead, and the panel's width in
// degrees, kMenuDistance in front of the head. The defaults are the panel's first place: 0.48 m wide,
// a little low, at 0.8 m.
struct MenuPlacement {
    int yaw = 0, pitch = -4, width = 33;
    bool operator==(const MenuPlacement&) const = default;
};
inline constexpr int kMenuYawMax = 35, kMenuPitchMax = 30, kMenuWidthMin = 20, kMenuWidthMax = 60;
inline constexpr float kMenuDistance = 0.8f;

// Percent values, as the launcher's Settings (src/launcher/settings.h), with its ranges; the panel's
// place in degrees.
struct MenuValues {
    int periphery = 50;  // periphery_scale
    int density = 100;   // focus_density
    int width = 39;      // focus_percent
    int height = 39;     // focus_height_percent
    int feather = 20;    // [quad] feather_percent
    int corners = 0;     // [quad] corner_radius_percent
    bool outline = false;     // [quad] debug_border
    bool eyeTracking = true;  // [quad] gaze
    FocusOffset offset;       // [quad] fixed_offset_y, gaze_offset_y
    OsdPlacement overlay;     // [osd]: the performance overlay, on or off, and its place
    MenuPlacement placement;  // [menu]
    bool crop = false;        // [crop] enabled (QVFR)
    int cropTop = 0, cropBottom = 0, cropOuter = 0, cropInner = 0;  // [crop] top, bottom, outer, inner: percent
    HelmetSettings helmet;    // [helmet] (QVFR)
    // The outer views' kept share of the eye (percent; 100 without a crop).
    double crop_width() const { return crop ? 100.0 - cropOuter - cropInner : 100.0; }
    double crop_height() const { return crop ? 100.0 - cropTop - cropBottom : 100.0; }
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
    std::wstring optimizeStatus;  // the optimizer's line, short (empty: not run yet)
    std::wstring optimizeDetail;  // what it measured and did, under the title of the settings page (empty: none)
    int optimizeRequest = 0;      // set by menu_key on Optimize: 1 run or stop, -1 undo; the layer takes it
};

// The menu's pages: the method's settings, for QVFR the view crop and the helmet, with Cheeky running
// Foveated DLSS and Frame generation, then the performance overlay and the panels' places.
enum class MenuPage { Settings, Crop, Helmet, Dlss, Fg, Headset };
std::vector<MenuPage> menu_pages(const SettingsMenu& menu);
MenuPage menu_page(const SettingsMenu& menu);

// The items for the method: QVFR also resizes its views live (and the settings page ends with Optimize).
std::vector<MenuItem> menu_items(bool qvfr);
// The current page's items: the page switch first, then the page's.
std::vector<MenuItem> menu_page_items(const SettingsMenu& menu);
// The current page's title, and its line under it: Cheeky's pages their credit ("CheekyFoveatedDLSS - v0.6.3"),
// the settings page the optimizer's detail while it has one; else empty.
std::wstring menu_title(const SettingsMenu& menu);
std::wstring menu_subtitle(const SettingsMenu& menu);
// One key: Up/Down select (wrapping), Left/Right step the value. True when a value changed.
bool menu_key(SettingsMenu& menu, MenuKey key);

// The values in an ini, read as the launcher reads them (unset periphery and density follow eye_scale).
MenuValues read_menu_values(const wchar_t* ini);
// Writes the menu's keys (resolution, the crop and the helmet only for QVFR). False if the file cannot be written.
bool write_menu_values(const wchar_t* ini, const MenuValues& values, bool qvfr);

// What the values ask for against the launch (QVFR), by the layer's rule (views4_layout.h's
// views4_fit): the views share ACE's launch target, so periphery and focus trade width; the focus
// views keep what they ask for first; the outer views take the values' crop. Sizes in percent of the
// native eye (periphery: its density).
struct MenuFit {
    bool peripheryCapped = false, focusCapped = false;
    bool peripheryByRoom = false;  // the periphery is held because the focus views use the width
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
};
std::vector<MenuLine> menu_lines(const SettingsMenu& menu);
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

}  // namespace aceqv

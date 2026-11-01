// The in-headset settings menu (Ctrl+Alt+arrows while ACE has focus): the live keys as the launcher
// shows them, stepped from hotkeys and written to the ini the layer reloads from (and the
// launcher's), so a change applies like one made in the launcher window. Pure: no D3D12 or OpenXR.
#pragma once

#include <string>
#include <vector>

namespace aceqv {

enum class MenuKey { Up, Down, Left, Right };
enum class MenuItem { Periphery, Density, Width, Height, Feather, Corners, Outline, EyeTracking };

// Percent values, as the launcher's Settings (src/launcher/settings.h), with its ranges.
struct MenuValues {
    int periphery = 50;  // periphery_scale
    int density = 100;   // focus_density
    int width = 39;      // focus_percent
    int height = 39;     // focus_height_percent
    int feather = 20;    // [quad] feather_percent
    int corners = 0;     // [quad] corner_radius_percent
    bool outline = false;     // [quad] debug_border
    bool eyeTracking = true;  // [quad] gaze
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
};

struct SettingsMenu {
    MenuLaunch launch;
    MenuValues values;
    int selected = 0;
};

// The items for the method: QVFR also resizes its views live.
std::vector<MenuItem> menu_items(bool qvfr);
// One key: Up/Down select (wrapping), Left/Right step the value. True when a value changed.
bool menu_key(SettingsMenu& menu, MenuKey key);

// The values in an ini, read as the launcher reads them (unset periphery and density follow eye_scale).
MenuValues read_menu_values(const wchar_t* ini);
// Writes the menu's keys (resolution only for QVFR). False if the file cannot be written.
bool write_menu_values(const wchar_t* ini, const MenuValues& values, bool qvfr);

// What the values ask for against the launch (QVFR), by the layer's rule (views4_layout.h's
// views4_fit): the views share ACE's launch target, so periphery and focus trade width; the focus
// views keep what they ask for first. Sizes in percent of the native eye.
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

}  // namespace aceqv

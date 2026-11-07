// The launcher window's shared state (launcher_window.cpp's modules): the panes, the controls' ids, the hit
// rectangles, popups and sheets, the App struct with everything the window keeps between paints, and the
// functions one module calls in another. Every colour, size and type style comes from theme_tokens.h.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "../install.h"
#include "../launch_plan.h"
#include "../launch_watch.h"
#include "../layer_order.h"
#include "../preflight.h"
#include "../session_history.h"
#include "../settings.h"
#include "../update_check.h"
#include "../../games/game.h"
#include "../../input/hotkeys.h"
#include "../../input/pad_input.h"
#include "canvas.h"
#include "theme_tokens.h"

namespace simqvfr::launcher::ui::win {

namespace t = tokens;

enum class Pane { Checks, Profiles, Resolution, Foveation, Overlay, Dlss, Menu, Hotkeys, Games, Layers, Diagnostics, Report, About };

inline constexpr int kPanes = 13;

enum Ctl {
    kNone, kSidebar, kGameTab /* option: the game's index in games::all_games */, kMethod, kMode, kOuter, kInner, kObjectDetail, kWidth, kHeight, kCorner, kFocusOffset, kGazeDeadzone, kOutline, kColor, kFeather, kMenuYaw,
    kMenuPitch, kMenuWidth, kMenuPanel, kSupport, kRestore, kLaunch,
    // The focus area's shape ([quad] focus_shape, Foveation pane): box or corners, then each corner's insets
    // (FocusCorner's order, across then up or down).
    kShape, kShapeTopLeftIn, kShapeTopLeftDown, kShapeTopRightIn, kShapeTopRightDown, kShapeBottomRightIn,
    kShapeBottomRightUp, kShapeBottomLeftIn, kShapeBottomLeftUp,
    // Foveated DLSS (Cheeky Foveated DLSS, [cheeky]).
    kCheekyUse, kCheekyRuntime, kCheekyOn, kCheekyOuter, kCheekyScale, kCheekyOuterPreset, kCheekyFocusPreset,
    kCheekySupersampling, kCheekyMotionFix, kCheekyNr, kCheekyNrPreset, kCheekyNrPasses,
    kFgOn, kFgMultiplier, kFgSize, kFgScale, kFgPeriphery, kFgRelease, kFgBorder,
    // The performance overlay ([osd], Headset menu pane).
    kOsdOn, kOsdYaw, kOsdPitch, kOsdSize, kOsdPanel,
    // The FOV crop ([crop], Resolution pane).
    kCropOn, kCropTop, kCropBottom, kCropOuter, kCropInner,
    // The helmet overlay ([helmet], Resolution pane).
    kHelmetOn, kHelmetRim, kHelmetCorners, kHelmetLight, kVisor, kHelmetSize, kHelmetHeight, kHelmetDepth, kHelmetDetail,
    // The OpenXR layers pane (layer_order.h): the profile, the list (keyboard), each row's switch and moves.
    kLayerProfile, kLayerList, kLayerOn, kLayerUp, kLayerDown, kLayerApply, kLayerRevert, kLayerReset,
    // The Report an issue pane (its kind, the description box, the button, the zip it made to drag into the
    // issue, exporting the logs alone) and the Diagnostics pane (the CPU capture switch, copying the summary, the
    // logs folder).
    kDiagKind, kDiagDescription, kDiagReport, kDiagZip, kDiagExport, kDiagCapture, kDiagFrameCapture, kDiagBaseline, kDiagCopy, kDiagLogs,
    // Launch checks (preflight.h): a check's fix button (its index in app.checks), the action bar's badge.
    kCheckFix, kChecksBadge,
    // The Hotkeys pane: an action's key and wheel button (option: its HotkeyAction), the reset button.
    kHotkeyKey, kHotkeyPad, kHotkeysReset,
    // The Profiles pane (profile/profiles.h): a profile's Use, Share and Delete, and the one in use's Update and Revert
    // once the settings differ from it (option: its index in app.profileNames), saving the settings as one, adding a
    // shared one from the clipboard.
    kProfileUse, kProfileShare, kProfileDelete, kProfileUpdate, kProfileRevert, kProfileSave, kProfilePaste,
    // The Headset menu pane: the hotkeys footer ([menu] hotkey_footer).
    kHotkeyFooter,
    // The About pane's update check (update_check.h): the start-up switch, Check now, Download, and the action bar's badge.
    kUpdateCheck, kUpdateNow, kUpdateDownload, kUpdateBadge, kUpdateInstall,
    // The Games pane: a game's tab on or off (option: its index in games::all_games), the game whose colour is edited
    // (option the same), the colour wheel, its brightness, and the game's own colour back.
    kGameShown, kColorGame, kColorWheel, kColorValue, kColorReset,
    // A setting's ⓘ: its help shows while the pointer is over it (option: its index in kHelp).
    kHelpIcon,
    // The action bar: the launched game's status (launch_watch.h; it opens Diagnostics or Report an issue) and
    // Restore Defaults' Undo.
    kLaunchBadge, kUndoBadge,
    // An open sheet (open_sheet): its buttons (option: the button's index) and its name field.
    kSheetButton, kSheetField
};

inline constexpr wchar_t kLayerProfileNames[2][40] = {L"With the launcher", L"Without the launcher (Windows)"};

inline constexpr wchar_t kFgMultipliers[3][4] = {L"2x", L"3x", L"4x"};

inline constexpr wchar_t kFgQuarters[4][8] = {L"25%", L"50%", L"75%", L"100%"};

inline constexpr wchar_t kSupportUrl[] = L"https://ko-fi.com/williem3";

// The exe's hash, done on a worker: its message carries a new std::string (the hex digest).
inline constexpr UINT kHashedMessage = WM_APP + 1;

// The update check, done on a worker: its message carries a new UpdateResult.
inline constexpr UINT kUpdateMessage = WM_APP + 2;

// Dev: a development build (no version to compare): the newest release is shown, not offered as an update.
enum class Update { Off, Checking, UpToDate, Available, Dev, Failed };

struct UpdateResult {
    bool ok = false;
    Release release;
    std::wstring error;
};

// An install (install.h), done on a worker: its progress and its outcome, each message a new InstallProgress or
// InstallResult.
inline constexpr UINT kInstallProgressMessage = WM_APP + 3, kInstallDoneMessage = WM_APP + 4;

// The Hotkeys pane catching a wheel or controller button: polled this often.
inline constexpr UINT_PTR kCaptureTimer = 2;

inline constexpr UINT kCaptureMs = 30;

// The hotkeys' names, in HotkeyAction's order.
inline constexpr const wchar_t* kHotkeyLabels[kHotkeyActions] = {L"Open or close", L"Up",      L"Down",         L"Left",
                                                          L"Right",         L"Overlay",       L"Helmet visor",
                                                          L"CPU capture",   L"Frame capture"};

struct Hit {
    Ctl id;
    Rect r;
    int option = -1;  // sidebar row, segment, popup item
};

enum class Status { Idle, Starting, Error };

// After a change while ACE runs: its live keys went to the running game (Applied), and resolution
// or method differ from what it runs with (NextLaunch).
enum class Live { Off, Applied, NextLaunch };

struct Popup {
    bool open = false;
    Ctl owner = kNone;
    Rect anchor;
    std::vector<std::wstring> items;
    int selected = 0, hover = -1;
};

// A sheet (open_sheet): a question or notice over the dimmed window, in the window's theme, in place of a Windows
// message box. While one shows, only it takes the mouse and the keys: Tab and the arrows move between its buttons,
// Return and Space press the focused one, Escape presses `cancel`.
struct Sheet {
    bool open = false;
    std::wstring title, text;
    std::vector<std::wstring> buttons;  // left to right; the last is the main one (the accent, or red when `danger`)
    int focus = -1;   // the button with the focus when it opens; -1: the main one (the name field, with one)
    int cancel = 0;   // the button Escape presses
    bool danger = false;
    bool field = false;  // a name field above the buttons (Return in it presses the main button)
    std::wstring fieldLabel, fieldText;
    std::function<void(int button, const std::wstring& text)> done;  // the button pressed, and the field's text
};

void open_sheet(Sheet sheet);

// A notice with OK; `then` once it's closed.
void notify(const std::wstring& title, const std::wstring& text, std::function<void()> then = nullptr);

inline constexpr wchar_t kMethodNames[2][40] = {L"Quad View Foveated Rendering", L"Two-loop Foveated Interlaced (legacy)"};

inline constexpr wchar_t kModeNames[2][16] = {L"Eye tracked", L"Fixed"};

inline constexpr wchar_t kColorNames[3][8] = {L"Teal", L"Magenta", L"Green"};

inline constexpr wchar_t kShapeNames[2][8] = {L"Box", L"Corners"};

inline constexpr float kMinWidth = 880, kMinHeight = 600, kDefaultWidth = 1160, kDefaultHeight = 740;

inline constexpr float kTitlebar = t::kTitlebarHeight;  // the kit's window includes its title bar; ours is native

// The placement preview's reference view (degrees) and the panel's distance (the layer's kMenuDistance).
inline constexpr float kRefFovX = 100, kRefFovY = 80, kMenuDistance = 0.8f;

// The in-headset panel's image (the layer's kPanelWidth x kPanelHeight) and colours (menu_panel.cpp).
inline constexpr float kMenuAspect = 972.0f / 1024;

inline constexpr uint32_t kMenuBackground = 0x16181c, kMenuHighlight = 0x0a6e78, kMenuLabel = 0xe8eaed, kMenuDim = 0x9aa0a6;

// The performance overlay's card (quad/osd_panel.cpp's colours).
inline constexpr uint32_t kOsdCard = 0x1c1c1e, kOsdTrack = 0x2c2c2e, kOsdGreen = 0x30d158, kOsdYellow = 0xffd60a;

struct App {
    HWND hwnd = nullptr;
    Canvas canvas;
    bool dark = false;
    t::Colors theme = t::kLight;  // the system theme's colours with the chosen game's accent (apply_theme)
    const t::Colors* c = &theme;
    // The pointer (DIPs; off the window: -1) and the control under it in the last frame's hits (hover states).
    float mouseX = -1, mouseY = -1;
    Ctl hover = kNone;
    int hoverOption = -1;
    Settings s;
    RuntimeInfo runtime;
    std::wstring ini;
    bool iniFound = false;
    std::string iniText;  // the ini's bytes when last read or saved here (follow_ini)
    Pane pane = Pane::Checks;
    // The control with the keyboard focus (Tab, or the last one clicked) and, for one of many (a profile's Delete, a
    // check's fix), which (its hit's option; -1 from Tab).
    Ctl focus = kNone;
    int focusOption = -1;
    bool keyboard = false;  // focus rings show after keyboard use only (:focus-visible)
    Ctl pressed = kNone, dragging = kNone;
    Rect dragTrack;
    Range dragRange{0, 100};
    Rect placeFrame;                   // the placement preview's reference view, as last drawn
    float grabYaw = 0, grabPitch = 0;  // dragging the panel: its centre less the pointer, degrees
    Popup popup;
    Status status = Status::Idle;
    Live live = Live::Off;
    std::wstring error;
    float scroll = 0, scrollMax = 0;
    Rect scrollArea;
    // Report an issue: the kind (kIssueKinds) and the description, a native multi-line edit over the canvas
    // at reportEditRect (empty when this frame drew none), with its font and background brush.
    int reportKind = 0;
    HWND reportEdit = nullptr;
    Rect reportEditRect;
    HFONT reportFont = nullptr;
    UINT reportFontDpi = 0;
    HBRUSH reportBrush = nullptr;
    uint32_t reportBrushRgb = 0;
    std::wstring reportZip;  // the last report's zip (full path): the pane shows it to drag into the issue
    std::wstring exportZip;  // the last Export logs zip (full path), named under its button
    std::vector<Hit> hits;
    // The OpenXR layers pane: 0 with the launcher (layerPlan, from the ini), 1 Windows' list (layersDraft,
    // edited until applied over layersWindows). layerRow: the keyboard's row.
    int layerProfile = 0, layerRow = 0;
    LayerPlan layerPlan;
    std::vector<SystemLayer> layersWindows, layersDraft;
    bool layersLockMoved = false;  // Windows has the Sim-QVFR layer below another one: applying moves it first
    std::wstring layerMessage;
    int layerTone = 0;
    // Launch checks (preflight.h), refreshed every few seconds. The game exe is hashed once per file (its size
    // and write time): hashedExe/hashedStamp name the file exeSha is for, or the one being hashed.
    std::vector<Check> checks;
    std::wstring hashedExe;
    uint64_t hashedStamp = 0;
    std::string exeSha;
    bool hashing = false;
    // The Hotkeys pane: the action catching a press (-1: none), from the keyboard or (capturePad) a wheel or
    // controller, whose controls already held when it started don't count (captureHeld).
    int captureAction = -1;
    bool capturePad = false;
    PadInput pads;
    bool padsOpen = false;
    std::vector<PadHeld> captureHeld;
    // The Profiles pane: the presets and saved profiles, the one in use and whether the ini still matches it
    // (refresh_profiles: when the pane shows, after its actions and when the ini changes), and its last message.
    std::vector<std::wstring> profileNames;
    std::wstring profileCurrent;
    bool profileMatches = false;
    std::wstring profileMessage;
    // The update check: where it stands, the newest release found and why a check failed.
    Update update = Update::Off;
    Release release;
    std::wstring updateError;
    // An install of `release`: running (its step and share), or how the last one failed.
    bool installing = false;
    InstallProgress installProgress;
    std::wstring installError;
    // The help under the pointer (kHelp's index, -1: none) and the ⓘ it belongs to, as last drawn.
    int tip = -1;
    Rect tipAnchor;
    // The Games pane's colour: the game edited (index in games::all_games; -1: the selected game), the wheel as last
    // drawn, and the colour as hue (degrees), saturation and brightness (percent), kept apart from its RGB so a dark
    // colour keeps its hue; colorLoaded is the game they were read for.
    int colorGame = -1, colorLoaded = -2;
    Rect colorWheel;
    float colorHue = 0, colorSat = 1;
    int colorValue = 100;
    // The game launched from here, followed until the next launch (launch_watch.h): the action bar's status.
    LaunchWatch watch;
    // Restore Defaults: the settings before it, for Undo (the action bar, Ctrl+Z) until the next change.
    std::optional<Settings> undoRestore;
    // The last few sessions (session_history.h), newest first, each with the settings it ran with: Diagnostics compares
    // the last session with the one before it.
    std::vector<SessionRecord> sessions;
    // Which games are installed, by id (refresh_installed): the others get no tab unless the Games pane gives them one.
    std::map<std::wstring, bool> installed;
    // The open sheet, its name field (a native edit over the canvas at sheetEditRect, like the report's), and whether
    // the next paint gives that field the keyboard.
    Sheet sheet;
    HWND sheetEdit = nullptr;
    Rect sheetEditRect;
    bool sheetFocusField = false;
    bool drawingSheet = false;  // paint is drawing the sheet: its controls show hover
};

// The window (window_state.cpp).
extern App app;

// The timer follow_ini runs on (app_actions.cpp).
inline constexpr UINT_PTR kFollowTimer = 1;

// A setting's help (help.cpp): what it does and what it costs, by its label.
struct Help {
    const wchar_t* label;
    const wchar_t* text;
};

// window_state.cpp
std::wstring game_name();
std::wstring short_name();
bool game_has(bool games::GameFeatures::*feature);
std::wstring not_yet();
Color col(uint32_t rgb, float alpha = 1);
bool layers_dirty();

// app_actions.cpp
COLORREF colorref(uint32_t rgb);
t::Accent builtin_accent(const games::GameInfo& game);
std::wstring game_id(const games::GameInfo& game);
t::Accent game_accent(const games::GameInfo& game);
void apply_theme();
bool eye_warning();
void push_live();
std::string ini_text();
void changed(bool save = true);
void refresh_profiles();
void reload_runtime();
void follow_ini();
void refresh_checks();
int* slider_value(Ctl id);
Range slider_range(Ctl id);
bool method_shown();
std::vector<Ctl> focus_order();
void launch();

// update_install.cpp
void start_update_check();
void update_done(UpdateResult* r);
void ask_update_check();
bool installable();
bool game_running();
void start_install();
void install_done(InstallResult* r);

// help.cpp
extern const Help kHelp[];
const wchar_t* help_for(const std::wstring& label);
void help_icon(const std::wstring& label, float x, float y);
void help_tip();

// sheet.cpp
void close_sheet(int button);
void sheet_key(WPARAM key);
void sheet_card();
void place_sheet_edit();
void create_sheet_edit();

// launcher_window.cpp
std::wstring clipboard_text();
bool installed(const games::GameInfo& game);
bool pane_shown(int pane);
void edit_font();
void choose_game_folder();
void activate(Ctl id, int option);

// pane_profiles.cpp
float profiles_pane(float x, float y, float w, bool draw);
void use_profile(const std::wstring& name);
void save_current_profile();
void paste_profile();

// pane_checks.cpp
float checks_pane(float x, float y, float w, bool draw);
void fix_check(const Check& c);

// pane_resolution.cpp
float resolution_pane(float x, float y, float w, bool draw);

// pane_foveation.cpp
float foveation_pane(float x, float y, float w, bool draw);
float overlay_pane(float x, float y, float w, bool draw);
float preview(float x, float y, float w, bool draw);
float preview_legend(float x, float y, float w, bool draw);
float readout(float x, float y, float w, const std::wstring& name, const std::wstring& value, bool draw,
              const std::wstring& of = L"");
float eyebrow(float x, float y, float w, const std::wstring& text, bool draw);
float readouts_card(float x, float y, float w, bool draw);

// pane_dlss.cpp
float dlss_pane(float x, float y, float w, bool draw);

}  // namespace simqvfr::launcher::ui::win

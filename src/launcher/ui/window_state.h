// The launcher window's shared state (launcher_window.cpp's modules): the panes, the controls' ids, the hit
// rectangles, popups and sheets, the App struct with everything the window keeps between paints, and the
// functions one module calls in another. Every colour, size and type style comes from theme_tokens.h.
#pragma once

#include <functional>
#include <map>
#include <set>
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

// The pages, one per tab: the Overview hub (overview.cpp) and the Picture, Headset and Game tabs' cards (picture.cpp,
// headset.cpp, pane_games.cpp game_page). Manage profiles and About are panels (panels.cpp).
enum class Pane { Overview, Picture, Headset, Game };

inline constexpr int kPanes = 4;

// The top bar's tabs (docs/ui/OVERHAUL_PLAN.md). Until each tab has its cards, the old panes are its pages, picked in a
// strip under the top bar (shell.cpp kTabPages).
enum class Tab { Overview, Picture, Headset, Game };

inline constexpr int kTabs = 4;

enum Ctl {
    // kPage: a page (option: its Pane) in the strip under the top bar; kGameTab: a game on the rail (option: its index in
    // games::all_games).
    kNone, kPage, kGameTab, kMethod, kMode, kOuter, kInner, kObjectDetail, kWidth, kHeight, kCorner, kFocusOffset, kGazeDeadzone, kOutline, kColor, kFeather, kMenuYaw,
    kMenuPitch, kMenuWidth, kMenuPanel, kSupport, kRestore, kLaunch,
    // The focus area's shape ([quad] focus_shape, Foveation pane): box or corners, then each corner's insets
    // (FocusCorner's order, across then up or down).
    kShape, kShapeTopLeftIn, kShapeTopLeftDown, kShapeTopRightIn, kShapeTopRightDown, kShapeBottomRightIn,
    kShapeBottomRightUp, kShapeBottomLeftIn, kShapeBottomLeftUp,
    // Foveated DLSS (Cheeky Foveated DLSS, [cheeky]).
    kCheekyUse, kCheekyRuntime, kCheekyOn, kCheekyOuter, kCheekyScale, kCheekyOuterPreset, kCheekyFocusPreset,
    kCheekySupersampling, kCheekyMotionFix, kCheekyNr, kCheekyNrPreset, kCheekyNrPasses,
    kFgOn, kFgMultiplier, kFgSize, kFgScale, kFgPeriphery, kFgRelease, kFgBorder,
    // Sim-QVFR's own DLSS ([dlss], DLSS pane; a game with GameFeatures::sideLoadedDlss): on, another nvngx_dlss.dll's
    // folder (Choose…, Use included), the focus views' mode and preset, the eyes' preset, texture sharpening, the other
    // cars' motion.
    kDlssOn, kDlssFolder, kDlssIncluded, kDlssFocusMode, kDlssFocusPreset, kDlssEyePreset, kDlssSharpening,
    kDlssCarVectors,
    // DLSS-NR on those views ([dlss_nr], DLSS pane): on, the player's nvngx_dlssnr.dll (Choose…), the model's settings,
    // where it runs relative to DLSS, the working sizes, the edge blend, the periphery and how much of it.
    kNrOn, kNrFolder, kNrIntensity, kNrStyle, kNrPreset, kNrLocalTone, kNrLocalStructure, kNrAutoMask, kNrOrder,
    kNrFocusScale, kNrEdgeBlend, kNrPeriphery, kNrPeripheryScale, kNrPeripheryCoverage, kNrLimit,
    // The performance overlay ([osd], Headset menu pane).
    kOsdOn, kOsdYaw, kOsdPitch, kOsdSize, kOsdPanel,
    // The runtime composing the views ([experiment] native_quad_views, Resolution pane).
    kNativeQuad,
    // Sharpening ([quad] sharpen_percent, Resolution pane).
    kSharpen,
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
    kGameShown, kColorGame, kColorWheel, kColorValue, kColorReset, kAntiCheat,
    // A setting's ⓘ: its help shows while the pointer is over it (option: its index in kHelp).
    kHelpIcon,
    // The action bar: the launched game's status (launch_watch.h; it opens Diagnostics or Report an issue) and
    // Restore Defaults' Undo.
    kLaunchBadge, kUndoBadge,
    // An open sheet (open_sheet): its buttons (option: the button's index) and its name field.
    kSheetButton, kSheetField,
    // The theme button: following Windows, light or dark ([launcher] ui_theme).
    kThemeToggle,
    // The top bar: a tab (option: its Tab), Find a setting, the profile menu; the rail's version (it opens About).
    kTopTab, kSearch, kProfileMenu, kVersion,
    // Find a setting's results (option: the result's index) and its field.
    kSearchResult, kSearchField,
    // A card's ⋯ menu (option: the card's index this frame), a card's More (option: its index in app.moreKeys), and the
    // helmet's images folder.
    kCardMenu, kMore, kHelmetFolder,
    // Get help's Report an issue… (its panel), an open panel's close button, a colour preset on the Games card (option:
    // its index in kColorPresets) and Custom… (the colour panel).
    kReportOpen, kPanelClose, kColorPreset, kColorCustom,
    // The Overview: the profile's segments (option: a preset's index, or Custom), a summary card (option: its index in
    // app.overviewTargets) and the Ko-fi tile.
    kOverviewProfile, kOverviewCard, kKofiTile,
    // The Preview card's eye: left, both, right (option: its index; app.previewEye).
    kPreviewEye,
    // Text a screen reader reads (reader_text): never a click target or a Tab stop.
    kText
};

// A panel (panels.cpp): a modal card over the dimmed window for what doesn't fit a card.
enum class PanelKind { None, Report, Color, Profiles, About };

// A stop in the keyboard's Tab order: a control, and which one of several (a card's menu), or -1.
struct FocusStop {
    Ctl id;
    int option = -1;
    bool operator==(const FocusStop&) const = default;
};

// The theme the window uses: Windows' app theme, or one chosen with the theme button ([launcher] ui_theme).
enum class ThemeChoice { System, Light, Dark };

inline constexpr wchar_t kLayerProfileNames[2][40] = {L"With the launcher", L"Windows’ list"};

inline constexpr wchar_t kFgMultipliers[3][4] = {L"2x", L"3x", L"4x"};

inline constexpr wchar_t kFgQuarters[4][8] = {L"25%", L"50%", L"75%", L"100%"};

inline constexpr wchar_t kSupportUrl[] = L"https://ko-fi.com/williem3";

// The exe's hash, done on a worker: its message carries a new std::string (the hex digest).
inline constexpr UINT kHashedMessage = WM_APP + 1;

// The update check, done on a worker: its message carries a new UpdateResult.
inline constexpr UINT kUpdateMessage = WM_APP + 2;

// A screen reader's action, posted to the window (accessibility.cpp): a control pressed (wParam its Ctl, lParam its
// option), as a click would; a slider set (wParam its Ctl, lParam the value), as the arrow keys would.
inline constexpr UINT kAccessActivateMessage = WM_APP + 5, kAccessValueMessage = WM_APP + 6;

// A screen reader chose an item of the open pop-up menu (wParam its index).
inline constexpr UINT kAccessMenuMessage = WM_APP + 7;

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
    int option = -1;  // a page, a segment, a popup item
    std::wstring tip;  // shown under it while the pointer is over it (help_tip), if any
    // For screen readers (accessibility.cpp): its own name (a button's label, a segment's item; empty: the row's label),
    // and its state as drawn (a switch: 0 off, 1 on; a segment, tab or game: 1 the chosen one; -1 none).
    std::wstring name;
    int state = -1;  // kText: its heading level (1-3), -1 none
    std::wstring desc;  // a switch's line under its label
};

// A label as last drawn (form.cpp section, controls.cpp checkbox): its text, its section's title and its row. Find a
// setting indexes them and highlights the one it jumps to.
struct DrawnLabel {
    std::wstring label, section;
    Rect r;
    std::wstring hiddenBy;  // hidden while this switch is off (r is the switch's row)
    std::wstring more;      // hidden in this More while it is closed (app.moreOpen's key)
};

// A setting Find a setting can jump to: its label, its section's title, its page, what hides it, and the panel it is in
// (Manage profiles, About), if any.
struct SearchEntry {
    std::wstring label, section;
    Pane pane;
    std::wstring hiddenBy, more;
    PanelKind panel = PanelKind::None;
};

// app.tip for a hit's own tip text (Hit::tip) rather than an entry of kHelp.
inline constexpr int kTipText = 100000;

enum class Status { Idle, Starting, Error };

// After a change while ACE runs: its live keys went to the running game (Applied), and resolution
// or method differ from what it runs with (NextLaunch).
enum class Live { Off, Applied, NextLaunch };

// A popup row's kind: an item to choose, a separator line, or a note (text that isn't a choice).
enum class PopupRow : uint8_t { Item, Separator, Note };

struct Popup {
    bool open = false;
    Ctl owner = kNone;
    Rect anchor;
    std::vector<std::wstring> items;
    std::vector<PopupRow> kinds;  // per item; empty: all are items
    std::vector<int> actions;     // per item, what it does (the profile menu's: pointer.cpp profile_menu_action)
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

inline constexpr float kMinWidth = 880, kMinHeight = 600, kDefaultWidth = 1280, kDefaultHeight = 838;  // a 1280 x 800 page

// Find a setting's highlight on the row it jumped to, and the timer that clears it.
inline constexpr UINT_PTR kHighlightTimer = 3;

inline constexpr UINT kHighlightMs = 1600;

// A disclosure's reveal (card.h Body::reveal): its timer, and how long it takes.
inline constexpr UINT_PTR kRevealTimer = 4;

inline constexpr unsigned long long kRevealMs = 160;

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
    ThemeChoice themeChoice = ThemeChoice::System;  // [launcher] ui_theme (read_theme_choice)
    t::Colors theme = t::kLight;  // the theme's colours (apply_theme)
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
    Pane pane = Pane::Overview;
    Pane tabPage[kTabs] = {Pane::Overview, Pane::Picture, Pane::Headset, Pane::Game};  // each tab's page
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
    // The labels drawn this frame (Find a setting), and the section being drawn (form.cpp).
    std::vector<DrawnLabel> drawnLabels;
    std::wstring drawingSection;
    // Find a setting (search.cpp): open, its field (a native edit over the canvas), the pages' labels as last indexed,
    // the matches for the field's text and the one Return picks.
    bool searchOpen = false, searchIndexed = false, searchFocusField = false;
    HWND searchEdit = nullptr;
    Rect searchEditRect, searchAnchor;
    std::vector<SearchEntry> searchIndex;
    std::vector<int> searchResults;
    int searchSelected = 0;
    // The row it jumped to: highlighted until highlightUntil (GetTickCount64), scrolled into view once.
    DrawnLabel highlight;
    unsigned long long highlightUntil = 0;
    bool highlightScroll = false;
    // A hit's own tip (Hit::tip) under the pointer: its text (app.tip is then kTipText).
    std::wstring tipText;
    // The cards drawn this frame (card.cpp): their titles and what each one's Reset this card does.
    std::vector<std::wstring> cardTitles;
    std::vector<std::function<void(Settings&)>> cardResets;
    // The cards' Mores: those open (by key: card title / its label), and this frame's keys (kMore's options).
    std::set<std::wstring> moreOpen;
    std::vector<std::wstring> moreKeys;
    // Disclosures: when each parent (by Ctl) turned on, while its rows slide open, and its state as last drawn.
    std::map<int, unsigned long long> revealStart;
    std::map<int, bool> revealLast;
    // The open panel (panels.cpp), its content's scroll and the content's rect as last drawn.
    PanelKind panel = PanelKind::None;
    float panelScroll = 0, panelScrollMax = 0;
    Rect panelBody;
    // The Overview's summary cards as last drawn: the card each one opens (kOverviewCard's options).
    std::vector<SearchEntry> overviewTargets;
    // The Preview card shows the left eye (0), both (1) or the right eye (2): a view, not a setting.
    int previewEye = 0;
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

// A pop-up menu item's height (chrome.cpp's popup_menu_rect).
inline constexpr float kMenuItemH = 32;

// window_state.cpp
std::wstring game_name();
std::wstring short_name();
bool game_has(bool games::GameFeatures::*feature);
std::wstring not_yet();
// picture.cpp: Sim-QVFR's own DLSS can be used with these settings (a game with it, QVFR, an RTX card, multi-view
// rendering); otherwise `why` says what's missing.
bool own_dlss_available(std::wstring* why = nullptr);
Color col(uint32_t rgb, float alpha = 1);
bool layers_dirty();

// app_actions.cpp
COLORREF colorref(uint32_t rgb);
t::Accent builtin_accent(const games::GameInfo& game);
std::wstring game_id(const games::GameInfo& game);
t::Accent game_accent(const games::GameInfo& game);
ThemeChoice read_theme_choice();
void cycle_theme();
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
std::vector<FocusStop> focus_order();
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

// pane_layers.cpp
void refresh_layers(bool keepDraft = true);
int layer_rows();
std::wstring friendly_layer(const std::string& name, const std::wstring& manifest = {});
void edit_launcher_layers(const std::function<void(LauncherLayers&)>& change);
void toggle_layer(int i);
void move_layer(int i, int step);
void apply_layers();

// pane_diagnostics.cpp
std::wstring logs_folder();
bool copy_text(const std::wstring& text);
void copy_summary();
std::wstring clipboard_text();
bool report_edit_fits();

// pane_report.cpp
void export_logs();
void report_issue();
void show_report_zip();
void drag_report_zip(int px, int py);
float report_pane(float x, float y, float w, bool draw);
void place_report_edit();
void create_report_edit();

// pane_profiles.cpp: Manage profiles (a panel), and using, saving and pasting profiles.
float profiles_pane(float x, float y, float w, bool draw);
void use_profile(const std::wstring& name);
void save_current_profile();
void paste_profile();

// pane_hotkeys.cpp
void open_pads();
std::wstring pads_found();
void end_capture();
void start_capture(int action, bool pad);
bool capture_key(WPARAM vk);
void capture_poll();

// pane_checks.cpp
void fix_check(const Check& c);

// pane_games.cpp
void refresh_installed();
bool installed(const games::GameInfo& game);
bool game_shown(const games::GameInfo& game);
int shown_game_count();
const games::GameInfo& color_game();
void load_color_edit();
void apply_color_edit();
void set_color_from(float x, float y);
int color_game_index();
void use_color_preset(int preset);
// The colour panel's content (Custom… on the Games card), and the Game tab's cards.
float color_panel(float x, float y, float w, bool draw);
float game_page(float x, float y, float w, bool draw);

// panels.cpp: a panel over the window (Report an issue, a game's colour, Manage profiles, About); Escape or its ✕ closes
// it. Its title and content are Find a setting's too.
std::wstring panel_title(PanelKind kind);
float panel_content(PanelKind kind, float x, float y, float w, bool draw);
void open_panel(PanelKind kind);
void close_panel();
void panel_overlay();

// accessibility.cpp: UI Automation (screen readers): the window's provider (WM_GETOBJECT), the focus and state events
// after each paint, and letting go of it when the window closes.
bool accessibility_object(HWND hwnd, WPARAM wParam, LPARAM lParam, LRESULT& result);  // false: not UIA's request
void accessibility_after_paint();
void accessibility_close();

// overview.cpp: the Overview tab, and its profile segments (the presets, then Custom) and the one chosen.
float overview_page(float x, float y, float w, bool draw);
int overview_profile(std::vector<std::wstring>& items);
void choose_overview_profile(int option);

// picture.cpp: the Picture tab's cards.
float picture_page(float x, float y, float w, bool draw);

// pane_foveation.cpp: the foveation preview, its legend and its readouts (the Preview card's).
float preview(float x, float y, float w, bool draw);
float preview_legend(float x, float y, float w, bool draw);
float readout(float x, float y, float w, const std::wstring& name, const std::wstring& value, bool draw,
              const std::wstring& of = L"");
float eyebrow(float x, float y, float w, const std::wstring& text, bool draw);
float readouts_card(float x, float y, float w, bool draw);


// pane_menu.cpp
float placement_preview(float x, float y, float w, bool draw);
float placement_caption(float x, float y, float w, bool draw);
float placement_readouts(float x, float y, float w, bool draw);

// headset.cpp: the Headset tab's cards.
float headset_page(float x, float y, float w, bool draw);

// pane_about.cpp: the About panel's content.
float about_pane(float x, float y, float w, bool draw);

// chrome.cpp
bool pane_shown(int pane);
int shown_pane(int pane, int step, int from);
const wchar_t* pane_label(int pane);
float pane_content(float x, float y, float w, bool draw);
Rect popup_menu_rect();
float popup_row_height(size_t i);
void edit_font();
void paint();

// shell.cpp: the tabs and their pages, the rail, the top bar and the page strip.
Tab tab_of(Pane pane);
const wchar_t* tab_label(Tab tab);
uint32_t section_color(Tab tab);
void section_shape(Tab tab, float cx, float cy, float size);
std::vector<Pane> tab_pages(Tab tab);
void show_tab(Tab tab);
void show_page(Pane pane);
float rail_width();
std::wstring rail_abbreviation(const games::GameInfo& game);
void rail(float H);
void topbar(float W);
float page_strip(float x, float W);

// search.cpp: Find a setting.
void open_search();
void close_search();
void search_changed();
void search_card();
void search_index_pages(float x, float w);
void place_search_edit();
void create_search_edit();
void jump_to(const SearchEntry& entry);
void draw_highlight();

// pointer.cpp
const Hit* hit_at(float x, float y);
void set_slider_from_x(float x);
std::pair<int*, int*> placement_of(Ctl id);
void set_panel_from(float x, float y);
void open_popup(Ctl owner);
void open_profile_menu();
void open_card_menu(int card);
// A choice's items and the one chosen (a pop-up's, a segmented control's), and choosing one.
int choice_items(Ctl owner, std::vector<std::wstring>& items);
void choose_option(Ctl owner, int option);
// The choices shown as segmented controls (their arrows step through them).
bool segmented_ctl(Ctl id);
void select_game(int option);
int game_index();
void close_popup(bool accept);
int popup_item_at(float x, float y);
void choose_cheeky_runtime();
void choose_dlss_dll();
void use_included_dlss();
void choose_nr_dll();
// picture.cpp: DLSS-NR can be turned on with these settings (Sim-QVFR's own DLSS available and on, and the player's
// nvngx_dlssnr.dll chosen and there).
bool own_nr_available(std::wstring* why = nullptr);
void choose_game_folder();
void undo_restore();

// input.cpp
void activate(Ctl id, int option);
void set_slider(Ctl id, int value);
float dip(LPARAM lp, bool y);
void on_key(WPARAM key);

}  // namespace simqvfr::launcher::ui::win

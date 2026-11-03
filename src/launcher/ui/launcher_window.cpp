// The launcher window, built natively from the QVFR launcher kit (docs/qvfr-launcher-kit): a sidebar
// of panes (Launch checks, Rendering, Resolution, Foveation, Focus overlay, Foveated DLSS, Headset menu, Hotkeys, OpenXR layers, Diagnostics,
// Report an issue, About), grouped forms with
// the kit's controls, a live foveation preview with readouts (the Headset menu pane: a placement preview
// to drag the in-headset settings menu around), and an action bar with the status badge,
// Restore Defaults and Launch. Every colour, size and type style comes from theme_tokens.h; every
// label is the kit's. Settings save as they change (settings.h); Launch runs launch_ace (main.cpp).
//
// Immediate mode: each paint lays out and draws the current pane and records hit rectangles; input
// acts on the last frame's rectangles and the settings, then repaints.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "../preflight.h"
#include "../report.h"
#include "../settings.h"
#include "../../input/pad_input.h"
#include "../../sha256_file.h"
#include "canvas.h"
#include "launcher_version.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")

namespace aceqv::launcher {
namespace ui {
namespace {

namespace t = tokens;

enum class Pane { Checks, Rendering, Resolution, Foveation, Overlay, Dlss, Menu, Hotkeys, Layers, Diagnostics, Report, About };
constexpr int kPanes = 12;
enum Ctl {
    kNone, kSidebar, kMethod, kMode, kOuter, kInner, kWidth, kHeight, kCorner, kFocusOffset, kGazeDeadzone, kOutline, kColor, kFeather, kMenuYaw,
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
    // The Report an issue pane (its kind, the description box, the button) and the Diagnostics pane (the CPU
    // capture switch, copying the summary, the logs folder).
    kDiagKind, kDiagDescription, kDiagReport, kDiagCapture, kDiagBaseline, kDiagCopy, kDiagLogs,
    // Launch checks (preflight.h): a check's fix button (its index in app.checks), the action bar's badge.
    kCheckFix, kChecksBadge,
    // The Hotkeys pane: an action's key and wheel button (option: its HotkeyAction), the reset button.
    kHotkeyKey, kHotkeyPad, kHotkeysReset
};

constexpr wchar_t kLayerProfileNames[2][40] = {L"With the launcher", L"Without the launcher (Windows)"};

constexpr wchar_t kFgMultipliers[3][4] = {L"2x", L"3x", L"4x"};
constexpr wchar_t kFgQuarters[4][8] = {L"25%", L"50%", L"75%", L"100%"};

constexpr wchar_t kSupportUrl[] = L"https://ko-fi.com/williem3";
// The exe's hash, done on a worker: its message carries a new std::string (the hex digest).
constexpr UINT kHashedMessage = WM_APP + 1;
// The Hotkeys pane catching a wheel or controller button: polled this often.
constexpr UINT_PTR kCaptureTimer = 2;
constexpr UINT kCaptureMs = 30;
// The hotkeys' names, in HotkeyAction's order.
constexpr const wchar_t* kHotkeyLabels[kHotkeyActions] = {L"Open or close", L"Up",      L"Down",         L"Left",
                                                          L"Right",         L"Overlay",       L"Helmet visor",
                                                          L"CPU capture"};

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

constexpr wchar_t kMethodNames[2][32] = {L"Quad View Foveated Rendering", L"Two-loop Foveated Interlaced"};
constexpr wchar_t kModeNames[2][16] = {L"Eye tracked", L"Fixed"};
constexpr wchar_t kColorNames[3][8] = {L"Teal", L"Magenta", L"Green"};
constexpr wchar_t kShapeNames[2][8] = {L"Box", L"Corners"};
constexpr float kMinWidth = 820, kMinHeight = 540, kDefaultWidth = 960, kDefaultHeight = 600;
constexpr float kTitlebar = t::kTitlebarHeight;  // the kit's window includes its title bar; ours is native
// The placement preview's reference view (degrees) and the panel's distance (the layer's kMenuDistance).
constexpr float kRefFovX = 100, kRefFovY = 80, kMenuDistance = 0.8f;
// The in-headset panel's image (the layer's kPanelWidth x kPanelHeight) and colours (menu_panel.cpp).
constexpr float kMenuAspect = 972.0f / 1024;
constexpr uint32_t kMenuBackground = 0x16181c, kMenuHighlight = 0x0a6e78, kMenuLabel = 0xe8eaed, kMenuDim = 0x9aa0a6;
// The performance overlay's card (quad/osd_panel.cpp's colours).
constexpr uint32_t kOsdCard = 0x1c1c1e, kOsdTrack = 0x2c2c2e, kOsdGreen = 0x30d158, kOsdYellow = 0xffd60a;

struct App {
    HWND hwnd = nullptr;
    Canvas canvas;
    bool dark = false;
    const t::Colors* c = &t::kLight;
    Settings s;
    RuntimeInfo runtime;
    std::wstring ini;
    bool iniFound = false;
    std::string iniText;  // the ini's bytes when last read or saved here (follow_ini)
    Pane pane = Pane::Rendering;
    Ctl focus = kNone;
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
    std::vector<Hit> hits;
    // The OpenXR layers pane: 0 with the launcher (layerPlan, from the ini), 1 Windows' list (layersDraft,
    // edited until applied over layersWindows). layerRow: the keyboard's row.
    int layerProfile = 0, layerRow = 0;
    LayerPlan layerPlan;
    std::vector<SystemLayer> layersWindows, layersDraft;
    bool layersLockMoved = false;  // Windows has the ACE layer below another one: applying moves it first
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
} app;

Color col(uint32_t rgb, float alpha = 1) { return {rgb, alpha}; }

// The OpenXR layers pane: Windows' list has changes not applied yet.
bool layers_dirty() { return app.layersDraft != app.layersWindows || app.layersLockMoved; }

bool system_dark() {
    // ACE_QV_LAUNCHER_THEME=light|dark overrides the Windows app theme (for checking both).
    wchar_t forced[16] = {};
    if (GetEnvironmentVariableW(L"ACE_QV_LAUNCHER_THEME", forced, 16))
        return _wcsicmp(forced, L"dark") == 0;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

COLORREF colorref(uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

void apply_theme() {
    app.dark = system_dark();
    app.c = app.dark ? &t::kDark : &t::kLight;
    const BOOL dark = app.dark;
    DwmSetWindowAttribute(app.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF caption = colorref(app.c->titlebar_bg), text = colorref(app.c->label);
    DwmSetWindowAttribute(app.hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(app.hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
}

// ---- Settings flow -----------------------------------------------------------------------------

bool eye_warning() { return app.s.eyeTracking && app.runtime.eyeTracker == EyeTracker::Missing; }

// A running ACE reloads the live keys from its own ini (the run folder's copy) between frames.
void push_live() {
    app.live = Live::Off;
    const std::wstring running = running_live_ini(runtime_info_path());
    if (running.empty() || !save_live_settings(running, app.s))
        return;
    // QVFR resolution applies live up to the launch size; more, the method or Two-loop's resolution next launch.
    app.live = fits_launch(app.s, load_launch_sizes(runtime_info_path())) ? Live::Applied : Live::NextLaunch;
}

// The ini's bytes: follow_ini compares them, not the write time, which can miss a change (a read between
// the menu's writes that lands on the same timestamp as the last one; that lost a release point).
std::string ini_text() {
    std::string text;
    HANDLE file = CreateFileW(app.ini.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return text;
    char buffer[16 * 1024];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read)
        text.append(buffer, read);
    CloseHandle(file);
    return text;
}

void changed(bool save = true) {
    app.status = Status::Idle;
    if (save && app.iniFound) {
        save_settings(app.ini, app.s);
        app.iniText = ini_text();
        push_live();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The in-headset settings menu saves into the same ini: show what it set (not while a knob is held). A
// read halfway through the menu's writes is read again on the next tick (the bytes differ by then).
constexpr UINT_PTR kFollowTimer = 1;
void follow_ini() {
    if (!app.iniFound || app.dragging != kNone)
        return;
    std::string text = ini_text();
    if (text == app.iniText)
        return;
    app.iniText = std::move(text);
    app.s = load_settings(app.ini);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The exe's size and write time: a game update changes them.
uint64_t file_stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime) ^
           (static_cast<uint64_t>(a.nFileSizeHigh) << 32 | a.nFileSizeLow);
}

void refresh_checks() {
    const std::wstring exe = launcher_game_exe();
    const uint64_t stamp = exe.empty() ? 0 : file_stamp(exe);
    if (!exe.empty() && (exe != app.hashedExe || stamp != app.hashedStamp) && !app.hashing) {
        app.hashedExe = exe;
        app.hashedStamp = stamp;
        app.exeSha.clear();
        app.hashing = true;
        std::thread([hwnd = app.hwnd, exe] {  // ~100 MB: a second or so from a cold disk
            PostMessageW(hwnd, kHashedMessage, 0, reinterpret_cast<LPARAM>(new std::string(sha256_file(exe.c_str()))));
        }).detach();
    }
    const bool current = exe == app.hashedExe && stamp == app.hashedStamp;
    app.checks = preflight_checks(launcher_preflight(exe, build_state(current ? app.exeSha : "", current && app.hashing)));
}

int* slider_value(Ctl id) {
    switch (id) {
    case kOuter: return &app.s.outer;
    case kInner: return &app.s.inner;
    case kWidth: return &app.s.width;
    case kHeight: return &app.s.height;
    case kFeather: return &app.s.feather;
    case kCorner: return &app.s.corner;
    case kFocusOffset: return &app.s.offset.for_mode(app.s.eyeTracking);
    case kGazeDeadzone: return &app.s.gazeDeadzone;
    case kShapeTopLeftIn:
    case kShapeTopLeftDown:
    case kShapeTopRightIn:
    case kShapeTopRightDown:
    case kShapeBottomRightIn:
    case kShapeBottomRightUp:
    case kShapeBottomLeftIn:
    case kShapeBottomLeftUp: {
        const int i = id - kShapeTopLeftIn;
        return i % 2 ? &app.s.shape.vertical[i / 2] : &app.s.shape.in[i / 2];
    }
    case kMenuYaw: return &app.s.menuYaw;
    case kMenuPitch: return &app.s.menuPitch;
    case kMenuWidth: return &app.s.menuWidth;
    case kCheekyScale: return &app.s.cheeky.outerScale;
    case kCheekySupersampling: return &app.s.cheeky.supersampling;
    case kFgSize: return &app.s.cheeky.fgSize;
    case kOsdYaw: return &app.s.osd.yaw;
    case kOsdPitch: return &app.s.osd.pitch;
    case kOsdSize: return &app.s.osd.width;
    case kCropTop: return &app.s.cropTop;
    case kCropBottom: return &app.s.cropBottom;
    case kCropOuter: return &app.s.cropOuter;
    case kCropInner: return &app.s.cropInner;
    case kHelmetRim: return &app.s.helmet.rim;
    case kHelmetCorners: return &app.s.helmet.corners;
    case kHelmetLight: return &app.s.helmet.light;
    case kHelmetSize: return &app.s.helmet.size;
    case kHelmetHeight: return &app.s.helmet.height;
    case kHelmetDepth: return &app.s.helmet.depth;
    default: return nullptr;
    }
}

Range slider_range(Ctl id) {
    switch (id) {
    case kOuter: return kOuterRange;
    case kInner: return kInnerRange;
    case kFeather: return kFeatherRange;
    case kCorner: return kCornerRange;
    case kFocusOffset: return kFocusOffsetRange;
    case kGazeDeadzone: return kGazeDeadzoneRange;
    case kShapeTopLeftIn:
    case kShapeTopLeftDown:
    case kShapeTopRightIn:
    case kShapeTopRightDown:
    case kShapeBottomRightIn:
    case kShapeBottomRightUp:
    case kShapeBottomLeftIn:
    case kShapeBottomLeftUp: return kFocusCornerRange;
    case kMenuYaw: return kMenuYawRange;
    case kMenuPitch: return kMenuPitchRange;
    case kMenuWidth: return kMenuWidthRange;
    case kCheekyScale: return {kCheekyScaleMin, kCheekyScaleMax};
    case kCheekySupersampling: return {kCheekySupersamplingMin, kCheekySupersamplingMax};
    case kFgSize: return {kCheekyFgSizeMin, kCheekyFgSizeMax};
    case kOsdYaw: return {-kOsdYawMax, kOsdYawMax};
    case kOsdPitch: return {-kOsdPitchMax, kOsdPitchMax};
    case kOsdSize: return {kOsdWidthMin, kOsdWidthMax};
    case kCropTop:
    case kCropBottom:
    case kCropOuter:
    case kCropInner: return kCropRange;
    case kHelmetRim: return {0, kHelmetRimMax};
    case kHelmetCorners: return {0, kHelmetCornersMax};
    case kHelmetLight: return {0, kHelmetLightMax};
    case kHelmetSize: return {kHelmetSizeMin, kHelmetSizeMax};
    case kHelmetHeight: return {-kHelmetHeightMax, kHelmetHeightMax};
    case kHelmetDepth: return {0, kHelmetDepthMax};
    default: return kSizeRange;
    }
}

bool inner_disabled() { return app.s.mode == RenderMode::TwoLoop; }

std::vector<Ctl> focus_order() {
    std::vector<Ctl> order = {kSidebar};
    switch (app.pane) {
    case Pane::Checks: break;  // the fix buttons are the mouse's (one Ctl for them all)
    case Pane::Hotkeys: order.push_back(kHotkeysReset); break;  // the bindings are the mouse's, as the fix buttons
    case Pane::Rendering: order.insert(order.end(), {kMethod, kMode}); break;
    case Pane::Resolution:
        order.push_back(kOuter);
        if (!inner_disabled()) {
            order.insert(order.end(), {kInner, kCropOn});
            if (app.s.crop)
                order.insert(order.end(), {kCropTop, kCropBottom, kCropOuter, kCropInner});
            order.push_back(kHelmetOn);
            if (app.s.helmet.on)
                order.insert(order.end(), {kHelmetRim, kHelmetCorners, kHelmetLight, kVisor, kHelmetSize, kHelmetHeight,
                                           kHelmetDepth, kHelmetDetail});
        }
        break;
    case Pane::Foveation:
        order.insert(order.end(), {kWidth, kHeight});
        if (!app.s.shape.corners)
            order.push_back(kCorner);
        order.push_back(kFocusOffset);
        if (app.s.eyeTracking)
            order.push_back(kGazeDeadzone);
        order.push_back(kShape);
        if (app.s.shape.corners)  // in reading order, as the pane lists them
            order.insert(order.end(), {kShapeTopLeftIn, kShapeTopLeftDown, kShapeTopRightIn, kShapeTopRightDown,
                                       kShapeBottomLeftIn, kShapeBottomLeftUp, kShapeBottomRightIn, kShapeBottomRightUp});
        break;
    case Pane::Overlay:
        order.push_back(kOutline);
        if (app.s.outline)
            order.push_back(kColor);
        order.push_back(kFeather);
        break;
    case Pane::Dlss:
        order.insert(order.end(), {kCheekyUse, kCheekyRuntime, kCheekyOn, kCheekyOuter, kCheekyScale, kCheekyOuterPreset,
                                   kCheekyFocusPreset, kCheekySupersampling, kCheekyMotionFix, kCheekyNr});
        if (app.s.cheeky.nr)
            order.insert(order.end(), {kCheekyNrPreset, kCheekyNrPasses});
        order.push_back(kFgOn);
        if (app.s.cheeky.fg)
            order.insert(order.end(), {kFgMultiplier, kFgSize, kFgScale, kFgPeriphery, kFgRelease, kFgBorder});
        break;
    case Pane::Menu:
        order.insert(order.end(), {kMenuYaw, kMenuPitch, kMenuWidth, kMenuPanel, kOsdOn});
        if (app.s.osd.enabled)
            order.insert(order.end(), {kOsdYaw, kOsdPitch, kOsdSize, kOsdPanel});
        break;
    case Pane::Layers:
        order.insert(order.end(), {kLayerProfile, kLayerList});
        if (app.layerProfile == 0)
            order.push_back(kLayerReset);
        else if (layers_dirty())
            order.insert(order.end(), {kLayerRevert, kLayerApply});
        break;
    case Pane::Diagnostics: order.insert(order.end(), {kDiagCapture, kDiagBaseline, kDiagCopy, kDiagLogs}); break;
    case Pane::Report: order.insert(order.end(), {kDiagKind, kDiagDescription, kDiagReport}); break;
    case Pane::About: order.push_back(kSupport); break;
    }
    order.insert(order.end(), {kRestore, kLaunch});
    return order;
}

void launch() {
    if (!app.iniFound) {
        app.status = Status::Error;
        app.error = L"ace-quadview.ini not found next to the launcher.";
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    // Every change here was saved when it was made; the ini may have changed since (the headset menu), so
    // launch from what it holds, not from what this window last read.
    app.s = load_settings(app.ini);
    save_settings(app.ini, app.s);
    app.iniText = ini_text();
    // A launch that would crash the game, or run it without ace-quadview: ask first.
    refresh_checks();
    std::wstring problems;
    for (const Check& c : app.checks)
        if (c.confirm)
            problems += L"\u2022 " + c.title + L"\n";
    if (!problems.empty() &&
        MessageBoxW(app.hwnd, (L"The launch checks found:\n\n" + problems + L"\nLaunch Assetto Corsa EVO anyway?").c_str(),
                    L"QVFR Launcher", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
        app.pane = Pane::Checks;
        app.scroll = 0;
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    app.status = Status::Starting;
    RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    std::wstring report;
    if (launch_ace(false, report) != 0) {
        app.status = Status::Error;
        const size_t at = report.find(L"ERROR: ");
        app.error = at == std::wstring::npos ? L"Launch failed." : report.substr(at + 7, report.find(L'\n', at) - at - 7);
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// ---- Controls ----------------------------------------------------------------------------------

void focus_ring(const Rect& r, float radius) {
    // Solid 3px focus-ring, offset 1px.
    app.canvas.stroke({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, col(app.c->focus_ring), 3, radius + 4);
}

bool focused(Ctl id) { return app.keyboard && app.focus == id && !app.popup.open; }

// A primary button's label (Launch's); measure with it when sizing one.
constexpr t::TypeStyle kPrimaryLabel{15, 20, 500, false};

void button(Ctl id, const Rect& r, const std::wstring& label, bool primary) {
    const bool down = app.pressed == id;
    Color bg = col(primary ? app.c->accent_fill : app.c->control_bg);
    if (down)
        bg.rgb = ((((bg.rgb >> 16) & 0xff) * 92 / 100) << 16) | ((((bg.rgb >> 8) & 0xff) * 92 / 100) << 8) |
                 ((bg.rgb & 0xff) * 92 / 100);  // brightness 92%
    if (!primary)
        app.canvas.control_shadow(r, t::kRadiusControl, app.dark);
    app.canvas.fill(r, bg, t::kRadiusControl);
    app.canvas.stroke(r, col(primary ? app.c->accent_fill : app.c->control_border), 1, t::kRadiusControl);
    const t::TypeStyle style = primary ? kPrimaryLabel : t::kBody;
    app.canvas.text(label, style, r, col(primary ? app.c->on_accent : app.c->label), Align::Center);
    if (focused(id))
        focus_ring(r, t::kRadiusControl);
    app.hits.push_back({id, r});
}

float popup_width(const std::wstring* items, int count, float minWidth) {
    float w = 0;
    for (int i = 0; i < count; ++i)
        w = std::max(w, app.canvas.measure(items[i], t::kBody));
    return std::max(minWidth, std::ceil(w) + t::kSpace2 + 26);
}

void popup_button(Ctl id, const Rect& r, const std::wstring& value, bool disabled) {
    if (!disabled)
        app.canvas.control_shadow(r, t::kRadiusControl, app.dark);
    app.canvas.fill(r, col(app.c->control_bg), t::kRadiusControl);
    app.canvas.stroke(r, col(disabled ? app.c->separator : app.c->control_border), 1, t::kRadiusControl);
    app.canvas.text(value, t::kBody, {r.x + t::kSpace2, r.y, r.w - t::kSpace2 - 26, r.h},
                    col(disabled ? app.c->label_tertiary : app.c->label));
    // Chevron 8 x 12, 8px from the trailing edge.
    app.canvas.path("M1.5 4.25L4 1.75l2.5 2.5M1.5 7.75L4 10.25l2.5-2.5", {r.right() - 16, r.y + (r.h - 12) / 2, 8, 12},
                    8, col(app.c->label_secondary), 1.5f);
    if (focused(id))
        focus_ring(r, t::kRadiusControl);
    app.hits.push_back({id, r});
}

// The kit's PercentSlider: track, value readout (in `unit`) and an optional detail line. Height 20 (+16 with detail).
// `tenths`: the value is in tenths, shown with one decimal ("1.5°").
void slider(Ctl id, const Rect& r, int value, Range range, const std::wstring& detail, bool disabled,
            const wchar_t* unit = L"%", bool tenths = false) {
    const float valueW = 40, trackW = r.w - valueW - t::kSpace3;
    const Rect track{r.x, r.y, trackW, 20};
    const float knob = 18, travel = track.w - knob;
    const float f = range.max > range.min ? static_cast<float>(value - range.min) / (range.max - range.min) : 0;
    const float kx = track.x + travel * f, cy = track.y + 10;
    const Color fill = col(disabled ? app.c->label_tertiary : app.c->accent_fill);
    app.canvas.fill({track.x, cy - 2, track.w, 4}, col(app.c->segment_track), 2);
    // Filled from the range's start, or from 0 when it spans both signs (degrees left and right).
    const float zx = range.min < 0 && range.max > 0 ? track.x + travel * (0.0f - range.min) / (range.max - range.min) : track.x;
    const float kc = kx + knob / 2;
    app.canvas.fill({std::min(zx, kc), cy - 2, std::abs(kc - zx), 4}, fill, 2);
    const Rect k{kx, cy - knob / 2, knob, knob};
    app.canvas.control_shadow(k, knob / 2, app.dark);
    app.canvas.fill(k, col(app.c->control_bg), knob / 2);
    app.canvas.stroke(k, col(app.c->control_border), 1, knob / 2);
    if (focused(id))
        app.canvas.circle(k.x + knob / 2, k.y + knob / 2, knob / 2 + 1.5f, col(app.c->focus_ring), false, 3);
    app.canvas.text((tenths ? gaze_deadzone_text(value) : std::to_wstring(value)) + unit, t::kValue, {track.right() + t::kSpace3, r.y, valueW, 20},
                    col(disabled ? app.c->label_tertiary : app.c->label), Align::Trailing);
    if (!detail.empty())
        app.canvas.text(detail, t::kSubheadline, {r.x, r.y + 22, r.w, 14}, col(disabled ? app.c->label_tertiary : app.c->label_secondary));
    if (!disabled)
        app.hits.push_back({id, track});
}

float checkbox_height(const std::wstring& desc, float width) {
    return t::kBody.lineHeight + (desc.empty() ? 0 : 2 + app.canvas.measure_height(desc, t::kCallout, width - 22));
}

void checkbox(Ctl id, const Rect& r, bool checked, const std::wstring& label, const std::wstring& desc) {
    const Rect box{r.x, r.y + 1, 14, 14};
    if (checked) {
        app.canvas.fill(box, col(app.c->accent_fill), t::kRadiusCheck);
        app.canvas.path("M4 7.2l2.1 2.1L10.2 4.6", box, 14, col(app.c->on_accent), 2);
    } else {
        app.canvas.control_shadow(box, t::kRadiusCheck, app.dark);
        app.canvas.fill(box, col(app.c->control_bg), t::kRadiusCheck);
        app.canvas.stroke(box, col(app.c->control_border), 1, t::kRadiusCheck);
    }
    if (focused(id))
        focus_ring(box, t::kRadiusCheck);
    const float tx = r.x + 14 + t::kSpace2, tw = r.w - 14 - t::kSpace2;
    app.canvas.text(label, t::kBody, {tx, r.y, tw, t::kBody.lineHeight}, col(app.c->label));
    if (!desc.empty())
        app.canvas.text(desc, t::kCallout, {tx, r.y + t::kBody.lineHeight + 2, tw, 200}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    app.hits.push_back({id, {r.x, r.y, 14 + t::kSpace2 + app.canvas.measure(label, t::kBody), t::kBody.lineHeight + 2}});
}

float segmented_width() {
    float w = 4;
    for (const auto& name : kColorNames)
        w += t::kSpace3 * 2 + 10 + 6 + app.canvas.measure(name, t::kBody) + 2;
    return std::ceil(w - 2);
}

uint32_t overlay(int i) { return i == 0 ? app.c->overlay_teal : i == 1 ? app.c->overlay_magenta : app.c->overlay_green; }

void segmented(Ctl id, const Rect& r, int selected, bool disabled) {
    app.canvas.fill(r, col(app.c->segment_track), t::kRadiusControl);
    float x = r.x + 2;
    for (int i = 0; i < 3; ++i) {
        const float w = t::kSpace3 * 2 + 10 + 6 + app.canvas.measure(kColorNames[i], t::kBody);
        const Rect item{x, r.y + 2, w, r.h - 4};
        if (i == selected) {
            if (!disabled)
                app.canvas.control_shadow(item, t::kRadiusControl - 2, app.dark);
            app.canvas.fill(item, col(app.c->control_bg), t::kRadiusControl - 2);
        }
        app.canvas.circle(item.x + t::kSpace3 + 5, item.y + item.h / 2, 5, col(overlay(i), disabled ? 0.4f : 1), true);
        app.canvas.text(kColorNames[i], t::kBody, {item.x + t::kSpace3 + 16, item.y, w - t::kSpace3 - 16, item.h},
                        col(disabled ? app.c->label_tertiary : app.c->label));
        if (i == selected && focused(id))
            focus_ring(item, t::kRadiusControl - 2);
        if (!disabled)
            app.hits.push_back({id, item, i});
        x += w + 2;
    }
}

void glyph(int tone, float x, float y, Color c) {
    const Rect r{x, y, 12, 12};
    if (tone == 0) {  // success: circle + check
        app.canvas.path("M3.75 6.1l1.5 1.5 3-3.2", r, 12, c, 1.3f);
        app.canvas.circle(x + 6, y + 6, 5, c, false, 1.3f);
    } else if (tone == 1) {  // warning: triangle + mark
        app.canvas.path("M6 1.5l5 9H1zM6 5v2.2", r, 12, c, 1.3f);
        app.canvas.circle(x + 6, y + 8.8f, 0.6f, c, true);
    } else if (tone == 2) {  // danger: circle + x
        app.canvas.circle(x + 6, y + 6, 5, c, false, 1.3f);
        app.canvas.path("M4.25 4.25l3.5 3.5M7.75 4.25l-3.5 3.5", r, 12, c, 1.3f);
    } else {
        app.canvas.circle(x + 6, y + 6, 2.5f, c, true);
    }
}

// StatusBadge: tone 0 success, 1 warning, 2 danger, 3 neutral. Returns its width.
float badge(float x, float y, int tone, const std::wstring& text, float maxWidth = 10000) {
    const t::TypeStyle style{11, 14, 500, false};
    const float w = std::min(maxWidth, 7 + 12 + 5 + std::ceil(app.canvas.measure(text, style)) + 9);
    const uint32_t bg[] = {app.c->success_bg, app.c->warning_bg, app.c->danger_bg, app.c->segment_track};
    const uint32_t fg[] = {app.c->success, app.c->warning, app.c->danger, app.c->label};
    app.canvas.fill({x, y, w, 20}, col(bg[tone]), 10);
    glyph(tone, x + 7, y + 4, col(fg[tone]));
    app.canvas.text(text, style, {x + 24, y, w - 24 - 9, 20}, col(fg[tone]));
    return w;
}

// ---- Form layout -------------------------------------------------------------------------------

// One FormRow inside a section box: label (+ description) leading, the control trailing.
struct Row {
    std::wstring label, desc;
    float controlW = 0, controlH = 0;
    bool disabled = false;
    std::function<void(const Rect&)> control;  // drawn at its rect
    std::function<float(float)> fullHeight;    // full-width rows (checkboxes): height for a width
    std::function<void(const Rect&)> full;
};

float row_height(const Row& row, float w) {
    const float inner = w - 2 * t::kSpace3;
    if (row.full)
        return std::max(t::kRowMinHeight, row.fullHeight(inner) + 2 * t::kSpace2);
    const float textW = inner - row.controlW - t::kSpace4;
    float textH = t::kBody.lineHeight;
    if (!row.desc.empty())
        textH += 2 + app.canvas.measure_height(row.desc, t::kCallout, textW);
    return std::max(t::kRowMinHeight, std::max(textH, row.controlH) + 2 * t::kSpace2);
}

// FormSection at (x, y), width w. Returns its height.
float section(float x, float y, float w, const std::wstring& title, const std::vector<Row>& rows, const std::wstring& footer,
              bool draw) {
    float h = 0;
    if (!title.empty()) {
        if (draw)
            app.canvas.text(title, t::kHeadline, {x + t::kSpace3, y, w - t::kSpace3, 16}, col(app.c->label));
        h += 16 + 6;
    }
    float boxH = 0;
    for (const Row& row : rows)
        boxH += row_height(row, w);
    const Rect box{x, y + h, w, boxH};
    if (draw) {
        app.canvas.fill(box, col(app.c->group_bg), t::kRadiusGroup);
        app.canvas.stroke(box, col(app.c->separator), 0.5f, t::kRadiusGroup);
        float ry = box.y;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& row = rows[i];
            const float rh = row_height(row, w);
            if (i > 0)
                app.canvas.line(box.x + t::kSpace3, ry, box.right() - t::kSpace3, ry, col(app.c->separator), 0.5f);
            const Rect inner{box.x + t::kSpace3, ry + t::kSpace2, w - 2 * t::kSpace3, rh - 2 * t::kSpace2};
            if (row.full) {
                const float fh = row.fullHeight(inner.w);
                row.full({inner.x, inner.y + (inner.h - fh) / 2, inner.w, fh});
            } else {
                const float textW = inner.w - row.controlW - t::kSpace4;
                float textH = t::kBody.lineHeight;
                const float descH = row.desc.empty() ? 0 : app.canvas.measure_height(row.desc, t::kCallout, textW);
                if (!row.desc.empty())
                    textH += 2 + descH;
                const float ty = inner.y + (inner.h - textH) / 2;
                const Color labelColor = col(row.disabled ? app.c->label_tertiary : app.c->label);
                app.canvas.text(row.label, t::kBody, {inner.x, ty, textW, t::kBody.lineHeight}, labelColor);
                if (!row.desc.empty())
                    app.canvas.text(row.desc, t::kCallout, {inner.x, ty + t::kBody.lineHeight + 2, textW, descH + 2},
                                    col(row.disabled ? app.c->label_tertiary : app.c->label_secondary), Align::Leading, false, true);
                row.control({inner.right() - row.controlW, inner.y + (inner.h - row.controlH) / 2, row.controlW, row.controlH});
            }
            ry += rh;
        }
    }
    h += boxH;
    if (!footer.empty()) {
        const float fw = w - 2 * t::kSpace3, fh = app.canvas.measure_height(footer, t::kSubheadline, fw);
        if (draw)
            app.canvas.text(footer, t::kSubheadline, {x + t::kSpace3, y + h + 6 + 2, fw, fh + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
        h += 6 + 2 + fh;
    }
    return h;
}

std::wstring px(Size s) { return std::to_wstring(s.width) + L" × " + std::to_wstring(s.height) + L" px"; }
std::wstring dims(Size s) { return std::to_wstring(s.width) + L" × " + std::to_wstring(s.height); }

// A row with a popup button: `names[selected]` of `count` choices.
Row choice_row(Ctl id, const std::wstring& label, const std::wstring* names, int count, int selected, bool disabled) {
    Row row;
    row.label = label;
    row.disabled = disabled;
    row.controlW = popup_width(names, count, 140);
    row.controlH = t::kControlHeight;
    const std::wstring value = names[selected];
    row.control = [id, value, disabled](const Rect& r) { popup_button(id, r, value, disabled); };
    return row;
}

Row slider_row(Ctl id, const std::wstring& label, const std::wstring& desc, int value, const std::wstring& detail,
               bool disabled = false, const wchar_t* unit = L"%", bool tenths = false) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = 240;
    row.controlH = detail.empty() ? 20.0f : 36.0f;
    row.disabled = disabled;
    const Range range = slider_range(id);
    row.control = [=](const Rect& r) { slider(id, r, value, range, detail, disabled, unit, tenths); };
    return row;
}

// ---- OpenXR layers -----------------------------------------------------------------------------

// Reads both profiles again; Windows' draft is kept while it has changes (`keepDraft`).
void refresh_layers(bool keepDraft = true) {
    app.layerPlan = launcher_layer_plan();
    const bool dirty = app.layersDraft != app.layersWindows || app.layersLockMoved;
    app.layersWindows = read_system_layers(HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, kImplicitLayersKey, &app.layersLockMoved);
    if (!keepDraft || !dirty || app.layersDraft.size() != app.layersWindows.size())
        app.layersDraft = app.layersWindows;
}

int layer_rows() {
    return static_cast<int>(app.layerProfile == 0 ? app.layerPlan.layers.size() : app.layersDraft.size());
}

// A layer's name as people know it; the registered name for the rest.
std::wstring friendly_layer(const std::string& name, const std::wstring& manifest = {}) {
    static const std::pair<const char*, const wchar_t*> kKnown[] = {
        {"XR_APILAYER_MBUCCHIA_toolkit", L"OpenXR Toolkit"},
        {"XR_APILAYER_DIENERTECH_VECTORXR", L"VectorXR"},
        {"XR_APILAYER_CHEEKY_foveated_dlss", L"Cheeky Foveated DLSS"},
        {"XR_APILAYER_MBUCCHIA_quad_views_foveated", L"Quad-Views-Foveated"},
        {"XR_APILAYER_ACE_quadview", L"ACE layer (ace-quadview)"},
        {"XR_APILAYER_NOVENDOR_motion_compensation", L"OpenXR Motion Compensation"},
        {"XR_APILAYER_FREDEMMOTT_OpenKneeboard", L"OpenKneeboard"},
        {"XR_APILAYER_FREDEMMOTT_HandTrackedCockpitClicking", L"Hand Tracked Cockpit Clicking"},
    };
    for (const auto& [n, label] : kKnown)
        if (_stricmp(n, name.c_str()) == 0)
            return label;
    if (name.empty())
        return std::filesystem::path(manifest).filename().wstring();
    return widen(name);
}

// The launcher's profile changes in the ini, then the plan is read again.
void edit_launcher_layers(const std::function<void(LauncherLayers&)>& change) {
    if (!app.iniFound)
        return;
    LauncherLayers profile = read_launcher_layers(app.ini);
    change(profile);
    write_launcher_layers(app.ini, profile);
    app.iniText = ini_text();
    refresh_layers();
}

void toggle_layer(int i) {
    if (app.layerProfile == 0) {
        if (i < 0 || i >= static_cast<int>(app.layerPlan.layers.size()) || !app.layerPlan.layers[i].lock.empty())
            return;
        const PlannedLayer l = app.layerPlan.layers[i];
        edit_launcher_layers([&](LauncherLayers& p) { set_launcher_layer(p, l.name, !l.on, l.systemOn); });
    } else if (i >= 0 && i < static_cast<int>(app.layersDraft.size())) {
        app.layersDraft[i].on = !app.layersDraft[i].on;
    }
    app.layerMessage.clear();
}

void move_layer(int i, int step) {
    if (app.layerProfile == 0) {
        if (i < 0 || i >= static_cast<int>(app.layerPlan.layers.size()))
            return;
        const std::string name = app.layerPlan.layers[i].name;
        bool moved = false;
        edit_launcher_layers([&](LauncherLayers& p) { moved = move_launcher_layer(p, app.layerPlan, name, step); });
        if (moved)
            app.layerRow = i + step;
    } else if (move_system_layer(app.layersDraft, static_cast<size_t>(i), step)) {
        app.layerRow = i + step;
    }
    app.layerMessage.clear();
}

void apply_layers() {
    const std::wstring error = apply_system_layers(app.hwnd, app.layersDraft);
    refresh_layers(!error.empty());
    app.layerMessage = error.empty() ? L"Windows’ OpenXR layers updated. A backup of each list is in the logs folder." : error;
    app.layerTone = error.empty() ? 0 : 2;
}

void mini_check(int option, const Rect& box, bool on, bool disabled) {
    const Color fill = col(disabled ? app.c->label_tertiary : app.c->accent_fill);
    if (on) {
        app.canvas.fill(box, fill, t::kRadiusCheck);
        app.canvas.path("M4 7.2l2.1 2.1L10.2 4.6", box, 14, col(app.c->on_accent), 2);
    } else {
        app.canvas.fill(box, col(app.c->control_bg), t::kRadiusCheck);
        app.canvas.stroke(box, col(disabled ? app.c->separator : app.c->control_border), 1, t::kRadiusCheck);
    }
    if (!disabled)
        app.hits.push_back({kLayerOn, {box.x - 4, box.y - 4, box.w + 8, box.h + 8}, option});
}

void arrow_button(Ctl id, int option, const Rect& r, bool disabled) {
    if (!disabled)
        app.canvas.control_shadow(r, t::kRadiusControl, app.dark);
    app.canvas.fill(r, col(app.c->control_bg), t::kRadiusControl);
    app.canvas.stroke(r, col(disabled ? app.c->separator : app.c->control_border), 1, t::kRadiusControl);
    app.canvas.path(id == kLayerUp ? "M3 7.5L6 4.5l3 3" : "M3 4.5L6 7.5l3-3", {r.x + (r.w - 12) / 2, r.y + (r.h - 12) / 2, 12, 12},
                    12, col(disabled ? app.c->label_tertiary : app.c->label), 1.5f);
    if (!disabled)
        app.hits.push_back({id, r, option});
}

constexpr float kLayerControlsW = 14 + 12 + 26 + 4 + 26;

// One movable layer: its switch (disabled when locked) and its moves.
Row layer_row(const std::wstring& label, const std::wstring& desc, int index, bool on, bool switchLocked, bool canUp,
              bool canDown) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = kLayerControlsW;
    row.controlH = 24;
    row.control = [=](const Rect& r) {
        mini_check(index, {r.x, r.y + 5, 14, 14}, on, switchLocked);
        arrow_button(kLayerUp, index, {r.x + 26, r.y, 26, 24}, !canUp);
        arrow_button(kLayerDown, index, {r.x + 56, r.y, 26, 24}, !canDown);
        if (focused(kLayerList) && app.layerRow == index)
            focus_ring({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, t::kRadiusControl);
    };
    return row;
}

// A layer nothing here can change: a badge saying so.
Row locked_row(const std::wstring& label, const std::wstring& desc, const std::wstring& text) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = 7 + 12 + 5 + std::ceil(app.canvas.measure(text, {11, 14, 500, false})) + 9;
    row.controlH = 20;
    row.control = [text](const Rect& r) { badge(r.x, r.y, 3, text); };
    return row;
}

float layers_pane(float x, float y, float w, bool draw) {
    float h = 0;
    {
        const std::wstring names[2] = {kLayerProfileNames[0], kLayerProfileNames[1]};
        Row profile = choice_row(kLayerProfile, L"Profile", names, 2, app.layerProfile, false);
        profile.desc = app.layerProfile == 0 ? L"What ACE loads when you launch it from here. Windows’ own list stays as it is."
                                             : L"What every other app loads, and ACE started from Steam. Changes Windows’ registry.";
        h += section(x, y, w, L"", {profile}, L"", draw) + t::kSpace6;
    }
    if (app.layerProfile == 0) {
        std::vector<Row> rows;
        for (const std::string& name : app.layerPlan.above)
            rows.push_back(locked_row(friendly_layer(name), L"Has no switch to turn it off per launch, so it stays above the ACE layer.",
                                      L"Above"));
        rows.push_back(locked_row(friendly_layer(kLayerName), L"Always nearest the game.", L"Locked"));
        const int n = static_cast<int>(app.layerPlan.layers.size());
        for (int i = 0; i < n; ++i) {
            const PlannedLayer& l = app.layerPlan.layers[i];
            std::wstring desc = !l.lock.empty() ? widen(l.lock) : l.systemOn ? L"" : L"Off in Windows";
            if (friendly_layer(l.name) != widen(l.name))
                desc += (desc.empty() ? L"" : L" · ") + widen(l.name);
            rows.push_back(layer_row(friendly_layer(l.name), desc, i, l.on, !l.lock.empty(), i > 0, i + 1 < n));
        }
        h += section(x, y + h, w, L"Load order for ACE",
                     rows,
                     L"Nearest the game first. Each launch loads these below the ACE layer in this order; a layer off in "
                     L"Windows can still be loaded for ACE alone. Locked switches keep the mod and ACE working.",
                     draw);
        Row reset;
        reset.label = L"Windows’ order";
        reset.desc = L"Forget this profile: launches follow Windows’ order and switches again.";
        reset.controlW = std::ceil(app.canvas.measure(L"Reset", t::kBody)) + 2 * t::kSpace3;
        reset.controlH = t::kControlHeight;
        reset.control = [](const Rect& r) { button(kLayerReset, r, L"Reset", false); };
        h += t::kSpace6 + section(x, y + h + t::kSpace6, w, L"", {reset}, L"", draw);
    } else {
        for (const bool machine : {true, false}) {
            std::vector<Row> rows;
            const int n = static_cast<int>(app.layersDraft.size());
            for (int i = 0; i < n; ++i) {
                const SystemLayer& l = app.layersDraft[i];
                if (l.machine != machine)
                    continue;
                const bool up = i > 0 && app.layersDraft[i - 1].machine == machine && app.layersDraft[i - 1].lock.empty() && l.lock.empty();
                const bool down = i + 1 < n && app.layersDraft[i + 1].machine == machine && l.lock.empty();
                std::wstring desc = l.lock.empty() ? std::wstring() : widen(l.lock) + L" · ";
                desc += l.name.empty() ? L"Unreadable manifest: " + l.manifest : l.manifest;
                rows.push_back(layer_row(friendly_layer(l.name, l.manifest), desc, i, l.on, false, up, down));
            }
            if (rows.empty())
                continue;
            h += section(x, y + h, w, machine ? L"All users (HKLM)" : L"This user (HKCU)", rows,
                         machine ? L"Changing these needs administrator rights: Windows asks once when you apply."
                                 : L"Windows loads these after the machine-wide ones.",
                         draw) +
                 t::kSpace6;
        }
        if (app.layersDraft.empty()) {
            if (draw)
                badge(x, y + h, 3, L"No OpenXR layers are registered in Windows.", w);
            h += 20 + t::kSpace6;
        }
        Row apply;
        apply.label = layers_dirty() ? L"Changes not applied" : L"Windows’ list";
        apply.desc = app.layersLockMoved ? L"The ACE layer is not first in its list: applying moves it there."
                                         : L"Nearest the game first. The current list is backed up into logs before it changes.";
        const float applyW = std::ceil(app.canvas.measure(L"Apply", t::kBody)) + 2 * t::kSpace3,
                    revertW = std::ceil(app.canvas.measure(L"Revert", t::kBody)) + 2 * t::kSpace3;
        apply.controlW = applyW + t::kSpace2 + revertW;
        apply.controlH = t::kControlHeight;
        const bool dirty = layers_dirty();
        apply.control = [=](const Rect& r) {
            if (dirty) {
                button(kLayerRevert, {r.x, r.y, revertW, r.h}, L"Revert", false);
                button(kLayerApply, {r.x + revertW + t::kSpace2, r.y, applyW, r.h}, L"Apply", true);
            }
        };
        h += section(x, y + h, w, L"", {apply}, L"", draw);
    }
    if (!app.layerMessage.empty()) {
        if (draw)
            badge(x, y + h + t::kSpace4, app.layerTone, app.layerMessage, w);
        h += t::kSpace4 + 20;
    }
    return h;
}

// The preview: the chain a frame goes through, the game at the top, the runtime at the bottom.
float layers_preview(float x, float y, float w, bool draw) {
    std::vector<std::pair<std::wstring, int>> chain;  // tone: 0 end, 1 locked, 2 layer
    chain.push_back({L"Assetto Corsa EVO", 0});
    if (app.layerProfile == 0) {
        for (const std::string& name : app.layerPlan.above)
            chain.push_back({friendly_layer(name), 2});
        chain.push_back({friendly_layer(kLayerName), 1});
        for (const PlannedLayer& l : app.layerPlan.layers)
            if (l.on)
                chain.push_back({friendly_layer(l.name), l.lock.empty() ? 2 : 1});
    } else {
        for (const SystemLayer& l : app.layersDraft)
            if (l.on)
                chain.push_back({friendly_layer(l.name, l.manifest), l.lock.empty() ? 2 : 1});
    }
    chain.push_back({L"OpenXR runtime", 0});
    const float pill = 30, gap = 14;
    if (draw)
        for (size_t i = 0; i < chain.size(); ++i) {
            const Rect r{x, y + i * (pill + gap), w, pill};
            const auto& [text, tone] = chain[i];
            app.canvas.fill(r, col(tone == 0 ? app.c->segment_track : tone == 1 ? app.c->accent_fill : app.c->group_bg), pill / 2);
            if (tone == 2)
                app.canvas.stroke(r, col(app.c->separator), 1, pill / 2);
            app.canvas.text(text, t::kBody, {r.x + 14, r.y, r.w - 28, r.h}, col(tone == 1 ? app.c->on_accent : app.c->label),
                            Align::Center);
            if (i + 1 < chain.size()) {
                const float cx = x + w / 2, top = r.bottom() + 2, bottom = r.bottom() + gap - 2;
                app.canvas.line(cx, top, cx, bottom, col(app.c->label_secondary), 1.5f);
                app.canvas.path("M2 3l3 3 3-3", {cx - 5, bottom - 6, 10, 8}, 10, col(app.c->label_secondary), 1.5f);
            }
        }
    return chain.size() * (pill + gap) - gap;
}

// The current pane's content at (x, y), width w. Returns its height.
// ---- Diagnostics -------------------------------------------------------------------------------
// The last session's measurements (runtime.ini's session_* keys, written by the layer every ~10 s while ACE
// runs), for users to read and to screenshot or copy when asking for help; the CPU capture switch.

// A read-only row: its label (and a line under it) with the value on the right.
Row value_row(const std::wstring& label, const std::wstring& desc, const std::wstring& value) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = std::ceil(app.canvas.measure(value, t::kValue)) + 2;
    row.controlH = 16;
    row.control = [value](const Rect& r) { app.canvas.text(value, t::kValue, r, col(app.c->label), Align::Trailing); };
    return row;
}

// `format` with one number, at whatever length it comes to (a fixed buffer too short for a description made
// swprintf_s end the launcher: the Diagnostics pane's first version).
std::wstring formatted(const wchar_t* format, double v) {
    const int n = _scwprintf(format, v);
    if (n <= 0)
        return {};
    std::wstring text(static_cast<size_t>(n), L'\0');
    swprintf_s(text.data(), static_cast<size_t>(n) + 1, format, v);
    return text;
}

// What a thread is, for the names users will see most (others: none).
std::wstring thread_note(const std::wstring& name) {
    if (name == L"GameThread")
        return L"ACE's main thread, which here also submits each frame. Near 100% it limits the frame rate.";
    if (name == L"Physics")
        return L"Waits for each physics step by spinning on the clock, so it always looks busy.";
    if (name.find(L"aSeeVRClient") != std::wstring::npos)
        return L"The Pimax eye tracker's client: it polls the tracker continuously, so it always looks busy.";
    if (name.find(L"Cheeky") != std::wstring::npos)
        return L"Cheeky Foveated DLSS's frame generation.";
    return L"";
}

// The layers below ace-quadview as people know them: "XR_APILAYER_MBUCCHIA_toolkit, ..." -> "OpenXR Toolkit, ...".
std::wstring friendly_layers(const std::wstring& names) {
    std::wstring out;
    for (size_t at = 0; at < names.size();) {
        const size_t comma = names.find(L',', at), end = comma == std::wstring::npos ? names.size() : comma;
        std::wstring name = names.substr(at, end - at);
        name.erase(0, name.find_first_not_of(L' '));
        if (!name.empty()) {
            std::string ascii;  // layer names are ASCII
            for (const wchar_t c : name)
                ascii += static_cast<char>(c);
            out += (out.empty() ? L"" : L", ") + friendly_layer(ascii);
        }
        at = end + 1;
    }
    return out;
}

std::wstring logs_folder() {
    const std::wstring runtime = runtime_info_path();  // the launcher's folder\runtime.ini
    const size_t slash = runtime.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring(L".") : runtime.substr(0, slash)) + L"\\logs";
}

void copy_summary() {
    const std::wstring text = session_summary_text(app.runtime);
    if (!OpenClipboard(app.hwnd))
        return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(memory), text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory))
            GlobalFree(memory);
    }
    CloseClipboard();
}

// Whether the description box's field is wholly inside the scrolling area: only then is the native edit
// shown (a child window cannot be clipped like the canvas); otherwise the canvas draws its text.
bool report_edit_fits() {
    const Rect& r = app.reportEditRect;
    return r.w > 0 && r.y >= app.scrollArea.y && r.bottom() <= app.scrollArea.bottom();
}

// Report an issue: the logs zipped into logs\ace-quadview-report-<time>.zip (with the issue's text as
// issue.md), Explorer showing it, and a new issue on the kind's form, its fields prefilled (report.h).
void report_issue() {
    const std::wstring logs = logs_folder();
    CreateDirectoryW(logs.c_str(), nullptr);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[64];
    swprintf_s(name, L"ace-quadview-report-%04u%02u%02u-%02u%02u%02u.zip", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);
    app.runtime = load_runtime_info(runtime_info_path());
    IssueReport report{widen(ACE_QV_VERSION), app.s, app.runtime, system_info(), name, app.reportKind};
    const int length = GetWindowTextLengthW(app.reportEdit);
    report.description.resize(static_cast<size_t>(std::max(length, 0)));
    GetWindowTextW(app.reportEdit, report.description.data(), length + 1);
    for (size_t at; (at = report.description.find(L"\r\n")) != std::wstring::npos;)  // the edit's line ends
        report.description.erase(at, 1);
    report.causes = likely_causes(app.s, app.runtime);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    std::vector<ReportFile> files =
        report_files({logs, runtime_info_path(), app.ini}, (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    files.push_back({L"issue.md", L"", utf8(issue_body(report))});
    const std::wstring zip = logs + L"\\" + name;
    const std::wstring error = write_zip(zip, files);
    if (error.empty()) {
        const std::wstring select = L"/select,\"" + zip + L"\"";
        ShellExecuteW(app.hwnd, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        app.status = Status::Error;
        app.error = L"Could not write the report's zip: " + error;
        report.zipName.clear();
        InvalidateRect(app.hwnd, nullptr, FALSE);
    }
    // [launcher] issue_repo: another repository with the forms, to try them before they reach this one's.
    wchar_t repo[128] = {};
    GetPrivateProfileStringW(L"launcher", L"issue_repo", kIssueRepo, repo, static_cast<DWORD>(std::size(repo)), app.ini.c_str());
    const std::string url = issue_url(repo, kIssueTemplates[std::clamp(report.kind, 0, kIssueKindCount - 1)],
                                      issue_title(report), issue_fields(report));
    ShellExecuteA(app.hwnd, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The Report an issue pane: what the last session points to, then the kind of problem, what happened and the
// button (report_issue).
float report_pane(float x, float y, float w, bool draw) {
    float h = 0;
    // Known causes the last session shows come first: many problems end there.
    const std::vector<Cause> causes = likely_causes(app.s, app.runtime);
    if (!causes.empty()) {
        std::vector<Row> rows;
        for (const Cause& c : causes) {
            Row row;
            row.label = c.title;
            row.desc = c.fix;
            row.control = [](const Rect&) {};  // text only
            rows.push_back(row);
        }
        h += section(x, y + h, w, L"Before you report", rows,
                     L"What the launcher noticed about the last session, and what to try. The report includes these.", draw);
        h += t::kSpace6;
    }
    Row kind;
    kind.label = L"Kind of problem";
    const std::vector<std::wstring> kinds(std::begin(kIssueKinds), std::end(kIssueKinds));
    kind.controlW = popup_width(kinds.data(), kIssueKindCount, 160);
    kind.controlH = t::kControlHeight;
    kind.control = [](const Rect& r) { popup_button(kDiagKind, r, kIssueKinds[app.reportKind], false); };
    Row description;
    constexpr float kEditH = 96;
    description.fullHeight = [](float) { return t::kBody.lineHeight + t::kSpace2 + kEditH; };
    description.full = [](const Rect& r) {
        app.canvas.text(L"What happened?", t::kBody, {r.x, r.y, r.w, t::kBody.lineHeight}, col(app.c->label));
        const Rect field{r.x, r.y + t::kBody.lineHeight + t::kSpace2, r.w, kEditH};
        app.canvas.fill(field, col(app.c->field_bg), t::kRadiusControl);
        app.canvas.stroke(field, col(app.c->control_border), 1, t::kRadiusControl);
        if (focused(kDiagDescription) || GetFocus() == app.reportEdit)
            focus_ring(field, t::kRadiusControl);
        app.reportEditRect = {field.x + 6, field.y + 5, field.w - 12, field.h - 10};
        app.hits.push_back({kDiagDescription, field});
        if (!IsWindowVisible(app.reportEdit) || !report_edit_fits()) {  // the edit is hidden: its text, clipped here
            std::wstring text(static_cast<size_t>(GetWindowTextLengthW(app.reportEdit)), L'\0');
            GetWindowTextW(app.reportEdit, text.data(), static_cast<int>(text.size()) + 1);
            app.canvas.push_clip(field);
            app.canvas.text(text.empty() ? L"What you saw, and what you expected instead." : text, t::kBody,
                            app.reportEditRect, col(text.empty() ? app.c->label_secondary : app.c->label), Align::Leading,
                            false, true);
            app.canvas.pop_clip();
        }
    };
    Row report;
    report.fullHeight = [](float) { return t::kControlHeight; };
    report.full = [](const Rect& r) {
        const float reportW = std::ceil(app.canvas.measure(L"Report an issue", kPrimaryLabel)) + 2 * t::kSpace4;
        button(kDiagReport, {r.x, r.y, reportW, t::kControlHeight}, L"Report an issue", true);
    };
    h += section(x, y + h, w, L"Describe the problem", {kind, description, report},
                 L"Zips the logs into the logs folder and shows it in Explorer, then opens a new issue on GitHub with "
                 L"what you wrote, your settings, the last session and your PC filled in. Add the steps there, and "
                 L"drag in the zip and any screenshots.",
                 draw);
    return h;
}

float diagnostics_pane(float x, float y, float w, bool draw) {
    const SessionInfo& s = app.runtime.session;
    const std::wstring dash = L"—";
    float h = 0;
    if (!s.known()) {
        const std::wstring none = L"No session measured yet. Start the game with this launcher: this page fills in "
                                  L"while it runs, and keeps the last session's numbers after it closes.";
        const float nh = app.canvas.measure_height(none, t::kBody, w);
        if (draw)
            app.canvas.text(none, t::kBody, {x, y + h, w, nh + 2}, col(app.c->label_secondary), Align::Leading, false, true);
        h += nh + t::kSpace6;
    } else {
        std::wstring head = s.time + L"  ·  " + s.mode;
        if (s.refreshHz > 0)
            head += formatted(L"  ·  %.0f Hz", s.refreshHz);
        head += s.frameGeneration ? L"  ·  Frame generation on" : L"  ·  Frame generation off";
        if (draw)
            app.canvas.text(head, t::kSubheadline, {x + t::kSpace3, y + h, w - t::kSpace3, 18}, col(app.c->label_secondary));
        h += 18 + t::kSpace4;
        const double budget = s.refreshHz > 0 ? 1000 / s.refreshHz : 0;
        std::vector<Row> rows = {
            value_row(L"Frame rate", L"", formatted(L"%.1f fps", s.fps)),
            value_row(L"Frame time", L"Median and 90th percentile: the slower frames, which you feel as stutter.",
                      s.frameP50 > 0 ? formatted(L"%.2f ms", s.frameP50) + formatted(L"  ·  %.2f ms", s.frameP90) : dash),
            value_row(L"Late frames", L"Frames that missed the headset's refresh.",
                      s.latePercent >= 0 ? formatted(L"%.1f%%", s.latePercent) : dash),
            value_row(L"CPU frame time",
                      budget > 0 ? formatted(L"ACE's render thread's work per frame. Close to %.1f ms, the CPU limits the frame.",
                                             budget)
                                 : L"ACE's render thread's work per frame.",
                      s.cpuMs >= 0 ? formatted(L"%.2f ms", s.cpuMs) : dash),
            value_row(L"GPU frame time", L"The whole GPU per ACE frame, the compositor and frame generation included.",
                      s.gpuMs >= 0 ? formatted(L"%.2f ms", s.gpuMs) + formatted(L"  ·  %.0f%% busy", s.gpuBusy) : dash),
            value_row(L"Limited by", L"", s.limited.empty() ? dash : s.limited),
            value_row(L"CPU in use", L"All of ACE's threads together.", s.cores >= 0 ? formatted(L"%.1f cores", s.cores) : dash)};
        h += section(x, y + h, w, L"Last session", rows, L"", draw);
        h += t::kSpace6;
        std::vector<Row> threads;
        for (const auto& [name, percent] : s.threads)
            threads.push_back(value_row(name, thread_note(name), formatted(L"%.0f%% of a core", percent)));
        if (threads.empty())
            threads.push_back(value_row(L"Not measured", L"", dash));
        h += section(x, y + h, w, L"Busiest threads", threads,
                     L"Each thread is a share of ONE core. A thread near 100% can hold the frame rate back even when "
                     L"Windows shows the whole CPU at 15%; a thread that spins by design looks busy whatever limits it.",
                     draw);
        h += t::kSpace6;
        const bool toolkit = s.layersBelow.find(L"MBUCCHIA_toolkit") != std::wstring::npos;
        std::vector<Row> layer;
        if (s.gazeMs >= 0)
            layer.push_back(value_row(L"Eye tracking lookup", L"On the layer's own thread, not ACE's.",
                                      formatted(L"%.3f ms", s.gazeMs)));
        layer.push_back(value_row(L"Layer on the render thread", L"ace-quadview's own time in each frame (composition apart).",
                                  s.layerMs >= 0 ? formatted(L"%.2f ms", s.layerMs) : dash));
        layer.push_back(value_row(L"Layers below", toolkit ? L"OpenXR Toolkit adds time to every frame's submission (about "
                                                             L"0.8 ms measured): switch it off for ACE in OpenXR layers if you "
                                                             L"don't use it."
                                                           : L"",
                                  s.layersBelow.empty() ? L"None" : friendly_layers(s.layersBelow)));
        if (app.runtime.vramUsedMb > 0)
            layer.push_back(value_row(L"VRAM", L"ACE's use against what Windows allows it.",
                                      formatted(L"%.1f GB", app.runtime.vramUsedMb / 1024) +
                                          formatted(L" of %.1f GB", app.runtime.vramBudgetMb / 1024)));
        h += section(x, y + h, w, L"Layer and memory", layer, L"", draw);
        h += t::kSpace6;
    }
    // Asking for help: the capture switch, copying the numbers above, the logs folder.
    Row capture;
    const std::wstring captureDesc =
        L"At the next launch, Ctrl+Alt+C while driving records for 20 s where ACE's busiest threads spend their time, "
        L"into a cpu-profile file in the logs folder. The headset menu shows when it starts and ends; the game may "
        L"stutter slightly while it records.";
    capture.fullHeight = [captureDesc](float width) { return checkbox_height(captureDesc, width); };
    capture.full = [captureDesc](const Rect& r) { checkbox(kDiagCapture, r, app.s.cpuProfile, L"CPU capture", captureDesc); };
    Row baseline;
    const std::wstring baselineDesc =
        L"Starts the game once with its own two views and QVFR off, still measuring, so this page can compare the "
        L"same race with and without QVFR. It turns itself off after that launch; the next one has QVFR again. The "
        L"performance overlay works in it, the settings menu does not.";
    baseline.fullHeight = [baselineDesc](float width) { return checkbox_height(baselineDesc, width); };
    baseline.full = [baselineDesc](const Rect& r) {
        checkbox(kDiagBaseline, r, app.s.baselineNext, L"Next launch without QVFR", baselineDesc);
    };
    Row actions;
    actions.fullHeight = [](float) { return t::kControlHeight; };
    actions.full = [](const Rect& r) {
        const float copyW = std::ceil(app.canvas.measure(L"Copy summary", t::kBody)) + 2 * t::kSpace3;
        const float logsW = std::ceil(app.canvas.measure(L"Open logs folder", t::kBody)) + 2 * t::kSpace3;
        button(kDiagCopy, {r.x, r.y, copyW, t::kControlHeight}, L"Copy summary", false);
        button(kDiagLogs, {r.x + copyW + t::kSpace3, r.y, logsW, t::kControlHeight}, L"Open logs folder", false);
    };
    std::wstring footer = L"To report a problem, use Report an issue in the sidebar: it zips these logs and fills in the "
                          L"issue. Elsewhere, send a screenshot of this page (or paste the summary) and the session's ace-*.log";
    footer += s.cpuProfile.empty() ? L"." : L", with the latest cpu-profile file.";
    h += section(x, y + h, w, L"Capture and share", {capture, baseline, actions}, footer, draw);
    return h;
}

// The wheels and controllers DirectInput finds (opened when the Hotkeys pane shows), for the pane's note.
void open_pads() {
    if (!app.padsOpen)
        app.padsOpen = app.pads.open(app.hwnd);
    else
        app.pads.rescan();  // a wheel plugged in since
}

std::wstring pads_found() {
    if (!app.padsOpen)
        return L"DirectInput isn’t available: wheel and controller buttons can’t be read.";
    std::wstring names;
    for (const auto& [id, name] : app.pads.devices())
        names += (names.empty() ? L"" : L", ") + name;
    return names.empty() ? L"No wheel or controller found: plug it in, then come back to this pane."
                         : L"Found: " + names + L".";
}

// Hotkeys ([hotkeys], input/hotkeys.h): each action's key and wheel button. Click one, then press.
float hotkeys_pane(float x, float y, float w, bool draw) {
    static const wchar_t* const kDescriptions[kHotkeyActions] = {
        L"Opens the headset menu, or closes it.",
        L"Selects the line above.",
        L"Selects the line below.",
        L"Lowers the selected value.",
        L"Raises the selected value.",
        L"Turns the performance overlay on or off.",
        L"Steps the helmet’s visor: closed, half, open.",
        L"Records a CPU capture, with CPU capture on in the Diagnostics pane."};
    const float slotW = 136;
    const auto row = [slotW](HotkeyAction action) {
        const int a = static_cast<int>(action);
        const Hotkey& h = app.s.hotkeys[a];
        Row r;
        r.label = kHotkeyLabels[a];
        r.desc = kDescriptions[a];
        if (h.pad.set())
            r.desc += L" Wheel button on " + (h.pad.name.empty() ? h.pad.device : h.pad.name) + L".";
        if (const int other = hotkey_clash(app.s.hotkeys, action, false); other >= 0)
            r.desc += L" " + key_text(h.key) + L" is also " + kHotkeyLabels[other] + L".";
        if (const int other = hotkey_clash(app.s.hotkeys, action, true); other >= 0)
            r.desc += L" Its wheel button is also " + std::wstring(kHotkeyLabels[other]) + L".";
        r.controlW = 2 * slotW + t::kSpace2;
        r.controlH = t::kControlHeight;
        r.control = [a, slotW](const Rect& rect) {
            const Hotkey& k = app.s.hotkeys[a];
            const bool keyCapture = app.captureAction == a && !app.capturePad, padCapture = app.captureAction == a && app.capturePad;
            const std::wstring keyLabel = keyCapture ? L"Press keys…" : k.key.set() ? key_text(k.key) : L"No key";
            const std::wstring padLabel = padCapture ? L"Press a button…" : k.pad.set() ? control_text(k.pad.control) : L"No wheel button";
            button(kHotkeyKey, {rect.x, rect.y, slotW, rect.h}, keyLabel, keyCapture);
            app.hits.back().option = a;
            button(kHotkeyPad, {rect.x + slotW + t::kSpace2, rect.y, slotW, rect.h}, padLabel, padCapture);
            app.hits.back().option = a;
        };
        return r;
    };
    float h = 0;
    h += section(x, y + h, w, L"Headset menu",
                 {row(HotkeyAction::MenuToggle), row(HotkeyAction::MenuUp), row(HotkeyAction::MenuDown),
                  row(HotkeyAction::MenuLeft), row(HotkeyAction::MenuRight)},
                 L"A key’s first press also opens the menu. A wheel’s directions act only while the menu shows, so a "
                 L"switch the game also uses doesn’t open it: use Open or close first. " + pads_found(),
                 draw) +
         t::kSpace6;
    h += section(x, y + h, w, L"In the game",
                 {row(HotkeyAction::Overlay), row(HotkeyAction::Visor), row(HotkeyAction::CpuCapture)},
                 L"Keys work while the game window has focus; wheel, button box and controller buttons work whenever "
                 L"the game runs. Click a button above, then press the keys or the wheel button: Esc cancels, Delete "
                 L"clears it. They apply to a running game at once.",
                 draw) +
         t::kSpace6;
    Row reset;
    reset.fullHeight = [](float) { return t::kControlHeight; };
    reset.full = [](const Rect& r) {
        const float resetW = std::ceil(app.canvas.measure(L"Reset hotkeys", t::kBody)) + 2 * t::kSpace3;
        button(kHotkeysReset, {r.x, r.y, resetW, t::kControlHeight}, L"Reset hotkeys", false);
    };
    h += section(x, y + h, w, L"", {reset}, L"Back to Ctrl+Alt+arrows, Ctrl+Alt+P, Ctrl+Alt+V and Ctrl+Alt+C, without wheel buttons. "
                                        L"Restore Defaults leaves the hotkeys as they are.",
                 draw);
    return h;
}

// Launch checks (preflight.h): what needs attention first, each with its fix, then what is ready.
float checks_pane(float x, float y, float w, bool draw) {
    static const wchar_t* const kToneText[] = {L"OK", L"Check", L"Problem", L"Checking"};  // CheckTone's order
    // Each row: its fix's button, else a badge with its state (the section says which need attention).
    const auto check_row = [](size_t i) {
        const Check& c = app.checks[i];
        Row row;
        row.label = c.title;
        row.desc = c.detail;
        if (!c.fixLabel.empty()) {
            row.controlW = std::ceil(app.canvas.measure(c.fixLabel, t::kBody)) + 2 * t::kSpace3;
            row.controlH = t::kControlHeight;
            row.control = [i](const Rect& r) {
                button(kCheckFix, r, app.checks[i].fixLabel, false);
                app.hits.back().option = static_cast<int>(i);
            };
        } else {
            const int tone = static_cast<int>(c.tone);  // the badge's tones are CheckTone's
            const std::wstring text = kToneText[tone];
            row.controlW = 7 + 12 + 5 + std::ceil(app.canvas.measure(text, {11, 14, 500, false})) + 9;
            row.controlH = 20;
            row.control = [tone, text](const Rect& r) { badge(r.x, r.y, tone, text); };
        }
        return row;
    };
    std::vector<Row> attention, ready;
    for (size_t i = 0; i < app.checks.size(); ++i)
        (app.checks[i].tone == CheckTone::Bad || app.checks[i].tone == CheckTone::Warn ? attention : ready).push_back(check_row(i));
    float h = 0;
    if (!attention.empty())
        h += section(x, y + h, w, L"Needs attention", attention,
                     L"Launch asks before starting the game while a problem would crash it or leave ace-quadview off.", draw) +
             t::kSpace6;
    if (!ready.empty())
        h += section(x, y + h, w, attention.empty() ? L"Ready to launch" : L"Ready", ready,
                     attention.empty() ? L"Nothing found that would stop ace-quadview at the next launch." : L"", draw);
    return h;
}

float pane_content(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool qvfr = app.s.mode == RenderMode::Qvfr;
    float h = 0;
    switch (app.pane) {
    case Pane::Checks: h += checks_pane(x, y, w, draw); break;
    case Pane::Hotkeys: h += hotkeys_pane(x, y, w, draw); break;
    case Pane::Rendering: {
        Row method, mode;
        method.label = L"Rendering method";
        {
            const std::wstring names[2] = {kMethodNames[0], kMethodNames[1]};
            method.controlW = popup_width(names, 2, 180);
        }
        method.controlH = t::kControlHeight;
        method.control = [](const Rect& r) { popup_button(kMethod, r, kMethodNames[app.s.mode == RenderMode::Qvfr ? 0 : 1], false); };
        mode.label = L"Foveated mode";
        mode.desc = app.s.eyeTracking ? L"The focus region follows your gaze." : L"The focus region stays at the center of each lens.";
        {
            const std::wstring names[2] = {kModeNames[0], kModeNames[1]};
            mode.controlW = popup_width(names, 2, 140);
        }
        mode.controlH = t::kControlHeight;
        mode.control = [](const Rect& r) { popup_button(kMode, r, kModeNames[app.s.eyeTracking ? 0 : 1], false); };
        h += section(x, y, w, L"Rendering", {method, mode},
                     qvfr ? L"Quad View Foveated Rendering draws a sharp focus view inside a lower-resolution outer view for each eye."
                          : L"Two-loop Foveated Interlaced renders each headset frame twice: the full view, then the focus view.",
                     draw);
        break;
    }
    case Pane::Resolution: {
        const bool known = app.runtime.known();
        const std::wstring footer =
            known ? L"Percent of the headset’s recommended resolution (" + std::to_wstring(app.runtime.width) + L" × " +
                        std::to_wstring(app.runtime.height) + L" px per eye). Outer and inner are set independently. Applies at next launch."
                  : L"Percent of the headset’s recommended resolution. Pixel sizes appear after the first launch. Outer and inner are "
                    L"set independently. Applies at next launch.";
        std::vector<Row> rows = {
            slider_row(kOuter, L"Outer resolution", L"The full field of view, rendered at lower density.", app.s.outer,
                       known ? px(ro.outer) : L""),
            slider_row(kInner, L"Inner resolution",
                       qvfr ? L"Pixel density inside the focus region."
                            : L"Set by outer resolution and foveated size in Two-loop Foveated Interlaced.",
                       qvfr ? app.s.inner : static_cast<int>(std::lround(ro.focusDensity * 100)),
                       known && qvfr ? px(ro.focus) : L"", !qvfr),
        };
        h += section(x, y, w, L"Per-eye resolution", rows, footer, draw);
        h += t::kSpace6;
        // The FOV crop: each side on its own, QVFR only.
        Row cropOn;
        const std::wstring cropDesc = qvfr ? L"Skips the edges of each eye that a helmet covers, so they are never rendered."
                                           : L"Available in Quad View Foveated Rendering.";
        cropOn.fullHeight = [cropDesc](float width) { return checkbox_height(cropDesc, width); };
        cropOn.full = [cropDesc](const Rect& r) { checkbox(kCropOn, r, app.s.crop, L"Crop the view", cropDesc); };
        const bool cropOff = !qvfr || !app.s.crop;
        h += section(x, y + h, w, L"Field of view crop",
                     {cropOn, slider_row(kCropTop, L"Top", L"", app.s.cropTop, L"", cropOff),
                      slider_row(kCropBottom, L"Bottom", L"", app.s.cropBottom, L"", cropOff),
                      slider_row(kCropOuter, L"Outer side", L"Toward the temple, mirrored for each eye.", app.s.cropOuter, L"",
                                 cropOff),
                      slider_row(kCropInner, L"Inner side", L"Toward the nose.", app.s.cropInner, L"", cropOff)},
                     L"Share of each eye’s field of view removed at that side; the headset shows black there. The outer "
                     L"view keeps its sharpness and the focus region its size. Applies live while the game runs; with DLSS "
                     L"on, or with less crop than at launch, the pixel count follows at the next launch.",
                     draw);
        h += t::kSpace6;
        Row helmetOn;
        const std::wstring helmetDesc = qvfr ? L"Draws a helmet’s visor opening around the view, lit by the scene."
                                             : L"Available in Quad View Foveated Rendering.";
        helmetOn.fullHeight = [helmetDesc](float width) { return checkbox_height(helmetDesc, width); };
        helmetOn.full = [helmetDesc](const Rect& r) { checkbox(kHelmetOn, r, app.s.helmet.on, L"Show a helmet", helmetDesc); };
        const bool helmetOff = !qvfr || !app.s.helmet.on;
        h += section(x, y + h, w, L"Helmet overlay",
                     {helmetOn,
                      slider_row(kHelmetRim, L"Rim", L"How far the padding reaches into the view.", app.s.helmet.rim, L"",
                                 helmetOff),
                      slider_row(kHelmetCorners, L"Corners", L"Rounds the visor opening.", app.s.helmet.corners, L"",
                                 helmetOff),
                      slider_row(kHelmetLight, L"Lighting", L"How much the scene lights the helmet.", app.s.helmet.light, L"",
                                 helmetOff),
                      [&] {
                          const std::wstring names[3] = {kVisorPositionNames[0], kVisorPositionNames[1], kVisorPositionNames[2]};
                          Row visor = choice_row(kVisor, L"Visor", names, 3, static_cast<int>(app.s.helmet.visor), helmetOff);
                          visor.desc = L"Your images only. Ctrl+Alt+V in the game steps it.";
                          return visor;
                      }(),
                      slider_row(kHelmetSize, L"Size", L"Your images only: how wide they are, in degrees of view.",
                                 app.s.helmet.size, L"", helmetOff, L"°"),
                      slider_row(kHelmetHeight, L"Height", L"Your images only: degrees above straight ahead.",
                                 app.s.helmet.height, L"", helmetOff, L"°"),
                      slider_row(kHelmetDepth, L"Depth",
                                 L"Your images only: how far in front of your eyes, in cm. 0 is far away, with no depth.",
                                 app.s.helmet.depth, L"", helmetOff, L" cm"),
                      [&] {
                          const std::wstring names[2] = {L"Performance (2048)", L"Detail (4096)"};
                          Row detail = choice_row(kHelmetDetail, L"Detail", names, 2, app.s.helmet.detail ? 1 : 0, helmetOff);
                          detail.desc = L"Your images only: Detail is sharper, Performance costs less GPU and memory.";
                          return detail;
                      }()},
                     L"Built in, the visor opening follows the crop (Rim and Corners). Your own helmet: top.png, bottom.png "
                     L"and visor.png with alpha in the helmet folder next to the layer; the log suggests the crop that "
                     L"matches them. Applies live.",
                     draw);
        break;
    }
    case Pane::Foveation: {
        if (eye_warning()) {
            if (draw)
                badge(x, y, 1, L"Eye tracker not detected. The focus region stays centered until one connects.", w);
            h += 20 + t::kSpace6;
        }
        h += section(x, y + h, w, L"Focus region",
                     {slider_row(kWidth, L"Foveated width", L"", app.s.width, L""),
                      slider_row(kHeight, L"Foveated height", L"", app.s.height, L""),
                      slider_row(kCorner, L"Corner radius",
                                 app.s.shape.corners ? L"Box shape only." : L"Rounds the corners of the focus region.",
                                 app.s.corner, L"", app.s.shape.corners),
                      app.s.eyeTracking
                          ? slider_row(kFocusOffset, L"Gaze offset",
                                       L"Moves the focus region above (+) or below (−) where you look, if it sits off.",
                                       app.s.offset.gaze, L"", false, L"°")
                          : slider_row(kFocusOffset, L"Vertical position",
                                       L"Raises (+) or lowers (−) the fixed focus region, e.g. toward the road or the dash.",
                                       app.s.offset.fixed, L"", false, L"°"),
                      slider_row(kGazeDeadzone, L"Gaze dead-zone",
                                 L"How far your gaze moves before the focus region follows. Higher holds it steadier "
                                 L"against eye-tracker jitter (shimmer with MSAA); 0 follows every sample.",
                                 app.s.gazeDeadzone, L"", !app.s.eyeTracking, L"°", true)},
                     L"Share of the outer view that the focus view covers. Fixed and eye-tracked modes keep their own "
                     L"vertical offset. All apply live.",
                     draw);
        h += t::kSpace6;
        {
            const std::wstring names[2] = {kShapeNames[0], kShapeNames[1]};
            Row shape = choice_row(kShape, L"Shape", names, 2, app.s.shape.corners ? 1 : 0, false);
            shape.desc = L"Box: a rectangle, with the corner radius above. Corners: move each corner in on its own.";
            const bool off = !app.s.shape.corners;
            const auto corner = [&](Ctl id, const wchar_t* label) {
                return slider_row(id, label, L"", *slider_value(id), L"", off);
            };
            h += section(x, y + h, w, L"Focus shape",
                         {shape, corner(kShapeTopLeftIn, L"Top left in"), corner(kShapeTopLeftDown, L"Top left down"),
                          corner(kShapeTopRightIn, L"Top right in"), corner(kShapeTopRightDown, L"Top right down"),
                          corner(kShapeBottomLeftIn, L"Bottom left in"), corner(kShapeBottomLeftUp, L"Bottom left up"),
                          corner(kShapeBottomRightIn, L"Bottom right in"),
                          corner(kShapeBottomRightUp, L"Bottom right up")},
                         L"Each corner moves toward the middle of the focus region, percent of its width (in) or "
                         L"height (down, up). Pull the top corners in for a trapezoid: more of the dash sharp, less "
                         L"of the far field. The focus view still renders the whole box, so the cost stays the same. "
                         L"Applies live.",
                         draw);
        }
        break;
    }
    case Pane::Overlay: {
        Row show;
        const std::wstring desc = L"Draws a colored border around the focus region.";
        show.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
        show.full = [desc](const Rect& r) { checkbox(kOutline, r, app.s.outline, L"Show focus outline", desc); };
        Row color;
        color.label = L"Outline color";
        color.disabled = !app.s.outline;
        color.controlW = segmented_width();
        color.controlH = t::kControlHeight;
        color.control = [](const Rect& r) { segmented(kColor, r, static_cast<int>(app.s.color), !app.s.outline); };
        h += section(x, y, w, L"Focus overlay",
                     {show, color, slider_row(kFeather, L"Feather", L"Softens the edge of the focus region.", app.s.feather, L"")},
                     L"The outline is drawn into the headset image, to show where the focus region is.", draw);
        break;
    }
    case Pane::Dlss: {
        // Cheeky's author asked for this credit, with the version, wherever the UI controls Cheeky.
        const std::wstring credit = cheeky_version_label(app.runtime.cheekyVersion) +
                                    (app.runtime.cheekyVersion.empty() ? L"  ·  its version shows after a launch with it" : L"");
        if (draw)
            app.canvas.text(credit, t::kSubheadline, {x + t::kSpace3, y, w - t::kSpace3, 16}, col(app.c->label_secondary));
        h += 16 + t::kSpace4;
        const std::wstring runtime = cheeky_runtime_path(app.ini, layer_folder());
        const bool found = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        const CheekyLayer layer = cheeky_layer_state();
        const auto badge_row = [](const std::wstring& label, const std::wstring& desc, int tone, const std::wstring& text) {
            Row row;
            row.label = label;
            row.desc = desc;
            row.controlW = 7 + 12 + 5 + std::ceil(app.canvas.measure(text, {11, 14, 500, false})) + 9;
            row.controlH = 20;
            row.control = [tone, text](const Rect& r) { badge(r.x, r.y, tone, text); };
            return row;
        };
        Row use;
        const std::wstring useDesc = L"Loads Cheeky’s DLSS runtime into Assetto Corsa EVO, so the outer views can use a "
                                     L"cheaper DLSS pass. Applies at the next launch.";
        use.fullHeight = [useDesc](float width) { return checkbox_height(useDesc, width); };
        use.full = [useDesc](const Rect& r) { checkbox(kCheekyUse, r, app.s.cheeky.load, L"Use Cheeky Foveated DLSS", useDesc); };
        Row rt;
        rt.label = L"Runtime";
        rt.desc = (found ? L"" : L"Not found: ") + runtime;
        rt.controlW = std::ceil(app.canvas.measure(L"Choose…", t::kBody)) + 2 * t::kSpace3;
        rt.controlH = t::kControlHeight;
        rt.control = [](const Rect& r) { button(kCheekyRuntime, r, L"Choose…", false); };
        const Row xr = layer == CheekyLayer::Enabled
                           ? badge_row(L"OpenXR layer", L"Cheeky’s layer tells its runtime the frame timing it needs.", 0, L"Installed")
                       : layer == CheekyLayer::Disabled
                           ? badge_row(L"OpenXR layer", L"Registered but turned off: run CheekyOpenXRSetup.exe again.", 1, L"Turned off")
                           : badge_row(L"OpenXR layer", L"Install it with CheekyOpenXRSetup.exe from Cheeky’s release.", 2, L"Not installed");
        const std::wstring& status = app.runtime.cheekyStatus;
        const std::wstring last = status == L"running"   ? L"Cheeky ran at the last launch."
                                  : status == L"missing" ? L"At the last launch, Cheeky’s runtime was not found."
                                  : status == L"refused" ? L"At the last launch, Cheeky did not start: see its log next to its runtime."
                                                         : L"";
        h += section(x, y + h, w, L"Cheeky Foveated DLSS", {use, rt, xr},
                     L"Put the CheekyFoveatedDLSS folder from Cheeky’s Standalone ZIP next to ace-quadview.dll, or choose "
                     L"its runtime. No Cheeky loader goes in the game folder. " + last,
                     draw) + t::kSpace6;
        const bool outer = app.s.cheeky.outerDlaa;
        Row on, cheap;
        const std::wstring onDesc = L"Off leaves ACE’s own DLSS on every view.";
        on.fullHeight = [onDesc](float width) { return checkbox_height(onDesc, width); };
        on.full = [onDesc](const Rect& r) { checkbox(kCheekyOn, r, app.s.cheeky.processing, L"Foveated DLSS", onDesc); };
        const std::wstring cheapDesc = L"The outer views get Cheeky’s own DLAA pass at a lower resolution instead of ACE’s DLSS.";
        cheap.fullHeight = [cheapDesc](float width) { return checkbox_height(cheapDesc, width); };
        cheap.full = [cheapDesc](const Rect& r) { checkbox(kCheekyOuter, r, app.s.cheeky.outerDlaa, L"Cheaper outer views", cheapDesc); };
        std::wstring presetNames[std::size(kCheekyPresets)];
        for (size_t i = 0; i < std::size(kCheekyPresets); ++i)
            presetNames[i] = kCheekyPresets[i].name;
        const float presetW = popup_width(presetNames, static_cast<int>(std::size(presetNames)), 140);
        const auto preset_row = [presetW](Ctl id, const std::wstring& label, const std::wstring& desc, int value, bool disabled) {
            Row row;
            row.label = label;
            row.desc = desc;
            row.disabled = disabled;
            row.controlW = presetW;
            row.controlH = t::kControlHeight;
            row.control = [id, value, disabled](const Rect& r) { popup_button(id, r, cheeky_preset_name(value), disabled); };
            return row;
        };
        h += section(x, y + h, w, L"Processing",
                     {on, cheap,
                      slider_row(kCheekyScale, L"Outer DLAA scale", L"That pass’s resolution, a share of ACE’s render size.",
                                 app.s.cheeky.outerScale, L"", !outer),
                      preset_row(kCheekyOuterPreset, L"Outer preset", L"", app.s.cheeky.outerPreset, !outer),
                      preset_row(kCheekyFocusPreset, L"Focus preset", L"DLSS preset for the sharp focus views.",
                                 app.s.cheeky.focusPreset, false)},
                     L"Applies live while the game runs. In Quad View Foveated Rendering, ace-quadview tells Cheeky which "
                     L"views are outer and which are focus.",
                     draw);
        h += t::kSpace6;
        Row fix, nr;
        const std::wstring fixDesc = L"Cheeky’s workaround for smearing with some presets. Adds GPU work.";
        fix.fullHeight = [fixDesc](float width) { return checkbox_height(fixDesc, width); };
        fix.full = [fixDesc](const Rect& r) { checkbox(kCheekyMotionFix, r, app.s.cheeky.motionFix, L"Fix motion-vector blur", fixDesc); };
        const std::wstring nrDesc = L"NVIDIA’s DLSS-NR (DLSS 5) on the focus views. Needs nvngx_dlssnr.dll next to the game.";
        nr.fullHeight = [nrDesc](float width) { return checkbox_height(nrDesc, width); };
        nr.full = [nrDesc](const Rect& r) { checkbox(kCheekyNr, r, app.s.cheeky.nr, L"DLSS-NR", nrDesc); };
        const std::wstring nrPresets[] = {kCheekyNrPresets[0], kCheekyNrPresets[1], kCheekyNrPresets[2]};
        const std::wstring nrPasses[] = {L"1 pass", L"2 passes", L"3 passes"};
        std::vector<Row> experimental = {
            slider_row(kCheekySupersampling, L"Focus supersampling",
                       L"Runs the focus views’ DLSS above their size for extra sharpness, at a GPU cost.",
                       app.s.cheeky.supersampling, L""),
            fix, nr,
            choice_row(kCheekyNrPreset, L"DLSS-NR preset", nrPresets, 3, std::clamp(app.s.cheeky.nrPreset, 0, 2), !app.s.cheeky.nr),
            choice_row(kCheekyNrPasses, L"DLSS-NR passes", nrPasses, 3, std::clamp(app.s.cheeky.nrPasses, 1, 3) - 1,
                       !app.s.cheeky.nr)};
        h += section(x, y + h, w, L"Experimental: focus views", experimental,
                     L"Cheeky applies these to the focus views only. Applies live. Each extra DLSS-NR pass costs about "
                     L"one more NR evaluation.",
                     draw);
        h += t::kSpace6;
        Row fgOn;
        const std::wstring fgDesc = L"Cheeky’s DLSS-G adds generated frames between ACE’s, guided by its depth and motion. "
                                    L"Turn OpenXR Toolkit’s Turbo mode and other frame generation (VectorXR’s) off.";
        fgOn.fullHeight = [fgDesc](float width) { return checkbox_height(fgDesc, width); };
        fgOn.full = [fgDesc](const Rect& r) { checkbox(kFgOn, r, app.s.cheeky.fg, L"Frame generation", fgDesc); };
        const bool fgOff = !app.s.cheeky.fg;
        Row fgBorder;
        const std::wstring borderDesc = L"Outlines the region in the headset, to check that it follows your eyes.";
        fgBorder.fullHeight = [borderDesc](float width) { return checkbox_height(borderDesc, width); };
        fgBorder.full = [borderDesc](const Rect& r) {
            checkbox(kFgBorder, r, app.s.cheeky.fgBorder, L"Show the center region", borderDesc);
        };
        const std::wstring multipliers[] = {kFgMultipliers[0], kFgMultipliers[1], kFgMultipliers[2]};
        const std::wstring quarters[] = {kFgQuarters[0], kFgQuarters[1], kFgQuarters[2], kFgQuarters[3]};
        h += section(x, y + h, w, L"Frame generation (experimental)",
                     {fgOn,
                      choice_row(kFgMultiplier, L"Multiplier", multipliers, 3, std::clamp(app.s.cheeky.fgMultiplier - 2, 0, 2), fgOff),
                      slider_row(kFgSize, L"Center region", L"Where frames are generated at full quality, a share of each eye.",
                                 app.s.cheeky.fgSize, L"", fgOff),
                      choice_row(kFgScale, L"Center FG scale", quarters, 4, std::clamp(app.s.cheeky.fgScale / 25 - 1, 0, 3), fgOff),
                      choice_row(kFgPeriphery, L"Periphery FG scale", quarters, 4,
                                 std::clamp(app.s.cheeky.fgPeriphery / 25 - 1, 0, 3), fgOff),
                      [&] {
                          const std::wstring names[3] = {kCheekyFgReleaseNames[0], kCheekyFgReleaseNames[1],
                                                         kCheekyFgReleaseNames[2]};
                          Row release = choice_row(kFgRelease, L"Release ACE", names, 3,
                                                   cheeky_fg_release_index(app.s.cheeky.fgRelease), fgOff);
                          release.desc = L"When ACE starts its next frame. Never: CPU and GPU overlap (most frames). "
                                         L"Capture: after the GPU finishes. Generated: after the generated frame.";
                          return release;
                      }(),
                      fgBorder},
                     L"Outside the center region frames are warped with ACE’s motion. Lower scales use less GPU time "
                     L"and memory. Applies live. With an eye-tracked focus (Rendering pane) the region follows your eyes too, through Cheeky’s OpenXR layer.",
                     draw);
        const std::wstring license = L"Cheeky Foveated DLSS is made by ClarkCheekyKent and is free software under the "
                                     L"GNU GPL v3 (github.com/ClarkCheekyKent/CheekyFoveatedDLSS).";
        const float lh = app.canvas.measure_height(license, t::kCallout, w);
        if (draw)
            app.canvas.text(license, t::kCallout, {x, y + h + t::kSpace6, w, lh + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
        h += t::kSpace6 + lh;
        break;
    }
    case Pane::Menu: {
        h += section(x, y, w, L"Placement",
                     {slider_row(kMenuYaw, L"Horizontal", L"Degrees right of straight ahead. Negative is left.", app.s.menuYaw,
                                 L"", false, L"°"),
                      slider_row(kMenuPitch, L"Vertical", L"Degrees above eye level. Negative is below.", app.s.menuPitch, L"",
                                 false, L"°"),
                      slider_row(kMenuWidth, L"Size", L"Width of the panel, in degrees of your view.", app.s.menuWidth, L"",
                                 false, L"°")},
                     L"The settings menu stays 0.8 m in front of your head, wherever you look. Drag it in the preview, "
                     L"or move it from the menu itself in the headset. While the game runs, it moves at once.",
                     draw);
        const std::wstring open = L"Ctrl+Alt+arrows while the game has focus, or click the left controller stick. The right "
                                  L"stick selects and changes values; the menu hides after a few seconds.";
        const float oh = app.canvas.measure_height(open, t::kCallout, w - 2 * t::kSpace3);
        if (draw) {
            app.canvas.text(L"Opening the menu", t::kHeadline, {x + t::kSpace3, y + h + t::kSpace6, w - t::kSpace3, 16},
                            col(app.c->label));
            app.canvas.text(open, t::kCallout, {x + t::kSpace3, y + h + t::kSpace6 + 16 + 6, w - 2 * t::kSpace3, oh + 2},
                            col(app.c->label_secondary), Align::Leading, false, true);
        }
        h += t::kSpace6 + 16 + 6 + oh;
        h += t::kSpace6;
        Row osdOn;
        const std::wstring osdDesc = L"Frame rate and pacing, CPU and GPU time with their headroom and which one limits the "
                                     L"frame, VRAM, frame generation and eye tracking, floating in your view.";
        osdOn.fullHeight = [osdDesc](float width) { return checkbox_height(osdDesc, width); };
        osdOn.full = [osdDesc](const Rect& r) { checkbox(kOsdOn, r, app.s.osd.enabled, L"Show the overlay", osdDesc); };
        const bool osdOff = !app.s.osd.enabled;
        h += section(x, y + h, w, L"Performance overlay",
                     {osdOn,
                      slider_row(kOsdYaw, L"Horizontal", L"Degrees right of straight ahead. Negative is left.", app.s.osd.yaw,
                                 L"", osdOff, L"°"),
                      slider_row(kOsdPitch, L"Vertical", L"Degrees above eye level. Negative is below.", app.s.osd.pitch, L"",
                                 osdOff, L"°"),
                      slider_row(kOsdSize, L"Size", L"Width of the overlay, in degrees of your view.", app.s.osd.width, L"",
                                 osdOff, L"°")},
                     L"Drag it in the preview, or move it from the headset menu’s Overlay and menu page. Ctrl+Alt+P "
                     L"while the game has focus turns it on and off. While the game runs, it changes at once.",
                     draw);
        break;
    }
    case Pane::Layers: h += layers_pane(x, y, w, draw); break;
    case Pane::Diagnostics: h += diagnostics_pane(x, y, w, draw); break;
    case Pane::Report: h += report_pane(x, y, w, draw); break;
    case Pane::About: {
        if (draw) {
            app.canvas.text(L"QVFR Launcher", t::kTitle1, {x, y, w, 26}, col(app.c->label));
            app.canvas.text(L"Starts Assetto Corsa EVO with foveated rendering enabled.", t::kBody, {x, y + 26 + 8, w, 16},
                            col(app.c->label_secondary));
            app.canvas.text(L"Version " + widen(ACE_QV_VERSION), t::kBody, {x, y + 26 + 8 + 16 + 8, w, 16},
                            col(app.c->label_secondary));
        }
        h += 26 + 8 + 16 + 8 + 16;
        // The same notice as the README's: unofficial, at the user's own risk.
        const std::wstring notice =
            L"ace-quadview is not made, approved or sanctioned by Kunos Simulazioni or the Assetto Corsa EVO "
            L"developers. It hooks the game's code and changes how it renders, so a game update can break it, and "
            L"it can crash the game. Use it in offline sessions only, and please don't report problems you see "
            L"while using it to the ACE developers.";
        const float nh = app.canvas.measure_height(notice, t::kCallout, w);
        if (draw) {
            app.canvas.text(L"Experimental and unofficial. Use at your own risk.", t::kHeadline,
                            {x, y + h + t::kSpace6, w, 16}, col(app.c->label));
            app.canvas.text(notice, t::kCallout, {x, y + h + t::kSpace6 + 16 + t::kSpace2, w, nh + 2},
                            col(app.c->label_secondary), Align::Leading, false, true);
        }
        h += t::kSpace6 + 16 + t::kSpace2 + nh;
        // Support: a line and a button that opens the Ko-fi page in the browser.
        const std::wstring support = L"If ace-quadview improves your races, you can support its development on Ko-fi.";
        const float sh = app.canvas.measure_height(support, t::kCallout, w);
        if (draw)
            app.canvas.text(support, t::kCallout, {x, y + h + t::kSpace6, w, sh + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
        h += t::kSpace6 + sh + t::kSpace2;
        const float bw = std::ceil(app.canvas.measure(L"Support on Ko-fi", t::kBody)) + 2 * t::kSpace3;
        if (draw)
            button(kSupport, {x, y + h, bw, t::kControlHeight}, L"Support on Ko-fi", false);
        h += t::kControlHeight;
        break;
    }
    }
    return h;
}

// FoveationPreview (one eye, a schematic) at (x, y), width w. Returns its height.
float preview(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool known = app.runtime.known();
    // The whole eye (the FOV crop is drawn over it); without a recommended size, the kit's example
    // aspect (2584 x 2160) shapes the drawing.
    const Size outer = known ? ro.runtime : Size{2584, 2160};
    const float W = 320, H = std::round(W * outer.height / std::max(1, outer.width)), k = w / W;
    const float frameH = H * k;
    if (draw) {
        const Rect frame{x, y, w, frameH};
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        const auto R = [&](float rx, float ry, float rw, float rh) { return Rect{x + rx * k, y + ry * k, rw * k, rh * k}; };
        const bool tfi = app.s.mode == RenderMode::TwoLoop;
        const bool eye = app.s.eyeTracking && !eye_warning();
        const float fw = W * app.s.width / 100, fh = H * app.s.height / 100;
        // Centred on the lens in both modes (eye tracked: looking straight ahead), moved by the vertical
        // offset, roughly: about 100 degrees of view top to bottom.
        const float gx = 0.5f, gy = 0.5f - app.s.offset.for_mode(app.s.eyeTracking) / 100.0f;
        // The kept view (the left eye: its outer side is on the left); the focus region stays inside it.
        const float cl = W * static_cast<float>(app.s.crop_width() < 1 ? app.s.cropOuter : 0) / 100,
                    cr = W * static_cast<float>(app.s.crop_width() < 1 ? app.s.cropInner : 0) / 100,
                    ct = H * static_cast<float>(app.s.crop_height() < 1 ? app.s.cropTop : 0) / 100,
                    cb = H * static_cast<float>(app.s.crop_height() < 1 ? app.s.cropBottom : 0) / 100;
        const float fx = std::clamp(gx * W - fw / 2, cl, std::max(cl, W - cr - fw)),
                    fy = std::clamp(gy * H - fh / 2, ct, std::max(ct, H - cb - fh));
        // Grid cell size shows pixel density: 8 in the periphery, 8 / ratio in the focus region.
        const double ratio = tfi ? ro.focusDensity * 100 / app.s.outer : ro.focusDensity * 100 / app.s.outer;
        const float pc = 8, fc = std::clamp(static_cast<float>(pc / std::max(0.01, ratio)), 2.0f, 8.0f);
        app.canvas.fill(R(0, 0, W, H), col(app.c->preview_periphery));
        for (float gxl = 0; gxl <= W; gxl += pc)
            app.canvas.line(x + gxl * k, y, x + gxl * k, y + frameH, col(app.c->preview_canvas), 0.6f * k);
        for (float gyl = 0; gyl <= H; gyl += pc)
            app.canvas.line(x, y + gyl * k, x + w, y + gyl * k, col(app.c->preview_canvas), 0.6f * k);
        if (tfi)
            for (float iy = 0; iy < H; iy += 4)
                app.canvas.fill(R(0, iy, W, 2), col(app.c->preview_canvas, 0.55f));
        // Feather: the focus region fades in over feather % of its size, as the compositor blends it;
        // its corners rounded by corner % of half the shorter side, or with the corner shape, each corner
        // pulled in (focus_shape.h; its feather drawn as the quadrilateral shrunk toward its middle).
        const float band = std::min(fw, fh) * app.s.feather / 100.0f;
        const float corner = app.s.shape.corners ? 0.0f : std::min(fw, fh) / 2 * app.s.corner / 100.0f;
        const auto quad = [&](float in) {  // the corner shape, `in` from its edges across and down
            ComPtr<ID2D1PathGeometry> path;
            app.canvas.factory()->CreatePathGeometry(&path);
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            const float sx = (fw - 2 * in) / fw, sy = (fh - 2 * in) / fh;
            for (int i = 0; i < kFocusCorners; ++i) {
                float u, v;
                focus_corner_point(app.s.shape, static_cast<FocusCorner>(i), u, v);
                const D2D1_POINT_2F p{x + (fx + fw / 2 + (u - 0.5f) * fw * sx) * k, y + (fy + fh / 2 + (v - 0.5f) * fh * sy) * k};
                if (i == 0)
                    sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
                else
                    sink->AddLine(p);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            return path;
        };
        const int steps = band > 0.5f ? 6 : 1;
        for (int i = 0; i < steps; ++i) {
            const float in = band * i / steps, alpha = steps == 1 ? 1.0f : static_cast<float>(i + 1) / steps;
            if (app.s.shape.corners)
                app.canvas.fill(quad(in).Get(), col(app.c->preview_focus, alpha));
            else
                app.canvas.fill(R(fx + in, fy + in, fw - 2 * in, fh - 2 * in), col(app.c->preview_focus, alpha),
                                std::max(0.0f, corner - in) * k);
        }
        // The focus region's finer grid (the kit's focus pattern), on its own grid origin, inside its shape.
        ComPtr<ID2D1Geometry> shape;
        if (app.s.shape.corners) {
            shape = quad(0);
        } else {
            ComPtr<ID2D1RoundedRectangleGeometry> box;
            app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(R(fx, fy, fw, fh).d2d(), corner * k, corner * k), &box);
            shape = box;
        }
        ComPtr<ID2D1Layer> focusLayer;
        rt->CreateLayer(&focusLayer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), focusLayer.Get());
        for (float lx = fx; lx <= fx + fw; lx += fc)
            app.canvas.line(x + lx * k, y + fy * k, x + lx * k, y + (fy + fh) * k, col(app.c->preview_periphery), 0.4f * k);
        for (float ly = fy; ly <= fy + fh; ly += fc)
            app.canvas.line(x + fx * k, y + ly * k, x + (fx + fw) * k, y + ly * k, col(app.c->preview_periphery), 0.4f * k);
        rt->PopLayer();
        if (app.s.outline && app.s.shape.corners)
            app.canvas.stroke(quad(1).Get(), col(overlay(static_cast<int>(app.s.color))), 2 * k);
        else if (app.s.outline)
            app.canvas.stroke(R(fx, fy, fw, fh), col(overlay(static_cast<int>(app.s.color))), 2 * k, corner * k);
        // The cropped edges, as the headset shows them; with the helmet, its rim reaches into the view.
        const bool helmet = app.s.mode == RenderMode::Qvfr && app.s.helmet.on;
        const float rim = helmet ? H * app.s.helmet.rim / 100.0f : 0;
        const Color cropped = col(app.c->preview_canvas, helmet ? 1.0f : 0.9f);
        app.canvas.fill(R(0, 0, W, ct + rim), cropped);
        app.canvas.fill(R(0, H - cb - rim, W, cb + rim), cropped);
        app.canvas.fill(R(0, ct, cl + rim, H - ct - cb), cropped);
        app.canvas.fill(R(W - cr - rim, ct, cr + rim, H - ct - cb), cropped);
        if (eye) {
            const float cx = x + gx * W * k, cy = y + gy * H * k;
            const Color label = col(app.c->preview_label);
            app.canvas.circle(cx, cy, 5 * k, label, false, 1.2f * k);
            app.canvas.line(cx - 9 * k, cy, cx - 5 * k, cy, label, 1.2f * k);
            app.canvas.line(cx + 5 * k, cy, cx + 9 * k, cy, label, 1.2f * k);
            app.canvas.line(cx, cy - 9 * k, cx, cy - 5 * k, label, 1.2f * k);
            app.canvas.line(cx, cy + 5 * k, cx, cy + 9 * k, label, 1.2f * k);
        }
        const t::TypeStyle tiny{9.5f * k, 12 * k, 400, false};
        if (known) {
            app.canvas.text(L"Focus " + dims(ro.focus), tiny, R(fx + 6, fy + 4, W - fx - 6, 12), col(app.c->preview_label), Align::Leading, false);
            app.canvas.text(L"Outer " + dims(ro.outer), tiny, R(cl + 8, H - cb - 17, W - cl - cr - 16, 12), col(app.c->preview_label),
                            Align::Leading, false);
        }
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
    }
    float h = frameH + t::kSpace2;
    // Caption: method · mode · focus density.
    const std::wstring caption = std::wstring(kMethodNames[app.s.mode == RenderMode::Qvfr ? 0 : 1]) + L"  ·  " +
                                 (app.s.eyeTracking && !eye_warning() ? L"Eye tracked" : L"Fixed") + L"  ·  Focus density " +
                                 std::to_wstring(static_cast<int>(std::lround(ro.focusDensity * 100))) + L"%";
    const float ch = app.canvas.measure_height(caption, t::kSubheadline, w);
    if (draw)
        app.canvas.text(caption, t::kSubheadline, {x, y + h, w, ch + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    h += ch + t::kSpace4;
    // Readouts.
    const t::TypeStyle dt{12, 16, 400, false};
    const std::wstring dash = L"—";
    char mp[32], saved[48];
    std::snprintf(mp, sizeof(mp), "%.2f M of %.2f M", ro.pixels / 1e6, ro.runtimePixels / 1e6);
    const int savedPercent = static_cast<int>(std::lround(ro.saved * 100));
    if (savedPercent >= 0)
        std::snprintf(saved, sizeof(saved), "%d%%", savedPercent);
    else
        std::snprintf(saved, sizeof(saved), "None (%d%% more)", -savedPercent);
    // VRAM, scaled from the last QVFR launch's measurement (settings.h's vram_estimate).
    const VramEstimate vram = vram_estimate(app.s, ro, app.runtime);
    char vramText[48] = "";
    if (vram.budgetMb > 0)
        std::snprintf(vramText, sizeof(vramText), "%.1f GB of %.1f GB", vram.mb / 1024, vram.budgetMb / 1024);
    else
        std::snprintf(vramText, sizeof(vramText), "%.1f GB", vram.mb / 1024);
    const std::pair<std::wstring, std::wstring> lines[] = {
        {L"Runtime resolution", known ? dims(ro.runtime) : dash},
        {L"Outer view", known ? dims(ro.outer) : dash},
        {L"Focus view", known ? dims(ro.focus) : dash},
        {L"Pixels per frame", known ? widen(mp) : dash},
        {L"Pixels saved", known ? widen(saved) : dash},
        {L"Center PPD", ro.centerPpd > 0 ? std::to_wstring(std::lround(ro.centerPpd)) : dash},
        {L"VRAM (estimate)", known && vram.mb > 0 ? widen(vramText) : dash},
    };
    for (const auto& [name, value] : lines) {
        if (draw) {
            app.canvas.text(name, dt, {x, y + h, w / 2, 16}, col(app.c->label_secondary));
            app.canvas.text(value, t::kValue, {x + w / 2, y + h, w / 2, 16}, col(app.c->label), Align::Trailing);
        }
        h += 16 + 4;
    }
    return h - 4;
}

// Angular size of a panel: its height in degrees for a width in degrees, at any distance.
float panel_height_deg(int widthDeg, float aspect) {
    constexpr float kRad = 3.14159265f / 180;
    return 2 * std::atan(std::tan(widthDeg * kRad / 2) * aspect) / kRad;
}
float menu_height_deg(int widthDeg) { return panel_height_deg(widthDeg, kMenuAspect); }
float osd_height_deg(int widthDeg) { return panel_height_deg(widthDeg, static_cast<float>(kOsdHeight) / kOsdWidth); }

std::wstring degrees(int v, const wchar_t* plus, const wchar_t* minus) {
    return v == 0 ? L"0°" : std::to_wstring(std::abs(v)) + L"° " + (v > 0 ? plus : minus);
}

// The settings menu's place (Headset menu pane) at (x, y), width w: a kRefFovX x kRefFovY view in
// degrees with the panel, drawn in its own colours, where it floats. Returns its height.
float placement_preview(float x, float y, float w, bool draw) {
    const float k = w / kRefFovX, frameH = std::round(kRefFovY * k);
    const Rect frame{x, y, w, frameH};
    const float pw = app.s.menuWidth * k, ph = menu_height_deg(app.s.menuWidth) * k;
    const float pcx = x + (kRefFovX / 2 + app.s.menuYaw) * k, pcy = y + (kRefFovY / 2 - app.s.menuPitch) * k;
    const Rect panel{pcx - pw / 2, pcy - ph / 2, pw, ph};
    if (draw) {
        app.placeFrame = frame;
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        app.canvas.fill(frame, col(app.c->preview_periphery));
        // A 10° grid, with the horizon and straight ahead stronger.
        for (float d = 10; d < kRefFovX; d += 10)
            app.canvas.line(x + d * k, y, x + d * k, y + frameH, col(app.c->preview_canvas), 0.6f);
        for (float d = 10; d < kRefFovY; d += 10)
            app.canvas.line(x, y + d * k, x + w, y + d * k, col(app.c->preview_canvas), 0.6f);
        const float cx = x + w / 2, cy = y + frameH / 2;
        const Color axis = col(app.c->preview_label, 0.35f);
        app.canvas.line(cx, y, cx, y + frameH, axis, 1);
        app.canvas.line(x, cy, x + w, cy, axis, 1);
        app.canvas.circle(cx, cy, 5, col(app.c->preview_label, 0.8f), false, 1.2f);
        // The panel as the headset shows it: title, resolution line, the settings rows (the first
        // selected) and the hint, in its own colours (in panel pixels, 1024 x 972).
        const bool active = app.dragging == kMenuPanel;
        app.canvas.fill({panel.x, panel.y + 2, panel.w, panel.h}, {0, 0.35f}, 4);
        app.canvas.fill(panel, col(kMenuBackground, 0.92f), 4);
        const float m = pw * 36 / 1024, u = ph / 972;
        const auto bar = [&](float bx, float by, float bw, float bh, Color c) {
            app.canvas.fill({panel.x + bx, panel.y + by * u, bw, std::max(1.5f, bh * u)}, c, 1);
        };
        bar(m, 30, pw * 0.42f, 34, col(kMenuLabel));
        bar(m, 100, pw * 0.36f, 22, col(kMenuDim));
        bar(pw / 2, 100, pw * 0.3f, 22, col(kMenuLabel, 0.8f));
        for (int i = 0; i < 11; ++i) {
            const float ry = 200 + 50.0f * i;
            if (i == 0)
                app.canvas.fill({panel.x + m * 0.55f, panel.y + (ry - 8) * u, pw - 1.1f * m, 50 * u}, col(kMenuHighlight), 1);
            bar(m, ry + 8, pw * (0.22f + 0.04f * (i % 3)), 18, col(kMenuLabel, 0.85f));
            const float vw = pw * 0.12f;
            bar(pw - m - vw, ry + 8, vw, 18, col(kMenuLabel, 0.85f));
        }
        bar(m, 880, pw * 0.6f, 14, col(kMenuDim, 0.7f));
        app.canvas.stroke(panel, col(active ? app.c->accent : app.c->preview_label, active ? 1 : 0.45f), active ? 1.5f : 0.75f, 4);
        if (focused(kMenuPanel))
            focus_ring(panel, 4);
        // The performance overlay, when it is on: four number tiles, the frame-time strip and its cells.
        Rect card{};
        if (app.s.osd.enabled) {
            const float cw = app.s.osd.width * k, ch = osd_height_deg(app.s.osd.width) * k;
            const float ccx = x + (kRefFovX / 2 + app.s.osd.yaw) * k, ccy = y + (kRefFovY / 2 - app.s.osd.pitch) * k;
            card = {ccx - cw / 2, ccy - ch / 2, cw, ch};
            const bool held = app.dragging == kOsdPanel;
            const float r = std::max(2.0f, cw * 32 / kOsdWidth), cu = cw / kOsdWidth, cv = ch / kOsdHeight;
            app.canvas.fill({card.x, card.y + 2, card.w, card.h}, {0, 0.35f}, r);
            app.canvas.fill(card, col(kOsdCard, 0.9f), r);
            const auto block = [&](float bx, float by, float bw, float bh, Color c) {
                app.canvas.fill({card.x + bx * cu, card.y + by * cv, std::max(1.0f, bw * cu), std::max(1.0f, bh * cv)}, c, 1);
            };
            for (int i = 0; i < 4; ++i) {
                const float tx = 36 + i * 238.0f + (i ? 24 : 0);
                block(tx, 30, 90, 14, col(kMenuDim, 0.8f));
                block(tx, 58, 110.0f + 20 * (i % 2), 40, col(kMenuLabel));
                block(tx, 126, 120, 14, col(i == 3 ? kOsdYellow : kOsdGreen, 0.9f));
            }
            block(36, 166, kOsdWidth - 72, 60, col(kOsdTrack));
            block(44, 184, kOsdWidth - 88, 36, col(kOsdGreen, 0.75f));
            for (int i = 0; i < 6; ++i) {
                const float cx2 = 36 + (i % 3) * 317.0f, cy2 = 244 + (i / 3) * 76.0f;
                block(cx2, cy2, 100, 12, col(kMenuDim, 0.8f));
                block(cx2, cy2 + 30, 150, 22, i % 3 == 1 || i == 5 ? col(kOsdGreen, 0.55f) : col(kMenuLabel, 0.85f));
            }
            app.canvas.stroke(card, col(held ? app.c->accent : app.c->preview_label, held ? 1 : 0.45f), held ? 1.5f : 0.75f, r);
            if (focused(kOsdPanel))
                focus_ring(card, r);
        }
        const t::TypeStyle tiny{9.5f, 12, 400, false};
        app.canvas.text(L"Reference view  " + std::to_wstring(static_cast<int>(kRefFovX)) + L"° × " +
                            std::to_wstring(static_cast<int>(kRefFovY)) + L"°",
                        tiny, {x + 8, y + 5, w - 16, 12}, col(app.c->preview_label, 0.75f), Align::Leading, false);
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
        app.hits.push_back({kMenuPanel, frame, 0});  // empty space: the panel's centre jumps there
        app.hits.push_back({kMenuPanel, panel, 1});  // the panel: dragged from where it is held
        if (app.s.osd.enabled)
            app.hits.push_back({kOsdPanel, card, 1});  // the overlay, over the menu
    }
    float h = frameH + t::kSpace2;
    const std::wstring caption = app.s.osd.enabled ? L"Drag the menu or the overlay to place it  ·  Headsets’ fields of view vary"
                                                   : L"Drag the panel to place it  ·  Headsets’ fields of view vary";
    const float ch = app.canvas.measure_height(caption, t::kSubheadline, w);
    if (draw)
        app.canvas.text(caption, t::kSubheadline, {x, y + h, w, ch + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    h += ch + t::kSpace4;
    const t::TypeStyle dt{12, 16, 400, false};
    char size[48], metres[48], distance[16];
    std::snprintf(size, sizeof(size), "%d° × %d°", app.s.menuWidth, static_cast<int>(std::lround(menu_height_deg(app.s.menuWidth))));
    std::snprintf(metres, sizeof(metres), "%.2f m", 2 * kMenuDistance * std::tan(app.s.menuWidth * 3.14159265 / 360));
    std::snprintf(distance, sizeof(distance), "%.1f m", kMenuDistance);
    const std::pair<std::wstring, std::wstring> lines[] = {
        {L"Horizontal", degrees(app.s.menuYaw, L"right", L"left")},
        {L"Vertical", degrees(app.s.menuPitch, L"up", L"down")},
        {L"Panel size", widen(size)},
        {L"Panel width", widen(metres)},
        {L"Distance", widen(distance)},
    };
    for (const auto& [name, value] : lines) {
        if (draw) {
            app.canvas.text(name, dt, {x, y + h, w / 2, 16}, col(app.c->label_secondary));
            app.canvas.text(value, t::kValue, {x + w / 2, y + h, w / 2, 16}, col(app.c->label), Align::Trailing);
        }
        h += 16 + 4;
    }
    return h - 4;
}

// The preview column: the placement preview on the Headset menu pane, the foveation preview elsewhere.
float side_preview(float x, float y, float w, bool draw) {
    return app.pane == Pane::Menu     ? placement_preview(x, y, w, draw)
           : app.pane == Pane::Layers ? layers_preview(x, y, w, draw)
                                      : preview(x, y, w, draw);
}

// ---- Window ------------------------------------------------------------------------------------

struct Item {
    const wchar_t* label;
    const char* icon[3];
};
// The kit's sidebar glyphs (16 x 16, 1.5 stroke); circles and rects are drawn as paths.
const Item kItems[] = {
    {L"Launch checks", {"M2 4.25l1.5 1.5 2.75-3", "M2 10.75l1.5 1.5 2.75-3", "M8.75 4.5H14M8.75 11H14"}},
    {L"Rendering", {"M8 2l6 3-6 3-6-3 6-3z", "M2 8.5l6 3 6-3", "M2 11.5l6 3 6-3"}},
    {L"Resolution", {"M3.25 2.75H12.75C13.6 2.75 14.25 3.4 14.25 4.25V9.75C14.25 10.6 13.6 11.25 12.75 11.25H3.25C2.4 11.25 1.75 10.6 1.75 9.75V4.25C1.75 3.4 2.4 2.75 3.25 2.75Z", "M5.5 14h5M8 11.25V14", nullptr}},
    {L"Foveation", {"M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z", nullptr, nullptr}},
    {L"Focus overlay", {"M5 5H11V11H5Z", nullptr, nullptr}},
    {L"Foveated DLSS", {"M8 1.75L9.6 6.4 14.25 8 9.6 9.6 8 14.25 6.4 9.6 1.75 8 6.4 6.4Z", nullptr, nullptr}},
    {L"Headset menu", {"M3.25 3.25H12.75C13.6 3.25 14.25 3.9 14.25 4.75V11.25C14.25 12.1 13.6 12.75 12.75 12.75H3.25C2.4 12.75 1.75 12.1 1.75 11.25V4.75C1.75 3.9 2.4 3.25 3.25 3.25Z", "M4.5 6.5H8.5", "M4.5 9.5H11.5"}},
    {L"Hotkeys", {"M2.25 4.25H13.75V11.75H2.25Z", "M4.75 6.75H5.25M7.75 6.75H8.25M10.75 6.75H11.25", "M5.25 9.25H10.75"}},
    {L"OpenXR layers", {"M2.5 4.5H10.5", "M2.5 8H10.5", "M2.5 11.5H10.5"}},
    {L"Diagnostics", {"M1.5 8.5H4.5L6.25 4.5L9.75 11.5L11.5 8.5H14.5", nullptr, nullptr}},
    {L"Report an issue", {"M3.5 14.25V2", "M3.5 2.75H12.5L10.5 5.5L12.5 8.25H3.5", nullptr}},
    {L"About", {"M8 7.25V11", nullptr, nullptr}},
};

static_assert(std::size(kItems) == kPanes, "a sidebar item for every pane");

void sidebar_icon(int i, const Rect& r, Color c) {
    for (const char* p : kItems[i].icon)
        if (p)
            app.canvas.path(p, r, 16, c, 1.5f);
    if (i == static_cast<int>(Pane::Foveation))
        app.canvas.circle(r.x + 8, r.y + 8, 2, c, false, 1.5f);
    if (i == static_cast<int>(Pane::Overlay))  // the outer square, dashed 2 / 2 along each edge
        for (float d = 1.75f; d < 14.25f; d += 4) {
            const float e = std::min(d + 2, 14.25f);
            app.canvas.line(r.x + d, r.y + 1.75f, r.x + e, r.y + 1.75f, c, 1.5f);
            app.canvas.line(r.x + d, r.y + 14.25f, r.x + e, r.y + 14.25f, c, 1.5f);
            app.canvas.line(r.x + 1.75f, r.y + d, r.x + 1.75f, r.y + e, c, 1.5f);
            app.canvas.line(r.x + 14.25f, r.y + d, r.x + 14.25f, r.y + e, c, 1.5f);
        }
    if (i == static_cast<int>(Pane::Layers)) {  // the list's order: an up and a down arrow
        app.canvas.path("M12.5 6.5V2.5M11 4l1.5-1.5L14 4", r, 16, c, 1.5f);
        app.canvas.path("M12.5 9.5V13.5M11 12l1.5 1.5L14 12", r, 16, c, 1.5f);
    }
    if (i == static_cast<int>(Pane::About)) {
        app.canvas.circle(r.x + 8, r.y + 8, 6.25f, c, false, 1.5f);
        app.canvas.circle(r.x + 8, r.y + 5, 0.9f, c, true);
    }
}

// The open pop-up menu: under its button, as wide as the button or its longest item (check mark
// column included), right-aligned with the button when it would leave the window.
Rect popup_menu_rect() {
    const Popup& p = app.popup;
    float w = p.anchor.w;
    for (const auto& item : p.items)
        w = std::max(w, std::ceil(app.canvas.measure(item, t::kBody)) + 22 + 4 + 10 + 8);
    Rect menu{p.anchor.x, p.anchor.y + p.anchor.h + 4, w, 22.0f * p.items.size() + 10};
    if (menu.right() > app.canvas.width() - 8)
        menu.x = p.anchor.right() - w;
    if (menu.bottom() > app.canvas.height())
        menu.y = p.anchor.y - menu.h - 4;
    return menu;
}

// The description box (a native edit, kept in step with the canvas after each paint): over the field the
// Diagnostics pane drew; hidden when it is not wholly in view, or a popup is open.
void place_report_edit() {
    const Rect& r = app.reportEditRect;
    if (!app.reportEdit)
        return;
    if (!report_edit_fits() || app.popup.open) {
        if (GetFocus() == app.reportEdit)
            SetFocus(app.hwnd);
        ShowWindow(app.reportEdit, SW_HIDE);
        return;
    }
    const float k = app.canvas.dpi_scale();
    const UINT dpi = static_cast<UINT>(std::lround(96 * k));
    if (app.reportFontDpi != dpi) {
        HFONT font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(app.reportEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (app.reportFont)
            DeleteObject(app.reportFont);
        app.reportFont = font;
        app.reportFontDpi = dpi;
    }
    const int x = static_cast<int>(std::lround(r.x * k)), y = static_cast<int>(std::lround(r.y * k));
    const int w = static_cast<int>(std::lround(r.w * k)), h = static_cast<int>(std::lround(r.h * k));
    SetWindowPos(app.reportEdit, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (app.reportBrushRgb != app.c->field_bg)  // the theme changed: its colours (WM_CTLCOLOREDIT)
        InvalidateRect(app.reportEdit, nullptr, TRUE);
}

void on_key(WPARAM key);

// The description box's keys: Tab and Shift+Tab move on through the window's focus order; Escape leaves it.
LRESULT CALLBACK report_edit_proc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam) {
    static const WNDPROC base = reinterpret_cast<WNDPROC>(GetPropW(edit, L"aceqv.base"));
    if (message == WM_KEYDOWN && (wParam == VK_TAB || wParam == VK_ESCAPE)) {
        app.focus = kDiagDescription;
        SetFocus(app.hwnd);
        if (wParam == VK_TAB)
            on_key(VK_TAB);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return 0;
    }
    if (message == WM_CHAR && (wParam == L'\t' || wParam == 27))  // already handled as keys
        return 0;
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
        InvalidateRect(app.hwnd, nullptr, FALSE);  // the field's focus ring
    return CallWindowProcW(base, edit, message, wParam, lParam);
}

void create_report_edit() {
    app.reportEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                     0, 0, 0, 0, app.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!app.reportEdit)
        return;
    SendMessageW(app.reportEdit, EM_SETLIMITTEXT, 4000, 0);  // the link has room for about that much
    SetPropW(app.reportEdit, L"aceqv.base", reinterpret_cast<HANDLE>(GetWindowLongPtrW(app.reportEdit, GWLP_WNDPROC)));
    SetWindowLongPtrW(app.reportEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(report_edit_proc));
}

// The action bar's badge for the launch checks' first problem (and how many more); it opens the pane.
void checks_badge(float y, int tone, const Check& first, float maxWidth) {
    size_t more = 0;
    for (const Check& c : app.checks)
        more += c.tone == CheckTone::Bad || c.tone == CheckTone::Warn ? 1 : 0;
    const std::wstring text = first.title + (more > 1 ? L"  ·  " + std::to_wstring(more - 1) + L" more" : L"");
    const float w = badge(t::kSpace5, y, tone, text, maxWidth);
    app.hits.push_back({kChecksBadge, {t::kSpace5, y, w, 20}});
}

void paint() {
    if (!app.canvas.begin())
        return;
    app.hits.clear();
    app.reportEditRect = {};
    Canvas& cv = app.canvas;
    const float W = cv.width(), Hh = cv.height();
    const float actionH = 10 + t::kControlHeightLarge + 10;
    const float bodyH = Hh - actionH;
    cv.fill({0, 0, W, Hh}, col(app.c->window_bg));

    // Sidebar.
    cv.fill({0, 0, t::kSidebarWidth, bodyH}, col(app.c->sidebar_bg));
    cv.line(t::kSidebarWidth - 0.25f, 0, t::kSidebarWidth - 0.25f, bodyH, col(app.c->separator), 0.5f);
    float sy = t::kSpace2;
    for (int i = 0; i < kPanes; ++i) {
        const Rect row{10, sy, t::kSidebarWidth - 20, 28};
        const bool selected = static_cast<int>(app.pane) == i;
        if (selected)
            cv.fill(row, col(app.c->sidebar_selection), t::kRadiusControl);
        const Color fg = col(selected ? app.c->on_accent : app.c->label);
        sidebar_icon(i, {row.x + t::kSpace2, row.y + 6, 16, 16}, col(selected ? app.c->on_accent : app.c->accent));
        cv.text(kItems[i].label, t::kBody, {row.x + t::kSpace2 + 16 + t::kSpace2, row.y, row.w - 40, 28}, fg);
        // A mark on Foveation without an eye tracker, and on Launch checks with a problem (red) or warning.
        const CheckTone worst = worst_tone(app.checks);
        const bool checksMark = i == static_cast<int>(Pane::Checks) && worst != CheckTone::Ok;
        if ((i == static_cast<int>(Pane::Foveation) && eye_warning()) || checksMark) {
            const bool danger = checksMark && worst == CheckTone::Bad;
            const float bx = row.right() - t::kSpace2 - 16, by = row.y + 6;
            cv.circle(bx + 8, by + 8, 8, col(selected ? app.c->on_accent : danger ? app.c->danger_bg : app.c->warning_bg), true);
            const Color mark = col(selected ? app.c->accent_fill : danger ? app.c->danger : app.c->warning);
            cv.line(bx + 8, by + 5, bx + 8, by + 8.5f, mark, 1.6f);
            cv.circle(bx + 8, by + 10.75f, 0.9f, mark, true);
        }
        if (selected && focused(kSidebar))
            focus_ring(row, t::kRadiusControl);
        app.hits.push_back({kSidebar, row, i});
        sy += 28 + 2;
    }

    // Main and preview columns; below 900 wide the preview drops under the form.
    const bool narrow = W < 900;
    const float mainX = t::kSidebarWidth, mainW = narrow ? W - mainX : W - mainX - t::kPreviewWidth;
    const float contentX = mainX + t::kSpace5, contentW = mainW - 2 * t::kSpace5;
    app.scrollArea = {mainX, 0, W - mainX, bodyH};
    cv.push_clip(app.scrollArea);
    const float y0 = t::kSpace5 - app.scroll;
    cv.text(kItems[static_cast<int>(app.pane)].label, t::kTitle2, {contentX, y0, contentW, 22}, col(app.c->label));
    float contentH = 22 + t::kSpace6;
    contentH += pane_content(contentX, y0 + contentH, contentW, true);
    float total = contentH + t::kSpace5;
    if (narrow) {
        const float py = y0 + total + t::kSpace2;
        cv.text(L"Preview", t::kHeadline, {contentX + 2, py, contentW, 16}, col(app.c->label));
        const float pw = std::min(contentW, t::kPreviewWidth - t::kSpace5);
        total += t::kSpace2 + 16 + t::kSpace4 + side_preview(contentX, py + 16 + t::kSpace4, pw, true) + t::kSpace5;
    }
    cv.pop_clip();
    app.scrollMax = std::max(0.0f, total + t::kSpace5 - bodyH);
    if (!narrow) {
        const float ax = W - t::kPreviewWidth, aw = t::kPreviewWidth - t::kSpace5;
        cv.text(L"Preview", t::kHeadline, {ax + 2, t::kSpace5, aw, 16}, col(app.c->label));
        side_preview(ax, t::kSpace5 + 16 + t::kSpace4, aw, true);
    }

    // Action bar: status badge, Restore Defaults, Launch.
    const Rect bar{0, bodyH, W, actionH};
    cv.fill(bar, col(app.c->titlebar_bg));
    cv.line(0, bodyH + 0.25f, W, bodyH + 0.25f, col(app.c->separator), 0.5f);
    const float launchW = std::ceil(cv.measure(L"Launch Assetto Corsa EVO", {15, 20, 500, false})) + 36;
    const Rect launchR{W - t::kSpace5 - launchW, bodyH + 10, launchW, t::kControlHeightLarge};
    const float restoreW = std::ceil(cv.measure(L"Restore Defaults", t::kBody)) + 2 * t::kSpace3;
    const Rect restoreR{launchR.x - t::kSpace3 - restoreW, bodyH + (actionH - t::kControlHeight) / 2, restoreW, t::kControlHeight};
    const float badgeMax = restoreR.x - t::kSpace3 - t::kSpace5;
    const float by = bodyH + (actionH - 20) / 2;
    const Readouts ro = readouts(app.s, app.runtime);
    if (app.status == Status::Starting)
        badge(t::kSpace5, by, 3, L"Starting Assetto Corsa EVO…", badgeMax);
    else if (app.status == Status::Error)
        badge(t::kSpace5, by, 2, app.error, badgeMax);
    else if (!app.iniFound)
        badge(t::kSpace5, by, 2, L"ace-quadview.ini not found next to the launcher.", badgeMax);
    else if (const Check* bad = first_with(app.checks, CheckTone::Bad))
        checks_badge(by, 2, *bad, badgeMax);
    else if (eye_warning())
        badge(t::kSpace5, by, 1, L"Eye tracker not detected", badgeMax);
    else if (app.runtime.known() && ro.overRuntimeLimit)
        badge(t::kSpace5, by, 1, L"Larger than the runtime’s maximum image size", badgeMax);
    else if (const Check* warn = first_with(app.checks, CheckTone::Warn))
        checks_badge(by, 1, *warn, badgeMax);
    else if (app.live == Live::NextLaunch)
        badge(t::kSpace5, by, 1, app.s.mode == RenderMode::Qvfr
                                     ? L"Applied live up to the launch resolution · the rest at the next launch"
                                     : L"Foveation applied live · resolution and method at the next launch",
              badgeMax);
    else if (app.live == Live::Applied)
        badge(t::kSpace5, by, 0, L"Applied to the running game", badgeMax);
    else
        badge(t::kSpace5, by, 0, L"Ready", badgeMax);
    button(kRestore, restoreR, L"Restore Defaults", false);
    button(kLaunch, launchR, L"Launch Assetto Corsa EVO", true);

    // An open pop-up menu, over everything.
    if (app.popup.open) {
        const Popup& p = app.popup;
        const float itemH = 22;
        const Rect menu = popup_menu_rect();
        cv.fill({menu.x, menu.y + 2, menu.w, menu.h}, {0, app.dark ? 0.5f : 0.18f}, t::kRadiusControl + 2);
        cv.fill(menu, col(app.c->control_bg), t::kRadiusControl + 2);
        cv.stroke(menu, col(app.c->separator), 1, t::kRadiusControl + 2);
        for (size_t i = 0; i < p.items.size(); ++i) {
            const Rect item{menu.x + 5, menu.y + 5 + itemH * i, menu.w - 10, itemH};
            const bool hot = static_cast<int>(i) == p.hover;
            if (hot)
                cv.fill(item, col(app.c->accent_fill), 4);
            if (static_cast<int>(i) == p.selected)
                cv.path("M3.5 8.2l2.6 2.6L12.5 4.5", {item.x + 4, item.y + 4, 14, 14}, 16, col(hot ? app.c->on_accent : app.c->label), 1.6f);
            cv.text(p.items[i], t::kBody, {item.x + 22, item.y, item.w - 26, itemH}, col(hot ? app.c->on_accent : app.c->label));
        }
    }
    cv.end();
    place_report_edit();
}

// ---- Input -------------------------------------------------------------------------------------

const Hit* hit_at(float x, float y) {
    for (auto it = app.hits.rbegin(); it != app.hits.rend(); ++it)
        if (it->r.contains(x, y))
            return &*it;
    return nullptr;
}

void set_slider_from_x(float x) {
    int* v = slider_value(app.dragging);
    if (!v)
        return;
    const float travel = app.dragTrack.w - 18;
    const float f = std::clamp((x - app.dragTrack.x - 9) / travel, 0.0f, 1.0f);
    const int before = *v;
    *v = app.dragRange.min + static_cast<int>(std::lround(f * (app.dragRange.max - app.dragRange.min)));
    changed(false);
    if (*v != before && app.iniFound)
        push_live();  // the running game follows the knob; the settings save once it is let go
}

// The place a preview drag or the arrows change: the settings menu's or the performance overlay's.
std::pair<int*, int*> placement_of(Ctl id) {
    return id == kOsdPanel ? std::pair{&app.s.osd.yaw, &app.s.osd.pitch} : std::pair{&app.s.menuYaw, &app.s.menuPitch};
}

// Dragging a panel in the placement preview: its centre follows the pointer (less where it was held).
void set_panel_from(float x, float y) {
    const float k = app.placeFrame.w / kRefFovX;
    if (k <= 0)
        return;
    const auto [yawOf, pitchOf] = placement_of(app.dragging);
    const int yaw = std::clamp(static_cast<int>(std::lround((x - app.placeFrame.x) / k - kRefFovX / 2 + app.grabYaw)),
                               kMenuYawRange.min, kMenuYawRange.max);
    const int pitch = std::clamp(static_cast<int>(std::lround(kRefFovY / 2 - (y - app.placeFrame.y) / k + app.grabPitch)),
                                 kMenuPitchRange.min, kMenuPitchRange.max);
    const bool moved = yaw != *yawOf || pitch != *pitchOf;
    *yawOf = yaw;
    *pitchOf = pitch;
    changed(false);
    if (moved && app.iniFound)
        push_live();  // the running game moves it as it is dragged; the settings save once it is let go
}

void open_popup(Ctl owner) {
    for (const Hit& h : app.hits)
        if (h.id == owner) {
            app.popup.anchor = h.r;
            break;
        }
    app.popup.owner = owner;
    if (owner == kCheekyOuterPreset || owner == kCheekyFocusPreset) {  // Cheeky's presets (the outer views have no game default)
        const bool focus = owner == kCheekyFocusPreset;
        const int value = focus ? app.s.cheeky.focusPreset : app.s.cheeky.outerPreset;
        app.popup.items.clear();
        app.popup.selected = 0;
        for (const CheekyPreset& p : kCheekyPresets)
            if (focus || p.value != 0) {
                if (p.value == value)
                    app.popup.selected = static_cast<int>(app.popup.items.size());
                app.popup.items.push_back(p.name);
            }
    } else if (owner == kCheekyNrPreset) {
        app.popup.items.assign(std::begin(kCheekyNrPresets), std::end(kCheekyNrPresets));
        app.popup.selected = app.s.cheeky.nrPreset;
    } else if (owner == kFgMultiplier) {
        app.popup.items.assign(std::begin(kFgMultipliers), std::end(kFgMultipliers));
        app.popup.selected = std::clamp(app.s.cheeky.fgMultiplier - 2, 0, 2);
    } else if (owner == kFgRelease) {
        app.popup.items.assign(std::begin(kCheekyFgReleaseNames), std::end(kCheekyFgReleaseNames));
        app.popup.selected = cheeky_fg_release_index(app.s.cheeky.fgRelease);
    } else if (owner == kFgScale || owner == kFgPeriphery) {
        app.popup.items.assign(std::begin(kFgQuarters), std::end(kFgQuarters));
        app.popup.selected = std::clamp((owner == kFgScale ? app.s.cheeky.fgScale : app.s.cheeky.fgPeriphery) / 25 - 1, 0, 3);
    } else if (owner == kVisor) {
        app.popup.items.assign(std::begin(kVisorPositionNames), std::end(kVisorPositionNames));
        app.popup.selected = static_cast<int>(app.s.helmet.visor);
    } else if (owner == kShape) {
        app.popup.items.assign(std::begin(kShapeNames), std::end(kShapeNames));
        app.popup.selected = app.s.shape.corners ? 1 : 0;
    } else if (owner == kDiagKind) {
        app.popup.items.assign(std::begin(kIssueKinds), std::end(kIssueKinds));
        app.popup.selected = app.reportKind;
    } else if (owner == kHelmetDetail) {
        app.popup.items = {L"Performance (2048)", L"Detail (4096)"};
        app.popup.selected = app.s.helmet.detail ? 1 : 0;
    } else if (owner == kLayerProfile) {
        app.popup.items = {kLayerProfileNames[0], kLayerProfileNames[1]};
        app.popup.selected = app.layerProfile;
    } else if (owner == kCheekyNrPasses) {
        app.popup.items = {L"1 pass", L"2 passes", L"3 passes"};
        app.popup.selected = app.s.cheeky.nrPasses - 1;
    } else {
        app.popup.items = owner == kMethod ? std::vector<std::wstring>{kMethodNames[0], kMethodNames[1]}
                                           : std::vector<std::wstring>{kModeNames[0], kModeNames[1]};
        app.popup.selected = owner == kMethod ? (app.s.mode == RenderMode::Qvfr ? 0 : 1) : (app.s.eyeTracking ? 0 : 1);
    }
    app.popup.hover = app.popup.selected;
    app.popup.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void choose(Ctl owner, int option) {
    if (owner == kDiagKind) {  // not a setting
        app.reportKind = option;
        return;
    }
    if (owner == kMethod)
        app.s.mode = option == 0 ? RenderMode::Qvfr : RenderMode::TwoLoop;
    else if (owner == kMode)
        app.s.eyeTracking = option == 0;
    else if (owner == kCheekyFocusPreset)
        app.s.cheeky.focusPreset = kCheekyPresets[option].value;
    else if (owner == kCheekyOuterPreset)
        app.s.cheeky.outerPreset = kCheekyPresets[option + 1].value;  // the list without the game default
    else if (owner == kCheekyNrPreset)
        app.s.cheeky.nrPreset = option;
    else if (owner == kCheekyNrPasses)
        app.s.cheeky.nrPasses = option + 1;
    else if (owner == kFgMultiplier)
        app.s.cheeky.fgMultiplier = option + 2;
    else if (owner == kFgScale)
        app.s.cheeky.fgScale = (option + 1) * 25;
    else if (owner == kFgPeriphery)
        app.s.cheeky.fgPeriphery = (option + 1) * 25;
    else if (owner == kFgRelease)
        app.s.cheeky.fgRelease = kCheekyFgReleasePoints[std::clamp(option, 0, 2)];
    else if (owner == kVisor)
        app.s.helmet.visor = static_cast<VisorPosition>(std::clamp(option, 0, 2));
    else if (owner == kHelmetDetail)
        app.s.helmet.detail = option == 1;
    else if (owner == kShape)
        app.s.shape.corners = option == 1;
    else if (owner == kLayerProfile) {
        app.layerProfile = std::clamp(option, 0, 1);
        app.layerRow = 0;
        app.layerMessage.clear();
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    changed();
}

void close_popup(bool accept) {
    if (accept && app.popup.hover >= 0)
        choose(app.popup.owner, app.popup.hover);
    app.popup.open = false;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int popup_item_at(float x, float y) {
    const Popup& p = app.popup;
    const float itemH = 22;
    const Rect menu = popup_menu_rect();
    for (size_t i = 0; i < p.items.size(); ++i)
        if (Rect{menu.x + 5, menu.y + 5 + itemH * i, menu.w - 10, itemH}.contains(x, y))
            return static_cast<int>(i);
    return -1;
}

// Picks Cheeky's runtime DLL (from Cheeky's Standalone ZIP) and saves its path to [cheeky] runtime.
void choose_cheeky_runtime() {
    wchar_t path[MAX_PATH] = {};
    const std::wstring current = cheeky_runtime_path(app.ini, layer_folder());
    wcsncpy_s(path, current.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFilter = L"Cheeky runtime (CheekyFoveatedDLSSRuntime.dll)\0CheekyFoveatedDLSSRuntime.dll\0DLL files\0*.dll\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose Cheeky Foveated DLSS's runtime";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) && app.iniFound) {
        save_cheeky_runtime(app.ini, path);
        app.iniText = ini_text();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void activate(Ctl id, int option);

void end_capture() {
    if (app.captureAction < 0)
        return;
    app.captureAction = -1;
    KillTimer(app.hwnd, kCaptureTimer);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The Hotkeys pane: `action`'s key or (pad) wheel button takes the next press.
void start_capture(int action, bool pad) {
    if (pad) {
        open_pads();
        if (!app.padsOpen) {
            MessageBoxW(app.hwnd, L"DirectInput isn’t available, so wheel and controller buttons can’t be read.", L"QVFR Launcher",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        app.captureHeld = app.pads.poll();
        SetTimer(app.hwnd, kCaptureTimer, kCaptureMs, nullptr);
    }
    app.captureAction = action;
    app.capturePad = pad;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void set_hotkey(const KeyCombo* key, const PadControl* pad) {
    Hotkey& h = app.s.hotkeys[app.captureAction];
    if (key)
        h.key = *key;
    if (pad)
        h.pad = *pad;
    end_capture();
    changed();
}

// A key while catching one: Esc cancels, Delete or Backspace alone clears, modifiers wait for their key.
bool capture_key(WPARAM vk) {
    if (app.captureAction < 0)
        return false;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (vk == VK_ESCAPE) {
        end_capture();
    } else if ((vk == VK_DELETE || vk == VK_BACK) && !ctrl && !alt && !shift) {
        const KeyCombo none;
        const PadControl noPad;
        set_hotkey(app.capturePad ? nullptr : &none, app.capturePad ? &noPad : nullptr);
    } else if (!app.capturePad && vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LWIN && vk != VK_RWIN) {
        const KeyCombo key{static_cast<int>(vk), ctrl, alt, shift};
        set_hotkey(&key, nullptr);
    }
    return true;
}

// The capture timer: the first control pressed that wasn't held when the capture started.
void capture_poll() {
    if (app.captureAction < 0 || !app.capturePad)
        return;
    const std::vector<PadHeld> held = app.pads.poll();
    const auto in = [](const std::vector<PadHeld>& list, const PadHeld& p) {
        for (const PadHeld& q : list)
            if (q.control == p.control && q.device == p.device)
                return true;
        return false;
    };
    for (const PadHeld& p : held)
        if (!in(app.captureHeld, p)) {
            const PadControl pad{p.device, p.control, p.name};
            set_hotkey(nullptr, &pad);
            return;
        }
    // A control let go since counts when pressed again.
    std::vector<PadHeld> still;
    for (const PadHeld& p : app.captureHeld)
        if (in(held, p))
            still.push_back(p);
    app.captureHeld = std::move(still);
}

// A launch check's button (preflight.h's CheckFix).
void fix_check(const Check& c) {
    switch (c.fix) {
    case CheckFix::OpenReleases: ShellExecuteW(app.hwnd, L"open", kReleasesUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    case CheckFix::StartSteam: ShellExecuteW(app.hwnd, L"open", L"steam://open/main", nullptr, nullptr, SW_SHOWNORMAL); break;
    case CheckFix::ShowFile: {
        const std::wstring select = L"/select,\"" + c.fixTarget + L"\"";
        ShellExecuteW(app.hwnd, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    case CheckFix::OpenLayers: activate(kSidebar, static_cast<int>(Pane::Layers)); return;
    case CheckFix::OpenDlss: activate(kSidebar, static_cast<int>(Pane::Dlss)); return;
    case CheckFix::OpenRendering: activate(kSidebar, static_cast<int>(Pane::Rendering)); return;
    case CheckFix::TurboOff:
        if (!toolkit_turbo_off(launcher_game_exe()))
            MessageBoxW(app.hwnd, L"OpenXR Toolkit’s settings for ACE could not be changed. Turn Turbo mode off in Toolkit’s menu in the game.",
                        L"QVFR Launcher", MB_OK | MB_ICONWARNING);
        break;
    case CheckFix::None: return;
    }
    refresh_checks();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void activate(Ctl id, int option) {
    end_capture();  // any click ends catching a hotkey (one on a hotkey's button starts again)
    switch (id) {
    case kSidebar:
        app.pane = static_cast<Pane>(option);
        app.scroll = 0;
        if (app.pane == Pane::Layers)
            refresh_layers();
        if (app.pane == Pane::Hotkeys)
            open_pads();
        if (app.pane == Pane::Diagnostics || app.pane == Pane::Report)
            app.runtime = load_runtime_info(runtime_info_path());
        break;
    case kChecksBadge: activate(kSidebar, static_cast<int>(Pane::Checks)); return;
    case kHotkeyKey:
    case kHotkeyPad:
        if (option >= 0 && option < kHotkeyActions)
            start_capture(option, id == kHotkeyPad);
        return;
    case kHotkeysReset:
        app.s.hotkeys = default_hotkeys();
        changed();
        return;
    case kCheckFix:
        if (option >= 0 && option < static_cast<int>(app.checks.size()))
            fix_check(app.checks[option]);
        return;
    case kDiagCapture: app.s.cpuProfile = !app.s.cpuProfile; changed(); return;
    case kDiagBaseline: app.s.baselineNext = !app.s.baselineNext; changed(); return;
    case kDiagKind: open_popup(id); return;
    case kDiagDescription:
        if (!report_edit_fits()) {  // scroll the field into view first: the edit shows only then
            const Rect& r = app.reportEditRect;
            const float by = r.bottom() > app.scrollArea.bottom() ? r.bottom() + 5 - app.scrollArea.bottom() : r.y - 5 - app.scrollArea.y;
            app.scroll = std::clamp(app.scroll + by + (by > 0 ? t::kSpace4 : -t::kSpace4), 0.0f, app.scrollMax);
            RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
        SetFocus(app.reportEdit);
        return;
    case kDiagReport: report_issue(); return;
    case kDiagCopy: copy_summary(); return;
    case kDiagLogs: ShellExecuteW(app.hwnd, L"explore", logs_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return;
    case kLayerProfile: open_popup(id); return;
    case kLayerOn: app.layerRow = option; toggle_layer(option); break;
    case kLayerUp: move_layer(option, -1); break;
    case kLayerDown: move_layer(option, 1); break;
    case kLayerApply: apply_layers(); break;
    case kLayerRevert:
        app.layersDraft = app.layersWindows;
        app.layersLockMoved = false;
        app.layerMessage.clear();
        refresh_layers(false);
        break;
    case kLayerReset:
        edit_launcher_layers([](LauncherLayers& p) { p = {}; });
        app.layerMessage.clear();
        break;
    case kMethod:
    case kMode:
    case kCheekyOuterPreset:
    case kCheekyFocusPreset:
    case kCheekyNrPreset:
    case kCheekyNrPasses:
    case kFgMultiplier:
    case kFgScale:
    case kFgPeriphery:
    case kFgRelease:
    case kVisor:
    case kShape:
    case kHelmetDetail: open_popup(id); return;
    case kFgOn: app.s.cheeky.fg = !app.s.cheeky.fg; changed(); return;
    case kOsdOn: app.s.osd.enabled = !app.s.osd.enabled; changed(); return;
    case kCropOn:
        if (app.s.mode == RenderMode::Qvfr) {
            app.s.crop = !app.s.crop;
            changed();
        }
        return;
    case kHelmetOn:
        if (app.s.mode == RenderMode::Qvfr) {
            app.s.helmet.on = !app.s.helmet.on;
            changed();
        }
        return;
    case kFgBorder: app.s.cheeky.fgBorder = !app.s.cheeky.fgBorder; changed(); return;
    case kCheekyMotionFix: app.s.cheeky.motionFix = !app.s.cheeky.motionFix; changed(); return;
    case kCheekyNr: app.s.cheeky.nr = !app.s.cheeky.nr; changed(); return;
    case kOutline: app.s.outline = !app.s.outline; changed(); return;
    case kCheekyUse: app.s.cheeky.load = !app.s.cheeky.load; changed(); return;
    case kCheekyOn: app.s.cheeky.processing = !app.s.cheeky.processing; changed(); return;
    case kCheekyOuter: app.s.cheeky.outerDlaa = !app.s.cheeky.outerDlaa; changed(); return;
    case kCheekyRuntime: choose_cheeky_runtime(); return;
    case kColor: app.s.color = static_cast<OutlineColor>(option); changed(); return;
    case kRestore: {
        const Hotkeys hotkeys = app.s.hotkeys;  // the Hotkeys pane resets those
        app.s = Settings{};
        app.s.hotkeys = hotkeys;
        changed();
        return;
    }
    case kLaunch: launch(); return;
    case kSupport: ShellExecuteW(app.hwnd, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    default: break;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

float dip(LPARAM lp, bool y) {
    return (y ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp)) / app.canvas.dpi_scale();
}

void on_key(WPARAM key) {
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (app.popup.open) {
        const int n = static_cast<int>(app.popup.items.size());
        if (key == VK_UP || key == VK_DOWN)
            app.popup.hover = (app.popup.hover + (key == VK_DOWN ? 1 : n - 1)) % n;
        else if (key == VK_RETURN || key == VK_SPACE)
            close_popup(true);
        else if (key == VK_ESCAPE)
            close_popup(false);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_TAB) {
        const auto order = focus_order();
        auto it = std::find(order.begin(), order.end(), app.focus);
        int i = it == order.end() ? (shift ? 0 : -1) : static_cast<int>(it - order.begin());
        i = (i + (shift ? static_cast<int>(order.size()) - 1 : 1)) % static_cast<int>(order.size());
        app.focus = order[i];
        app.keyboard = true;
        if (app.focus == kDiagDescription)  // typing goes to the description box
            activate(kDiagDescription, -1);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_RETURN) {
        launch();
        return;
    }
    app.keyboard = true;
    if (int* v = slider_value(app.focus)) {
        const Range r = slider_range(app.focus);
        int nv = *v;
        if (key == VK_LEFT || key == VK_DOWN)
            nv -= 1;
        else if (key == VK_RIGHT || key == VK_UP)
            nv += 1;
        else if (key == VK_PRIOR)
            nv += 10;
        else if (key == VK_NEXT)
            nv -= 10;
        else if (key == VK_HOME)
            nv = r.min;
        else if (key == VK_END)
            nv = r.max;
        else
            return;
        *v = std::clamp(nv, r.min, r.max);
        changed();
        return;
    }
    switch (app.focus) {
    case kLayerList: {  // Up/Down pick a row, Space switches it, Ctrl+Up/Down moves it
        const int n = layer_rows();
        if (n == 0)
            break;
        app.layerRow = std::clamp(app.layerRow, 0, n - 1);
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if ((key == VK_UP || key == VK_DOWN) && ctrl)
            move_layer(app.layerRow, key == VK_UP ? -1 : 1);
        else if (key == VK_UP || key == VK_DOWN)
            app.layerRow = std::clamp(app.layerRow + (key == VK_UP ? -1 : 1), 0, n - 1);
        else if (key == VK_SPACE)
            toggle_layer(app.layerRow);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        break;
    }
    case kSidebar:
        if (key == VK_UP || key == VK_DOWN) {
            const int p = std::clamp(static_cast<int>(app.pane) + (key == VK_DOWN ? 1 : -1), 0, kPanes - 1);
            activate(kSidebar, p);
        }
        break;
    case kMenuPanel:
    case kOsdPanel: {  // arrows move the panel a degree, Page Up / Page Down ten
        const int step = key == VK_PRIOR || key == VK_NEXT ? 10 : 1;
        const int dx = key == VK_RIGHT ? step : key == VK_LEFT ? -step : 0;
        const int dy = key == VK_UP || key == VK_PRIOR ? step : key == VK_DOWN || key == VK_NEXT ? -step : 0;
        if (dx || dy) {
            const auto [yawOf, pitchOf] = placement_of(app.focus);
            *yawOf = std::clamp(*yawOf + dx, kMenuYawRange.min, kMenuYawRange.max);
            *pitchOf = std::clamp(*pitchOf + dy, kMenuPitchRange.min, kMenuPitchRange.max);
            changed();
        }
        break;
    }
    case kColor:
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN) {
            const int d = key == VK_RIGHT || key == VK_DOWN ? 1 : 2;
            activate(kColor, (static_cast<int>(app.s.color) + d) % 3);
        }
        break;
    case kMethod:
    case kMode:
    case kOutline:
    case kCheekyUse:
    case kCheekyRuntime:
    case kCheekyOn:
    case kCheekyOuter:
    case kCheekyOuterPreset:
    case kCheekyFocusPreset:
    case kCheekyMotionFix:
    case kCheekyNr:
    case kCheekyNrPreset:
    case kCheekyNrPasses:
    case kFgOn:
    case kFgBorder:
    case kFgMultiplier:
    case kFgScale:
    case kFgPeriphery:
    case kFgRelease:
    case kOsdOn:
    case kCropOn:
    case kHelmetOn:
    case kVisor:
    case kShape:
    case kHelmetDetail:
    case kLayerProfile:
    case kLayerApply:
    case kLayerRevert:
    case kLayerReset:
    case kSupport:
    case kRestore:
    case kLaunch:
        if (key == VK_SPACE)
            activate(app.focus, -1);
        break;
    default: break;
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        app.hwnd = hwnd;
        apply_theme();
        if (!app.canvas.initialize(hwnd))
            return -1;
        app.iniText = ini_text();
        SetTimer(hwnd, kFollowTimer, 500, nullptr);
        create_report_edit();
        refresh_checks();
        return 0;
    case kHashedMessage: {
        const std::unique_ptr<std::string> sha(reinterpret_cast<std::string*>(lParam));
        app.exeSha = *sha;
        app.hashing = false;
        refresh_checks();  // also hashes again if the exe changed meanwhile
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CTLCOLOREDIT:  // the description box in the theme's field colours
        if (reinterpret_cast<HWND>(lParam) == app.reportEdit) {
            if (!app.reportBrush || app.reportBrushRgb != app.c->field_bg) {
                if (app.reportBrush)
                    DeleteObject(app.reportBrush);
                app.reportBrush = CreateSolidBrush(colorref(app.c->field_bg));
                app.reportBrushRgb = app.c->field_bg;
            }
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, colorref(app.c->label));
            SetBkColor(dc, colorref(app.c->field_bg));
            return reinterpret_cast<LRESULT>(app.reportBrush);
        }
        break;
    case WM_TIMER:
        if (wParam == kCaptureTimer)
            capture_poll();
        if (wParam == kFollowTimer) {
            follow_ini();
            // Launch checks: Steam, ACE, the layers and the game folder change outside the launcher.
            static int ticks = 0;
            if (++ticks % 4 == 0) {
                refresh_checks();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // Diagnostics and Report an issue follow a running session: the layer rewrites runtime.ini every ~10 s.
            static FILETIME seen{};
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if ((app.pane == Pane::Diagnostics || app.pane == Pane::Report) &&
                GetFileAttributesExW(runtime_info_path().c_str(), GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &seen) != 0) {
                seen = a.ftLastWriteTime;
                app.runtime = load_runtime_info(runtime_info_path());
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        app.canvas.resize();
        app.scroll = std::min(app.scroll, app.scrollMax);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        app.canvas.set_dpi(HIWORD(wParam));
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(hwnd);
        RECT r{0, 0, MulDiv(static_cast<int>(kMinWidth), dpi, 96), MulDiv(static_cast<int>(kMinHeight - kTitlebar), dpi, 96)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            apply_theme();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {  // a session may have reported the headset meanwhile
            app.runtime = load_runtime_info(runtime_info_path());
            if (app.pane == Pane::Layers)  // a layer may have been installed or switched meanwhile
                refresh_layers();
            refresh_checks();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN: {
        const float x = dip(lParam, false), y = dip(lParam, true);
        app.keyboard = false;
        if (GetFocus() != hwnd)  // a click outside the description box leaves it
            SetFocus(hwnd);
        if (app.popup.open) {
            app.popup.hover = popup_item_at(x, y);
            close_popup(app.popup.hover >= 0);
            return 0;
        }
        const Hit* h = hit_at(x, y);
        if (!h)
            return 0;
        app.focus = h->id;
        if (slider_value(h->id)) {
            app.dragging = h->id;
            app.dragTrack = h->r;
            app.dragRange = slider_range(h->id);
            SetCapture(hwnd);
            set_slider_from_x(x);
        } else if (h->id == kMenuPanel || h->id == kOsdPanel) {
            app.dragging = h->id;
            const float k = app.placeFrame.w / kRefFovX;
            const auto [yawOf, pitchOf] = placement_of(h->id);
            // Held on the panel: that point stays under the pointer; on empty space, the centre jumps there.
            app.grabYaw = h->option == 1 ? *yawOf - ((x - app.placeFrame.x) / k - kRefFovX / 2) : 0;
            app.grabPitch = h->option == 1 ? *pitchOf - (kRefFovY / 2 - (y - app.placeFrame.y) / k) : 0;
            SetCapture(hwnd);
            set_panel_from(x, y);
        } else if (h->id == kRestore || h->id == kLaunch || h->id == kSupport || h->id == kLayerApply ||
                   h->id == kLayerRevert || h->id == kLayerReset) {
            app.pressed = h->id;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            activate(h->id, h->option);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = dip(lParam, false), y = dip(lParam, true);
        if (app.dragging == kMenuPanel || app.dragging == kOsdPanel)
            set_panel_from(x, y);
        else if (app.dragging != kNone)
            set_slider_from_x(x);
        else if (app.popup.open) {
            const int hover = popup_item_at(x, y);
            if (hover != app.popup.hover) {
                app.popup.hover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:  // a drag that lost the mouse (no button-up): end it, or follow_ini stays off
        if (app.dragging != kNone) {
            app.dragging = kNone;
            changed();
        }
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (app.dragging != kNone) {
            app.dragging = kNone;
            changed();  // save once the knob is let go
        }
        if (app.pressed != kNone) {
            const Ctl pressed = app.pressed;
            app.pressed = kNone;
            const Hit* h = hit_at(dip(lParam, false), dip(lParam, true));
            if (h && h->id == pressed)
                activate(pressed, -1);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (app.scrollMax > 0) {
            app.scroll = std::clamp(app.scroll - GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 40, 0.0f, app.scrollMax);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:  // Alt held: the Hotkeys pane catches Alt combinations
        if (capture_key(wParam))
            return 0;
        if (message == WM_SYSKEYDOWN)
            break;
        on_key(wParam);
        return 0;
    case WM_SYSCHAR:
    case WM_SYSKEYUP:
        if (app.captureAction >= 0)
            return 0;  // no beep, and no system menu when Alt is let go, while catching a key
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace
}  // namespace ui

int run_window(HINSTANCE instance, int show) {
    using namespace ui;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    app.ini = settings_path();
    app.iniFound = GetFileAttributesW(app.ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (app.iniFound)
        app.s = load_settings(app.ini);
    app.runtime = load_runtime_info(runtime_info_path());
    app.focus = kLaunch;  // Return launches

    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"QvfrLauncher";
    // The app icon (launcher.rc): the large one for Alt+Tab and the taskbar, the small one for the title bar.
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&cls);
    // The kit's 960 x 600 window includes its 38px title bar; the native caption replaces it.
    const UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(static_cast<int>(kDefaultWidth), dpi, 96), MulDiv(static_cast<int>(kDefaultHeight - kTitlebar), dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"QVFR Launcher", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace aceqv::launcher

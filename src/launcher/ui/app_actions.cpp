// What the window does with the settings: the theme and the game's accent, saving a change (and pushing it to
// a running game), the profiles, following the ini, the launch checks, the sliders' values and ranges, the
// focus order, and Launch (launch_game, main.cpp).
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <dwmapi.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "../preflight.h"
#include "../report.h"
#include "../game_tabs.h"
#include "../settings.h"
#include "../session_history.h"
#include "../../games/game.h"
#include "../../sha256_file.h"
#include "window_state.h"

#pragma comment(lib, "dwmapi.lib")

namespace simqvfr::launcher::ui::win {

namespace {

bool system_dark() {
    // SIM_QVFR_LAUNCHER_THEME=light|dark overrides the Windows app theme (for checking both).
    wchar_t forced[16] = {};
    if (GetEnvironmentVariableW(L"SIM_QVFR_LAUNCHER_THEME", forced, 16))
        return _wcsicmp(forced, L"dark") == 0;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

}  // namespace

COLORREF colorref(uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

// A game tab's palette (tokens.json "games"), for the theme in use; the kit's default accent for a game without one.
t::Accent builtin_accent(const games::GameInfo& game) {
    for (const t::GamePalette& p : t::kGamePalettes)
        if (_stricmp(p.id, game.id) == 0)
            return app.dark ? p.dark : p.light;
    const t::Colors& base = app.dark ? t::kDark : t::kLight;
    return {base.accent, base.accent_fill, base.on_accent, base.focus_ring};
}

std::wstring game_id(const games::GameInfo& game) { return widen(std::string(game.id)); }

// A game's accent: the colour picked for it in the Games pane (game_tabs.h accent_from), else its palette.
t::Accent game_accent(const games::GameInfo& game) {
    const auto custom = app.s.gameColors.find(game_id(game));
    if (custom == app.s.gameColors.end())
        return builtin_accent(game);
    const AccentColors a = accent_from(custom->second, app.dark);
    return {a.accent, a.fill, a.onAccent, a.ring};
}

void apply_theme() {
    app.dark = system_dark();
    app.theme = app.dark ? t::kDark : t::kLight;
    const t::Accent a = game_accent(games::launcher_game());
    app.theme.accent = a.accent;
    app.theme.accent_fill = a.accent_fill;
    app.theme.on_accent = a.on_accent;
    app.theme.focus_ring = a.focus_ring;
    app.theme.sidebar_selection = a.accent_fill;
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

void apply_color_edit();  // the Games pane's colour into the settings (below)

void changed(bool save) {
    app.status = Status::Idle;
    if (save)
        app.undoRestore.reset();  // Undo is for Restore Defaults alone: a change since makes it the user's
    if (save && app.iniFound) {
        save_settings(app.ini, app.s);
        app.iniText = ini_text();
        push_live();
        refresh_profiles();  // whether the settings still match the profile in use
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void refresh_profiles() {
    const std::vector<std::wstring> before = app.profileNames;
    app.profileNames = profile_names(profiles_folder(app.ini));
    // A profile's button keeps the focus by its index: with the list changed it would be another profile's.
    if (app.profileNames != before && app.focus >= kProfileUse && app.focus <= kProfileRevert)
        app.focus = kNone;
    app.profileCurrent = current_profile(app.ini.c_str());
    Profile p;
    app.profileMatches = !app.profileCurrent.empty() && load_profile(profiles_folder(app.ini), app.profileCurrent, p, app.s.eyeTracking) &&
                         profile_matches(app.ini.c_str(), p);
}

namespace {

// The session runtime.ini reports, into the history (session_history.h) when its numbers are new: with the settings
// its game has now in the launcher's folder (the launcher's ini or the game's copy, where the headset menu also writes).
void record_session() {
    const SessionInfo& s = app.runtime.session;
    if (!app.iniFound || !s.known())
        return;
    const std::wstring id = session_id(s.log);
    if (id.empty())
        return;
    for (const SessionRecord& r : app.sessions)
        if (r.id == id && r.same_numbers(s))
            return;
    // Its game: the run folder's copy of the ini names it ([launcher] game, ACE when unset); without one, the game shown.
    const std::wstring runIni = session_run_ini(s.log);
    std::wstring game = game_id(games::launcher_game());
    if (GetFileAttributesW(runIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
        wchar_t named[32] = {};
        GetPrivateProfileStringW(L"launcher", L"game", L"ace", named, static_cast<DWORD>(std::size(named)), runIni.c_str());
        game = named;
    }
    // Every ~10 s while the game runs, so the history is kept up to date here rather than read again (12 files of ~100
    // keys): the shown game's settings are the window's own, kept in step with the ini (follow_ini).
    const std::wstring source = game_settings_target(app.ini, game);
    SessionRecord record = session_record(s, game, game == game_id(games::launcher_game()) ? app.s : load_settings(source));
    if (!store_session(sessions_folder(app.ini), record, source))
        return;
    std::vector<SessionRecord>& list = app.sessions;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SessionRecord& r) { return r.id == record.id; }), list.end());
    const auto at = std::find_if(list.begin(), list.end(), [&](const SessionRecord& r) { return r.id < record.id; });  // newest first
    list.insert(at, std::move(record));
    if (list.size() > static_cast<size_t>(kSessionHistory))
        list.resize(kSessionHistory);
}

}  // namespace

// runtime.ini read again (the headset, the last session), and that session kept in the history.
void reload_runtime() {
    app.runtime = load_runtime_info(runtime_info_path());
    record_session();
}

// The in-headset settings menu saves into the same ini: show what it set (not while a knob is held). A
// read halfway through the menu's writes is read again on the next tick (the bytes differ by then).
void follow_ini() {
    if (!app.iniFound || app.dragging != kNone)
        return;
    std::string text = ini_text();
    if (text == app.iniText)
        return;
    app.iniText = std::move(text);
    app.s = load_settings(app.ini);
    app.undoRestore.reset();
    refresh_profiles();  // the headset menu may have switched profiles (the action bar names the one in use)
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

// The exe's size and write time: a game update changes them.
uint64_t file_stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime) ^
           (static_cast<uint64_t>(a.nFileSizeHigh) << 32 | a.nFileSizeLow);
}

}  // namespace

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
    PreflightInput in = launcher_preflight(exe, build_state(current ? app.exeSha : "", current && app.hashing));
    // The game launched from here: still starting, or Sim-QVFR seen loading in it (its live_pid) though runtime.ini's
    // live_ini isn't read yet.
    const LaunchState launchState = app.watch.game() == &games::launcher_game() ? app.watch.outcome().state : LaunchState::Closed;
    in.gameStarting = launchState == LaunchState::Starting;
    in.gameHasLayer |= in.gameRunning && launchState == LaunchState::Running;
    app.checks = preflight_checks(in);
}

int* slider_value(Ctl id) {
    switch (id) {
    case kOuter: return &app.s.outer;
    case kInner: return &app.s.inner;
    case kObjectDetail: return &app.s.objectDetail;
    case kColorValue: return &app.colorValue;
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
    case kObjectDetail: return kObjectDetailRange;
    case kColorValue: return {0, 100};
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

namespace {

bool inner_disabled() { return app.s.mode == RenderMode::TwoLoop; }

}  // namespace

// Two-loop is legacy (ini mode=quad still runs it): the method picker shows only while it is chosen, to leave it.
bool method_shown() { return app.s.mode == RenderMode::TwoLoop; }

std::vector<Ctl> focus_order() {
    std::vector<Ctl> order = {kGameTab, kSidebar};
    switch (app.pane) {
    case Pane::Checks: break;  // the fix buttons are the mouse's (one Ctl for them all)
    case Pane::Hotkeys: order.push_back(kHotkeysReset); break;
    case Pane::Profiles: order.insert(order.end(), {kProfileSave, kProfilePaste}); break;  // each profile's buttons: the mouse's  // the bindings are the mouse's, as the fix buttons
    case Pane::Resolution:
        order.push_back(kOuter);
        if (!inner_disabled()) {
            order.push_back(kInner);
            if (game_has(&games::GameFeatures::objectDetail))
                order.push_back(kObjectDetail);
            if (game_has(&games::GameFeatures::fovCrop))
                order.push_back(kCropOn);
            if (app.s.crop && game_has(&games::GameFeatures::fovCrop))
                order.insert(order.end(), {kCropTop, kCropBottom, kCropOuter, kCropInner});
            if (game_has(&games::GameFeatures::helmet))
                order.push_back(kHelmetOn);
            if (app.s.helmet.on && game_has(&games::GameFeatures::helmet))
                order.insert(order.end(), {kHelmetRim, kHelmetCorners, kHelmetLight, kVisor, kHelmetSize, kHelmetHeight,
                                           kHelmetDepth, kHelmetDetail});
        }
        break;
    case Pane::Foveation:
        if (method_shown())
            order.push_back(kMethod);
        order.insert(order.end(), {kMode, kWidth, kHeight});
        if (!app.s.shape.corners)
            order.push_back(kCorner);
        order.insert(order.end(), {kFeather, kFocusOffset});
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
        break;
    case Pane::Dlss:
        if (!game_has(&games::GameFeatures::foveatedDlss))
            break;
        order.insert(order.end(), {kCheekyUse, kCheekyRuntime, kCheekyOn, kCheekyOuter, kCheekyScale, kCheekyOuterPreset,
                                   kCheekyFocusPreset, kCheekySupersampling, kCheekyMotionFix, kCheekyNr});
        if (app.s.cheeky.nr)
            order.insert(order.end(), {kCheekyNrPreset, kCheekyNrPasses});
        if (!game_has(&games::GameFeatures::frameGeneration))
            break;
        order.push_back(kFgOn);
        if (app.s.cheeky.fg)
            order.insert(order.end(), {kFgMultiplier, kFgSize, kFgScale, kFgPeriphery, kFgRelease, kFgBorder});
        break;
    case Pane::Menu:
        order.insert(order.end(), {kMenuYaw, kMenuPitch, kMenuWidth, kMenuPanel, kHotkeyFooter, kOsdOn});
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
    case Pane::Games: order.insert(order.end(), {kColorValue, kColorReset}); break;
    case Pane::Diagnostics: order.insert(order.end(), {kDiagCapture, kDiagFrameCapture, kDiagBaseline, kDiagCopy, kDiagLogs}); break;
    case Pane::Report:
        order.insert(order.end(), {kDiagKind, kDiagDescription, kDiagReport});
        if (!app.reportZip.empty())
            order.push_back(kDiagZip);
        order.push_back(kDiagExport);
        break;
    case Pane::About: order.insert(order.end(), {kSupport, kUpdateCheck, kUpdateNow}); break;
    }
    order.insert(order.end(), {kRestore, kLaunch});
    return order;
}

namespace {

void start_launch();

}  // namespace

void launch() {
    if (!app.iniFound) {
        app.status = Status::Error;
        app.error = L"sim-qvfr.ini not found next to the launcher.";
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    // Every change here was saved when it was made; the ini may have changed since (the headset menu), so
    // launch from what it holds, not from what this window last read.
    app.s = load_settings(app.ini);
    save_settings(app.ini, app.s);
    app.iniText = ini_text();
    refresh_checks();
    // Running already (from here or not): Launch checks says how, and Launch starts nothing.
    if (game_running()) {
        app.pane = Pane::Checks;
        app.scroll = 0;
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    for (const Check& c : app.checks)
        if (c.id == "game" && c.tone == CheckTone::Bad) {  // its fix, Choose folder…, is in Launch checks
            app.status = Status::Error;
            app.error = game_name() + L" not found: choose its folder in Launch checks.";
            app.pane = Pane::Checks;
            app.scroll = 0;
            InvalidateRect(app.hwnd, nullptr, FALSE);
            return;
        }
    // A launch that would crash the game, or run it without Sim-QVFR: asked first, Show Checks the focused answer.
    std::wstring problems;
    for (const Check& c : app.checks)
        if (c.confirm)
            problems += (problems.empty() ? L"" : L"\n") + std::wstring(L"\u2022 ") + c.title;
    if (problems.empty()) {
        start_launch();
        return;
    }
    Sheet s;
    s.title = L"Launch " + game_name() + L" anyway?";
    s.text = L"The launch checks found:\n" + problems;
    s.buttons = {L"Show Checks", L"Launch Anyway"};
    s.focus = 0;
    s.danger = true;
    s.done = [](int button, const std::wstring&) {
        if (button == 1) {
            start_launch();
            return;
        }
        app.pane = Pane::Checks;
        app.scroll = 0;
        InvalidateRect(app.hwnd, nullptr, FALSE);
    };
    open_sheet(std::move(s));
}

namespace {

// The launch itself (main.cpp's launch_game), then the watch on what becomes of it.
void start_launch() {
    app.status = Status::Starting;
    RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    std::wstring report;
    LaunchedGame launched;
    if (launch_game(false, report, &launched) != 0) {
        app.status = Status::Error;
        const size_t at = report.find(L"ERROR: ");
        app.error = at == std::wstring::npos ? L"Launch failed." : report.substr(at + 7, report.find(L'\n', at) - at - 7);
    } else {
        // From here the watch says what became of it: running with Sim-QVFR, without it, closed or crashed.
        app.status = Status::Idle;
        app.watch.start(launched, games::launcher_game(), runtime_info_path(),
                        game_crashes_folder(games::launcher_game().crashReports));
        refresh_checks();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

}  // namespace

}  // namespace simqvfr::launcher::ui::win

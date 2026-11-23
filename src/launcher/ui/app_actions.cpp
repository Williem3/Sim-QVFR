// What the window does with the settings: the theme and the game's accent, saving a change (and pushing it to
// a running game), the profiles, following the ini, the launch checks, the sliders' values and ranges, the
// focus order, and Launch (launch_game, main.cpp).
#include <algorithm>
#include <iterator>
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
#include "../../engine/fsr_probe.h"
#include "../../ini_put.h"
#include "../../sha256_file.h"
#include "window_state.h"
#include "controls.h"

#pragma comment(lib, "dwmapi.lib")

namespace simqvfr::launcher::ui::win {

void start_fsr_probe() {
    if (app.fsrProbing || app.fsrOffer.checked)
        return;
    app.fsrProbing = true;
    std::thread([hwnd = app.hwnd, folder = layer_folder()] {  // a D3D12 device and AMD's 28 MB DLL: under a second
        PostMessageW(hwnd, kFsrProbedMessage, 0, reinterpret_cast<LPARAM>(new fsr::Offer(fsr::probe_offer(folder))));
    }).detach();
}

namespace {

bool system_dark() {
    // SIM_QVFR_LAUNCHER_THEME=light|dark overrides the theme button and the Windows app theme (for checking both).
    wchar_t forced[16] = {};
    if (GetEnvironmentVariableW(L"SIM_QVFR_LAUNCHER_THEME", forced, 16))
        return _wcsicmp(forced, L"dark") == 0;
    if (app.themeChoice != ThemeChoice::System)
        return app.themeChoice == ThemeChoice::Dark;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

constexpr const wchar_t* kThemeChoiceNames[] = {L"system", L"light", L"dark"};  // ThemeChoice's order

}  // namespace

// [launcher] ui_theme: system (the default, also for anything else), light or dark. One for every game: [launcher] is
// in no profile and no game's copy.
ThemeChoice read_theme_choice() {
    if (!app.iniFound)
        return app.themeChoice;
    wchar_t value[16] = {};
    GetPrivateProfileStringW(L"launcher", L"ui_theme", L"", value, static_cast<DWORD>(std::size(value)), app.ini.c_str());
    for (int i = 0; i < 3; ++i)
        if (_wcsicmp(value, kThemeChoiceNames[i]) == 0)
            return static_cast<ThemeChoice>(i);
    return ThemeChoice::System;
}

// The theme button: following Windows, then light, then dark; kept in the ini (in this window alone without one).
void cycle_theme() {
    app.themeChoice = static_cast<ThemeChoice>((static_cast<int>(app.themeChoice) + 1) % 3);
    if (app.iniFound) {
        ini_put(app.ini.c_str(), L"launcher", L"ui_theme", kThemeChoiceNames[static_cast<int>(app.themeChoice)]);
        app.iniText = ini_text();  // not a change follow_ini needs to read back
    }
    apply_theme();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

// lang\ beside the launcher (sim-qvfr.ini's folder): a release's and a development build's alike.
std::filesystem::path language_folder() { return lang::folder_near(std::filesystem::path(settings_path()).parent_path()); }

// The language a choice shows in: the file's own name, or English.
std::wstring language_name(const std::wstring& code) {
    for (const lang::Info& l : app.languages)
        if (_wcsicmp(l.code.c_str(), code.c_str()) == 0)
            return l.name;
    return L"English";
}

}  // namespace

// [launcher] language: auto (the default: Windows' display language when a file has it, else English), en, or a
// lang\ file's code. One for every game, as ui_theme.
void read_language() {
    app.languages = lang::available(language_folder());
    if (app.iniFound) {
        app.languageChoice = ini_get(app.ini.c_str(), L"launcher", L"language", L"auto");
        if (app.languageChoice.empty())
            app.languageChoice = L"auto";
    }
    app.languageInUse = lang::select(language_folder(), app.languageChoice);
}

// The pop-up: Windows' language (and what it shows in), English, then each file. Returns the one chosen.
int language_items(std::vector<std::wstring>& items) {
    items.clear();
    const bool automatic = _wcsicmp(app.languageChoice.c_str(), L"auto") == 0;
    items.push_back(trf(L"Windows language ({language})",
                        {{L"language", language_name(lang::resolve(L"auto", app.languages))}}));
    items.push_back(L"English");
    int selected = automatic ? 0 : 1;
    for (size_t i = 0; i < app.languages.size(); ++i) {
        items.push_back(app.languages[i].name);
        if (!automatic && _wcsicmp(app.languages[i].code.c_str(), app.languageChoice.c_str()) == 0)
            selected = static_cast<int>(i) + 2;
    }
    return selected;
}

// A choice from the pop-up: saved, the file loaded, the window redrawn in it (every text is looked up as it draws).
void choose_language(int option) {
    if (option < 0 || option >= static_cast<int>(app.languages.size()) + 2)
        return;
    app.languageChoice = option == 0 ? L"auto" : option == 1 ? L"en" : app.languages[static_cast<size_t>(option) - 2].code;
    if (app.iniFound) {
        ini_put(app.ini.c_str(), L"launcher", L"language", app.languageChoice.c_str());
        app.iniText = ini_text();  // not a change follow_ini needs to read back
    }
    app.languageInUse = lang::select(language_folder(), app.languageChoice);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

COLORREF colorref(uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

// A game tab's palette (tokens.json "games"), for the theme in use; the kit's default accent for a game without one.
t::Accent builtin_accent(const games::GameInfo& game) {
    for (const t::GamePalette& p : t::kGamePalettes)
        if (_stricmp(p.id, game.id) == 0)
            return app.dark ? p.dark : p.light;
    const t::Colors& base = app.dark ? t::kDark : t::kLight;
    return {base.accent, base.accent_fill, base.on_accent, base.focus_ring};
}

// A game's accent: the colour picked for it in the Games pane (game_tabs.h accent_from), else its palette.
t::Accent game_accent(const games::GameInfo& game) {
    const auto custom = app.s.gameColors.find(games::wide_id(game));
    if (custom == app.s.gameColors.end())
        return builtin_accent(game);
    const AccentColors a = accent_from(custom->second, app.dark);
    return {a.accent, a.fill, a.onAccent, a.ring};
}

// The theme's colours. A game's own colour is its marker only (its tab, the rail): selection stays blue and Launch red,
// whichever game is chosen, so each colour keeps its one meaning.
void apply_theme() {
    app.dark = system_dark();
    app.theme = app.dark ? t::kDark : t::kLight;
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
    if (save) {  // what this change changed, for Undo (the settings session history compares; others aren't offered)
        const std::vector<std::wstring> diff = setting_changes(app.lastSaved, app.s);
        if (app.undoing) {
            app.undoChange.reset();
            app.undoText = diff.empty() ? L"" : trf(L"Undone: {changes}", {{L"changes", changes_text(diff, 2)}});
        } else if (!diff.empty()) {
            app.undoChange = app.lastSaved;
            app.undoText = changes_text(diff, 2);
        }
        if (!diff.empty()) {
            app.undoUntil = GetTickCount64() + kUndoMs;
            SetTimer(app.hwnd, kUndoTimer, static_cast<UINT>(kUndoMs + 50), nullptr);
        }
        app.lastSaved = app.s;
        app.undoing = false;
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
    const std::wstring id = session_record_id(s);
    if (id.empty())
        return;
    for (const SessionRecord& r : app.sessions)
        if (r.id == id && r.same_numbers(s))
            return;
    // Its game: the run folder's copy of the ini names it ([launcher] game, ACE when unset); without one, the game shown.
    const std::wstring runIni = session_run_ini(s.log);
    std::wstring game = games::wide_id(games::launcher_game());
    if (GetFileAttributesW(runIni.c_str()) != INVALID_FILE_ATTRIBUTES)
        game = launcher_game_id(runIni.c_str());
    // Every ~10 s while the game runs, so the history is kept up to date here rather than read again (12 files of ~100
    // keys): the shown game's settings are the window's own, kept in step with the ini (follow_ini).
    const std::wstring source = game_settings_target(app.ini, game);
    SessionRecord record = session_record(s, game, game == games::wide_id(games::launcher_game()) ? app.s : load_settings(source));
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
    settings_reloaded();  // the headset menu's change isn't the launcher's to undo
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
    case kSharpen: return &app.s.sharpen;
    case kTextureMatch: return &app.s.textureMatch;
    case kGradeExposure: return &app.s.grade.exposure;
    case kGradeBrightness: return &app.s.grade.brightness;
    case kGradeContrast: return &app.s.grade.contrast;
    case kGradeSaturation: return &app.s.grade.saturation;
    case kNightFill: return &app.s.carLight.fill;
    case kNightOcclusion: return &app.s.carLight.occlusion;
    case kNightHighlights: return &app.s.carLight.highlights;
    case kNightGloss: return &app.s.carLight.gloss;
    case kNightSmooth: return &app.s.carLight.smooth;
    case kCorridorWidth: return &app.s.corridorWidth;
    case kCorridorHeight: return &app.s.corridorHeight;
    case kCorridorBlend: return &app.s.corridorBlend;
    case kColorValue: return &app.colorValue;
    case kWidth: return &app.s.width;
    case kHeight: return &app.s.height;
    case kFeather: return &app.s.feather;
    case kCorner: return &app.s.corner;
    case kFocusOffset: return &app.s.offset.for_mode(app.s.eyeTracking);
    case kFocusOffsetX: return &app.s.offset.for_mode_x(app.s.eyeTracking);
    case kGazeDeadzone: return &app.s.gazeDeadzone;
    case kShapePointY:
    case kShapePointX: {  // the picked point of the custom shape, or the whole shape's middle (shape_slider_set)
        app.shapePoint = std::clamp(app.shapePoint, kFocusWholeShape, std::clamp(app.s.shape.count, 1, kFocusPointsMax) - 1);
        if (app.shapePoint == kFocusWholeShape) {
            const FocusPoint c = focus_shape_centre(app.s.shape);
            app.shapeCentre[0] = c.x;
            app.shapeCentre[1] = c.y;
            return id == kShapePointY ? &app.shapeCentre[1] : &app.shapeCentre[0];
        }
        FocusPoint& p = app.s.shape.points[app.shapePoint];
        return id == kShapePointY ? &p.y : &p.x;
    }
    case kMenuYaw: return &app.s.menuYaw;
    case kMenuPitch: return &app.s.menuPitch;
    case kMenuWidth: return &app.s.menuWidth;
    case kCheekyScale: return &app.s.cheeky.outerScale;
    case kNrIntensity: return &app.s.nr.intensity;
    case kNrLocalTone: return &app.s.nr.localTone;
    case kNrLocalStructure: return &app.s.nr.localStructure;
    case kNrFocusScale: return &app.s.nr.focusScale;
    case kNrEdgeBlend: return &app.s.nr.edgeBlend;
    case kNrPeripheryScale: return &app.s.nr.peripheryScale;
    case kNrPeripheryCoverage: return &app.s.nr.peripheryCoverage;
    case kNrLimit: return &app.s.nr.workingLimit;
    case kCheekySupersampling: return &app.s.cheeky.supersampling;
    case kFgSize: return &app.s.cheeky.fgSize;
    case kOsdYaw: return &app.s.osd.yaw;
    case kOsdPitch: return &app.s.osd.pitch;
    case kOsdSize: return &app.s.osd.width;
    case kClockYaw: return &app.s.clock.yaw;
    case kClockPitch: return &app.s.clock.pitch;
    case kClockSize: return &app.s.clock.width;
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
    case kSharpen: return kSharpenRange;
    case kTextureMatch: return kTextureMatchRange;
    case kGradeExposure: return kColorExposureRange;
    case kGradeBrightness: return kColorBrightnessRange;
    case kGradeContrast: return kColorContrastRange;
    case kGradeSaturation: return kColorSaturationRange;
    case kNightFill: return kCarFillRange;
    case kNightOcclusion: return kCarOcclusionRange;
    case kNightHighlights: return kCarHighlightsRange;
    case kNightGloss: return kCarGlossRange;
    case kNightSmooth: return kCarSmoothRange;
    case kCorridorWidth: return kCorridorWidthRange;
    case kCorridorHeight: return kCorridorHeightRange;
    case kCorridorBlend: return kCorridorBlendRange;
    case kColorValue: return {0, 100};
    case kFeather: return kFeatherRange;
    case kCorner: return kCornerRange;
    case kFocusOffset:
    case kFocusOffsetX: return kFocusOffsetRange;
    case kGazeDeadzone: return kGazeDeadzoneRange;
    case kShapePointY:
    case kShapePointX: return kFocusPointRange;
    case kMenuYaw: return kMenuYawRange;
    case kMenuPitch: return kMenuPitchRange;
    case kMenuWidth: return kMenuWidthRange;
    case kCheekyScale: return {kCheekyScaleMin, kCheekyScaleMax};
    case kNrIntensity: return {0, 100};
    case kNrLocalTone:
    case kNrLocalStructure: return {0, 200};
    case kNrFocusScale:
    case kNrPeripheryScale: return {kNrScaleMin, 100};
    case kNrEdgeBlend: return {0, kNrEdgeMax};
    case kNrPeripheryCoverage: return {kNrCoverageMin, 100};
    case kNrLimit: return {kNrLimitMin, kNrLimitMax};
    case kCheekySupersampling: return {kCheekySupersamplingMin, kCheekySupersamplingMax};
    case kFgSize: return {kCheekyFgSizeMin, kCheekyFgSizeMax};
    case kOsdYaw: return {-kOsdYawMax, kOsdYawMax};
    case kOsdPitch: return {-kOsdPitchMax, kOsdPitchMax};
    case kOsdSize: return {kOsdWidthMin, kOsdWidthMax};
    case kClockYaw: return {-kClockYawMax, kClockYawMax};
    case kClockPitch: return {-kClockPitchMax, kClockPitchMax};
    case kClockSize: return {kClockWidthMin, kClockWidthMax};
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

// Two-loop is legacy (ini mode=quad still runs it): the method picker shows only while it is chosen, to leave it.
bool method_shown() { return app.s.mode == RenderMode::TwoLoop; }

namespace {

// The controls of a page of cards in the order they were drawn (card by card, top to bottom), from the last frame's
// hits: each once, a card's menu once per card. What a disclosure hides was not drawn, so it isn't in the order.
// In a panel only its own hits were drawn, and its Ko-fi button (About) is one of them.
std::vector<FocusStop> drawn_order(bool panel = false) {
    static const Ctl kNotStops[] = {kNone, kPage, kGameTab, kTopTab, kSearch, kProfileMenu, kVersion, kThemeToggle,
                                    kLaunch, kSupport, kHelpIcon, kSearchResult, kSearchField, kSheetButton, kSheetField,
                                    kChecksBadge, kLaunchBadge, kUndoBadge, kUpdateBadge, kColorWheel, kDiagZip, kText};
    std::vector<FocusStop> out;
    for (const Hit& h : app.hits) {
        if (std::find(std::begin(kNotStops), std::end(kNotStops), h.id) != std::end(kNotStops) && !(panel && h.id == kSupport))
            continue;
        if (h.r.bottom() < app.scrollArea.y - 2000 || h.r.y > app.scrollArea.bottom() + 2000)
            continue;  // not this page's (an index pass draws far away)
        const bool several = h.id == kCardMenu || h.id == kMore || h.id == kHotkeyKey || h.id == kHotkeyPad ||
                             h.id == kCheckFix || h.id == kCheckOff || h.id == kCheckGuide || h.id == kGameShown ||
                             h.id == kColorGame || h.id == kColorPreset ||
                             h.id == kOverviewCard || (h.id >= kProfileUse && h.id <= kProfileRevert);
        // A layer's switch and moves: one stop, the list (the arrows pick a row, Space and Ctrl+arrows act on it).
        const bool layer = h.id == kLayerOn || h.id == kLayerUp || h.id == kLayerDown;
        const FocusStop stop{layer ? kLayerList : h.id, several ? h.option : -1};
        if (std::find(out.begin(), out.end(), stop) == out.end())
            out.push_back(stop);
    }
    return out;
}

}  // namespace

std::vector<FocusStop> focus_order() {
    if (app.panel != PanelKind::None) {  // a panel: only its own controls (its ✕ first, as drawn)
        std::vector<FocusStop> order = drawn_order(true);
        if (app.panel == PanelKind::Report && !app.reportZip.empty())
            order.push_back({kDiagZip});  // Return shows it in Explorer
        return order;
    }
    std::vector<FocusStop> order = {{kGameTab}, {kTopTab}, {kPage}};
    if (tab_pages(tab_of(app.pane)).size() < 2)
        order.pop_back();  // no page strip
    switch (app.pane) {
    case Pane::Overview:
    case Pane::Picture:
    case Pane::Headset:
    case Pane::Game: {
        const std::vector<FocusStop> drawn = drawn_order();
        order.insert(order.end(), drawn.begin(), drawn.end());
        break;
    }
    }
    order.insert(order.end(), {{kThemeToggle}, {kSearch}, {kProfileMenu}, {kLaunch}, {kVersion}});  // the top bar, the rail's foot
    return order;
}

namespace {

void start_launch();

}  // namespace

void launch() {
    if (!app.iniFound) {
        app.status = Status::Error;
        app.error = tr(L"sim-qvfr.ini not found next to the launcher.");
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    // Every change here was saved when it was made; the ini may have changed since (the headset menu), so
    // launch from what it holds, not from what this window last read.
    app.s = load_settings(app.ini);
    save_settings(app.ini, app.s);
    app.iniText = ini_text();
    settings_reloaded();
    refresh_checks();
    // Running already (from here or not): Launch checks says how, and Launch starts nothing.
    if (game_running()) {
        jump_to({tr(L"Launch checks"), tr(L"Launch checks"), Pane::Game});
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    for (const Check& c : app.checks)
        if (c.id == "game" && c.tone == CheckTone::Bad) {  // its fix, Choose folder…, is in Launch checks
            app.status = Status::Error;
            app.error = trf(L"{game} not found: choose its folder in Launch checks.", {{L"game", game_name()}});
            jump_to({tr(L"Launch checks"), tr(L"Launch checks"), Pane::Game});
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
    s.title = trf(L"Launch {game} anyway?", {{L"game", game_name()}});
    s.text = trf(L"The launch checks found:\n{problems}", {{L"problems", problems}});
    s.buttons = {tr(L"Show Checks"), tr(L"Launch Anyway")};
    s.focus = 0;
    s.danger = true;
    s.done = [](int button, const std::wstring&) {
        if (button == 1) {
            start_launch();
            return;
        }
        jump_to({tr(L"Launch checks"), tr(L"Launch checks"), Pane::Game});
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
        app.error = at == std::wstring::npos ? std::wstring(tr(L"Launch failed."))
                                             : report.substr(at + 7, report.find(L'\n', at) - at - 7);
    } else {
        // From here the watch says what became of it: running with Sim-QVFR, without it, closed or crashed.
        app.status = Status::Idle;
        // The launch took the one-launch settings it ran with (Next launch without QVFR, a Detailed logs launch) in the
        // ini: the window's copy follows, so the next save doesn't put them back.
        const Settings now = load_settings(app.ini);
        app.s.baselineNext = now.baselineNext;
        app.s.debugLogs = now.debugLogs;
        app.watch.start(launched, games::launcher_game(), runtime_info_path(),
                        game_crashes_folder(games::launcher_game().crashReports));
        refresh_checks();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

}  // namespace

}  // namespace simqvfr::launcher::ui::win

// Acting on a control (activate), from a click or the keyboard (push_button, space_control, on_key).
#include <algorithm>
#include <string>

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include "../launch_plan.h"
#include "../launcher.h"
#include "../game_tabs.h"
#include "../settings.h"
#include "../update_check.h"
#include "../../engine/nvapi_gpus.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

void activate(Ctl id, int option) {
    end_capture();  // any click ends catching a hotkey (one on a hotkey's button starts again)
    if (lmu_game_ctl(id) || id == kLmuReload || id == kLmuOpenFolder || id == kLmuRestore || id == kLmuFovReset) {
        lmu_game_activate(id, option);  // LMU's own files (pane_lmu_game.cpp)
        return;
    }
    if (developer_press(id))  // the Developer tab's switches (developer.cpp)
        return;
    if (mods_press(id, option))  // the Mods card (pane_mods.cpp)
        return;
    switch (id) {
    case kPage:
        app.pane = static_cast<Pane>(option);
        app.scroll = 0;
        if (app.pane == Pane::Game) {  // a layer may have changed, a session may have ended
            refresh_layers();
            reload_runtime();
        }
        if (app.pane == Pane::Headset)
            open_pads();
        if (app.pane == Pane::LmuGame)  // the game may have written its files since
            lmu_game_refresh();
        if (app.pane == Pane::Overview) {  // the profile in use, the last session, the layers below
            refresh_profiles();
            reload_runtime();
            refresh_layers();
        }
        app.tabPage[static_cast<int>(tab_of(app.pane))] = app.pane;  // the tab comes back to it
        break;
    case kTopTab: show_tab(static_cast<Tab>(std::clamp(option, 0, kTabs - 1))); return;
    case kSearch: open_search(); return;
    case kProfileMenu: open_profile_menu(); return;
    case kVersion: open_panel(PanelKind::About); return;
    case kSettings: open_panel(PanelKind::Settings); return;
    case kSearchResult:
        if (option >= 0 && option < static_cast<int>(app.searchResults.size()))
            jump_to(app.searchIndex[static_cast<size_t>(app.searchResults[static_cast<size_t>(option)])]);
        return;
    case kSearchField: SetFocus(app.searchEdit); return;
    case kSheetButton: close_sheet(option); return;
    case kChecksBadge: jump_to({tr(L"Launch checks"), tr(L"Launch checks"), Pane::Game}); return;
    case kUndoBadge: undo_restore(); return;
    case kUndoChange: undo_change(); return;
    case kLaunchBadge: {
        const LaunchOutcome o = app.watch.outcome();
        if (o.link == LaunchLink::Diagnostics) {
            jump_to({tr(L"Diagnostics"), tr(L"Diagnostics"), Pane::Game});
        } else if (o.link == LaunchLink::Report) {
            if (o.state == LaunchState::Crashed)
                app.reportKind = 1;  // kIssueKinds: Crash or freeze
            show_page(Pane::Game);
            open_panel(PanelKind::Report);
            if (o.finished())
                app.watch.stop();  // seen: Report an issue has its logs
        }
        return;
    }
    case kHotkeyKey:
    case kHotkeyPad:
        if (option >= 0 && option < kHotkeyActions)
            start_capture(option, id == kHotkeyPad);
        return;
    case kHotkeysReset:
        app.s.hotkeys = default_hotkeys();
        changed();
        return;
    case kProfileUse:
    case kProfileRevert:
    case kProfileUpdate:
    case kProfileShare:
    case kProfileDelete: {
        if (option < 0 || option >= static_cast<int>(app.profileNames.size()))
            return;
        const std::wstring name = app.profileNames[option];
        Profile p;
        if (id == kProfileUse || id == kProfileRevert) {
            use_profile(name);
            if (id == kProfileRevert && app.profileMatches)
                app.profileMessage = trf(L"Back to {name}.", {{L"name", name}});
        } else if (id == kProfileUpdate) {  // the settings now, into the profile in use: no question, it's its own button
            if (save_profile(profiles_folder(app.ini), settings_profile(app.s, name))) {
                app.profileMessage = trf(L"Updated {name} with the settings you have now.", {{L"name", name}});
            } else {
                app.profileMessage = trf(L"Couldn’t update {name}.", {{L"name", name}});
            }
            refresh_profiles();
        } else if (id == kProfileShare && load_profile(profiles_folder(app.ini), name, p, app.s.eyeTracking)) {
            app.profileMessage = copy_text(widen(share_profile(p)))
                                     ? trf(L"Copied {name} to the clipboard: paste it in a chat or a post.",
                                           {{L"name", name}})
                                     : std::wstring(tr(L"Couldn’t copy to the clipboard."));
        } else if (id == kProfileDelete) {
            Sheet s;
            s.title = trf(L"Delete {name}?", {{L"name", name}});
            s.text = tr(L"It goes from Your profiles. The settings you have now stay as they are.");
            s.buttons = {tr(L"Cancel"), tr(L"Delete")};
            s.focus = 0;
            s.danger = true;
            s.done = [name](int button, const std::wstring&) {
                if (button != 1)
                    return;
                delete_profile(profiles_folder(app.ini), name);
                if (_wcsicmp(name.c_str(), app.profileCurrent.c_str()) == 0)
                    set_current_profile(app.ini.c_str(), L"");
                app.profileMessage = trf(L"Deleted {name}.", {{L"name", name}});
                refresh_profiles();
            };
            open_sheet(std::move(s));
        }
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    case kProfileSave: save_current_profile(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kProfilePaste: paste_profile(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kCheckFix:
        if (option >= 0 && option < static_cast<int>(app.checks.size()))
            fix_check(app.checks[option]);
        return;
    case kCheckOff:
        if (option >= 0 && option < static_cast<int>(app.checks.size()))
            turn_off_for(app.checks[option]);
        return;
    case kCheckGuide: ShellExecuteW(app.hwnd, L"open", kCheekyGuideUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    case kDiagCapture: app.s.cpuProfile = !app.s.cpuProfile; changed(); return;
    case kDiagFrameCapture: app.s.frameCapture = !app.s.frameCapture; changed(); return;
    case kDiagBaseline: app.s.baselineNext = !app.s.baselineNext; changed(); return;
    case kDiagDebugLogs: app.s.debugLogs = app.s.debugLogs > 0 ? 0 : kDebugLogLaunches; changed(); return;
    case kAntiCheat: {
        if (!games::launcher_game().antiCheatStart)
            return;
        if (app.s.antiCheat) {  // off: at once
            app.s.antiCheat = false;
            changed();
            return;
        }
        // On: only once the player has read and accepted that a kick or a ban is theirs to risk.
        Sheet s;
        s.title = trf(L"Run {game} with its anti-cheat?", {{L"game", short_name()}});
        s.text = tr(L"This lets you join online servers with Sim-QVFR running. The game’s anti-cheat can kick you, or "
                    L"ban your account, for using Sim-QVFR.\n\nSim-QVFR and its developer are not responsible for any "
                    L"kick or ban. By turning this on, you accept that risk and the responsibility for it.");
        s.buttons = {tr(L"Cancel"), tr(L"I accept the risk")};
        s.focus = 0;
        s.danger = true;
        s.done = [](int button, const std::wstring&) {
            if (button != 1)
                return;
            app.s.antiCheat = true;
            changed();
        };
        open_sheet(std::move(s));
        return;
    }
    case kDiagKind:
    case kLanguage: open_popup(id); return;
    case kDiagDescription:
        if (app.panel != PanelKind::Report)
            return;
        if (!report_edit_fits()) {  // scroll the field into view in the panel first: the edit shows only then
            const Rect& r = app.reportEditRect;
            const Rect& body = app.panelBody;
            const float by = r.bottom() > body.bottom() ? r.bottom() + 5 - body.bottom() : r.y - 5 - body.y;
            app.panelScroll = std::clamp(app.panelScroll + by + (by > 0 ? t::kSpace4 : -t::kSpace4), 0.0f, app.panelScrollMax);
            RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
        SetFocus(app.reportEdit);
        return;
    case kReportOpen: open_panel(PanelKind::Report); return;
    case kOverviewCard:
        if (option >= 0 && option < static_cast<int>(app.overviewTargets.size()))
            jump_to(app.overviewTargets[static_cast<size_t>(option)]);
        return;
    case kKofiTile: ShellExecuteW(app.hwnd, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    case kPanelClose: close_panel(); return;
    case kColorPreset: use_color_preset(option); return;
    case kColorCustom: open_panel(PanelKind::Color); return;
    case kDiagReport: report_issue(); return;
    case kDiagExport: export_logs(); return;
    case kDiagZip: show_report_zip(); return;  // the keyboard's; the mouse drags it (drag_report_zip)
    case kDiagCopy: copy_summary(); return;
    case kDiagLogs: ShellExecuteW(app.hwnd, L"explore", logs_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return;
    case kDiagClearCaptures: clear_captures(); return;
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
    case kGameTab: select_game(option); return;
    case kNightQuality:
    case kMode:
    case kShape:
    case kVisor:
    case kHelmetDetail:
    case kEyeOff:
    case kCorridorPreset:
    case kDlssFocusPreset:
    case kDlssEyePreset:
    case kNrOrder:
    case kFgMultiplier:
    case kFgScale:
    case kFgPeriphery:
    case kLayerProfile:
    case kOverviewProfile:
    case kPreviewEye: {  // segmented: the segment clicked, or (the keyboard's Space) the next one
        std::vector<std::wstring> items;
        const int selected = choice_items(id, items);
        if (!items.empty())
            choose_option(id, option >= 0 ? option : (selected + 1) % static_cast<int>(items.size()));
        return;
    }
    case kCardMenu: open_card_menu(option); return;
    case kMore: {
        if (option < 0 || option >= static_cast<int>(app.moreKeys.size()))
            return;
        const std::wstring& key = app.moreKeys[static_cast<size_t>(option)];
        if (!app.moreOpen.erase(key))
            app.moreOpen.insert(key);
        break;
    }
    case kHelmetFolder: {
        const std::wstring folder = layer_folder() + L"\\helmet";
        CreateDirectoryW(folder.c_str(), nullptr);
        ShellExecuteW(app.hwnd, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    case kCheekyOuterPreset:
    case kCheekyFocusPreset:
    case kCheekyNrPreset:
    case kCheekyNrPasses:
    case kFgRelease:
    case kDlssFocusMode:
    case kDlssSharpening:
    case kNrStyle:
    case kNrPreset:
    case kShapePointPick: open_popup(id); return;
    case kShapeRemove:  // the picked point of the custom shape (not below three; nothing with Whole shape picked)
        if (!focus_shape_remove_point(app.s.shape, app.shapePoint))
            return;
        app.shapePoint = std::clamp(app.shapePoint - 1, 0, app.s.shape.count - 1);
        changed();
        return;
    case kShapeReset:  // the custom shape back to the box's four corners
        focus_shape_reset(app.s.shape);
        app.shapePoint = kFocusWholeShape;
        changed();
        return;
    case kNrOn:
        if (own_nr_available()) {  // greyed out without DLSS on or the player's nvngx_dlssnr.dll
            app.s.nr.on = !app.s.nr.on;
            changed();
        }
        return;
    case kNrAutoMask:
    case kNrPeriphery:
        if (own_nr_available() && app.s.nr.on) {
            bool& value = id == kNrAutoMask ? app.s.nr.autoMask : app.s.nr.periphery;
            value = !value;
            changed();
        }
        return;
    case kNrFolder: choose_nr_dll(); return;
    case kDlssOn:
        if (own_upscaler_available()) {  // greyed out without QVFR (DLSS or, without an RTX card, FSR)
            app.s.dlss.on = !app.s.dlss.on;
            changed();
        }
        return;
    case kDlssCarVectors:
        if (own_upscaler_available() && app.s.dlss.on) {  // greyed out while DLSS (or FSR) is off
            app.s.dlss.carVectors = !app.s.dlss.carVectors;
            changed();
        }
        return;
    case kDlssFolder: choose_dlss_dll(); return;
    case kDlssIncluded: use_included_dlss(); return;
    case kFgOn:
        if (game_has(&games::GameFeatures::frameGeneration)) {  // greyed out for a game without it
            app.s.cheeky.fg = !app.s.cheeky.fg;
            changed();
        }
        return;
    case kOsdOn: app.s.osd.enabled = !app.s.osd.enabled; changed(); return;
    case kClockOn: app.s.clock.enabled = !app.s.clock.enabled; changed(); return;
    case kHotkeyFooter: app.s.hotkeyFooter = !app.s.hotkeyFooter; changed(); return;
    case kCpuIsolateMain: app.s.cpuIsolateMain = !app.s.cpuIsolateMain; changed(); return;
    case kTurbo: app.s.turbo = !app.s.turbo; changed(); return;
    case kUpdateCheck:
        app.s.updateCheck = !app.s.updateCheck;
        app.s.updateAsked = true;  // a choice: the first start's question is answered
        changed();
        if (app.s.updateCheck && app.update == Update::Off)
            start_update_check();
        return;
    case kUpdateNow: start_update_check(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kUpdateDownload:
        // The page GitHub's API named: opened only if it is one of this repository's release pages.
        if (trusted_release_page(app.release.url))
            ShellExecuteW(app.hwnd, L"open", app.release.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kUpdateBadge: open_panel(PanelKind::About); return;
    case kUpdateInstall: start_install(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kUpdateRollback: start_restore(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kUpdateSkip: skip_release(true); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kUpdateUnskip: skip_release(false); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kGameShown: {
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        if (option < 0 || static_cast<std::size_t>(option) >= count)
            return;
        const bool hide = game_shown(*all[option]);
        if (hide && shown_game_count() <= 1)
            return;  // the last tab stays
        // An installed game is hidden by its listing in hidden_games; one Steam doesn't find is shown by shown_games.
        if (installed(*all[option]))
            app.s.hiddenGames = with_game_hidden(app.s.hiddenGames, games::wide_id(*all[option]), hide);
        else
            app.s.shownGames = with_game_hidden(app.s.shownGames, games::wide_id(*all[option]), !hide);
        changed();
        if (hide && all[option] == &games::launcher_game())  // the game in use went: the first shown takes over
            for (std::size_t i = 0; i < count; ++i)
                if (game_shown(*all[i])) {
                    select_game(static_cast<int>(i));
                    break;
                }
        return;
    }
    case kColorGame: open_color_menu(option); return;
    case kColorReset:
        app.s.gameColors.erase(games::wide_id(color_game()));
        app.colorLoaded = -2;
        load_color_edit();
        apply_theme();
        changed();
        return;
    case kCropOn:
        if (game_has(&games::GameFeatures::fovCrop)) {
            app.s.crop = !app.s.crop;
            changed();
        }
        return;
    case kHelmetOn:
        if (game_has(&games::GameFeatures::helmet)) {
            app.s.helmet.on = !app.s.helmet.on;
            changed();
        }
        return;
    case kFgBorder: app.s.cheeky.fgBorder = !app.s.cheeky.fgBorder; changed(); return;
    case kCheekyMotionFix: app.s.cheeky.motionFix = !app.s.cheeky.motionFix; changed(); return;
    case kCheekyNr: app.s.cheeky.nr = !app.s.cheeky.nr; changed(); return;
    case kOutline: app.s.outline = !app.s.outline; changed(); return;
    case kCorridorShow:
        if (game_has(&games::GameFeatures::focusCorridor)) {
            app.s.corridorShow = !app.s.corridorShow;
            changed();
        }
        return;
    case kCorridorOn:
        if (game_has(&games::GameFeatures::focusCorridor)) {
            // On: the cockpit's shape and size (the player's kept); off: the player's back (road_focus_shape.h).
            // Under native quad views (the last session's), no larger than the runtime's focus region (rounded, a
            // point under it: inside it).
            const RuntimeInfo& rt = app.runtime;
            const bool region = rt.quadViewsUsed && rt.quadFocusWidth > 1 && rt.quadFocusHeight > 1;
            road_focus_switched(!app.s.corridor, app.s.corridorPreset, app.s.shape, app.s.width, app.s.height,
                                app.s.offset, app.s.roadSaved, region ? rt.quadFocusWidth - 1 : 0,
                                region ? rt.quadFocusHeight - 1 : 0);
            app.s.corridor = !app.s.corridor;
            changed();
        }
        return;
    case kGradeOn:
        if (game_has(&games::GameFeatures::sharpening)) {
            app.s.grade.on = !app.s.grade.on;
            changed();
        }
        return;
    case kNightOn:
        if (game_has(&games::GameFeatures::carLighting)) {
            app.s.carLight.on = !app.s.carLight.on;
            changed();
        }
        return;
    case kNightShadows: app.s.carLight.shadows = !app.s.carLight.shadows; changed(); return;
    case kNightDetail: app.s.carLight.detail = !app.s.carLight.detail; changed(); return;
    case kNightReflections: app.s.carLight.reflections = !app.s.carLight.reflections; changed(); return;
    case kNightHeadlights: app.s.carLight.headlights = !app.s.carLight.headlights; changed(); return;
    case kNightGround: app.s.carLight.ground = !app.s.carLight.ground; changed(); return;
    case kNightVision: app.s.carLight.nightVision = !app.s.carLight.nightVision; changed(); return;
    case kCheekyUse: app.s.cheeky.load = !app.s.cheeky.load; changed(); return;
    case kCheekyOn: app.s.cheeky.processing = !app.s.cheeky.processing; changed(); return;
    case kCheekyOuter: app.s.cheeky.outerDlaa = !app.s.cheeky.outerDlaa; changed(); return;
    case kCheekyRuntime: choose_cheeky_runtime(); return;
    case kColor: app.s.color = static_cast<OutlineColor>(option); changed(); return;
    case kRestore: {
        const Settings before = app.s;
        app.s = restored_defaults(app.s);
        changed();
        app.undoRestore = before;
        app.undoUntil = 0;  // the status says it, with its own Undo  // the action bar offers Undo until the next change
        return;
    }
    case kLaunch: launch(); return;
    case kThemeToggle: cycle_theme(); return;
    case kSupport: ShellExecuteW(app.hwnd, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    default: break;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// A slider set from the keyboard or a screen reader: within its range, saved and sent as any change (the colour
// panel's brightness recolours the game, as dragging it does).
void set_slider(Ctl id, int value) {
    int* v = slider_value(id);
    if (!v)
        return;
    const Range r = slider_range(id);
    *v = std::clamp(value, r.min, r.max);
    if (id == kColorValue)
        apply_color_edit();
    shape_slider_set(id);
    changed();
}

float dip(LPARAM lp, bool y) {
    return (y ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp)) / app.canvas.dpi_scale();
}

namespace {

// A push button: Return presses it while it has the focus (as on Windows); anywhere else Return presses Launch, the
// window's default button. Space presses these too.
bool push_button(Ctl id) {
    switch (id) {
    case kShapeRemove:
    case kShapeReset:
    case kCheekyRuntime:
    case kDlssFolder:
    case kDlssIncluded:
    case kNrFolder:
    case kUpdateNow:
    case kUpdateDownload:
    case kUpdateInstall:
    case kUpdateRollback:
    case kUpdateSkip:
    case kUpdateUnskip:
    case kUpdateBadge:
    case kLayerApply:
    case kLayerRevert:
    case kLayerReset:
    case kLayerUp:
    case kLayerDown:
    case kSupport:
    case kSettings:
    case kRestore:
    case kCardMenu:
    case kColorGame:
    case kMore:
    case kHelmetFolder:
    case kThemeToggle:
    case kSearch:
    case kProfileMenu:
    case kVersion:
    case kLaunch:
    case kHotkeysReset:
    case kHotkeyKey:
    case kHotkeyPad:
    case kProfileUse:
    case kProfileShare:
    case kProfileDelete:
    case kProfileUpdate:
    case kProfileRevert:
    case kProfileSave:
    case kProfilePaste:
    case kCheckFix:
    case kCheckOff:
    case kCheckGuide:
    case kChecksBadge:
    case kUndoBadge:
    case kUndoChange:
    case kLaunchBadge:
    case kDiagReport:
    case kDiagZip:
    case kDiagExport:
    case kDiagCopy:
    case kDiagLogs:
    case kDiagClearCaptures:
    case kReportOpen:
    case kPanelClose:
    case kColorPreset:
    case kColorCustom:
    case kOverviewCard:
    case kKofiTile:
    case kColorReset: return true;
    default: return false;
    }
}

// A switch or a pop-up button: Space toggles or opens it; Return, as on Windows, presses Launch.
bool space_control(Ctl id) {
    if (lmu_game_ctl(id))  // LMU's switches and pop-ups
        return true;
    if (id == kDevStock || id == kDevAbTable || (id >= kDevSwitch0 && id <= kDevSwitchLast))
        return true;  // the Developer tab's switches
    if ((id >= kModOnline0 && id <= kModOnlineLast) || (id >= kModSetting0 && id <= kModSettingLast))
        return true;  // the Mods card's switches
    switch (id) {
    case kMode:
    case kOutline:
    case kCorridorOn:
    case kCorridorPreset:
    case kCorridorShow:
    case kGradeOn:
    case kNightOn:
    case kNightQuality:
    case kNightShadows:
    case kNightDetail:
    case kNightReflections:
    case kNightHeadlights:
    case kNightGround:
    case kNightVision:
    case kCheekyUse:
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
    case kDlssOn:
    case kDlssCarVectors:
    case kNrOn:
    case kNrAutoMask:
    case kNrPeriphery:
    case kNrStyle:
    case kNrPreset:
    case kNrOrder:
    case kDlssFocusMode:
    case kDlssFocusPreset:
    case kDlssEyePreset:
    case kDlssSharpening:
    case kOsdOn:
    case kClockOn:
    case kHotkeyFooter:
    case kCpuIsolateMain:
    case kTurbo:
    case kUpdateCheck:
    case kCropOn:
    case kHelmetOn:
    case kVisor:
    case kShape:
    case kHelmetDetail:
    case kEyeOff:
    case kLayerProfile:
    case kOverviewProfile:
    case kPreviewEye:
    case kLayerOn:
    case kDiagCapture:
    case kDiagFrameCapture:
    case kDiagBaseline:
    case kDiagDebugLogs:
    case kDiagKind:
    case kLanguage:
    case kGameShown:
    case kAntiCheat: return true;
    default: return false;
    }
}

}  // namespace

void on_key(WPARAM key) {
    if (app.sheet.open) {
        sheet_key(key);
        return;
    }
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (app.popup.open) {
        const int n = static_cast<int>(app.popup.items.size());
        const auto item = [](int i) {
            return app.popup.kinds.empty() || app.popup.kinds[static_cast<size_t>(i)] == PopupRow::Item;
        };
        if ((key == VK_UP || key == VK_DOWN) && n > 0) {  // the next item that is one (not a separator or a note)
            int h = app.popup.hover < 0 ? (key == VK_DOWN ? n - 1 : 0) : app.popup.hover;
            for (int step = 0; step < n; ++step) {
                h = (h + (key == VK_DOWN ? 1 : n - 1)) % n;
                if (item(h))
                    break;
            }
            app.popup.hover = h;
        }
        else if (key == VK_RETURN || key == VK_SPACE)
            close_popup(true);
        else if (key == VK_ESCAPE)
            close_popup(false);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == 'K' && GetKeyState(VK_CONTROL) < 0) {  // Ctrl+K: Find a setting
        open_search();
        return;
    }
    if (key == VK_ESCAPE && app.searchOpen) {
        close_search();
        return;
    }
    if (key == VK_ESCAPE && app.panel != PanelKind::None && app.modDetail >= 0) {  // a mod's Details first
        close_mod_details();
        return;
    }
    if (key == VK_ESCAPE && app.panel != PanelKind::None) {
        close_panel();
        return;
    }
    if (key == 'Z' && GetKeyState(VK_CONTROL) < 0 && (app.undoRestore || app.undoChange)) {  // Ctrl+Z: Undo
        if (app.undoRestore)
            undo_restore();
        else
            undo_change();
        return;
    }
    if (key == VK_TAB && GetKeyState(VK_CONTROL) < 0) {  // Ctrl+Tab, Ctrl+Shift+Tab: the next or previous game with a tab
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        const int n = static_cast<int>(count);
        for (int step = 1; step < n; ++step) {
            const int i = (game_index() + (shift ? n - step : step)) % n;
            if (game_shown(*all[i])) {
                select_game(i);
                break;
            }
        }
        return;
    }
    if (key == VK_TAB) {
        const auto order = focus_order();
        const auto it = std::find_if(order.begin(), order.end(), [](const FocusStop& s) {
            return s.id == app.focus && (s.option < 0 || s.option == app.focusOption);
        });
        int i = it == order.end() ? (shift ? 0 : -1) : static_cast<int>(it - order.begin());
        i = (i + (shift ? static_cast<int>(order.size()) - 1 : 1)) % static_cast<int>(order.size());
        app.focus = order[static_cast<size_t>(i)].id;
        app.focusOption = order[static_cast<size_t>(i)].option;
        app.keyboard = true;
        if (app.focus == kDiagDescription)  // typing goes to the description box
            activate(kDiagDescription, -1);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_RETURN) {  // the focused button, else Launch (the window's default button)
        if (push_button(app.focus))
            activate(app.focus, app.focusOption);
        else
            launch();
        return;
    }
    app.keyboard = true;
    if (const int* v = slider_value(app.focus)) {
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
        set_slider(app.focus, nv);
        return;
    }
    if (segmented_ctl(app.focus) && (key == VK_LEFT || key == VK_RIGHT)) {  // a segmented choice: the arrows step it
        std::vector<std::wstring> items;
        const int selected = choice_items(app.focus, items), n = static_cast<int>(items.size());
        if (n > 0)
            choose_option(app.focus, std::clamp(selected + (key == VK_RIGHT ? 1 : -1), 0, n - 1));
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
    case kGameTab: {  // Left and Right pick the game, Home and End the first and last (shown games only)
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        const auto next_shown = [&](int from, int step) {
            for (int i = from; i >= 0 && i < static_cast<int>(count); i += step)
                if (game_shown(*all[i]))
                    return i;
            return -1;
        };
        int to = -1;
        const bool next = key == VK_RIGHT || key == VK_DOWN;  // the rail is a column; Left and Right still work
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)
            to = next_shown(game_index() + (next ? 1 : -1), next ? 1 : -1);
        else if (key == VK_HOME || key == VK_END)
            to = key == VK_HOME ? next_shown(0, 1) : next_shown(static_cast<int>(count) - 1, -1);
        if (to >= 0)
            select_game(to);
        break;
    }
    case kPage: {  // the strip of the tab's pages: Left and Right (Up and Down too) the previous or next page
        const std::vector<Pane> pages = tab_pages(tab_of(app.pane));
        const auto at = std::find(pages.begin(), pages.end(), app.pane);
        if (at == pages.end() || pages.empty())
            break;
        const int i = static_cast<int>(at - pages.begin()), n = static_cast<int>(pages.size());
        if (key == VK_LEFT || key == VK_UP)
            show_page(pages[static_cast<size_t>((i + n - 1) % n)]);
        else if (key == VK_RIGHT || key == VK_DOWN)
            show_page(pages[static_cast<size_t>((i + 1) % n)]);
        else if (key == VK_HOME || key == VK_END)
            show_page(key == VK_HOME ? pages.front() : pages.back());
        break;
    }
    case kTopTab: {  // the tabs: Left and Right, Home and End
        const int i = static_cast<int>(tab_of(app.pane));
        if (key == VK_LEFT || key == VK_RIGHT)
            show_tab(static_cast<Tab>((i + (key == VK_RIGHT ? 1 : tab_count() - 1)) % tab_count()));
        else if (key == VK_HOME || key == VK_END)
            show_tab(key == VK_HOME ? Tab::Overview : static_cast<Tab>(tab_count() - 1));
        break;
    }
    case kMenuPanel:
    case kOsdPanel:
    case kClockPanel: {  // arrows move the panel a degree, Page Up / Page Down ten
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
    default:
        if (key == VK_SPACE && (push_button(app.focus) || space_control(app.focus)))
            activate(app.focus, app.focusOption);
        break;
    }
}

}  // namespace simqvfr::launcher::ui::win

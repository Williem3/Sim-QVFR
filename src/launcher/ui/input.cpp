// Acting on a control (activate), from a click or the keyboard (push_button, space_control, on_key).
#include <algorithm>
#include <string>

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include "../launch_plan.h"
#include "../game_tabs.h"
#include "../settings.h"
#include "../../engine/nvapi_gpus.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

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
        if (app.pane == Pane::Profiles) {
            app.profileMessage.clear();
            refresh_profiles();
        }
        if (app.pane == Pane::Diagnostics || app.pane == Pane::Report)
            reload_runtime();
        break;
    case kSheetButton: close_sheet(option); return;
    case kChecksBadge: activate(kSidebar, static_cast<int>(Pane::Checks)); return;
    case kUndoBadge: undo_restore(); return;
    case kLaunchBadge: {
        const LaunchOutcome o = app.watch.outcome();
        if (o.link == LaunchLink::Diagnostics) {
            activate(kSidebar, static_cast<int>(Pane::Diagnostics));
        } else if (o.link == LaunchLink::Report) {
            if (o.state == LaunchState::Crashed)
                app.reportKind = 1;  // kIssueKinds: Crash or freeze
            activate(kSidebar, static_cast<int>(Pane::Report));
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
                app.profileMessage = L"Back to " + name + L".";
        } else if (id == kProfileUpdate) {  // the settings now, into the profile in use: no question, it's its own button
            if (save_profile(profiles_folder(app.ini), settings_profile(app.s, name))) {
                app.profileMessage = L"Updated " + name + L" with the settings you have now.";
            } else {
                app.profileMessage = L"Couldn’t update " + name + L".";
            }
            refresh_profiles();
        } else if (id == kProfileShare && load_profile(profiles_folder(app.ini), name, p, app.s.eyeTracking)) {
            app.profileMessage = copy_text(widen(share_profile(p)))
                                     ? L"Copied " + name + L" to the clipboard: paste it in a chat or a post."
                                     : L"Couldn’t copy to the clipboard.";
        } else if (id == kProfileDelete) {
            Sheet s;
            s.title = L"Delete " + name + L"?";
            s.text = L"It goes from Your profiles. The settings you have now stay as they are.";
            s.buttons = {L"Cancel", L"Delete"};
            s.focus = 0;
            s.danger = true;
            s.done = [name](int button, const std::wstring&) {
                if (button != 1)
                    return;
                delete_profile(profiles_folder(app.ini), name);
                if (_wcsicmp(name.c_str(), app.profileCurrent.c_str()) == 0)
                    set_current_profile(app.ini.c_str(), L"");
                app.profileMessage = L"Deleted " + name + L".";
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
    case kDiagCapture: app.s.cpuProfile = !app.s.cpuProfile; changed(); return;
    case kDiagFrameCapture: app.s.frameCapture = !app.s.frameCapture; changed(); return;
    case kDiagBaseline: app.s.baselineNext = !app.s.baselineNext; changed(); return;
    case kAntiCheat:
        if (games::launcher_game().antiCheatStart) {
            app.s.antiCheat = !app.s.antiCheat;
            changed();
        }
        return;
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
    case kDiagExport: export_logs(); return;
    case kDiagZip: show_report_zip(); return;  // the keyboard's; the mouse drags it (drag_report_zip)
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
    case kGameTab: select_game(option); return;
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
    case kDlssFocusMode:
    case kDlssFocusPreset:
    case kDlssEyePreset:
    case kDlssSharpening:
    case kNrStyle:
    case kNrPreset:
    case kNrOrder:
    case kHelmetDetail: open_popup(id); return;
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
        if (own_dlss_available()) {  // greyed out without QVFR, an RTX card or multi-view rendering
            app.s.dlss.on = !app.s.dlss.on;
            changed();
        }
        return;
    case kDlssCarVectors:
        if (own_dlss_available() && app.s.dlss.on) {  // greyed out while DLSS is off
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
    case kHotkeyFooter: app.s.hotkeyFooter = !app.s.hotkeyFooter; changed(); return;
    case kUpdateCheck:
        app.s.updateCheck = !app.s.updateCheck;
        app.s.updateAsked = true;  // a choice: the first start's question is answered
        changed();
        if (app.s.updateCheck && app.update == Update::Off)
            start_update_check();
        return;
    case kUpdateNow: start_update_check(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kUpdateDownload:
        if (!app.release.url.empty() && app.release.url.rfind(L"https://github.com/", 0) == 0)
            ShellExecuteW(app.hwnd, L"open", app.release.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kUpdateBadge: activate(kSidebar, static_cast<int>(Pane::About)); return;
    case kUpdateInstall: start_install(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
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
            app.s.hiddenGames = with_game_hidden(app.s.hiddenGames, game_id(*all[option]), hide);
        else
            app.s.shownGames = with_game_hidden(app.s.shownGames, game_id(*all[option]), !hide);
        changed();
        if (hide && all[option] == &games::launcher_game())  // the game in use went: the first shown takes over
            for (std::size_t i = 0; i < count; ++i)
                if (game_shown(*all[i])) {
                    select_game(static_cast<int>(i));
                    break;
                }
        return;
    }
    case kColorGame:
        app.colorGame = option;
        app.colorLoaded = -2;
        load_color_edit();
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    case kColorReset:
        app.s.gameColors.erase(game_id(color_game()));
        app.colorLoaded = -2;
        load_color_edit();
        apply_theme();
        changed();
        return;
    case kCropOn:
        if (app.s.mode == RenderMode::Qvfr && game_has(&games::GameFeatures::fovCrop)) {
            app.s.crop = !app.s.crop;
            changed();
        }
        return;
    case kNativeQuad:  // auto (1) or off (0); a hand-set 2 (asked for behind another layer) becomes off, then auto
        if (app.s.mode == RenderMode::Qvfr && game_has(&games::GameFeatures::nativeQuadViews)) {
            app.s.nativeQuad = app.s.nativeQuad ? 0 : 1;
            changed();
        }
        return;
    case kHelmetOn:
        if (app.s.mode == RenderMode::Qvfr && game_has(&games::GameFeatures::helmet)) {
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
        const Settings before = app.s;
        app.s = restored_defaults(app.s);
        changed();
        app.undoRestore = before;  // the action bar offers Undo until the next change
        return;
    }
    case kLaunch: launch(); return;
    case kThemeToggle: cycle_theme(); return;
    case kSupport: ShellExecuteW(app.hwnd, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    default: break;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

float dip(LPARAM lp, bool y) {
    return (y ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp)) / app.canvas.dpi_scale();
}

namespace {

// A push button: Return presses it while it has the focus (as on Windows); anywhere else Return presses Launch, the
// window's default button. Space presses these too.
bool push_button(Ctl id) {
    switch (id) {
    case kCheekyRuntime:
    case kDlssFolder:
    case kDlssIncluded:
    case kNrFolder:
    case kUpdateNow:
    case kUpdateDownload:
    case kUpdateInstall:
    case kUpdateBadge:
    case kLayerApply:
    case kLayerRevert:
    case kLayerReset:
    case kLayerUp:
    case kLayerDown:
    case kSupport:
    case kRestore:
    case kThemeToggle:
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
    case kChecksBadge:
    case kUndoBadge:
    case kLaunchBadge:
    case kDiagReport:
    case kDiagZip:
    case kDiagExport:
    case kDiagCopy:
    case kDiagLogs:
    case kColorReset: return true;
    default: return false;
    }
}

// A switch or a pop-up button: Space toggles or opens it; Return, as on Windows, presses Launch.
bool space_control(Ctl id) {
    switch (id) {
    case kMethod:
    case kMode:
    case kOutline:
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
    case kHotkeyFooter:
    case kUpdateCheck:
    case kNativeQuad:
    case kCropOn:
    case kHelmetOn:
    case kVisor:
    case kShape:
    case kHelmetDetail:
    case kLayerProfile:
    case kLayerOn:
    case kDiagCapture:
    case kDiagFrameCapture:
    case kDiagBaseline:
    case kDiagKind:
    case kGameShown:
    case kAntiCheat:
    case kColorGame: return true;
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
        if (key == VK_UP || key == VK_DOWN)
            app.popup.hover = (app.popup.hover + (key == VK_DOWN ? 1 : n - 1)) % n;
        else if (key == VK_RETURN || key == VK_SPACE)
            close_popup(true);
        else if (key == VK_ESCAPE)
            close_popup(false);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == 'Z' && GetKeyState(VK_CONTROL) < 0 && app.undoRestore) {  // Ctrl+Z: Restore Defaults' Undo
        undo_restore();
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
        auto it = std::find(order.begin(), order.end(), app.focus);
        int i = it == order.end() ? (shift ? 0 : -1) : static_cast<int>(it - order.begin());
        i = (i + (shift ? static_cast<int>(order.size()) - 1 : 1)) % static_cast<int>(order.size());
        app.focus = order[i];
        app.focusOption = -1;
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
        if (key == VK_LEFT || key == VK_RIGHT)
            to = next_shown(game_index() + (key == VK_RIGHT ? 1 : -1), key == VK_RIGHT ? 1 : -1);
        else if (key == VK_HOME || key == VK_END)
            to = key == VK_HOME ? next_shown(0, 1) : next_shown(static_cast<int>(count) - 1, -1);
        if (to >= 0)
            select_game(to);
        break;
    }
    case kSidebar:
        if (key == VK_UP || key == VK_DOWN) {
            const int step = key == VK_DOWN ? 1 : -1, here = static_cast<int>(app.pane);
            activate(kSidebar, shown_pane(std::clamp(here + step, 0, kPanes - 1), step, here));
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
    default:
        if (key == VK_SPACE && (push_button(app.focus) || space_control(app.focus)))
            activate(app.focus, app.focusOption);
        break;
    }
}

}  // namespace simqvfr::launcher::ui::win

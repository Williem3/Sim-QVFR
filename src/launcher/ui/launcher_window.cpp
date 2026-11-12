// The launcher window, built natively from the QVFR launcher kit (docs/qvfr-launcher-kit): a tab per game
// across the top, a sidebar of panes in groups, cards of the kit's controls, a live preview, and an action bar
// with the status, Restore Defaults and Launch. This file has the window procedure and run_window; the state
// is in window_state.h, the panes in pane_*.cpp, the window around them in chrome.cpp, input in pointer.cpp
// and input.cpp. Settings save as they change (settings.h); Launch runs launch_game (main.cpp).
//
// Immediate mode: each paint lays out and draws the current pane and records hit rectangles; input
// acts on the last frame's rectangles and the settings, then repaints.
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include <windows.h>
#include <windowsx.h>

#include "../launcher.h"
#include "../settings.h"
#include "../session_history.h"
#include "../install.h"
#include "../../games/game.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

namespace {

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
        create_sheet_edit();
        create_search_edit();
        refresh_installed();
        if (!game_shown(games::launcher_game())) {  // e.g. ACE, the first default, where only another game is installed
            std::size_t count = 0;
            const games::GameInfo* const* all = games::all_games(count);
            for (std::size_t i = 0; i < count; ++i)
                if (game_shown(*all[i])) {
                    select_game(static_cast<int>(i));
                    break;
                }
        }
        refresh_checks();
        if (app.iniFound)
            refresh_profiles();
        // The window opens on Overview (docs/ui/OVERHAUL_PLAN.md §6.4); the top bar's status names what the launch checks
        // found (often a new day: a game update, Steam closed) and opens them. A tuning session keeps its page.
        show_tab(Tab::Overview);
        if (app.s.updateCheck)
            start_update_check();
        else if (app.iniFound && !app.s.updateAsked)
            ask_update_check();  // the first start, once
        return 0;
    case kUpdateMessage:
        update_done(reinterpret_cast<UpdateResult*>(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kInstallProgressMessage: {
        const std::unique_ptr<InstallProgress> p(reinterpret_cast<InstallProgress*>(lParam));
        app.installProgress = *p;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case kInstallDoneMessage:
        install_done(reinterpret_cast<InstallResult*>(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kHashedMessage: {
        const std::unique_ptr<std::string> sha(reinterpret_cast<std::string*>(lParam));
        app.exeSha = *sha;
        app.hashing = false;
        refresh_checks();  // also hashes again if the exe changed meanwhile
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_COMMAND:  // Find a setting's field changed: its matches follow
        if (reinterpret_cast<HWND>(lParam) == app.searchEdit && app.searchEdit && HIWORD(wParam) == EN_CHANGE) {
            search_changed();
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:  // the description box in the theme's field colours
        if (reinterpret_cast<HWND>(lParam) == app.reportEdit || reinterpret_cast<HWND>(lParam) == app.sheetEdit ||
            reinterpret_cast<HWND>(lParam) == app.searchEdit) {
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
        if (wParam == kHighlightTimer) {  // Find a setting's highlight ends
            KillTimer(hwnd, kHighlightTimer);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (wParam == kFollowTimer) {
            follow_ini();
            if (app.watch.poll()) {  // the launched game's status changed: the checks follow it (running, starting)
                refresh_checks();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // Launch checks: Steam, ACE, the layers and the game folder change outside the launcher.
            static int ticks = 0;
            if (++ticks % 4 == 0) {
                refresh_checks();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // A running session: the layer rewrites runtime.ini every ~10 s. Diagnostics and Report an issue follow it,
            // and the session history keeps its numbers whichever pane shows.
            static FILETIME seen{};
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW(runtime_info_path().c_str(), GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &seen) != 0) {
                seen = a.ftLastWriteTime;
                reload_runtime();
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
            reload_runtime();
            refresh_installed();  // a game installed or removed meanwhile
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
        if (app.searchOpen && !app.sheet.open) {  // a result, the field, or anywhere else: closed
            if (h && (h->id == kSearchResult || h->id == kSearchField))
                activate(h->id, h->option);
            else
                close_search();
            return 0;
        }
        if (app.sheet.open) {  // only its buttons (pressed until the button is let go) and its field
            if (h && h->id == kSheetButton) {
                app.pressed = kSheetButton;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (h && h->id == kSheetField) {
                SetFocus(app.sheetEdit);
            }
            return 0;
        }
        if (!h || h->id == kNone)  // nothing there, or something with a tip only
            return 0;
        app.focus = h->id;  // as Windows does: Return now presses it, if it's a button
        app.focusOption = h->option;
        if (slider_value(h->id)) {
            app.dragging = h->id;
            app.dragTrack = h->r;
            app.dragRange = slider_range(h->id);
            SetCapture(hwnd);
            set_slider_from_x(x);
        } else if (h->id == kColorWheel) {
            app.dragging = kColorWheel;
            SetCapture(hwnd);
            set_color_from(x, y);
        } else if (h->id == kMenuPanel || h->id == kOsdPanel) {
            app.dragging = h->id;
            const float k = app.placeFrame.w / kRefFovX;
            const auto [yawOf, pitchOf] = placement_of(h->id);
            // Held on the panel: that point stays under the pointer; on empty space, the centre jumps there.
            app.grabYaw = h->option == 1 ? *yawOf - ((x - app.placeFrame.x) / k - kRefFovX / 2) : 0;
            app.grabPitch = h->option == 1 ? *pitchOf - (kRefFovY / 2 - (y - app.placeFrame.y) / k) : 0;
            SetCapture(hwnd);
            set_panel_from(x, y);
        } else if (h->id == kDiagZip) {
            drag_report_zip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
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
        app.mouseX = x;
        app.mouseY = y;
        if (app.dragging == kMenuPanel || app.dragging == kOsdPanel)
            set_panel_from(x, y);
        else if (app.dragging == kColorWheel)
            set_color_from(x, y);
        else if (app.dragging != kNone)
            set_slider_from_x(x);
        else if (app.popup.open) {
            const int hover = popup_item_at(x, y);
            if (hover != app.popup.hover) {
                app.popup.hover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else {
            const Hit* h = hit_at(x, y);
            const int tip = !h ? -1 : h->id == kHelpIcon ? h->option : !h->tip.empty() ? kTipText : -1;
            const Ctl hover = h ? h->id : kNone;
            const int hoverOption = h ? h->option : -1;
            const bool newText = tip == kTipText && h->tip != app.tipText;
            if (tip == kTipText) {  // a hit's own tip: its text, under it
                app.tipText = h->tip;
                app.tipAnchor = h->r;
            }
            if (tip != app.tip || newText || hover != app.hover || hoverOption != app.hoverOption) {  // hover states follow
                app.tip = tip;
                app.hover = hover;
                app.hoverOption = hoverOption;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (hover != kNone) {  // a WM_MOUSELEAVE when the pointer leaves the window, to clear them
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        app.mouseX = app.mouseY = -1;
        if (app.tip >= 0 || app.hover != kNone) {
            app.tip = -1;
            app.hover = kNone;
            app.hoverOption = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
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
                activate(pressed, h->option);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (app.sheet.open)
            return 0;
        app.tip = -1;  // its ⓘ moves away
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

}  // namespace simqvfr::launcher::ui::win

namespace simqvfr::launcher {

int run_window(HINSTANCE instance, int show) {
    using namespace ui::win;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);  // dragging the report's zip out (SHDoDragDrop)
    app.ini = settings_path();
    app.iniFound = GetFileAttributesW(app.ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    app.themeChoice = read_theme_choice();  // before the window's first apply_theme
    clean_update_folder(std::filesystem::path(app.ini).parent_path().wstring());  // what an install left but its backup
    if (app.iniFound) {
        // Per-game settings: a game without its own copy yet (the first start after the update, or a new game)
        // starts from the settings in use, so updating changes nothing until a game's settings are changed.
        seed_game_settings(app.ini);
        app.s = load_settings(app.ini);
        app.sessions = load_sessions(sessions_folder(app.ini));
    }
    reload_runtime();
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
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"Sim-QVFR Launcher", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
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

}  // namespace simqvfr::launcher

// Sheets (open_sheet, notify): a question or notice over the dimmed window, its keys, and its name field.
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include <windows.h>

#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

// ---- Sheets ------------------------------------------------------------------------------------

void open_sheet(Sheet sheet) {
    app.popup.open = false;
    app.tip = -1;
    app.pressed = kNone;
    sheet.open = true;
    if (sheet.buttons.empty())
        sheet.buttons = {L"OK"};
    const int main = static_cast<int>(sheet.buttons.size()) - 1;
    sheet.cancel = std::clamp(sheet.cancel, 0, main);
    if (sheet.focus < 0 || sheet.focus > main)
        sheet.focus = sheet.field ? -1 : main;
    app.sheet = std::move(sheet);
    if (app.sheet.field && app.sheetEdit) {
        SetWindowTextW(app.sheetEdit, app.sheet.fieldText.c_str());
        SendMessageW(app.sheetEdit, EM_SETSEL, 0, -1);
        app.sheetFocusField = app.sheet.focus < 0;  // once the paint has placed it
    }
    if (GetFocus() != app.hwnd)
        SetFocus(app.hwnd);  // out of the report's description box
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void notify(const std::wstring& title, const std::wstring& text, std::function<void()> then) {
    Sheet s;
    s.title = title;
    s.text = text;
    if (then)
        s.done = [then](int, const std::wstring&) { then(); };
    open_sheet(std::move(s));
}

// Closes the sheet with `button` pressed, then tells its `done` (which may open another).
void close_sheet(int button) {
    if (!app.sheet.open)
        return;
    std::wstring text;
    if (app.sheet.field && app.sheetEdit) {
        text.resize(static_cast<size_t>(std::max(0, GetWindowTextLengthW(app.sheetEdit))));
        GetWindowTextW(app.sheetEdit, text.data(), static_cast<int>(text.size()) + 1);
    }
    const auto done = std::move(app.sheet.done);
    app.sheet = {};
    app.sheetFocusField = false;
    if (app.sheetEdit) {
        if (GetFocus() == app.sheetEdit)
            SetFocus(app.hwnd);
        ShowWindow(app.sheetEdit, SW_HIDE);
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
    if (done)
        done(button, text);
}

namespace {

// Tab and Shift+Tab: the focus to the next or previous of the name field (-1, with one) and the buttons.
void move_sheet_focus(int step) {
    Sheet& s = app.sheet;
    const int n = static_cast<int>(s.buttons.size()), first = s.field ? -1 : 0;
    int f = s.focus + step;
    if (f >= n)
        f = first;
    if (f < first)
        f = n - 1;
    s.focus = f;
    app.keyboard = true;
    if (f < 0)
        SetFocus(app.sheetEdit);
    else if (GetFocus() == app.sheetEdit)
        SetFocus(app.hwnd);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

}  // namespace

// The window's keys while a sheet shows (its name field has its own: sheet_edit_proc).
void sheet_key(WPARAM key) {
    Sheet& s = app.sheet;
    if (key == VK_ESCAPE)
        close_sheet(s.cancel);
    else if ((key == VK_RETURN || key == VK_SPACE) && s.focus >= 0)
        close_sheet(s.focus);
    else if (key == VK_TAB)
        move_sheet_focus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
    else if ((key == VK_LEFT || key == VK_RIGHT) && s.focus >= 0) {  // along the buttons only
        s.focus = std::clamp(s.focus + (key == VK_RIGHT ? 1 : -1), 0, static_cast<int>(s.buttons.size()) - 1);
        app.keyboard = true;
        InvalidateRect(app.hwnd, nullptr, FALSE);
    }
}

// The sheet over the window: the window dimmed, a card in its middle with the title, the text, the field and the
// buttons (trailing, the main one last). Modal: the hits under it go.
void sheet_card() {
    Sheet& s = app.sheet;
    Canvas& cv = app.canvas;
    const float W = cv.width(), H = cv.height();
    cv.fill({0, 0, W, H}, {0, app.dark ? 0.55f : 0.32f});
    app.hits.clear();
    app.drawingSheet = true;
    const float w = std::min(460.0f, W - 32), pad = t::kSpace6, inner = w - 2 * pad;
    const float textH = s.text.empty() ? 0 : cv.measure_height(s.text, t::kBody, inner);
    constexpr float kFieldH = 32;
    const float labelH = s.fieldLabel.empty() ? 0 : t::kCallout.lineHeight + t::kSpace2;
    const float h = pad + t::kTitle3.lineHeight + (textH > 0 ? t::kSpace2 + textH : 0) + (s.field ? t::kSpace4 + labelH + kFieldH : 0) +
                    t::kSpace6 + t::kControlHeight + pad;
    const float radius = t::kRadiusGroup;
    const Rect card{std::round((W - w) / 2), std::round(std::max(16.0f, (H - h) / 2 - 24)), w, h};
    cv.fill(card, col(app.c->group_bg), radius);  // the panes' cards: their buttons read on it
    cv.stroke(card, col(app.dark ? app.c->control_hover : app.c->card_border), 1, radius);
    float y = card.y + pad;
    cv.text(s.title, t::kTitle3, {card.x + pad, y, inner, t::kTitle3.lineHeight}, col(app.c->label));
    y += t::kTitle3.lineHeight;
    if (textH > 0) {
        cv.text(s.text, t::kBody, {card.x + pad, y + t::kSpace2, inner, textH + 2}, col(app.c->label_secondary), Align::Leading,
                false, true);
        y += t::kSpace2 + textH;
    }
    if (s.field) {
        y += t::kSpace4;
        if (labelH > 0) {
            cv.text(s.fieldLabel, t::kCallout, {card.x + pad, y, inner, t::kCallout.lineHeight}, col(app.c->label));
            y += labelH;
        }
        const Rect field{card.x + pad, y, inner, kFieldH};
        cv.fill(field, col(app.c->field_bg), t::kRadiusControl);
        cv.stroke(field, col(app.c->control_border), 1, t::kRadiusControl);
        if (s.focus < 0)
            focus_ring(field, t::kRadiusControl);
        app.sheetEditRect = {field.x + 8, field.y + (kFieldH - 20) / 2, field.w - 16, 20};
        app.hits.push_back({kSheetField, field});
        y += kFieldH;
    }
    y += t::kSpace6;
    float bx = card.right() - pad;
    const int main = static_cast<int>(s.buttons.size()) - 1;
    for (int i = main; i >= 0; --i) {
        const float bw = std::max(88.0f, button_width(s.buttons[i]));
        bx -= bw;
        const Rect r{bx, y, bw, t::kControlHeight};
        button(kSheetButton, r, s.buttons[i], i == main ? (s.danger ? Btn::Danger : Btn::Primary) : Btn::Secondary);
        app.hits.back().option = i;
        if (s.focus == i)  // Return presses it: shown as a dialog's default button is
            focus_ring(r, t::kRadiusControl);
        bx -= t::kSpace2;
    }
    app.drawingSheet = false;
}

// The sheet's name field over the field sheet_card drew; hidden without one.
void place_sheet_edit() {
    if (!app.sheetEdit)
        return;
    const Rect& r = app.sheetEditRect;
    if (!app.sheet.open || !app.sheet.field || r.w <= 0) {
        if (IsWindowVisible(app.sheetEdit))
            ShowWindow(app.sheetEdit, SW_HIDE);
        return;
    }
    edit_font();
    const float k = app.canvas.dpi_scale();
    SetWindowPos(app.sheetEdit, HWND_TOP, static_cast<int>(std::lround(r.x * k)), static_cast<int>(std::lround(r.y * k)),
                 static_cast<int>(std::lround(r.w * k)), static_cast<int>(std::lround(r.h * k)), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (app.reportBrushRgb != app.c->field_bg)
        InvalidateRect(app.sheetEdit, nullptr, TRUE);
    if (app.sheetFocusField) {
        app.sheetFocusField = false;
        SetFocus(app.sheetEdit);
    }
}

namespace {

// The name field's keys: Return presses the main button, Escape the cancel one, Tab moves on to the buttons.
LRESULT CALLBACK sheet_edit_proc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam) {
    static const WNDPROC base = reinterpret_cast<WNDPROC>(GetPropW(edit, L"simqvfr.base"));
    if (message == WM_KEYDOWN && app.sheet.open) {
        if (wParam == VK_RETURN) {
            close_sheet(static_cast<int>(app.sheet.buttons.size()) - 1);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            close_sheet(app.sheet.cancel);
            return 0;
        }
        if (wParam == VK_TAB) {
            move_sheet_focus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            return 0;
        }
    }
    if (message == WM_CHAR && (wParam == L'\r' || wParam == L'\t' || wParam == 27))  // handled as keys: no beep
        return 0;
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        if (message == WM_SETFOCUS && app.sheet.open)
            app.sheet.focus = -1;
        InvalidateRect(app.hwnd, nullptr, FALSE);  // the field's focus ring
    }
    return CallWindowProcW(base, edit, message, wParam, lParam);
}

}  // namespace

void create_sheet_edit() {
    app.sheetEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, app.hwnd, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    if (!app.sheetEdit)
        return;
    SendMessageW(app.sheetEdit, EM_SETLIMITTEXT, 40, 0);  // a profile's name (valid_profile_name)
    SetPropW(app.sheetEdit, L"simqvfr.base", reinterpret_cast<HANDLE>(GetWindowLongPtrW(app.sheetEdit, GWLP_WNDPROC)));
    SetWindowLongPtrW(app.sheetEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(sheet_edit_proc));
}

}  // namespace simqvfr::launcher::ui::win

// Find a setting (Ctrl+K, the top bar's magnifier): a field and the settings whose name (or section, or help) matches,
// taken from what every page draws, so the list can't drift from the pages; choosing one shows its page, scrolls to it
// and highlights its row.
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#include <windows.h>
#include <commctrl.h>

#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace {

constexpr float kFieldH = 40, kResultH = 48;
constexpr size_t kMaxResults = 8;

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring field_text() {
    std::wstring text(static_cast<size_t>(std::max(0, GetWindowTextLengthW(app.searchEdit))), L'\0');
    if (!text.empty())
        GetWindowTextW(app.searchEdit, text.data(), static_cast<int>(text.size()) + 1);
    return text;
}

// The field's text against the index: names starting with it first, then names, sections and help containing it.
void match() {
    app.searchResults.clear();
    app.searchSelected = 0;
    std::wstring q = lower(field_text());
    q.erase(0, q.find_first_not_of(L' '));
    q.erase(q.find_last_not_of(L' ') + 1);
    if (q.empty())
        return;
    std::vector<std::pair<int, int>> scored;  // score, index
    for (size_t i = 0; i < app.searchIndex.size(); ++i) {
        const SearchEntry& e = app.searchIndex[i];
        const std::wstring label = lower(e.label);
        const wchar_t* help = help_for(e.label);
        int score = -1;
        if (label.rfind(q, 0) == 0)
            score = 0;
        else if (label.find(q) != std::wstring::npos)
            score = 1;
        else if (lower(e.section).find(q) != std::wstring::npos)
            score = 2;
        else if (help && lower(help).find(q) != std::wstring::npos)
            score = 3;
        if (score >= 0)
            scored.push_back({score, static_cast<int>(i)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < scored.size() && i < kMaxResults; ++i)
        app.searchResults.push_back(scored[i].second);
}

// Where a result is: "Picture › Resolution · Field of view crop".
std::wstring where(const SearchEntry& e) {
    std::wstring s = std::wstring(tab_label(tab_of(e.pane)));
    if (e.panel != PanelKind::None)
        s += L" › " + panel_title(e.panel);
    else if (tab_pages(tab_of(e.pane)).size() > 1)
        s += std::wstring(L" › ") + pane_label(static_cast<int>(e.pane));
    if (!e.section.empty() && e.section != e.label)
        s += L" › " + e.section;
    if (!e.hiddenBy.empty())
        s += L"  ·  shows with " + e.hiddenBy;
    return s;
}

}  // namespace

void open_search() {
    app.popup.open = false;
    app.searchOpen = true;
    app.searchIndexed = false;  // the pages are indexed again at the next paint: the game or its features may have changed
    app.searchResults.clear();
    app.searchSelected = 0;
    if (app.searchEdit) {
        SetWindowTextW(app.searchEdit, L"");
        app.searchFocusField = true;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void close_search() {
    if (!app.searchOpen)
        return;
    app.searchOpen = false;
    if (app.searchEdit) {
        if (GetFocus() == app.searchEdit)
            SetFocus(app.hwnd);
        ShowWindow(app.searchEdit, SW_HIDE);
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The field's text changed (EN_CHANGE).
void search_changed() {
    match();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// Every page's labels (form.cpp and controls.cpp record them as they are drawn), each page drawn once where nothing
// shows. paint() calls it, then starts its frame again.
void search_index_pages(float x, float w) {
    const Pane shown = app.pane;
    const float scroll = app.scroll;
    app.searchIndex.clear();
    app.canvas.push_clip({0, 0, 0, 0});
    for (int t = 0; t < kTabs; ++t)
        for (Pane p : tab_pages(static_cast<Tab>(t))) {
            app.pane = p;
            app.drawnLabels.clear();
            app.searchIndex.push_back({pane_label(static_cast<int>(p)), L"", p});  // the page itself
            pane_content(x, -100000, w, true);
            for (const DrawnLabel& d : app.drawnLabels) {
                const bool known = std::any_of(app.searchIndex.begin(), app.searchIndex.end(), [&](const SearchEntry& e) {
                    return e.pane == p && e.label == d.label && e.section == d.section;
                });
                if (!d.label.empty() && !known)
                    app.searchIndex.push_back({d.label, d.section, p, d.hiddenBy, d.more});
            }
        }
    // Manage profiles and About: their labels open them (Overview's panels).
    for (const PanelKind kind : {PanelKind::Profiles, PanelKind::About}) {
        app.drawnLabels.clear();
        app.drawingSection = panel_title(kind);
        app.searchIndex.push_back({panel_title(kind), L"", Pane::Overview, L"", L"", kind});
        panel_content(kind, x, -100000, std::min(w, 560.0f), true);
        for (const DrawnLabel& d : app.drawnLabels)
            if (!d.label.empty())
                app.searchIndex.push_back({d.label, d.section, Pane::Overview, d.hiddenBy, d.more, kind});
    }
    app.drawingSection.clear();
    app.canvas.pop_clip();
    app.pane = shown;
    app.scroll = scroll;
    app.searchIndexed = true;
    match();
}

// The search card under the magnifier: the field, then the matches (the one Return picks in blue), or a hint.
void search_card() {
    Canvas& cv = app.canvas;
    Rect anchor{};
    for (const Hit& h : app.hits)
        if (h.id == kSearch)
            anchor = h.r;
    const float W = cv.width(), w = std::min(460.0f, W - rail_width() - 32);
    const size_t n = app.searchResults.size();
    const bool hint = n == 0;
    const float h = 8 + kFieldH + 8 + (hint ? 44 : n * kResultH) + 8;
    Rect card{std::max(rail_width() + 16, anchor.right() - w), anchor.bottom() + 8, w, h};
    app.hits.push_back({kSearchField, card});  // a click on the card but no result keeps it open
    cv.fill(card, col(app.c->surface), t::kRadiusGroup);
    cv.stroke(card, col(app.c->field_border), 1, t::kRadiusGroup);
    // The field: the magnifier, then the native edit.
    const Rect field{card.x + 8, card.y + 8, card.w - 16, kFieldH};
    cv.fill(field, col(app.c->field_bg), t::kRadiusControl);  // the native edit paints field_bg (WM_CTLCOLOREDIT)
    cv.stroke(field, col(app.c->field_border), 1, t::kRadiusControl);
    const Color mark = col(app.c->label_secondary);
    cv.circle(field.x + 18, field.y + kFieldH / 2 - 1, 5, mark, false, 1.6f);
    cv.line(field.x + 21.7f, field.y + kFieldH / 2 + 2.7f, field.x + 25.5f, field.y + kFieldH / 2 + 6.5f, mark, 1.6f);
    app.searchEditRect = {field.x + 36, field.y + (kFieldH - 20) / 2, field.w - 44, 20};
    app.hits.push_back({kSearchField, field});
    float y = field.bottom() + 8;
    if (hint) {
        const std::wstring text = field_text().empty() ? L"Type a setting’s name: Feather, Crop, Hotkeys, DLSS…"
                                                       : L"No setting matches “" + field_text() + L"”.";
        cv.text(text, t::kCallout, {card.x + 16, y, card.w - 32, 44}, col(app.c->label_secondary));
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const SearchEntry& e = app.searchIndex[static_cast<size_t>(app.searchResults[i])];
        const Rect r{card.x + 6, y, card.w - 12, kResultH};
        const bool selected = static_cast<int>(i) == app.searchSelected;
        if (selected)
            cv.fill(r, col(app.c->accent_fill, app.dark ? 0.28f : 0.12f), t::kRadiusControl);
        else if (pointer_in(r))
            cv.fill(r, col(app.c->control_hover), t::kRadiusControl);
        cv.text(e.label, t::kBodyMedium, {r.x + 12, r.y + 5, r.w - 24, 20}, col(app.c->label));
        cv.text(where(e), t::kCallout, {r.x + 12, r.y + 25, r.w - 24, 17}, col(app.c->label_secondary));
        app.hits.push_back({kSearchResult, r, static_cast<int>(i)});
        y += kResultH;
    }
}

// A result chosen: its page, scrolled to its row, the row highlighted for a moment. Values change nothing.
void jump_to(const SearchEntry& entry) {
    close_search();
    show_page(entry.pane);
    if (entry.panel != PanelKind::None) {  // in a panel: it opens
        open_panel(entry.panel);
        return;
    }
    if (!entry.more.empty())  // inside a closed More: opened
        app.moreOpen.insert(entry.more);
    app.highlight = {entry.label, entry.section, {}};
    app.highlightUntil = GetTickCount64() + kHighlightMs;
    app.highlightScroll = true;
    SetTimer(app.hwnd, kHighlightTimer, kHighlightMs + 50, nullptr);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The highlighted row (inside the page's clip): scrolled into view on the first frame it is found, then outlined.
void draw_highlight() {
    if (app.highlightUntil == 0 || GetTickCount64() >= app.highlightUntil)
        return;
    const DrawnLabel* found = nullptr;
    for (const DrawnLabel& d : app.drawnLabels)
        if (d.label == app.highlight.label && (d.section == app.highlight.section || !found))
            found = &d;
    if (!found)
        return;
    const Rect r = found->r;
    const Rect& area = app.scrollArea;
    if (app.highlightScroll) {
        app.highlightScroll = false;
        if (r.y < area.y + 8 || r.bottom() > area.bottom() - 8) {
            app.scroll = std::clamp(app.scroll + r.y - (area.y + 72), 0.0f, app.scrollMax);
            InvalidateRect(app.hwnd, nullptr, FALSE);
        }
    }
    const Rect ring{r.x - 6, r.y - 2, r.w + 12, r.h + 4};
    app.canvas.fill(ring, col(app.c->accent_fill, app.dark ? 0.22f : 0.1f), t::kRadiusControl);
    app.canvas.stroke(ring, col(app.c->focus_ring), 2, t::kRadiusControl);
}

void place_search_edit() {
    if (!app.searchEdit)
        return;
    const Rect& r = app.searchEditRect;
    if (!app.searchOpen || r.w <= 0 || app.sheet.open || app.popup.open) {
        if (IsWindowVisible(app.searchEdit))
            ShowWindow(app.searchEdit, SW_HIDE);
        return;
    }
    edit_font();
    const float k = app.canvas.dpi_scale();
    SetWindowPos(app.searchEdit, HWND_TOP, static_cast<int>(std::lround(r.x * k)), static_cast<int>(std::lround(r.y * k)),
                 static_cast<int>(std::lround(r.w * k)), static_cast<int>(std::lround(r.h * k)), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (app.reportBrushRgb != app.c->field_bg)
        InvalidateRect(app.searchEdit, nullptr, TRUE);
    if (app.searchFocusField) {
        app.searchFocusField = false;
        SetFocus(app.searchEdit);
    }
}

namespace {

// The field's keys: Up and Down pick a result, Return goes to it, Escape (or Tab) closes the search.
LRESULT CALLBACK search_edit_proc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam) {
    static const WNDPROC base = reinterpret_cast<WNDPROC>(GetPropW(edit, L"simqvfr.base"));
    if (message == WM_KEYDOWN && app.searchOpen) {
        const int n = static_cast<int>(app.searchResults.size());
        if ((wParam == VK_DOWN || wParam == VK_UP) && n > 0) {
            app.searchSelected = (app.searchSelected + (wParam == VK_DOWN ? 1 : n - 1)) % n;
            InvalidateRect(app.hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (n > 0)
                jump_to(app.searchIndex[static_cast<size_t>(app.searchResults[static_cast<size_t>(app.searchSelected)])]);
            return 0;
        }
        if (wParam == VK_ESCAPE || wParam == VK_TAB) {
            close_search();
            return 0;
        }
    }
    if (message == WM_CHAR && (wParam == L'\r' || wParam == L'\t' || wParam == 27))  // handled as keys: no beep
        return 0;
    return CallWindowProcW(base, edit, message, wParam, lParam);
}

}  // namespace

void create_search_edit() {
    app.searchEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, app.hwnd, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!app.searchEdit)
        return;
    SendMessageW(app.searchEdit, EM_SETLIMITTEXT, 60, 0);
    SendMessageW(app.searchEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find a setting"));
    SetPropW(app.searchEdit, L"simqvfr.base", reinterpret_cast<HANDLE>(GetWindowLongPtrW(app.searchEdit, GWLP_WNDPROC)));
    SetWindowLongPtrW(app.searchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(search_edit_proc));
}

}  // namespace simqvfr::launcher::ui::win

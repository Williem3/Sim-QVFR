// The page frame: each page's header, the pop-up menu, the native edits' font, and paint, which lays out and draws the
// whole window each frame (the rail, the top bar and the page strip are shell.cpp's).
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include <windows.h>

#include "../preflight.h"
#include "../settings.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

// The current page's content at (x, y), width w. Returns its height.
float pane_content(float x, float y, float w, bool draw) {
    switch (app.pane) {
    case Pane::Checks: return checks_pane(x, y, w, draw);
    case Pane::Profiles: return profiles_pane(x, y, w, draw);
    case Pane::Picture: return picture_page(x, y, w, draw);
    case Pane::Headset: return headset_page(x, y, w, draw);
    case Pane::Games: return games_pane(x, y, w, draw);
    case Pane::Layers: return layers_pane(x, y, w, draw);
    case Pane::Diagnostics: return diagnostics_pane(x, y, w, draw);
    case Pane::Report: return report_pane(x, y, w, draw);
    case Pane::About: return about_pane(x, y, w, draw);
    }
    return 0;
}

namespace {

constexpr const wchar_t* kPaneLabels[] = {L"Launch checks", L"Profiles",    L"Picture",         L"Headset", L"Games",
                                          L"OpenXR layers", L"Diagnostics", L"Report an issue", L"About"};

static_assert(std::size(kPaneLabels) == kPanes, "a name for every pane");

}  // namespace

// A page this game has: every one (a game's own features are cards and rows the Picture tab leaves out).
bool pane_shown(int) { return true; }

// A page's name in the page strip and as its title.
const wchar_t* pane_label(int pane) { return kPaneLabels[std::clamp(pane, 0, kPanes - 1)]; }

// The shown pane nearest `pane` in `step`'s direction (-1 or 1), or `pane` itself if shown; `from` if none.
int shown_pane(int pane, int step, int from) {
    for (int p = pane; p >= 0 && p < kPanes; p += step)
        if (pane_shown(p))
            return p;
    return from;
}

namespace {

// What a pane is for, under its title.
std::wstring pane_description(Pane pane) {
    switch (pane) {
    case Pane::Checks: return L"What would stop Sim-QVFR at the next launch, each with its fix.";
    case Pane::Profiles: return L"A profile saves the whole picture: resolution, foveation, crop, overlay and DLSS.";
    case Pane::Picture: return L"";
    case Pane::Headset: return L"";
    case Pane::Games: return L"Which games have a tab, and each game’s colour.";
    case Pane::Layers: return L"The OpenXR layers " + game_name() + L" loads, and their order.";
    case Pane::Diagnostics: return L"The last session’s numbers against the ones before it, and what to share when you ask for help.";
    case Pane::Report: return L"Zips the logs and opens a GitHub issue with the details filled in.";
    case Pane::About: return L"The version, updates and support.";
    }
    return L"";
}

}  // namespace

// A popup row's height: an item, a separator, or a note.
float popup_row_height(size_t i) {
    const Popup& p = app.popup;
    const PopupRow kind = i < p.kinds.size() ? p.kinds[i] : PopupRow::Item;
    return kind == PopupRow::Separator ? 9.0f : kind == PopupRow::Note ? 28.0f : kMenuItemH;
}

// The open pop-up menu: under its button, as wide as the button or its longest row (check mark column included),
// right-aligned with the button when it would leave the window.
Rect popup_menu_rect() {
    const Popup& p = app.popup;
    float w = p.anchor.w, h = 12;
    for (size_t i = 0; i < p.items.size(); ++i) {
        w = std::max(w, std::ceil(app.canvas.measure(p.items[i], t::kBody)) + 28 + 6 + 12 + 8);
        h += popup_row_height(i);
    }
    Rect menu{p.anchor.x, p.anchor.y + p.anchor.h + 4, w, h};
    if (menu.right() > app.canvas.width() - 8)
        menu.x = p.anchor.right() - w;
    if (menu.bottom() > app.canvas.height())
        menu.y = p.anchor.y - menu.h - 4;
    return menu;
}

// The native edits' font (Segoe UI 13 at the window's DPI), made again when the DPI changes: the report's description
// box, the sheet's name field and Find a setting's field.
void edit_font() {
    const UINT dpi = static_cast<UINT>(std::lround(96 * app.canvas.dpi_scale()));
    if (app.reportFont && app.reportFontDpi == dpi)
        return;
    HFONT font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND edit : {app.reportEdit, app.sheetEdit, app.searchEdit})
        if (edit)
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (app.reportFont)
        DeleteObject(app.reportFont);
    app.reportFont = font;
    app.reportFontDpi = dpi;
}

namespace {

// The page's header: its tab, its title and what it is for. Returns its height.
float pane_header(float x, float y, float w, bool draw) {
    const std::wstring desc = pane_description(app.pane);
    const float dh = app.canvas.measure_height(desc, t::kBody, w);
    if (draw) {
        app.canvas.text(tab_label(tab_of(app.pane)), t::kFootnote, {x, y, w, 16}, col(app.c->label_muted));
        app.canvas.text(pane_label(static_cast<int>(app.pane)), t::kLargeTitle, {x, y + 20, w, t::kLargeTitle.lineHeight},
                        col(app.c->label));
        app.canvas.text(desc, t::kBody, {x, y + 20 + t::kLargeTitle.lineHeight + 4, w, dh + 2}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    }
    return 20 + t::kLargeTitle.lineHeight + 4 + dh;
}

// The open pop-up menu, over everything: its items (the chosen one checked, the one under the pointer or the arrows in
// blue), separators and notes.
void popup_menu() {
    Canvas& cv = app.canvas;
    const Popup& p = app.popup;
    const Rect menu = popup_menu_rect();
    cv.fill(menu, col(app.c->surface), t::kRadiusGroup);
    cv.stroke(menu, col(app.c->field_border), 1, t::kRadiusGroup);
    float y = menu.y + 6;
    for (size_t i = 0; i < p.items.size(); ++i) {
        const float h = popup_row_height(i);
        const PopupRow kind = i < p.kinds.size() ? p.kinds[i] : PopupRow::Item;
        const Rect item{menu.x + 6, y, menu.w - 12, h};
        if (kind == PopupRow::Separator) {
            cv.line(item.x + 6, item.y + h / 2, item.right() - 6, item.y + h / 2, col(app.c->line), 1);
        } else if (kind == PopupRow::Note) {
            cv.text(p.items[i], t::kCallout, {item.x + 12, item.y, item.w - 18, h}, col(app.c->label_secondary));
        } else {
            const bool hot = static_cast<int>(i) == p.hover;
            if (hot)
                cv.fill(item, col(app.c->accent_fill), t::kRadiusControl);
            if (static_cast<int>(i) == p.selected)
                cv.path("M3.5 8.2l2.6 2.6L12.5 4.5", {item.x + 8, item.y + (h - 14) / 2, 14, 14}, 16,
                        col(hot ? app.c->on_accent : app.c->accent), 1.8f);
            cv.text(p.items[i], t::kBody, {item.x + 28, item.y, item.w - 34, h}, col(hot ? app.c->on_accent : app.c->label));
        }
        y += h;
    }
}

}  // namespace

void paint() {
    if (!app.canvas.begin())
        return;
    app.hits.clear();
    app.drawnLabels.clear();
    app.reportEditRect = {};
    app.sheetEditRect = {};
    app.searchEditRect = {};
    Canvas& cv = app.canvas;
    const float W = cv.width(), Hh = cv.height();
    const float railW = rail_width();
    cv.fill({0, 0, W, Hh}, col(app.c->bg));

    // Main and preview columns, right of the rail and under the top bar and the page strip; narrow, the preview drops
    // under the page.
    // A page of cards (Picture, Headset) takes the whole width, with no header and no preview column (its Preview is a card).
    const float pad = t::kSpace7;
    const bool cards = app.pane == Pane::Picture || app.pane == Pane::Headset;
    const float stripH = tab_pages(tab_of(app.pane)).size() > 1 ? 44.0f : 0.0f;
    const float mainX = railW, bodyTop = t::kTopbarHeight + stripH, bodyH = Hh - bodyTop;
    const bool narrow = cards || W - railW < 780;
    const float mainW = narrow ? W - mainX : W - mainX - t::kPreviewWidth;
    const float contentX = mainX + pad, contentW = cards ? mainW - 2 * pad : std::min(mainW - 2 * pad, 760.0f);
    // Find a setting indexes every page's labels once per opening, drawn where nothing shows (search.cpp).
    app.cardTitles.clear();
    app.cardResets.clear();
    app.moreKeys.clear();
    if (app.searchOpen && !app.searchIndexed) {
        search_index_pages(contentX, contentW);
        app.hits.clear();
        app.drawnLabels.clear();
        app.reportEditRect = {};
        app.cardTitles.clear();
        app.cardResets.clear();
        app.moreKeys.clear();
    }
    app.scrollArea = {mainX, bodyTop, W - mainX, bodyH};
    cv.push_clip(app.scrollArea);
    const float y0 = bodyTop + pad - app.scroll;
    // About has no header: its own title, the app's name, says what the page is. Cards have none either.
    float contentH = app.pane == Pane::About || cards ? 0 : pane_header(contentX, y0, contentW, true) + t::kSpace7;
    contentH += pane_content(contentX, y0 + contentH, contentW, true);
    float total = contentH + pad;
    if (narrow && !cards) {
        const float pw = std::min(contentW, 460.0f);
        total += side_preview(contentX, y0 + total, pw, true) + pad;
    }
    float previewOver = 0;
    if (!narrow) {  // the preview stays in view, scrolling only as far as its own bottom needs
        const float ax = W - t::kPreviewWidth, aw = t::kPreviewWidth - pad;
        const float ph = side_preview(ax, 0, aw, false) + 2 * pad;
        previewOver = std::max(0.0f, ph - bodyH);
        side_preview(ax, bodyTop + pad - std::min(app.scroll, previewOver), aw, true);
    }
    app.scrollMax = std::max({0.0f, total - bodyH, previewOver});
    draw_highlight();  // the row Find a setting jumped to
    cv.pop_clip();

    page_strip(mainX + pad - 12, W);
    topbar(W);
    rail(Hh);  // last: its notch reaches over the page

    if (app.searchOpen)
        search_card();
    if (app.sheet.open)
        sheet_card();  // over everything: nothing under it answers
    else
        help_tip();
    if (app.popup.open)
        popup_menu();
    cv.end();
    place_report_edit();
    place_sheet_edit();
    place_search_edit();
}

}  // namespace simqvfr::launcher::ui::win

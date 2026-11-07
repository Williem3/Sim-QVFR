// The launcher window, built natively from the QVFR launcher kit (docs/qvfr-launcher-kit): a tab per game
// across the top, a sidebar of panes in groups, cards of the kit's controls, a live preview, and an action bar
// with the status, Restore Defaults and Launch. This file has the window procedure and run_window; the state
// is in window_state.h, the panes in pane_*.cpp, the window around them in chrome.cpp, input in pointer.cpp
// and input.cpp. Settings save as they change (settings.h); Launch runs launch_game (main.cpp).
//
// Immediate mode: each paint lays out and draws the current pane and records hit rectangles; input
// acts on the last frame's rectangles and the settings, then repaints.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "../preflight.h"
#include "../report.h"
#include "../game_tabs.h"
#include "../settings.h"
#include "../session_history.h"
#include "../install.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"

#pragma comment(lib, "comdlg32.lib")

namespace simqvfr::launcher::ui::win {

namespace {

float pane_content(float x, float y, float w, bool draw) {
    float h = 0;
    switch (app.pane) {
    case Pane::Checks: h += checks_pane(x, y, w, draw); break;
    case Pane::Hotkeys: h += hotkeys_pane(x, y, w, draw); break;
    case Pane::Profiles: h += profiles_pane(x, y, w, draw); break;
    case Pane::Resolution: h += resolution_pane(x, y, w, draw); break;
    case Pane::Foveation: h += foveation_pane(x, y, w, draw); break;
    case Pane::Overlay: h += overlay_pane(x, y, w, draw); break;
    case Pane::Dlss: h += dlss_pane(x, y, w, draw); break;
    case Pane::Menu: h += menu_pane(x, y, w, draw); break;
    case Pane::Games: h += games_pane(x, y, w, draw); break;
    case Pane::Layers: h += layers_pane(x, y, w, draw); break;
    case Pane::Diagnostics: h += diagnostics_pane(x, y, w, draw); break;
    case Pane::Report: h += report_pane(x, y, w, draw); break;
    case Pane::About: h += about_pane(x, y, w, draw); break;
    }
    return h;
}

// ---- Window ------------------------------------------------------------------------------------

struct Item {
    const wchar_t* label;
    const char* icon[3];
};

// The kit's sidebar glyphs (16 x 16, 1.5 stroke); circles and rects are drawn as paths.
const Item kItems[] = {
    {L"Launch checks", {"M2 4.25l1.5 1.5 2.75-3", "M2 10.75l1.5 1.5 2.75-3", "M8.75 4.5H14M8.75 11H14"}},
    {L"Profiles", {"M3 3.25H10.5V12.75H3Z", "M10.5 5.25H13V14.75H5.5V12.75", "M5 6.25H8.5M5 8.75H8.5"}},
    {L"Resolution", {"M3.25 2.75H12.75C13.6 2.75 14.25 3.4 14.25 4.25V9.75C14.25 10.6 13.6 11.25 12.75 11.25H3.25C2.4 11.25 1.75 10.6 1.75 9.75V4.25C1.75 3.4 2.4 2.75 3.25 2.75Z", "M5.5 14h5M8 11.25V14", nullptr}},
    {L"Foveation", {"M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z", nullptr, nullptr}},
    {L"Focus overlay", {"M5 5H11V11H5Z", nullptr, nullptr}},
    {L"Foveated DLSS", {"M8 1.75L9.6 6.4 14.25 8 9.6 9.6 8 14.25 6.4 9.6 1.75 8 6.4 6.4Z", nullptr, nullptr}},
    {L"Headset menu", {"M3.25 3.25H12.75C13.6 3.25 14.25 3.9 14.25 4.75V11.25C14.25 12.1 13.6 12.75 12.75 12.75H3.25C2.4 12.75 1.75 12.1 1.75 11.25V4.75C1.75 3.9 2.4 3.25 3.25 3.25Z", "M4.5 6.5H8.5", "M4.5 9.5H11.5"}},
    {L"Hotkeys", {"M2.25 4.25H13.75V11.75H2.25Z", "M4.75 6.75H5.25M7.75 6.75H8.25M10.75 6.75H11.25", "M5.25 9.25H10.75"}},
    {L"Games", {"M2.5 2.5H7V7H2.5Z", "M9 2.5H13.5V7H9Z", "M2.5 9H7V13.5H2.5ZM9 9H13.5V13.5H9Z"}},
    {L"OpenXR layers", {"M2.5 4.5H10.5", "M2.5 8H10.5", "M2.5 11.5H10.5"}},
    {L"Diagnostics", {"M1.5 8.5H4.5L6.25 4.5L9.75 11.5L11.5 8.5H14.5", nullptr, nullptr}},
    {L"Report an issue", {"M3.5 14.25V2", "M3.5 2.75H12.5L10.5 5.5L12.5 8.25H3.5", nullptr}},
    {L"About", {"M8 7.25V11", nullptr, nullptr}},
};

static_assert(std::size(kItems) == kPanes, "a sidebar item for every pane");

}  // namespace

// A pane the sidebar lists for this game: Foveated DLSS only where the game has it.
bool pane_shown(int pane) {
    return pane != static_cast<int>(Pane::Dlss) || game_has(&games::GameFeatures::foveatedDlss);
}

namespace {

// The shown pane nearest `pane` in `step`'s direction (-1 or 1), or `pane` itself if shown; `from` if none.
int shown_pane(int pane, int step, int from) {
    for (int p = pane; p >= 0 && p < kPanes; p += step)
        if (pane_shown(p))
            return p;
    return from;
}

// The sidebar's groups, each a run of panes in Pane's order; a pane's eyebrow is its group.
struct Group {
    const wchar_t* label;
    int first, end;
};

constexpr Group kGroups[] = {{L"Setup", 0, 2}, {L"Picture", 2, 6}, {L"In the headset", 6, 8}, {L"Advanced", 8, 11}, {L"Help", 11, 13}};
static_assert(kGroups[std::size(kGroups) - 1].end == kPanes, "every pane in a group");

const wchar_t* group_of(int pane) {
    for (const Group& g : kGroups)
        if (pane >= g.first && pane < g.end)
            return g.label;
    return L"";
}

// What a pane is for, under its title.
std::wstring pane_description(Pane pane) {
    switch (pane) {
    case Pane::Checks: return L"What would stop Sim-QVFR at the next launch, each with its fix.";
    case Pane::Profiles: return L"A profile saves the whole picture: resolution, foveation, crop, overlay and DLSS.";
    case Pane::Resolution: {
        const bool crop = game_has(&games::GameFeatures::fovCrop), helmet = game_has(&games::GameFeatures::helmet);
        return std::wstring(L"How many pixels each view draws") + (crop && helmet ? L", the edges it skips and the helmet around it."
                                                                   : crop       ? L" and the edges it skips."
                                                                   : helmet     ? L" and the helmet around it."
                                                                                : L".");
    }
    case Pane::Foveation: return L"The size, shape and movement of the sharp focus region.";
    case Pane::Overlay: return L"An outline in the headset that shows where the focus region is.";
    case Pane::Dlss: return L"Cheaper outer views, a sharper centre and frame generation, with Cheeky Foveated DLSS.";
    case Pane::Menu: return L"Where the settings menu and the performance overlay float in your view.";
    case Pane::Hotkeys: return L"The keys and wheel buttons for the headset menu and the in-game toggles.";
    case Pane::Games: return L"Which games have a tab, and each game’s colour.";
    case Pane::Layers: return L"The OpenXR layers " + game_name() + L" loads, and their order.";
    case Pane::Diagnostics: return L"The last session’s numbers against the ones before it, and what to share when you ask for help.";
    case Pane::Report: return L"Zips the logs and opens a GitHub issue with the details filled in.";
    case Pane::About: return L"The version, updates and support.";
    }
    return L"";
}

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
constexpr float kMenuItemH = 32;

Rect popup_menu_rect() {
    const Popup& p = app.popup;
    float w = p.anchor.w;
    for (const auto& item : p.items)
        w = std::max(w, std::ceil(app.canvas.measure(item, t::kBody)) + 28 + 6 + 12 + 8);
    Rect menu{p.anchor.x, p.anchor.y + p.anchor.h + 4, w, kMenuItemH * p.items.size() + 12};
    if (menu.right() > app.canvas.width() - 8)
        menu.x = p.anchor.right() - w;
    if (menu.bottom() > app.canvas.height())
        menu.y = p.anchor.y - menu.h - 4;
    return menu;
}

}  // namespace

// The description box (a native edit, kept in step with the canvas after each paint): over the field the
// Diagnostics pane drew; hidden when it is not wholly in view, or a popup is open.
// The native edits' font (Segoe UI 13 at the window's DPI), made again when the DPI changes: the report's description
// box and the sheet's name field.
void edit_font() {
    const UINT dpi = static_cast<UINT>(std::lround(96 * app.canvas.dpi_scale()));
    if (app.reportFont && app.reportFontDpi == dpi)
        return;
    HFONT font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND edit : {app.reportEdit, app.sheetEdit})
        if (edit)
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (app.reportFont)
        DeleteObject(app.reportFont);
    app.reportFont = font;
    app.reportFontDpi = dpi;
}

namespace {

// The action bar's badge for the launch checks' first problem (and how many more); it opens the pane. Returns its width.
constexpr float kBarX = t::kSpace5;

float checks_badge(float y, int tone, const Check& first, float maxWidth) {
    size_t more = 0;
    for (const Check& c : app.checks)
        more += c.tone == CheckTone::Bad || c.tone == CheckTone::Warn ? 1 : 0;
    const std::wstring text = first.title + (more > 1 ? L"  ·  " + std::to_wstring(more - 1) + L" more" : L"");
    const float w = badge(kBarX, y, tone, text, maxWidth);
    app.hits.push_back({kChecksBadge, {kBarX, y, w, 20}});
    return w;
}

constexpr t::TypeStyle kBodyMedium{t::kBody.size, t::kBody.lineHeight, 500, false};

constexpr t::TypeStyle kBodyStrong{t::kBody.size, t::kBody.lineHeight, 600, false};

// The app's mark: the game's accent with an eye.
void brand_mark(float x, float y, float size) {
    app.canvas.fill({x, y, size, size}, col(app.c->accent_fill), size * 0.28f);
    const float g = size * 0.66f, gx = x + (size - g) / 2, gy = y + (size - g) / 2;
    app.canvas.path("M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6S2 12 2 12z", {gx, gy, g, g}, 24, col(app.c->on_accent), 2.2f);
    app.canvas.circle(gx + g / 2, gy + g / 2, g * 0.11f, col(app.c->on_accent), false, 2.2f * g / 24);
}

// The tab strip: the app's name, then a tab per game, each marked in its own accent; the chosen one's palette is the
// window's (apply_theme).
void game_tabs(float W) {
    Canvas& cv = app.canvas;
    const float H = t::kTabbarHeight;
    cv.fill({0, 0, W, H}, col(app.c->titlebar_bg));
    cv.line(0, H - 0.5f, W, H - 0.5f, col(app.c->card_border), 1);
    // The app: its mark, its name and BETA.
    const float bx = t::kSpace4;
    brand_mark(bx, (H - 24) / 2, 24);
    const float nameW = std::ceil(cv.measure(L"Sim-QVFR", kBodyStrong));
    cv.text(L"Sim-QVFR", kBodyStrong, {bx + 24 + 10, 0, nameW + 2, H}, col(app.c->label));
    const t::TypeStyle beta{10.5f, 14, 600, false};
    const float betaW = std::ceil(cv.measure(L"BETA", beta)) + 14, betaX = bx + 24 + 10 + nameW + 8;
    cv.fill({betaX, (H - 18) / 2, betaW, 18}, col(app.c->warning_bg), 9);
    cv.text(L"BETA", beta, {betaX, (H - 18) / 2, betaW, 18}, col(app.c->warning), Align::Center);
    // The games, from the content column's edge; short names when the full ones don't fit.
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    const float x0 = std::max(t::kSidebarWidth, betaX + betaW + t::kSpace5);
    const auto tab_width = [&](const std::wstring& label) { return 14 + 8 + 8 + std::ceil(cv.measure(label, kBodyStrong)) + 16; };
    float full = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (game_shown(*all[i]) || all[i] == &games::launcher_game())
            full += tab_width(all[i]->name) + 4;
    const bool shortNames = x0 + full > W - t::kSpace4;
    float x = x0;
    for (std::size_t i = 0; i < count; ++i) {
        const games::GameInfo& game = *all[i];
        const bool selected = &game == &games::launcher_game();
        if (!selected && !game_shown(game))
            continue;  // hidden in the Games pane
        const std::wstring label = shortNames ? game.shortName : game.name;
        const t::Accent accent = game_accent(game);
        const Rect tab{x, 8, tab_width(label), H - 16};
        if (selected)
            cv.fill(tab, col(accent.accent_fill, app.dark ? 0.22f : 0.12f), t::kRadiusControl);
        else if (pointer_in(tab))
            cv.fill(tab, col(app.dark ? 0xffffff : 0, 0.06f), t::kRadiusControl);
        cv.circle(tab.x + 14 + 4, tab.y + tab.h / 2, 4, col(accent.accent), true);
        cv.text(label, selected ? kBodyStrong : kBodyMedium, {tab.x + 14 + 8 + 8, tab.y, tab.w - 30, tab.h},
                col(selected ? app.c->label : app.c->label_secondary));
        if (selected)  // the chosen tab's edge, in its accent, on the strip's bottom line
            cv.fill({tab.x + 10, H - 3, tab.w - 20, 3}, col(accent.accent), 1.5f);
        if (selected && focused(kGameTab))
            focus_ring(tab, t::kRadiusControl);
        app.hits.push_back({kGameTab, tab, static_cast<int>(i)});
        x += tab.w + 4;
    }
}

// The sidebar: the panes in their groups, each a row with its icon. When the groups don't fit, their labels go.
void sidebar(float top, float bottom) {
    Canvas& cv = app.canvas;
    const float W = t::kSidebarWidth;
    cv.fill({0, top, W, bottom - top}, col(app.c->sidebar_bg));
    cv.line(W - 0.5f, top, W - 0.5f, bottom, col(app.c->card_border), 1);
    const float labelH = t::kFootnote.lineHeight + 6, groupGap = 10, rowGap = 2;
    float fullH = 2 * t::kSpace3;
    for (const Group& g : kGroups) {
        fullH += labelH;
        for (int i = g.first; i < g.end; ++i)
            fullH += pane_shown(i) ? t::kSidebarRowHeight + rowGap : 0;
    }
    fullH += groupGap * (std::size(kGroups) - 1);
    const bool compact = fullH > bottom - top;
    const float rowH = compact ? 28.0f : t::kSidebarRowHeight;
    const CheckTone worst = worst_tone(app.checks);
    float sy = top + t::kSpace3;
    for (const Group& g : kGroups) {
        if (!compact) {
            eyebrow(12 + 10, sy, W - 44, g.label, true);
            sy += labelH;
        }
        for (int i = g.first; i < g.end; ++i) {
            if (!pane_shown(i))
                continue;
            const Rect row{12, sy, W - 24, rowH};
            const bool selected = static_cast<int>(app.pane) == i;
            if (selected)
                cv.fill(row, col(app.c->sidebar_selection, app.dark ? 0.2f : 0.12f), t::kRadiusControl);
            else if (pointer_in(row))
                cv.fill(row, col(app.dark ? 0xffffff : 0, 0.05f), t::kRadiusControl);
            sidebar_icon(i, {row.x + 10, row.y + (rowH - 16) / 2, 16, 16}, col(selected ? app.c->accent : app.c->label_muted));
            cv.text(kItems[i].label, selected ? kBodyStrong : kBodyMedium, {row.x + 10 + 16 + 10, row.y, row.w - 64, rowH},
                    col(selected ? app.c->label : app.c->heading));
            // A mark on Foveation without an eye tracker, and on Launch checks with a problem (red) or warning.
            const bool checksMark = i == static_cast<int>(Pane::Checks) && worst != CheckTone::Ok;
            if ((i == static_cast<int>(Pane::Foveation) && eye_warning()) || checksMark) {
                const bool danger = checksMark && worst == CheckTone::Bad;
                const float bx = row.right() - 10 - 16, by = row.y + (rowH - 16) / 2;
                cv.circle(bx + 8, by + 8, 8, col(danger ? app.c->danger_bg : app.c->warning_bg), true);
                const Color mark = col(danger ? app.c->danger : app.c->warning);
                cv.line(bx + 8, by + 4.5f, bx + 8, by + 8.5f, mark, 1.7f);
                cv.circle(bx + 8, by + 11, 1, mark, true);
            }
            if (selected && focused(kSidebar))
                focus_ring(row, t::kRadiusControl);
            app.hits.push_back({kSidebar, row, i});
            sy += rowH + rowGap;
        }
        sy += compact ? 6 : groupGap;
    }
}

// The pane's header: its group, its title and what it is for. Returns its height.
float pane_header(float x, float y, float w, bool draw) {
    const std::wstring desc = pane_description(app.pane);
    const float dh = app.canvas.measure_height(desc, t::kBody, w);
    if (draw) {
        app.canvas.text(group_of(static_cast<int>(app.pane)), {12, 16, 600, false}, {x, y, w, 16}, col(app.c->label_muted));
        app.canvas.text(kItems[static_cast<int>(app.pane)].label, t::kLargeTitle, {x, y + 20, w, t::kLargeTitle.lineHeight},
                        col(app.c->label));
        app.canvas.text(desc, t::kBody, {x, y + 20 + t::kLargeTitle.lineHeight + 4, w, dh + 2}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    }
    return 20 + t::kLargeTitle.lineHeight + 4 + dh;
}

void paint() {
    if (!app.canvas.begin())
        return;
    app.hits.clear();
    app.reportEditRect = {};
    app.sheetEditRect = {};
    Canvas& cv = app.canvas;
    const float W = cv.width(), Hh = cv.height();
    const float tabH = t::kTabbarHeight, actionH = t::kActionbarHeight;
    const float bodyTop = tabH, bodyH = Hh - tabH - actionH;
    cv.fill({0, 0, W, Hh}, col(app.c->window_bg));

    game_tabs(W);
    sidebar(bodyTop, bodyTop + bodyH);

    // Main and preview columns; below 1000 wide the preview drops under the form.
    const bool narrow = W < 1000;
    const float pad = t::kSpace7;
    const float mainX = t::kSidebarWidth, mainW = narrow ? W - mainX : W - mainX - t::kPreviewWidth;
    const float contentX = mainX + pad, contentW = std::min(mainW - 2 * pad, 760.0f);
    app.scrollArea = {mainX, bodyTop, W - mainX, bodyH};
    cv.push_clip(app.scrollArea);
    const float y0 = bodyTop + pad - app.scroll;
    // About has no header: its own title, the app's name, says what the page is.
    float contentH = app.pane == Pane::About ? 0 : pane_header(contentX, y0, contentW, true) + t::kSpace7;
    contentH += pane_content(contentX, y0 + contentH, contentW, true);
    float total = contentH + pad;
    if (narrow) {
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
    cv.pop_clip();
    app.scrollMax = std::max({0.0f, total - bodyH, previewOver});

    // Action bar: the status, Restore Defaults, Launch.
    const float barY = bodyTop + bodyH;
    cv.fill({0, barY, W, actionH}, col(app.c->sidebar_bg));
    cv.line(0, barY + 0.5f, W, barY + 0.5f, col(app.c->card_border), 1);
    std::wstring launchLabel = L"Launch " + game_name();
    float launchW = std::ceil(cv.measure(launchLabel, kPrimaryLabel)) + 16 + 10 + 2 * 22;
    const float restoreW = button_width(L"Restore Defaults") + 24;
    if (W - launchW - restoreW - 3 * t::kSpace5 < 200) {  // narrow: the game's short name
        launchLabel = L"Launch " + std::wstring(games::launcher_game().shortName);
        launchW = std::ceil(cv.measure(launchLabel, kPrimaryLabel)) + 16 + 10 + 2 * 22;
    }
    const Rect launchR{W - t::kSpace5 - launchW, barY + (actionH - t::kControlHeightLarge) / 2, launchW, t::kControlHeightLarge};
    const Rect restoreR{launchR.x - t::kSpace2 - restoreW, barY + (actionH - t::kControlHeight) / 2, restoreW, t::kControlHeight};
    const float badgeMax = restoreR.x - t::kSpace4 - kBarX;
    const float by = barY + (actionH - 20) / 2;
    const Readouts ro = readouts(app.s, app.runtime);
    // The game launched from here (for the game whose tab is chosen): starting, and what went wrong, come before the
    // launch checks; running and closed after them, and after what a change did to the running game.
    const bool watching = app.watch.game() == &games::launcher_game();
    const LaunchOutcome launchNow = watching ? app.watch.outcome() : LaunchOutcome{};
    const bool launchFirst = watching && (launchNow.state == LaunchState::Starting || launchNow.tone == 1 || launchNow.tone == 2);
    const auto launch_badge = [&] {
        const float w = badge(kBarX, by, launchNow.tone, launchNow.text, badgeMax);
        if (launchNow.link != LaunchLink::None)
            app.hits.push_back({kLaunchBadge, {kBarX, by, w, 20}});
        return w;
    };
    float bw = 0;
    if (app.status == Status::Starting)
        bw = badge(kBarX, by, 3, L"Starting " + game_name() + L"…", badgeMax);
    else if (app.status == Status::Error)
        bw = badge(kBarX, by, 2, app.error, badgeMax);
    else if (!app.iniFound)
        bw = badge(kBarX, by, 2, L"sim-qvfr.ini not found next to the launcher.", badgeMax);
    else if (app.undoRestore) {
        bw = badge(kBarX, by, 3, L"Defaults restored for " + short_name() + L" · Undo", badgeMax);
        app.hits.push_back({kUndoBadge, {kBarX, by, bw, 20}});
    } else if (launchFirst)
        bw = launch_badge();
    else if (const Check* bad = first_with(app.checks, CheckTone::Bad))
        bw = checks_badge(by, 2, *bad, badgeMax);
    else if (eye_warning())
        bw = badge(kBarX, by, 1, L"Eye tracker not detected", badgeMax);
    else if (app.runtime.known() && ro.overRuntimeLimit)
        bw = badge(kBarX, by, 1, L"Larger than the runtime’s maximum image size", badgeMax);
    else if (const Check* warn = first_with(app.checks, CheckTone::Warn))
        bw = checks_badge(by, 1, *warn, badgeMax);
    else if (app.live == Live::NextLaunch)
        bw = badge(kBarX, by, 1, app.s.mode == RenderMode::Qvfr
                                     ? L"Applied live up to the launch resolution · the rest at the next launch"
                                     : L"Foveation applied live · resolution and method at the next launch",
                   badgeMax);
    else if (app.live == Live::Applied)
        bw = badge(kBarX, by, 0, L"Applied to the running game", badgeMax);
    else if (watching)
        bw = launch_badge();
    else if (app.update == Update::Available) {
        bw = badge(kBarX, by, 3,
                   app.installing ? L"Installing Sim-QVFR " + app.release.tag + L"…"
                                  : L"Sim-QVFR " + app.release.tag + L" is available" + (installable() ? L" · Install" : L""),
                   badgeMax);
        app.hits.push_back({kUpdateBadge, {kBarX, by, bw, 20}});
    } else
        bw = badge(kBarX, by, 0, L"Ready", badgeMax);
    // What the launch will use, after the status when there is room: the profile, the mode, the focus density.
    {
        const std::wstring profile = !app.profileCurrent.empty() && app.profileMatches ? app.profileCurrent : L"Custom";
        const std::wstring rest = L"  ·  " + std::wstring(app.s.eyeTracking ? L"Eye tracked" : L"Fixed") + L"  ·  Focus " +
                                  std::to_wstring(static_cast<int>(std::lround(ro.focusDensity * 100))) + L"%";
        const float sx = kBarX + bw + t::kSpace4, room = badgeMax + kBarX - sx;
        const float w1 = std::ceil(cv.measure(L"Profile ", t::kBody)), w2 = std::ceil(cv.measure(profile, kBodyMedium)),
                    w3 = std::ceil(cv.measure(rest, t::kBody));
        if (room > 1 + t::kSpace4 + w1 + w2 + w3) {
            cv.line(sx, by + 2, sx, by + 18, col(app.c->card_border), 1);
            const float tx = sx + t::kSpace4;
            cv.text(L"Profile ", t::kBody, {tx, by - 2, w1 + 2, 24}, col(app.c->label_secondary));
            cv.text(profile, kBodyMedium, {tx + w1, by - 2, w2 + 2, 24}, col(app.c->label));
            cv.text(rest, t::kBody, {tx + w1 + w2, by - 2, w3 + 2, 24}, col(app.c->label_secondary));
        }
    }
    button(kRestore, restoreR, L"Restore Defaults", Btn::Ghost, kIconReset);
    if (game_running())  // a second launch would start nothing: the game's state is in Launch checks
        button(kLaunch, launchR, short_name() + L" is running", Btn::Secondary);
    else
        button(kLaunch, launchR, launchLabel, Btn::Primary, kIconPlay);

    if (app.sheet.open)
        sheet_card();  // over everything: nothing under it answers
    else
        help_tip();

    // An open pop-up menu, over everything.
    if (app.popup.open) {
        const Popup& p = app.popup;
        const float itemH = kMenuItemH;
        const Rect menu = popup_menu_rect();
        cv.fill({menu.x, menu.y + 4, menu.w, menu.h}, {0, app.dark ? 0.5f : 0.16f}, t::kRadiusGroup);
        cv.fill(menu, col(app.dark ? app.c->control_bg : app.c->group_bg), t::kRadiusGroup);
        cv.stroke(menu, col(app.dark ? app.c->control_hover : app.c->card_border), 1, t::kRadiusGroup);
        for (size_t i = 0; i < p.items.size(); ++i) {
            const Rect item{menu.x + 6, menu.y + 6 + itemH * i, menu.w - 12, itemH};
            const bool hot = static_cast<int>(i) == p.hover;
            if (hot)
                cv.fill(item, col(app.c->accent_fill), 6);
            if (static_cast<int>(i) == p.selected)
                cv.path("M3.5 8.2l2.6 2.6L12.5 4.5", {item.x + 8, item.y + (itemH - 14) / 2, 14, 14}, 16,
                        col(hot ? app.c->on_accent : app.c->accent), 1.8f);
            cv.text(p.items[i], t::kBody, {item.x + 28, item.y, item.w - 34, itemH}, col(hot ? app.c->on_accent : app.c->label));
        }
    }
    cv.end();
    place_report_edit();
    place_sheet_edit();
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
    // Inner resolution in its steps (100% reachable); the arrow keys still move every slider by 1.
    *v = slider_value_at(app.dragRange, f, app.dragging == kInner ? kInnerDragStep : 1);
    if (app.dragging == kColorValue)
        apply_color_edit();  // the brightness: the edited game's colour follows
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

// A game tab: [launcher] game, then everything that follows the game (its checks, its features, its palette).
void select_game(int option) {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    if (option < 0 || static_cast<std::size_t>(option) >= count || all[option] == &games::launcher_game())
        return;
    if (app.iniFound) {
        // Each game keeps its own settings: this one's go to its copy, the other's come back into the ini.
        switch_game_settings(app.ini, widen(games::launcher_game().id), widen(all[option]->id));
        WritePrivateProfileStringW(L"launcher", L"game", widen(all[option]->id).c_str(), app.ini.c_str());
        choose_launcher_game();
        app.s = load_settings(app.ini);
        app.iniText = ini_text();
        app.undoRestore.reset();  // it held the other game's settings
        refresh_profiles();  // the profile in use is the game's
    } else {
        games::set_launcher_game(*all[option]);  // for this window only: there is no ini to keep it in
    }
    if (!pane_shown(static_cast<int>(app.pane)))
        app.pane = static_cast<Pane>(shown_pane(static_cast<int>(app.pane), -1, 0));
    apply_theme();
    refresh_checks();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int game_index() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    for (std::size_t i = 0; i < count; ++i)
        if (all[i] == &games::launcher_game())
            return static_cast<int>(i);
    return 0;
}

void close_popup(bool accept) {
    if (accept && app.popup.hover >= 0)
        choose(app.popup.owner, app.popup.hover);
    app.popup.open = false;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int popup_item_at(float x, float y) {
    const Popup& p = app.popup;
    const float itemH = kMenuItemH;
    const Rect menu = popup_menu_rect();
    for (size_t i = 0; i < p.items.size(); ++i)
        if (Rect{menu.x + 6, menu.y + 6 + itemH * i, menu.w - 12, itemH}.contains(x, y))
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

}  // namespace

// Launch checks' Choose folder… (a game Steam doesn't list): the game's folder, saved as [launcher] game_dir_<id>.
void choose_game_folder() {
    const games::GameInfo& game = games::launcher_game();
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const std::wstring title = L"Choose " + game_name() + L"’s folder";
    dialog->SetTitle(title.c_str());
    std::wstring picked;
    if (SUCCEEDED(dialog->Show(app.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (picked.empty())
        return;
    const std::wstring folder = game_folder_from(picked, game);
    if (folder.empty()) {
        notify(L"That folder doesn’t hold " + game_name(),
               picked + L" has no " + game.exeName + L". Choose the folder Steam’s Browse local files opens for " + game_name() + L".");
        return;
    }
    if (!app.iniFound || !WritePrivateProfileStringW(L"launcher", (L"game_dir_" + widen(game.id)).c_str(), folder.c_str(),
                                                     app.ini.c_str())) {
        notify(L"The folder couldn’t be saved", L"sim-qvfr.ini wasn’t found next to the launcher.");
        return;
    }
    app.iniText = ini_text();
    refresh_installed();  // found now
    if (app.status == Status::Error)
        app.status = Status::Idle;  // "not found: choose its folder" is answered
}

namespace {

// Restore Defaults' Undo: the settings from before it.
void undo_restore() {
    if (!app.undoRestore)
        return;
    app.s = *app.undoRestore;
    changed();  // also ends the Undo
}

}  // namespace

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
    case kHelmetDetail: open_popup(id); return;
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
    case kSupport: ShellExecuteW(app.hwnd, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    default: break;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

float dip(LPARAM lp, bool y) {
    return (y ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp)) / app.canvas.dpi_scale();
}

// A push button: Return presses it while it has the focus (as on Windows); anywhere else Return presses Launch, the
// window's default button. Space presses these too.
bool push_button(Ctl id) {
    switch (id) {
    case kCheekyRuntime:
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
    case kOsdOn:
    case kHotkeyFooter:
    case kUpdateCheck:
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
        // The window opens on Launch checks: what the game, Steam and the layers look like now (often a new day: a game
        // update, Steam closed). The launcher stays open while the game runs, so a tuning session keeps its pane.
        activate(kSidebar, static_cast<int>(Pane::Checks));
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
    case WM_CTLCOLOREDIT:  // the description box in the theme's field colours
        if (reinterpret_cast<HWND>(lParam) == app.reportEdit || reinterpret_cast<HWND>(lParam) == app.sheetEdit) {
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
        if (!h)
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
            const int tip = h && h->id == kHelpIcon ? h->option : -1;
            const Ctl hover = h ? h->id : kNone;
            const int hoverOption = h ? h->option : -1;
            if (tip != app.tip || hover != app.hover || hoverOption != app.hoverOption) {  // hover states follow
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

// The window around the panes: the game tabs, the sidebar (items, groups, icons), each pane's header, the
// popup menu, the action bar, and paint, which lays out and draws the whole window each frame.
#include <algorithm>
#include <cmath>
#include <string>

#include <windows.h>

#include "../preflight.h"
#include "../settings.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"

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

// The shown pane nearest `pane` in `step`'s direction (-1 or 1), or `pane` itself if shown; `from` if none.
int shown_pane(int pane, int step, int from) {
    for (int p = pane; p >= 0 && p < kPanes; p += step)
        if (pane_shown(p))
            return p;
    return from;
}

namespace {

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

}  // namespace

// The open pop-up menu: under its button, as wide as the button or its longest item (check mark
// column included), right-aligned with the button when it would leave the window.
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

}  // namespace

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

}  // namespace simqvfr::launcher::ui::win

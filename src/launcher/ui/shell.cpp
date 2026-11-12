// The window's shell (docs/ui/OVERHAUL_PLAN.md §2): the game rail on the left, the top bar (the tabs, the theme
// button, Find a setting, the profile menu, the status and Launch) and the strip of the tab's pages under it.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "../launch_plan.h"
#include "../preflight.h"
#include "../settings.h"
#include "../update_check.h"
#include "../../games/game.h"
#include "launcher_version.h"
#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace {

// Each tab's pages, in order (the old panes, until the tab has its cards).
const std::vector<Pane> kTabPages[kTabs] = {
    {Pane::Profiles, Pane::About},
    {Pane::Resolution, Pane::Foveation, Pane::Overlay, Pane::Dlss},
    {Pane::Menu, Pane::Hotkeys},
    {Pane::Checks, Pane::Diagnostics, Pane::Report, Pane::Games, Pane::Layers},
};

constexpr const wchar_t* kTabLabels[kTabs] = {L"Overview", L"Picture", L"Headset", L"Game"};

// A tab's colour (its shape, its underline): ink for Overview, then blue, red and yellow.
uint32_t tab_color(Tab tab) {
    switch (tab) {
    case Tab::Picture: return app.c->blue;
    case Tab::Headset: return app.c->red;
    case Tab::Game: return app.c->yellow;
    default: return app.c->text;
    }
}

// A tab's shape, `s` across, centred at (cx, cy): an outlined square, a square, a circle, a triangle.
void tab_shape(Tab tab, float cx, float cy, float s) {
    const Color c = col(tab_color(tab));
    switch (tab) {
    case Tab::Overview: app.canvas.stroke({cx - s / 2, cy - s / 2, s, s}, c, 1.6f); break;
    case Tab::Picture: app.canvas.fill({cx - s / 2, cy - s / 2, s, s}, c); break;
    case Tab::Headset: app.canvas.circle(cx, cy, s / 2, c, true); break;
    case Tab::Game: app.canvas.path("M0 10L5 0.5L10 10Z", {cx - s / 2, cy - s / 2, s, s}, 10, c, 0, true); break;
    }
}

// The logo: a blue square, a red circle and a yellow triangle (Picture, Headset, Game), `size` high, centred on cx.
void brand_mark(float cx, float y, float size) {
    const float s = size * 0.62f, gap = size * 0.14f, w = 3 * s + 2 * gap, x = cx - w / 2, top = y + (size - s) / 2;
    app.canvas.fill({x, top, s, s}, col(app.c->blue));
    app.canvas.circle(x + s + gap + s / 2, top + s / 2, s / 2, col(app.c->red), true);
    app.canvas.path("M0 12L6 0L12 12Z", {x + 2 * (s + gap), top, s, s}, 12, col(app.c->yellow), 0, true);
}

// The theme button's glyph: a sun (light), a crescent (dark), or a half-filled circle (following Windows).
void theme_glyph(const Rect& r, Color c) {
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    switch (app.themeChoice) {
    case ThemeChoice::Light:
        app.canvas.circle(cx, cy, 3.5f, c, false, 1.5f);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159265f / 4, dx = std::cos(a), dy = std::sin(a);
            app.canvas.line(cx + dx * 5.75f, cy + dy * 5.75f, cx + dx * 7.5f, cy + dy * 7.5f, c, 1.5f);
        }
        break;
    case ThemeChoice::Dark:
        app.canvas.path("M11.5 3C7.5 1.5 2.5 4.5 2.5 8.5C2.5 12.5 7.5 15 11.5 13C8 12 6.5 10.5 6.5 8C6.5 5.5 8 4 11.5 3Z", r, 16,
                        c, 1.5f);
        break;
    default:
        app.canvas.circle(cx, cy, 6, c, false, 1.5f);
        app.canvas.path("M8 2C11.3 2 14 4.7 14 8S11.3 14 8 14Z", r, 16, c, 0, true);
        break;
    }
}

const wchar_t* theme_tip() {
    switch (app.themeChoice) {
    case ThemeChoice::Light: return L"Theme: light. Click for dark.";
    case ThemeChoice::Dark: return L"Theme: dark. Click to follow Windows.";
    default: return L"Theme: following Windows. Click for light.";
    }
}

// An icon button in the top bar (32 x 32): a hover fill, the glyph, its focus ring and its tip.
void icon_button(Ctl id, const Rect& r, const std::wstring& tip) {
    if (pointer_in(r))
        app.canvas.fill(r, col(app.c->control_hover), t::kRadiusControl);
    if (focused(id))
        focus_ring(r, t::kRadiusControl);
    app.hits.push_back({id, r, -1, tip});
}

// The status: what is true now, the same states and order as the action bar had, as a dot and a few words (the
// whole sentence as its tip); `link` is what clicking it does (none: kNone).
struct StatusNow {
    int tone = 0;  // badge's: 0 success, 1 warning, 2 danger, 3 neutral
    std::wstring word, text;
    Ctl link = kNone;
};

std::wstring plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

std::wstring launch_word(LaunchState state) {
    switch (state) {
    case LaunchState::Starting: return L"Starting…";
    case LaunchState::Running: return L"Running";
    case LaunchState::NotLoaded: return L"Not loaded yet";
    case LaunchState::NoLayer: return L"No Sim-QVFR";
    case LaunchState::Crashed: return L"Stopped";
    default: return L"Closed";
    }
}

StatusNow status_now() {
    StatusNow s;
    const Readouts ro = readouts(app.s, app.runtime);
    const bool watching = app.watch.game() == &games::launcher_game();
    const LaunchOutcome launchNow = watching ? app.watch.outcome() : LaunchOutcome{};
    const bool launchFirst = watching && (launchNow.state == LaunchState::Starting || launchNow.tone == 1 || launchNow.tone == 2);
    const auto launch_status = [&] {
        return StatusNow{launchNow.tone, launch_word(launchNow.state), launchNow.text,
                         launchNow.link != LaunchLink::None ? kLaunchBadge : kNone};
    };
    size_t bad = 0, warn = 0;
    for (const Check& c : app.checks) {
        bad += c.tone == CheckTone::Bad ? 1 : 0;
        warn += c.tone == CheckTone::Warn ? 1 : 0;
    }
    const auto checks_text = [&](const Check& first) {
        return first.title + (bad + warn > 1 ? L"  ·  " + std::to_wstring(bad + warn - 1) + L" more" : L"");
    };
    if (app.status == Status::Starting)
        return {3, L"Starting…", L"Starting " + game_name() + L"…"};
    if (app.status == Status::Error)
        return {2, L"Error", app.error};
    if (!app.iniFound)
        return {2, L"No settings file", L"sim-qvfr.ini not found next to the launcher."};
    if (app.undoRestore)
        return {3, L"Defaults restored · Undo", L"Defaults restored for " + short_name() + L". Click or press Ctrl+Z to undo.",
                kUndoBadge};
    if (launchFirst)
        return launch_status();
    if (const Check* b = first_with(app.checks, CheckTone::Bad))
        return {2, plural(bad, L"problem", L"problems"), checks_text(*b), kChecksBadge};
    if (eye_warning())
        return {1, L"No eye tracker", L"Eye tracker not detected"};
    if (app.runtime.known() && ro.overRuntimeLimit)
        return {1, L"Too large", L"Larger than the runtime’s maximum image size"};
    if (const Check* w = first_with(app.checks, CheckTone::Warn))
        return {1, plural(warn, L"to check", L"to check"), checks_text(*w), kChecksBadge};
    if (app.live == Live::NextLaunch)
        return {1, L"Partly live",
                app.s.mode == RenderMode::Qvfr ? L"Applied live up to the launch resolution · the rest at the next launch"
                                               : L"Foveation applied live · resolution and method at the next launch"};
    if (app.live == Live::Applied)
        return {0, L"Applied live", L"Applied to the running game"};
    if (watching)
        return launch_status();
    if (app.update == Update::Available)
        return {3, app.installing ? L"Installing…" : L"Update available",
                app.installing ? L"Installing Sim-QVFR " + app.release.tag + L"…"
                               : L"Sim-QVFR " + app.release.tag + L" is available" + (installable() ? L" · Install" : L""),
                kUpdateBadge};
    return {0, L"Ready", L"Ready to launch"};
}

uint32_t tone_color(int tone) {
    switch (tone) {
    case 0: return app.c->success;
    case 1: return app.c->warning;
    case 2: return app.c->danger;
    default: return app.c->label_secondary;
    }
}

// `text` cut to `w` in `style`, with an ellipsis when it doesn't fit.
std::wstring fit(const std::wstring& text, const t::TypeStyle& style, float w) {
    if (app.canvas.measure(text, style) <= w)
        return text;
    std::wstring s = text;
    while (!s.empty() && app.canvas.measure(s + L"…", style) > w)
        s.pop_back();
    return s.empty() ? std::wstring() : s + L"…";
}

}  // namespace

// ---- Tabs and pages ----------------------------------------------------------------------------

Tab tab_of(Pane pane) {
    for (int t = 0; t < kTabs; ++t)
        if (std::find(kTabPages[t].begin(), kTabPages[t].end(), pane) != kTabPages[t].end())
            return static_cast<Tab>(t);
    return Tab::Overview;
}

const wchar_t* tab_label(Tab tab) { return kTabLabels[static_cast<int>(tab)]; }

// The tab's pages this game has (the DLSS page only where the game has DLSS).
std::vector<Pane> tab_pages(Tab tab) {
    std::vector<Pane> out;
    for (Pane p : kTabPages[static_cast<int>(tab)])
        if (pane_shown(static_cast<int>(p)))
            out.push_back(p);
    return out;
}

void show_page(Pane pane) { activate(kPage, static_cast<int>(pane)); }

// A tab: the page it showed last, or its first.
void show_tab(Tab tab) {
    Pane p = app.tabPage[static_cast<int>(tab)];
    const std::vector<Pane> pages = tab_pages(tab);
    if (std::find(pages.begin(), pages.end(), p) == pages.end() && !pages.empty())
        p = pages.front();
    show_page(p);
}

// ---- Rail --------------------------------------------------------------------------------------

float rail_width() { return app.canvas.width() < 1024 ? t::kRailWidthCompact : t::kRailWidth; }

// A game's abbreviation on the rail: its short name, but for names longer than the rail's big letters fit.
std::wstring rail_abbreviation(const games::GameInfo& game) {
    if (std::strcmp(game.id, "xplane") == 0)
        return L"XP12";
    return game.shortName;
}

// The rail: the logo, a block per game with a tab (the selected one yellow, with a notch toward the page), and at the
// bottom Support on Ko-fi and the version (it opens About; a dot when an update is available).
void rail(float H) {
    Canvas& cv = app.canvas;
    const float W = rail_width(), top = t::kTopbarHeight;
    const bool compact = W < t::kRailWidth;
    cv.fill({0, 0, W, H}, col(app.c->rail));
    brand_mark(W / 2, (top - 20) / 2, 20);
    // The bottom first: the blocks take what is left above it.
    const float versionH = 16, heartR = compact ? 14.0f : 16.0f;
    const float bottomH = t::kSpace4 + 2 * heartR + (compact ? 0 : t::kSpace2 + t::kRailName.lineHeight) + t::kSpace3 + versionH +
                          t::kSpace4;
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    std::vector<int> shown;
    for (std::size_t i = 0; i < count; ++i)
        if (game_shown(*all[i]) || all[i] == &games::launcher_game())
            shown.push_back(static_cast<int>(i));
    const float room = H - top - bottomH;
    const bool names = !compact && !shown.empty() && room / shown.size() >= 64;
    const float blockH = std::min(compact || !names ? 52.0f : 76.0f, shown.empty() ? 76.0f : room / shown.size());
    float y = top;
    for (const int i : shown) {
        const games::GameInfo& game = *all[i];
        const bool selected = &game == &games::launcher_game();
        const Rect block{0, y, W, blockH};
        if (selected)
            cv.fill(block, col(app.c->yellow));
        else if (pointer_in(block))
            cv.fill(block, col(0xffffff, 0.06f));
        else
            cv.line(12, y + blockH - 0.5f, W - 12, y + blockH - 0.5f, col(0xffffff, 0.08f), 1);
        cv.fill({0, y, 4, blockH}, col(game_accent(game).accent_fill));  // the game's own colour, its marker
        const Color ink = col(selected ? app.c->ink : app.c->rail_text);
        const float pad = compact ? 0.0f : 16.0f;
        const float textH = t::kRailAbbrev.lineHeight + (names ? 2 + 2 * t::kRailName.lineHeight : 0);
        const float ty = y + (blockH - textH) / 2;
        const Rect abbr = compact ? Rect{4, ty, W - 4, t::kRailAbbrev.lineHeight} : Rect{pad, ty, W - pad - 8, t::kRailAbbrev.lineHeight};
        const t::TypeStyle abbrStyle = compact ? t::TypeStyle{16, 24, 900, false, t::Family::Jost} : t::kRailAbbrev;
        cv.text(rail_abbreviation(game), abbrStyle, abbr, ink, compact ? Align::Center : Align::Leading);
        if (names)
            cv.text(game.name, t::kRailName, {pad, ty + t::kRailAbbrev.lineHeight + 2, W - pad - 10, 2 * t::kRailName.lineHeight},
                    col(selected ? app.c->ink : app.c->rail_text_muted), Align::Leading, false, true);
        if (selected)  // the notch, pointing at the page
            cv.path("M0 0L8 8L0 16Z", {W, y + blockH / 2 - 8, 16, 16}, 16, col(app.c->yellow), 0, true);  // 8 x 16
        if (selected && focused(kGameTab))
            cv.stroke(block.inset(3, 3), col(app.c->ink), 2, t::kRadiusControl);
        app.hits.push_back({kGameTab, block, i, compact ? std::wstring(game.name) : std::wstring()});
        y += blockH;
    }
    // Support on Ko-fi: a red circle with a heart.
    float by = H - bottomH + t::kSpace4;
    const float cx = W / 2;
    const Rect kofi{0, by, W, 2 * heartR + (compact ? 0 : t::kSpace2 + t::kRailName.lineHeight)};
    cv.circle(cx, by + heartR, heartR, col(app.c->red), true);
    const float hs = heartR;
    cv.path("M8 13.5C8 13.5 2 9.9 2 5.9 2 4 3.4 2.75 5 2.75 6.3 2.75 7.4 3.5 8 4.6 8.6 3.5 9.7 2.75 11 2.75 12.6 2.75 14 4 14 5.9 "
            "14 9.9 8 13.5 8 13.5Z",
            {cx - hs / 2, by + heartR - hs / 2 + 1, hs, hs}, 16, col(app.c->on_red), 0, true);
    if (!compact)
        cv.text(L"Support on Ko-fi", {11, 13, 700, false}, {0, by + 2 * heartR + t::kSpace2, W, t::kRailName.lineHeight},
                col(app.c->rail_text), Align::Center);
    if (pointer_in(kofi))
        cv.circle(cx, by + heartR, heartR + 3, col(0xffffff, 0.5f), false, 1.5f);
    if (focused(kSupport))
        cv.stroke(kofi.inset(6, -3), col(app.c->rail_text), 2, t::kRadiusControl);
    app.hits.push_back({kSupport, kofi, -1, L"Support Sim-QVFR on Ko-fi"});
    by += kofi.h + t::kSpace3;
    // The version (About), with a dot when an update is available.
    // A release's version without its commit ("1.0.5-beta (a78fc1f)"); a development build the release it is headed for.
    const std::wstring built = widen(SIM_QVFR_VERSION);
    std::wstring version = known_version(built) ? L"v" + built.substr(0, built.find(L' ')) : L"v" + widen(SIM_QVFR_RELEASE) + L" dev";
    if (compact)  // the number alone ("v1.0.5")
        version = version.substr(0, std::min(version.find(L'-'), version.find(L' ')));
    const Rect vr{0, by, W, versionH};
    cv.text(compact ? fit(version, t::kRailName, W - 8) : version, t::kRailName, vr,
            col(pointer_in(vr) ? app.c->rail_text : app.c->rail_text_muted), Align::Center);
    if (app.update == Update::Available)
        cv.circle(cx + std::min(W / 2 - 6, cv.measure(version, t::kRailName) / 2 + 6), by + versionH / 2, 3, col(app.c->yellow), true);
    if (focused(kVersion))
        cv.stroke(vr.inset(6, -2), col(app.c->rail_text), 2, t::kRadiusControl);
    app.hits.push_back({kVersion, vr, -1,
                        app.update == Update::Available ? L"Sim-QVFR " + app.release.tag + L" is available: About and updates"
                                                        : L"About Sim-QVFR and updates"});
}

// ---- Top bar -----------------------------------------------------------------------------------

// The top bar: the tabs at the left; the theme button, Find a setting, the profile menu, the status and Launch at the
// right. Narrow windows drop the profile button's "Profile:", then tighten the tabs, then shorten the status.
void topbar(float W) {
    Canvas& cv = app.canvas;
    const float x0 = rail_width(), H = t::kTopbarHeight, pad = t::kSpace4;
    cv.fill({x0, 0, W - x0, H}, col(app.c->surface));
    cv.line(x0, H - 0.5f, W, H - 0.5f, col(app.c->line), 1);
    // The right side's parts, from the right.
    const bool running = game_running();
    const std::wstring launchLabel = running ? short_name() + L" is running" : L"Launch " + short_name();
    float launchPad = 20;  // Launch's side padding
    const auto launch_width = [&] {
        return running ? button_width(launchLabel) : std::ceil(cv.measure(launchLabel, kPrimaryLabel)) + 16 + 10 + 2 * launchPad;
    };
    const StatusNow status = status_now();
    const std::wstring profile = !app.profileCurrent.empty() && app.profileMatches ? app.profileCurrent : L"Custom";
    const float chevronW = 8 + 12;
    const float prefixW = std::ceil(cv.measure(L"Profile: ", t::kBody));
    const float nameW = std::ceil(cv.measure(profile, t::kBodyStrong));
    const float statusTextW = std::ceil(cv.measure(status.word, t::kBodyMedium));
    // Tabs' widths at full spacing.
    const auto tabs_width = [&](float tabPad) {
        float w = 0;
        for (int i = 0; i < kTabs; ++i)
            w += tabPad + 10 + 8 + std::ceil(cv.measure(kTabLabels[i], t::kTabLabel)) + tabPad;
        return w;
    };
    bool withPrefix = true;
    float statusW = 8 + 6 + statusTextW, tabPad = 16;
    const auto right_width = [&] {
        const float profileW = 12 + (withPrefix ? prefixW : 0) + nameW + chevronW + 4;
        return 32 + 4 + 32 + 8 + profileW + 16 + statusW + 16 + launch_width();
    };
    const float room = W - x0 - 2 * pad - 16;  // 16: between the tabs and the right side
    if (tabs_width(tabPad) + right_width() > room)
        withPrefix = false;
    if (tabs_width(tabPad) + right_width() > room)
        tabPad = 10;
    if (tabs_width(tabPad) + right_width() > room)  // then the status's words (its tip keeps the sentence)
        statusW = std::max(8.0f, room - tabs_width(tabPad) - (right_width() - statusW));
    if (tabs_width(tabPad) + right_width() > room) {  // the narrowest windows (880): all four tabs still fit
        tabPad = 6;
        launchPad = 12;
    }
    const float launchW = launch_width();
    // Launch.
    const Rect launchR{W - pad - launchW, (H - 40) / 2, launchW, 40};
    if (running)
        button(kLaunch, launchR, launchLabel, Btn::Secondary);
    else
        button(kLaunch, launchR, launchLabel, Btn::Launch, kIconPlay);
    // The status: a dot and its words in its tone's colour; its sentence as the tip; clicked, what it links to.
    float x = launchR.x - 16 - statusW;
    {
        const Rect sr{x - 6, (H - 32) / 2, statusW + 12, 32};
        if (status.link != kNone && pointer_in(sr))
            cv.fill(sr, col(app.c->control_hover), t::kRadiusControl);
        const Color c = col(tone_color(status.tone));
        cv.circle(x + 4, H / 2, 4, c, true);
        const std::wstring word = fit(status.word, t::kBodyMedium, statusW - 14);
        if (!word.empty())
            cv.text(word, t::kBodyMedium, {x + 14, sr.y, statusW - 14 + 2, sr.h}, c);
        app.hits.push_back({status.link, sr, -1, status.text != status.word ? status.text : std::wstring()});
    }
    // The profile menu: the profile in use (Custom when the settings differ from every profile), a chevron.
    const float profileW = 12 + (withPrefix ? prefixW : 0) + nameW + chevronW + 4;
    x -= 16 + profileW;
    {
        const Rect pr{x, (H - 34) / 2, profileW, 34};
        const bool open = app.popup.open && app.popup.owner == kProfileMenu;
        cv.fill(pr, col(open || pointer_in(pr) ? app.c->control_hover : app.c->control_bg), t::kRadiusControl);
        float tx = pr.x + 12;
        if (withPrefix) {
            cv.text(L"Profile: ", t::kBody, {tx, pr.y, prefixW + 2, pr.h}, col(app.c->label_secondary));
            tx += prefixW;
        }
        cv.text(profile, t::kBodyStrong, {tx, pr.y, nameW + 2, pr.h}, col(app.c->label));
        cv.path("M1.5 4.5L5 8l3.5-3.5", {pr.right() - 8 - 12, pr.y + (pr.h - 12) / 2, 10, 12}, 10, col(app.c->label_secondary), 1.5f);
        if (focused(kProfileMenu))
            focus_ring(pr, t::kRadiusControl);
        app.hits.push_back({kProfileMenu, pr, -1, L"Profiles, saving and restoring defaults"});
    }
    // Find a setting and the theme button.
    x -= 8 + 32;
    {
        const Rect r{x, (H - 32) / 2, 32, 32};
        icon_button(kSearch, r, L"Find a setting (Ctrl+K)");
        const Color c = col(app.c->label);
        cv.circle(r.x + 14.5f, r.y + 14.5f, 5, c, false, 1.6f);
        cv.line(r.x + 18.2f, r.y + 18.2f, r.x + 22, r.y + 22, c, 1.6f);
    }
    x -= 4 + 32;
    {
        const Rect r{x, (H - 32) / 2, 32, 32};
        icon_button(kThemeToggle, r, theme_tip());
        theme_glyph({r.x + 8, r.y + 8, 16, 16}, col(app.c->label));
    }
    // The tabs: a shape and the name; the selected one in full colour with a rule under it in the tab's colour.
    const Tab current = tab_of(app.pane);
    float tx = x0 + pad;
    for (int i = 0; i < kTabs; ++i) {
        const Tab tab = static_cast<Tab>(i);
        const float lw = std::ceil(cv.measure(kTabLabels[i], t::kTabLabel));
        const Rect r{tx, 0, tabPad + 10 + 8 + lw + tabPad, H};
        if (r.right() > x - 16)
            break;  // no room (the minimum window leaves room for all four)
        const bool selected = tab == current, over = pointer_in(r);
        tab_shape(tab, r.x + tabPad + 5, H / 2, 10);
        cv.text(kTabLabels[i], t::kTabLabel, {r.x + tabPad + 18, 0, lw + 4, H},
                col(selected || over ? app.c->text : app.c->text_muted));
        if (selected)
            cv.fill({r.x + tabPad - 4, H - 3, r.w - 2 * tabPad + 8, 3}, col(tab_color(tab)));
        if (selected && focused(kTopTab))
            focus_ring(r.inset(4, 12), t::kRadiusControl);
        app.hits.push_back({kTopTab, r, i});
        tx = r.right();
    }
}

// ---- Page strip --------------------------------------------------------------------------------

// The tab's pages, as text tabs under the top bar from `x`; the selected one strong, with a rule in the tab's colour.
// Returns its height.
float page_strip(float x, float W) {
    Canvas& cv = app.canvas;
    const float y = t::kTopbarHeight, H = 44;
    const Tab tab = tab_of(app.pane);
    cv.fill({rail_width(), y, W - rail_width(), H}, col(app.c->bg));
    cv.line(rail_width(), y + H - 0.5f, W, y + H - 0.5f, col(app.c->line), 1);
    float px = x;
    for (Pane p : tab_pages(tab)) {
        const bool selected = p == app.pane;
        const std::wstring label = pane_label(static_cast<int>(p));
        const t::TypeStyle style = selected ? t::kBodyStrong : t::kBodyMedium;
        const float lw = std::ceil(cv.measure(label, style));
        const Rect r{px, y, lw + 24, H};
        const bool over = pointer_in(r);
        cv.text(label, style, {r.x + 12, y, lw + 4, H}, col(selected || over ? app.c->text : app.c->text_muted));
        if (selected)
            cv.fill({r.x + 12, y + H - 3, lw, 3}, col(tab_color(tab)));
        if (selected && focused(kPage))
            focus_ring(r.inset(2, 8), t::kRadiusControl);
        app.hits.push_back({kPage, r, static_cast<int>(p)});
        px = r.right();
    }
    return H;
}

}  // namespace simqvfr::launcher::ui::win

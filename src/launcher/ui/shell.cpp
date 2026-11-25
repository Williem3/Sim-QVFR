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
#include "../../profile/profiles.h"
#include "brand_logo.h"
#include "launcher_version.h"
#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace {

// Each tab's pages: one each (a tab of several would get a strip of them under the top bar).
const std::vector<Pane> kTabPages[kTabs] = {
    {Pane::Overview},
    {Pane::Picture},
    {Pane::Headset},
    {Pane::Game, Pane::LmuGame},  // the second only for LMU (pane_shown)
    {Pane::Developer},
};

// English, as identities; tab_label() and the top bar show them through tr().
constexpr const wchar_t* kTabLabels[kTabs] = {I18N(L"Overview"), I18N(L"Picture"), I18N(L"Headset"), I18N(L"Game"),
                                                   I18N(L"Developer")};

// A tab's colour (its shape, its underline): ink for Overview, then blue, red and yellow.
uint32_t tab_color(Tab tab) {
    switch (tab) {
    case Tab::Picture: return app.c->blue;
    case Tab::Headset: return app.c->red;
    case Tab::Game: return app.c->yellow;
    case Tab::Developer: return app.c->text_muted;
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
    case Tab::Developer: app.canvas.circle(cx, cy, s / 2, c, false, 1.6f); break;  // an outlined circle: not a player's
    }
}

// The logo (brand_logo.h): with `compact`, the app icon's tile, `w` x `w`, centred on (x, y); otherwise the SQVFR
// wordmark, its letters starting at x (in line with the games' abbreviations under it) and `w` wide, its baseline on y;
// its trail runs left of x. The rail is dark in both themes, so it always uses the on-dark colours.
void brand_mark(float x, float y, float w, bool compact) {
    Canvas& cv = app.canvas;
    const auto letters = [&](const char* sq, const char* vfr, float trail, bool trailAll, float x, float y, float box, float s) {
        // The motion-blur trail: faint copies of the letters, spread over the SVG's offset to the left.
        for (const float k : {0.7f, 1.0f, 1.3f}) {
            const Rect r{x - trail * k * s, y, box * s, box * s};
            cv.path(sq, r, box, col(0x2f6fb8, 0.2f), 0, true);
            if (trailAll)
                cv.path(vfr, r, box, col(0x2f6fb8, 0.2f), 0, true);
        }
        const Rect r{x, y, box * s, box * s};
        cv.path(sq, r, box, col(0xf2f4f7), 0, true);
        cv.path(vfr, r, box, col(0x5fe0d2), 0, true);
    };
    if (!compact) {  // the wordmark: its letters span x 55..289; its baseline is y 84 of its 308 x 120 units
        const float s = w / 234;
        letters(brand::kWordSq, brand::kWordVfr, brand::kWordTrail, true, x - 55 * s, y - 84 * s, 308, s);
        return;
    }
    const Rect tile{x - w / 2, y - w / 2, w, w};
    cv.fill(tile, col(0x111418), w * 0.22f);
    cv.push_clip(tile);
    letters(brand::kIconSq, brand::kIconVfr, brand::kIconTrail, false, tile.x, tile.y, 100, w / 100);
    cv.pop_clip();
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
    case ThemeChoice::Light: return tr(L"Theme: light. Click for dark.");
    case ThemeChoice::Dark: return tr(L"Theme: dark. Click to follow Windows.");
    default: return tr(L"Theme: following Windows. Click for light.");
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

std::wstring launch_word(LaunchState state) {
    switch (state) {
    case LaunchState::Starting: return tr(L"Starting…");
    case LaunchState::Running: return tr(L"Running");
    case LaunchState::NotLoaded: return tr(L"Not loaded yet");
    case LaunchState::NoLayer: return tr(L"No Sim-QVFR");
    case LaunchState::Crashed: return tr(L"Stopped");
    default: return tr(L"Closed");
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
        return first.title +
               (bad + warn > 1 ? L"  ·  " + trn(static_cast<long long>(bad + warn - 1), L"{count} more",
                                                  L"{count} more", {{L"count", std::to_wstring(bad + warn - 1)}})
                               : L"");
    };
    if (app.status == Status::Starting)
        return {3, tr(L"Starting…"), trf(L"Starting {game}…", {{L"game", game_name()}})};
    if (app.status == Status::Error)
        return {2, tr(L"Error"), app.error};
    if (!app.iniFound)
        return {2, tr(L"No settings file"), tr(L"sim-qvfr.ini not found next to the launcher.")};
    if (app.undoRestore)
        return {3, tr(L"Defaults restored · Undo"),
                trf(L"Defaults restored for {game}. Click or press Ctrl+Z to undo.", {{L"game", short_name()}}),
                kUndoBadge};
    if (launchFirst)
        return launch_status();
    if (const Check* b = first_with(app.checks, CheckTone::Bad))
        return {2,
                trn(static_cast<long long>(bad), L"{count} problem", L"{count} problems",
                    {{L"count", std::to_wstring(bad)}}),
                checks_text(*b), kChecksBadge};
    if (eye_warning())
        return {1, tr(L"No eye tracker"), tr(L"Eye tracker not detected")};
    if (app.runtime.known() && ro.overRuntimeLimit)
        return {1, tr(L"Too large"), tr(L"Larger than the runtime’s maximum image size")};
    if (const Check* w = first_with(app.checks, CheckTone::Warn))
        return {1, trf(L"{count} to check", {{L"count", std::to_wstring(warn)}}), checks_text(*w), kChecksBadge};
    if (app.live == Live::NextLaunch)
        return {1, tr(L"Partly live"), tr(L"Applied live up to the launch resolution · the rest at the next launch")};
    if (app.live == Live::Applied)
        return {0, tr(L"Applied live"), tr(L"Applied to the running game")};
    if (watching)
        return launch_status();
    if (app.update == Update::Available)
        return {3, app.installing ? tr(L"Installing…") : tr(L"Update available"),
                app.installing ? trf(L"Installing Sim-QVFR {version}…", {{L"version", app.release.tag}})
                               : trf(L"Sim-QVFR {version} is available", {{L"version", app.release.tag}}) +
                                     (installable() ? L" · " + std::wstring(tr(L"Install")) : L""),
                kUpdateBadge};
    return {0, tr(L"Ready"), tr(L"Ready to launch")};
}

uint32_t tone_color(int tone) {
    switch (tone) {
    case 0: return app.c->success;
    case 1: return app.c->warning;
    case 2: return app.c->danger;
    default: return app.c->label_secondary;
    }
}

}  // namespace

// `text` cut to `w` in `style`, with an ellipsis when it doesn't fit (a longer language's words in a fixed place).
std::wstring fit(const std::wstring& text, const t::TypeStyle& style, float w) {
    if (app.canvas.measure(text, style) <= w)
        return text;
    std::wstring s = text;
    while (!s.empty() && app.canvas.measure(s + L"…", style) > w)
        s.pop_back();
    return s.empty() ? std::wstring() : s + L"…";
}

uint32_t section_color(Tab tab) { return tab_color(tab); }

void section_shape(Tab tab, float cx, float cy, float size) { tab_shape(tab, cx, cy, size); }

// ---- Tabs and pages ----------------------------------------------------------------------------

int tab_count() { return app.s.developer ? kTabs : kTabs - 1; }

Tab tab_of(Pane pane) {
    for (int t = 0; t < kTabs; ++t)
        if (std::find(kTabPages[t].begin(), kTabPages[t].end(), pane) != kTabPages[t].end())
            return static_cast<Tab>(t);
    return Tab::Overview;
}

// A tab's name as shown (the current language).
const wchar_t* tab_label(Tab tab) { return tr(kTabLabels[static_cast<int>(tab)]); }

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
    if (compact)
        brand_mark(W / 2, top / 2, 40, true);
    else  // the abbreviations' left edge and width below; its baseline level with the top bar's tab labels (topbar()
          // centres them in the same height)
        brand_mark(16, (top - t::kTabLabel.lineHeight) / 2 + cv.baseline(t::kTabLabel), W - 16 - 8, false);
    // The bottom first: the blocks take what is left above it.
    // The About button: the kit's small control height; compact, two lines (the icon over the version number).
    const float aboutH = compact ? 4 + 12 + 2 + t::kRailName.lineHeight + 4 : 26, heartR = compact ? 14.0f : 16.0f;
    const float bottomH = t::kSpace4 + 2 * heartR + (compact ? 0 : t::kSpace2 + t::kRailName.lineHeight) + t::kSpace3 + aboutH +
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
        cv.text(fit(rail_abbreviation(game), abbrStyle, abbr.w), abbrStyle, abbr, ink, compact ? Align::Center : Align::Leading);
        if (names)
            cv.text(game.name, t::kRailName, {pad, ty + t::kRailAbbrev.lineHeight + 2, W - pad - 10, 2 * t::kRailName.lineHeight},
                    col(selected ? app.c->ink : app.c->rail_text_muted), Align::Leading, false, true);
        if (selected)  // the notch, pointing at the page
            cv.path("M0 0L8 8L0 16Z", {W, y + blockH / 2 - 8, 16, 16}, 16, col(app.c->yellow), 0, true);  // 8 x 16
        if (selected && focused(kGameTab))
            cv.stroke(block.inset(3, 3), col(app.c->ink), 2, t::kRadiusControl);
        app.hits.push_back({kGameTab, block, i, compact ? std::wstring(game.name) : std::wstring(), game.name, selected ? 1 : 0});
        y += blockH;
    }
    // Support on Ko-fi: a red circle with a heart.
    float by = H - bottomH + t::kSpace4;
    const float cx = W / 2;
    const Rect kofi{0, by, W, 2 * heartR + (compact ? 0 : t::kSpace2 + t::kRailName.lineHeight)};
    cv.circle(cx, by + heartR, heartR, col(app.c->red), true);
    if (app.pressed == kSupport && kofi.contains(app.mouseX, app.mouseY))  // held: a shade darker, as a button's
        cv.circle(cx, by + heartR, heartR, col(0, 0.12f), true);
    const float hs = heartR;
    cv.path("M8 13.5C8 13.5 2 9.9 2 5.9 2 4 3.4 2.75 5 2.75 6.3 2.75 7.4 3.5 8 4.6 8.6 3.5 9.7 2.75 11 2.75 12.6 2.75 14 4 14 5.9 "
            "14 9.9 8 13.5 8 13.5Z",
            {cx - hs / 2, by + heartR - hs / 2 + 1, hs, hs}, 16, col(app.c->on_red), 0, true);
    if (!compact) {
        const t::TypeStyle kofiStyle{11, 13, 700, false};
        cv.text(fit(tr(L"Support on Ko-fi"), kofiStyle, W - 2 * t::kSpace2), kofiStyle,
                {0, by + 2 * heartR + t::kSpace2, W, t::kRailName.lineHeight}, col(app.c->rail_text), Align::Center);
    }
    if (pointer_in(kofi))
        cv.circle(cx, by + heartR, heartR + 3, col(0xffffff, 0.5f), false, 1.5f);
    if (focused(kSupport))
        cv.stroke(kofi.inset(6, -3), col(app.c->rail_text), 2, t::kRadiusControl);
    app.hits.push_back({kSupport, kofi, -1, tr(L"Support Sim-QVFR on Ko-fi")});
    by += kofi.h + t::kSpace3;
    // The About button: an info icon and the version ("About · v1.2.3-beta" where it fits), a dot when an update is
    // available. A release's version without its commit ("1.0.5-beta (a78fc1f)"); a development build the release it is
    // headed for.
    const std::wstring built = widen(SIM_QVFR_VERSION);
    const std::wstring version =
        known_version(built) ? L"v" + built.substr(0, built.find(L' ')) : L"v" + widen(SIM_QVFR_RELEASE) + L" dev";
    const std::wstring number = version.substr(0, std::min(version.find(L'-'), version.find(L' ')));  // "v1.0.5"
    const Rect br{6, by, W - 12, aboutH};
    const Rect aboutHit{0, by, W, aboutH};  // the rail's width, 26 (compact 35) high: no smaller than the version's was
    const bool aboutHover = pointer_in(aboutHit);
    cv.fill(br, col(0xffffff, aboutHover ? 0.12f : 0.05f), t::kRadiusControl);
    cv.stroke(br.inset(0.5f, 0.5f), col(0xffffff, aboutHover ? 0.36f : 0.18f), 1, t::kRadiusControl);
    if (app.pressed == kVersion && aboutHit.contains(app.mouseX, app.mouseY))  // held: a shade darker, as a button's
        cv.fill(br, col(0, 0.16f), t::kRadiusControl);
    // The longest label that fits beside the icon; compact: the icon over the number (cut to the button).
    const float iconS = compact ? 12.0f : 14.0f, gap = 4.0f, pad = compact ? 4.0f : 7.0f;
    const float textW = compact ? br.w - 2 * pad : br.w - 2 * pad - iconS - gap;
    const std::wstring dot = L" · ";
    const std::wstring about = tr(L"About");
    std::wstring label;
    if (compact) {
        label = fit(number, t::kRailName, textW);
    } else {
        for (const std::wstring& l : {about + dot + version, about + dot + number, version, number})
            if (cv.measure(l, t::kRailName) <= textW) {
                label = l;
                break;
            }
        if (label.empty())
            label = fit(number, t::kRailName, textW);
    }
    const float labelW = label.empty() ? 0.0f : cv.measure(label, t::kRailName);
    const float lh = t::kRailName.lineHeight;
    // The icon's top-left: compact, centred over the number (4 from the top, 2 above it); else before the label.
    const float gx = compact ? br.x + (br.w - iconS) / 2 : br.x + (br.w - iconS - (label.empty() ? 0.0f : gap + labelW)) / 2;
    const float my = compact ? br.y + 4 + iconS / 2 : br.y + br.h / 2;
    const Color ink = col(app.c->rail_text);
    cv.circle(gx + iconS / 2, my, iconS / 2 - 0.75f, ink, false, 1.4f);  // the info icon: a ring, a stem and a dot
    cv.path("M8 7.25V11.75", {gx, my - iconS / 2, iconS, iconS}, 16, ink, 1.6f);
    cv.circle(gx + iconS / 2, my - iconS * 0.19f, iconS * 0.07f + 0.3f, ink, true);
    if (compact && !label.empty())
        cv.text(label, t::kRailName, {br.x, my + iconS / 2 + 2, br.w, lh}, ink, Align::Center);
    else if (!label.empty())
        cv.text(label, t::kRailName, {gx + iconS + gap, my - lh / 2, labelW + 2, lh}, ink, Align::Leading);
    if (app.update == Update::Available) {  // on the button's corner, ringed in the rail's colour
        cv.circle(br.right() - 2, br.y + 2, 5, col(app.c->rail), true);
        cv.circle(br.right() - 2, br.y + 2, 3.5f, col(app.c->yellow), true);
    }
    if (focused(kVersion))
        cv.stroke(br.inset(-3, -3), col(app.c->rail_text), 2, t::kRadiusControl + 2);
    app.hits.push_back({kVersion, aboutHit, -1,  // 26 high: above the smallest target (WCAG 2.5.8)
                        app.update == Update::Available
                            ? trf(L"Sim-QVFR {version} is available: About and updates",
                                  {{L"version", app.release.tag}})
                            : std::wstring(tr(L"About Sim-QVFR and updates"))});
}

// ---- Top bar -----------------------------------------------------------------------------------

// The top bar: the tabs at the left; the theme button, Find a setting, the profile menu, the status and Launch at the
// right. Narrow windows (or a language with longer words) drop the profile button's "Profile:", then tighten the tabs,
// then drop the game's name from Launch, then shorten the status, and last shorten the tabs' names (never a tab).
void topbar(float W) {
    Canvas& cv = app.canvas;
    const float x0 = rail_width(), H = t::kTopbarHeight, pad = t::kSpace4;
    cv.fill({x0, 0, W - x0, H}, col(app.c->surface));
    cv.line(x0, H - 0.5f, W, H - 0.5f, col(app.c->line), 1);
    // The right side's parts, from the right.
    const bool running = game_running();
    const std::wstring launchFull = running ? trf(L"{game} is running", {{L"game", short_name()}})
                                             : trf(L"Launch {game}", {{L"game", short_name()}});
    std::wstring launchLabel = launchFull;  // without the game's name when there's no room (its tip keeps it)
    float launchPad = 20;  // Launch's side padding
    const auto launch_width = [&] {
        return running ? button_width(launchLabel) : std::ceil(cv.measure(launchLabel, kPrimaryLabel)) + 16 + 10 + 2 * launchPad;
    };
    const StatusNow status = status_now();
    // The profile in use (a preset's name in the player's language), else Custom.
    const std::wstring profile = app.profileCurrent.empty() || !app.profileMatches ? std::wstring(tr(L"Custom"))
                                 : is_preset_name(app.profileCurrent)              ? tr(app.profileCurrent)
                                                                                   : app.profileCurrent;
    const float chevronW = 8 + 12;
    const float prefixW = std::ceil(cv.measure(tr(L"Profile: "), t::kBody));
    const float nameW = std::ceil(cv.measure(profile, t::kBodyStrong));
    const float statusTextW = std::ceil(cv.measure(status.word, t::kBodyMedium));
    // Tabs' widths at full spacing.
    const auto tabs_width = [&](float tabPad) {
        float w = 0;
        for (int i = 0; i < tab_count(); ++i)
            w += tabPad + 10 + 8 + std::ceil(cv.measure(tab_label(static_cast<Tab>(i)), t::kTabLabel)) + tabPad;
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
    if (tabs_width(tabPad) + right_width() > room)
        launchLabel = running ? tr(L"Game running") : tr(L"Launch");
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
    if (launchLabel != launchFull && !app.hits.empty() && app.hits.back().id == kLaunch)
        app.hits.back().tip = launchFull;
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
        app.hits.push_back({status.link, sr, -1, status.text != status.word ? status.text : std::wstring(), status.text});
    }
    // The profile menu: the profile in use (Custom when the settings differ from every profile), a chevron.
    const float profileW = 12 + (withPrefix ? prefixW : 0) + nameW + chevronW + 4;
    x -= 16 + profileW;
    {
        const Rect pr{x, (H - 34) / 2, profileW, 34};
        // Open: the menu shows under this button (not under the Overview's Custom segment).
        const bool open = app.popup.open && app.popup.owner == kProfileMenu && app.popup.opener == kProfileMenu;
        cv.fill(pr, col(open || pointer_in(pr) ? app.c->control_hover : app.c->control_bg), t::kRadiusControl);
        float tx = pr.x + 12;
        if (withPrefix) {
            cv.text(tr(L"Profile: "), t::kBody, {tx, pr.y, prefixW + 2, pr.h}, col(app.c->label_secondary));
            tx += prefixW;
        }
        cv.text(profile, t::kBodyStrong, {tx, pr.y, nameW + 2, pr.h}, col(app.c->label));
        cv.path("M1.5 4.5L5 8l3.5-3.5", {pr.right() - 8 - 12, pr.y + (pr.h - 12) / 2, 10, 12}, 10, col(app.c->label_secondary), 1.5f);
        if (focused(kProfileMenu))
            focus_ring(pr, t::kRadiusControl);
        app.hits.push_back({kProfileMenu, pr, -1, tr(L"Profiles, saving and restoring defaults")});
    }
    // Find a setting and the theme button.
    x -= 8 + 32;
    {
        const Rect r{x, (H - 32) / 2, 32, 32};
        icon_button(kSearch, r, tr(L"Find a setting (Ctrl+K)"));
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
    // The tabs: a shape and the name; the selected one in full colour with a rule under it in the tab's colour. Still
    // too wide, each name is cut to an equal share of the room ("…"; the full name as its tip and accessible name).
    const Tab current = tab_of(app.pane);
    const float tabsRoom = x - 16 - (x0 + pad);
    const float labelMax = tabs_width(tabPad) > tabsRoom ? std::max(24.0f, tabsRoom / tab_count() - (2 * tabPad + 18)) : 1e9f;
    float tx = x0 + pad;
    for (int i = 0; i < tab_count(); ++i) {
        const Tab tab = static_cast<Tab>(i);
        const std::wstring full = tab_label(tab);
        const std::wstring label = std::ceil(cv.measure(full, t::kTabLabel)) > labelMax ? fit(full, t::kTabLabel, labelMax) : full;
        const float lw = std::ceil(cv.measure(label, t::kTabLabel));
        const Rect r{tx, 0, tabPad + 10 + 8 + lw + tabPad, H};
        const bool selected = tab == current, over = pointer_in(r);
        tab_shape(tab, r.x + tabPad + 5, H / 2, 10);
        cv.text(label, t::kTabLabel, {r.x + tabPad + 18, 0, lw + 4, H},
                col(selected || over ? app.c->text : app.c->text_muted));
        if (selected || over)  // under the pointer, a faint rule: the selected one's in full
            cv.fill({r.x + tabPad - 4, H - 3, r.w - 2 * tabPad + 8, 3}, col(tab_color(tab), selected ? 1.0f : 0.3f));
        if (selected && focused(kTopTab))
            focus_ring(r.inset(4, 12), t::kRadiusControl);
        app.hits.push_back({kTopTab, r, i, label != full ? full : std::wstring(), full, selected ? 1 : 0});
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
    if (tab_pages(tab).size() < 2)
        return 0;  // a tab of one page (its cards): no strip
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
        if (selected || over)  // as the top bar's tabs: a faint rule under the pointer
            cv.fill({r.x + 12, y + H - 3, lw, 3}, col(tab_color(tab), selected ? 1.0f : 0.3f));
        if (selected && focused(kPage))
            focus_ring(r.inset(2, 8), t::kRadiusControl);
        app.hits.push_back({kPage, r, static_cast<int>(p)});
        px = r.right();
    }
    return H;
}

// The undo bar: what the last change changed and its Undo, at the page's bottom for a few seconds after it (kUndoMs);
// after an undo, what was undone.
void undo_bar(float W, float H) {
    if (app.undoText.empty() || GetTickCount64() >= app.undoUntil || app.sheet.open || app.panel != PanelKind::None)
        return;
    Canvas& cv = app.canvas;
    const float left = rail_width(), pad = 16, h = 44;
    const bool canUndo = app.undoChange.has_value();
    const std::wstring undo = tr(L"Undo");
    const float uw = canUndo ? std::ceil(cv.measure(undo, t::kBodyStrong)) + 24 : 0;
    const float tw = std::min(std::ceil(cv.measure(app.undoText, t::kBody)) + 2, W - left - 64 - uw - 3 * pad);
    const float w = pad + tw + (canUndo ? pad + uw : 0) + pad;
    const Rect bar{std::round(left + (W - left - w) / 2), H - h - 24, w, h};
    const uint32_t bg = app.dark ? app.c->surface_3 : app.c->ink, fg = app.dark ? app.c->label : app.c->rail_text;
    cv.fill(bar, col(bg), t::kRadiusGroup);
    cv.text(fit(app.undoText, t::kBody, tw), t::kBody, {bar.x + pad, bar.y, tw, h}, col(fg));  // narrow: cut, read whole
    reader_text(bar, app.undoText);
    if (canUndo) {
        const Rect u{bar.right() - pad - uw, bar.y + 6, uw, h - 12};
        if (pointer_in(u))
            cv.fill(u, col(fg, 0.12f), t::kRadiusControl);
        cv.text(undo, t::kBodyStrong, u, col(app.c->yellow), Align::Center);
        if (focused(kUndoChange))
            focus_ring(u, t::kRadiusControl);
        app.hits.push_back(
            {kUndoChange, u, -1, tr(L"Undo (Ctrl+Z)"), trf(L"Undo {change}", {{L"change", app.undoText}})});
    }
}

}  // namespace simqvfr::launcher::ui::win

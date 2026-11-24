// The Games card (which games are in the rail, each one's colour), the colour panel, the game's own options, and the
// Game tab's page of cards.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../game_tabs.h"
#include "../../games/game.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

// ---- Games ------------------------------------------------------------------------------------

// Which games are installed (launcher.h game_installed), by id: looked up when the window opens, comes back to the front
// or a game's folder is chosen.
void refresh_installed() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    for (std::size_t i = 0; i < count; ++i)
        app.installed[games::wide_id(*all[i])] = game_installed(*all[i]);
}

bool installed(const games::GameInfo& game) {
    const auto it = app.installed.find(games::wide_id(game));
    return it == app.installed.end() || it->second;
}

// A game has a tab when it is installed and not hidden, or when the user gave it one (one Steam doesn't find, installed
// elsewhere: its folder is chosen in Launch checks).
bool game_shown(const games::GameInfo& game) {
    return installed(game) ? !game_hidden(app.s.hiddenGames, games::wide_id(game)) : game_hidden(app.s.shownGames, games::wide_id(game));
}

int shown_game_count() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    int shown = 0;
    for (std::size_t i = 0; i < count; ++i)
        shown += game_shown(*all[i]) ? 1 : 0;
    return shown;
}

// The game whose colour the colour drop-down and panel edit: the one whose colour button opened it, else the selected
// game.
int color_game_index() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    if (app.colorGame >= 0 && static_cast<std::size_t>(app.colorGame) < count)
        return app.colorGame;
    for (std::size_t i = 0; i < count; ++i)
        if (all[i] == &games::launcher_game())
            return static_cast<int>(i);
    return 0;
}

const games::GameInfo& color_game() {
    std::size_t count = 0;
    return *games::all_games(count)[color_game_index()];
}

// The wheel's hue, saturation and brightness from the edited game's colour (its own, else its palette's fill).
void load_color_edit() {
    const int index = color_game_index();
    if (app.colorLoaded == index)
        return;
    app.colorLoaded = index;
    const auto it = app.s.gameColors.find(games::wide_id(color_game()));
    const uint32_t rgb = it != app.s.gameColors.end() ? it->second : builtin_accent(color_game()).accent_fill;
    float value = 1;
    rgb_hsv(rgb, app.colorHue, app.colorSat, value);
    app.colorValue = static_cast<int>(std::lround(value * 100));
}

void apply_color_edit() {
    app.s.gameColors[games::wide_id(color_game())] = hsv_rgb(app.colorHue, app.colorSat, app.colorValue / 100.0f);
    apply_theme();  // the selected game's accent may be the one changed
}

// A point on the wheel: its angle the hue, its distance from the centre the saturation (the rim: full).
void set_color_from(float x, float y) {
    const Rect& r = app.colorWheel;
    if (r.w <= 0)
        return;
    const float radius = r.w / 2, dx = (x - (r.x + radius)) / radius, dy = (y - (r.y + radius)) / radius;
    app.colorSat = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
    float hue = std::atan2(-dy, dx) * 180.0f / 3.14159265f;
    app.colorHue = hue < 0 ? hue + 360 : hue;
    apply_color_edit();
    changed(false);  // saved once the pointer lets go (WM_LBUTTONUP), as a slider is
}

namespace {

// The colour wheel at `r` (square): hue round it, saturation outward, at the brightness set; a ring marks the colour.
void color_wheel(const Rect& r) {
    static std::vector<uint32_t> pixels;
    static int madeSize = -1, madeValue = -1;
    const int n = std::max(16, static_cast<int>(std::lround(r.w * app.canvas.dpi_scale())));
    if (n != madeSize || app.colorValue != madeValue) {
        madeSize = n;
        madeValue = app.colorValue;
        pixels.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0);
        const float half = n / 2.0f, value = app.colorValue / 100.0f;
        for (int py = 0; py < n; ++py)
            for (int px = 0; px < n; ++px) {
                const float dx = (px + 0.5f - half) / half, dy = (py + 0.5f - half) / half;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float alpha = std::clamp((1.0f - d) * half + 0.5f, 0.0f, 1.0f);  // a smooth rim
                if (alpha <= 0)
                    continue;
                float hue = std::atan2(-dy, dx) * 180.0f / 3.14159265f;
                const uint32_t rgb = hsv_rgb(hue < 0 ? hue + 360 : hue, std::min(d, 1.0f), value);
                const auto pre = [alpha](uint32_t c) { return static_cast<uint32_t>(std::lround(c * alpha)); };
                pixels[static_cast<std::size_t>(py) * n + px] = static_cast<uint32_t>(std::lround(alpha * 255)) << 24 |
                                                                pre((rgb >> 16) & 0xff) << 16 | pre((rgb >> 8) & 0xff) << 8 |
                                                                pre(rgb & 0xff);
            }
    }
    app.canvas.image(r, pixels, n, n, static_cast<uint64_t>(n) * 1000 + static_cast<uint64_t>(app.colorValue) + 1);
    const float radius = r.w / 2, angle = app.colorHue * 3.14159265f / 180.0f;
    const float mx = r.x + radius + std::cos(angle) * app.colorSat * radius, my = r.y + radius - std::sin(angle) * app.colorSat * radius;
    app.canvas.circle(mx, my, 7, col(hsv_rgb(app.colorHue, app.colorSat, app.colorValue / 100.0f)), true);
    app.canvas.circle(mx, my, 7.5f, col(0xffffff), false, 2);
    app.canvas.circle(mx, my, 9, col(0x000000, 0.35f), false, 1);
    app.colorWheel = r;
    app.hits.push_back({kColorWheel, r});
}

}  // namespace

// A game's colour among the presets (a game's colour drop-down on the Games card; Custom… for any other): apart from the games' own colours
// (theme_tokens.h kGamePalettes) and the sections' blue, red and yellow, so a game given one stands out.
const uint32_t kColorPresets[] = {0x7a3db8, 0xc0267a, 0x4d7c0f, 0x8b5e34, 0x0e7490, 0x4b5563};
const wchar_t* const kColorPresetNames[] = {I18N(L"Purple"), I18N(L"Magenta"), I18N(L"Olive"),
                                            I18N(L"Brown"),  I18N(L"Teal blue"), I18N(L"Slate")};
static_assert(std::size(kColorPresets) == 6, "six presets");

// A preset chosen on the Games card: the edited game's colour, at once.
void use_color_preset(int preset) {
    if (preset < 0 || preset >= static_cast<int>(std::size(kColorPresets)))
        return;
    app.s.gameColors[games::wide_id(color_game())] = kColorPresets[preset];
    app.colorLoaded = -2;
    load_color_edit();
    apply_theme();  // the selected game's accent may be the one changed
    changed();
}

namespace {

// A game's note on the Games card: why its switch is locked, that Steam didn't find it, or that it's hidden.
std::wstring game_note(const games::GameInfo& game) {
    const bool shown = game_shown(game), locked = shown && shown_game_count() <= 1, found = installed(game);
    return locked  ? tr(L"The last game in the rail: show another to hide it.")
           : !found ? shown ? tr(L"Not found by Steam: choose its folder in Launch checks.")
                            : tr(L"Not found by Steam. Turn it on if it’s installed elsewhere.")
           : shown  ? L""
                    : tr(L"Hidden. Its settings are kept for when you show it again.");
}

// A game's colour button (game_row), and where its name starts after it.
constexpr float kSwatchW = 40, kSwatchH = 28;
constexpr float kGameTextX = kSwatchW + t::kSpace3;

// A game's row's height in a card `w` wide: its name, and its note under it.
float game_row_height(size_t i, float w) {
    std::size_t count = 0;
    const std::wstring desc = game_note(*games::all_games(count)[i]);
    const float tw = w - kGameTextX - kSwitchW - t::kSpace4;
    return std::max(28.0f, t::kBody.lineHeight + (desc.empty() ? 0 : 2 + app.canvas.measure_height(desc, t::kCallout, tw)));
}

// A game's row on the Games card: its colour (click: the drop-down of its colours), its name and where it stands, its switch.
void game_row(const Rect& r, size_t i) {
    std::size_t count = 0;
    const games::GameInfo* game = games::all_games(count)[i];
    const bool shown = game_shown(*game), locked = shown && shown_game_count() <= 1, found = installed(*game);
    const std::wstring desc = game_note(*game);
    Canvas& cv = app.canvas;
    const t::Accent a = game_accent(*game);
    // Its colour as a small pop-up button (popup_button's fill and edge): the dot, then a chevron for the drop-down of
    // its colours it opens (open_color_menu); hovered while that shows.
    const Rect swatch{r.x, r.y + (r.h - kSwatchH) / 2, kSwatchW, kSwatchH};
    const bool open = app.popup.open && app.popup.owner == kColorGame && static_cast<int>(i) == color_game_index();
    cv.fill(swatch, col(open || pointer_in(swatch) ? app.c->control_hover : app.c->control_bg), t::kRadiusControl);
    cv.stroke(swatch.inset(0.5f, 0.5f), col(app.c->control_border), 1, t::kRadiusControl);
    cv.circle(swatch.x + 13, swatch.y + kSwatchH / 2, 7, col(a.accent_fill), true);
    cv.path("M1.5 4.5L5 8l3.5-3.5", {swatch.x + 25, swatch.y + (kSwatchH - 12) / 2, 10, 12}, 10, col(app.c->label_secondary),
            1.5f);
    if (focused(kColorGame) && app.focusOption == static_cast<int>(i))
        focus_ring(swatch, t::kRadiusControl);
    app.hits.push_back({kColorGame, swatch, static_cast<int>(i),
                        trf(L"Change {game}’s colour", {{L"game", game->shortName}}), tr(L"Change its colour")});
    const float tx = r.x + kGameTextX, tw = r.w - kGameTextX - kSwitchW - t::kSpace4;
    const float dh = desc.empty() ? 0 : cv.measure_height(desc, t::kCallout, tw);
    const float ty = r.y + (r.h - t::kBody.lineHeight - (dh > 0 ? 2 + dh : 0)) / 2;
    const std::wstring name = fit(game->name, t::kBody, tw);  // a long name in a narrow card: cut before the switch
    cv.text(name, t::kBody, {tx, ty, tw, t::kBody.lineHeight}, col(app.c->label));
    if (!found) {
        const float nx = tx + std::ceil(cv.measure(name, t::kBody)) + t::kSpace2;
        badge(nx, ty, 3, tr(L"Not found"), std::max(0.0f, tx + tw - nx));
    }
    if (dh > 0)
        cv.text(desc, t::kCallout, {tx, ty + t::kBody.lineHeight + 2, tw, dh + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    const Rect sw{r.right() - kSwitchW, r.y + (r.h - kSwitchH) / 2, kSwitchW, kSwitchH};
    // Its hit is the row from the name on, so the switch answers the pointer anywhere there (a checkbox row's look).
    const Rect hit{tx, r.y, r.right() - tx, r.h};
    const int state = locked                                                             ? 0
                      : app.pressed == kGameShown && hit.contains(app.mouseX, app.mouseY) ? 2
                      : pointer_in(hit)                                                    ? 1
                                                                                           : 0;
    switch_track(sw, shown, locked, state);
    if (!locked) {
        if (focused(kGameShown) && app.focusOption == static_cast<int>(i))
            focus_ring(sw, t::kRadiusSwitch);
        app.hits.push_back({kGameShown, hit, static_cast<int>(i), L"", tr(L"In the rail"), shown ? 1 : 0});
    }
    app.drawnLabels.push_back({game->name, app.drawingSection, r});
    if (i == 0)  // Find a setting's "Colour": the games' rows, where each game's colour button is
        app.drawnLabels.push_back({tr(L"Colour"), app.drawingSection, r});
}

// The colour drop-down's actions besides a preset (its index in kColorPresets, 0 and up).
enum ColorAction { kColorActOwn = -1, kColorActCustom = -2 };

}  // namespace

// A game's colour button: the drop-down of its colours under it, the one it has checked: the game's own, the presets,
// a colour of its own (from the wheel) when it has one, then Custom… (the colour panel). It edits that game's colour.
void open_color_menu(int game) {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    if (game < 0 || static_cast<std::size_t>(game) >= count)
        return;
    app.colorGame = game;
    app.colorLoaded = -2;
    load_color_edit();
    const games::GameInfo& g = *all[game];
    const auto custom = app.s.gameColors.find(games::wide_id(g));
    const bool hasCustom = custom != app.s.gameColors.end();
    Popup& p = app.popup;
    p = {};
    p.owner = kColorGame;
    for (const Hit& h : app.hits)
        if (h.id == kColorGame && h.option == game)
            p.anchor = h.r;
    const auto add = [&p](PopupRow kind, const std::wstring& text, int action, uint32_t rgb, bool chosen) {
        if (chosen)
            p.selected = static_cast<int>(p.items.size());
        p.items.push_back(text);
        p.kinds.push_back(kind);
        p.actions.push_back(action);
        p.swatches.push_back(rgb);
    };
    p.selected = -1;
    add(PopupRow::Item, trf(L"{game}’s own colour", {{L"game", g.shortName}}), kColorActOwn,
        builtin_accent(g).accent_fill, !hasCustom);
    for (int i = 0; i < static_cast<int>(std::size(kColorPresets)); ++i)
        add(PopupRow::Item, tr(kColorPresetNames[i]), i, kColorPresets[i], hasCustom && custom->second == kColorPresets[i]);
    if (hasCustom && std::find(std::begin(kColorPresets), std::end(kColorPresets), custom->second) == std::end(kColorPresets))
        add(PopupRow::Item, tr(L"Its own colour"), kColorActCustom, custom->second, true);
    add(PopupRow::Separator, L"", 0, kNoSwatch, false);
    add(PopupRow::Item, tr(L"Custom…"), kColorActCustom, kNoSwatch, false);
    p.hover = p.selected;
    p.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// An item of the colour drop-down chosen: the game's own colour back (kColorReset), a preset, or the colour panel
// (kColorCustom), each as its own control does; app.colorGame is the game.
void choose_color_menu(int option) {
    if (option < 0 || option >= static_cast<int>(app.popup.actions.size()))
        return;
    const int action = app.popup.actions[static_cast<size_t>(option)];
    if (action == kColorActOwn)
        activate(kColorReset, -1);
    else if (action == kColorActCustom)
        activate(kColorCustom, -1);
    else
        use_color_preset(action);
}

// The Games card: which games are in the rail, and each one's colour (its marker on the rail).
Card games_card() {
    Card c{Tab::Game, tr(L"Games in the rail")};
    c.body = [](Body& b) {
        std::size_t count = 0;
        games::all_games(count);
        load_color_edit();
        for (size_t i = 0; i < count; ++i)
            b.custom(game_row_height(i, b.width()), [i](const Rect& r) { game_row(r, i); });
        b.note(tr(L"A game’s colour marks it on the rail. Installed games are in the rail unless you hide them."));
    };
    return c;
}

// Whether the selected game can be raced online with Sim-QVFR, in one sentence the Overview and the Online racing card
// share; `tone` is badge's (1 a warning, 3 plain).
std::wstring online_status(int& tone) {
    const games::GameInfo& g = games::launcher_game();
    if (g.antiCheatStart && app.s.antiCheat) {
        tone = 1;
        return tr(L"Online at your own risk: the anti-cheat can ban you");
    }
    tone = 3;
    return tr(L"Offline sessions only");
}

// Online racing, for every game: whether Sim-QVFR can be used online, and for a game with an anti-cheat bootstrapper (LMU)
// the switch that starts it with its anti-cheat, at the player's risk.
Card online_card() {
    Card c{Tab::Game, tr(L"Online racing")};
    c.body = [](Body& b) {
        const games::GameInfo& g = games::launcher_game();
        int tone = 3;
        b.chip(tone == 3 ? 3 : tone, online_status(tone));
        if (!g.antiCheatStart) {
            b.note(trf(L"Sim-QVFR runs inside {game} and changes how it renders. Use it in offline sessions, and don’t "
                       L"report problems you see with it to the game’s developers.",
                       {{L"game", g.shortName}}));
            return;
        }
        const bool on = app.s.antiCheat;
        b.toggle(kAntiCheat, tr(L"Run with anti-cheat"), on,
                 on ? trf(L"Starts {game} through its anti-cheat, as Steam does, with Sim-QVFR running.",
                          {{L"game", g.shortName}})
                    : trf(L"Off: {game} starts without its anti-cheat, so online servers won’t let you in. Turning it "
                          L"on asks you to accept the risk first.",
                          {{L"game", g.shortName}}),
                 false, Applies::NextLaunch);
        if (on)  // the risk, only while it's on (accepted when it was turned on)
            b.note(tr(L"Sim-QVFR runs inside the game, and the anti-cheat may kick you or ban your account for it. You "
                      L"accepted that risk: Sim-QVFR and its developer aren’t responsible for kicks or bans."));
    };
    if (games::launcher_game().antiCheatStart)
        c.reset = [](Settings& s) { s.antiCheat = Settings{}.antiCheat; };
    return c;
}

// The colour panel (Custom…): the wheel and its brightness, how the game looks with it on the rail, its own colour back.
float color_panel(float x, float y, float w, bool draw) {
    Body b(x, y, w, draw);
    b.custom(184, [](const Rect& r) {
        color_wheel({r.x, r.y, r.h, r.h});
        const games::GameInfo& game = color_game();
        const t::Accent a = game_accent(game);
        const float px = r.x + r.h + t::kSpace6, pw = std::min(200.0f, r.right() - px);
        if (pw < 100)
            return;
        app.canvas.text(tr(L"On the rail"), t::kSubheadline, {px, r.y + 20, pw, 18}, col(app.c->label_secondary));
        const Rect block{px, r.y + 44, pw, 64};
        app.canvas.fill(block, col(app.c->rail));
        app.canvas.fill({block.x, block.y, 4, block.h}, col(a.accent_fill));
        app.canvas.text(rail_abbreviation(game), t::kRailAbbrev, {block.x + 16, block.y + 10, pw - 20, 24}, col(app.c->rail_text));
        app.canvas.text(fit(game.name, t::kRailName, pw - 20), t::kRailName, {block.x + 16, block.y + 36, pw - 20, 16},
                        col(app.c->rail_text_muted));
    });
    b.slider({kColorValue, tr(L"Accent brightness"), app.colorValue});
    b.actions({{kColorReset, tr(L"Use its own colour")}, {kPanelClose, tr(L"Done"), Btn::Primary}});
    return b.height();
}

// The game's main thread on a core of its own ([cpu] isolate_main_thread, probe/cpu_isolation.h): every game.
Card cpu_card() {
    Card c{Tab::Game, L"CPU", Applies::Live};
    c.body = [](Body& b) {
        b.toggle(kCpuIsolateMain, tr(L"Give the main thread a core of its own"), app.s.cpuIsolateMain,
                 app.s.cpuIsolateMain
                     ? tr(L"On: the game thread that finishes each frame runs alone on one core, and the game’s other "
                          L"threads (its physics among them) use the rest, so none slows it down by sharing that core.")
                     : tr(L"Off (the default): the game’s threads share every core, as Windows places them."));
        b.note(tr(L"Only the game’s own threads move: no Windows setting or other program changes. It applies on CPUs "
                  L"with 6 or more cores and two threads per core, and stays off if another tool (such as Process "
                  L"Lasso) already set the game’s CPU sets or affinity. Try it on and watch your frames; the headset "
                  L"menu’s Game page changes it too."));
    };
    c.reset = [](Settings& s) { s.cpuIsolateMain = Settings{}.cpuIsolateMain; };
    return c;
}

// The Game tab's cards, in order: what everyone needs first (the checks, racing online, getting help), the game's CPU,
// then the last sessions and the games, the OpenXR layers (for troubleshooting) last.
float game_page(float x, float y, float w, bool draw) {
    const std::vector<Card> cards = {checks_card(), online_card(), help_card(), cpu_card(),
                                     diagnostics_card(), games_card(), layers_card()};
    return card_grid(cards, x, y, w, draw);
}

}  // namespace simqvfr::launcher::ui::win

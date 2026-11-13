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
        app.installed[game_id(*all[i])] = game_installed(*all[i]);
}

bool installed(const games::GameInfo& game) {
    const auto it = app.installed.find(game_id(game));
    return it == app.installed.end() || it->second;
}

// A game has a tab when it is installed and not hidden, or when the user gave it one (one Steam doesn't find, installed
// elsewhere: its folder is chosen in Launch checks).
bool game_shown(const games::GameInfo& game) {
    return installed(game) ? !game_hidden(app.s.hiddenGames, game_id(game)) : game_hidden(app.s.shownGames, game_id(game));
}

int shown_game_count() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    int shown = 0;
    for (std::size_t i = 0; i < count; ++i)
        shown += game_shown(*all[i]) ? 1 : 0;
    return shown;
}

// The game whose colour the colour row and panel edit: the one picked by its swatch, else the selected game.
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
    const auto it = app.s.gameColors.find(game_id(color_game()));
    const uint32_t rgb = it != app.s.gameColors.end() ? it->second : builtin_accent(color_game()).accent_fill;
    float value = 1;
    rgb_hsv(rgb, app.colorHue, app.colorSat, value);
    app.colorValue = static_cast<int>(std::lround(value * 100));
}

void apply_color_edit() {
    app.s.gameColors[game_id(color_game())] = hsv_rgb(app.colorHue, app.colorSat, app.colorValue / 100.0f);
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

// A game's colour among the presets (the Games card's swatches; Custom… for any other): apart from the games' own colours
// (theme_tokens.h kGamePalettes) and the sections' blue, red and yellow, so a game given one stands out.
const uint32_t kColorPresets[] = {0x7a3db8, 0xc0267a, 0x4d7c0f, 0x8b5e34, 0x0e7490, 0x4b5563};
const wchar_t* const kColorPresetNames[] = {L"Purple", L"Magenta", L"Olive", L"Brown", L"Teal blue", L"Slate"};
static_assert(std::size(kColorPresets) == 6, "six presets");

// A preset chosen on the Games card: the edited game's colour, at once.
void use_color_preset(int preset) {
    if (preset < 0 || preset >= static_cast<int>(std::size(kColorPresets)))
        return;
    app.s.gameColors[game_id(color_game())] = kColorPresets[preset];
    app.colorLoaded = -2;
    load_color_edit();
    apply_theme();  // the selected game's accent may be the one changed
    changed();
}

namespace {

// A game's note on the Games card: why its switch is locked, that Steam didn't find it, or that it's hidden.
std::wstring game_note(const games::GameInfo& game) {
    const bool shown = game_shown(game), locked = shown && shown_game_count() <= 1, found = installed(game);
    return locked  ? L"The last game in the rail: show another to hide it."
           : !found ? shown ? L"Not found by Steam: choose its folder in Launch checks."
                            : L"Not found by Steam. Turn it on if it’s installed elsewhere."
           : shown  ? L""
                    : L"Hidden. Its settings are kept for when you show it again.";
}

constexpr float kGameTextX = 36;

// A game's row's height in a card `w` wide: its name, and its note under it.
float game_row_height(size_t i, float w) {
    std::size_t count = 0;
    const std::wstring desc = game_note(*games::all_games(count)[i]);
    const float tw = w - kGameTextX - kSwitchW - t::kSpace4;
    return std::max(28.0f, t::kBody.lineHeight + (desc.empty() ? 0 : 2 + app.canvas.measure_height(desc, t::kCallout, tw)));
}

// A game's row on the Games card: its colour (click: its colour below), its name and where it stands, its switch.
void game_row(const Rect& r, size_t i) {
    std::size_t count = 0;
    const games::GameInfo* game = games::all_games(count)[i];
    const bool shown = game_shown(*game), locked = shown && shown_game_count() <= 1, found = installed(*game);
    const std::wstring desc = game_note(*game);
    Canvas& cv = app.canvas;
    const t::Accent a = game_accent(*game);
    const Rect swatch{r.x, r.y + (r.h - 22) / 2, 22, 22};
    cv.circle(swatch.x + 11, swatch.y + 11, 9, col(a.accent_fill), true);
    if (static_cast<int>(i) == color_game_index())  // the game the colour row edits
        cv.circle(swatch.x + 11, swatch.y + 11, 11.5f, col(app.c->label), false, 1.5f);
    if (focused(kColorGame) && app.focusOption == static_cast<int>(i))
        focus_ring(swatch, 11);
    app.hits.push_back({kColorGame, swatch.inset(-4, -4), static_cast<int>(i), L"Change " + std::wstring(game->shortName) + L"’s colour",
                        L"Change its colour"});
    const float tx = r.x + kGameTextX, tw = r.w - kGameTextX - kSwitchW - t::kSpace4;
    const float dh = desc.empty() ? 0 : cv.measure_height(desc, t::kCallout, tw);
    const float ty = r.y + (r.h - t::kBody.lineHeight - (dh > 0 ? 2 + dh : 0)) / 2;
    cv.text(game->name, t::kBody, {tx, ty, tw, t::kBody.lineHeight}, col(app.c->label));
    if (!found) {
        const float nx = tx + std::ceil(cv.measure(game->name, t::kBody)) + t::kSpace2;
        badge(nx, ty, 3, L"Not found", std::max(0.0f, tx + tw - nx));
    }
    if (dh > 0)
        cv.text(desc, t::kCallout, {tx, ty + t::kBody.lineHeight + 2, tw, dh + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    const Rect sw{r.right() - kSwitchW, r.y + (r.h - kSwitchH) / 2, kSwitchW, kSwitchH};
    switch_track(sw, shown, locked);
    if (!locked) {
        if (focused(kGameShown) && app.focusOption == static_cast<int>(i))
            focus_ring(sw, t::kRadiusSwitch);
        app.hits.push_back({kGameShown, {tx, r.y, r.right() - tx, r.h}, static_cast<int>(i), L"", L"In the rail", shown ? 1 : 0});
    }
    app.drawnLabels.push_back({game->name, app.drawingSection, r});
}

// The colour row: the edited game's own colour (Default), the presets, Custom… (the panel with the wheel).
void color_row(const Rect& r) {
    Canvas& cv = app.canvas;
    const games::GameInfo& game = color_game();
    const auto custom = app.s.gameColors.find(game_id(game));
    const bool hasCustom = custom != app.s.gameColors.end();
    const std::wstring label = L"Colour for " + std::wstring(game.shortName);
    cv.text(label, t::kBody, {r.x, r.y, r.w, 20}, col(app.c->label));
    app.drawnLabels.push_back({L"Colour", app.drawingSection, r});
    float x = r.x;
    const float y = r.y + 28, d = 26;
    const auto swatch = [&](Ctl id, int option, uint32_t rgb, bool chosen, const std::wstring& tip) {
        const Rect s{x, y, d, d};
        cv.circle(s.x + d / 2, s.y + d / 2, d / 2 - 2, col(rgb), true);
        if (chosen)
            cv.circle(s.x + d / 2, s.y + d / 2, d / 2 + 1, col(app.c->label), false, 2);
        if (focused(id) && (option < 0 || app.focusOption == option))
            focus_ring(s, d / 2);
        app.hits.push_back({id, s.inset(-2, -2), option, tip, tip, chosen ? 1 : 0});
        x += d + 10;
    };
    swatch(kColorReset, -1, builtin_accent(game).accent_fill, !hasCustom, std::wstring(game.shortName) + L"’s own colour");
    for (int p = 0; p < static_cast<int>(std::size(kColorPresets)); ++p)
        swatch(kColorPreset, p, kColorPresets[p], hasCustom && custom->second == kColorPresets[p], kColorPresetNames[p]);
    const bool other = hasCustom && std::find(std::begin(kColorPresets), std::end(kColorPresets), custom->second) == std::end(kColorPresets);
    const float bw = button_width(L"Custom…");
    if (other)  // a colour of its own: shown before Custom…
        swatch(kColorCustom, -1, custom->second, true, L"Its own colour");
    button(kColorCustom, {std::min(x, r.right() - bw), y - 4, bw, t::kControlHeight}, L"Custom…", Btn::Secondary);
}

}  // namespace

// The Games card: which games are in the rail, and each one's colour (its marker on the rail).
Card games_card() {
    Card c{Tab::Game, L"Games in the rail"};
    c.body = [](Body& b) {
        std::size_t count = 0;
        games::all_games(count);
        load_color_edit();
        for (size_t i = 0; i < count; ++i)
            b.custom(game_row_height(i, b.width()), [i](const Rect& r) { game_row(r, i); });
        b.divider();
        b.custom(28 + 26, color_row);
        b.note(L"A game’s colour marks it on the rail. Installed games are in the rail unless you hide them.");
    };
    return c;
}

// Whether the selected game can be raced online with Sim-QVFR, in one sentence the Overview and the Online racing card
// share; `tone` is badge's (1 a warning, 3 plain).
std::wstring online_status(int& tone) {
    const games::GameInfo& g = games::launcher_game();
    if (g.antiCheatStart && app.s.antiCheat) {
        tone = 1;
        return L"Online at your own risk: the anti-cheat can ban you";
    }
    tone = 3;
    return L"Offline sessions only";
}

// Online racing, for every game: whether Sim-QVFR can be used online, and for a game with an anti-cheat bootstrapper (LMU)
// the switch that starts it with its anti-cheat, at the player's risk.
Card online_card() {
    Card c{Tab::Game, L"Online racing"};
    c.body = [](Body& b) {
        const games::GameInfo& g = games::launcher_game();
        int tone = 3;
        b.chip(tone == 3 ? 3 : tone, online_status(tone));
        if (!g.antiCheatStart) {
            b.note(L"Sim-QVFR runs inside " + std::wstring(g.shortName) + L" and changes how it renders. Use it in offline "
                   L"sessions, and don’t report problems you see with it to the game’s developers.");
            return;
        }
        const bool on = app.s.antiCheat;
        b.toggle(kAntiCheat, L"Run with anti-cheat", on,
                 on ? L"Starts " + std::wstring(g.shortName) + L" through its anti-cheat, as Steam does, with Sim-QVFR running."
                    : L"Off · " + std::wstring(g.shortName) + L" starts without its anti-cheat, so online servers won’t let you in",
                 false, Applies::NextLaunch);
        if (on)  // the risk, only while it's on
            b.note(L"Sim-QVFR runs inside the game, and the anti-cheat may kick you or ban your account for it.");
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
        app.canvas.text(L"On the rail", t::kSubheadline, {px, r.y + 20, pw, 18}, col(app.c->label_secondary));
        const Rect block{px, r.y + 44, pw, 64};
        app.canvas.fill(block, col(app.c->rail));
        app.canvas.fill({block.x, block.y, 4, block.h}, col(a.accent_fill));
        app.canvas.text(rail_abbreviation(game), t::kRailAbbrev, {block.x + 16, block.y + 10, pw - 20, 24}, col(app.c->rail_text));
        app.canvas.text(game.name, t::kRailName, {block.x + 16, block.y + 36, pw - 20, 16}, col(app.c->rail_text_muted));
    });
    b.slider({kColorValue, L"Brightness", app.colorValue});
    b.actions({{kColorReset, L"Use its own colour"}, {kPanelClose, L"Done", Btn::Primary}});
    return b.height();
}

// The Game tab's cards, in order: what everyone needs first (the checks, racing online, getting help), then the last
// sessions and the games, the OpenXR layers (for troubleshooting) last.
float game_page(float x, float y, float w, bool draw) {
    const std::vector<Card> cards = {checks_card(), online_card(), help_card(), diagnostics_card(), games_card(), layers_card()};
    return card_grid(cards, x, y, w, draw);
}

}  // namespace simqvfr::launcher::ui::win

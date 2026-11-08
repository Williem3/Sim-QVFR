// The Games pane: which games get a tab, and each game's colour (the colour wheel).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../game_tabs.h"
#include "../../games/game.h"
#include "window_state.h"
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

namespace {

// The game whose colour the wheel edits: the one picked by its swatch, else the selected game.
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

}  // namespace

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

float games_pane(float x, float y, float w, bool draw) {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    load_color_edit();
    const int editing = color_game_index();
    const bool onlyOne = shown_game_count() <= 1;
    std::vector<Row> rows;
    for (std::size_t i = 0; i < count; ++i) {
        const games::GameInfo* game = all[i];
        const bool shown = game_shown(*game), locked = shown && onlyOne;  // the last tab stays
        const bool found = installed(*game);
        const std::wstring desc = locked  ? L"The last tab: show another game to hide this one."
                                  : !found ? shown ? L"Has a tab: choose its folder in Launch checks."
                                                   : L"Steam doesn’t list it. Turn it on if it’s installed elsewhere."
                                  : shown  ? L"Has a tab at the top of the window."
                                           : L"No tab. Its settings are kept for when you show it again.";
        Row row;
        row.fullHeight = [desc](float width) { return checkbox_height(desc, width - 36); };
        row.full = [=](const Rect& r) {
            const t::Accent a = game_accent(*game);
            const Rect swatch{r.x, r.y + (r.h - 22) / 2, 22, 22};
            app.canvas.circle(swatch.x + 11, swatch.y + 11, 9, col(a.accent_fill), true);
            if (static_cast<int>(i) == editing)  // the game the wheel below edits
                app.canvas.circle(swatch.x + 11, swatch.y + 11, 11.5f, col(app.c->label), false, 1.5f);
            if (focused(kColorGame) && app.hoverOption == static_cast<int>(i))
                focus_ring(swatch, 11);
            app.hits.push_back({kColorGame, swatch.inset(-4, -4), static_cast<int>(i)});
            const Rect rest{r.x + 36, r.y, r.w - 36, r.h};
            const float tw = rest.w - kSwitchW - t::kSpace5;
            const float descH = app.canvas.measure_height(desc, t::kCallout, tw);
            const float ty = rest.y + (rest.h - t::kBody.lineHeight - 2 - descH) / 2;
            app.canvas.text(game->name, t::kBody, {rest.x, ty, tw, t::kBody.lineHeight}, col(app.c->label));
            if (!found) {  // tagged: Steam doesn't have it
                const float nx = rest.x + std::ceil(app.canvas.measure(game->name, t::kBody)) + t::kSpace2;
                badge(nx, ty, 3, L"Not found", std::max(0.0f, rest.x + tw - nx));
            }
            app.canvas.text(desc, t::kCallout, {rest.x, ty + t::kBody.lineHeight + 2, tw, descH + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
            switch_track({rest.right() - kSwitchW, rest.y + (rest.h - kSwitchH) / 2, kSwitchW, kSwitchH}, shown, locked);
            if (!locked)
                app.hits.push_back({kGameShown, rest, static_cast<int>(i)});
        };
        rows.push_back(row);
    }
    float h = section(x, y, w, L"Game tabs", rows,
                      L"Installed games get a tab; turn one off to hide it. Click a colour dot to change it below.", draw);
    h += t::kSpace6;
    const games::GameInfo& game = color_game();
    const bool custom = app.s.gameColors.count(game_id(game)) > 0;
    Row wheel;
    wheel.fullHeight = [](float) { return 184.0f; };
    wheel.full = [&game](const Rect& r) {
        color_wheel({r.x, r.y, r.h, r.h});
        // What it looks like: the game's tab and a button in its colours.
        const t::Accent a = game_accent(game);
        const float px = r.x + r.h + t::kSpace6, pw = std::min(260.0f, r.right() - px);
        if (pw < 120)
            return;
        const Rect tab{px, r.y + 40, pw, 34};
        app.canvas.fill(tab, col(a.accent_fill, app.dark ? 0.22f : 0.12f), t::kRadiusControl);
        app.canvas.circle(tab.x + 18, tab.y + tab.h / 2, 4, col(a.accent), true);
        app.canvas.text(game.name, t::kBody, {tab.x + 30, tab.y, tab.w - 34, tab.h}, col(app.c->label));
        app.canvas.fill({tab.x + 10, tab.bottom() - 3, tab.w - 20, 3}, col(a.accent), 1.5f);
        const Rect button{px, tab.bottom() + 16, std::min(pw, 140.0f), t::kControlHeight};
        app.canvas.fill(button, col(a.accent_fill), t::kRadiusControl);
        app.canvas.text(L"Launch", kButtonLabel, button, col(a.on_accent), Align::Center);
        app.canvas.text(L"Preview", t::kSubheadline, {px, r.y + 14, pw, 18}, col(app.c->label_secondary));
    };
    Row reset;
    reset.label = L"Built-in colour";
    reset.desc = custom ? L"Go back to " + std::wstring(game.shortName) + L"’s own colour." : L"In use.";
    reset.controlW = button_width(L"Use default");
    reset.controlH = t::kControlHeight;
    if (custom)
        reset.control = [](const Rect& r) { button(kColorReset, r, L"Use default", Btn::Secondary); };
    h += section(x, y + h, w, L"Colour for " + std::wstring(game.name),
                 {wheel, slider_row(kColorValue, L"Brightness", L"", app.colorValue, L""), reset},
                 L"Drag in the wheel to pick a colour; it applies at once.", draw);
    // Running with the game's anti-cheat (LMU's Easy Anti-Cheat): the user's choice and risk (games/game.h antiCheatStart).
    const games::GameInfo& launching = games::launcher_game();
    if (launching.antiCheatStart) {
        h += t::kSpace6;
        const std::wstring desc =
            L"Starts " + std::wstring(launching.shortName) +
            L" through its anti-cheat, as Steam does, with Sim-QVFR running. Sim-QVFR changes the game’s code and "
            L"the anti-cheat may kick you or ban your account for it. Off starts the game without the anti-cheat "
            L"(offline only).";
        Row on;
        on.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
        on.full = [desc](const Rect& r) { checkbox(kAntiCheat, r, app.s.antiCheat, L"Run with anti-cheat", desc, false); };
        h += section(x, y + h, w, L"Anti-cheat for " + std::wstring(launching.name), {on},
                     L"At your own risk.", draw, Applies::NextLaunch);
    }
    return h;
}

}  // namespace simqvfr::launcher::ui::win

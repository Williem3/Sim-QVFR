// The launcher's game tabs, as the user arranges them (the Games pane under Advanced): which games have a tab, and
// each game's own colour in place of its built-in accent. Launcher-wide ([launcher] hidden_games, color_<id>), not
// per-game settings. Pure: colour maths and the hidden list's text.
#pragma once

#include <cstdint>
#include <string>

namespace simqvfr::launcher {

// The hidden games as [launcher] hidden_games holds them: game ids, comma-separated ("acc,xplane").
bool game_hidden(const std::wstring& hidden, const std::wstring& id);
// The list with `id` hidden or shown, ids in the order given, no duplicates.
std::wstring with_game_hidden(const std::wstring& hidden, const std::wstring& id, bool hide);

// A colour as [launcher] color_<id> holds it: six hex digits, RRGGBB ("#" optional). False: not one.
bool parse_color(const std::wstring& text, uint32_t& rgb);
std::wstring color_text(uint32_t rgb);  // "RRGGBB"

// HSV: hue 0..360, saturation and value 0..1.
uint32_t hsv_rgb(float hue, float saturation, float value);
void rgb_hsv(uint32_t rgb, float& hue, float& saturation, float& value);

// WCAG contrast ratio of two colours (1..21).
double contrast(uint32_t a, uint32_t b);

// A game's accent from the colour picked for it, as the built-in palettes have it (theme_tokens.h Accent): the fill
// (buttons, the selected tab) is the colour itself; the accent (text, edges, the tab's dot) the colour darkened (light
// theme) or lightened (dark) until it reads against the window's background (3:1); text on the fill black or white,
// whichever contrasts more; the focus ring the accent.
struct AccentColors {
    uint32_t accent, fill, onAccent, ring;
};
AccentColors accent_from(uint32_t rgb, bool dark);

}  // namespace simqvfr::launcher

// The Overview tab (docs/ui/OVERHAUL_PLAN.md §2), where the launcher opens: the selected game's panel (its profile, what
// the picture is, the last session and its chart) beside summary cards grouped by section, each saying its settings in
// plain words and opening its card on the other tabs, with the Ko-fi tile among them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include <windows.h>

#include "../session_history.h"
#include "../settings.h"
#include "../../games/game.h"
#include "../../input/hotkeys.h"
#include "../../profile/profiles.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

namespace {

// The game's name on its yellow header, and the profile's segments (four in the panel's width).
constexpr t::TypeStyle kGameTitle = {30.0f, 32.0f, 900, false, t::Family::Jost, true, 0.0f};
constexpr t::TypeStyle kSegment = {12.0f, 16.0f, 600, false, t::Family::Inter, false, 0.0f};

constexpr float kPanelW = 320, kPad = 20, kGap = 16, kTileMinW = 200, kTilePad = 16, kChartH = 96;
constexpr int kChartBars = 5;

std::wstring percent_of(int v) { return std::to_wstring(v) + L"%"; }

std::wstring fixed(const wchar_t* format, double v) {
    wchar_t buf[48];
    std::swprintf(buf, std::size(buf), format, v);
    return buf;
}

// "2026-11-12 14:35" as "12 Nov, 14:35" (anything else as it is).
std::wstring short_time(const std::wstring& time) {
    static const wchar_t* const kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                             L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (swscanf_s(time.c_str(), L"%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12)
        return time;
    wchar_t buf[32];
    std::swprintf(buf, std::size(buf), L"%d %s, %02d:%02d", d, kMonths[mo - 1], h, mi);
    return buf;
}

// "22° left" (positive: `plus`).
std::wstring degrees_to(int v, const wchar_t* plus, const wchar_t* minus) {
    return v == 0 ? L"centred" : std::to_wstring(std::abs(v)) + L"° " + (v > 0 ? plus : minus);
}

// The selected game's sessions in the history, newest first.
std::vector<const SessionRecord*> game_sessions() {
    const std::wstring id = game_id(games::launcher_game());
    std::vector<const SessionRecord*> out;
    for (const SessionRecord& r : app.sessions)
        if (r.game == id)
            out.push_back(&r);
    return out;
}

// The headset's refresh rate, the frame rate a session aims for (runtime.ini's last session; 0: not known).
double target_hz() { return app.runtime.session.refreshHz > 0 ? app.runtime.session.refreshHz : 0; }

bool qvfr_mode() { return app.s.mode == RenderMode::Qvfr; }

// ---- The game's panel ----------------------------------------------------------------------------

// The profile as four segments: the three presets of this foveated mode, then Custom (the profile menu).
void profile_segments(const Rect& r) {
    Canvas& cv = app.canvas;
    std::vector<std::wstring> items;
    const int selected = choice_items(kOverviewProfile, items);
    cv.fill(r, col(app.c->segment_track), t::kRadiusControl);
    std::vector<float> widths;
    float natural = 0;
    for (const std::wstring& item : items) {
        widths.push_back(std::ceil(cv.measure(item, kSegment)));
        natural += widths.back();
    }
    const float room = r.w - 4 - 2 * (items.size() - 1);
    const float extra = std::max(0.0f, (room - natural) / items.size());
    float x = r.x + 2;
    for (size_t i = 0; i < items.size(); ++i) {
        const float iw = natural > room ? room / items.size() : widths[i] + extra;
        const Rect item{x, r.y + 2, iw, r.h - 4};
        const bool on = static_cast<int>(i) == selected;
        if (on)
            cv.fill(item, col(app.c->segment_selected), t::kRadiusControl - 2);
        else if (pointer_in(item))
            cv.fill(item, col(app.c->control_hover), t::kRadiusControl - 2);
        cv.text(items[i], kSegment, item, col(on ? app.c->on_segment : app.c->label), Align::Center);
        if (on && focused(kOverviewProfile))
            focus_ring(item, t::kRadiusControl - 2);
        app.hits.push_back({kOverviewProfile, item, static_cast<int>(i),
                            i + 1 == items.size() ? L"Saved profiles and more: the profile menu" : L""});
        x = item.right() + 2;
    }
}

// What the picture is, in a line: "Eye tracked · Outer 20% · Focus 150%".
std::wstring picture_summary() {
    std::wstring s = app.s.eyeTracking ? L"Eye tracked" : L"Fixed";
    s += L" · Outer " + percent_of(app.s.outer);
    s += qvfr_mode() ? L" · Focus " + percent_of(app.s.inner) : L" · Two-loop";
    return s;
}

// A saved profile in use, under the summary ("Profile: Night race, changed since"); empty for a preset or none.
std::wstring saved_profile_line() {
    if (app.profileCurrent.empty() || is_preset_name(app.profileCurrent))
        return L"";
    return L"Profile: " + app.profileCurrent + (app.profileMatches ? L"" : L", changed since");
}

// The last few sessions' frame rates as bars, oldest left, the newest in blue, the target as a dashed red line; a tip
// says the same in words.
void session_chart(const Rect& r, const std::vector<const SessionRecord*>& sessions, double target) {
    Canvas& cv = app.canvas;
    const size_t n = std::min<size_t>(sessions.size(), kChartBars);
    double top = target * 1.15;
    for (size_t i = 0; i < n; ++i)
        top = std::max(top, sessions[i]->fps * 1.08);
    if (top <= 0)
        top = 1;
    const float gap = 8, bw = (r.w - gap * (kChartBars - 1)) / kChartBars, base = r.bottom() - 2;
    std::wstring tip = L"The last " + std::to_wstring(n) + (n == 1 ? L" session: " : L" sessions, oldest first: ");
    for (size_t k = 0; k < n; ++k) {
        const SessionRecord& s = *sessions[n - 1 - k];
        const size_t slot = kChartBars - n + k;
        const float bh = std::max(2.0f, static_cast<float>(std::max(0.0, s.fps) / top) * (r.h - 4));
        cv.fill({r.x + slot * (bw + gap), base - bh, bw, bh}, col(k + 1 == n ? app.c->blue : app.c->field_border));
        tip += (k ? L", " : L"") + fixed(L"%.1f", s.fps);
    }
    tip += L" fps";
    if (target > 0) {
        tip += fixed(L"; the target is %.0f fps.", target);
        const float ty = base - static_cast<float>(target / top) * (r.h - 4);
        for (float dx = 0; dx < r.w; dx += 10)
            cv.line(r.x + dx, ty, r.x + std::min(r.w, dx + 6), ty, col(app.c->red), 2);
    }
    cv.fill({r.x, base, r.w, 2}, col(app.c->label));
    app.hits.push_back({kNone, r, -1, tip});
}

// The panel: the yellow header with the game's name, the profile and the summary, then the last session (its numbers
// and the chart), or what fills it in.
float game_panel(float x, float y, float w, bool draw) {
    Canvas& cv = app.canvas;
    if (draw) {  // its card first, as high as its content
        const float h = game_panel(x, y, w, false);
        cv.fill({x, y, w, h}, col(app.c->surface), t::kRadiusGroup);
    }
    const games::GameInfo& game = games::launcher_game();
    const float inner = w - 2 * kPad;
    // The header.
    const std::wstring name = game.name;
    const float nameH = cv.measure_height(name, kGameTitle, inner);
    const float headH = kPad + 14 + 8 + nameH + kPad;
    if (draw) {
        const Rect head{x, y, w, headH};
        cv.fill(head, col(app.c->yellow), t::kRadiusGroup);
        cv.fill({x, y + headH - t::kRadiusGroup, w, t::kRadiusGroup}, col(app.c->yellow));  // square at the bottom
        cv.push_clip(head);
        cv.circle(x + w - 36, y + 36, 74, col(app.c->ink, 0.07f), true);
        cv.pop_clip();
        cv.path("M0 10L5 0.5L10 10Z", {x + kPad, y + kPad + 2, 10, 10}, 10, col(app.c->ink), 0, true);
        cv.text(L"Selected game", t::kFootnote, {x + kPad + 16, y + kPad, inner - 16, 14}, col(app.c->ink));
        cv.text(name, kGameTitle, {x + kPad, y + kPad + 22, inner, nameH + 2}, col(app.c->ink), Align::Leading, false, true);
    }
    float by = y + headH + kPad;
    // The profile and what the picture is.
    if (draw)
        cv.text(L"Profile", t::kFootnote, {x + kPad, by, inner, 14}, col(app.c->label_muted));
    by += 14 + 8;
    if (draw)
        profile_segments({x + kPad, by, inner, 34});
    by += 34 + 10;
    const std::wstring summary = picture_summary(), saved = saved_profile_line();
    const float sh = cv.measure_height(summary, t::kCallout, inner);
    if (draw)
        cv.text(summary, t::kCallout, {x + kPad, by, inner, sh + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    by += sh;
    if (!saved.empty()) {
        const float ph = cv.measure_height(saved, t::kCallout, inner);
        if (draw)
            cv.text(saved, t::kCallout, {x + kPad, by + 2, inner, ph + 2}, col(app.c->label_secondary), Align::Leading, false, true);
        by += 2 + ph;
    }
    by += 16;
    if (draw)
        cv.fill({x + kPad, by, inner, 2}, col(app.c->label));
    by += 2 + 16;
    // The last session.
    const std::vector<const SessionRecord*> sessions = game_sessions();
    if (draw) {
        cv.text(L"Last session", t::kFootnote, {x + kPad, by + 2, inner, 14}, col(app.c->label));
        if (!sessions.empty())
            cv.text(short_time(sessions.front()->time), t::kCallout, {x + kPad, by, inner, 18}, col(app.c->label_secondary),
                    Align::Trailing);
    }
    by += 18 + 10;
    if (sessions.empty()) {
        const std::wstring empty = L"No sessions yet. Launch " + game_name() + L" from here and this fills in while you drive.";
        const float eh = cv.measure_height(empty, t::kCallout, inner);
        if (draw)
            cv.text(empty, t::kCallout, {x + kPad, by, inner, eh + 2}, col(app.c->label_secondary), Align::Leading, false, true);
        by += eh;
    } else {
        const SessionRecord& last = *sessions.front();
        const double target = target_hz();
        const Readouts ro = readouts(last.settings, app.runtime);
        const bool saving = !last.baseline && app.runtime.known();
        struct Figure {
            std::wstring value, label;
            uint32_t color;
        };
        const Figure figures[3] = {
            {fixed(L"%.1f", last.fps), target > 0 ? fixed(L"fps · target %.0f", target) : L"fps", app.c->label},
            {last.frameP90 > 0 ? fixed(L"%.1f", last.frameP90) : L"—", L"ms, slow frames", app.c->label},
            {saving ? percent_of(static_cast<int>(std::lround(ro.saved * 100))) : L"—",
             last.baseline ? L"pixels saved (QVFR was off)" : L"pixels saved", app.c->blue_text},
        };
        const float colW = inner / 3;
        float labelH = 0;
        for (const Figure& f : figures)
            labelH = std::max(labelH, cv.measure_height(f.label, t::kCallout, colW - 6));
        if (draw)
            for (int i = 0; i < 3; ++i) {
                const float fx = x + kPad + i * colW;
                cv.text(figures[i].value, t::kBigNumber, {fx, by, colW - 4, t::kBigNumber.lineHeight}, col(figures[i].color));
                cv.text(figures[i].label, t::kCallout, {fx, by + t::kBigNumber.lineHeight + 2, colW - 6, labelH + 2},
                        col(app.c->label_secondary), Align::Leading, false, true);
            }
        by += t::kBigNumber.lineHeight + 2 + labelH + 16;
        if (draw)
            session_chart({x + kPad, by, inner, kChartH}, sessions, target);
        by += kChartH + 8;
        if (draw) {
            const size_t n = std::min<size_t>(sessions.size(), kChartBars);
            cv.text(n == 1 ? L"Last session" : L"Last " + std::to_wstring(n) + L" sessions", t::kSubheadline,
                    {x + kPad, by, inner, 16}, col(app.c->label_secondary));
            if (target > 0)
                cv.text(fixed(L"Dashed line: %.0f fps target", target), t::kSubheadline, {x + kPad, by, inner, 16},
                        col(app.c->label_secondary), Align::Trailing);
        }
        by += 16;
    }
    return by + kPad - y;
}

// ---- The summary cards -----------------------------------------------------------------------------

// A summary card: its title, a line or two in plain words, a chip, and the card it opens (its title, its page).
struct Tile {
    std::wstring title, line1, line2, chip;
    int tone = 3;  // badge's tones: 0 good, 1 warning, 2 problem, 3 neutral
    std::wstring opens;
    Pane pane = Pane::Picture;
};

std::vector<Tile> picture_tiles() {
    std::vector<Tile> out;
    const Readouts ro = readouts(app.s, app.runtime);
    const bool known = app.runtime.known(), q = qvfr_mode();
    out.push_back({L"Resolution",
                   q ? L"Outer " + percent_of(app.s.outer) + L" · Focus " + percent_of(app.s.inner)
                     : L"Outer " + percent_of(app.s.outer) + L" · Two-loop",
                   known ? px(ro.outer) + L" outer view" : L"Pixel sizes after the first launch", L"", 3, L"Resolution"});
    Tile fov{L"Foveation",
             std::wstring(app.s.eyeTracking ? L"Eye tracked" : L"Fixed") + L" · " + std::to_wstring(app.s.width) + L" × " +
                 std::to_wstring(app.s.height) + L"%",
             L"", app.s.outline ? L"Outline on" : L"Outline off", 3, L"Foveation"};
    if (eye_warning()) {
        fov.chip = L"Eye tracker not detected";
        fov.tone = 1;
    }
    out.push_back(fov);
    const bool crop = game_has(&games::GameFeatures::fovCrop), helmet = game_has(&games::GameFeatures::helmet);
    if (crop || helmet) {
        std::wstring line = crop ? (app.s.crop ? L"Crop on" : L"Crop off") : L"";
        if (helmet)
            line += (line.empty() ? L"" : L" · ") + std::wstring(app.s.helmet.on ? L"Helmet on" : L"Helmet off");
        out.push_back({L"View & helmet", line,
                       !q ? L"In Quad View Foveated Rendering only" : crop && app.s.crop ? L"Edges cropped" : L"Whole view rendered",
                       L"", 3, L"View & helmet"});
    }
    const bool nativeQuad = game_has(&games::GameFeatures::nativeQuadViews), ownDlss = game_has(&games::GameFeatures::sideLoadedDlss),
               cheeky = game_has(&games::GameFeatures::foveatedDlss);
    if (method_shown() || nativeQuad || ownDlss || cheeky) {
        std::wstring line = q ? L"QVFR" : L"Two-loop (legacy)";
        if (ownDlss)
            line += own_dlss_available() && app.s.dlss.on ? L" · DLSS on" : L" · DLSS off";
        if (cheeky)
            line += app.s.cheeky.load ? L" · Foveated DLSS on" : L" · Foveated DLSS off";
        out.push_back({L"Rendering", line,
                       nativeQuad && q ? (app.s.nativeQuad ? L"Runtime composes where it can" : L"Sim-QVFR composes") : L"",
                       L"", 3, L"Rendering"});
    }
    for (Tile& tile : out)
        tile.pane = Pane::Picture;
    return out;
}

// The menu's keys in a few words: "Ctrl+Alt+arrows" when the four directions share their modifiers.
std::wstring menu_keys() {
    const Hotkeys& hk = app.s.hotkeys;
    const auto text = [&hk](HotkeyAction a) { return key_text(hk[static_cast<int>(a)].key); };
    const auto prefix = [](const std::wstring& s, const std::wstring& name) {
        return s.size() > name.size() && s.compare(s.size() - name.size(), name.size(), name) == 0 ? s.substr(0, s.size() - name.size())
                                                                                                  : std::wstring(L"?");
    };
    const std::wstring up = text(HotkeyAction::MenuUp), p = prefix(up, L"Up");
    if (p != L"?" && p == prefix(text(HotkeyAction::MenuDown), L"Down") && p == prefix(text(HotkeyAction::MenuLeft), L"Left") &&
        p == prefix(text(HotkeyAction::MenuRight), L"Right"))
        return p + L"arrows";
    return up.empty() ? L"No menu keys" : up + L", " + text(HotkeyAction::MenuDown) + L"…";
}

std::vector<Tile> headset_tiles() {
    std::vector<Tile> out;
    out.push_back({L"Menu & overlay",
                   L"Menu " + degrees_to(app.s.menuYaw, L"right", L"left") + L", " + degrees_to(app.s.menuPitch, L"up", L"down"),
                   std::to_wstring(app.s.menuWidth) + L"° wide", app.s.osd.enabled ? L"Overlay on" : L"Overlay off", 3,
                   L"Settings menu", Pane::Headset});
    const std::wstring open = key_text(app.s.hotkeys[static_cast<int>(HotkeyAction::MenuToggle)].key);
    int pads = 0;
    bool clash = false;
    for (int a = 0; a < kHotkeyActions; ++a) {
        if (static_cast<HotkeyAction>(a) == HotkeyAction::Visor && !game_has(&games::GameFeatures::helmet))
            continue;  // the visor's key only where there is a helmet
        pads += app.s.hotkeys[a].pad.set() ? 1 : 0;
        clash = clash || hotkey_clash(app.s.hotkeys, static_cast<HotkeyAction>(a), false) >= 0 ||
                hotkey_clash(app.s.hotkeys, static_cast<HotkeyAction>(a), true) >= 0;
    }
    // No key of its own opens the menu by default: a menu key's first press does.
    out.push_back({L"Hotkeys", open.empty() ? menu_keys() : open + L" opens the menu · " + menu_keys(),
                   pads == 0 ? L"No wheel buttons set" : std::to_wstring(pads) + (pads == 1 ? L" wheel button set" : L" wheel buttons set"),
                   clash ? L"Clash" : L"", 1, L"Hotkeys", Pane::Headset});
    return out;
}

std::vector<Tile> game_tiles() {
    std::vector<Tile> out;
    // Launch checks, first, only when something needs attention.
    size_t bad = 0, warn = 0;
    std::wstring first;
    for (const Check& c : app.checks)
        if (c.tone == CheckTone::Bad || c.tone == CheckTone::Warn) {
            bad += c.tone == CheckTone::Bad ? 1 : 0;
            warn += c.tone == CheckTone::Warn ? 1 : 0;
            if (first.empty())
                first = c.title;
        }
    if (bad + warn > 0)
        out.push_back({L"Launch checks", first, L"",
                       bad > 0 ? std::to_wstring(bad) + (bad == 1 ? L" problem" : L" problems") : std::to_wstring(warn) + L" to check",
                       bad > 0 ? 2 : 1, L"Launch checks", Pane::Game});
    const std::vector<const SessionRecord*> sessions = game_sessions();
    Tile diag{L"Diagnostics",
              sessions.empty()        ? L"No sessions yet"
              : sessions.size() == 1 ? L"1 session recorded"
                                      : std::to_wstring(sessions.size()) + L" sessions recorded",
              !sessions.empty() && !sessions.front()->limited.empty() ? L"Limited by " + sessions.front()->limited : L"", L"", 1,
              L"Diagnostics", Pane::Game};
    if (const double target = target_hz(); !sessions.empty() && target > 0 && sessions.front()->fps < target * 0.97)
        diag.chip = fixed(L"Below %.0f fps", target);
    out.push_back(diag);
    std::size_t count = 0;
    games::all_games(count);
    const games::GameInfo& game = games::launcher_game();
    out.push_back({L"Games", std::to_wstring(shown_game_count()) + L" of " + std::to_wstring(count) + L" in the rail",
                   game.antiCheatStart ? (app.s.antiCheat ? L"Anti-cheat on" : L"Anti-cheat off") : L"", L"", 3,
                   L"Games in the rail", Pane::Game});
    int below = 0;
    for (const PlannedLayer& l : app.layerPlan.layers)
        below += l.on ? 1 : 0;
    const bool lastIsThisGame = !sessions.empty() && sessions.front()->id == session_id(app.runtime.session.log);
    out.push_back({L"OpenXR layers",
                   below == 0 ? L"None below Sim-QVFR" : std::to_wstring(below) + (below == 1 ? L" layer" : L" layers") + L" below Sim-QVFR",
                   lastIsThisGame && app.runtime.session.layerMs >= 0 ? fixed(L"%.2f ms per frame", app.runtime.session.layerMs) : L"",
                   L"", 3, L"OpenXR layers", Pane::Game});
    return out;
}

float tile_height(const Tile& tile, float w) {
    Canvas& cv = app.canvas;
    const float inner = w - 2 * kTilePad;
    float h = kTilePad + t::kTitle2.lineHeight + 4;
    if (!tile.line1.empty())
        h += cv.measure_height(tile.line1, t::kCallout, inner);
    if (!tile.line2.empty())
        h += 2 + cv.measure_height(tile.line2, t::kSubheadline, inner);
    if (!tile.chip.empty())
        h += 8 + 22;
    return std::max(96.0f, h + kTilePad);
}

void draw_tile(const Tile& tile, const Rect& r) {
    Canvas& cv = app.canvas;
    const int index = static_cast<int>(app.overviewTargets.size());
    app.overviewTargets.push_back({tile.opens, tile.opens, tile.pane});
    cv.fill(r, col(app.c->surface), t::kRadiusGroup);
    if (pointer_in(r))
        cv.stroke(r.inset(0.5f, 0.5f), col(app.c->control_border), 1, t::kRadiusGroup);
    const float inner = r.w - 2 * kTilePad;
    float y = r.y + kTilePad;
    cv.text(tile.title, t::kTitle2, {r.x + kTilePad, y, inner, t::kTitle2.lineHeight}, col(app.c->label));
    y += t::kTitle2.lineHeight + 4;
    if (!tile.line1.empty()) {
        const float lh = cv.measure_height(tile.line1, t::kCallout, inner);
        cv.text(tile.line1, t::kCallout, {r.x + kTilePad, y, inner, lh + 2}, col(app.c->label_secondary), Align::Leading, false, true);
        y += lh;
    }
    if (!tile.line2.empty()) {
        const float lh = cv.measure_height(tile.line2, t::kSubheadline, inner);
        cv.text(tile.line2, t::kSubheadline, {r.x + kTilePad, y + 2, inner, lh + 2}, col(app.c->label_secondary), Align::Leading,
                false, true);
        y += 2 + lh;
    }
    if (!tile.chip.empty())
        badge(r.x + kTilePad, y + 8 + 1, tile.tone, tile.chip, inner);
    if (focused(kOverviewCard) && app.focusOption == index)
        focus_ring(r, t::kRadiusGroup);
    app.hits.push_back({kOverviewCard, r, index});
}

// The Ko-fi tile: dark and quiet, among the Game cards.
void kofi_tile(const Rect& r) {
    Canvas& cv = app.canvas;
    cv.fill(r, col(app.dark ? app.c->surface : app.c->ink), t::kRadiusGroup);
    cv.push_clip(r);
    cv.circle(r.right() - 22, r.bottom() - 6, 46, col(app.c->red), true);
    cv.pop_clip();
    if (pointer_in(r))
        cv.stroke(r.inset(0.5f, 0.5f), col(app.c->rail_text_muted), 1, t::kRadiusGroup);
    const float inner = r.w - 2 * kTilePad;
    const std::wstring line = L"Made by one sim racer, in spare time.";
    const float lh = cv.measure_height(line, t::kCallout, inner - 20);
    cv.text(line, t::kCallout, {r.x + kTilePad, r.y + kTilePad, inner - 20, lh + 2}, col(app.c->rail_text), Align::Leading, false,
            true);
    const float by = r.bottom() - kTilePad - 18;
    cv.path(kIconHeart, {r.x + kTilePad, by + 2, 14, 14}, 16, col(app.c->yellow), 0, true);
    cv.text(L"Support on Ko-fi", t::kTabLabel, {r.x + kTilePad + 22, by, inner - 22, 18}, col(app.c->rail_text));
    if (focused(kKofiTile))
        focus_ring(r, t::kRadiusGroup);
    app.hits.push_back({kKofiTile, r, -1, L"Support Sim-QVFR on Ko-fi (opens ko-fi.com)"});
}

// A group's header: its section's shape, its name in capitals and a rule in its colour to the right edge.
float group_header(Tab tab, float x, float y, float w, bool draw) {
    if (draw) {
        section_shape(tab, x + 6, y + 11, 12);
        const std::wstring label = tab_label(tab);
        const float lw = std::ceil(app.canvas.measure(label, t::kTabLabel));
        app.canvas.text(label, t::kTabLabel, {x + 20, y + 2, lw + 4, 18}, col(app.c->label));
        app.canvas.fill({x + 20 + lw + 14, y + 10, std::max(0.0f, w - (20 + lw + 14)), 2}, col(section_color(tab)));
    }
    return 22;
}

// A group's cards in rows (4 across where they fit, 1 at the narrowest), each row as high as its tallest; the Ko-fi
// tile last when `kofi`.
float tile_grid(const std::vector<Tile>& tiles, bool kofi, float x, float y, float w, bool draw) {
    const size_t n = tiles.size() + (kofi ? 1 : 0);
    const int cols = std::clamp(static_cast<int>((w + kGap) / (kTileMinW + kGap)), 1, 4);
    const float tw = std::floor((w - kGap * (cols - 1)) / cols);
    float h = 0;
    for (size_t row = 0; row * cols < n; ++row) {
        float rh = 0;
        for (size_t c = 0; c < static_cast<size_t>(cols) && row * cols + c < n; ++c) {
            const size_t i = row * cols + c;
            rh = std::max(rh, i < tiles.size() ? tile_height(tiles[i], tw) : 96.0f);
        }
        if (draw)
            for (size_t c = 0; c < static_cast<size_t>(cols) && row * cols + c < n; ++c) {
                const size_t i = row * cols + c;
                const Rect r{x + c * (tw + kGap), y + h, tw, rh};
                if (i < tiles.size())
                    draw_tile(tiles[i], r);
                else
                    kofi_tile(r);
            }
        h += rh + kGap;
    }
    return std::max(0.0f, h - kGap);
}

}  // namespace

// The profile segments' items (the presets of this foveated mode, then Custom) and the one chosen: a preset in use and
// still matching, else Custom.
int overview_profile(std::vector<std::wstring>& items) {
    const std::vector<Profile> presets = builtin_profiles(app.s.eyeTracking);
    int selected = -1;
    for (size_t i = 0; i < presets.size(); ++i) {
        items.push_back(presets[i].name);
        if (app.profileMatches && _wcsicmp(presets[i].name.c_str(), app.profileCurrent.c_str()) == 0)
            selected = static_cast<int>(i);
    }
    items.push_back(L"Custom");
    return selected >= 0 ? selected : static_cast<int>(items.size()) - 1;
}

// A segment chosen: a preset is used (as Use in Manage profiles); Custom opens the profile menu.
void choose_overview_profile(int option) {
    const std::vector<Profile> presets = builtin_profiles(app.s.eyeTracking);
    if (option >= 0 && option < static_cast<int>(presets.size())) {
        use_profile(presets[static_cast<size_t>(option)].name);
        InvalidateRect(app.hwnd, nullptr, FALSE);
    } else {
        open_profile_menu();
    }
}

float overview_page(float x, float y, float w, bool draw) {
    if (draw)
        app.overviewTargets.clear();
    // The panel beside the cards; narrow, above them.
    const bool side = w >= kPanelW + 24 + kTileMinW * 2 + kGap;
    const float pw = side ? kPanelW : std::min(w, 520.0f);
    const float ph = game_panel(x, y, pw, draw);
    const float rx = side ? x + pw + 24 : x, rw = side ? w - pw - 24 : w;
    float ry = side ? y : y + ph + 24;
    const struct {
        Tab tab;
        std::vector<Tile> tiles;
        bool kofi;
    } groups[] = {{Tab::Picture, picture_tiles(), false}, {Tab::Headset, headset_tiles(), false}, {Tab::Game, game_tiles(), true}};
    for (const auto& g : groups) {
        ry += group_header(g.tab, rx, ry, rw, draw) + 12;
        ry += tile_grid(g.tiles, g.kofi, rx, ry, rw, draw) + 24;
    }
    ry -= 24;
    return std::max(side ? ph : 0.0f, ry - y);
}

}  // namespace simqvfr::launcher::ui::win

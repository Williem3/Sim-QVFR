// The Headset tab (docs/ui/OVERHAUL_PLAN.md §2): the settings menu's place, the performance overlay, the placement
// preview to drag them in, and the hotkeys as a table of actions, keys and wheel buttons.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>

#include "../../games/game.h"
#include "../../input/hotkeys.h"
#include "card.h"
#include "controls.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

namespace {

Card menu_card() {
    Card c{Tab::Headset, tr(L"Headset menu"), Applies::Live};
    c.body = [](Body& b) {
        b.sliders({kMenuYaw, tr(L"Horizontal"), app.s.menuYaw, L"°"},
                  {kMenuPitch, tr(L"Vertical"), app.s.menuPitch, L"°"});
        b.slider({kMenuWidth, tr(L"Size"), app.s.menuWidth, L"°"});
        b.toggle(kHotkeyFooter, tr(L"Show the hotkeys under the menu"), app.s.hotkeyFooter,
                 tr(L"The in-game hotkeys and their wheel buttons, on a line under it."));
        b.note(tr(L"Ctrl+Alt+arrows (or your keys and wheel buttons below) open it while the game has focus, or a "
                  L"click of the left stick; the right stick selects and changes. It hides 6 s after your last press, "
                  L"15 s with the performance overlay on, which also puts the frame times beside its title."));
    };
    c.reset = [](Settings& s) {
        const Settings d;
        s.menuYaw = d.menuYaw;
        s.menuPitch = d.menuPitch;
        s.menuWidth = d.menuWidth;
        s.hotkeyFooter = d.hotkeyFooter;
    };
    return c;
}

Card overlay_card() {
    Card c{Tab::Headset, tr(L"Performance overlay"), Applies::Live};
    c.body = [](Body& b) {
        const bool on = app.s.osd.enabled;
        b.toggle(kOsdOn, tr(L"Show the overlay"), on,
                 on ? tr(L"Frame rate and pacing, CPU and GPU time, what limits the frame, VRAM.") : tr(L"Off"));
        b.reveal(kOsdOn, on, tr(L"Show the overlay"), {tr(L"Horizontal"), tr(L"Vertical"), tr(L"Size")}, [](Body& r) {
            r.sliders({kOsdYaw, tr(L"Horizontal"), app.s.osd.yaw, L"°"},
                      {kOsdPitch, tr(L"Vertical"), app.s.osd.pitch, L"°"});
            r.slider({kOsdSize, tr(L"Size"), app.s.osd.width, L"°"});
        });
        b.note(tr(L"Ctrl+Alt+P in the game turns it on and off; the headset menu’s Headset page moves it too."));
    };
    c.reset = [](Settings& s) { s.osd = OsdPlacement{}; };
    return c;
}

// The clock ([clock]): the local time on a card of its own, for players who want it without the overlay.
Card clock_card() {
    Card c{Tab::Headset, tr(L"Clock"), Applies::Live};
    c.body = [](Body& b) {
        const bool on = app.s.clock.enabled;
        b.toggle(kClockOn, tr(L"Show the clock"), on,
                 on ? tr(L"The local time, on a small card of its own.") : tr(L"Off"));
        b.reveal(kClockOn, on, tr(L"Show the clock"), {tr(L"Horizontal"), tr(L"Vertical"), tr(L"Size")}, [](Body& r) {
            r.sliders({kClockYaw, tr(L"Horizontal"), app.s.clock.yaw, L"°"},
                      {kClockPitch, tr(L"Vertical"), app.s.clock.pitch, L"°"});
            r.slider({kClockSize, tr(L"Size"), app.s.clock.width, L"°"});
        });
        b.note(tr(L"The performance overlay keeps its own clock; the headset menu’s Headset page turns this one on "
                  L"and off and moves it too."));
    };
    c.reset = [](Settings& s) { s.clock = ClockPlacement{}; };
    return c;
}

// One eye off ([quad] eye_off): for a player who sees with one eye.
Card vision_card() {
    Card c{Tab::Headset, tr(L"Vision"), Applies::Live};
    c.body = [](Body& b) {
        std::vector<std::wstring> items;
        const int selected = choice_items(kEyeOff, items);
        b.segments(kEyeOff, tr(L"Eye off"), items, selected);
        b.note(app.s.eyeOff == 0
                   ? tr(L"For a player who sees with one eye: the eye you choose shows black, and the game stops "
                        L"rendering most of it, which saves GPU time.")
                   : tr(L"That eye shows black, and the game renders only a few pixels of it. The headset menu’s "
                        L"Picture tab changes it too."));
    };
    c.reset = [](Settings& s) { s.eyeOff = Settings{}.eyeOff; };
    return c;
}

Card placement_card() {
    Card c{Tab::Headset, tr(L"Placement"), Applies::Live};
    c.body = [](Body& b) {
        const float w = b.width();
        b.custom(placement_preview(0, 0, w, false), [](const Rect& r) { placement_preview(r.x, r.y, r.w, true); });
        b.custom(placement_caption(0, 0, w, false), [](const Rect& r) { placement_caption(r.x, r.y, r.w, true); });
        b.divider();
        b.custom(placement_readouts(0, 0, w, false), [](const Rect& r) { placement_readouts(r.x, r.y, r.w, true); });
    };
    return c;
}

// ---- Hotkeys -----------------------------------------------------------------------------------

constexpr float kHotkeyRowH = 34, kHotkeyRowGap = 6, kHotkeyGap = 8;

// The table's columns at width w: the action's, then its key's and its wheel button's.
struct Columns {
    float labelW, buttonW;
};

Columns columns(float w) {
    float labelW = 0;
    for (const wchar_t* label : kHotkeyLabels)
        labelW = std::max(labelW, std::ceil(app.canvas.measure(tr(label), t::kBody)));
    labelW += 30;  // its ⓘ, or the clash mark
    return {labelW, std::max(80.0f, (w - labelW - kHotkeyGap) / 2)};
}

// What clashes for an action (its key or its wheel button is also another action's), for its tip; empty for none.
std::wstring clash_text(HotkeyAction action) {
    const Hotkey& h = app.s.hotkeys[static_cast<int>(action)];
    std::wstring text;
    if (const int other = hotkey_clash(app.s.hotkeys, action, false); other >= 0)
        text += trf(L"{key} is also {action}.", {{L"key", key_text(h.key)}, {L"action", tr(kHotkeyLabels[other])}});
    if (const int other = hotkey_clash(app.s.hotkeys, action, true); other >= 0)
        text += std::wstring(text.empty() ? L"" : L" ") +
                trf(L"Its wheel button is also {action}.", {{L"action", tr(kHotkeyLabels[other])}});
    return text;
}

// An action's row: its name (a warning mark and tip on a clash), its key's button, its wheel button's.
void hotkey_row(const Rect& r, HotkeyAction action) {
    const int a = static_cast<int>(action);
    const Hotkey& k = app.s.hotkeys[a];
    const Columns c = columns(r.w);
    Canvas& cv = app.canvas;
    const std::wstring label = tr(kHotkeyLabels[a]);
    const float lw = std::ceil(cv.measure(label, t::kBody));
    cv.text(label, t::kBody, {r.x, r.y, c.labelW, r.h}, col(app.c->label));
    const std::wstring clash = clash_text(action);
    if (!clash.empty()) {  // a warning triangle with a word for it in the tip
        const Rect m{r.x + lw + 6, r.y + (r.h - 14) / 2, 14, 14};
        cv.path("M7 1.5l6 11H1zM7 5.5v3.2", m, 14, col(app.c->warning), 1.4f);
        cv.circle(m.x + 7, m.y + 10.6f, 0.7f, col(app.c->warning), true);
        app.hits.push_back({kNone, {r.x, r.y, c.labelW, r.h}, -1, trf(L"Clash: {clash}", {{L"clash", clash}})});
    } else {
        help_icon(label, r.x + lw + 6, r.y + (r.h - t::kBody.lineHeight) / 2);
    }
    const bool keyCapture = app.captureAction == a && !app.capturePad, padCapture = app.captureAction == a && app.capturePad;
    // Open or close without a key of its own: any of the menu's arrow keys opens it.
    const std::wstring keyLabel = keyCapture   ? std::wstring(tr(L"Press keys…"))
                                  : k.key.set() ? key_text(k.key)
                                  : a == static_cast<int>(HotkeyAction::MenuToggle) ? std::wstring(tr(L"Any arrow key"))
                                                                                     : std::wstring(tr(L"No key"));
    const std::wstring padLabel = padCapture    ? std::wstring(tr(L"Press a button…"))
                                  : k.pad.set() ? control_text(k.pad.control)
                                                : std::wstring(tr(L"No button"));
    const float x = r.x + c.labelW;
    button(kHotkeyKey, {x, r.y, c.buttonW, r.h}, keyLabel, keyCapture ? Btn::Primary : Btn::Secondary, nullptr, a);
    app.hits.back().tip = tr(L"Click, then press the keys: Esc cancels, Delete clears it.");
    button(kHotkeyPad, {x + c.buttonW + kHotkeyGap, r.y, c.buttonW, r.h}, padLabel, padCapture ? Btn::Primary : Btn::Secondary,
           nullptr, a);
    app.hits.back().tip = k.pad.set() ? trf(L"On {device}. Click, then press another.",
                                            {{L"device", k.pad.name.empty() ? k.pad.device : k.pad.name}})
                                      : std::wstring(tr(L"Click, then press a wheel or controller button."));
    app.drawnLabels.push_back({label, app.drawingSection, r});
}

// The actions' rows as one block, closer together than a card's rows (a table).
void hotkey_rows(Body& b, const std::vector<HotkeyAction>& actions) {
    const float h = actions.size() * kHotkeyRowH + (actions.size() - 1) * kHotkeyRowGap;
    b.custom(h, [actions](const Rect& r) {
        for (size_t i = 0; i < actions.size(); ++i)
            hotkey_row({r.x, r.y + i * (kHotkeyRowH + kHotkeyRowGap), r.w, kHotkeyRowH}, actions[i]);
    });
}

Card hotkeys_card() {
    Card c{Tab::Headset, tr(L"Hotkeys"), Applies::Live};
    c.body = [](Body& b) {
        // The columns' names over the buttons.
        b.custom(t::kFootnote.lineHeight, [](const Rect& r) {
            const Columns c = columns(r.w);
            app.canvas.text(tr(L"Keyboard"), t::kFootnote, {r.x + c.labelW, r.y, c.buttonW, r.h},
                            col(app.c->label_muted));
            app.canvas.text(tr(L"Wheel"), t::kFootnote, {r.x + c.labelW + c.buttonW + kHotkeyGap, r.y, c.buttonW, r.h},
                            col(app.c->label_muted));
        });
        b.note(tr(L"To open the headset menu from your wheel, click No button beside Open or close, then press the "
                  L"button on your wheel."));
        b.group(tr(L"Headset menu"));
        hotkey_rows(b, {HotkeyAction::MenuToggle, HotkeyAction::MenuUp, HotkeyAction::MenuDown, HotkeyAction::MenuLeft,
                        HotkeyAction::MenuRight});
        b.group(tr(L"In the game"));
        std::vector<HotkeyAction> inGame = {HotkeyAction::Overlay};
        if (game_has(&games::GameFeatures::helmet))  // the visor steps the helmet overlay
            inGame.push_back(HotkeyAction::Visor);
        inGame.insert(inGame.end(), {HotkeyAction::CpuCapture, HotkeyAction::FrameCapture});
        if (game_has(&games::GameFeatures::showcase))  // the showcase: Stock or QVFR, live
            inGame.push_back(HotkeyAction::ShowcaseStock);
        hotkey_rows(b, inGame);
        b.note(pads_found());
    };
    c.reset = [](Settings& s) { s.hotkeys = default_hotkeys(); };  // the Ctrl+Alt keys, no wheel buttons
    return c;
}

std::vector<Card> headset_cards() {
    std::vector<Card> cards = {menu_card(), overlay_card(), clock_card(), placement_card(), hotkeys_card()};
    if (game_has(&games::GameFeatures::eyeOff))  // a game whose eye passes can be turned off (LMU)
        cards.insert(cards.begin() + 3, vision_card());
    return cards;
}

}  // namespace

float headset_page(float x, float y, float w, bool draw) { return card_grid(headset_cards(), x, y, w, draw); }

}  // namespace simqvfr::launcher::ui::win

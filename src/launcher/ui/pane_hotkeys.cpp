// The Hotkeys pane, and catching a key, a wheel or a controller button for an action.
#include <string>
#include <vector>

#include <windows.h>

#include "../../games/game.h"
#include "../../input/pad_input.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

// The wheels and controllers DirectInput finds (opened when the Hotkeys pane shows), for the pane's note.
void open_pads() {
    if (!app.padsOpen)
        app.padsOpen = app.pads.open(app.hwnd);
    else
        app.pads.rescan();  // a wheel plugged in since
}

namespace {

std::wstring pads_found() {
    if (!app.padsOpen)
        return L"DirectInput isn’t available: wheel and controller buttons can’t be read.";
    std::wstring names;
    for (const auto& [id, name] : app.pads.devices())
        names += (names.empty() ? L"" : L", ") + name;
    return names.empty() ? L"No wheel or controller found: plug it in, then come back to this pane."
                         : L"Found: " + names + L".";
}

}  // namespace

// Hotkeys ([hotkeys], input/hotkeys.h): each action's key and wheel button. Click one, then press.
float hotkeys_pane(float x, float y, float w, bool draw) {
    static const wchar_t* const kDescriptions[kHotkeyActions] = {
        L"Opens the headset menu, or closes it.",
        L"Selects the line above.",
        L"Selects the line below.",
        L"Lowers the selected value.",
        L"Raises the selected value.",
        L"Turns the performance overlay on or off.",
        L"Steps the helmet’s visor: closed, half, open.",
        L"Records a CPU capture, with CPU capture on in the Diagnostics pane.",
        L"Saves the frame the headset gets, with Frame capture on in the Diagnostics pane."};
    const float slotW = 136;
    const auto row = [slotW](HotkeyAction action) {
        const int a = static_cast<int>(action);
        const Hotkey& h = app.s.hotkeys[a];
        Row r;
        r.label = kHotkeyLabels[a];
        r.desc = kDescriptions[a];
        if (h.pad.set())
            r.desc += L" Wheel button on " + (h.pad.name.empty() ? h.pad.device : h.pad.name) + L".";
        if (const int other = hotkey_clash(app.s.hotkeys, action, false); other >= 0)
            r.desc += L" " + key_text(h.key) + L" is also " + kHotkeyLabels[other] + L".";
        if (const int other = hotkey_clash(app.s.hotkeys, action, true); other >= 0)
            r.desc += L" Its wheel button is also " + std::wstring(kHotkeyLabels[other]) + L".";
        r.controlW = 2 * slotW + t::kSpace2;
        r.controlH = t::kControlHeight;
        r.control = [a, slotW](const Rect& rect) {
            const Hotkey& k = app.s.hotkeys[a];
            const bool keyCapture = app.captureAction == a && !app.capturePad, padCapture = app.captureAction == a && app.capturePad;
            const std::wstring keyLabel = keyCapture ? L"Press keys…" : k.key.set() ? key_text(k.key) : L"No key";
            const std::wstring padLabel = padCapture ? L"Press a button…" : k.pad.set() ? control_text(k.pad.control) : L"No wheel button";
            button(kHotkeyKey, {rect.x, rect.y, slotW, rect.h}, keyLabel, keyCapture);
            app.hits.back().option = a;
            button(kHotkeyPad, {rect.x + slotW + t::kSpace2, rect.y, slotW, rect.h}, padLabel, padCapture);
            app.hits.back().option = a;
        };
        return r;
    };
    float h = 0;
    h += section(x, y + h, w, L"Headset menu",
                 {row(HotkeyAction::MenuToggle), row(HotkeyAction::MenuUp), row(HotkeyAction::MenuDown),
                  row(HotkeyAction::MenuLeft), row(HotkeyAction::MenuRight)},
                 pads_found(),
                 draw, Applies::Live) +
         t::kSpace6;
    std::vector<Row> inGame = {row(HotkeyAction::Overlay)};
    if (game_has(&games::GameFeatures::helmet))  // the visor steps the helmet overlay
        inGame.push_back(row(HotkeyAction::Visor));
    inGame.insert(inGame.end(), {row(HotkeyAction::CpuCapture), row(HotkeyAction::FrameCapture)});
    h += section(x, y + h, w, L"In the game", inGame,
                 L"Click a button, then press the keys or a wheel button: Esc cancels, Delete clears it.",
                 draw, Applies::Live) +
         t::kSpace6;
    Row reset;
    reset.fullHeight = [](float) { return t::kControlHeight; };
    reset.full = [](const Rect& r) {
        const float resetW = button_width(L"Reset hotkeys");
        button(kHotkeysReset, {r.x, r.y, resetW, t::kControlHeight}, L"Reset hotkeys", false);
    };
    h += section(x, y + h, w, L"", {reset}, L"Back to the Ctrl+Alt keys, without wheel buttons. Restore Defaults leaves them as they are.",
                 draw);
    return h;
}

void end_capture() {
    if (app.captureAction < 0)
        return;
    app.captureAction = -1;
    KillTimer(app.hwnd, kCaptureTimer);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The Hotkeys pane: `action`'s key or (pad) wheel button takes the next press.
void start_capture(int action, bool pad) {
    if (pad) {
        open_pads();
        if (!app.padsOpen) {
            notify(L"Wheel buttons can’t be read", L"DirectInput isn’t available, so wheel and controller buttons can’t be read.");
            return;
        }
        app.captureHeld = app.pads.poll();
        SetTimer(app.hwnd, kCaptureTimer, kCaptureMs, nullptr);
    }
    app.captureAction = action;
    app.capturePad = pad;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

void set_hotkey(const KeyCombo* key, const PadControl* pad) {
    Hotkey& h = app.s.hotkeys[app.captureAction];
    if (key)
        h.key = *key;
    if (pad)
        h.pad = *pad;
    end_capture();
    changed();
}

}  // namespace

// A key while catching one: Esc cancels, Delete or Backspace alone clears, modifiers wait for their key.
bool capture_key(WPARAM vk) {
    if (app.captureAction < 0)
        return false;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (vk == VK_ESCAPE) {
        end_capture();
    } else if ((vk == VK_DELETE || vk == VK_BACK) && !ctrl && !alt && !shift) {
        const KeyCombo none;
        const PadControl noPad;
        set_hotkey(app.capturePad ? nullptr : &none, app.capturePad ? &noPad : nullptr);
    } else if (!app.capturePad && vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LWIN && vk != VK_RWIN) {
        const KeyCombo key{static_cast<int>(vk), ctrl, alt, shift};
        set_hotkey(&key, nullptr);
    }
    return true;
}

// The capture timer: the first control pressed that wasn't held when the capture started.
void capture_poll() {
    if (app.captureAction < 0 || !app.capturePad)
        return;
    const std::vector<PadHeld> held = app.pads.poll();
    const auto in = [](const std::vector<PadHeld>& list, const PadHeld& p) {
        for (const PadHeld& q : list)
            if (q.control == p.control && q.device == p.device)
                return true;
        return false;
    };
    for (const PadHeld& p : held)
        if (!in(app.captureHeld, p)) {
            const PadControl pad{p.device, p.control, p.name};
            set_hotkey(nullptr, &pad);
            return;
        }
    // A control let go since counts when pressed again.
    std::vector<PadHeld> still;
    for (const PadHeld& p : app.captureHeld)
        if (in(held, p))
            still.push_back(p);
    app.captureHeld = std::move(still);
}

}  // namespace simqvfr::launcher::ui::win

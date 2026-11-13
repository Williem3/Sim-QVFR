// Catching a key, a wheel or a controller button for an action (the Headset tab's Hotkeys card), and the wheels found.
#include <string>
#include <vector>

#include <windows.h>

#include "../../games/game.h"
#include "../../input/pad_input.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

// The wheels and controllers DirectInput finds (opened when the Headset tab shows), for the Hotkeys card's note.
void open_pads() {
    if (!app.padsOpen)
        app.padsOpen = app.pads.open(app.hwnd);
    else
        app.pads.rescan();  // a wheel plugged in since
}

// The wheels and controllers found, for the Hotkeys card's note.
std::wstring pads_found() {
    if (!app.padsOpen)
        return L"DirectInput isn’t available: wheel and controller buttons can’t be read.";
    std::wstring names;
    for (const auto& [id, name] : app.pads.devices())
        names += (names.empty() ? L"" : L", ") + name;
    return names.empty() ? L"No wheel or controller found: plug it in, then come back to this tab."
                         : L"Wheels and controllers found: " + names + L".";
}

void end_capture() {
    if (app.captureAction < 0)
        return;
    app.captureAction = -1;
    KillTimer(app.hwnd, kCaptureTimer);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The Hotkeys card: `action`'s key or (pad) wheel button takes the next press.
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

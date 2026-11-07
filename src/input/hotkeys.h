// Hotkeys ([hotkeys]): a keyboard combination and a wheel or controller button for each of the layer's
// actions in the game: the headset menu's open/close and its four directions, the performance overlay,
// the helmet's visor, a CPU capture and a frame capture. The launcher's Hotkeys pane sets them; the layer reloads them live.
// Keyboard combinations act while ACE has focus; wheel and controller buttons (DirectInput, pad_input.h)
// whenever the game runs. A wheel's menu directions act only while the menu shows (its toggle opens it), so
// a funky switch the game also uses doesn't pop the menu up.
//
// ini: two keys per action, e.g.
//   overlay=Ctrl+Alt+P                       a key with its modifiers (empty: none)
//   overlay_pad=0EB7_0020:button13:Wheel     a device (USB vendor_product), its control, its name (empty: none)
#pragma once

#include <array>
#include <string>

namespace simqvfr {

enum class HotkeyAction { MenuToggle, MenuUp, MenuDown, MenuLeft, MenuRight, Overlay, Visor, CpuCapture, FrameCapture };
inline constexpr int kHotkeyActions = 9;

struct KeyCombo {
    int vk = 0;  // 0: none
    bool ctrl = false, alt = false, shift = false;
    bool set() const { return vk != 0; }
    bool operator==(const KeyCombo&) const = default;
};

// A device's control: buttons 0..127 (DirectInput's order; Windows' controller panel shows them from 1), then
// each of four hats' four directions: kPadHatBase + hat * 4 + kHatUp..kHatLeft.
inline constexpr int kPadButtons = 128, kPadHatBase = 128, kPadHats = 4, kPadControls = kPadHatBase + 4 * kPadHats;
enum { kHatUp, kHatRight, kHatDown, kHatLeft };

struct PadControl {
    std::wstring device;  // "VVVV_PPPP", the USB vendor and product (several alike count as one)
    int control = -1;     // -1: none
    std::wstring name;    // the device's name when it was bound, to show while it is unplugged
    bool set() const { return control >= 0 && !device.empty(); }
    bool same(const PadControl& o) const;  // the same device's same control
    bool operator==(const PadControl&) const = default;
};

struct Hotkey {
    KeyCombo key;
    PadControl pad;
    bool operator==(const Hotkey&) const = default;
};
using Hotkeys = std::array<Hotkey, kHotkeyActions>;

// Ctrl+Alt+arrows for the menu (its first press opens it), Ctrl+Alt+P, Ctrl+Alt+V, Ctrl+Alt+C and Ctrl+Alt+F; no wheel
// buttons.
Hotkeys default_hotkeys();
Hotkeys read_hotkeys(const wchar_t* ini);
bool write_hotkeys(const wchar_t* ini, const Hotkeys& hotkeys);

const wchar_t* hotkey_ini_key(HotkeyAction action);  // "menu_toggle", "overlay", ...

// "Ctrl+Alt+P", "Shift+F5", "Ctrl+Alt+Up"; empty for none. parse_key reads it back (false: not a key).
std::wstring key_text(const KeyCombo& key);
bool parse_key(const std::wstring& text, KeyCombo& key);
// "Button 13", "Hat up" ("Hat 2 up" past the first hat).
std::wstring control_text(int control);
// The ini's value ("0EB7_0020:button13:Wheel"; empty for none) and back (false: not one).
std::wstring pad_value(const PadControl& pad);
bool parse_pad(const std::wstring& value, PadControl& pad);
// The control held by a DirectInput hat at `angle` (hundredths of a degree, 0xFFFF centred): its directions
// (a diagonal holds two), as a bit per kHatUp..kHatLeft.
unsigned hat_directions(unsigned long angle);

// Another action with the same key (or, `pad`, the same wheel control) as `action`'s, or -1.
int hotkey_clash(const Hotkeys& hotkeys, HotkeyAction action, bool pad);

}  // namespace simqvfr

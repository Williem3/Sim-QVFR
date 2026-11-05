#include "hotkeys.h"

#include <cwchar>
#include <cwctype>

#include <windows.h>

namespace simqvfr {
namespace {

constexpr const wchar_t* kIniKeys[kHotkeyActions] = {L"menu_toggle", L"menu_up", L"menu_down", L"menu_left",
                                                     L"menu_right",  L"overlay", L"visor",     L"cpu_capture"};

struct KeyName {
    int vk;
    const wchar_t* name;
};
// Keys by name; letters, digits, F1..F24 and Num 0..Num 9 are worked out.
constexpr KeyName kKeyNames[] = {
    {VK_UP, L"Up"},          {VK_DOWN, L"Down"},          {VK_LEFT, L"Left"},       {VK_RIGHT, L"Right"},
    {VK_HOME, L"Home"},      {VK_END, L"End"},            {VK_PRIOR, L"Page Up"},   {VK_NEXT, L"Page Down"},
    {VK_INSERT, L"Insert"},  {VK_DELETE, L"Delete"},      {VK_SPACE, L"Space"},     {VK_RETURN, L"Enter"},
    {VK_TAB, L"Tab"},        {VK_BACK, L"Backspace"},     {VK_PAUSE, L"Pause"},     {VK_SCROLL, L"Scroll Lock"},
    {VK_MULTIPLY, L"Num *"}, {VK_ADD, L"Num +"},          {VK_SUBTRACT, L"Num -"},  {VK_DIVIDE, L"Num /"},
    {VK_DECIMAL, L"Num ."},  {VK_OEM_PLUS, L"="},         {VK_OEM_MINUS, L"-"},     {VK_OEM_COMMA, L","},
    {VK_OEM_PERIOD, L"."},   {VK_OEM_1, L";"},            {VK_OEM_2, L"/"},         {VK_OEM_3, L"`"},
    {VK_OEM_4, L"["},        {VK_OEM_5, L"\\"},           {VK_OEM_6, L"]"},         {VK_OEM_7, L"'"},
};

std::wstring vk_name(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    for (const KeyName& k : kKeyNames)
        if (k.vk == vk)
            return k.name;
    wchar_t hex[16];
    swprintf_s(hex, L"Key 0x%02X", vk);
    return hex;
}

int vk_of(const std::wstring& name) {
    if (name.size() == 1) {
        const wchar_t c = static_cast<wchar_t>(std::towupper(name[0]));
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            return c;
    }
    for (const KeyName& k : kKeyNames)
        if (_wcsicmp(k.name, name.c_str()) == 0)
            return k.vk;
    int n = 0;
    if ((name[0] == L'F' || name[0] == L'f') && swscanf_s(name.c_str() + 1, L"%d", &n) == 1 && n >= 1 && n <= 24 &&
        _wcsicmp(name.c_str(), (L"F" + std::to_wstring(n)).c_str()) == 0)
        return VK_F1 + n - 1;
    if (_wcsnicmp(name.c_str(), L"Num ", 4) == 0 && name.size() == 5 && name[4] >= L'0' && name[4] <= L'9')
        return VK_NUMPAD0 + (name[4] - L'0');
    if (_wcsnicmp(name.c_str(), L"Key 0x", 6) == 0 && swscanf_s(name.c_str() + 6, L"%x", &n) == 1 && n > 0 && n < 256)
        return n;
    return 0;
}

std::wstring read_ini(const wchar_t* ini, const std::wstring& key) {
    wchar_t text[256] = {};
    GetPrivateProfileStringW(L"hotkeys", key.c_str(), L"\x01", text, static_cast<DWORD>(std::size(text)), ini);
    return text;
}

constexpr const wchar_t* kHatNames[4] = {L"up", L"right", L"down", L"left"};

}  // namespace

bool PadControl::same(const PadControl& o) const {
    return set() && o.set() && control == o.control && _wcsicmp(device.c_str(), o.device.c_str()) == 0;
}

Hotkeys default_hotkeys() {
    Hotkeys h;
    const auto ctrl_alt = [](int vk) { return KeyCombo{vk, true, true, false}; };
    h[static_cast<int>(HotkeyAction::MenuUp)].key = ctrl_alt(VK_UP);
    h[static_cast<int>(HotkeyAction::MenuDown)].key = ctrl_alt(VK_DOWN);
    h[static_cast<int>(HotkeyAction::MenuLeft)].key = ctrl_alt(VK_LEFT);
    h[static_cast<int>(HotkeyAction::MenuRight)].key = ctrl_alt(VK_RIGHT);
    h[static_cast<int>(HotkeyAction::Overlay)].key = ctrl_alt('P');
    h[static_cast<int>(HotkeyAction::Visor)].key = ctrl_alt('V');
    h[static_cast<int>(HotkeyAction::CpuCapture)].key = ctrl_alt('C');
    return h;
}

Hotkeys read_hotkeys(const wchar_t* ini) {
    Hotkeys h = default_hotkeys();
    for (int i = 0; i < kHotkeyActions; ++i) {
        // Unset keys keep the default; an empty value is none.
        const std::wstring key = read_ini(ini, kIniKeys[i]);
        if (key != L"\x01") {
            KeyCombo k;
            h[i].key = parse_key(key, k) ? k : KeyCombo{};
        }
        const std::wstring pad = read_ini(ini, std::wstring(kIniKeys[i]) + L"_pad");
        PadControl p;
        h[i].pad = pad != L"\x01" && parse_pad(pad, p) ? p : PadControl{};
    }
    return h;
}

bool write_hotkeys(const wchar_t* ini, const Hotkeys& h) {
    bool ok = true;
    for (int i = 0; i < kHotkeyActions; ++i) {
        ok &= WritePrivateProfileStringW(L"hotkeys", kIniKeys[i], key_text(h[i].key).c_str(), ini) != 0;
        ok &= WritePrivateProfileStringW(L"hotkeys", (std::wstring(kIniKeys[i]) + L"_pad").c_str(), pad_value(h[i].pad).c_str(),
                                         ini) != 0;
    }
    return ok;
}

const wchar_t* hotkey_ini_key(HotkeyAction action) { return kIniKeys[static_cast<int>(action)]; }

std::wstring key_text(const KeyCombo& k) {
    if (!k.set())
        return {};
    return std::wstring(k.ctrl ? L"Ctrl+" : L"") + (k.alt ? L"Alt+" : L"") + (k.shift ? L"Shift+" : L"") + vk_name(k.vk);
}

bool parse_key(const std::wstring& text, KeyCombo& key) {
    KeyCombo k;
    size_t at = 0;
    while (true) {
        // "+" itself is no key name here (Num + is "Num +", = is the plus key), so split at each "+" but a last one.
        const size_t plus = text.find(L'+', at);
        const std::wstring part = plus == std::wstring::npos || plus + 1 == text.size() ? text.substr(at) : text.substr(at, plus - at);
        const bool last = plus == std::wstring::npos || plus + 1 == text.size();
        if (!last && _wcsicmp(part.c_str(), L"Ctrl") == 0)
            k.ctrl = true;
        else if (!last && _wcsicmp(part.c_str(), L"Alt") == 0)
            k.alt = true;
        else if (!last && _wcsicmp(part.c_str(), L"Shift") == 0)
            k.shift = true;
        else if (last && !part.empty())
            k.vk = vk_of(part);
        else
            return false;
        if (last)
            break;
        at = plus + 1;
    }
    if (!k.set())
        return false;
    key = k;
    return true;
}

std::wstring control_text(int control) {
    if (control < 0)
        return {};
    if (control < kPadButtons)
        return L"Button " + std::to_wstring(control + 1);
    const int hat = (control - kPadHatBase) / 4, dir = (control - kPadHatBase) % 4;
    return (hat ? L"Hat " + std::to_wstring(hat + 1) + L" " : std::wstring(L"Hat ")) + kHatNames[dir];
}

std::wstring pad_value(const PadControl& p) {
    if (!p.set())
        return {};
    std::wstring control;
    if (p.control < kPadButtons)
        control = L"button" + std::to_wstring(p.control + 1);
    else
        control = L"hat" + std::to_wstring((p.control - kPadHatBase) / 4 + 1) + kHatNames[(p.control - kPadHatBase) % 4];
    std::wstring name = p.name;  // no line breaks or ";" (an ini comment) in a value
    for (wchar_t& c : name)
        if (c == L'\r' || c == L'\n' || c == L';')
            c = L' ';
    return p.device + L":" + control + L":" + name;
}

bool parse_pad(const std::wstring& value, PadControl& pad) {
    const size_t a = value.find(L':');
    if (a == std::wstring::npos || a == 0)
        return false;
    const size_t b = value.find(L':', a + 1);
    const std::wstring control = value.substr(a + 1, b == std::wstring::npos ? std::wstring::npos : b - a - 1);
    PadControl p;
    p.device = value.substr(0, a);
    p.name = b == std::wstring::npos ? std::wstring() : value.substr(b + 1);
    int n = 0;
    wchar_t dir[8] = {};
    if (_wcsnicmp(control.c_str(), L"button", 6) == 0 && swscanf_s(control.c_str() + 6, L"%d", &n) == 1 && n >= 1 &&
        n <= kPadButtons) {
        p.control = n - 1;
    } else if (_wcsnicmp(control.c_str(), L"hat", 3) == 0 &&
               swscanf_s(control.c_str() + 3, L"%d%7ls", &n, dir, static_cast<unsigned>(std::size(dir))) == 2 && n >= 1 &&
               n <= kPadHats) {
        for (int d = 0; d < 4; ++d)
            if (_wcsicmp(dir, kHatNames[d]) == 0)
                p.control = kPadHatBase + (n - 1) * 4 + d;
    }
    if (!p.set())
        return false;
    pad = p;
    return true;
}

unsigned hat_directions(unsigned long angle) {
    if (LOWORD(angle) == 0xFFFF)
        return 0;
    const unsigned long a = angle % 36000;
    unsigned bits = 0;
    if (a <= 4500 || a >= 31500)
        bits |= 1u << kHatUp;
    if (a >= 4500 && a <= 13500)
        bits |= 1u << kHatRight;
    if (a >= 13500 && a <= 22500)
        bits |= 1u << kHatDown;
    if (a >= 22500 && a <= 31500)
        bits |= 1u << kHatLeft;
    return bits;
}

int hotkey_clash(const Hotkeys& h, HotkeyAction action, bool pad) {
    const int a = static_cast<int>(action);
    for (int i = 0; i < kHotkeyActions; ++i)
        if (i != a && (pad ? h[i].pad.same(h[a].pad) : h[a].key.set() && h[i].key == h[a].key))
            return i;
    return -1;
}

}  // namespace simqvfr

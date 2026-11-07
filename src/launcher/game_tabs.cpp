#include "game_tabs.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

namespace simqvfr::launcher {
namespace {

std::vector<std::wstring> ids_of(const std::wstring& list) {
    std::vector<std::wstring> ids;
    std::wstring id;
    for (const wchar_t c : list + L",") {
        if (c == L',') {
            while (!id.empty() && std::iswspace(id.back()))
                id.pop_back();
            if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
            id.clear();
        } else if (!(id.empty() && std::iswspace(c))) {
            id += static_cast<wchar_t>(std::towlower(c));
        }
    }
    return ids;
}

double channel(uint32_t rgb, int shift) { return static_cast<double>((rgb >> shift) & 0xff) / 255.0; }

double luminance(uint32_t rgb) {
    const auto linear = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(channel(rgb, 16)) + 0.7152 * linear(channel(rgb, 8)) + 0.0722 * linear(channel(rgb, 0));
}

// `a` moved toward `b` by `f` (0..1), per channel.
uint32_t mix(uint32_t a, uint32_t b, double f) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const double ca = static_cast<double>((a >> shift) & 0xff), cb = static_cast<double>((b >> shift) & 0xff);
        out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * f)) << shift;
    }
    return out;
}

// The window backgrounds the accent is read against (theme_tokens.h kLight / kDark window_bg).
constexpr uint32_t kLightWindow = 0xf2f2f5, kDarkWindow = 0x1c1c1e;

}  // namespace

bool game_hidden(const std::wstring& hidden, const std::wstring& id) {
    std::wstring lower;
    for (const wchar_t c : id)
        lower += static_cast<wchar_t>(std::towlower(c));
    const std::vector<std::wstring> ids = ids_of(hidden);
    return std::find(ids.begin(), ids.end(), lower) != ids.end();
}

std::wstring with_game_hidden(const std::wstring& hidden, const std::wstring& id, bool hide) {
    std::vector<std::wstring> ids = ids_of(hidden);
    const std::vector<std::wstring> one = ids_of(id);
    if (one.empty())
        return hidden;
    ids.erase(std::remove(ids.begin(), ids.end(), one[0]), ids.end());
    if (hide)
        ids.push_back(one[0]);
    std::wstring out;
    for (const std::wstring& i : ids)
        out += (out.empty() ? L"" : L",") + i;
    return out;
}

bool parse_color(const std::wstring& text, uint32_t& rgb) {
    std::wstring t = text;
    while (!t.empty() && std::iswspace(t.front()))
        t.erase(t.begin());
    while (!t.empty() && std::iswspace(t.back()))
        t.pop_back();
    if (!t.empty() && t.front() == L'#')
        t.erase(t.begin());
    if (t.size() != 6 || !std::all_of(t.begin(), t.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; }))
        return false;
    rgb = static_cast<uint32_t>(std::wcstoul(t.c_str(), nullptr, 16));
    return true;
}

std::wstring color_text(uint32_t rgb) {
    wchar_t text[8];
    swprintf(text, 8, L"%06X", rgb & 0xffffffu);
    return text;
}

uint32_t hsv_rgb(float hue, float saturation, float value) {
    const float h = std::fmod(std::fmod(hue, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float s = std::clamp(saturation, 0.0f, 1.0f), v = std::clamp(value, 0.0f, 1.0f);
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h) % 6) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    const auto byte = [m](float f) { return static_cast<uint32_t>(std::lround((f + m) * 255.0f)); };
    return byte(r) << 16 | byte(g) << 8 | byte(b);
}

void rgb_hsv(uint32_t rgb, float& hue, float& saturation, float& value) {
    const float r = static_cast<float>(channel(rgb, 16)), g = static_cast<float>(channel(rgb, 8)),
                b = static_cast<float>(channel(rgb, 0));
    const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    value = hi;
    saturation = hi > 0 ? d / hi : 0;
    if (d <= 0)
        hue = 0;
    else if (hi == r)
        hue = 60 * std::fmod((g - b) / d + 6, 6.0f);
    else if (hi == g)
        hue = 60 * ((b - r) / d + 2);
    else
        hue = 60 * ((r - g) / d + 4);
}

double contrast(uint32_t a, uint32_t b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

AccentColors accent_from(uint32_t rgb, bool dark) {
    AccentColors a;
    a.fill = rgb & 0xffffffu;
    a.onAccent = contrast(0xffffff, a.fill) >= contrast(0x000000, a.fill) ? 0xffffffu : 0x000000u;
    // Toward black (light theme) or white (dark) in small steps until it reads against the window: 3:1, as the
    // built-in accents do.
    const uint32_t window = dark ? kDarkWindow : kLightWindow, toward = dark ? 0xffffffu : 0x000000u;
    a.accent = a.fill;
    for (int step = 1; step <= 20 && contrast(a.accent, window) < 3.0; ++step)
        a.accent = mix(a.fill, toward, step * 0.05);
    a.ring = a.accent;
    return a;
}

}  // namespace simqvfr::launcher

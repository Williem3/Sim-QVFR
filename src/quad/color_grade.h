// Colour correction ([quad] color_correction and its four values; qvfr_settings.h's keys): exposure, brightness,
// contrast and saturation applied to the composed image as the compositor draws it (compose_shader.h color_grade), the
// periphery and the focus views alike, so the focus area never stands out. Off, or on with every value 0, the shader
// leaves the image as it was. The launcher's Colour card, the headset menu's Colour page and the layer read and write
// it through these. Pure: no D3D or OpenXR.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../ini.h"
#include "qvfr_settings.h"

namespace simqvfr {

struct ColorGrade {
    bool on = kColorCorrection.def != 0;
    int exposure = kColorExposure.def;      // tenths of a stop (-30..30: three stops darker to three brighter)
    int brightness = kColorBrightness.def;  // percent, a power curve in display terms, black and white kept (-50..50)
    int contrast = kColorContrast.def;      // percent, an S-curve of the luminance about mid grey, its ends kept (-50..50)
    int saturation = kColorSaturation.def;  // percent more or less colour; -100 grey (-100..100)
    bool operator==(const ColorGrade&) const = default;
    // Whether it changes the image: on, with a value away from 0.
    bool active() const { return on && (exposure || brightness || contrast || saturation); }
};

inline ColorGrade clamped(ColorGrade g) {
    g.exposure = kColorExposure.clamp(g.exposure);
    g.brightness = kColorBrightness.clamp(g.brightness);
    g.contrast = kColorContrast.clamp(g.contrast);
    g.saturation = kColorSaturation.clamp(g.saturation);
    return g;
}

inline ColorGrade read_color_grade(const wchar_t* ini) {
    ColorGrade g;
    g.on = ini_flag(ini, kColorCorrection);
    g.exposure = ini_int(ini, kColorExposure);
    g.brightness = ini_int(ini, kColorBrightness);
    g.contrast = ini_int(ini, kColorContrast);
    g.saturation = ini_int(ini, kColorSaturation);
    return g;
}

inline bool write_color_grade(const wchar_t* ini, const ColorGrade& in) {
    const ColorGrade g = clamped(in);
    bool ok = ini_put(ini, kColorCorrection.section, kColorCorrection.key, g.on ? 1 : 0);
    ok &= ini_put(ini, kColorExposure.section, kColorExposure.key, g.exposure);
    ok &= ini_put(ini, kColorBrightness.section, kColorBrightness.key, g.brightness);
    ok &= ini_put(ini, kColorContrast.section, kColorContrast.key, g.contrast);
    ok &= ini_put(ini, kColorSaturation.section, kColorSaturation.key, g.saturation);
    return ok;
}

// Packed into one word for the layer's live atomic: bit 0 on, then each value + 128 a byte (exposure, brightness,
// contrast, saturation).
inline uint64_t color_grade_bits(const ColorGrade& in) {
    const ColorGrade g = clamped(in);
    const auto byte = [](int v) { return static_cast<uint64_t>(v + 128) & 0xff; };
    return (g.on ? 1u : 0u) | byte(g.exposure) << 8 | byte(g.brightness) << 16 | byte(g.contrast) << 24 |
           byte(g.saturation) << 32;
}
inline ColorGrade color_grade_from_bits(uint64_t bits) {
    const auto value = [bits](int shift) { return static_cast<int>((bits >> shift) & 0xff) - 128; };
    ColorGrade g;
    g.on = (bits & 1) != 0;
    g.exposure = value(8);
    g.brightness = value(16);
    g.contrast = value(24);
    g.saturation = value(32);
    return clamped(g);
}
inline constexpr uint64_t kColorGradeNone = 128ull << 8 | 128ull << 16 | 128ull << 24 | 128ull << 32;  // off, all 0

// The shader's three constants (compose_shader.h color_grade): x the exposure's scale (2^stops; 0: the image as it
// was), y the brightness's power in display terms (2^(-1.5 x brightness): +50% 0.59, lighter; -50% 1.68, darker), z
// the contrast's S-curve exponent in thousandths (1 + contrast: 500..1500) + 2048 x the saturation's scale in
// thousandths (0..2000): a whole number under 2^24, exact in a float (no bit pattern a driver could flush).
inline constexpr int kColorPackShift = 2048;
inline void color_grade_constants(const ColorGrade& in, float (&out)[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!in.active())
        return;
    const ColorGrade g = clamped(in);
    out[0] = static_cast<float>(std::exp2(g.exposure / 10.0));
    out[1] = static_cast<float>(std::exp2(-1.5 * g.brightness / 100.0));
    const int contrast = 1000 + 10 * g.contrast, saturation = 1000 + 10 * g.saturation;
    out[2] = static_cast<float>(contrast + kColorPackShift * saturation);
}

// "+0.5 EV", "-1.2 EV", "0 EV".
inline std::wstring color_exposure_text(int tenths) {
    if (tenths == 0)
        return L"0 EV";
    const int a = tenths < 0 ? -tenths : tenths;
    return std::wstring(tenths > 0 ? L"+" : L"-") + std::to_wstring(a / 10) + L"." + std::to_wstring(a % 10) + L" EV";
}
// "+12%", "-30%", "0%".
inline std::wstring color_percent_text(int percent) {
    return (percent > 0 ? L"+" : L"") + std::to_wstring(percent) + L"%";
}
// For the log: "off", or "exposure +0.5 EV, brightness +4%, contrast +10%, saturation -20%".
inline std::string color_grade_log(const ColorGrade& g) {
    if (!g.on)
        return "off";
    char text[160];
    snprintf(text, sizeof(text), "exposure %+.1f EV, brightness %+d%%, contrast %+d%%, saturation %+d%%%s",
             g.exposure / 10.0, g.brightness, g.contrast, g.saturation, g.active() ? "" : " (no change)");
    return text;
}

}  // namespace simqvfr

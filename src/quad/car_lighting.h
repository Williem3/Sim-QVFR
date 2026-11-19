// Night car lighting ([quad] car_light and its four values; qvfr_settings.h's keys): a game that relights its cars
// after its scene (GameFeatures::carLighting; LMU's games/lmu/car_light.h) reads these. The headset menu, the layer and
// the game read and write them through here. Pure: no D3D or OpenXR.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../ini.h"
#include "qvfr_settings.h"

namespace simqvfr {

struct CarLighting {
    bool on = kCarLight.def != 0;
    int fill = kCarLightFill.def;              // the game's own light on the cars kept, percent (30..100)
    int occlusion = kCarLightOcclusion.def;    // ambient occlusion's strength, percent (0..100)
    int highlights = kCarLightHighlights.def;  // the floodlights' lacquer highlights, percent (0..200)
    int gloss = kCarLightGloss.def;            // the lacquer's gloss, percent (0 rough .. 100 mirror-like)
    int smooth = kCarLightSmooth.def;          // the surface directions smoothed over this width, cm (0..50; 0 none)
    bool operator==(const CarLighting&) const = default;
};

inline CarLighting clamped(CarLighting c) {
    c.fill = kCarLightFill.clamp(c.fill);
    c.occlusion = kCarLightOcclusion.clamp(c.occlusion);
    c.highlights = kCarLightHighlights.clamp(c.highlights);
    c.gloss = kCarLightGloss.clamp(c.gloss);
    c.smooth = kCarLightSmooth.clamp(c.smooth);
    return c;
}

inline CarLighting read_car_lighting(const wchar_t* ini) {
    CarLighting c;
    c.on = ini_flag(ini, kCarLight);
    c.fill = ini_int(ini, kCarLightFill);
    c.occlusion = ini_int(ini, kCarLightOcclusion);
    c.highlights = ini_int(ini, kCarLightHighlights);
    c.gloss = ini_int(ini, kCarLightGloss);
    c.smooth = ini_int(ini, kCarLightSmooth);
    return c;
}

inline bool write_car_lighting(const wchar_t* ini, const CarLighting& in) {
    const CarLighting c = clamped(in);
    bool ok = ini_put(ini, kCarLight.section, kCarLight.key, c.on ? 1 : 0);
    ok &= ini_put(ini, kCarLightFill.section, kCarLightFill.key, c.fill);
    ok &= ini_put(ini, kCarLightOcclusion.section, kCarLightOcclusion.key, c.occlusion);
    ok &= ini_put(ini, kCarLightHighlights.section, kCarLightHighlights.key, c.highlights);
    ok &= ini_put(ini, kCarLightGloss.section, kCarLightGloss.key, c.gloss);
    ok &= ini_put(ini, kCarLightSmooth.section, kCarLightSmooth.key, c.smooth);
    return ok;
}

// Packed into one word for the live atomic: bit 0 on, then a byte each (fill, occlusion, highlights, gloss, smooth).
inline uint64_t car_lighting_bits(const CarLighting& in) {
    const CarLighting c = clamped(in);
    const auto byte = [](int v) { return static_cast<uint64_t>(v) & 0xff; };
    return (c.on ? 1u : 0u) | byte(c.fill) << 8 | byte(c.occlusion) << 16 | byte(c.highlights) << 24 |
           byte(c.gloss) << 32 | byte(c.smooth) << 40;
}
inline CarLighting car_lighting_from_bits(uint64_t bits) {
    const auto value = [bits](int shift) { return static_cast<int>((bits >> shift) & 0xff); };
    CarLighting c;
    c.on = (bits & 1) != 0;
    c.fill = value(8);
    c.occlusion = value(16);
    c.highlights = value(24);
    c.gloss = value(32);
    c.smooth = value(40);
    return clamped(c);
}

// The settings as the layer applied them last (live_apply), read from any thread (the game's render thread).
inline std::atomic<uint64_t>& car_lighting_state() {
    static std::atomic<uint64_t> bits{car_lighting_bits(CarLighting{})};
    return bits;
}
inline CarLighting car_lighting() { return car_lighting_from_bits(car_lighting_state().load(std::memory_order_relaxed)); }
inline void set_car_lighting(const CarLighting& c) {
    car_lighting_state().store(car_lighting_bits(c), std::memory_order_relaxed);
}

// For the log: "off", or "fill 80%, occlusion 70%, highlights 100%, gloss 70%, smoothing 15 cm".
inline std::string car_lighting_log(const CarLighting& c) {
    if (!c.on)
        return "off";
    char text[128];
    snprintf(text, sizeof(text), "fill %d%%, occlusion %d%%, highlights %d%%, gloss %d%%, smoothing %d cm", c.fill,
             c.occlusion, c.highlights, c.gloss, c.smooth);
    return text;
}

}  // namespace simqvfr

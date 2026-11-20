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
    bool shadows = kCarLightShadows.def != 0;  // the floodlights' shadows
    bool detail = kCarLightDetail.def != 0;    // the paint's fine detail (the game's normal maps)
    bool reflections = kCarLightReflections.def != 0;  // the lacquer's reflections of what the eye sees
    bool headlights = kCarLightHeadlights.def != 0;    // the headlights' highlights on the paint
    bool ground = kCarLightGround.def != 0;            // the cars' shadows on the ground
    bool nightVision = kCarLightNightVision.def != 0;  // night vision's colour
    int quality = kCarLightQuality.def;                // each effect's samples and steps: 0 Low, 1 Medium, 2 High
    bool operator==(const CarLighting&) const = default;
};

inline CarLighting clamped(CarLighting c) {
    c.fill = kCarLightFill.clamp(c.fill);
    c.occlusion = kCarLightOcclusion.clamp(c.occlusion);
    c.highlights = kCarLightHighlights.clamp(c.highlights);
    c.gloss = kCarLightGloss.clamp(c.gloss);
    c.smooth = kCarLightSmooth.clamp(c.smooth);
    c.quality = kCarLightQuality.clamp(c.quality);
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
    c.shadows = ini_flag(ini, kCarLightShadows);
    c.detail = ini_flag(ini, kCarLightDetail);
    c.reflections = ini_flag(ini, kCarLightReflections);
    c.headlights = ini_flag(ini, kCarLightHeadlights);
    c.ground = ini_flag(ini, kCarLightGround);
    c.nightVision = ini_flag(ini, kCarLightNightVision);
    c.quality = ini_int(ini, kCarLightQuality);
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
    ok &= ini_put(ini, kCarLightShadows.section, kCarLightShadows.key, c.shadows ? 1 : 0);
    ok &= ini_put(ini, kCarLightDetail.section, kCarLightDetail.key, c.detail ? 1 : 0);
    ok &= ini_put(ini, kCarLightReflections.section, kCarLightReflections.key, c.reflections ? 1 : 0);
    ok &= ini_put(ini, kCarLightHeadlights.section, kCarLightHeadlights.key, c.headlights ? 1 : 0);
    ok &= ini_put(ini, kCarLightGround.section, kCarLightGround.key, c.ground ? 1 : 0);
    ok &= ini_put(ini, kCarLightNightVision.section, kCarLightNightVision.key, c.nightVision ? 1 : 0);
    ok &= ini_put(ini, kCarLightQuality.section, kCarLightQuality.key, c.quality);
    return ok;
}

// Packed into one word for the live atomic: bit 0 on, then a byte each (fill, occlusion, highlights, gloss, smooth),
// then bit 48 the shadows, bit 49 the detail, bit 50 the reflections, bits 51-53 the headlights, the ground, night vision,
// bits 56-57 the quality.
inline uint64_t car_lighting_bits(const CarLighting& in) {
    const CarLighting c = clamped(in);
    const auto byte = [](int v) { return static_cast<uint64_t>(v) & 0xff; };
    return (c.on ? 1u : 0u) | byte(c.fill) << 8 | byte(c.occlusion) << 16 | byte(c.highlights) << 24 |
           byte(c.gloss) << 32 | byte(c.smooth) << 40 | uint64_t{c.shadows ? 1u : 0u} << 48 |
           uint64_t{c.detail ? 1u : 0u} << 49 | uint64_t{c.reflections ? 1u : 0u} << 50 |
           uint64_t{c.headlights ? 1u : 0u} << 51 | uint64_t{c.ground ? 1u : 0u} << 52 |
           uint64_t{c.nightVision ? 1u : 0u} << 53 | (static_cast<uint64_t>(c.quality) & 3) << 56;
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
    c.shadows = (bits >> 48) & 1;
    c.detail = (bits >> 49) & 1;
    c.reflections = (bits >> 50) & 1;
    c.headlights = (bits >> 51) & 1;
    c.ground = (bits >> 52) & 1;
    c.nightVision = (bits >> 53) & 1;
    c.quality = static_cast<int>((bits >> 56) & 3);
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

// For the log: "off", or "fill 80%, occlusion 70%, highlights 100%, gloss 70%, smoothing 15 cm, shadows on, detail on".
// Quality presets: the effects' tier (car_light_quality); the switches stay as the player set them. The A/B of
// 2026-11-19 (race traffic, everything on, High): 2.70 ms GPU, the base relight ~1.0, ground shadows 0.56, floodlight
// shadows 0.50, reflections 0.15, paint detail 0.10, headlights 0.05, night vision 0.03.
inline constexpr int kCarLightPresets = 3;
inline void car_lighting_preset(CarLighting& c, int preset) { c.quality = kCarLightQuality.clamp(preset); }
// The preset in use (every quality is one).
inline int car_lighting_preset_of(const CarLighting& c) { return kCarLightQuality.clamp(c.quality); }
// A step (Left -1, Right +1), held at Low and High.
inline int car_lighting_preset_step(const CarLighting& c, int step) {
    return kCarLightQuality.clamp(car_lighting_preset_of(c) + step);
}

inline std::string car_lighting_log(const CarLighting& c) {
    if (!c.on)
        return "off";
    char text[256];
    static const char* const kQuality[3] = {"low", "medium", "high"};
    snprintf(text, sizeof(text), "quality %s, fill %d%%, occlusion %d%%, highlights %d%%, gloss %d%%, smoothing %d cm, "
             "shadows %s, detail %s, reflections %s, headlights %s, ground %s, night vision %s",
             kQuality[kCarLightQuality.clamp(c.quality)], c.fill, c.occlusion, c.highlights, c.gloss, c.smooth,
             c.shadows ? "on" : "off", c.detail ? "on" : "off", c.reflections ? "on" : "off",
             c.headlights ? "on" : "off", c.ground ? "on" : "off", c.nightVision ? "on" : "off");
    return text;
}

}  // namespace simqvfr

// The GPU regulator's settings ([regulator], live): it watches the GPU time of the game's eye passes and takes
// Periphery and/or Focus density down one percent at a time while frames run over `decreaseAbove`, and back up
// while they run under `increaseBelow`. The set values (periphery_scale, focus_density) are the ceiling: the
// regulator never goes above them and never writes them. For a game whose adapter makes QVFR (rFactor 2).
// Shared by the layer (games/rf2/regulator.h acts on it) and the launcher.
#pragma once

#include <algorithm>
#include <string>

#include <windows.h>

#include "../ini_put.h"

namespace simqvfr {

// Milliseconds are in tenths (the settings are ints, as the gaze dead-zone's).
inline constexpr int kRegulatorMsMin = 30, kRegulatorMsMax = 200;
// Frames over the lower limit before a step down (fast), and under the raise limit before a step up (slow).
inline constexpr int kRegulatorLowerFramesMin = 1, kRegulatorLowerFramesMax = 50;
inline constexpr int kRegulatorRaiseFramesMin = 50, kRegulatorRaiseFramesMax = 300;
inline constexpr int kRegulatorDecreaseDefault = 100;  // 10.0 ms: pacing holds up to about this on a 90 Hz headset
inline constexpr int kRegulatorIncreaseDefault = 90;   // 9.0 ms
inline constexpr int kRegulatorLowerFramesDefault = 1;   // react at once to a heavy scene
inline constexpr int kRegulatorRaiseFramesDefault = 50;  // about 0.6 s at 90 Hz

struct GpuRegulatorSettings {
    bool focus = false;      // [regulator] focus: Focus density is regulated
    bool periphery = false;  // [regulator] periphery: Periphery resolution is regulated
    int decreaseAbove = kRegulatorDecreaseDefault;  // [regulator] decrease_above: tenths of a ms of GPU time per frame
    int increaseBelow = kRegulatorIncreaseDefault;  // [regulator] increase_below: tenths of a ms
    int lowerFrames = kRegulatorLowerFramesDefault;  // [regulator] lower_frames: frames over decrease_above before a 1% step down
    int raiseFrames = kRegulatorRaiseFramesDefault;  // [regulator] raise_frames: frames under increase_below before a 1% step up
    bool on() const { return focus || periphery; }
    bool operator==(const GpuRegulatorSettings&) const = default;
};

// The settings as they act: in range, and the "increase" limit never above the "decrease" one (a band between them
// where nothing happens).
inline GpuRegulatorSettings clamp_regulator(GpuRegulatorSettings s) {
    s.decreaseAbove = std::clamp(s.decreaseAbove, kRegulatorMsMin, kRegulatorMsMax);
    s.increaseBelow = std::clamp(s.increaseBelow, kRegulatorMsMin, kRegulatorMsMax);
    s.increaseBelow = std::min(s.increaseBelow, s.decreaseAbove);
    s.lowerFrames = std::clamp(s.lowerFrames, kRegulatorLowerFramesMin, kRegulatorLowerFramesMax);
    s.raiseFrames = std::clamp(s.raiseFrames, kRegulatorRaiseFramesMin, kRegulatorRaiseFramesMax);
    return s;
}

// "10.5": tenths as a number with one decimal (the launcher's slider text for milliseconds, and the gaze dead-zone's).
inline std::wstring tenths_text(int tenths) {
    const int v = std::max(tenths, 0);
    return std::to_wstring(v / 10) + L"." + std::to_wstring(v % 10);
}

inline GpuRegulatorSettings read_regulator(const wchar_t* ini) {
    const auto number = [ini](const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(L"regulator", key, fallback, ini));
    };
    GpuRegulatorSettings s;
    s.focus = number(L"focus", 0) != 0;
    s.periphery = number(L"periphery", 0) != 0;
    s.decreaseAbove = number(L"decrease_above", kRegulatorDecreaseDefault);
    s.increaseBelow = number(L"increase_below", kRegulatorIncreaseDefault);
    s.lowerFrames = number(L"lower_frames", kRegulatorLowerFramesDefault);
    s.raiseFrames = number(L"raise_frames", kRegulatorRaiseFramesDefault);
    return clamp_regulator(s);
}

inline bool write_regulator(const wchar_t* ini, const GpuRegulatorSettings& in) {
    const GpuRegulatorSettings s = clamp_regulator(in);
    bool ok = ini_put(ini, L"regulator", L"focus", s.focus ? 1 : 0);
    ok &= ini_put(ini, L"regulator", L"periphery", s.periphery ? 1 : 0);
    ok &= ini_put(ini, L"regulator", L"decrease_above", s.decreaseAbove);
    ok &= ini_put(ini, L"regulator", L"increase_below", s.increaseBelow);
    ok &= ini_put(ini, L"regulator", L"lower_frames", s.lowerFrames);
    ok &= ini_put(ini, L"regulator", L"raise_frames", s.raiseFrames);
    return ok;
}

}  // namespace simqvfr

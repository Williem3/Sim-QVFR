#pragma once

#include <algorithm>

#include "regulator_settings.h"

namespace simqvfr {

// The GPU regulator's decision, pure (no graphics API and no threading of its own; callers provide synchronization).
//
// It is fed the GPU time of every timed frame by the game adapter.
// A frame over `decreaseAbove` counts towards a step down, a frame under `increaseBelow` towards a step up; the band
// between the two counts for neither. A frame in the opposite zone resets a count, so a step up needs `raiseFrames` calm
// frames (many: it comes back slowly) with no frame over the upper limit between, and a step down needs `lowerFrames`
// frames (few: it reacts fast) over the limit without a calm one between them (a rare spike in a light scene never adds
// up). A step is one percentage point of the regulated setting; both counts restart after every step.
//
// The time is the GPU's wall clock over the eye passes, which includes any time the GPU waited for the CPU's commands:
// a CPU-bound frame runs long without the GPU being busy. A step down therefore also needs the GPU busy (`gpuBound`
// in feed: Windows' engine counters, probe/gpu_load.h), so a CPU-bound stretch never takes the views down for nothing;
// a frame over the limit while the GPU isn't busy counts for neither way.
//
// The set values are the ceiling: the regulator keeps only how far below them it is (`cut`, in percentage points),
// at most down to a floor. Nothing is written to the ini.
inline constexpr int kRegulatorPeripheryFloor = 20;  // percent of the native eye
inline constexpr int kRegulatorDensityFloor = 50;    // percent of the native eye's pixel density (the menu's lowest)
inline constexpr double kRegulatorIgnoreMs = 100;    // a frame this long is a hitch (loading, a pause), not load
inline constexpr double kRegulatorBusyPercent = 90;  // the GPU's busiest 3D engine at least this busy: GPU-bound

class Regulator {
public:
    // The settings (clamped). True when the cuts changed (an axis turned off goes back to its set value).
    bool set_settings(const GpuRegulatorSettings& in) {
        const GpuRegulatorSettings s = clamp_regulator(in);
        if (s == settings_) return false;
        const int p = cutPeriphery_, d = cutDensity_;
        settings_ = s;
        over_ = under_ = 0;
        sum_ = 0;
        if (!s.periphery) cutPeriphery_ = 0;
        if (!s.focus) cutDensity_ = 0;
        return p != cutPeriphery_ || d != cutDensity_;
    }
    const GpuRegulatorSettings& settings() const { return settings_; }
    bool active() const { return settings_.on(); }

    // How far each axis can go down from the set value (percentage points), from the set values and the floors. True when
    // a cut was above it and came down.
    bool set_limits(int maxPeriphery, int maxDensity) {
        maxPeriphery_ = std::max(0, maxPeriphery);
        maxDensity_ = std::max(0, maxDensity);
        const int p = cutPeriphery_, d = cutDensity_;
        cutPeriphery_ = std::min(cutPeriphery_, maxPeriphery_);
        cutDensity_ = std::min(cutDensity_, maxDensity_);
        return p != cutPeriphery_ || d != cutDensity_;
    }

    // One timed frame's GPU time in ms, and whether the GPU is busy now (true when that isn't known). True when it made a
    // step that changed a cut.
    bool feed(double ms, bool gpuBound = true) {
        if (!settings_.on() || !(ms > 0) || ms > kRegulatorIgnoreMs) return false;
        const double high = settings_.decreaseAbove / 10.0, low = settings_.increaseBelow / 10.0;
        if (ms > high && !gpuBound) {  // long, but the GPU waited: not its load
            over_ = under_ = 0;
            sum_ = 0;
            ++heldNotBound_;
            return false;
        }
        if (ms > high) {
            if (over_ == 0) sum_ = 0;
            ++over_;
            under_ = 0;
            sum_ += ms;
        } else if (ms < low) {
            if (under_ == 0) sum_ = 0;
            ++under_;
            over_ = 0;
            sum_ += ms;
        }
        if (over_ >= settings_.lowerFrames) return step(+1);
        if (under_ >= settings_.raiseFrames) return step(-1);
        return false;
    }

    int periphery_cut() const { return cutPeriphery_; }
    int density_cut() const { return cutDensity_; }
    // The last step's direction (+1 down in quality, -1 up) and the mean GPU time of the frames that made it.
    int last_direction() const { return lastDirection_; }
    double last_mean_ms() const { return lastMean_; }
    // Frames over the limit that didn't count because the GPU wasn't busy (since the start).
    unsigned long long held_not_gpu_bound() const { return heldNotBound_; }

private:
    bool step(int direction) {
        lastDirection_ = direction;
        const int count = direction > 0 ? over_ : under_;
        lastMean_ = count ? sum_ / count : 0;
        over_ = under_ = 0;
        sum_ = 0;
        const int p = cutPeriphery_, d = cutDensity_;
        if (settings_.periphery) cutPeriphery_ = std::clamp(cutPeriphery_ + direction, 0, maxPeriphery_);
        if (settings_.focus) cutDensity_ = std::clamp(cutDensity_ + direction, 0, maxDensity_);
        return p != cutPeriphery_ || d != cutDensity_;
    }

    GpuRegulatorSettings settings_;
    int cutPeriphery_ = 0, cutDensity_ = 0, maxPeriphery_ = 0, maxDensity_ = 0;
    int over_ = 0, under_ = 0;
    double sum_ = 0, lastMean_ = 0;
    int lastDirection_ = 0;
    unsigned long long heldNotBound_ = 0;
};

}  // namespace simqvfr

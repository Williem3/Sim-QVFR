// The session record's numbers over a stint: the driving between two settings changes, with the report windows
// (900 frames, ~10 s each) the headset menu was open in left out. The launcher's Diagnostics compares stints, so a
// change made in the headset gets its own record, and the numbers cover the whole stint, not its last ten seconds.
// Pure: the layer feeds it each report window (layer/reports.cpp) and writes what it holds.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace simqvfr {

// One report window: the game's frame intervals (ms), the display period, the runtime's slow frames in it, and the
// render thread's CPU and the GPU per frame, the GPU's busy share and the cores in use (-1: not measured).
struct StintWindow {
    const std::vector<float>* interval = nullptr;
    double periodMs = 0;
    uint64_t slowFrames = 0;  // frames the runtime gave a longer display period (half rate or slower)
    double cpuMs = -1, gpuMs = -1, gpuBusy = -1, cores = -1;
};

// A stint's numbers, as the session summary has them.
struct StintNumbers {
    int index = 0;  // 1 the session's first stint
    double seconds = 0, fps = 0, frameP50 = 0, frameP90 = 0, latePercent = 0, halfRatePercent = 0, periodMs = 0;
    double cpuMs = -1, gpuMs = -1, gpuBusy = -1, cores = -1;
};

class SessionStint {
public:
    // The frames a stint keeps for its percentiles, a sample: past it, every other one is dropped and later windows add
    // every other frame (then every fourth, ...), so the sample stays even over the stint. Small, since the percentiles
    // are taken on the render thread every report (~10 s): ~0.1 ms at this size.
    static constexpr size_t kMaxFrames = 1u << 14;

    // A window of the stint whose settings are `generation`. A new generation starts the next stint and drops this
    // window (its frames ran under both settings); `menuOpen` drops it too (the menu's frames aren't the drive's).
    void add(uint32_t generation, bool menuOpen, const StintWindow& w) {
        if (generation != generation_) {
            generation_ = generation;
            if (frames_ > 0)
                ++index_;
            clear();
            return;
        }
        if (menuOpen || !w.interval || w.interval->empty())
            return;
        double sum = 0;
        size_t late = 0;
        for (const float i : *w.interval) {
            sum += i;
            if (w.periodMs > 0 && std::lround(i / w.periodMs) >= 2)
                ++late;
        }
        const double n = static_cast<double>(w.interval->size());
        frames_ += w.interval->size();
        ms_ += sum;
        late_ += late;
        slow_ += w.slowFrames;
        periodMs_ = w.periodMs;
        for (const float i : *w.interval)
            if (sampled_++ % stride_ == 0)
                intervals_.push_back(i);
        if (intervals_.size() > kMaxFrames) {  // every other one: the percentiles keep their place
            size_t k = 0;
            for (size_t i = 0; i < intervals_.size(); i += 2)
                intervals_[k++] = intervals_[i];
            intervals_.resize(k);
            stride_ *= 2;
        }
        const auto mean = [n](Mean& m, double v) {
            if (v >= 0) {
                m.sum += v * n;
                m.frames += n;
            }
        };
        mean(cpu_, w.cpuMs);
        mean(gpu_, w.gpuMs);
        mean(busy_, w.gpuBusy);
        mean(cores_, w.cores);
    }

    // Whether it has numbers yet (a stint just begun, or the menu open the whole time: none).
    bool known() const { return frames_ > 0 && ms_ > 0; }

    StintNumbers numbers() const {
        StintNumbers s;
        s.index = index_;
        if (!known())
            return s;
        const double n = static_cast<double>(frames_);
        s.seconds = ms_ / 1000;
        s.fps = 1000 * n / ms_;
        percentiles(s.frameP50, s.frameP90);
        s.latePercent = 100.0 * static_cast<double>(late_) / n;
        s.halfRatePercent = std::min(100.0, 100.0 * static_cast<double>(slow_) / n);
        s.periodMs = periodMs_;
        s.cpuMs = cpu_.value();
        s.gpuMs = gpu_.value();
        s.gpuBusy = busy_.value();
        s.cores = cores_.value();
        return s;
    }

private:
    struct Mean {
        double sum = 0, frames = 0;
        double value() const { return frames > 0 ? sum / frames : -1; }
    };

    void clear() {
        intervals_.clear();
        frames_ = late_ = slow_ = 0;
        sampled_ = 0;
        stride_ = 1;
        ms_ = periodMs_ = 0;
        cpu_ = gpu_ = busy_ = cores_ = {};
    }

    void percentiles(double& p50, double& p90) const {
        std::vector<float> v = intervals_;
        p50 = p90 = 0;
        if (v.empty())
            return;
        const auto rank = [&](double p) { return std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size()))); };
        const auto mid = v.begin() + static_cast<std::ptrdiff_t>(rank(0.5)), high = v.begin() + static_cast<std::ptrdiff_t>(rank(0.9));
        std::nth_element(v.begin(), mid, v.end());
        p50 = *mid;
        std::nth_element(mid, high, v.end());  // above the median: the 90th percentile's rank is there
        p90 = *high;
    }

    uint32_t generation_ = 0;
    int index_ = 1;
    std::vector<float> intervals_;
    size_t frames_ = 0, late_ = 0;
    uint64_t slow_ = 0, sampled_ = 0, stride_ = 1;
    double ms_ = 0, periodMs_ = 0;
    Mean cpu_, gpu_, busy_, cores_;
};

}  // namespace simqvfr

// The settings menu's "Optimize" (QVFR): finds the periphery, focus density and focus box that keep
// ACE's GPU time just under the frame budget, so it holds the headset's rate (90 fps at 90 Hz). It
// measures a couple of seconds of frames, picks values from a cost model, applies them, measures again
// and refines the model, a few times over. Pure: the layer feeds it measurement windows and applies the
// values it asks for (xr_layer.cpp).
//
// The model: GPU time = fixed + perPixel x pixels, where pixels is what the four views render against
// the native eye (qvfr_pixels). One measurement cannot split the two, so the first step assumes
// kFixedShare of the time is fixed; every later one fits the line through the last two measurements.
//
// Which values give: lowering, the periphery first (least noticed), then the focus density, then the
// box; raising (headroom left), the other way round, up to `ceiling`. A pixel count maps to one set of
// values (qvfr_values_for), the same however the search got there.
//
// With DLSS on, the views keep their launch sizes (views4_dlss_held): nothing can be measured live, so
// one step from the first measurement is written for the next launch (held).
#pragma once

#include <string>

namespace aceqv {

// The QVFR resolution keys, percent: periphery_scale, focus_density, focus_percent, focus_height_percent.
struct QvfrValues {
    int periphery = 50, density = 100, width = 39, height = 39;
    bool operator==(const QvfrValues&) const = default;
};

// Pixels the views render, in native eyes (2.0: both eyes at the native size): two outer views at
// `periphery` over the crop's share of the eye (percent), two focus views at density x the box.
double qvfr_pixels(const QvfrValues& v, double cropWidth = 100, double cropHeight = 100);

struct QvfrLimits {
    QvfrValues floor{25, 80, 25, 25};     // the least the optimizer lowers to ([optimize] min_*)
    QvfrValues ceiling{100, 200, 100, 100};  // the most it raises to (live: the launch size)
    double cropWidth = 100, cropHeight = 100;  // the FOV crop's kept share of the eye, percent
    int nativeWidth = 0;                       // the native eye in pixels (0: unknown, no target width check)
    int maxTargetWidth = 8192;                 // the runtime's widest image: ACE's target is the four views in a row
};

// The values for a pixel count from `start`, by the order above; steps of 5 for the periphery and the
// density, 1 for the box, rounded to stay at or under `pixels` (or the floors, when they are above it).
QvfrValues qvfr_values_for(double pixels, const QvfrValues& start, const QvfrLimits& limits);

// One measurement window (about half a second of frames).
struct QvfrWindow {
    double gpuMs = -1;     // ACE's GPU time per frame, 90th percentile (-1: not timed)
    double cpuMs = -1;     // the render thread's work per frame (-1: unknown)
    double budgetMs = 0;   // the frame budget (the display period; 0: unknown)
    bool cpuBound = false; // the overlay's rule says the CPU limits the frame
    size_t frames = 0;
};

enum class QvfrState { Idle, Measuring, Settling, Done, Failed };

// What the layer does after a window: nothing, or apply `values` (written to the ini).
struct QvfrAction {
    bool apply = false;
    QvfrValues values;
};

class QvfrOptimizer {
public:
    static constexpr double kTargetShare = 0.88;   // of the budget: room for busier moments on track
    static constexpr double kBandShare = 0.08;     // done within this much under the target
    static constexpr double kFixedShare = 0.3;     // the first step's guess at the fixed part
    static constexpr int kSettleWindows = 3;       // after applying: the new sizes in, DLSS/TAA history settled
    static constexpr int kMeasureWindows = 4;      // then this many are measured (about 2 s)
    static constexpr int kMaxSteps = 5;

    // Starts from the values in effect. `held`: DLSS keeps the launch sizes (one step, for the next launch).
    // `targetShare`: of the budget ([optimize] target_percent).
    void start(const QvfrValues& current, const QvfrLimits& limits, bool held, double targetShare = kTargetShare);
    // Back to the values it started from (the layer applies them); Idle.
    QvfrAction undo();
    void cancel();
    QvfrAction feed(const QvfrWindow& window);

    QvfrState state() const { return state_; }
    bool running() const { return state_ == QvfrState::Measuring || state_ == QvfrState::Settling; }
    bool can_undo() const { return started_ && !(current_ == start_); }
    const QvfrValues& values() const { return current_; }
    // For the menu line and the log: "Measuring…", "9.6 ms of 11.1 (target 9.8)", why it stopped.
    const std::wstring& status() const { return status_; }

private:
    struct Point {
        double pixels = 0, gpuMs = 0;
    };
    QvfrAction step(double gpuMs, double budgetMs);
    void finish(QvfrState state, std::wstring status);

    QvfrState state_ = QvfrState::Idle;
    QvfrLimits limits_;
    QvfrValues start_, current_;
    bool held_ = false, started_ = false;
    double targetShare_ = kTargetShare;
    int windows_ = 0, steps_ = 0;
    double gpuSum_ = 0, cpuSum_ = 0, budget_ = 0;
    int gpuCount_ = 0, cpuCount_ = 0, cpuBoundCount_ = 0;
    Point last_, previous_;
    bool havePrevious_ = false;
    std::wstring status_ = L"Right: run";
};

}  // namespace aceqv

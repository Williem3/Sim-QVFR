#include "qvfr_optimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace simqvfr {
namespace {

double outer_pixels(int periphery, double cw, double ch) {
    return 2.0 * (periphery / 100.0) * (periphery / 100.0) * (cw / 100) * (ch / 100);
}

double focus_pixels(int density, int width, int height) {
    return 2.0 * (density / 100.0 * width / 100.0) * (density / 100.0 * height / 100.0);
}

// Down to a multiple of `step`.
int down(double v, int step) { return static_cast<int>(std::floor(v / step + 1e-9)) * step; }

// The most `x` may be so that ACE's target (the four views in a row) stays within the runtime's width:
// 2 x native x (periphery x crop + density x box) <= maxTargetWidth. `other` is the other view's share.
double width_room(const QvfrLimits& l, double other, double perUnit) {
    if (l.nativeWidth <= 0 || perUnit <= 0)
        return 1e9;
    return (static_cast<double>(l.maxTargetWidth) / (2.0 * l.nativeWidth) - other) / perUnit;
}

std::wstring ms(double v) {
    wchar_t text[16];
    swprintf_s(text, L"%.1f", v);
    return text;
}

}  // namespace

double qvfr_pixels(const QvfrValues& v, double cropWidth, double cropHeight) {
    return outer_pixels(v.periphery, cropWidth, cropHeight) + focus_pixels(v.density, v.width, v.height);
}

QvfrValues qvfr_values_for(double pixels, const QvfrValues& start, const QvfrLimits& l) {
    const double cw = l.cropWidth, ch = l.cropHeight;
    // The start's own values stay reachable: floors no higher, ceilings no lower. The box never grows past
    // the start's (it is the user's layout; only the pixels it costs are traded).
    const QvfrValues lo{std::min(l.floor.periphery, start.periphery), std::min(l.floor.density, start.density),
                        std::min(l.floor.width, start.width), std::min(l.floor.height, start.height)};
    const QvfrValues hi{std::max(l.ceiling.periphery, start.periphery), std::max(l.ceiling.density, start.density),
                        start.width, start.height};
    QvfrValues v = start;
    if (pixels >= qvfr_pixels(start, cw, ch)) {
        // Raising: the focus density first (what you look at), then the periphery.
        const double outerShare = v.periphery / 100.0 * cw / 100;
        const double room = width_room(l, outerShare, v.width / 100.0) * 100;
        const double wantDensity = 100 * std::sqrt(std::max(0.0, pixels - outer_pixels(v.periphery, cw, ch)) /
                                                   (2.0 * v.width / 100 * v.height / 100));
        v.density = std::max(start.density, down(std::min({wantDensity, static_cast<double>(hi.density), room}), 5));
        if (v.density < hi.density && wantDensity < room)
            return v;
        const double focusShare = v.density / 100.0 * v.width / 100;
        const double roomP = width_room(l, focusShare, cw / 100) * 100;
        const double wantPeriphery =
            100 * std::sqrt(std::max(0.0, pixels - focus_pixels(v.density, v.width, v.height)) / (2.0 * cw / 100 * ch / 100));
        v.periphery = std::max(start.periphery, down(std::min({wantPeriphery, static_cast<double>(hi.periphery), roomP}), 5));
        return v;
    }
    // Lowering: the periphery first (least noticed), then the focus density, then the box.
    const double focusNow = focus_pixels(v.density, v.width, v.height);
    const double wantPeriphery = 100 * std::sqrt(std::max(0.0, pixels - focusNow) / (2.0 * cw / 100 * ch / 100));
    v.periphery = std::min(start.periphery, std::max(lo.periphery, down(wantPeriphery, 5)));
    if (wantPeriphery >= lo.periphery)
        return v;
    const double outer = outer_pixels(v.periphery, cw, ch);
    const double wantDensity = 100 * std::sqrt(std::max(0.0, pixels - outer) / (2.0 * v.width / 100 * v.height / 100));
    v.density = std::min(start.density, std::max(lo.density, down(wantDensity, 5)));
    if (wantDensity >= lo.density)
        return v;
    // The box: width and height by the same factor.
    const double focus = std::max(0.0, pixels - outer);
    const double scale = std::sqrt(focus / focus_pixels(v.density, v.width, v.height));
    v.width = std::min(start.width, std::max(lo.width, down(v.width * scale, 1)));
    v.height = std::min(start.height, std::max(lo.height, down(v.height * scale, 1)));
    return v;
}

void QvfrOptimizer::start(const QvfrValues& current, const QvfrLimits& limits, double targetShare) {
    limits_ = limits;
    start_ = current_ = current;
    targetShare_ = std::clamp(targetShare, 0.5, 0.98);
    started_ = true;
    steps_ = 0;
    havePrevious_ = false;
    state_ = QvfrState::Measuring;
    windows_ = 0;
    gpuSum_ = cpuSum_ = budget_ = 0;
    gpuCount_ = cpuCount_ = cpuBoundCount_ = 0;
    status_ = L"Measuring… keep driving";
}

QvfrAction QvfrOptimizer::undo() {
    if (!started_)
        return {};
    state_ = QvfrState::Idle;
    status_ = L"Restored";
    const bool changed = !(current_ == start_);
    current_ = start_;
    return {changed, start_};
}

void QvfrOptimizer::cancel() {
    if (!running())
        return;
    state_ = QvfrState::Idle;
    status_ = L"Stopped";
}

void QvfrOptimizer::finish(QvfrState state, std::wstring status) {
    state_ = state;
    status_ = std::move(status);
}

QvfrAction QvfrOptimizer::feed(const QvfrWindow& w) {
    if (!running())
        return {};
    ++windows_;
    if (state_ == QvfrState::Settling) {
        if (windows_ >= kSettleWindows) {
            state_ = QvfrState::Measuring;
            windows_ = 0;
            gpuSum_ = cpuSum_ = 0;
            gpuCount_ = cpuCount_ = cpuBoundCount_ = 0;
        }
        return {};
    }
    if (w.frames > 0 && w.gpuMs >= 0) {
        gpuSum_ += w.gpuMs;
        ++gpuCount_;
        cpuBoundCount_ += w.cpuBound;
        if (w.cpuMs >= 0) {
            cpuSum_ += w.cpuMs;
            ++cpuCount_;
        }
    }
    if (w.budgetMs > 0)
        budget_ = w.budgetMs;
    if (windows_ < kMeasureWindows)
        return {};
    if (gpuCount_ == 0 || budget_ <= 0) {
        finish(QvfrState::Failed, L"No GPU frame timing: is ACE rendering?");
        return {};
    }
    const double gpu = gpuSum_ / gpuCount_;
    if (cpuBoundCount_ * 2 > gpuCount_) {
        const std::wstring cpu = cpuCount_ ? L"CPU " + ms(cpuSum_ / cpuCount_) + L" ms, " : L"";
        if (steps_ == 0)
            finish(QvfrState::Failed, L"CPU limited (" + cpu + L"GPU " + ms(gpu) + L" ms): resolution won't help");
        else
            finish(QvfrState::Done, L"GPU " + ms(gpu) + L" ms; the CPU limits now");
        return {};
    }
    return step(gpu, budget_);
}

QvfrAction QvfrOptimizer::step(double gpu, double budget) {
    const double target = targetShare_ * budget, low = target - kBandShare * budget;
    const double pixels = qvfr_pixels(current_, limits_.cropWidth, limits_.cropHeight);
    if (steps_ > 0) {
        previous_ = last_;
        havePrevious_ = true;
    }
    last_ = {pixels, gpu};
    const std::wstring measured = L"GPU " + ms(gpu) + L" of " + ms(budget) + L" ms";
    if (gpu <= target && gpu >= low) {
        finish(QvfrState::Done, steps_ == 0 ? measured + L": already on target" : measured + L": on target");
        return {};
    }
    if (steps_ >= kMaxSteps) {
        finish(QvfrState::Done, measured + L": best found");
        return {};
    }
    // The model: the line through the last two measurements when it is sensible (more pixels cost more,
    // and the fixed part is between none and all of it), else the first step's guess.
    double perPixel = 0, fixed = 0;
    if (havePrevious_ && std::abs(pixels - previous_.pixels) > 1e-3) {
        perPixel = (gpu - previous_.gpuMs) / (pixels - previous_.pixels);
        fixed = gpu - perPixel * pixels;
    }
    if (perPixel <= 0 || fixed < 0 || fixed >= gpu) {
        fixed = kFixedShare * gpu;
        perPixel = (gpu - fixed) / pixels;
    }
    // Aim a little under the target, inside the band.
    const double aim = target - 0.4 * kBandShare * budget;
    const QvfrValues next = qvfr_values_for(std::max(0.0, (aim - fixed) / perPixel), start_, limits_);
    if (next == current_) {
        finish(QvfrState::Done, gpu > target ? measured + L": at the floors, QVFR alone can't reach it"
                                             : measured + L": at the most this launch allows");
        return {};
    }
    current_ = next;
    ++steps_;
    state_ = QvfrState::Settling;
    windows_ = 0;
    status_ = L"Step " + std::to_wstring(steps_) + L": " + measured + L", trying periphery " +
              std::to_wstring(next.periphery) + L"%, density " + std::to_wstring(next.density) + L"%";
    return {true, next};
}

}  // namespace simqvfr

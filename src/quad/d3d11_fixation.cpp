#include "d3d11_fixation.h"

#include <algorithm>
#include <iterator>

#include "../log.h"

namespace simqvfr {

bool D3D11Fixation::start(ID3D11Device* device, const char* tag) {
    std::lock_guard guard(mutex_);
    tag_ = tag;
    started_ = probe_.initialize(device);
    filter_ = {};
    fixation_.store(0);
    queuedCount_ = 0;
    stats_ = {};
    lastRead_ = {};
    logf("%s: %s", tag_,
         started_ ? "on (the scene depth at the left eye's box centre converges the focus boxes)"
                  : "the depth probe is unavailable: the focus boxes stay parallel");
    return started_;
}

void D3D11Fixation::stop() {
    std::lock_guard guard(mutex_);
    started_ = false;
    fixation_.store(0);
    probe_.shutdown();
}

bool D3D11Fixation::request(ID3D11Texture2D* depth, const int (&rect)[4], int px, int py, int step, double tx, double ty,
                            double a, double b) {
    std::lock_guard guard(mutex_);
    if (!started_ || !depth || rect[2] <= 0 || rect[3] <= 0)
        return false;
    int points[D3D11DepthProbe::kPoints][2];
    for (int i = 0; i < D3D11DepthProbe::kPoints; ++i) {
        points[i][0] = std::clamp(px + (i % 3 - 1) * step, rect[0], rect[0] + rect[2] - 1);
        points[i][1] = std::clamp(py + (i / 3 - 1) * step, rect[1], rect[1] + rect[3] - 1);
    }
    if (!probe_.request(depth, points)) {
        ++stats_.refused;
        return false;
    }
    Queued& q = queued_[queuedCount_ % std::size(queued_)];
    q.tan[0] = tx;
    q.tan[1] = ty;
    q.a = a;
    q.b = b;
    ++queuedCount_;
    ++stats_.requests;
    stats_.pixel[0] = px;
    stats_.pixel[1] = py;
    return true;
}

void D3D11Fixation::frame_end(std::uint64_t logEvery) {
    std::lock_guard guard(mutex_);
    if (!started_)
        return;
    ++stats_.frames;
    float values[D3D11DepthProbe::kPoints];
    int age = 0;
    if (probe_.latest(values, age) && queuedCount_ > static_cast<std::uint64_t>(age)) {
        const Queued& q = queued_[(queuedCount_ - 1 - static_cast<std::uint64_t>(age)) % std::size(queued_)];
        int sky = 0;
        const double median = fixation_from_depths(values, D3D11DepthProbe::kPoints, q.tan[0], q.tan[1], q.a, q.b, sky);
        const auto now = std::chrono::steady_clock::now();
        const double dt =
            lastRead_.time_since_epoch().count() ? std::chrono::duration<double>(now - lastRead_).count() : 0.011;
        lastRead_ = now;
        const double published = filter_.update(median, std::min(dt, 0.1));
        fixation_.store(published, std::memory_order_relaxed);
        ++stats_.reads;
        stats_.sky += sky > D3D11DepthProbe::kPoints / 2 ? 1 : 0;
        stats_.minM = std::min(stats_.minM, published);
        stats_.maxM = std::max(stats_.maxM, published);
    }
    if (logEvery && stats_.frames % logEvery == 0) {
        logf("%s: %llu reads of %llu queued (%llu refused), %llu at the sky over the last %llu frames; distance now "
             "%.2f m (%.2f-%.2f); the left eye's box centre at texel %d,%d",
             tag_, static_cast<unsigned long long>(stats_.reads), static_cast<unsigned long long>(stats_.requests),
             static_cast<unsigned long long>(stats_.refused), static_cast<unsigned long long>(stats_.sky),
             static_cast<unsigned long long>(logEvery), fixation_.load(), stats_.minM < 1e9 ? stats_.minM : 0.0,
             stats_.maxM, stats_.pixel[0], stats_.pixel[1]);
        const std::uint64_t frames = stats_.frames;
        stats_ = {};
        stats_.frames = frames;
    }
}

}  // namespace simqvfr

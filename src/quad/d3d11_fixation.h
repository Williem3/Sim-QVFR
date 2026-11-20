// Where the eyes look, how far, on D3D11: a game's scene depth read at a few texels round one eye's box centre (without
// stalling: d3d11_depth_probe.h), a frame or two later turned into metres along that ray (focus_converge.h) and smoothed
// (FixationFilter). The layer aims both eyes' focus boxes at that point (GameAdapter::qvfr_fixation_m), so near things
// (the dash, a mirror) sit in both eyes' boxes alike. Each game says where its scene depth is, which texel the box
// centre is and how its depth turns into distance; this does the rest. Calls on the thread that renders with the
// device's immediate context (the probe executes on it), metres() from any.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "d3d11_depth_probe.h"
#include "focus_converge.h"

namespace simqvfr {

class D3D11Fixation {
public:
    // `tag` the log's prefix ("lmu fixation"). False: no probe (the boxes stay parallel; logged).
    bool start(ID3D11Device* device, const char* tag);
    void stop();
    // Queues a read of `depth` round texel (px, py), a 3x3 grid `step` texels apart kept inside `rect` (x, y, width,
    // height); the ray's tangents in the eye (tx, ty) and the depth terms (depth = a + b / z, metres) for when it's back.
    bool request(ID3D11Texture2D* depth, const int (&rect)[4], int px, int py, int step, double tx, double ty, double a,
                 double b);
    // A read that's back taken into the distance; every `logEvery` frames (0: never) a log line. Once per frame.
    void frame_end(std::uint64_t logEvery = 900);
    // The smoothed distance in metres; 0 before the first read. Any thread.
    double metres() const { return fixation_.load(std::memory_order_relaxed); }
    bool started() const { return started_; }

private:
    struct Queued {
        double tan[2] = {};
        double a = 0, b = 0;
    };
    std::mutex mutex_;
    D3D11DepthProbe probe_;
    const char* tag_ = "fixation";
    bool started_ = false;
    std::atomic<double> fixation_{0};
    FixationFilter filter_;
    std::chrono::steady_clock::time_point lastRead_{};
    Queued queued_[8];
    std::uint64_t queuedCount_ = 0;
    struct Stats {
        std::uint64_t reads = 0, requests = 0, refused = 0, sky = 0, frames = 0;
        double minM = 1e9, maxM = 0;
        int pixel[2] = {};
    } stats_;
};

}  // namespace simqvfr

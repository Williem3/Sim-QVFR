#include "fixation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../log.h"
#include "../../quad/d3d11_depth_probe.h"
#include "../../quad/focus_converge.h"
#include "views.h"
#include "views_map.h"

namespace acc {
namespace {

using Microsoft::WRL::ComPtr;

std::mutex mutex;  // the render thread's (hooks, frame end) against fixation_stop
simqvfr::D3D11DepthProbe probe;
ComPtr<ID3D11Texture2D> sceneDepth;  // the base pass's depth target, the latest seen
bool started = false;
std::atomic<double> fixation{0};
simqvfr::FixationFilter filter;
std::chrono::steady_clock::time_point lastRead{};
// Each queued read's tangents (the left eye's gaze then): a read comes back a frame or two later.
struct Queued {
    double tan[2] = {};
    double nearCm = 0;
};
Queued queued[8];
std::uint64_t queuedCount = 0;
// For the report.
struct Stats {
    std::uint64_t reads = 0, misses = 0, sky = 0, frames = 0;
    double minM = 1e9, maxM = 0;
    int pixel[2] = {};
    unsigned depthW = 0, depthH = 0;
    int depthFormat = 0;
};
Stats stats;

bool is_depth_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return true;
    default:
        return false;
    }
}

}  // namespace

void fixation_start(ID3D11Device* device) {
    std::lock_guard guard(mutex);
    started = probe.initialize(device);
    sceneDepth.Reset();
    filter = {};
    fixation.store(0);
    simqvfr::log_line(started ? "acc fixation: on (the scene depth at the left eye's gaze point converges the focus boxes)"
                              : "acc fixation: the depth probe is unavailable: the focus boxes stay parallel");
}

void fixation_stop() {
    std::lock_guard guard(mutex);
    started = false;
    sceneDepth.Reset();
    probe.shutdown();
}

void fixation_targets(unsigned colourTargets, ID3D11DepthStencilView* dsv) {
    if (!started || colourTargets < 3 || !dsv)
        return;  // the base pass draws into the G-buffer: three or more colour targets and the scene depth
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (!r)
        return;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        auto* t = static_cast<ID3D11Texture2D*>(r);
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        if (is_depth_format(d.Format) && t != sceneDepth.Get()) {
            std::lock_guard guard(mutex);
            sceneDepth = t;
            stats.depthW = d.Width;
            stats.depthH = d.Height;
            stats.depthFormat = static_cast<int>(d.Format);
        }
    }
    r->Release();
}

void fixation_frame_end() {
    std::lock_guard guard(mutex);
    if (!started)
        return;
    ++stats.frames;
    // A finished read: the median distance of its texels, along the ray it was taken at.
    float values[simqvfr::D3D11DepthProbe::kPoints];
    int age = 0;
    if (probe.latest(values, age) && queuedCount > static_cast<std::uint64_t>(age)) {
        const Queued& q = queued[(queuedCount - 1 - static_cast<std::uint64_t>(age)) % std::size(queued)];
        double metres[simqvfr::D3D11DepthProbe::kPoints];
        int n = 0, skyCount = 0;
        for (float v : values) {
            if (!(v > 1e-7f)) {  // reversed-Z: 0 is the far plane at infinity (the sky)
                ++skyCount;
                metres[n++] = 1e4;
                continue;
            }
            const double viewZ = q.nearCm / v / 100.0;
            metres[n++] = simqvfr::ray_distance(viewZ, q.tan[0], q.tan[1]);
        }
        std::sort(metres, metres + n);
        const double median = std::clamp(metres[n / 2], 0.2, 1e4);
        const auto now = std::chrono::steady_clock::now();
        const double dt = lastRead.time_since_epoch().count() ? std::chrono::duration<double>(now - lastRead).count() : 0.011;
        lastRead = now;
        const double published = filter.update(median, std::min(dt, 0.1));
        fixation.store(published, std::memory_order_relaxed);
        ++stats.reads;
        stats.sky += skyCount > simqvfr::D3D11DepthProbe::kPoints / 2 ? 1 : 0;
        stats.minM = std::min(stats.minM, published);
        stats.maxM = std::max(stats.maxM, published);
    }
    // This frame's read: the left eye's gaze point in its view (render resolution), a 3x3 grid round it.
    int rect[4];
    float m[16];
    float gaze[2];
    if (sceneDepth && views_view_rect(0, rect) && views_eye_projection(0, m) && views_eye_gaze(0, gaze) && m[0] > 0 &&
        m[5] > 0) {
        const double xn = m[0] * gaze[0] + m[8], yn = m[5] * gaze[1] + m[9];  // UE's NDC (y up)
        const int px = rect[0] + static_cast<int>((xn + 1) / 2 * rect[2]);
        const int py = rect[1] + static_cast<int>((1 - yn) / 2 * rect[3]);
        const int step = std::max(2, rect[2] / 150);
        int points[simqvfr::D3D11DepthProbe::kPoints][2];
        for (int i = 0; i < simqvfr::D3D11DepthProbe::kPoints; ++i) {
            points[i][0] = std::clamp(px + (i % 3 - 1) * step, rect[0], rect[0] + rect[2] - 1);
            points[i][1] = std::clamp(py + (i / 3 - 1) * step, rect[1], rect[1] + rect[3] - 1);
        }
        if (probe.request(sceneDepth.Get(), points)) {
            Queued& q = queued[queuedCount % std::size(queued)];
            q.tan[0] = gaze[0];
            q.tan[1] = gaze[1];
            q.nearCm = m[14];
            ++queuedCount;
            stats.pixel[0] = px;
            stats.pixel[1] = py;
        } else {
            ++stats.misses;
        }
    } else {
        ++stats.misses;
    }
    if (stats.frames % 900 == 0) {
        simqvfr::logf("acc fixation: %llu reads, %llu missed, %llu at the sky over the last 900 frames; distance now %.2f m "
                      "(%.2f-%.2f); depth target %ux%u format %d; the left eye's gaze at pixel %d,%d",
                      static_cast<unsigned long long>(stats.reads), static_cast<unsigned long long>(stats.misses),
                      static_cast<unsigned long long>(stats.sky), fixation.load(), stats.minM < 1e9 ? stats.minM : 0.0,
                      stats.maxM, stats.depthW, stats.depthH, stats.depthFormat, stats.pixel[0], stats.pixel[1]);
        stats.reads = stats.misses = stats.sky = 0;
        stats.minM = 1e9;
        stats.maxM = 0;
    }
}

double fixation_m() { return fixation.load(std::memory_order_relaxed); }

}  // namespace acc

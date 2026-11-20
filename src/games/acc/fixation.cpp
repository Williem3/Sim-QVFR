#include "fixation.h"

#include <algorithm>
#include <cstdint>
#include <mutex>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../log.h"
#include "../../quad/d3d11_fixation.h"
#include "views.h"
#include "views_map.h"

namespace acc {
namespace {

using Microsoft::WRL::ComPtr;

std::mutex mutex;  // the render thread's (hooks, frame end) against fixation_stop
simqvfr::D3D11Fixation tracker;
ComPtr<ID3D11Texture2D> sceneDepth;  // the base pass's depth target, the latest seen
bool started = false;
struct DepthTarget {
    unsigned w = 0, h = 0;
    int format = 0;
} target;
std::uint64_t frames = 0;

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
    started = tracker.start(device, "acc fixation");
    sceneDepth.Reset();
}

void fixation_stop() {
    std::lock_guard guard(mutex);
    started = false;
    sceneDepth.Reset();
    tracker.stop();
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
            target = {d.Width, d.Height, static_cast<int>(d.Format)};
        }
    }
    r->Release();
}

void fixation_frame_end() {
    std::lock_guard guard(mutex);
    if (!started)
        return;
    tracker.frame_end();  // a finished read into the distance (logged every 900 frames)
    // This frame's read: the left eye's gaze point in its view (render resolution), a 3x3 grid round it. UE's reversed
    // infinite depth: view z = near / depth, the near plane m[14] in centimetres.
    int rect[4];
    float m[16];
    float gaze[2];
    if (sceneDepth && views_view_rect(0, rect) && views_eye_projection(0, m) && views_eye_gaze(0, gaze) && m[0] > 0 &&
        m[5] > 0) {
        const double xn = m[0] * gaze[0] + m[8], yn = m[5] * gaze[1] + m[9];  // UE's NDC (y up)
        const int px = rect[0] + static_cast<int>((xn + 1) / 2 * rect[2]);
        const int py = rect[1] + static_cast<int>((1 - yn) / 2 * rect[3]);
        tracker.request(sceneDepth.Get(), rect, px, py, std::max(2, rect[2] / 150), gaze[0], gaze[1], 0.0, m[14] / 100.0);
    }
    if (++frames % 900 == 0)
        simqvfr::logf("acc fixation: depth target %ux%u format %d", target.w, target.h, target.format);
}

double fixation_m() { return tracker.metres(); }

}  // namespace acc

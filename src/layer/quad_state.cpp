// The quad pipeline's shared pieces: the session's graphics, the helmet set, the views4 outline, failing or
// readying the pipeline, and turning a view toward the focus.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <dxgi1_4.h>

#include "games/adapter.h"
#include "log.h"
#include "quad/compositor.h"
#include "quad/focus_outline.h"
#include "quad/focus_turn.h"
#include "quad/session_graphics.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

SessionGraphics session_graphics_now() {
    std::lock_guard guard(quad.mutex);
    return sessionGraphics;
}

// The compositor that holds the helmet's pieces and light: the D3D11 one for a D3D11 game (ACC; under
// v4d3d11.mutex), else ACE's D3D12 one (under quad.mutex).
HelmetPieceSet& helmet_set() {
    if (experiment.views4ComposeD3D11)
        return v4d3d11.compositor;
    return quad.compositor;
}

// Without composition the outline goes on ACE's outer views; with it, on the composed output.
bool views4_outline_on() {
    return experiment.views4 && experiment.borderAtLaunch && !experiment.views4ShowCopies && !experiment.composing();
}

void views4_outline_draw(XrSwapchain swapchain) {
    std::lock_guard guard(v4outline.mutex);
    if (swapchain != v4outline.chain || !v4outline.fovValid || v4outline.failed || !experiment.border)
        return;
    if (!v4outline.ready) {
        if (!quad.device || !quad.queue)
            return;
        const auto format = v4outline.formats.find(swapchain);
        if (format == v4outline.formats.end() ||
            !v4outline.outline.initialize(quad.device, quad.queue, static_cast<DXGI_FORMAT>(format->second))) {
            v4outline.failed = true;
            log_line("views4: focus outline unavailable");
            return;
        }
        v4outline.ready = true;
        std::memcpy(v4outline.rgba, experiment.borderColor, sizeof(v4outline.rgba));
        logf("views4: focus outline on, colour %.2f/%.2f/%.2f", v4outline.rgba[0], v4outline.rgba[1], v4outline.rgba[2]);
    }
    const auto images = v4outline.images.find(swapchain);
    const auto index = v4outline.acquired.find(swapchain);
    if (images == v4outline.images.end() || index == v4outline.acquired.end() || index->second >= images->second.size())
        return;
    ID3D12Resource* image = images->second[index->second];
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    // Eye e's outer view is view e of the image (views4_submit); the focus box is focus_percent of
    // the eye's tangent extent around its forward axis, as views4's focus cameras use.
    std::vector<OutlineBox> boxes;
    int thickness = 2;
    Views4Frame frame{};
    const bool recorded = game->qvfr_latest_frame(frame);  // ACE's rendered FOV, else the located one
    const Views4Layout layout = views4_with_live(experiment.views4Layout, frame.live, frame.width, frame.height);
    for (int e = 0; e < 2; ++e) {
        const ViewRect v = views4_rect(layout, e, static_cast<int>(desc.Width), static_cast<int>(desc.Height));
        float tan[4];
        if (recorded)
            std::memcpy(tan, frame.outerTan[e], sizeof(tan));
        else
            fov_tangents(v4outline.fov[e], tan);
        const double l = tan[0], r = tan[1], u = tan[2], d = tan[3];
        double hw, hh;
        focus_extents(tan, experiment.box_x(), experiment.box_y(), hw, hh);
        if (!(r > l && u > d))
            return;
        boxes.push_back(OutlineBox{v.left + static_cast<int>((-hw - l) / (r - l) * v.width),
                                   static_cast<int>((u - hh) / (u - d) * v.height),
                                   v.left + static_cast<int>((hw - l) / (r - l) * v.width),
                                   static_cast<int>((u + hh) / (u - d) * v.height)});
        thickness = std::max(2, v.width / 300);
    }
    std::memcpy(v4outline.rgba, experiment.borderColor, sizeof(v4outline.rgba));  // live (debug_border_color)
    v4outline.outline.draw(image, boxes, thickness, v4outline.rgba);
}

void quad_fail(const char* why) {
    ++quad.fallbacks;
    ++quad.fallbackRun;
    if (!quad.failureLogged)
        logf("quad: showing ACE's frame B unchanged: %s", why);
    quad.failureLogged = true;
}

// two_loops arms only once ACE's eye swapchain is adopted: unarmed, ACE renders its normal full view.
bool quad_ready() {
    std::lock_guard guard(quad.mutex);
    return quad.outChain != XR_NULL_HANDLE;
}

// Quad frame B: instead of an off-centre window of the eye's view (a strongly asymmetric projection,
// which ACE's screen-space clouds and other effects get wrong: dark skies when the box is far from
// the centre), the eye camera is turned to face tangent (cx, cy) of the eye's view, without roll,
// and given a centred projection of the same tangent size.
void turn_view(XrView& view, double fractionX, double fractionY, double tanX, double tanY) {
    const double l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
    const double d = std::tan(view.fov.angleDown), u = std::tan(view.fov.angleUp);
    const double hw = (r - l) * fractionX / 2, hh = (u - d) * fractionY / 2;
    view.pose.orientation = qmul(view.pose.orientation, focus_turn(view.fov, fractionX, fractionY, tanX, tanY));
    view.fov = {static_cast<float>(-std::atan(hw)), static_cast<float>(std::atan(hw)),
                static_cast<float>(std::atan(hh)), static_cast<float>(-std::atan(hh))};
}

// Where frame B's camera points, as tangents of frame A's view (for logs).
void focus_centre(const EyeComposition& eye, float& tanX, float& tanY) {
    // B's forward (0,0,-1) in A's frame is the transpose applied: -(row 2) = (-m6, -m7, -m8).
    tanX = -eye.rotation[6] / eye.rotation[8];
    tanY = -eye.rotation[7] / eye.rotation[8];
}

bool sampled(uint64_t count) { return count <= kDetailedFrames || count % kSampleEveryFrames == 0; }

}  // namespace simqvfr::layer

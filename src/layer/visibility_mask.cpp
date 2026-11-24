// The visibility mask hook (xrGetVisibilityMaskKHR): the runtime's mask passed on and logged, or for views4 an empty
// one (views4_mask=0) or the test masks.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "log.h"
#include "quad/focus_offset.h"
#include "quad/focus_turn.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

// views4 composing: the game draws each eye's hidden-area mesh into every view, so the focus views got the
// lens mask's black corners in their own frame (ACE: composition run 2; ACC through OpenComposite: a dark rim
// round the focus area, 2026-11-04). views4_mask=0 answers the hidden
// mesh with one zero-area triangle: nothing is masked in any view (the lenses hide those pixels anyway).
// views4_test_visible_mask=1 answers the visible mesh with the whole view, as runtimes that offer one (VDXR) do, to
// reproduce their path on one that doesn't (SteamVR with the Pimax).
// views4_test_hidden_mask=1 answers the hidden mesh with one of ours, so a game's lens mask can be seen where the runtime
// gives none (SteamVR on the user's headset): each corner of the view's located FOV cut off by a triangle a quarter of
// each side long, and a small triangle straight ahead (inside the focus box when the eyes look at it).
namespace {
std::atomic<bool> runtimeMaskLogged[3];  // per XrVisibilityMaskTypeKHR (1-3), the runtime's answer logged once
XrResult test_visible_mask(XrVisibilityMaskKHR* mask) {
    mask->vertexCountOutput = 4;
    mask->indexCountOutput = 6;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < 4 || mask->indexCapacityInput < 6 || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    // Tangents well past any FOV, counter-clockwise: the whole view is visible.
    const XrVector2f corners[4] = {{-4.0f, -4.0f}, {4.0f, -4.0f}, {4.0f, 4.0f}, {-4.0f, 4.0f}};
    const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    std::memcpy(mask->vertices, corners, sizeof(corners));
    std::memcpy(mask->indices, indices, sizeof(indices));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        log_line("views4: visible-area mesh replaced by the whole view (views4_test_visible_mask=1)");
    return XR_SUCCESS;
}

XrResult test_hidden_mask(uint32_t viewIndex, XrVisibilityMaskKHR* mask) {
    constexpr uint32_t kCount = 15;  // five triangles, each its own three vertices
    mask->vertexCountOutput = kCount;
    mask->indexCountOutput = kCount;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < kCount || mask->indexCapacityInput < kCount || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    // The view's FOV as the game located it (cropped, if a crop is set); tangents.
    float l = -1, r = 1, d = -1, u = 1;
    {
        std::lock_guard guard(quad.mutex);
        if (viewIndex < 2) {
            const XrFovf& fov = quad.v4Fov[viewIndex];
            if (fov.angleRight > fov.angleLeft && fov.angleUp > fov.angleDown) {
                l = std::tan(fov.angleLeft);
                r = std::tan(fov.angleRight);
                d = std::tan(fov.angleDown);
                u = std::tan(fov.angleUp);
            }
        }
    }
    const float a = 0.25f * (r - l), b = 0.25f * (u - d), s = 0.04f;
    // Counter-clockwise (x right, y up).
    const XrVector2f v[kCount] = {{l, d},  {l + a, d}, {l, d + b},    // bottom left
                                  {r, d},  {r, d + b}, {r - a, d},    // bottom right
                                  {r, u},  {r - a, u}, {r, u - b},    // top right
                                  {l, u},  {l, u - b}, {l + a, u},    // top left
                                  {-s, -s}, {s, -s},   {0.0f, s}};    // straight ahead
    std::memcpy(mask->vertices, v, sizeof(v));
    for (uint32_t i = 0; i < kCount; ++i)
        mask->indices[i] = i;
    logf("views4: hidden-area mesh of view %u replaced by a test one (views4_test_hidden_mask=1): the corners of its FOV "
         "(tangents %.3f/%.3f/%.3f/%.3f) cut a quarter of each side, a triangle %.2f wide straight ahead",
         viewIndex, l, r, d, u, 2 * s);
    return XR_SUCCESS;
}

// views4_test_hidden_mask=2: what a trapezoid focus shape wastes, as a hidden area: the fixed focus box (gaze off, no
// convergence: on the eye's forward axis, moved by the fixed offset, kept inside the eye) with its two top corners cut,
// each a triangle from the top corner views4TestMaskCut percent of the box's width in down to the bottom corner on its
// side. Together `cut` percent of the box. Drawn by the game with two views, the same angles in the eye (behind the box)
// and in its focus view.
XrResult test_focus_corner_mask(uint32_t viewIndex, XrVisibilityMaskKHR* mask) {
    constexpr uint32_t kCount = 6;
    mask->vertexCountOutput = kCount;
    mask->indexCountOutput = kCount;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < kCount || mask->indexCapacityInput < kCount || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    float tan[4] = {-1, 1, 1, -1};  // left, right, up, down (fov_tangents' order)
    {
        std::lock_guard guard(quad.mutex);
        if (viewIndex < 2) {
            const XrFovf& fov = quad.v4Fov[viewIndex];
            if (fov.angleRight > fov.angleLeft && fov.angleUp > fov.angleDown)
                fov_tangents(fov, tan);
        }
    }
    double hw, hh;
    focus_extents(tan, experiment.box_x(), experiment.box_y(), hw, hh);
    const auto keep_in = [](double t, double lo, double hi, double half) {
        return lo + half >= hi - half ? (lo + hi) / 2 : std::clamp(t, lo + half, hi - half);
    };
    const double cx = keep_in(experiment.fixedOffsetX ? offset_tan_x(0.0, experiment.fixedOffsetX) : 0.0, tan[0], tan[1], hw);
    const double cy = keep_in(experiment.fixedOffsetY ? offset_tan_y(0.0, experiment.fixedOffsetY) : 0.0, tan[3], tan[2], hh);
    const float l = static_cast<float>(cx - hw), r = static_cast<float>(cx + hw);
    const float d = static_cast<float>(cy - hh), u = static_cast<float>(cy + hh);
    const float a = static_cast<float>(2 * hw * experiment.views4TestMaskCut / 100.0);
    // Counter-clockwise (x right, y up).
    const XrVector2f v[kCount] = {{l, d}, {l + a, u}, {l, u},    // top left
                                  {r, d}, {r, u}, {r - a, u}};   // top right
    std::memcpy(mask->vertices, v, sizeof(v));
    for (uint32_t i = 0; i < kCount; ++i)
        mask->indices[i] = i;
    logf("views4: hidden-area mesh of view %u replaced by the focus box's cut corners (views4_test_hidden_mask=2): box "
         "tangents %.3f/%.3f/%.3f/%.3f in the eye's %.3f/%.3f/%.3f/%.3f, top corners %d%% of its width in: %d%% of the box "
         "hidden",
         viewIndex, l, r, d, u, tan[0], tan[1], tan[3], tan[2], experiment.views4TestMaskCut,
         experiment.views4TestMaskCut);
    return XR_SUCCESS;
}
}  // namespace
XrResult XRAPI_CALL layerGetVisibilityMaskKHR(XrSession session, XrViewConfigurationType type, uint32_t viewIndex,
                                              XrVisibilityMaskTypeKHR maskType, XrVisibilityMaskKHR* mask) {
    if (experiment.views4 && maskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR && mask &&
        mask->vertexCapacityInput > 0)
        game->visibility_mask_taken(viewIndex);  // the mesh itself, not the size query
    if (experiment.views4TestHiddenMask && maskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR && mask)
        return experiment.views4TestHiddenMask == 2 ? test_focus_corner_mask(viewIndex, mask)
                                                    : test_hidden_mask(viewIndex, mask);
    if (experiment.views4TestVisibleMask && maskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR && mask)
        return test_visible_mask(mask);
    if (!experiment.views4NoMask || maskType != XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR || !mask) {
        const XrResult r = next.GetVisibilityMaskKHR(session, type, viewIndex, maskType, mask);
        // What the runtime offers, once per type (the size query already has the counts; a game builds its mesh from it).
        const int t = static_cast<int>(maskType) - 1;
        if (mask && t >= 0 && t < 3 && !runtimeMaskLogged[t].exchange(true))
            logf("visibility mask from the runtime: %s, view %u: %u vertices, %u indices, result=%d",
                 maskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR    ? "hidden triangles"
                 : maskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR ? "visible triangles"
                                                                                 : "line loop",
                 viewIndex, mask->vertexCountOutput, mask->indexCountOutput, r);
        return r;
    }
    mask->vertexCountOutput = 3;
    mask->indexCountOutput = 3;
    if (mask->vertexCapacityInput == 0 && mask->indexCapacityInput == 0)
        return XR_SUCCESS;  // the size query
    if (mask->vertexCapacityInput < 3 || mask->indexCapacityInput < 3 || !mask->vertices || !mask->indices)
        return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < 3; ++i) {
        mask->vertices[i] = {0.0f, 0.0f};
        mask->indices[i] = i;
    }
    if (!visibilityMaskLogged) {
        visibilityMaskLogged = true;
        log_line("views4: hidden-area mesh replaced by an empty one (views4_mask=0)");
    }
    return XR_SUCCESS;
}

}  // namespace simqvfr::layer

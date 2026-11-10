// Native quad views ([experiment] native_quad_views=1; layer/native_quad.cpp): on a runtime that composes quad views
// itself (XR_VARJO_quad_views: Varjo's and Pimax's OpenXR runtimes), the session runs PRIMARY_QUAD_VARJO and every
// projection layer has four views: the eyes (0-1) and the focus views (2-3). The runtime samples the focus views
// straight onto the panel at their own resolution, which the layer's own composition into a native-size stereo image
// can't (2026-11-09: focus density above 100% only antialiased).
//
// The game keeps seeing a stereo session; the layer translates at its bottom edge. This header holds the pure part:
// a frame's layers made fit for a quad view session, and the geometry that makes views 2-3 cover exactly the
// runtime's own focus views.
//
// Views 2-3 always go to the runtime with the FOV it located for them: a runtime may draw them into its own focus
// region whatever FOV it is given (2026-11-10, a Pimax tester: menu frames' eye copies, the whole eye's FOV, showed
// shrunk into its focus region, an image in the image). So the copies are cut to the runtime's focus views
// (crop_to_region), and the race frames' focus image covers them too, the focus box inside (focus_region_image).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../xr.h"
#include "focus_converge.h"

namespace simqvfr {

inline constexpr uint32_t kQuadViews = 4;

// The runtime's focus view for one eye, in that eye's frame (x right, y up, looking down -z), as tangents: left, right,
// up, down. A focus view turned against its eye: the bounding box of its corners as the eye sees them (`turned`), the
// best an eye-frame region can do (Pimax and Varjo keep them unturned).
struct FocusRegion {
    float tan[4] = {};
    bool turned = false;
};

// `focus` (the runtime's view 2 or 3) as seen from its eye `eye` (view 0 or 1). False: a corner behind the eye.
inline bool focus_region(const XrQuaternionf& eye, const XrPosef& focus, const XrFovf& fov, FocusRegion& out) {
    const XrQuaternionf rel = qmul(qconj(eye), focus.orientation);
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), u = std::tan(fov.angleUp),
                d = std::tan(fov.angleDown);
    // Turned by more than ~0.01 degrees: the corners' bounding box.
    out.turned = std::fabs(rel.x) > 1e-4f || std::fabs(rel.y) > 1e-4f || std::fabs(rel.z) > 1e-4f;
    if (!out.turned) {
        out.tan[0] = l;
        out.tan[1] = r;
        out.tan[2] = u;
        out.tan[3] = d;
        return r > l && u > d;
    }
    float box[4] = {1e9f, -1e9f, -1e9f, 1e9f};
    const float xs[2] = {l, r}, ys[2] = {u, d};
    for (float x : xs)
        for (float y : ys) {
            const XrVector3f a = rotate_by(rel, XrVector3f{x, y, -1});
            if (!(a.z < -1e-4f))
                return false;
            const float tx = a.x / -a.z, ty = a.y / -a.z;
            box[0] = std::min(box[0], tx);
            box[1] = std::max(box[1], tx);
            box[2] = std::max(box[2], ty);
            box[3] = std::min(box[3], ty);
        }
    for (int i = 0; i < 4; ++i)
        out.tan[i] = box[i];
    return box[1] > box[0] && box[2] > box[3];
}

inline XrFovf tangents_to_fov(const float (&t)[4]) {
    return {std::atan(t[0]), std::atan(t[1]), std::atan(t[2]), std::atan(t[3])};
}

// Eye view `eye` cut to `region` (tangents in its frame): the sub-rect of its image covering the part of the region
// inside its FOV, in whole texels (out to the next texel edge), and that rect's own FOV; pose and swapchain the eye's.
// False: no overlap of at least a texel each way (the caller keeps the plain copy).
inline bool crop_to_region(const XrCompositionLayerProjectionView& eye, const float (&region)[4],
                           XrCompositionLayerProjectionView& out) {
    const double el = std::tan(eye.fov.angleLeft), er = std::tan(eye.fov.angleRight), eu = std::tan(eye.fov.angleUp),
                 ed = std::tan(eye.fov.angleDown);
    const int w = eye.subImage.imageRect.extent.width, h = eye.subImage.imageRect.extent.height;
    if (!(er > el) || !(eu > ed) || w < 1 || h < 1)
        return false;
    const auto column = [&](double t, bool up) {
        const double x = (t - el) / (er - el) * w;
        return std::clamp(static_cast<int>(up ? std::ceil(x - 1e-3) : std::floor(x + 1e-3)), 0, w);
    };
    const auto row = [&](double t, bool down) {
        const double y = (eu - t) / (eu - ed) * h;
        return std::clamp(static_cast<int>(down ? std::ceil(y - 1e-3) : std::floor(y + 1e-3)), 0, h);
    };
    const int x0 = column(region[0], false), x1 = column(region[1], true);
    const int y0 = row(region[2], false), y1 = row(region[3], true);
    if (x1 - x0 < 1 || y1 - y0 < 1)
        return false;
    const float t[4] = {static_cast<float>(el + (er - el) * x0 / w), static_cast<float>(el + (er - el) * x1 / w),
                        static_cast<float>(eu - (eu - ed) * y0 / h), static_cast<float>(eu - (eu - ed) * y1 / h)};
    out = eye;
    out.fov = tangents_to_fov(t);
    out.subImage.imageRect.offset.x += x0;
    out.subImage.imageRect.offset.y += y0;
    out.subImage.imageRect.extent = {x1 - x0, y1 - y0};
    return true;
}

// The race frames' views 2-3 image (FocusLayerPS over the runtime's focus view): the focus box's texel grid (its
// tangents `box`, left right up down, `boxW` x `boxH` texels) extended to whole texels over `region`, so the box's
// texels land on the image's own (copied, not resampled) wherever the box sits in the region. `tan` the image's
// tangents, `width` x `height` its texels, the box's top left texel at `boxX`, `boxY` in it (negative, or past the
// image: the box reaches out of the region, that part not shown). False: degenerate input.
struct RegionImage {
    float tan[4] = {};
    int width = 0, height = 0;
    int boxX = 0, boxY = 0;
};
inline bool focus_region_image(const float (&box)[4], int boxW, int boxH, const float (&region)[4], RegionImage& out) {
    if (boxW < 1 || boxH < 1 || !(box[1] > box[0]) || !(box[2] > box[3]) || !(region[1] > region[0]) ||
        !(region[2] > region[3]))
        return false;
    const double sx = boxW / (static_cast<double>(box[1]) - box[0]), sy = boxH / (static_cast<double>(box[2]) - box[3]);
    // Texel edges of the box's grid at or beyond the region's edges (a hair inside counts as on the edge).
    const long i0 = static_cast<long>(std::floor((region[0] - box[0]) * sx + 1e-3));
    const long i1 = static_cast<long>(std::ceil((region[1] - box[0]) * sx - 1e-3));
    const long j0 = static_cast<long>(std::floor((box[2] - region[2]) * sy + 1e-3));
    const long j1 = static_cast<long>(std::ceil((box[2] - region[3]) * sy - 1e-3));
    if (i1 - i0 < 1 || j1 - j0 < 1 || i1 - i0 > 65536 || j1 - j0 > 65536)
        return false;
    out.tan[0] = static_cast<float>(box[0] + i0 / sx);
    out.tan[1] = static_cast<float>(box[0] + i1 / sx);
    out.tan[2] = static_cast<float>(box[2] - j0 / sy);
    out.tan[3] = static_cast<float>(box[2] - j1 / sy);
    out.width = static_cast<int>(i1 - i0);
    out.height = static_cast<int>(j1 - j0);
    out.boxX = static_cast<int>(-i0);
    out.boxY = static_cast<int>(-j0);
    return true;
}

// The share of `box`'s area (tangents) inside `region`, 0..1: how much of the focus box the runtime shows.
inline double box_share_in_region(const float (&box)[4], const float (&region)[4]) {
    const double w = std::max(0.0, static_cast<double>(std::min(box[1], region[1])) - std::max(box[0], region[0]));
    const double h = std::max(0.0, static_cast<double>(std::min(box[2], region[2])) - std::max(box[3], region[3]));
    const double area = (static_cast<double>(box[1]) - box[0]) * (static_cast<double>(box[2]) - box[3]);
    return area > 0 ? std::clamp(w * h / area, 0.0, 1.0) : 0.0;
}

// One frame's layers for a quad view session, built by quad_frame. Its pointers point into itself: keep it alive
// until the runtime's xrEndFrame returns, and don't copy it.
struct QuadFrame {
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<XrCompositionLayerProjectionView> views;  // kQuadViews per projection, in order
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    uint32_t widened = 0;  // stereo projection layers given two more views (copies of their eyes)
    uint32_t cropped = 0;  // of their copies, those cut to the runtime's focus views
    uint32_t refused = 0;  // projection layers with neither two nor four views: left out
};

// `info`'s layers for a quad view session: a projection layer with four views (the focus views already in 2-3) as it
// is; one with two gets views 2-3 as copies of 0-1 (frames without focus views: the menus, loading, a composed frame,
// the runtime's inner views then showing the same pixels as its outer ones), each cut to the runtime's focus view
// `regions[eye]` (tangents in the eye's frame) when given, so the runtime gets them at the FOV it located. A
// projection layer of any other view count is left out (the runtime would refuse the whole frame). Other layer types
// pass through.
inline void quad_frame(const XrFrameEndInfo& info, QuadFrame& out, const float (*regions)[4] = nullptr) {
    out = {};
    uint32_t stereo = 0;
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const auto* l = info.layers[i];
        if (l && l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
            reinterpret_cast<const XrCompositionLayerProjection*>(l)->viewCount == 2)
            ++stereo;
    }
    out.projections.reserve(stereo);  // no reallocation: `layers` points at them
    out.views.reserve(static_cast<size_t>(stereo) * kQuadViews);
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = info.layers[i];
        if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            out.layers.push_back(l);
            continue;
        }
        const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(l);
        if (p->viewCount == kQuadViews) {
            out.layers.push_back(l);
            continue;
        }
        if (p->viewCount != 2 || !p->views) {
            ++out.refused;
            continue;
        }
        const size_t first = out.views.size();
        for (uint32_t v = 0; v < kQuadViews; ++v) {
            out.views.push_back(p->views[v % 2]);
            if (v >= 2 && regions && crop_to_region(p->views[v % 2], regions[v % 2], out.views.back()))
                ++out.cropped;
        }
        XrCompositionLayerProjection& q = out.projections.emplace_back(*p);
        q.viewCount = kQuadViews;
        q.views = out.views.data() + first;
        out.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
        ++out.widened;
    }
}

}  // namespace simqvfr

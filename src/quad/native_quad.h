// Native quad views ([experiment] native_quad_views=1; layer/native_quad.cpp): on a runtime that composes quad views
// itself (XR_VARJO_quad_views: Varjo's and Pimax's OpenXR runtimes), the session runs PRIMARY_QUAD_VARJO and every
// projection layer has four views: the eyes (0-1) and the focus views (2-3). The runtime samples the focus views
// straight onto the panel at their own resolution, which the layer's own composition into a native-size stereo image
// can't (2026-11-09: focus density above 100% only antialiased).
//
// The game keeps seeing a stereo session; the layer translates at its bottom edge. This header holds the pure part:
// a frame's layers made fit for a quad view session.
#pragma once

#include <cstdint>
#include <vector>

#include "../xr.h"

namespace simqvfr {

inline constexpr uint32_t kQuadViews = 4;

// One frame's layers for a quad view session, built by quad_frame. Its pointers point into itself: keep it alive
// until the runtime's xrEndFrame returns, and don't copy it.
struct QuadFrame {
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<XrCompositionLayerProjectionView> views;  // kQuadViews per projection, in order
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    uint32_t widened = 0;  // stereo projection layers given two more views (copies of their eyes)
    uint32_t refused = 0;  // projection layers with neither two nor four views: left out
};

// `info`'s layers for a quad view session: a projection layer with four views (the focus views already in 2-3) as it
// is; one with two gets views 2-3 as copies of 0-1 (frames without focus views: the menus, loading, a composed frame,
// the runtime's inner views then showing the same pixels as its outer ones). A projection layer of any other view
// count is left out (the runtime would refuse the whole frame). Other layer types pass through.
inline void quad_frame(const XrFrameEndInfo& info, QuadFrame& out) {
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
        for (uint32_t v = 0; v < kQuadViews; ++v)
            out.views.push_back(p->views[v % 2]);
        XrCompositionLayerProjection& q = out.projections.emplace_back(*p);
        q.viewCount = kQuadViews;
        q.views = out.views.data() + first;
        out.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
        ++out.widened;
    }
}

}  // namespace simqvfr

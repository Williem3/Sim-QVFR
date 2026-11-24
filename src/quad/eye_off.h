// One eye off ([quad] eye_off, live; GameFeatures::eyeOff): for a player who sees with one eye. The headset shows
// nothing in the other eye, and a game that knows how (GameAdapter::qvfr_eye_off_work: LMU) stops rendering most of
// it. The layer does the showing for every game in one place, at the frame's submission to the runtime
// (layer/eye_off.cpp), whatever composed the frame: every projection layer's view of that eye takes a black image,
// every quad layer for both eyes is shown to the other eye only. Pure: OpenXR structs only, no runtime or device.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "../xr.h"

namespace simqvfr {

// The eye that's off: kEyeOffNone, or 0 (left) and 1 (right), the views' order. The ini's value: 0 none, 1 the left
// eye off, 2 the right.
inline constexpr int kEyeOffNone = -1;
inline int eye_off_from_ini(int value) { return value == 1 ? 0 : value == 2 ? 1 : kEyeOffNone; }
inline int eye_off_to_ini(int eye) { return eye == 0 ? 1 : eye == 1 ? 2 : 0; }
inline const char* eye_off_name(int eye) { return eye == 0 ? "the left eye off" : eye == 1 ? "the right eye off" : "both eyes on"; }

// The setting as the layer applied it last (live_apply), read from any thread: the game's passes (LMU's) and the
// submission.
// The one copy is the core's (quad/shared_state.cpp).
int eye_off();
void set_eye_off(int eye);

// A frame's layers with one eye off, and the structs they point into (kept until the runtime's xrEndFrame returns).
struct EyeOffFrame {
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<XrCompositionLayerProjectionView> views;
    std::vector<XrCompositionLayerQuad> quads;
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    unsigned blankedViews = 0;  // projection views pointed at the black image
    unsigned quadsHidden = 0;   // quad layers for both eyes now shown to the other eye alone
    unsigned quadsDropped = 0;  // quad layers for that eye alone, left out
};

// The view's eye in a projection layer of `count` views: stereo's 0 and 1; with four (the quad view configuration,
// XR_VARJO_quad_views) 2 and 3 are the focus views of eyes 0 and 1. -1: none known (another count).
inline int projection_view_eye(uint32_t view, uint32_t count) {
    if (count == 2 && view < 2)
        return static_cast<int>(view);
    if (count == 4 && view < 4)
        return static_cast<int>(view % 2);
    return -1;
}

// `info`'s layers with eye `eye` off into `out`: every projection view of that eye shows `black` (its top-left
// `width` x `height`, array index 0; its chained structs, a depth image among them, left out); a quad layer shown to
// both eyes is shown to the other alone, one shown to that eye alone is left out; any other layer as it is. Its pose
// and FOV stay (the runtime reprojects black). An eye other than 0 or 1: the layers as they are.
inline void eye_off_frame(const XrFrameEndInfo& info, int eye, XrSwapchain black, int32_t width, int32_t height,
                          EyeOffFrame& out) {
    out = {};
    size_t projections = 0, views = 0, quads = 0;
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = info.layers[i];
        if (l && l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            ++projections;
            views += reinterpret_cast<const XrCompositionLayerProjection*>(l)->viewCount;
        } else if (l && l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            ++quads;
        }
    }
    // Reserved up front: the layers point into these vectors.
    out.projections.reserve(projections);
    out.views.reserve(views);
    out.quads.reserve(quads);
    out.layers.reserve(info.layerCount);
    const bool off = eye == 0 || eye == 1;
    const XrEyeVisibility other = eye == 0 ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
    const XrEyeVisibility self = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = info.layers[i];
        if (!off || !l) {
            out.layers.push_back(l);
            continue;
        }
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            XrCompositionLayerProjection p = *reinterpret_cast<const XrCompositionLayerProjection*>(l);
            const size_t first = out.views.size();
            for (uint32_t v = 0; v < p.viewCount; ++v) {
                XrCompositionLayerProjectionView view = p.views[v];
                if (projection_view_eye(v, p.viewCount) == eye) {
                    view.next = nullptr;
                    view.subImage.swapchain = black;
                    view.subImage.imageRect = {{0, 0}, {width, height}};
                    view.subImage.imageArrayIndex = 0;
                    ++out.blankedViews;
                }
                out.views.push_back(view);
            }
            p.views = p.viewCount ? &out.views[first] : nullptr;
            out.projections.push_back(p);
            out.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&out.projections.back()));
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            XrCompositionLayerQuad q = *reinterpret_cast<const XrCompositionLayerQuad*>(l);
            if (q.eyeVisibility == self) {
                ++out.quadsDropped;
                continue;
            }
            if (q.eyeVisibility == XR_EYE_VISIBILITY_BOTH) {
                q.eyeVisibility = other;
                ++out.quadsHidden;
            }
            out.quads.push_back(q);
            out.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&out.quads.back()));
        } else {
            out.layers.push_back(l);
        }
    }
}

}  // namespace simqvfr

#pragma once

// RF2's two views in each eye's own scene target (quad/two_view_atlas.h): where they sit, and the focus view's clip
// remap and rectangle from the OpenXR FOV. No hooks, no state of its own.

#include <d3d11.h>

#include <cmath>

#include "../../quad/offaxis_focus.h"
#include "../../quad/two_view_atlas.h"

namespace simqvfr::games::rf2 {

// The focus box's share of the eye's field of view per axis until the ini's focus_percent arrives (Renderer::set_focus).
constexpr double kDefaultFocusFraction = 0.30;

// The eye's scene target as the game made it (Renderer locks onto it): its size is the native eye.
struct SceneTarget {
    UINT width = 0, height = 0, samples = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool valid() const { return width && height; }
    bool matches(const D3D11_TEXTURE2D_DESC& d) const {
        return valid() && d.Width == width && d.Height == height && d.Format == format && d.SampleDesc.Count == samples;
    }
};

// Where the two views of an eye pass sit in the scene target: the outer view at the top left, the focus view at the top
// right. The composition (compositor.h) reads them back and writes the finished eye over the whole resolved target, so
// the game's post-processing sees a normal eye image.
struct AtlasLayout {
    UINT width = 0, height = 0;  // the scene target
    UINT outerW = 0, outerH = 0, focusW = 0, focusH = 0;
    UINT focus_x() const { return width - focusW; }
    bool valid() const { return width && height && outerW && outerH && focusW && focusH; }
    bool operator==(const AtlasLayout&) const = default;
};

// From the ini's sizes (layer/config.cpp views4_sizes), as shares of the native eye: `outerScale` the periphery's size
// per axis (periphery_scale), `focusW`/`focusH` the focus view's (focus density x the box's share of the field of view).
inline AtlasLayout make_atlas_layout(const SceneTarget& target, double outerScale, double focusW, double focusH) {
    const TwoViewAtlas fit = fit_two_view_atlas(target.width, target.height, outerScale, focusW, focusH);
    AtlasLayout l;
    l.width = target.width;
    l.height = target.height;
    l.focusW = fit.focusW;
    l.focusH = fit.focusH;
    l.outerW = fit.outerW;
    l.outerH = fit.outerH;
    return l;
}

// Per eye: the clip remap the focus view's twin applies ({a, b, c, d}: x' = a x + b w, y' = c y + d w) and the focus box
// in the eye's image (left, top, right, bottom as shares of it).
struct FocusMapping {
    float remap[2][4]{};
    float rect[2][4]{};
};

// The centred box of `fractionX` x `fractionY` with no FOV known yet.
inline FocusMapping centred_focus_mapping(double fractionX, double fractionY) {
    FocusMapping m;
    for (int eye = 0; eye < 2; ++eye) {
        m.remap[eye][0] = static_cast<float>(1.0 / fractionX);
        m.remap[eye][2] = static_cast<float>(1.0 / fractionY);
        m.rect[eye][0] = static_cast<float>(0.5 - fractionX / 2);
        m.rect[eye][1] = static_cast<float>(0.5 - fractionY / 2);
        m.rect[eye][2] = static_cast<float>(0.5 + fractionX / 2);
        m.rect[eye][3] = static_cast<float>(0.5 + fractionY / 2);
    }
    return m;
}

// The focus box of `fractionX` x `fractionY` of each eye's field of view (`tanFov`: left, right, down, up), centred on
// the eye's forward axis and moved `offsetYDegrees` up (+) or down (-) from it (the ini's fixed_offset_y), kept inside
// the eye (clamp_focus_centre). False (`out` unchanged) for a FOV or box that gives no focus view.
inline bool calculate_focus_mapping(const float (&tanFov)[2][4], double fractionX, double fractionY, FocusMapping& out,
                                    double offsetYDegrees = 0.0) {
    FocusMapping next{};
    for (int eye = 0; eye < 2; ++eye) {
        const double l = tanFov[eye][0], r = tanFov[eye][1], d = tanFov[eye][2], u = tanFov[eye][3];
        const double w = r - l, h = u - d;
        if (!(w > 0.0) || !(h > 0.0))
            return false;
        float ep[16]{};
        ep[0] = static_cast<float>(2.0 / w);
        ep[5] = static_cast<float>(2.0 / h);
        ep[8] = static_cast<float>(-(r + l) / w);
        ep[9] = static_cast<float>(-(u + d) / h);
        ep[10] = ep[11] = 1.0f;
        double cx = 0, cy = 0;
        if (offsetYDegrees != 0.0) {
            const double clamped = offsetYDegrees < -80.0 ? -80.0 : offsetYDegrees > 80.0 ? 80.0 : offsetYDegrees;
            cy = std::tan(clamped * 3.14159265358979 / 180.0);
        }
        if (!simqvfr::clamp_focus_centre(ep, fractionX, fractionY, cx, cy))
            return false;
        simqvfr::FocusBox box;
        float fp[16]{};
        if (!simqvfr::focus_projection(ep, fractionX, fractionY, cx, cy, box, fp) || !box.valid)
            return false;
        const auto m = simqvfr::clip_remap(ep, fp);
        next.remap[eye][0] = static_cast<float>(m.a);
        next.remap[eye][1] = static_cast<float>(m.b);
        next.remap[eye][2] = static_cast<float>(m.c);
        next.remap[eye][3] = static_cast<float>(m.d);
        for (int i = 0; i < 4; ++i)
            next.rect[eye][i] = static_cast<float>(box.rect[i]);
    }
    out = next;
    return true;
}

}  // namespace simqvfr::games::rf2

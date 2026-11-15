// The corner's and the focus view's sizes and rectangles (corner_internal.h).
#include "corner_internal.h"

#include <algorithm>
#include <cmath>

#include "../../log.h"

namespace scs {
namespace detail {

bool focus_on() { return g.focusPercent && !g.focusOff; }

bool eye_shaped(unsigned w, unsigned h) {
    if (!g.eyeW || w < 16 || w > g.eyeW || h > g.eyeH)
        return false;
    const double a = static_cast<double>(w) * g.eyeH, b = static_cast<double>(h) * g.eyeW;
    return std::abs(a - b) <= std::max(g.eyeW, g.eyeH);  // the same shape within a pixel of rounding
}

void size_of(ID3D11View* view, unsigned& w, unsigned& h) {
    w = h = 0;
    if (!view)
        return;
    ComPtr<ID3D11Resource> res;
    view->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (res && SUCCEEDED(res.As(&tex))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        w = d.Width;
        h = d.Height;
    }
}

void scale_viewports(UINT count, const D3D11_VIEWPORT* in, D3D11_VIEWPORT* out) {
    for (UINT i = 0; i < count; ++i) {
        out[i] = in[i];
        out[i].TopLeftX *= g.fx;
        out[i].TopLeftY *= g.fy;
        out[i].Width *= g.fx;
        out[i].Height *= g.fy;
    }
}

void scale_scissors(UINT count, const D3D11_RECT* in, D3D11_RECT* out) {
    for (UINT i = 0; i < count; ++i) {
        out[i].left = static_cast<LONG>(std::floor(in[i].left * g.fx));
        out[i].top = static_cast<LONG>(std::floor(in[i].top * g.fy));
        out[i].right = static_cast<LONG>(std::ceil(in[i].right * g.fx));
        out[i].bottom = static_cast<LONG>(std::ceil(in[i].bottom * g.fy));
    }
}

void exact_fraction() {
    // Whole pixels of the eye's own targets on each axis.
    g.fx = std::max(1.0f, std::round(g.percent / 100.0f * g.eyeW)) / static_cast<float>(g.eyeW);
    g.fy = std::max(1.0f, std::round(g.percent / 100.0f * g.eyeH)) / static_cast<float>(g.eyeH);
    simqvfr::logf("scs corner: eye targets %ux%u, the eye drawn into their top-left %ux%u (%.4f x %.4f)", g.eyeW, g.eyeH,
                  static_cast<unsigned>(std::lround(g.fx * g.eyeW)), static_cast<unsigned>(std::lround(g.fy * g.eyeH)),
                  g.fx, g.fy);
}

// The clip remap of a box: the focus view's clip position from the eye's (x' = a x + b w, y' = c y + d w).
void box_remap(const float (&box)[2], float (&out)[4]) {
    const float bx = beta_x(), by = beta_y();
    out[0] = 1 / bx;
    out[1] = (1 - 2 * box[0] - bx) / bx;
    out[2] = 1 / by;
    out[3] = (2 * box[1] + by - 1) / by;
}

// The box's shares of the eye's screen.
float beta_x() { return g.focusPercent / 100.0f; }

float beta_y() { return (g.focusPercentY > 0 ? g.focusPercentY : g.focusPercent) / 100.0f; }

// The focus region's shares of a full-size target: the box at the focus view's density, in whole texels.
float focus_fx() { return std::max(1.0f, std::round(beta_x() * g.density * g.eyeW)) / static_cast<float>(g.eyeW); }

float focus_fy() { return std::max(1.0f, std::round(beta_y() * g.density * g.eyeH)) / static_cast<float>(g.eyeH); }

// A twin holds the focus region at its origin and a little of the eye's picture around it, no more: 4% past the region
// on each axis, in multiples of 8 texels at full size so the half- and eighth-size textures' twins keep the same share.
// (At the game's own size the 34 twins of a 5192x4168 eye took up to 4.3 GB.)
unsigned twin_full(unsigned eye, float focus) {
    const unsigned n = (static_cast<unsigned>(std::ceil(focus * eye * 1.04f)) + 7) / 8 * 8;
    return std::min(eye, std::max(8u, n));
}

unsigned twin_w(unsigned w) {
    return std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(twin_full(g.eyeW, focus_fx())) * w / g.eyeW)));
}

unsigned twin_h(unsigned h) {
    return std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(twin_full(g.eyeH, focus_fy())) * h / g.eyeH)));
}

// The focus region's share of a twin (what focus_fx is of a texture of the game's).
float twin_fx() { return focus_fx() * g.eyeW / twin_full(g.eyeW, focus_fx()); }

float twin_fy() { return focus_fy() * g.eyeH / twin_full(g.eyeH, focus_fy()); }

// The focus view's rectangle in a w x h texture of the eye's shape.
D3D11_RECT focus_rect_in(unsigned w, unsigned h) {
    const LONG x = static_cast<LONG>(std::lround(g.fx * w));
    const LONG fw = std::min<LONG>(std::lround(focus_fx() * w), static_cast<LONG>(w) - x);
    const LONG fh = std::min<LONG>(std::lround(focus_fy() * h), static_cast<LONG>(h));
    return {x, 0, x + fw, fh};
}

// The focus view's rectangle in a full-size target: right of the corner, from the top.
D3D11_RECT focus_rect() {
    const LONG x = static_cast<LONG>(std::lround(g.fx * g.eyeW));
    const LONG w = std::min<LONG>(std::lround(focus_fx() * g.eyeW), static_cast<LONG>(g.eyeW) - x);
    const LONG h = std::min<LONG>(std::lround(focus_fy() * g.eyeH), static_cast<LONG>(g.eyeH));
    return {x, 0, x + w, h};
}

}  // namespace detail

}  // namespace scs

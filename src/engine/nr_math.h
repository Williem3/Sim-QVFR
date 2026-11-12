// DLSS-NR on the views (engine/nr_views.h): where the model works in a view and at what size. Pure arithmetic.
#pragma once

#include <algorithm>
#include <cmath>

namespace simqvfr::nr {

// A part of a view, in its pixels.
struct Region {
    unsigned x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Region&) const = default;
};

// The model works on no less than this each way (the runtime's features start there).
inline constexpr unsigned kMinWorking = 64;

// `percent` of a view width x height each way, centred on (u, v) (fractions of the view, v down) and moved to stay
// inside it; whole pixels, even sizes. 100 or more: the whole view. A view smaller than kMinWorking each way: itself.
inline Region covered(unsigned width, unsigned height, double u, double v, int percent) {
    Region r{0, 0, width, height};
    if (percent >= 100 || !width || !height)
        return r;
    const auto part = [percent](unsigned size) {
        const unsigned want = static_cast<unsigned>(std::lround(size * (std::max)(percent, 1) / 100.0)) & ~1u;
        return (std::min)(size, (std::max)(want, (std::min)(size, kMinWorking)));
    };
    r.width = part(width);
    r.height = part(height);
    const auto origin = [](double centre, unsigned size, unsigned part) {
        const double at = std::isfinite(centre) ? centre * size - part / 2.0 : (size - part) / 2.0;
        return static_cast<unsigned>(std::lround(std::clamp(at, 0.0, static_cast<double>(size - part))));
    };
    r.x = origin(u, width, r.width);
    r.y = origin(v, height, r.height);
    return r;
}

// What the model works at for a region `size` long at `percent`: no less than kMinWorking (or the region, if smaller),
// no more than the region.
inline unsigned working(unsigned size, int percent) {
    const unsigned want = static_cast<unsigned>(std::lround(size * std::clamp(percent, 1, 100) / 100.0));
    return (std::min)(size, (std::max)(want, (std::min)(size, kMinWorking)));
}

// Where the focus view sits in its eye's view, from the clip remap the eye's pass draws it with (x' = a x + b w,
// y' = c y + d w; engine/dlss_views.h DlssPass::remap): its centre as fractions of the eye's view, v down. False: no
// usable remap (the centre then is the view's).
inline bool focus_centre(const float remap[4], double& u, double& v) {
    u = v = 0.5;
    const double a = remap[0], c = remap[2];
    if (!(std::fabs(a) > 1e-6) || !(std::fabs(c) > 1e-6))
        return false;
    const double x = -remap[1] * (1.0 / a), y = -remap[3] * (1.0 / c);
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 1.5 || std::fabs(y) > 1.5)
        return false;
    u = 0.5 + 0.5 * x;
    v = 0.5 - 0.5 * y;
    return true;
}

// The model's share of the picture `t` of the way across a region (0..1) with a blend band `band` wide (a fraction of
// the region; 0: none) at the sides marked: 0 at such an edge, 1 from the band on. The shader's own (nr_views.cpp) in
// C++, for the checks.
inline double edge_weight(double t, double band, bool low, bool high) {
    if (band <= 0)
        return 1;
    const auto step = [](double x) {
        x = std::clamp(x, 0.0, 1.0);
        return x * x * (3 - 2 * x);
    };
    double w = 1;
    if (low)
        w *= step(t / band);
    if (high)
        w *= step((1 - t) / band);
    return w;
}

// The curve the scene goes to the model through and back (nr_views.cpp's shaders, in C++ for the checks): scene-linear
// HDR times `exposure`, the largest channel brought under 1 with the others in proportion (the hue kept), then sRGB's
// curve if `gamma`.
inline double srgb_encode(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
inline double srgb_decode(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
inline void encode(const double in[3], double exposure, bool gamma, double out[3]) {
    double c[3], m = 0;
    for (int i = 0; i < 3; ++i) {
        c[i] = (std::max)(in[i], 0.0) * exposure;
        m = (std::max)(m, c[i]);
    }
    for (int i = 0; i < 3; ++i) {
        out[i] = c[i] / (1 + m);
        if (gamma)
            out[i] = srgb_encode(out[i]);
    }
}
inline void decode(const double in[3], double exposure, bool gamma, double out[3]) {
    double c[3], m = 0;
    for (int i = 0; i < 3; ++i) {
        c[i] = std::clamp(in[i], 0.0, 1.0);
        if (gamma)
            c[i] = srgb_decode(c[i]);
        m = (std::max)(m, c[i]);
    }
    m = (std::min)(m, 0.9995);  // a value of 1 would be an endless brightness
    for (int i = 0; i < 3; ++i)
        out[i] = c[i] / (1 - m) / exposure;
}

}  // namespace simqvfr::nr

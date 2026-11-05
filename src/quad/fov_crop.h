// Field-of-view crop (QVFR, [crop]): each eye's located FOV loses a share of its tangent extent at
// each side before ACE sees it, so ACE never renders what a helmet (or anything else in front of the
// eyes) would cover. The sides are independent: top and bottom are shares of the eye's tangent height,
// outer (the temple side, mirrored per eye) and inner (the nose side) shares of its width. The outer
// views shrink by the kept share on each axis, so their pixel density stays the same and the cropped
// pixels are not rendered at all; the headset shows black outside the cropped FOV.
#pragma once

#include <algorithm>
#include <cmath>

#include "../xr.h"

namespace simqvfr {

// Each side's most, so every axis keeps at least 1 - 2 x kCropMax of the view.
inline constexpr double kCropMax = 0.40;

struct FovCrop {
    double top = 0, bottom = 0, outer = 0, inner = 0;  // shares removed at each side (0..kCropMax)
    double kept_width() const { return 1 - outer - inner; }
    double kept_height() const { return 1 - top - bottom; }
    bool on() const { return top > 0 || bottom > 0 || outer > 0 || inner > 0; }
};

// From percents (each side clamped on its own: changing one leaves the others as they are).
inline FovCrop fov_crop(int top, int bottom, int outer, int inner) {
    const auto share = [](int percent) { return std::clamp(percent, 0, static_cast<int>(kCropMax * 100)) / 100.0; };
    return {share(top), share(bottom), share(outer), share(inner)};
}

// Eye `eye`'s FOV (0 left, 1 right) with the crop's sides removed, in tangent space.
inline XrFovf crop_fov(const XrFovf& fov, const FovCrop& c, int eye) {
    if (!c.on())
        return fov;
    double l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    double u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const double w = r - l, h = u - d;
    const double cutLeft = eye == 0 ? c.outer : c.inner, cutRight = eye == 0 ? c.inner : c.outer;
    l += cutLeft * w;
    r -= cutRight * w;
    u -= c.top * h;
    d += c.bottom * h;
    return {static_cast<float>(std::atan(l)), static_cast<float>(std::atan(r)), static_cast<float>(std::atan(u)),
            static_cast<float>(std::atan(d))};
}

// The inverse, in tangents (left, right, up, down): the uncropped eye a cropped view `tan` was cut from.
inline void uncrop_tangents(const float (&tan)[4], const FovCrop& c, int eye, float (&full)[4]) {
    const double w = (tan[1] - tan[0]) / c.kept_width(), h = (tan[2] - tan[3]) / c.kept_height();
    const double cutLeft = eye == 0 ? c.outer : c.inner;
    full[0] = static_cast<float>(tan[0] - cutLeft * w);
    full[1] = static_cast<float>(full[0] + w);
    full[2] = static_cast<float>(tan[2] + c.top * h);
    full[3] = static_cast<float>(full[2] - h);
}

// A focus box given as a share of the uncropped view, as a share of the cropped view ACE renders (at
// most all of it): the focus box keeps its size, and its density, whatever the crop.
inline double cropped_share(double fraction, double kept) { return std::min(1.0, fraction / kept); }

}  // namespace simqvfr

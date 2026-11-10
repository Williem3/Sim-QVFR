// The driver tags' viewport under live resolution (docs/recon-lmu.md "Driver labels"). LMU draws the tags after a view's
// post-processing with a viewport of the whole eye target, while a pass renders into the top-left of it at its own size:
// in a pass rendered smaller than the target the tags land too far from the top-left and too large, by target / rendered
// (a second tag in the periphery). The fix cuts that viewport to the size the pass rendered.
#pragma once

#include <algorithm>
#include <cmath>

namespace lmu {

struct TagViewport {
    float x = 0, y = 0, width = 0, height = 0;
};

// `vp`: the viewport the tag draw gets. True, with `out` the viewport cut to the pass's rendered `passW` x `passH`, when it
// is the whole `targetW` x `targetH` eye target from its top-left and the pass rendered less than that. False otherwise
// (another target, e.g. a mirror's; or the pass rendered the whole target): the draw keeps its own.
inline bool tag_viewport_cut(const TagViewport& vp, unsigned targetW, unsigned targetH, unsigned passW, unsigned passH,
                             TagViewport& out) {
    if (!targetW || !targetH || !passW || !passH)
        return false;
    const auto same = [](float a, unsigned b) { return std::fabs(a - static_cast<float>(b)) <= 0.5f; };
    if (std::fabs(vp.x) > 0.5f || std::fabs(vp.y) > 0.5f || !same(vp.width, targetW) || !same(vp.height, targetH))
        return false;
    const unsigned w = std::min(passW, targetW), h = std::min(passH, targetH);
    if (w == targetW && h == targetH)
        return false;
    out = {vp.x, vp.y, static_cast<float>(w), static_cast<float>(h)};
    return true;
}

}  // namespace lmu

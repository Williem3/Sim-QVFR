// Which eye a pass belongs to: learned per headset image from the final blits (corner_internal.h).
#include "corner_internal.h"

#include "../../log.h"

namespace scs {
namespace detail {

// The frame's first scene pass begins (its G-buffer bind): the order learned for the headset image being drawn.
void scene_eye() {
    if (g.gbufferPasses != 1)
        return;
    const int image = g.acquiredIndex.load(std::memory_order_acquire);
    g.frameImage = image >= 0 && image < State::kOrderImages ? image : -1;
    const int first = g.frameImage >= 0 ? g.firstEyeByImage[g.frameImage] : -1;
    if (first < 0) {
        ++g.orderMisses;
        return;
    }
    ++g.orderHits;
    g.passEye[0] = first;
    g.passEye[1] = 1 - first;
}

// The frame's first final blit went to `eye`: the order of this frame's image index.
void learn_order(int eye) {
    if (g.frameImage < 0 || eye < 0)
        return;
    int& known = g.firstEyeByImage[g.frameImage];
    if (known >= 0 && known != eye && ++g.orderConflicts <= 8)
        simqvfr::logf("scs focus: headset image %d drawn with the %s eye first, before with the %s (%llu such)",
                      g.frameImage, eye ? "right" : "left", known ? "right" : "left",
                      static_cast<unsigned long long>(g.orderConflicts));
    known = eye;
    if (!g.orderLogged) {
        int learned = 0;
        for (int i : g.firstEyeByImage)
            learned += i >= 0 ? 1 : 0;
        if (learned >= 3) {
            g.orderLogged = true;
            simqvfr::log_line("scs focus: each frame's eye order now from the headset image it draws (learned from the "
                              "final blits)");
        }
    }
}

// The eye the pass being drawn belongs to: a headset image's own; in an eye's scene the pass's (scene_eye: the order
// learned for this frame's headset image); past it (the post-processing) the eye whose final blit is next.
int current_eye() {
    if (g.boundSwapchain && g.boundChainEye >= 0)
        return g.boundChainEye;
    return g.passEye[std::min(std::max(g.postPhase ? g.blits : g.gbufferPasses - 1, 0), 1)];
}

}  // namespace detail

}  // namespace scs

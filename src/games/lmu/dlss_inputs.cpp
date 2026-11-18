// LMU's inputs to the engine's DLAA (dlss_inputs.h).
#include "dlss_inputs.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../../engine/code_patch.h"
#include "../../engine/dlss_math.h"
#include "../../engine/dlss_views.h"
#include "../../log.h"
#include "abi.h"
#include "mvr_pass.h"
#include "qvfr_frame.h"
#include "view_pair.h"

namespace lmu {
namespace {

namespace dlss = simqvfr::dlss;

bool logged = false;
bool drawnLogged = false;
int drawnMisses = 0;
// The comparison of the two sources on a known build (the log): passes compared, those that differ, the first said.
unsigned long long compared = 0, differ = 0, noPair = 0;
int differSaid = 0;

// The pass's view and projection as the game's renderer holds them (its view parameters: a read). The projection
// there isn't one in every build: the eye's projection slot then. A build abi.h's addresses are from only.
bool renderer_matrices(int eye, float (&view)[16], float (&projection)[16], bool& fromSlot) {
    const std::uintptr_t base = simqvfr::module_base();
    const auto* renderer = *reinterpret_cast<const std::uint8_t* const*>(base + kRenderer);
    if (!renderer)
        return false;
    const std::uint8_t* params = renderer + kRendererViewEye;
    std::memcpy(view, params + kParamsView, sizeof(view));
    std::memcpy(projection, params + kParamsProjection, sizeof(projection));
    fromSlot = false;
    if (!dlss::projection(dlss::from_floats(projection))) {
        std::memcpy(projection,
                    reinterpret_cast<const void*>(base + kEyeProjection + static_cast<std::size_t>(eye) * kEyeSlotStride +
                                                  kSlotMatrix),
                    sizeof(projection));
        fromSlot = true;
    }
    return true;
}

// The pass's view and projection from what its draws uploaded (view_pair.h): of this frame's pairs in the pass, the one
// whose projection is the eye's. Any build.
bool drawn_matrices(int eye, float (&view)[16], float (&projection)[16]) {
    float pairs[4][32];
    const int n = mvr_view_pairs(eye, pairs, 4);
    float expected[16];
    if (!n || !qvfr_frame_eye_projection(eye, expected))
        return false;
    for (int i = 0; i < n; ++i) {
        float v[16], vp[16], p[16];
        std::memcpy(v, pairs[i], sizeof(v));
        std::memcpy(vp, pairs[i] + 16, sizeof(vp));
        if (projection_of_pair(v, vp, p) && same_eye(p, expected)) {
            std::memcpy(view, v, sizeof(view));
            std::memcpy(projection, p, sizeof(projection));
            return true;
        }
    }
    return false;
}

void log_once(const float (&view)[16], const float (&projection)[16], bool rigid, bool shaped, const char* from) {
    if (logged)
        return;
    logged = true;
    const float* v = view;
    simqvfr::logf("lmu dlss: the %s view matrix %s (rows %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f "
                  "%.4f | %.3f %.3f %.3f %.4f); projection %s (m0 %.5f m5 %.5f m8 %.5f m9 %.5f m10 %.6f m14 %.6f)",
                  from, rigid ? "is rigid" : "ISN'T rigid", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
                  v[10], v[11], v[12], v[13], v[14], v[15], shaped ? "has the shape" : "HASN'T the shape", projection[0],
                  projection[5], projection[8], projection[9], projection[10], projection[14]);
}

// A known build: the drawn camera against the renderer's, every pass (the log: the first three that differ, a count
// every 5400 passes). Nothing changes with it.
void compare(int eye, const float (&view)[16], const float (&projection)[16]) {
    float v[16], p[16];
    if (!drawn_matrices(eye, v, p)) {
        ++noPair;
        return;
    }
    ++compared;
    float worstView = 0, worstProj = 0;
    for (int i = 0; i < 16; ++i) {
        // The translation in metres, the rest unitless: both against a scale of their own.
        const float scale = (i >= 12 && i < 15) ? (std::max)(1.0f, std::fabs(view[i])) : 1.0f;
        worstView = (std::max)(worstView, std::fabs(v[i] - view[i]) / scale);
    }
    const int at[] = {0, 5, 8, 9, 10, 14};
    for (int i : at)
        worstProj = (std::max)(worstProj, std::fabs(p[i] - projection[i]) / (std::max)(1e-3f, std::fabs(projection[i])));
    if (worstView > 1e-4f || worstProj > 1e-3f) {
        ++differ;
        if (differSaid < 3) {
            ++differSaid;
            simqvfr::logf("lmu dlss: build-free camera: pass %d's drawn camera differs from the renderer's (view %.2g, "
                          "projection %.2g relative; m10 %.6f/%.6f m14 %.6f/%.6f drawn/renderer)",
                          eye, worstView, worstProj, p[10], projection[10], p[14], projection[14]);
        }
    }
    if ((compared + noPair) % 5400 == 0)
        simqvfr::logf("lmu dlss: build-free camera: the drawn camera agrees with the renderer's in %llu of %llu passes "
                      "(%llu without a drawn one)",
                      compared - differ, compared, noPair);
}

bool pass_matrices(int eye, float (&view)[16], float (&projection)[16]) {
    if (qvfr_frame_build_free()) {  // any build: what the pass's draws uploaded
        if (!drawn_matrices(eye, view, projection)) {
            if (!drawnLogged && ++drawnMisses == 90) {
                drawnLogged = true;
                simqvfr::log_line("lmu dlss: build-free camera: no pass camera found in the draws' constant buffers for "
                                  "90 passes (DLSS leaves those views as drawn)");
            }
            return false;
        }
        const bool rigid = dlss::rigid(dlss::from_floats(view)), shaped = dlss::projection(dlss::from_floats(projection));
        log_once(view, projection, rigid, shaped, "drawn (the per-draw buffer's)");
        return rigid && shaped;
    }
    bool fromSlot = false;
    if (!renderer_matrices(eye, view, projection, fromSlot))
        return false;
    const bool rigid = dlss::rigid(dlss::from_floats(view)), shaped = dlss::projection(dlss::from_floats(projection));
    log_once(view, projection, rigid, shaped, fromSlot ? "renderer's (projection from the eye's slot)" : "renderer's");
    if (rigid && shaped)
        compare(eye, view, projection);
    return rigid && shaped;
}

}  // namespace

void dlss_use_lmu_inputs() {
    logged = drawnLogged = false;
    drawnMisses = 0;
    simqvfr::dlss_set_matrices(&pass_matrices);
}

}  // namespace lmu

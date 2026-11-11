// LMU's inputs to the engine's DLAA (dlss_inputs.h).
#include "dlss_inputs.h"

#include <cstdint>
#include <cstring>

#include "../../engine/code_patch.h"
#include "../../engine/dlss_math.h"
#include "../../engine/dlss_views.h"
#include "../../log.h"
#include "abi.h"

namespace lmu {
namespace {

namespace dlss = simqvfr::dlss;

bool logged = false;

// The pass's view and projection as the game's renderer holds them (its view parameters: a read). The projection
// there isn't one in every build: the eye's projection slot then.
bool pass_matrices(int eye, float (&view)[16], float (&projection)[16]) {
    const std::uintptr_t base = simqvfr::module_base();
    const auto* renderer = *reinterpret_cast<const std::uint8_t* const*>(base + kRenderer);
    if (!renderer)
        return false;
    const std::uint8_t* params = renderer + kRendererViewEye;
    std::memcpy(view, params + kParamsView, sizeof(view));
    std::memcpy(projection, params + kParamsProjection, sizeof(projection));
    bool fromSlot = false;
    if (!dlss::projection(dlss::from_floats(projection))) {
        std::memcpy(projection,
                    reinterpret_cast<const void*>(base + kEyeProjection + static_cast<std::size_t>(eye) * kEyeSlotStride +
                                                  kSlotMatrix),
                    sizeof(projection));
        fromSlot = true;
    }
    const bool rigid = dlss::rigid(dlss::from_floats(view)), shaped = dlss::projection(dlss::from_floats(projection));
    if (!logged) {
        logged = true;
        const float* v = view;
        simqvfr::logf("lmu dlss: the renderer's view matrix %s (rows %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f "
                      "%.4f %.4f %.4f | %.3f %.3f %.3f %.4f); projection %s%s (m0 %.5f m5 %.5f m8 %.5f m9 %.5f m10 %.6f "
                      "m14 %.6f)",
                      rigid ? "is rigid" : "ISN'T rigid", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10],
                      v[11], v[12], v[13], v[14], v[15], shaped ? "has the shape" : "HASN'T the shape",
                      fromSlot ? " (from the eye's slot)" : "", projection[0], projection[5], projection[8], projection[9],
                      projection[10], projection[14]);
    }
    return rigid && shaped;
}

}  // namespace

void dlss_use_lmu_inputs() {
    logged = false;
    simqvfr::dlss_set_matrices(&pass_matrices);
}

}  // namespace lmu

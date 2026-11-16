// Offline check of the output density (quad/output_density.h): the composed output at the focus views' density, capped
// by the runtime's largest image and [quad] output_density_max, in steps that hold still, the chain rounded up.
#include <cmath>

#include "quad/output_density.h"
#include "quad/qvfr_settings.h"
#include "test_util.h"

using namespace simqvfr;

namespace {

bool near_(double a, double b) { return std::abs(a - b) < 1e-9; }

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // A 35% box at 200% density: the focus views are 0.7 native widths over 0.35 of the FOV's tangents.
    const double eyeTan = 2.0, nativeW = 2000;
    check(std::abs(output_density_wanted(0.7 * nativeW, 0.35 * eyeTan, nativeW, eyeTan) - 2.0) < 1e-9,
          "wanted: 200% density gives two texels per native pixel");
    check(std::abs(output_density_wanted(0.35 * nativeW, 0.35 * eyeTan, nativeW, eyeTan) - 1.0) < 1e-9,
          "wanted: 100% density gives one");
    check(output_density_wanted(0, 1, nativeW, eyeTan) == 1 && output_density_wanted(100, 0, nativeW, eyeTan) == 1 &&
              output_density_wanted(100, 1, 0, eyeTan) == 1,
          "wanted: unknown inputs mean native");

    check(near_(output_density_cap(2000, 2000, 0, 0, 400), 4.0), "cap: no runtime limit, output_density_max 400%");
    check(near_(output_density_cap(2000, 2000, 10000, 8000, 400), 2.5),
          "cap: half the runtime's largest width (both eyes side by side in the chain)");
    check(near_(output_density_cap(2000, 2000, 16000, 3000, 400), 1.5), "cap: the runtime's largest height");
    check(near_(output_density_cap(2000, 2000, 16000, 8000, 150), 1.5), "cap: output_density_max 150%");
    check(near_(output_density_cap(2000, 2000, 16000, 8000, 100), 1.0), "cap: 100% is native, as before");
    check(near_(output_density_cap(2000, 2000, 1500, 1500, 400), 1.0), "cap: never below native");
    // 2026-11-16, LMU on SteamVR: a 5190 x 4168 eye, the runtime's largest image 16384 x 16384. The chain is two eyes
    // across: 200% made it 20760 wide, past what a texture can be, and the right eye was broken.
    const double lmu = output_density_cap(5190, 4168, 16384, 16384, 400);
    check(2 * std::lround(5190 * lmu) <= 16384 && lmu > 1.5, "cap: two eyes at the cap fit the runtime's largest image");
    check(2 * std::lround(5190 * output_density_cap(5190, 4168, 0, 0, 400)) <= 16384,
          "cap: with no runtime limit, the texture limit");
    check(output_eye_max_width(16384) == 8192 && output_eye_max_width(0) == 8192 && output_eye_max_width(40000) == 8192 &&
              output_eye_max_height(0) == 16384,
          "the chain's eye: half the largest width, at most the texture limit");
    check(kOutputDensityMax.def == 400 && kOutputDensityMax.min == 100 && kOutputDensityMax.clamp(50) == 100,
          "the ini key: default 400, at least 100");

    check(near_(output_density_next(2.0, 1.0, 4.0), 2.0), "next: moves to the wanted scale");
    check(near_(output_density_next(0.6, 1.0, 4.0), 1.0), "next: density below 100% stays native");
    check(near_(output_density_next(3.0, 1.0, 1.7), 1.7), "next: capped");
    check(near_(output_density_next(1.72, 1.70, 4.0), 1.70) && near_(output_density_next(1.68, 1.70, 4.0), 1.70),
          "next: holds within the hold band (no flicker)");
    check(near_(output_density_next(1.80, 1.70, 4.0), 1.80), "next: a step past the band moves");
    check(near_(output_density_next(2.0, 2.0, 1.5), 1.5), "next: a lowered cap pulls the current scale down");
    check(near_(output_density_next(1.0, 1.02, 4.0), 1.02) && near_(output_density_next(0.9, 1.02, 4.0), 1.0),
          "next: back to native once clearly below");

    check(near_(output_chain_scale(1.0, 4.0), 1.0) && near_(output_chain_scale(1.05, 4.0), 1.25) &&
              near_(output_chain_scale(2.0, 4.0), 2.0) && near_(output_chain_scale(2.0, 1.8), 1.8),
          "chain: rounded up to quarter steps, within the cap");

    check(output_density_pixels(2000, 1.0, 1.0, 0) == 2000 && output_density_pixels(2000, 1.7, 1.0, 4000) == 3400,
          "pixels: native times the scale");
    check(output_density_pixels(2000, 1.7, 0.8, 4000) == 2720, "pixels: the FOV crop's kept share");
    check(output_density_pixels(2000, 2.5, 1.0, 4000) == 4000, "pixels: never past the chain's eye");
    check(output_density_pixels(2000, 0, 1.0, 0) == 1, "pixels: at least one");
    return test_result();
}

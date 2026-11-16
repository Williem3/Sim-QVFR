// Output density: the composed eyes at the focus views' density, not the runtime's native eye size. Composed at native
// size, a focus density above 100% was averaged back down before the runtime's lens pass (antialiasing, no detail);
// composed larger, the runtime resamples the focus texels itself, as mbucchia's Quad-Views-Foveated does
// (recommended x focus multiplier, capped by the runtime's largest image). The periphery keeps its own size against
// the native eye: it is only resampled into the larger output. Pure math; the layer holds the state (OutputDensity).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace simqvfr {

// The step the output's scale moves in (of the native eye), and how far the wanted scale must be from the current
// one to move it (no flicker from a density that sits between two steps).
inline constexpr double kOutputDensityStep = 0.05;
inline constexpr double kOutputDensityHold = 0.75 * kOutputDensityStep;
// A grown output chain: its scale rounded up to this, so a few steps up the menu don't each make a new chain.
inline constexpr double kOutputChainStep = 0.25;

// The focus views' texels per native output pixel, across: `focusTexels` over the focus box's tangent width, against
// the native eye's `nativeW` pixels over the eye's whole (uncropped) tangent width. 1 when anything is unknown.
inline double output_density_wanted(double focusTexels, double focusTanWidth, double nativeW, double eyeTanWidth) {
    if (!(focusTexels > 0) || !(focusTanWidth > 0) || !(nativeW > 0) || !(eyeTanWidth > 0))
        return 1;
    return (focusTexels / focusTanWidth) / (nativeW / eyeTanWidth);
}

// The largest image a D3D11 or D3D12 texture can be across or down (D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION): the
// output chain's limit too where the runtime says none.
inline constexpr uint32_t kOutputImageMax = 16384;

// The largest output eye, in pixels across: the output chain holds both eyes side by side (session.cpp,
// views4_d3d11.cpp: 2 x the eye wide), so half the runtime's largest image width (0: unknown, then the texture limit).
inline uint32_t output_eye_max_width(uint32_t maxW) { return std::min(maxW ? maxW : kOutputImageMax, kOutputImageMax) / 2; }
inline uint32_t output_eye_max_height(uint32_t maxH) { return std::min(maxH ? maxH : kOutputImageMax, kOutputImageMax); }

// The largest scale of the native eye: the output chain's largest eye (output_eye_max_width: half the runtime's
// largest image across, both eyes side by side; before 2026-11-16 the whole width, and above ~158% of a 5190 px eye
// the chain came out wider than a texture can be and the right eye was broken) and [quad] output_density_max
// (percent; 100 = native, as before this). At least 1.
inline double output_density_cap(uint32_t nativeW, uint32_t nativeH, uint32_t maxW, uint32_t maxH, int maxPercent) {
    double cap = std::max(1.0, maxPercent / 100.0);
    if (nativeW)
        cap = std::min(cap, static_cast<double>(output_eye_max_width(maxW)) / nativeW);
    if (nativeH)
        cap = std::min(cap, static_cast<double>(output_eye_max_height(maxH)) / nativeH);
    return std::max(1.0, cap);
}

// The next frame's scale: the wanted one in steps, within [1, cap]; the current one kept while the wanted one stays
// within kOutputDensityHold of it.
inline double output_density_next(double wanted, double current, double cap) {
    cap = std::max(1.0, cap);
    if (current >= 1 && current <= cap && std::abs(wanted - current) < kOutputDensityHold)
        return current;
    return std::clamp(std::round(wanted / kOutputDensityStep) * kOutputDensityStep, 1.0, cap);
}

// The scale a chain is made at for `scale`: rounded up to kOutputChainStep, within [1, cap].
inline double output_chain_scale(double scale, double cap) {
    cap = std::max(1.0, cap);
    return std::clamp(std::ceil(scale / kOutputChainStep - 1e-9) * kOutputChainStep, 1.0, cap);
}

// `native` pixels at `scale`, times `share` (the FOV crop's kept share), within [1, limit] (the chain's eye; 0: none).
inline int32_t output_density_pixels(uint32_t native, double scale, double share, uint32_t limit) {
    long n = std::lround(native * scale * share);
    if (limit)
        n = std::min<long>(n, static_cast<long>(limit));
    return static_cast<int32_t>(std::max<long>(n, 1));
}

}  // namespace simqvfr

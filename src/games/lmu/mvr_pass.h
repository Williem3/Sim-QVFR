// LMU's multi-view rendering in the eye passes (gate C of docs/recon-lmu.md "Multi-view rendering"; [experiment]
// lmu_mvr=1 with lmu_qvfr=1). In passes 0 and 1 every draw with a depth target bound (and no geometry, tessellation or
// stream output stage) runs the game shader's converted twin (mvr_probe.h) with NVIDIA's single-pass stereo on, so it
// also draws that eye's focus view beside the eye: the clip remap in cb13, a second viewport (and scissor) right of the
// game's, scaled from the eye's size to the focus view's (so the quarter-resolution pass gets its own). The lens mask
// draws the eye only (mvr_suspend). Gate C (lmu_mvr=1): passes 2-3 still render the focus views and the headset shows
// those; at sample frames the focus region of pass 0/1's resolved scene is compared with pass 2/3's own (alignment,
// brightness), the images saved in logs\captures. Gate D (lmu_mvr=2, mvr_keep_focus): the focus region is kept after
// pass 0/1's resolve, and pass 2/3's resolve (in cubeDX11_ApplyPostProcess) becomes a copy of it into the top-left, so
// their post-processing runs on the focus view the eye pass drew while they skip the scene (four_passes.cpp). The
// eyes' post-processing reads only their own corner of the targets.
#pragma once

#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace lmu {

struct MvrPassView {
    int eye = 0;
    float remap[4] = {1, 0, 1, 0};    // a, b, c, d: the focus view's clip x = a x + b w, y = c y + d w (focus_math.h)
    unsigned eyeW = 0, eyeH = 0;      // the size the eye renders at (its corner of the targets)
    unsigned focusW = 0, focusH = 0;  // the focus view's
};

// The immediate context's vtable entries the hooks use (d3d11.h order: ID3D11DeviceChild's 7, then the context's own).
// The vtable is inside the context and d3d11 rewrites its entries as it runs: every command's as the context goes idle
// (a flush) and busy (a command), and 130 of them, all of these among them, as multithread protection goes on or off.
// A hook on one function an entry held misses the calls through the others, so the hooks are on every function each
// entry is seen holding (mvr_pass.cpp). nvapi_stereo_check lists what d3d11 rewrites.
namespace context_slot {
inline constexpr unsigned kVSSetShader = 11, kGSSetShader = 23, kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUavs = 34,
                          kSOSetTargets = 37, kRSSetViewports = 44, kRSSetScissorRects = 45, kResolveSubresource = 57,
                          kHSSetShader = 60, kDSSetShader = 64;
inline constexpr unsigned kHooked[] = {kVSSetShader,       kGSSetShader,        kOMSetRenderTargets,
                                       kOMSetRenderTargetsAndUavs, kSOSetTargets, kRSSetViewports,
                                       kRSSetScissorRects, kResolveSubresource, kHSSetShader,
                                       kDSSetShader};
}  // namespace context_slot

// The game's immediate context hooked (VSSetShader, the render targets, viewports, scissors, resolves). False: off.
bool mvr_pass_install(ID3D11Device* device);
// Every pass: `view` for a pass that also draws its focus view (0-1), null otherwise. `sample` (pass 0): this frame's
// focus views are compared (the regions copied at each pass's end).
void mvr_pass_begin(int pass, const MvrPassView* view, bool sample, bool replaceResolve = false);
void mvr_pass_end(int pass);
void mvr_suspend(bool on);  // the hidden-area mask: the eye only
// Gate D: keep each focus region after its eye pass's resolve; true once eye `eye`'s is kept this frame (its pass drew
// the focus view), so pass 2/3 may skip the scene and have its resolve replaced (mvr_pass_begin's replaceResolve).
void mvr_keep_focus(bool on);
bool mvr_focus_ready(int eye);
// Every frame: a sampled frame's comparison; the counts, the hooks' CPU and each pass's GPU time every 900 frames.
void mvr_frame_end(unsigned long long frame);
// Before pass 0 (an A/B run): 0 for a frame with MVR, 1 for one with the four passes alone; GPU times kept apart.
void mvr_frame_mode(int mode);

// Tests: the single-pass stereo switch (NVAPI's by default) and where the comparison's images go.
using TwoViewsFn = int (*)(ID3D11DeviceContext*, bool);
void mvr_set_two_views(TwoViewsFn fn);
// A twin attached to a shader that had none (tests: the probe attaches them at creation, which the cache sees itself).
void mvr_twins_changed();
// The CPU in the context hooks timed (TSC) and logged with the counts; off by default (it costs a little itself).
void mvr_time_hooks(bool on);

struct MvrComparison {
    bool ok = false;
    int shiftX = 0, shiftY = 0;  // b's offset that matches a best (b(x + dx, y + dy) ~ a(x, y)); 0, 0: aligned
    double meanDiff = 0;         // mean |luminance a - b| at that offset, over mean luminance a
    double brightness = 0;       // mean luminance b / a
    double cellMedian = 0;       // median over 16 x 16 cells of b / a
    double differingPct = 0;     // pixels whose luminance differs by more than 10%
};
// a and b: RGBA16F as halves, w x h each; b searched +-range pixels for the best alignment (on every 4th pixel).
MvrComparison mvr_compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, unsigned w, unsigned h,
                          int range = 4);

struct MvrPassStats {
    uint64_t twinBinds = 0, plainBinds = 0, toggles = 0, viewportRewrites = 0, scissorRewrites = 0, stereoErrors = 0;
    uint64_t passesWithFocus = 0, samples = 0, resolves = 0, keptRegions = 0, replacedResolves = 0;
    uint64_t lookups = 0, lookupMisses = 0;  // twin lookups, and those not answered by the cache (GetPrivateData)
    uint64_t contextFunctions = 0;           // the context's functions hooked (every one its entries were seen holding)
    uint64_t resolveFunctions = 0;           // of those, ResolveSubresource's
};
MvrPassStats mvr_pass_stats();

}  // namespace lmu

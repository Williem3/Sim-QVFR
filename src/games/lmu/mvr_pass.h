// LMU's multi-view rendering in the eye passes (gate C of docs/recon-lmu.md "Multi-view rendering"; [experiment]
// lmu_mvr=1 with lmu_qvfr=1). In passes 0 and 1 every draw with a depth target bound (and no geometry, tessellation or
// stream output stage) runs the game shader's converted twin (mvr_probe.h) with NVIDIA's single-pass stereo on, or with
// instancing (mvr_probe.h MvrBackend: the draw made with twice its instances, the instanced twin picking the viewport
// by the instance's parity), so it also draws that eye's focus view beside the eye: the clip remap in cb13, a second
// viewport (and scissor) right of the game's, scaled from the eye's size to the focus view's (so the
// quarter-resolution pass gets its own). If NVIDIA's driver refuses single-pass stereo, instancing takes over for the
// rest of the session where the probe attached its twins. The lens mask
// draws with two views too (its mesh is in the eye's clip space, so the twin puts it where the focus view sees the hidden
// area: docs/recon-lmu.md "R6a run"). Gate C (lmu_mvr=1): passes 2-3 still render the focus views and the headset shows
// those; at sample frames the focus region of pass 0/1's resolved scene is compared with pass 2/3's own (alignment,
// brightness), the images saved in logs\captures. Gate D (lmu_mvr=2, mvr_keep_focus): the focus region is kept after
// pass 0/1's resolve, and pass 2/3's resolve (in cubeDX11_ApplyPostProcess) becomes a copy of it into the top-left, so
// their post-processing runs on the focus view the eye pass drew while they skip the scene (four_passes.cpp). The
// eyes' post-processing reads only their own corner of the targets.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "focus_math.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Resource;
struct ID3D11Texture2D;

namespace lmu {

struct MvrPassView {
    int eye = 0;
    float remap[4] = {1, 0, 1, 0};    // a, b, c, d: the focus view's clip x = a x + b w, y = c y + d w (focus_math.h)
    unsigned eyeW = 0, eyeH = 0;      // the size the eye renders at (its corner of the targets)
    unsigned focusW = 0, focusH = 0;  // the focus view's (its region in the targets, what the post reads)
    // What the scene's draws draw it at from the same corner (engine/dlss_views.h dlss_focus_scale: smaller for DLSS
    // super resolution, which upscales it into the region before the post); 0: focusW x focusH.
    unsigned focusRenderW = 0, focusRenderH = 0;
    // The eye's corner of its final (the eye target): eyeW x eyeH stretched by the game's final blit when its render
    // scale isn't 1 (focus_math.h final_extent); 0: as eyeW x eyeH. The focus final goes beside it, at the focus size.
    unsigned finalEyeW = 0, finalEyeH = 0;
    FocusBox box{};                 // its tangents and the eye's (the focus post's glare mapping; valid: known)
    // DLAA (engine/dlss_views.h): the clip shift (x += s[0] w, y += s[1] w) the eye's [0] and the focus view's [1] draws carry
    // this frame, which their images lose in DLAA. The overlays drawn after the post (the driver tags, the VR UI) are
    // drawn with their viewports moved back by it, so they land where the world is in the image. Zero: no jitter.
    double overlayShift[2][2] = {};
    // Texture sharpening (engine/dlss_views.h dlss_mip_bias): the mip bias the eye's [0] and the focus view's [1]
    // pixels sample with, through the game's pixel shaders' biased twins (engine/dxbc_patch.h add_focus_mip_bias). Both
    // 0: the game's own pixel shaders.
    float mipBias[2] = {};
};

// The immediate context's vtable entries the hooks use (d3d11.h order: ID3D11DeviceChild's 7, then the context's own).
// The vtable is inside the context and d3d11 rewrites its entries as it runs: every command's as the context goes idle
// (a flush) and busy (a command), and 130 of them, all of these among them, as multithread protection goes on or off.
// A hook on one function an entry held misses the calls through the others, so the hooks are on every function each
// entry is seen holding (mvr_pass.cpp). nvapi_stereo_check lists what d3d11 rewrites.
namespace context_slot {
inline constexpr unsigned kPSSetShader = 9, kVSSetShader = 11, kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20,
                          kDrawInstanced = 21, kGSSetShader = 23, kOMSetRenderTargets = 33,
                          kOMSetRenderTargetsAndUavs = 34, kSOSetTargets = 37, kDrawAuto = 38,
                          kDrawIndexedInstancedIndirect = 39, kDrawInstancedIndirect = 40, kRSSetViewports = 44,
                          kRSSetScissorRects = 45, kCopySubresourceRegion = 46, kResolveSubresource = 57, kHSSetShader = 60,
                          kDSSetShader = 64;
// The draws and CopySubresourceRegion only with the focus post (mvr_focus_replay) or instancing (the draws doubled);
// DrawAuto and the indirect draws only with instancing (their instance count can't be doubled: the eye's alone);
// PSSetShader only with texture sharpening possible (mvr_mip_bias_hooks).
inline constexpr unsigned kHooked[] = {kVSSetShader,       kGSSetShader,        kOMSetRenderTargets,
                                       kOMSetRenderTargetsAndUavs, kSOSetTargets, kRSSetViewports,
                                       kRSSetScissorRects, kResolveSubresource, kHSSetShader,
                                       kDSSetShader,       kDrawIndexed,        kDraw,
                                       kDrawIndexedInstanced, kDrawInstanced, kCopySubresourceRegion,
                                       kDrawAuto,          kDrawIndexedInstancedIndirect, kDrawInstancedIndirect,
                                       kPSSetShader};
}  // namespace context_slot

// The game's immediate context hooked (VSSetShader, the render targets, viewports, scissors, resolves). False: off.
bool mvr_pass_install(ID3D11Device* device);
// Every pass: `view` for a pass that also draws its focus view (0-1), null otherwise. `sample` (pass 0): this frame's
// focus views are compared (the regions copied at each pass's end).
void mvr_pass_begin(int pass, const MvrPassView* view, bool sample, bool replaceResolve = false);
void mvr_pass_end(int pass);
// R6c: the lens mask's draws (its vertex shader's role, yebis_shaders.h VsRole) are drawn like the scene's, or dropped
// while `*stale` (a live FOV crop change since the game built the mask: four_passes.cpp maskStale). Null: never dropped.
void mvr_mask_stale(const std::atomic<bool>* stale);
// The lens mask's draws dropped since the last call (four_passes' log).
int mvr_mask_dropped();
// R6b: a Draw with the VR UI panel's vertex shader outside the passes in a frame without focus finals (the menus): `fn`
// right before it (after = false) and right after it (true), the game's state bound; it may draw the panel again
// elsewhere with mvr_redraw_panel. Null: none.
using PanelFn = void (*)(ID3D11DeviceContext* context, bool after, unsigned vertexCount, unsigned startVertex);
void mvr_panel_draws(PanelFn fn);
// The source of the game's latest CopySubresourceRegion into `dst` in this frame or the one before (raw, compared only),
// null if none: LMU's submit copies each eye's image into that eye's swapchain image that way, which names the eye images
// from the d3d11 calls. Hooked with the focus post.
ID3D11Resource* mvr_copy_source(ID3D11Resource* dst);
// Eye `eye`'s final as its pass's final blit last drew into it (raw), null if none yet.
ID3D11Texture2D* mvr_eye_final(int eye);
// From a PanelFn: the panel's draw again into `rtv`, a width x height image, with the game's viewport and scissors
// scaled from its bound target's size to that; its targets, viewports and scissors put back.
void mvr_redraw_panel(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
                      unsigned vertexCount, unsigned startVertex);
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
// Instancing instead of NVIDIA's single-pass stereo (the probe's backend at mvr_pass_install; tests set it), and whether
// it may take over when single-pass stereo is refused. True: the passes draw with instancing now.
void mvr_use_instanced(bool on, bool fallback = true);
bool mvr_instanced();
// A twin attached to a shader that had none (tests: the probe attaches them at creation, which the cache sees itself).
void mvr_twins_changed();
// The CPU in the context hooks timed (TSC) and logged with the counts; off by default (it costs a little itself).
void mvr_time_hooks(bool on);
// The focus post (focus_post.h; [experiment] lmu_focus_replay): in passes 0-1 that drew their focus view, YEBIS's copy and
// composite run once more for it right after the eye's (the game's pixel shaders recognised at creation, mvr_probe.cpp;
// the draws seen through DrawIndexed after the pass's multisample resolve, the bound pixel shader read there). Before
// mvr_pass_install: DrawIndexed is hooked only with it on (turned on later, its functions are hooked as pass starts see
// them).
void mvr_focus_replay(bool on);
// The VR UI panel's canvas (2560x1440 with one mip level, sampled trilinearly: docs/recon-lmu.md "run 8") is drawn
// about a fifth of its size into a periphery at 20% of native: minified without mips it aliases, and the HUD, menus and
// MFD shimmer as the panel moves on the pixel grid. On: the panel's doubled draws sample a copy of it with its mips
// (copied and its mips made once a frame, bound for those draws only).
void mvr_ui_mips(bool on);
// Texture sharpening under DLSS (MvrPassView::mipBias): the game's pixel shader binds are hooked (before the install;
// only when DLSS can run, as each bind then passes through this code) and, while a pass draws two views with a bias,
// the game's pixel shaders are swapped for their mip bias twins (mvr_probe.h kMvrMipBiasTwinGuid) with the focus
// viewport and the two biases in pixel shader constant buffer 13.
void mvr_mip_bias_hooks(bool on);
// Those hooks are installed (texture sharpening was on at launch and its buffer could be made).
bool mvr_mip_bias_ready();
// Eye `eye`'s focus image from the focus post (RGBA16F, as the eye's composite) and the composites into it so far
// (null, 0: none yet).
ID3D11Texture2D* mvr_focus_image(int eye, unsigned long long& composites);
// R3: true when this frame's eye `eye` pass put its focus view's final into the eye's final target, beside the eye
// (the game's final blit replayed, its overlays doubled into it): that region of the target.
bool mvr_focus_final(int eye, unsigned& x, unsigned& y, unsigned& width, unsigned& height);

struct MvrComparison {
    bool ok = false;
    int shiftX = 0, shiftY = 0;  // b's offset that matches a best (b(x + dx, y + dy) ~ a(x, y)); 0, 0: aligned
    double meanDiff = 0;         // mean |luminance a - b| at that offset, over mean luminance a
    double brightness = 0;       // mean luminance b / a
    double cellMedian = 0;       // median over 16 x 16 cells of b / a
    double differingPct = 0;     // pixels whose luminance differs by more than 10%
    // mvr_compare_bgra8 only, at that offset: the largest and mean colour-channel difference (of 255), and the pixels
    // with a channel more than 4 off.
    int maxByte = 0;
    double meanByte = 0, bytesOverPct = 0;
};
// a and b: RGBA16F as halves, w x h each; b searched +-range pixels for the best alignment (on every 4th pixel).
MvrComparison mvr_compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, unsigned w, unsigned h,
                          int range = 4);
// The same for two gamma-encoded 8-bit BGRA images (as d3d11_read_bgra8 gives them; luminance after sRGB decoding).
MvrComparison mvr_compare_bgra8(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, unsigned w, unsigned h,
                                int range = 4);

struct MvrPassStats {
    uint64_t twinBinds = 0, plainBinds = 0, toggles = 0, viewportRewrites = 0, scissorRewrites = 0, stereoErrors = 0;
    uint64_t passesWithFocus = 0, samples = 0, resolves = 0, keptRegions = 0, replacedResolves = 0;
    uint64_t singleSamplePosts = 0;          // MSAA off: passes whose post-processing began at YEBIS's copy
    uint64_t lookups = 0, lookupMisses = 0;  // twin lookups, and those not answered by the cache (GetPrivateData)
    uint64_t contextEntries = 0;             // the context's entries hooked (DrawIndexed's with the focus post only)
    uint64_t contextFunctions = 0;           // the context's functions hooked (every one its entries were seen holding)
    uint64_t resolveFunctions = 0;           // of those, ResolveSubresource's
    uint64_t focusCopies = 0, focusComposites = 0, focusFailures = 0;  // the focus post's steps (mvr_focus_replay)
    // Passes whose copy ran but no composite we know followed, and draws into the resolve's target after the copy with
    // another pixel shader (the log names them).
    uint64_t focusNoComposite = 0, focusUnknownComposites = 0;
    uint64_t focusCompositesByBindings = 0;  // of the composites, those not of a known checksum (another variant)
    // R3: the focus view's final blits beside the eye; overlay draws doubled into it in the passes (the driver tags)
    // and after them (the VR UI).
    uint64_t focusBlits = 0, overlayDraws = 0, overlaysOutside = 0;
    uint64_t uiMipFrames = 0;  // frames whose VR UI canvas was copied with mips for the panel (mvr_ui_mips)
    // R6: the lens mask's draws (by its vertex shader), those with two views, those dropped while stale; the VR UI panel's
    // draws replayed for the menus (mvr_redraw_panel).
    uint64_t maskDraws = 0, maskDrawsTwo = 0, maskDropped = 0, panelReplays = 0;
    // Instancing: the game's draws made with twice their instances; those with a per-instance layout's twin bound around
    // them; DrawAuto and indirect draws drawn into the eye alone (no instance count to double).
    uint64_t doubledDraws = 0, layoutSwaps = 0, eyeOnlyDraws = 0;
    uint64_t instancedTakeovers = 0;  // single-pass stereo refused and instancing took over (once a session)
    // Texture sharpening: passes with a mip bias; pixel shader binds of a twin, of a game shader with none while it
    // was wanted, and the twin lookups (and those not cached).
    uint64_t biasPasses = 0, psTwinBinds = 0, psNoTwin = 0, psLookups = 0, psLookupMisses = 0;
    uint64_t biasCbLost = 0;  // checks that found the bias buffer unbound by the game while a twin was in use (rebound)
};
MvrPassStats mvr_pass_stats();
// Single-pass stereo was refused at a draw and instancing couldn't take over (off for the session since): no focus
// views from the eye passes.
bool mvr_broken();

// R4b (docs/recon-lmu.md "R4 plan"): the eye passes as the d3d11 calls show them, without the game's scene render hook.
// A pass begins at the first bind of a multisample render target with a depth target after the frame's start or the
// previous pass's end; it ends at the first bind of another render target after its final blit (the focus post's,
// mvr_focus_final), at the next pass's scene, or at the frame end. Observe only: each frame's tracked passes compared
// with the game's (mvr_pass_begin / mvr_pass_end), by the draws between their bounds; passes found in frames without the
// game's (menus) counted. Needs the focus post (the draws hooked).
void mvr_track_passes(bool on);
// R4b-2: the tracker drives the passes: `begin(p)` at a tracked pass's begin (before its draws), `end(p)` at its end, on
// the render thread inside the context's hooks; their work calls mvr_pass_begin / mvr_pass_end as the game's scene
// render hook did. Null: observe only.
using TrackFn = void (*)(int pass);
void mvr_track_drive(TrackFn begin, TrackFn end);
struct MvrTrackStats {
    uint64_t frames = 0, matched = 0, countDiffers = 0, beganBefore = 0, endedAfter = 0, passesCompared = 0;
    uint64_t startGapSum = 0, startGapMax = 0, endGapSum = 0, endGapMax = 0;  // draws of the game's pass left out
    uint64_t framesWithoutGame = 0, passesWithoutGame = 0;                    // false finds (menus)
    uint64_t singleSamplePasses = 0;  // passes begun on the learned single-sample scene target (MSAA off)
    uint64_t sceneRelearned = 0;      // that target learned again (the game made its targets anew)
    uint64_t singleSampleBlits = 0;   // those passes' final blits seen (their end), with the focus post or without
    uint64_t blitsBeforePost = 0;     // blits reading the scene's target before a pass's post began (not taken)
    uint64_t passesWithoutPost = 0;   // single-sample passes that ended before reaching YEBIS's copy
};
MvrTrackStats mvr_track_stats();
// MSAA off: the tracker has learned the scene's single-sample target (from YEBIS's copy) and seen no multisample scene
// since. Then the post-processing begins at YEBIS's copy (no resolve), and the four passes aren't fallen back to (they
// would hook the game's code), whatever happens to the passes.
bool mvr_single_sample_passes();

}  // namespace lmu

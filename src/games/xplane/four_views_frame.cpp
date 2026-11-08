// The focus boxes, one frame at a time (four_views.h; docs/recon-xplane.md "Live focus size"): the settings and the
// gaze latched once a frame into what that frame renders with, the push constants that take it to every shader after
// each pipeline bind, and the view rects' shares.
#include "four_views.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "abi.h"
#include "four_views_state.h"
#include "spirv_patch.h"

namespace xplane {

using namespace fv;

namespace {

using simqvfr::logf;

// Per-view sizes (four_views_set_view_sizes): each kind of view's rect, a share of its layers (x, y; 1: the whole
// layer), the outer views' then the focus views'. Taken into the latched frame.
struct ViewShares {
    std::atomic<bool> on{false};
    std::atomic<double> share[2][2] = {{1.0, 1.0}, {1.0, 1.0}};
};
ViewShares g_viewShares;
// Diagnostics (four_views_set_diagnostics): which outputs are off.
std::atomic<bool> g_diagSsrOff{false}, g_diagAoOff{false}, g_diagShadowOff{false};
// The focus views (four_views_set_focus): off = slots 2-3 copy the eyes (the copies stage).
std::atomic<bool> g_focus{false};

}  // namespace

namespace fv {

std::atomic<double> g_focusFraction{0.2}, g_focusFractionY{0.2};  // four_views_set_focus

LatchedFrame g_frame;
void latched_rects(float (&rects)[4]) { std::memcpy(rects, g_frame.load().rects, sizeof(rects)); }

// ---- The focus boxes in the shaders: the latched frame's, pushed after every pipeline bind ----
// The shaders read them from a push constant block (spirv_patch.h kFocusBlockBytes; docs/recon-xplane.md "Gaze: the
// focus box off the eye's axis", "Live focus size"): every pipeline layout X-Plane makes gets its range, and every
// pipeline bind is followed by a push. On only when all three call sites are patched, before X-Plane makes any shader
// or layout; without it the shader edits bake in the launch size, on the eyes' axes.
std::atomic<bool> g_gazeWanted{true};  // four_views_set_gaze_follow
std::atomic<bool> g_focusPush{false};
// VkPipelineLayoutCreateInfo and VkPushConstantRange, as far as needed (x64 layout).
struct PushConstantRange {
    std::uint32_t stageFlags, offset, size;
};
struct PipelineLayoutCreateInfo {
    std::uint32_t sType;
    const void* pNext;
    std::uint32_t flags;
    std::uint32_t setLayoutCount;
    const std::uint64_t* pSetLayouts;
    std::uint32_t pushConstantRangeCount;
    const PushConstantRange* pPushConstantRanges;
};

}  // namespace fv

namespace {

using CreatePipelineLayout = std::int32_t (*)(void* device, const PipelineLayoutCreateInfo* info, const void* allocator,
                                             std::uint64_t* layout);
using CmdBindPipeline = void (*)(void* commandBuffer, std::uint32_t bindPoint, std::uint64_t pipeline);
using CmdPushConstants = void (*)(void* commandBuffer, std::uint64_t layout, std::uint32_t stages, std::uint32_t offset,
                                  std::uint32_t size, const void* values);
constexpr std::uint32_t kFocusStages = 0x1 | 0x10 | 0x20;  // vertex, fragment, compute
constexpr PushConstantRange kFocusRange = {kFocusStages, 0, kFocusBlockBytes};
std::once_flag g_pushLayoutOnce;
std::uint64_t g_pushLayout = 0;  // ours, with the range alone: pushes through it reach every layout (same ranges)
std::atomic<int> g_layoutsExtended{0}, g_layoutsOwnRanges{0};
std::atomic<std::uint64_t> g_focusPushes{0};

// The gaze as the layer last gave it (four_views_set_gaze), for the next latch.
struct PendingGaze {
    std::mutex mutex;
    float tan[2][2] = {};
    bool valid[2] = {};
};
PendingGaze g_pendingGaze;

}  // namespace

namespace fv {

// X-Plane's vkCreatePipelineLayout call (kPipelineLayoutLoad): the boxes' range added to a layout without ranges of
// its own (X-Plane's shaders have no push constants; a layout with some is left as it is and counted).
std::int32_t create_pipeline_layout(void* device, const PipelineLayoutCreateInfo* info, const void* allocator,
                                    std::uint64_t* layout) {
    const auto real = *reinterpret_cast<CreatePipelineLayout*>(simqvfr::module_base() + kVkCreatePipelineLayoutSlot);
    if (!g_focusPush.load(std::memory_order_relaxed) || !info)
        return real(device, info, allocator, layout);
    std::call_once(g_pushLayoutOnce, [&] {
        PipelineLayoutCreateInfo own{};
        own.sType = info->sType;
        own.pushConstantRangeCount = 1;
        own.pPushConstantRanges = &kFocusRange;
        const std::int32_t r = real(device, &own, nullptr, &g_pushLayout);
        logf("xplane: focus boxes: own pipeline layout for the pushes: %d", r);
        if (r != 0)
            g_pushLayout = 0;
    });
    if (info->pushConstantRangeCount != 0) {
        if (g_layoutsOwnRanges.fetch_add(1) == 0)
            logf("xplane: focus boxes: a pipeline layout with push constants of its own (left as it is; its shaders "
                 "would read no boxes)");
        return real(device, info, allocator, layout);
    }
    PipelineLayoutCreateInfo extended = *info;
    extended.pushConstantRangeCount = 1;
    extended.pPushConstantRanges = &kFocusRange;
    g_layoutsExtended.fetch_add(1, std::memory_order_relaxed);
    return real(device, &extended, allocator, layout);
}

// X-Plane's vkCmdBindPipeline calls (kBindPipelineCompute, kBindPipelineGraphics): after the bind, the latched frame's
// boxes and values (spirv_patch.h kFocusBlockEntries).
void bind_pipeline(void* commandBuffer, std::uint32_t bindPoint, std::uint64_t pipeline) {
    const auto base = simqvfr::module_base();
    (*reinterpret_cast<CmdBindPipeline*>(base + kVkCmdBindPipelineSlot))(commandBuffer, bindPoint, pipeline);
    if (!g_pushLayout)
        return;
    const FocusFrame frame = g_frame.load();
    float values[kFocusBlockBytes / 4];
    std::memcpy(values, frame.box, sizeof(frame.box));
    std::memcpy(values + 4 * kFocusValuesEntry, frame.values, sizeof(frame.values));
    std::memcpy(values + 4 * kViewRectsEntry, frame.rects, sizeof(frame.rects));
    std::memcpy(values + 4 * kDiagEntry, frame.diag, sizeof(frame.diag));
    (*reinterpret_cast<CmdPushConstants*>(base + kVkCmdPushConstantsSlot))(commandBuffer, g_pushLayout, kFocusStages, 0,
                                                                           kFocusBlockBytes, values);
    g_focusPushes.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace fv

namespace {

// The latch's writer lock, and whether a frame has latched yet (until then a setting takes effect at once: the menu
// and loading screens draw before the first xrEndFrame).
std::mutex g_latchMutex;
bool g_latched = false;

// The AO edge band follows the box's size (docs/recon-xplane.md "Live focus size"). CACAO's edge error reached 12% of the
// focus view in with a 30% box (capture XPlane_frame2060); its sampling radius on screen goes as 1 / tan(half the view's
// field of view) (FFX CACAO's NDC-to-view scale), i.e. as 1 / the box's size for the same scene. So 12% x 0.3 / f, at
// most half the view. Only the 30% point is measured.
constexpr double kAoEdgeBandMeasuredAt = 0.3;
float ao_edge_band(double fx, double fy) {
    const double f = std::clamp(std::min(fx, fy), 0.05, 1.0);
    return static_cast<float>(std::min(0.5, kAoEdgeBand * kAoEdgeBandMeasuredAt / f));
}

// The settings and the gaze become the boxes of the frames drawn from now on (under g_latchMutex).
void latch_frame() {
    float tan[2][2];
    bool valid[2];
    {
        std::lock_guard guard(g_pendingGaze.mutex);
        std::memcpy(tan, g_pendingGaze.tan, sizeof(tan));
        std::memcpy(valid, g_pendingGaze.valid, sizeof(valid));
    }
    float outer[2][4] = {};
    bool recorded = false;
    {
        std::lock_guard guard(g_record.mutex);
        recorded = g_record.valid;
        std::memcpy(outer, g_record.outer, sizeof(outer));
    }
    FocusFrame f;
    f.focus = g_focus.load() ? 1u : 0u;
    for (int k = 0; k < 2; ++k)
        for (int a = 0; a < 2; ++a)
            f.rects[2 * k + a] = static_cast<float>(g_viewShares.on.load() ? g_viewShares.share[k][a].load() : 1.0);
    const double fx = f.focus ? g_focusFraction.load() : 1.0, fy = f.focus ? g_focusFractionY.load() : 1.0;
    // The centres move only when the shaders get them pushed: baked edits place the box on the eye's axis.
    const bool follow = g_focusPush.load() && g_gazeWanted.load();
    for (int e = 0; e < 2; ++e) {
        const double l = outer[e][0], r = outer[e][1], u = outer[e][2], d = outer[e][3];
        double cx = 0, cy = 0;
        if (!f.focus && recorded && g_focusPush.load()) {
            // Focus off: the box is the whole eye (its centre the eye's, the span 1), so the edits give views 2-3
            // their eye's own coefficients, which copy it.
            cx = 0.5 * (l + r);
            cy = 0.5 * (u + d);
        } else if (f.focus && follow && valid[e] && recorded) {
            // Clamped so the box stays inside its eye's view (left, right, up, down), as focus_turn does for ACE.
            const double hw = 0.5 * fx * (r - l), hh = 0.5 * fy * (u - d);
            if (l + hw <= r - hw && d + hh <= u - hh) {
                cx = std::clamp(static_cast<double>(tan[e][0]), l + hw, r - hw);
                cy = std::clamp(static_cast<double>(tan[e][1]), d + hh, u - hh);
            }
        }
        const float box[4] = {static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(fx), static_cast<float>(fy)};
        std::memcpy(f.box[e], box, sizeof(box));
    }
    // The periphery's alpha-test mip bias: log2 of a focus pixel's size over a periphery pixel's (spirv_patch.h): the
    // box's share of the eye, times the outer views' share of their layers over the focus views'.
    f.values[0] = f.focus ? static_cast<float>(0.5 * std::log2(fx * fy * f.rects[0] * f.rects[1] /
                                                                (static_cast<double>(f.rects[2]) * f.rects[3])))
                          : 0.0f;
    f.values[1] = ao_edge_band(fx, fy);
    f.diag[0] = g_diagSsrOff.load() ? 1.0f : 0.0f;
    f.diag[1] = g_diagAoOff.load() ? 1.0f : 0.0f;
    f.diag[2] = g_diagShadowOff.load() ? 1.0f : 0.0f;
    g_frame.store(f);
    update_lod_scale();
}

// Before the first frame's latch, a setting takes effect at once.
void latch_if_no_frame() {
    std::lock_guard lock(g_latchMutex);
    if (!g_latched)
        latch_frame();
}

// The view rects' shares (four_views_set_view_sizes, four_views_set_view_shares), all four under the latch's lock so a
// frame never latches half of a change; before the first latch, at once.
void store_view_shares(bool on, const double (&share)[2][2]) {
    std::lock_guard lock(g_latchMutex);
    for (int k = 0; k < 2; ++k)
        for (int a = 0; a < 2; ++a)
            g_viewShares.share[k][a] = share[k][a];
    g_viewShares.on = on;
    if (!g_latched)
        latch_frame();
}

}  // namespace

void four_views_set_view_sizes(bool on, double outerWidth, double outerHeight, double focusWidth, double focusHeight) {
    // The layer is the larger of the two per axis (the size the layer offers X-Plane: qvfr_one_view_size).
    const double w = std::max(outerWidth, focusWidth), h = std::max(outerHeight, focusHeight);
    const bool valid = w > 0 && h > 0 && outerWidth > 0 && outerHeight > 0 && focusWidth > 0 && focusHeight > 0;
    on = on && valid && g_viewRects.load();
    const double share[2][2] = {{on ? outerWidth / w : 1.0, on ? outerHeight / h : 1.0},
                                {on ? focusWidth / w : 1.0, on ? focusHeight / h : 1.0}};
    store_view_shares(on, share);
    logf("xplane: per-view sizes %s: outer views %.0f%% x %.0f%% of their layers, focus views %.0f%% x %.0f%% (views "
         "%.2f x %.2f and %.2f x %.2f of the native eye, layers %.2f x %.2f)%s",
         on ? "on" : "off", share[0][0] * 100, share[0][1] * 100, share[1][0] * 100, share[1][1] * 100, outerWidth,
         outerHeight, focusWidth, focusHeight, w, h,
         !g_viewRects.load() ? " (needs view rects)" : !valid ? " (no layout)" : "");
}

void four_views_set_gaze_follow(bool on) { g_gazeWanted = on; }

bool four_views_gaze_follows() { return g_focusPush.load() && g_gazeWanted.load(); }

void four_views_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]) {
    std::lock_guard guard(g_pendingGaze.mutex);
    std::memcpy(g_pendingGaze.tan, tan, sizeof(tan));
    std::memcpy(g_pendingGaze.valid, valid, sizeof(valid));
}

void four_views_latch_frame() {
    std::lock_guard lock(g_latchMutex);
    g_latched = true;
    latch_frame();
}

void four_views_set_diagnostics(bool ssrOff, bool aoOff, bool shadowOff) {
    if (ssrOff == g_diagSsrOff.load() && aoOff == g_diagAoOff.load() && shadowOff == g_diagShadowOff.load())
        return;
    {
        std::lock_guard lock(g_latchMutex);
        g_diagSsrOff = ssrOff;
        g_diagAoOff = aoOff;
        g_diagShadowOff = shadowOff;
        if (!g_latched)
            latch_frame();
    }
    logf("xplane: diagnostics: SSR %s, AO %s, sun shadow %s (from the next frame)%s", ssrOff ? "OFF" : "on",
         aoOff ? "OFF" : "on", shadowOff ? "OFF" : "on",
         g_diagModules.load() > 0 ? "" : " (no shader has the switch: xplane_diag=1 wasn't set at launch)");
}

bool four_views_set_view_shares(double outerX, double outerY, double focusX, double focusY) {
    if (!g_viewShares.on.load())
        return false;
    const auto share = [](double s) { return std::clamp(s, 0.01, 1.0); };
    const double shares[2][2] = {{share(outerX), share(outerY)}, {share(focusX), share(focusY)}};
    store_view_shares(true, shares);
    return true;
}

void four_views_gaze_log() {
    const FocusFrame f = g_frame.load();
    logf("xplane: focus boxes (tangents, shares of the eye) L centre %.3f, %.3f size %.3f x %.3f, R centre %.3f, %.3f "
         "size %.3f x %.3f; alpha mip bias %.2f, AO edge band %.3f; view rects (shares of the layers) outer %.3f x %.3f, "
         "focus %.3f x %.3f; %s, %llu pushes so far, %d pipeline layouts given their range, %d with push constants of "
         "their own",
         f.box[0][0], f.box[0][1], f.box[0][2], f.box[0][3], f.box[1][0], f.box[1][1], f.box[1][2], f.box[1][3],
         f.values[0], f.values[1], f.rects[0], f.rects[1], f.rects[2], f.rects[3],
         g_focusPush.load() ? "pushed" : "NOT pushed (baked into the shaders)",
         static_cast<unsigned long long>(g_focusPushes.load()), g_layoutsExtended.load(), g_layoutsOwnRanges.load());
}

void four_views_set_focus(bool on, double fraction, double fractionY) {
    fractionY = fractionY > 0.0 ? fractionY : fraction;
    const bool changed = fraction != g_focusFraction.load() || fractionY != g_focusFractionY.load();
    if (changed && !g_focusPush.load() && g_coefficientModules.load() > 0)
        logf("xplane: focus size changed after X-Plane made its shaders, which have the launch size baked in (the "
             "pipeline calls aren't wrapped): the sky and screen-space passes keep it until X-Plane restarts");
    g_focusFraction = fraction;
    g_focusFractionY = fractionY;
    g_focus = on;
    latch_if_no_frame();
    logf("xplane: focus views %s (%.0f%% x %.0f%% of each eye, %s; from the next frame)", on ? "on" : "off (copies of the eyes)",
         fraction * 100.0, fractionY * 100.0, four_views_gaze_follows() ? "centred on the gaze" : "centred on the eye's axis");
}

}  // namespace xplane

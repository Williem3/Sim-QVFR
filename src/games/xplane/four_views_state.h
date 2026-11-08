// The state the four views' files share (namespace xplane::fv): the draw path's code patches (four_views.cpp), the
// vkCreateShaderModule hook and its SPIR-V edits (four_views_shaders.cpp), and the focus boxes latched once a frame
// (four_views_frame.cpp). The latched frame, the tangents the views were rendered with, the switches more than one of
// them reads, and the hooks four_views_copies_start puts in. Each global is defined in the file named beside it.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>

#include "spirv_patch.h"

namespace xplane::fv {

// The focus box's size (four_views_set_focus), the centres on the gaze (four_views_set_gaze_follow), and whether the
// shaders get the boxes pushed (four_views_copies_start turns it on): four_views_frame.cpp.
extern std::atomic<double> g_focusFraction, g_focusFractionY;
extern std::atomic<bool> g_gazeWanted;
extern std::atomic<bool> g_focusPush;
// One viewport per view (four_views_set_view_rects): four_views.cpp.
extern std::atomic<bool> g_viewRects;
// Shader modules given the projection coefficients' edit, and the diagnostics' switch: four_views_shaders.cpp.
extern std::atomic<int> g_coefficientModules;
extern std::atomic<int> g_diagModules;

// The share of a focus view, from each edge, over which its AO goes from its eye's to its own: CACAO's edge error died
// out 160 rows (12%) in (capture XPlane_frame2060, docs/recon-xplane.md "Measured: a dark rim at the top of the focus
// views").
inline constexpr float kAoEdgeBand = 0.12f;

// ---- One frame's focus boxes (docs/recon-xplane.md "Live focus size") ----
// What a frame renders with: per eye its focus box (centre tangents; size as shares of the eye's tangent span), the
// values that follow the size (spirv_patch.h kFocusValuesEntry) and each view's rect in its layers. Settings and gaze
// reach it only at the latch, once a frame (four_views_latch_frame, at xrEndFrame), so one frame's focus projections,
// push constants, viewports, LOD and composition record agree however often the size or the gaze changes.
struct FocusFrame {
    float box[2][4] = {{0, 0, 1, 1}, {0, 0, 1, 1}};  // eye e: centre x, y; size x, y (the push block's entry e)
    float values[4] = {};                            // the alpha-test mip bias, the AO edge band, 0, 0 (entry 2)
    float rects[4] = {1, 1, 1, 1};                   // outer views' share x, y; focus views' (entry 3)
    float diag[4] = {};                              // diagnostics: SSR off, AO off, sun shadow off, 0 (entry 4)
    std::uint32_t focus = 0;                         // the focus views are on (else slots 2-3 copy the eyes)
};
static_assert(sizeof(FocusFrame) % 4 == 0 && std::is_trivially_copyable_v<FocusFrame>, "copied as words");
static_assert(sizeof(FocusFrame::box) + sizeof(FocusFrame::values) + sizeof(FocusFrame::rects) +
                          sizeof(FocusFrame::diag) == kFocusBlockBytes &&
                  offsetof(FocusFrame, values) == 16 * kFocusValuesEntry &&
                  offsetof(FocusFrame, rects) == 16 * kViewRectsEntry && offsetof(FocusFrame, diag) == 16 * kDiagEntry,
              "the push block's layout");

// A seqlock: one writer at a time (g_latchMutex); readers on any thread (every draw and pipeline bind) never block it
// and retry while a store is under way.
class LatchedFrame {
public:
    LatchedFrame() { store(FocusFrame{}); }
    void store(const FocusFrame& f) {
        std::uint32_t raw[kWords];
        std::memcpy(raw, &f, sizeof(raw));
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t k = 0; k < kWords; ++k)
            words_[k].store(raw[k], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }
    FocusFrame load() const {
        std::uint32_t raw[kWords];
        for (;;) {
            const std::uint32_t before = seq_.load(std::memory_order_acquire);
            if (before & 1) {
                YieldProcessor();
                continue;
            }
            for (std::size_t k = 0; k < kWords; ++k)
                raw[k] = words_[k].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == before)
                break;
        }
        FocusFrame f;
        std::memcpy(&f, raw, sizeof(raw));
        return f;
    }

private:
    static constexpr std::size_t kWords = sizeof(FocusFrame) / 4;
    std::atomic<std::uint32_t> seq_{0};
    std::atomic<std::uint32_t> words_[kWords];
};
// This frame's (four_views_frame.cpp).
extern LatchedFrame g_frame;
// The latched frame's view rects.
void latched_rects(float (&rects)[4]);

// The tangents the views were rendered with (four_views_tangents): taken by the first draw after each request.
struct TangentRecord {
    std::mutex mutex;
    float outer[2][4] = {}, focus[2][4] = {};
    bool valid = false;
};
// Taken by fill_views, read by the latch and four_views_tangents: four_views.cpp.
extern TangentRecord g_record;
extern std::atomic<bool> g_recordWanted;

// The object and tree LOD's width scale, from the latched frame and the settings: four_views.cpp.
void update_lod_scale();

// The hooks four_views_copies_start puts on X-Plane's Vulkan calls: vkCreateShaderModule (four_views_shaders.cpp),
// vkCreatePipelineLayout and vkCmdBindPipeline (four_views_frame.cpp).
struct ShaderModuleCreateInfo;
std::int32_t create_shader_module(void* device, const ShaderModuleCreateInfo* info, const void* allocator,
                                  std::uint64_t* module);
struct PipelineLayoutCreateInfo;
std::int32_t create_pipeline_layout(void* device, const PipelineLayoutCreateInfo* info, const void* allocator,
                                    std::uint64_t* layout);
void bind_pipeline(void* commandBuffer, std::uint32_t bindPoint, std::uint64_t pipeline);

}  // namespace xplane::fv

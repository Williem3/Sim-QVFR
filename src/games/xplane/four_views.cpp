// X-Plane's draw path in four views (four_views.h): the code patches four_views_copies_start and
// four_views_eye_columns_start put in (the multiplier, the viewports, the culls, the LOD width, the per-draw fill of
// slots 2-3) and their logs. The shader-module hook is in four_views_shaders.cpp, the per-frame focus boxes and their
// push constants in four_views_frame.cpp.
#include "four_views.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "MinHook.h"
#include "../../log.h"
#include "abi.h"
#include "focus_projection.h"
#include "four_views_state.h"

namespace xplane {

using namespace fv;

namespace {

using simqvfr::code_matches;
using simqvfr::code_redirect;
using simqvfr::logf;

std::atomic<int> g_viewRectSets{0};     // set viewports calls given four rects

// set viewports(count, multiplier, rects {x, y, w, h} x count) (abi.h kSetViewports). Stereo (multiplier 2) becomes
// four views; with view rects, a four-view pass's one rect becomes one per view, each view's scaled by its share of
// the layer (the latched frame's) from the rect's origin, which is the layer's top-left in memory: X-Plane flips
// within each viewport (docs/recon-xplane.md "Per-view sizes, stage 2").
using SetViewports = void (*)(int count, int multiplier, const std::int32_t* rects);
SetViewports g_setViewportsOriginal = nullptr;
void set_viewports(int count, int multiplier, const std::int32_t* rects) {
    if (multiplier == 2)
        multiplier = 4;
    if (g_viewRects.load(std::memory_order_relaxed) && count == 1 && multiplier == 4 && rects) {
        float share[4];
        latched_rects(share);
        std::int32_t four[16];
        for (int v = 0; v < 4; ++v) {
            std::int32_t* r = four + v * 4;
            const float* s = share + (v < 2 ? 0 : 2);
            r[0] = rects[0];
            r[1] = rects[1];
            r[2] = s[0] < 1.0f ? std::max(1, static_cast<std::int32_t>(rects[2] * s[0] + 0.5f)) : rects[2];
            r[3] = s[1] < 1.0f ? std::max(1, static_cast<std::int32_t>(rects[3] * s[1] + 0.5f)) : rects[3];
        }
        g_viewRectSets.fetch_add(1, std::memory_order_relaxed);
        g_setViewportsOriginal(4, 4, four);
        return;
    }
    g_setViewportsOriginal(count, multiplier, rects);
}
// Stereo vegetation culls given u_render_layers 4, by the setup's view count (max(multiplier, viewport count)): index
// 0-3 that count, 4 for 4 or more (the stub counts; four_views_vegetation_log).
std::atomic<int> g_vegetationCulls[5]{};
std::atomic<std::uint32_t> g_vegetationLodBias{0};  // the last stereo cull's u_lod_bias (float bits; the stub writes)
// The LOD width stubs' state (abi.h kWorldLodHeight, kGraphLodHeight), one per site: the scale (16.16) the VR world
// graph's width is multiplied by, the calls given that width (scaled) and another (kept), and the last other width.
struct LodHeightSite {
    std::atomic<std::uint32_t> scale{65536};
    std::atomic<std::uint32_t> scaled{0};
    std::atomic<std::uint32_t> other{0};
    std::atomic<std::int32_t> lastOther{0};
};
static_assert(sizeof(LodHeightSite) == 16 && sizeof(std::atomic<std::uint32_t>) == 4, "the stubs' [r11 + 4n]");
LodHeightSite g_lodHeight[2];         // the world pass (kWorldLodHeight), the graph setup (kGraphLodHeight)
std::atomic<bool> g_focusLod{false};  // four_views_set_focus_lod
std::atomic<int> g_objectDetail{100};  // four_views_set_object_detail: percent of the way to the focus views' density

}  // namespace

namespace fv {

std::atomic<bool> g_viewRects{false};   // four_views_set_view_rects: one viewport per view
TangentRecord g_record;
std::atomic<bool> g_recordWanted{true};

// The LOD width: the eye's width at the finest view's pixel density, so every view's objects and trees take the LOD the
// finest needs (the cull and its LOD are shared by all four views). W is the layer's width; a focus view spans its share
// of it over the box's share of the eye (s_focus W / box), the eye view its own share (s_outer W). From the latched
// frame's size and rects. Object detail d (percent) takes the way there in steps of equal ratio: W x finest^(d / 100),
// so 50% is the geometric middle between X-Plane's own LOD (0%) and the focus views' (100%).
void update_lod_scale() {
    const FocusFrame frame = g_frame.load();
    const double fx = frame.box[0][2];
    const bool on = g_focusLod.load() && frame.focus && fx > 0.0 && fx < 1.0;
    const double finest = std::max(static_cast<double>(frame.rects[0]), frame.rects[2] / std::max(fx, 0.05));
    const double detail = std::clamp(g_objectDetail.load(), 0, 100) / 100.0;
    const auto scale = static_cast<std::uint32_t>(on ? 65536.0 * std::pow(finest, detail) + 0.5 : 65536.0);
    for (LodHeightSite& s : g_lodHeight)
        s.scale = scale;
}

}  // namespace fv

namespace {

// The stub for one LOD width site: the original instructions (which load W into `reg`), then `if (the VR context exists
// and W is its width) W = W x scale >> 16`, counting either case. `reg`: 0 = rcx, 8 = r8 (W zero-extended in both).
std::vector<std::uint8_t> lod_height_stub(const std::uint8_t* original, std::size_t size, int reg, LodHeightSite* site) {
    const bool r8 = reg == 8;
    std::vector<std::uint8_t> c(original, original + size);
    auto bytes = [&c](std::initializer_list<std::uint8_t> b) { c.insert(c.end(), b); };
    auto imm64 = [&c](std::uint64_t v) {
        for (int i = 0; i < 8; ++i)
            c.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    bytes({0x49, 0xBB});  // mov r11, site
    imm64(reinterpret_cast<std::uintptr_t>(site));
    bytes({0x48, 0xB8});  // mov rax, &the VR context pointer
    imm64(simqvfr::module_base() + kVrContextPointer);
    bytes({0x48, 0x8B, 0x00, 0x48, 0x85, 0xC0});  // mov rax, [rax]; test rax, rax
    bytes({0x74, 0x00});                          // jz other
    const std::size_t jzOther = c.size();
    static_assert(kContextWidth == 4, "cmp reg, [rax + 4]");
    if (r8)
        bytes({0x44, 0x3B, 0x40, 0x04});  // cmp r8d, [rax + 4] (the VR context's width)
    else
        bytes({0x3B, 0x48, 0x04});  // cmp ecx, [rax + 4]
    bytes({0x75, 0x00});  // jne other
    const std::size_t jneOther = c.size();
    bytes({0xF0, 0x41, 0xFF, 0x43, 0x04});  // lock inc dword [r11 + 4] (scaled)
    bytes({0x41, 0x8B, 0x03});              // mov eax, [r11] (the scale)
    if (r8)
        bytes({0x4C, 0x0F, 0xAF, 0xC0, 0x49, 0xC1, 0xE8, 0x10});  // imul r8, rax; shr r8, 16
    else
        bytes({0x48, 0x0F, 0xAF, 0xC8, 0x48, 0xC1, 0xE9, 0x10});  // imul rcx, rax; shr rcx, 16
    bytes({0xEB, 0x00});  // jmp done
    const std::size_t jmpDone = c.size();
    const std::size_t other = c.size();
    bytes({0xF0, 0x41, 0xFF, 0x43, 0x08});  // lock inc dword [r11 + 8] (other)
    if (r8)
        bytes({0x45, 0x89, 0x43, 0x0C});  // mov [r11 + 0xc], r8d
    else
        bytes({0x41, 0x89, 0x4B, 0x0C});  // mov [r11 + 0xc], ecx
    const std::size_t done = c.size();
    c[jzOther - 1] = static_cast<std::uint8_t>(other - jzOther);
    c[jneOther - 1] = static_cast<std::uint8_t>(other - jneOther);
    c[jmpDone - 1] = static_cast<std::uint8_t>(done - jmpDone);
    return c;
}
std::atomic<int> g_columnViewports{0};  // final resolve viewports given the whole column (the stub counts)
static_assert(sizeof(std::atomic<int>) == 4, "the stub's lock inc dword");
// The last 64 calls of the single-viewport setter (x, y, w, h), written by the stub: the index, then the entries.
struct ViewportRing {
    volatile std::int32_t next;
    std::int32_t pad;
    volatile std::int32_t entry[64][4];
};
ViewportRing g_viewportRing{};
static_assert(offsetof(ViewportRing, entry) == 8 && sizeof(ViewportRing::entry[0]) == 16, "the stub's ring layout");

std::atomic<int> g_fillLogs{0};
std::atomic<std::uint64_t> g_fourViewDraws{0};  // draws with four views (fill_views calls)

// (left, right, up, down) from a projection's ascending x and y tangents (projection_tangents).
void lrud(const double (&t)[4], float (&out)[4]) {
    out[0] = static_cast<float>(t[0]);
    out[1] = static_cast<float>(t[1]);
    out[2] = static_cast<float>(t[3]);
    out[3] = static_cast<float>(t[2]);
}

// From the per-draw upload (kTransformUpload), with four views, before the transform block is filled from the render
// state: modelview slots 2-3 = slots 0-1 (the focus views look where their eyes look), projection slots 2-3 = the
// eyes' projections, narrowed to the focus box when the focus is on. Doubles, 16 per matrix, 0x80 per slot.
void fill_views(std::uint8_t* state) {
    auto slot = [&](std::size_t base, int i) {
        return reinterpret_cast<double(*)[16]>(state + base + static_cast<std::size_t>(i) * kStateViewStride);
    };
    g_fourViewDraws.fetch_add(1, std::memory_order_relaxed);
    // This frame's boxes (four_views_latch_frame): the same the shaders get pushed.
    const FocusFrame frame = g_frame.load();
    const bool focus = frame.focus != 0;
    if (g_recordWanted.load(std::memory_order_relaxed) && g_recordWanted.exchange(false)) {
        float outer[2][4] = {}, inner[2][4] = {};
        bool ok = true;
        for (int eye = 0; eye < 2; ++eye) {
            double tan[4];
            ok &= projection_tangents(*slot(kStateProjections, eye), tan);
            lrud(tan, outer[eye]);
            // The focus box around its centre (focus_projection's).
            const float* b = frame.box[eye];
            const double hw = focus ? 0.5 * b[2] * (tan[1] - tan[0]) : 0.0, hh = focus ? 0.5 * b[3] * (tan[3] - tan[2]) : 0.0;
            const double cx = b[0], cy = b[1];
            const float box[4] = {static_cast<float>(cx - hw), static_cast<float>(cx + hw), static_cast<float>(cy + hh),
                                  static_cast<float>(cy - hh)};
            if (focus)
                std::memcpy(inner[eye], box, sizeof(box));
            else
                std::memcpy(inner[eye], outer[eye], sizeof(box));
        }
        if (ok) {
            std::lock_guard guard(g_record.mutex);
            std::memcpy(g_record.outer, outer, sizeof(outer));
            std::memcpy(g_record.focus, inner, sizeof(inner));
            g_record.valid = true;
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        std::memcpy(*slot(kStateModelviews, eye + 2), *slot(kStateModelviews, eye), sizeof(double) * 16);
        const double(&eyeProj)[16] = *slot(kStateProjections, eye);
        double(&focusProj)[16] = *slot(kStateProjections, eye + 2);
        if (!focus) {
            std::memcpy(focusProj, eyeProj, sizeof(double) * 16);
            continue;
        }
        const float* b = frame.box[eye];
        double out[16];
        const bool ok = focus_projection(eyeProj, b[2], b[3], b[0], b[1], out);
        std::memcpy(focusProj, out, sizeof(out));
        if (g_fillLogs.load(std::memory_order_relaxed) < 4) {
            ++g_fillLogs;
            bool cm = false;
            double tan[4] = {};
            projection_layout(eyeProj, cm);
            projection_tangents(eyeProj, tan);
            logf("xplane: focus view %d %s: eye tangents x %.3f..%.3f y %.3f..%.3f (%s), focus %.0f%% x %.0f%%", eye + 2,
                 ok ? "built" : "NOT built (not a projection: copy of the eye)", tan[0], tan[1], tan[2], tan[3],
                 cm ? "column-major" : "row-major", b[2] * 100.0, b[3] * 100.0);
        }
    }
}

bool redirect(std::uintptr_t rva, const std::uint8_t* anchor, std::size_t size, const std::uint8_t* stub,
              std::size_t stubSize, const char* what) {
    const bool ok = code_redirect(rva, anchor, size, stub, stubSize);
    logf("xplane: %s at 0x%llx: %s", what, static_cast<unsigned long long>(rva), ok ? "patched" : "FAILED");
    return ok;
}

}  // namespace

bool four_views_copies_start() {
    if (!code_matches(kSetViewports, kSetViewportsAnchor, sizeof(kSetViewportsAnchor)) ||
        !code_matches(kMultiplierStoreA, kMultiplierStoreAnchor, sizeof(kMultiplierStoreAnchor)) ||
        !code_matches(kMultiplierStoreB, kMultiplierStoreAnchor, sizeof(kMultiplierStoreAnchor)) ||
        !code_matches(kModelviewCopyEnd, kModelviewCopyEndAnchor, sizeof(kModelviewCopyEndAnchor)) ||
        !code_matches(kShaderModuleLoad, kShaderModuleLoadAnchor, sizeof(kShaderModuleLoadAnchor)) ||
        !code_matches(kCloudDispatchDepth, kCloudDispatchDepthAnchor, sizeof(kCloudDispatchDepthAnchor)) ||
        !code_matches(kTransformUpload, kTransformUploadAnchor, sizeof(kTransformUploadAnchor))) {
        logf("xplane: four views not started: the draw path code isn't this build's");
        return false;
    }
    // set viewports: a multiplier of 2 (stereo) becomes 4.
    static const std::uint8_t setViewports[] = {
        0x83, 0xFA, 0x02,              // cmp edx, 2
        0x75, 0x05,                    // jne +5
        0xBA, 0x04, 0x00, 0x00, 0x00,  // mov edx, 4
        0x48, 0x89, 0x5C, 0x24, 0x08,  // mov [rsp+8], rbx (the original)
    };
    // The direct stores: 2 becomes 4.
    static const std::uint8_t store[] = {
        0x83, 0xF9, 0x02,                    // cmp ecx, 2
        0x75, 0x05,                          // jne +5
        0xB9, 0x04, 0x00, 0x00, 0x00,        // mov ecx, 4
        0x89, 0x88, 0x90, 0x24, 0x00, 0x00,  // mov [rax+0x2490], ecx (the original)
    };
    // The end of the modelview copy: the render state's modelview and projection slots 0-1 copied into 2-3.
    static const std::uint8_t slotCopy[] = {
        0x83, 0x08, 0x18,                          // or dword [rax], 0x18 (the original)
        0x48, 0x8B, 0x5C, 0x24, 0x30,              // mov rbx, [rsp+0x30] (the original)
        0x48, 0x8D, 0x88, 0x88, 0x00, 0x00, 0x00,  // lea rcx, [rax + 0x88]   (modelview slot 0)
        0xBA, 0x10, 0x00, 0x00, 0x00,              // mov edx, 16             (2 slots of 0x80, 16 bytes a time)
        0x0F, 0x10, 0x01,                          // loop: movups xmm0, [rcx]
        0x0F, 0x11, 0x81, 0x00, 0x01, 0x00, 0x00,  //   movups [rcx + 0x100], xmm0   (modelview slots 2-3)
        0x0F, 0x10, 0x81, 0x00, 0x02, 0x00, 0x00,  //   movups xmm0, [rcx + 0x200]   (projection slots 0-1)
        0x0F, 0x11, 0x81, 0x00, 0x03, 0x00, 0x00,  //   movups [rcx + 0x300], xmm0   (projection slots 2-3)
        0x48, 0x83, 0xC1, 0x10,                    //   add rcx, 16
        0xFF, 0xCA,                                //   dec edx
        0x75, 0xE0,                                //   jnz loop
    };
    static_assert(kStateProjections - kStateModelviews == 0x200 && kStateViewStride * 2 == 0x100,
                  "the slot copy's offsets");
    // vkCreateShaderModule: rax = ours instead of X-Plane's table entry (the call that follows is `call rax`).
    std::uint8_t shader[10] = {0x48, 0xB8};
    const auto hook = reinterpret_cast<std::uintptr_t>(&create_shader_module);
    std::memcpy(shader + 2, &hook, 8);

    bool ok = false;
    if (g_viewRects) {
        // One viewport per view: the pipelines take four (multiViewport; X-Plane makes them with two), and set
        // viewports is detoured (set_viewports) instead of the multiplier stub.
        const bool count = code_matches(kPipelineViewportCount, kPipelineViewportCountAnchor,
                                        sizeof(kPipelineViewportCountAnchor)) &&
                           simqvfr::code_write(kPipelineViewportCount, kPipelineViewportCountFour,
                                               sizeof(kPipelineViewportCountFour));
        logf("xplane: view rects: pipelines with 4 viewports and scissors at 0x%llx: %s",
             static_cast<unsigned long long>(kPipelineViewportCount), count ? "patched" : "FAILED");
        const MH_STATUS init = MH_Initialize();
        void* target = reinterpret_cast<void*>(simqvfr::module_base() + kSetViewports);
        MH_STATUS detour = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED
                             ? MH_CreateHook(target, reinterpret_cast<void*>(&set_viewports),
                                             reinterpret_cast<void**>(&g_setViewportsOriginal))
                             : init;
        if (detour == MH_OK)
            detour = MH_EnableHook(target);
        logf("xplane: view rects: set viewports detoured (multiplier 4, one rect per view) at 0x%llx: %s",
             static_cast<unsigned long long>(kSetViewports), detour == MH_OK ? "patched" : MH_StatusToString(detour));
        // 3D lines: room for two viewports' entries; write the first two (abi.h kLineViewportAssert).
        static const std::uint8_t nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
        std::uint8_t loopEnd[] = {
            0x48, 0x03, 0xD0,                    // add rdx, rax (the original: the entry count)
            0x48, 0x83, 0xFA, 0x02,              // cmp rdx, 2
            0x76, 0x05,                          // jbe +5
            0xBA, 0x02, 0x00, 0x00, 0x00,        // mov edx, 2
            0x4C, 0x3B, 0xC2,                    // cmp r8, rdx (the original)
            0x73, 0x0C,                          // jae done (the loop is over)
            0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, loop top (rax is free there)
            0xFF, 0xE0,                          // jmp rax
        };                                       // done: back after the original jb
        static_assert(sizeof(loopEnd) == 19 + 12, "the jae skips the jump");
        const std::uintptr_t top = simqvfr::module_base() + kLineViewportLoopTop;
        std::memcpy(loopEnd + 21, &top, 8);
        const bool lines =
            code_matches(kLineViewportAssert, kLineViewportAssertAnchor, sizeof(kLineViewportAssertAnchor)) &&
            code_matches(kLineViewportLoopEnd, kLineViewportLoopEndAnchor, sizeof(kLineViewportLoopEndAnchor)) &&
            redirect(kLineViewportLoopEnd, kLineViewportLoopEndAnchor, sizeof(kLineViewportLoopEndAnchor), loopEnd,
                     sizeof(loopEnd), "view rects: 3D lines write the first two viewports' entries") &&
            simqvfr::code_write(kLineViewportAssert, nop6, sizeof(nop6));
        logf("xplane: view rects: 3D lines' viewport assertion (<= 2) at 0x%llx: %s",
             static_cast<unsigned long long>(kLineViewportAssert), lines ? "lifted" : "FAILED");
        ok = count && detour == MH_OK && lines;
    } else {
        ok = redirect(kSetViewports, kSetViewportsAnchor, sizeof(kSetViewportsAnchor), setViewports,
                      sizeof(setViewports), "multiplier 4 in set viewports");
    }
    ok &= redirect(kMultiplierStoreA, kMultiplierStoreAnchor, sizeof(kMultiplierStoreAnchor), store, sizeof(store),
                   "multiplier 4 (g-buffer store A)");
    ok &= redirect(kMultiplierStoreB, kMultiplierStoreAnchor, sizeof(kMultiplierStoreAnchor), store, sizeof(store),
                   "multiplier 4 (g-buffer store B)");
    ok &= redirect(kModelviewCopyEnd, kModelviewCopyEndAnchor, sizeof(kModelviewCopyEndAnchor), slotCopy,
                   sizeof(slotCopy), "render-state slots 2-3 = copies of 0-1");
    ok &= redirect(kShaderModuleLoad, kShaderModuleLoadAnchor, sizeof(kShaderModuleLoadAnchor), shader, sizeof(shader),
                   "vkCreateShaderModule wrapped (final resolve: layers 2-3)");
    // The focus boxes' push constants (g_focusPush): every pipeline layout gets their range, and every bind is
    // followed by a push of this frame's boxes. All three call sites or none; without them the shader edits bake in the
    // launch size and keep the boxes on the eyes' axes. In whether or not the boxes follow the gaze: the size needs them.
    {
        std::uint8_t layoutCall[10] = {0x48, 0xB8};  // mov rax, create_pipeline_layout (the call that follows: call rax)
        const auto layoutHook = reinterpret_cast<std::uintptr_t>(&create_pipeline_layout);
        std::memcpy(layoutCall + 2, &layoutHook, 8);
        std::uint8_t bindCall[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0};  // mov rax, bind_pipeline; call rax
        const auto bindHook = reinterpret_cast<std::uintptr_t>(&bind_pipeline);
        std::memcpy(bindCall + 2, &bindHook, 8);
        const bool sites =
            code_matches(kPipelineLayoutLoad, kPipelineLayoutLoadAnchor, sizeof(kPipelineLayoutLoadAnchor)) &&
            code_matches(kBindPipelineCompute, kBindPipelineComputeAnchor, sizeof(kBindPipelineComputeAnchor)) &&
            code_matches(kBindPipelineGraphics, kBindPipelineGraphicsAnchor, sizeof(kBindPipelineGraphicsAnchor));
        const bool hooked =
            sites &&
            redirect(kPipelineLayoutLoad, kPipelineLayoutLoadAnchor, sizeof(kPipelineLayoutLoadAnchor), layoutCall,
                     sizeof(layoutCall), "focus boxes: vkCreatePipelineLayout wrapped (their push constant range)") &&
            redirect(kBindPipelineCompute, kBindPipelineComputeAnchor, sizeof(kBindPipelineComputeAnchor), bindCall,
                     sizeof(bindCall), "focus boxes: compute vkCmdBindPipeline wrapped (then the push)") &&
            redirect(kBindPipelineGraphics, kBindPipelineGraphicsAnchor, sizeof(kBindPipelineGraphicsAnchor), bindCall,
                     sizeof(bindCall), "focus boxes: graphics vkCmdBindPipeline wrapped (then the push)");
        g_focusPush = hooked;
        logf("xplane: focus boxes %s", hooked ? (g_gazeWanted.load()
                                                     ? "pushed to the shaders each frame: size live, centres on the gaze"
                                                     : "pushed to the shaders each frame: size live, centres on the eyes' axes")
                                              : "baked into the shaders (launch size, eyes' axes): the pipeline calls "
                                                "aren't this build's");
    }
    // The per-draw upload: with four views, fill_views (slots 2-3 from 0-1: copies, or focus projections) right before
    // the block is filled, whichever setter wrote the slots. The function keeps rsp 16-aligned in its body (5 pushes
    // and sub rsp, 0x830 from entry); xmm0-xmm2 are live there, so the stub saves xmm0-xmm5 and the volatile registers.
    std::uint8_t uploadCall[] = {
        0x41, 0x83, 0xFE, 0x04,                          // cmp r14d, 4
        0x75, 0x6F,                                      // jne done
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51,        // push rax, rcx, rdx, r8, r9
        0x41, 0x52, 0x41, 0x53,                          // push r10, r11
        0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,        // sub rsp, 0x88  (7 pushes + 0x88: aligned for the call)
        0x0F, 0x11, 0x44, 0x24, 0x20,                    // movups [rsp+0x20], xmm0
        0x0F, 0x11, 0x4C, 0x24, 0x30,                    // movups [rsp+0x30], xmm1
        0x0F, 0x11, 0x54, 0x24, 0x40,                    // movups [rsp+0x40], xmm2
        0x0F, 0x11, 0x5C, 0x24, 0x50,                    // movups [rsp+0x50], xmm3
        0x0F, 0x11, 0x64, 0x24, 0x60,                    // movups [rsp+0x60], xmm4
        0x0F, 0x11, 0x6C, 0x24, 0x70,                    // movups [rsp+0x70], xmm5
        0x48, 0x89, 0xF9,                                // mov rcx, rdi (the render state)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,              // mov rax, fill_views
        0xFF, 0xD0,                                      // call rax
        0x0F, 0x10, 0x44, 0x24, 0x20,                    // movups xmm0, [rsp+0x20]
        0x0F, 0x10, 0x4C, 0x24, 0x30,                    // movups xmm1, [rsp+0x30]
        0x0F, 0x10, 0x54, 0x24, 0x40,                    // movups xmm2, [rsp+0x40]
        0x0F, 0x10, 0x5C, 0x24, 0x50,                    // movups xmm3, [rsp+0x50]
        0x0F, 0x10, 0x64, 0x24, 0x60,                    // movups xmm4, [rsp+0x60]
        0x0F, 0x10, 0x6C, 0x24, 0x70,                    // movups xmm5, [rsp+0x70]
        0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,        // add rsp, 0x88
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,  // pop r11, r10, r9, r8
        0x5A, 0x59, 0x58,                                // pop rdx, rcx, rax
        0x8B, 0x87, 0x94, 0x24, 0x00, 0x00,              // done: mov eax, [rdi + 0x2494] (the original)
    };
    static_assert(sizeof(uploadCall) == 6 + 0x6F + 6, "the jne skips the call block");
    const auto fill = reinterpret_cast<std::uintptr_t>(&fill_views);
    std::memcpy(uploadCall + 0x3B, &fill, 8);
    ok &= redirect(kTransformUpload, kTransformUploadAnchor, sizeof(kTransformUploadAnchor), uploadCall,
                   sizeof(uploadCall), "per-draw upload: slots 2-3 filled (copies or focus)");
    // The other multiplier writes: a 2 stored becomes 4 (the register itself is left as it is).
    for (const MultiplierWrite& w : kMoreMultiplierWrites) {
        const std::uint8_t stub[] = {
            0x83, static_cast<std::uint8_t>(0xF8 | w.reg), 0x02,          // cmp reg32, 2
            0x75, 0x0C,                                                   // jne original
            0xC7, static_cast<std::uint8_t>(0x80 | w.base), 0x90, 0x24,   // mov dword [base + 0x2490], 4
            0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
            0xEB, 0x06,                                                   // jmp done
            w.anchor[0], w.anchor[1], w.anchor[2], w.anchor[3], w.anchor[4], w.anchor[5],  // original: the store
        };                                                                // done
        static_assert(sizeof(stub) == 23, "the jumps' offsets");
        char what[96];
        std::snprintf(what, sizeof(what), "multiplier 4 (restore or draw-path store) at 0x%llx",
                      static_cast<unsigned long long>(w.rva));
        ok &= redirect(w.rva, w.anchor, sizeof(w.anchor), stub, sizeof(stub), what);
    }
    // The vegetation cull's u_render_layers (abi.h kVegetationRenderLayers): stereo ? 4 : 1 instead of stereo + 1, so
    // the cull writes four instances per tree (mesh trees: visible x 4, the VS taking point i / 4 and layer i % 4) and
    // four billboard instances (layer i % 4). With 2 the mesh trees drew only the first half of the visible points
    // (popping) and the billboards only layers 0-1 (none in the focus views). Each stereo cull is counted by the
    // setup's own view count (rcx, rax free here: the next instructions load ecx and eax; r9, rbx, rdi, esi = 0 kept),
    // and its u_lod_bias (the uniform's +0x19c, written just before) kept for the log.
    std::uint8_t renderLayers[] = {
        0x0F, 0xB6, 0x87, 0xC8, 0x01, 0x00, 0x00,  // movzx eax, byte [rdi + 0x1c8]  (the stereo flag)
        0x85, 0xC0,                                // test eax, eax
        0x74, 0x38,                                // jz one
        0x8B, 0x8B, 0x9C, 0x01, 0x00, 0x00,        // mov ecx, [rbx + 0x19c]  (the setup's view count)
        0x83, 0xF9, 0x04,                          // cmp ecx, 4
        0x76, 0x05,                                // jbe +5
        0xB9, 0x04, 0x00, 0x00, 0x00,              // mov ecx, 4
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // mov rax, g_vegetationCulls
        0xF0, 0xFF, 0x04, 0x88,                    // lock inc dword [rax + rcx*4]
        0x41, 0x8B, 0x89, 0x9C, 0x01, 0x00, 0x00,  // mov ecx, [r9 + 0x19c]  (u_lod_bias)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // mov rax, g_vegetationLodBias
        0x89, 0x08,                                // mov [rax], ecx
        0xB8, 0x04, 0x00, 0x00, 0x00,              // mov eax, 4
        0xEB, 0x05,                                // jmp store
        0xB8, 0x01, 0x00, 0x00, 0x00,              // one: mov eax, 1
        0x41, 0x89, 0x81, 0xB8, 0x01, 0x00, 0x00,  // store: mov [r9 + 0x1b8], eax (the original)
    };
    static_assert(sizeof(renderLayers) == 11 + 0x38 + 5 + 7, "the jz lands on one, the jmp on store");
    static_assert(kCullSetupViewCount == 0x19c && sizeof(std::atomic<int>) == 4, "the stub's offsets and counters");
    const auto culls = reinterpret_cast<std::uintptr_t>(&g_vegetationCulls[0]);
    const auto lodBias = reinterpret_cast<std::uintptr_t>(&g_vegetationLodBias);
    std::memcpy(renderLayers + 29, &culls, 8);
    std::memcpy(renderLayers + 50, &lodBias, 8);
    ok &= redirect(kVegetationRenderLayers, kVegetationRenderLayersAnchor, sizeof(kVegetationRenderLayersAnchor),
                   renderLayers, sizeof(renderLayers), "vegetation cull: u_render_layers 4 in stereo (was 2)");
    // The cull frustum (abi.h kStereoFrustumTest): with the multiplier 4 X-Plane culled against the left eye's frustum
    // alone, so the right eye's outer strip (its tangents 1.213 to 1.402) lost objects and lights, which popped in and
    // out there. 2 or 4 take the two-eye path; others go on as before. rax is free here (the function's next write is
    // `lea rax`, and the other path starts with compares and calls).
    std::uint8_t frustum[] = {
        0x83, 0xFE, 0x02,                    // cmp esi, 2
        0x74, 0x11,                          // je two-eye path (the stub's end: back to the original's next instruction)
        0x83, 0xFE, 0x04,                    // cmp esi, 4
        0x74, 0x0C,                          // je two-eye path
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, the other multipliers' path
        0xFF, 0xE0,                          // jmp rax
    };
    static_assert(sizeof(frustum) == 22, "the je offsets land on the stub's end");
    const std::uintptr_t other = simqvfr::module_base() + kStereoFrustumOther;
    std::memcpy(frustum + 12, &other, 8);
    ok &= redirect(kStereoFrustumTest, kStereoFrustumTestAnchor, sizeof(kStereoFrustumTestAnchor), frustum,
                   sizeof(frustum), "cull frustum: both eyes' with 4 views (was the left eye's alone)");
    // Stereo as `count == 2` (abi.h kStereoCountJumps): 2 or 4 run the eyes' work; otherwise the original jne's target,
    // through `jmp [rip]` (no register used). The flags are not read after the jne.
    for (const StereoCountJump& s : kStereoCountJumps) {
        std::uint8_t stub[] = {
            s.anchor[0], s.anchor[1], 0x02,      // cmp X, 2 (the original)
            0x74, 0x13,                          // je: the eyes' work (the stub's end, back after the jne)
            s.anchor[0], s.anchor[1], 0x04,      // cmp X, 4
            0x74, 0x0E,                          // je: the eyes' work
            0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,  // jmp [rip]: the original jne's target
            0, 0, 0, 0, 0, 0, 0, 0,
        };
        static_assert(sizeof(stub) == 24, "the je offsets land on the stub's end");
        const std::uintptr_t target = simqvfr::module_base() + s.target;
        std::memcpy(stub + 16, &target, 8);
        char what[96];
        std::snprintf(what, sizeof(what), "stereo test (count == 2) takes 4 at 0x%llx", static_cast<unsigned long long>(s.rva));
        ok &= redirect(s.rva, s.anchor, sizeof(s.anchor), stub, sizeof(stub), what);
    }
    // The SSR fill's count (abi.h kSsrFillCount) and the reprojection globals' (kReprojectionCount): 4 becomes 2, so
    // the stereo flag is set and the right eye's matrices are written (they were left stale: garbage in u_ssr_data[1]).
    static const std::uint8_t ssrCount[] = {
        0x83, 0xF8, 0x04,                    // cmp eax, 4
        0x75, 0x05,                          // jne +5
        0xB8, 0x02, 0x00, 0x00, 0x00,        // mov eax, 2
        0x89, 0x85, 0x18, 0x05, 0x00, 0x00,  // mov [rbp+0x518], eax (the original)
    };
    ok &= redirect(kSsrFillCount, kSsrFillCountAnchor, sizeof(kSsrFillCountAnchor), ssrCount, sizeof(ssrCount),
                   "SSR fill: the eyes' stereo count with 4 views (right eye's u_proj/u_reproj written)");
    static const std::uint8_t reprojectionCount[] = {
        0x83, 0xFB, 0x04,              // cmp ebx, 4
        0x75, 0x05,                    // jne +5
        0xBB, 0x02, 0x00, 0x00, 0x00,  // mov ebx, 2
        0x83, 0xFB, 0x02,              // cmp ebx, 2 (the original)
        0x40, 0x0F, 0x94, 0xC5,        // sete bpl (the original)
    };
    ok &= redirect(kReprojectionCount, kReprojectionCountAnchor, sizeof(kReprojectionCountAnchor), reprojectionCount,
                   sizeof(reprojectionCount), "reprojection globals: the eyes' stereo count with 4 views (right eye's written)");
    // The object and tree LOD's width (abi.h kWorldLodHeight, kGraphLodHeight): the VR world graph's W becomes the
    // eye's width at the focus views' pixel density (update_lod_scale), so the focus views' trees and objects aren't
    // the coarse eye's LOD magnified. The stubs always go in; scale 1 leaves W as it is.
    update_lod_scale();
    const std::vector<std::uint8_t> worldLod =
        lod_height_stub(kWorldLodHeightAnchor, sizeof(kWorldLodHeightAnchor), 8, &g_lodHeight[0]);
    ok &= redirect(kWorldLodHeight, kWorldLodHeightAnchor, sizeof(kWorldLodHeightAnchor), worldLod.data(), worldLod.size(),
                   "world pass LOD width (objects, the vegetation cull's u_lod_bias): the focus views' density");
    const std::vector<std::uint8_t> graphLod =
        lod_height_stub(kGraphLodHeightAnchor, sizeof(kGraphLodHeightAnchor), 0, &g_lodHeight[1]);
    ok &= redirect(kGraphLodHeight, kGraphLodHeightAnchor, sizeof(kGraphLodHeightAnchor), graphLod.data(), graphLod.size(),
                   "graph LOD width (REN_graph_info+0x1e8, the other object draws): the focus views' density");
    const bool clouds = simqvfr::code_write(kCloudDispatchDepth, kCloudDispatchDepthCount, sizeof(kCloudDispatchDepthCount));
    logf("xplane: cloud render dispatch depth = view count (was stereo ? 2 : 1) at 0x%llx: %s",
         static_cast<unsigned long long>(kCloudDispatchDepth), clouds ? "patched" : "FAILED");
    ok &= clouds;
    return ok;
}

void four_views_set_view_rects(bool on) {
    g_viewRects = on;
    logf("xplane: view rects %s (one viewport per view)", on ? "on" : "off");
}

int four_views_view_rect_sets() { return g_viewRectSets.load(); }

bool four_views_eye_columns_start() {
    if (!code_matches(kRenderSizeHeight, kRenderSizeHeightAnchor, sizeof(kRenderSizeHeightAnchor)) ||
        !code_matches(kWorldSubgraphHeight, kWorldSubgraphHeightAnchor, sizeof(kWorldSubgraphHeightAnchor)) ||
        !code_matches(kVrContextHeight, kVrContextHeightAnchor, sizeof(kVrContextHeightAnchor)) ||
        !code_matches(kVrContextResizeHeightA, kVrContextResizeHeightAAnchor, sizeof(kVrContextResizeHeightAAnchor)) ||
        !code_matches(kVrContextResizeHeightB, kVrContextResizeHeightBAnchor, sizeof(kVrContextResizeHeightBAnchor))) {
        logf("xplane: eye columns not started: the view size code isn't this build's (or the VR context isn't at 4 layers)");
        return false;
    }
    static const std::uint8_t renderSize[] = {0x41, 0x8B, 0x8F, 0x88, 0x00, 0x00, 0x00,  // mov ecx, [r15+0x88]
                                              0xD1, 0xE9};                               // shr ecx, 1
    static const std::uint8_t subgraph[] = {0x8B, 0xB8, 0x88, 0x00, 0x00, 0x00,  // mov edi, [rax+0x88]
                                            0xD1, 0xEF};                         // shr edi, 1
    static const std::uint8_t context[] = {0x89, 0xD8,                    // mov eax, ebx
                                           0xD1, 0xE8,                    // shr eax, 1
                                           0x89, 0x44, 0x24, 0x20,        // mov [rsp+0x20], eax
                                           0x4C, 0x8D, 0x45, 0x10};       // lea r8, [rbp+0x10]
    static const std::uint8_t resizeA[] = {0x41, 0xB9, 0x04, 0x00, 0x00, 0x00,  // mov r9d, 4
                                           0x44, 0x8B, 0xC3,                    // mov r8d, ebx
                                           0x41, 0xD1, 0xE8};                   // shr r8d, 1
    static const std::uint8_t resizeB[] = {0x44, 0x8B, 0xC0,  // mov r8d, eax
                                           0x41, 0xD1, 0xE8,  // shr r8d, 1
                                           0x8B, 0xC3};       // mov eax, ebx
    bool ok = redirect(kRenderSizeHeight, kRenderSizeHeightAnchor, sizeof(kRenderSizeHeightAnchor), renderSize,
                       sizeof(renderSize), "eye columns: render size height / 2");
    ok &= redirect(kWorldSubgraphHeight, kWorldSubgraphHeightAnchor, sizeof(kWorldSubgraphHeightAnchor), subgraph,
                   sizeof(subgraph), "eye columns: 3D world graph height / 2");
    ok &= redirect(kVrContextHeight, kVrContextHeightAnchor, sizeof(kVrContextHeightAnchor), context, sizeof(context),
                   "eye columns: VR context height / 2");
    ok &= redirect(kVrContextResizeHeightA, kVrContextResizeHeightAAnchor, sizeof(kVrContextResizeHeightAAnchor), resizeA,
                   sizeof(resizeA), "eye columns: VR context resize A height / 2");
    ok &= redirect(kVrContextResizeHeightB, kVrContextResizeHeightBAnchor, sizeof(kVrContextResizeHeightBAnchor), resizeB,
                   sizeof(resizeB), "eye columns: VR context resize B height / 2");
    // The final resolve's viewport (0, 0, max(2 x context width, desktop window width), context height) gets the whole
    // column: twice the height.
    std::uint8_t viewport[] = {
        // Recorded first (diagnostics, four_views_viewport_log): (x, y, w, h) into a ring of 64.
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, &g_viewportRing
        0x41, 0xBA, 0x01, 0x00, 0x00, 0x00,  // mov r10d, 1
        0xF0, 0x44, 0x0F, 0xC1, 0x10,        // lock xadd [rax], r10d
        0x41, 0x83, 0xE2, 0x3F,              // and r10d, 63
        0x41, 0xC1, 0xE2, 0x04,              // shl r10d, 4
        0x4A, 0x8D, 0x44, 0x10, 0x08,        // lea rax, [rax + r10 + 8]
        0x89, 0x08,                          // mov [rax], ecx
        0x89, 0x50, 0x04,                    // mov [rax+4], edx
        0x44, 0x89, 0x40, 0x08,              // mov [rax+8], r8d
        0x44, 0x89, 0x48, 0x0C,              // mov [rax+12], r9d
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // 0:  mov rax, &the VR context pointer
        0x48, 0x8B, 0x00,                    // 10: mov rax, [rax]
        0x48, 0x85, 0xC0,                    // 13: test rax, rax
        0x74, 0x2A,                          // 16: jz done
        0x85, 0xC9,                          // 18: test ecx, ecx       (x = 0)
        0x75, 0x26,                          // 20: jnz done
        0x85, 0xD2,                          // 22: test edx, edx       (y = 0)
        0x75, 0x22,                          // 24: jnz done
        0x44, 0x3B, 0x48, 0x08,              // 26: cmp r9d, [rax+8]    (h = context height)
        0x75, 0x1C,                          // 30: jne done
        0x44, 0x8B, 0x50, 0x04,              // 32: mov r10d, [rax+4]
        0x45, 0x01, 0xD2,                    // 36: add r10d, r10d
        0x45, 0x39, 0xD0,                    // 39: cmp r8d, r10d       (w >= 2 x context width: the resolve's
        0x72, 0x10,                          // 42: jb done              target is the desktop window's width when wider)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // 44: mov rax, &g_columnViewports
        0xF0, 0xFF, 0x00,                    // 54: lock inc dword [rax]
        0x45, 0x01, 0xC9,                    // 57: add r9d, r9d
        0x48, 0x89, 0x5C, 0x24, 0x08,        // 60: done: mov [rsp+8], rbx (the original)
    };
    constexpr std::size_t kRecord = 47;  // the recording prefix; the check's offsets (comments) start after it
    static_assert(sizeof(viewport) == kRecord + 65, "the jumps land on done (60)");
    const std::uintptr_t contextPointer = simqvfr::module_base() + kVrContextPointer;
    const auto counter = reinterpret_cast<std::uintptr_t>(&g_columnViewports);
    const auto ring = reinterpret_cast<std::uintptr_t>(&g_viewportRing);
    std::memcpy(viewport + 2, &ring, 8);
    std::memcpy(viewport + kRecord + 2, &contextPointer, 8);
    std::memcpy(viewport + kRecord + 46, &counter, 8);
    static_assert(kContextWidth == 4 && kContextHeight == 8, "the stub's offsets");
    if (!code_matches(kSetViewport, kSetViewportAnchor, sizeof(kSetViewportAnchor))) {
        logf("xplane: eye columns: the viewport setter at 0x%llx isn't this build's", static_cast<unsigned long long>(kSetViewport));
        return false;
    }
    ok &= redirect(kSetViewport, kSetViewportAnchor, sizeof(kSetViewportAnchor), viewport, sizeof(viewport),
                   "eye columns: the final resolve's viewport the whole column");
    return ok;
}

int four_views_column_viewports() { return g_columnViewports.load(); }

void four_views_vegetation_log() {
    int c[5];
    for (int i = 0; i < 5; ++i)
        c[i] = g_vegetationCulls[i].load();
    logf("xplane: stereo vegetation culls given u_render_layers 4: %d; the setup's view count (max(multiplier, "
         "viewports)) was 2 in %d, 4+ in %d, other in %d",
         c[0] + c[1] + c[2] + c[3] + c[4], c[2], c[4], c[0] + c[1] + c[3]);
    const std::uint32_t bits = g_vegetationLodBias.load();
    float bias = 0.0f;
    std::memcpy(&bias, &bits, sizeof(bias));
    const LodHeightSite& w = g_lodHeight[0];
    const LodHeightSite& g = g_lodHeight[1];
    logf("xplane: LOD width x%.3f (%s, object detail %d%%): world pass %u scaled, %u kept (last kept W %d); graph setup %u scaled, %u kept "
         "(last kept W %d); the last stereo vegetation cull's u_lod_bias %.5f",
         w.scale.load() / 65536.0, g_focusLod.load() ? "the focus views' density" : "off", g_objectDetail.load(),
         w.scaled.load(), w.other.load(),
         w.lastOther.load(), g.scaled.load(), g.other.load(), g.lastOther.load(), bias);
}

void four_views_set_focus_lod(bool on) {
    g_focusLod = on;
    update_lod_scale();
    logf("xplane: object and tree LOD %s", on ? "for the focus views' pixel density (all four views)"
                                              : "X-Plane's own (the eye view's density)");
}

void four_views_set_object_detail(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (g_objectDetail.exchange(percent) == percent)
        return;
    update_lod_scale();
    logf("xplane: object detail %d%%: LOD width x%.3f (from the next frame)", percent, g_lodHeight[0].scale.load() / 65536.0);
}

std::uint64_t four_views_draws() { return g_fourViewDraws.load(std::memory_order_relaxed); }

void four_views_viewport_log() {
    std::int32_t seen[64][4];
    int count = 0;
    for (int i = 0; i < 64; ++i) {
        std::int32_t v[4];
        for (int k = 0; k < 4; ++k)
            v[k] = g_viewportRing.entry[i][k];
        bool dup = v[2] == 0 && v[3] == 0;
        for (int j = 0; j < count && !dup; ++j)
            dup = std::memcmp(seen[j], v, sizeof(v)) == 0;
        if (!dup)
            std::memcpy(seen[count++], v, sizeof(v));
    }
    int cw = 0, ch = 0, cl = 0;
    if (const auto* ctx = *reinterpret_cast<std::uint8_t* const*>(simqvfr::module_base() + kVrContextPointer)) {
        cw = *reinterpret_cast<const std::int32_t*>(ctx + kContextWidth);
        ch = *reinterpret_cast<const std::int32_t*>(ctx + kContextHeight);
        cl = *reinterpret_cast<const std::int32_t*>(ctx + 0xc);
    }
    std::string list;
    for (int j = 0; j < count; ++j) {
        char item[64];
        std::snprintf(item, sizeof(item), "%s(%d,%d %dx%d)", j ? " " : "", seen[j][0], seen[j][1], seen[j][2], seen[j][3]);
        list += item;
    }
    logf("xplane: viewports set lately (x,y wxh; %d distinct of the last 64): %s; VR context %dx%dx%d; whole-column "
         "grants %d",
         count, list.c_str(), cw, ch, cl, g_columnViewports.load());
}

bool four_views_tangents(float (&outer)[2][4], float (&focus)[2][4]) {
    g_recordWanted = true;
    std::lock_guard guard(g_record.mutex);
    if (!g_record.valid)
        return false;
    std::memcpy(outer, g_record.outer, sizeof(outer));
    std::memcpy(focus, g_record.focus, sizeof(focus));
    return true;
}

}  // namespace xplane

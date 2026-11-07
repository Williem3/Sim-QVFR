#include "four_views.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "../../engine/code_patch.h"
#include "MinHook.h"
#include "../../log.h"
#include "abi.h"
#include "focus_projection.h"
#include "spirv_patch.h"

namespace xplane {
namespace {

using simqvfr::code_matches;
using simqvfr::code_redirect;
using simqvfr::logf;

// VkShaderModuleCreateInfo, as far as needed: sType, pNext, flags, codeSize, pCode (x64 layout).
struct ShaderModuleCreateInfo {
    std::uint32_t sType;
    const void* pNext;
    std::uint32_t flags;
    std::size_t codeSize;
    const std::uint32_t* pCode;
};
using CreateShaderModule = std::int32_t (*)(void* device, const ShaderModuleCreateInfo* info, const void* allocator,
                                           std::uint64_t* module);

std::atomic<int> g_finalResolveEdits{0};
std::atomic<int> g_eyeArrayModules{0};
std::atomic<int> g_layerFromZModules{0};
std::atomic<int> g_inverseModules{0};
std::atomic<bool> g_eyeColumns{false};  // four_views_set_eye_columns
std::atomic<double> g_focusFraction{0.2}, g_focusFractionY{0.2};  // four_views_set_focus
std::atomic<bool> g_viewRects{false};   // four_views_set_view_rects: one viewport per view
std::atomic<bool> g_bloomFromEye{false};  // four_views_set_bloom_from_eye
std::atomic<int> g_eyeMapModules{0}, g_bloomEdits{0}, g_coefficientModules{0}, g_parityModules{0}, g_ssrModules{0};
std::atomic<bool> g_peripheryAlphaLod{false};  // four_views_set_periphery_alpha_lod
std::atomic<bool> g_ssrFromEye{true};          // four_views_set_ssr_from_eye
std::atomic<bool> g_aoEdgesFromEye{true};      // four_views_set_ao_edges_from_eye
std::atomic<int> g_aoEdgeModules{0};
// The share of a focus view, from each edge, over which its AO goes from its eye's to its own: CACAO's edge error died
// out 160 rows (12%) in (capture XPlane_frame2060, docs/recon-xplane.md "Measured: a dark rim at the top of the focus
// views").
constexpr float kAoEdgeBand = 0.12f;
std::atomic<int> g_layerOutModules{0}, g_alphaLodModules{0}, g_tileModules{0};
std::atomic<bool> g_focusFullRate{false};  // four_views_set_focus_full_rate
// Per-view sizes (four_views_set_view_sizes): each kind of view's rect, a share of its layers (x, y; 1: the whole
// layer), the outer views' then the focus views'. Taken into the latched frame.
struct ViewShares {
    std::atomic<bool> on{false};
    std::atomic<double> share[2][2] = {{1.0, 1.0}, {1.0, 1.0}};
};
ViewShares g_viewShares;
std::atomic<int> g_viewRectSets{0};     // set viewports calls given four rects
std::atomic<int> g_splitOffModules{0};  // blit shaders whose side-by-side split is off in four-view passes
std::atomic<int> g_histogramModules{0};  // auto exposure histograms bounded by the outer view's rect
std::atomic<int> g_ssrRectModules{0};    // SSR shaders reading each view's rect
std::atomic<int> g_viewportSizeModules{0};  // shaders reading u_viewport_size as the layer (u_gbuffer_size)
std::atomic<int> g_hizRectModules{0};       // depth-chain reductions clamped to each view's rect
// Diagnostics (four_views_set_diag_edits, four_views_set_diagnostics): the switch edits in, and which outputs are off.
std::atomic<bool> g_diagEdits{false}, g_diagSsrOff{false}, g_diagAoOff{false}, g_diagShadowOff{false};
std::atomic<int> g_diagModules{0};

// set viewports(count, multiplier, rects {x, y, w, h} x count) (abi.h kSetViewports). Stereo (multiplier 2) becomes
// four views; with view rects, a four-view pass's one rect becomes one per view, each view's scaled by its share of
// the layer (the latched frame's) from the rect's origin, which is the layer's top-left in memory: X-Plane flips
// within each viewport (docs/recon-xplane.md "Per-view sizes, stage 2").
using SetViewports = void (*)(int count, int multiplier, const std::int32_t* rects);
SetViewports g_setViewportsOriginal = nullptr;
void latched_rects(float (&rects)[4]);  // this frame's (the latched frame, below)
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
// The focus views (four_views_set_focus): off = slots 2-3 copy the eyes (the copies stage).
std::atomic<bool> g_focus{false};

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
LatchedFrame g_frame;
void latched_rects(float (&rects)[4]) { std::memcpy(rects, g_frame.load().rects, sizeof(rects)); }
void latch_if_no_frame();  // a setting before the first frame's latch takes effect at once (below)
void store_view_shares(bool on, const double (&share)[2][2]);  // under the latch's lock (below)

// The LOD width: the eye's width at the finest view's pixel density, so every view's objects and trees take the LOD the
// finest needs (the cull and its LOD are shared by all four views). W is the layer's width; a focus view spans its share
// of it over the box's share of the eye (s_focus W / box), the eye view its own share (s_outer W). From the latched
// frame's size and rects.
void update_lod_scale() {
    const FocusFrame frame = g_frame.load();
    const double fx = frame.box[0][2];
    const bool on = g_focusLod.load() && frame.focus && fx > 0.0 && fx < 1.0;
    const double finest = std::max(static_cast<double>(frame.rects[0]), frame.rects[2] / std::max(fx, 0.05));
    const auto scale = static_cast<std::uint32_t>(on ? 65536.0 * finest + 0.5 : 65536.0);
    for (LodHeightSite& s : g_lodHeight)
        s.scale = scale;
}

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

// The uniform blocks with per-eye arrays of two (docs/recon-xplane.md, "Inventory"), by their variables' names.
const std::vector<std::string> kEyeArrayBlocks = {
    "u_misc_data_var",          "u_new_sky_data_var",     "u_gbuffer_data_var",       "u_dome_data_var",
    "u_particle_data_var",      "u_ocean_shading_data_var", "u_line3d_data_var",      "u_ocean_meta_data_var",
    "u_ssr_data_var",           "u_ogl_light_data_var",   "u_cloud_upscale_data_var", "u_volumetric_apply_data_var",
    "u_rain_draw_info_var",
};

std::string sha256_hex(const void* data, std::size_t size) {
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32];
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
        static const char* digits = "0123456789abcdef";
        for (UCHAR b : digest) {
            hex += digits[b >> 4];
            hex += digits[b & 15];
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

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

// Diagnostics: a final resolve variant not in abi.h kFinalResolveFs (a shader using u_y_offset that the eye-columns
// edit would apply to) stays unedited and breaks the image, as the MSAA variant did (frame saves 9716-9915,
// 2026-11-06): each is saved once to <logs>\shaders and logged.
void save_resolve_family(const ShaderModuleCreateInfo* info) {
    static const char kName[] = "u_y_offset";
    const auto* bytes = reinterpret_cast<const char*>(info->pCode);
    if (std::search(bytes, bytes + info->codeSize, kName, kName + sizeof(kName) - 1) == bytes + info->codeSize)
        return;
    std::vector<std::uint32_t> words(info->pCode, info->pCode + info->codeSize / 4);
    if (!spirv_eye_columns_resolve(words, nullptr))
        return;
    const std::string sha = sha256_hex(info->pCode, info->codeSize);
    static std::mutex mutex;
    static std::set<std::string> saved;
    std::lock_guard guard(mutex);
    if (!saved.insert(sha).second)
        return;
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"shaders";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / ("resolve-family-" + std::to_string(info->codeSize) + "-" + sha.substr(0, 12) + ".spv");
    std::ofstream out(file, std::ios::binary);
    out.write(bytes, static_cast<std::streamsize>(info->codeSize));
    logf("xplane: an UNKNOWN final resolve variant (left unedited: the image breaks if X-Plane uses it): %zu bytes, sha256 "
         "%s, saved to %ls (%s)",
         info->codeSize, sha.c_str(), file.c_str(), out ? "written" : "NOT written");
}

// One of the final resolve's variants (abi.h kFinalResolveFs): X-Plane makes them all when a flight loads and uses one
// by its settings (FXAA with or without the stencil mask, MSAA; docs/recon-xplane.md "The final resolve's variants").
bool final_resolve(const ShaderModuleCreateInfo* info) {
    if (!info || !info->pCode)
        return false;
    for (const ShaderHash& h : kFinalResolveFs)
        if (info->codeSize == h.bytes)
            return sha256_hex(info->pCode, info->codeSize) == h.sha256;
    return false;
}

// X-Plane's vkCreateShaderModule call lands here (kShaderModuleLoad). The final resolve's fragment shader gets
// v_eye + 2 (it then shows layers 2-3); every other module goes through as it is.
std::int32_t create_shader_module(void* device, const ShaderModuleCreateInfo* info, const void* allocator,
                                  std::uint64_t* module) {
    const auto real = *reinterpret_cast<CreateShaderModule*>(simqvfr::module_base() + kVkCreateShaderModuleSlot);
    if (final_resolve(info)) {
        std::vector<std::uint32_t> words(info->pCode, info->pCode + info->codeSize / 4);
        std::string why;
        const bool columns = g_eyeColumns.load();
        if (columns ? spirv_eye_columns_resolve(words, &why) : spirv_add_to_input(words, "v_eye", 2, &why)) {
            // Per-view rects (docs/recon-xplane.md "Measured: stage 2's rects"): each view's samples come from its
            // rect, so it fills its part of the column (the composition reads whole parts).
            int rectSamples = 0;
            std::string rectWhy;
            if (columns && g_focusPush.load() && g_viewRects.load())
                rectSamples = spirv_sample_view_rects(words, &rectWhy);
            ShaderModuleCreateInfo edited = *info;
            edited.codeSize = words.size() * 4;
            edited.pCode = words.data();
            const std::int32_t result = real(device, &edited, allocator, module);
            logf("xplane: final resolve shader (%zu bytes) edited (%s; view rects: %s), vkCreateShaderModule %d, edit %d",
                 info->codeSize, columns ? "eye columns: each eye's view over its focus view" : "v_eye + 2: layers 2-3",
                 rectSamples > 0 ? (std::to_string(rectSamples) + " samples from each view's rect").c_str()
                                 : (columns && g_focusPush.load() && g_viewRects.load() ? ("NOT: " + rectWhy).c_str()
                                                                                        : "off"),
                 result, ++g_finalResolveEdits);
            return result;
        }
        logf("xplane: final resolve shader NOT edited: %s", why.c_str());
    } else if (info && info->pCode && info->codeSize >= 20) {
        save_resolve_family(info);
    }
    // Every shader reading X-Plane's per-eye arrays: views 2-3 read their eye's entry (index & 1).
    if (info && info->pCode && info->codeSize >= 20) {
        std::vector<std::uint32_t> words(info->pCode, info->pCode + info->codeSize / 4);
        // The light billboards' size: projected and un-projected with the eye's pair (docs/recon-xplane.md, "Measured:
        // focus views darker"), so a light covers the same angle in a focus view as in its eye. The fold then takes the
        // inverse's index & 1.
        const int inverted = spirv_eye_projection_beside(words, "u_ogl_light_data", "u_proj_inv",
                                                         "u_transform_block_fat", "projection_matrix");
        if (inverted > 0)
            logf("xplane: light size from the eye's projection in shader module (%d accesses), %d so far", inverted,
                 ++g_inverseModules);
        // Shadows (docs/recon-xplane.md "Measured: dark shadow edges in the focus views"): deferred shading's right-eye
        // test (v_eye == 1) takes v_eye & 1, so the right focus view's shadow lookups come from its eye, not 6 cm off.
        const int parity = spirv_eye_parity_test(words, "u_shadow_data", "u_eye_right_to_left", "v_eye");
        if (parity > 0)
            logf("xplane: shadows: the right-eye test takes v_eye & 1 in shader module (%d tests), %d so far", parity,
                 ++g_parityModules);
        int skipped = 0;
        const int folded = spirv_fold_eye_arrays(words, kEyeArrayBlocks, &skipped);
        if (folded > 0 || skipped > 0) {
            const int n = ++g_eyeArrayModules;
            if (skipped > 0 || n <= 3 || n % 25 == 0)
                logf("xplane: per-eye arrays folded in shader module %d: %d accesses, %d skipped", n, folded, skipped);
        }
        // The focus box's size and centre: this frame's, from the push constants, in every edit that places the box
        // (docs/recon-xplane.md "Live focus size"). Without them (the pipeline calls not wrapped), the size wanted now
        // is baked in, the box on the eye's axis.
        const bool push = g_focusPush.load();
        const float bakedFx = static_cast<float>(g_focusFraction.load()), bakedFy = static_cast<float>(g_focusFractionY.load());
        // The projection coefficients (view rays from screen positions): the focus views' own, from their eye's and
        // the focus box (docs/recon-xplane.md "Measured: the sky and positions in the focus views").
        int coefSkipped = 0, coefByPixels = 0;
        const int coefficients =
            folded > 0 ? spirv_focus_proj_coefficients(words, "u_proj_coefficients", bakedFx, bakedFy, &coefSkipped, push,
                                                       &coefByPixels)
                       : 0;
        if (coefficients > 0 || coefSkipped > 0)
            logf("xplane: projection coefficients: focus views' own in shader module (%d loads, %d by pixels taking the "
                 "view rects, %d component reads left), %d so far",
                 coefficients, coefByPixels, coefSkipped, ++g_coefficientModules);
        // SSR in the focus views. From the eye (default; docs/recon-xplane.md "Measured: focus views reflect only what
        // is in the box"): a focus pixel's ray is projected with its eye's u_proj (the fold's) and marched in its eye's
        // hi-Z and lit history, so it reflects what lies outside the box too. Or in the focus view's own screen
        // (`xplane_ssr_from_eye=0`; docs/recon-xplane.md "Measured: the wing lit differently in the focus views"):
        // sharper, but blind to anything outside the box. Only in modules binding u_ssr_data (the hi-Z builder reads
        // tex_hiZ too).
        static const char kSsrBlock[] = "u_ssr_data";
        const auto* bytes = reinterpret_cast<const char*>(words.data());
        const bool ssrModule =
            std::search(bytes, bytes + words.size() * 4, kSsrBlock, kSsrBlock + sizeof(kSsrBlock) - 1) != bytes + words.size() * 4;
        int ssrProjections = 0, ssrRays = 0;
        int ssrStarts = 0;
        if (folded > 0 && ssrModule && g_ssrFromEye.load()) {
            ssrRays = spirv_ray_reads_from_eye(words, {"tex_hiZ", "tex_ssr", "gbuffer_vel"});
            // ... from their eye pixel's start (docs/recon-xplane.md "Measured: a dot grid on the wing in the focus
            // views").
            if (ssrRays > 0)
                ssrStarts = spirv_ray_start_at_eye_pixel(words);
        }
        else if (folded > 0 && ssrModule)
            ssrProjections = spirv_focus_projection_matrix(words, "u_ssr_data", "u_proj", bakedFx, bakedFy, push);
        if (ssrRays > 0)
            logf("xplane: SSR: focus views march in their eye's screen in shader module (%d reads, rays from the eye "
                 "pixel's start %s), %d so far",
                 ssrRays, ssrStarts > 0 ? "yes" : "NO: not found", ++g_ssrModules);
        if (ssrProjections > 0)
            logf("xplane: SSR: focus views trace with their own projection in shader module (%d loads), %d so far",
                 ssrProjections, ++g_ssrModules);
        // SSR over each view's rect (docs/recon-xplane.md "Measured: SSR's saw-tooth at 30%"): its projection takes the
        // view's place in the layer, so the march runs in the layer's texel grid. After the edits above (the eye's
        // projection, the eye-pixel start).
        int ssrRects = 0;
        if (ssrModule && g_viewRects.load() && push) {
            ssrRects = spirv_ssr_view_rects(words, ssrRays > 0, nullptr);
            if (ssrRects > 0)
                logf("xplane: SSR marches in each view's rect of the layer (%d projections) in shader module, %d so far",
                     ssrRects, ++g_ssrRectModules);
        }
        // Diagnostics ([experiment] xplane_diag=1; docs/recon-xplane.md "Measured: the wing after the SSR edit"): the
        // SSR results and CACAO's output can be switched off live, to see which pass draws an artefact.
        int diag = 0;
        if (g_diagEdits.load() && push) {
            diag = spirv_diag_override_write(words, "u_ssr_data", "i_output", 0, 0.0f, nullptr) +
                   spirv_diag_override_write(words, "u_ffx_cacao_data", "g_RwOutput", 1, 1.0f, nullptr) +
                   spirv_diag_override_sample(words, "u_shadow_data", "tex_smap0", 2, 1.0f, nullptr);
            if (diag > 0)
                logf("xplane: diagnostics: switchable output in shader module (%d writes), %d so far", diag,
                     ++g_diagModules);
        }
        // Tiled lights (docs/recon-xplane.md "Measured: right-eye popping near the wing"): the focus views pick the
        // tile of their pixel's position in the eye. After the coefficient edit, which would otherwise turn the eye's
        // coefficients this reads into the focus box's.
        const int tiles = spirv_focus_light_tiles(words, bakedFx, bakedFy, nullptr, push);
        if (tiles > 0)
            logf("xplane: tiled lights: focus views take their eye's tile in shader module (%d coordinates), %d so far",
                 tiles, ++g_tileModules);
        // AO at the focus views' edges from the eye (docs/recon-xplane.md "Measured: a dark rim at the top of the focus
        // views").
        if (g_aoEdgesFromEye.load()) {
            const int aoEdges = spirv_focus_ao_edges_from_eye(words, bakedFx, bakedFy, kAoEdgeBand, nullptr, push);
            if (aoEdges > 0)
                logf("xplane: AO: focus views take their eye's at their edges (%s) in shader module (%d samples), %d so far",
                     push ? "band following the focus size" : "12% band", aoEdges, ++g_aoEdgeModules);
        }
        // One viewport per view (docs/recon-xplane.md "Measured: the zoom with one viewport per view"): X-Plane's blit
        // vertex shader splits its source into u_viewport_count side-by-side parts; in four-view passes it takes 1.
        int splitOff = 0;
        if (g_viewRects.load()) {
            splitOff = spirv_viewport_split_off(words, nullptr, push);
            if (splitOff > 0)
                logf("xplane: view rects: the blit's side-by-side split off in four-view passes (%d loads), module %d",
                     splitOff, ++g_splitOffModules);
        }
        // The auto exposure's histogram over the outer view's rect, not the whole layer (docs/recon-xplane.md
        // "Measured: the smaller periphery traced"): the undrawn rest of the layer counted as black and raised the
        // exposure as the periphery shrank.
        int histogram = 0;
        if (g_viewRects.load() && push) {
            histogram = spirv_histogram_view_rect(words, nullptr);
            if (histogram > 0)
                logf("xplane: auto exposure histogram reads the outer view's rect (%d fetches), module %d", histogram,
                     ++g_histogramModules);
        }
        // Compute shaders taking their eye from the dispatch depth (the cloud render): write layer z, eye = z & 1.
        const bool layered = spirv_layer_from_dispatch_z(words);
        if (layered)
            logf("xplane: compute shader writes layer = dispatch z (eye from z & 1), module %d", ++g_layerFromZModules);
        // Bloom from the eye (docs/recon-xplane.md "Measured: bloom per view"): vertex shaders pass each view's map
        // into its eye's view, and the bloom apply samples the eye's bloom there, so a focus view gets its eye's glow.
        bool mapped = false, bloom = false;
        if (g_bloomFromEye.load()) {
            mapped = spirv_view_to_eye_map_out(words, kEyeMapLocation, nullptr);
            if (mapped) {
                const int n = ++g_eyeMapModules;
                if (n <= 3 || n % 25 == 0)
                    logf("xplane: vertex shader passes the view-to-eye map, module %d", n);
            }
            if (info->codeSize == kBloomApplyFsBytes && sha256_hex(info->pCode, info->codeSize) == kBloomApplyFsSha256) {
                std::string why;
                bloom = spirv_sample_eye_box(words, kEyeMapLocation, &why, push);
                logf("xplane: bloom apply %s%s", bloom ? "samples the eye's bloom in the focus views, edit " : "NOT edited: ",
                     bloom ? std::to_string(++g_bloomEdits).c_str() : why.c_str());
            }
        }
        // VRS: the focus views shaded at the full rate (docs/recon-xplane.md "Measured: the focus views shaded
        // coarsely").
        int fullRate = 0;
        if (g_focusFullRate.load()) {
            std::string why;
            fullRate = spirv_full_rate_from_layer(words, 2, &why);
            if (fullRate > 0)
                logf("xplane: shading-rate shader: layers 2-3 at the full rate (%d writes)", fullRate);
        }
        // Alpha tests matched across views (docs/recon-xplane.md "Measured: a 'wall' missing in the focus view"):
        // vertex shaders pass their layer on, and alpha-tested fragment shaders take the alpha of the focus view's mip
        // in views 0-1.
        bool layerOut = false;
        int alphaLod = 0;
        if (g_peripheryAlphaLod.load()) {
            layerOut = spirv_layer_out(words, kLayerLocation);
            if (layerOut) {
                const int n = ++g_layerOutModules;
                if (n <= 3 || n % 25 == 0)
                    logf("xplane: vertex shader passes its layer to the fragment shader, module %d", n);
            }
            const float bias = static_cast<float>(0.5 * std::log2(static_cast<double>(bakedFx) * bakedFy));
            std::string why;
            alphaLod = spirv_alpha_test_lod_bias(words, kLayerLocation, bias, &why, push);
            if (alphaLod > 0) {
                const int n = ++g_alphaLodModules;
                if (n <= 3 || n % 25 == 0)
                    logf("xplane: alpha test takes the focus view's mip in the periphery (bias %s %.2f, %d samples), "
                         "module %d",
                         push ? "following the focus size, now" : "baked,", push ? g_frame.load().values[0] : bias,
                         alphaLod, n);
            }
        }
        // The depth chains over each view's rect (docs/recon-xplane.md "Measured: SSR's strip and the depth chain"): the
        // min/max reductions read only the view, so SSR's and CACAO's coarse cells hold no undrawn depth.
        int hizRects = 0;
        if (g_viewRects.load() && push) {
            hizRects = spirv_hiz_view_rects(words, nullptr);
            if (hizRects > 0)
                logf("xplane: depth chain reads each view's rect (%d limits) in shader module, %d so far", hizRects,
                     ++g_hizRectModules);
        }
        // u_viewport_size as the whole layer (docs/recon-xplane.md "Measured: u_viewport_size follows the first rect"):
        // X-Plane fills it from the pass's first viewport, the outer view's rect once that is smaller. After every edit
        // above, which reads it too.
        int viewportSize = 0;
        if (g_viewRects.load()) {
            viewportSize = spirv_viewport_size_from_gbuffer(words, nullptr);
            if (viewportSize > 0) {
                const int n = ++g_viewportSizeModules;
                if (n <= 3 || n % 25 == 0)
                    logf("xplane: u_viewport_size read as the layer (u_gbuffer_size) in shader module (%d reads), %d so far",
                         viewportSize, n);
            }
        }
        if (inverted > 0 || parity > 0 || folded > 0 || layered || mapped || bloom || fullRate > 0 || coefficients > 0 ||
            ssrProjections > 0 || ssrRays > 0 || tiles > 0 || layerOut || alphaLod > 0 || splitOff > 0 || histogram > 0 ||
            ssrRects > 0 || diag > 0 || viewportSize > 0 || hizRects > 0) {
            ShaderModuleCreateInfo edited = *info;
            edited.codeSize = words.size() * 4;
            edited.pCode = words.data();
            return real(device, &edited, allocator, module);
        }
    }
    return real(device, info, allocator, module);
}

std::atomic<int> g_fillLogs{0};

// The tangents the views were rendered with (four_views_tangents): taken by the first draw after each request.
struct TangentRecord {
    std::mutex mutex;
    float outer[2][4] = {}, focus[2][4] = {};
    bool valid = false;
};
TangentRecord g_record;
std::atomic<bool> g_recordWanted{true};
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

int four_views_view_rect_sets() { return g_viewRectSets.load(); }

void four_views_set_diag_edits(bool on) {
    g_diagEdits = on;
    if (on)
        logf("xplane: diagnostics on: SSR, AO and the sun's shadow switchable live (Ctrl+Alt+1 SSR, Ctrl+Alt+2 AO, "
             "Ctrl+Alt+3 sun shadow; or xplane_diag_ssr_off, _ao_off, _shadow_off)");
}

void four_views_set_focus_full_rate(bool on) {
    g_focusFullRate = on;
    logf("xplane: focus views at the full shading rate %s", on ? "on" : "off");
}

void four_views_set_ao_edges_from_eye(bool on) {
    g_aoEdgesFromEye = on;
    logf("xplane: AO at the focus views' edges %s", on ? "from their eye's (no rim from CACAO's edges)"
                                                       : "their own (CACAO's edge rim)");
}

void four_views_set_ssr_from_eye(bool on) {
    g_ssrFromEye = on;
    logf("xplane: SSR in the focus views %s", on ? "marched in the eye's screen (reflects what lies outside the box)"
                                                 : "traced in the focus view's own screen (sharper, blind outside the box)");
}

void four_views_set_periphery_alpha_lod(bool on) {
    g_peripheryAlphaLod = on;
    logf("xplane: alpha tests in the periphery at the focus view's mip %s (fences and foliage alike in both views)",
         on ? "on" : "off");
}

void four_views_set_bloom_from_eye(bool on) {
    g_bloomFromEye = on;
    logf("xplane: bloom from the eye %s (focus views take their eye's glow)", on ? "on" : "off");
}

void four_views_set_eye_columns(bool on) {
    g_eyeColumns = on;
    logf("xplane: eye columns %s (the final resolve writes each eye's view over its focus view)", on ? "on" : "off");
}

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
    logf("xplane: LOD width x%.3f (%s): world pass %u scaled, %u kept (last kept W %d); graph setup %u scaled, %u kept "
         "(last kept W %d); the last stereo vegetation cull's u_lod_bias %.5f",
         w.scale.load() / 65536.0, g_focusLod.load() ? "the focus views' density" : "off", w.scaled.load(), w.other.load(),
         w.lastOther.load(), g.scaled.load(), g.other.load(), g.lastOther.load(), bias);
}

void four_views_set_focus_lod(bool on) {
    g_focusLod = on;
    update_lod_scale();
    logf("xplane: object and tree LOD %s", on ? "for the focus views' pixel density (all four views)"
                                              : "X-Plane's own (the eye view's density)");
}

std::uint64_t four_views_draws() { return g_fourViewDraws.load(std::memory_order_relaxed); }

bool four_views_final_resolve_made() { return g_finalResolveEdits.load() > 0; }

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

void four_views_set_gaze_follow(bool on) { g_gazeWanted = on; }

bool four_views_gaze_follows() { return g_focusPush.load() && g_gazeWanted.load(); }

void four_views_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]) {
    std::lock_guard guard(g_pendingGaze.mutex);
    std::memcpy(g_pendingGaze.tan, tan, sizeof(tan));
    std::memcpy(g_pendingGaze.valid, valid, sizeof(valid));
}

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

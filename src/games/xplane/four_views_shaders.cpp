// X-Plane's shader modules in four views (four_views.h): the vkCreateShaderModule hook four_views_copies_start puts
// in, the final resolve's edit, and every SPIR-V edit (spirv_patch.h) in the order each module gets them, with the
// switches that turn them on and the counts the log gives.
#include "four_views.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "abi.h"
#include "four_views_state.h"
#include "spirv_patch.h"

namespace xplane {

using namespace fv;

namespace {

using simqvfr::logf;

}  // namespace

namespace fv {

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

std::atomic<int> g_coefficientModules{0};
std::atomic<int> g_diagModules{0};

}  // namespace fv

namespace {

std::atomic<int> g_finalResolveEdits{0};
std::atomic<int> g_eyeArrayModules{0};
std::atomic<int> g_layerFromZModules{0};
std::atomic<int> g_inverseModules{0};
std::atomic<bool> g_eyeColumns{false};  // four_views_set_eye_columns
std::atomic<bool> g_bloomFromEye{false};  // four_views_set_bloom_from_eye
std::atomic<int> g_eyeMapModules{0}, g_bloomEdits{0}, g_parityModules{0}, g_ssrModules{0};
std::atomic<bool> g_peripheryAlphaLod{false};  // four_views_set_periphery_alpha_lod
std::atomic<bool> g_ssrFromEye{true};          // four_views_set_ssr_from_eye
std::atomic<bool> g_aoEdgesFromEye{true};      // four_views_set_ao_edges_from_eye
std::atomic<int> g_aoEdgeModules{0};
std::atomic<int> g_layerOutModules{0}, g_alphaLodModules{0}, g_tileModules{0};
std::atomic<bool> g_focusFullRate{false};  // four_views_set_focus_full_rate
std::atomic<int> g_splitOffModules{0};  // blit shaders whose side-by-side split is off in four-view passes
std::atomic<int> g_histogramModules{0};  // auto exposure histograms bounded by the outer view's rect
std::atomic<int> g_ssrRectModules{0};    // SSR shaders reading each view's rect
std::atomic<int> g_viewportSizeModules{0};  // shaders reading u_viewport_size as the layer (u_gbuffer_size)
std::atomic<int> g_hizRectModules{0};       // depth-chain reductions clamped to each view's rect
// Diagnostics (four_views_set_diag_edits): the switch edits in.
std::atomic<bool> g_diagEdits{false};

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

}  // namespace

namespace fv {

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

}  // namespace fv

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

bool four_views_final_resolve_made() { return g_finalResolveEdits.load() > 0; }

}  // namespace xplane

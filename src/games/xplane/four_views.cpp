#include "four_views.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
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

// The uniform blocks with per-eye arrays of two (README.md, "Inventory"), by their variables' names.
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

// X-Plane's vkCreateShaderModule call lands here (kShaderModuleLoad). The final resolve's fragment shader gets
// v_eye + 2 (it then shows layers 2-3); every other module goes through as it is.
std::int32_t create_shader_module(void* device, const ShaderModuleCreateInfo* info, const void* allocator,
                                  std::uint64_t* module) {
    const auto real = *reinterpret_cast<CreateShaderModule*>(simqvfr::module_base() + kVkCreateShaderModuleSlot);
    if (info && info->pCode && info->codeSize == kFinalResolveFsBytes &&
        sha256_hex(info->pCode, info->codeSize) == kFinalResolveFsSha256) {
        std::vector<std::uint32_t> words(info->pCode, info->pCode + info->codeSize / 4);
        std::string why;
        const bool columns = g_eyeColumns.load();
        if (columns ? spirv_eye_columns_resolve(words, &why) : spirv_add_to_input(words, "v_eye", 2, &why)) {
            ShaderModuleCreateInfo edited = *info;
            edited.codeSize = words.size() * 4;
            edited.pCode = words.data();
            const std::int32_t result = real(device, &edited, allocator, module);
            logf("xplane: final resolve shader edited (%s), vkCreateShaderModule %d, edit %d",
                 columns ? "eye columns: each eye's view over its focus view" : "v_eye + 2: layers 2-3", result,
                 ++g_finalResolveEdits);
            return result;
        }
        logf("xplane: final resolve shader NOT edited: %s", why.c_str());
    }
    // Every shader reading X-Plane's per-eye arrays: views 2-3 read their eye's entry (index & 1).
    if (info && info->pCode && info->codeSize >= 20) {
        std::vector<std::uint32_t> words(info->pCode, info->pCode + info->codeSize / 4);
        // The light billboards' size: projected and un-projected with the eye's pair (README.md, "Measured: focus
        // views darker"), so a light covers the same angle in a focus view as in its eye. The fold then takes the
        // inverse's index & 1.
        const int inverted = spirv_eye_projection_beside(words, "u_ogl_light_data", "u_proj_inv",
                                                         "u_transform_block_fat", "projection_matrix");
        if (inverted > 0)
            logf("xplane: light size from the eye's projection in shader module (%d accesses), %d so far", inverted,
                 ++g_inverseModules);
        int skipped = 0;
        const int folded = spirv_fold_eye_arrays(words, kEyeArrayBlocks, &skipped);
        if (folded > 0 || skipped > 0) {
            const int n = ++g_eyeArrayModules;
            if (skipped > 0 || n <= 3 || n % 25 == 0)
                logf("xplane: per-eye arrays folded in shader module %d: %d accesses, %d skipped", n, folded, skipped);
        }
        // Compute shaders taking their eye from the dispatch depth (the cloud render): write layer z, eye = z & 1.
        const bool layered = spirv_layer_from_dispatch_z(words);
        if (layered)
            logf("xplane: compute shader writes layer = dispatch z (eye from z & 1), module %d", ++g_layerFromZModules);
        if (inverted > 0 || folded > 0 || layered) {
            ShaderModuleCreateInfo edited = *info;
            edited.codeSize = words.size() * 4;
            edited.pCode = words.data();
            return real(device, &edited, allocator, module);
        }
    }
    return real(device, info, allocator, module);
}

// The focus views (four_views_set_focus): off = slots 2-3 copy the eyes (the copies stage).
std::atomic<bool> g_focus{false};
std::atomic<double> g_focusFraction{0.2}, g_focusFractionY{0.2};
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
    const bool focus = g_focus.load(std::memory_order_relaxed);
    const double fx = g_focusFraction.load(std::memory_order_relaxed), fy = g_focusFractionY.load(std::memory_order_relaxed);
    if (g_recordWanted.load(std::memory_order_relaxed) && g_recordWanted.exchange(false)) {
        float outer[2][4] = {}, inner[2][4] = {};
        bool ok = true;
        for (int eye = 0; eye < 2; ++eye) {
            double tan[4];
            ok &= projection_tangents(*slot(kStateProjections, eye), tan);
            lrud(tan, outer[eye]);
            // The focus box, centred on the eye's axis (focus_projection with cx = cy = 0).
            const double hw = focus ? 0.5 * fx * (tan[1] - tan[0]) : 0.0, hh = focus ? 0.5 * fy * (tan[3] - tan[2]) : 0.0;
            const float box[4] = {static_cast<float>(-hw), static_cast<float>(hw), static_cast<float>(hh),
                                  static_cast<float>(-hh)};
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
        double out[16];
        const bool ok = focus_projection(eyeProj, fx, fy, 0.0, 0.0, out);
        std::memcpy(focusProj, out, sizeof(out));
        if (g_fillLogs.load(std::memory_order_relaxed) < 4) {
            ++g_fillLogs;
            bool cm = false;
            double tan[4] = {};
            projection_layout(eyeProj, cm);
            projection_tangents(eyeProj, tan);
            logf("xplane: focus view %d %s: eye tangents x %.3f..%.3f y %.3f..%.3f (%s), focus %.0f%% x %.0f%%", eye + 2,
                 ok ? "built" : "NOT built (not a projection: copy of the eye)", tan[0], tan[1], tan[2], tan[3],
                 cm ? "column-major" : "row-major", fx * 100.0, fy * 100.0);
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

    bool ok = redirect(kSetViewports, kSetViewportsAnchor, sizeof(kSetViewportsAnchor), setViewports,
                       sizeof(setViewports), "multiplier 4 in set viewports");
    ok &= redirect(kMultiplierStoreA, kMultiplierStoreAnchor, sizeof(kMultiplierStoreAnchor), store, sizeof(store),
                   "multiplier 4 (g-buffer store A)");
    ok &= redirect(kMultiplierStoreB, kMultiplierStoreAnchor, sizeof(kMultiplierStoreAnchor), store, sizeof(store),
                   "multiplier 4 (g-buffer store B)");
    ok &= redirect(kModelviewCopyEnd, kModelviewCopyEndAnchor, sizeof(kModelviewCopyEndAnchor), slotCopy,
                   sizeof(slotCopy), "render-state slots 2-3 = copies of 0-1");
    ok &= redirect(kShaderModuleLoad, kShaderModuleLoadAnchor, sizeof(kShaderModuleLoadAnchor), shader, sizeof(shader),
                   "vkCreateShaderModule wrapped (final resolve: layers 2-3)");
    // The per-draw upload: with four views, fill_views (slots 2-3 from 0-1: copies, or focus projections) right before
    // the block is filled, whichever setter wrote the slots. The function keeps rsp 16-aligned in its body (5 pushes and
    // sub rsp, 0x830 from entry); xmm0-xmm2 are live there, so the stub saves xmm0-xmm5 and the volatile registers.
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
    const bool clouds = simqvfr::code_write(kCloudDispatchDepth, kCloudDispatchDepthCount, sizeof(kCloudDispatchDepthCount));
    logf("xplane: cloud render dispatch depth = view count (was stereo ? 2 : 1) at 0x%llx: %s",
         static_cast<unsigned long long>(kCloudDispatchDepth), clouds ? "patched" : "FAILED");
    ok &= clouds;
    return ok;
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
    // The final resolve's viewport (0, 0, 2 x context width, context height) gets the whole column: twice the height.
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
        0x45, 0x39, 0xD0,                    // 39: cmp r8d, r10d       (w = 2 x context width)
        0x75, 0x10,                          // 42: jne done
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

void four_views_set_focus(bool on, double fraction, double fractionY) {
    g_focusFraction = fraction;
    g_focusFractionY = fractionY > 0.0 ? fractionY : fraction;
    g_focus = on;
    logf("xplane: focus views %s (%.0f%% x %.0f%% of each eye, centred)", on ? "on" : "off (copies of the eyes)",
         fraction * 100.0, g_focusFractionY.load() * 100.0);
}

}  // namespace xplane

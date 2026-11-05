#include "four_views.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "abi.h"
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
        if (spirv_add_to_input(words, "v_eye", 2, &why)) {
            ShaderModuleCreateInfo edited = *info;
            edited.codeSize = words.size() * 4;
            edited.pCode = words.data();
            const std::int32_t result = real(device, &edited, allocator, module);
            logf("xplane: final resolve shader edited (v_eye + 2: layers 2-3), vkCreateShaderModule %d, edit %d", result,
                 ++g_finalResolveEdits);
            return result;
        }
        logf("xplane: final resolve shader NOT edited: %s", why.c_str());
    }
    // Every shader reading X-Plane's per-eye arrays: views 2-3 read their eye's entry (index & 1).
    if (info && info->pCode && info->codeSize >= 20) {
        std::vector<std::uint32_t> words(info->pCode, info->pCode + info->codeSize / 4);
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
        if (folded > 0 || layered) {
            ShaderModuleCreateInfo edited = *info;
            edited.codeSize = words.size() * 4;
            edited.pCode = words.data();
            return real(device, &edited, allocator, module);
        }
    }
    return real(device, info, allocator, module);
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
    // The per-draw upload: with four views, slots 2-3 = slots 0-1 for both arrays right before the block is filled,
    // whichever setter wrote them (the projection setter copies four entries from a two-eye source, so a path that
    // sets projections after modelviews left slots 2-3 with the camera's mono matrices: the light sprites' white).
    // xmm0-xmm2 are live there; the stub uses xmm3, rcx and rdx (set again before their next use).
    static const std::uint8_t uploadCopy[] = {
        0x41, 0x83, 0xFE, 0x04,                    // cmp r14d, 4
        0x75, 0x2C,                                // jne done
        0x48, 0x8D, 0x8F, 0x88, 0x00, 0x00, 0x00,  // lea rcx, [rdi + 0x88]   (modelview slot 0)
        0xBA, 0x10, 0x00, 0x00, 0x00,              // mov edx, 16
        0x0F, 0x10, 0x19,                          // loop: movups xmm3, [rcx]
        0x0F, 0x11, 0x99, 0x00, 0x01, 0x00, 0x00,  //   movups [rcx + 0x100], xmm3   (modelview slots 2-3)
        0x0F, 0x10, 0x99, 0x00, 0x02, 0x00, 0x00,  //   movups xmm3, [rcx + 0x200]   (projection slots 0-1)
        0x0F, 0x11, 0x99, 0x00, 0x03, 0x00, 0x00,  //   movups [rcx + 0x300], xmm3   (projection slots 2-3)
        0x48, 0x83, 0xC1, 0x10,                    //   add rcx, 16
        0xFF, 0xCA,                                //   dec edx
        0x75, 0xE0,                                //   jnz loop
        0x8B, 0x87, 0x94, 0x24, 0x00, 0x00,        // done: mov eax, [rdi + 0x2494] (the original)
    };
    ok &= redirect(kTransformUpload, kTransformUploadAnchor, sizeof(kTransformUploadAnchor), uploadCopy,
                   sizeof(uploadCopy), "per-draw upload: slots 2-3 = 0-1");
    const bool clouds = simqvfr::code_write(kCloudDispatchDepth, kCloudDispatchDepthCount, sizeof(kCloudDispatchDepthCount));
    logf("xplane: cloud render dispatch depth = view count (was stereo ? 2 : 1) at 0x%llx: %s",
         static_cast<unsigned long long>(kCloudDispatchDepth), clouds ? "patched" : "FAILED");
    ok &= clouds;
    return ok;
}

}  // namespace xplane

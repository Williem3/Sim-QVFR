// YEBIS's pixel shaders that LMU MVR's focus post needs (focus_post.h), known by their DXBC checksum (the 16 bytes from
// offset 4, as four little-endian words: what RenderDoc shows as the shader hash; docs/recon-lmu.md "Step 3, revised").
// The device's shader creation marks each with its role as private data (mvr_probe.cpp); the context's DrawIndexed hook
// reads it back from the bound pixel shader while an eye pass post-processes (mvr_pass.cpp). Header-only, for tests.
#pragma once

#include <guiddef.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lmu {

enum class YebisRole : unsigned { none = 0, copy = 1, composite = 2, blit = 3 };

// The copy (and every downsample): the texture at TEXCOORD0 x afRGBA_Modulate[0].
inline constexpr std::uint32_t kYebisCopyPs[4] = {0xa99a4efc, 0x0bbc5945, 0x1b33a200, 0xbb7f22f2};
// The composite: the scene copy (t0, TEXCOORD0), the glare (t1, TEXCOORD1) and the 1x1 exposure (t2, TEXCOORD3) through
// exposure, glare scale, tone curve, colour matrix and gamma; per pixel.
inline constexpr std::uint32_t kYebisCompositePs[4] = {0x60678228, 0xd8545570, 0xc4b42610, 0x164cfab4};
// The game's final blit after YEBIS (not YEBIS's): t0 x COLOR0 at TEXCOORD0, from a pass-through vertex shader; the
// resolved scene into the eye's final (and the desktop mirror's).
inline constexpr std::uint32_t kFinalBlitPs[4] = {0x0a09af97, 0xa8f63369, 0x391f83dd, 0xf5318e54};

// The private data a game pixel shader carries its role under (a UINT).
inline constexpr GUID kYebisRoleGuid = {0x3f0b6c21, 0x9d4e, 0x4a77, {0x8e, 0x15, 0x6b, 0x2c, 0x90, 0x41, 0xd3, 0x5a}};
// With the focus post on, every game vertex and pixel shader also carries its DXBC checksum (16 bytes): the post's
// log and the overlay trace name the shaders the game draws with.
inline constexpr GUID kShaderChecksumGuid = {0x7a2e9c43, 0x51d0, 0x4b8f, {0xa6, 0x3c, 0x0e, 0x94, 0x2b, 0x7f, 0x18, 0xc5}};

// A DXBC container's checksum words (none if it isn't one).
inline bool dxbc_checksum(const void* code, std::size_t size, std::uint32_t (&out)[4]) {
    const auto* b = static_cast<const unsigned char*>(code);
    if (!b || size < 20 || std::memcmp(b, "DXBC", 4) != 0)
        return false;
    std::memcpy(out, b + 4, 16);
    return true;
}

// Game vertex shaders the multi-view path knows by checksum, marked with their role at creation (mvr_probe.cpp, under
// kVsRoleGuid) and read back at their binds (mvr_pass.cpp; docs/recon-lmu.md "R6a run"): the lens mask's (its mesh
// already in the eye's clip space: `mov o0, v0`) and the VR UI panel's (the HUD and menus onto each eye's image).
enum class VsRole : unsigned { none = 0, lensMask = 1, vruiPanel = 2 };
inline constexpr std::uint32_t kLensMaskVs[4] = {0x4b92f26a, 0x3208d4b1, 0x46b29820, 0x43c439df};
inline constexpr std::uint32_t kVruiPanelVs[4] = {0x794627d7, 0x0ceee05e, 0x76cbc5c8, 0xc0d5d119};
inline constexpr GUID kVsRoleGuid = {0x5c81d2e7, 0x3a4f, 0x4e19, {0x9b, 0x62, 0x1d, 0xa8, 0x47, 0xc3, 0x0e, 0x95}};
inline VsRole vs_role(const void* code, std::size_t size) {
    std::uint32_t c[4];
    if (!dxbc_checksum(code, size, c))
        return VsRole::none;
    if (std::memcmp(c, kLensMaskVs, sizeof(c)) == 0)
        return VsRole::lensMask;
    if (std::memcmp(c, kVruiPanelVs, sizeof(c)) == 0)
        return VsRole::vruiPanel;
    return VsRole::none;
}

inline YebisRole yebis_role(const void* code, std::size_t size) {
    std::uint32_t c[4];
    if (!dxbc_checksum(code, size, c))
        return YebisRole::none;
    if (std::memcmp(c, kYebisCopyPs, sizeof(c)) == 0)
        return YebisRole::copy;
    if (std::memcmp(c, kYebisCompositePs, sizeof(c)) == 0)
        return YebisRole::composite;
    if (std::memcmp(c, kFinalBlitPs, sizeof(c)) == 0)
        return YebisRole::blit;
    return YebisRole::none;
}

}  // namespace lmu

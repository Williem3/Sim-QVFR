// X-Plane 12 facts from static analysis (README.md). Pinned to the exe build in game.h: patches check the anchor
// bytes before writing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xplane {

// The render size query (int* width, int* height, int* layers). With the VR object present it returns half the
// stereo image's width, its height, and a hard-coded layer count of 2, which REN_build_render_graph puts in
// REN_graph_info+0x10 (the per-eye targets' array layers).
inline constexpr std::uintptr_t kRenderSizeQuery = 0xc83a40;
// `mov dword ptr [r12], 2`: the layer count. Its immediate is the byte at +4.
inline constexpr std::uintptr_t kVrLayerCount = 0xc83b4a;
inline constexpr std::uint8_t kVrLayerCountAnchor[8] = {0x41, 0xC7, 0x04, 0x24, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kVrLayerCountImm = 4;

// The VR REN_context ("VR context", made in 0xc66670 through the REN_context constructor 0x196b5e0): its layer
// count, `mov dword ptr [rsp+0x28], 2` (the constructor's 6th argument). Immediate at +4.
inline constexpr std::uintptr_t kVrContextLayers = 0xc6680f;
inline constexpr std::uint8_t kVrContextLayersAnchor[8] = {0xC7, 0x44, 0x24, 0x28, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kVrContextLayersImm = 4;
// The camera setup (0x196c5d0): stereo flag = (layers == 2), `sete al` after `cmp dword ptr [r15+0xc], 2`.
inline constexpr std::uintptr_t kStereoTestCmp = 0x196ca8b;
inline constexpr std::uint8_t kStereoTestCmpAnchor[5] = {0x41, 0x83, 0x7F, 0x0C, 0x02};
inline constexpr std::uintptr_t kStereoTestSet = 0x196ca9e;
inline constexpr std::uint8_t kStereoTestSetAnchor[3] = {0x0F, 0x94, 0xC0};  // sete al
inline constexpr std::uint8_t kStereoTestSetGe[3] = {0x0F, 0x9D, 0xC0};      // setge al
// Its loop copying each view's tangents (ctx+0x54 + i*0x10) into the camera, `cmp r8d, [r15+0xc]`: the camera has
// room for two, so with more layers the bound becomes `cmp r8d, 2` (same length).
inline constexpr std::uintptr_t kTangentCopyBound = 0x196cafe;
inline constexpr std::uint8_t kTangentCopyBoundAnchor[4] = {0x45, 0x3B, 0x47, 0x0C};
inline constexpr std::uint8_t kTangentCopyBoundTwo[4] = {0x41, 0x83, 0xF8, 0x02};

}  // namespace xplane

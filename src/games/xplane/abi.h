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

}  // namespace xplane

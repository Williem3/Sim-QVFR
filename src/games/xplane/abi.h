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
// The VR context's resizes (0x196cd90(ctx, width, height, layers), which also rebuilds its lit-image miptree, the
// reflections' input): `mov r9d, 2` in the VR setup (0xc66670) and the VR window resize (0xc86410). Immediate at +2.
inline constexpr std::uintptr_t kVrContextResizeA = 0xc6684f;
inline constexpr std::uintptr_t kVrContextResizeB = 0xc86675;
inline constexpr std::uint8_t kVrContextResizeAnchor[6] = {0x41, 0xB9, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kVrContextResizeImm = 2;
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

// REN_build_render_graph_3d_world_subgraph (0x198f270) rewrites its REN_graph_info for the VR window: width = the
// stereo image's width / 2, its height, and layers from `mov esi, 2` (the G-buffer and the main per-eye targets read
// REN_graph_info+0x10). Immediate at +1.
inline constexpr std::uintptr_t kWorldSubgraphLayers = 0x198f33c;
inline constexpr std::uint8_t kWorldSubgraphLayersAnchor[5] = {0xBE, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kWorldSubgraphLayersImm = 1;

// The art control "tonemap/exposure_fusion" (registered at 0x550f20 through 0xd758c0, type 5 = float, default 1.0):
// its value is the first float of its object, in .data. 0xb0fc00 (exposure fusion on?) returns value > 0, so 0 turns
// the pass off. Exposure fusion builds its pyramids for two layers only (lambda 4 of
// OGL_build_exposure_fusion_graph, 0xae8e00: per-layer views for layers 0 and 1, `if (layers > 1)`), so with four
// views it is turned off.
inline constexpr std::uintptr_t kExposureFusionControl = 0x575fd80;

// ---- Four views, drawn (README.md "The stereo count in the draw path") ----
// The per-thread render state: `+0x2490` the instance multiplier; per-view modelviews at +0x88 + i*0x80 and
// projections at +0x288 + i*0x80, room for four of each.
inline constexpr std::size_t kStateModelviews = 0x88;
inline constexpr std::size_t kStateProjections = 0x288;
inline constexpr std::size_t kStateViewStride = 0x80;
// set viewports (count ecx, multiplier edx, rects r8): its first instruction, `mov [rsp+8], rbx`. Callers pass
// multiplier = stereo + 1 (0x7ba150, 0x7a7a70 twice, 0x1995650).
inline constexpr std::uintptr_t kSetViewports = 0x1595790;
inline constexpr std::uint8_t kSetViewportsAnchor[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
// The two direct stores of multiplier = stereo + 1 (OGL_build_world_gbuffer_graph's lambdas): `mov [rax+0x2490], ecx`.
inline constexpr std::uintptr_t kMultiplierStoreA = 0x79ca48;
inline constexpr std::uintptr_t kMultiplierStoreB = 0x7ab258;
inline constexpr std::uint8_t kMultiplierStoreAnchor[6] = {0x89, 0x88, 0x90, 0x24, 0x00, 0x00};
// The end of the modelview copy (0x15955d0, after 0x1595510 copied the projections in 0x7b9f00's stereo path):
// `or dword [rax], 0x18; mov rbx, [rsp+0x30]`, rax the render state.
inline constexpr std::uintptr_t kModelviewCopyEnd = 0x1595653;
inline constexpr std::uint8_t kModelviewCopyEndAnchor[8] = {0x83, 0x08, 0x18, 0x48, 0x8B, 0x5C, 0x24, 0x30};
// The per-draw transform block upload (0x1594600): for i < r14d (the transform count) it reads modelview
// state+0x88+i*0x80 and projection state+0x288+i*0x80 (doubles) into the block. Hooked at `mov eax, [rdi+0x2494]`
// (rdi the state), just before that loop: every draw passes here, whichever setter filled the slots.
inline constexpr std::uintptr_t kTransformUpload = 0x1594bf9;
inline constexpr std::uint8_t kTransformUploadAnchor[6] = {0x8B, 0x87, 0x94, 0x24, 0x00, 0x00};
// The cloud render's dispatch depth (0x1b63980): count = max(multiplier, layers), then z = (count > 1) + 1, i.e. 2 in
// stereo: `cmp dword [rdx], 1; setg r14b; inc r14d`. Replaced by z = count: `mov r14d, [rdx]` and a 7-byte nop.
inline constexpr std::uintptr_t kCloudDispatchDepth = 0x1b6460f;
inline constexpr std::uint8_t kCloudDispatchDepthAnchor[10] = {0x83, 0x3A, 0x01, 0x41, 0x0F, 0x9F, 0xC6, 0x41, 0xFF, 0xC6};
inline constexpr std::uint8_t kCloudDispatchDepthCount[10] = {0x44, 0x8B, 0x32, 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00};
// X-Plane's one vkCreateShaderModule call loads the function from its table: `mov rax, [rip+...]` (0x632dad0).
inline constexpr std::uintptr_t kShaderModuleLoad = 0x64495a;
inline constexpr std::uint8_t kShaderModuleLoadAnchor[7] = {0x48, 0x8B, 0x05, 0x6F, 0x91, 0xCE, 0x05};
inline constexpr std::uintptr_t kVkCreateShaderModuleSlot = 0x632dad0;
// The final resolve's fragment shader (HDR array -> the side-by-side image; layer = (u > 1) + v_eye), as X-Plane
// creates it: its size and SHA-256.
inline constexpr std::size_t kFinalResolveFsBytes = 16716;
inline constexpr const char* kFinalResolveFsSha256 = "bddaf459f44fe41d16eaf8c672eadf99a15afbce80713b3fb33958f3a1a95e0f";

// ---- Eye columns (README.md "Composition"): the stereo image is (2 x eye width) x (outer + focus height), the views
// half its height. Where X-Plane takes a view's height from the stereo image (its `+0x88`) or its own VR context:
// The render size query (0xc83a40): `mov ecx, [r15+0x88]` (the stereo image's height) before the min with the caller's.
inline constexpr std::uintptr_t kRenderSizeHeight = 0xc83b3c;
inline constexpr std::uint8_t kRenderSizeHeightAnchor[7] = {0x41, 0x8B, 0x8F, 0x88, 0x00, 0x00, 0x00};
// REN_build_render_graph_3d_world_subgraph (0x198f270), the VR window: `mov edi, [rax+0x88]`, its graph's height.
inline constexpr std::uintptr_t kWorldSubgraphHeight = 0x198f333;
inline constexpr std::uint8_t kWorldSubgraphHeightAnchor[6] = {0x8B, 0xB8, 0x88, 0x00, 0x00, 0x00};
// The VR context made in 0xc66670 (width = image width / 2, height ebx = the image's): `mov [rsp+0x20], ebx;
// lea r8, [rbp+0x10]` (the height argument, then the name).
inline constexpr std::uintptr_t kVrContextHeight = 0xc66817;
inline constexpr std::uint8_t kVrContextHeightAnchor[8] = {0x89, 0x5C, 0x24, 0x20, 0x4C, 0x8D, 0x45, 0x10};
// Its two resizes (0x196cd90: ctx, width, height r8d, layers r9d). A: `mov r9d, 4` (kVrContextResizeA, after
// xplane_context_layers) then `mov r8d, ebx`; B: `mov r8d, eax; mov eax, ebx`.
inline constexpr std::uintptr_t kVrContextResizeHeightA = 0xc6684f;
inline constexpr std::uint8_t kVrContextResizeHeightAAnchor[9] = {0x41, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x44, 0x8B, 0xC3};
inline constexpr std::uintptr_t kVrContextResizeHeightB = 0xc8665c;
inline constexpr std::uint8_t kVrContextResizeHeightBAnchor[5] = {0x44, 0x8B, 0xC0, 0x8B, 0xC3};
// The single-viewport setter (x ecx, y edx, w r8d, h r9d; it also gives the scissor): its first instruction,
// `mov [rsp+8], rbx`. Every one-viewport pass comes here (0x1595790 with count 1 among them). The final resolve sets
// (0, 0, 2 x context width, context height): with the context at half the column (capture XPlane_frame2096.rdc), it
// drew the top row only.
inline constexpr std::uintptr_t kSetViewport = 0x104a500;
inline constexpr std::uint8_t kSetViewportAnchor[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
// The VR REN_context's pointer (written by 0xc66670), and its width and height (`+4`, `+8`; "Updating REN_context").
inline constexpr std::uintptr_t kVrContextPointer = 0x5777c60;
inline constexpr std::size_t kContextWidth = 4;
inline constexpr std::size_t kContextHeight = 8;

}  // namespace xplane

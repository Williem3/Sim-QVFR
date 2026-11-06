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
// Per-view sizes (README.md "Design: true per-view sizes"): the graphics pipelines' viewport and scissor count,
// `setne cl; inc ecx` = (device+0x91 != 0) + 1, i.e. 2 with multiViewport. Written as `mov ecx, 4`.
inline constexpr std::uintptr_t kPipelineViewportCount = 0x628437;
inline constexpr std::uint8_t kPipelineViewportCountAnchor[5] = {0x0F, 0x95, 0xC1, 0xFF, 0xC1};
inline constexpr std::uint8_t kPipelineViewportCountFour[5] = {0xB9, 0x04, 0x00, 0x00, 0x00};
// 3D line drawing (`u_line3d_data`, 0x80 bytes) writes one {w, h, w/h, 1} entry per viewport at +0x10 and +0x20, room
// for two (+0x30 is the line width), and asserts "Viewport count out of range for line drawing! viewports.size() <= 2".
// With four viewports: the assertion's `ja` (count > 2) is skipped, and the loop's end (`add rdx, rax; cmp r8, rdx;
// jb loop`) stops at min(count, 2). The shader reads entry index & 1 (the eye-array fold).
inline constexpr std::uintptr_t kLineViewportAssert = 0xcb9ee1;
inline constexpr std::uint8_t kLineViewportAssertAnchor[6] = {0x0F, 0x87, 0x54, 0x05, 0x00, 0x00};
inline constexpr std::uintptr_t kLineViewportLoopEnd = 0xcb9f58;
inline constexpr std::uint8_t kLineViewportLoopEndAnchor[8] = {0x48, 0x03, 0xD0, 0x4C, 0x3B, 0xC2, 0x72, 0x92};
inline constexpr std::uintptr_t kLineViewportLoopTop = 0xcb9ef2;
inline constexpr std::uint8_t kSetViewportsAnchor[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
// The two direct stores of multiplier = stereo + 1 (OGL_build_world_gbuffer_graph's lambdas): `mov [rax+0x2490], ecx`.
inline constexpr std::uintptr_t kMultiplierStoreA = 0x79ca48;
inline constexpr std::uintptr_t kMultiplierStoreB = 0x7ab258;
inline constexpr std::uint8_t kMultiplierStoreAnchor[6] = {0x89, 0x88, 0x90, 0x24, 0x00, 0x00};
// More writes of the multiplier (README.md "Measured: trees missing in the focus views"), each `mov [base + 0x2490],
// reg`: the render-state restore (0x7ac280, from a saved block's +0x480: the world passes' lambdas restore a block
// saved with 2), and two in one draw path that write their own count back right after set viewports. A 2 written
// there becomes 4. {rva, register (x86 number), base register}.
struct MultiplierWrite {
    std::uintptr_t rva;
    std::uint8_t reg, base;
    std::uint8_t anchor[6];
};
inline constexpr MultiplierWrite kMoreMultiplierWrites[] = {
    {0x7ac28d, 0 /* eax */, 2 /* rdx */, {0x89, 0x82, 0x90, 0x24, 0x00, 0x00}},
    {0x92f34c, 5 /* ebp */, 3 /* rbx */, {0x89, 0xAB, 0x90, 0x24, 0x00, 0x00}},
    {0x92f61f, 6 /* esi */, 0 /* rax */, {0x89, 0xB0, 0x90, 0x24, 0x00, 0x00}},
};
// The vegetation cull's uniform fill (0x9382f0, one call per cull from 0x1932ae8; README.md "Measured: trees missing in
// the focus views"): rdi the cull object, rbx the setup's struct (0x1935b70: +0x19c = max(multiplier, viewport count),
// +0x1a0 = that > 1), r9 the uniform data. `u_render_layers` (+0x1b8) = ([rdi+0x1c8], the copied flag, != 0) + 1: a
// hard-coded 2 in stereo, whatever the multiplier. It is the mesh trees' instance factor and the billboards' instance
// count. {mov eax, esi; cmp byte [rdi+0x1c8], al; setne al; inc eax; mov [r9+0x1b8], eax}.
inline constexpr std::uintptr_t kVegetationRenderLayers = 0x9384a9;
inline constexpr std::uint8_t kVegetationRenderLayersAnchor[20] = {0x8B, 0xC6, 0x38, 0x87, 0xC8, 0x01, 0x00,
                                                                   0x00, 0x0F, 0x95, 0xC0, 0xFF, 0xC0, 0x41,
                                                                   0x89, 0x81, 0xB8, 0x01, 0x00, 0x00};
inline constexpr std::size_t kCullSetupViewCount = 0x19c;  // in the setup's struct (rbx)
// The cull frustum builder (0xce08d0, into state+0x24a0; README.md "Measured: the right eye's outer strip culled"):
// `if (multiplier == 2 && projection 0 is perspective)` it builds one frustum covering both eyes (0xcb3930 from
// modelview/projection slots 0-1); any other multiplier takes a per-view loop whose combined frustum is view 0's, the
// left eye alone. {cmp esi, 2; jne 0xce095b}: 4 now takes the two-eye path too (the eyes' union holds the focus views).
inline constexpr std::uintptr_t kStereoFrustumTest = 0xce08ec;
inline constexpr std::uint8_t kStereoFrustumTestAnchor[5] = {0x83, 0xFE, 0x02, 0x75, 0x6A};
inline constexpr std::uintptr_t kStereoFrustumOther = 0xce095b;  // the jne's target: the other multipliers
// Stereo as `max(multiplier, viewport count) == 2` (README.md "Measured: the right eye's SSR matrices never written"):
// with four views these skip the eyes' second entry or their stereo-only work, leaving it stale. Each now takes 2 or 4.
// {cmp X, 2; jne target}: eye-1 / stereo-only blocks (0x7b888d: per-eye globals from modelview and projection slots
// 0-1; 0x93005d: a per-eye fill reading projection slot 1; 0x1910eff: a stereo-only branch).
struct StereoCountJump {
    std::uintptr_t rva, target;
    std::uint8_t anchor[9];  // the cmp (3 bytes, immediate 2 at +2) and the jne rel32
};
inline constexpr StereoCountJump kStereoCountJumps[] = {
    {0x7b888d, 0x7b899b, {0x83, 0xFD, 0x02, 0x0F, 0x85, 0x05, 0x01, 0x00, 0x00}},
    {0x93005d, 0x930212, {0x83, 0x3A, 0x02, 0x0F, 0x85, 0xAC, 0x01, 0x00, 0x00}},
    {0x1910eff, 0x1911481, {0x83, 0x3A, 0x02, 0x0F, 0x85, 0x79, 0x05, 0x00, 0x00}},
};
// The SSR fill (0x918cd0, u_ssr_data): the count stored at [rbp+0x518], read only by `stereo = (count == 2)` (sete,
// passed to the reprojection 0x927040) and `if (count == 2)` eye 1's u_proj/u_reproj/u_local_reproj. A 4 is stored as 2.
inline constexpr std::uintptr_t kSsrFillCount = 0x918f7f;
inline constexpr std::uint8_t kSsrFillCountAnchor[6] = {0x89, 0x85, 0x18, 0x05, 0x00, 0x00};  // mov [rbp+0x518], eax
// The reprojection globals (0x925d70): `cmp ebx, 2; sete bpl` (stereo flag), then `if (ebx == 2)` eye 1. A 4 becomes 2.
inline constexpr std::uintptr_t kReprojectionCount = 0x925dc8;
inline constexpr std::uint8_t kReprojectionCountAnchor[7] = {0x83, 0xFB, 0x02, 0x40, 0x0F, 0x94, 0xC5};
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
// The bloom apply (fragment shader: the bloom mips summed at gl_FragCoord.xy * u_offset in layer v_eye, into
// "resolved msaa"; README.md "Measured: bloom per view").
inline constexpr std::size_t kBloomApplyFsBytes = 3084;
inline constexpr const char* kBloomApplyFsSha256 = "131f984cc9e2bd4fb801f987969d49e92cc6bad16e5c44a89b2384d5b27b6938";
// The vertex outputs' location for the view-to-eye map (spirv_view_to_eye_map_out); X-Plane's use 0-2 here.
inline constexpr std::uint32_t kEyeMapLocation = 15;
// The layer passed from vertex to fragment shaders for the alpha tests' mip bias (spirv_layer_out,
// spirv_alpha_test_lod_bias). X-Plane's shaders use locations up to 16 (capture XPlane_frame2409.rdc, 201 modules).
inline constexpr std::uint32_t kLayerLocation = 20;

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
// drew the top row only. Its width is the desktop window's when that is wider (a 1614-wide image on a 2560-wide
// monitor: 2560x675, session 2026-11-05 22:23), so the match is width >= 2 x context width.
inline constexpr std::uintptr_t kSetViewport = 0x104a500;
inline constexpr std::uint8_t kSetViewportAnchor[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
// The VR REN_context's pointer (written by 0xc66670), and its width and height (`+4`, `+8`; "Updating REN_context").
inline constexpr std::uintptr_t kVrContextPointer = 0x5777c60;
inline constexpr std::size_t kContextWidth = 4;
inline constexpr std::size_t kContextHeight = 8;

}  // namespace xplane

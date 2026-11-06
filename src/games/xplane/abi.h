// X-Plane 12 addresses, pinned to the exe build in game.h: patches check the anchor bytes before writing. What each
// patch does and how it was found: docs/recon-xplane.md, section "abi.h", under the [name] given here (local notes,
// not in the repository).
#pragma once

#include <cstddef>
#include <cstdint>

namespace xplane {

// The render size query, and its layer count (immediate at +4). [Render size query]
inline constexpr std::uintptr_t kRenderSizeQuery = 0xc83a40;
inline constexpr std::uintptr_t kVrLayerCount = 0xc83b4a;
inline constexpr std::uint8_t kVrLayerCountAnchor[8] = {0x41, 0xC7, 0x04, 0x24, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kVrLayerCountImm = 4;

// The VR context's layer count when made (immediate at +4) and in its resizes (at +2). [VR context layers]
inline constexpr std::uintptr_t kVrContextLayers = 0xc6680f;
inline constexpr std::uint8_t kVrContextLayersAnchor[8] = {0xC7, 0x44, 0x24, 0x28, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kVrContextLayersImm = 4;
inline constexpr std::uintptr_t kVrContextResizeA = 0xc6684f;
inline constexpr std::uintptr_t kVrContextResizeB = 0xc86675;
inline constexpr std::uint8_t kVrContextResizeAnchor[6] = {0x41, 0xB9, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kVrContextResizeImm = 2;
// The camera setup's stereo test, and its tangent copy's bound. [Camera stereo test]
inline constexpr std::uintptr_t kStereoTestCmp = 0x196ca8b;
inline constexpr std::uint8_t kStereoTestCmpAnchor[5] = {0x41, 0x83, 0x7F, 0x0C, 0x02};
inline constexpr std::uintptr_t kStereoTestSet = 0x196ca9e;
inline constexpr std::uint8_t kStereoTestSetAnchor[3] = {0x0F, 0x94, 0xC0};  // sete al
inline constexpr std::uint8_t kStereoTestSetGe[3] = {0x0F, 0x9D, 0xC0};      // setge al
inline constexpr std::uintptr_t kTangentCopyBound = 0x196cafe;
inline constexpr std::uint8_t kTangentCopyBoundAnchor[4] = {0x45, 0x3B, 0x47, 0x0C};
inline constexpr std::uint8_t kTangentCopyBoundTwo[4] = {0x41, 0x83, 0xF8, 0x02};

// The 3D world graph's layer count (immediate at +1). [World subgraph layers]
inline constexpr std::uintptr_t kWorldSubgraphLayers = 0x198f33c;
inline constexpr std::uint8_t kWorldSubgraphLayersAnchor[5] = {0xBE, 0x02, 0x00, 0x00, 0x00};
inline constexpr std::size_t kWorldSubgraphLayersImm = 1;

// The exposure fusion art control's value: 0 turns the pass off. [Exposure fusion]
inline constexpr std::uintptr_t kExposureFusionControl = 0x575fd80;

// ---- Four views, drawn ----
// The render state's per-view modelviews and projections. [Render state]
inline constexpr std::size_t kStateModelviews = 0x88;
inline constexpr std::size_t kStateProjections = 0x288;
inline constexpr std::size_t kStateViewStride = 0x80;
// set viewports (count, multiplier, rects). [Set viewports]
inline constexpr std::uintptr_t kSetViewports = 0x1595790;
// The graphics pipelines' viewport and scissor count, written as 4. [Pipeline viewport count]
inline constexpr std::uintptr_t kPipelineViewportCount = 0x628437;
inline constexpr std::uint8_t kPipelineViewportCountAnchor[5] = {0x0F, 0x95, 0xC1, 0xFF, 0xC1};
inline constexpr std::uint8_t kPipelineViewportCountFour[5] = {0xB9, 0x04, 0x00, 0x00, 0x00};
// 3D line drawing's per-viewport entries: its assertion and its loop. [3D line viewports]
inline constexpr std::uintptr_t kLineViewportAssert = 0xcb9ee1;
inline constexpr std::uint8_t kLineViewportAssertAnchor[6] = {0x0F, 0x87, 0x54, 0x05, 0x00, 0x00};
inline constexpr std::uintptr_t kLineViewportLoopEnd = 0xcb9f58;
inline constexpr std::uint8_t kLineViewportLoopEndAnchor[8] = {0x48, 0x03, 0xD0, 0x4C, 0x3B, 0xC2, 0x72, 0x92};
inline constexpr std::uintptr_t kLineViewportLoopTop = 0xcb9ef2;
inline constexpr std::uint8_t kSetViewportsAnchor[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
// The instance multiplier's two direct stores. [Multiplier stores]
inline constexpr std::uintptr_t kMultiplierStoreA = 0x79ca48;
inline constexpr std::uintptr_t kMultiplierStoreB = 0x7ab258;
inline constexpr std::uint8_t kMultiplierStoreAnchor[6] = {0x89, 0x88, 0x90, 0x24, 0x00, 0x00};
// More writes of the multiplier: a 2 written there becomes 4. [More multiplier writes]
struct MultiplierWrite {
    std::uintptr_t rva;
    std::uint8_t reg, base;  // the stored register and the base register (x86 numbers)
    std::uint8_t anchor[6];
};
inline constexpr MultiplierWrite kMoreMultiplierWrites[] = {
    {0x7ac28d, 0 /* eax */, 2 /* rdx */, {0x89, 0x82, 0x90, 0x24, 0x00, 0x00}},
    {0x92f34c, 5 /* ebp */, 3 /* rbx */, {0x89, 0xAB, 0x90, 0x24, 0x00, 0x00}},
    {0x92f61f, 6 /* esi */, 0 /* rax */, {0x89, 0xB0, 0x90, 0x24, 0x00, 0x00}},
};
// The vegetation cull's render layers, and the view count in its setup. [Vegetation render layers]
inline constexpr std::uintptr_t kVegetationRenderLayers = 0x9384a9;
inline constexpr std::uint8_t kVegetationRenderLayersAnchor[20] = {0x8B, 0xC6, 0x38, 0x87, 0xC8, 0x01, 0x00,
                                                                   0x00, 0x0F, 0x95, 0xC0, 0xFF, 0xC0, 0x41,
                                                                   0x89, 0x81, 0xB8, 0x01, 0x00, 0x00};
inline constexpr std::size_t kCullSetupViewCount = 0x19c;  // in the setup's struct (rbx)
// The cull frustum's stereo test: 4 takes the two-eye path too. [Cull frustum]
inline constexpr std::uintptr_t kStereoFrustumTest = 0xce08ec;
inline constexpr std::uint8_t kStereoFrustumTestAnchor[5] = {0x83, 0xFE, 0x02, 0x75, 0x6A};
inline constexpr std::uintptr_t kStereoFrustumOther = 0xce095b;  // the jne's target: the other multipliers
// Stereo tests on `count == 2` that now take 2 or 4. [Stereo count jumps]
struct StereoCountJump {
    std::uintptr_t rva, target;
    std::uint8_t anchor[9];  // the cmp (3 bytes, immediate 2 at +2) and the jne rel32
};
inline constexpr StereoCountJump kStereoCountJumps[] = {
    {0x7b888d, 0x7b899b, {0x83, 0xFD, 0x02, 0x0F, 0x85, 0x05, 0x01, 0x00, 0x00}},
    {0x93005d, 0x930212, {0x83, 0x3A, 0x02, 0x0F, 0x85, 0xAC, 0x01, 0x00, 0x00}},
    {0x1910eff, 0x1911481, {0x83, 0x3A, 0x02, 0x0F, 0x85, 0x79, 0x05, 0x00, 0x00}},
};
// The SSR fill's stereo count: a 4 is stored as 2. [SSR fill count]
inline constexpr std::uintptr_t kSsrFillCount = 0x918f7f;
inline constexpr std::uint8_t kSsrFillCountAnchor[6] = {0x89, 0x85, 0x18, 0x05, 0x00, 0x00};  // mov [rbp+0x518], eax
// The reprojection globals' stereo count: a 4 becomes 2. [Reprojection count]
inline constexpr std::uintptr_t kReprojectionCount = 0x925dc8;
inline constexpr std::uint8_t kReprojectionCountAnchor[7] = {0x83, 0xFB, 0x02, 0x40, 0x0F, 0x94, 0xC5};
// The object and tree LOD's screen width, read at two sites; rax and r11 are free at both. [LOD width]
inline constexpr std::uintptr_t kWorldLodHeight = 0x7ab3f6;
inline constexpr std::uint8_t kWorldLodHeightAnchor[9] = {0x45, 0x8B, 0x40, 0x04,         // mov r8d, [r8 + 4]
                                                          0x48, 0x89, 0x44, 0x24, 0x28};  // mov [rsp + 0x28], rax
inline constexpr std::uintptr_t kGraphLodHeight = 0x196ccd0;
inline constexpr std::uint8_t kGraphLodHeightAnchor[8] = {0x41, 0x8B, 0x4F, 0x04,   // mov ecx, [r15 + 4]
                                                          0x49, 0x8D, 0x57, 0x10};  // lea rdx, [r15 + 0x10]
// The end of the modelview copy, rax the render state. [Modelview copy end]
inline constexpr std::uintptr_t kModelviewCopyEnd = 0x1595653;
inline constexpr std::uint8_t kModelviewCopyEndAnchor[8] = {0x83, 0x08, 0x18, 0x48, 0x8B, 0x5C, 0x24, 0x30};
// The per-draw transform block upload, rdi the render state. [Transform upload]
inline constexpr std::uintptr_t kTransformUpload = 0x1594bf9;
inline constexpr std::uint8_t kTransformUploadAnchor[6] = {0x8B, 0x87, 0x94, 0x24, 0x00, 0x00};
// The cloud render's dispatch depth, replaced by the view count. [Cloud dispatch depth]
inline constexpr std::uintptr_t kCloudDispatchDepth = 0x1b6460f;
inline constexpr std::uint8_t kCloudDispatchDepthAnchor[10] = {0x83, 0x3A, 0x01, 0x41, 0x0F, 0x9F, 0xC6, 0x41, 0xFF, 0xC6};
inline constexpr std::uint8_t kCloudDispatchDepthCount[10] = {0x44, 0x8B, 0x32, 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00};
// vkCreateShaderModule: the call's function load and its table slot. [Shader module load]
inline constexpr std::uintptr_t kShaderModuleLoad = 0x64495a;
inline constexpr std::uint8_t kShaderModuleLoadAnchor[7] = {0x48, 0x8B, 0x05, 0x6F, 0x91, 0xCE, 0x05};
inline constexpr std::uintptr_t kVkCreateShaderModuleSlot = 0x632dad0;
// Shaders recognised by size and SHA-256: the final resolve and the bloom apply (fragment). [Shader hashes]
inline constexpr std::size_t kFinalResolveFsBytes = 16716;
inline constexpr const char* kFinalResolveFsSha256 = "bddaf459f44fe41d16eaf8c672eadf99a15afbce80713b3fb33958f3a1a95e0f";
inline constexpr std::size_t kBloomApplyFsBytes = 3084;
inline constexpr const char* kBloomApplyFsSha256 = "131f984cc9e2bd4fb801f987969d49e92cc6bad16e5c44a89b2384d5b27b6938";
// Vertex output locations of our own, above X-Plane's: the view-to-eye map, the layer. [Shader locations]
inline constexpr std::uint32_t kEyeMapLocation = 15;
inline constexpr std::uint32_t kLayerLocation = 20;

// ---- Eye columns: where a view's height comes from the stereo image or the VR context. [Eye columns]
inline constexpr std::uintptr_t kRenderSizeHeight = 0xc83b3c;
inline constexpr std::uint8_t kRenderSizeHeightAnchor[7] = {0x41, 0x8B, 0x8F, 0x88, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t kWorldSubgraphHeight = 0x198f333;
inline constexpr std::uint8_t kWorldSubgraphHeightAnchor[6] = {0x8B, 0xB8, 0x88, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t kVrContextHeight = 0xc66817;
inline constexpr std::uint8_t kVrContextHeightAnchor[8] = {0x89, 0x5C, 0x24, 0x20, 0x4C, 0x8D, 0x45, 0x10};
inline constexpr std::uintptr_t kVrContextResizeHeightA = 0xc6684f;
inline constexpr std::uint8_t kVrContextResizeHeightAAnchor[9] = {0x41, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x44, 0x8B, 0xC3};
inline constexpr std::uintptr_t kVrContextResizeHeightB = 0xc8665c;
inline constexpr std::uint8_t kVrContextResizeHeightBAnchor[5] = {0x44, 0x8B, 0xC0, 0x8B, 0xC3};
// The single-viewport setter (x, y, w, h; the scissor too). [Single viewport setter]
inline constexpr std::uintptr_t kSetViewport = 0x104a500;
inline constexpr std::uint8_t kSetViewportAnchor[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
// The VR context's pointer, and its width and height. [VR context]
inline constexpr std::uintptr_t kVrContextPointer = 0x5777c60;
inline constexpr std::size_t kContextWidth = 4;
inline constexpr std::size_t kContextHeight = 8;

}  // namespace xplane

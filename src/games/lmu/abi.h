// Le Mans Ultimate 1.4.2.0 (game.h kBuilds[0]): RVAs and anchor bytes. One line per group; the why is in
// docs/recon-lmu.md "## abi.h". The anchors are the code's bytes in memory.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lmu {

// The renderer's per-view render, entered once per pipeline view; its sub-views run in its "Main View(s)" loop. [Render view]
inline constexpr std::uintptr_t kRenderView = 0x5df6f0;
inline constexpr std::uint8_t kRenderViewAnchor[] = {0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20, 0x4c,
                                                     0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55};
inline constexpr std::size_t kViewFlags = 0x0;           // uint: bit 30, the camera is linked (kViewLinkedCamera)
inline constexpr std::size_t kViewSubViews = 0x4d0;      // std::vector of kSubViewSize records (begin, end)
inline constexpr std::size_t kSubViewSize = 0x94;        // +0 enabled (byte)
inline constexpr std::size_t kViewLinkedCamera = 0x550;  // the camera used when flags bit 30 is set
inline constexpr std::size_t kCameraEye = 0x5cc;         // int: the eye index the view params get

// cubeDirectX11 (the renderer; RTTI): vtable slot 248, SetViewParams(this, params) copies params to this + 0x77388. [View params]
inline constexpr std::uintptr_t kSetViewParamsSlot = 0x1114ff0 + 248 * 8;
inline constexpr std::uintptr_t kSetViewParams = 0x5ad9d0;
inline constexpr std::size_t kParamsEye = 0x0;   // int
inline constexpr std::size_t kParamsNear = 0x8;  // float
inline constexpr std::size_t kParamsFar = 0xc;   // float
inline constexpr std::size_t kParamsView = 0x2c;        // 4x4: the view matrix SetViewParams hands YEBIS (row vectors)
inline constexpr std::size_t kParamsProjection = 0x94;  // 4x4: the projection it hands YEBIS

// The VR eye loop in the game's frame: its head hands the camera update the eye (edi), its tail counts to 2. [Eye loop]
inline constexpr std::uintptr_t kEyeLoopHead = 0xcd15f0;
inline constexpr std::uint8_t kEyeLoopHeadAnchor[] = {0x44, 0x8b, 0xc7, 0xb2, 0x01};  // mov r8d, edi; mov dl, 1
inline constexpr std::uintptr_t kEyeLoopTail = 0xcd17c1;
inline constexpr std::uint8_t kEyeLoopTailAnchor[] = {0xff, 0xc7, 0x48, 0x83, 0xc3, 0x6c,  // inc edi; add rbx, 0x6c
                                                      0x83, 0xff, 0x02};                     // cmp edi, 2
inline constexpr std::size_t kCameraHistorySize = 0x6c;   // rbx: this eye's record (two on the stack)
inline constexpr std::uintptr_t kCameraHistoryCtor = 0x3695e0;
inline constexpr std::uint8_t kCameraHistoryCtorAnchor[] = {0x33, 0xc0, 0x48, 0xc7, 0x41, 0x04, 0x00, 0x00, 0x80, 0x3f};

// One eye's scene render (arg, later eye): a later eye skips the frame's shared Pre Render (shadows, cubemaps). [Scene render]
inline constexpr std::uintptr_t kRenderScene = 0xd58a70;
inline constexpr std::uint8_t kRenderSceneAnchor[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18};

// The renderer and the eye its view params hold, its current eye target, and that texture's D3D11 object. [Eye target]
inline constexpr std::uintptr_t kRenderer = 0x1b12be8;      // cubeDirectX11*
inline constexpr std::size_t kRendererViewEye = 0x77388;    // int: the view params' eye (SetViewParams copies them here)
inline constexpr std::uintptr_t kGetCurrentEyeTarget = 0x5ad930;  // texture object* (cubeDirectX11*)
inline constexpr std::uint8_t kGetCurrentEyeTargetAnchor[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x01};
inline constexpr std::uintptr_t kTextureSurface = 0x550e60;  // surface* (texture object*, usage)
inline constexpr std::uint8_t kTextureSurfaceAnchor[] = {0x85, 0xd2, 0x75, 0x02, 0x8b, 0x11, 0x0f, 0xba, 0xe2, 0x09};
inline constexpr unsigned kRenderTargetUsage = 0x2000;
inline constexpr std::uintptr_t kSurfaceD3D = 0x605220;      // the surface's D3D11 object (not AddRef'd)
inline constexpr std::uint8_t kSurfaceD3DAnchor[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8};

// The per-eye projections VR_UpdateEyes writes each frame; each pass's camera update copies its eye's into the camera. [Eye projection slots]
inline constexpr std::uintptr_t kEyeProjection = 0x1b1bf74;     // eye e at + e * kEyeSlotStride: the camera's +0x360
inline constexpr std::uintptr_t kEyeProjectionFar = 0x1b1c04c;  // the same with a fixed far plane: the camera's +0x58c
inline constexpr std::size_t kEyeSlotStride = 0x6c;
inline constexpr std::size_t kSlotMatrix = 4;  // the row-major 4x4 (left-handed, row vectors) after an int
// The eye render textures (texture objects), eye e at + e * 8. [Eye textures]
inline constexpr std::uintptr_t kEyeTextures = 0x1b1c740;
// The render scale (float): the main targets are recommended x it, the eye targets recommended; GetRenderSize reads it
// each call, so a pass renders into the top-left ceil(recommended x scale) of the launch-size targets (probe run
// 2026-11-07); the final blit or FSR stretches the main target over the eye target (focus_math.h). [Live resolution]
inline constexpr std::uintptr_t kRenderScale = 0x199cc18;
inline constexpr std::uintptr_t kRecommendedSize = 0x1b1c654;  // uint32 width, height (the runtime's, as offered)
// The frame's eye render size (float width, height = ceil(recommended x scale)), set once per frame by VR_UpdateEyes and
// copied into the camera at each pass's head (with the VR state block, 0x418 bytes from 0x1b1be30): a pass renders at
// the size this holds when its camera is set up, not at the scale in effect during the pass (live run 2026-11-08).
inline constexpr std::uintptr_t kEyeRenderSize = 0x1b1c240;
// The renderer's FSR 1 (FSR.cpp, made at device init); method != 0: the post's end upscales, no final blit. [FSR]
inline constexpr std::size_t kRendererFsr = 0x1778;  // FSR object*
inline constexpr std::size_t kFsrMethod = 0x40;      // int: UpscaleMethod (0 off)

// VRHiddenAreaMask::Draw(this, camera): the lens mask in the eye's screen space, mesh by camera eye (+0x5cc). [Hidden-area mask]
inline constexpr std::uintptr_t kHiddenAreaDraw = 0x6188d0;
inline constexpr std::uint8_t kHiddenAreaDrawAnchor[] = {0x48, 0x8b, 0xc4, 0x41, 0x54, 0x41, 0x55,
                                                         0x48, 0x83, 0xec, 0x68, 0x80};

// A view's scene work before cubeDX11_ApplyPostProcess (renderer vtable 0xb00: the MSAA resolve, YEBIS, the eye final),
// all called from Pipeline_RenderView and child views only: skipped in passes 2-3 once the eye passes drew the focus views. [MVR]
inline constexpr std::uintptr_t kViewScene = 0x51d8a0;  // Renderer_RenderViewScene(renderer, scene, view, flags, last): every bucket's draws
inline constexpr std::uint8_t kViewSceneAnchor[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
inline constexpr std::uintptr_t kSortMeshBucket = 0x5ddc70;  // SortMesh::Process for one bucket (6 args; the count it returns unused)
inline constexpr std::uint8_t kSortMeshBucketAnchor[] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18};
inline constexpr std::uintptr_t kCullItem = 0x5e1140;  // Cull_Item(item, view, ?, float): recursive, void
inline constexpr std::uint8_t kCullItemAnchor[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x48};
inline constexpr std::uintptr_t kCullSceneGraph = 0x614d70;  // Cull_SceneGraph(graph, view, ?): void
inline constexpr std::uint8_t kCullSceneGraphAnchor[] = {0x4c, 0x8b, 0xdc, 0x55, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x49, 0x8d};

// The VR UI (VRUI.cpp): VR_Present's draw of the HUD canvas onto a curved panel in each eye's final, after the eye loop. [VR UI]
inline constexpr std::uintptr_t kVruiDraw = 0x617090;  // (vrui, context, eyes): eyes = {int mode; 2 kEyeSlotStride slots}
inline constexpr std::uint8_t kVruiDrawAnchor[] = {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x02};
inline constexpr std::uintptr_t kVruiDrawEye = 0x617fb0;  // (vrui, context, eyes, eye): one eye's panel
inline constexpr std::uint8_t kVruiDrawEyeAnchor[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57};
inline constexpr std::size_t kVruiEyeRtv = 0xd8;     // ID3D11RenderTargetView*: eye e's at + e * 8
inline constexpr std::size_t kVruiViewport = 0xbc;   // int width, height: the panel draws' viewport (the eye targets')
inline constexpr std::size_t kVruiEyesSize = 0xdc;   // the eyes block: mode, then each eye's projection x view slot
inline constexpr std::size_t kVruiEyeMatrix = 8;     // eye e's 4x4 at + e * kEyeSlotStride (right-handed projection)

// DriverLabels' draw callback (camera, labels), in each camera's after-post list (+0x480): its viewport is the whole eye
// target, not the part the pass rendered (tag_viewport.h). [Driver labels]
inline constexpr std::uintptr_t kLabelsDraw = 0xe78a90;
inline constexpr std::uint8_t kLabelsDrawAnchor[] = {0x48, 0x8b, 0xc2, 0x4c, 0x8b, 0xc1, 0x48, 0x8b, 0x92, 0xc8,
                                                     0x06, 0x00, 0x00, 0x48, 0x8b, 0x88, 0xd8, 0x06, 0x00, 0x00};

// YEBIS CPostEffect::SetDeltaTime(this, seconds): each pass's post-processing sets it; auto exposure adapts by it. [Post-effect time]
inline constexpr std::uintptr_t kPostEffectSetDeltaTime = 0x924b10;
inline constexpr std::uint8_t kPostEffectSetDeltaTimeAnchor[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30,
                                                                 0x83, 0x3d, 0x8f, 0x2e, 0x0c, 0x01};

// YEBIS CRenderGlare::GenerateGlare_Composite(this): a pass's glare combined into the result the tonemap adds. [Glare share]
inline constexpr std::uintptr_t kGlareComposite = 0x994240;
inline constexpr std::uint8_t kGlareCompositeAnchor[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70,
                                                         0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55,
                                                         0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xe8, 0xfe, 0xff,
                                                         0xff, 0x48, 0x81, 0xec, 0x30, 0x02, 0x00, 0x00};
inline constexpr std::size_t kGlareResult = 0x308;        // CRenderTexture*: the composite's result
inline constexpr std::size_t kRenderTextureSize = 0x60;   // uint32 width, height
inline constexpr std::size_t kRenderTextureScale = 0x90;  // float scale x, y, offset x, y: the top-left region in use
inline constexpr std::size_t kRenderTextureGpu = 0xb0;    // IGPUTexture*
inline constexpr std::size_t kGpuTextureResource = 0x38;  // ID3D11Resource* (its view's resource)

}  // namespace lmu

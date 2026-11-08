// Le Mans Ultimate 1.4.2.0 (game.h kBuilds[0]): RVAs and anchor bytes. One line per group; the why is in
// docs/recon-lmu.md "## abi.h". The exe's code is SteamStub-encrypted on disk: the anchors are the decrypted bytes.
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

// VRHiddenAreaMask::Draw(this, camera): the lens mask in the eye's screen space, mesh by camera eye (+0x5cc). [Hidden-area mask]
inline constexpr std::uintptr_t kHiddenAreaDraw = 0x6188d0;
inline constexpr std::uint8_t kHiddenAreaDrawAnchor[] = {0x48, 0x8b, 0xc4, 0x41, 0x54, 0x41, 0x55,
                                                         0x48, 0x83, 0xec, 0x68, 0x80};

// The VR UI (VRUI.cpp): VR_Present's draw of the HUD canvas onto a curved panel in each eye's final, after the eye loop. [VR UI]
inline constexpr std::uintptr_t kVruiDraw = 0x617090;  // (vrui, context, eyes): eyes = {int mode; 2 kEyeSlotStride slots}
inline constexpr std::uint8_t kVruiDrawAnchor[] = {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x02};
inline constexpr std::uintptr_t kVruiDrawEye = 0x617fb0;  // (vrui, context, eyes, eye): one eye's panel
inline constexpr std::uint8_t kVruiDrawEyeAnchor[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57};
inline constexpr std::size_t kVruiEyeRtv = 0xd8;     // ID3D11RenderTargetView*: eye e's at + e * 8
inline constexpr std::size_t kVruiEyesSize = 0xdc;   // the eyes block: mode, then each eye's projection x view slot
inline constexpr std::size_t kVruiEyeMatrix = 8;     // eye e's 4x4 at + e * kEyeSlotStride (right-handed projection)

// YEBIS CPostEffect::SetDeltaTime(this, seconds): each pass's post-processing sets it; auto exposure adapts by it. [Post-effect time]
inline constexpr std::uintptr_t kPostEffectSetDeltaTime = 0x924b10;
inline constexpr std::uint8_t kPostEffectSetDeltaTimeAnchor[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30,
                                                                 0x83, 0x3d, 0x8f, 0x2e, 0x0c, 0x01};

}  // namespace lmu

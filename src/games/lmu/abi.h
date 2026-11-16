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

// The renderer and the eye its view params hold. [Eye target]
inline constexpr std::uintptr_t kRenderer = 0x1b12be8;      // cubeDirectX11*
inline constexpr std::size_t kRendererViewEye = 0x77388;    // int: the view params' eye (SetViewParams copies them here)

// The per-eye projections VR_UpdateEyes writes each frame; each pass's camera update copies its eye's into the camera. [Eye projection slots]
inline constexpr std::uintptr_t kEyeProjection = 0x1b1bf74;     // eye e at + e * kEyeSlotStride: the camera's +0x360
inline constexpr std::uintptr_t kEyeProjectionFar = 0x1b1c04c;  // the same with a fixed far plane: the camera's +0x58c
inline constexpr std::size_t kEyeSlotStride = 0x6c;
inline constexpr std::size_t kSlotMatrix = 4;  // the row-major 4x4 (left-handed, row vectors) after an int
// The game's render scale (float, read only): main targets = recommended x it, the eye targets recommended. [Live resolution]
inline constexpr std::uintptr_t kRenderScale = 0x199cc18;
inline constexpr std::uintptr_t kRecommendedSize = 0x1b1c654;  // uint32 width, height (the runtime's, as offered)
// The renderer's FSR 1 (FSR.cpp, made at device init); method != 0: the post's end upscales, no final blit. [FSR]
inline constexpr std::size_t kRendererFsr = 0x1778;  // FSR object*
inline constexpr std::size_t kFsrMethod = 0x40;      // int: UpscaleMethod (0 off)

// YEBIS CRenderGlare: a pass's glare combined into the result the tonemap adds (read only). [Glare share]
inline constexpr std::size_t kGlareResult = 0x308;        // CRenderTexture*: the composite's result
inline constexpr std::size_t kRenderTextureSize = 0x60;   // uint32 width, height
inline constexpr std::size_t kRenderTextureScale = 0x90;  // float scale x, y, offset x, y: the top-left region in use
inline constexpr std::size_t kRenderTextureGpu = 0xb0;    // IGPUTexture*
inline constexpr std::size_t kGpuTextureResource = 0x38;  // ID3D11Resource* (its view's resource)

}  // namespace lmu

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

}  // namespace lmu

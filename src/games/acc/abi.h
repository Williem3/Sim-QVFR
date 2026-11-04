// Assetto Corsa Competizione facts from static analysis (README.md). Pinned to the exe build in game.h.
// ACC is UE 4.26; its stereo device is the SteamVR plugin's FSteamVRHMD (no symbols, no RTTI). Each slot
// below holds, in this build, the function given next to it: patches check that before writing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace acc {

struct Slot {
    std::uintptr_t rva;       // the vtable slot
    std::uintptr_t function;  // what it holds in this build
};

// FSteamVRHMD's IStereoRendering vtable (subobject +0xA0), slots in the order of 4.26's StereoRendering.h.
inline constexpr std::uintptr_t kStereoVtable = 0x3a4f070;
inline constexpr Slot kGetDesiredNumberOfViews = {kStereoVtable + 4 * 8, 0x7b5e60};  // default: stereo ? 2 : 1
inline constexpr Slot kGetViewPassForIndex = {kStereoVtable + 5 * 8, 0x7b7fb0};      // default: 1 + (index != 0)
inline constexpr Slot kGetViewIndexForPass = {kStereoVtable + 6 * 8, 0x7b7f90};      // default: 0, 1, else -1
inline constexpr Slot kAdjustViewRect = {kStereoVtable + 16 * 8, 0x7ae650};          // halves of the target
inline constexpr Slot kGetStereoProjectionMatrix = {kStereoVtable + 20 * 8, 0x7b75e0};  // non-left = right eye
// FSteamVRHMD's IXRTrackingSystem vtable (primary, +0x00): GetRelativeEyePose answers passes 1-2 only.
inline constexpr std::uintptr_t kTrackingVtable = 0x3a4ecc0;
inline constexpr Slot kGetRelativeEyePose = {kTrackingVtable + 12 * 8, 0x7b7290};

// r.AllowSubPrimitiveQueries (4.26 SceneVisibility.cpp: "currently only used by hierarchical instanced static
// meshes"): the static int32 the cvar wraps. Foliage's per-cluster occlusion results crash with the side views
// on track (README.md, stage gate runs), so views.cpp sets it to 0: one query per foliage proxy instead.
inline constexpr std::uintptr_t kAllowSubPrimitiveQueries = 0x4c4bc94;  // .data, 1 at load
// Its registration (static init 0x686c10): `lea r8, [rip + 0x45c5057]`, the int passed to
// IConsoleManager::RegisterConsoleVariableRef, next to `lea rdx, u"r.AllowSubPrimitiveQueries"` (0x3e70308).
inline constexpr std::uintptr_t kAllowSubPrimitiveQueriesRef = 0x686c36;
inline constexpr std::uint8_t kAllowSubPrimitiveQueriesRefAnchor[7] = {0x4c, 0x8d, 0x05, 0x57, 0x50, 0x5c, 0x04};

}  // namespace acc

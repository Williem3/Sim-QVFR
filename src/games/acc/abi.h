// Assetto Corsa Competizione addresses, pinned to the exe build in game.h: patches check that each slot holds its
// function, or each anchor its bytes, before writing. What each patch does and how it was found: docs/recon-acc.md,
// section "abi.h", under the [name] given here (local notes, not in the repository).
#pragma once

#include <cstddef>
#include <cstdint>

namespace acc {

struct Slot {
    std::uintptr_t rva;       // the vtable slot
    std::uintptr_t function;  // what it holds in this build
};

// The stereo device's vtable slots, and the view's fields they go with. [Stereo device]
inline constexpr std::uintptr_t kStereoVtable = 0x3a4f070;
inline constexpr Slot kGetDesiredNumberOfViews = {kStereoVtable + 4 * 8, 0x7b5e60};
inline constexpr Slot kGetViewPassForIndex = {kStereoVtable + 5 * 8, 0x7b7fb0};
inline constexpr Slot kGetViewIndexForPass = {kStereoVtable + 6 * 8, 0x7b7f90};
inline constexpr Slot kDeviceIsAPrimaryPass = {kStereoVtable + 9 * 8, 0x7b1c90};
inline constexpr Slot kDeviceIsAPrimaryView = {kStereoVtable + 10 * 8, 0x2da8900};
inline constexpr std::ptrdiff_t kViewStereoPass = 0xad0;
inline constexpr std::ptrdiff_t kViewProjection = 0x280;  // with the temporal jitter
inline constexpr std::ptrdiff_t kViewProjectionNoAA = 0x2c0;
inline constexpr std::ptrdiff_t kViewMatrix = 0x340;
inline constexpr Slot kAdjustViewRect = {kStereoVtable + 16 * 8, 0x7ae650};
inline constexpr Slot kSetFinalViewRect = {kStereoVtable + 17 * 8, 0x776df0};
inline constexpr Slot kGetStereoProjectionMatrix = {kStereoVtable + 20 * 8, 0x7b75e0};
inline constexpr std::uintptr_t kTrackingVtable = 0x3a4ecc0;
inline constexpr Slot kGetRelativeEyePose = {kTrackingVtable + 12 * 8, 0x7b7290};

// The HMD's visible-area mesh check: its meshes are kept for two views only. [Visible-area mesh]
inline constexpr std::uintptr_t kHmdVtable = 0x3a4eec0;
inline constexpr Slot kHasVisibleAreaMesh = {kHmdVtable + 35 * 8, 0x7b8380};

// r.AllowSubPrimitiveQueries: the int it wraps, and its registration. [Sub-primitive queries]
inline constexpr std::uintptr_t kAllowSubPrimitiveQueries = 0x4c4bc94;
inline constexpr std::uintptr_t kAllowSubPrimitiveQueriesRef = 0x686c36;
inline constexpr std::uint8_t kAllowSubPrimitiveQueriesRefAnchor[7] = {0x4c, 0x8d, 0x05, 0x57, 0x50, 0x5c, 0x04};

// Console variables held at a value while QVFR runs: bloom and lens flares off. [Held console variables]
struct HeldCvar {
    const char* name;
    std::uintptr_t ref;
    std::uint8_t refAnchor[7];
    std::uintptr_t store;
    std::uint8_t storeAnchor[7];
    std::uintptr_t data;
    int value;
};
inline constexpr HeldCvar kHeldCvars[] = {
    {"r.BloomQuality", 0x617514, {0x48, 0x8d, 0x15, 0x9d, 0x51, 0x60, 0x03}, 0x617543,
     {0x48, 0x89, 0x05, 0x86, 0xb0, 0x9f, 0x04}, 0x50125d0, 0},
    {"r.LensFlareQuality", 0x672e94, {0x48, 0x8d, 0x15, 0x4d, 0xbd, 0x7c, 0x03}, 0x672ec3,
     {0x48, 0x89, 0x05, 0xae, 0x10, 0xa9, 0x04}, 0x5103f78, 0},
};
// Switched off live (acc_diag=1) or held off (contact shadows): SSR, AO, the sun's shadows, contact shadows. [Diagnostic cvars]
inline constexpr HeldCvar kDiagCvars[] = {
    {"r.SSR.Quality", 0x687dc4, {0x48, 0x8d, 0x15, 0x05, 0x73, 0x7e, 0x03}, 0x687df3,
     {0x48, 0x89, 0x05, 0xfe, 0x11, 0xaa, 0x04}, 0x5128ff8, 0},
    {"r.AmbientOcclusionLevels", 0x645d54, {0x48, 0x8d, 0x15, 0xcd, 0x37, 0x7b, 0x03}, 0x645d83,
     {0x48, 0x89, 0x05, 0x16, 0xc7, 0xa4, 0x04}, 0x50924a0, 0},
    {"r.ShadowQuality", 0x619884, {0x48, 0x8d, 0x15, 0xfd, 0x3c, 0x70, 0x03}, 0x6198b3,
     {0x48, 0x89, 0x05, 0x76, 0x8d, 0x9f, 0x04}, 0x5012630, 0},
    {"r.ContactShadows", 0x617684, {0x48, 0x8d, 0x15, 0x75, 0x79, 0x70, 0x03}, 0x6176b3,
     {0x48, 0x89, 0x05, 0x96, 0xb0, 0x9f, 0x04}, 0x5012750, 0},
};

// Auto exposure: every view takes the left eye's. [Auto exposure]
inline constexpr std::uintptr_t kExposureSecondary = 0x2cebde5;
inline constexpr std::uint8_t kExposureSecondaryAnchor[5] = {0xe8, 0x96, 0x2e, 0x0c, 0x00};
inline constexpr std::uintptr_t kExposurePrimary[2] = {0x1ad45e2, 0x1ad7d2b};
inline constexpr std::uint8_t kExposurePrimaryAnchor[2][5] = {{0xe8, 0x39, 0xa6, 0x2d, 0x01}, {0xe8, 0xf0, 0x6e, 0x2d, 0x01}};
// Pre-exposure: the left eye's reaches the other views. [Pre-exposure]
inline constexpr std::uintptr_t kPreExposurePrimary[2] = {0x1c5df37, 0x1c5df9f};
inline constexpr std::uint8_t kPreExposurePrimaryAnchor[2][5] = {{0xe8, 0xe4, 0x0c, 0x15, 0x01},
                                                                 {0xe8, 0x7c, 0x0c, 0x15, 0x01}};
inline constexpr std::uintptr_t kPreExposureSecondary = 0x1c5df6f;
inline constexpr std::uint8_t kPreExposureSecondaryAnchor[5] = {0xe8, 0x0c, 0x0d, 0x15, 0x01};

// The sun's cascaded shadows: only the left eye (or a mono view) creates them. [Shadow cascades]
inline constexpr std::uintptr_t kShadowCascadesCreate = 0x1c5dfb9;
inline constexpr std::uint8_t kShadowCascadesCreateAnchor[11] = {0x41, 0x83, 0xbe, 0xd0, 0x0a, 0x00,
                                                                  0x00, 0x02, 0x0f, 0x95, 0xc0};
// Shadow projection: all four views project the left eye's cascades. [Shadow visibility]
inline constexpr std::uintptr_t kShadowVisibilitySecondary = 0x1c71363;
inline constexpr std::uint8_t kShadowVisibilitySecondaryAnchor[5] = {0xe8, 0x18, 0xd9, 0x13, 0x01};
inline constexpr std::uintptr_t kShadowVisibilityPrimary = 0x1c71370;
inline constexpr std::uint8_t kShadowVisibilityPrimaryAnchor[5] = {0xe8, 0xab, 0xd8, 0x13, 0x01};

// The tonemapper's vignette, off in every view while QVFR runs. [Vignette]
inline constexpr std::uintptr_t kTonemapperVignette = 0x1b31875;
inline constexpr std::uint8_t kTonemapperVignetteAnchor[10] = {0x0f, 0x2f, 0x87, 0xf0, 0x11, 0x00, 0x00, 0x0f, 0x92, 0xc0};

// Views per instanced draw: 2, for two instanced pairs. [Views per instanced draw]
struct CountRead {
    std::uintptr_t rva;
    std::uint8_t size;  // 3, or 4 with REX.R (r8d-r15d)
    std::uint8_t bytes[4];
};
inline constexpr CountRead kInstancedViewCounts[] = {
    {0x18648b1, 3, {0x8b, 0x48, 0x08}},       {0x1864a2a, 4, {0x44, 0x8b, 0x70, 0x08}},
    {0x18ac4f8, 3, {0x8b, 0x48, 0x08}},       {0x18ac739, 3, {0x8b, 0x48, 0x08}},
    {0x18ac9a8, 3, {0x8b, 0x48, 0x08}},       {0x18acbee, 3, {0x8b, 0x48, 0x08}},
    {0x19ef921, 3, {0x8b, 0x78, 0x08}},       {0x19efb81, 3, {0x8b, 0x78, 0x08}},
    {0x1a094b1, 3, {0x8b, 0x48, 0x08}},       {0x1b95540, 3, {0x8b, 0x48, 0x08}},
    {0x1bc08aa, 3, {0x8b, 0x78, 0x08}},       {0x1bc0a7d, 3, {0x8b, 0x78, 0x08}},
    {0x1bc0cd4, 3, {0x8b, 0x78, 0x08}},       {0x1bc0eb0, 3, {0x8b, 0x78, 0x08}},
    {0x1cc3dbf, 3, {0x8b, 0x48, 0x08}},       {0x1cc3f4c, 4, {0x44, 0x8b, 0x68, 0x08}},
    {0x1cd853f, 3, {0x8b, 0x48, 0x08}},       {0x1cd876c, 3, {0x8b, 0x48, 0x08}},
    {0x1cf35a8, 3, {0x8b, 0x48, 0x08}},       {0x2bef0f6, 4, {0x44, 0x8b, 0x78, 0x08}},
    {0x2befafa, 3, {0x8b, 0x43, 0x08}},
};
// The view constants' views per instanced draw, clamped to 2. [Instanced count in the view constants]
inline constexpr std::uintptr_t kViewInstancedCount = 0x2d085ab;
inline constexpr std::uint8_t kViewInstancedCountAnchor[7] = {0x48, 0x8b, 0xbd, 0xe8, 0x00, 0x00, 0x00};

// The view constants' stereo pass index: 0, 1, 0, 1. [Stereo pass index]
inline constexpr std::uintptr_t kStereoPassIndexCall = 0x1c11f29;
inline constexpr std::uint8_t kStereoPassIndexCallAnchor[15] = {0x41, 0x8b, 0x96, 0xd0, 0x0a, 0x00, 0x00, 0xff,
                                                                0x50, 0x30, 0xeb, 0x03, 0x41, 0x8b, 0xc5};
inline constexpr std::uintptr_t kStereoPassIndexStore = 0x1c11f38;
inline constexpr std::uint8_t kStereoPassIndexStoreAnchor[6] = {0x89, 0x86, 0x5c, 0x0c, 0x00, 0x00};

// The instanced view's constants: each pair's own second view. [Instanced view constants]
struct RightEyePick {
    std::uintptr_t rva;
    std::uint8_t view;  // the register holding the view there (x64 number)
    bool passInEax;
};
inline constexpr RightEyePick kRightEyePicks[] = {
    {0x18d20d8, 6, false},  {0x18f6a99, 14, false}, {0x1916f2f, 3, false},  {0x19d15f4, 15, false},
    {0x19d2133, 6, false},  {0x1bfa6b8, 1, false},  {0x1cc4f01, 14, false}, {0x1cc5b51, 14, false},
    {0x1ccc9eb, 6, true},   {0x1d0236b, 14, false}, {0x24fc2df, 14, false}, {0x24fd2e1, 15, false},
};
inline constexpr std::uintptr_t kGetStereoEyeView = 0x2cf8140;

// Foliage culling: each pair's primary culls with its own left plane and its partner's right. [Foliage culling]
inline constexpr std::uintptr_t kFoliageUnionGate = 0x2a435f2;
inline constexpr std::uint8_t kFoliageUnionGateAnchor[5] = {0x83, 0x7c, 0x24, 0x38, 0x00};
inline constexpr std::uintptr_t kFoliageLeftView = 0x2a43637;
inline constexpr std::uint8_t kFoliageLeftViewAnchor[7] = {0x48, 0x8b, 0x01, 0x41, 0x0f, 0x28, 0xe7};
inline constexpr std::uintptr_t kFoliageRightView = 0x2a4366a;
inline constexpr std::uint8_t kFoliageRightViewAnchor[7] = {0x48, 0x63, 0x46, 0x08, 0x0f, 0x59, 0xc7};
inline constexpr std::uintptr_t kFoliageRightViewRead = 0x2a43737;
inline constexpr std::uint8_t kFoliageRightViewReadAnchor[5] = {0x48, 0x8b, 0x4c, 0xc1, 0xf8};

// Instanced stereo's viewport width per pair: FViewInfo fields, the renderer-wide reads, the eye padding. [Instanced width per pair]
inline constexpr std::ptrdiff_t kViewRect = 0x15e0;                  // FViewInfo::ViewRect (min x, min y, max x, max y)
inline constexpr std::ptrdiff_t kViewInstancedStereoWidth = 0x5910;  // FViewInfo::InstancedStereoWidth
inline constexpr std::ptrdiff_t kViewInfoSize = 0x5920;              // FSceneRenderer::Views' stride
struct WidthRead {
    std::uintptr_t rva;
    std::uint8_t size;      // the anchor's: `mov eax, [renderer + 0x3e0]`
    std::uint8_t anchor[7];
    std::uint8_t viewSize;  // `mov eax, [view + 0x5910]`: written over the anchor, or through a stub when longer
    std::uint8_t view[7];
};
inline constexpr WidthRead kInstancedWidthReads[] = {
    {0x1c0f91f, 6, {0x8b, 0x86, 0xe0, 0x03, 0x00, 0x00}, 6, {0x8b, 0x87, 0x10, 0x59, 0x00, 0x00}},              // rdi
    {0x1c845c8, 6, {0x8b, 0x80, 0xe0, 0x03, 0x00, 0x00}, 7, {0x41, 0x8b, 0x86, 0x10, 0x59, 0x00, 0x00}},        // r14
    {0x1cc2dc7, 7, {0x41, 0x8b, 0x85, 0xe0, 0x03, 0x00, 0x00}, 7, {0x41, 0x8b, 0x86, 0x10, 0x59, 0x00, 0x00}},  // r14
};
inline constexpr std::uintptr_t kEyePaddingViews = 0x1c11e3c;
inline constexpr std::uint8_t kEyePaddingViewsAnchor[7] = {0x48, 0x8b, 0x50, 0x08, 0x48, 0x8b, 0x08};

// Foliage LOD: the focus views take the left eye's projection, on the CPU and in the shader. [Foliage LOD]
inline constexpr std::uintptr_t kFoliageLodProjection[4] = {0x2a446fb, 0x2a44791, 0x2a44f7d, 0x2a45007};
inline constexpr std::uint8_t kFoliageLodProjectionAnchor[7] = {0x4d, 0x8d, 0x87, 0x80, 0x02, 0x00, 0x00};
inline constexpr std::uintptr_t kFoliageShaderLodProjection = 0x2a7e01f;
inline constexpr std::uint8_t kFoliageShaderLodProjectionAnchor[7] = {0x49, 0x81, 0xc6, 0x80, 0x02, 0x00, 0x00};

// The view state's temporal counters: the focus views take their eye's each frame. [Temporal state]
inline constexpr std::ptrdiff_t kViewState = 0x15f0;                  // FViewInfo::ViewState
inline constexpr std::ptrdiff_t kStateFrameIndex = 0x4d8;             // uint32
inline constexpr std::ptrdiff_t kStateTemporalAASampleIndex = 0x4d2;  // int8
inline constexpr std::ptrdiff_t kStateTemporalAASampleCount = 0x4d4;  // uint32, unclamped
inline constexpr std::uintptr_t kViewStateLoad = 0x1c4173f;
inline constexpr std::uint8_t kViewStateLoadAnchor[7] = {0x4c, 0x8b, 0xb7, 0xf0, 0x15, 0x00, 0x00};  // mov r14, [rdi + 0x15f0]
inline constexpr std::uintptr_t kFrameIndexStep = 0x1c4188e;
inline constexpr std::uint8_t kFrameIndexStepAnchor[7] = {0x41, 0xff, 0x86, 0xd8, 0x04, 0x00, 0x00};  // inc [r14 + 0x4d8]
inline constexpr std::uintptr_t kSampleCountStep = 0x1c41966;
inline constexpr std::uint8_t kSampleCountStepAnchor[7] = {0x41, 0xff, 0x86, 0xd4, 0x04, 0x00, 0x00};  // inc [r14 + 0x4d4]
inline constexpr std::uintptr_t kSampleIndexStore = 0x1c4196d;
inline constexpr std::uint8_t kSampleIndexStoreAnchor[7] = {0x45, 0x88, 0xbe, 0xd2, 0x04, 0x00, 0x00};  // mov [r14 + 0x4d2], r15b

}  // namespace acc

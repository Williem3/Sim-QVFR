// Assetto Corsa EVO addresses, pinned to the exe build in game.h: patches check each anchor's bytes before writing.
// What each group is and how it was found: docs/recon-ace.md, section "abi.h", under the [name] given here (local
// notes, not in the repository).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ace {

// A function the probe hooks, with the bytes it checks first. [Probe target]
struct ProbeTarget {
    const char* name;
    std::uintptr_t rva;
    std::size_t anchorSize;
    std::uint8_t anchor[16];
};

// Functions the probe hooks log-only (frame markers, the frame graph, the final write). [Probe targets]
inline constexpr ProbeTarget kProbeTargets[] = {
    {"camera_update", 0x1e06190, 16, {0x4c,0x8b,0xdc,0x55,0x56,0x57,0x49,0x8d,0xab,0xa8,0xfc,0xff,0xff,0x48,0x81,0xec}},
    {"end_frame", 0x1dfc3f0, 16, {0x40,0x55,0x53,0x57,0x48,0x8d,0x6c,0x24,0xb9,0x48,0x81,0xec,0xb0,0x00,0x00,0x00}},
    {"graph_builder", 0x1d84a80, 16, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x54,0x41,0x55,0x41}},
    {"graph_execute_1db19c0", 0x1db19c0, 16, {0x48,0x89,0x5c,0x24,0x10,0x4c,0x89,0x44,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41}},
    {"final_pass_1f83390", 0x1f83390, 16, {0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x70,0x20,0x55,0x57,0x41,0x54,0x41}},
    {"renderer_begin_1d6d7f0", 0x1d6d7f0, 16, {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}},
};
// The per-frame render entry; the double-render experiment hooks it. [Render frame]
inline constexpr ProbeTarget kRenderFrame = {
    "render_frame", 0x896540, 16, {0x48,0x8b,0xc4,0x55,0x41,0x56,0x48,0x83,0xec,0x58,0x80,0xb9,0x89,0x02,0x00,0x00}};

// The renderer's view mode (view count = mode + 1, VR = 1) and frame index. [View mode]
inline constexpr std::ptrdiff_t kRendererActiveViewMode = 0x1d4cdc;
inline constexpr std::ptrdiff_t kRendererConfiguredViewMode = 0x1d4cd8;
inline constexpr std::ptrdiff_t kRendererFrameIndex = 0x1d4628;
// Per-view std::vector members sized at init: {offset, element size}. [Per-view vectors]
struct PerViewVector {
    std::ptrdiff_t offset;
    std::size_t elementSize;
};
inline constexpr PerViewVector kRendererPerViewVectors[] = {{0x1d5188, 0x20}, {0x1d51a0, 0x28}};
// views3 experiment: the VR branch's view-mode immediate. [views3 immediate]
inline constexpr std::uintptr_t kInitVrModeImmediateRva = 0x1d4447c;
inline constexpr std::uint8_t kInitVrModeAnchor[16] = {0xbb, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x47,
                                                       0x08, 0x48, 0x8b, 0x08, 0x80, 0xb9, 0x55, 0x01};

// views4: the renderer's DLSS-ready flag and its per-view DLSS features. [DLSS state]
inline constexpr std::ptrdiff_t kRendererDlssReady = 0x1d5171, kRendererDlssFeatures = 0x1d5188;
// viewCount(renderer). [View count]
inline constexpr std::uintptr_t kViewCountRva = 0x1dc7f50;
inline constexpr std::uint8_t kViewCountAnchor[9] = {0x8b, 0x81, 0xdc, 0x4c, 0x1d, 0x00, 0xff, 0xc0, 0xc3};
// viewport(renderer, out, renderSize, view): one view's viewport, scissor and UV transform. [Viewport]
inline constexpr std::uintptr_t kViewportRva = 0x1d922b0;
inline constexpr std::uint8_t kViewportAnchor[16] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                     0x24, 0x18, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56};
// Init-time callers that size per-view resources from view 0's viewport. [Init viewport callers]
inline constexpr std::uintptr_t kTileGridViewportCallSite = 0x1d97049;
inline constexpr std::uintptr_t kFogViewportCallSite = 0x1da1edf;
// The frame-graph builder's two viewport calls: render size (DLSS input) and output size. [Builder viewports]
inline constexpr std::uintptr_t kBuilderRenderViewportCallSite = 0x1d8828c;
inline constexpr std::uintptr_t kBuilderOutputViewportCallSite = 0x1d882e7;
// view(renderer, i): the per-view record. [View record]
inline constexpr std::uintptr_t kViewRecordRva = 0x1d92f50;
inline constexpr std::uint8_t kViewRecordAnchor[16] = {0x48, 0x63, 0xc2, 0x48, 0x8d, 0x04, 0x80, 0x48,
                                                       0xc1, 0xe0, 0x06, 0x48, 0x03, 0x81, 0xb8, 0x07};
// The renderer's three camera slots. [Camera block]
inline constexpr std::ptrdiff_t kCameraSlotBase = 0x1d4ce0;
inline constexpr std::ptrdiff_t kCameraSlotStride = 0x184;
// Sites that index the camera slots by view. [Camera slot index]
struct ImulSite {
    std::uintptr_t rva;
    std::uint8_t anchor[7];
};
inline constexpr ImulSite kCameraSlotIndexSites[] = {
    {0x1d85cd3, {0x49, 0x69, 0xff, 0x84, 0x01, 0x00, 0x00}},  // builder: camera slot -> view record
    {0x1dc4e84, {0x4d, 0x69, 0xf4, 0x84, 0x01, 0x00, 0x00}},  // per-view pass setup
    {0x1dc59cb, {0x4d, 0x69, 0xc4, 0x84, 0x01, 0x00, 0x00}},
    {0x1dc6c93, {0x49, 0x69, 0xdf, 0x84, 0x01, 0x00, 0x00}},
    {0x1dbd4f0, {0x4c, 0x69, 0xc0, 0x84, 0x01, 0x00, 0x00}},  // reads [pass+0x9ac] as the view
};
// Per-view pass setups and the pass viewport and scissor they write. [Pass setup]
inline constexpr std::uintptr_t kPassSetupRvas[] = {0x1dc4b70, 0x1dc68e0};
inline constexpr std::uint8_t kPassSetupAnchors[][16] = {
    {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56},
    {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8}};
inline constexpr std::ptrdiff_t kPassViewport = 0x934, kPassScissor = 0x94c;
// The pass's renderPassData screenOffset. [Pass screen offset]
inline constexpr std::ptrdiff_t kPassScreenOffset = 0x764;
// The pass's renderPassData screenSizeDiv. [Pass screen size]
inline constexpr std::ptrdiff_t kPassScreenSizeDiv = 0x76c;
// The pass setup's eye branch and where its paths rejoin. [Pass eye branch]
inline constexpr std::uintptr_t kPassEyeBranchRva = 0x1dc5052, kPassEyeBranchTarget = 0x1dc532d;
inline constexpr std::uint8_t kPassEyeBranchAnchor[9] = {0x45, 0x85, 0xe4, 0x0f, 0x85, 0xd2, 0x02, 0x00, 0x00};
inline constexpr std::uintptr_t kPassRejoinRva = 0x1dc5658;
inline constexpr std::uint8_t kPassRejoinAnchor[6] = {0x89, 0x86, 0x4c, 0x04, 0x00, 0x00};  // mov [rsi+0x44c], eax
// Fields of the stand-in renderer views4 points the pass setup at. [Phantom renderer]
inline constexpr std::ptrdiff_t kRendererField8 = 0x8, kPhantomRenderer = 0x10;
// The builder's per-view loops: view-count call sites and the lighting loop's bound. [Builder loops]
inline constexpr std::uintptr_t kDepthLoopViewCountCallSites[] = {0x1d870d8, 0x1d875ae};
inline constexpr std::uintptr_t kLightingLoopBoundRva = 0x1d88090;
inline constexpr std::uint8_t kLightingLoopBoundAnchor[9] = {0x41, 0x8b, 0x85, 0xdc, 0x4c, 0x1d, 0x00, 0xff, 0xc0};
// The lighting loop's new bound, and the loops' history-slot index sites. [Lighting loop bound]
inline constexpr std::uint8_t kLightingLoopBoundStub[] = {
    0x41, 0x8b, 0x85, 0xdc, 0x4c, 0x1d, 0x00,  // mov eax, [r13 + 0x1d4cdc]
    0x83, 0xf8, 0x01,                          // cmp eax, 1
    0x75, 0x05,                                // jne +5
    0xb8, 0x03, 0x00, 0x00, 0x00,              // mov eax, 3
    0xff, 0xc0,                                // inc eax
};
inline constexpr ImulSite kHistorySlotIndexSites[] = {
    {0x1d870e5, {0x48, 0x63, 0xde, 0x48, 0xc1, 0xe3, 0x05}},  // loop A: rbx = view << 5
    {0x1d87354, {0x48, 0x63, 0xce, 0x48, 0xc1, 0xe1, 0x05}},  // loop A: +0x75e8
    {0x1d874ba, {0x48, 0x63, 0xce, 0x48, 0xc1, 0xe1, 0x05}},  // loop A: +0x7648
    {0x1d87613, {0x48, 0x63, 0xdf, 0x48, 0xc1, 0xe3, 0x05}},  // loop B: rbx = view << 5
    {0x1d87f69, {0x48, 0x63, 0xcf, 0x48, 0xc1, 0xe1, 0x05}},  // loop B: +0x14a318
    {0x1d8804c, {0x48, 0x63, 0xcf, 0x48, 0xc1, 0xe1, 0x05}},  // loop B: +0x14a378
};
// Reads of the pass's view that views4 folds onto the eyes. [Pass view reads]
struct ViewReadSite {
    std::uintptr_t rva;
    std::uint8_t anchor[6];
};
inline constexpr ViewReadSite kPassViewReadSites[] = {
    {0x1dbe211, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},
    {0x1dbe22d, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},
    {0x1dbe242, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},
    {0x1dbe25e, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},
    {0x1dbed0c, {0x8b, 0x93, 0xac, 0x09, 0x00, 0x00}},
};
// Tiled light culling's view-count call sites (views4 "tiles"). [Tile culling view count]
inline constexpr std::uintptr_t kTileCullingViewCountCallSites[] = {
    0x1d96fc8, 0x1d96fe0, 0x1d96ffc, 0x1d970a6, 0x1d97673,
    0x1db880a, 0x1db8fb7, 0x1db8fe6, 0x1db95ef,
};
// Pass registration's view index, folded onto the eyes without "tiles". [Pass registration]
inline constexpr std::uintptr_t kVelocityPassViewRva = 0x1da7b65;
inline constexpr std::uint8_t kVelocityPassViewAnchor[6] = {0x49, 0x63, 0xe8, 0x48, 0x8b, 0xda};
inline constexpr std::uint8_t kVelocityPassViewStub[] = {0x41, 0x8b, 0xe8, 0x83, 0xe5, 0x01, 0x48, 0x8b, 0xda};
// History-slot advance calls in the builder loops. [Slot advance]
inline constexpr std::uintptr_t kSlotAdvanceRva = 0x1dc3fe0;
inline constexpr int kSlotFrameFlag = 0x18;
struct SlotAdvanceSite {
    std::uintptr_t rva;
    std::uint8_t anchor[5];
    std::uint8_t viewRegister;  // 6 = esi, 7 = edi
};
inline constexpr SlotAdvanceSite kSlotAdvanceSites[] = {
    {0x1d870f7, {0xe8, 0x80, 0xe4, 0x2e, 0xfe}, 6},  // loop A: +0x7588
    {0x1d87106, {0xe8, 0x71, 0xe4, 0x2e, 0xfe}, 6},  // loop A: +0x75e8
    {0x1d87115, {0xe8, 0x62, 0xe4, 0x2e, 0xfe}, 6},  // loop A: +0x7648
    {0x1d87625, {0xe8, 0x52, 0xdf, 0x2e, 0xfe}, 7},  // loop B: +0x14a2b8
    {0x1d87634, {0xe8, 0x43, 0xdf, 0x2e, 0xfe}, 7},  // loop B: +0x14a318
    {0x1d87643, {0xe8, 0x34, 0xdf, 0x2e, 0xfe}, 7},  // loop B: +0x14a378
};
// views4 "slots": the per-view history slot arrays and their element constructor. [Per-view slots]
inline constexpr std::uintptr_t kSlotElementCtorRva = 0x1d391e0;
inline constexpr std::uint8_t kSlotElementCtorAnchor[10] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x48, 0x83, 0xec, 0x30};
inline constexpr std::ptrdiff_t kSlotArrayOffsets[] = {0x7588, 0x75e8, 0x7648, 0x76b8, 0x14a2b8, 0x14a318, 0x14a378};
inline constexpr std::size_t kSlotSize = 0x20;
// The slot creator's loop base, index and bound. [Slot creator]
inline constexpr std::uintptr_t kCreatorBaseRva = 0x1d95970;
inline constexpr std::uint8_t kCreatorBaseAnchor[10] = {0x4d, 0x8b, 0xf4, 0x49, 0xc1, 0xe6, 0x05, 0x4d, 0x03, 0xf5};
inline constexpr std::uintptr_t kCreatorIndexRvas[] = {0x1d95a6b, 0x1d95c61, 0x1d95e39};
inline constexpr std::uint8_t kCreatorIndexAnchor[7] = {0x49, 0x8b, 0xf4, 0x48, 0xc1, 0xe6, 0x05};
inline constexpr std::uintptr_t kCreatorBoundRva = 0x1d965ca;
inline constexpr std::uint8_t kCreatorBoundAnchor[4] = {0x49, 0x83, 0xfc, 0x03};
// The history import loops' step and view-count call sites. [History import]
inline constexpr std::uintptr_t kImportStepRvas[] = {0x1d82a93, 0x1d82b3a, 0x1d82ea5, 0x1d82f51, 0x1d82fb3};
inline constexpr std::uint8_t kImportStepAnchor[7] = {0x48, 0x83, 0xc7, 0x20, 0x49, 0x8b, 0xcd};
inline constexpr std::uintptr_t kImportViewCountCallSites[] = {0x1d82a05, 0x1d82a9a, 0x1d82af0, 0x1d82b41, 0x1d82e58,
                                                               0x1d82eac, 0x1d82efe, 0x1d82f58, 0x1d82f66, 0x1d82fba};
// views4 "cameras": camera slot fields for the focus views. [Focus cameras]
inline constexpr std::ptrdiff_t kCameraProjection = 0x40, kCameraPreviousProjection = 0xc0;
inline constexpr std::ptrdiff_t kCameraView = 0x000, kCameraPreviousView = 0x080, kCameraWorldView = 0x100,
                                kCameraPreviousWorldView = 0x140;
// The register holding the renderer at each camera slot index site. [Camera slot registers]
inline constexpr std::uint8_t kCameraSlotIndexBase[] = {13, 7, 7, 6, 6};
// The builder's per-view record loop: view-count call sites. [Record loop]
inline constexpr std::uintptr_t kRecordLoopViewCountCallSites[] = {0x1d85cbb, 0x1d85ee3};
// Further view-count call sites, one group per views4_callers token. [Optional view-count groups]
struct CallSiteGroup {
    const wchar_t* token;
    std::uintptr_t sites[5];
};
inline constexpr CallSiteGroup kOptionalViewCountGroups[] = {
    {L"consts", {0x1d881c9, 0x1d881e1, 0x1d881f8, 0x1d88212, 0x1d8830e}},  // builder: per-view vectors +0x7d0/+0x7e8
    {L"fog", {0x1da1ee7}},
    {L"lens", {0x1d9cbfa}},
    {L"final", {0x1db1d62, 0x1db1d7c, 0x1db1daf}},                        // executor: the final pass's view list
    {L"oitpass", {0x1dab9a2, 0x1dac60d}},
    {L"dlss", {0x1d4509b, 0x1d45161, 0x1d4521c, 0x1d45429, 0x1d4547b}},
};
// The instance visibility batch merge, its group layout and limits. [Batch merge]
inline constexpr std::uintptr_t kBatchMergeRva = 0x1d6bfd0;
inline constexpr std::uint8_t kBatchMergeAnchor[16] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
                                                       0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
inline constexpr std::uintptr_t kBatchLodLimitRva = 0x1d6c932;
inline constexpr std::uint8_t kBatchLodLimitAnchor[5] = {0x3d, 0x00, 0x00, 0x10, 0x00};  // cmp eax, 0x100000
inline constexpr std::ptrdiff_t kBatchGroupsBegin = 0x1a8, kBatchGroupsEnd = 0x1b0, kBatchGroupStride = 0x330,
                                kBatchEntries = 0x150, kBatchEntryStride = 0xa20, kBatchEntryName = 0x8,
                                kBatchLimits = 0x498;
inline constexpr std::ptrdiff_t kGroupDraws = 0x30, kGroupSubEntries = 0x48, kGroupInstances = 0x7c, kGroupLods = 0x80;
inline constexpr std::size_t kDrawSize = 0xa8, kSubEntrySize = 0x20;
inline constexpr std::int64_t kBatchLodLimit = 0x100000, kBatchDrawCap = 0x20000;
// The batch limit stores, and the doubled limits views4 writes. [Batch limits]
inline constexpr std::uintptr_t kBatchLimitStoresRva = 0x1ed9dc1;
inline constexpr std::uint8_t kBatchLimitStoresAnchor[20] = {0xc7, 0x01, 0x00, 0x00, 0xc0, 0x00, 0xc7,
                                                             0x41, 0x04, 0x10, 0x27, 0x00, 0x00, 0xc7,
                                                             0x41, 0x08, 0x80, 0x38, 0x01, 0x00};
inline constexpr std::uint8_t kBatchLimitStoresPatch[20] = {0xc7, 0x01, 0x00, 0x00, 0x80, 0x01,  // 0x1800000
                                                            0xc7, 0x41, 0x04, 0x20, 0x4e, 0x00, 0x00,  // 20000
                                                            0xc7, 0x41, 0x08, 0x00, 0x71, 0x02, 0x00};  // 160000
// Diagnostic: the deferred shadow loop's second-eye shortcut. [Shadow view 1]
inline constexpr std::uintptr_t kShadowView1BranchRva = 0x1dabd72;
inline constexpr std::uint8_t kShadowView1BranchAnchor[6] = {0x0f, 0x85, 0xad, 0x03, 0x00, 0x00};  // jne 0x1dac125
inline constexpr std::uint8_t kShadowView1BranchPatch[6] = {0xe9, 0xae, 0x03, 0x00, 0x00, 0x90};   // jmp 0x1dac125
// normalReconstructionViewSpace and its parameter block. [Normal reconstruction]
inline constexpr std::uintptr_t kNormalReconstructRva = 0x1f8b4c0;
inline constexpr std::uint8_t kNormalReconstructAnchor[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                                               0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
inline constexpr std::ptrdiff_t kNormalParamsRecordA = 0x10, kNormalParamsTarget = 0x18, kNormalParamsRecordB = 0x30,
                                kNormalParamsSplit = 0x38, kNormalParamsSize = 0x40, kTextureWidth = 0x30;
// Its dispatch's x group count. [Normal reconstruction dispatch]
inline constexpr std::uintptr_t kNormalReconstructDispatchRva = 0x1f8b7ac;
inline constexpr std::uint8_t kNormalReconstructDispatchAnchor[11] = {0x41, 0x8d, 0x56, 0x07, 0x41, 0xc1,
                                                                      0xe8, 0x03, 0xc1, 0xea, 0x03};
// Tiled light culling's second-eye block choice, removed by views4. [Tile culling view 1]
inline constexpr std::uintptr_t kTileCullingView1BlockRva = 0x1db8c52;
inline constexpr std::uint8_t kTileCullingView1BlockAnchor[4] = {0x44, 0x0f, 0xb6, 0xf3};  // movzx r14d, bl
inline constexpr std::uint8_t kTileCullingView1BlockPatch[4] = {0x0f, 0x1f, 0x40, 0x00};   // nop dword [rax]
// Diagnostic dump site and the view structure's blocks. [Tile culling dump]
inline constexpr std::uintptr_t kTileCullingEyeStructRva = 0x1db8c56;
inline constexpr std::uint8_t kTileCullingEyeStructAnchor[7] = {0x48, 0x8b, 0xbd, 0x98, 0x00, 0x00, 0x00};
inline constexpr std::ptrdiff_t kTileEyeBlocks[2] = {0x50, 0x5b0};
inline constexpr std::ptrdiff_t kTileEyeMatrix = 0x224;
// Per-view vectors the VR branch fills for the eyes. [Per-view eye vectors]
inline constexpr std::ptrdiff_t kPerViewMatrix = 0x640, kPerViewTanX = 0x658, kPerViewTanY = 0x670;
// Renderer init's per-view vector sizings. [Init view count]
inline constexpr std::uintptr_t kInitViewCountCallSites[] = {0x1d444b5, 0x1d444cc, 0x1d444e3, 0x1d444fa};
// NGX's NVSDK_NGX_D3D12_CreateFeature and NVSDK_NGX_D3D12_EvaluateFeature_C, linked into ACE [dlss-extent]
inline constexpr std::uintptr_t kNgxCreateFeatureRva = 0x20b3a00;
inline constexpr std::uint8_t kNgxCreateFeatureAnchor[16] = {0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                             0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41};
inline constexpr std::uintptr_t kNgxEvaluateFeatureRva = 0x20b3bb0;
inline constexpr std::uint8_t kNgxEvaluateFeatureAnchor[16] = {0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0xc1, 0x48,
                                                               0x8b, 0x0d, 0x82, 0x30, 0x8b, 0x04, 0x48, 0x85};

// D3D12 tracing and the focus-only experiment live in probe/ and the layer. [Probe rounds]

}  // namespace ace

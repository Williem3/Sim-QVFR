// Assetto Corsa EVO facts from static analysis (see ../../docs/recon.md). Pinned to one exe build.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ace {

inline constexpr char kExecutableSha256[] = "ab11fc2a35ef9c2c61c6e0bcbdeb755b0d7787d101c3fdebfee12df3eadedbcb";
inline constexpr wchar_t kExecutableName[] = L"AssettoCorsaEVO.exe";

// VRManager (ksRenderer\VRManager.cpp) and camera functions that the probe observes.
struct ProbeTarget {
    const char* name;
    std::uintptr_t rva;
    std::size_t anchorSize;
    std::uint8_t anchor[16];
};

// Round 1 (2026-10-31) established, on the main thread, once per game frame:
//   main loop 0x6a4240 -> 0x74c6a0 -> 0x896540 -> scene setup 0x741aa0 -> prepare views 0x95fb40
//   -> camera_update (frustum twice, once per eye); begin/end frame also under 0x74c6a0.
// Round 2 targets the render side: who reads the eye camera FOV, and how often per frame.
inline constexpr ProbeTarget kProbeTargets[] = {
    // XrView FOV -> eye camera: 8 tan() calls, logs "VR World Scale" (frame marker)
    {"camera_update", 0x1e06190, 16, {0x4c,0x8b,0xdc,0x55,0x56,0x57,0x49,0x8d,0xab,0xa8,0xfc,0xff,0xff,0x48,0x81,0xec}},
    // xrEndFrame (frame marker)
    {"end_frame", 0x1dfc3f0, 16, {0x40,0x55,0x53,0x57,0x48,0x8d,0x6c,0x24,0xb9,0x48,0x81,0xec,0xb0,0x00,0x00,0x00}},
    // frame-graph builder: creates ZPrepass, Velocity, Lighting ... " view:N" passes (round 2)
    {"graph_builder", 0x1d84a80, 16, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x54,0x41,0x55,0x41}},
    // render-graph execution path of the final swapchain write (round 3, render worker thread)
    {"graph_execute_1db19c0", 0x1db19c0, 16, {0x48,0x89,0x5c,0x24,0x10,0x4c,0x89,0x44,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41}},
    {"final_pass_1f83390", 0x1f83390, 16, {0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x70,0x20,0x55,0x57,0x41,0x54,0x41}},
    // renderer per-frame setup: sets view mode renderer[+0x1d4cdc] (round 4)
    {"renderer_begin_1d6d7f0", 0x1d6d7f0, 16, {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}},
};
// render_frame(this, float, float, scene*): one call per frame from the main frame loop 0x74c6a0.
// Runs scene->update (vtable +0x10 of [r9+0x38]), renderer begin 0x1d6d7f0, scene setup 0x741aa0
// (cameras, frame graph), the UI and the final submit. Round 6 hooks it for the double-render
// experiment (experiment/double_render.cpp), so it is no longer a probe target.
inline constexpr ProbeTarget kRenderFrame = {
    "render_frame", 0x896540, 16, {0x48,0x8b,0xc4,0x55,0x41,0x56,0x48,0x83,0xec,0x58,0x80,0xb9,0x89,0x02,0x00,0x00}};

// Renderer view mode (recon "Toward single-frame quad views"): viewCount() = mode + 1, VR = mode 1.
// Renderer init 0x1d43090 picks the mode and stores it at both offsets, then sizes the per-view
// vectors from viewCount(). Renderer begin copies the configured mode to the active one each frame.
inline constexpr std::ptrdiff_t kRendererActiveViewMode = 0x1d4cdc;
inline constexpr std::ptrdiff_t kRendererConfiguredViewMode = 0x1d4cd8;
inline constexpr std::ptrdiff_t kRendererFrameIndex = 0x1d4628;
// Per-view std::vector members sized at init: {offset, element size}.
struct PerViewVector {
    std::ptrdiff_t offset;
    std::size_t elementSize;
};
inline constexpr PerViewVector kRendererPerViewVectors[] = {{0x1d5188, 0x20}, {0x1d51a0, 0x28}};
// views3 experiment: init's `mov ebx, 1` (VR branch, at 0x1d4447b) becomes `mov ebx, 2`.
inline constexpr std::uintptr_t kInitVrModeImmediateRva = 0x1d4447c;
inline constexpr std::uint8_t kInitVrModeAnchor[16] = {0xbb, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x47,
                                                       0x08, 0x48, 0x8b, 0x08, 0x80, 0xb9, 0x55, 0x01};

// views4 stage gate (recon "The VR view path, mapped"). VR keeps view mode 1; only the view count
// grows to 4 for chosen callers.
// viewCount(renderer) = mode + 1: `mov eax,[rcx+0x1d4cdc]; inc eax; ret`.
inline constexpr std::uintptr_t kViewCountRva = 0x1dc7f50;
inline constexpr std::uint8_t kViewCountAnchor[9] = {0x8b, 0x81, 0xdc, 0x4c, 0x1d, 0x00, 0xff, 0xc0, 0xc3};
// viewport(renderer, out, const float* renderSize, int view) -> out. Mode 1 splits the render size
// into two halves. Out: D3D12_VIEWPORT at +0x00, D3D12_RECT scissor at +0x18, view size and its
// reciprocal at +0x28, pixel offset at +0x38, UV scale at +0x40, UV offset at +0x48.
inline constexpr std::uintptr_t kViewportRva = 0x1d922b0;
inline constexpr std::uint8_t kViewportAnchor[16] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                     0x24, 0x18, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56};
// Two init-time callers size per-view resources once, from view 0's viewport, for every view
// (static, 2026-11-01). Tile culling setup 0x1d96c80 takes ceil(view size / 16) (`(size + 15) *
// 0.0625`) as the one tile grid at renderer + 0x14a498/+0x14a49c; it sizes every view's tile lists
// and tile frusta, and the per-frame culling (0x1db890d) passes it as each view's tile count. The
// volumetrics setup 0x1da1e20 hands view 0's viewport to the system at renderer + 0x13e0. With
// unequal views both must see the largest view, or the bigger views overrun them.
inline constexpr std::uintptr_t kTileGridViewportCallSite = 0x1d97049;
inline constexpr std::uintptr_t kFogViewportCallSite = 0x1da1edf;
// view(renderer, int i) = renderer[+0x7b8] + i * 0x140 (per-view records).
inline constexpr std::uintptr_t kViewRecordRva = 0x1d92f50;
inline constexpr std::uint8_t kViewRecordAnchor[16] = {0x48, 0x63, 0xc2, 0x48, 0x8d, 0x04, 0x80, 0x48,
                                                       0xc1, 0xe0, 0x06, 0x48, 0x03, 0x81, 0xb8, 0x07};
// Camera block: 3 slots of 0x184 at renderer + 0x1d4ce0 (six 0x40 matrices, then a float at
// +0x180). The builder's VR branch fills slots 0-1 from the VR camera's per-eye matrices.
inline constexpr std::ptrdiff_t kCameraSlotBase = 0x1d4ce0;
inline constexpr std::ptrdiff_t kCameraSlotStride = 0x184;
// `imul dst, view, 0x184`: index into the 3-slot camera block at renderer + 0x1d4ce0.
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
// Per-view pass setup (renderer, pass, view): computes the pass viewport inline as halves of the
// pass target (`right >> 1`, x = view == 0 ? 0 : half) into pass +0x934 (D3D12_VIEWPORT) and
// +0x94c (D3D12_RECT). Most per-view passes take their viewport from here (views4 run 1).
inline constexpr std::uintptr_t kPassSetupRvas[] = {0x1dc4b70, 0x1dc68e0};
inline constexpr std::uint8_t kPassSetupAnchors[][16] = {
    {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56},
    {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8}};
inline constexpr std::ptrdiff_t kPassViewport = 0x934, kPassScissor = 0x94c;
// The pass embeds the shaders' renderPassData cbuffer at +0x224, so its float2 screenOffset (cbuffer
// 1344) is pass +0x764. Lighting finds its light tile as (SV_Position - screenOffset) / 16. The setups
// write stereo values: 0 on the view-0 path (0x1dc52f7), the target's half width on the other
// (0x1dc5627), view * width / 2 in 0x1dc68e0. With quarters, views 1-3 read tiles whole rows away
// (capture 2400: view 1's bands, the focus views' stepped shadows).
inline constexpr std::ptrdiff_t kPassScreenOffset = 0x764;
// The float2 after screenOffset (pass +0x76c/+0x770) is renderPassData.screenSizeDiv (embedded shader
// reflection: screenOffset, screenSizeDiv, jitter, jitterOffset): pass +0x484 (a target width) x 0.5
// in 0x1dc4b70 and / 3 in 0x1dc68e0, and pass +0x488 (its height), i.e. one view's size in stock.
// views4 writes each view's own size (until 2026-11-01 it kept the stereo ratio: twice the view's width).
inline constexpr std::ptrdiff_t kPassScreenSizeDiv = 0x76c;
// 0x1dc4b70 copies the view, previous view and previous projection into the pass with fixed
// displacements from the renderer in rdi: view 0 reads slot 0, any other view slot 1 (`test r12d,
// r12d; jne` at kPassEyeBranchRva). The two paths rejoin at kPassRejoinRva; between them rdi is used
// only for those slot fields and `mov rcx, [rdi+8]`. views4 branches on view & 1 and, for views 2-3,
// points rdi at a phantom renderer whose slots 0 and 1 are the focus slots (C2, C3 = C2 + 0x184),
// restoring it at the rejoin from the phantom's kPhantomRenderer field (composition run 5: both
// focus views were the right eye's; gaze run 1: the focus views ignored their turned cameras).
inline constexpr std::uintptr_t kPassEyeBranchRva = 0x1dc5052, kPassEyeBranchTarget = 0x1dc532d;
inline constexpr std::uint8_t kPassEyeBranchAnchor[9] = {0x45, 0x85, 0xe4, 0x0f, 0x85, 0xd2, 0x02, 0x00, 0x00};
inline constexpr std::uintptr_t kPassRejoinRva = 0x1dc5658;
inline constexpr std::uint8_t kPassRejoinAnchor[6] = {0x89, 0x86, 0x4c, 0x04, 0x00, 0x00};  // mov [rsi+0x44c], eax
// The phantom renderer's own fields: a copy of renderer[+8] (read by both paths) and the real renderer.
inline constexpr std::ptrdiff_t kRendererField8 = 0x8, kPhantomRenderer = 0x10;
// Builder per-view loops. Loop A (ZPrepass, Velocity, Flags) runs over viewCount() (entry and
// loop-back call sites below). Loop B (Lighting Opaque/Transparent/OIT) takes its bound inline:
// `mov eax,[r13+0x1d4cdc]; inc eax` at kLightingLoopBoundRva. Both index fixed 3-slot history
// arrays (stride 0x20: +0x7588, +0x75e8, +0x7648; +0x14a2b8, +0x14a318, +0x14a378) with
// `movsxd dst, view; shl dst, 5`; views4 folds those onto the eyes' slots.
inline constexpr std::uintptr_t kDepthLoopViewCountCallSites[] = {0x1d870d8, 0x1d875ae};
inline constexpr std::uintptr_t kLightingLoopBoundRva = 0x1d88090;
inline constexpr std::uint8_t kLightingLoopBoundAnchor[9] = {0x41, 0x8b, 0x85, 0xdc, 0x4c, 0x1d, 0x00, 0xff, 0xc0};
// Its replacement: mode + 1, or 4 in VR (mode 1).
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
// Reads of the pass's view (`mov edx, [pass+0x9ac]`) passed to per-view resource lookups that only
// hold the two eyes (views4 run 3 crashed in 0x1dbea70 on view 2). views4 appends `and edx, 1`.
// Not folded: 0x1dbd3ad compares the view with viewCount() - 1 for a once-per-frame update.
struct ViewReadSite {
    std::uintptr_t rva;
    std::uint8_t anchor[6];
};
inline constexpr ViewReadSite kPassViewReadSites[] = {
    {0x1dbe211, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},  // 0x1dbce80: resources at pass +0x968...
    {0x1dbe22d, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},
    {0x1dbe242, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},
    {0x1dbe25e, {0x8b, 0x97, 0xac, 0x09, 0x00, 0x00}},
    {0x1dbed0c, {0x8b, 0x93, 0xac, 0x09, 0x00, 0x00}},  // 0x1dbea70 -> lookup thunk 0x2cde
};
// Tiled light culling, ACE's own N-view code driven by viewCount(). Setup 0x1d96c80 sizes and
// creates per-view tileFrustums (+0x14a4a0), entityTileOpaque (+0x400) and entityTileTransparent
// (+0x418) buffers; 0x1db8510 builds each view's tiles every frame. views4 "tiles" widens both, so
// views 2-3 get their own tile lists (the race hang in gate run 13: lighting for views 2-3 read the
// eyes' tiles).
inline constexpr std::uintptr_t kTileCullingViewCountCallSites[] = {
    0x1d96fc8, 0x1d96fe0, 0x1d96ffc, 0x1d970a6, 0x1d97673,  // setup 0x1d96c80
    0x1db880a, 0x1db8fb7, 0x1db8fe6, 0x1db95ef,             // per frame 0x1db8510
};
// Pass registration 0x1da7b50(renderer, pass, view) reads the pass's tile lists from
// +0x400/+0x418/+0x430/+0x438 by view (run 4 crashed at 0x1da7bce with 2-entry lists). Without
// "tiles" its entry `movsxd rbp, r8d; mov rbx, rdx` becomes `mov ebp, r8d; and ebp, 1; mov rbx, rdx`.
inline constexpr std::uintptr_t kVelocityPassViewRva = 0x1da7b65;
inline constexpr std::uint8_t kVelocityPassViewAnchor[6] = {0x49, 0x63, 0xe8, 0x48, 0x8b, 0xda};
inline constexpr std::uint8_t kVelocityPassViewStub[] = {0x41, 0x8b, 0xe8, 0x83, 0xe5, 0x01, 0x48, 0x8b, 0xda};
// Each loop iteration advances its view's history slots: `call 0x7557c` -> 0x1dc3fe0(slot), a
// `lock inc` of the slot's ping-pong counter that picks this frame's buffer, plus
// `mov byte [slot+0x18], 0`. Folded, the eyes' slots advanced twice a frame and later passes read
// the buffer holding only views 2-3 (run 6: black eyes). Skipping the whole call for views >= 2
// left the flag set and broke command-list recording (runs 7-8: allocator reset while recording).
// views4 therefore, for views >= 2 (esi in loop A, edi in loop B), clears the flag only.
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
// views4 "slots": real per-view history slots for views 2-3 (recon "Per-view slots").
// Seven per-view slot arrays of 3 x 0x20, slot = {handle[2], int* counter, bool frameFlag}, built
// by the element ctor 0x1d391e0 (counter = new int 0, flag = 1): depth block 0x7588, 0x75e8,
// 0x7648 (member at +0x7548), TextureFeedback 0x76b8 (member at +0x76a8), lighting block
// 0x14a2b8, 0x14a318, 0x14a378. View 2 uses the real slot 2. View 3 uses slots the layer builds in
// its own memory at a phantom renderer R' (R' + array offset = view 3's slot).
inline constexpr std::uintptr_t kSlotElementCtorRva = 0x1d391e0;
inline constexpr std::uint8_t kSlotElementCtorAnchor[10] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x48, 0x83, 0xec, 0x30};
inline constexpr std::ptrdiff_t kSlotArrayOffsets[] = {0x7588, 0x75e8, 0x7648, 0x76b8, 0x14a2b8, 0x14a318, 0x14a378};
inline constexpr std::size_t kSlotSize = 0x20;
// Slot creator 0x1d958c0 loops r12 = 0..2: r14 = r13 + (r12 << 5) at 0x1d95970 (10 bytes), and
// rsi = r12 << 5 at three sites (used as [rsi + r13 + disp]); bound `cmp r12, 3` at 0x1d965ca.
inline constexpr std::uintptr_t kCreatorBaseRva = 0x1d95970;
inline constexpr std::uint8_t kCreatorBaseAnchor[10] = {0x4d, 0x8b, 0xf4, 0x49, 0xc1, 0xe6, 0x05, 0x4d, 0x03, 0xf5};
inline constexpr std::uintptr_t kCreatorIndexRvas[] = {0x1d95a6b, 0x1d95c61, 0x1d95e39};
inline constexpr std::uint8_t kCreatorIndexAnchor[7] = {0x49, 0x8b, 0xf4, 0x48, 0xc1, 0xe6, 0x05};
inline constexpr std::uintptr_t kCreatorBoundRva = 0x1d965ca;
inline constexpr std::uint8_t kCreatorBoundAnchor[4] = {0x49, 0x83, 0xfc, 0x03};
// History import loops in 0x1d7f8e0: `inc esi; add rdi, 0x20; mov rcx, r13`, with
// rdi - r13 = slot field offset + view * 0x20. Their viewCount call sites (entry, loop back).
inline constexpr std::uintptr_t kImportStepRvas[] = {0x1d82a93, 0x1d82b3a, 0x1d82ea5, 0x1d82f51, 0x1d82fb3};
inline constexpr std::uint8_t kImportStepAnchor[7] = {0x48, 0x83, 0xc7, 0x20, 0x49, 0x8b, 0xcd};
inline constexpr std::uintptr_t kImportViewCountCallSites[] = {0x1d82a05, 0x1d82a9a, 0x1d82af0, 0x1d82b41, 0x1d82e58,
                                                               0x1d82eac, 0x1d82efe, 0x1d82f58, 0x1d82f66, 0x1d82fba};
// views4 "cameras": focus cameras for views 2-3. Camera slot layout (dump, gate run 18):
// +0x000 camera-relative view, +0x040 projection, +0x080 / +0x0c0 their previous frame, +0x100
// world view, +0x140 its previous, +0x180 a per-view float. Projection is row-major, reverse-Z
// infinite: [0] = 2/(tanR-tanL), [5] = 2/(tanU-tanD), [8] = (tanR+tanL)/(tanR-tanL),
// [9] = (tanU+tanD)/(tanU-tanD); the depth terms [10], [11], [14] stay.
inline constexpr std::ptrdiff_t kCameraProjection = 0x40, kCameraPreviousProjection = 0xc0;
inline constexpr std::ptrdiff_t kCameraView = 0x000, kCameraPreviousView = 0x080, kCameraWorldView = 0x100,
                                kCameraPreviousWorldView = 0x140;
// Registers holding the renderer at each kCameraSlotIndexSites entry (r13, rdi, rdi, rsi, rsi).
inline constexpr std::uint8_t kCameraSlotIndexBase[] = {13, 7, 7, 6, 6};
// The builder's per-view record loop (copies camera slots into the view records) runs over
// viewCount(): entry and loop back. views4 fills slots 2-3 at the entry call.
inline constexpr std::uintptr_t kRecordLoopViewCountCallSites[] = {0x1d85cbb, 0x1d85ee3};
// Remaining viewCount() loops that ran for 2 views in composition run 2 (the focus views were
// darker). Each is a views4_callers token. Not included: 0x1dab753 (`count == 2` stereo check) and
// 0x1dbd3a6 (last-view once-per-frame update).
struct CallSiteGroup {
    const wchar_t* token;
    std::uintptr_t sites[5];
};
inline constexpr CallSiteGroup kOptionalViewCountGroups[] = {
    {L"consts", {0x1d881c9, 0x1d881e1, 0x1d881f8, 0x1d88212, 0x1d8830e}},  // builder: per-view vectors +0x7d0/+0x7e8
    // 0x1da1e20: passes viewCount() to the volumetrics/atmosphere system (renderer + 0x13e0, built in
    // 0x1f9c450: "volumetrics#N", voxel grids, atmosphereInScatter[Sky]_srv_), whose per-view tables
    // at +0x3f40/+0x3f58/+0x3fd0/+0x4000 the kPassViewReadSites and 0x1fa9780 read. With "fog" they
    // have 4 entries and the reads are not folded; "consts" needs it (crash at 0x1fab399 without).
    {L"fog", {0x1da1ee7}},
    {L"lens", {0x1d9cbfa}},                                               // 0x1d9ca10: lens dirt / post setup
    {L"final", {0x1db1d62, 0x1db1d7c, 0x1db1daf}},                        // executor: the final pass's view list
    {L"oitpass", {0x1dab9a2, 0x1dac60d}},  // 0x1dab9af: the deferredShadows sun-shadow mask, one cell per view
    // DLSS features, made in renderer init 0x1d43090: two vector sizings, the loop entry, a per-view flag and
    // the loop back. The loop creates one feature per view (0x1f90430) at the view's rect (viewport 0x1d922b0).
    // The final pass 0x1f7a6e0 runs DLSS only when it has as many features as views (0x1f7ab05), so with
    // "final" and 2 features it skipped DLSS and showed the 2/3-size render unscaled.
    {L"dlss", {0x1d4509b, 0x1d45161, 0x1d4521c, 0x1d45429, 0x1d4547b}},
};
// Instance visibility batch merge 0x1d6bfd0 (renderer in rcx; builder 0x1d84a80 calls it at
// 0x1d88ec0 every frame). It appends every visibility group's lists to the renderer's frame batch
// and throws "Instance visibility batch is too large" (crash report 2026-11-01: 22-car grid,
// views4) when a total passes a limit. Groups: renderer + 0x1a8/+0x1b0, 0x330 each, parallel to
// the named entries at renderer + 0x150 (0xa20 each, std::string name at +8). Per group: draws
// +0x30/+0x38 (0xa8 each), sub-entries +0x48/+0x50 (0x20 each), instance count +0x7c (int),
// per-LOD entries +0x80/+0x88 (4 each); a group with no draws is skipped. Limits: renderer +
// 0x498 -> {instances (total must stay below), draws, sub-entries}, set once by 0x1ed9da0 to
// 0xc00000, 10000, 80000; per-LOD entries at most 0x100000 and draws at most 0x20000 (immediates).
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
// The three limit stores in 0x1ed9da0 (`mov dword [rcx], 0xc00000; mov dword [rcx+4], 10000;
// mov dword [rcx+8], 80000`). Every per-view group counts about the whole scene's instances (the
// GPU culls per view later), so views 2-3 add 8+ groups: a 22-car grid reached 14.1M instances and
// 9775 draws (crash report run 22:33). views4 doubles all three before the renderer is created;
// draws stay under kBatchDrawCap.
inline constexpr std::uintptr_t kBatchLimitStoresRva = 0x1ed9dc1;
inline constexpr std::uint8_t kBatchLimitStoresAnchor[20] = {0xc7, 0x01, 0x00, 0x00, 0xc0, 0x00, 0xc7,
                                                             0x41, 0x04, 0x10, 0x27, 0x00, 0x00, 0xc7,
                                                             0x41, 0x08, 0x80, 0x38, 0x01, 0x00};
inline constexpr std::uint8_t kBatchLimitStoresPatch[20] = {0xc7, 0x01, 0x00, 0x00, 0x80, 0x01,  // 0x1800000
                                                            0xc7, 0x41, 0x04, 0x20, 0x4e, 0x00, 0x00,  // 20000
                                                            0xc7, 0x41, 0x08, 0x00, 0x71, 0x02, 0x00};  // 160000
// The deferredShadows loop (0x1dab9af, "oitpass") gives view 1, under a flag (0xb0753), two matrices
// from a per-frame structure instead of its own: a two-eye shortcut. Diagnostic: always the own path.
inline constexpr std::uintptr_t kShadowView1BranchRva = 0x1dabd72;
inline constexpr std::uint8_t kShadowView1BranchAnchor[6] = {0x0f, 0x85, 0xad, 0x03, 0x00, 0x00};  // jne 0x1dac125
inline constexpr std::uint8_t kShadowView1BranchPatch[6] = {0xe9, 0xae, 0x03, 0x00, 0x00, 0x90};   // jmp 0x1dac125
// normalReconstructionViewSpace (0x1f8b4c0, called from the deferredShadows pass at 0x1dab7f7):
// one dispatch over the whole target, choosing per pixel between two view records by a split x
// (stereo: the second eye's record, split = target width / 2). With four quarters that gave views 1-3
// the wrong records (bands in view 1, stepped contact shadows in view 2). views4 runs it twice: focus
// records with split = 3 quarters, then the eyes' records with split = 1 quarter, dispatched over the
// first two quarters only (kNormalReconstructDispatch caps the x group count).
inline constexpr std::uintptr_t kNormalReconstructRva = 0x1f8b4c0;
inline constexpr std::uint8_t kNormalReconstructAnchor[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                                               0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
inline constexpr std::ptrdiff_t kNormalParamsRecordA = 0x10, kNormalParamsTarget = 0x18, kNormalParamsRecordB = 0x30,
                                kNormalParamsSplit = 0x38, kNormalParamsSize = 0x40, kTextureWidth = 0x30;
// `lea edx, [r14+7]; shr r8d, 3; shr edx, 3`: edx = x groups of 8 px over the target width.
inline constexpr std::uintptr_t kNormalReconstructDispatchRva = 0x1f8b7ac;
inline constexpr std::uint8_t kNormalReconstructDispatchAnchor[11] = {0x41, 0x8d, 0x56, 0x07, 0x41, 0xc1,
                                                                      0xe8, 0x03, 0xc1, 0xea, 0x03};
// Tiled light culling (0x1db8510) takes each view's culling matrix from that view's structure
// (`mov rdi, [rbp+0x98]`), which has two blocks (+0x50, +0x5b0; matrix at +0x224). Block 0 holds the
// view's own camera-to-world matrix; under the VR flag (0xb0753) view 1 alone reads block 1, which
// stays zero with four views (dump, tile probe run): view 1's lights were culled with a zero matrix,
// the bands. views4 removes that choice (`movzx r14d, bl` -> nops): every view uses block 0.
inline constexpr std::uintptr_t kTileCullingView1BlockRva = 0x1db8c52;
inline constexpr std::uint8_t kTileCullingView1BlockAnchor[4] = {0x44, 0x0f, 0xb6, 0xf3};  // movzx r14d, bl
inline constexpr std::uint8_t kTileCullingView1BlockPatch[4] = {0x0f, 0x1f, 0x40, 0x00};   // nop dword [rax]
// Diagnostic dump site:
inline constexpr std::uintptr_t kTileCullingEyeStructRva = 0x1db8c56;
inline constexpr std::uint8_t kTileCullingEyeStructAnchor[7] = {0x48, 0x8b, 0xbd, 0x98, 0x00, 0x00, 0x00};
inline constexpr std::ptrdiff_t kTileEyeBlocks[2] = {0x50, 0x5b0};
inline constexpr std::ptrdiff_t kTileEyeMatrix = 0x224;
// Per-view vectors filled by the VR branch for the eyes: +0x640 (0x40 matrix), +0x658 and +0x670
// (tangent extents, floats).
inline constexpr std::ptrdiff_t kPerViewMatrix = 0x640, kPerViewTanX = 0x658, kPerViewTanY = 0x670;
// Renderer init sizes the per-view vectors (+0x7b8, +0x658, +0x670, +0x640) from viewCount().
inline constexpr std::uintptr_t kInitViewCountCallSites[] = {0x1d444b5, 0x1d444cc, 0x1d444e3, 0x1d444fa};

// Round 3 added D3D12 tracing (probe/d3d12_trace.cpp): which code renders into the swapchain.
// Round 4 extended it to eye-sized internal render targets and their allocations.
// Round 5 adds the focus-only experiment in the layer (xrLocateViews FOV narrowing).

}  // namespace ace

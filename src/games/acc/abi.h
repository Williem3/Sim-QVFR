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
// Primary passes draw their meshes; under instanced stereo a primary pass draws itself and the pass after it in
// one instanced draw (4.26 FSceneRenderer: GetInstancedView = pass + 1), secondary passes draw none. Defaults:
// FULL and LEFT primary. Slot 11 (secondary pass) calls slot 9 and slot 12 (secondary view) calls slot 10.
inline constexpr Slot kDeviceIsAPrimaryPass = {kStereoVtable + 9 * 8, 0x7b1c90};   // `cmp edx, 1; setbe al`
inline constexpr Slot kDeviceIsAPrimaryView = {kStereoVtable + 10 * 8, 0x2da8900};  // engine's, inlined: below
inline constexpr std::ptrdiff_t kViewStereoPass = 0xad0;  // FSceneView::StereoPass (`cmp [rdx + 0xad0], 1`)
// FSceneView::ViewMatrices (FViewMatrices, 4.26 SceneView.h: ProjectionMatrix, ProjectionNoAAMatrix,
// InvProjectionMatrix, ViewMatrix, InvViewMatrix, ViewProjectionMatrix, ... 64 bytes each) at 0x280: the foliage
// culling reads Views[0]->ViewMatrices.GetViewProjectionMatrix() at 0x3c0 (HISM, below), and DispatchPassSetup
// copies 64 bytes from 0x340 (the view matrix, for translucency sorting). FSceneView::Family is at 0.
inline constexpr std::ptrdiff_t kViewProjection = 0x280;  // with the temporal jitter
inline constexpr std::ptrdiff_t kViewProjectionNoAA = 0x2c0;
inline constexpr std::ptrdiff_t kViewMatrix = 0x340;
inline constexpr Slot kAdjustViewRect = {kStereoVtable + 16 * 8, 0x7ae650};          // halves of the target
// SetFinalViewRect(pass, const FIntRect&): the renderer reports each view's final rect (screen percentage applied)
// every frame (4.26 FSceneRenderer::PrepareViewRectsForRendering). FSteamVRHMD's is empty, shared with others.
inline constexpr Slot kSetFinalViewRect = {kStereoVtable + 17 * 8, 0x776df0};
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

// Views per instanced draw. Kunos extended instanced stereo to the triple-screen views: where 4.26 has a literal 2
// (`View.IsInstancedStereoPass() ? 2 : 1`), ACC reads Family->Views.Num() (FSceneViewFamily has no vtable, so
// Views.Num sits at +8), and its vertex shaders take eye = SV_InstanceID % N, N in the view constants. With our four
// views every instanced draw went 4 wide (RenderDoc, README.md); two instanced pairs need 2.
// Each is `mov r32, [rax + 8]` (rbx at 0x2befafa) right after `call IsInstancedStereoPass (0x2cfe700); test al, al;
// je`, with Family in rax; views.cpp writes `push 2; pop r64` over it (same length). Found by walking that
// function's 43 callers; the other 22 only test the count against 0, or read other fields. 0x1a094b1 is
// TaskContext.InstanceFactor (FParallelMeshDrawCommandPass::DispatchPassSetup 0x1a09300, found through
// r.MeshDrawCommands.ParallelPassSetup).
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
// The view constants' N (cb0[135].y, byte 0x874 of the view uniform struct), in what looks like
// FSceneView::SetupCommonViewUniformBufferParameters (0x2d08510): `ecx = Family ? Family->Views.Num() : 0`, then
// this `mov rdi, [rbp + 0xe8]`, then `mov [rdx + 0x874], ecx`. Written for every view, mono families too, so
// views.cpp clamps it to 2 in a stub here instead of writing 2: a capture or mirror family keeps N = 1.
inline constexpr std::uintptr_t kViewInstancedCount = 0x2d085ab;
inline constexpr std::uint8_t kViewInstancedCountAnchor[7] = {0x48, 0x8b, 0xbd, 0xe8, 0x00, 0x00, 0x00};

// The view constants' StereoPassIndex: 4.26 FViewInfo::SetupUniformBufferParameters (SceneRendering.cpp) stores
// `StereoRenderingDevice->GetViewIndexForPass(StereoPass)` there, so the focus views got 2 and 3. Shaders index
// two-entry tables with it as the eye within an instanced pair: planar reflections' ProjectionWithExtraFOV[2] and
// PlanarReflectionScreenScaleBias[2] (PlanarReflectionShared.ush) read past the end for 2 and 3, and view 3 drew
// the dash's windscreen reflection from the wrong place (README.md). GetViewIndexForPass must stay 0-3 (each view's
// own FSceneViewState in ULocalPlayer), so views.cpp redirects this store, `mov [rsi + 0xc5c], eax`, through
// `and eax, 1`: 0, 1, 0, 1. Before it: `mov edx, [r14 + 0xad0]; call [rax + 0x30]; jmp; mov eax, r13d` (no
// device: 0), both paths meeting here.
inline constexpr std::uintptr_t kStereoPassIndexCall = 0x1c11f29;
inline constexpr std::uint8_t kStereoPassIndexCallAnchor[15] = {0x41, 0x8b, 0x96, 0xd0, 0x0a, 0x00, 0x00, 0xff,
                                                                0x50, 0x30, 0xeb, 0x03, 0x41, 0x8b, 0xc5};
inline constexpr std::uintptr_t kStereoPassIndexStore = 0x1c11f38;
inline constexpr std::uint8_t kStereoPassIndexStoreAnchor[6] = {0x89, 0x86, 0x5c, 0x0c, 0x00, 0x00};

// The instanced view's constants (shaders' second view of a pair, cb2 in ACC's): 4.26 has two ways to pick it.
// Scene->UniformBuffers.GetInstancedView and the base pass take `View.StereoPass + 1` (right for both pairs); 12
// other places (custom depth in SceneRendering.cpp, and Kunos's, the translucent base pass among them) take
// `IStereoRendering::IsStereoEyeView(View) ? eSSP_RIGHT_EYE : eSSP_FULL`, so the focus pair's second view got the
// right eye's constants: the windscreen drew into view 3 with the right eye's camera, a faint, larger copy of the
// dash's reflection in it (RenderDoc, README.md: event 26464's cb2 held the right eye's projection). Each is
// `neg al; sbb edx, edx; and edx, 2` (al: IsStereoEyeView), then `call GetStereoEyeView (0x2cf8140)` with rcx the
// family; views.cpp redirects those 7 bytes to `edx = al ? (View.StereoPass + 1) & ~1 : 0`: pass 2 for the eyes
// as before, 4 for the focus views. `view` is the register holding the view there (x64 number: rsi 6, rbx 3,
// r14 14, r15 15; rcx 1 at 0x1bfa6b8, loaded from [rdi + 8] just before); 0x1ccc9eb tests the pass in eax instead
// (`neg eax`). Found by walking GetStereoEyeView's 26 callers.
struct RightEyePick {
    std::uintptr_t rva;
    std::uint8_t view;
    bool passInEax;
};
inline constexpr RightEyePick kRightEyePicks[] = {
    {0x18d20d8, 6, false},  {0x18f6a99, 14, false}, {0x1916f2f, 3, false},  {0x19d15f4, 15, false},
    {0x19d2133, 6, false},  {0x1bfa6b8, 1, false},  {0x1cc4f01, 14, false}, {0x1cc5b51, 14, false},
    {0x1ccc9eb, 6, true},   {0x1d0236b, 14, false}, {0x24fc2df, 14, false}, {0x24fd2e1, 15, false},
};
inline constexpr std::uintptr_t kGetStereoEyeView = 0x2cf8140;

// Foliage (HISM) culling for an instanced eye pair: 4.26 FHierarchicalStaticMeshSceneProxy::GetDynamicMeshElements
// culls view 0 with its own frustum but the right plane of `Views[1]` (`check(Views.Num() == 2)`), so trees seen
// only by the right eye survive. Kunos made it `Views.Last()` (the right triple screen): with four views, view 3,
// the right focus view, so the eyes culled foliage past the focus box's right edge, in both eyes, and only on that
// side (README.md). And only view 0 gets the union, so the focus pair (view 2 primary) culled with view 2 alone.
// Found through foliage.DitheredLOD's reader (0x2a42ff1) and the IsInstancedStereoPass call (0x2a435c3) in the
// same function; `[rsp + 0x38]` is the loop's ViewIndex (0 at 0x2a43033, compared with Views.Num). views.cpp
// redirects three spots so each pair's primary (view 0 or 2) culls with its own left plane and its partner's right:
//  - the union's gate, `cmp dword [rsp + 0x38], 0` (then jne): `test dword [rsp + 0x38], ~2`, views 0 and 2;
//  - the left view, `mov rax, [rcx]` (Views[0]; rcx = Views.Data) and the next `movaps xmm4, xmm15`:
//    `rax = Views[ViewIndex]`;
//  - the right view's index, `movsxd rax, [rsi + 8]` (Views.Num, for `mov rcx, [rcx + rax*8 - 8]` at 0x2a43737)
//    and the next `mulps xmm0, xmm7`: `rax = ViewIndex + 2`, so Views[ViewIndex + 1].
inline constexpr std::uintptr_t kFoliageUnionGate = 0x2a435f2;
inline constexpr std::uint8_t kFoliageUnionGateAnchor[5] = {0x83, 0x7c, 0x24, 0x38, 0x00};
inline constexpr std::uintptr_t kFoliageLeftView = 0x2a43637;
inline constexpr std::uint8_t kFoliageLeftViewAnchor[7] = {0x48, 0x8b, 0x01, 0x41, 0x0f, 0x28, 0xe7};
inline constexpr std::uintptr_t kFoliageRightView = 0x2a4366a;
inline constexpr std::uint8_t kFoliageRightViewAnchor[7] = {0x48, 0x63, 0x46, 0x08, 0x0f, 0x59, 0xc7};
inline constexpr std::uintptr_t kFoliageRightViewRead = 0x2a43737;
inline constexpr std::uint8_t kFoliageRightViewReadAnchor[5] = {0x48, 0x8b, 0x4c, 0xc1, 0xf8};

}  // namespace acc

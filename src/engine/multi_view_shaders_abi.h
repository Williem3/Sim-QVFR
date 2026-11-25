// The multi-view shader service's private data on a game's D3D11 shaders and input layouts (multi_view_shaders.h):
// the GUIDs each twin or fact is stored under with SetPrivateData(Interface), and what it holds. A game mod reads them
// straight off the game's shaders with GetPrivateData(Interface), with no call into the core per draw.
#pragma once

#include <guiddef.h>

#include <cstdint>

namespace simqvfr::mvs {

// The private data a game vertex shader carries its converted twins under (an ID3D11VertexShader each): NVIDIA's
// single-pass stereo's and the instanced one.
inline constexpr GUID kMvrTwinGuid = {0x6d1c4a52, 0x3b8e, 0x4f61, {0x9a, 0x27, 0x5e, 0x10, 0xc3, 0x7b, 0x44, 0xd9}};
inline constexpr GUID kMvrInstancedTwinGuid = {0x2f7c9e14, 0x58a1, 0x4b3d, {0x8e, 0x06, 0xc1, 0x9d, 0x4a, 0x72, 0x35, 0xb0}};
// A game pixel shader's twin whose plain samples take a mip bias in the focus view's viewport and another outside it
// (engine/dxbc_patch.h add_focus_mip_bias; an ID3D11PixelShader): texture sharpening under DLSS (mvr_pass.cpp).
inline constexpr GUID kMvrMipBiasTwinGuid = {0x5a8e2c71, 0x0d4f, 0x4b96, {0xb1, 0x3e, 0x72, 0x9c, 0x05, 0xd8, 0x6e, 0x2a}};
// Other cars' motion vectors (lmu_car_track, car_motion.h): where a game vertex shader reads its `worldMatrix`, as two
// UINTs: the constant buffer slot and the byte offset in it (from the shader's reflection; none when it reads none).
inline constexpr GUID kVsWorldMatrixGuid = {0x3e9b7a14, 0x6c25, 0x4f81, {0x8d, 0x47, 0xa2, 0x19, 0xe5, 0x6b, 0x0c, 0x73}};
// With it, where the shader reads `viewMatrix` and `viewProjMatrix` in that same buffer, as two UINT byte offsets: the
// pass's camera for DLSS on any game build (view_pair.h). None when it reads them elsewhere or not at all.
inline constexpr GUID kVsViewMatricesGuid = {0x9b1e4c27, 0x3d58, 0x4a06, {0xa7, 0x92, 0x5c, 0xe1, 0x08, 0x3f, 0xb6, 0x4d}};
// Night car lighting (car_light.h): where a game vertex shader reads its floodlights (GlobalCBFloats_OmniLights'
// omniPosRad and omniRGBDelta), as four UINTs: the constant buffer slot, the two arrays' byte offsets, their length.
inline constexpr GUID kVsOmniLightsGuid = {0x8f2d6b41, 0x5c07, 0x4e93, {0xa1, 0x6e, 0x3b, 0xd8, 0x92, 0x0f, 0x47, 0xc5}};
// Night car lighting with the game's normals: a game vertex shader that hands its pixel shader the surface normal in
// the .z of TEXCOORD2-4 (engine/dxbc_patch.h vertex_normal_rows; a UINT 1), and a game pixel shader whose slot twin
// writes that normal to render target 2 (a UINT 1).
inline constexpr GUID kVsGameNormalGuid = {0x46c2e8b7, 0x91d3, 0x4a5f, {0x8b, 0x2e, 0x07, 0xf4, 0x6d, 0x19, 0xa3, 0x5c}};
inline constexpr GUID kPsSlotNormalGuid = {0xb53a07d1, 0x6e2c, 0x4f84, {0x9a, 0x41, 0xd8, 0x0c, 0x5e, 0x73, 0x2b, 0x96}};
// And whose slot twin writes its baked ambient occlusion (aoSpecularMap / aoSpecMask's .y) to that target's w (a UINT 1).
inline constexpr GUID kPsSlotAoGuid = {0x2e9c5f13, 0xa47b, 0x4d06, {0x83, 0x5a, 0x1f, 0x6e, 0xc9, 0x02, 0xb7, 0x48}};
// And whose slot twin writes the normal its normal maps bent (irradDiffuse's lookup) to render target 3 (a UINT 1).
// Night car lighting's headlights: where a game pixel shader reads its spotlights (GlobalCBFloats_SpotlightGlobal's
// spotLightGlobals, 64 bytes each) and, in the per-draw buffer (GlobalCBFloats_Draw_0, the vertex shaders' worldMatrix
// one), the draw's count of them (f_spotCount) and their indices (f_packedSpotLightIndex_0.., two 16-bit to a uint):
// four UINTs: the spotlights' slot, their count in the array, the count's offset, the indices' offset.
inline constexpr GUID kPsSpotsGuid = {0x95b1e4a2, 0x37cd, 0x4f60, {0x8e, 0x12, 0x5b, 0xd9, 0x61, 0x0a, 0xc4, 0x73}};
inline constexpr GUID kPsSlotDetailGuid = {0x7a05d3c8, 0x2f61, 0x4b9e, {0xa4, 0x18, 0x6c, 0x93, 0xe0, 0x5d, 0x27, 0xb1}};
// And a game pixel shader that shades a car body's lacquer (its reflection names lacquerThickness or clearCoatStrength):
// a UINT 1.
inline constexpr GUID kPsPaintGuid = {0x1d73a9e5, 0x42bc, 0x4f18, {0x96, 0x0d, 0xe4, 0x2a, 0x7b, 0x51, 0xc8, 0x3e}};
// Other cars' motion vectors, stage 3 (lmu_car_vectors; engine/dxbc_patch.h add_slot_output): a game pixel shader's twin
// that also writes the draw's car part slot to render target 1, and the same made of its mip bias twin (ID3D11PixelShader
// each; texture sharpening with the slots).
inline constexpr GUID kMvrSlotTwinGuid = {0x7c41d2e9, 0x1b5a, 0x4e0f, {0x9d, 0x33, 0x58, 0xa6, 0x2e, 0xc1, 0x07, 0x94}};
inline constexpr GUID kMvrSlotBiasTwinGuid = {0x2a96f0c3, 0x84d7, 0x4b21, {0xae, 0x15, 0x6f, 0x0b, 0xd2, 0x39, 0x7e, 0x58}};
// An input layout with per-instance data: its twin with every per-instance step rate doubled (an ID3D11InputLayout).
// A game shader's (vertex or pixel) declared size of the constant buffer in slot 0, in vectors (a UINT).
inline constexpr GUID kCb0VectorsGuid = {0x64b0e3a9, 0x2f17, 0x4c58, {0xb4, 0x0a, 0x91, 0x7e, 0x3d, 0x52, 0xc6, 0x1f}};
// A pixel shader's id (a uint64): its DXBC checksum's first two words, as the shader dump names its file (PS_<id>.dxbc).
inline constexpr GUID kPixelIdGuid = {0x3c8d1f52, 0x6a94, 0x4e07, {0x9b, 0x21, 0x5d, 0xe3, 0x70, 0x8a, 0x44, 0xc6}};
// A pixel shader's rows of constant buffer 0 read by a fixed index (512 bytes: dxbc::constant_buffer_static_rows).
inline constexpr GUID kCb0StaticRowsGuid = {0x5e2a7b14, 0xc39d, 0x4f61, {0x8e, 0x05, 0xa7, 0x3b, 0x91, 0xd2, 0x6c, 0x48}};
// The SCS games (mvr_probe_tint_rows): a vertex or pixel shader that reads a row of constant buffer 0 as a colour
// (dxbc::constant_buffer_colour_rows) carries 1024 bytes: the rows it reads by a fixed index (64 uint64), then its colour
// rows (64 uint64). Our copies must not rewrite those as a pair for its draws (the violet flash).
inline constexpr GUID kCb0TintRowsGuid = {0x0c9e4a71, 0x3b58, 0x4d26, {0x95, 0x1f, 0x6a, 0xe2, 0x07, 0xc4, 0x8b, 0x3d}};
// The SCS games' focus replays (mvr_probe_position_twins): a full-screen pass's vertex shader that turns its vertex into
// screen pixels carries its dxbc::add_position_remap twin (an interface) and the twin's rectangle and size rows (two ints).
inline constexpr GUID kFocusPositionTwinGuid = {0x8b3e51c7, 0x2d64, 0x4a19, {0x9f, 0x0e, 0x73, 0xc2, 0x58, 0xa1, 0x4d, 0x36}};
// The SCS games' scene draws (mvr_probe_own_pixel_loads): a pixel shader whose loads read its own pixel at a projected
// screen position carries its dxbc::add_own_pixel_loads twin (an interface): those loads at SV_Position.
inline constexpr GUID kOwnPixelLoadsTwinGuid = {0x3d71b9e4, 0x5a26, 0x4c18, {0x8f, 0x43, 0x92, 0xe0, 0x1b, 0x6d, 0xa7, 0x55}};
inline constexpr GUID kShaderIdGuid = {0x2b9e64d1, 0x8f30, 0x4c57, {0xb2, 0x1a, 0x6e, 0x95, 0x03, 0xd8, 0x47, 0xc2}};
inline constexpr GUID kCbRows4Guid = {0x7a15c3e8, 0x46b2, 0x4e9d, {0x81, 0x6c, 0x2f, 0xa0, 0x5b, 0x93, 0xd4, 0x17}};
inline constexpr GUID kFocusPositionRowsGuid = {0x1f6a9c38, 0x7e05, 0x4b72, {0xa4, 0xd9, 0x0c, 0x83, 0x6e, 0x27, 0xb5, 0x91}};
inline constexpr GUID kMvrInstancedLayoutGuid = {0x91e3b6d0, 0x7c42, 0x4e85, {0xa3, 0x5f, 0x0d, 0x28, 0xe6, 0x19, 0xbc, 0x47}};
// Every input layout the probe sees made, twinned or not: its kind (a UINT). Unmarked: not seen (made before the
// probe hooked the device), not known.
inline constexpr GUID kMvrLayoutKindGuid = {0x4c27a9e1, 0xd36b, 0x4f05, {0x8a, 0x71, 0x2e, 0xb4, 0x90, 0x5d, 0x13, 0xc8}};
inline constexpr std::uint32_t kMvrLayoutPerVertex = 1, kMvrLayoutPerInstance = 2;

}  // namespace simqvfr::mvs

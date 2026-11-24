// replay_probe.cpp's HLSL sources (our compute passes, compiled at the focus setup) and our slot / descriptor layout
// constants (replay_internal.h has the file map).
#pragma once

#include <d3d12.h>

namespace simqvfr::replay_detail {

// The pass constants copied into our slot with the off-axis focus remap (ACE's layout, recon-ace.md "R1"): left
// projections at rows 13/17/21/25, right at 95/103/107, each 4 rows x, y, z, w with w = (0,0,-1,0); row 38.xy target
// size, row 84 the viewport rect, row 112.z eyeHalfWidthPixels. Rows 128-131 keep each eye's unjittered x/y rows.
// Jitter (ACE's DLSS; 21:20 capture): row 85.xy is the frame's jitter in the eye's NDC (0 under MSAA), and the rows named
// "unjittered" carry it too: 13, 17, 25 and the right eye's 95, 103, 107 are all centre - jitter; only prevProjection
// (21) and inverseViewProjection (29-32) are without. So each eye's centre is row 17 / 103 plus row 85, and the rows'
// own jitter is row 85 by that list (not read back from a row's z: 21:38 capture, the focus views drawn 0.14 of the
// eye's NDC off). The focus views take the same jitter in their own pixels
// (the eye's NDC jitter x eye size / focus size), not the box's magnification of it: DLSS takes sub-pixel jitter.
constexpr char kPatchShader[] = R"(
cbuffer Source : register(b0) { float4 src[128]; };
cbuffer Params : register(b1) { float2 halfExtent; float2 focusSize; uint4 from0; uint4 from1; uint4 to0; uint4 to1; uint fogSrv; uint shadowSrv; uint fixPrev; uint rawCopy; uint4 atmo0; uint4 atmo1; uint blurSrv; uint fogSrv1; uint eyeMode; float mipBias; float4 gaze; float4 gazePrev; };
RWByteAddressBuffer dst : register(u0);
bool is_in(uint r, uint a, uint b, uint c, uint d) { return r == a || r == b || r == c || r == d; }
// ACE's single-pass stereo lists (eyeHalfWidthPixels, 112.z, set) carry the right eye's current unjittered projection
// in prevProjectionUnjitteredRight (107-110: the same as 103-106), so the right eye's velocities lack the frame-to-frame
// jitter change and DLSS smears its periphery (2026-11-24, preset K). fixPrev: the left eye's previous-to-current change
// (25-28 minus 17-20) added to the right eye's current one. Per-eye lists are left as they are.
// fixPrev bit 1: ACE's stereo lists draw the right eye with its vertical jitter the other way round from the left eye's
// (96/104/108 .z = centre + jy where the left's 14/18/26 .z = -jy; 2026-11-24 dumps), while its velocity packing and
// NGX take one jitter for both: the right eye's rows are given the left's sign.
float4 row(uint r) {
    const bool stereo = src[112].z != 0;
    if ((fixPrev & 1) != 0 && r >= 107 && r <= 110 && stereo) return src[r - 4] + (src[r - 82] - src[r - 90]);
    float4 v = src[r];
    if ((fixPrev & 2) != 0 && stereo && (r == 96 || r == 104 || r == 108)) v.z -= 2 * src[85].y;
    return v;
}
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 132) return;
    if (rawCopy != 0) {  // (Detailed logs: the constants as ACE gave them)
        if (i < 128) dst.Store4(i * 16, asuint(src[i]));
        return;
    }
    if (i >= 128) {
        const uint from[4] = {17, 18, 103, 104};
        float4 kept = src[from[i - 128]];
        kept.z += (i & 1) ? src[85].y : src[85].x;  // without the jitter
        dst.Store4(i * 16, asuint(kept));
        return;
    }
    float4 v = row(i);
    const bool left = i < 64;
    // The box's centre off the eye's forward axis (the gaze; NDC, per eye: gaze.xy left, .zw right; a right-eye list
    // holds its eye in the left rows). The previous frame's projections take the previous frame's centre, so velocities
    // and reprojection stay right while the box moves.
    const bool rightEye = !left || src[84].x > 0;
    const bool prevRow = (i >= 21 && i <= 28) || (i >= 107 && i <= 110);
    const float2 g = prevRow ? (rightEye ? gazePrev.zw : gazePrev.xy) : (rightEye ? gaze.zw : gaze.xy);
    const float2 jitter = src[85].xy;
    const float ux = (left ? src[17].z : src[103].z) + jitter.x;  // each eye's own centre, without the jitter
    const float uy = (left ? src[18].z : row(104).z) + jitter.y;
    // eyeMode (the live periphery): the eye's own projection, drawn smaller: no remap, the jitter kept in pixels
    const float ox = eyeMode ? 0 : ux - g.x, oy = eyeMode ? 0 : uy - g.y;
    const bool remap = eyeMode == 0;
    const float ax = 1.0 / halfExtent.x, ay = 1.0 / halfExtent.y;
    // The jitter's scale into the focus view's NDC: the same pixels' worth there as in the eye.
    const float sx = src[84].z > 0 ? src[84].z / focusSize.x : ax, sy = src[84].w > 0 ? src[84].w / focusSize.y : ay;
    if (is_in(i, 1, 13, 17, 21) || is_in(i, 25, 95, 103, 107)) {  // viewProjection and the projections
        const float own = i == 21 ? 0 : jitter.x;                 // this row's own jitter: all but prevProjection carry it
        v = ax * v + (ax * ox - own * (ax - sx)) * row(i + 3);    // x' = a x + b w, b = -a * centre = a * ox
    } else if (is_in(i, 2, 14, 18, 22) || is_in(i, 26, 96, 104, 108)) {
        const float own = i == 22 ? 0 : jitter.y;
        v = ay * v + (ay * oy - own * (ay - sy)) * row(i + 2);    // y' = c y + d w
    }
    if (i == 85) v.xy = jitter * float2(sx, sy);
    if (i == 90) v.x += mipBias;  // globalMaterialMipBias (the periphery texture bias: ACE's own eyes; 0 otherwise)
    if (i == 38 && remap) v.xy = float2(focusSize.x * 2, focusSize.y);  // (the eye stays in ACE's own target)
    if (i == 84) v = float4(v.x > 0 ? focusSize.x : 0, 0, focusSize.x, focusSize.y);  // a right-eye list: its offset
    if (i >= 29 && i <= 32)  // inverseViewProjection: inverse(R VP) = inverse(VP) inverse(R)
        v = float4(v.x * halfExtent.x, v.y * halfExtent.y, v.z, v.w - v.x * ox - v.y * oy);
    if (i == 112 && v.z != 0) v.z = focusSize.x;  // eyeHalfWidthPixels (a float, as ACE writes it; 0 in per-eye lists)
    // Bindless indices the game's -> ours (R2b tile lists, R2c previous-frame colour): entityTilesIndex (row 0.x),
    // entityTilesIndexRight (112.y), the screen-space map indices (rows 76, 77). Unused pairs never match.
    const uint from[8] = {from0.x, from0.y, from0.z, from0.w, from1.x, from1.y, from1.z, from1.w};
    const uint to[8] = {to0.x, to0.y, to0.z, to0.w, to1.x, to1.y, to1.z, to1.w};
    [unroll] for (uint k = 0; k < 8; ++k) {
        if (remap && i == 0 && asuint(v.x) == from[k]) v.x = asfloat(to[k]);
        if (remap && i == 112 && asuint(v.y) == from[k]) v.y = asfloat(to[k]);
        if (remap && (i == 76 || i == 77)) {
            if (asuint(v.x) == from[k]) v.x = asfloat(to[k]);
            if (asuint(v.y) == from[k]) v.y = asfloat(to[k]);
            if (asuint(v.z) == from[k]) v.z = asfloat(to[k]);
            if (asuint(v.w) == from[k]) v.w = asfloat(to[k]);
        }
    }
    // volumetricFogMapIndex (76.w): ACE's stereo lighting reads voxelGrid0 for both eyes; ours is eye 0's focus grid.
    if (i == 76 && fogSrv != 0) v.w = asfloat(src[84].x > 0 && fogSrv1 != 0 ? fogSrv1 : fogSrv);  // right-eye list: grid 1
    // deferredShadowMapIndex (76.x): ours, from the focus depth (R2c).
    if (i == 76 && shadowSrv != 0) v.x = asfloat(shadowSrv);
    // refractionMapIndex (76.z): our opaqueSceneBlur (P1).
    if (i == 76 && blurSrv != 0) v.z = asfloat(blurSrv);
    // The atmosphere maps (73.y in-scatter, 73.z out-scatter, 75.x / 75.y sky): our focus copies, the right eye's set
    // for a right-eye list (its own offset, row 84.x > 0), eye 0's otherwise (ACE's stereo lists read _0 for both).
    if (atmo0.x != 0 && (i == 73 || i == 75)) {
        const uint4 a = src[84].x > 0 ? atmo1 : atmo0;
        if (i == 73) { v.y = asfloat(a.x); v.z = asfloat(a.y); }
        else if (a.z != 0) { v.x = asfloat(a.z); v.y = asfloat(a.w); }  // sky maps: 0 = keep ACE's
    }
    dst.Store4(i * 16, asuint(v));
}
)";

// kPatchShader's Params as 32-bit values, at HLSL's packing (a vector never straddles a 16-byte register): 0-3 the
// floats, 4-11 from, 12-19 to, 20 fogSrv, 21 shadowSrv, 22 fixPrev, 23 rawCopy, 24-27 atmo0, 28-31 atmo1, 32 blurSrv, 33
// fogSrv1, 34 eyeMode, 35 mipBias, 36-39 gaze, 40-43 gazePrev.
constexpr UINT kPatchParams = 44;
// ACE's tyre shaders have no single-pass-stereo variant (the vertex shader takes no instance id and writes no viewport
// index; 16:43 capture): the pass constants copied with the right eye's matrices in the left eye's rows, for a second
// draw into the right eye's viewport. The tyre VS reads view (5-8), prevView (9-12), projection (13-16) and
// prevProjection (21-24); the right eye's are viewRight (91-94), prevViewRight (99-102), projectionRight (95-98) and
// prevProjectionUnjitteredRight (107-110; the previous jitter = the left eye's 21-24 minus its unjittered 25-28). The
// pixel shader takes its eye from the pixel position, as in ACE's stereo draws: the rest stays.
constexpr char kTyreRightShader[] = R"(
cbuffer Src : register(b0) { float4 src[113]; };
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 113) return;
    float4 v = src[i];
    if (i >= 5 && i <= 8) v = src[i + 86];
    else if (i >= 9 && i <= 12) v = src[i + 90];
    else if (i >= 13 && i <= 16) v = src[i + 82];
    else if (i >= 17 && i <= 20) v = src[i + 86];
    else if (i >= 21 && i <= 24) v = src[i + 86] + (src[i] - src[i + 4]);
    else if (i >= 25 && i <= 28) v = src[i + 82];
    dst.Store4(i * 16, asuint(v));
}
)";
constexpr UINT kTyreSlots = 64, kTyreSlotBytes = 2048;

// frameData (root 0, 1136 B) for the focus replays: tileCount (uint2 at 0, the lighting's tile-list stride) is the
// focus views' own grid. The game's grid is its eye's; a focus view with more pixels than the eye has more tiles, and
// with the game's stride its lists wrapped and ran out (18:39 capture: a dark band and blocks in the focus image).
// depthBindlessIndex (int at 1024, row 64.x): the scene depth ACE's particles sample at their own screen position for
// their depth fade (water spray, smoke: Material DepthFade_Enable). The game's names its eye's depth; read at a focus
// view's coordinates the spray compared against another part of the scene and faded out or flickered (2026-11-20, a
// wet race). Ours is the focus views' resolved scene depth (0: left as the game's).
constexpr char kFrameShader[] = R"(
cbuffer Src : register(b0) { float4 src[72]; };
cbuffer P : register(b1) { uint tx; uint ty; uint depthSrv; };
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 72) return;
    uint4 v = asuint(src[i]);
    if (i == 0) { v.x = tx; v.y = ty; }
    if (i == 64 && depthSrv != 0) v.x = depthSrv;
    dst.Store4(i * 16, v);
}
)";

constexpr UINT kSlotBytes = 4096;  // 2 KB constants + 4 kept rows, CBV-aligned
constexpr UINT kSlots = 512;
constexpr UINT kRtvSlots = 32, kDsvSlots = 16;

// Our descriptors at the top of the bindless range: 0-11 tile lists, 12 focus depth, 13 previous colour, 14-15 shadow,
// 16-27 fog, 28 normals UAV, 29-31 OIT views, 32-55 atmosphere (game SRVs, our SRVs, our UAVs), 56-60 post, 61-66
// refraction blur.
constexpr UINT kTileSlots = 160, kBindlessRange = 131072, kTileStride = 64;  // 16 uints a tile
// the sky: 80.. our CBV table sets (kSkyRing x kSkyTable), 110/111 our clouds' SRV / UAV
constexpr UINT kSkyTable = 10, kSkyRing = 3, kSkySlotsFirst = 80, kSkyCloudSrv = 110, kSkySlots = 16, kSkyCloudSize = 512;
constexpr UINT kFogFirst = 16, kFogUavSlots = 40;  // 0-11 fog, 24-27 packing, 28 CAS, 29-31 resolves, 32 MSAA AA
constexpr UINT kShadowSlots = 48, kShadowSlotBytes = 1024;
constexpr UINT kPostSlots = 16;  // 256 B each
constexpr UINT kFogSlots = 32, kFogSlotBytes = 16384;  // the injection constants are 15,840 B

// The culling constants (tiledCulling cb0, 240 B: inverseView, view, inverseProjectionMatrix at 128, tileCount 192,
// entityCount, dimension 208, pixelOffset 216, aabbTolerance) copied with the focus inverse projection and its region
// of our focus depth twin.
constexpr char kCullShader[] = R"(
cbuffer Source : register(b0) { float4 src[15]; };
cbuffer Params : register(b1) { float4 inverseProjection[4]; int2 dimension; int2 pixelOffset; float2 tiles; };
RWByteAddressBuffer dst : register(u0);
[numthreads(16, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 15) return;
    float4 v = src[i];
    if (i >= 8 && i < 12) v = inverseProjection[i - 8];
    if (i == 12) v.xy = tiles;  // tileCount: the focus views' own grid
    if (i == 13) v = float4(asfloat(dimension), asfloat(pixelOffset));
    dst.Store4(i * 16, asuint(v));
}
)";

// R2c: ACE's fog constants copied for a focus view. Injection (mode 0, 15,840 B: all 990 registers, the tail holds
// voxelGridSize, cameraForward, shLumaFactor): its matrices' columns are in the
// registers (column_major, out = M v): viewProjection 4-7 and prevViewProjection 8-11 get x' = a x + a ox w,
// y' = a y + a oy w per register; invViewProjection 16-19 the inverse (columns 0/1 x box, column 3 - ox c0 - oy c1).
// The eye's centre (ox, oy) = P02, P12 of P = VP inverse(view): view is rigid, so inverse(view)'s column 2 is the
// view rotation's row 2 (registers 0-2 .z). temporalReproject (24.z) off for a fresh history; historyIndex and
// outputIndex (25.z/w) ours. Scatter (mode 1, 32 B): outputIndex and inputIndex (1.x/y) ours.
constexpr char kFogShader[] = R"(
cbuffer Source : register(b0) { float4 src[990]; };
cbuffer Params : register(b1) { float halfExtent; uint mode; uint historySrv; uint outputUav; uint inputSrv; uint fresh; float gx; float gy; float halfY; };
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (mode == 1) {
        if (i >= 2) return;
        uint4 s = asuint(src[i]);
        if (i == 1) { s.x = outputUav; s.y = inputSrv; }
        dst.Store4(i * 16, s);
        return;
    }
    if (i >= 990) return;
    float4 v = src[i];
    const float ox = src[4].x * src[0].z + src[5].x * src[1].z + src[6].x * src[2].z - gx;  // less the gaze offset
    const float oy = src[4].y * src[0].z + src[5].y * src[1].z + src[6].y * src[2].z - gy;
    const float a = 1.0 / halfExtent, ay = 1.0 / halfY;
    if (i >= 4 && i <= 11) {
        v.x = a * v.x + a * ox * v.w;
        v.y = ay * v.y + ay * oy * v.w;
    }
    if (i == 16) v *= halfExtent;
    if (i == 17) v *= halfY;
    if (i == 19) v = v - ox * src[16] - oy * src[17];
    if (i == 24 && fresh != 0) v.z = asfloat(0u);
    if (i == 25) { v.z = asfloat(historySrv); v.w = asfloat(outputUav); }
    dst.Store4(i * 16, asuint(v));
}
)";

// R2c: ACE's deferred-shadow constants for the focus views. Mode 0: normalReconstructionViewSpace's (288 B): the
// inverse projections (left 4-7, right 12-15; columns in the registers) remapped like the fog's (columns 0/1 x box,
// column 3 - ox c0 - oy c1; ox = P02 = c3.x / c0.x, oy = c3.y / c1.y), bufferSize = both focus views, eyeSplitX and
// rcpEyeWidth the focus width. Mode 1: deferredShadows' (560 B) for eye `eye`: inverseViewProjection (0-3) the same
// way, with that eye's centre from the normals constants (b0).
constexpr char kShadowShader[] = R"(
cbuffer Normals : register(b0) { float4 nrm[18]; };
cbuffer Shadow : register(b1) { float4 shd[35]; };
cbuffer Params : register(b2) { float box; uint mode; uint eye; float fw; float fh; float boxY; float4 gaze; };  // gaze: value 8
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (mode == 0) {
        if (i >= 18) return;
        float4 v = nrm[i];
        if ((i >= 4 && i <= 7) || (i >= 12 && i <= 15)) {
            const uint b = i < 8 ? 4 : 12;
            const float2 g = i < 8 ? gaze.xy : gaze.zw;
            const float ox = nrm[b + 3].x / nrm[b].x - g.x, oy = nrm[b + 3].y / nrm[b + 1].y - g.y;
            if (i == b) v *= box;
            if (i == b + 1) v *= boxY;
            if (i == b + 3) v = v - ox * nrm[b] - oy * nrm[b + 1];
        }
        if (i == 16) v = float4(2 * fw, fh, 1.0 / (2 * fw), 1.0 / fh);
        if (i == 17) { v.z = fw; v.w = 1.0 / fw; }
        dst.Store4(i * 16, asuint(v));
        return;
    }
    if (i >= 35) return;
    float4 v = shd[i];
    const uint b = eye == 0 ? 4 : 12;
    const float2 g = eye == 0 ? gaze.xy : gaze.zw;
    const float ox = nrm[b + 3].x / nrm[b].x - g.x, oy = nrm[b + 3].y / nrm[b + 1].y - g.y;
    if (i == 0) v *= box;
    if (i == 1) v *= boxY;
    if (i == 3) v = v - ox * shd[0] - oy * shd[1];
    dst.Store4(i * 16, asuint(v));
}
)";

// R2c: an atmosphere map resampled for a focus view. ACE's maps are screen-shaped (looked up at (SV_Position - offset) /
// size, row 84; 3D ones with a log-distance slice in z): the focus texel at focus uv takes the eye map at the eye uv
// the remap gives (eye NDC = centre + box * focus NDC); z slices kept. Built twice: 2D (ATMO_3D 0) and 3D.
constexpr char kAtmoShader[] = R"(
cbuffer P : register(b0) { float box; float cx; float cy; uint sx; uint sy; uint sz; uint stereo; float cx1; float cy1; float mipLevel; float boxY; float ux; float uy; };
SamplerState lin : register(s0);
#if ATMO_3D
Texture3D<float4> src : register(t0);
RWTexture3D<float4> dst : register(u0);
#else
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
#endif
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= sx || id.y >= sy || id.z >= sz) return;
    float2 uf = (float2(id.xy) + 0.5) / float2(sx, sy);
    float2 c = float2(cx, cy);
    float eye = 0;
    if (stereo != 0) {  // two eyes side by side (the veil): each half through its own eye's box
        eye = uf.x >= 0.5 ? 1 : 0;
        uf.x = uf.x * 2 - eye;
        if (eye > 0) c = float2(cx1, cy1);
    }
    const float2 ndc = c + float2(box, boxY) * float2(uf.x * 2 - 1, 1 - uf.y * 2);
    float2 ue = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (stereo != 0) ue.x = (ue.x + eye) * 0.5;
    if (ux > 0) ue *= float2(ux, uy);  // the source's region in use (the live periphery: ACE's eyes drawn smaller)
#if ATMO_3D
    dst[id] = src.SampleLevel(lin, float3(ue, (id.z + 0.5) / sz), 0);
#else
    dst[id.xy] = src.SampleLevel(lin, ue, mipLevel);
#endif
}
)";

// P1: a constant buffer copied with one register replaced (the tonemap's target size).
constexpr char kCbPatchShader[] = R"(
cbuffer Src : register(b0) { float4 src[16]; };
cbuffer P : register(b1) { uint count; uint reg; uint mask; uint pad; float4 value; };  // mask 0: all of the register
RWByteAddressBuffer dst : register(u0);
[numthreads(16, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= count) return;
    float4 v = src[i];
    if (i == reg && !((mask & 16) && asint(v.x) < 0)) {  // mask 16: an index of -1 (none) stays
        if (mask == 0 || (mask & 1)) v.x = value.x;
        if (mask == 0 || (mask & 2)) v.y = value.y;
        if (mask == 0 || (mask & 4)) v.z = value.z;
        if (mask == 0 || (mask & 8)) v.w = value.w;
    }
    dst.Store4(i * 16, asuint(v));
}
)";

// P2a: ACE's VelocityAndDepthPacking constants (352 B, 22 registers) for a focus eye: inverseViewProj (0-3),
// viewProj (4-7), prevViewProj (8-11), columns in the registers: x' = a x + a ox w per column, the inverse columns 0/1 x
// box and column 3 - ox c0 - oy c1 (ox = -the eye's NDC centre); rcpRes and resolution the focus size; the jitters (pixels;
// 12.zw half the step from last frame's, 14.xy this frame's) as ACE's: the focus views jitter by the same pixels; pixelOffset the focus eye's; the texture indices ours (SRVs: velocity, depth, flags; UAVs: MB, velocity only,
// flags output, AA).
constexpr char kPackShader[] = R"(
cbuffer Src : register(b0) { float4 src[22]; };
cbuffer P : register(b1) { float box; float ox; float oy; float k; float fw; float fh; float offX; uint velSrv;
                           uint depthSrv; uint flagsSrv; uint mbUav; uint velOnlyUav; uint flagsUav; uint aaUav; float pox; float poy; float boxY; float ky; };
RWByteAddressBuffer dst : register(u0);
[numthreads(32, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 22) return;
    float4 v = src[i];
    const float a = 1.0 / box, ay = 1.0 / boxY;
    if (i == 0) v *= box;
    if (i == 1) v *= boxY;
    if (i == 3) v = v - ox * src[0] - oy * src[1];
    if (i >= 4 && i <= 7) { v.x = a * v.x + a * ox * v.w; v.y = ay * v.y + ay * oy * v.w; }
    if (i >= 8 && i <= 11) { v.x = a * v.x + a * pox * v.w; v.y = ay * v.y + ay * poy * v.w; }  // last frame's box
    if (i == 12) v.xy = float2(1.0 / fw, 1.0 / fh);
    if (i == 13) { v.x = asfloat(velSrv); v.y = asfloat(depthSrv); }
    if (i == 15) { v.x = asfloat(mbUav); v.y = asfloat(uint(fw)); v.z = asfloat(uint(fh)); }
    if (i == 16) { v.y = asfloat(velOnlyUav); v.w = asfloat(flagsSrv); }
    if (i == 17) v.x = offX;
    if (i == 20) v.w = asfloat(flagsUav);
    if (i == 21) v.w = asfloat(aaUav);
    dst.Store4(i * 16, asuint(v));
}
)";

// P2c: ACE's vrAntialiasing constants (272 B, 17 registers) for a focus eye: TextureSize (0.xy) the focus target;
// inverseViewProj (3-6), viewProj (7-10), prevViewProj (11-14) remapped like the packing's (columns in the registers);
// viewportOffset / Size (16) the focus eye's.
constexpr char kAaShader[] = R"(
cbuffer Src : register(b0) { float4 src[17]; };
cbuffer P : register(b1) { float box; float ox; float oy; float fw; float fh; float offX; float pox; float poy; float boxY; };
RWByteAddressBuffer dst : register(u0);
[numthreads(32, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 17) return;
    float4 v = src[i];
    const float a = 1.0 / box, ay = 1.0 / boxY;
    if (i == 0) v.xy = float2(2 * fw, fh);
    if (i == 3) v *= box;
    if (i == 4) v *= boxY;
    if (i == 6) v = v - ox * src[3] - oy * src[4];
    if (i >= 7 && i <= 10) { v.x = a * v.x + a * ox * v.w; v.y = ay * v.y + ay * oy * v.w; }
    if (i >= 11 && i <= 14) { v.x = a * v.x + a * pox * v.w; v.y = ay * v.y + ay * poy * v.w; }  // last frame's box
    if (i == 16) v = float4(offX, 0, fw, fh);
    dst.Store4(i * 16, asuint(v));
}
)";

// replay_atmosphere=4: ACE's scatteringSettings (96 B, 6 registers) for a focus eye: the ray grid narrowed to the box.
// Per cell ray = eyeDirection + dx x + dy y over the W x H grid; the focus cell (x, y) is the eye's cell (ox + b x,
// oy + b y): eyeDirection' = eyeDirection + ox dx + oy dy, dx' = b dx, dy' = b dy (w components kept).
constexpr char kAtmoGridShader[] = R"(
cbuffer Src : register(b0) { float4 src[6]; };
cbuffer P : register(b1) { float box; float ox; float oy; float p3; float p4; float p5; };
RWByteAddressBuffer dst : register(u0);
[numthreads(8, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= 6) return;
    float4 v = src[i];
    if (i == 0) v.xyz = src[0].xyz + ox * src[2].xyz + oy * src[3].xyz;
    if (i == 2) v.xyz = box * src[2].xyz;
    if (i == 3) v.xyz = p3 * src[3].xyz;  // p3: the box's height
    dst.Store4(i * 16, asuint(v));
}
)";

// The sky's screen-space clouds for a focus view: the cloud texture the sky draw's cbSky names (CloudsScreenBindless
// Index, register 4.x; read on the GPU) sampled through the box (eye uv = centre + box x focus uv) into ours.
constexpr char kSkyCloudShader[] = R"(
cbuffer Sky : register(b0) { float4 sky[5]; };
cbuffer P : register(b1) { float box; float cx; float cy; uint size; float boxY; };
Texture2D<float4> bindless[] : register(t0, space1);
RWTexture2D<float4> dst : register(u0);
SamplerState lin : register(s0);
[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
    if (id.x >= size || id.y >= size) return;
    // ACE's own rule for this lookup (the sky PS's screen-clouds sample: index < range ? index : 0; other lookups
    // fall back to 1): no clouds (clear sky) is -1, which reads descriptor 0.
    uint idx = asuint(sky[4].x);
    if (idx >= 131072) idx = 0;
    const float2 uf = (float2(id) + 0.5) / size;
    const float2 ndc = float2(cx, cy) + float2(box, boxY) * float2(uf.x * 2 - 1, 1 - uf.y * 2);
    const float2 ue = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    dst[id] = bindless[NonUniformResourceIndex(idx)].SampleLevel(lin, ue, 0);
}
)";

}  // namespace simqvfr::replay_detail

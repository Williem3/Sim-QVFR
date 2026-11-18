// ATS's full-screen passes pointed at a corner of their targets (docs/recon-ats.md, local "Full-screen passes and the room
// problem", "Stage 1 design notes"): the rules that rewrite one draw's constant range, by value. The same rules run on
// the GPU (corner.cpp's compute shader) on a copy of the game's range; this CPU version is their reference (tests).
//
// Prism's full-screen passes carry their region as constants (its deferred parameters, named in the exe):
// normalized_to_texture (1,1,1,1) followed by viewport_size (w,h,1,1): screen -> texture scale in xy, its inverse in zw;
// the viewport's pixel size and its share of the texture. For a corner fx x fy of the target: (fx,fy,1/fx,1/fy) and
// (fx*w, fy*h, fx, fy) ("the pair").
//
// A range is bound in blocks of 16 vectors and a shader declares fewer: the rest is whatever older frames left in the
// game's buffer, often a pair of some other draw. So nothing here may be decided by scanning the range (S1 runs 2-6:
// a left-over pair behind a bloom downsample's two vectors made "the pair" win, its source rectangle stayed whole and
// the bloom came out shrunk towards the top-left on some frames: the strobe on the cab roof; the final blit likewise:
// "a smaller copy popping in"). A vertex range is told by its first two vectors, which every layout declares:
//   V1. (0,0,1,1) then (1,-0.5,0.5,*): screen rectangle, flip: the pair follows at 2 or 3 -> the pair rewritten.
//   V2. (1,-0.5,0.5,*) then (0,0,1,1): flip, source rectangle (the downsamples and blurs: uv = rect, times the pair's
//       xy where one follows) -> the rectangle (0,0,fx,fy); a pair behind it stays as it is (rewritten too, the blurs
//       would shrink twice; behind a downsample it is a left-over).
//   V3. (0,0,1,1) then (0,0,1,1): the final blit's source and destination -> the source (0,0,fx,fy).
//   V4. a size (w,h,1,1) then a pixel rectangle inside it (the histogram's region) -> the rectangle times f.
// A pixel range only ever needs the pair: every pair in it is rewritten (one in the left-over part harms nothing).
#pragma once

#include <cmath>

namespace scs {

enum class CornerRule { none, pair, rect, pixelRect };

inline bool corner_eq(const float (&v)[4], float x, float y, float z, float w) {
    return v[0] == x && v[1] == y && v[2] == z && v[3] == w;
}
// A size vector (w,h,1,1): w and h at least 2, whole numbers.
inline bool corner_size(const float (&v)[4]) {
    return v[0] >= 2 && v[1] >= 2 && v[2] == 1 && v[3] == 1 && v[0] == std::floor(v[0]) && v[1] == std::floor(v[1]);
}
// Prism's flip vector (1,-0.5,0.5,*).
inline bool corner_flip(const float (&v)[4]) { return v[0] == 1 && v[1] == -0.5f && v[2] == 0.5f; }

inline bool corner_pair_at(float (*v)[4], int i, int count, float fx, float fy) {
    if (i + 1 >= count || !corner_eq(v[i], 1, 1, 1, 1) || !corner_size(v[i + 1]))
        return false;
    const float w = v[i + 1][0], h = v[i + 1][1];
    v[i][0] = fx, v[i][1] = fy, v[i][2] = 1 / fx, v[i][3] = 1 / fy;
    v[i + 1][0] = fx * w, v[i + 1][1] = fy * h, v[i + 1][2] = fx, v[i + 1][3] = fy;
    return true;
}

// Rewrites a range of `count` vectors for a corner fx x fy (0 < f <= 1); `vertex`: a vertex shader's range (V1-V4),
// else a pixel shader's (every pair). Returns the rule applied.
inline CornerRule corner_patch(float (*v)[4], int count, float fx, float fy, bool vertex = true) {
    if (!vertex) {
        bool pair = false;
        for (int i = 0; i + 1 < count; ++i)
            if (corner_pair_at(v, i, count, fx, fy)) {
                pair = true;
                ++i;
            }
        return pair ? CornerRule::pair : CornerRule::none;
    }
    if (count < 2)
        return CornerRule::none;
    const bool rect0 = corner_eq(v[0], 0, 0, 1, 1), rect1 = corner_eq(v[1], 0, 0, 1, 1);
    if (rect0 && corner_flip(v[1]))
        return corner_pair_at(v, 2, count, fx, fy) || corner_pair_at(v, 3, count, fx, fy) ? CornerRule::pair
                                                                                             : CornerRule::none;
    if (corner_flip(v[0]) && rect1) {
        v[1][2] = fx, v[1][3] = fy;
        return CornerRule::rect;
    }
    if (rect0 && rect1) {
        v[0][2] = fx, v[0][3] = fy;
        return CornerRule::rect;
    }
    float (&r)[4] = v[1];
    if (corner_size(v[0]) && r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && r[0] + r[2] <= v[0][0] &&
        r[1] + r[3] <= v[0][1] && !(r[2] == 1 && r[3] == 1)) {
        r[0] *= fx, r[1] *= fy, r[2] *= fx, r[3] *= fy;
        return CornerRule::pixelRect;
    }
    return CornerRule::none;
}

// The same rules in HLSL (cs_5_0): 64 threads, one group; `count` vectors from t0 (a raw buffer) to u0. cb0: f, count,
// flags (1: a vertex range; 2: for the focus view), the focus box and its clip remap. Thread 0 rewrites after all have
// copied.
//
// For the focus view (stage 3: a full-screen pass drawn again over the focus region, its inputs' focus regions copied
// to the top-left of textures of ours) a pixel range also gets the focus camera:
//   - viewport_coord_to_es_ray, three vectors after the pair's first: ray(s) = s * zw + xy over the eye's screen; the
//     focus view's screen s' covers the box, s = box.xy + s' * box.zw, so (xy + box.xy * zw, box.zw * zw).
//   - the AO compute pass's radius and its cap, over the box's share (see the shader below).
//   - every view -> clip matrix beside its view -> view matrix (Prism's v_to_prev_vp next to v_to_prev_v: four rows
//     whose last is the negative of the other's third row, the other's last (0,0,0,1)): its x and y rows through the
//     clip remap (x' = a x + b w, y' = c y + d w), so the reprojections of the TAA and the AO land in the focus view.
inline constexpr char kCornerPatchCs[] = R"(
// `px`: the region's share of the texture size the game names (f itself, except for a focus replay: its inputs are twins,
// smaller than the game's textures, so the region's share of a twin (f) isn't its share of the game's size (px)).
cbuffer Params : register(b0) { float2 f; uint count; uint flags; float4 box; float4 remap; float2 px; float2 pad; };
// flags & 4: the rows the shader reads by a fixed index (bit r & 31 of rows[r >> 7][(r >> 5) & 3]). Every focus rule writes
// only such rows (a whole matrix, a pair, its ray, the AO's two): run 16's Aftermath markers (2026-11-17) put the hang in
// the occluder pass's replay even with only the written rows checked: a leftover matrix at rows 31-34 remapped row 32.
// Before: ATS's occluder pass (2026-11-17) keeps its occluder count in cb0[32].x and
// its occluders in an array behind it (read cb0[i + 33]); leftovers of other draws' matrices there matched the rule,
// its x and y rows were remapped over the count, and the occluder loop ran ~a billion times: the GPU hang.
cbuffer Rows : register(b1) { uint4 rows[32]; };
bool fixedRow(uint r) { return (flags & 4) == 0 || (r < 4096 && ((rows[r >> 7][(r >> 5) & 3] >> (r & 31)) & 1) != 0); }
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
float4 at(uint i) { return asfloat(src.Load4(i * 16)); }
bool eq(float4 v, float4 w) { return all(v == w); }
bool isSize(float4 v) { return v.x >= 2 && v.y >= 2 && v.z == 1 && v.w == 1 && v.x == floor(v.x) && v.y == floor(v.y); }
bool isFlip(float4 v) { return v.x == 1 && v.y == -0.5 && v.z == 0.5; }
bool pairAt(uint p) {
    if (p + 1 >= count)
        return false;
    float4 a = at(p), b = at(p + 1);
    if (!eq(a, float4(1, 1, 1, 1)) || !isSize(b) || !fixedRow(p) || !fixedRow(p + 1))
        return false;
    dst.Store4(p * 16, asuint(float4(f, 1 / f)));
    dst.Store4((p + 1) * 16, asuint(float4(px * b.xy, f)));
    if ((flags & 3) == 2 && p + 3 < count && fixedRow(p + 3)) {  // the focus view's ray
        float4 ray = at(p + 3);
        dst.Store4((p + 3) * 16, asuint(float4(ray.xy + box.xy * ray.zw, box.zw * ray.zw)));
    }
    return true;
}
void remapMatrix(uint j) {  // rows j, j+1 (x, y) with row j+3 (w)
    if (!fixedRow(j) || !fixedRow(j + 1) || !fixedRow(j + 2) || !fixedRow(j + 3))  // the whole matrix (run 16: rows 31-34 over the count at 32)
        return;  // not rows the shader reads as a matrix (an array's, or leftovers)
    float4 x = at(j), y = at(j + 1), w = at(j + 3);
    dst.Store4(j * 16, asuint(remap.x * x + remap.y * w));
    dst.Store4((j + 1) * 16, asuint(remap.z * y + remap.w * w));
}
[numthreads(64, 1, 1)] void main(uint t : SV_GroupIndex) {
    for (uint i = t; i < count; i += 64)
        dst.Store4(i * 16, src.Load4(i * 16));
    GroupMemoryBarrierWithGroupSync();
    DeviceMemoryBarrierWithGroupSync();
    if (t != 0)
        return;
    if ((flags & 1) == 0) {
        for (uint p = 0; p + 1 < count; ++p)
            if (pairAt(p))
                ++p;
        // The AO's main pass (its range: (0.5,0.5,..), gtao_config0, gtao_config1, the pair). Its radius is a share of
        // the view's screen (config0.w over the distance, at most config1.y), and the same number times the distance is
        // its reach in the world (the falloff). The focus view's screen is box.z of the eye's, so the share must grow by
        // 1 / box.z on the screen but not in the world: the config stays, and the two factors that turn the share into
        // a step do the growing: the half-size scale (0.5,0.5: the radius in pixels) and normalized_to_texture.xy (the
        // step in texture coordinates; its only use in this shader). S4 run 2 scaled the config instead: the occlusion
        // then reached 1 / box.z as far in the world (S6: a wide dark band under the cab's visor, the box darker).
        if ((flags & 2) && count >= 5 && all(at(0).xy == float2(0.5, 0.5)) && eq(at(3), float4(1, 1, 1, 1)) && isSize(at(4)) &&
            fixedRow(0) && fixedRow(3)) {
            float4 h = at(0);
            dst.Store4(0, asuint(float4(h.xy / box.z, h.zw)));
            dst.Store4(48, asuint(float4(f / box.z, 1 / f)));
        }
        if (flags & 2)
            for (uint n = 0; n + 4 <= count; ++n) {
                if (!eq(at(n + 3), float4(0, 0, 0, 1)))
                    continue;
                float4 back = -at(n + 2);
                if (n >= 4 && eq(at(n - 1), back))
                    remapMatrix(n - 4);
                if (n + 8 <= count && eq(at(n + 7), back))
                    remapMatrix(n + 4);
            }
        return;
    }
    if (count < 2)
        return;
    float4 a = at(0), b = at(1);
    const float4 whole = float4(0, 0, 1, 1);
    if (eq(a, whole) && isFlip(b)) {
        if (!pairAt(2))
            pairAt(3);
    } else if (isFlip(a) && eq(b, whole)) {
        dst.Store4(16, asuint(float4(0, 0, f)));
    } else if (eq(a, whole) && eq(b, whole)) {
        dst.Store4(0, asuint(float4(0, 0, f)));
    } else if (isSize(a) && b.x >= 0 && b.y >= 0 && b.z > 0 && b.w > 0 && b.x + b.z <= a.x && b.y + b.w <= a.y &&
               !(b.z == 1 && b.w == 1)) {
        dst.Store4(16, asuint(b * float4(px, px)));
    }
}
)";

// The whole-buffer pass (corner.cpp's mirrors of the game's large constant buffers, each written once at the frame's
// start): every normalized_to_texture / viewport_size pair whose size has the eye's shape (so a shadow map's or a
// probe's size is left alone), in parallel, one thread per vector. cb0: f, count, the eye's size. (Measured on ETS2
// 2026-11-15 over 10,800 frames: every pair it took was one of the eye's target sizes or their halvings.)
inline constexpr char kCornerMirrorCs[] = R"(
cbuffer Params : register(b0) { float2 f; uint count; uint pad; float2 eye; float2 pad2; };
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
float4 at(uint i) { return asfloat(src.Load4(i * 16)); }
bool isOnes(float4 v) { return all(v == float4(1, 1, 1, 1)); }
bool isEyeSize(float4 v) {
    return v.z == 1 && v.w == 1 && v.x >= 16 && v.x <= eye.x && v.y <= eye.y && v.x == floor(v.x) && v.y == floor(v.y) &&
           abs(v.x * eye.y - v.y * eye.x) <= max(eye.x, eye.y);
}
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {
    if (i >= count)
        return;
    float4 v = at(i);
    if (i > 0 && isOnes(at(i - 1)) && isEyeSize(v))
        v = float4(f * v.xy, f);
    else if (i + 1 < count && isOnes(v) && isEyeSize(at(i + 1)))
        v = float4(f, 1 / f);
    dst.Store4(i * 16, asuint(v));
}
)";

}  // namespace scs

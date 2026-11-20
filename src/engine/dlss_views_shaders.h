// The HLSL of dlss_views.cpp: the motion vectors, the gate and the stretch (compiled at runtime, macro MS).
#pragma once

namespace simqvfr {

// One full-screen triangle; the pixel shader resolves the view's depth (sample 0 of the scene's multisample depth, or
// the texel of a single-sample one) and writes its motion vector: the pixel reprojected into the frame before.
inline constexpr char kVectorsShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;    // this frame's clip (as rendered) -> the frame before's (without its jitter): the world
    row_major float4x4 R2;   // the same for what is fixed to the player's seat (the head's motion alone)
    float4 js;               // this frame's clip shift x, y; the view's width, height
    float4 origin;           // the view's top-left in the depth texture; the projection's depth terms
    float4 extra;            // x: 1 with the car's box on
    row_major float4x4 T;    // this frame's clip -> the tracking space (through the head's view)
    float4 head;             // the eye's position in the tracking space
    float4 boxMin;           // the car's box about it: x right, y up, z back
    float4 boxMax;
    row_major float4x4 Inv;  // moving objects: this frame's clip (as rendered) -> the world
    row_major float4x4 B;    // the world -> the frame before's clip (without its jitter)
    float4 objects;          // x: how many object slots (0: none)
    float4 band;             // a band of the view's rows (DlssPass::focusBandTop): x its top row, y the view's height
                             // (0: no band), z the band's move since the frame before (rows, its vectors' y)
};
// DEPTH_MS: the depth multisample (its first sample read); MS: the object slots too. MS alone: the depth is our copy
// (dlss_depth_copy_list, single-sample) while the scene and its slots are multisample.
#ifdef DEPTH_MS
Texture2DMS<float> depthIn : register(t0);
#else
Texture2D<float> depthIn : register(t0);
#endif
// Moving objects (DlssPass::objectSlots, objectMotion): the surface's object slot + 1 (0: the world), each object's
// motion in the world (rows: a point [x y z 1] times it is where it was the frame before).
#ifdef MS
Texture2DMS<uint> slotsIn : register(t1);  // multisample with the scene: its first sample, as the depth's
#else
Texture2D<uint> slotsIn : register(t1);
#endif
struct Motion {
    float3 r0, r1, r2, r3;
};
StructuredBuffer<Motion> motionIn : register(t2);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
bool in_car(float2 ndc, float d) {
    if (extra.x <= 0)
        return false;
    float4 q = mul(float4(ndc, d, 1), T);
    if (abs(q.w) < 1e-9)
        return false;
    float3 rel = q.xyz / q.w - head.xyz;
    return all(rel > boxMin.xyz) && all(rel < boxMax.xyz);
}
struct Out {
    float depth : SV_Target0;
    float2 motion : SV_Target1;
};
Out ps(float4 pos : SV_Position) {
    int2 p = int2(pos.xy) + int2(origin.xy);
#ifdef DEPTH_MS
    float d = depthIn.Load(p, 0);
#else
    float d = depthIn.Load(int3(p, 0));
#endif
    // In a band of the view's rows: the pixel's place in the whole view (the reprojection's clip is the view's).
    float2 size = band.y > 0 ? float2(js.z, band.y) : js.zw;
    float2 at = pos.xy + float2(0, band.x);
    float2 ndc = float2(at.x / size.x * 2 - 1, 1 - at.y / size.y * 2);
    bool car = in_car(ndc, d);
    float4 before = car ? mul(float4(ndc, d, 1), R2) : mul(float4(ndc, d, 1), R);
    if (!car && objects.x > 0) {  // a moving object's surface: its point moved back with it, then into the frame before
#ifdef MS
        uint word = slotsIn.Load(p, 0);
#else
        uint word = slotsIn.Load(int3(p, 0));
#endif
        if (word > 0 && word <= (uint)objects.x) {
            float4 w = mul(float4(ndc, d, 1), Inv);
            if (abs(w.w) > 1e-20) {
                float3 at = w.xyz / w.w;
                Motion m = motionIn[word - 1];
                float3 was = at.x * m.r0 + at.y * m.r1 + at.z * m.r2 + m.r3;
                before = mul(float4(was, 1), B);
            }
        }
    }
    Out o;
    o.depth = d;
    // Where it was, in the band as it was (its move added: the frame before's band rows start band.z rows higher).
    o.motion = (before.w > 1e-6 ? (before.xy / before.w - (ndc - js.xy)) * float2(0.5 * size.x, -0.5 * size.y) : float2(0, 0)) +
               float2(0, band.z);
    return o;
}
)";

// The gate: over a 64x64 grid, 16 points a cell, apart for the world (first target) and what is taken for the player's
// car (in its box, its vector the head's motion alone; with the box off: nearer than 8 m, the world's vectors; second
// target), each point looked up in the frame before at its own jitter (that frame was rendered with it). Its colour,
// both frames after the same small blur (a 3x3 tent: four bilinear taps half a texel off): how much the frame differs
// from the one before where the point stood (x), where its vector says it was (y), a pixel off that (z, four ways); the
// points counted (w). Its depth (third and fourth targets), which detail finer than a pixel doesn't scramble as it does
// the colour: whether the frame before's depth where the vector says the point was is the depth the reprojection
// predicts for it (within 2%), and whether it is this frame's where the point stood; for points within 1 km. Third
// target: the world's summed vector length in pixels, its points whose depth agrees along the vectors, standing; the
// car's summed length. Fourth: the car's agreeing along the vectors, standing; the points depth judged, the world's and
// the car's.
inline constexpr char kCheckShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;
    row_major float4x4 R2;
    float4 js;
    float4 origin;           // z, w: the projection's depth terms (depth = z + w / distance)
    float4 extra;            // x: 1 with the car's box on; y, z: the frame before's clip shift (its jitter as rendered)
    row_major float4x4 T;    // this frame's clip -> the tracking space (through the head's view)
    float4 head;             // the eye's position in the tracking space
    float4 boxMin;           // the car's box about it: x right, y up, z back
    float4 boxMax;
};
Texture2D<float4> nowIn : register(t0);
Texture2D<float4> beforeIn : register(t1);
Texture2D<float> depthIn : register(t2);
Texture2D<float> depthBeforeIn : register(t3);
SamplerState linearClamp : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float lum(float3 c) { return log2(1 + dot(max(c, 0), float3(0.2126, 0.7152, 0.0722))); }
// Whether `predicted` lies within the depths about uv (the four texels a bilinear lookup there would blend), 2% wider:
// a point between texels on a slope (the road far ahead changes depth faster than 2% a pixel in a small view) and an
// edge the jitter moved (the point's own side among them) both agree.
bool agrees(float2 uv, float predicted) {
    float4 seen = depthBeforeIn.GatherRed(linearClamp, uv);
    float lo = min(min(seen.x, seen.y), min(seen.z, seen.w)), hi = max(max(seen.x, seen.y), max(seen.z, seen.w));
    float slack = 0.02 * max(abs(predicted), 1e-7);
    return predicted >= lo - slack && predicted <= hi + slack;
}
float tent(Texture2D<float4> t, float2 uv) {
    float2 h = 0.5 / js.zw;
    float3 c = t.SampleLevel(linearClamp, uv + float2(-h.x, -h.y), 0).rgb + t.SampleLevel(linearClamp, uv + float2(h.x, -h.y), 0).rgb +
               t.SampleLevel(linearClamp, uv + float2(-h.x, h.y), 0).rgb + t.SampleLevel(linearClamp, uv + h, 0).rgb;
    return lum(0.25 * c);
}
float around(Texture2D<float4> t, float2 uv, float v) {
    float2 px = 1 / js.zw;
    return 0.25 * (abs(v - tent(t, uv + float2(px.x, 0))) + abs(v - tent(t, uv - float2(px.x, 0))) +
                   abs(v - tent(t, uv + float2(0, px.y))) + abs(v - tent(t, uv - float2(0, px.y))));
}
bool where(float2 ndc, float d, float4x4 m, out float2 uv, out float predicted) {
    float4 b = mul(float4(ndc, d, 1), m);
    uv = float2(0, 0);
    predicted = 0;
    if (b.w <= 1e-6)
        return false;
    float2 n = b.xy / b.w + extra.yz;
    uv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    predicted = b.z / b.w;
    return all(abs(n) < 1);
}
bool in_car(float2 ndc, float d) {
    if (extra.x <= 0)
        return false;
    float4 q = mul(float4(ndc, d, 1), T);
    if (abs(q.w) < 1e-9)
        return false;
    float3 rel = q.xyz / q.w - head.xyz;
    return all(rel > boxMin.xyz) && all(rel < boxMax.xyz);
}
struct Out {
    float4 world : SV_Target0;
    float4 car : SV_Target1;
    float4 more : SV_Target2;
    float4 carDepth : SV_Target3;
};
Out ps(float4 pos : SV_Position) {
    Out o;
    o.world = 0;
    o.car = 0;
    o.more = 0;
    o.carDepth = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            float2 at = (floor(pos.xy) + (float2(i, j) + 0.5) / 4) / 64;
            int2 p = int2(at * js.zw);
            float2 c = (float2(p) + 0.5) / js.zw;
            float2 ndc = float2(c.x * 2 - 1, 1 - c.y * 2);
            float d = depthIn.Load(int3(p, 0));
            float2 a;
            float away = d - origin.z;
            float metres = abs(away) > 1e-9 ? origin.w / away : 1e9;
            bool car = extra.x > 0 ? in_car(ndc, d) : metres > 0 && metres < 8;
            bool on;
            float predicted;
            if (car && extra.x > 0)
                on = where(ndc, d, R2, a, predicted);
            else
                on = where(ndc, d, R, a, predicted);
            if (!on)
                continue;
            float now = tent(nowIn, c);
            float4 pt = float4(abs(now - tent(beforeIn, c)), abs(now - tent(beforeIn, a)), around(beforeIn, a, now), 1);
            float len = length((a - c) * js.zw);
            // Depth judges points within 1 km: farther (the sky at the far plane, depth 0) nothing moves with the
            // car's travel, and a reversed depth that near 0 is below what a 2% tolerance can tell from rounding.
            float judged = metres > 0 && metres < 1000 ? 1 : 0;
            float alongOk = judged * (agrees(a, predicted) ? 1 : 0);
            float stillOk = judged * (agrees(c, d) ? 1 : 0);
            if (car) {
                o.car += pt;
                o.more.w += len;
                o.carDepth.xyw += float3(alongOk, stillOk, judged);
            } else {
                o.world += pt;
                o.more.xyz += float3(len, alongOk, stillOk);
                o.carDepth.z += judged;
            }
        }
    return o;
}
)";

// A focus view drawn smaller that DLSS didn't upscale this frame: its render stretched over its region (bilinear).
inline constexpr char kStretchShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;
    row_major float4x4 R2;
    float4 js;               // z, w: the region's width, height
};
Texture2D<float4> src : register(t0);
SamplerState linearClamp : register(s0);
float4 ps(float4 pos : SV_Position) : SV_Target { return src.SampleLevel(linearClamp, pos.xy / js.zw, 0); }
)";

// The focus view after DLSS where the game drew only part of it (DlssPass::focusKeep, focusBandTop): what isn't kept
// takes the eye's image as DLSS made it, through the focus view's clip remap (unjittered both), bilinear from our copy
// of the eye's region under the focus view; alpha the eye's share (blended over the focus view), kept texels discarded.
inline constexpr char kBandOutsideShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;    // [0]: the focus view's left in the target, the band's top row, its height (0: none), the
                             // feather; [1]: the keep texture's width, height, 1 with it, 1 with the stamps; [2].x: the
                             // stamp (its bits)
    row_major float4x4 R2;
    float4 js;               // the eye's width, height; the focus view's width, height
    float4 origin;           // the copied region's top-left in the eye (pixels); its width, height
    float4 extra;            // the remap without jitter: focus clip x = a eye x + b, y = c eye y + d
};
Texture2D<float4> src : register(t0);
Texture2D<float> keepIn : register(t1);
Texture2D<uint> stampsIn : register(t2);
SamplerState linearClamp : register(s0);
float4 ps(float4 pos : SV_Position) : SV_Target {
    float2 fp = pos.xy - float2(R[0].x, 0);  // the focus view's pixel
    float keep = 1;
    if (R[0].z > 0 && (fp.y < R[0].y || fp.y >= R[0].y + R[0].z)) {
        keep = 0;  // outside the band DLAA ran over
    } else {
        if (R[1].z > 0) {
            int2 at = min(int2(fp / js.zw * R[1].xy), int2(R[1].xy) - 1);
            float m = keepIn.Load(int3(at, 0));
            keep = R[0].w > 0 ? saturate(m / R[0].w) : (m > 0 ? 1 : 0);
        }
        if (R[1].w > 0 && stampsIn.Load(int3(int2(fp), 0)) == asuint(R[2].x))
            keep = 0;
    }
    if (keep >= 1)
        discard;
    float2 f = float2(fp.x / js.z * 2 - 1, 1 - fp.y / js.w * 2);
    float2 e = float2((f.x - extra.y) / extra.x, (f.y - extra.w) / extra.z);
    float2 px = float2((e.x + 1) * 0.5 * js.x, (1 - e.y) * 0.5 * js.y);
    return float4(src.SampleLevel(linearClamp, (px - origin.xy) / origin.zw, 0).rgb, 1 - keep);
}
)";

// The scene's depth as it stands mid-pass (dlss_depth_copy_list), before transparents that write depth (a car's glass):
// its first sample per pixel into our single-sample copy (macro MS: the depth is multisample).
inline constexpr char kDepthCopyShader[] = R"(
#ifdef MS
Texture2DMS<float> depthIn : register(t0);
#else
Texture2D<float> depthIn : register(t0);
#endif
float ps(float4 pos : SV_Position) : SV_Target {
#ifdef MS
    return depthIn.Load(int2(pos.xy), 0);
#else
    return depthIn.Load(int3(pos.xy, 0));
#endif
}
)";

// The copy kept only where the transparents drawn after it are near (DlssRun::depthCopyNear): where the depth at the
// pass's end lies at or beyond the distance (its depth value at most extra.w, reversed depth), that depth is written
// over the copy; nearer, the copy stays (discard). The depth test only lets a transparent nearer, so the end's depth is
// never farther than the copy's.
inline constexpr char kDepthKeepFarShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;
    row_major float4x4 R2;
    float4 js;
    float4 origin;
    float4 extra;  // w: the depth value at the distance
};
#ifdef MS
Texture2DMS<float> depthIn : register(t0);
#else
Texture2D<float> depthIn : register(t0);
#endif
float ps(float4 pos : SV_Position) : SV_Target {
#ifdef MS
    float d = depthIn.Load(int2(pos.xy), 0);
#else
    float d = depthIn.Load(int3(pos.xy, 0));
#endif
    if (d > extra.w)
        discard;
    return d;
}
)";

// The measurement (DlssRun::depthMeasure): every 8th texel each way, the copy's depth (before the transparents) and
// the scene's at the pass's end (after them), read back and compared on the CPU.
inline constexpr char kDepthMeasureShader[] = R"(
Texture2D<float> before : register(t0);
#ifdef MS
Texture2DMS<float> after : register(t1);
#else
Texture2D<float> after : register(t1);
#endif
float2 ps(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy) * 8 + 4;
#ifdef MS
    float a = after.Load(p, 0);
#else
    float a = after.Load(int3(p, 0));
#endif
    return float2(before.Load(int3(p, 0)), a);
}
)";

}  // namespace simqvfr

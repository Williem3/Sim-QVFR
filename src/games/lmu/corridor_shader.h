// The focus corridor's shaders (mvr_pass_corridor.h; [experiment] lmu_corridor_ab, lmu_corridor_show): HLSL source.
// Entries: vs_main (a full-viewport triangle); copy_ms / copy_ss (the eye's depth over the focus box into a single-sample
// R32F copy); the road pass on a grid of half the copy's size each way (the road's height changes slowly; the corridor's
// own test stays at the copy's texels): classify_main (compute, a thread per grid texel: is it road, at what height, and
// its share of the road's centre), columns_main (compute, a thread per grid column: carries the last road's height up
// the column, sums the centre's shares), centre_main (compute, one thread: the road's centre, smoothed); margin_main
// (compute, a thread per copy texel: how far inside the corridor its point lies, the map everything after reads);
// mask_main (over the focus viewport: discards the texels whose point lies in the corridor, so the rest get the
// viewport's single depth, the nearest, and this pass's stamp in our own texture); show_main (the texels outside it
// tinted blue by multiplying, the same in sunlight and shade); copy_colour_ms / copy_colour_ss (the eye's colour over
// the box), fill_outside_main and fill_edge_main (the eye's colour and depth into the focus view outside the corridor,
// blended within the feather of its edge: the periphery composed into the focus view in the game's own frame).
// Constant buffer b0, 32 floats (see the source).
// Its own header so tests/lmu_corridor_test.cpp runs the same source on WARP.
//
// The corridor follows the terrain: a point is in it when it lies ahead of the start, within the half width of the
// road's centre, and between 1 m below and `tall` above the road beneath it. "The road beneath it": the nearest surface
// below it in the copy's column (towards the viewer on screen) that faces up (its normal, from the depth, within the
// steepest slope of the vertical): a tree's foot, the grass under a barrier, the climbing road itself. "The road's
// centre": the mean sideways place of the up-facing ground 10-80 m ahead (each texel by the ground it covers), within
// reach of the head's line, smoothed over passes and held within that reach.
#pragma once

namespace lmu {

inline constexpr char kCorridorShaders[] = R"(
cbuffer Corridor : register(b0) {
    float4 box;     // the eye's box region in the depth target: x0, y0 (texels), width, height
    float4 tans;    // the focus view's tangents: left, right, up, down
    float4 terms;   // depth = x + y / z (the depth target's own convention); z: the corridor's start (m), w: its height
    float4 up;      // the world's up in the eye's view space (x right, y up, z ahead), w: the eye's height above the road
    float4 extent;  // x: half width (m), y: far end (m, 0 none), zw: the copy's size
    float4 ground;  // x: cos of the steepest slope taken as road, y: the centre's smoothing (0..1 a pass), z: its reach (m)
    float4 fill;    // x: the car kept: whatever lies nearer than this (m; 0 none), y: the feather (m), zw: the focus view's size
    float4 focus;   // xy: the focus viewport's corner in the target (texels), z: this pass's mask stamp (its bits), w: the
                    // corridor's widening with distance past its start (the tangent of its angle either side; 0 straight)
    // The mask's read of the last pass's margin map where the box was then (the box follows the gaze): uv there = uv now
    // x xy + zw (0, 0: the same box).
    float4 shift;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    return v;
}
Texture2DMS<float> depthMs : register(t0);
Texture2D<float> depthSs : register(t0);
// The copy (extent.zw texels over the box): the focus view's depth where it drew it (fill.zw its size, focus.xy its corner
// in the target), at its own resolution whatever the periphery's; the eye's where this pass's mask skipped the focus
// view (its texels stamped focus.z) or without one (fill.z 0). Nearest texels both.
Texture2D<uint> copyMasked : register(t4);
int2 copy_focus_at(float2 uv) { return min(int2(uv * fill.zw), int2(fill.zw) - 1); }
bool copy_from_focus(float2 uv) { return fill.z > 0 && copyMasked.Load(int3(copy_focus_at(uv), 0)) != asuint(focus.z); }
int2 copy_eye_at(float2 uv) { return int2(box.xy) + min(int2(uv * box.zw), int2(box.zw) - 1); }
int2 copy_at(V v) {
    const float2 uv = v.pos.xy / extent.zw;
    return copy_from_focus(uv) ? copy_focus_at(uv) + int2(focus.xy) : copy_eye_at(uv);
}
float copy_ms(V v) : SV_Target { return depthMs.Load(copy_at(v), 0); }
float copy_ss(V v) : SV_Target { return depthSs.Load(int3(copy_at(v), 0)); }
// The eye's colour over the box (the fill's source): multisampled, its samples averaged (a resolve).
Texture2DMS<float4> colourMs : register(t0);
Texture2D<float4> colourSs : register(t0);
float4 copy_colour_ms(V v) : SV_Target {
    uint w, h, samples;
    colourMs.GetDimensions(w, h, samples);
    float4 sum = 0;
    for (uint i = 0; i < samples; ++i)
        sum += colourMs.Load(int2(v.pos.xy) + int2(box.xy), i);
    return sum / samples;
}
float4 copy_colour_ss(V v) : SV_Target { return colourSs.Load(int3(int2(v.pos.xy) + int2(box.xy), 0)); }

Texture2D<float> eyeDepth : register(t0);       // the copy
Texture2D<float> groundBelow : register(t1);    // the road beneath each grid texel: its height (m), -1e9 none
StructuredBuffer<float> centreNow : register(t2);  // [0]: the road's centre across (m, + right)
static const float kNone = -1e9;

// Copy texel `at`'s point in the eye's view space (x right, y up, z ahead); the sky far away.
float3 point_at(int2 at) {
    const float2 uv = (float2(at) + 0.5) / extent.zw;
    const float over = eyeDepth.Load(int3(at, 0)) - terms.x;
    const float z = abs(over) > 1e-12 ? terms.y / over : 1e9;
    return float3(lerp(tans.x, tans.y, uv.x), lerp(tans.z, tans.w, uv.y), 1) * (z > 0 ? z : 1e9);
}
// The head's level heading and its right.
float3 heading() { return normalize(float3(0, 0, 1) - up.xyz * up.z); }
float height_of(float3 p) { return dot(p, up.xyz) + up.w; }

RWTexture2D<float> groundOut : register(u0);           // the grid: the road beneath each texel
RWStructuredBuffer<float2> columnSums : register(u1);  // per grid column: the road's sideways sum and weight (its area)
RWStructuredBuffer<float> centreOut : register(u2);
RWTexture2D<float4> roadOut : register(u3);            // the grid: road height (kNone: not road), across x area, area
Texture2D<float4> roadIn : register(t3);
// The road grid's size: half the copy's each way, rounded up.
int2 grid_size() { return (int2(extent.zw) + 1) / 2; }

// Grid texel g: is the copy texel at its lower left road (a surface facing up, not an edge), its height, and its share of
// the road's centre (its sideways place times the ground it covers, and that ground) when 10-80 m ahead within reach.
[numthreads(8, 8, 1)]
void classify_main(uint3 id : SV_DispatchThreadID) {
    const int2 gs = grid_size();
    if (any(int2(id.xy) >= gs))
        return;
    const int w = (int)extent.z, h = (int)extent.w;
    // The lower texel of its 2 x 2 (the nearer road on a climb): the texel above it then reads a little above its road,
    // which the corridor's height takes, rather than below it, which past a metre it doesn't.
    const int2 at = min(int2(id.xy) * 2 + int2(0, 1), int2(w, h) - 1);
    const float3 p = point_at(at);
    const float3 fwd = heading(), side = cross(up.xyz, fwd);
    const float ahead = dot(p, fwd);
    float4 result = float4(kNone, 0, 0, 0);
    if (p.z < 1e8 && ahead > terms.z) {
        const float3 right = point_at(int2(min(at.x + 1, w - 1), at.y)) - p, left = p - point_at(int2(max(at.x - 1, 0), at.y));
        const float3 down = point_at(int2(at.x, min(at.y + 1, h - 1))) - p, upward = p - point_at(int2(at.x, max(at.y - 1, 0)));
        float3 n = cross(right + left, down + upward);
        const float len = length(n);
        // A surface, not an edge between two: each axis's step to one side within 3x the other's (a surface seen at a
        // grazing angle far away steps far but evenly; a silhouette jumps on one side). Then facing up.
        const float lr = length(right), ll = length(left), ld = length(down), lu = length(upward);
        const bool smooth = max(lr, ll) <= 3 * min(lr, ll) + 1e-4 && max(ld, lu) <= 3 * min(ld, lu) + 1e-4;
        if (len > 0 && smooth) {
            n /= len;
            if (dot(n, p) > 0)
                n = -n;  // towards the eye
            if (dot(n, up.xyz) > ground.x) {
                result.x = height_of(p);
                const float across = dot(p, side);
                if (ahead > 10 && ahead < 80 && abs(across) < ground.z + extent.x) {
                    // Weighted by the ground the texel covers (its distance squared over how squarely it faces the
                    // eye), so the road far ahead counts as much as the near road's many texels.
                    const float area = dot(p, p) / max(dot(n, -normalize(p)), 0.05);
                    result.y = across * area;
                    result.z = area;
                }
            }
        }
    }
    roadOut[id.xy] = result;
}
// A thread per grid column, bottom up (what lies below each texel on screen comes first): the last road's height
// carried up, the centre's shares summed.
[numthreads(64, 1, 1)]
void columns_main(uint3 id : SV_DispatchThreadID) {
    const int2 gs = grid_size();
    if ((int)id.x >= gs.x)
        return;
    float last = kNone, sum = 0, count = 0;
    for (int y = gs.y - 1; y >= 0; --y) {
        const float4 r = roadIn.Load(int3(id.x, y, 0));
        if (r.x > kNone * 0.5) {
            last = r.x;
            sum += r.y;
            count += r.z;
        }
        groundOut[int2(id.x, y)] = last;
    }
    columnSums[id.x] = float2(sum, count);
}
[numthreads(1, 1, 1)]
void centre_main() {
    float sum = 0, count = 0;
    for (int x = 0; x < grid_size().x; ++x) {
        sum += columnSums[x].x;
        count += columnSums[x].y;
    }
    if (count > 0)  // road seen; else the centre holds
        centreOut[0] = lerp(centreOut[0], clamp(sum / count, -ground.z, ground.z), ground.y);
}

// How far inside the corridor the focus texel at `uv` (0..1 across the focus viewport) lies, in metres: the least of its
// margins (past the start, inside the half width of the road's centre, above 1 m below its road, below the corridor's
// top, before the far end); negative outside.
float corridor_margin(float2 uv) {
    const int2 at = min(int2(uv * extent.zw), int2(extent.zw) - 1);
    const float3 p = point_at(at);
    if (p.z >= 1e8)
        return -1e9;  // the sky
    const float3 fwd = heading(), side = cross(up.xyz, fwd);
    const float ahead = dot(p, fwd), across = abs(dot(p, side) - centreNow[0]);
    const float road = groundBelow.Load(int3(at / 2, 0));  // the road grid: half the copy's size
    const float above = height_of(p) - (road > kNone * 0.5 ? road : 0);
    // Its half width: the road width near, opening by focus.w's angle further out, so a bend (the road leaving the
    // head's straight line far ahead: Eau Rouge, a hairpin, the pit exit) stays inside; far things are small on screen.
    const float half = extent.x + max(ahead - terms.z, 0) * focus.w;
    float m = min(min(ahead - terms.z, half - across), min(above + 1, terms.w - above));
    if (extent.y > 0)
        m = min(m, extent.y - ahead);
    if (fill.x > 0)  // the car (cockpit, mirrors, bonnet) kept too: whatever lies nearer than fill.x
        m = max(m, fill.x - length(p));
    return m;
}
// The margin map (margin_main, once a pass after the road pass): corridor_margin at every copy texel, held within
// +-kMarginReach (the sky's -1e9 out of R16F's range); read by the mask, the fill
// and show mode at the focus view's texels.
static const float kMarginReach = 50;
RWTexture2D<float> marginOut : register(u4);
// The rows the corridor covers (for DLSS's band, mvr_pass_corridor.h corridor_band): the copy's height less its top row,
// and its bottom row + 1, as maxima (cleared to 0: no row). Unbound (the test): no rows.
RWByteAddressBuffer rowsOut : register(u5);
groupshared uint gTop, gBottom;
[numthreads(8, 8, 1)]
void margin_main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) {
        gTop = 0;
        gBottom = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < uint2(extent.zw))) {
        const float m = clamp(corridor_margin((float2(id.xy) + 0.5) / extent.zw), -kMarginReach, kMarginReach);
        marginOut[id.xy] = m;
        if (m > 0) {
            InterlockedMax(gTop, uint(extent.w) - id.y);
            InterlockedMax(gBottom, id.y + 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gBottom > 0) {
        uint was;
        rowsOut.InterlockedMax(0, gTop, was);
        rowsOut.InterlockedMax(4, gBottom, was);
    }
}
Texture2D<float> marginIn : register(t6);
SamplerState linearClamp : register(s0);
// The nearest texel, as corridor_margin reads the copy: a filtered read blends the road with the wall or sky beside it
// near the horizon (one texel spans metres there); the fill's feather smooths the edge in world space instead.
float margin_at(float2 uv) { return marginIn.Load(int3(min(int2(uv * extent.zw), int2(extent.zw) - 1), 0)); }
bool in_corridor(float2 uv) { return margin_at(uv) > 0; }
// The mask: depth only (the viewport's single depth, the nearest), and each texel it covers stamped with this pass's
// number in our own texture (the focus viewport's size), so the fill knows them without reading the depth back.
RWTexture2D<uint> maskedOut : register(u0);
void mask_main(V v) {
    // The last pass's map, where this texel lay in the box as it was then; outside it, kept (rendered).
    const float2 at = any(shift.xy != 0) ? v.uv * shift.xy + shift.zw : v.uv;
    if (any(at < 0) || any(at > 1) || in_corridor(at))
        discard;
    maskedOut[uint2(v.pos.xy - focus.xy)] = asuint(focus.z);
}
// Show mode: the texels outside the corridor multiplied by blue (the blend: dest x src), the rest left as drawn.
float4 show_main(V v) : SV_Target {
    if (in_corridor(v.uv))
        discard;
    return float4(0.25, 0.4, 1.0, 1.0);
}
// The fill (the focus view composed with the eye inside the game's frame), two draws over the focus viewport: outside
// the corridor, and every texel the mask skipped (stamped this pass), the eye's colour (upscaled) and depth written as
// they are (no blend: nothing read back); within the feather of the corridor's edge, the eye's colour blended over the
// focus view (src x alpha + dest x (1 - alpha), alpha the eye's share). Deeper inside, the focus view as drawn.
Texture2D<uint> maskedIn : register(t4);      // the mask's stamps (the focus viewport's size)
Texture2D<float4> eyeColour : register(t5);   // the eye's colour over the box (the copy's size)
bool masked_at(float4 pos) { return maskedIn.Load(int3(int2(pos.xy - focus.xy), 0)) == asuint(focus.z); }
struct Filled { float4 colour : SV_Target; float depth : SV_Depth; };
Filled fill_outside_main(V v) {
    if (!masked_at(v.pos) && margin_at(v.uv) > 0)
        discard;
    Filled f;
    f.colour = float4(eyeColour.SampleLevel(linearClamp, v.uv, 0).rgb, 1);
    f.depth = eyeDepth.Load(int3(min(int2(v.uv * extent.zw), int2(extent.zw) - 1), 0));
    return f;
}
float4 fill_edge_main(V v) : SV_Target {
    const float margin = margin_at(v.uv);
    if (margin <= 0 || margin >= fill.y || masked_at(v.pos))
        discard;
    return float4(eyeColour.SampleLevel(linearClamp, v.uv, 0).rgb, 1 - margin / max(fill.y, 1e-3));
}
)";

}  // namespace lmu

// The quad-view compose shader, shared by the D3D12 compositor (compositor.h) and the D3D11 one
// (d3d11_compositor.h): a full-screen triangle per eye, the outer view upscaled over the eye's tangents
// with the focus view blended into the focus box, and the helmet overlay. Its constants, cbuffer b0:
// kComposeConstants floats, compose_constants filling all but the helmet's (40-59), helmet_constants those.
// Sources: t0 the outer view's texture, t1 the focus view's (the same texture when one image holds both),
// t2 the helmet's light, t3-t5 its pieces (helmet_pieces.h); s0 a linear clamp sampler.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "compositor.h"

namespace simqvfr {

inline constexpr unsigned kComposeConstants = 60;

// EyeComposition `c` for eye `eye` as the shader's cbuffer, but for the helmet's constants (40-59: 0).
inline void compose_constants(const EyeComposition& c, int eye, float (&constants)[kComposeConstants]) {
    for (int i = 0; i < 4; ++i) {
        constants[i] = c.outerTan[i];
        constants[4 + i] = c.focusTan[i];
        constants[20 + i] = c.outerSource[i];
        constants[24 + i] = c.focusSource[i];
    }
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            constants[8 + row * 4 + col] = c.rotation[row * 3 + col];
    constants[28] = c.feather;
    constants[29] = c.border;
    constants[30] = c.cornerRadius;
    constants[31] = static_cast<float>(eye);
    // The pulled-in corners, a byte each in percent (the shader's rot0.w, rot1.w).
    for (int word = 0; word < 2; ++word) {
        uint32_t packed = 0;
        for (int i = 0; i < 4; ++i)
            packed |= static_cast<uint32_t>(std::lround(std::clamp(c.cornerInset[word * 4 + i], 0.0f, 0.45f) * 100))
                      << (8 * i);
        std::memcpy(&constants[11 + 4 * word], &packed, sizeof(packed));
    }
    for (int i = 0; i < 4; ++i) {
        constants[32 + i] = c.borderColor[i];
        constants[36 + i] = c.viewTan[i];
    }
}

// The helmet's constants (40-59) for eye `c`, with the compositor's loaded pieces (`pieceMask`, 1 << HelmetPiece)
// and its visor drawn as a tint or not (HelmetPieceSet::visor_tints, visor_white_point). Without pieces the pieces'
// look falls back to the built-in one; with them, the light diagnostic keeps their shapes.
inline void helmet_constants(const EyeComposition& c, unsigned pieceMask, bool visorTint, double visorWhitePoint,
                             float (&constants)[kComposeConstants]) {
    for (int i = 0; i < 4; ++i)
        constants[40 + i] = c.helmetOpening[i];
    const int look = c.helmet == 2 && !pieceMask ? 1 : c.helmet == 3 && pieceMask ? 4 : c.helmet;
    const bool pieceLook = look == 2 || look == 4;
    constants[44] = pieceLook ? static_cast<float>(pieceMask | (visorTint ? 8u : 0u)) : c.helmetRim;
    constants[45] = pieceLook ? static_cast<float>(visorWhitePoint) : c.helmetRadius;
    if (pieceLook) {  // the pieces' looks: the depth shift in place of the opening
        constants[40] = c.helmetShift[0];
        constants[41] = c.helmetShift[1];
    }
    constants[46] = c.helmetLight;
    constants[47] = static_cast<float>(look);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            constants[48 + row * 4 + col] = c.helmetRotation[row * 3 + col];
    constants[51] = c.helmetTanHalf[0];
    constants[55] = c.helmetTanHalf[1];
    constants[59] = c.visorOffset;
}

// HelmetLayerPS's constants for the helmet's own layer `d`, with the loaded pieces (as helmet_constants).
inline void helmet_layer_constants(const HelmetLayerDraw& d, unsigned pieceMask, bool visorTint, double visorWhitePoint,
                                   float (&constants)[kComposeConstants]) {
    constants[0] = d.coverTan[0];
    constants[1] = d.coverTan[1];
    constants[2] = d.imageTan[0];
    constants[3] = d.imageTan[1];
    constants[4] = d.visorOffset;
    constants[44] = static_cast<float>(pieceMask | (visorTint ? 8u : 0u));
    constants[45] = static_cast<float>(visorWhitePoint);
    constants[46] = d.light;
    constants[47] = d.checkLight ? 4.0f : 2.0f;
}

// LightPS's constants: each eye's outer view in the image it samples (eye 0's as outerSrc, eye 1's as focusSrc).
inline void light_constants(const float (&source0)[4], const float (&source1)[4], float (&constants)[kComposeConstants]) {
    std::memcpy(&constants[20], source0, sizeof(source0));
    std::memcpy(&constants[24], source1, sizeof(source1));
}

// The helmet's light texture: one texel per eye side (4 x 2), blended over composed frames by kLightBlend
// (it settles within about half a second).
inline constexpr DXGI_FORMAT kLightFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr float kLightBlend = 0.08f;

// IEEE half to float (the light texture is R16G16B16A16_FLOAT, for read_light).
inline float half_to_float(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ffu;
    uint32_t bits;
    if (exponent == 0)
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31)
        bits = sign | 0x7f800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline constexpr char kComposeShader[] = R"(
cbuffer Eye : register(b0) {
    float4 outerTan;  // left, right, up, down
    float4 focusTan;
    // Rows, A eye frame -> B eye frame. rot0.w, rot1.w (as uints): the focus area's corners pulled in, a byte
    // each in percent of the box: top left across and down, top right across and down; bottom right across
    // and up, bottom left across and up (EyeComposition::cornerInset). All 0: the box.
    float4 rot0, rot1, rot2;
    float4 outerSrc, focusSrc;
    float4 params;  // feather, border, corner radius
    float4 borderColor;
    float4 viewTan;        // the output's tangents (all 0: outerTan)
    float4 helmetOpening;  // the visor opening's tangents / looks 2 and 4: xy the eye's shift of the images' tangents (depth)
    // x: rim (tangents) / looks 2 and 4: the loaded pieces (1 visor, 2 bottom, 4 top, 8 the visor as a tint);
    // y: corner radius (tangents) / the visor's white point (linear); z: light (0..1); w: the look (EyeComposition)
    float4 helmet;
    // Look 2: rows taking a direction in the eye's frame into the images'; w: tangent half extents across and
    // up/down, the visor's slide in image heights.
    float4 helmetRot0, helmetRot1, helmetRot2;
};
Texture2D outerTex : register(t0);
Texture2D focusTex : register(t1);
Texture2D lightTex : register(t2);   // the frame's light per eye side (LightPS)
Texture2D visorTex : register(t3);   // look 2: the pieces
Texture2D bottomTex : register(t4);
Texture2D topTex : register(t5);
SamplerState linearClamp : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
// Keeps bilinear taps inside a source region (half a texel in from its edges): the views abut each
// other in ACE's target, and a shorter pair has unused rows below it.
float2 inside(float2 uv, float4 src, float2 size) {
    const float2 h = 0.5 / size;
    return clamp(uv, src.xy + h, src.xy + src.zw - h);
}
// How far inside the quadrilateral of the pulled-in corners a point is, measured as the square box is
// (in shares of each axis, so the corners all left in place give the box). Its edges never cross (each
// corner stays in its quarter of the box); concave is fine. Negative outside.
float inside_quad(float2 f, uint2 packed) {
    float2 c[4];
    [unroll] for (int i = 0; i < 4; ++i) {
        const uint word = i < 2 ? packed.x : packed.y, shift = (i & 1) * 16;
        const float2 pulled = float2((word >> shift) & 0xff, (word >> (shift + 8)) & 0xff) / 100;
        c[i] = float2(i == 1 || i == 2 ? 1 - pulled.x : pulled.x, i >= 2 ? 1 - pulled.y : pulled.y);
    }
    // The distance to the nearest edge, its sign from the crossings of a ray to the right (even: outside).
    float d = 1e9;
    bool outside = true;
    [unroll] for (int j = 0; j < 4; ++j) {
        const float2 a = c[j], e = c[(j + 1) & 3] - a, w = f - a;
        const float2 b = w - e * saturate(dot(w, e) / dot(e, e));
        d = min(d, dot(b, b));
        if ((a.y > f.y) != (a.y + e.y > f.y) && f.x < a.x + e.x * (f.y - a.y) / e.y)
            outside = !outside;
    }
    return outside ? -sqrt(d) : sqrt(d);
}
// How far inside the focus box a point is (f: 0..1 across it), as a share of the box. Square corners:
// the nearer edge's share of its own axis. Rounded corners (params.z: the radius as a share of half
// the shorter side): the distance to the rounded edge over the shorter side, measured in the box's
// tangent proportions so the corners are circular. Pulled-in corners: inside_quad. Negative outside.
float inside_box(float2 f) {
    const uint2 corners = uint2(asuint(rot0.w), asuint(rot1.w));
    if (any(corners))
        return inside_quad(f, corners);
    if (params.z <= 0) {
        const float2 e = min(f, 1 - f);
        return min(e.x, e.y);
    }
    const float2 size = abs(float2(focusTan.y - focusTan.x, focusTan.z - focusTan.w));
    const float2 hs = 0.5 * size;
    const float r = params.z * min(hs.x, hs.y);
    const float2 q = abs((f - 0.5) * size) - hs + r;
    const float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
    return -d / min(size.x, size.y);
}
// The helmet over a point `a` (tangents) at `p` (0..1 across the eye's output): the visor opening is a
// rounded rectangle; the lip reaches helmet.x into it, soft (out of focus), lit by the frame's light
// at the nearest sides and brightest where light comes in; a dark seal runs along the opening's edge,
// and deeper in the shell takes less light. The lining is a mid grey, so the frame's light (a tunnel,
// a bright sky) shows plainly. Look 3 (the light diagnostic) shows the frame's light itself.
float3 helmet_apply(float3 scene, float2 a, float2 p) {
    const float2 c = 0.5 * float2(helmetOpening.x + helmetOpening.y, helmetOpening.z + helmetOpening.w);
    const float2 hs = 0.5 * float2(helmetOpening.y - helmetOpening.x, helmetOpening.z - helmetOpening.w);
    const float r = min(helmet.y, min(hs.x, hs.y));
    const float2 q = abs(a - c) - hs + r;
    const float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;  // > 0 outside the opening
    const float rim = max(helmet.x, 1e-4);
    float4 w = float4((1 - p.x) * (1 - p.x), p.x * p.x, (1 - p.y) * (1 - p.y), p.y * p.y);
    w /= dot(w, float4(1, 1, 1, 1));
    const int row = (int)params.w;  // the eye
    const float3 frame = w.x * lightTex.Load(int3(0, row, 0)).rgb + w.y * lightTex.Load(int3(1, row, 0)).rgb +
                         w.z * lightTex.Load(int3(2, row, 0)).rgb + w.w * lightTex.Load(int3(3, row, 0)).rgb;
    const float alpha = smoothstep(-rim, -0.4 * rim, d);
    if (helmet.w > 2.5 && helmet.w < 3.5)
        return lerp(scene, frame, alpha);
    if (helmet.w > 1.5) {
        // The pieces, fixed to the head: this eye's direction in the images' frame, then over each other.
        const float3 a3 = float3(a, -1);
        const float3 d = float3(dot(helmetRot0.xyz, a3), dot(helmetRot1.xyz, a3), dot(helmetRot2.xyz, a3));
        const float2 t = d.xy / max(-d.z, 1e-3) + helmetOpening.xy;  // + the parallax of the images' depth
        const float2 uv = float2(0.5 + t.x / (2 * helmetRot0.w), 0.5 - t.y / (2 * helmetRot1.w));
        const float3 lit = lerp(1.0, 2.5 * frame, helmet.z);
        const uint pieces = (uint)helmet.x;
        const bool check = helmet.w > 3.5;  // the light diagnostic: the pieces' shapes in the frame's light
        float3 color = scene;  // the pieces over it, back to front (premultiplied, as they are loaded)
        const float2 vu = uv + float2(0, helmetRot2.w);
        if ((pieces & 1) && vu.y >= 0 && vu.y <= 1) {  // no visor above or below its image (open clears the view)
            const float4 v = visorTex.SampleLevel(linearClamp, vu, 0);
            if (pieces & 8) {
                // Glass: it passes the scene filtered by its colour (the image's brightest part is clear) and
                // reflects a little of the frame's light.
                const float3 through = saturate(v.rgb / max(v.a, 1e-4) / max(helmet.y, 1e-3));
                if (!check)
                    color = color * lerp(1.0, through, v.a) + 0.05 * helmet.z * frame * v.a;
            } else {
                color = (check ? frame * v.a : v.rgb * lit) + color * (1 - v.a);
            }
        }
        if (pieces & 2) {
            const float4 v = bottomTex.SampleLevel(linearClamp, uv, 0);
            color = (check ? frame * v.a : v.rgb * lit) + color * (1 - v.a);
        }
        if (pieces & 4) {
            const float4 v = topTex.SampleLevel(linearClamp, uv, 0);
            color = (check ? frame * v.a : v.rgb * lit) + color * (1 - v.a);
        }
        return color;
    }
    const float3 light = lerp(0.3, frame, helmet.z);
    const float lip = exp(-max(d + rim, 0) / (1.2 * rim));
    const float seal = 1 - 0.55 * exp(-(d * d) / (0.02 * rim * rim));
    return lerp(scene, 0.30 * (0.35 + 1.4 * lip) * seal * light, alpha);
}
float4 PS(V v) : SV_Target {
    float2 outerSize;
    outerTex.GetDimensions(outerSize.x, outerSize.y);
    const float4 vt = viewTan.y > viewTan.x ? viewTan : outerTan;
    const float3 a = float3(lerp(vt.x, vt.y, v.uv.x), lerp(vt.z, vt.w, v.uv.y), -1);
    // Where the point is in the rendered outer view (v.uv itself when the output covers just it).
    const float2 o = viewTan.y > viewTan.x
                         ? float2((a.x - outerTan.x) / (outerTan.y - outerTan.x), (outerTan.z - a.y) / (outerTan.z - outerTan.w))
                         : v.uv;
    const bool rendered = all(o >= 0) && all(o <= 1);
    float4 color = rendered ? outerTex.SampleLevel(linearClamp, inside(outerSrc.xy + o * outerSrc.zw, outerSrc, outerSize), 0)
                            : float4(0, 0, 0, 1);
    const float3 b = float3(dot(rot0.xyz, a), dot(rot1.xyz, a), dot(rot2.xyz, a));
    const float2 t = b.xy / max(-b.z, 1e-6);
    const float2 f = float2((t.x - focusTan.x) / (focusTan.y - focusTan.x), (focusTan.z - t.y) / (focusTan.z - focusTan.w));
    // Derivatives before any branch (they are undefined inside divergent flow).
    const float2 uv = focusSrc.xy + f * focusSrc.zw;
    const float2 dx = ddx(uv), dy = ddy(uv);
    const float e = b.z < 0 && all(f >= 0) && all(f <= 1) ? inside_box(f) : -1;
    if (e >= 0) {
        float w = saturate(e / max(params.x, 1e-4));
        w = w * w * (3 - 2 * w);
        // A supersampled focus view has several texels per output pixel: average four bilinear taps
        // spread over the pixel's footprint instead of sampling it once (which would alias).
        float2 size;
        focusTex.GetDimensions(size.x, size.y);
        float4 focus;
        if (max(length(dx * size), length(dy * size)) > 1.25) {
            focus = 0.25 * (focusTex.SampleLevel(linearClamp, inside(uv + 0.25 * (dx + dy), focusSrc, size), 0) +
                            focusTex.SampleLevel(linearClamp, inside(uv + 0.25 * (dx - dy), focusSrc, size), 0) +
                            focusTex.SampleLevel(linearClamp, inside(uv - 0.25 * (dx - dy), focusSrc, size), 0) +
                            focusTex.SampleLevel(linearClamp, inside(uv - 0.25 * (dx + dy), focusSrc, size), 0));
        } else {
            focus = focusTex.SampleLevel(linearClamp, inside(uv, focusSrc, size), 0);
        }
        color = lerp(color, focus, w);
        if (e < params.y)
            color = borderColor;
    }
    if (helmet.w > 0)
        color.rgb = helmet_apply(color.rgb, a.xy, v.uv);
    return color;
}
// The helmet as its own layer: the quad spans tangents +-outerTan.xy of the images' plane, the images
// +-outerTan.zw; focusTan.x is the visor's slide; helmet as in helmet_apply's looks 2 and 4. Premultiplied
// colour, alpha how much of the scene the pieces hide (a glass visor: its transmission's brightness).
float4 HelmetLayerPS(V v) : SV_Target {
    const float2 t = float2((v.uv.x - 0.5) * 2 * outerTan.x, (0.5 - v.uv.y) * 2 * outerTan.y);
    const float2 uv = float2(0.5 + t.x / (2 * outerTan.z), 0.5 - t.y / (2 * outerTan.w));
    // The frame's light at this side of the view, both eyes'.
    const float2 p = v.uv;
    float4 w = float4((1 - p.x) * (1 - p.x), p.x * p.x, (1 - p.y) * (1 - p.y), p.y * p.y);
    w /= dot(w, float4(1, 1, 1, 1));
    float3 frame = 0;
    for (int row = 0; row < 2; ++row)
        frame += 0.5 * (w.x * lightTex.Load(int3(0, row, 0)).rgb + w.y * lightTex.Load(int3(1, row, 0)).rgb +
                        w.z * lightTex.Load(int3(2, row, 0)).rgb + w.w * lightTex.Load(int3(3, row, 0)).rgb);
    const float3 lit = lerp(1.0, 2.5 * frame, helmet.z);
    const uint pieces = (uint)helmet.x;
    const bool check = helmet.w > 3.5;
    float3 color = 0;     // premultiplied, back to front
    float through = 1;    // how much of the scene shows through
    const float2 vu = uv + float2(0, focusTan.x);
    if ((pieces & 1) && vu.y >= 0 && vu.y <= 1) {
        const float4 v4 = visorTex.SampleLevel(linearClamp, vu, 0);
        if (pieces & 8) {  // glass: the scene through it at the brightness of its transmission, and a faint reflection
            const float3 glass = saturate(v4.rgb / max(v4.a, 1e-4) / max(helmet.y, 1e-3));
            through = lerp(1.0, dot(glass, float3(0.2126, 0.7152, 0.0722)), v4.a);
            color = check ? 0 : 0.05 * helmet.z * frame * v4.a;
        } else {
            color = check ? frame * v4.a : v4.rgb * lit;
            through = 1 - v4.a;
        }
    }
    if (pieces & 2) {
        const float4 v4 = bottomTex.SampleLevel(linearClamp, uv, 0);
        color = (check ? frame * v4.a : v4.rgb * lit) + color * (1 - v4.a);
        through *= 1 - v4.a;
    }
    if (pieces & 4) {
        const float4 v4 = topTex.SampleLevel(linearClamp, uv, 0);
        color = (check ? frame * v4.a : v4.rgb * lit) + color * (1 - v4.a);
        through *= 1 - v4.a;
    }
    return float4(color, 1 - through);
}
// The helmet's light: one texel per eye side (x: left, right, top, bottom; y: the eye), from a band 30%
// deep along that side of the rendered outer view (eye 0's region in outerSrc, eye 1's in focusSrc),
// weighted by brightness so the sky or the sun through a window counts more than the dark cockpit
// around it, blended into the last frames' by the pipeline's blend factor.
float4 LightPS(V v) : SV_Target {
    const int side = (int)v.position.x, eye = (int)v.position.y;
    const float4 src = eye == 0 ? outerSrc : focusSrc;
    float2 size;
    outerTex.GetDimensions(size.x, size.y);
    float3 sum = 0;
    float weights = 0;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 6; ++j) {
            const float along = (i + 0.5) / 8, across = (j + 0.5) / 6 * 0.30;
            const float2 f = side == 0 ? float2(across, along) : side == 1 ? float2(1 - across, along)
                           : side == 2 ? float2(along, across) : float2(along, 1 - across);
            const float3 c = outerTex.SampleLevel(linearClamp, inside(src.xy + f * src.zw, src, size), 0).rgb;
            const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
            const float w = lum * lum + 1e-4;
            sum += c * w;
            weights += w;
        }
    return float4(sum / weights, 1);
}
)";

}  // namespace simqvfr

// LMU's night car lighting (car_light.h).
#include "car_light.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../log.h"
#include "../../probe/d3d11_capture.h"

namespace lmu {

using Microsoft::WRL::ComPtr;

// ---- The light table ----

namespace {

bool usable(const CarLight& l) {
    const float c = (std::max)({l.colour[0], l.colour[1], l.colour[2]});
    return std::isfinite(l.pos[0]) && std::isfinite(l.pos[1]) && std::isfinite(l.pos[2]) && l.radius >= 1.0f &&
           l.radius <= 5000.0f && std::isfinite(c) && c > 1e-3f && c < 1e4f;
}

}  // namespace

int CarLightTable::light_index(const CarLight& l) {
    // The frame's lights are few (LMU hands a car 2-20 of the track's): a short scan, nearest-match by position.
    for (size_t i = 0; i < lights_.size(); ++i) {
        const CarLight& o = lights_[i];
        if (std::fabs(o.pos[0] - l.pos[0]) < 1e-2f && std::fabs(o.pos[1] - l.pos[1]) < 1e-2f &&
            std::fabs(o.pos[2] - l.pos[2]) < 1e-2f)
            return static_cast<int>(i);
    }
    if (lights_.size() >= kMaxLights)
        return -1;
    lights_.push_back(l);
    return static_cast<int>(lights_.size() - 1);
}

void CarLightTable::draw(unsigned slot, const float (&origin)[3], const uint8_t* buffer, size_t bufferBytes,
                         const OmniLayout& layout, uint64_t bufferGeneration, bool paint, bool normal, bool ao) {
    if (slot >= slotLights_.size() || !buffer)
        return;
    const size_t need = (std::max)(layout.posOffset, layout.colourOffset) + size_t{16} * layout.count;
    if (bufferBytes < need)
        return;
    SlotLights& s = slotLights_[slot];
    if (slotFrame_[slot] != frame_) {  // its first draw this frame: a list of its own again
        slotFrame_[slot] = frame_;
        s.count = 0;
        s.generation = ~0ull;
        slotPaint_[slot] = (normal ? 2 : 0) | (normal && ao ? 4 : 0);  // the game's: while every draw of the part has it
    }
    slotPaint_[slot] |= paint ? 1 : 0;
    if (!normal)
        slotPaint_[slot] &= static_cast<uint8_t>(~6u);
    if (!ao)
        slotPaint_[slot] &= static_cast<uint8_t>(~4u);
    firstSlot_ = (std::min)(firstSlot_, slot);
    lastSlot_ = (std::max)(lastSlot_, slot);
    if (s.generation == bufferGeneration)  // drawn again with the buffer as it was: nothing new
        return;
    s.generation = bufferGeneration;
    const unsigned count = (std::min)(layout.count, 32u);
    // The buffer's lights in the frame's table: as the draw before's when its bytes are the same.
    const bool same = lastFrame_ == frame_ && lastBuffer_.size() == need &&
                      std::memcmp(lastBuffer_.data(), buffer, need) == 0;
    if (!same) {
        lastBuffer_.assign(buffer, buffer + need);
        lastFrame_ = frame_;
        for (unsigned i = 0; i < count; ++i) {
            CarLight l;
            std::memcpy(l.pos, buffer + layout.posOffset + 16 * i, 12);
            std::memcpy(&l.radius, buffer + layout.posOffset + 16 * i + 12, 4);
            std::memcpy(l.colour, buffer + layout.colourOffset + 16 * i, 12);
            lastIndex_[i] = usable(l) ? light_index(l) : -1;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        const int index = lastIndex_[i];
        if (index < 0)
            continue;
        const CarLight& l = lights_[static_cast<size_t>(index)];
        const float dx = l.pos[0] - origin[0], dy = l.pos[1] - origin[1], dz = l.pos[2] - origin[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > l.radius + 6.0f)  // beyond its reach from the part (a car's half length of margin)
            continue;
        bool known = false;
        for (uint8_t k = 0; k < s.count; ++k)
            known = known || s.index[k] == index;
        if (known)
            continue;
        const float key = distance / l.radius;  // the nearest by its reach first
        if (s.count < kPerSlot) {
            s.index[s.count] = static_cast<uint8_t>(index);
            s.distance[s.count] = key;
            ++s.count;
        } else {
            uint8_t worst = 0;
            for (uint8_t k = 1; k < kPerSlot; ++k)
                if (s.distance[k] > s.distance[worst])
                    worst = k;
            if (key < s.distance[worst]) {
                s.index[worst] = static_cast<uint8_t>(index);
                s.distance[worst] = key;
            }
        }
    }
}

void CarLightTable::packed(unsigned slot, uint32_t (&out)[4]) const {
    out[0] = out[1] = 0xffffffffu;
    out[2] = out[3] = 0;
    if (slot >= slotLights_.size() || slotFrame_[slot] != frame_)
        return;
    const SlotLights& s = slotLights_[slot];
    for (uint8_t k = 0; k < s.count; ++k) {
        uint32_t& word = out[k / 4];
        const unsigned shift = 8 * (k % 4);
        word = (word & ~(0xffu << shift)) | (static_cast<uint32_t>(s.index[k]) << shift);
    }
    out[2] = slotPaint_[slot];
}

// ---- The comparison image ----

bool car_light_side_by_side(const std::wstring& path, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after,
                            unsigned w, unsigned h, const wchar_t* beforeLabel, const wchar_t* afterLabel) {
    if (!w || !h || before.size() < size_t{w} * h * 4 || after.size() < size_t{w} * h * 4)
        return false;
    const unsigned gap = (std::max)(8u, w / 120), bar = (std::max)(40u, h / 16);
    const unsigned W = 2 * w + gap, H = h + bar;
    // A top-down DIB the labels are drawn into with GDI (ClearType off: the PNG goes anywhere).
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(W);
    bi.bmiHeader.biHeight = -static_cast<LONG>(H);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!dib || !bits) {
        if (dib)
            DeleteObject(dib);
        if (dc)
            DeleteDC(dc);
        return false;
    }
    auto* px = static_cast<uint32_t*>(bits);
    std::fill(px, px + size_t{W} * H, 0xff141416u);
    for (unsigned y = 0; y < h; ++y) {
        std::memcpy(px + size_t{y + bar} * W, before.data() + size_t{y} * w * 4, size_t{w} * 4);
        std::memcpy(px + size_t{y + bar} * W + w + gap, after.data() + size_t{y} * w * 4, size_t{w} * 4);
    }
    HGDIOBJ old = SelectObject(dc, dib);
    HFONT font = CreateFontW(-static_cast<int>(bar * 6 / 10), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    const int ty = static_cast<int>(bar / 5);
    TextOutW(dc, static_cast<int>(bar / 3), ty, beforeLabel, static_cast<int>(wcslen(beforeLabel)));
    TextOutW(dc, static_cast<int>(w + gap + bar / 3), ty, afterLabel, static_cast<int>(wcslen(afterLabel)));
    GdiFlush();
    if (oldFont)
        SelectObject(dc, oldFont);
    if (font)
        DeleteObject(font);
    SelectObject(dc, old);
    std::vector<uint8_t> image(size_t{W} * H * 4);
    std::memcpy(image.data(), px, image.size());
    for (size_t i = 3; i < image.size(); i += 4)
        image[i] = 0xff;  // GDI's text leaves alpha 0
    DeleteObject(dib);
    DeleteDC(dc);
    return simqvfr::write_png_bgra(path, image, W, H);
}

// ---- The math the shader does (tests) ----

void car_light_unproject(const float (&inv)[16], float x, float y, float depth, float (&out)[3]) {
    const float v[4] = {x, y, depth, 1};
    float r[4] = {};
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k)
            r[c] += v[k] * inv[k * 4 + c];
    for (int c = 0; c < 3; ++c)
        out[c] = r[c] / r[3];
}

float car_light_highlight(const float (&n)[3], const float (&toEye)[3], const float (&toLight)[3], float roughness) {
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float h[3] = {toEye[0] + toLight[0], toEye[1] + toLight[1], toEye[2] + toLight[2]};
    const float hl = std::sqrt(dot(h, h));
    if (hl <= 0)
        return 0;
    for (float& c : h)
        c /= hl;
    const float nl = std::clamp(dot(n, toLight), 0.0f, 1.0f), nv = std::clamp(dot(n, toEye), 0.0f, 1.0f) + 1e-4f;
    const float nh = std::clamp(dot(n, h), 0.0f, 1.0f), vh = std::clamp(dot(toEye, h), 0.0f, 1.0f);
    const float a = roughness * roughness, a2 = a * a;
    const float dd = nh * nh * (a2 - 1) + 1;
    const float d = a2 / (3.14159265f * dd * dd);
    const float k = a * 0.5f;
    const float g = (nl / (nl * (1 - k) + k)) * (nv / (nv * (1 - k) + k));
    const float f = 0.04f + 0.96f * std::pow(1 - vh, 5.0f);
    return (std::min)(d * g * f / (4 * nv), 4.0f);  // kHighlightPeak
}

// ---- The pass ----

namespace {

constexpr char kShader[] = R"(
cbuffer CarLightPass : register(b0) {
    row_major float4x4 invProj;  // clip -> view (the eye's projection as its draws used it)
    row_major float4x4 proj;     // view -> clip
    row_major float4x4 viewM;    // world -> view (the lights)
    float4 region;               // eye w, h; focus w, h (0: none), the focus view at x = eye w
    float4 remap;                // the focus view's ndc = (a x + b, c y + d) of the eye's
    float4 shade;                // fill, occlusion, highlights, roughness
    float4 extra;                // occlusion radius (m), near fade start, end (m), smoothing width (m; 0 none)
};
cbuffer CarLightLights : register(b1) {
    float4 lightPos[128];  // world xyz, radius
    float4 lightCol[128];
};
#if MS
Texture2DMS<float> depthTex : register(t0);
Texture2DMS<uint> slotTex : register(t1);
float load_depth(int2 p) { return depthTex.Load(p, 0); }
uint load_slot(int2 p) { return slotTex.Load(p, 0); }
#else
Texture2D<float> depthTex : register(t0);
Texture2D<uint> slotTex : register(t1);
float load_depth(int2 p) { return depthTex.Load(int3(p, 0)); }
uint load_slot(int2 p) { return slotTex.Load(int3(p, 0)); }
#endif
Buffer<uint4> slotInfo : register(t2);
#if MS
Texture2DMS<float4> normalTex : register(t3);
float4 load_normal(int2 p) { return normalTex.Load(p, 0); }
#else
Texture2D<float4> normalTex : register(t3);
float4 load_normal(int2 p) { return normalTex.Load(int3(p, 0)); }
#endif

void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position) {
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

int region_of(int2 p, out float2 origin, out float2 size) {
    if (p.x < (int)region.x) {
        origin = float2(0, 0);
        size = region.xy;
        return p.y < (int)region.y ? 0 : -1;
    }
    origin = float2(region.x, 0);
    size = region.zw;
    return (region.z > 0 && p.x < (int)(region.x + region.z) && p.y < (int)region.w) ? 1 : -1;
}

float3 view_pos(float2 pixel, float depth, int r, float2 origin, float2 size) {
    float2 ndc = (pixel - origin) / size * float2(2, -2) + float2(-1, 1);
    if (r == 1)
        ndc = float2((ndc.x - remap.y) / remap.x, (ndc.y - remap.w) / remap.z);
    float4 v = mul(float4(ndc, depth, 1), invProj);
    return v.xyz / v.w;
}

float2 pixel_of(float3 v, int r, float2 origin, float2 size) {
    float4 c = mul(float4(v, 1), proj);
    float2 ndc = c.xy / c.w;
    if (r == 1)
        ndc = float2(remap.x * ndc.x + remap.y, remap.z * ndc.y + remap.w);
    return origin + (ndc * float2(0.5, -0.5) + 0.5) * size;
}

float3 neighbour(int2 q, int r, float2 origin, float2 size, float3 none) {
    q = clamp(q, int2(origin), int2(origin + size) - 1);
    float d = load_depth(q);
    if (d <= 0)
        return none;
    return view_pos(float2(q) + 0.5, d, r, origin, size);
}

// The surface's direction smoothed over `width` metres of the same car part (slot `s`): the depth is flat across each
// triangle, so pixel-to-pixel differences give each triangle one direction and the shading steps at its edges (run 1,
// 2026-11-18: facets in the highlights). Two rings of eight points around the pixel, at the width and half of it, each
// point kept when it is the same part and on the same surface (its distance from the camera within a slope's reach);
// the fan of triangles between consecutive kept points summed (each by its area) into one direction. Too few kept: the
// pixel's own.
static const float2 kRing[8] = {float2(1, 0), float2(0.7071, 0.7071), float2(0, 1), float2(-0.7071, 0.7071),
                                float2(-1, 0), float2(-0.7071, -0.7071), float2(0, -1), float2(0.7071, -0.7071)};
float3 smooth_normal(int2 p, float3 P, float3 own, float3 V, uint s, int r, float2 origin, float2 size, float pixelMetres,
                     float width) {
    if (width <= 0 || pixelMetres <= 0)
        return own;
    float px = clamp(width / pixelMetres, 1.5, 160);
    float lp = length(P);
    float3 sum = 0;
    int kept = 0;
    [unroll] for (int ring = 0; ring < 2; ++ring) {
        float reach = px * (ring == 0 ? 1.0 : 0.5);
        float3 pts[8];
        bool ok[8];
        [unroll] for (int k = 0; k < 8; ++k) {
            int2 q = clamp(p + int2(round(kRing[k] * reach)), int2(origin), int2(origin + size) - 1);
            float dq = load_depth(q);
            ok[k] = false;
            pts[k] = 0;
            if (dq > 0 && load_slot(q) == s) {
                float3 Q = view_pos(float2(q) + 0.5, dq, r, origin, size);
                float span = reach * pixelMetres;
                ok[k] = abs(length(Q) - lp) < 2.5 * span;  // a surface up to ~68 degrees to the view, not a step
                pts[k] = Q - P;
            }
        }
        [unroll] for (int k2 = 0; k2 < 8; ++k2) {
            int n = (k2 + 1) & 7;
            if (ok[k2] && ok[n]) {
                sum += cross(pts[n], pts[k2]);
                ++kept;
            }
        }
    }
    if (kept < 3 || dot(sum, sum) < 1e-16)
        return own;
    float3 N = normalize(sum);
    return dot(N, V) < 0 ? -N : N;
}

// One light's highlight at most, and all of them together (the scene's units: lit paint ~0.1-0.3, a floodlit wall ~0.2).
static const float kHighlightPeak = 4, kHighlightMost = 1.5;

static const float3 kKernel[8] = {
    float3(0.355, 0.120, 0.180), float3(-0.210, 0.420, 0.300), float3(-0.380, -0.150, 0.420),
    float3(0.140, -0.520, 0.260), float3(0.610, 0.330, 0.520), float3(-0.580, 0.470, 0.180),
    float3(0.050, -0.240, 0.880), float3(0.420, -0.610, 0.640)};

struct Out {
    float4 add : SV_Target0;
    float4 mul : SV_Target1;
};

Out ps_main(float4 pos : SV_Position) {
    int2 p = int2(pos.xy);
    float2 origin, size;
    int r = region_of(p, origin, size);
    if (r < 0)
        discard;
    uint s = load_slot(p);
    if (s == 0)
        discard;
    float d = load_depth(p);
    if (d <= 0)
        discard;
    float3 P = view_pos(pos.xy, d, r, origin, size);
    float3 Pr = neighbour(p + int2(1, 0), r, origin, size, P), Pl = neighbour(p - int2(1, 0), r, origin, size, P);
    float3 Pd = neighbour(p + int2(0, 1), r, origin, size, P), Pu = neighbour(p - int2(0, 1), r, origin, size, P);
    float lp = length(P);
    float3 dx = abs(length(Pr) - lp) < abs(lp - length(Pl)) ? Pr - P : P - Pl;
    float3 dy = abs(length(Pd) - lp) < abs(lp - length(Pu)) ? Pd - P : P - Pu;
    if (dot(dx, dx) < 1e-12) dx = Pr - Pl;
    if (dot(dy, dy) < 1e-12) dy = Pd - Pu;
    float3 V = -P / max(lp, 1e-4);
    float3 c = cross(dy, dx);
    float3 N = dot(c, c) > 1e-14 ? normalize(c) : V;
    if (dot(N, V) < 0)
        N = -N;
    uint4 info = slotInfo[s - 1];
    float baked = 1;  // the game's own baked occlusion where its shader has one (the normals' w)
    if ((info.z & 2) != 0) {
        // The game's own normal (the vertex shader's, interpolated: smooth, its creases kept), world to view.
        float4 game = load_normal(p);
        float3 world = game.xyz * 2 - 1;
        if ((info.z & 4) != 0)
            baked = saturate(game.w);
        float3 Nv = mul(float4(world, 0), viewM).xyz;
        if (dot(Nv, Nv) > 1e-6) {
            N = normalize(Nv);
            if (dot(N, V) < 0)
                N = -N;
        }
    } else {
        // The pixel's size on the surface's distance (one pixel right, at its depth), then the smoothed direction.
        float pixelMetres = length(view_pos(pos.xy + float2(1, 0), d, r, origin, size) - P);
        N = smooth_normal(p, P, N, V, s, r, origin, size, pixelMetres, extra.w);
    }

    // Ambient occlusion: points of a hemisphere on the surface, occluded where the scene's depth there is nearer.
    float noise = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));
    float angle = noise * 6.2831853;
    float3 t = normalize(abs(N.y) < 0.99 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
    float3 b = cross(N, t);
    float ca = cos(angle), sa = sin(angle);
    float3 t2 = t * ca + b * sa, b2 = b * ca - t * sa;
    float occluded = 0;
    [unroll] for (int k = 0; k < 8; ++k) {
        float3 kk = kKernel[k];
        float3 S = P + (t2 * kk.x + b2 * kk.y + N * kk.z) * extra.x;
        float2 q = pixel_of(S, r, origin, size);
        if (any(q < origin) || any(q >= origin + size))
            continue;
        float ds = load_depth(int2(q));
        if (ds <= 0)
            continue;
        float3 Q = view_pos(floor(q) + 0.5, ds, r, origin, size);
        float lq = length(Q);
        float range = saturate(extra.x / max(abs(lp - lq), 1e-3));
        occluded += (length(S) - lq > 0.02) ? range : 0;
    }
    // The screen's occlusion (between parts and objects: under the car, the wheels in their arches) times the model's
    // own baked one (panel gaps, vents, the inside of the arches: what the screen can't see).
    float ao = (1 - occluded / 8) * baked;

    // The floodlights the part's draws were handed: a GGX highlight each, as lacquer reflects a lamp.
    float rough = shade.w, a = rough * rough, a2 = a * a, kq = a * 0.5;
    float NdotV = saturate(dot(N, V)) + 1e-4;
    float3 spec = 0;
    [unroll] for (int i = 0; i < 8; ++i) {
        uint idx = ((i < 4 ? info.x : info.y) >> (8 * (i & 3))) & 0xff;
        if (idx == 0xff)
            break;
        float3 lv = mul(float4(lightPos[idx].xyz, 1), viewM).xyz;
        float3 toL = lv - P;
        float dist = length(toL), radius = lightPos[idx].w;
        if (dist >= radius || dist <= 1e-3)
            continue;
        float att = 1 - dist / radius;
        float3 L = toL / dist;
        float NdotL = saturate(dot(N, L));
        float3 H = normalize(L + V);
        float NdotH = saturate(dot(N, H)), VdotH = saturate(dot(V, H));
        float dd = NdotH * NdotH * (a2 - 1) + 1;
        float D = a2 / (3.14159265 * dd * dd);
        float G = (NdotL / (NdotL * (1 - kq) + kq)) * (NdotV / (NdotV * (1 - kq) + kq));
        float F = 0.04 + 0.96 * pow(1 - VdotH, 5);
        spec += lightCol[idx].rgb * att * min(D * G * F / (4 * NdotV), kHighlightPeak);
    }
    if ((info.z & 1) == 0)
        spec *= 0.15;  // not a body's lacquer (tyres, carbon, the cockpit): a faint sheen at most
    // A glint, not a blowout: a nearby floodlight on glossy lacquer (gloss 100) reached tens of times the paint's own
    // brightness and the game's bloom spread it into a haze over the car beside the player (run 2, 2026-11-18).
    float peak = max(spec.r, max(spec.g, spec.b));
    spec *= peak > kHighlightMost ? kHighlightMost / peak : 1;

    float fade = saturate((lp - extra.y) / max(extra.z - extra.y, 1e-3));  // the player's own cockpit left alone
    Out o;
    o.add = float4(spec * shade.z * fade, 0);
    float m = shade.x * lerp(1, ao, shade.y);
    o.mul = float4(lerp(1, m, fade).xxx, 1);
    return o;
}
)";

constexpr unsigned kSlotsMax = 4096;
constexpr unsigned kTimerSlots = 8;

bool invert4(const double (&m)[16], double (&out)[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] -
             m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] +
             m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] +
              m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] +
             m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] +
             m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] +
             m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] +
              m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] +
             m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] +
              m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-30)
        return false;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return true;
}

void mul4(const double (&a)[16], const double (&b)[16], double (&out)[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

DXGI_FORMAT srv_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return f;
    }
}

DXGI_FORMAT rtv_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return f;
    }
}

}  // namespace

struct CarLightPass::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps[2];  // single-sample inputs, multisample
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depthOff;
    ComPtr<ID3D11Buffer> passCb, lightCb, slotBuffer;
    ComPtr<ID3D11ShaderResourceView> slotView;
    std::vector<uint32_t> slotWords;
    // The scene's render target view and the depth's resource view, kept for the resources they're of.
    ID3D11Resource* rtvFor = nullptr;
    ComPtr<ID3D11Resource> rtvHeld;
    ComPtr<ID3D11RenderTargetView> rtv;
    ID3D11Resource* depthFor = nullptr;
    ComPtr<ID3D11Resource> depthHeld;
    ComPtr<ID3D11ShaderResourceView> depthView;
    bool depthViewMs = false;
    bool made = false, failed = false;
    // GPU timing: a ring of query sets, each one run's; read back frames later without waiting.
    struct Timer {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    Timer timers[kTimerSlots];
    unsigned nextTimer = 0;
    double gpuSum = 0, cpuSum = 0, collectSum = 0;
    unsigned gpuRuns = 0, runs = 0, frames = 0;
    double qpcPerMs = 0;

    bool make(ID3D11Device* d) {
        device = d;
        const auto compile = [&](const char* entry, const char* profile, const D3D_SHADER_MACRO* macros,
                                 ComPtr<ID3DBlob>& code) {
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "car-light", macros, nullptr, entry, profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
                simqvfr::logf("lmu car light: shader %s didn't compile: %s", entry,
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> code;
        const D3D_SHADER_MACRO single[] = {{"MS", "0"}, {nullptr, nullptr}}, multi[] = {{"MS", "1"}, {nullptr, nullptr}};
        if (!compile("vs_main", "vs_5_0", single, code) ||
            FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs)))
            return false;
        if (!compile("ps_main", "ps_5_0", single, code) ||
            FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps[0])))
            return false;
        if (!compile("ps_main", "ps_5_0", multi, code) ||
            FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps[1])))
            return false;
        // The scene = our add + the scene x our scale (dual-source blending); its alpha kept.
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO,
                              D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        dd.StencilEnable = FALSE;
        if (FAILED(device->CreateBlendState(&bd, &blend)) || FAILED(device->CreateRasterizerState(&rd, &raster)) ||
            FAILED(device->CreateDepthStencilState(&dd, &depthOff)))
            return false;
        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.ByteWidth = 4 * 16 * 3 + 16 * 4;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &passCb)))
            return false;
        cb.ByteWidth = 16 * CarLightTable::kMaxLights * 2;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &lightCb)))
            return false;
        D3D11_BUFFER_DESC sb{};
        sb.Usage = D3D11_USAGE_DEFAULT;
        sb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sb.ByteWidth = 16 * kSlotsMax;
        if (FAILED(device->CreateBuffer(&sb, nullptr, &slotBuffer)))
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 0;
        sv.Buffer.NumElements = kSlotsMax;
        if (FAILED(device->CreateShaderResourceView(slotBuffer.Get(), &sv, &slotView)))
            return false;
        for (Timer& t : timers) {
            D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(device->CreateQuery(&q, &t.disjoint)))
                return false;
            q.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(device->CreateQuery(&q, &t.begin)) || FAILED(device->CreateQuery(&q, &t.end)))
                return false;
        }
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        qpcPerMs = static_cast<double>(f.QuadPart) / 1000.0;
        return true;
    }

    ID3D11RenderTargetView* scene_rtv(ID3D11Resource* scene) {
        if (scene == rtvFor && rtv)
            return rtv.Get();
        rtv.Reset();
        rtvFor = scene;
        rtvHeld = scene;
        ComPtr<ID3D11Texture2D> t;
        if (!scene || FAILED(scene->QueryInterface(IID_PPV_ARGS(&t))))
            return nullptr;
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        if (d.SampleDesc.Count > 1 || !(d.BindFlags & D3D11_BIND_RENDER_TARGET))
            return nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = rtv_format(d.Format);
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateRenderTargetView(scene, &rd, &rtv)))
            rtv.Reset();
        return rtv.Get();
    }

    ID3D11ShaderResourceView* depth_srv(ID3D11Resource* depth, bool& ms) {
        if (depth == depthFor && depthView) {
            ms = depthViewMs;
            return depthView.Get();
        }
        depthView.Reset();
        depthFor = depth;
        depthHeld = depth;
        ComPtr<ID3D11Texture2D> t;
        if (!depth || FAILED(depth->QueryInterface(IID_PPV_ARGS(&t))))
            return nullptr;
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE))
            return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = srv_format(d.Format);
        depthViewMs = d.SampleDesc.Count > 1;
        sd.ViewDimension = depthViewMs ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        if (!depthViewMs)
            sd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(depth, &sd, &depthView)))
            depthView.Reset();
        ms = depthViewMs;
        return depthView.Get();
    }
};

CarLightPass::CarLightPass() : impl_(new Impl) {}
CarLightPass::~CarLightPass() { delete impl_; }

bool CarLightPass::run(ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* depth,
                       ID3D11ShaderResourceView* slots, bool slotsMs, const CarLightTable& table,
                       const CarLightView& view, const CarLightSettings& settings, int eye,
                       ID3D11ShaderResourceView* normals) {
    (void)eye;
    Impl& m = *impl_;
    if (failed_ || !context || !scene || !depth || !slots || !view.eyeW || !view.eyeH)
        return false;
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    if (!m.made) {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        m.made = true;
        if (!device || !m.make(device.Get())) {
            failed_ = true;
            simqvfr::log_line("lmu car light: the pass couldn't be made (shaders, states or buffers); off for the session");
            return false;
        }
        simqvfr::log_line("lmu car light: the pass is made (relights the cars after each eye pass's scene)");
        QueryPerformanceCounter(&t0);  // its making (the shaders' compile, once) isn't a frame's cost
    }
    if (table.first_slot() > table.last_slot())
        return false;  // no car part drawn
    bool depthMs = false;
    ID3D11RenderTargetView* rtv = m.scene_rtv(scene);
    ID3D11ShaderResourceView* depthView = m.depth_srv(depth, depthMs);
    if (!rtv || !depthView || depthMs != slotsMs)
        return false;

    // The camera: projection = inverse(view) x viewProj (row vectors), and its inverse.
    double v[16], vp[16], vi[16], p[16], pi[16];
    for (int i = 0; i < 16; ++i) {
        v[i] = view.view[i];
        vp[i] = view.viewProj[i];
    }
    if (!invert4(v, vi))
        return false;
    mul4(vi, vp, p);
    if (!invert4(p, pi))
        return false;

    // The slots this pass drew: their lights and flags.
    const unsigned first = table.first_slot(), last = (std::min)(table.last_slot(), kSlotsMax - 1);
    if (first <= last) {
        m.slotWords.resize(size_t{4} * (last - first + 1));
        for (unsigned s = first; s <= last; ++s) {
            uint32_t w[4];
            table.packed(s, w);
            std::memcpy(&m.slotWords[size_t{4} * (s - first)], w, sizeof(w));
        }
        const D3D11_BOX box{16 * first, 0, 0, 16 * (last + 1), 1, 1};
        context->UpdateSubresource(m.slotBuffer.Get(), 0, &box, m.slotWords.data(), 0, 0);
    }
    {
        float lights[CarLightTable::kMaxLights * 8] = {};
        const auto& ls = table.lights();
        for (size_t i = 0; i < ls.size() && i < CarLightTable::kMaxLights; ++i) {
            float* pos = lights + 4 * i;
            float* col = lights + 4 * CarLightTable::kMaxLights + 4 * i;
            pos[0] = ls[i].pos[0];
            pos[1] = ls[i].pos[1];
            pos[2] = ls[i].pos[2];
            pos[3] = ls[i].radius;
            col[0] = ls[i].colour[0];
            col[1] = ls[i].colour[1];
            col[2] = ls[i].colour[2];
        }
        context->UpdateSubresource(m.lightCb.Get(), 0, nullptr, lights, 0, 0);
    }
    {
        float cb[4 * 16 * 3 / 4 + 16] = {};
        for (int i = 0; i < 16; ++i) {
            cb[i] = static_cast<float>(pi[i]);
            cb[16 + i] = static_cast<float>(p[i]);
            cb[32 + i] = view.view[i];
        }
        float* rest = cb + 48;
        rest[0] = static_cast<float>(view.eyeW);
        rest[1] = static_cast<float>(view.eyeH);
        rest[2] = static_cast<float>(view.focusW);
        rest[3] = static_cast<float>(view.focusH);
        std::memcpy(rest + 4, view.remap, sizeof(view.remap));
        const CarLightSettings s = simqvfr::clamped(settings);
        rest[8] = s.fill / 100.0f;
        rest[9] = s.occlusion / 100.0f;
        rest[10] = s.highlights / 100.0f;
        rest[11] = 0.55f + (0.08f - 0.55f) * (s.gloss / 100.0f);  // roughness from the gloss
        rest[12] = 0.40f;  // occlusion radius, metres
        rest[13] = 1.0f;   // the player's cockpit: nothing within 1 m, fully from 2 m
        rest[14] = 2.0f;
        rest[15] = s.smooth / 100.0f;  // the smoothing width, cm to m
        context->UpdateSubresource(m.passCb.Get(), 0, nullptr, cb, 0, 0);
    }

    Impl::Timer& timer = m.timers[m.nextTimer];
    const bool timed = !timer.pending;
    if (timed) {
        context->Begin(timer.disjoint.Get());
        context->End(timer.begin.Get());
    }
    ComPtr<ID3D11Texture2D> sceneTex;
    scene->QueryInterface(IID_PPV_ARGS(&sceneTex));
    D3D11_TEXTURE2D_DESC sd{};
    if (sceneTex)
        sceneTex->GetDesc(&sd);
    // Every target the pass uses: nothing of ours left bound on a target the game then reads (the caller restores the
    // game's own).
    ID3D11ShaderResourceView* none[4] = {};
    context->PSSetShaderResources(0, 4, none);
    context->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vpPort{0, 0, static_cast<float>(sd.Width), static_cast<float>(sd.Height), 0, 1};
    const LONG right = static_cast<LONG>((std::min)(sd.Width, view.eyeW + view.focusW));
    const LONG bottom = static_cast<LONG>((std::min)(sd.Height, (std::max)(view.eyeH, view.focusH)));
    const D3D11_RECT scissor{0, 0, right, bottom};
    context->RSSetViewports(1, &vpPort);
    context->RSSetScissorRects(1, &scissor);
    context->RSSetState(m.raster.Get());
    const float factor[4] = {1, 1, 1, 1};
    context->OMSetBlendState(m.blend.Get(), factor, 0xffffffffu);
    context->OMSetDepthStencilState(m.depthOff.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m.vs.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(m.ps[slotsMs ? 1 : 0].Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {m.passCb.Get(), m.lightCb.Get()};
    context->PSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* views[4] = {depthView, slots, m.slotView.Get(), normals};
    context->PSSetShaderResources(0, 4, views);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 4, none);
    if (timed) {
        context->End(timer.end.Get());
        context->End(timer.disjoint.Get());
        timer.pending = true;
        m.nextTimer = (m.nextTimer + 1) % kTimerSlots;
    }
    ++m.runs;
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    m.cpuSum += static_cast<double>(t1.QuadPart - t0.QuadPart) / m.qpcPerMs;
    return true;
}

void CarLightPass::add_collect_ms(double ms) { impl_->collectSum += ms; }

void CarLightPass::frame_end(ID3D11DeviceContext* context) {
    Impl& m = *impl_;
    if (!m.made || failed_ || !context)
        return;
    for (Impl::Timer& t : m.timers) {
        if (!t.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 b = 0, e = 0;
        if (context->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(t.begin.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(t.end.Get(), &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        if (dj.Disjoint || !dj.Frequency || e < b)
            continue;
        m.gpuSum += static_cast<double>(e - b) * 1000.0 / static_cast<double>(dj.Frequency);
        ++m.gpuRuns;
    }
    ++m.frames;
    if (m.frames >= 90) {  // the averages per frame over the last 90 frames (both eyes' runs in each)
        cpuMs_ = (m.cpuSum + m.collectSum) / m.frames;
        // Runs not timed (their query set still pending) count as the timed ones' average.
        const double runsPerFrame = static_cast<double>(m.runs) / m.frames;
        gpuMs_ = m.gpuRuns ? m.gpuSum / m.gpuRuns * runsPerFrame : 0;
        m.cpuSum = m.gpuSum = m.collectSum = 0;
        m.gpuRuns = m.runs = 0;
        m.frames = 0;
        ++windows_;
    }
}

}  // namespace lmu

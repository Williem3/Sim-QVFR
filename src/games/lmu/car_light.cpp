// LMU's night car lighting (car_light.h).
#include "car_light.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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
                         const OmniLayout& layout, uint64_t bufferGeneration, bool paint, bool normal, bool ao,
                         bool detail) {
    if (slot >= slotLights_.size() || !buffer)
        return;
    const size_t need = (std::max)(layout.posOffset, layout.colourOffset) + size_t{16} * layout.count;
    if (bufferBytes < need)
        return;
    SlotLights& s = slotLights_[slot];
    if (slotFrozen_[slot] == frame_ && slotFrame_[slot] == frame_) {  // relit already this frame: its lists kept
        // Its targets' flags still go with this draw: drawn in this pass by a shader that doesn't write the normal
        // (occlusion, bent normal), the target holds none for it here (cleared: a black normal, occlusion 0).
        if (!normal)
            slotPaint_[slot] &= static_cast<uint8_t>(~14u);
        if (!ao)
            slotPaint_[slot] &= static_cast<uint8_t>(~4u);
        if (!detail)
            slotPaint_[slot] &= static_cast<uint8_t>(~8u);
        firstSlot_ = (std::min)(firstSlot_, slot);
        lastSlot_ = (std::max)(lastSlot_, slot);
        return;
    }
    if (slotFrame_[slot] != frame_) {  // its first draw this frame: a list of its own again
        slotFrame_[slot] = frame_;
        s.count = 0;
        s.spotCount = 0;
        s.generation = ~0ull;
        // The game's: while every draw of the part has it.
        slotPaint_[slot] = static_cast<uint8_t>((normal ? 2 : 0) | (normal && ao ? 4 : 0) | (normal && detail ? 8 : 0));
    }
    slotPaint_[slot] |= paint ? 1 : 0;
    if (!normal)
        slotPaint_[slot] &= static_cast<uint8_t>(~14u);
    if (!ao)
        slotPaint_[slot] &= static_cast<uint8_t>(~4u);
    if (!detail)
        slotPaint_[slot] &= static_cast<uint8_t>(~8u);
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

namespace {
// A spotlight's strength at a point (64 bytes as LMU writes them: position and range, direction and outer cone's cosine,
// attenuation and inner cone's cosine, colour and falloff): its colour's brightness over its attenuation, times its
// range falloff; 0 beyond its range or unreadable.
float spot_weight(const uint8_t* spotBuffer, size_t spotBytes, unsigned index, const float (&at)[3]) {
    if (!spotBuffer || size_t{index} * 64 + 64 > spotBytes)
        return 0;
    float d[16];
    std::memcpy(d, spotBuffer + size_t{index} * 64, 64);
    for (float v : d)
        if (!std::isfinite(v))
            return 0;
    const float dx = at[0] - d[0], dy = at[1] - d[1], dz = at[2] - d[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d[3] <= 0 || dist >= d[3] + 3.0f)  // a car's half length of margin, as the floodlights'
        return 0;
    const float atten = (std::max)(d[8] + d[9] * dist + d[10] * dist * dist, 1e-3f);
    const float bright = 0.3f * d[12] + 0.59f * d[13] + 0.11f * d[14];
    return (std::max)(bright, 0.0f) / atten * (std::max)(1 - dist / (d[3] + 3.0f), 0.0f);
}
}  // namespace

void CarLightTable::add_spots(unsigned slot, uint32_t count, const uint32_t* packed, unsigned capacity,
                              const float (&origin)[3], const uint8_t* spotBuffer, size_t spotBytes) {
    if (!packed || count > 20)
        return;
    SlotLights* s = slot < slotLights_.size() && slotFrame_[slot] == frame_ && slotFrozen_[slot] != frame_
                        ? &slotLights_[slot]
                        : nullptr;
    if (!s)
        return;
    // The slot's own: LMU's indices kept (the strongest six at it); the frame's list (what the GPU's spotlights are)
    // gets only kept ones, when a pass packs the slot. Every listed one there filled its 32 first-come, and cars drawn
    // later lost their headlights (review, 2026-11-19).
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t index = (packed[k / 2] >> (16 * (k & 1))) & 0xffff;
        if (index >= capacity)
            continue;
        bool known = false;
        for (uint8_t j = 0; j < s->spotCount; ++j)
            known = known || s->spot[j] == index;
        if (known)
            continue;
        const float w = spot_weight(spotBuffer, spotBytes, index, origin);
        if (w <= 0) {
            ++spotOutOfReach_;
            continue;  // out of its reach, or unreadable
        }
        ++spotKept_;
        if (s->spotCount < kSpotsPerSlot) {
            s->spot[s->spotCount] = static_cast<uint16_t>(index);
            s->spotWeight[s->spotCount++] = w;
        } else {  // full: in place of the weakest, when stronger
            uint8_t weakest = 0;
            for (uint8_t j = 1; j < kSpotsPerSlot; ++j)
                if (s->spotWeight[j] < s->spotWeight[weakest])
                    weakest = j;
            if (w > s->spotWeight[weakest]) {
                s->spot[weakest] = static_cast<uint16_t>(index);
                s->spotWeight[weakest] = w;
            }
        }
    }
}

void CarLightTable::top_lights(int (&out)[4]) const {
    if (topFrame_ == frame_) {
        for (int k = 0; k < 4; ++k)
            out[k] = top_[k];
        return;
    }
    for (int& o : out)
        o = -1;
    std::vector<unsigned> refs(lights_.size(), 0);
    for (size_t slot = 0; slot < slotLights_.size(); ++slot) {
        if (slotFrame_[slot] != frame_)
            continue;
        for (uint8_t k = 0; k < slotLights_[slot].count; ++k)
            if (slotLights_[slot].index[k] < refs.size())
                ++refs[slotLights_[slot].index[k]];
    }
    for (int n = 0; n < 4; ++n) {
        int best = -1;
        for (size_t i = 0; i < refs.size(); ++i)
            if (refs[i] && (best < 0 || refs[i] > refs[static_cast<size_t>(best)]))
                best = static_cast<int>(i);
        if (best < 0)
            break;
        out[n] = best;
        refs[static_cast<size_t>(best)] = 0;
    }
    topFrame_ = frame_;
    for (int k = 0; k < 4; ++k)
        top_[k] = out[k];
}

void CarLightTable::packed(unsigned slot, uint32_t (&out)[4]) const {
    out[0] = out[1] = out[3] = 0xffffffffu;
    out[2] = 0xffff00u;  // no flags, no spotlights (a 0 there read as spotlight 0: review, 2026-11-19)
    if (slot >= slotLights_.size() || slotFrame_[slot] != frame_)
        return;
    slotFrozen_[slot] = frame_;
    const SlotLights& s = slotLights_[slot];
    for (uint8_t k = 0; k < s.count; ++k) {
        uint32_t& word = out[k / 4];
        const unsigned shift = 8 * (k % 4);
        word = (word & ~(0xffu << shift)) | (static_cast<uint32_t>(s.index[k]) << shift);
    }
    // The spotlights: 0-3 in the last word, 4-5 above the flags, as their places in the frame's list (added now: the
    // ones a pass relights; one past its 32 left out).
    out[3] = 0xffffffffu;
    out[2] = slotPaint_[slot] | 0xffff00u;
    uint8_t n = 0;
    for (uint8_t k = 0; k < s.spotCount; ++k) {
        auto at = std::find(spots_.begin(), spots_.end(), s.spot[k]);
        if (at == spots_.end()) {
            if (spots_.size() >= kMaxSpots)
                continue;
            spots_.push_back(s.spot[k]);
            at = spots_.end() - 1;
        }
        const uint32_t local = static_cast<uint32_t>(at - spots_.begin());
        uint32_t& word = n < 4 ? out[3] : out[2];
        const unsigned shift = n < 4 ? 8u * n : 8u * (n - 3);
        word = (word & ~(0xffu << shift)) | (local << shift);
        ++n;
    }
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
    float4 mode;                 // x: the floodlights' shadows, y: the paint's detail, z: the lacquer's reflections (1 on)
    float4 mode2;                // x: the headlights' count (0: off), y: the ground's shadows, z: night vision (1 on)
    float4 ground;               // the ground's floodlights: the frame's strongest (indices, -1 none)
    row_major float4x4 otherClip;     // this eye's view space -> the other eye's clip (its eye region)
    row_major float4x4 otherInvProj;  // the other eye's clip -> its view space
    float4 otherEye;             // the other eye's place in this eye's view space; w: 1 with its depth (0: none)
    float4 otherSize;            // its depth's width and height; z, w the quality tier of the focus view, the periphery
};
cbuffer CarLightSpots : register(b2) {
    float4 spotData[128];  // the headlights: per spotlight its position and range, direction and outer cone, attenuation
                           // and inner cone, colour and cone falloff (LMU's spotLightGlobals, world space)
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
Texture2DMS<float4> detailTex : register(t4);
float3 load_detail(int2 p) { return detailTex.Load(p, 0).xyz; }
#else
Texture2D<float4> detailTex : register(t4);
float3 load_detail(int2 p) { return detailTex.Load(int3(p, 0)).xyz; }
#endif
Texture2D<float4> sceneTex : register(t5);  // the scene as the pass began (the reflections read it)
Texture2D<float> otherDepth : register(t6); // the other eye's depth (its eye region), as its last pass left it
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

// pixel_of, and whether the point is in front of the camera (w > 0): a reflected ray may turn back past it.
bool pixel_ahead(float3 v, int r, float2 origin, float2 size, out float2 q) {
    float4 c = mul(float4(v, 1), proj);
    float2 ndc = c.xy / max(abs(c.w), 1e-6);
    if (r == 1)
        ndc = float2(remap.x * ndc.x + remap.y, remap.z * ndc.y + remap.w);
    q = origin + (ndc * float2(0.5, -0.5) + 0.5) * size;
    return c.w > 1e-4;
}

float2 pixel_of(float3 v, int r, float2 origin, float2 size) {
    float4 c = mul(float4(v, 1), proj);
    float2 ndc = c.xy / c.w;
    if (r == 1)
        ndc = float2(remap.x * ndc.x + remap.y, remap.z * ndc.y + remap.w);
    return origin + (ndc * float2(0.5, -0.5) + 0.5) * size;
}

// A point (view space) looked up where it was drawn: in the focus view when it falls inside it (sharper), else in the
// eye's own region, whichever view the pixel asking is in. Both views then see the same occluders: the focus view
// alone lost every car past its edge, and its searches stopped there (run 4, 2026-11-19: shadows cast away from a
// light ended short or went missing in the focus area, not around it). `lq`: the drawn surface's distance from the
// camera there, `slot` its car slot. 1: found; 0: nothing drawn there (the sky); -1: off both views (a search toward
// a light stops there: it only leaves further, and carrying on doubled the floodlight shadows' cost, run 5).
// Two values in [0, 1) fixed to the world point `P` (view space, `lp` its distance): the AO kernel's turn and the
// shadow searches' offsets. In cells about a periphery pixel across at that distance (rounded to a power of two, so
// neighbours share them), the same in both eyes and in the focus view as around it: from the screen's pixels the two
// eyes dithered the same spot differently (review, 2026-11-19).
float2 world_noise(float3 P, float lp) {
    float3 W = mul((float3x3)viewM, P - viewM[3].xyz);  // view to world (viewM is rigid: its rotation's transpose)
    float cell = exp2(round(log2(max(lp * 0.0008, 1e-4))));
    int3 c = int3(floor(W / cell));
    uint h = (uint)c.x * 73856093u ^ (uint)c.y * 19349663u ^ (uint)c.z * 83492791u;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h ^= h >> 16;
    return float2(h & 0xffff, h >> 16) / 65536.0;
}

// Our focus mask (mvr_pass_focus_mask.h) writes the nearest depth (1 under LMU's reversed depth) into the focus box's
// texels outside the focus shape: nothing was drawn there. Read as a surface at the near plane it was a hit for every
// search crossing a masked corner (review, 2026-11-19).
static const float kMaskDepth = 0.99999;
bool drawn_depth(float d) { return d > 0 && d < kMaskDepth; }

int lookup_world(float3 S, out float lq, out uint slot, out int2 pixel, out float edge) {
    lq = 0;
    slot = 0;
    pixel = int2(0, 0);
    edge = 0;
    float4 c = mul(float4(S, 1), proj);
    if (c.w <= 1e-4)
        return -1;
    float2 ndc = c.xy / c.w;
    edge = 1 - max(abs(ndc.x), abs(ndc.y));  // to the eye's own screen edge (ndc units): the focus view's isn't one
    if (region.z > 0) {
        float2 f = float2(remap.x * ndc.x + remap.y, remap.z * ndc.y + remap.w);
        if (all(abs(f) < 1)) {
            float2 q = float2(region.x, 0) + (f * float2(0.5, -0.5) + 0.5) * region.zw;
            float ds = load_depth(int2(q));
            if (drawn_depth(ds)) {  // else (the sky, a masked corner) the eye's region below
                pixel = int2(q);
                lq = length(view_pos(floor(q) + 0.5, ds, 1, float2(region.x, 0), region.zw));
                slot = load_slot(pixel);
                return 1;
            }
        }
    }
    if (any(abs(ndc) >= 1))
        return -1;
    float2 q = (ndc * float2(0.5, -0.5) + 0.5) * region.xy;
    pixel = int2(q);
    float ds = load_depth(pixel);
    if (!drawn_depth(ds))
        return 0;
    lq = length(view_pos(floor(q) + 0.5, ds, 0, float2(0, 0), region.xy));
    slot = load_slot(pixel);
    return 1;
}

// How much of a reflection's hit `H` (view space) the other eye sees too: 1 where it does, 0 where a surface nearer
// to that eye hides it or it lies off that eye's screen (faded toward its edge, as at this eye's own). Screen-space
// reflections show what each eye's own image holds: the ground beside a car, hidden from the left eye by the player's
// door and in view of the right, was mirrored in that car's paint in the right eye alone (race, 2026-11-19). Kept only
// where both eyes could show it, the two agree. Nothing drawn there in the other eye's depth (the sky, a texel not
// drawn): no telling, kept. No other eye's depth: 1.
float other_sees(float3 H) {
    if (otherEye.w <= 0)
        return 1;
    float4 c = mul(float4(H, 1), otherClip);
    if (c.w <= 1e-4)
        return 0;
    float2 ndc = c.xy / c.w;
    float edge = 1 - max(abs(ndc.x), abs(ndc.y));
    if (edge <= 0)
        return 0;
    float seen = saturate(edge * 5);
    int2 q = min(int2((ndc * float2(0.5, -0.5) + 0.5) * otherSize.xy), int2(otherSize.xy) - 1);
    float d = otherDepth.Load(int3(q, 0));
    if (!drawn_depth(d))
        return seen;
    float2 centre = (float2(q) + 0.5) / otherSize.xy * float2(2, -2) + float2(-1, 1);
    float4 v = mul(float4(centre, d, 1), otherInvProj);
    float lo = length(v.xyz / v.w), lh = length(H - otherEye.xyz);
    float tol = 0.1 + 0.05 * lh;  // its depth is a pass old for the left eye, and coarser than the focus view's
    return seen * saturate(2 - (lh - lo) / tol);
}

int sample_world(float3 S, out float lq, out uint slot) {
    int2 pixel;
    float edge;
    return lookup_world(S, lq, slot, pixel, edge);
}

float3 neighbour(int2 q, int r, float2 origin, float2 size, float3 none) {
    q = clamp(q, int2(origin), int2(origin + size) - 1);
    float d = load_depth(q);
    if (!drawn_depth(d))
        return none;
    return view_pos(float2(q) + 0.5, d, r, origin, size);
}

// The surface's direction smoothed over `width` metres of the same car part (slot `s`): the depth is flat across each
// triangle, so pixel-to-pixel differences give each triangle one direction and the shading steps at its edges (run 1,
// 2026-11-18: facets in the highlights). Two rings of eight points around the pixel, at the width and half of it, each
// point kept when it is the same part and on the same surface (its distance from the camera within a slope's reach);
// the fan of triangles between consecutive kept points summed (each by its area) into one direction. Too few kept: the
// pixel's own.
// Quality tiers (otherSize.z: the focus view's, otherSize.w: the periphery's; 0 Low, 1 Medium, 2 High): how many
// samples and steps the occlusion, smoothing and shadow searches take (the reflections take all theirs in every tier). The periphery runs a tier below the focus view (seen at a lower resolution, in
// side vision): the same averages, a little more noise.
static const int kAoTaps[3] = {4, 6, 8};
static const int kSmoothRings[3] = {1, 2, 2};
static const int kShadowSteps[3] = {5, 7, 10};
static const int kGroundRays[3] = {1, 2, 2};
static const int kGroundSteps[3] = {5, 6, 8};
int tier_of(int r) { return clamp((int)(r == 1 || region.z <= 0 ? otherSize.z : otherSize.w), 0, 2); }

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
    const int rings = kSmoothRings[tier_of(r)];
    [loop] for (int ring = 0; ring < rings; ++ring) {
        float reach = px * (ring == 0 ? 1.0 : 0.5);
        float3 pts[8];
        bool ok[8];
        [unroll] for (int k = 0; k < 8; ++k) {
            int2 q = clamp(p + int2(round(kRing[k] * reach)), int2(origin), int2(origin + size) - 1);
            float dq = load_depth(q);
            ok[k] = false;
            pts[k] = 0;
            if (drawn_depth(dq) && load_slot(q) == s) {
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

)" R"(
struct Out {
    float4 add : SV_Target0;
    float4 mul : SV_Target1;
};

// Night vision's colour: as light fades the eye loses colour and sees a little bluer (rods take over from cones); the
// lit stays as it was. Luminance as the scene holds it (LMU's night: lit paint ~0.1-0.3, the track ~0.05, the sky
// ~0.001).
float3 night_vision(float3 c) {
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    float k = (1 - smoothstep(0.004, 0.08, lum)) * 0.7;
    return lerp(c, lum * float3(0.80, 0.95, 1.25), k);
}

// The ground around the cars (the track, the walls: any pixel not a car's, within ~1.5 m of one on the screen): the
// cars' occlusion (points of the hemisphere above it hidden by a car) and the floodlights' shadows (the frame's
// strongest floodlights, the depth searched toward each up to 4 m for a car in the way); the share of the light a
// hidden floodlight gave taken out, a third of the game's light (headlights, sky) kept. 1: nothing to darken.
float3 ground_neighbour(int2 q, int r, float2 origin, float2 size, float3 none) {
    int2 c = clamp(q, int2(origin), int2(origin + size) - 1);
    return load_slot(c) != 0 ? none : neighbour(q, r, origin, size, none);
}

float ground_shade(float4 pos, int2 p, int r, float2 origin, float2 size, float d) {
    float3 P = view_pos(pos.xy, d, r, origin, size);
    float lp = length(P);
    if (lp < extra.z)
        return 1;
    // The ground's own direction: its neighbours alone, not a car's beside it (at the line where a car stands on the
    // track the car's pixel is the nearest in depth, and the ground's direction tilted toward the eye with it).
    float3 Pr = ground_neighbour(p + int2(1, 0), r, origin, size, P), Pl = ground_neighbour(p - int2(1, 0), r, origin, size, P);
    float3 Pd = ground_neighbour(p + int2(0, 1), r, origin, size, P), Pu = ground_neighbour(p - int2(0, 1), r, origin, size, P);
    // A neighbour that isn't the ground's (a car's: `none`, P itself) gives way to the other side.
    float3 dx = length(Pr - P) < 1e-6 ? P - Pl : length(Pl - P) < 1e-6 ? Pr - P
              : abs(length(Pr) - lp) < abs(lp - length(Pl)) ? Pr - P : P - Pl;
    float3 dy = length(Pd - P) < 1e-6 ? P - Pu : length(Pu - P) < 1e-6 ? Pd - P
              : abs(length(Pd) - lp) < abs(lp - length(Pu)) ? Pd - P : P - Pu;
    float3 V = -P / max(lp, 1e-4);
    float3 c = cross(dy, dx);
    float3 N = dot(c, c) > 1e-14 ? normalize(c) : V;
    if (dot(N, V) < 0)
        N = -N;
    float3 t = normalize(abs(N.y) < 0.99 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
    float3 b = cross(N, t);
    // Near a car: points on the ground around it, 1.5 m and 0.7 m out, any of them where a car was drawn. In metres,
    // so the focus view and the periphery agree (in pixels it was capped at 300: fewer metres at the focus density).
    bool near = false;
    float lq;
    uint sl;
    [unroll] for (int k = 0; k < 8; ++k) {
        float2 ring = kRing[k] * ((k & 1) ? 0.7 : 1.5);
        near = near || (sample_world(P + (t * ring.x + b * ring.y) + N * 0.3, lq, sl) > 0 && sl != 0);
    }
    if (!near)
        return 1;
    float2 wn = world_noise(P, lp);
    float noise = wn.x, noise2 = wn.y;
    float angle = noise * 6.2831853;
    float3 t2 = t * cos(angle) + b * sin(angle), b2 = b * cos(angle) - t * sin(angle);
    float occluded = 0;
    const int tier = tier_of(r);
    const int taps = kAoTaps[tier];
    [loop] for (int k2 = 0; k2 < taps; ++k2) {
        float3 kk = kKernel[k2];
        float3 S = P + (t2 * kk.x + b2 * kk.y + N * kk.z) * 1.0;
        if (sample_world(S, lq, sl) <= 0 || sl == 0)
            continue;  // only the cars' occlusion: the game's ground is as it drew it
        float range = saturate(1.0 / max(abs(lp - lq), 1e-3));
        occluded += (length(S) - lq > 0.02) ? range : 0;
    }
    float ao = 1 - occluded / taps;
    float lit = 0, litSeen = 0;
    [unroll] for (int g = 0; g < 4; ++g) {
        int idx = (int)ground[g];
        if (idx < 0)
            continue;
        float3 lv = mul(float4(lightPos[idx].xyz, 1), viewM).xyz;
        float3 toL = lv - P;
        float dist = length(toL), radius = lightPos[idx].w;
        if (dist >= radius || dist <= 1e-3)
            continue;
        float3 L = toL / dist;
        float diffuse = dot(lightCol[idx].rgb, float3(0.3, 0.59, 0.11)) * (1 - dist / radius) * saturate(dot(N, L));
        if (diffuse <= 0)
            continue;
        // Soft: two rays toward either side of the light (a floodlight's panel, ~7 degrees across, turned per pixel),
        // each hidden by a car fading out with the distance to it (full within 1.5 m, gone at 4 m, where the search
        // ends). One ray stopping at 4 m drew hard, blotchy patches cut off at their far end (run 4, 2026-11-19).
        float3 lt = normalize(abs(L.y) < 0.99 ? cross(L, float3(0, 1, 0)) : cross(L, float3(1, 0, 0)));
        float3 lb = cross(L, lt);
        float turn = noise2 * 6.2831853;
        float3 spread = (lt * cos(turn) + lb * sin(turn)) * 0.12;
        float seen = 0;
        float span = min(dist, 4.0);
        // One ray (Low): toward one side of the light, the side turned per pixel (a dithered penumbra).
        const int rays = kGroundRays[tier], steps = kGroundSteps[tier];
        [loop] for (int ray = 0; ray < rays; ++ray) {
            float3 Lr = normalize(L + (ray == 0 ? spread : -spread));
            float vis = 1;
            [loop] for (int k3 = 1; k3 <= steps; ++k3) {
                float h = span * (k3 - 0.5 + noise) / steps;
                float3 S = P + N * 0.03 + Lr * h;
                int found = sample_world(S, lq, sl);
                if (found < 0)
                    break;
                if (found == 0)
                    continue;
                float behind = length(S) - lq;
                if (behind > 0.03 && behind < 1.0 && sl != 0) {  // a car between it and the light
                    vis = smoothstep(1.5, 4.0, h);
                    break;
                }
            }
            seen += vis / rays;
        }
        lit += diffuse;
        litSeen += diffuse * seen;
    }
    float shadow = lit > 1e-4 ? (litSeen + 0.5 * lit) / (1.5 * lit) : 1;
    return lerp(1, ao, shade.y) * shadow;
}

)" R"(
// A pixel that isn't a car's: the ground's shadows near the cars, night vision's colour (from the scene's copy).
Out other_pixel(float4 pos, int2 p, int r, float2 origin, float2 size, float d) {
    float m = 1;
    if (mode2.y > 0 && d > 0)
        m = ground_shade(pos, p, r, origin, size, d);
    Out o;
    if (mode.w > 0) {  // dev builds' debug view: the rest dimmed, its ground shading kept
        o.add = float4(0, 0, 0, 0);
        o.mul = float4((0.25 * m).xxx, 1);
        return o;
    }
    if (mode2.z > 0) {
        o.add = float4(night_vision(sceneTex.Load(int3(p, 0)).rgb * m), 0);
        o.mul = float4(0, 0, 0, 1);
        return o;
    }
    if (m >= 0.999)
        discard;
    o.add = float4(0, 0, 0, 0);
    o.mul = float4(m.xxx, 1);
    return o;
}

)" R"(
Out ps_main(float4 pos : SV_Position) {
    int2 p = int2(pos.xy);
    float2 origin, size;
    int r = region_of(p, origin, size);
    if (r < 0)
        discard;
    if (mode2.w > 0) {  // night vision's colour alone (no camera or depth for the relight this pass): every pixel
        Out nv;
        nv.add = float4(night_vision(sceneTex.Load(int3(p, 0)).rgb), 0);
        nv.mul = float4(0, 0, 0, 1);
        return nv;
    }
    uint s = load_slot(p);
    float d = load_depth(p);
    if (d >= kMaskDepth)
        discard;  // a masked texel of the focus box: nothing drawn there
    if (s == 0)
        return other_pixel(pos, p, r, origin, size, d);
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
    // A slot past the per-slot buffer (a load out of it reads 0: spotlight 0, six times): no lights, no flags.
    uint4 info = s <= 4096 ? slotInfo[s - 1] : uint4(0xffffffff, 0xffffffff, 0xffff00, 0xffffffff);
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
    float noise = world_noise(P, lp).x;
    float angle = noise * 6.2831853;
    float3 t = normalize(abs(N.y) < 0.99 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
    float3 b = cross(N, t);
    float ca = cos(angle), sa = sin(angle);
    float3 t2 = t * ca + b * sa, b2 = b * ca - t * sa;
    float occluded = 0;
    const int tier = tier_of(r);
    const int taps = kAoTaps[tier];
    [loop] for (int k = 0; k < taps; ++k) {
        float3 kk = kKernel[k];
        float3 S = P + (t2 * kk.x + b2 * kk.y + N * kk.z) * extra.x;
        float lq;
        uint sq;
        if (sample_world(S, lq, sq) <= 0)
            continue;
        float range = saturate(extra.x / max(abs(lp - lq), 1e-3));
        occluded += (length(S) - lq > 0.02) ? range : 0;
    }
    // The screen's occlusion (between parts and objects: under the car, the wheels in their arches) times the model's
    // own baked one (panel gaps, vents, the inside of the arches: what the screen can't see).
    float ao = (1 - occluded / taps) * baked;

    // The highlights' normal: with the paint's detail, the one the game's normal maps bent (orange peel, the carbon's
    // weave; its ambient cube map's lookup); the shading and the shadows keep the smooth one (the maps' detail would
    // only make them noisy).
    float3 Nh = N;
    if (mode.y > 0 && (info.z & 8) != 0) {
        float3 bent = mul(float4(load_detail(p), 0), viewM).xyz;  // as the game looked up with it (any length)
        // Taken while it stays near the surface's own (a normal map bends it by tens of degrees at most): a lookup in
        // another space (swizzled or negated axes) or with another vector would point elsewhere.
        if (dot(bent, bent) > 1e-6 && dot(normalize(bent), N) > 0.5) {
            Nh = normalize(bent);
            if (dot(Nh, V) < 0)
                Nh = -Nh;
        }
    }

    // The floodlights the part's draws were handed: a GGX highlight each, as lacquer reflects a lamp. With the shadows,
    // each light's visibility from the depth: points toward it, up to 1.2 m, any of them behind a nearer surface (a wing,
    // the bodywork, another car) hides it. Hidden, its highlight goes, and its share of the game's floodlight light
    // (the diffuse it gave: N.L, its falloff) is taken out of the pixel; the rest of the game's light (headlights, sky)
    // taken as a quarter of it stays.
    float rough = shade.w, a = rough * rough, a2 = a * a, kq = a * 0.5;
    float NdotV = saturate(dot(Nh, V)) + 1e-4;
    float3 spec = 0;
    float lit = 0, litSeen = 0;
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
        float diffuse = dot(lightCol[idx].rgb, float3(0.3, 0.59, 0.11)) * att * saturate(dot(N, L));
        float seen = 1;
        if (mode.x > 0 && diffuse > 0) {
            float reach = min(dist, 1.2);
            const int steps = kShadowSteps[tier];
            [loop] for (int k = 1; k <= steps; ++k) {
                float3 S = P + N * 0.02 + L * (reach * (k - 0.5 + noise) / steps);
                float lq;
                uint sq;
                int found = sample_world(S, lq, sq);
                if (found < 0)
                    break;
                if (found == 0)
                    continue;
                float behind = length(S) - lq;
                if (behind > 0.03 && behind < 0.6) {  // a surface in front of the point, not one far behind it
                    seen = 0;
                    break;
                }
            }
        }
        lit += diffuse;
        litSeen += diffuse * seen;
        float NdotL = saturate(dot(Nh, L));
        float3 H = normalize(L + V);
        float NdotH = saturate(dot(Nh, H)), VdotH = saturate(dot(V, H));
        float dd = NdotH * NdotH * (a2 - 1) + 1;
        float D = a2 / (3.14159265 * dd * dd);
        float G = (NdotL / (NdotL * (1 - kq) + kq)) * (NdotV / (NdotV * (1 - kq) + kq));
        float F = 0.04 + 0.96 * pow(1 - VdotH, 5);
        spec += seen * lightCol[idx].rgb * att * min(D * G * F / (4 * NdotV), kHighlightPeak);
    }
    // The headlights (other cars' and the player's): LMU's spotlights the frame's car parts were handed, their light
    // as LMU's shaders weigh it (the cone, its falloff, the attenuation, the range, the surface facing it), a
    // highlight each as the floodlights'. No shadows: a headlight's own car rarely stands between it and the paint lit.
    // The part's own (up to six: LMU's per-draw list), in view space already.
    [loop] for (int j = 0; j < 6 && mode2.x > 0; ++j) {
        uint si = (j < 4 ? info.w >> (8 * j) : info.z >> (8 * (j - 3))) & 0xff;
        if (si == 0xff)
            break;
        if (si >= (uint)mode2.x)
            continue;
        float4 sPos = spotData[si * 4], sDir = spotData[si * 4 + 1], sAtt = spotData[si * 4 + 2], sCol = spotData[si * 4 + 3];
        if (sPos.w <= 0)
            continue;
        float3 sd = sDir.xyz;
        float3 fromL = P - sPos.xyz;
        float dist = length(fromL);
        if (dist >= sPos.w || dist < 1e-3)
            continue;
        float3 dirn = fromL / dist;
        float cs = dot(sd, dirn);
        if (cs < sDir.w)
            continue;
        float cone = cs < sAtt.w ? pow(saturate((cs - sDir.w) / max(sAtt.w - sDir.w, 1e-4)), max(sCol.w, 0.01)) : 1;
        float atten = max(sAtt.x + sAtt.y * dist + sAtt.z * dist * dist, 1e-3);
        float face = saturate((dot(-sd, N) + 0.05) * 4);
        float val = saturate(cone / atten) * (1 - dist / sPos.w) * face * face * (3 - 2 * face);
        float3 Ls = -dirn;
        float NdotLs = saturate(dot(Nh, Ls));
        float3 Hs = normalize(Ls + V);
        float NdotHs = saturate(dot(Nh, Hs)), VdotHs = saturate(dot(V, Hs));
        float dds = NdotHs * NdotHs * (a2 - 1) + 1;
        float Ds = a2 / (3.14159265 * dds * dds);
        float Gs = (NdotLs / (NdotLs * (1 - kq) + kq)) * (NdotV / (NdotV * (1 - kq) + kq));
        float Fs = 0.04 + 0.96 * pow(1 - VdotHs, 5);
        spec += sCol.rgb * val * min(Ds * Gs * Fs / (4 * NdotV), kHighlightPeak);
    }
    float shadow = lit > 1e-4 ? (litSeen + 0.25 * lit) / (1.25 * lit) : 1;
    if ((info.z & 1) == 0)
        spec *= 0.15;  // not a body's lacquer (tyres, carbon, the cockpit): a faint sheen at most
    // A glint, not a blowout: a nearby floodlight on glossy lacquer (gloss 100) reached tens of times the paint's own
    // brightness and the game's bloom spread it into a haze over the car beside the player (run 2, 2026-11-18).
    float peak = max(spec.r, max(spec.g, spec.b));
    spec *= peak > kHighlightMost ? kHighlightMost / peak : 1;

    // The lacquer's reflection of what the eye sees (screen space): the view ray bounced off the paint (its bent normal:
    // the orange peel ripples it) marched through the depth, 16 steps growing to ~17 m, then 4 halvings onto the
    // surface it meets; that point's colour as the scene stood before this pass, weighted by the lacquer's Fresnel
    // (faint face on, strong at grazing angles), less for a rougher paint, faded at the screen's edges and with the
    // distance marched. Nothing where the ray leaves the screen (the game's own reflection, its cube map, stays).
    float3 refl = 0;
    if (mode.z > 0 && (info.z & 1) != 0) {
        // The ray off the smooth normal, the detail's only a little (the orange peel's ripples tipped grazing rays into
        // the panel they left: the panel's own vents and edges reflected beside themselves, in one eye: 2026-11-19).
        float3 Nr = normalize(lerp(N, Nh, 0.3));
        if (dot(Nr, V) < 0)
            Nr = N;
        float3 R = reflect(-V, Nr);
        float fres = 0.04 + 0.96 * pow(1 - saturate(dot(Nh, V)), 5);
        float weight = fres * saturate(1 - rough * 1.6);
        if (weight > 0.002) {
            // Marched through both views (lookup_world: the focus view where the point falls in it, else the eye's),
            // faded at the eye's own screen edge: in the focus view alone each eye's ray stopped at its own box's edge
            // (the boxes sit differently per eye), so the two eyes reflected different things (replay, 2026-11-19).
            float3 O = P + N * 0.03;
            float prev = 0;
            float lq;
            uint sq;
            int2 qi;
            float edge;
            // The start in the eye's own pixels: the skip below the same distance in the focus view as around it (in
            // the asking view's pixels it was fewer metres at the focus density).
            float2 start;
            bool startSeen = pixel_ahead(O, 0, float2(0, 0), region.xy, start);
            // Every tier and both views alike: fewer steps lost a fifth of the hits, a seam at the focus box's edge
            // (and the reflections cost ~0.15 ms).
            [loop] for (int k = 1; k <= 16; ++k) {
                float t = 0.08 * pow(1.4, k);
                float2 own;
                if (startSeen && pixel_ahead(O + R * t, 0, float2(0, 0), region.xy, own) && length(own - start) < 2.5) {
                    prev = t;  // still on the pixel's own surface: no hit there (grazing rays)
                    continue;
                }
                int found = lookup_world(O + R * t, lq, sq, qi, edge);
                if (found < 0)
                    break;
                float behind = found > 0 ? length(O + R * t) - lq : -1;
                // In front of a surface by up to a step's length (the steps grow: a fixed thickness let the far ones
                // jump past a wall), not one far behind it.
                // Not the car part the ray left (its own panel a step on: a doubled image of itself).
                if (behind > 0 && behind < 0.05 + 0.75 * (t - prev) && sq != s) {
                    float lo = prev, hi = t;
                    [unroll] for (int b = 0; b < 4; ++b) {
                        float mid = 0.5 * (lo + hi);
                        int fm = lookup_world(O + R * mid, lq, sq, qi, edge);
                        if (fm > 0 && length(O + R * mid) - lq > 0)
                            hi = mid;
                        else
                            lo = mid;
                    }
                    if (lookup_world(O + R * hi, lq, sq, qi, edge) > 0) {
                        float edgeFade = saturate(edge * 5);
                        float3 seen = sceneTex.Load(int3(qi, 0)).rgb;
                        refl = min(seen, 4) * weight * edgeFade * saturate(1 - (hi / 17.0) * (hi / 17.0)) *
                               other_sees(O + R * hi);
                    }
                    break;
                }
                prev = t;
            }
        }
        float rpeak = max(refl.r, max(refl.g, refl.b));
        refl *= rpeak > kHighlightMost ? kHighlightMost / rpeak : 1;  // a reflected lamp: bright, not a blowout
    }

    float fade = saturate((lp - extra.y) / max(extra.z - extra.y, 1e-3));  // the player's own cockpit left alone
    float3 added = (spec * shade.z + refl) * fade;
    float scaled = lerp(1, shade.x * lerp(1, ao, shade.y) * shadow, fade);
    Out o;
    if (mode.w > 0) {  // dev builds' debug view: what the relight sees per car part, its highlights on top
        uint lights = 0;
        [unroll] for (int li = 0; li < 8; ++li)
            lights += (((li < 4 ? info.x : info.y) >> (8 * (li & 3))) & 0xff) != 0xff ? 1 : 0;
        uint spotsHere = (info.w & 0xff) != 0xff ? 1 : 0;
        // Red: lacquer (bright) or not (dim); green: its floodlights (of 8); blue: the game's normal; white: headlights.
        float3 c = float3((info.z & 1) ? 0.6 : 0.12, lights / 8.0 * 0.6, (info.z & 2) ? 0.5 : 0.0) + spotsHere * 0.3;
        o.add = float4(c * (0.3 + 0.7 * fade) + added, 0);
        o.mul = float4(0, 0, 0, 1);
        return o;
    }
    if (mode2.z > 0) {  // night vision's colour over the relit car: the result made here from the scene's copy
        o.add = float4(night_vision(sceneTex.Load(int3(p, 0)).rgb * scaled + added), 0);
        o.mul = float4(0, 0, 0, 1);
        return o;
    }
    o.add = float4(added, 0);
    o.mul = float4(scaled.xxx, 1);
    return o;
}

// The eye's depth as it is (its eye region), kept for the other eye's pass (other_sees).
float ps_depth(float4 pos : SV_Position) : SV_Target { return load_depth(int2(pos.xy)); }
)";

constexpr unsigned kSlotsMax = 4096;
std::atomic<bool> debugView{false};
std::atomic<bool> bothEyes{true};
std::atomic<bool> bothEyesAbOff{false};
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
    ComPtr<ID3D11Buffer> passCb, lightCb, spotCb, slotBuffer;
    ComPtr<ID3D11ShaderResourceView> slotView;
    std::vector<uint32_t> slotWords;
    unsigned uploadedFirst = 1, uploadedLast = 0;  // the slots the last upload wrote (emptied when not drawn again)
    CarLightPass::Reasons why;
    bool copyFailSaid = false;
    // The scene's render target view and the depth's resource view, kept for the resources they're of.
    ID3D11Resource* rtvFor = nullptr;
    ComPtr<ID3D11Resource> rtvHeld;
    ComPtr<ID3D11RenderTargetView> rtv;
    ID3D11Resource* depthFor = nullptr;
    ComPtr<ID3D11Resource> depthHeld;
    ComPtr<ID3D11ShaderResourceView> depthView;
    // The reflections' copy of the scene (the scene's format and size; made again when they change).
    ComPtr<ID3D11Texture2D> sceneCopy;
    ComPtr<ID3D11ShaderResourceView> sceneCopyView;
    bool depthViewMs = false;
    bool made = false, failed = false;
    // The reflections' check against the other eye (the shader's other_sees): per eye its depth (the eye region, as
    // its last relit pass left it), that pass's projection and its inverse, its view, and which relit run it was; and
    // the eyes' places to each other (eyeTo[e]: eye e's view space to the other's), fixed to the head: taken when the
    // right eye's pass follows the left's of the same frame. The left eye's pass reads the right's of the frame
    // before through it: as far as the head carries both, the same picture (the cockpit, the cars beside).
    struct EyeSeen {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11ShaderResourceView> view;
        unsigned w = 0, h = 0;
        double camera[16] = {}, proj[16] = {}, invProj[16] = {};
        unsigned long long run = 0;  // the relit run that wrote it (0: none)
    } eyeSeen[2];
    ComPtr<ID3D11PixelShader> depthPs[2];  // single-sample depth, multisample
    double eyeTo[2][16] = {};
    bool haveEyeTo = false, eyeToSaid = false;
    int eyeToSaidCount = 0;
    double eyeToSaidApart = 0;
    unsigned long long relitRuns = 0;
    // GPU timing: a ring of query sets, each one run's; read back frames later without waiting.
    struct Timer {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
        unsigned block = 0;  // the A/B block it was issued in
    };
    static constexpr unsigned kBlocks = 32;
    CarLightBlock blocks[kBlocks];
    unsigned block = 0;  // the A/B block now (0: none)
    CarLightBlock* block_of(unsigned serial) {
        CarLightBlock& b = blocks[serial % kBlocks];
        return serial && b.serial == serial ? &b : nullptr;
    }
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
        // The other eye's check is an extra: without its shaders the reflections stay as each eye finds them.
        for (int k = 0; k < 2; ++k)
            if (!compile("ps_depth", "ps_5_0", k ? multi : single, code) ||
                FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &depthPs[k])))
                depthPs[k].Reset();
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
        cb.ByteWidth = 4 * 16 * 3 + 16 * 7 + 4 * 16 * 2 + 16 * 2;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &passCb)))
            return false;
        cb.ByteWidth = 16 * CarLightTable::kMaxLights * 2;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &lightCb)))
            return false;
        cb.ByteWidth = 64 * CarLightTable::kMaxSpots;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &spotCb)))
            return false;
        D3D11_BUFFER_DESC sb{};
        sb.Usage = D3D11_USAGE_DEFAULT;
        sb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sb.ByteWidth = 16 * kSlotsMax;
        // Every slot empty to start (no lights, flags or spotlights): a slot the twins write that no draw recorded.
        std::vector<uint32_t> empty(size_t{4} * kSlotsMax);
        for (size_t i = 0; i < empty.size(); i += 4) {
            empty[i] = empty[i + 1] = empty[i + 3] = 0xffffffffu;
            empty[i + 2] = 0xffff00u;
        }
        const D3D11_SUBRESOURCE_DATA emptyData{empty.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&sb, &emptyData, &slotBuffer)))
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
                       ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* detail,
                       const uint8_t* spotBuffer, size_t spotBytes) {
    Impl& m = *impl_;
    if (failed_ || !context || !scene || !slots || !view.eyeW || !view.eyeH)
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
    const bool anyCar = table.first_slot() <= table.last_slot();
    if (!anyCar && !settings.nightVision)
        return false;  // no car part drawn (night vision's colour still wanted on the rest)
    bool depthMs = false;
    ID3D11RenderTargetView* rtv = m.scene_rtv(scene);
    if (!rtv)
        return false;
    ID3D11ShaderResourceView* depthView = depth ? m.depth_srv(depth, depthMs) : nullptr;

    // The camera: projection = inverse(view) x viewProj (row vectors), and its inverse. The relight needs it, the depth
    // (alike in samples with the slots) and a car part; night vision's colour none of them: without them it runs alone
    // (one eye desaturated and the other not, where a pass missed its camera, stood out far more than a highlight).
    double v[16], vp[16], vi[16], p[16] = {}, pi[16] = {};
    for (int i = 0; i < 16; ++i) {
        v[i] = view.view[i];
        vp[i] = view.viewProj[i];
    }
    bool relight = anyCar && view.camera && depthView && depthMs == slotsMs && invert4(v, vi);
    if (relight) {
        mul4(vi, vp, p);
        relight = invert4(p, pi);
        m.why.singular += relight ? 0 : 1;
    } else {
        m.why.noCar += anyCar ? 0 : 1;
        m.why.noCamera += view.camera ? 0 : 1;
        m.why.noDepth += depthView ? 0 : 1;
        m.why.samples += depthView && depthMs != slotsMs ? 1 : 0;
    }
    ++(relight ? m.why.relit : m.why.alone);
    if (!relight && !settings.nightVision)
        return false;
    if (!relight)
        for (int i = 0; i < 16; ++i)
            p[i] = pi[i] = (i % 5 == 0) ? 1.0 : 0.0;
    // The reflections' check against the other eye: its depth from the relit run right before this one (the left
    // eye's of this frame for the right eye; the right eye's of the frame before for the left), at this eye's size.
    bool keepSeen = false, otherSeen = false;
    m.relitRuns += relight ? 1 : 0;  // every relit run counted: one without the check leaves the next without
    if (relight && settings.reflections && (eye == 0 || eye == 1) && m.depthPs[slotsMs ? 1 : 0] &&
        car_light_both_eyes() && !bothEyesAbOff.load(std::memory_order_relaxed)) {
        keepSeen = true;
        const Impl::EyeSeen& other = m.eyeSeen[1 - eye];
        const bool fresh = other.view && other.run + 1 == m.relitRuns && other.w == view.eyeW && other.h == view.eyeH;
        if (fresh && eye == 1) {  // both of one frame: where the eyes are to each other
            double back[16];
            mul4(vi, other.camera, m.eyeTo[1]);
            // Eyes are centimetres apart: anything else isn't two eyes of one frame (a camera held from before).
            const double* t = m.eyeTo[1] + 12;
            m.haveEyeTo = t[0] * t[0] + t[1] * t[1] + t[2] * t[2] < 0.2 * 0.2 && invert4(m.eyeTo[1], back);
            if (m.haveEyeTo)
                std::memcpy(m.eyeTo[0], back, sizeof(back));
            // Said at first, then whenever it moves by over a centimetre (up to 6 times): two eyes 0 cm apart would be
            // one camera for both (2026-11-19 run: "0.0 cm" said once, at the start).
            const double apart = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (m.haveEyeTo && m.eyeToSaidCount < 6 && (!m.eyeToSaid || std::fabs(apart - m.eyeToSaidApart) > 0.01)) {
                m.eyeToSaid = true;
                ++m.eyeToSaidCount;
                m.eyeToSaidApart = apart;
                simqvfr::logf("lmu car light: lacquer reflections kept where both eyes see the reflected point (the eyes "
                              "%.1f cm apart, each eye's depth %ux%u)",
                              100 * std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]), view.eyeW, view.eyeH);
            }
        }
        otherSeen = fresh && m.haveEyeTo;
    }

    // The slots this pass drew: their lights and flags; the ones the last upload wrote and this frame didn't draw
    // emptied (a slot id the twins still write, no draw recorded, would read its old lists).
    const unsigned first = table.first_slot(), last = (std::min)(table.last_slot(), kSlotsMax - 1);
    if (relight) {
        const unsigned lo = m.uploadedFirst <= m.uploadedLast ? (std::min)(first, m.uploadedFirst) : first;
        const unsigned hi = m.uploadedFirst <= m.uploadedLast ? (std::max)(last, m.uploadedLast) : last;
        m.slotWords.resize(size_t{4} * (hi - lo + 1));
        m.why.slots = m.why.lit = m.why.paint = m.why.normal = m.why.spotted = 0;
        for (unsigned slot = lo; slot <= hi; ++slot) {
            uint32_t w[4];
            table.packed(slot, w);
            std::memcpy(&m.slotWords[size_t{4} * (slot - lo)], w, sizeof(w));
            if (slot >= first && slot <= last && (w[0] != 0xffffffffu || (w[2] & 0xff) || w[3] != 0xffffffffu ||
                                                  table.slot_drawn(slot))) {
                ++m.why.slots;
                m.why.lit += (w[0] & 0xff) != 0xff ? 1 : 0;
                m.why.paint += (w[2] & 1) ? 1 : 0;
                m.why.normal += (w[2] & 2) ? 1 : 0;
                m.why.spotted += (w[3] & 0xff) != 0xff ? 1 : 0;
            }
        }
        const D3D11_BOX box{16 * lo, 0, 0, 16 * (hi + 1), 1, 1};
        context->UpdateSubresource(m.slotBuffer.Get(), 0, &box, m.slotWords.data(), 0, 0);
        m.uploadedFirst = first;
        m.uploadedLast = last;
    }

    Impl::Timer& timer = m.timers[m.nextTimer];
    const bool timed = !timer.pending;
    if (timed) {
        timer.block = m.block;
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
    ID3D11ShaderResourceView* none[7] = {};
    context->PSSetShaderResources(0, 7, none);
    const LONG right = static_cast<LONG>((std::min)(sd.Width, view.eyeW + view.focusW));
    const LONG bottom = static_cast<LONG>((std::min)(sd.Height, (std::max)(view.eyeH, view.focusH)));
    // The reflections and night vision read the scene as it stood: the eye's and focus view's regions copied first.
    ID3D11ShaderResourceView* sceneView = nullptr;
    if ((settings.reflections || settings.nightVision) && sceneTex && right > 0 && bottom > 0) {
        D3D11_TEXTURE2D_DESC cd{};
        if (m.sceneCopy)
            m.sceneCopy->GetDesc(&cd);
        if (!m.sceneCopy || cd.Width != sd.Width || cd.Height != sd.Height || cd.Format != sd.Format) {
            m.sceneCopy.Reset();
            m.sceneCopyView.Reset();
            D3D11_TEXTURE2D_DESC c = sd;
            c.MipLevels = 1;
            c.ArraySize = 1;
            c.SampleDesc = {1, 0};
            c.Usage = D3D11_USAGE_DEFAULT;
            c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            c.CPUAccessFlags = 0;
            c.MiscFlags = 0;
            D3D11_SHADER_RESOURCE_VIEW_DESC copyView{};
            copyView.Format = rtv_format(sd.Format);
            copyView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            copyView.Texture2D.MipLevels = 1;
            if (FAILED(m.device->CreateTexture2D(&c, nullptr, &m.sceneCopy)) ||
                FAILED(m.device->CreateShaderResourceView(m.sceneCopy.Get(), &copyView, &m.sceneCopyView))) {
                m.sceneCopy.Reset();
                m.sceneCopyView.Reset();
                if (!m.copyFailSaid) {
                    m.copyFailSaid = true;
                    simqvfr::logf("lmu car light: the scene's copy (%ux%u, format %d) couldn't be made: no lacquer "
                                  "reflections or night vision colour",
                                  sd.Width, sd.Height, static_cast<int>(sd.Format));
                }
            }
        }
        if (m.sceneCopy) {
            const D3D11_BOX box{0, 0, 0, static_cast<UINT>(right), static_cast<UINT>(bottom), 1};
            context->CopySubresourceRegion(m.sceneCopy.Get(), 0, 0, 0, 0, scene, 0, &box);
            sceneView = m.sceneCopyView.Get();
        }
    }
    // Without the copy the modes that read it are off (night vision's output from a missing copy is black).
    const bool copied = sceneView != nullptr;
    if (!relight && !copied) {
        if (timed) {  // ended, not counted (no run: its time would skew the averages)
            context->End(timer.end.Get());
            context->End(timer.disjoint.Get());
        }
        return false;
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
        float cb[4 * 16 * 3 / 4 + 28 + 40] = {};
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
        rest[16] = s.shadows ? 1.0f : 0.0f;
        rest[17] = s.detail ? 1.0f : 0.0f;
        rest[18] = s.reflections && copied ? 1.0f : 0.0f;
        rest[19] = car_light_debug_view() ? 1.0f : 0.0f;
        // The headlights: the frame's spotlights the car parts were handed, from the spotlights' buffer as written.
        unsigned spots = 0;
        if (s.headlights && spotBuffer) {
            float spotData[CarLightTable::kMaxSpots * 16] = {};
            for (uint16_t index : table.spots()) {
                if (spots >= CarLightTable::kMaxSpots)
                    break;
                float* d = spotData + spots * 16;
                if (size_t{index} * 64 + 64 > spotBytes) {  // past the buffer: kept in place, range 0 (unused)
                    ++spots;
                    continue;
                }
                std::memcpy(d, spotBuffer + size_t{index} * 64, 64);
                bool usable = true;
                for (int k = 0; k < 16; ++k)
                    usable = usable && std::isfinite(d[k]);
                // World to view (row vectors): the position as a point, the direction as a direction.
                const float* vm = view.view;
                const float px = d[0], py = d[1], pz = d[2], dx = d[4], dy = d[5], dz = d[6];
                for (int j = 0; j < 3; ++j) {
                    d[j] = px * vm[j] + py * vm[4 + j] + pz * vm[8 + j] + vm[12 + j];
                    d[4 + j] = dx * vm[j] + dy * vm[4 + j] + dz * vm[8 + j];
                }
                const float len = std::sqrt(d[4] * d[4] + d[5] * d[5] + d[6] * d[6]);
                usable = usable && len > 1e-4f;
                if (usable) {
                    d[4] /= len;
                    d[5] /= len;
                    d[6] /= len;
                }
                // Kept in place either way (the slots' indices point at spots()' order); an unusable one has range 0.
                if (!usable)
                    d[3] = 0;
                ++spots;
            }
            context->UpdateSubresource(m.spotCb.Get(), 0, nullptr, spotData, 0, 0);
        }
        rest[20] = static_cast<float>(spots);
        rest[21] = s.ground ? 1.0f : 0.0f;
        rest[22] = s.nightVision && copied ? 1.0f : 0.0f;
        rest[66] = static_cast<float>(s.quality);                        // the focus view's tier
        rest[67] = static_cast<float>(s.quality > 0 ? s.quality - 1 : 0);  // the periphery a tier below
        rest[23] = relight ? 0.0f : 1.0f;  // night vision's colour alone
        int top[4] = {-1, -1, -1, -1};
        if (relight)  // (night vision's colour alone doesn't fix the frame's choice: none of its parts were drawn yet)
            table.top_lights(top);
        for (int k = 0; k < 4; ++k)
            rest[24 + k] = static_cast<float>(top[k]);
        if (otherSeen) {  // the reflections' check against the other eye (other_sees)
            const Impl::EyeSeen& other = m.eyeSeen[1 - eye];
            double clip[16];
            mul4(m.eyeTo[eye], other.proj, clip);
            for (int i = 0; i < 16; ++i) {
                rest[28 + i] = static_cast<float>(clip[i]);
                rest[44 + i] = static_cast<float>(other.invProj[i]);
            }
            // The other eye's place here: its view space's origin through its way to this eye's.
            for (int i = 0; i < 3; ++i)
                rest[60 + i] = static_cast<float>(m.eyeTo[1 - eye][12 + i]);
            rest[63] = 1.0f;
            rest[64] = static_cast<float>(other.w);
            rest[65] = static_cast<float>(other.h);
        }
        context->UpdateSubresource(m.passCb.Get(), 0, nullptr, cb, 0, 0);
    }

    context->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vpPort{0, 0, static_cast<float>(sd.Width), static_cast<float>(sd.Height), 0, 1};
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
    ID3D11Buffer* cbs[3] = {m.passCb.Get(), m.lightCb.Get(), m.spotCb.Get()};
    context->PSSetConstantBuffers(0, 3, cbs);
    ID3D11ShaderResourceView* views[7] = {depthView, slots, m.slotView.Get(), normals, detail, sceneView,
                                          otherSeen ? m.eyeSeen[1 - eye].view.Get() : nullptr};
    context->PSSetShaderResources(0, 7, views);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 7, none);
    if (keepSeen) {  // this eye's depth and camera, for the other eye's next pass
        Impl::EyeSeen& own = m.eyeSeen[eye];
        if (!own.texture || own.w != view.eyeW || own.h != view.eyeH) {
            own = Impl::EyeSeen{};
            const D3D11_TEXTURE2D_DESC td = {view.eyeW, view.eyeH, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                             D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
            if (SUCCEEDED(m.device->CreateTexture2D(&td, nullptr, &own.texture)) &&
                SUCCEEDED(m.device->CreateRenderTargetView(own.texture.Get(), nullptr, &own.target)) &&
                SUCCEEDED(m.device->CreateShaderResourceView(own.texture.Get(), nullptr, &own.view))) {
                own.w = view.eyeW;
                own.h = view.eyeH;
            } else {
                own = Impl::EyeSeen{};
            }
        }
        if (own.view) {
            ID3D11RenderTargetView* target = own.target.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            const D3D11_VIEWPORT eyePort{0, 0, static_cast<float>(own.w), static_cast<float>(own.h), 0, 1};
            const D3D11_RECT eyeRect{0, 0, static_cast<LONG>(own.w), static_cast<LONG>(own.h)};
            context->RSSetViewports(1, &eyePort);
            context->RSSetScissorRects(1, &eyeRect);
            context->OMSetBlendState(nullptr, factor, 0xffffffffu);
            context->PSSetShader(m.depthPs[slotsMs ? 1 : 0].Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, &depthView);
            context->Draw(3, 0);
            context->PSSetShaderResources(0, 1, none);
            ID3D11RenderTargetView* noTarget = nullptr;
            context->OMSetRenderTargets(1, &noTarget, nullptr);
            for (int i = 0; i < 16; ++i) {
                own.camera[i] = v[i];
                own.proj[i] = p[i];
                own.invProj[i] = pi[i];
            }
            own.run = m.relitRuns;
        }
    }
    if (timed) {
        context->End(timer.end.Get());
        context->End(timer.disjoint.Get());
        timer.pending = true;
        m.nextTimer = (m.nextTimer + 1) % kTimerSlots;
    }
    ++m.runs;
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    const double cpu = static_cast<double>(t1.QuadPart - t0.QuadPart) / m.qpcPerMs;
    m.cpuSum += cpu;
    if (CarLightBlock* b = m.block_of(m.block)) {
        ++b->runs;
        b->cpuSum += cpu;
    }
    return true;
}

void car_light_set_debug_view(bool on) { debugView.store(on, std::memory_order_relaxed); }
void car_light_set_both_eyes(bool on) { bothEyes.store(on, std::memory_order_relaxed); }
void car_light_ab_both_eyes_off(bool off) { bothEyesAbOff.store(off, std::memory_order_relaxed); }
bool car_light_both_eyes() { return bothEyes.load(std::memory_order_relaxed); }
bool car_light_debug_view() { return debugView.load(std::memory_order_relaxed); }

CarLightPass::Reasons CarLightPass::take_reasons() {
    Reasons r = impl_->why;
    impl_->why.relit = impl_->why.alone = impl_->why.noCar = impl_->why.noCamera = impl_->why.noDepth =
        impl_->why.samples = impl_->why.singular = 0;
    return r;
}

void CarLightPass::add_collect_ms(double ms) {
    impl_->collectSum += ms;
    if (CarLightBlock* b = impl_->block_of(impl_->block))
        b->cpuSum += ms;
}

namespace {
bool& car_light_switch(CarLightSettings& c, int s) {
    switch (s) {
    case 0: return c.shadows;
    case 1: return c.detail;
    case 2: return c.reflections;
    case 3: return c.headlights;
    case 4: return c.ground;
    default: return c.nightVision;
    }
}
const char* const kCarLightSwitchNames[8] = {"floodlight shadows",   "paint detail",          "lacquer reflections",
                                             "headlight highlights", "shadows on the ground", "night vision color",
                                             "every switch (the base relight)",
                                             "the reflections' both-eyes check"};
}  // namespace

bool CarLightAb::start(const CarLightSettings& light) {
    *this = CarLightAb{};
    if (!light.on)
        return false;
    CarLightSettings c = light;
    int on = 0;
    for (int s = 0; s < 6; ++s)
        if (car_light_switch(c, s)) {
            switches_[variants_] = s;
            names_[variants_++] = kCarLightSwitchNames[s];
            ++on;
        }
    if (on > 1) {
        switches_[variants_] = 6;
        names_[variants_++] = kCarLightSwitchNames[6];
    }
    if (light.reflections && car_light_both_eyes()) {  // the reflections' check against the other eye, its own cost
        switches_[variants_] = 7;
        names_[variants_++] = kCarLightSwitchNames[7];
    }
    if (!variants_)
        return false;
    active_ = true;
    serial_ = 1;
    kinds_[serial_ % kSeen] = -1;
    tainted_[serial_ % kSeen] = false;
    return true;
}

void CarLightAb::apply(int variant, CarLightSettings& light) const {
    if (variant < 0)
        return;
    if (switches_[variant] == 7)
        return;  // the both-eyes check: both_eyes_off_now(), not a setting
    if (switches_[variant] == 6) {
        for (int s = 0; s < 6; ++s)
            car_light_switch(light, s) = false;
    } else {
        car_light_switch(light, switches_[variant]) = false;
    }
}

unsigned CarLightAb::frame(CarLightSettings& light, unsigned& ended) {
    ended = 0;
    if (!active_)
        return 0;
    if (inBlock_ == kFrames) {  // the next block
        inBlock_ = 0;
        ++serial_;
        const unsigned inRound = (serial_ - 1) % (2 * static_cast<unsigned>(variants_));
        kinds_[serial_ % kSeen] = inRound % 2 ? static_cast<int>(inRound / 2) : -1;
        tainted_[serial_ % kSeen] = false;
        if (serial_ > 2)
            ended = serial_ - 2;  // ended a block's length ago: its GPU times are back
    }
    ++inBlock_;
    if (++frames_ >= kMaxFrames)
        active_ = false;
    apply(kinds_[serial_ % kSeen], light);
    return serial_;
}

void CarLightAb::add(const CarLightBlock& b) {
    if (!b.serial)
        return;
    Seen& s = seen_[b.serial % kSeen];
    s.serial = b.serial;
    s.kind = kinds_[b.serial % kSeen];
    s.gpu = b.gpu_ms();
    s.cpu = b.cpu_ms();
    // Counted when the pass ran on every frame (both eyes) and was timed: with the cars out of view a block says
    // nothing either way.
    s.ok = !tainted_[b.serial % kSeen] && b.frames && b.gpuRuns && b.runs >= 2 * b.frames;
    if (s.kind == -1 && s.ok)
        all_.add(s.gpu, s.cpu);
    // An A block closes the pair before it: A (serial - 2), V (serial - 1), A (this one).
    if (s.kind != -1 || b.serial < 3)
        return;
    const Seen& v = seen_[(b.serial - 1) % kSeen];
    const Seen& a = seen_[(b.serial - 2) % kSeen];
    if (v.serial != b.serial - 1 || a.serial != b.serial - 2 || v.kind < 0 || a.kind != -1 || !v.ok || !a.ok || !s.ok)
        return;
    pairs_[v.kind].add((a.gpu + s.gpu) / 2 - v.gpu, (a.cpu + s.cpu) / 2 - v.cpu);
}

std::vector<std::string> CarLightAb::report() const {
    std::vector<std::string> lines;
    const auto err = [](double sum, double sum2, unsigned n) {
        if (n < 2)
            return 0.0;
        const double mean = sum / n, var = (std::max)(0.0, (sum2 - n * mean * mean) / (n - 1));
        return std::sqrt(var / n);
    };
    char text[256];
    if (all_.n) {
        std::snprintf(text, sizeof(text), "your settings: %.3f ms GPU, %.3f ms CPU per frame (%u blocks)",
                      all_.gpu / all_.n, all_.cpu / all_.n, all_.n);
        lines.emplace_back(text);
    }
    for (int v = 0; v < variants_; ++v) {
        const Sum& p = pairs_[v];
        if (!p.n)
            std::snprintf(text, sizeof(text), "%s: no pairs yet (cars out of view?)", names_[v]);
        else
            std::snprintf(text, sizeof(text), "%s: %.3f ms GPU (+-%.3f), %.3f ms CPU (+-%.3f), %u pairs", names_[v],
                          p.gpu / p.n, err(p.gpu, p.gpu2, p.n), p.cpu / p.n, err(p.cpu, p.cpu2, p.n), p.n);
        lines.emplace_back(text);
    }
    return lines;
}

void CarLightPass::set_block(unsigned serial) {
    Impl& m = *impl_;
    if (serial == m.block)
        return;
    m.block = serial;
    if (serial) {
        m.blocks[serial % Impl::kBlocks] = CarLightBlock{};
        m.blocks[serial % Impl::kBlocks].serial = serial;
    }
}

bool CarLightPass::block(unsigned serial, CarLightBlock& out) const {
    const CarLightBlock& b = impl_->blocks[serial % Impl::kBlocks];
    if (!serial || b.serial != serial)
        return false;
    out = b;
    return true;
}

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
        const double gpu = static_cast<double>(e - b) * 1000.0 / static_cast<double>(dj.Frequency);
        m.gpuSum += gpu;
        ++m.gpuRuns;
        if (CarLightBlock* blk = m.block_of(t.block)) {
            blk->gpuSum += gpu;
            ++blk->gpuRuns;
        }
    }
    ++m.frames;
    if (CarLightBlock* blk = m.block_of(m.block))
        ++blk->frames;
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

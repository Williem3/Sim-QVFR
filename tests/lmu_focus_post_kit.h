// Shared by the tests of LMU MVR's focus post (tests/lmu_focus_post*_test.cpp, lmu_mvr_tracker_test.cpp): the
// headers, the fake two-view switch, the tracker's and the panel's callbacks, the stand-in shaders (YEBIS's copy and
// composite, the scene, the glare, the game's final blit), textures, constants and readbacks, and Kit (an eye pass's
// post-processing). Everything in an anonymous namespace, as test_util.h's. The device, the targets and the eye pass
// itself: tests/lmu_focus_post_rig.h.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "games/lmu/mvr_pass.h"
#include "games/lmu/mvr_probe.h"
#include "games/lmu/yebis_shaders.h"
#include "probe/d3d11_capture.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;

namespace {

int twoViewsOn = 0, twoViewsCalls = 0;
inline int fake_two_views(ID3D11DeviceContext*, bool on) {
    ++twoViewsCalls;
    twoViewsOn = on ? 1 : 0;
    return 0;
}

inline ID3D11VertexShader* bound_vs(ID3D11DeviceContext* context) {
    ID3D11VertexShader* vs = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    if (vs)
        vs->Release();
    return vs;
}

// R4b-2: the tracker's calls when it drives the passes (as qvfr_frame's pass_begin / pass_end would).
const lmu::MvrPassView* driveViews[2] = {};
std::string driveLog;
bool driveFinals = false;  // both eyes' focus finals there at pass 1's end (as qvfr_frame's after_two_passes reads them)
inline void drive_begin(int pass) {
    driveLog += " b" + std::to_string(pass);
    lmu::mvr_pass_begin(pass, driveViews[pass & 1], false);
}
inline void drive_end(int pass) {
    driveLog += " e" + std::to_string(pass);
    lmu::mvr_pass_end(pass);
    unsigned x = 0, y = 0, w = 0, h = 0;
    if (pass == 1)
        driveFinals = lmu::mvr_focus_final(0, x, y, w, h) && lmu::mvr_focus_final(1, x, y, w, h);
}

// R6b: the panel callback (as qvfr_frame's panel_draw): logs before/after; after, the panel again into panelTarget.
std::string panelLog;
ID3D11RenderTargetView* panelTarget = nullptr;
unsigned panelW = 0, panelH = 0;
inline void panel_cb(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start) {
    panelLog += after ? " a" : " b";
    if (after && panelTarget)
        lmu::mvr_redraw_panel(context, panelTarget, panelW, panelH, count, start);
}

inline bool same_vp(const D3D11_VIEWPORT& v, float x, float y, float w, float h) {
    return std::fabs(v.TopLeftX - x) < 1e-3f && std::fabs(v.TopLeftY - y) < 1e-3f && std::fabs(v.Width - w) < 1e-3f &&
           std::fabs(v.Height - h) < 1e-3f;
}

inline ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "t", nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
        std::printf("     %s: %s\n", target, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

inline ComPtr<ID3D11VertexShader> vs(ID3D11Device* device, const char* source) {
    ComPtr<ID3D11VertexShader> s;
    if (ComPtr<ID3DBlob> code = compile(source, "vs_5_0"))
        device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s);
    return s;
}

inline ComPtr<ID3D11PixelShader> ps(ID3D11Device* device, const char* source, lmu::YebisRole role = lmu::YebisRole::none) {
    ComPtr<ID3D11PixelShader> s;
    if (ComPtr<ID3DBlob> code = compile(source, "ps_5_0"))
        device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s);
    if (s && role != lmu::YebisRole::none) {  // as mvr_probe.cpp marks the game's
        const UINT r = static_cast<UINT>(role);
        s->SetPrivateData(lmu::kYebisRoleGuid, sizeof(r), &r);
    }
    return s;
}

// A full-screen triangle over the viewport.
constexpr char kPlainVs[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0, 1);
})";
// YEBIS's vertex shaders' outputs: five TEXCOORDs then the position; the UVs over `rect` (offset, size) of the source.
constexpr char kYebisVs[] = R"(
cbuffer R : register(b1) { float4 rect; };
struct O { float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
           float4 t4 : TEXCOORD4; float4 pos : SV_Position; };
O main(uint id : SV_VertexID) {
    float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);
    float2 uv = rect.xy + float2((p.x + 1) * 0.5, (1 - p.y) * 0.5) * rect.zw;
    O o; o.t0 = float4(uv, 0, 0); o.t1 = o.t0; o.t2 = 0; o.t3 = float4(0.5, 0.5, 0, 0); o.t4 = 0; o.pos = float4(p, 0, 1);
    return o;
})";
// The scene: HDR, the same picture every 100 pixels across (the eye at 0, its focus view beside it at 100).
constexpr char kScenePs[] = R"(
float4 main(float4 p : SV_Position) : SV_Target {
    float x = (uint)p.x % 100u, y = (uint)p.y;  // integers: fmod is frac(x / y) * y, not exact
    return float4(x / 40.0, y / 64.0, frac(x * 0.13 + y * 0.07), 1);
})";
// The glare: its UV (a gradient across the frame).
constexpr char kGlarePs[] = R"(
cbuffer S : register(b0) { float4 size; };
float4 main(float4 p : SV_Position) : SV_Target { return float4(p.xy / size.xy, 0.25, 1); }
)";
// Stand-ins for YEBIS's copy (texture x afRGBA_Modulate[0]) and composite.
constexpr char kCopyPs[] = R"(
cbuffer G : register(b0) { float4 c[15]; };
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
float4 main(float4 v0 : TEXCOORD0) : SV_Target { return t0.Sample(s0, v0.xy) * c[14]; }
)";
constexpr char kCompositePs[] = R"(
cbuffer G : register(b0) { float4 c[21]; };
Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2);
SamplerState s0 : register(s0); SamplerState s1 : register(s1); SamplerState s2 : register(s2);
float4 main(float4 v0 : TEXCOORD0, float4 v1 : TEXCOORD1, float4 v2 : TEXCOORD2, float4 v3 : TEXCOORD3,
            float4 v4 : TEXCOORD4) : SV_Target {
    float3 scene = t0.Sample(s0, v0.xy).rgb, glare = t1.Sample(s1, v1.xy).rgb;
    float e = t2.Sample(s2, v3.xy).r;
    float3 col = scene * e * c[20].x + glare * c[20].y;
    return float4(pow(saturate(col / (1 + col)), 1 / 2.2), saturate(dot(scene, 0.333)));
})";

// The game's final blit after YEBIS: a pass-through vertex shader (clip position, COLOR0, TEXCOORD0 from the vertices:
// the input signature of LMU's, VS 458c0204) and t0 x COLOR0 (PS 0a09af97).
constexpr char kBlitVs[] = R"(
struct I { float4 pos : POSITION; float4 normal : NORMAL; float4 col : COLOR0; float4 col1 : COLOR1; float2 t0 : TEXCOORD0;
           float2 t1 : TEXCOORD1; float2 t2 : TEXCOORD2; float2 t3 : TEXCOORD3; float2 t4 : TEXCOORD4;
           float4 bi : BLENDINDICES; float4 bw : BLENDWEIGHT; };
struct O { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };
O main(I i) { O o; o.pos = i.pos; o.col = i.col; o.uv = i.t0 + 0 * (i.normal.xy + i.col1.xy + i.t1 + i.t2 + i.t3 + i.t4 + i.bi.xy + i.bw.xy);
              return o; }
)";
constexpr char kBlitPs[] = R"(
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
float4 main(float4 pos : SV_Position, float4 col : COLOR0, float2 uv : TEXCOORD0) : SV_Target {
    return t0.Sample(s0, uv) * col;
}
)";
struct BlitVertex {
    float pos[4], normal[4], col[4], col1[4], t[5][2], blendIndices[4], blendWeight[4];
};
constexpr D3D11_INPUT_ELEMENT_DESC kBlitLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 72, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, 88, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT, 0, 96, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 104, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 120, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

inline ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT samples = 1,
                                       UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                       const void* data = nullptr, UINT pitch = 0) {
    const D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, format, {samples, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    const D3D11_SUBRESOURCE_DATA init = {data, pitch, 0};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&d, data ? &init : nullptr, &t);
    return t;
}

// A dynamic constant buffer, written as the game does (map, discard).
inline ComPtr<ID3D11Buffer> constants(ID3D11Device* device, UINT bytes) {
    const D3D11_BUFFER_DESC d = {bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> b;
    device->CreateBuffer(&d, nullptr, &b);
    return b;
}

inline void write(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, data, bytes);
        context->Unmap(buffer, 0);
    }
}

inline float half_to_float(uint16_t v) {
    const uint32_t sign = (v >> 15) & 1, exponent = (v >> 10) & 0x1f, mantissa = v & 0x3ff;
    float f = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                            : std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -f : f;
}

// A texture's w x h top-left as RGBA bytes: RGBA8 as it is, RGBA16F rounded to 8 bits (as the game's final blit makes it).
inline std::vector<uint8_t> read_rgba8(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* t, unsigned w,
                                       unsigned h) {
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint8_t> out;
    if (FAILED(device->CreateTexture2D(&s, nullptr, &staging)))
        return out;
    context->CopyResource(staging.Get(), t);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return out;
    out.resize(static_cast<size_t>(w) * h * 4);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            for (unsigned c = 0; c < 4; ++c) {
                const auto* row = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
                uint8_t v;
                if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    v = row[x * 4 + c];
                } else {
                    uint16_t half;
                    std::memcpy(&half, row + x * 8 + c * 2, 2);
                    const float f = half_to_float(half);
                    v = static_cast<uint8_t>(std::lround(255.0f * (f < 0 ? 0 : f > 1 ? 1 : f)));
                }
                out[(static_cast<size_t>(y) * w + x) * 4 + c] = v;
            }
    context->Unmap(staging.Get(), 0);
    return out;
}

template <class T>
T* bound(T* p) {  // a Get* result as a raw pointer (released: the pipeline holds it)
    if (p)
        p->Release();
    return p;
}

// The pixel shaders an eye pass's post-processing runs and the constants it gives them: for the copy, then for the
// downsample right after (the game rewrites its buffer), then for the composite.
struct Kit {
    ID3D11PixelShader* copy = nullptr;
    ID3D11PixelShader* composite = nullptr;
    ID3D11Buffer* copyCb = nullptr;
    ID3D11Buffer* compositeCb = nullptr;
    const void* copyData = nullptr;
    size_t copyBytes = 0;
    const void* downData = nullptr;
    size_t downBytes = 0;
    const void* compositeData = nullptr;
    size_t compositeBytes = 0;
    ID3D11VertexShader* blitVs = nullptr;  // the final blit's
    ID3D11PixelShader* blitPs = nullptr;
    // A stage between the resolve and YEBIS (reflections high, 2026-11-11): 0 none; 1 its image over the whole target;
    // 2 over the eye only. YEBIS's chain then starts from its image (no copy of the resolve).
    int stage = 0;
    bool fsr = false;  // FSR on: no final blit, the eye's image written whole from the scene's target
    bool resolveTwice = false;  // a second multisample resolve right after the first (not a new post)
};

inline std::vector<uint8_t> file_bytes(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
        uint8_t buffer[4096];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
            out.insert(out.end(), buffer, buffer + n);
        std::fclose(f);
    }
    return out;
}

}  // namespace

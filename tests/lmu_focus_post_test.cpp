// Offline check of LMU MVR's focus post (games/lmu/focus_post.h, mvr_pass.h mvr_focus_replay) on a WARP device. An eye
// pass as LMU's runs on the hooked immediate context: the scene (with the focus view beside the eye) resolved, then
// YEBIS-like draws with stand-ins for its copy and composite pixel shaders (same inputs: the copy's texture at TEXCOORD0
// times a constant; the composite's scene copy, glare and 1x1 exposure at TEXCOORD0, 1 and 3), marked with their role as
// the probe marks the game's. The scene's focus region holds the same picture as the eye's top-left, and the focus box
// is exactly that crop, so the focus post's image must equal the eye's own composite over it: the copy's region, the
// constants as bound for the eye (the game rewrites its buffer right after), the glare resampled through the box and
// the exposure all show there. And the game's state after each step as it set it, nothing replayed outside passes 0-1.
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

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

int twoViewsOn = 0, twoViewsCalls = 0;
int fake_two_views(ID3D11DeviceContext*, bool on) {
    ++twoViewsCalls;
    twoViewsOn = on ? 1 : 0;
    return 0;
}

ID3D11VertexShader* bound_vs(ID3D11DeviceContext* context) {
    ID3D11VertexShader* vs = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    if (vs)
        vs->Release();
    return vs;
}

// R4b-2: the tracker's calls when it drives the passes (as four_passes' pass_begin / pass_end would).
const lmu::MvrPassView* driveViews[2] = {};
std::string driveLog;
bool driveFinals = false;  // both eyes' focus finals there at pass 1's end (as four_passes' after_two_passes reads them)
void drive_begin(int pass) {
    driveLog += " b" + std::to_string(pass);
    lmu::mvr_pass_begin(pass, driveViews[pass & 1], false);
}
void drive_end(int pass) {
    driveLog += " e" + std::to_string(pass);
    lmu::mvr_pass_end(pass);
    unsigned x = 0, y = 0, w = 0, h = 0;
    if (pass == 1)
        driveFinals = lmu::mvr_focus_final(0, x, y, w, h) && lmu::mvr_focus_final(1, x, y, w, h);
}

// R6b: the panel callback (as four_passes' panel_draw): logs before/after; after, the panel again into panelTarget.
std::string panelLog;
ID3D11RenderTargetView* panelTarget = nullptr;
unsigned panelW = 0, panelH = 0;
void panel_cb(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start) {
    panelLog += after ? " a" : " b";
    if (after && panelTarget)
        lmu::mvr_redraw_panel(context, panelTarget, panelW, panelH, count, start);
}

bool same_vp(const D3D11_VIEWPORT& v, float x, float y, float w, float h) {
    return std::fabs(v.TopLeftX - x) < 1e-3f && std::fabs(v.TopLeftY - y) < 1e-3f && std::fabs(v.Width - w) < 1e-3f &&
           std::fabs(v.Height - h) < 1e-3f;
}

ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "t", nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
        std::printf("     %s: %s\n", target, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

ComPtr<ID3D11VertexShader> vs(ID3D11Device* device, const char* source) {
    ComPtr<ID3D11VertexShader> s;
    if (ComPtr<ID3DBlob> code = compile(source, "vs_5_0"))
        device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s);
    return s;
}

ComPtr<ID3D11PixelShader> ps(ID3D11Device* device, const char* source, lmu::YebisRole role = lmu::YebisRole::none) {
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

ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT samples = 1,
                                UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                const void* data = nullptr, UINT pitch = 0) {
    const D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, format, {samples, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    const D3D11_SUBRESOURCE_DATA init = {data, pitch, 0};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&d, data ? &init : nullptr, &t);
    return t;
}

// A dynamic constant buffer, written as the game does (map, discard).
ComPtr<ID3D11Buffer> constants(ID3D11Device* device, UINT bytes) {
    const D3D11_BUFFER_DESC d = {bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> b;
    device->CreateBuffer(&d, nullptr, &b);
    return b;
}

void write(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, data, bytes);
        context->Unmap(buffer, 0);
    }
}

float half_to_float(uint16_t v) {
    const uint32_t sign = (v >> 15) & 1, exponent = (v >> 10) & 0x1f, mantissa = v & 0x3ff;
    float f = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                            : std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -f : f;
}

// A texture's w x h top-left as RGBA bytes: RGBA8 as it is, RGBA16F rounded to 8 bits (as the game's final blit makes it).
std::vector<uint8_t> read_rgba8(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* t, unsigned w,
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
};

std::vector<uint8_t> file_bytes(const std::string& path) {
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

// Optional: a folder with YEBIS's own copy and composite (copy-ps.dxbc, composite-ps.dxbc and their constant buffers
// copy-ps-cb0.bin, composite-ps-cb0.bin, from a capture: tools/rdc/shader_bytes.py; the game's code, kept local). Then
// the same pass runs with them, under the debug layer where it is installed (linkage between our vertex shaders and
// theirs checked).
int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    const std::string folder = argc > 1 ? argv[1] : "";
    char what[256];
    {  // The roles by checksum (the probe's marking at creation).
        unsigned char code[24] = {'D', 'X', 'B', 'C'};
        std::memcpy(code + 4, lmu::kYebisCompositePs, 16);
        const lmu::YebisRole composite = lmu::yebis_role(code, sizeof(code));
        std::memcpy(code + 4, lmu::kYebisCopyPs, 16);
        const lmu::YebisRole copy = lmu::yebis_role(code, sizeof(code));
        code[4] ^= 1;
        check(composite == lmu::YebisRole::composite && copy == lmu::YebisRole::copy &&
                  lmu::yebis_role(code, sizeof(code)) == lmu::YebisRole::none && lmu::yebis_role(code, 12) == lmu::YebisRole::none,
              "roles by DXBC checksum: YEBIS's copy and composite known, any other shader none");
    }
    {  // R6: the vertex shaders' roles (the probe's marking at creation).
        unsigned char code[24] = {'D', 'X', 'B', 'C'};
        std::memcpy(code + 4, lmu::kLensMaskVs, 16);
        const lmu::VsRole mask = lmu::vs_role(code, sizeof(code));
        std::memcpy(code + 4, lmu::kVruiPanelVs, 16);
        const lmu::VsRole panel = lmu::vs_role(code, sizeof(code));
        code[5] ^= 1;
        check(mask == lmu::VsRole::lensMask && panel == lmu::VsRole::vruiPanel &&
                  lmu::vs_role(code, sizeof(code)) == lmu::VsRole::none,
              "vertex shader roles by DXBC checksum: the lens mask's and the VR UI panel's known, any other none");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const bool debug = !folder.empty() &&
                       SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                                   nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    if (!debug && FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &device, nullptr, &context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    ComPtr<ID3D11InfoQueue> infoQueue;
    if (debug)
        device.As(&infoQueue);
    if (!folder.empty())
        std::printf("     the debug layer: %s\n", infoQueue ? "on" : "not installed (no linkage check)");
    lmu::mvr_set_two_views(&fake_two_views);
    lmu::mvr_focus_replay(true);  // before the install: DrawIndexed hooked too
    check(lmu::mvr_pass_install(device.Get()) && lmu::mvr_pass_stats().contextEntries == 15,
          "the immediate context hooked, the four draws and CopySubresourceRegion among its entries (the focus post on)");

    // The targets: the eye 100x80 at the top-left of 256x128, its focus view 60x50 beside it at (100, 0).
    constexpr unsigned W = 256, H = 128, eyeW = 100, eyeH = 80, focusW = 60, focusH = 50, glareW = 102, glareH = 51;
    ComPtr<ID3D11Texture2D> msaa = texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, D3D11_BIND_RENDER_TARGET),
                            depth = texture(device.Get(), W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, D3D11_BIND_DEPTH_STENCIL),
                            scene = texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT),
                            sceneCopy = texture(device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT),
                            half = texture(device.Get(), W / 2, H / 2, DXGI_FORMAT_R16G16B16A16_FLOAT),
                            glare = texture(device.Get(), glareW, glareH, DXGI_FORMAT_R11G11B10_FLOAT);
    const uint32_t grey = 0xff808080;
    ComPtr<ID3D11Texture2D> exposure =
        texture(device.Get(), 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, &grey, 4);
    ComPtr<ID3D11RenderTargetView> msaaRtv, sceneRtv, copyRtv, halfRtv, glareRtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> sceneSrv, copySrv, glareSrv, exposureSrv;
    device->CreateRenderTargetView(msaa.Get(), nullptr, &msaaRtv);
    device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    device->CreateRenderTargetView(scene.Get(), nullptr, &sceneRtv);
    device->CreateRenderTargetView(sceneCopy.Get(), nullptr, &copyRtv);
    device->CreateRenderTargetView(half.Get(), nullptr, &halfRtv);
    device->CreateRenderTargetView(glare.Get(), nullptr, &glareRtv);
    device->CreateShaderResourceView(scene.Get(), nullptr, &sceneSrv);
    device->CreateShaderResourceView(sceneCopy.Get(), nullptr, &copySrv);
    device->CreateShaderResourceView(glare.Get(), nullptr, &glareSrv);
    device->CreateShaderResourceView(exposure.Get(), nullptr, &exposureSrv);
    // The eye's final (RGBA8, as LMU's): the game's blit puts the eye there, the focus post its focus view beside it.
    ComPtr<ID3D11Texture2D> eyeFinal = texture(device.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D11RenderTargetView> eyeFinalRtv;
    device->CreateRenderTargetView(eyeFinal.Get(), nullptr, &eyeFinalRtv);

    ComPtr<ID3D11VertexShader> plainVs = vs(device.Get(), kPlainVs), yebisVs = vs(device.Get(), kYebisVs),
                               twinned = vs(device.Get(), kPlainVs), twin = vs(device.Get(), kPlainVs);
    twinned->SetPrivateDataInterface(lmu::kMvrTwinGuid, twin.Get());
    ComPtr<ID3D11PixelShader> scenePs = ps(device.Get(), kScenePs), glarePs = ps(device.Get(), kGlarePs),
                              copyPs = ps(device.Get(), kCopyPs, lmu::YebisRole::copy),
                              compositePs = ps(device.Get(), kCompositePs, lmu::YebisRole::composite);
    ComPtr<ID3DBlob> blitVsCode = compile(kBlitVs, "vs_5_0");
    ComPtr<ID3D11VertexShader> blitVs;
    ComPtr<ID3D11InputLayout> blitLayout;
    if (blitVsCode) {
        device->CreateVertexShader(blitVsCode->GetBufferPointer(), blitVsCode->GetBufferSize(), nullptr, &blitVs);
        device->CreateInputLayout(kBlitLayout, static_cast<UINT>(std::size(kBlitLayout)), blitVsCode->GetBufferPointer(),
                                  blitVsCode->GetBufferSize(), &blitLayout);
    }
    ComPtr<ID3D11PixelShader> blitPs = ps(device.Get(), kBlitPs, lmu::YebisRole::blit);
    // Its vertices: 6 others first (the game draws from a start vertex), then the eye's quad.
    const D3D11_BUFFER_DESC vbd = {12 * sizeof(BlitVertex), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
                                   D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> blitVb;
    device->CreateBuffer(&vbd, nullptr, &blitVb);
    float blitColour[4] = {1, 1, 1, 1};  // the game's fades
    check(plainVs && yebisVs && twinned && scenePs && glarePs && copyPs && compositePs && blitVs && blitLayout && blitPs &&
              blitVb,
          "the stand-in shaders compiled");
    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ComPtr<ID3D11SamplerState> point, linear;
    device->CreateSamplerState(&sd, &point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device->CreateSamplerState(&sd, &linear);
    const uint16_t indices[3] = {0, 1, 2};
    const D3D11_BUFFER_DESC ibd = {sizeof(indices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA ibi = {indices, 0, 0};
    ComPtr<ID3D11Buffer> ib, copyCb = constants(device.Get(), 15 * 16), compositeCb = constants(device.Get(), 21 * 16),
                             rectCb = constants(device.Get(), 16), glareCb = constants(device.Get(), 16);
    device->CreateBuffer(&ibd, &ibi, &ib);

    // The glare: its UV across the frame, at 0.4 of the scene's size (as LMU's: 646x506 for 1614x1266).
    {
        const float size[4] = {static_cast<float>(glareW), static_cast<float>(glareH), 0, 0};
        write(context.Get(), glareCb.Get(), size, sizeof(size));
        const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(glareW), static_cast<float>(glareH), 0, 1};
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(glarePs.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, glareCb.GetAddressOf());
        context->RSSetViewports(1, &vp);
        context->OMSetRenderTargets(1, glareRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
    }

    // The focus box: exactly the eye's top-left 60x50 (the eye's tangents -1..1 across 100x80 pixels).
    lmu::MvrPassView view;
    view.eye = 0;
    view.eyeW = eyeW;
    view.eyeH = eyeH;
    view.focusW = focusW;
    view.focusH = focusH;
    view.box.valid = true;
    const float eyeTan[4] = {-1, 1, -1, 1};  // left, right, down, up
    const float focusTan[4] = {-1, -1 + 2.0f * focusW / eyeW, 1 - 2.0f * focusH / eyeH, 1};
    std::memcpy(view.box.eyeTan, eyeTan, sizeof(eyeTan));
    std::memcpy(view.box.focusTan, focusTan, sizeof(focusTan));

    const float copyConstants[15][4] = {{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {1.5f, 1.5f, 1.5f, 1}};
    const float unitConstants[15][4] = {{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {1, 1, 1, 1}};
    const float downConstants[15][4] ={{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {0.1f, 0.1f, 0.1f, 1}};
    float compositeConstants[21][4] = {};
    compositeConstants[20][0] = 1.7f;  // exposure gain
    compositeConstants[20][1] = 0.6f;  // glare
    const float eyeRect[4] = {0, 0, static_cast<float>(eyeW) / W, static_cast<float>(eyeH) / H};
    const D3D11_VIEWPORT eyeVp = {0, 0, static_cast<float>(eyeW), static_cast<float>(eyeH), 0, 1};

    // One eye pass as LMU's: the scene (the focus view drawn beside the eye: a twin bound on the depth target), the
    // resolve, then the post-processing's copy, a downsample, the composite.
    const Kit standIn = {copyPs.Get(),    compositePs.Get(),      copyCb.Get(),         compositeCb.Get(),
                         copyConstants,   sizeof(copyConstants),  downConstants,        sizeof(downConstants),
                         compositeConstants, sizeof(compositeConstants), blitVs.Get(), blitPs.Get()};
    // MSAA off (`single`): the scene drawn into its single-sample target with a single-sample depth target, no resolve;
    // the post-processing reads that target directly (docs/recon-lmu.md "MSAA off").
    ComPtr<ID3D11Texture2D> depth1 = texture(device.Get(), W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
    ComPtr<ID3D11DepthStencilView> dsv1;
    device->CreateDepthStencilView(depth1.Get(), nullptr, &dsv1);
    // The stage's single-sample depth for YEBIS (as LMU's `$Z Buffer Single Sample`, R32) and its pixel shader: the copy's
    // code, not marked (not YEBIS's), its gain in constants of its own.
    ComPtr<ID3D11Texture2D> zSingle = texture(device.Get(), W, H, DXGI_FORMAT_R32_FLOAT);
    ComPtr<ID3D11RenderTargetView> zSingleRtv;
    device->CreateRenderTargetView(zSingle.Get(), nullptr, &zSingleRtv);
    ComPtr<ID3D11Texture2D> depthZ = texture(device.Get(), W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
    ComPtr<ID3D11DepthStencilView> dsvZ;
    device->CreateDepthStencilView(depthZ.Get(), nullptr, &dsvZ);
    ComPtr<ID3D11PixelShader> stagePs = ps(device.Get(), kCopyPs);
    ComPtr<ID3D11Buffer> stageCb = constants(device.Get(), 15 * 16);
    const float wholeRect[4] = {0, 0, 1, 1};
    const auto eye_pass = [&](int pass, const lmu::MvrPassView* v, bool checkState, const Kit& k,
                              bool overlayCheck = false, bool driven = false, bool single = false) {
        if (!driven)  // driven: the tracker begins it at the scene's bind below
            lmu::mvr_pass_begin(pass, v, false);  // as the game's scene render: the pass begins, then its scene's bind
        ID3D11RenderTargetView* const sceneTarget = single ? sceneRtv.Get() : msaaRtv.Get();
        context->OMSetRenderTargets(1, &sceneTarget, single ? dsv1.Get() : dsv.Get());
        context->VSSetShader(twinned.Get(), nullptr, 0);
        const D3D11_VIEWPORT whole = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->OMSetRenderTargets(1, &sceneTarget, nullptr);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->RSSetViewports(1, &whole);
        context->Draw(3, 0);
        if (!single)
            context->ResolveSubresource(scene.Get(), 0, msaa.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);

        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->IASetInputLayout(nullptr);
        write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
        context->VSSetShader(yebisVs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(1, 1, rectCb.GetAddressOf());
        if (k.stage) {
            // A stage between the resolve and YEBIS, as the pass trace showed it: the single-sample depth drawn, the
            // scene's target bound again with its depth, then the stage's image of the resolved scene (x1.5) into the
            // scene copy, which YEBIS's chain starts from.
            context->VSSetShader(plainVs.Get(), nullptr, 0);
            context->PSSetShader(scenePs.Get(), nullptr, 0);
            context->OMSetRenderTargets(1, zSingleRtv.GetAddressOf(), dsvZ.Get());
            context->RSSetViewports(1, &whole);
            context->Draw(3, 0);
            context->OMSetRenderTargets(1, &sceneTarget, single ? dsv1.Get() : dsv.Get());
            context->OMSetRenderTargets(1, &sceneTarget, nullptr);
            write(context.Get(), rectCb.Get(), k.stage == 1 ? wholeRect : eyeRect, sizeof(eyeRect));
            write(context.Get(), stageCb.Get(), copyConstants, sizeof(copyConstants));
            context->VSSetShader(yebisVs.Get(), nullptr, 0);
            context->PSSetShader(stagePs.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, stageCb.GetAddressOf());
            context->PSSetSamplers(0, 1, point.GetAddressOf());
            context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            context->RSSetViewports(1, k.stage == 1 ? &whole : &eyeVp);
            context->DrawIndexed(3, 0, 0);
            write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
            context->PSSetShader(k.copy, nullptr, 0);  // YEBIS's chain from here: its downsample first
            context->PSSetConstantBuffers(0, 1, &k.copyCb);
        }
        // The copy of the resolved scene (none past a stage: its image is the scene copy).
        if (!k.stage) {
        write(context.Get(), k.copyCb, k.copyData, k.copyBytes);
        context->PSSetShader(k.copy, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &k.copyCb);
        context->PSSetSamplers(0, 1, point.GetAddressOf());
        context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
        context->RSSetViewports(1, &eyeVp);
        context->DrawIndexed(3, 0, 0);
        if (checkState) {
            ID3D11PixelShader* p = nullptr;
            ID3D11VertexShader* q = nullptr;
            ID3D11RenderTargetView* r = nullptr;
            ID3D11ShaderResourceView* t = nullptr;
            ID3D11Buffer* b = nullptr;
            ID3D11Buffer* b12 = nullptr;
            context->PSGetShader(&p, nullptr, nullptr);
            context->VSGetShader(&q, nullptr, nullptr);
            context->OMGetRenderTargets(1, &r, nullptr);
            context->PSGetShaderResources(0, 1, &t);
            context->PSGetConstantBuffers(0, 1, &b);
            context->VSGetConstantBuffers(12, 1, &b12);
            D3D11_VIEWPORT vp[16];
            UINT n = 16;
            context->RSGetViewports(&n, vp);
            check(bound(p) == k.copy && bound(q) == yebisVs.Get() && bound(r) == copyRtv.Get() &&
                      bound(t) == sceneSrv.Get() && bound(b) == k.copyCb && !bound(b12) && n == 1 &&
                      vp[0].Width == eyeW && vp[0].Height == eyeH,
                  "after the copy: the game's shaders, target, texture, constants and viewport as it set them");
        }
        }
        // A downsample (the copy's shader on another texture; the game's constants rewritten): not the scene's copy.
        write(context.Get(), k.copyCb, k.downData, k.downBytes);
        const D3D11_VIEWPORT halfVp = {0, 0, eyeW / 2.0f, eyeH / 2.0f, 0, 1};
        context->OMSetRenderTargets(1, halfRtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, copySrv.GetAddressOf());
        context->RSSetViewports(1, &halfVp);
        context->DrawIndexed(3, 0, 0);
        // The composite: the scene copy, the glare, the exposure; into the resolved scene (as LMU's).
        write(context.Get(), k.compositeCb, k.compositeData, k.compositeBytes);
        context->PSSetShader(k.composite, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &k.compositeCb);
        ID3D11SamplerState* samplers[3] = {point.Get(), linear.Get(), point.Get()};
        context->PSSetSamplers(0, 3, samplers);
        ID3D11ShaderResourceView* inputs[3] = {copySrv.Get(), glareSrv.Get(), exposureSrv.Get()};
        context->PSSetShaderResources(0, 3, inputs);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->RSSetViewports(1, &eyeVp);
        context->DrawIndexed(3, 0, 0);
        if (checkState) {
            ID3D11PixelShader* p = nullptr;
            ID3D11RenderTargetView* r = nullptr;
            ID3D11ShaderResourceView* t[3] = {};
            ID3D11SamplerState* s[3] = {};
            ID3D11Buffer* b = nullptr;
            context->PSGetShader(&p, nullptr, nullptr);
            context->OMGetRenderTargets(1, &r, nullptr);
            context->PSGetShaderResources(0, 3, t);
            context->PSGetSamplers(0, 3, s);
            context->PSGetConstantBuffers(0, 1, &b);
            bool same = bound(p) == k.composite && bound(r) == sceneRtv.Get() && bound(b) == k.compositeCb;
            for (int i = 0; i < 3; ++i)
                same = (bound(t[i]) == inputs[i]) && (bound(s[i]) == samplers[i]) && same;
            check(same, "after the composite: the game's shader, target, textures, samplers and constants as it set them");
        }
        // The game's final blit: the resolved scene's eye part into the eye's final, the quad over the eye from the
        // whole target's viewport, times the vertex colour.
        // FSR on: no final blit; the upscale's stand-in after the eye's image is bound (the game's compute pass writes
        // it before the bind): the whole scene target into the eye's image, by a shader that isn't YEBIS's.
        if (k.fsr) {
            ID3D11ShaderResourceView* none[3] = {};
            context->PSSetShaderResources(0, 3, none);
            context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
            write(context.Get(), rectCb.Get(), wholeRect, sizeof(wholeRect));
            write(context.Get(), stageCb.Get(), unitConstants, sizeof(unitConstants));
            context->VSSetShader(yebisVs.Get(), nullptr, 0);
            context->PSSetShader(stagePs.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, stageCb.GetAddressOf());
            context->PSSetSamplers(0, 1, point.GetAddressOf());
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            context->RSSetViewports(1, &whole);
            context->DrawIndexed(3, 0, 0);
            write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
        } else
        {
            BlitVertex verts[12] = {};
            const float x1 = -1 + 2.0f * eyeW / W, y1 = 1 - 2.0f * eyeH / H, u1 = static_cast<float>(eyeW) / W,
                        v1 = static_cast<float>(eyeH) / H;
            const float corners[6][4] = {{-1, 1, 0, 0}, {x1, 1, u1, 0}, {-1, y1, 0, v1},
                                         {x1, 1, u1, 0}, {x1, y1, u1, v1}, {-1, y1, 0, v1}};
            for (int i = 0; i < 6; ++i) {
                BlitVertex& q = verts[6 + i];
                q.pos[0] = corners[i][0];
                q.pos[1] = corners[i][1];
                q.pos[3] = 1;
                std::memcpy(q.col, blitColour, sizeof(blitColour));
                q.t[0][0] = corners[i][2];
                q.t[0][1] = corners[i][3];
            }
            write(context.Get(), blitVb.Get(), verts, sizeof(verts));
            const UINT stride = sizeof(BlitVertex), offset = 0;
            context->IASetInputLayout(blitLayout.Get());
            context->IASetVertexBuffers(0, 1, blitVb.GetAddressOf(), &stride, &offset);
            context->VSSetShader(k.blitVs, nullptr, 0);
            context->PSSetShader(k.blitPs, nullptr, 0);
            context->PSSetSamplers(0, 1, point.GetAddressOf());
            ID3D11ShaderResourceView* none[3] = {};
            context->PSSetShaderResources(0, 3, none);
            context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            context->RSSetViewports(1, &whole);  // the whole target, as the scene's
            context->Draw(6, 6);
            if (checkState) {
                ID3D11PixelShader* p = nullptr;
                ID3D11ShaderResourceView* s0 = nullptr;
                D3D11_VIEWPORT vp[16];
                UINT n = 16;
                context->PSGetShader(&p, nullptr, nullptr);
                context->PSGetShaderResources(0, 1, &s0);
                context->RSGetViewports(&n, vp);
                check(bound(p) == k.blitPs && bound(s0) == sceneSrv.Get() && n >= 1 && vp[0].Width == W && vp[0].Height == H,
                      "after the final blit: the game's pixel shader, texture and viewport as it set them");
            }
        }
        if (overlayCheck && pass < 2) {  // an overlay after the blit (the driver tags): doubled by MVR, its viewport cut
            context->VSSetShader(twinned.Get(), nullptr, 0);
            context->PSSetShader(nullptr, nullptr, 0);  // nothing written: only the state checked
            context->RSSetViewports(1, &whole);
            D3D11_VIEWPORT vp[16];
            UINT n = 16;
            context->RSGetViewports(&n, vp);
            const uint64_t before = lmu::mvr_pass_stats().overlayDraws;
            context->Draw(3, 0);
            std::snprintf(what, sizeof(what),
                          "an overlay into the eye's final after the blit: the twin, two views, its whole-target viewport "
                          "cut to the eye (%u: %.0fx%.0f, then %.0f,%.0f %.0fx%.0f), counted",
                          n, n ? vp[0].Width : 0.f, n ? vp[0].Height : 0.f, n > 1 ? vp[1].TopLeftX : 0.f,
                          n > 1 ? vp[1].TopLeftY : 0.f, n > 1 ? vp[1].Width : 0.f, n > 1 ? vp[1].Height : 0.f);
            check(bound_vs(context.Get()) == twin.Get() && twoViewsOn == 1 && n == 2 && same_vp(vp[0], 0, 0, eyeW, eyeH) &&
                      same_vp(vp[1], eyeW, 0, focusW, focusH) && lmu::mvr_pass_stats().overlayDraws == before + 1,
                  what);
        }
        if (!driven)  // driven: the tracker ends it (another target after the blit, the next scene, the frame end)
            lmu::mvr_pass_end(pass);
        context->PSSetShaderResources(0, 3, std::vector<ID3D11ShaderResourceView*>(3, nullptr).data());
        context->IASetInputLayout(nullptr);
    };

    // Eye 0's focus image against the eye's own composite over the same crop: colour within 3/255 (mean < 0.5), the
    // eye's values spanning at least `range` (a picture, not a flat colour); alpha within 1 if `alpha`.
    const auto same_as_eye = [&](const char* label, unsigned range, bool alpha) {
        unsigned long long n = 0;
        ID3D11Texture2D* image = lmu::mvr_focus_image(0, n);
        if (!image) {
            check(false, "a focus image to compare");
            return;
        }
        const std::vector<uint8_t> mine = read_rgba8(device.Get(), context.Get(), image, focusW, focusH);
        const std::vector<uint8_t> eye = read_rgba8(device.Get(), context.Get(), scene.Get(), focusW, focusH);
        int worst = 0, alphaWorst = 0;
        double sum = 0;
        unsigned lo = 255, hi = 0;
        for (size_t i = 0; i < mine.size() && mine.size() == eye.size(); ++i) {
            const int d = std::abs(static_cast<int>(mine[i]) - static_cast<int>(eye[i]));
            if (i % 4 == 3) {  // alpha: the composite's luminance (YEBIS's: for later passes)
                alphaWorst = (std::max)(alphaWorst, d);
                continue;
            }
            if (d > 3 && d > worst)
                std::printf("     differs at %zu,%zu channel %zu: focus post %u, eye %u\n", (i / 4) % focusW,
                            (i / 4) / focusW, i % 4, mine[i], eye[i]);
            worst = d > worst ? d : worst;
            sum += d;
            lo = eye[i] < lo ? eye[i] : lo;
            hi = eye[i] > hi ? eye[i] : hi;
        }
        const double mean = mine.empty() ? 1e9 : sum / static_cast<double>(mine.size() * 3 / 4);
        std::snprintf(what, sizeof(what),
                      "%s: the focus image equals the eye's own composite over the same crop: largest difference %d/255, "
                      "mean %.3f (the eye's values %u..%u); alpha largest difference %d",
                      label, worst, mean, lo, hi, alphaWorst);
        check(!mine.empty() && mine.size() == eye.size() && worst <= 3 && mean < 0.5 && hi - lo >= range &&
                  (!alpha || alphaWorst <= 1),
              what);
    };

    // Eye 0's focus final (the focus view beside the eye in the eye's final) against the eye's own final over the same
    // crop: within 1/255 (the eye's values spanning at least `range`).
    const auto final_as_eye = [&](const char* label, unsigned range) {
        unsigned x = 0, y = 0, fw = 0, fh = 0;
        if (!lmu::mvr_focus_final(0, x, y, fw, fh) || x != eyeW || y != 0 || fw != focusW || fh != focusH) {
            check(false, "a focus final beside the eye (100,0 60x50)");
            return;
        }
        const std::vector<uint8_t> all = read_rgba8(device.Get(), context.Get(), eyeFinal.Get(), W, H);
        int worst = 0;
        unsigned lo = 255, hi = 0;
        for (unsigned yy = 0; yy < focusH; ++yy)
            for (unsigned xx = 0; xx < focusW; ++xx)
                for (unsigned c = 0; c < 3; ++c) {
                    const uint8_t e = all[(static_cast<size_t>(yy) * W + xx) * 4 + c];
                    const uint8_t f = all[(static_cast<size_t>(yy) * W + eyeW + xx) * 4 + c];
                    worst = (std::max)(worst, std::abs(static_cast<int>(e) - static_cast<int>(f)));
                    lo = (std::min)(lo, static_cast<unsigned>(e));
                    hi = (std::max)(hi, static_cast<unsigned>(e));
                }
        std::snprintf(what, sizeof(what),
                      "%s: the focus final beside the eye equals the eye's own final over the same crop: largest "
                      "difference %d/255 (the eye's values %u..%u)",
                      label, worst, lo, hi);
        check(worst <= 1 && hi - lo >= range, what);
    };

    eye_pass(0, &view, true, standIn);
    lmu::MvrPassStats s = lmu::mvr_pass_stats();
    unsigned long long composites = 0;
    ID3D11Texture2D* focus = lmu::mvr_focus_image(0, composites);
    std::snprintf(what, sizeof(what),
                  "pass 0: one copy and one composite for its focus view (%llu, %llu; %llu failed; the downsample not "
                  "taken for the copy), eye 0's focus image made",
                  static_cast<unsigned long long>(s.focusCopies), static_cast<unsigned long long>(s.focusComposites),
                  static_cast<unsigned long long>(s.focusFailures));
    check(s.focusCopies == 1 && s.focusComposites == 1 && s.focusFailures == 0 && focus && composites == 1, what);

    if (focus) {
        D3D11_TEXTURE2D_DESC fd;
        focus->GetDesc(&fd);
        check(fd.Width == focusW && fd.Height == focusH && fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
              "the focus image: the focus view's size, RGBA16F (as the eye's composite)");
        same_as_eye("stand-ins", 100, true);
        final_as_eye("stand-ins", 100);
    }

    {  // A fade (the blit's vertex colour at half): the focus final fades with the eye.
        blitColour[0] = blitColour[1] = blitColour[2] = 0.5f;
        eye_pass(0, &view, false, standIn);
        final_as_eye("a fade to half", 50);
        blitColour[0] = blitColour[1] = blitColour[2] = 1;
    }

    {  // Overlays: in the pass after the blit (doubled by MVR, checked in the pass), and after the passes into the eye's
       // final (the VR UI): drawn with two views and put back.
        eye_pass(0, &view, false, standIn, true);
        const int callsBefore = twoViewsCalls;
        const uint64_t outsideBefore = lmu::mvr_pass_stats().overlaysOutside;
        context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
        context->VSSetShader(twinned.Get(), nullptr, 0);
        context->PSSetShader(nullptr, nullptr, 0);
        const D3D11_VIEWPORT whole = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        context->RSSetViewports(1, &whole);
        context->Draw(3, 0);
        D3D11_VIEWPORT vp[16];
        UINT n = 16;
        context->RSGetViewports(&n, vp);
        check(lmu::mvr_pass_stats().overlaysOutside == outsideBefore + 1 && twoViewsCalls == callsBefore + 2 &&
                  twoViewsOn == 0 && bound_vs(context.Get()) == twinned.Get() && n == 1 && same_vp(vp[0], 0, 0, W, H),
              "an overlay after the passes into the eye's final (the VR UI): drawn with two views, the game's state back");
        const uint64_t again = lmu::mvr_pass_stats().overlaysOutside;
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        check(lmu::mvr_pass_stats().overlaysOutside == again, "a draw into another target after the passes: as it is");
        lmu::mvr_pass_begin(0, nullptr, false);  // the next frame: no focus final yet, nothing doubled
        lmu::mvr_pass_end(0);
        unsigned x = 0, y = 0, fw = 0, fh = 0;
        check(!lmu::mvr_focus_final(0, x, y, fw, fh), "a new frame: no focus final until its pass makes one");
    }

    {  // The glare's part: without the glare (its weight 0) the images still agree, so the glare does show above.
        compositeConstants[20][1] = 0;
        eye_pass(0, &view, false, standIn);
        const std::vector<uint8_t> noGlare = read_rgba8(device.Get(), context.Get(), focus, focusW, focusH);
        compositeConstants[20][1] = 0.6f;
        eye_pass(0, &view, false, standIn);
        const std::vector<uint8_t> withGlare = read_rgba8(device.Get(), context.Get(), focus, focusW, focusH);
        int most = 0;
        for (size_t i = 0; i < noGlare.size() && noGlare.size() == withGlare.size(); ++i)
            most = (std::max)(most, std::abs(static_cast<int>(noGlare[i]) - static_cast<int>(withGlare[i])));
        std::snprintf(what, sizeof(what), "the glare shows in the focus image (up to %d/255 with it vs without)", most);
        check(most > 20, what);
    }

    {  // Not replayed: pass 2 (renders its focus view itself), a pass that drew no focus view, the replay off.
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(2, nullptr, false, standIn);
        lmu::MvrPassView noBox = view;
        noBox.box.valid = false;
        eye_pass(1, &noBox, false, standIn);
        lmu::mvr_focus_replay(false);
        eye_pass(0, &view, false, standIn);
        lmu::mvr_focus_replay(true);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies && after.focusComposites == before.focusComposites &&
                  after.focusFailures == before.focusFailures,
              "no focus post in pass 2, in a pass without the focus box, or with the replay off");
        eye_pass(1, &view, false, standIn);
        unsigned long long c1 = 0;
        check(lmu::mvr_focus_image(1, c1) && c1 == 1 && lmu::mvr_pass_stats().focusComposites == after.focusComposites + 1,
              "pass 1: eye 1's focus image of its own");
    }

    {  // A composite of a checksum not known (another tone curve): taken by its bindings, the same image.
        ComPtr<ID3D11PixelShader> unmarked = ps(device.Get(), kCompositePs);
        Kit other = standIn;
        other.composite = unmarked.Get();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, other);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites + 1 &&
                  after.focusCompositesByBindings == before.focusCompositesByBindings + 1 &&
                  after.focusNoComposite == before.focusNoComposite,
              "a composite of another checksum: taken by its bindings (the copy's output, a glare, the 1x1 exposure)");
        same_as_eye("a composite taken by its bindings", 100, true);
    }

    {  // A draw into the resolve's target that isn't the composite (here: reading the scene, not the copy's output):
       // not taken; the pass counted without a composite, the draw logged.
        Kit other = standIn;
        ComPtr<ID3D11PixelShader> unmarked = ps(device.Get(), kCompositePs);
        other.composite = unmarked.Get();
        ID3D11ShaderResourceView* wrong = sceneSrv.Get();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        // The pass as eye_pass runs it, but the last draw reads the resolved scene at t0 (into a copy of it: the game
        // can't read and write one texture at once).
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), dsv.Get());
        context->VSSetShader(twinned.Get(), nullptr, 0);
        lmu::mvr_pass_begin(0, &view, false);
        context->ResolveSubresource(scene.Get(), 0, msaa.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
        context->VSSetShader(yebisVs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(1, 1, rectCb.GetAddressOf());
        write(context.Get(), copyCb.Get(), copyConstants, sizeof(copyConstants));
        context->PSSetShader(copyPs.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, copyCb.GetAddressOf());
        context->PSSetSamplers(0, 1, point.GetAddressOf());
        context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, &wrong);
        context->RSSetViewports(1, &eyeVp);
        context->DrawIndexed(3, 0, 0);  // the copy
        ID3D11ShaderResourceView* inputs[3] = {copySrv.Get(), glareSrv.Get(), exposureSrv.Get()};
        inputs[0] = glareSrv.Get();      // not the copy's output
        context->PSSetShader(unmarked.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 3, inputs);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->DrawIndexed(3, 0, 0);
        lmu::mvr_pass_end(0);
        context->PSSetShaderResources(0, 3, std::vector<ID3D11ShaderResourceView*>(3, nullptr).data());
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites &&
                  after.focusNoComposite == before.focusNoComposite + 1 &&
                  after.focusUnknownComposites == before.focusUnknownComposites + 1,
              "a draw into the resolve's target that doesn't read the copy's output: not the composite, logged");
    }

    {  // The overlay trace's first frame (600 frame ends after the install): a pass and a draw outside it, logged
       // (SIM_QVFR_LOG), the focus post as before.
        for (unsigned long long f = 1; f < 600; ++f)
            lmu::mvr_frame_end(f);
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, standIn);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        lmu::mvr_frame_end(600);
        check(lmu::mvr_pass_stats().focusComposites == before.focusComposites + 1,
              "a traced frame: the focus post as before (the trace's lines in SIM_QVFR_LOG)");
    }

    {  // R4b's pass tracker (observe only): a frame of two eye passes found where the game's are; a frame without the
       // game's passes (a menu) with a multisample bind reported as a false find.
        lmu::mvr_track_passes(true);
        lmu::mvr_frame_end(100000);  // a fresh frame for the tracker
        const lmu::MvrTrackStats before = lmu::mvr_track_stats();
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        eye_pass(0, &view, false, standIn);
        eye_pass(1, &view1, false, standIn);
        lmu::mvr_frame_end(100001);
        const lmu::MvrTrackStats after = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "pass tracker: the game's two passes found as its own (%llu frame matched, %llu passes compared; "
                      "draws left out at the start %llu, at the end %llu)",
                      static_cast<unsigned long long>(after.matched - before.matched),
                      static_cast<unsigned long long>(after.passesCompared - before.passesCompared),
                      static_cast<unsigned long long>(after.startGapSum - before.startGapSum),
                      static_cast<unsigned long long>(after.endGapSum - before.endGapSum));
        check(after.frames == before.frames + 1 && after.matched == before.matched + 1 &&
                  after.passesCompared == before.passesCompared + 2 && after.beganBefore == before.beganBefore &&
                  after.endedAfter == before.endedAfter && after.countDiffers == before.countDiffers &&
                  after.startGapSum == before.startGapSum && after.endGapSum == before.endGapSum,
              what);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), dsv.Get());
        context->Draw(3, 0);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        lmu::mvr_frame_end(100002);
        const lmu::MvrTrackStats menu = lmu::mvr_track_stats();
        check(menu.framesWithoutGame == after.framesWithoutGame + 1 && menu.passesWithoutGame == after.passesWithoutGame + 1,
              "pass tracker: a frame without the game's passes with a multisample scene bind: counted as a false find");
        lmu::mvr_track_passes(false);
    }

    {  // R4b-2: the tracker drives the passes: begun at the scene's bind, ended after the blit or at the frame end, the
       // focus post and finals as with the game's bounds.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100010);
        driveLog.clear();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        driveFinals = false;
        lmu::mvr_frame_end(100011);  // pass 1 ends here (no other target after its blit)
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        const bool finals = driveFinals;
        std::snprintf(what, sizeof(what),
                      "the tracker drives the passes:%s; %llu composites and %llu final blits, both eyes' focus finals made",
                      driveLog.c_str(), static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits));
        check(driveLog == " b0 e0 b1 e1" && after.focusComposites == before.focusComposites + 2 &&
                  after.focusBlits == before.focusBlits + 2 && finals,
              what);
        driveLog.clear();
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->OMSetRenderTargets(1, sceneRtv.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        lmu::mvr_frame_end(100012);
        check(driveLog.empty(), "the tracker drives no pass in a frame without a multisample scene bind (a menu)");
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    {  // A stage between the resolve and YEBIS (reflections high, the 2026-11-11 pass trace): a single-sample depth
       // drawn, the scene's target bound again, YEBIS's chain from the stage's image. Its image over the focus view:
       // the focus view's scene from it (the eye's picture by construction); over the eye only: from the resolve.
        Kit staged = standIn;
        staged.stage = 1;
        lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, staged);
        lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusStageCopies == before.focusStageCopies + 1 &&
                  after.focusComposites == before.focusComposites + 1 && after.focusFailures == before.focusFailures,
              "past a stage over the whole target: the focus view's scene from its image, then the composite");
        same_as_eye("past a stage over the whole target", 100, true);
        final_as_eye("past a stage over the whole target", 100);
        staged.stage = 2;
        before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, staged);
        after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusStageResolves == before.focusStageResolves + 1 &&
                  after.focusComposites == before.focusComposites + 1 && after.focusFailures == before.focusFailures,
              "past a stage over the eye only: the focus view's scene from the resolve, then the composite");

        // The tracker driving: the rebind of the pass's own scene before YEBIS keeps the pass open (before, it ended
        // there at "the next pass's scene" and YEBIS ran outside it: no focus finals, the fallback after 90 frames).
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100015);
        driveLog.clear();
        staged.stage = 1;
        before = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackBefore = lmu::mvr_track_stats();
        eye_pass(0, nullptr, false, staged, false, true);
        eye_pass(1, nullptr, false, staged, false, true);
        driveFinals = false;
        lmu::mvr_frame_end(100016);
        after = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackAfter = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "past a stage, the tracker driving:%s; scene rebinds kept %llu; %llu composites, %llu final blits, "
                      "both eyes' focus finals made",
                      driveLog.c_str(), static_cast<unsigned long long>(trackAfter.sceneRebinds - trackBefore.sceneRebinds),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits));
        check(driveLog == " b0 e0 b1 e1" && trackAfter.sceneRebinds == trackBefore.sceneRebinds + 2 &&
                  after.focusComposites == before.focusComposites + 2 && after.focusBlits == before.focusBlits + 2 &&
                  driveFinals,
              what);
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    {  // MSAA off: no multisample target, no resolve; YEBIS's copy reads the scene's own target. A frame of it without
       // passes teaches the tracker that target (what the copy reads, drawn with depth); from the next frame it drives
       // the passes from its binds with depth, and their post-processing begins at the copy: the focus post and finals
       // as with MSAA. A multisample frame after it: the passes from the multisample binds again.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100020);
        driveLog.clear();
        // The frame it learns in: both eyes, no pass begun (eye 1's scene bind comes after the learning, mid-frame).
        eye_pass(0, nullptr, false, standIn, false, true, true);
        eye_pass(1, nullptr, false, standIn, false, true, true);
        lmu::mvr_frame_end(100021);
        const bool learnedWithoutPasses = driveLog.empty();
        // A frame whose focus views don't fit (the first after loading, at the game's own size: no view handed over):
        // no focus post, yet both passes found, pass 0 ending after its own final blit (after which pass 1's end
        // applies the live resolution).
        driveLog.clear();
        driveViews[0] = driveViews[1] = nullptr;
        const lmu::MvrTrackStats unfitBefore = lmu::mvr_track_stats();
        const lmu::MvrPassStats unfitPost = lmu::mvr_pass_stats();
        eye_pass(0, nullptr, false, standIn, false, true, true);
        eye_pass(1, nullptr, false, standIn, false, true, true);
        lmu::mvr_frame_end(100024);
        const lmu::MvrTrackStats unfit = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "MSAA off, the focus views not fitting: passes%s, %llu final blits seen, %llu composites, the "
                      "fallback refused (single-sample: %s)",
                      driveLog.c_str(), static_cast<unsigned long long>(unfit.singleSampleBlits - unfitBefore.singleSampleBlits),
                      static_cast<unsigned long long>(lmu::mvr_pass_stats().focusComposites - unfitPost.focusComposites),
                      lmu::mvr_single_sample_passes() ? "yes" : "NO");
        check(driveLog == " b0 e0 b1 e1" && unfit.singleSampleBlits == unfitBefore.singleSampleBlits + 2 &&
                  lmu::mvr_pass_stats().focusComposites == unfitPost.focusComposites && lmu::mvr_single_sample_passes(),
              what);
        // A blit reading the scene's target before the pass's post-processing began (a draw like the final blit
        // mid-pass): not the pass's end, so the eye's pass stays one and the other eye is still found.
        driveLog.clear();
        const lmu::MvrTrackStats strayBefore = lmu::mvr_track_stats();
        const uint64_t postsBefore = lmu::mvr_pass_stats().singleSamplePosts;
        {
            ID3D11RenderTargetView* const sceneTarget = sceneRtv.Get();
            context->OMSetRenderTargets(1, &sceneTarget, dsv1.Get());  // eye 0's scene: its pass begins
            context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
            context->IASetInputLayout(blitLayout.Get());
            const UINT stride = sizeof(BlitVertex), offset = 0;
            context->IASetVertexBuffers(0, 1, blitVb.GetAddressOf(), &stride, &offset);
            context->VSSetShader(blitVs.Get(), nullptr, 0);
            context->PSSetShader(blitPs.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, sceneSrv.GetAddressOf());
            const D3D11_VIEWPORT whole = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
            context->RSSetViewports(1, &whole);
            context->Draw(6, 6);  // the stray blit, before any copy
            context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);  // another target after it
        }
        eye_pass(0, nullptr, false, standIn, false, true, true);  // the rest of eye 0 (its scene bind again, its post)
        eye_pass(1, nullptr, false, standIn, false, true, true);
        lmu::mvr_frame_end(100025);
        const lmu::MvrTrackStats stray = lmu::mvr_track_stats();
        const uint64_t posts = lmu::mvr_pass_stats().singleSamplePosts - postsBefore;
        std::snprintf(what, sizeof(what),
                      "MSAA off, a blit reading the scene before the copy: passes%s, both eyes' post-processing begun (%llu), "
                      "%llu blits before a post not taken, %llu passes ended before their post",
                      driveLog.c_str(), static_cast<unsigned long long>(posts),
                      static_cast<unsigned long long>(stray.blitsBeforePost - strayBefore.blitsBeforePost),
                      static_cast<unsigned long long>(stray.passesWithoutPost - strayBefore.passesWithoutPost));
        check(driveLog == " b0 e0 b1 e1" && posts == 2 && stray.blitsBeforePost == strayBefore.blitsBeforePost + 1 &&
                  stray.passesWithoutPost == strayBefore.passesWithoutPost,
              what);
        {  // A small view of the world post-processed the same way, outside the passes (LMU's mirror: 2560x160 beside a
           // 3120x2505 scene): drawn with a depth target, read by YEBIS's copy. It must not be learned in the scene's
           // place: the next passes are still found on the scene's own target.
            const UINT mh = H / 8 ? H / 8 : 1;
            ComPtr<ID3D11Texture2D> mirror = texture(device.Get(), W, mh, DXGI_FORMAT_R16G16B16A16_FLOAT, 1,
                                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            ComPtr<ID3D11Texture2D> mirrorDepth = texture(device.Get(), W, mh, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
            ComPtr<ID3D11RenderTargetView> mirrorRtv;
            ComPtr<ID3D11ShaderResourceView> mirrorSrv;
            ComPtr<ID3D11DepthStencilView> mirrorDsv;
            device->CreateRenderTargetView(mirror.Get(), nullptr, &mirrorRtv);
            device->CreateShaderResourceView(mirror.Get(), nullptr, &mirrorSrv);
            device->CreateDepthStencilView(mirrorDepth.Get(), nullptr, &mirrorDsv);
            const lmu::MvrTrackStats mirrorBefore = lmu::mvr_track_stats();
            driveLog.clear();
            context->OMSetRenderTargets(1, mirrorRtv.GetAddressOf(), mirrorDsv.Get());  // drawn with depth, before the eyes
            context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
            context->IASetInputLayout(nullptr);
            context->VSSetShader(yebisVs.Get(), nullptr, 0);
            context->VSSetConstantBuffers(1, 1, rectCb.GetAddressOf());
            write(context.Get(), standIn.copyCb, standIn.copyData, standIn.copyBytes);
            context->PSSetShader(standIn.copy, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &standIn.copyCb);
            context->OMSetRenderTargets(1, copyRtv.GetAddressOf(), nullptr);
            context->PSSetShaderResources(0, 1, mirrorSrv.GetAddressOf());  // YEBIS's copy reads it
            context->RSSetViewports(1, &eyeVp);
            context->DrawIndexed(3, 0, 0);
            ID3D11ShaderResourceView* noInput = nullptr;
            context->PSSetShaderResources(0, 1, &noInput);
            eye_pass(0, nullptr, false, standIn, false, true, true);
            eye_pass(1, nullptr, false, standIn, false, true, true);
            lmu::mvr_frame_end(100026);
            const lmu::MvrTrackStats mirrorAfter = lmu::mvr_track_stats();
            std::snprintf(what, sizeof(what),
                          "MSAA off, a %ux%u view post-processed outside the passes (a mirror): passes%s, the scene's target "
                          "learned again %llu times, %llu candidates refused",
                          W, mh, driveLog.c_str(),
                          static_cast<unsigned long long>(mirrorAfter.sceneRelearned - mirrorBefore.sceneRelearned),
                          static_cast<unsigned long long>(mirrorAfter.sceneCandidatesRefused - mirrorBefore.sceneCandidatesRefused));
            check(driveLog == " b0 e0 b1 e1" && mirrorAfter.sceneRelearned == mirrorBefore.sceneRelearned &&
                      mirrorAfter.sceneCandidatesRefused == mirrorBefore.sceneCandidatesRefused + 1,
                  what);
        }
        driveViews[0] = &view;
        driveViews[1] = &view1;
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackBefore = lmu::mvr_track_stats();
        driveLog.clear();
        eye_pass(0, nullptr, false, standIn, false, true, true);
        eye_pass(1, nullptr, false, standIn, false, true, true);
        driveFinals = false;
        lmu::mvr_frame_end(100022);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackAfter = lmu::mvr_track_stats();
        const bool finals = driveFinals, single = lmu::mvr_single_sample_passes();
        std::snprintf(what, sizeof(what),
                      "MSAA off: the scene's target learned in a frame without passes (%s), then the tracker drives the "
                      "passes:%s; post-processings begun at the copy %llu, resolves %llu; %llu composites and %llu final "
                      "blits, both eyes' focus finals made; the passes said single-sample",
                      learnedWithoutPasses ? "none driven" : "SOME DRIVEN", driveLog.c_str(),
                      static_cast<unsigned long long>(after.singleSamplePosts - before.singleSamplePosts),
                      static_cast<unsigned long long>(after.resolves - before.resolves),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits));
        check(learnedWithoutPasses && driveLog == " b0 e0 b1 e1" && after.singleSamplePosts == before.singleSamplePosts + 2 &&
                  after.resolves == before.resolves && after.focusComposites == before.focusComposites + 2 &&
                  after.focusBlits == before.focusBlits + 2 && finals && single &&
                  trackAfter.singleSamplePasses == trackBefore.singleSamplePasses + 2,
              what);
        driveLog.clear();
        eye_pass(0, nullptr, false, standIn, false, true);
        eye_pass(1, nullptr, false, standIn, false, true);
        lmu::mvr_frame_end(100023);
        const lmu::MvrPassStats msaaAgain = lmu::mvr_pass_stats();
        check(driveLog == " b0 e0 b1 e1" && msaaAgain.resolves == after.resolves + 2 &&
                  msaaAgain.singleSamplePosts == after.singleSamplePosts && !lmu::mvr_single_sample_passes(),
              "MSAA on again: the passes from the multisample binds, their post-processing at the resolve");
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }
    {  // MSAA off past a stage (post effects medium and up, the 2026-11-12 pass trace): the stage draws its image of
       // the scene, YEBIS's copy starts from that image and its composite draws into the scene's target. The copy
       // never reads the scene's target, so it is learned from the composite (a draw reading the copy's source into a
       // target of its size drawn with depth); in the passes the post-processing begins at the stage's draw.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        Kit staged = standIn;
        staged.stage = 1;
        lmu::mvr_track_passes(true);  // (off above: the scene's target forgotten)
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_frame_end(100040);
        const lmu::MvrTrackStats learnBefore = lmu::mvr_track_stats();
        driveLog.clear();
        eye_pass(0, nullptr, false, staged, false, true, true);
        eye_pass(1, nullptr, false, staged, false, true, true);
        lmu::mvr_frame_end(100041);
        const lmu::MvrTrackStats learnAfter = lmu::mvr_track_stats();
        const bool learnedQuietly = driveLog.empty();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackBefore = lmu::mvr_track_stats();
        eye_pass(0, nullptr, false, staged, false, true, true);
        eye_pass(1, nullptr, false, staged, false, true, true);
        driveFinals = false;
        lmu::mvr_frame_end(100042);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        const lmu::MvrTrackStats trackAfter = lmu::mvr_track_stats();
        std::snprintf(what, sizeof(what),
                      "MSAA off past a stage: the scene's target learned at the composite %llu times (%s), then the "
                      "tracker drives the passes:%s; post-processings begun %llu (%llu at the stage's draw); the focus "
                      "view's scene from the stage's image %llu, %llu composites, %llu final blits, %llu final blits seen, "
                      "%llu steps failed, finals %s",
                      static_cast<unsigned long long>(learnAfter.sceneAtComposite - learnBefore.sceneAtComposite),
                      learnedQuietly ? "none driven" : "SOME DRIVEN", driveLog.c_str(),
                      static_cast<unsigned long long>(after.singleSamplePosts - before.singleSamplePosts),
                      static_cast<unsigned long long>(after.singleSampleStagePosts - before.singleSampleStagePosts),
                      static_cast<unsigned long long>(after.focusStageCopies - before.focusStageCopies),
                      static_cast<unsigned long long>(after.focusComposites - before.focusComposites),
                      static_cast<unsigned long long>(after.focusBlits - before.focusBlits),
                      static_cast<unsigned long long>(trackAfter.singleSampleBlits - trackBefore.singleSampleBlits),
                      static_cast<unsigned long long>(after.focusFailures - before.focusFailures),
                      driveFinals ? "made" : "NOT MADE");
        check(learnAfter.sceneAtComposite == learnBefore.sceneAtComposite + 1 && learnedQuietly &&
                  driveLog == " b0 e0 b1 e1" && after.singleSamplePosts == before.singleSamplePosts + 2 &&
                  after.singleSampleStagePosts == before.singleSampleStagePosts + 2 &&
                  after.focusStageCopies == before.focusStageCopies + 2 &&
                  after.focusComposites == before.focusComposites + 2 && after.focusBlits == before.focusBlits + 2 &&
                  trackAfter.singleSampleBlits == trackBefore.singleSampleBlits + 2 &&
                  after.focusFailures == before.focusFailures && driveFinals && lmu::mvr_single_sample_passes(),
              what);
        same_as_eye("MSAA off past a stage", 100, true);  // (the finals: driveFinals, read at pass 1's end)
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    {  // FSR on (mvr_fsr): no final blit; after YEBIS's composite the game's compute upscale writes the whole scene
       // target into the eye's image, then binds that image. The focus composite is put beside the eye in the scene's
       // target right after the composite, so the upscale (here: a copy of the whole target, by an unmarked shader after
       // the bind) carries it beside the eye; the focus final is found at the eye's image's bind.
        lmu::MvrPassView view1 = view;
        view1.eye = 1;
        driveViews[0] = &view;
        driveViews[1] = &view1;
        Kit fsr = standIn;
        fsr.fsr = true;
        lmu::mvr_track_passes(true);
        lmu::mvr_track_drive(&drive_begin, &drive_end);
        lmu::mvr_fsr(true, eyeFinal.Get(), eyeFinal.Get());
        lmu::mvr_frame_end(100050);
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        driveLog.clear();
        driveFinals = false;
        eye_pass(0, nullptr, false, fsr, false, true);
        unsigned x = 0, y = 0, fw = 0, fh = 0;
        const bool final0 = lmu::mvr_focus_final(0, x, y, fw, fh) && x == eyeW && y == 0 && fw == focusW && fh == focusH;
        const std::vector<uint8_t> all = read_rgba8(device.Get(), context.Get(), eyeFinal.Get(), W, H);
        int worst = 0;
        unsigned lo = 255, hi = 0;
        for (unsigned yy = 0; yy < focusH; ++yy)
            for (unsigned xx = 0; xx < focusW; ++xx)
                for (unsigned c = 0; c < 3; ++c) {
                    const uint8_t e = all[(static_cast<size_t>(yy) * W + xx) * 4 + c];
                    const uint8_t f = all[(static_cast<size_t>(yy) * W + eyeW + xx) * 4 + c];
                    worst = (std::max)(worst, std::abs(static_cast<int>(e) - static_cast<int>(f)));
                    lo = (std::min)(lo, static_cast<unsigned>(e));
                    hi = (std::max)(hi, static_cast<unsigned>(e));
                }
        eye_pass(1, nullptr, false, fsr, false, true);
        lmu::mvr_frame_end(100051);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        std::snprintf(what, sizeof(what),
                      "FSR on, the tracker driving:%s; %llu focus composites put beside the eye, %llu finals found at the "
                      "eye's image's bind (eye 0's %s), finals %s; the focus final equals the eye's own over the same crop: "
                      "largest difference %d/255 (the eye's values %u..%u)",
                      driveLog.c_str(), static_cast<unsigned long long>(after.fsrPlaced - before.fsrPlaced),
                      static_cast<unsigned long long>(after.fsrFinals - before.fsrFinals), final0 ? "beside the eye" : "NOT THERE",
                      driveFinals ? "made" : "NOT MADE", worst, lo, hi);
        check(driveLog == " b0 e0 b1 e1" && after.fsrPlaced == before.fsrPlaced + 2 && after.fsrFinals == before.fsrFinals + 2 &&
                  after.focusFailures == before.focusFailures && final0 && driveFinals && worst <= 3 && hi - lo >= 100,
              what);
        lmu::mvr_fsr(false, nullptr, nullptr);
        lmu::mvr_track_drive(nullptr, nullptr);
        lmu::mvr_track_passes(false);
    }

    const auto marked = [&](lmu::VsRole role) {  // a vertex shader with a role, as the probe marks the game's
        ComPtr<ID3D11VertexShader> s = vs(device.Get(), kPlainVs);
        const UINT r = static_cast<UINT>(role);
        if (s)
            s->SetPrivateData(lmu::kVsRoleGuid, sizeof(r), &r);
        return s;
    };
    {  // R6c: the lens mask's draw by its vertex shader's role: in a pass drawing its focus view, like the scene's (the
       // twin, two views); while the mask is stale, not drawn at all.
        lmu::mvr_frame_end(100030);
        ComPtr<ID3D11VertexShader> maskVs = marked(lmu::VsRole::lensMask), maskTwin = vs(device.Get(), kPlainVs);
        maskVs->SetPrivateDataInterface(lmu::kMvrTwinGuid, maskTwin.Get());
        lmu::mvr_twins_changed();
        std::atomic<bool> stale{false};
        lmu::mvr_mask_stale(&stale);
        ComPtr<ID3D11Query> query;
        const D3D11_QUERY_DESC qd = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
        device->CreateQuery(&qd, &query);
        const auto vertices_drawn = [&]() -> UINT64 {
            context->Begin(query.Get());
            context->Draw(3, 0);
            context->End(query.Get());
            D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
            while (context->GetData(query.Get(), &s, sizeof(s), 0) == S_FALSE) {
            }
            return s.IAVertices;
        };
        lmu::mvr_pass_begin(0, &view, false);
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), dsv.Get());
        context->IASetInputLayout(nullptr);
        context->VSSetShader(maskVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->RSSetViewports(1, &eyeVp);
        const lmu::MvrPassStats s0 = lmu::mvr_pass_stats();
        const UINT64 drawn = vertices_drawn();
        const bool twoViews = bound_vs(context.Get()) == maskTwin.Get() && twoViewsOn == 1;
        stale = true;
        const UINT64 dropped = vertices_drawn();
        stale = false;
        const lmu::MvrPassStats s1 = lmu::mvr_pass_stats();
        lmu::mvr_pass_end(0);
        lmu::mvr_mask_stale(nullptr);
        std::snprintf(what, sizeof(what),
                      "R6c: the lens mask's draw (its shader's role) drawn with two views like the scene's (%llu vertices), "
                      "not at all while stale (%llu); counted %llu drawn, %llu with two views, %llu dropped",
                      static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(dropped),
                      static_cast<unsigned long long>(s1.maskDraws - s0.maskDraws),
                      static_cast<unsigned long long>(s1.maskDrawsTwo - s0.maskDrawsTwo),
                      static_cast<unsigned long long>(s1.maskDropped - s0.maskDropped));
        check(drawn == 3 && twoViews && dropped == 0 && s1.maskDraws == s0.maskDraws + 1 &&
                  s1.maskDrawsTwo == s0.maskDrawsTwo + 1 && s1.maskDropped == s0.maskDropped + 1,
              what);
    }
    {  // R6b: a VR UI panel's draw outside the passes in a frame without focus finals (a menu): the callback right before
       // and right after it; mvr_redraw_panel draws it again into another image at that image's size, the game's target,
       // viewport and scissor put back. Another shader's draw: no callback.
        lmu::mvr_frame_end(100031);  // a frame without focus finals
        ComPtr<ID3D11VertexShader> panelVs = marked(lmu::VsRole::vruiPanel), otherVs = vs(device.Get(), kPlainVs);
        lmu::mvr_twins_changed();
        ComPtr<ID3D11PixelShader> panelPs =
            ps(device.Get(), "float4 main() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }");
        constexpr unsigned mw = 64, mh = 32;
        ComPtr<ID3D11Texture2D> menu = texture(device.Get(), mw, mh, DXGI_FORMAT_R8G8B8A8_UNORM);
        ComPtr<ID3D11RenderTargetView> menuRtv;
        device->CreateRenderTargetView(menu.Get(), nullptr, &menuRtv);
        const float zero[4] = {};
        context->ClearRenderTargetView(menuRtv.Get(), zero);
        panelLog.clear();
        panelTarget = menuRtv.Get();
        panelW = mw;
        panelH = mh;
        lmu::mvr_panel_draws(&panel_cb);
        const lmu::MvrPassStats s0 = lmu::mvr_pass_stats();
        const D3D11_VIEWPORT gameVp = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};  // the eye's image
        const D3D11_RECT gameSc = {0, 0, static_cast<LONG>(W), static_cast<LONG>(H)};
        context->OMSetRenderTargets(1, eyeFinalRtv.GetAddressOf(), nullptr);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(panelVs.Get(), nullptr, 0);
        context->PSSetShader(panelPs.Get(), nullptr, 0);
        context->RSSetViewports(1, &gameVp);
        context->RSSetScissorRects(1, &gameSc);
        context->Draw(3, 0);
        ID3D11RenderTargetView* r = nullptr;
        context->OMGetRenderTargets(1, &r, nullptr);
        D3D11_VIEWPORT vp[16];
        UINT n = 16;
        context->RSGetViewports(&n, vp);
        D3D11_RECT sc[16];
        UINT sn = 16;
        context->RSGetScissorRects(&sn, sc);
        const std::vector<uint8_t> px = read_rgba8(device.Get(), context.Get(), menu.Get(), mw, mh);
        bool filled = px.size() == static_cast<size_t>(mw) * mh * 4;
        const int want[4] = {64, 128, 191, 255};
        for (size_t i = 0; filled && i < px.size(); ++i)
            filled = std::abs(static_cast<int>(px[i]) - want[i % 4]) <= 1;
        const std::string first = panelLog;
        context->VSSetShader(otherVs.Get(), nullptr, 0);
        context->Draw(3, 0);
        const lmu::MvrPassStats s1 = lmu::mvr_pass_stats();
        lmu::mvr_panel_draws(nullptr);
        panelTarget = nullptr;
        std::snprintf(what, sizeof(what),
                      "R6b: a menu frame's VR UI panel draw: the callback before and after it ('%s', then '%s' with another "
                      "shader), drawn again over the whole %ux%u image, the game's target, viewport and scissor back",
                      first.c_str(), panelLog.c_str(), mw, mh);
        check(first == " b a" && panelLog == first && filled && bound(r) == eyeFinalRtv.Get() && n == 1 &&
                  same_vp(vp[0], 0, 0, W, H) && sn == 1 && sc[0].right == static_cast<LONG>(W) &&
                  sc[0].bottom == static_cast<LONG>(H) && s1.panelReplays == s0.panelReplays + 1,
              what);
    }

    {  // R5c: the game's CopySubresourceRegion remembered: its source found by the destination (LMU's submit names its eye
       // images this way, at an offset into the swapchain image), only this frame's or the one before's.
        ComPtr<ID3D11Texture2D> image0 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                image1 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                chain0 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                chain1 = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM),
                                other = texture(device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM);
        lmu::mvr_frame_end(100040);
        context->CopySubresourceRegion(chain0.Get(), 0, 0, 0, 0, image0.Get(), 0, nullptr);
        context->CopySubresourceRegion(chain1.Get(), 0, 0, 0, 0, image1.Get(), 0, nullptr);
        const bool found = lmu::mvr_copy_source(chain0.Get()) == image0.Get() &&
                           lmu::mvr_copy_source(chain1.Get()) == image1.Get() && !lmu::mvr_copy_source(other.Get());
        lmu::mvr_frame_end(100041);
        const bool lastFrame = lmu::mvr_copy_source(chain0.Get()) == image0.Get();
        lmu::mvr_frame_end(100042);
        const bool old = lmu::mvr_copy_source(chain0.Get()) == nullptr;
        check(found && lastFrame && old,
              "R5c: the game's CopySubresourceRegion into a swapchain image names its source (this frame's or the one "
              "before's, not older)");
    }

    if (!folder.empty()) {  // YEBIS's own copy and composite with a capture's constants
        const std::vector<uint8_t> copyCode = file_bytes(folder + "\\copy-ps.dxbc"),
                                   compositeCode = file_bytes(folder + "\\composite-ps.dxbc"),
                                   copyData = file_bytes(folder + "\\copy-ps-cb0.bin"),
                                   compositeData = file_bytes(folder + "\\composite-ps-cb0.bin");
        const lmu::YebisRole copyRole = lmu::yebis_role(copyCode.data(), copyCode.size()),
                             compositeRole = lmu::yebis_role(compositeCode.data(), compositeCode.size());
        check(copyRole == lmu::YebisRole::copy && compositeRole == lmu::YebisRole::composite,
              "YEBIS's own copy and composite (a capture's bytecode): their roles by checksum, as the probe marks them");
        ComPtr<ID3D11PixelShader> realCopy, realComposite;
        if (!copyCode.empty())
            device->CreatePixelShader(copyCode.data(), copyCode.size(), nullptr, &realCopy);
        if (!compositeCode.empty())
            device->CreatePixelShader(compositeCode.data(), compositeCode.size(), nullptr, &realComposite);
        for (auto [shader, role] : {std::pair{realCopy.Get(), copyRole}, std::pair{realComposite.Get(), compositeRole}})
            if (shader) {
                const UINT r = static_cast<UINT>(role);
                shader->SetPrivateData(lmu::kYebisRoleGuid, sizeof(r), &r);
            }
        // YEBIS's $Globals: one 64 KB buffer, bound whole (tools/rdc/shader_bytes.py: offset 0, size 65536).
        constexpr UINT kGlobals = 65536;
        ComPtr<ID3D11Buffer> realCopyCb = constants(device.Get(), kGlobals), realCompositeCb = constants(device.Get(), kGlobals);
        const std::vector<uint8_t> zeros(copyData.size(), 0);
        check(realCopy && realComposite && !copyData.empty() && copyData.size() <= kGlobals && !compositeData.empty() &&
                  compositeData.size() <= kGlobals,
              "YEBIS's own shaders created, their constants read");
        if (realCopy && realComposite && !copyData.empty() && !compositeData.empty()) {
            // The game's own final blit too, when the folder has it (blit-vs.dxbc, blit-ps.dxbc): our blit's pixel
            // shader must link with its vertex shader.
            const std::vector<uint8_t> blitVsCode2 = file_bytes(folder + "\\blit-vs.dxbc"),
                                       blitPsCode = file_bytes(folder + "\\blit-ps.dxbc");
            ComPtr<ID3D11VertexShader> realBlitVs;
            ComPtr<ID3D11PixelShader> realBlitPs;
            if (!blitVsCode2.empty())
                device->CreateVertexShader(blitVsCode2.data(), blitVsCode2.size(), nullptr, &realBlitVs);
            if (!blitPsCode.empty() && lmu::yebis_role(blitPsCode.data(), blitPsCode.size()) == lmu::YebisRole::blit) {
                device->CreatePixelShader(blitPsCode.data(), blitPsCode.size(), nullptr, &realBlitPs);
                const UINT r = static_cast<UINT>(lmu::YebisRole::blit);
                if (realBlitPs)
                    realBlitPs->SetPrivateData(lmu::kYebisRoleGuid, sizeof(r), &r);
            }
            std::printf("     the game's final blit: %s\n", realBlitVs && realBlitPs ? "its own shaders (role by checksum)"
                                                                                     : "not in the folder: the stand-in");
            const Kit real = {realCopy.Get(),  realComposite.Get(), realCopyCb.Get(),     realCompositeCb.Get(),
                              copyData.data(), copyData.size(),     zeros.data(),         zeros.size(),
                              compositeData.data(), compositeData.size(),
                              realBlitVs && realBlitPs ? realBlitVs.Get() : blitVs.Get(),
                              realBlitVs && realBlitPs ? realBlitPs.Get() : blitPs.Get()};
            if (infoQueue)
                infoQueue->ClearStoredMessages();
            const lmu::MvrPassStats before = lmu::mvr_pass_stats();
            eye_pass(0, &view, true, real);
            const lmu::MvrPassStats after = lmu::mvr_pass_stats();
            check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites + 1 &&
                      after.focusFailures == before.focusFailures,
                  "YEBIS's own shaders: its copy and composite replayed once each");
            same_as_eye("YEBIS's own shaders", 20, true);
            final_as_eye("YEBIS's own shaders and the game's blit", 20);
            if (infoQueue) {
                int errors = 0;
                const UINT64 n = infoQueue->GetNumStoredMessages();
                for (UINT64 i = 0; i < n; ++i) {
                    SIZE_T length = 0;
                    infoQueue->GetMessage(i, nullptr, &length);
                    std::vector<uint64_t> storage(length / sizeof(uint64_t) + 1);
                    auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                    if (SUCCEEDED(infoQueue->GetMessage(i, m, &length)) && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                        std::printf("     debug layer: %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
                        errors += m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR ? 1 : 0;
                    }
                }
                std::snprintf(what, sizeof(what),
                              "the debug layer: no errors in that pass, %llu messages (our vertex shaders link with "
                              "YEBIS's pixel shaders)",
                              static_cast<unsigned long long>(n));
                check(errors == 0, what);
            }
        }
    }

    {  // The desktop mirror as LMU's: its swapchain's back buffer bound with a depth target outside the passes, the
       // tracker on (it notes what is drawn with depth there, for MSAA off). Nothing of it may be held: ResizeBuffers
       // fails while any reference to a back buffer is (it did, "Resize swap chain failed" at the session's start,
       // 2026-11-10 19:13). The control: a view of ours kept alive makes it fail, so the check can see a reference.
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"lmu_focus_post_test";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance,
                                  nullptr);
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        ComPtr<IDXGISwapChain> chain;
        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferDesc.Width = scd.BufferDesc.Height = 64;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.OutputWindow = hwnd;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const bool made = hwnd && SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
                          SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) &&
                          SUCCEEDED(factory->CreateSwapChain(device.Get(), &scd, &chain));
        lmu::mvr_track_passes(true);
        lmu::mvr_frame_end(100030);
        ComPtr<ID3D11Texture2D> mirrorDepth =
            texture(device.Get(), 64, 64, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, D3D11_BIND_DEPTH_STENCIL);
        ComPtr<ID3D11DepthStencilView> mirrorDsv;
        device->CreateDepthStencilView(mirrorDepth.Get(), nullptr, &mirrorDsv);
        const auto mirror_frame = [&](ComPtr<ID3D11RenderTargetView>* keep) {
            ComPtr<ID3D11Texture2D> back;
            ComPtr<ID3D11RenderTargetView> backRtv;
            if (!chain || FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
                FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &backRtv)))
                return false;
            context->OMSetRenderTargets(1, backRtv.GetAddressOf(), mirrorDsv.Get());
            context->VSSetShader(plainVs.Get(), nullptr, 0);
            context->PSSetShader(scenePs.Get(), nullptr, 0);
            const D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
            context->RSSetViewports(1, &vp);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            if (keep)
                *keep = backRtv;
            return true;
        };
        const bool drawn = mirror_frame(nullptr);
        context->Flush();
        const HRESULT resized = chain ? chain->ResizeBuffers(0, 96, 96, DXGI_FORMAT_UNKNOWN, 0) : E_FAIL;
        ComPtr<ID3D11RenderTargetView> kept;
        mirror_frame(&kept);
        context->Flush();
        const HRESULT held = chain ? chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0) : S_OK;
        kept.Reset();
        context->Flush();
        const HRESULT released = chain ? chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0) : E_FAIL;
        lmu::mvr_frame_end(100031);
        lmu::mvr_track_passes(false);
        std::snprintf(what, sizeof(what),
                      "the mirror's back buffer bound with depth outside the passes, the tracker on: ResizeBuffers 0x%08lx "
                      "(the control, a view kept: 0x%08lx; let go: 0x%08lx)",
                      static_cast<unsigned long>(resized), static_cast<unsigned long>(held),
                      static_cast<unsigned long>(released));
        check(made && drawn && SUCCEEDED(resized) && FAILED(held) && SUCCEEDED(released), what);
        chain.Reset();
        if (hwnd)
            DestroyWindow(hwnd);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

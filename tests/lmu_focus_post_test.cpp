// Offline check of LMU MVR's focus post (games/lmu/focus_post.h, mvr_pass.h mvr_focus_replay) on a WARP device. An eye
// pass as LMU's runs on the hooked immediate context: the scene (with the focus view beside the eye) resolved, then
// YEBIS-like draws with stand-ins for its copy and composite pixel shaders (same inputs: the copy's texture at TEXCOORD0
// times a constant; the composite's scene copy, glare and 1x1 exposure at TEXCOORD0, 1 and 3), marked with their role as
// the probe marks the game's. The scene's focus region holds the same picture as the eye's top-left, and the focus box
// is exactly that crop, so the focus post's image must equal the eye's own composite over it: the copy's region, the
// constants as bound for the eye (the game rewrites its buffer right after), the glare resampled through the box and
// the exposure all show there. And the game's state after each step as it set it, nothing replayed outside passes 0-1.
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

int fake_two_views(ID3D11DeviceContext*, bool) { return 0; }

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
int main(int argc, char** argv) {
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
    check(lmu::mvr_pass_install(device.Get()) && lmu::mvr_pass_stats().contextEntries == 11,
          "the immediate context hooked, DrawIndexed among its entries (the focus post on)");

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

    ComPtr<ID3D11VertexShader> plainVs = vs(device.Get(), kPlainVs), yebisVs = vs(device.Get(), kYebisVs),
                               twinned = vs(device.Get(), kPlainVs), twin = vs(device.Get(), kPlainVs);
    twinned->SetPrivateDataInterface(lmu::kMvrTwinGuid, twin.Get());
    ComPtr<ID3D11PixelShader> scenePs = ps(device.Get(), kScenePs), glarePs = ps(device.Get(), kGlarePs),
                              copyPs = ps(device.Get(), kCopyPs, lmu::YebisRole::copy),
                              compositePs = ps(device.Get(), kCompositePs, lmu::YebisRole::composite);
    check(plainVs && yebisVs && twinned && scenePs && glarePs && copyPs && compositePs, "the stand-in shaders compiled");
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
    const float downConstants[15][4] = {{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {0.1f, 0.1f, 0.1f, 1}};
    float compositeConstants[21][4] = {};
    compositeConstants[20][0] = 1.7f;  // exposure gain
    compositeConstants[20][1] = 0.6f;  // glare
    const float eyeRect[4] = {0, 0, static_cast<float>(eyeW) / W, static_cast<float>(eyeH) / H};
    const D3D11_VIEWPORT eyeVp = {0, 0, static_cast<float>(eyeW), static_cast<float>(eyeH), 0, 1};

    // One eye pass as LMU's: the scene (the focus view drawn beside the eye: a twin bound on the depth target), the
    // resolve, then the post-processing's copy, a downsample, the composite.
    const Kit standIn = {copyPs.Get(),    compositePs.Get(),      copyCb.Get(),         compositeCb.Get(),
                         copyConstants,   sizeof(copyConstants),  downConstants,        sizeof(downConstants),
                         compositeConstants, sizeof(compositeConstants)};
    const auto eye_pass = [&](int pass, const lmu::MvrPassView* v, bool checkState, const Kit& k) {
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), dsv.Get());
        context->VSSetShader(twinned.Get(), nullptr, 0);
        lmu::mvr_pass_begin(pass, v, false);
        const D3D11_VIEWPORT whole = {0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->OMSetRenderTargets(1, msaaRtv.GetAddressOf(), nullptr);
        context->VSSetShader(plainVs.Get(), nullptr, 0);
        context->PSSetShader(scenePs.Get(), nullptr, 0);
        context->RSSetViewports(1, &whole);
        context->Draw(3, 0);
        context->ResolveSubresource(scene.Get(), 0, msaa.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);

        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->IASetInputLayout(nullptr);
        write(context.Get(), rectCb.Get(), eyeRect, sizeof(eyeRect));
        context->VSSetShader(yebisVs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(1, 1, rectCb.GetAddressOf());
        // The copy of the resolved scene.
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
        lmu::mvr_pass_end(pass);
        context->PSSetShaderResources(0, 3, std::vector<ID3D11ShaderResourceView*>(3, nullptr).data());
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
        check(fd.Width == focusW && fd.Height == focusH && fd.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
              "the focus image: the focus view's size, RGBA8 (as the eye's final)");
        const std::vector<uint8_t> mine = read_rgba8(device.Get(), context.Get(), focus, focusW, focusH);
        same_as_eye("stand-ins", 100, true);
        std::vector<uint8_t> bgra;
        unsigned bw = 0, bh = 0;
        bool swapped = simqvfr::d3d11_read_bgra8(focus, bgra, bw, bh) && bw == focusW && bh == focusH &&
                       bgra.size() == mine.size();
        for (size_t i = 0; swapped && i < mine.size(); i += 4)
            swapped = bgra[i] == mine[i + 2] && bgra[i + 1] == mine[i + 1] && bgra[i + 2] == mine[i] && bgra[i + 3] == 255;
        check(swapped, "the frame read-back (R2's comparison) takes the focus image: BGRA as it is, alpha opaque");
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

    {  // A composite the hooks don't know (another variant): the pass counted as without one, the draw into the resolve's
       // target logged; nothing composited.
        ComPtr<ID3D11PixelShader> unmarked = ps(device.Get(), kCompositePs);
        Kit other = standIn;
        other.composite = unmarked.Get();
        const lmu::MvrPassStats before = lmu::mvr_pass_stats();
        eye_pass(0, &view, false, other);
        const lmu::MvrPassStats after = lmu::mvr_pass_stats();
        check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites &&
                  after.focusNoComposite == before.focusNoComposite + 1 &&
                  after.focusUnknownComposites == before.focusUnknownComposites + 1,
              "an unknown composite: the pass counted without one, its draw into the resolve's target counted");
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
            const Kit real = {realCopy.Get(),  realComposite.Get(), realCopyCb.Get(),     realCompositeCb.Get(),
                              copyData.data(), copyData.size(),     zeros.data(),         zeros.size(),
                              compositeData.data(), compositeData.size()};
            if (infoQueue)
                infoQueue->ClearStoredMessages();
            const lmu::MvrPassStats before = lmu::mvr_pass_stats();
            eye_pass(0, &view, true, real);
            const lmu::MvrPassStats after = lmu::mvr_pass_stats();
            check(after.focusCopies == before.focusCopies + 1 && after.focusComposites == before.focusComposites + 1 &&
                      after.focusFailures == before.focusFailures,
                  "YEBIS's own shaders: its copy and composite replayed once each");
            same_as_eye("YEBIS's own shaders", 20, true);
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

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

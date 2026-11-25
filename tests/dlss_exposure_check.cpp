// Hardware check of what the lights' glow costs the rest of the picture (engine/dlss_views.h DlssRun::lightGlow: DLSS's
// exposure lowered). Not part of the suite (no _test suffix): it needs an NVIDIA RTX GPU, a build with the NGX SDK and a
// folder holding nvngx_dlss.dll.
//
//   sim_qvfr_tests dlss_exposure_check <folder with nvngx_dlss.dll> [frames]
//
// DLSS tonemaps its input by its exposure (its auto exposure times NGX's InExposureScale) before it filters: a lower
// exposure keeps very bright lights (lmu_glow_check). Here what it does to everything else: a night scene of fine
// detail (lines under a texel wide, a checker, rings up to the texel's frequency), lit windows far brighter than the
// rest and a light 0.3 texels across, drawn one sample a pixel at DLSS's jitter, DLAA after it, against the scene
// supersampled 8x8 without jitter. Still, panning slowly and panning fast (the vectors exact). Per exposure scale: the
// error against the reference outside the windows and inside them (display terms, x / (x + 0.05): the night's
// background about mid-grey), the flicker still (the frame-to-frame change; the reference has none), and the light's
// peak.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include "engine/dlss_views.h"
#include "engine/ngx_dlss.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::XMConvertHalfToFloat;

namespace {

constexpr unsigned kW = 1024, kH = 768;
constexpr int kBorder = 24;

constexpr char kShader[] = R"(
cbuffer C : register(b0) {
    float4 at;    // x, y: the pattern's offset (texels); z, w: this frame's jitter (texels, the sample's shift)
    float4 mode;  // x: 1 the reference (8x8 supersampled, no jitter)
};
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float pattern(float2 p) {
    float v = 0.02;
    const int2 c = int2(floor(p / 7));
    v += ((c.x + c.y) & 1) ? 0.03 : 0;                       // a checker, 7 texels
    if (frac(p.x / 13.0) * 13.0 < 0.35) v += 0.15;            // lines 0.35 texels wide
    if (frac((p.y + 0.3 * p.x) / 17.0) * 17.0 < 0.6) v += 0.08;
    const float2 d = p - float2(512, 384);
    const float r2 = dot(d, d);
    if (r2 < 300 * 300) v += 0.04 * (0.5 + 0.5 * cos(r2 * 0.0035));  // rings, finer outwards
    const float2 w = p - float2(150, 120);                     // lit windows, 6 x 3 texels, far brighter
    if (all(frac(w / float2(11, 9)) * float2(11, 9) < float2(6, 3)) && p.x > 150 && p.x < 400 && p.y > 120 && p.y < 260)
        v += 40;
    const float2 l = p - float2(700.4, 600.6);                 // a light 0.3 texels across
    if (dot(l, l) < 0.09) v += 300;
    return v;
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float v = 0;
    if (mode.x > 0) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                v += pattern(floor(pos.xy) + (float2(x, y) + 0.5) / 8 + at.xy);
        v /= 64;
    } else {
        v = pattern(pos.xy + at.zw + at.xy);
    }
    return float4(v, v, v, 1);
}
)";

struct Constants {
    float at[4];
    float mode[4];
};

double halton(int i, int b) {
    double f = 1, r = 0;
    for (; i > 0; i /= b) {
        f /= b;
        r += f * (i % b);
    }
    return r;
}

double display(double x) { return x / (x + 0.05); }

bool in_windows(unsigned x, unsigned y) { return x > 140 && x < 410 && y > 110 && y < 270; }
bool near_light(unsigned x, unsigned y) { return x > 690 && x < 710 && y > 590 && y < 610; }

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: sim_qvfr_tests dlss_exposure_check <folder with nvngx_dlss.dll> [frames]\n");
        return 2;
    }
    const std::wstring folder = argv[1];
    const int frames = argc > 2 ? (std::max)(60, _wtoi(argv[2])) : 160;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("no D3D11 hardware device\n");
        return 2;
    }
    std::string why;
    const std::wstring data = temp_path(L"sim-qvfr-exposure-check");
    CreateDirectoryW(data.c_str(), nullptr);
    if (!simqvfr::ngx::start(device.Get(), folder.c_str(), data.c_str(), why)) {
        std::printf("NGX didn't start: %s\n", why.c_str());
        return 2;
    }
    const auto texture = [&](DXGI_FORMAT format, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0) {
        D3D11_TEXTURE2D_DESC td{kW, kH, 1, 1, format, {1, 0}, usage, bind, cpu};
        ComPtr<ID3D11Texture2D> t;
        device->CreateTexture2D(&td, nullptr, &t);
        return t;
    };
    auto colour = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto reference = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET);
    auto output = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto vectors = texture(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto stagingOut = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    auto stagingRef = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    ComPtr<ID3D11RenderTargetView> colourRtv, referenceRtv, depthRtv, vectorsRtv;
    device->CreateRenderTargetView(colour.Get(), nullptr, &colourRtv);
    device->CreateRenderTargetView(reference.Get(), nullptr, &referenceRtv);
    device->CreateRenderTargetView(depth.Get(), nullptr, &depthRtv);
    device->CreateRenderTargetView(vectors.Get(), nullptr, &vectorsRtv);
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    D3D11_BUFFER_DESC bd{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    const bool made = colourRtv && referenceRtv && depthRtv && vectorsRtv && output && stagingOut && stagingRef &&
                      SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "exposure", nullptr, nullptr, "vs", "vs_5_0", 0,
                                           0, &vsCode, &errors)) &&
                      SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "exposure", nullptr, nullptr, "ps", "ps_5_0", 0,
                                           0, &psCode, &errors)) &&
                      SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
                      SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)) &&
                      SUCCEEDED(device->CreateBuffer(&bd, nullptr, &constants));
    check(made, "the scene's targets and shaders are made");
    if (!made) {
        if (errors)
            std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        simqvfr::ngx::shutdown(device.Get());
        return test_result();
    }
    // The setting's exposure scales (Off, Low, High) and a few around them.
    const simqvfr::DlssRun run;
    const double low = simqvfr::dlss_light_glow_scale(50, run.glowScaleLow, run.glowScaleHigh),
                 high = simqvfr::dlss_light_glow_scale(100, run.glowScaleLow, run.glowScaleHigh);
    const double scales[] = {1, 0.1, low, high, 0.003, 0.001};
    const double pans[] = {0, 0.37, 3.3};
    std::printf("     per DLSS exposure scale: error outside the windows (still, pan 0.37, pan 3.3), in the windows "
                "(still), flicker still, the 0.3-texel light's peak\n");
    double outside[std::size(scales)] = {}, inside[std::size(scales)] = {}, flick[std::size(scales)] = {};
    for (size_t si = 0; si < std::size(scales); ++si) {
        const double scale = scales[si];
        double err[3] = {}, win = 0, flicker = 0, light = 0;
        for (int pi = 0; pi < 3; ++pi) {
            unsigned result = 0;
            simqvfr::ngx::Feature* feature = simqvfr::ngx::create(context.Get(), kW, kH, kW, kH, false, 0, result);
            if (!feature) {
                std::printf("     the feature wasn't made (0x%08x)\n", result);
                break;
            }
            std::vector<float> before(static_cast<size_t>(kW) * kH, -1);
            double e = 0, w = 0, fl = 0, lp = 0;
            int counted = 0;
            for (int f = 0; f < frames; ++f) {
                const double jx = halton(f % 32 + 1, 2) - 0.5, jy = halton(f % 32 + 1, 3) - 0.5;
                const float clearDepth[4] = {0.5f, 0, 0, 0}, mv[4] = {static_cast<float>(pans[pi]), 0, 0, 0};
                context->ClearRenderTargetView(depthRtv.Get(), clearDepth);
                context->ClearRenderTargetView(vectorsRtv.Get(), mv);
                Constants c{};
                c.at[0] = static_cast<float>(pans[pi] * f);
                c.at[2] = static_cast<float>(jx), c.at[3] = static_cast<float>(jy);
                const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kW), static_cast<float>(kH), 0, 1};
                context->RSSetViewports(1, &vp);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->PSSetShader(ps.Get(), nullptr, 0);
                context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
                for (int pass = 0; pass < 2; ++pass) {
                    c.mode[0] = static_cast<float>(pass);
                    context->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
                    context->OMSetRenderTargets(1, pass ? referenceRtv.GetAddressOf() : colourRtv.GetAddressOf(), nullptr);
                    context->Draw(3, 0);
                }
                context->OMSetRenderTargets(0, nullptr, nullptr);
                simqvfr::ngx::Evaluation ev;
                ev.colour = colour.Get();
                ev.depth = depth.Get();
                ev.vectors = vectors.Get();
                ev.output = output.Get();
                ev.width = kW, ev.height = kH;
                // The sample sits at the pixel's centre + (jx, jy): NGX's jitter is the projection's offset (opposite).
                ev.jitterX = static_cast<float>(-jx), ev.jitterY = static_cast<float>(-jy);
                ev.reset = f == 0;
                ev.exposureScale = static_cast<float>(scale);
                if (!simqvfr::ngx::evaluate(context.Get(), feature, ev, result)) {
                    std::printf("     the evaluation failed (0x%08x)\n", result);
                    break;
                }
                if (f < frames / 3)
                    continue;
                context->CopyResource(stagingOut.Get(), output.Get());
                context->CopyResource(stagingRef.Get(), reference.Get());
                D3D11_MAPPED_SUBRESOURCE mo, mr;
                if (FAILED(context->Map(stagingOut.Get(), 0, D3D11_MAP_READ, 0, &mo)))
                    break;
                if (FAILED(context->Map(stagingRef.Get(), 0, D3D11_MAP_READ, 0, &mr))) {
                    context->Unmap(stagingOut.Get(), 0);
                    break;
                }
                double sum = 0, winSum = 0, diff = 0;
                long long n = 0, winN = 0;
                for (unsigned y = kBorder; y < kH - kBorder; ++y) {
                    const auto* ro = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mo.pData) + y * mo.RowPitch);
                    const auto* rr = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mr.pData) + y * mr.RowPitch);
                    for (unsigned x = kBorder; x < kW - kBorder; ++x) {
                        const double o = XMConvertHalfToFloat(ro[x * 4]), r = XMConvertHalfToFloat(rr[x * 4]);
                        const double d = display(o) - display(r);
                        if (in_windows(x, y)) {
                            winSum += d * d, ++winN;
                        } else if (!near_light(x, y)) {
                            sum += d * d, ++n;
                            float& b = before[static_cast<size_t>(y) * kW + x];
                            if (b >= 0)
                                diff += std::abs(display(o) - display(b));
                            b = static_cast<float>(o);
                        }
                    }
                }
                if (pi == 0) {  // the light's peak (its 3x3)
                    double peak = 0;
                    for (int y = 599; y <= 601; ++y) {
                        const auto* ro = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mo.pData) + y * mo.RowPitch);
                        for (int x = 699; x <= 701; ++x)
                            peak = (std::max)(peak, static_cast<double>(XMConvertHalfToFloat(ro[x * 4])));
                    }
                    lp += peak;
                }
                context->Unmap(stagingOut.Get(), 0);
                context->Unmap(stagingRef.Get(), 0);
                e += std::sqrt(sum / (std::max)(1LL, n));
                w += std::sqrt(winSum / (std::max)(1LL, winN));
                if (f > frames / 3)
                    fl += diff / (std::max)(1LL, n);
                ++counted;
            }
            simqvfr::ngx::release(feature);
            err[pi] = counted ? e / counted : -1;
            if (pi == 0) {
                win = counted ? w / counted : -1;
                flicker = counted > 1 ? fl / (counted - 1) : -1;
                light = counted ? lp / counted : 0;
            }
        }
        outside[si] = err[0], inside[si] = win, flick[si] = flicker;
        std::printf("     x%-8.4f%s  %8.5f %8.5f %8.5f   %8.5f   %8.5f   %7.2f\n", scale,
                    scale == low ? " (Low) " : scale == high ? " (High)" : scale == 1 ? " (Off) " : "       ", err[0],
                    err[1], err[2], win, flicker, light);
    }
    // The settings' scales against DLSS's own (measured 2026-11-22: lowering the exposure to x0.003 made the night's
    // fine detail closer to the reference, not further, and flickered about as much; x0.001 flickered five times as much).
    check(outside[2] <= outside[0] * 1.1 && outside[3] <= outside[0] * 1.1,
          "Low and High: the fine detail as close to the reference as DLSS's own exposure (within 10%)");
    check(flick[2] <= flick[0] * 1.5 && flick[3] <= flick[0] * 1.5,
          "Low and High: the fine detail flickers at most half again as much as at DLSS's own exposure");
    std::printf("     bright windows: x1 %.5f, Low %.5f, High %.5f (a lower exposure blends their edges in linear light: "
                "a texel's halo)\n", inside[0], inside[2], inside[3]);
    simqvfr::ngx::shutdown(device.Get());
    return test_result();
}

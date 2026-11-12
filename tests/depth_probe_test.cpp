// Offline check of the depth probe (quad/d3d11_depth_probe.h) on a WARP device: a depth texture cleared to a known
// depth reads back as that depth at every point (24-bit and 32-bit depth), a frame or more later, without stalling; a
// depth texture a shader can't read is refused.
#include <cmath>
#include <cstdio>

#include <d3d11.h>
#include <wrl/client.h>

#include "quad/d3d11_depth_probe.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11Texture2D> depth_texture(ID3D11Device* device, DXGI_FORMAT typeless, DXGI_FORMAT dsvFormat, bool readable,
                                      ID3D11DeviceContext* ctx, float clear) {
    D3D11_TEXTURE2D_DESC d{64, 48, 1, 1, typeless, {1, 0}, D3D11_USAGE_DEFAULT,
                           D3D11_BIND_DEPTH_STENCIL | (readable ? D3D11_BIND_SHADER_RESOURCE : 0u)};
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &t)))
        return nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC v{dsvFormat, D3D11_DSV_DIMENSION_TEXTURE2D};
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(device->CreateDepthStencilView(t.Get(), &v, &dsv)))
        return nullptr;
    ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, clear, 0);
    return t;
}

// Requests until one comes back (the GPU flushed between tries).
bool read_back(simqvfr::D3D11DepthProbe& probe, ID3D11DeviceContext* ctx, float (&values)[simqvfr::D3D11DepthProbe::kPoints],
               int& age) {
    for (int i = 0; i < 200; ++i) {
        ctx->Flush();
        if (probe.latest(values, age))
            return true;
        Sleep(5);
    }
    return false;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &ctx))) {
        std::printf("FAIL no WARP device\nRESULT: FAIL\n");
        return 1;
    }
    simqvfr::D3D11DepthProbe probe;
    check(probe.initialize(device.Get()), "the probe starts on a WARP device");
    const int points[simqvfr::D3D11DepthProbe::kPoints][2] = {{0, 0},   {63, 47}, {32, 24}, {10, 40}, {50, 5},
                                                             {-5, -5}, {99, 99}, {1, 46},  {62, 1}};
    float values[simqvfr::D3D11DepthProbe::kPoints] = {};
    int age = -1;

    auto d24 = depth_texture(device.Get(), DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, true, ctx.Get(), 0.25f);
    const bool queued = d24 && probe.request(d24.Get(), points);
    const bool came = queued && read_back(probe, ctx.Get(), values, age);
    bool all = came;
    for (float v : values)
        all &= std::fabs(v - 0.25f) < 1e-5f;
    check(all && age >= 0, "a 24-bit depth cleared to 0.25: 0.25 at every point (outside points clamped to the edge)");

    auto d32 = depth_texture(device.Get(), DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, true, ctx.Get(), 0.0078125f);
    const bool queued32 = d32 && probe.request(d32.Get(), points);
    const bool came32 = queued32 && read_back(probe, ctx.Get(), values, age);
    bool all32 = came32;
    for (float v : values)
        all32 &= v == 0.0078125f;
    check(all32, "a 32-bit float depth: exact");

    float none[simqvfr::D3D11DepthProbe::kPoints];
    int noAge = 0;
    check(!probe.latest(none, noAge), "nothing new after the last read was handed out");

    auto hidden = depth_texture(device.Get(), DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, false, ctx.Get(), 0.5f);
    check(hidden && !probe.request(hidden.Get(), points), "a depth texture without a shader binding: refused");

    probe.shutdown();
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

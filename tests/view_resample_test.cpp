// Offline check of the SSR resample (quad/d3d11_view_resample.h) on a WARP device, and of ACC's viewport-to-view
// matching (games/acc/view_share.h). The source holds an eye's region whose texels store their own coordinates;
// each texel of the focus region must read the eye's coordinates in its direction (straight ahead and turned),
// nothing outside the focus region may change, and the game's compute state must be as it was.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "games/acc/view_share.h"
#include "quad/d3d11_view_resample.h"
#include "quad/focus_turn.h"

using Microsoft::WRL::ComPtr;
using namespace aceqv;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

constexpr UINT kW = 128, kH = 160;  // eye region at 0,0 128x96; focus region at 0,96 128x64 (eye columns)
const ViewRegion kEye{0, 0, 128, 96}, kFocus{0, 96, 128, 64};

uint16_t half(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000;
    int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
    std::uint32_t m = x & 0x7fffff;
    if (e <= 0)
        return static_cast<uint16_t>(sign);
    return static_cast<uint16_t>(sign | (e << 10) | ((m + 0x1000) >> 13));
}
float from_half(uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const int e = (h >> 10) & 0x1f;
    const std::uint32_t m = h & 0x3ff;
    if (e == 0)
        return 0.0f;
    const std::uint32_t x = sign | static_cast<std::uint32_t>(e - 15 + 127) << 23 | m << 13;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// Where focus texel (i, j) looks in the eye's region, in texels (the shader's math, on the CPU).
void expected(const ViewMapping& m, int i, int j, float& px, float& py) {
    const float fx = (i + 0.5f) / kFocus.width, fy = (j + 0.5f) / kFocus.height;
    const float a[3] = {m.focusTan[0] + (m.focusTan[1] - m.focusTan[0]) * fx,
                        m.focusTan[2] + (m.focusTan[3] - m.focusTan[2]) * fy, -1};
    float b[3];
    for (int r = 0; r < 3; ++r)
        b[r] = m.rotation[r * 3] * a[0] + m.rotation[r * 3 + 1] * a[1] + m.rotation[r * 3 + 2] * a[2];
    const float tx = b[0] / -b[2], ty = b[1] / -b[2];
    const float ox = (tx - m.eyeTan[0]) / (m.eyeTan[1] - m.eyeTan[0]), oy = (m.eyeTan[2] - ty) / (m.eyeTan[2] - m.eyeTan[3]);
    px = kEye.x + std::fmin(std::fmax(ox * kEye.width, 0.5f), kEye.width - 0.5f);
    py = kEye.y + std::fmin(std::fmax(oy * kEye.height, 0.5f), kEye.height - 0.5f);
}

}  // namespace

int main() {
    {
        const int rects[4][4] = {{0, 0, 1816, 1519}, {1816, 0, 1816, 1519}, {0, 1520, 1816, 1013}, {1816, 1520, 1816, 1013}};
        bool have[4] = {true, true, true, true};
        check(acc::view_at(rects, have, 0, 0, 1816, 1519) == 0 && acc::view_at(rects, have, 1816, 0, 1816, 1519) == 1 &&
                  acc::view_at(rects, have, 0, 1520, 1816, 1013) == 2 &&
                  acc::view_at(rects, have, 1816, 1520, 1816, 1013) == 3,
              "each view's viewport is that view");
        check(acc::view_at(rects, have, 0, 0, 1024, 550) == -1, "another family's viewport is no view");
        have[2] = false;
        check(acc::view_at(rects, have, 0, 1520, 1816, 1013) == -1, "an unreported view is never matched");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::puts("no WARP D3D11 device");
        return 2;
    }
    // Source: the eye's texels hold (x, y, 7, 1); the rest of it 0. Destination: everything 5.
    std::vector<uint16_t> src(kW * kH * 4, 0), dst(kW * kH * 4, half(5));
    for (int y = 0; y < kEye.height; ++y)
        for (int x = 0; x < kEye.width; ++x) {
            uint16_t* p = &src[(y * kW + x) * 4];
            p[0] = half(static_cast<float>(x));
            p[1] = half(static_cast<float>(y));
            p[2] = half(7);
            p[3] = half(1);
        }
    D3D11_TEXTURE2D_DESC desc{kW, kH, 1, 1, DXGI_FORMAT_R16G16B16A16_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS};
    D3D11_SUBRESOURCE_DATA si{src.data(), kW * 8}, di{dst.data(), kW * 8};
    ComPtr<ID3D11Texture2D> source, target, readback;
    device->CreateTexture2D(&desc, &si, &source);
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateTexture2D(&desc, &di, &target);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&desc, nullptr, &readback);

    D3D11ViewResample resample;
    check(resample.initialize(device.Get()), "the resample starts on WARP");

    // The game's compute state: a UAV of its own on u0, which must be there afterwards.
    ComPtr<ID3D11UnorderedAccessView> gameUav;
    device->CreateUnorderedAccessView(target.Get(), nullptr, &gameUav);
    context->CSSetUnorderedAccessViews(0, 1, gameUav.GetAddressOf(), nullptr);

    const XrQuaternionf turns[2] = {{0, 0, 0, 1}, {0.0f, 0.130526f, 0.0f, 0.991445f}};  // straight; 15 deg yaw left
    const char* names[2] = {"straight ahead", "turned 15 deg"};
    for (int k = 0; k < 2; ++k) {
        ViewMapping m{{-0.4f, 0.4f, 0.3f, -0.3f}, {-1.2f, 1.0f, 1.0f, -1.0f}, {}};
        to_matrix(turns[k], m.rotation);
        check(resample.resample(source.Get(), kEye, target.Get(), kFocus, m), "a resample is recorded and run");
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        float worst = 0;
        bool outsideKept = true, constant = true;
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const auto* p = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch) + x * 4;
                const bool inFocus = static_cast<int>(y) >= kFocus.y && static_cast<int>(y) < kFocus.y + kFocus.height;
                if (!inFocus) {
                    outsideKept = outsideKept && from_half(p[0]) == 5 && from_half(p[3]) == 5;
                    continue;
                }
                float ex, ey;
                expected(m, static_cast<int>(x) - kFocus.x, static_cast<int>(y) - kFocus.y, ex, ey);
                // Bilinear over texels holding their own coordinates: the sample point less half a texel.
                worst = std::fmax(worst, std::fmax(std::fabs(from_half(p[0]) - (ex - 0.5f)), std::fabs(from_half(p[1]) - (ey - 0.5f))));
                constant = constant && from_half(p[2]) == 7 && from_half(p[3]) == 1;
            }
        context->Unmap(readback.Get(), 0);
        char what[128];
        std::snprintf(what, sizeof(what), "%s: each focus texel reads its direction in the eye (worst %.3f texels)", names[k], worst);
        check(worst < 0.1f, what);
        check(constant, "the eye's other channels come through");
        check(outsideKept, "nothing outside the focus region changes");
    }
    ComPtr<ID3D11UnorderedAccessView> after;
    context->CSGetUnorderedAccessViews(0, 1, &after);
    check(after.Get() == gameUav.Get(), "the game's compute UAV is bound as before");

    // The render-target path, as ACC's AO: one R8 texture holding both regions, drawn into its own focus region
    // from its eye region. Eye texels hold their x (as x / 255); the focus region starts at 200.
    {
        std::vector<uint8_t> ao(kW * kH, 200);
        for (int y = 0; y < kEye.height; ++y)
            for (int x = 0; x < kEye.width; ++x)
                ao[y * kW + x] = static_cast<uint8_t>(x);
        D3D11_TEXTURE2D_DESC d{kW, kH, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
        D3D11_SUBRESOURCE_DATA init{ao.data(), kW};
        ComPtr<ID3D11Texture2D> tex, back;
        device->CreateTexture2D(&d, &init, &tex);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&d, nullptr, &back);
        // The game's draw state, which must be as it was.
        ComPtr<ID3D11RenderTargetView> gameRtv;
        device->CreateRenderTargetView(tex.Get(), nullptr, &gameRtv);
        context->OMSetRenderTargets(1, gameRtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT gameVp{3, 4, 50, 60, 0, 1};
        context->RSSetViewports(1, &gameVp);
        ViewMapping m{{-0.4f, 0.4f, 0.3f, -0.3f}, {-1.2f, 1.0f, 1.0f, -1.0f}, {}};
        to_matrix(turns[1], m.rotation);
        check(resample.resample_target(tex.Get(), kEye, tex.Get(), kFocus, m), "a render-target resample is recorded and run");
        context->CopyResource(back.Get(), tex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        context->Map(back.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        float worst = 0;
        bool eyeKept = true;
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const float v = static_cast<const uint8_t*>(mapped.pData)[y * mapped.RowPitch + x];
                if (static_cast<int>(y) < kFocus.y) {
                    eyeKept = eyeKept && v == static_cast<float>(x);
                    continue;
                }
                float ex, ey;
                expected(m, static_cast<int>(x) - kFocus.x, static_cast<int>(y) - kFocus.y, ex, ey);
                worst = std::fmax(worst, std::fabs(v - (ex - 0.5f)));
            }
        context->Unmap(back.Get(), 0);
        char what[128];
        std::snprintf(what, sizeof(what), "render target, turned: each focus texel reads its eye's (worst %.2f of 255)", worst);
        check(worst < 1.01f, what);  // 8-bit rounding
        check(eyeKept, "the eye's region is left as it was");
        ComPtr<ID3D11RenderTargetView> rtvAfter;
        context->OMGetRenderTargets(1, &rtvAfter, nullptr);
        D3D11_VIEWPORT vpAfter{};
        UINT n = 1;
        context->RSGetViewports(&n, &vpAfter);
        check(rtvAfter.Get() == gameRtv.Get() && n == 1 && vpAfter.TopLeftX == 3 && vpAfter.Width == 50,
              "the game's render target and viewport are bound as before");
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// Offline check of the D3D11 compositor (quad/d3d11_compositor.h) on a WARP device: one image holding four views
// in eye columns (outer views on top, focus views below), each eye's outer and focus view a colour of its own,
// composed into a double-wide output. Each eye must show its own outer view at its edges and its own focus view
// in the middle (the focus box, centred), and the game's pipeline state must be as it was.
#include <cstdio>
#include <cstring>

#include <d3d11.h>
#include <wrl/client.h>

#include "quad/d3d11_compositor.h"

using Microsoft::WRL::ComPtr;
using namespace aceqv;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

constexpr UINT kW = 256, kH = 192;          // the views' image: two eye columns of 128, outer 128 high, focus 64
constexpr UINT kOutW = 2 * 96, kOutH = 96;  // the output: two eyes of 96 x 96
// RGBA8: outer left red, outer right blue, focus left green, focus right white.
constexpr uint32_t kOuter[2] = {0xff0000ff, 0xffff0000}, kFocus[2] = {0xff00ff00, 0xffffffff};

}  // namespace

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::puts("no WARP D3D11 device");
        return 2;
    }
    std::vector<uint32_t> pixels(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            pixels[y * kW + x] = y < 128 ? kOuter[x / 128] : kFocus[x / 128];
    D3D11_TEXTURE2D_DESC desc{kW, kH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    D3D11_SUBRESOURCE_DATA init{pixels.data(), kW * 4};
    ComPtr<ID3D11Texture2D> views, output, readback;
    device->CreateTexture2D(&desc, &init, &views);
    desc.Width = kOutW;
    desc.Height = kOutH;
    device->CreateTexture2D(&desc, nullptr, &output);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&desc, nullptr, &readback);

    D3D11Compositor compositor;
    check(compositor.initialize(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM), "the compositor starts on WARP");

    // The game's state: a viewport of its own, which the composition must leave as it was.
    const D3D11_VIEWPORT gameViewport{3, 4, 50, 60, 0, 1};
    context->RSSetViewports(1, &gameViewport);

    EyeComposition eyes[2] = {};
    for (int e = 0; e < 2; ++e) {
        const float outer[4] = {-1, 1, 1, -1}, focus[4] = {-0.4f, 0.4f, 0.4f, -0.4f};  // a 40% box, centred
        std::memcpy(eyes[e].outerTan, outer, sizeof(outer));
        std::memcpy(eyes[e].focusTan, focus, sizeof(focus));
        eyes[e].rotation[0] = eyes[e].rotation[4] = eyes[e].rotation[8] = 1;
        const float outerSrc[4] = {e * 0.5f, 0, 0.5f, 128.0f / kH}, focusSrc[4] = {e * 0.5f, 128.0f / kH, 0.5f, 64.0f / kH};
        std::memcpy(eyes[e].outerSource, outerSrc, sizeof(outerSrc));
        std::memcpy(eyes[e].focusSource, focusSrc, sizeof(focusSrc));
        eyes[e].feather = 0.1f;
    }
    check(compositor.compose(views.Get(), output.Get(), eyes), "compose");

    UINT count = 1;
    D3D11_VIEWPORT after{};
    context->RSGetViewports(&count, &after);
    check(count == 1 && std::memcmp(&after, &gameViewport, sizeof(after)) == 0, "the game's viewport is as it was");

    context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    const auto at = [&](UINT x, UINT y) { return static_cast<const uint32_t*>(mapped.pData)[y * mapped.RowPitch / 4 + x]; };
    for (int e = 0; e < 2; ++e) {
        const UINT x0 = e * kOutW / 2, mid = x0 + kOutW / 4;
        char what[96];
        std::snprintf(what, sizeof(what), "eye %d: its focus view in the middle (0x%08x)", e, at(mid, kOutH / 2));
        check(at(mid, kOutH / 2) == kFocus[e], what);
        std::snprintf(what, sizeof(what), "eye %d: its outer view at the edges (0x%08x, 0x%08x)", e, at(x0 + 2, 2),
                      at(x0 + kOutW / 2 - 3, kOutH - 3));
        check(at(x0 + 2, 2) == kOuter[e] && at(x0 + kOutW / 2 - 3, kOutH - 3) == kOuter[e], what);
        // The box spans tangents +-0.4 of +-1: 40% of the eye, centred; outside it plus the feather, the outer view.
        std::snprintf(what, sizeof(what), "eye %d: the outer view just outside the box (0x%08x)", e,
                      at(x0 + kOutW / 2 * 27 / 100, kOutH / 2));
        check(at(x0 + kOutW / 2 * 27 / 100, kOutH / 2) == kOuter[e], what);
    }
    context->Unmap(readback.Get(), 0);
    compositor.shutdown();

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

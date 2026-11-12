// Offline check of the panels' upload on D3D11 (quad/menu_panel.h make_panel_upload) on a WARP device: a panel's
// pixels land in a swapchain-like texture as drawn, and a session without a device gets no uploader.
#include <cstdio>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "quad/menu_panel.h"

using Microsoft::WRL::ComPtr;
using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    check(!make_panel_upload(SessionGraphics{}, 64, 8), "no device: no uploader");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::puts("no WARP D3D11 device");
        return 2;
    }
    constexpr UINT kW = 64, kH = 8;
    D3D11_TEXTURE2D_DESC desc{kW, kH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> image, readback;
    device->CreateTexture2D(&desc, nullptr, &image);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&desc, nullptr, &readback);

    SessionGraphics graphics;
    graphics.d3d11 = device.Get();
    LUID luid{};
    check(graphics.valid() && graphics.adapter_luid(luid), "a D3D11 session is valid and has an adapter");
    const std::unique_ptr<PanelUpload> upload = make_panel_upload(graphics, kW, kH);
    check(upload != nullptr, "a D3D11 session gets an uploader");
    std::vector<uint32_t> pixels(kW * kH);
    for (UINT i = 0; i < pixels.size(); ++i)
        pixels[i] = 0xff000000u | (i * 2654435761u >> 8);
    check(upload && upload->upload(image.Get(), pixels), "upload");
    check(upload && !upload->upload(image.Get(), std::vector<uint32_t>(3)), "a panel of the wrong size is refused");

    context->CopyResource(readback.Get(), image.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    bool same = SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; same && y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            same &= static_cast<const uint32_t*>(mapped.pData)[y * mapped.RowPitch / 4 + x] == pixels[y * kW + x];
    if (mapped.pData)
        context->Unmap(readback.Get(), 0);
    check(same, "the image holds the panel's pixels as drawn");

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

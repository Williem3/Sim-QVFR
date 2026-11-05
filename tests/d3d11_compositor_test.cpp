// Offline check of the D3D11 compositor (quad/d3d11_compositor.h) on a WARP device: one image holding four views
// in eye columns (outer views on top, focus views below), each eye's outer and focus view a colour of its own,
// composed into a double-wide output. Each eye must show its own outer view at its edges and its own focus view
// in the middle (the focus box, centred), and the game's pipeline state must be as it was. With the FOV crop
// (outputSize) each eye is drawn into the top left of its half only. The helmet: its built-in look lit by the
// frame's light, the light read back, the loaded pieces, and its own layer (only its bands).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "quad/d3d11_compositor.h"

using Microsoft::WRL::ComPtr;
using namespace simqvfr;

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

// A 1-pixel-wide PNG of `rows` RGBA rows (0xRRGGBBAA, straight alpha), for the helmet's pieces.
bool write_png(const std::wstring& path, const std::vector<uint32_t>& rows) {
    const UINT height = static_cast<UINT>(rows.size());
    std::vector<uint8_t> bgra(height * 4);
    for (UINT y = 0; y < height; ++y) {
        const uint32_t c = rows[y];
        bgra[y * 4 + 0] = static_cast<uint8_t>(c >> 8);
        bgra[y * 4 + 1] = static_cast<uint8_t>(c >> 16);
        bgra[y * 4 + 2] = static_cast<uint8_t>(c >> 24);
        bgra[y * 4 + 3] = static_cast<uint8_t>(c);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(1, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WritePixels(height, 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// A copy of `texture` (RGBA8) as rows of pixels.
struct Image {
    std::vector<uint32_t> px;
    UINT width = 0;
    uint32_t at(UINT x, UINT y) const { return px[y * width + x]; }
    uint8_t channel(UINT x, UINT y, int c) const { return static_cast<uint8_t>(at(x, y) >> (8 * c)); }
};
Image read_image(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Image image;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return image;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return image;
    image.width = desc.Width;
    image.px.resize(static_cast<size_t>(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(&image.px[static_cast<size_t>(y) * desc.Width],
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    return image;
}

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

    // The FOV crop: each eye drawn into the top left of its half (outputSize, 75% x 50%); the rest of the half is
    // left as it was (cleared here to black), the drawn part as before.
    const float black[4] = {0, 0, 0, 0};
    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateRenderTargetView(output.Get(), nullptr, &rtv);
    context->ClearRenderTargetView(rtv.Get(), black);
    for (int e = 0; e < 2; ++e) {
        eyes[e].outputSize[0] = 0.75f;
        eyes[e].outputSize[1] = 0.5f;
    }
    check(compositor.compose(views.Get(), output.Get(), eyes), "compose, cropped");
    context->CopyResource(readback.Get(), output.Get());
    context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    for (int e = 0; e < 2; ++e) {
        const UINT x0 = e * kOutW / 2, w = kOutW / 2 * 3 / 4, h = kOutH / 2;
        char what[128];
        std::snprintf(what, sizeof(what), "eye %d cropped: drawn in its top left (0x%08x, 0x%08x)", e, at(x0 + 2, 2),
                      at(x0 + w - 2, h - 2));
        check(at(x0 + 2, 2) == kOuter[e] && at(x0 + w - 2, h - 2) == kOuter[e], what);
        std::snprintf(what, sizeof(what), "eye %d cropped: its focus view in the drawn part's middle (0x%08x)", e,
                      at(x0 + w / 2, h / 2));
        check(at(x0 + w / 2, h / 2) == kFocus[e], what);
        std::snprintf(what, sizeof(what), "eye %d cropped: nothing right of or below it (0x%08x, 0x%08x)", e,
                      at(x0 + w + 1, 2), at(x0 + 2, h + 1));
        check(at(x0 + w + 1, 2) == 0 && at(x0 + 2, h + 1) == 0, what);
    }
    context->Unmap(readback.Get(), 0);

    // The helmet's built-in look: eye 0's output covers tangents -2..2, the game rendered -1..1 (the opening).
    // Inside the opening past the rim the view is untouched; the lip just inside its edge and the shell outside it
    // take the frame's light (eye 0's outer view is red), the lip brighter; eye 1 stays as it was.
    for (int e = 0; e < 2; ++e)
        eyes[e].outputSize[0] = eyes[e].outputSize[1] = 0;
    const float wide[4] = {-2, 2, 2, -2}, opening[4] = {-1, 1, 1, -1};
    std::memcpy(eyes[0].viewTan, wide, sizeof(wide));
    std::memcpy(eyes[0].helmetOpening, opening, sizeof(opening));
    eyes[0].helmetRim = 0.1f;
    eyes[0].helmetLight = 1;
    eyes[0].helmet = 1;
    float light[2][4][3] = {};
    check(!compositor.read_light(light), "helmet light: nothing to read before it is asked for");
    compositor.request_light();
    check(compositor.compose(views.Get(), output.Get(), eyes), "compose with the helmet");
    {
        const Image hq = read_image(device.Get(), context.Get(), output.Get());
        const UINT half = kOutW / 2;
        check(hq.at(half / 2, kOutH / 2) == kFocus[0], "helmet: eye 0's focus view in the middle, untouched");
        check(hq.at(static_cast<UINT>(0.30f * half), kOutH / 2) == kOuter[0], "helmet: inside the opening, the scene");
        const UINT lipX = static_cast<UINT>(0.252f * half), shellX = static_cast<UINT>(0.05f * half);
        const uint8_t lipR = hq.channel(lipX, kOutH / 2, 0), shellR = hq.channel(shellX, kOutH / 2, 0);
        const bool lit = lipR > 40 && lipR < 200 && hq.channel(lipX, kOutH / 2, 1) < 12 && hq.channel(lipX, kOutH / 2, 2) < 12 &&
                         shellR > 5 && shellR < lipR && hq.channel(shellX, kOutH / 2, 1) < 8;
        char what[96];
        std::snprintf(what, sizeof(what), "helmet: lip red %u brighter than the shell's %u, both in red light", lipR, shellR);
        check(lit, what);
        check(hq.at(half + half / 2, kOutH / 2) == kFocus[1], "helmet: eye 1 as it was");
    }
    // The light for the log: eye 0's sides red, eye 1's blue (its outer view), read once the GPU has copied it.
    bool readLight = false;
    for (int i = 0; i < 200 && !readLight; ++i) {
        context->Flush();
        readLight = compositor.read_light(light);
        if (!readLight)
            Sleep(1);
    }
    bool sides = readLight;
    for (int side = 0; side < 4; ++side)
        sides = sides && std::fabs(light[0][side][0] - 1) < 0.01f && light[0][side][2] < 0.01f &&
                std::fabs(light[1][side][2] - 1) < 0.01f && light[1][side][0] < 0.01f;
    check(sides && !compositor.read_light(light), "helmet light read back once: eye 0 red, eye 1 blue at every side");
    // The light diagnostic: the shell shows the frame's light itself.
    eyes[0].helmet = 3;
    check(compositor.compose(views.Get(), output.Get(), eyes), "compose with the helmet's light diagnostic");
    check(read_image(device.Get(), context.Get(), output.Get()).at(static_cast<UINT>(0.05f * kOutW / 2), kOutH / 2) ==
              kOuter[0],
          "helmet light diagnostic: the shell is the frame's light (red)");

    // The pieces (light 0: as drawn) over the whole output (tangents -2..2, no turn), 8 rows each: the top
    // piece's first quarter white, the rest clear; the scene through the clear part.
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring top = std::wstring(tempDir) + L"simqvfr-d3d11-helmet-top.png";
    const uint32_t clear = 0xFFFFFF00;
    const bool loaded = write_png(top, {0xFFFFFFFF, 0xFFFFFFFF, clear, clear, clear, clear, clear, clear}) &&
                        compositor.load_helmet_piece(HelmetPiece::Top, top);
    check(loaded && compositor.has_helmet_pieces() && std::fabs(compositor.helmet_edge(HelmetPiece::Top) - 0.25) < 1e-9 &&
              compositor.helmet_aspect() == 8,
          "helmet pieces: loaded, the top piece's opaque edge found down the middle");
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(eyes[0].helmetRotation, identity, sizeof(identity));
    eyes[0].helmetTanHalf[0] = eyes[0].helmetTanHalf[1] = 2;
    eyes[0].helmetLight = 0;
    eyes[0].helmet = 2;
    check(compositor.compose(views.Get(), output.Get(), eyes), "compose with the helmet's pieces");
    {
        const Image pq = read_image(device.Get(), context.Get(), output.Get());
        const UINT x = static_cast<UINT>(0.30f * kOutW / 2);
        check(pq.at(x, static_cast<UINT>(0.05f * kOutH)) == 0xffffffff, "helmet pieces: the top piece at the top");
        check(pq.at(x, static_cast<UINT>(0.60f * kOutH)) == kOuter[0], "helmet pieces: the scene through the clear part");
    }

    // The helmet's own layer (light 0: as drawn), the quad over exactly the images: the top piece white and
    // opaque, the clear middle transparent; then only a band drawn, the rest of the image left as it was.
    D3D11_TEXTURE2D_DESC layerDesc{64, 64, 1, 1, kHelmetLayerFormat, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> layer;
    device->CreateTexture2D(&layerDesc, nullptr, &layer);
    HelmetLayerDraw draw{};
    draw.coverTan[0] = draw.coverTan[1] = draw.imageTan[0] = draw.imageTan[1] = 1;
    std::memcpy(draw.outerSource[0], eyes[0].outerSource, sizeof(draw.outerSource[0]));
    std::memcpy(draw.outerSource[1], eyes[1].outerSource, sizeof(draw.outerSource[1]));
    check(compositor.draw_helmet_layer(views.Get(), layer.Get(), draw), "the helmet layer");
    {
        const Image lq = read_image(device.Get(), context.Get(), layer.Get());
        check(lq.at(32, 6) == 0xffffffff && lq.at(32, 42) == 0, "helmet layer: the top piece opaque white, the middle clear");
    }
    ComPtr<ID3D11RenderTargetView> layerRtv;
    device->CreateRenderTargetView(layer.Get(), nullptr, &layerRtv);
    const float magenta[4] = {1, 0, 1, 1};
    context->ClearRenderTargetView(layerRtv.Get(), magenta);
    draw.bands = {{0, 0.25f}};
    check(compositor.draw_helmet_layer(views.Get(), layer.Get(), draw), "the banded helmet layer");
    {
        const Image bq = read_image(device.Get(), context.Get(), layer.Get());
        check(bq.at(32, 4) == 0xffffffff && bq.at(32, 48) == 0xffff00ff,
              "helmet layer: only its band drawn (below it the image is untouched)");
    }
    check(compositor.load_helmet_piece(HelmetPiece::Top, L"") && !compositor.has_helmet_pieces() &&
              !compositor.draw_helmet_layer(views.Get(), layer.Get(), draw),
          "helmet pieces dropped: no layer drawn");
    DeleteFileW(top.c_str());
    compositor.shutdown();

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

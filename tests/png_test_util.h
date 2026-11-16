// A test helper of the compositor tests (compositor_test.cpp, d3d11_compositor_test.cpp), in an anonymous namespace as
// test_util.h's: every test is compiled into the one test program.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace {

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
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
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

}  // namespace

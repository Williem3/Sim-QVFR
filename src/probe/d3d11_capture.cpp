#include "d3d11_capture.h"

#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../log.h"

namespace simqvfr {
namespace {

std::mutex mutex;
ID3D11Device* device = nullptr;
uint64_t captureFrame = 0;
std::map<XrSwapchain, std::vector<ID3D11Texture2D*>> images;
std::map<XrSwapchain, uint32_t> acquired;
std::set<XrSwapchain> done;

bool write_png(const std::wstring& path, const std::vector<uint8_t>& bgra, UINT width, UINT height) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        IWICImagingFactory* wic = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
             format == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()))) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        for (IUnknown* u : {static_cast<IUnknown*>(frame), static_cast<IUnknown*>(encoder), static_cast<IUnknown*>(stream),
                            static_cast<IUnknown*>(wic)})
            if (u)
                u->Release();
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    return ok;
}

bool save(ID3D11Texture2D* texture, const std::wstring& path) {
    std::vector<uint8_t> pixels;
    unsigned width = 0, height = 0;
    return d3d11_read_bgra8(texture, pixels, width, height) && write_png(path, pixels, width, height);
}

}  // namespace

bool write_png_bgra(const std::wstring& path, const std::vector<uint8_t>& bgra, unsigned width, unsigned height) {
    return write_png(path, bgra, width, height);
}

namespace {

bool is_bgra8(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_TYPELESS || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
bool is_rgba8(DXGI_FORMAT f) { return f >= DXGI_FORMAT_R8G8B8A8_TYPELESS && f <= DXGI_FORMAT_R8G8B8A8_SINT; }
bool is_texel32(DXGI_FORMAT f) {
    return is_bgra8(f) || is_rgba8(f) || f == DXGI_FORMAT_R11G11B10_FLOAT ||
           (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_R10G10B10A2_UINT);
}

}  // namespace

bool d3d11_read_bgra8(ID3D11Texture2D* texture, std::vector<uint8_t>& pixels, unsigned& width, unsigned& height) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    const bool bgra = is_bgra8(desc.Format);
    if (!bgra && !is_rgba8(desc.Format)) {
        logf("d3d11 capture: format %d not saved", static_cast<int>(desc.Format));
        return false;
    }
    DXGI_FORMAT format{};
    if (!d3d11_read_texels32(texture, pixels, width, height, format))
        return false;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (!bgra)
            std::swap(pixels[i], pixels[i + 2]);
        pixels[i + 3] = 255;
    }
    return true;
}

bool d3d11_read_texels32(ID3D11Texture2D* texture, std::vector<uint8_t>& texels, unsigned& width, unsigned& height,
                         DXGI_FORMAT& format) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (!is_texel32(desc.Format))
        return false;
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.SampleDesc = {1, 0};
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ID3D11Device* dev = nullptr;
    texture->GetDevice(&dev);
    ID3D11Texture2D* copy = nullptr;
    if (!dev || desc.SampleDesc.Count != 1 || FAILED(dev->CreateTexture2D(&staging, nullptr, &copy))) {
        if (dev)
            dev->Release();
        return false;
    }
    ID3D11DeviceContext* context = nullptr;
    dev->GetImmediateContext(&context);
    context->CopySubresourceRegion(copy, 0, 0, 0, 0, texture, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok = false;
    if (SUCCEEDED(context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped))) {
        const size_t row = static_cast<size_t>(desc.Width) * 4;
        texels.assign(row * desc.Height, 0);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(texels.data() + y * row, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, row);
        context->Unmap(copy, 0);
        width = desc.Width;
        height = desc.Height;
        format = desc.Format;
        ok = true;
    }
    context->Release();
    copy->Release();
    dev->Release();
    return ok;
}

bool write_dds(const std::wstring& path, const std::vector<uint8_t>& texels, unsigned width, unsigned height,
               DXGI_FORMAT format) {
    if (texels.size() != static_cast<size_t>(width) * height * 4)
        return false;
    // "DDS ", DDS_HEADER (124 bytes: the pixel format at +72 of it says DX10), DDS_HEADER_DXT10 (20 bytes).
    uint32_t header[1 + 31 + 5] = {};
    header[0] = 0x20534444;                   // "DDS "
    header[1] = 124;                          // dwSize
    header[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;  // caps, height, width, pitch, pixel format
    header[3] = height;
    header[4] = width;
    header[5] = width * 4;  // pitch
    header[7] = 1;          // mip count
    header[19] = 32;        // DDS_PIXELFORMAT: dwSize
    header[20] = 0x4;       // DDPF_FOURCC
    header[21] = 0x30315844;  // "DX10"
    header[27] = 0x1000;      // DDSCAPS_TEXTURE
    header[32] = static_cast<uint32_t>(format);
    header[33] = 3;  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    header[35] = 1;  // array size
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return false;
    const bool ok = std::fwrite(header, sizeof(header), 1, f) == 1 && std::fwrite(texels.data(), texels.size(), 1, f) == 1;
    return std::fclose(f) == 0 && ok;
}

void d3d11_capture_start(ID3D11Device* d, uint64_t frame) {
    std::lock_guard guard(mutex);
    device = frame ? d : nullptr;
    captureFrame = frame;
    done.clear();
    if (device && frame == 0xFFFFFFFFull)
        logf("d3d11 capture: armed by the frame capture hotkey (each swapchain's image of the next frame), into %ls\\captures",
             log_directory().c_str());
    else if (device)
        logf("d3d11 capture: each swapchain's image once, from frame %llu, into %ls\\captures",
             static_cast<unsigned long long>(frame), log_directory().c_str());
}

bool d3d11_capture_arm(uint64_t frame) {
    std::lock_guard guard(mutex);
    if (!device)
        return false;
    captureFrame = frame;
    done.clear();
    logf("d3d11 capture: armed for frame %llu", static_cast<unsigned long long>(frame));
    return true;
}

uint64_t d3d11_capture_armed() {
    std::lock_guard guard(mutex);
    return device ? captureFrame : 0;
}

void d3d11_capture_stop() {
    std::lock_guard guard(mutex);
    device = nullptr;
    images.clear();
    acquired.clear();
}

void d3d11_capture_images(XrSwapchain chain, const XrSwapchainImageD3D11KHR* list, uint32_t count) {
    std::lock_guard guard(mutex);
    auto& v = images[chain];
    v.clear();
    for (uint32_t i = 0; i < count; ++i)
        v.push_back(list[i].texture);
}

void d3d11_capture_acquired(XrSwapchain chain, uint32_t index) {
    std::lock_guard guard(mutex);
    acquired[chain] = index;
}

void d3d11_capture_release(XrSwapchain chain, uint64_t frame) {
    std::lock_guard guard(mutex);
    if (!device || frame < captureFrame || done.count(chain))
        return;
    const auto list = images.find(chain);
    const auto index = acquired.find(chain);
    if (list == images.end() || index == acquired.end() || index->second >= list->second.size())
        return;
    done.insert(chain);
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wchar_t name[96];
    swprintf_s(name, L"d3d11-frame%llu-chain%p-image%u.png", static_cast<unsigned long long>(frame),
               static_cast<void*>(chain), index->second);
    const std::wstring path = (dir / name).wstring();
    const bool ok = save(list->second[index->second], path);
    logf("d3d11 capture: %ls %s", path.c_str(), ok ? "saved" : "failed");
}

}  // namespace simqvfr

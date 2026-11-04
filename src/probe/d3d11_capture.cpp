#include "d3d11_capture.h"

#include <wincodec.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../log.h"

namespace aceqv {
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

// The image, read back through a staging copy: 8-bit BGRA or RGBA formats, alpha forced opaque.
bool save(ID3D11Texture2D* texture, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    const DXGI_FORMAT f = desc.Format;
    const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                      f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_TYPELESS ||
                      f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool rgba = desc.Format >= DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format <= DXGI_FORMAT_R8G8B8A8_SINT;
    if (!bgra && !rgba) {
        logf("d3d11 capture: format %d not saved", static_cast<int>(desc.Format));
        return false;
    }
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.SampleDesc = {1, 0};
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ID3D11Texture2D* copy = nullptr;
    if (desc.SampleDesc.Count != 1 || FAILED(device->CreateTexture2D(&staging, nullptr, &copy)))
        return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    context->CopySubresourceRegion(copy, 0, 0, 0, 0, texture, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok = false;
    if (SUCCEEDED(context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped))) {
        std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t* dst = pixels.data() + static_cast<size_t>(y) * desc.Width * 4;
            for (UINT x = 0; x < desc.Width; ++x) {
                dst[x * 4 + 0] = src[x * 4 + (bgra ? 0 : 2)];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + (bgra ? 2 : 0)];
                dst[x * 4 + 3] = 255;
            }
        }
        context->Unmap(copy, 0);
        ok = write_png(path, pixels, desc.Width, desc.Height);
    }
    context->Release();
    copy->Release();
    return ok;
}

}  // namespace

void d3d11_capture_start(ID3D11Device* d, uint64_t frame) {
    std::lock_guard guard(mutex);
    device = frame ? d : nullptr;
    captureFrame = frame;
    done.clear();
    if (device)
        logf("d3d11 capture: each swapchain's image once, from frame %llu, into %ls\\captures",
             static_cast<unsigned long long>(frame), log_directory().c_str());
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

}  // namespace aceqv

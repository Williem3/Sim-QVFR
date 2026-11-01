// Offline check of FocusOutline on WARP: a black sRGB render target in RENDER_TARGET state gets a
// magenta box outline; read back and check the border, the inside and the outside.
#include <cstdio>
#include <cstring>

#include <d3d12.h>
#include <dxgi1_6.h>

#include "quad/focus_outline.h"

namespace {

template <typename T>
void release(T*& p) {
    if (p)
        p->Release();
    p = nullptr;
}

}  // namespace

int main() {
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device* device = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::puts("RESULT: FAIL (no WARP device)");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* queue = nullptr;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    const UINT W = 64, H = 48;
    const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;  // like a runtime swapchain image
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{format, {0, 0, 0, 1}};
    ID3D12Resource* image = nullptr;
    device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                    IID_PPV_ARGS(&image));

    // Clear to black first with the outline module itself (one box covering everything, 64 px thick).
    aceqv::FocusOutline outline;
    if (!image || !outline.initialize(device, queue, format)) {
        std::puts("RESULT: FAIL (setup)");
        return 1;
    }
    const float black[4] = {0, 0, 0, 1}, magenta[4] = {1, 0, 1, 1};
    outline.draw(image, {{0, 0, static_cast<int>(W), static_cast<int>(H)}}, 64, black);
    outline.draw(image, {{10, 8, 50, 40}}, 2, magenta);

    // Read back.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                    IID_PPV_ARGS(&readback));
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{image};
    dst.PlacedFootprint = fp;
    dst.PlacedFootprint.Footprint.Format = format;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, done);
    WaitForSingleObject(done, 5000);

    unsigned char* pixels = nullptr;
    readback->Map(0, nullptr, reinterpret_cast<void**>(&pixels));
    const auto at = [&](int x, int y) {
        const unsigned char* p = pixels + fp.Offset + y * fp.Footprint.RowPitch + x * 4;
        return (p[0] << 16) | (p[1] << 8) | p[2];
    };
    struct Probe {
        const char* what;
        int x, y, rgb;
    } probes[] = {
        {"left border", 10, 20, 0xFF00FF}, {"right border", 49, 20, 0xFF00FF}, {"top border", 30, 8, 0xFF00FF},
        {"bottom border", 30, 39, 0xFF00FF}, {"inside", 30, 20, 0x000000},    {"outside", 5, 5, 0x000000},
        {"just inside left", 12, 20, 0x000000},
    };
    int failures = 0;
    for (const Probe& p : probes) {
        const int got = at(p.x, p.y);
        const bool ok = got == p.rgb;
        std::printf("%-18s (%2d,%2d) = %06X %s\n", p.what, p.x, p.y, got, ok ? "ok" : "WRONG");
        failures += !ok;
    }
    readback->Unmap(0, nullptr);
    outline.shutdown();
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

// Debug outline for the focus region: thin rectangles cleared into an app swapchain image on the
// app's own queue (ClearRenderTargetView with rects, no shaders), just before the image is released.
// The image must be in D3D12_RESOURCE_STATE_RENDER_TARGET, as OpenXR requires at release.
#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace simqvfr {

struct OutlineBox {
    int left, top, right, bottom;  // pixels, exclusive right/bottom
};

class FocusOutline {
public:
    ~FocusOutline() { shutdown(); }
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format);
    void shutdown();
    // Draws each box's border, `thickness` pixels wide, in `rgba`.
    bool draw(ID3D12Resource* image, const std::vector<OutlineBox>& boxes, int thickness, const float (&rgba)[4]);

private:
    static constexpr int kFrames = 3;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap* rtvHeap_ = nullptr;
    ID3D12CommandAllocator* allocators_[kFrames] = {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    uint64_t fenceValues_[kFrames] = {};
    uint64_t nextValue_ = 1;
    int frame_ = 0;
};

}  // namespace simqvfr

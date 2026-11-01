#include "focus_outline.h"

#include <algorithm>

#include "../log.h"

namespace aceqv {

bool FocusOutline::initialize(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format) {
    shutdown();
    device_ = device;
    queue_ = queue;
    format_ = format;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    bool ok = SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap_)));
    for (auto& allocator : allocators_)
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0], nullptr,
                                                   IID_PPV_ARGS(&list_))) &&
         SUCCEEDED(list_->Close()) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ok || !event_) {
        log_line("focus outline: D3D12 setup failed");
        shutdown();
        return false;
    }
    list_->SetName(L"ace-quadview focus outline");
    return true;
}

void FocusOutline::shutdown() {
    if (fence_ && queue_ && event_) {  // let in-flight outlines finish before releasing their allocators
        const uint64_t last = nextValue_ - 1;
        if (fence_->GetCompletedValue() < last && SUCCEEDED(fence_->SetEventOnCompletion(last, event_)))
            WaitForSingleObject(event_, 1000);
    }
    if (list_)
        list_->Release();
    for (auto& allocator : allocators_)
        if (allocator) {
            allocator->Release();
            allocator = nullptr;
        }
    if (rtvHeap_)
        rtvHeap_->Release();
    if (fence_)
        fence_->Release();
    if (event_)
        CloseHandle(event_);
    list_ = nullptr;
    rtvHeap_ = nullptr;
    fence_ = nullptr;
    event_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
}

bool FocusOutline::draw(ID3D12Resource* image, const std::vector<OutlineBox>& boxes, int thickness,
                        const float (&rgba)[4]) {
    if (!list_ || !image || boxes.empty())
        return false;
    // Reuse this frame's allocator only once the GPU has finished the outline recorded with it.
    if (fence_->GetCompletedValue() < fenceValues_[frame_] &&
        SUCCEEDED(fence_->SetEventOnCompletion(fenceValues_[frame_], event_)))
        WaitForSingleObject(event_, 1000);
    ID3D12CommandAllocator* allocator = allocators_[frame_];
    if (FAILED(allocator->Reset()) || FAILED(list_->Reset(allocator, nullptr)))
        return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = format_;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(image, &rtv, handle);
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    const int width = static_cast<int>(desc.Width), height = static_cast<int>(desc.Height);
    std::vector<D3D12_RECT> rects;
    const auto add = [&](int l, int t, int r, int b) {
        l = std::clamp(l, 0, width);
        r = std::clamp(r, 0, width);
        t = std::clamp(t, 0, height);
        b = std::clamp(b, 0, height);
        if (r > l && b > t)
            rects.push_back(D3D12_RECT{l, t, r, b});
    };
    for (const OutlineBox& box : boxes) {
        add(box.left, box.top, box.right, box.top + thickness);        // top
        add(box.left, box.bottom - thickness, box.right, box.bottom);  // bottom
        add(box.left, box.top, box.left + thickness, box.bottom);      // left
        add(box.right - thickness, box.top, box.right, box.bottom);    // right
    }
    if (!rects.empty())
        list_->ClearRenderTargetView(handle, rgba, static_cast<UINT>(rects.size()), rects.data());
    if (FAILED(list_->Close()))
        return false;
    ID3D12CommandList* lists[] = {list_};
    queue_->ExecuteCommandLists(1, lists);
    fenceValues_[frame_] = nextValue_++;
    queue_->Signal(fence_, fenceValues_[frame_]);
    frame_ = (frame_ + 1) % kFrames;
    return true;
}

}  // namespace aceqv

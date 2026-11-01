// D3D12 quad-view compositor: keeps a copy of frame A's image (full FOV, "outer") and draws the
// final stereo image as the outer view upscaled to the full FOV with frame B's image (focus FOV)
// blended into the focus rectangle.
//
// Images follow XR_KHR_D3D12_enable: OpenXR swapchain images are in RENDER_TARGET state between
// acquire and release, and are left in that state.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <wrl/client.h>

#include "../xr.h"

namespace aceqv {

// Frame B may look in a different direction than frame A (its camera turned towards the gaze, with
// a centred projection): every output pixel's ray in A's eye frame is rotated into B's frame and
// projected with B's tangents, so the focus area is wherever B's view lands, exactly, at any depth.
struct EyeComposition {
    float outerTan[4];    // frame A's view tangents: left, right, up, down
    float focusTan[4];    // frame B's view tangents: left, right, up, down
    float rotation[9];    // row-major; takes a direction in A's eye frame into B's eye frame
    float outerSource[4]; // outer texture region for this eye: u0, v0, du, dv
    float focusSource[4]; // focus texture region for this eye: u0, v0, du, dv
    float feather;        // blend band, as a fraction of the focus box (0..0.5)
    float border;         // debug: width of an outline at the box edge, as a fraction of the box (0 = off)
    float borderColor[4]; // its colour (linear RGBA)
    float cornerRadius;   // rounded box corners: radius as a share of half the box's shorter side (0 = square)
};

class QuadCompositor {
public:
    // viewFormat is the swapchain's colour format (e.g. R8G8B8A8_UNORM_SRGB); views use it so
    // filtering happens in linear space and the output is re-encoded exactly like ACE's own.
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT viewFormat);
    // Copies frame A's image into the outer texture (created on first use with the image's description).
    bool capture_outer(ID3D12Resource* appImage);
    bool has_outer() const { return outerValid_; }
    // Draws both eyes into `output` (a double-wide swapchain image), side by side.
    bool compose(ID3D12Resource* focusImage, ID3D12Resource* output, const EyeComposition (&eyes)[2]);
    // Debug: saves the left eye of the outer copy, the focus image and the output (both in
    // RENDER_TARGET state) as <prefix>-outer.png, -focus.png, -output.png. Stalls the GPU.
    bool save_captures(ID3D12Resource* focusImage, ID3D12Resource* output, const std::wstring& prefix);
    // Debug, views4: saves the whole four-view image as <prefix>-views.png and the output's left eye
    // as <prefix>-output.png (both in RENDER_TARGET state). Stalls the GPU.
    bool save_views4_captures(ID3D12Resource* views, ID3D12Resource* output, const std::wstring& prefix);
    void shutdown();

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    static constexpr UINT kSlots = 8;

    struct Slot {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        UINT64 fenceValue = 0;
    };
    Slot* next_slot();
    bool submit(Slot& slot);
    // Saves the left 1/widthDivisor of the image (2: the left eye of a double-wide image; 1: all of it).
    bool save_left_eye(ID3D12Resource* image, D3D12_RESOURCE_STATES state, const std::wstring& path,
                       UINT widthDivisor = 2);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT index) const;

    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> queue_;
    Com<ID3D12RootSignature> rootSignature_;
    Com<ID3D12PipelineState> pipeline_;
    Com<ID3D12DescriptorHeap> srvHeap_, rtvHeap_;
    Com<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceCounter_ = 0;
    UINT srvIncrement_ = 0, rtvIncrement_ = 0;
    Slot slots_[kSlots];
    UINT slotIndex_ = 0;
    DXGI_FORMAT viewFormat_ = DXGI_FORMAT_UNKNOWN;
    Com<ID3D12Resource> outer_;
    bool outerValid_ = false;
    bool ready_ = false;
};

}  // namespace aceqv

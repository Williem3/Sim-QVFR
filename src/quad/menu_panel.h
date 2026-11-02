// The in-headset settings panel (settings_menu.h): its image drawn with GDI into a bitmap (no font
// of our own), then copied into an OpenXR D3D12 swapchain image on the app's queue. The swapchain is
// submitted as a quad layer (xr_layer.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>

#include "settings_menu.h"

namespace aceqv {

inline constexpr int kPanelWidth = 1024, kPanelHeight = 972;  // up to 12 lines and 3 notes

// RGBA8 pixels (sRGB-encoded, straight alpha), kPanelWidth x kPanelHeight, rows top to bottom.
// `title`, then ACE's render target (max and now, QVFR), the centre PPD and pixels saved (or, when
// `subtitle` is set, that line instead: the Foveated DLSS page's credit), the lines
// (capped values in red), then up to three red lines (what the values ask for when it is more than
// the max, and `notes`: why and what to lower) and `hint` at the bottom.
std::vector<uint32_t> draw_menu_panel(const std::wstring& title, const MenuResolution& resolution,
                                      const MenuStats& stats, const std::vector<MenuLine>& lines,
                                      const std::vector<std::wstring>& notes, const std::wstring& hint,
                                      const std::wstring& subtitle = L"");

// Copies a panel image into a swapchain image (kept in D3D12_RESOURCE_STATE_RENDER_TARGET around
// the copy, as OpenXR requires for a colour swapchain). `width` x 4 bytes must be a multiple of 256
// (the copy's row pitch): widths in multiples of 64.
class PanelUpload {
public:
    ~PanelUpload() { shutdown(); }
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue, int width = kPanelWidth, int height = kPanelHeight);
    void shutdown();
    bool upload(ID3D12Resource* image, const std::vector<uint32_t>& pixels);

private:
    int width_ = kPanelWidth, height_ = kPanelHeight;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Resource* buffer_ = nullptr;  // upload heap, persistently mapped
    void* mapped_ = nullptr;
    ID3D12CommandAllocator* allocator_ = nullptr;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    uint64_t value_ = 0;
    void wait();
};

}  // namespace aceqv

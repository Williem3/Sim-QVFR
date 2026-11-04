// The OpenXR session's graphics API as the layer's own drawing sees it: D3D12 (ACE) or D3D11 (ACC). Set at
// xrCreateSession from the graphics binding, cleared at xrDestroySession. The panels (menu_panel.h) and the
// compositors take their device from here, and swapchain images come back as that API's textures
// (ID3D12Resource* or ID3D11Texture2D*), so a feature works on either without knowing which.
#pragma once

#include <vector>

#include "../xr.h"

namespace aceqv {

struct SessionGraphics {
    ID3D12Device* d3d12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;  // the game's (D3D12)
    ID3D11Device* d3d11 = nullptr;
    bool valid() const { return (d3d12 && queue) || d3d11; }
    const char* name() const { return d3d12 ? "D3D12" : d3d11 ? "D3D11" : "none"; }
    // The device's adapter (false: none or unreadable).
    bool adapter_luid(LUID& luid) const {
        if (d3d12) {
            luid = d3d12->GetAdapterLuid();
            return true;
        }
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        DXGI_ADAPTER_DESC desc{};
        const bool ok = d3d11 && SUCCEEDED(d3d11->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
                        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc));
        if (adapter)
            adapter->Release();
        if (dxgi)
            dxgi->Release();
        if (ok)
            luid = desc.AdapterLuid;
        return ok;
    }
};

// From a session's graphics binding (D3D11 or D3D12); anything else: none.
inline SessionGraphics session_graphics(const void* binding) {
    SessionGraphics g;
    const auto* header = static_cast<const XrBaseInStructure*>(binding);
    if (header && header->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
        const auto* d3d12 = static_cast<const XrGraphicsBindingD3D12KHR*>(binding);
        g.d3d12 = d3d12->device;
        g.queue = d3d12->queue;
    } else if (header && header->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
        g.d3d11 = static_cast<const XrGraphicsBindingD3D11KHR*>(binding)->device;
    }
    return g;
}

// A swapchain's images as `g`'s textures, through `enumerate` (the next layer's xrEnumerateSwapchainImages).
inline XrResult swapchain_textures(const SessionGraphics& g, PFN_xrEnumerateSwapchainImages enumerate, XrSwapchain chain,
                                   std::vector<void*>& textures) {
    textures.clear();
    uint32_t count = 0;
    XrResult r = enumerate(chain, 0, &count, nullptr);
    if (XR_FAILED(r))
        return r;
    if (g.d3d12) {
        std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        r = enumerate(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        for (uint32_t i = 0; XR_SUCCEEDED(r) && i < count; ++i)
            textures.push_back(images[i].texture);
    } else if (g.d3d11) {
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        r = enumerate(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        for (uint32_t i = 0; XR_SUCCEEDED(r) && i < count; ++i)
            textures.push_back(images[i].texture);
    } else {
        r = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    return r;
}

}  // namespace aceqv

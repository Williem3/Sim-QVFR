// Diagnostic: saves what a D3D11 game (ACC through OpenComposite) put in its swapchains, as PNG, so the views
// can be checked against the rects the layer submits. [experiment] d3d11_capture_frame=N: each swapchain's
// image once, as it is released from frame N on, into <log folder>\captures. Reads on the game's own
// immediate context, in its release call, before the runtime gets the image.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../xr.h"

namespace simqvfr {

// An 8-bit RGBA or BGRA texture's pixels as BGRA (alpha forced opaque), read back through a staging copy on its own
// device's immediate context: call it on the thread that renders with that context. False for other formats or MSAA.
bool d3d11_read_bgra8(ID3D11Texture2D* texture, std::vector<uint8_t>& bgra, unsigned& width, unsigned& height);
// BGRA pixels as a PNG file.
bool write_png_bgra(const std::wstring& path, const std::vector<uint8_t>& bgra, unsigned width, unsigned height);
// A texture of 4-byte texels (R11G11B10_FLOAT, R10G10B10A2, 8-bit RGBA or BGRA) as it is, tight rows, read back like
// d3d11_read_bgra8 (same thread rule). False for other formats or MSAA.
bool d3d11_read_texels32(ID3D11Texture2D* texture, std::vector<uint8_t>& texels, unsigned& width, unsigned& height,
                         DXGI_FORMAT& format);
// Those texels as an uncompressed DDS file with a DX10 header (the format as is): what tools/dds_read.py reads.
bool write_dds(const std::wstring& path, const std::vector<uint8_t>& texels, unsigned width, unsigned height,
               DXGI_FORMAT format);

// From xrCreateSession with a D3D11 binding; `frame` 0: off.
void d3d11_capture_start(ID3D11Device* device, uint64_t frame);
void d3d11_capture_stop();
// From xrEnumerateSwapchainImages (D3D11 images) and xrAcquireSwapchainImage.
void d3d11_capture_images(XrSwapchain chain, const XrSwapchainImageD3D11KHR* images, uint32_t count);
void d3d11_capture_acquired(XrSwapchain chain, uint32_t index);
// From xrReleaseSwapchainImage, before passing it on; `frame`: xrEndFrame calls so far.
void d3d11_capture_release(XrSwapchain chain, uint64_t frame);
// [experiment] d3d11_capture_hotkey=1: started with a frame never reached, then armed by the frame capture hotkey to
// save each swapchain's image once more from `frame` (the next one). False without a D3D11 session.
bool d3d11_capture_arm(uint64_t frame);
// The frame the capture is armed for (each hotkey press arms a new one; never-reached 0xFFFFFFFF until the first), 0
// without a D3D11 session: a game adapter that saves its own images with the swapchains' watches it change.
uint64_t d3d11_capture_armed();

}  // namespace simqvfr

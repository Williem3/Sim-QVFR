// Diagnostic: saves what a D3D11 game (ACC through OpenComposite) put in its swapchains, as PNG, so the views
// can be checked against the rects the layer submits. [experiment] d3d11_capture_frame=N: each swapchain's
// image once, as it is released from frame N on, into <log folder>\captures. Reads on the game's own
// immediate context, in its release call, before the runtime gets the image.
#pragma once

#include <cstdint>

#include "../xr.h"

namespace simqvfr {

// From xrCreateSession with a D3D11 binding; `frame` 0: off.
void d3d11_capture_start(ID3D11Device* device, uint64_t frame);
void d3d11_capture_stop();
// From xrEnumerateSwapchainImages (D3D11 images) and xrAcquireSwapchainImage.
void d3d11_capture_images(XrSwapchain chain, const XrSwapchainImageD3D11KHR* images, uint32_t count);
void d3d11_capture_acquired(XrSwapchain chain, uint32_t index);
// From xrReleaseSwapchainImage, before passing it on; `frame`: xrEndFrame calls so far.
void d3d11_capture_release(XrSwapchain chain, uint64_t frame);

}  // namespace simqvfr

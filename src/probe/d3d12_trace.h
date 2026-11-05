// Log-only D3D12 tracing: reports every render-target binding that writes into an OpenXR
// swapchain image, with the caller's stack and the viewport set next on that thread.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;

namespace simqvfr {

bool d3d12_trace_start(ID3D12Device* device);
void d3d12_trace_register_image(ID3D12Resource* image, uint32_t width, uint32_t height, uint32_t index);
void d3d12_trace_frame_end(bool sequenceActive);

}  // namespace simqvfr

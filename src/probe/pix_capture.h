// Programmatic PIX GPU capture of one frame (OpenXR has no Present for PIX to delimit frames). The
// installed PIX's WinPixGpuCapturer.dll must be loaded before the D3D12 device exists; the capture
// then begins at xrEndFrame number `frame` and ends two frames later, so one complete frame's GPU
// work lies inside it. The .wpix lands in <log directory>\pix.
#pragma once

#include <cstdint>

namespace aceqv {

// Loads the newest installed PIX capturer; false (logged) when PIX is not installed.
bool pix_capture_load(uint64_t frame);
// Called at the start of every xrEndFrame with its 1-based number.
void pix_capture_frame(uint64_t endFrame);

}  // namespace aceqv

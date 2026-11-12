// GPU time of the layer's own work in a D3D11 game's frame (LMU, ACC: the views4 composition at xrEndFrame), from
// D3D11 timestamp queries around it on the immediate context: the performance overlay's "layer" part of the GPU
// (D3D12 games time it on their queue, gpu_frame_timer.h). Results are read a few frames later without waiting
// (D3D11_ASYNC_GETDATA_DONOTFLUSH), so the timer never stalls the frame; a disjoint span is dropped.
#pragma once

struct ID3D11Device;

namespace simqvfr {

bool d3d11_compose_timer_start(ID3D11Device* device);
void d3d11_compose_timer_stop();
// Around the work, on the thread that submits it (xrEndFrame's): begin reads finished spans and opens one, end closes it.
void d3d11_compose_timer_begin();
void d3d11_compose_timer_end();
// The average span (ms) over the spans finished since the last call, and resets; -1 with none (`frames` how many).
double d3d11_compose_timer_collect(int& frames);

}  // namespace simqvfr

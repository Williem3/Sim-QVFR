// GPU time of ACE's frame on its own queue, from D3D12 timestamps the layer puts into that queue:
// one when xrBeginFrame returns, one when ACE releases its eye image (all of the frame's rendering
// is submitted by then) and one after the views4 composition. The begin timestamp executes once the
// GPU reaches it, i.e. when it has finished the previous frame or right away if it was idle, so
// begin-to-release is the frame's GPU work plus any gaps where the GPU waited for ACE's submissions
// (an upper bound). Work ACE submits before xrBeginFrame or on another queue is not counted.
#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace simqvfr {

enum class GpuMark { kBegin, kRendered, kComposed };

struct GpuFrameStats {
    size_t frames = 0;       // frames with both begin and rendered timestamps
    double renderP50 = 0, renderP90 = 0, renderAvg = 0;  // begin to rendered, ms
    double intervalP50 = 0, intervalP90 = 0;  // rendered to the next frame's rendered, ms
    double composeAvg = 0, composeP90 = 0;     // rendered to composed, ms (0 when not composing)
    uint64_t skipped = 0;    // frames not timed: every timestamp slot was still in flight
};

bool gpu_frame_timer_start(ID3D12Device* device, ID3D12CommandQueue* queue);
void gpu_frame_timer_stop();
// Called on the thread that runs the frame: kBegin starts a frame, the others time points in it.
void gpu_frame_timer_mark(GpuMark mark);
// The frames timed since the last call (finished ones only), and resets (the performance overlay's).
GpuFrameStats gpu_frame_timer_collect();
// The same for the settings menu's QVFR optimizer (quad/qvfr_optimizer.h): its own window of samples.
GpuFrameStats gpu_frame_timer_collect_optimizer();
// Logs the frames timed since its last call as a "views4 gpu:" line: its own window of samples, so it
// logs while the overlay collects too.
void gpu_frame_timer_report();

}  // namespace simqvfr

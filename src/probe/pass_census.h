// Log-only per-pipeline census over a few frames: for each pipeline state, how many draws land in
// each quarter of the four-view target (from the viewport set on that command list) and how many
// compute dispatches run, with their group counts. Finds per-view work that runs for the eyes
// (quarters 0-1) but not for views 2-3 (views4 darkness bisection).
#pragma once

#include <cstdint>

struct ID3D12Device;

namespace aceqv {

bool pass_census_start(ID3D12Device* device);
// The width of one view in ACE's render target (a quarter of it), once known.
void pass_census_set_quarter_width(int width);
// Logs the stack and viewport of the first draws of the pipeline state with this debug name.
void pass_census_watch(const char* name);
// Diagnostic: a global UAV barrier after every compute dispatch, so no later work overlaps its writes.
void pass_census_set_uav_barriers(bool on);
// Whether the command-list hooks are in place (pass_census_start succeeded).
bool pass_census_installed();
// The next Dispatch recorded on this thread is followed by a UAV barrier (views4: two passes writing
// the same target must not overlap).
void pass_census_barrier_after_next_dispatch();
// Counts from frame `first` for `frames` frames, then logs the census once.
void pass_census_arm(uint64_t first, uint64_t frames);
void pass_census_frame_end();

}  // namespace aceqv

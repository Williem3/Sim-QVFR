// Log-only watch on ACE's instance visibility batch (ace_abi.h kBatchMergeRva): every frame it
// sums what each visibility group adds before the merge runs, and logs the totals against ACE's
// limits, with a per-group breakdown when a total nears or passes one (ACE then throws and exits).
#pragma once

namespace aceqv {

// Verifies the anchors and hooks the merge. Safe to call more than once.
bool batch_watch_start();
void batch_watch_stop();
// Called once per xrEndFrame: the periodic summary.
void batch_watch_frame_end();

}  // namespace aceqv

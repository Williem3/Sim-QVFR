#pragma once

#include "backend_stats.h"
#include "compositor.h"
#include "pass_state.h"

namespace simqvfr::games::rf2 {

inline void frame_end(PassState& pass, const BackendStats& stats, const Compositor& compositor,
                      unsigned long long frame) {
    pass.frame_end();
    stats.log_frame(frame, compositor.frames(0), compositor.frames(1));
}

}  // namespace simqvfr::games::rf2

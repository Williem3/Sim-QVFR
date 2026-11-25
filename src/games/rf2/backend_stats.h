#pragma once

// RF2's draw counts, logged every 300 frames: how many eye-pass draws drew both views, how many had no twin (drawn as the
// game made them), how many couldn't be doubled at all (indirect draws), and the compositions.

#include <cstdint>

#include "../../log.h"

namespace simqvfr::games::rf2 {

class BackendStats {
public:
    void note_draw() { ++draws_; }
    void note_doubled() { ++doubled_; }
    void note_no_twin() { ++noTwin_; }
    void note_unsupported() { ++unsupported_; }
    void note_composite(bool ok) {
        ++compositeAttempts_;
        if (!ok)
            ++compositeFailures_;
    }
    void log_frame(unsigned long long frame, uint64_t left, uint64_t right) const {
        if (!frame || frame % 300 != 0)
            return;
        simqvfr::logf("rf2 draws: frame=%llu eye-pass draws=%llu both views=%llu no twin=%llu indirect=%llu; "
                      "compositions L/R=%llu/%llu, %llu of %llu failed",
                      frame, static_cast<unsigned long long>(draws_), static_cast<unsigned long long>(doubled_),
                      static_cast<unsigned long long>(noTwin_), static_cast<unsigned long long>(unsupported_),
                      static_cast<unsigned long long>(left), static_cast<unsigned long long>(right),
                      static_cast<unsigned long long>(compositeFailures_),
                      static_cast<unsigned long long>(compositeAttempts_));
    }

private:
    uint64_t draws_ = 0, doubled_ = 0, noTwin_ = 0, unsupported_ = 0, compositeAttempts_ = 0, compositeFailures_ = 0;
};

}  // namespace simqvfr::games::rf2

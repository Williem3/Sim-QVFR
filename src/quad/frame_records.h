// What each frame was rendered with (a game's Views4Frame: its views' tangents and focus turns), carried from where
// the game sets the frame up to where the layer composes it, so the composition places the focus views exactly as
// they were rendered even when the gaze has moved since. ACE fills its focus cameras and composes on one thread, one
// after the other; ACC sets its views up on the game thread a frame or so ahead of the composition at xrEndFrame.
//
// Records are taken in the order pushed. A backlog longer than `maxBacklog` (frames set up but never composed:
// loading, a dropped frame) drops all but the newest. A backlog that stays above `inFlight` (the records normally
// pushed but not yet composed at a composition) for kDriftTakes compositions is a lasting offset, each image getting
// an older frame's record: it is resynced so `inFlight` remain. Taking before anything new was pushed returns the
// last record again.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace aceqv {

template <class Record, std::size_t kRing = 8>
class FrameRecords {
public:
    static constexpr uint64_t kDriftTakes = 90;

    FrameRecords(uint64_t inFlight, uint64_t maxBacklog) : inFlight_(inFlight), maxBacklog_(maxBacklog) {}

    void push(const Record& r) {
        std::lock_guard guard(mutex_);
        ring_[pushed_ % kRing] = r;
        lastPushed_ = r;
        ++pushed_;
    }
    // The newest record, without taking it; false before the first.
    bool latest(Record& r) const {
        std::lock_guard guard(mutex_);
        r = lastPushed_;
        return pushed_ > 0;
    }
    // The next record to compose; false before the first. `drifted`: this take resynced a lasting offset, with
    // `behind` records left over.
    bool take(Record& r, bool* drifted = nullptr, uint64_t* behind = nullptr) {
        std::lock_guard guard(mutex_);
        bool drift = false;
        uint64_t left = 0;
        if (taken_ < pushed_) {
            if (pushed_ - taken_ > maxBacklog_) {
                taken_ = pushed_ - 1;
                ++resyncs_;
            }
            lastTaken_ = ring_[taken_ % kRing];
            ++taken_;
            left = pushed_ - taken_;
            behindRuns_ = left > inFlight_ ? behindRuns_ + 1 : 0;
            if (behindRuns_ >= kDriftTakes) {
                taken_ = pushed_ - inFlight_;
                lastTaken_ = ring_[(taken_ - 1) % kRing];
                behindRuns_ = 0;
                ++resyncs_;
                drift = true;
            }
        } else if (pushed_ > 0) {
            ++emptyTakes_;  // composed before this frame's record was pushed: the last one again
            behindRuns_ = 0;
        }
        r = lastTaken_;
        if (drifted)
            *drifted = drift;
        if (behind)
            *behind = left;
        return pushed_ > 0;
    }

    struct Counts {
        uint64_t pushed = 0, taken = 0, resyncs = 0, emptyTakes = 0;
    };
    Counts counts() const {
        std::lock_guard guard(mutex_);
        return {pushed_, taken_, resyncs_, emptyTakes_};
    }

private:
    mutable std::mutex mutex_;
    const uint64_t inFlight_, maxBacklog_;
    Record ring_[kRing]{}, lastTaken_{}, lastPushed_{};
    uint64_t pushed_ = 0, taken_ = 0, resyncs_ = 0, emptyTakes_ = 0, behindRuns_ = 0;
};

}  // namespace aceqv

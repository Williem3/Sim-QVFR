// Turbo mode ([cpu] turbo): the game's xrWaitFrame doesn't block on the runtime's frame throttle. The runtime's
// xrWaitFrame runs on a worker thread of this layer's instead, and the game gets the frame state at once, the last
// real one with its display time moved on by the display periods since (turbo_predicted_time). The wait is joined in
// the game's xrBeginFrame, before the runtime's own, so the runtime still sees wait, begin, end in order: the game's
// CPU work between its xrWaitFrame and its xrBeginFrame (its simulation, culling, command recording) overlaps the
// runtime's wait instead of following it. OpenXR Toolkit's Turbo mode does the same.
//
// TurboWait is that state machine, apart from the layer (tests/turbo_test.cpp runs it against a fake runtime):
// - wait(): the game's xrWaitFrame. Armed and primed (an earlier real wait succeeded), it starts the real wait on the
//   worker and returns the extrapolated state; otherwise it waits for real on the game's thread (the first frame,
//   after an error, or turbo off). A game's xrWaitFrame for the next frame before its xrBeginFrame for this one
//   blocks until that begin, as the runtime's would (OpenXR: "a subsequent xrWaitFrame call must block until the
//   previous frame has been begun"), at most kUnbegunLimitMs: then the pending wait is joined and its real state given.
// - begin(): the game's xrBeginFrame, before the runtime's. Joins the pending wait: its result (an error is the
//   game's xrBeginFrame's, and the runtime's xrBeginFrame isn't called), its real state, and how long it held.
// - drain(): xrEndSession, a session's loss: a pending wait joined, the next wait real again.
// - stop(): xrDestroySession: drained, the worker ended.
// No lock is held across the runtime's wait.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "xr.h"

namespace simqvfr::layer {

// The display time for the game's frame: the last real one moved on by the whole display periods since it came
// (rounded to the nearest, at least one: the game asks once per frame, about a period after the last real wait
// returned; a frame that ran long skips the slots it missed). Unchanged without a period.
inline XrTime turbo_predicted_time(XrTime lastPredicted, XrDuration period, int64_t elapsedNs) {
    if (period <= 0)
        return lastPredicted;
    int64_t periods = elapsedNs > 0 ? (elapsedNs + period / 2) / period : 0;
    if (periods < 1)
        periods = 1;
    return lastPredicted + periods * period;
}

// Why turbo isn't armed this frame (None: armed).
enum class TurboBlock {
    None,
    Off,              // [cpu] turbo=0
    BaselineRun,      // a baseline or log-only run: the game's frame pacing left as it is
    FrameGeneration,  // Cheeky's frame generation (fg=1): the game crashed with OpenXR Toolkit's Turbo mode and it
    TwoLoops,         // the deprecated two_loops experiment already armed (never mixed mid-pair)
    Chained,          // the game chains structures into xrWaitFrame, which a wait on the worker can't carry
};

inline TurboBlock turbo_block(bool wanted, bool cpuSettings, bool frameGeneration, bool twoLoopsActive, bool chained) {
    if (!wanted)
        return TurboBlock::Off;
    if (!cpuSettings)
        return TurboBlock::BaselineRun;
    if (frameGeneration)
        return TurboBlock::FrameGeneration;
    if (twoLoopsActive)
        return TurboBlock::TwoLoops;
    if (chained)
        return TurboBlock::Chained;
    return TurboBlock::None;
}

// The log's line for turbo's state ("turbo: on", "turbo: off (frame generation on)").
inline const char* turbo_log_line(TurboBlock b) {
    switch (b) {
    case TurboBlock::None: return "turbo: on (the game's xrWaitFrame returns at once; the runtime's wait is joined in "
                                  "xrBeginFrame)";
    case TurboBlock::Off: return "turbo: off";
    case TurboBlock::BaselineRun: return "turbo: off (a baseline or log-only run)";
    case TurboBlock::FrameGeneration: return "turbo: off (frame generation on)";
    case TurboBlock::TwoLoops: return "turbo: off (the two_loops experiment is armed)";
    case TurboBlock::Chained: return "turbo: off (the game chains structures into xrWaitFrame)";
    }
    return "turbo: off";
}

class TurboWait {
public:
    // The runtime's xrWaitFrame for the session, filling `state` (type set, next null).
    using RealWait = std::function<XrResult(XrFrameState& state)>;
    // A monotonic clock in nanoseconds (QPC in the layer; the tests' own).
    using Clock = std::function<int64_t()>;
    static constexpr int kUnbegunLimitMs = 1000;

    enum class Kind {
        Real,    // waited for real on the game's thread
        Async,   // returned at once; the real wait runs on the worker
        Joined,  // a pending wait joined (the game waited again before beginning): its real state
    };
    struct Waited {
        Kind kind = Kind::Real;
        XrFrameState real{XR_TYPE_FRAME_STATE};  // Real, Joined: the runtime's state
    };
    struct Begun {
        bool joined = false;  // a pending wait was joined (else nothing was pending)
        XrResult result = XR_SUCCESS;
        XrFrameState real{XR_TYPE_FRAME_STATE};
        int64_t heldNs = 0;  // how long the game's xrBeginFrame waited for it
    };

    explicit TurboWait(Clock clock) : clock_(std::move(clock)) {}
    TurboWait(const TurboWait&) = delete;
    TurboWait& operator=(const TurboWait&) = delete;
    // Not joined here: a DLL's globals go under the loader lock, where joining a thread deadlocks. stop() joins it.
    ~TurboWait() {
        if (worker_.joinable())
            worker_.detach();
    }

    // In use: primed or holding a wait (the game's xrWaitFrame goes through wait() then, armed or not).
    bool active() const {
        std::lock_guard guard(mutex_);
        return primed_ || pending_ || unbegun_;
    }

    XrResult wait(bool armed, const RealWait& real, XrFrameState& state, Waited& out) {
        std::unique_lock lock(mutex_);
        if (unbegun_ && !beganCv_.wait_for(lock, std::chrono::milliseconds(kUnbegunLimitMs), [this] { return !unbegun_; })) {
            // No xrBeginFrame came: the pending wait's state is this frame's (the runtime has a wait not begun).
            unbegun_ = false;
            if (pending_) {
                const XrResult r = join_locked(lock, out.real, nullptr);
                out.kind = Kind::Joined;
                if (XR_SUCCEEDED(r))
                    copy_state(out.real, state);
                unbegun_ = XR_SUCCEEDED(r);
                return r;
            }
        }
        if (pending_) {  // not reached in order (begin() joins it), kept safe
            const XrResult r = join_locked(lock, out.real, nullptr);
            out.kind = Kind::Joined;
            if (XR_SUCCEEDED(r))
                copy_state(out.real, state);
            unbegun_ = XR_SUCCEEDED(r);
            return r;
        }
        if (!armed || !primed_) {
            primed_ = false;
            lock.unlock();  // the runtime's wait, on the game's thread
            XrFrameState s{XR_TYPE_FRAME_STATE};
            const XrResult r = real(s);
            const int64_t now = clock_();
            lock.lock();
            out.kind = Kind::Real;
            out.real = s;
            if (XR_SUCCEEDED(r)) {
                copy_state(s, state);
                unbegun_ = true;
                if (armed) {
                    last_ = s;
                    lastNs_ = now;
                    primed_ = true;
                }
            }
            return r;
        }
        // Armed and primed: this frame's state from the last real one; the real wait starts on the worker.
        out.kind = Kind::Async;
        state.predictedDisplayTime =
            turbo_predicted_time(last_.predictedDisplayTime, last_.predictedDisplayPeriod, clock_() - lastNs_);
        state.predictedDisplayPeriod = last_.predictedDisplayPeriod;
        state.shouldRender = last_.shouldRender;
        if (!worker_.joinable()) {
            quit_ = false;
            worker_ = std::thread([this] { run(); });
        }
        request_ = real;
        pending_ = requested_ = true;
        done_ = false;
        unbegun_ = true;
        wake_.notify_all();
        return XR_SUCCESS;
    }

    Begun begin() {
        Begun b;
        std::unique_lock lock(mutex_);
        if (pending_) {
            b.joined = true;
            b.result = join_locked(lock, b.real, &b.heldNs);
        }
        unbegun_ = false;
        beganCv_.notify_all();
        return b;
    }

    // A pending wait joined; the next wait real again. Returns whether one was pending.
    bool drain() {
        std::unique_lock lock(mutex_);
        bool had = false;
        if (pending_) {
            XrFrameState s{XR_TYPE_FRAME_STATE};
            join_locked(lock, s, nullptr);
            had = true;
        }
        primed_ = unbegun_ = false;
        beganCv_.notify_all();
        return had;
    }

    void stop() {
        drain();
        {
            std::lock_guard guard(mutex_);
            quit_ = true;
            wake_.notify_all();
        }
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
            worker_.join();
    }

private:
    static void copy_state(const XrFrameState& from, XrFrameState& to) {
        to.predictedDisplayTime = from.predictedDisplayTime;
        to.predictedDisplayPeriod = from.predictedDisplayPeriod;
        to.shouldRender = from.shouldRender;
    }

    // With the lock held: waits (releasing it) for the worker's wait, takes its result. A success primes the next
    // frame's extrapolation; an error unprimes (the next wait is real, on the game's thread).
    XrResult join_locked(std::unique_lock<std::mutex>& lock, XrFrameState& real, int64_t* heldNs) {
        const int64_t t0 = clock_();
        doneCv_.wait(lock, [this] { return done_; });
        if (heldNs)
            *heldNs = clock_() - t0;
        pending_ = done_ = false;
        real = result_;
        if (XR_SUCCEEDED(resultCode_)) {
            last_ = result_;
            lastNs_ = resultNs_;
        } else {
            primed_ = false;
        }
        return resultCode_;
    }

    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return quit_ || requested_; });
            if (requested_) {
                requested_ = false;
                const RealWait real = std::move(request_);
                request_ = nullptr;
                lock.unlock();  // never across the runtime's wait
                XrFrameState s{XR_TYPE_FRAME_STATE};
                const XrResult r = real ? real(s) : XR_ERROR_RUNTIME_FAILURE;
                const int64_t now = clock_();
                lock.lock();
                result_ = s;
                resultCode_ = r;
                resultNs_ = now;
                done_ = true;
                doneCv_.notify_all();
                continue;
            }
            if (quit_)
                return;
        }
    }

    Clock clock_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, doneCv_, beganCv_;
    std::thread worker_;
    RealWait request_;
    bool quit_ = false, requested_ = false, done_ = false;
    bool pending_ = false;  // a wait started on the worker and not joined yet
    bool primed_ = false;   // last_ is a real state to extrapolate from
    bool unbegun_ = false;  // the game's xrWaitFrame returned, its xrBeginFrame not called yet
    XrFrameState last_{XR_TYPE_FRAME_STATE};
    int64_t lastNs_ = 0;
    XrFrameState result_{XR_TYPE_FRAME_STATE};
    XrResult resultCode_ = XR_SUCCESS;
    int64_t resultNs_ = 0;
};

}  // namespace simqvfr::layer

// LMU's multi-view path: what it does when the focus views stop coming, in place of a fallback for the rest of the
// session (the four passes, which hook the game's code). Pure arithmetic: games/lmu/four_passes.cpp feeds it each
// frame's outcome and acts on its state; tests/lmu_mvr_recovery_test.cpp is its check.
//   foveated  the eyes at the periphery's size, each with its focus view beside it: the normal state.
//   plain     entered after kNoFinalsFrames frames in a row with an eye pass but without both focus views (a pipeline
//             the focus post doesn't handle, a pass the tracker lost): the eyes at full resolution, no focus views.
//             Slower, but a right and sharp picture, where the eyes alone at the periphery's size are a blur.
//   trying    from plain once its wait is over: foveated again for a few frames. Focus views within kTryFrames: back
//             to foveated. None: plain again, the wait doubled (kFirstWaitMs up to kMaxWaitMs), so a setting it can't
//             handle costs a short soft blink at most every 20 s. A setting changed back is picked up at once: LMU
//             makes its session anew on every graphics change (new_session: the next scene frame is a try).
// `ready` (mvr_pass.h mvr_shape_ready): the frame's eye passes had every step the focus views are made at (the
// post-processing's start, its copy, composite and final), seen while the eyes render alone too. Plain tries only
// while ready, so a pipeline that lacks a step costs no blink at all, and is picked up the frame it has them again.
// Ready and still no focus views (a step of ours failing): the timed tries above.
#pragma once

#include <algorithm>

namespace lmu {

struct MvrRecovery {
    enum class State { foveated, plain, trying };
    enum class Event { none, wentPlain, tryingAgain, recovered };

    static constexpr unsigned long long kNoFinalsFrames = 45;  // half a second at 90 Hz
    static constexpr unsigned long long kTryFrames = 6;        // the first renders at the full size still, by design
    static constexpr unsigned long long kSettledFrames = 600;  // focus views this long: the wait starts afresh
    static constexpr double kFirstWaitMs = 3000, kMaxWaitMs = 20000;

    State state = State::foveated;
    unsigned long long noFinals = 0;  // frames in a row with a pass but without both focus views
    unsigned long long good = 0;      // frames in a row with them
    unsigned long long timesPlain = 0;
    double retryAtMs = 0, waitMs = 0;
    bool stopped = false;  // stop(): plain for the rest of the session, no more tries
    bool readyFailed = false;  // the focus views were lost, or a try failed, while the frame was ready: timed tries

    bool plain() const { return state == State::plain; }

    // Multi-view rendering can't run at all (the driver refused it and instancing couldn't take over, or the game's
    // device can do neither): the eyes at full resolution for the rest of the session, no tries.
    void stop() {
        state = State::plain;
        stopped = true;
    }

    // The game made its session anew (LMU does on every graphics setting change): what stopped the focus views may
    // be gone, so the next scene frame tries, and the waits start afresh.
    void new_session() {
        retryAtMs = 0;
        waitMs = 0;
        readyFailed = false;
    }

    // One frame: `passes` eye passes found, `finals` both focus views made, at `nowMs`; `ready` as above.
    Event frame(int passes, bool finals, double nowMs, bool ready = true) {
        if (stopped)
            return Event::none;
        if (state == State::plain) {
            // Only from a frame that rendered both eyes: menus and loading screens don't use up a try. And only
            // while the frame has every step: at once if nothing of ours failed with them there, else on the timer.
            if (passes >= 2 && ready && (!readyFailed || nowMs >= retryAtMs)) {
                state = State::trying;
                noFinals = 0;
                return Event::tryingAgain;
            }
            return Event::none;
        }
        if (passes < 1)  // no scene this frame (or no pass found: the eyes are at the game's own size then)
            return Event::none;
        if (finals) {
            noFinals = 0;
            if (++good >= kSettledFrames)
                waitMs = 0;
            readyFailed = false;
            if (state == State::trying) {
                state = State::foveated;
                return Event::recovered;
            }
            return Event::none;
        }
        good = 0;
        if (++noFinals < (state == State::trying ? kTryFrames : kNoFinalsFrames))
            return Event::none;
        readyFailed = state == State::trying || ready;  // (a try is only ever made while ready)
        state = State::plain;
        noFinals = 0;
        ++timesPlain;
        waitMs = waitMs > 0 ? (std::min)(waitMs * 2, kMaxWaitMs) : kFirstWaitMs;
        retryAtMs = nowMs + waitMs;
        return Event::wentPlain;
    }
};

}  // namespace lmu

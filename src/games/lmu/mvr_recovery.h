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

    bool plain() const { return state == State::plain; }

    // The game made its session anew (LMU does on every graphics setting change): what stopped the focus views may
    // be gone, so the next scene frame tries, and the waits start afresh.
    void new_session() {
        retryAtMs = 0;
        waitMs = 0;
    }

    // One frame: `passes` eye passes found, `finals` both focus views made, at `nowMs`.
    Event frame(int passes, bool finals, double nowMs) {
        if (state == State::plain) {
            // Only from a frame that rendered both eyes: menus and loading screens don't use up a try.
            if (passes >= 2 && nowMs >= retryAtMs) {
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
            if (state == State::trying) {
                state = State::foveated;
                return Event::recovered;
            }
            return Event::none;
        }
        good = 0;
        if (++noFinals < (state == State::trying ? kTryFrames : kNoFinalsFrames))
            return Event::none;
        state = State::plain;
        noFinals = 0;
        ++timesPlain;
        waitMs = waitMs > 0 ? (std::min)(waitMs * 2, kMaxWaitMs) : kFirstWaitMs;
        retryAtMs = nowMs + waitMs;
        return Event::wentPlain;
    }
};

}  // namespace lmu

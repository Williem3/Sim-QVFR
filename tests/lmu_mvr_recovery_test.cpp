// Offline check of LMU's multi-view recovery rule (games/lmu/mvr_recovery.h): what happens when the focus views stop
// coming, in place of a fallback for the rest of the session.
#include <cstdio>

#include "games/lmu/mvr_recovery.h"

using lmu::MvrRecovery;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    using Event = MvrRecovery::Event;
    using State = MvrRecovery::State;
    const double frameMs = 11.1;
    {  // the normal run: focus views every frame, menus between
        MvrRecovery r;
        double t = 0;
        bool quiet = true;
        for (int f = 0; f < 2000; ++f, t += frameMs)
            quiet = quiet && r.frame(f % 500 < 400 ? 2 : 0, f % 500 < 400, t) == Event::none;
        check(quiet && r.state == State::foveated && r.timesPlain == 0, "focus views every scene frame, menus between: nothing happens");
    }
    {  // one bad frame (the first after loading renders at the game's own size by design), a few bad frames
        MvrRecovery r;
        double t = 0;
        bool quiet = true;
        for (int f = 0; f < 400; ++f, t += frameMs)
            quiet = quiet && r.frame(2, f % 50 >= 44 ? false : true, t) == Event::none;  // runs of 6 without
        check(quiet && r.state == State::foveated, "short runs without focus views (6 frames) don't leave the foveated state");
    }
    {  // the focus views stop for good (a post level the focus post doesn't handle)
        MvrRecovery r;
        double t = 0;
        for (int f = 0; f < 100; ++f, t += frameMs)
            r.frame(2, true, t);
        int at = -1;
        for (int f = 0; f < 100 && at < 0; ++f, t += frameMs)
            if (r.frame(2, false, t) == Event::wentPlain)
                at = f + 1;
        check(at == static_cast<int>(MvrRecovery::kNoFinalsFrames) && r.plain(),
              "45 frames in a row without focus views: plain (the eyes at full resolution), not a fallback for the session");
        check(r.waitMs == MvrRecovery::kFirstWaitMs, "the first wait before it tries again is 3 s");
        // Plain: no focus views are drawn, so none come; menus don't use up the try.
        bool held = true;
        const double enteredAt = t;
        while (t < enteredAt + 2900) {
            held = held && r.frame(2, false, t) == Event::none && r.plain();
            t += frameMs;
        }
        t = enteredAt + 3100;
        held = held && r.frame(0, false, t) == Event::none && r.plain();  // a menu frame at the wait's end
        check(held, "plain holds for its wait, and a frame without both eyes doesn't start the try");
        check(r.frame(2, false, t += frameMs) == Event::tryingAgain && r.state == State::trying, "then a scene frame starts a try");
        // The try fails (the setting is still the same): plain again after 6 frames, the wait doubled.
        int back = -1;
        for (int f = 0; f < 20 && back < 0; ++f)
            if (r.frame(2, false, t += frameMs) == Event::wentPlain)
                back = f + 1;
        check(back == static_cast<int>(MvrRecovery::kTryFrames) && r.waitMs == 6000 && r.timesPlain == 2,
              "a try without focus views in 6 frames: plain again, the next wait 6 s");
        // It keeps doubling to a minute, no further.
        for (int n = 0; n < 8; ++n) {
            t = r.retryAtMs + 1;
            r.frame(2, false, t);
            for (int f = 0; f < 6; ++f)
                r.frame(2, false, t += frameMs);
        }
        check(r.plain() && r.waitMs == MvrRecovery::kMaxWaitMs && MvrRecovery::kMaxWaitMs == 20000,
              "the wait doubles up to 20 s");
        {  // a new session (LMU's on a graphics setting change): the next scene frame tries, whatever the wait was
            MvrRecovery s = r;
            const double now = s.retryAtMs - 15000;
            const bool waiting = s.frame(2, false, now) == Event::none && s.plain();
            s.new_session();
            const bool tries = s.frame(2, false, now + frameMs) == Event::tryingAgain;
            const bool back = s.frame(2, false, now + 2 * frameMs) == Event::none &&
                              s.frame(2, true, now + 3 * frameMs) == Event::recovered;
            check(waiting && tries && back && s.waitMs == 0,
                  "a new session while plain: the next scene frame is a try, and the waits start afresh");
        }
        // The player changes the setting back: the next try gets its focus views and stays.
        t = r.retryAtMs + 1;
        r.frame(2, false, t);
        r.frame(2, false, t += frameMs);  // the first frame of a try still renders at the full size
        const Event e = r.frame(2, true, t += frameMs);
        check(e == Event::recovered && r.state == State::foveated, "a try that gets its focus views: foveated again, by itself");
        for (int f = 0; f < 700; ++f)
            r.frame(2, true, t += frameMs);
        check(r.waitMs == 0, "after 600 good frames the wait starts afresh (3 s the next time)");
    }
    {  // one pass a frame (the tracker lost the other): counted as without focus views
        MvrRecovery r;
        double t = 0;
        bool went = false;
        for (int f = 0; f < 60; ++f, t += frameMs)
            went = went || r.frame(1, false, t) == Event::wentPlain;
        check(went, "frames with one pass and no focus views lead to plain too");
        t = r.retryAtMs + 1;
        check(r.frame(1, false, t) == Event::none && r.plain(), "and a one-pass frame doesn't start a try");
    }
    {  // stopped (multi-view can't run at all): plain for good, a new session doesn't revive it
        MvrRecovery r;
        r.stop();
        r.new_session();
        bool held = r.plain();
        for (int f = 0; f < 3000; ++f)
            held = held && r.frame(2, f % 2 == 0, f * frameMs) == Event::none && r.plain();
        check(held, "stopped: plain for the rest of the session, no tries, whatever the frames bring");
    }
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}

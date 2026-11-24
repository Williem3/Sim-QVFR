// Offline check of turbo mode's frame loop (layer/turbo.h) against a fake runtime that keeps OpenXR's rules: its
// xrWaitFrame blocks until the previous frame was begun and then until the next display slot; its xrBeginFrame needs a
// wait before it. The game's xrWaitFrame returns at once once primed, with the display time moved on by whole periods;
// its xrBeginFrame holds until the runtime's wait is done; the runtime always sees wait, begin, wait, begin; an error
// from the wait on the worker comes back from xrBeginFrame and the next wait is the game's own; xrEndSession's drain
// and xrDestroySession's stop leave nothing running. Plus the guards' order (turbo_block), and the layer's own
// xrWaitFrame and xrBeginFrame with [cpu] turbo on: armed once the first frame read the settings, off with Cheeky's
// frame generation set, again without it, and xrEndSession drains.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "layer/layer.h"
#include "layer/turbo.h"
#include "test_util.h"

using namespace simqvfr::layer;

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr XrDuration kPeriod = 8'000'000;  // 8 ms slots

struct FakeRuntime {
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t waits = 0, begins = 0, calls = 0;
    int violations = 0;           // a begin with no wait before it
    int failAtWait = -1;          // that wait (1-based) fails with XR_ERROR_SESSION_LOST
    std::atomic<int> concurrentWaits{0}, maxConcurrentWaits{0};
    const int64_t start = now_ns();

    XrResult wait(XrFrameState& s) {
        const int inside = ++concurrentWaits;
        if (inside > maxConcurrentWaits)
            maxConcurrentWaits = inside;
        std::unique_lock lock(mutex);
        // OpenXR: a wait blocks until the previous frame was begun.
        cv.wait(lock, [this] { return begins == waits; });
        ++waits;
        const bool fail = failAtWait == static_cast<int>(++calls);
        lock.unlock();
        // Then until the next display slot.
        const int64_t t = now_ns() - start;
        const int64_t slot = (t / kPeriod + 1) * kPeriod;
        std::this_thread::sleep_for(std::chrono::nanoseconds(slot - t));
        --concurrentWaits;
        if (fail) {
            std::lock_guard guard(mutex);
            --waits;  // a failed wait begins no frame
            cv.notify_all();
            return XR_ERROR_SESSION_LOST;
        }
        s.predictedDisplayTime = start + slot + 2 * kPeriod;
        s.predictedDisplayPeriod = kPeriod;
        s.shouldRender = XR_TRUE;
        return XR_SUCCESS;
    }
    void begin() {
        std::lock_guard guard(mutex);
        if (waits != begins + 1)
            ++violations;
        else
            ++begins;
        cv.notify_all();
    }
};

// The runtime below the layer: OpenXR's order checked (wait, begin, end), the waits' threads counted.
struct LayerRuntime {
    std::mutex mutex;
    int waits = 0, begins = 0, ends = 0, endSessions = 0, violations = 0, workerWaits = 0, inFlight = 0;
    bool waited = false, begun = false;
    XrTime time = 1'000'000'000;
    std::thread::id game;
} lrt;

XrResult XRAPI_CALL layerFakeWait(XrSession, const XrFrameWaitInfo*, XrFrameState* s) {
    {
        std::lock_guard guard(lrt.mutex);
        ++lrt.inFlight;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // the runtime's throttle
    std::lock_guard guard(lrt.mutex);
    --lrt.inFlight;
    if (lrt.waited)
        ++lrt.violations;  // two waits with no begin between
    ++lrt.waits;
    lrt.workerWaits += std::this_thread::get_id() != lrt.game;
    lrt.waited = true;
    s->predictedDisplayTime = lrt.time += kPeriod;
    s->predictedDisplayPeriod = kPeriod;
    s->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL layerFakeBegin(XrSession, const XrFrameBeginInfo*) {
    std::lock_guard guard(lrt.mutex);
    ++lrt.begins;
    if (!lrt.waited || lrt.begun)
        ++lrt.violations;
    lrt.waited = false;
    lrt.begun = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL layerFakeEnd(XrSession, const XrFrameEndInfo*) {
    std::lock_guard guard(lrt.mutex);
    ++lrt.ends;
    if (!lrt.begun)
        ++lrt.violations;
    lrt.begun = false;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL layerFakeEndSession(XrSession) {
    std::lock_guard guard(lrt.mutex);
    ++lrt.endSessions;
    if (lrt.inFlight)  // a wait still running at the session's end
        ++lrt.violations;
    return XR_SUCCESS;
}

// `frames` frames through the layer; the runtime's waits made on the worker in them.
int layer_frames(int frames, int& errors) {
    using namespace simqvfr::layer;
    int before;
    {
        std::lock_guard guard(lrt.mutex);
        before = lrt.workerWaits;
    }
    for (int i = 0; i < frames; ++i) {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        errors += XR_FAILED(layerWaitFrame(nullptr, nullptr, &state));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // the game's work before xrBeginFrame
        errors += XR_FAILED(layerBeginFrame(nullptr, nullptr));
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        errors += XR_FAILED(layerEndFrame(nullptr, &end));
    }
    std::lock_guard guard(lrt.mutex);
    return lrt.workerWaits - before;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    {  // The display time: whole periods on from the last real one, the nearest, at least one.
        check(turbo_predicted_time(1000, 100, 100) == 1100, "one period on after one period");
        check(turbo_predicted_time(1000, 100, 0) == 1100 && turbo_predicted_time(1000, 100, 20) == 1100,
              "at least one period on, even asked at once");
        check(turbo_predicted_time(1000, 100, 140) == 1100 && turbo_predicted_time(1000, 100, 160) == 1200,
              "rounded to the nearest period");
        check(turbo_predicted_time(1000, 100, 330) == 1300, "a long frame skips the slots it missed");
        check(turbo_predicted_time(1000, 0, 500) == 1000 && turbo_predicted_time(1000, -5, 500) == 1000,
              "no period: unchanged");
    }
    {  // The guards, in order.
        check(turbo_block(false, true, false, false, false) == TurboBlock::Off, "off when not asked for");
        check(turbo_block(true, false, true, false, false) == TurboBlock::BaselineRun, "off in a baseline run");
        check(turbo_block(true, true, true, false, false) == TurboBlock::FrameGeneration, "off with frame generation");
        check(turbo_block(true, true, false, true, false) == TurboBlock::TwoLoops, "off while two_loops is armed");
        check(turbo_block(true, true, false, false, true) == TurboBlock::Chained, "off with chained structures");
        check(turbo_block(true, true, false, false, false) == TurboBlock::None, "armed otherwise");
        check(std::string(turbo_log_line(TurboBlock::FrameGeneration)) == "turbo: off (frame generation on)" &&
                  std::string(turbo_log_line(TurboBlock::Off)) == "turbo: off" &&
                  std::string(turbo_log_line(TurboBlock::None)).rfind("turbo: on", 0) == 0,
              "the log lines");
    }
    {  // The frame loop: the runtime's order kept, the game's wait returns at once, the hold moves to its begin.
        FakeRuntime rt;
        TurboWait turbo(now_ns);
        const TurboWait::RealWait real = [&rt](XrFrameState& s) { return rt.wait(s); };
        bool realFirst = false, allAsync = true, increasing = true, onTime = true, joinedEach = true;
        int64_t longestWaitNs = 0, heldTotalNs = 0;
        XrTime lastDisplay = 0, lastReal = 0;
        for (int frame = 0; frame < 40; ++frame) {
            XrFrameState s{XR_TYPE_FRAME_STATE};
            TurboWait::Waited w;
            const int64_t t0 = now_ns();
            const XrResult r = turbo.wait(true, real, s, w);
            const int64_t waited = now_ns() - t0;
            if (frame == 0) {
                realFirst = XR_SUCCEEDED(r) && w.kind == TurboWait::Kind::Real;
                lastReal = w.real.predictedDisplayTime;
            } else {
                allAsync &= XR_SUCCEEDED(r) && w.kind == TurboWait::Kind::Async;
                longestWaitNs = std::max(longestWaitNs, waited);
                // The last real display time moved on by whole periods.
                const XrDuration ahead = s.predictedDisplayTime - lastReal;
                onTime &= ahead >= kPeriod && ahead % kPeriod == 0;
            }
            increasing &= s.predictedDisplayTime > lastDisplay && s.predictedDisplayPeriod == kPeriod;
            lastDisplay = s.predictedDisplayTime;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));  // the game's work before xrBeginFrame
            const TurboWait::Begun b = turbo.begin();
            if (frame > 0) {
                joinedEach &= b.joined && XR_SUCCEEDED(b.result);
                heldTotalNs += b.heldNs;
                lastReal = b.real.predictedDisplayTime;
            }
            rt.begin();  // the runtime's xrBeginFrame, after the join
            std::this_thread::sleep_for(std::chrono::milliseconds(2));  // render, xrEndFrame
        }
        check(realFirst, "the first wait is the runtime's own, on the game's thread");
        check(allAsync, "the later waits return at once (the runtime's runs on the worker)");
        check(longestWaitNs < 3'000'000, "the game's xrWaitFrame no longer blocks for a display slot");
        check(joinedEach, "every xrBeginFrame joins its frame's wait");
        check(heldTotalNs > 0, "the hold is measured in xrBeginFrame");
        check(increasing, "display times move on and keep the period");
        check(onTime, "the display time given is the last real one, whole periods on");
        check(rt.violations == 0 && rt.waits == rt.begins && rt.waits == 40, "the runtime saw wait, begin in order");
        check(rt.maxConcurrentWaits == 1, "never two runtime waits at once");
        turbo.stop();
    }
    {  // An error from the wait on the worker: returned by xrBeginFrame, no runtime begin, the next wait the game's own.
        FakeRuntime rt;
        rt.failAtWait = 3;
        TurboWait turbo(now_ns);
        const TurboWait::RealWait real = [&rt](XrFrameState& s) { return rt.wait(s); };
        XrFrameState s{XR_TYPE_FRAME_STATE};
        TurboWait::Waited w;
        bool ok = true;
        for (int frame = 0; frame < 2; ++frame) {
            ok &= XR_SUCCEEDED(turbo.wait(true, real, s, w));
            ok &= XR_SUCCEEDED(turbo.begin().result);
            rt.begin();
        }
        ok &= XR_SUCCEEDED(turbo.wait(true, real, s, w)) && w.kind == TurboWait::Kind::Async;
        const TurboWait::Begun failed = turbo.begin();
        check(ok && failed.joined && failed.result == XR_ERROR_SESSION_LOST, "the worker's error comes from xrBeginFrame");
        const XrResult again = turbo.wait(true, real, s, w);
        check(XR_SUCCEEDED(again) && w.kind == TurboWait::Kind::Real, "after an error the next wait is real");
        turbo.begin();
        rt.begin();
        check(rt.violations == 0, "no runtime begin without its wait");
        turbo.stop();
    }
    {  // Turned off live: the next wait is real; xrEndSession's drain joins a pending wait.
        FakeRuntime rt;
        TurboWait turbo(now_ns);
        const TurboWait::RealWait real = [&rt](XrFrameState& s) { return rt.wait(s); };
        XrFrameState s{XR_TYPE_FRAME_STATE};
        TurboWait::Waited w;
        turbo.wait(true, real, s, w);
        turbo.begin();
        rt.begin();
        turbo.wait(false, real, s, w);
        check(w.kind == TurboWait::Kind::Real, "off: the runtime's own wait");
        turbo.begin();
        rt.begin();
        check(!turbo.active(), "off and begun: not in use");
        turbo.wait(true, real, s, w);  // real (primes)
        turbo.begin();
        rt.begin();
        turbo.wait(true, real, s, w);
        check(w.kind == TurboWait::Kind::Async && turbo.active(), "on again: primed by one real wait");
        check(turbo.drain(), "xrEndSession: the pending wait joined");
        check(!turbo.active(), "drained: nothing pending");
        turbo.stop();
        turbo.stop();  // twice is fine
        check(true, "stop joins the worker");
    }
    {  // A second xrWaitFrame before the frame's xrBeginFrame (another thread) blocks until that begin, as the runtime's.
        FakeRuntime rt;
        TurboWait turbo(now_ns);
        const TurboWait::RealWait real = [&rt](XrFrameState& s) { return rt.wait(s); };
        XrFrameState s{XR_TYPE_FRAME_STATE};
        TurboWait::Waited w;
        turbo.wait(true, real, s, w);
        turbo.begin();
        rt.begin();
        turbo.wait(true, real, s, w);  // async, not begun
        std::thread beginner([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            turbo.begin();
            rt.begin();
        });
        const int64_t t0 = now_ns();
        const XrResult r = turbo.wait(true, real, s, w);
        const int64_t blocked = now_ns() - t0;
        beginner.join();
        check(XR_SUCCEEDED(r) && blocked >= 25'000'000 && w.kind == TurboWait::Kind::Async,
              "the next wait held until the frame was begun");
        turbo.begin();
        rt.begin();
        check(rt.violations == 0, "and the runtime's order kept");
        turbo.stop();
    }
    {  // Through the layer: [cpu] turbo=1.
        lrt.game = std::this_thread::get_id();
        next.WaitFrame = layerFakeWait;
        next.BeginFrame = layerFakeBegin;
        next.EndFrame = layerFakeEnd;
        next.EndSession = layerFakeEndSession;
        experiment.twoLoops = false;
        experiment.cpuSettings = true;
        experiment.turbo = true;
        int errors = 0;
        const int onWorker = layer_frames(30, errors);
        check(errors == 0 && onWorker >= 25, "turbo on: the runtime's waits run on the worker after the first frames");
        experiment.fgSet = true;  // [cheeky] fg=1 read (config.cpp, live_ini.cpp)
        const int withFg = layer_frames(20, errors);
        check(errors == 0 && withFg <= 1, "Cheeky's frame generation set: turbo off, the game's own waits");
        experiment.fgSet = false;
        const int again = layer_frames(20, errors);
        check(errors == 0 && again >= 18, "frame generation off again: turbo back on");
        XrFrameState state{XR_TYPE_FRAME_STATE};
        layerWaitFrame(nullptr, nullptr, &state);  // a wait on the worker, then the session ends before its begin
        layerEndSession(nullptr);
        {
            std::lock_guard guard(lrt.mutex);
            lrt.waited = false;  // the runtime's wait ended with the session
        }
        experiment.turbo = false;
        const int off = layer_frames(10, errors);
        check(errors == 0 && off == 0, "turbo off: every wait the game's own");
        std::lock_guard guard(lrt.mutex);
        check(lrt.violations == 0 && lrt.endSessions == 1, "the runtime saw wait, begin, end in order, and no wait "
                                                           "in flight at xrEndSession");
    }
    return test_result();
}

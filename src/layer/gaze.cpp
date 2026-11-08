// Where the eyes look: the gaze direction, the gaze thread, the steady gaze and their reports.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>

#include "log.h"
#include "xr.h"
#include "layer/layer.h"

namespace simqvfr::layer {

namespace {

constexpr XrDuration kGazeHoldNs = 500'000'000;  // keep the last gaze this long, then recentre

}  // namespace

// Gaze direction in `base` at `time`; false while gaze is not tracked (then the last tracked
// direction is used for a short while, then straight ahead).
bool gaze_direction(XrSession session, XrSpace base, XrTime time, XrVector3f& direction) {
    std::lock_guard guard(gaze.mutex);
    if (!gaze.attached)
        return false;
    if (gaze.appSyncs == 0) {  // ACE never syncs actions: sync the layer's set alone
        const XrActiveActionSet active{gaze.set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        next.SyncActions(session, &sync);
    }
    if (!gaze.space) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = gaze.action;
        spaceInfo.poseInActionSpace.orientation.w = 1;
        if (XR_FAILED(next.CreateActionSpace(session, &spaceInfo, &gaze.space)))
            return false;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    ++gaze.queries;
    const bool ok = XR_SUCCEEDED(next.LocateSpace(gaze.space, base, time, &location)) &&
                    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
    if (ok) {
        ++gaze.tracked;
        gaze.direction = rotate(location.pose.orientation, {0, 0, -1});
        gaze.directionTime = time;
        if (!gaze.firstLogged)
            logf("gaze: first tracked sample, direction %.3f,%.3f,%.3f", gaze.direction.x, gaze.direction.y,
                 gaze.direction.z);
        gaze.firstLogged = true;
        runtime_info(L"eye_tracking", L"1");
    }
    if (!ok && (gaze.directionTime == 0 || time - gaze.directionTime > kGazeHoldNs))
        return false;
    direction = gaze.direction;
    return true;
}

namespace {

// While the layer's own gaze action has never tracked: after kGazeUntrackedLookups lookups, logs why (the lookup's
// result and flags, whether each gaze action is active), and moves the lookups to a space of the game's own gaze
// action if it bound one (X-Plane: the layer's action stayed untracked while the game's set and its binding were in
// place, 2026-11-06). From the gaze thread.
constexpr uint64_t kGazeUntrackedLookups = 500;

void gaze_untracked_check(XrSession session, bool ok, XrResult located, XrSpaceLocationFlags flags) {
    std::lock_guard guard(gaze.mutex);
    if (ok || gaze.tracked > 0 || gaze.queries + 1 != kGazeUntrackedLookups)
        return;
    const auto active = [&](XrAction action) {
        if (!action || !next.GetActionStatePose)
            return std::string("none");
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        const XrResult r = next.GetActionStatePose(session, &info, &state);
        return XR_SUCCEEDED(r) ? std::string(state.isActive ? "active" : "inactive") : "error " + std::to_string(r);
    };
    std::string eyesProfile = "?";
    XrPath eyes = XR_NULL_PATH;
    if (next.GetCurrentInteractionProfile && XR_SUCCEEDED(next.StringToPath(gaze.instance, "/user/eyes_ext", &eyes))) {
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        const XrResult r = next.GetCurrentInteractionProfile(session, eyes, &state);
        eyesProfile = XR_FAILED(r) ? "error " + std::to_string(r)
                      : state.interactionProfile == XR_NULL_PATH ? std::string("none")
                                                                 : path_name(gaze.instance, state.interactionProfile);
    }
    logf("gaze: no tracked sample in %llu lookups (xrLocateSpace %d, flags 0x%llx; the layer's gaze action %s, the "
         "game's %s; %llu game syncs; /user/eyes_ext profile %s)",
         static_cast<unsigned long long>(kGazeUntrackedLookups), located, static_cast<unsigned long long>(flags),
         active(gaze.action).c_str(), active(gaze.gameAction).c_str(), static_cast<unsigned long long>(gaze.appSyncs),
         eyesProfile.c_str());
    if (gaze.gameAction && !gaze.gameSpace) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = gaze.gameAction;
        spaceInfo.poseInActionSpace.orientation.w = 1;
        const XrResult r = next.CreateActionSpace(session, &spaceInfo, &gaze.gameSpace);
        if (XR_FAILED(r))
            gaze.gameSpace = XR_NULL_HANDLE;
        logf("gaze: lookups moved to the game's own gaze action: %d", r);
    }
}

void gaze_thread_run() {
    std::unique_lock lock(gazeThread.mutex);
    const auto period = std::chrono::microseconds(1'000'000 / std::max(1, experiment.gazePollHz));
    while (!gazeThread.stop) {
        const auto due = std::chrono::steady_clock::now() + period;
        const XrSession session = gazeThread.session;
        const XrSpace base = gazeThread.base;
        const XrTime time = gazeThread.time;
        lock.unlock();
        XrSpace space = XR_NULL_HANDLE;
        {
            std::lock_guard guard(gaze.mutex);
            // Attached alone (the game syncs nothing): the set synced here, off the game's thread (under the mutex, as
            // the settings menu's own sync).
            if (gaze.attached && gaze.syncsOwn && gaze.appSyncs == 0 && session) {
                const XrActiveActionSet active{gaze.set, XR_NULL_PATH};
                XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
                sync.countActiveActionSets = 1;
                sync.activeActionSets = &active;
                next.SyncActions(session, &sync);
            }
            if (gaze.attached && !gaze.space) {
                XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                spaceInfo.action = gaze.action;
                spaceInfo.poseInActionSpace.orientation.w = 1;
                next.CreateActionSpace(session, &spaceInfo, &gaze.space);
            }
            space = gaze.attached ? (gaze.gameSpace ? gaze.gameSpace : gaze.space) : XR_NULL_HANDLE;
        }
        if (space && base && time) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            const auto t0 = std::chrono::steady_clock::now();
            const XrResult located = next.LocateSpace(space, base, time, &location);
            const bool ok = XR_SUCCEEDED(located) &&
                            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            gaze_untracked_check(session, ok, located, location.locationFlags);
            const XrVector3f direction = rotate(location.pose.orientation, {0, 0, -1});
            bool first = false;
            {
                std::lock_guard guard(gaze.mutex);  // the counts the overlay and the log read
                ++gaze.queries;
                if (ok) {
                    ++gaze.tracked;
                    gaze.direction = direction;
                    gaze.directionTime = time;
                    first = !gaze.firstLogged;
                    gaze.firstLogged = true;
                }
            }
            if (first) {
                logf("gaze: first tracked sample (gaze thread, %d Hz), direction %.3f,%.3f,%.3f", experiment.gazePollHz,
                     direction.x, direction.y, direction.z);
                runtime_info(L"eye_tracking", L"1");
            }
            lock.lock();
            if (ok) {
                gazeThread.direction = direction;
                gazeThread.directionTime = time;
            }
            ++gazeThread.lookups;
            gazeThread.lookupMs += ms;
            gazeThread.lookupMaxMs = std::max(gazeThread.lookupMaxMs, ms);
        } else {
            lock.lock();
        }
        gazeThread.wake.wait_until(lock, due, [] { return gazeThread.stop; });
    }
}

}  // namespace

void gaze_thread_stop() {
    std::thread thread;
    {
        std::lock_guard guard(gazeThread.mutex);
        gazeThread.stop = true;
        thread.swap(gazeThread.thread);
    }
    gazeThread.wake.notify_all();
    if (thread.joinable())
        thread.join();
    std::lock_guard guard(gazeThread.mutex);  // ready for the next session
    gazeThread.stop = false;
    gazeThread.session = XR_NULL_HANDLE;
    gazeThread.base = XR_NULL_HANDLE;
    gazeThread.time = gazeThread.directionTime = 0;
    gazeThread.direction = {0, 0, -1};
    gazeThread.lookups = 0;
    gazeThread.lookupMs = gazeThread.lookupMaxMs = 0;
}

// The gaze for ACE's xrLocateViews in views4 (Two-loop looks it up per frame itself): from the gaze thread while it can run (it is started on the first
// call), else looked up here. False while not tracked (the last tracked gaze is kept kGazeHoldNs).
bool gaze_now(XrSession session, XrSpace base, XrTime time, XrVector3f& direction) {
    bool threaded = experiment.gazeThread;
    bool syncsOwn = false;
    if (threaded) {
        std::lock_guard guard(gaze.mutex);
        // The game syncs its actions (and ours with them), or nobody does and the thread syncs ours itself.
        syncsOwn = gaze.syncsOwn && gaze.appSyncs == 0;
        threaded = gaze.attached && (gaze.appSyncs > 0 || syncsOwn);
    }
    if (!threaded)
        return gaze_direction(session, base, time, direction);
    std::lock_guard guard(gazeThread.mutex);
    gazeThread.session = session;
    gazeThread.base = base;
    gazeThread.time = time;
    if (!gazeThread.thread.joinable() && !gazeThread.stop) {
        gazeThread.thread = std::thread(gaze_thread_run);
        logf("gaze: looked up on the layer's own thread at %d Hz (gaze_thread)%s", experiment.gazePollHz,
             syncsOwn ? ", which syncs the layer's action set itself (the game syncs none)" : "");
    }
    if (gazeThread.directionTime == 0 || time - gazeThread.directionTime > kGazeHoldNs)
        return false;
    direction = gazeThread.direction;
    return true;
}

// `direction` (unit, from gaze_now or gaze_direction) replaced by the held gaze; `tracked` false drops the hold.
void steady_gaze(bool tracked, XrVector3f& direction) {
    if (!tracked) {
        gazeHold.held = false;
        return;
    }
    const XrVector3f& h = gazeHold.direction;
    const double cosAngle = std::clamp(static_cast<double>(h.x * direction.x + h.y * direction.y + h.z * direction.z), -1.0, 1.0);
    const double deadzone = experiment.gazeDeadzone.load(std::memory_order_relaxed) * 3.14159265358979 / 180;
    ++gazeHold.samples;
    if (!gazeHold.held || std::acos(cosAngle) > deadzone) {
        gazeHold.direction = direction;
        gazeHold.held = true;
        ++gazeHold.moves;
    }
    direction = gazeHold.direction;
}

// For the log, every report: how often the held gaze moved, of the tracked frames since the last one.
void gaze_hold_report() {
    const uint64_t samples = gazeHold.samples.exchange(0), moves = gazeHold.moves.exchange(0);
    if (samples)
        logf("gaze dead-zone %.1f deg: the focus box moved on %llu of %llu tracked frames (%.0f%%)",
             experiment.gazeDeadzone.load(), moves, samples, 100.0 * moves / samples);
}

// For the log, every report: the gaze thread's lookups since the last one.
void gaze_thread_report(double seconds) {
    std::lock_guard guard(gazeThread.mutex);
    if (!gazeThread.lookups)
        return;
    logf("gaze thread: %.0f lookups/s, xrLocateSpace %.3f ms avg, %.3f ms max (off the game's render thread)",
         seconds > 0 ? gazeThread.lookups / seconds : 0.0, gazeThread.lookupMs / gazeThread.lookups, gazeThread.lookupMaxMs);
    gazeThread.lookups = 0;
    gazeThread.lookupMs = gazeThread.lookupMaxMs = 0;
}

}  // namespace simqvfr::layer

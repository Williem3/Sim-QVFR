// The layer's own work inside the game's xrEndFrame, in parts (the "views4 cpu" line's "layer" figure broken down):
// the headset menu and the overlay, the game's frame ends, the submission (the views' atlas, the output image's runtime
// calls, the composition, the helmet). Always on: one QueryPerformanceCounter pair per part, a dozen a frame. The layer
// logs them with its CPU times (layer/reports.cpp).
#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace simqvfr {

enum class FramePart : int {
    menu,       // menu_frame: the headset menu's frame, the overlay among it
    overlay,    // of menu: the performance overlay's card (osd_frame)
    keyboard,   // of menu: the hotkeys and wheel buttons read (menu_keyboard)
    controllers,  // of menu: the controllers' menu actions read from the runtime (menu_controllers), the lock included
    gazeLock,   // of controllers: waiting for the gaze's lock (the gaze thread holds it around its own runtime calls)
    actionSync, // of controllers: the layer's action set synced (only when the game syncs none)
    actionRead, // of controllers: the two action states read
    optimizer,  // of menu: the QVFR optimizer's tick
    gameEnd,    // the game adapter's frame_end (LMU: the multi-view probe's)
    qvfrEnd,    // the game adapter's qvfr_frame_end
    dlssEnd,    // of qvfrEnd: Sim-QVFR's own DLSS's frame end
    mvrEnd,     // of qvfrEnd: the multi-view passes' frame end (LMU)
    submit,     // views4_submit, the runtime's xrEndFrame included (the report takes it out)
    atlas,      // of submit: the game's views copied into the atlas (qvfr_views_atlas)
    chain,      // of submit: the output image acquired, waited for and released (the runtime's calls)
    compose,    // of submit: the composition recorded and executed on the game's context
    helmet,     // of submit: the helmet's layer
    count
};

struct FramePartSpan {
    std::atomic<uint64_t> ticks{0}, calls{0};
};
inline FramePartSpan framePartSpans[static_cast<int>(FramePart::count)];

// Times its scope into `part`.
struct FramePartTimer {
    FramePart part;
    LARGE_INTEGER t0;
    explicit FramePartTimer(FramePart p) : part(p) { QueryPerformanceCounter(&t0); }
    ~FramePartTimer() {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        FramePartSpan& s = framePartSpans[static_cast<int>(part)];
        s.ticks.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
        s.calls.fetch_add(1, std::memory_order_relaxed);
    }
    FramePartTimer(const FramePartTimer&) = delete;
    FramePartTimer& operator=(const FramePartTimer&) = delete;
};

// A part's ticks since the last take, and resets it.
inline uint64_t frame_part_take(FramePart p) {
    FramePartSpan& s = framePartSpans[static_cast<int>(p)];
    s.calls.store(0, std::memory_order_relaxed);
    return s.ticks.exchange(0, std::memory_order_relaxed);
}

}  // namespace simqvfr

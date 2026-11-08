// The adapter of a game without QVFR (adapter.h): any other process (`game` null), or a game known by its identity
// alone while it is reverse engineered. The layer then only logs: QVFR never starts and nothing changes what the game
// renders. A game at that stage can derive from it to add log-only probes (lmu/adapter.cpp).
#pragma once

#include "adapter.h"

namespace simqvfr::games {

class IdentityAdapter : public GameAdapter {
public:
    // `analysis`: a game being reverse engineered (the image dump and the game's probes may run); false in a launch its
    // anti-cheat protects, where nothing of ours runs.
    explicit IdentityAdapter(const GameInfo* game, unsigned hangWatchSeconds = 30, bool analysis = true)
        : game_(game), hangWatchSeconds_(hangWatchSeconds), analysis_(analysis) {}
    const GameInfo* info() const override { return game_; }
    bool has_qvfr() const override { return false; }
    unsigned hang_watch_s() const override { return hangWatchSeconds_; }
    bool qvfr_start(const QvfrStart&) override { return false; }
    void qvfr_set_gaze(const float (&)[2][2], const bool (&)[2], bool) override {}
    void qvfr_set_live_layout(const Views4Layout&) override {}
    void qvfr_set_live(double, double, double, bool) override {}
    bool qvfr_dlss_held() override { return false; }
    void qvfr_target_sizes(int (&render)[2], int (&output)[2]) override {
        render[0] = render[1] = output[0] = output[1] = 0;
    }
    bool qvfr_take_frame(Views4Frame&) override { return false; }
    bool qvfr_latest_frame(Views4Frame&) override { return false; }
    void qvfr_note_views(const XrView*, uint32_t) override {}
    void qvfr_frame_end() override {}
    // A game being reverse engineered: its decrypted code for static analysis when the exe's code is encrypted on
    // disk ([experiment] image_dump=0 turns it off). Derived adapters call this first.
    void instance_created(const wchar_t* ini, const wchar_t* mode) override;

protected:
    bool analysis() const { return game_ && analysis_; }

private:
    const GameInfo* game_;
    unsigned hangWatchSeconds_;
    bool analysis_;
};

}  // namespace simqvfr::games

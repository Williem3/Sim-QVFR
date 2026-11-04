#include "adapter.h"

#include "ace/adapter.h"
#include "ace/game.h"
#include "acc/game.h"

namespace aceqv::games {
namespace {

// A game without QVFR: any other process (`game` null), or a game known by its identity alone while it is
// reverse engineered (the layer then only logs). QVFR never starts and nothing else runs.
class NoQvfr final : public GameAdapter {
public:
    explicit NoQvfr(const GameInfo* game) : game_(game) {}
    const GameInfo* info() const override { return game_; }
    bool has_qvfr() const override { return false; }
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

private:
    const GameInfo* game_;
};

}  // namespace

GameAdapter& adapter_for(const GameInfo* game) {
    // A new game adds its adapter here (src/games/README.md).
    if (game == &::ace::kGame)
        return ::ace::adapter();
    if (game == &::acc::kGame) {  // identity only, being reverse engineered (acc/README.md)
        static NoQvfr acc(game);
        return acc;
    }
    static NoQvfr none(nullptr);
    return none;
}

}  // namespace aceqv::games

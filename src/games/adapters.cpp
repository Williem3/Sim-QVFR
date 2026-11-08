#include "adapter.h"

#include <windows.h>

#include <string>

#include "../log.h"
#include "../probe/image_dump.h"

#include "ace/adapter.h"
#include "ace/game.h"
#include "acc/adapter.h"
#include "acc/game.h"
#include "lmu/game.h"
#include "xplane/adapter.h"
#include "xplane/game.h"

namespace simqvfr::games {
namespace {

// A game without QVFR: any other process (`game` null), or a game known by its identity alone while it is
// reverse engineered (the layer then only logs). QVFR never starts and nothing else runs.
class NoQvfr final : public GameAdapter {
public:
    // `analysis`: a game being reverse engineered (the image dump runs); false in a launch its anti-cheat protects.
    explicit NoQvfr(const GameInfo* game, unsigned hangWatchSeconds = 30, bool analysis = true)
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
    // disk ([experiment] image_dump=0 turns it off).
    void instance_created(const wchar_t* ini, const wchar_t*) override {
        if (game_ && analysis_ && GetPrivateProfileIntW(L"experiment", L"image_dump", 1, ini) != 0)
            image_dump_if_packed();
    }

private:
    const GameInfo* game_;
    unsigned hangWatchSeconds_;
    bool analysis_;
};

}  // namespace

GameAdapter& adapter_for(const GameInfo* game) {
    static NoQvfr none(nullptr);
    if (!game)  // the layer's globals, before it knows the game
        return none;
    // A launch the game's anti-cheat protects gets nothing of ours, whatever the game's adapter: the layer only logs
    // (GameInfo::antiCheat). One game per process, so this is decided once.
    static std::wstring antiCheat;
    static const bool guarded = anticheat_running(*game, antiCheat);
    if (guarded) {
        static NoQvfr off(game, 30, false);
        static const bool logged = (logf("game: %ls's anti-cheat is running (%ls): Sim-QVFR stays off in this launch "
                                         "and only logs. Start the game from the Sim-QVFR launcher, offline.",
                                         game->name, antiCheat.c_str()),
                                    true);
        (void)logged;
        return off;
    }
    // A new game adds its adapter here (src/games/README.md).
    if (game == &::ace::kGame)
        return ::ace::adapter();
    if (game == &::acc::kGame)
        return ::acc::adapter();
    if (game == &::xplane::kGame)  // QVFR through the early load (docs/recon-xplane.md)
        return ::xplane::adapter();
    if (game == &::lmu::kGame) {  // identity only (docs/recon-lmu.md)
        static NoQvfr lmu(&::lmu::kGame);
        return lmu;
    }
    return none;
}

void early_load() {
    const GameInfo* game = current_game();
    if (!game || !game->earlyLoad)
        return;
    wchar_t ini[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"SIM_QVFR_INI", ini, MAX_PATH))
        return;
    adapter_for(game).early_start(ini);
}

}  // namespace simqvfr::games

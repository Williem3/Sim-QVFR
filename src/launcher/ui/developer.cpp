// The Developer tab ([launcher] developer=1 in the ini shows it; off by default): what recording and measuring want and
// the player's cards leave out. The showcase's live switch between Stock and QVFR in the running game and its A/B
// table, the launch switches a recording wants, the A/B measurements, and (development builds only, dev_build.h) the
// diagnostic switches (dev_switches.h).
#include <string>
#include <vector>

#include <windows.h>

#include "../../dev_build.h"
#include "../../games/game.h"
#include "../dev_switches.h"
#include "../launcher.h"
#include "../settings_ini.h"
#include "card.h"
#include "controls.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

namespace {

constexpr uint64_t kStockReadMs = 1000;  // how often the page reads the running game's [quad] stock back

// The running game's live ini (empty: no game running), where the layer reads [quad] stock.
std::wstring running_ini() { return running_live_ini(runtime_info_path()); }

// The running game's Stock or QVFR, read back now and then: the hotkey and the headset menu switch it too.
void read_stock(const std::wstring& ini) {
    const uint64_t now = GetTickCount64();
    if (ini.empty() || now - app.devStockAt < kStockReadMs)
        return;
    app.devStockAt = now;
    app.devStock = GetPrivateProfileIntW(kStock.section, kStock.key, 0, ini.c_str()) != 0;
}

// This group's switches for the game the launcher starts: on or off, at the next launch. False: none.
bool dev_switches(Body& b, DevGroup group) {
    const char* const game = games::launcher_game().id;
    bool any = false;
    for (int i = 0; i < kDevSwitchCount; ++i) {
        const DevSwitch& d = kDevSwitches[i];
        if (d.group != group || (d.game && std::string(d.game) != game))
            continue;
        b.toggle(static_cast<Ctl>(kDevSwitch0 + i), tr(d.label), app.s.dev[i] != 0, tr(d.help), false,
                 Applies::NextLaunch);
        any = true;
    }
    return any;
}

Card showcase_card() {
    Card c{Tab::Developer, tr(L"Showcase"), Applies::None};
    c.body = [](Body& b) {
        if (!game_has(&games::GameFeatures::showcase)) {
            b.note(not_yet());
            return;
        }
        const std::wstring ini = running_ini();
        read_stock(ini);
        const bool running = !ini.empty();
        b.group(tr(L"In the running game"));
        b.toggle(kDevStock, tr(L"Render Stock"), running && app.devStock,
                 !running ? tr(L"With the game running: each launch starts in QVFR.")
                 : app.devStock
                     ? tr(L"The game renders as without Sim-QVFR: its eyes at its own size, no focus views.")
                     : tr(L"QVFR as set. Ctrl+Alt+S in the game switches too."),
                 !running, Applies::Live);
        b.toggle(kDevAbTable, tr(L"A/B table"), app.s.osd.abTable,
                 tr(L"The performance overlay as Stock against QVFR: FPS, 1% lows, CPU and GPU time, pixels and "
                    L"sharpness, track frames only."),
                 false, Applies::Live);
        b.group(tr(L"At launch"));
        if (!dev_switches(b, DevGroup::Showcase))
            b.note(tr(L"Nothing to set at launch for this game."));
        b.note(tr(L"Record the same corner both ways; the table settles 1.5 s after each switch."));
    };
    c.reset = [](Settings& s) {
        s.osd.abTable = false;
        for (int i = 0; i < kDevSwitchCount; ++i)
            if (kDevSwitches[i].group == DevGroup::Showcase)
                s.dev[i] = 0;
    };
    return c;
}

Card measure_card() {
    Card c{Tab::Developer, tr(L"A/B measurements"), Applies::NextLaunch};
    c.body = [](Body& b) {
        if (!dev_switches(b, DevGroup::Measure))
            b.note(tr(L"None for this game yet."));
        b.note(tr(L"Each runs from the launch: the log's A/B lines compare the two sides."));
    };
    c.reset = [](Settings& s) {
        for (int i = 0; i < kDevSwitchCount; ++i)
            if (kDevSwitches[i].group == DevGroup::Measure)
                s.dev[i] = 0;
    };
    return c;
}

// Development builds only: a release has no diagnostic switches (dev_build.h).
Card diagnostics_dev_card() {
    Card c{Tab::Developer, tr(L"Diagnostics"), Applies::NextLaunch};
    c.body = [](Body& b) {
        if (!dev_switches(b, DevGroup::Diagnostic))
            b.note(tr(L"None for this game yet."));
        b.note(tr(L"The CPU and frame captures and detailed logs are on the Game tab's Diagnostics card."));
    };
    c.reset = [](Settings& s) {
        for (int i = 0; i < kDevSwitchCount; ++i)
            if (kDevSwitches[i].group == DevGroup::Diagnostic)
                s.dev[i] = 0;
    };
    return c;
}

Card about_dev_card() {
    Card c{Tab::Developer, tr(L"This tab"), Applies::None};
    c.body = [](Body& b) {
        b.note(tr(L"For recording and testing. Its switches stay out of profiles; Restore Defaults turns them off."));
        b.note(tr(L"developer=0 under [launcher] in sim-qvfr.ini hides this tab again."));
    };
    return c;
}

std::vector<Card> developer_cards() {
    std::vector<Card> cards = {showcase_card(), measure_card()};
    if constexpr (kDevBuild)
        cards.push_back(diagnostics_dev_card());
    cards.push_back(about_dev_card());
    return cards;
}

}  // namespace

float developer_page(float x, float y, float w, bool draw) { return card_grid(developer_cards(), x, y, w, draw); }

bool developer_press(Ctl id) {
    if (id == kDevStock) {
        const std::wstring ini = running_ini();
        if (ini.empty())
            return true;  // greyed out without a running game
        app.devStock = !app.devStock;
        app.devStockAt = GetTickCount64();  // shown as set until the next read
        settings_ini::write(ini, kStock, app.devStock ? L"1" : L"0");  // the running game's only: launches start QVFR
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return true;
    }
    if (id == kDevAbTable) {
        app.s.osd.abTable = !app.s.osd.abTable;
        if (app.s.osd.abTable)
            app.s.osd.enabled = true;  // the table is the overlay's card
        changed();
        return true;
    }
    if (id >= kDevSwitch0 && id <= kDevSwitchLast) {
        const int i = id - kDevSwitch0;
        if (i < kDevSwitchCount) {
            app.s.dev[i] = app.s.dev[i] ? 0 : kDevSwitches[i].on;
            changed();
        }
        return true;
    }
    return false;
}

static_assert(kDevSwitchCount <= kDevSwitchLast - kDevSwitch0 + 1, "a control for every Developer switch");

}  // namespace simqvfr::launcher::ui::win

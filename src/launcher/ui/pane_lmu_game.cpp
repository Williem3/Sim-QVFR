// The Game tab's In-game settings page (LMU only): Le Mans Ultimate's own graphics settings, read from and written to its
// files (lmu_game_settings.h) while the game is closed. Each change rewrites that one value in LMU's file, at once; LMU
// reads them when it starts.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "../launcher.h"
#include "../lmu_game_settings.h"
#include "../../games/game.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace ls = lmu_settings;

namespace {

int setting_index(Ctl id) { return static_cast<int>(id) - static_cast<int>(kLmuSetting); }

Ctl setting_ctl(std::size_t i) { return static_cast<Ctl>(static_cast<int>(kLmuSetting) + static_cast<int>(i)); }

// A setting this build shows (ls::shown): a release's development-only settings have no control, so nothing here writes them.
const ls::Def* def_of(Ctl id) {
    const int i = setting_index(id);
    if (i < 0 || i >= static_cast<int>(ls::defs().size()))
        return nullptr;
    const ls::Def& d = ls::defs()[static_cast<std::size_t>(i)];
    return ls::shown(d) ? &d : nullptr;
}

std::wstring widen_ascii(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool file_ok(ls::File f) { return f == ls::File::Config ? app.lmuGame.configOk : app.lmuGame.playerOk; }

bool file_read_only(ls::File f) { return f == ls::File::Config ? app.lmuGame.configReadOnly : app.lmuGame.playerReadOnly; }

// LMU keeps its settings in memory while it runs and writes them back when it closes: a change here would be lost.
bool locked() { return game_running(); }

void say(const std::wstring& text, int tone) {
    app.lmuGameMessage = text;
    app.lmuGameTone = tone;
}

// One setting written (lmu_settings::set_setting), the page read again, and what happened said.
void write(const ls::Def& d, const std::string& value) {
    if (locked()) {
        say(tr(L"Close LMU first: it writes its own settings back when it closes."), 1);
    } else {
        const std::wstring error = ls::set_setting(app.lmuGame.userFolder, d.file, d.key, value);
        if (error.empty())
            say(trf(L"{setting} saved for LMU’s next start.", {{L"setting", tr(d.label)}}), 0);
        else
            say(tr(error.c_str()), 2);
        lmu_game_refresh();
        return;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// A choice's value as shown: its option's name, else the file's own text.
std::wstring value_name(const ls::Def& d, const std::string& value) {
    const int i = ls::option_index(d, value);
    return i >= 0 ? std::wstring(tr(d.options[i].name)) : trf(L"Other ({value})", {{L"value", widen_ascii(value)}});
}

// A setting's row: a switch or a pop-up; read only (its value) while LMU runs; a note when LMU's file lacks it.
void setting_row(Body& b, std::size_t i) {
    const ls::Def& d = ls::defs()[i];
    const std::wstring label = tr(d.label), help = tr(d.help);
    if (!app.lmuGame.present[i]) {
        b.row(label, tr(L"Not in LMU’s file yet: set it once in LMU’s own menu."), L"—");
        return;
    }
    const std::string& value = app.lmuGame.values[i];
    const bool readOnly = locked() || file_read_only(d.file);
    if (d.kind == ls::Kind::Toggle) {
        bool known = false;
        const bool on = ls::toggle_on(value, known);
        b.toggle(setting_ctl(i), label, on, known ? help : trf(L"{help} (LMU’s file has “{value}”.)", {{L"help", help}, {L"value", widen_ascii(value)}}),
                 readOnly, Applies::None);
        return;
    }
    if (readOnly) {
        b.row(label, help, value_name(d, value));
        return;
    }
    std::vector<std::wstring> items;
    lmu_game_items(setting_ctl(i), items);
    b.popup(setting_ctl(i), label, value_name(d, value), items);
    b.note(help);
}

// The VR FOV scale (Config_DX11_VR.ini VRFOVScaling): anything but 100% x 100% and Sim-QVFR can't compose LMU's views.
void fov_rows(Body& b) {
    double x = 1, y = 1;
    if (app.lmuGame.fovScale.empty() || !ls::parse_fov_scale(app.lmuGame.fovScale, x, y))
        return;
    wchar_t text[64];
    std::swprintf(text, 64, L"%.0f%% × %.0f%%", x * 100, y * 100);
    if (std::fabs(x - 1) < 0.005 && std::fabs(y - 1) < 0.005) {
        b.row(tr(L"VR FOV scale"), tr(L"LMU’s own field of view scale: keep it at 100% with Sim-QVFR."), text);
        return;
    }
    b.status(tr(L"VR FOV scale"),
             tr(L"Below 100%, Sim-QVFR can’t compose LMU’s views and the headset shows a zoomed picture. Set it to 100% "
                L"and use Sim-QVFR’s FOV crop (Picture tab) instead."),
             1, text);
    if (!locked() && !app.lmuGame.configReadOnly)
        b.action(tr(L"Set the VR FOV scale to 100%"), L"", kLmuFovReset, tr(L"Set to 100%"));
}

bool any_backup() {
    return !app.lmuGame.userFolder.empty() && (ls::has_backup(ls::file_path(app.lmuGame.userFolder, ls::File::Config)) ||
                                               ls::has_backup(ls::file_path(app.lmuGame.userFolder, ls::File::Player)));
}

// The first card: where LMU's files are, whether they can be changed now, the last change's outcome, and the buttons.
Card files_card() {
    Card c{Tab::Game, tr(L"LMU’s settings files")};
    c.body = [](Body& b) {
        if (app.lmuGame.userFolder.empty()) {
            b.chip(1, tr(L"LMU not found"));
            b.note(tr(L"Sim-QVFR didn’t find Le Mans Ultimate. Choose its folder in Launch checks (Game tab)."));
            return;
        }
        if (locked())
            b.chip(1, tr(L"LMU is running: read only"));
        else if (!app.lmuGame.configOk || !app.lmuGame.playerOk)
            b.chip(1, tr(L"A file is missing: start LMU once to make it"));
        else
            b.chip(0, tr(L"Changes save at once, for LMU’s next start"));
        b.row(L"Config_DX11_VR.ini", app.lmuGame.configOk ? (app.lmuGame.configReadOnly ? tr(L"Read-only") : tr(L"Found")) : tr(L"Not found"), L"");
        b.row(L"player\\Settings.JSON", app.lmuGame.playerOk ? (app.lmuGame.playerReadOnly ? tr(L"Read-only") : tr(L"Found")) : tr(L"Not found"), L"");
        b.note(locked() ? tr(L"LMU keeps its settings while it runs and writes them back when it closes, so they can’t be "
                             L"changed here now. Change them in LMU’s menus, or here once LMU is closed.")
                        : tr(L"Only the settings you change are rewritten; the rest of each file stays as it is. Before "
                             L"the first change, a copy of each file is kept next to it (.sim-qvfr-backup)."));
        if (!app.lmuGameMessage.empty())
            b.chip(app.lmuGameTone, app.lmuGameMessage);
        std::vector<Body::ButtonSpec> buttons = {{kLmuReload, tr(L"Read again")}, {kLmuOpenFolder, tr(L"Open the folder")}};
        if (any_backup() && !locked())
            buttons.push_back({kLmuRestore, tr(L"Put back the originals…")});
        b.actions(buttons);
    };
    return c;
}

Card group_card(int group) {
    Card c{Tab::Game, tr(ls::kGroups[group]), Applies::NextLaunch};
    c.body = [group](Body& b) {
        bool any = false;
        for (std::size_t i = 0; i < ls::defs().size(); ++i)
            if (ls::defs()[i].group == group && ls::shown(ls::defs()[i]) && file_ok(ls::defs()[i].file)) {
                setting_row(b, i);
                any = true;
            }
        if (group == ls::kVrGroup && app.lmuGame.configOk) {
            fov_rows(b);
            any = true;
        }
        if (!any)
            b.note(tr(L"LMU’s file for these isn’t there yet: start LMU once."));
    };
    return c;
}

void restore_originals() {
    Sheet s;
    s.title = tr(L"Put LMU’s files back?");
    s.text = tr(L"Config_DX11_VR.ini and Settings.JSON go back to the copies kept before Sim-QVFR first changed them. "
                L"Anything changed since, here or in LMU’s own menus, is lost.");
    s.buttons = {tr(L"Cancel"), tr(L"Put back")};
    s.focus = 0;
    s.danger = true;
    s.done = [](int button, const std::wstring&) {
        if (button != 1 || locked())
            return;
        std::wstring error;
        for (const ls::File f : {ls::File::Config, ls::File::Player}) {
            const std::wstring path = ls::file_path(app.lmuGame.userFolder, f);
            if (ls::has_backup(path)) {
                const std::wstring e = ls::restore_backup(path);
                if (!e.empty() && error.empty())
                    error = e;
            }
        }
        if (error.empty())
            say(tr(L"LMU’s files are back as they were."), 0);
        else
            say(tr(error.c_str()), 2);
        lmu_game_refresh();
    };
    open_sheet(std::move(s));
}

}  // namespace

bool lmu_game_shown() { return games::wide_id(games::launcher_game()) == L"lmu"; }

void lmu_game_refresh() {
    app.lmuGame = ls::read_all(lmu_game_shown() ? ls::user_folder(launcher_game_exe()) : std::wstring());
    app.lmuGameRead = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

bool lmu_game_ctl(Ctl id) { return def_of(id) != nullptr; }

bool lmu_game_choice(Ctl id) {
    const ls::Def* d = def_of(id);
    return d && d->kind == ls::Kind::Choice;
}

int lmu_game_items(Ctl id, std::vector<std::wstring>& items) {
    items.clear();
    const ls::Def* d = def_of(id);
    if (!d || d->kind != ls::Kind::Choice)
        return 0;
    for (std::size_t i = 0; i < d->count; ++i)
        items.push_back(tr(d->options[i].name));
    const std::size_t at = static_cast<std::size_t>(setting_index(id));
    const std::string value = at < app.lmuGame.values.size() ? app.lmuGame.values[at] : std::string();
    const int selected = ls::option_index(*d, value);
    if (selected >= 0)
        return selected;
    items.push_back(trf(L"Other ({value})", {{L"value", widen_ascii(value)}}));  // LMU's own value, kept unless changed
    return static_cast<int>(items.size()) - 1;
}

void lmu_game_choose(Ctl id, int option) {
    const ls::Def* d = def_of(id);
    if (!d || d->kind != ls::Kind::Choice || option < 0 || static_cast<std::size_t>(option) >= d->count)
        return;  // "Other": the file's own value, left as it is
    write(*d, d->options[option].value);
}

void lmu_game_activate(Ctl id, int option) {
    switch (id) {
    case kLmuReload:
        app.lmuGameMessage.clear();
        lmu_game_refresh();
        return;
    case kLmuOpenFolder:
        if (!app.lmuGame.userFolder.empty())
            ShellExecuteW(app.hwnd, L"explore", app.lmuGame.userFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kLmuRestore: restore_originals(); return;
    case kLmuFovReset: {
        static const ls::Def fov{ls::File::Config, "VRFOVScaling", ls::Kind::Choice, L"VR FOV scale", L"", 0};
        write(fov, "(1.000, 1.000)");
        return;
    }
    default: break;
    }
    const ls::Def* d = def_of(id);
    if (!d)
        return;
    if (d->kind == ls::Kind::Choice) {
        if (option >= 0 && setting_index(id) >= 0)
            lmu_game_choose(id, option);
        else
            open_popup(id);
        return;
    }
    const std::size_t i = static_cast<std::size_t>(setting_index(id));
    bool known = false;
    const bool on = i < app.lmuGame.values.size() && ls::toggle_on(app.lmuGame.values[i], known);
    write(*d, ls::toggle_value(d->file, !on));
}

// The page: the files card, then a card per group of settings.
float lmu_game_page(float x, float y, float w, bool draw) {
    if (!app.lmuGameRead)
        lmu_game_refresh();
    std::vector<Card> cards = {files_card()};
    if (!app.lmuGame.userFolder.empty() && (app.lmuGame.configOk || app.lmuGame.playerOk))
        for (int g = 0; g < ls::kGroupCount; ++g)
            if (ls::group_shown(g) || g == ls::kVrGroup)  // a release has no Advanced performance card
                cards.push_back(group_card(g));
    return card_grid(cards, x, y, w, draw);
}

}  // namespace simqvfr::launcher::ui::win

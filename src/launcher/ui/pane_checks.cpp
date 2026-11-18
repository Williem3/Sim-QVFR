// The Launch checks card (preflight.h) and each check's fix.
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "../launcher.h"
#include "../preflight.h"
#include "../../games/game.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

// Launch checks (preflight.h) as a card: the verdict first, then what needs attention, each with its fix, then what is
// ready behind a More.
Card checks_card() {
    Card c{Tab::Game, tr(L"Launch checks")};
    c.body = [](Body& b) {
        static const wchar_t* const kToneText[] = {I18N(L"OK"), I18N(L"Check"), I18N(L"Problem"),
                                                   I18N(L"Checking")};  // CheckTone's order
        size_t bad = 0, warn = 0;
        std::vector<size_t> attention, ready;
        for (size_t i = 0; i < app.checks.size(); ++i) {
            const CheckTone tone = app.checks[i].tone;
            bad += tone == CheckTone::Bad ? 1 : 0;
            warn += tone == CheckTone::Warn ? 1 : 0;
            (tone == CheckTone::Bad || tone == CheckTone::Warn ? attention : ready).push_back(i);
        }
        if (bad > 0)
            b.chip(2, trn(bad, L"{count} problem · Launch will ask you to confirm first",
                          L"{count} problems · Launch will ask you to confirm first",
                          {{L"count", std::to_wstring(bad)}}));
        else if (warn > 0)
            b.chip(1, trn(warn, L"{count} to check", L"{count} to check", {{L"count", std::to_wstring(warn)}}));
        else
            b.chip(0, tr(L"Ready to launch"));
        const auto check_row = [](Body& r, size_t i) {
            const Check& k = app.checks[i];
            const std::wstring feature = check_feature(k);
            const bool attention = k.tone == CheckTone::Bad || k.tone == CheckTone::Warn;
            if (attention && !feature.empty()) {  // a way out first: turn it off; then install it (Cheeky) or its own fix
                r.row(k.title, k.detail, L"");  // the card's chip says it's a problem: its title gets the width
                std::vector<Body::ButtonSpec> ways = {{kCheckOff, trf(L"Turn {feature} off", {{L"feature", feature}}),
                                                       Btn::Primary, nullptr, static_cast<int>(i)}};
                if (k.id == "cheeky_runtime" || k.id == "cheeky_layer")
                    ways.push_back(
                        {kCheckGuide, tr(L"How to install it"), Btn::Secondary, nullptr, static_cast<int>(i)});
                else if (!k.fixLabel.empty())
                    ways.push_back({kCheckFix, k.fixLabel, Btn::Secondary, nullptr, static_cast<int>(i)});
                r.actions(ways);
            } else if (!k.fixLabel.empty()) {
                r.action(k.title, k.detail, kCheckFix, k.fixLabel, static_cast<int>(i));
            } else {
                r.status(k.title, k.detail, static_cast<int>(k.tone), tr(kToneText[static_cast<int>(k.tone)]));
            }
        };
        for (size_t i : attention)
            check_row(b, i);
        std::vector<std::wstring> labels;
        for (size_t i : ready)
            labels.push_back(app.checks[i].title);
        const long long readyCount = static_cast<long long>(ready.size());
        if (!ready.empty())
            b.more(attention.empty() ? trn(readyCount, L"all {count} check", L"all {count} checks",
                                           {{L"count", std::to_wstring(ready.size())}})
                                     : trn(readyCount, L"{count} ready check", L"{count} ready checks",
                                           {{L"count", std::to_wstring(ready.size())}}),
                   labels,
                   [ready, check_row](Body& m) {
                       for (size_t i : ready)
                           check_row(m, i);
                   });
    };
    return c;
}

// The feature a check is about, when turning it off clears the check: Cheeky Foveated DLSS's files and versions, Sim-QVFR's
// own DLSS file.
std::wstring check_feature(const Check& c) {
    if (c.id == "cheeky_runtime" || c.id == "cheeky_layer" || c.id == "dlss_version")
        return L"Foveated DLSS";
    if (c.id == "own_dlss")
        return L"DLSS";
    return L"";
}

// Turn it off ("Turn Foveated DLSS off"), as its switch would.
void turn_off_for(const Check& c) {
    const std::wstring feature = check_feature(c);
    if (feature == L"Foveated DLSS")
        app.s.cheeky.load = false;
    else if (feature == L"DLSS")
        app.s.dlss.on = false;
    else
        return;
    changed();
    refresh_checks();
}

// A launch check's button (preflight.h's CheckFix).
void fix_check(const Check& c) {
    switch (c.fix) {
    case CheckFix::OpenReleases: ShellExecuteW(app.hwnd, L"open", kReleasesUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    case CheckFix::OpenGuide:
        ShellExecuteW(app.hwnd, L"open", c.fixTarget.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case CheckFix::StartSteam: ShellExecuteW(app.hwnd, L"open", L"steam://open/main", nullptr, nullptr, SW_SHOWNORMAL); break;
    case CheckFix::ShowFile: {
        const std::wstring select = L"/select,\"" + c.fixTarget + L"\"";
        ShellExecuteW(app.hwnd, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    case CheckFix::OpenLayers:  // the card, its list open
        jump_to({tr(L"OpenXR layers"), tr(L"OpenXR layers"), Pane::Game, L"",
                 std::wstring(tr(L"OpenXR layers")) + L"/" + tr(kLayersMore)});
        return;
    case CheckFix::OpenDlss: {  // the card with DLSS's switch, highlighted
        const bool own = game_has(&games::GameFeatures::sideLoadedDlss);
        jump_to({own ? (fsr_pc() ? tr(L"Use FSR") : tr(L"Use DLSS")) : tr(L"Use Cheeky Foveated DLSS"),
                 own ? (fsr_pc() ? tr(L"FSR (experimental)") : L"DLSS") : tr(L"Foveated DLSS"), Pane::Picture});
        return;
    }
    case CheckFix::OpenFoveation: jump_to({tr(L"Foveation"), tr(L"Foveation"), Pane::Picture}); return;
    case CheckFix::TurboOff:
        if (!toolkit_turbo_off(launcher_game_exe()))
            notify(tr(L"Turbo mode is still on"),
                   trf(L"OpenXR Toolkit’s settings for {game} couldn’t be changed. Turn Turbo mode off in Toolkit’s "
                       L"menu in the game.",
                       {{L"game", short_name()}}));
        break;
    case CheckFix::ChooseGameFolder: choose_game_folder(); break;
    case CheckFix::None: return;
    }
    refresh_checks();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

}  // namespace simqvfr::launcher::ui::win

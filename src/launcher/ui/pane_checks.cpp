// The Launch checks pane (preflight.h) and each check's fix.
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "../launcher.h"
#include "../preflight.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

// Launch checks (preflight.h): what needs attention first, each with its fix, then what is ready.
float checks_pane(float x, float y, float w, bool draw) {
    static const wchar_t* const kToneText[] = {L"OK", L"Check", L"Problem", L"Checking"};  // CheckTone's order
    // Each row: its fix's button, else a badge with its state (the section says which need attention).
    const auto check_row = [](size_t i) {
        const Check& c = app.checks[i];
        Row row;
        row.label = c.title;
        row.desc = c.detail;
        if (!c.fixLabel.empty()) {
            row.controlW = button_width(c.fixLabel);
            row.controlH = t::kControlHeight;
            row.control = [i](const Rect& r) {
                button(kCheckFix, r, app.checks[i].fixLabel, false);
                app.hits.back().option = static_cast<int>(i);
            };
        } else {
            const int tone = static_cast<int>(c.tone);  // the badge's tones are CheckTone's
            const std::wstring text = kToneText[tone];
            row.controlW = badge_width(text);
            row.controlH = 20;
            row.control = [tone, text](const Rect& r) { badge(r.x, r.y, tone, text); };
        }
        return row;
    };
    std::vector<Row> attention, ready;
    for (size_t i = 0; i < app.checks.size(); ++i)
        (app.checks[i].tone == CheckTone::Bad || app.checks[i].tone == CheckTone::Warn ? attention : ready).push_back(check_row(i));
    float h = 0;
    if (!attention.empty())
        h += section(x, y + h, w, L"Needs attention", attention,
                     L"Launch asks before starting the game while a problem would crash it or leave Sim-QVFR off.", draw) +
             t::kSpace6;
    if (!ready.empty())
        h += section(x, y + h, w, attention.empty() ? L"Ready to launch" : L"Ready", ready,
                     attention.empty() ? L"Nothing found that would stop Sim-QVFR at the next launch." : L"", draw);
    return h;
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
    case CheckFix::OpenLayers: activate(kPage, static_cast<int>(Pane::Layers)); return;
    case CheckFix::OpenDlss:
        if (pane_shown(static_cast<int>(Pane::Dlss)))
            activate(kPage, static_cast<int>(Pane::Dlss));
        return;
    case CheckFix::OpenFoveation: activate(kPage, static_cast<int>(Pane::Foveation)); return;
    case CheckFix::TurboOff:
        if (!toolkit_turbo_off(launcher_game_exe()))
            notify(L"Turbo mode is still on", L"OpenXR Toolkit’s settings for " + short_name() +
                                                  L" couldn’t be changed. Turn Turbo mode off in Toolkit’s menu in the game.");
        break;
    case CheckFix::ChooseGameFolder: choose_game_folder(); break;
    case CheckFix::None: return;
    }
    refresh_checks();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

}  // namespace simqvfr::launcher::ui::win

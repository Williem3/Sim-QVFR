// The About pane: the version, Ko-fi, the update check and the disclaimer.
#include <string>
#include <vector>

#include <windows.h>

#include "../launch_plan.h"
#include "../update_check.h"
#include "launcher_version.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

float about_pane(float x, float y, float w, bool draw) {
    float h = 0;
    const float lh = t::kBody.lineHeight;
    if (draw) {
        app.canvas.text(L"Sim-QVFR Launcher", t::kTitle1, {x, y, w, t::kTitle1.lineHeight}, col(app.c->label));
        app.canvas.text(L"Starts " + game_name() + L" with foveated rendering enabled.", t::kBody,
                        {x, y + t::kTitle1.lineHeight + 8, w, lh}, col(app.c->label_secondary));
        // A packaged build has its release's version; a development build ("dev") names the release it is headed for.
        const std::wstring version = known_version(widen(SIM_QVFR_VERSION))
                                         ? widen(SIM_QVFR_VERSION)
                                         : widen(SIM_QVFR_RELEASE) + L" · development build";
        app.canvas.text(L"Version " + version, t::kBody, {x, y + t::kTitle1.lineHeight + 8 + lh + 4, w, lh},
                        col(app.c->label_secondary));
    }
    h += t::kTitle1.lineHeight + 8 + lh + 4 + lh;
    // Support, before Updates so a short window still shows it: a line and a button that opens the Ko-fi page.
    const std::wstring support = L"If Sim-QVFR improves your races, you can support its development on Ko-fi.";
    const float sh = app.canvas.measure_height(support, t::kCallout, w);
    if (draw)
        app.canvas.text(support, t::kCallout, {x, y + h + t::kSpace6, w, sh + 2}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    h += t::kSpace6 + sh + t::kSpace2;
    const float bw = button_width(L"Support on Ko-fi") + 24;
    if (draw)
        button(kSupport, {x, y + h, bw, t::kControlHeight}, L"Support on Ko-fi", Btn::Support, kIconHeart);
    h += t::kControlHeight + t::kSpace6;
    // Updates (update_check.h): the start-up switch, then where the last check stands with Check now and Download.
    Row autoCheck;
    const std::wstring autoDesc = L"Asks GitHub for the newest Sim-QVFR release when the launcher starts. Nothing else "
                                  L"is sent, and nothing downloads by itself.";
    autoCheck.fullHeight = [autoDesc](float width) { return checkbox_height(autoDesc, width); };
    autoCheck.full = [autoDesc](const Rect& r) {
        checkbox(kUpdateCheck, r, app.s.updateCheck, L"Check for updates when the launcher starts", autoDesc);
    };
    Row status;
    const std::wstring mine = widen(SIM_QVFR_VERSION);
    switch (app.update) {
    case Update::Off: status.label = L"Not checked"; break;
    case Update::Checking: status.label = L"Checking…"; break;
    case Update::UpToDate: status.label = L"Up to date"; status.desc = L"The newest release is " + app.release.tag + L"."; break;
    case Update::Available:
        status.label = app.release.tag + L" is available";
        status.desc = L"You have " + mine + L".";
        if (app.installing)
            status.desc = app.installProgress.step + L"… " + std::to_wstring(static_cast<int>(app.installProgress.share * 100)) + L"%";
        else if (!app.installError.empty())
            status.desc = L"The install stopped: " + app.installError + L". Nothing was changed.";
        else if (!installable())
            status.desc += L" Download it by hand: this copy can’t install it itself (a development build, or the release "
                           L"has no checked zip).";
        break;
    case Update::Dev:
        status.label = L"Newest release: " + app.release.tag;
        status.desc = L"This is a development build (" + mine + L").";
        break;
    case Update::Failed: status.label = L"Couldn’t check"; status.desc = app.updateError + L"."; break;
    }
    // The buttons in a row of their own under it (the status's text keeps the row's width): Install (primary) where
    // this copy can install the release, Download (its page; the primary without Install), Check now. None while
    // installing.
    std::vector<Row> updateRows = {autoCheck, status};
    if (!app.installing) {
        const bool install = installable();
        const bool download = (app.update == Update::Available || app.update == Update::Dev) && !app.release.url.empty();
        const std::wstring installLabel = app.installError.empty() ? L"Install " + app.release.tag : L"Try again";
        Row actions;
        actions.fullHeight = [](float) { return t::kControlHeight; };
        actions.full = [install, download, installLabel](const Rect& r) {
            float bx = r.x;
            const auto add = [&bx, &r](Ctl id, const std::wstring& label, bool primary) {
                const float bw = button_width(label);
                button(id, {bx, r.y, bw, t::kControlHeight}, label, primary);
                bx += bw + t::kSpace2;
            };
            if (install)
                add(kUpdateInstall, installLabel, true);
            if (download)
                add(kUpdateDownload, L"Download", !install);
            add(kUpdateNow, L"Check now", false);
        };
        updateRows.push_back(actions);
    }
    h += section(x, y + h, w, L"Updates", updateRows,
                 L"Install keeps your settings, profiles and changed files; Download opens the release’s page.",
                 draw);
    // The same notice as the README's: unofficial, at the user's own risk.
    const std::wstring notice =
        L"Sim-QVFR is not made, approved or sanctioned by Kunos Simulazioni or the developers of the games it "
        L"supports. It hooks the game's code and changes how it renders, so a game update can break it, and "
        L"it can crash the game. Use it in offline sessions only, and please don't report problems you see "
        L"while using it to the game's developers.";
    const float nh = app.canvas.measure_height(notice, t::kCallout, w);
    if (draw) {
        app.canvas.text(L"Experimental and unofficial. Use at your own risk.", t::kHeadline,
                        {x, y + h + t::kSpace6, w, 16}, col(app.c->label));
        app.canvas.text(notice, t::kCallout, {x, y + h + t::kSpace6 + 16 + t::kSpace2, w, nh + 2},
                        col(app.c->label_secondary), Align::Leading, false, true);
    }
    h += t::kSpace6 + 16 + t::kSpace2 + nh;
#ifdef SIM_QVFR_NGX
    // NVIDIA's RTX SDKs licence: an application with the NGX SDK credits it in its about box (text only: NVIDIA's
    // marks themselves need NVIDIA's approval first). NVIDIA's names stand on their own, never joined to ours (its
    // supplement, 7.3b): "Sim-QVFR uses NVIDIA DLSS", not "Sim-QVFR's DLSS".
    const std::wstring nvidia =
        L"In Le Mans Ultimate, Sim-QVFR uses NVIDIA DLSS through the NVIDIA DLSS SDK and includes NVIDIA’s "
        L"nvngx_dlss.dll, under NVIDIA’s RTX SDKs licence (licenses\\NVIDIA-RTX-SDKs-LICENSE.txt; your terms for "
        L"NVIDIA’s parts: THIRD_PARTY_NOTICES.md). NVIDIA, RTX and DLSS are trademarks and/or registered trademarks of "
        L"NVIDIA Corporation in the U.S. and other countries. Sim-QVFR is not sponsored or endorsed by NVIDIA.";
    const float vh = app.canvas.measure_height(nvidia, t::kCallout, w);
    if (draw) {
        app.canvas.text(L"NVIDIA DLSS", t::kHeadline, {x, y + h + t::kSpace6, w, 16}, col(app.c->label));
        app.canvas.text(nvidia, t::kCallout, {x, y + h + t::kSpace6 + 16 + t::kSpace2, w, vh + 2},
                        col(app.c->label_secondary), Align::Leading, false, true);
    }
    h += t::kSpace6 + 16 + t::kSpace2 + vh;
#endif
    return h;
}

}  // namespace simqvfr::launcher::ui::win

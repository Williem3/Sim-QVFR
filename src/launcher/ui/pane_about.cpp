// The About pane: the version, Ko-fi, the language, the update check and the disclaimer. NVIDIA's and AMD's
// attributions stay in English: their licences' wording.
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
        const std::wstring starts = trf(L"Starts {game} with foveated rendering enabled.", {{L"game", game_name()}});
        app.canvas.text(starts, t::kBody,
                        {x, y + t::kTitle1.lineHeight + 8, w, lh}, col(app.c->label_secondary));
        // A packaged build has its release's version; a development build ("dev") names the release it is headed for.
        const std::wstring version = known_version(widen(SIM_QVFR_VERSION))
                                         ? widen(SIM_QVFR_VERSION)
                                         : trf(L"{version} · development build", {{L"version", widen(SIM_QVFR_RELEASE)}});
        const std::wstring versionLine = trf(L"Version {version}", {{L"version", version}});
        app.canvas.text(versionLine, t::kBody, {x, y + t::kTitle1.lineHeight + 8 + lh + 4, w, lh},
                        col(app.c->label_secondary));
        reader_text({x, y, w, t::kTitle1.lineHeight}, L"Sim-QVFR Launcher", 2);
        reader_text({x, y + t::kTitle1.lineHeight + 8, w, 2 * lh + 4},
                    starts + L" " + versionLine);
    }
    h += t::kTitle1.lineHeight + 8 + lh + 4 + lh;
    // Support, before Updates so a short window still shows it: a line and a button that opens the Ko-fi page.
    const std::wstring support = tr(L"If Sim-QVFR improves your races, you can support its development on Ko-fi.");
    const float sh = app.canvas.measure_height(support, t::kCallout, w);
    if (draw) {
        reader_text({x, y + h + t::kSpace6, w, sh}, support);
        app.canvas.text(support, t::kCallout, {x, y + h + t::kSpace6, w, sh + 2}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    }
    h += t::kSpace6 + sh + t::kSpace2;
    const std::wstring supportLabel = tr(L"Support on Ko-fi");
    const float bw = button_width(supportLabel) + 24;
    if (draw)
        button(kSupport, {x, y + h, bw, t::kControlHeight}, supportLabel, Btn::Support, kIconHeart);
    h += t::kControlHeight + t::kSpace6;
    // Language (lang.h): Windows' language, English, or a lang\ file; it applies at once.
    {
        std::vector<std::wstring> names;
        const int selected = language_items(names);
        Row language;
        language.label = tr(L"Launcher language");
        language.desc = tr(L"The launcher’s and the headset menu’s text; it changes in a running game too. "
                           L"Translations are files in the lang folder; a line not translated yet shows in English.");
        language.controlW = popup_width(names.data(), static_cast<int>(names.size()), 160);
        language.controlH = t::kControlHeight;
        const std::wstring value = names[static_cast<size_t>(selected)];
        language.control = [value](const Rect& r) { popup_button(kLanguage, r, value, false); };
        h += section(x, y + h, w, tr(L"Language"), {language}, L"", draw) + t::kSpace6;
    }
    // Updates (update_check.h): the start-up switch, then where the last check stands with Check now and Download.
    Row autoCheck;
    const std::wstring autoDesc = tr(L"Asks GitHub for the newest Sim-QVFR release when the launcher starts. Nothing else "
                                     L"is sent, and nothing downloads by itself.");
    autoCheck.fullHeight = [autoDesc](float width) { return checkbox_height(autoDesc, width); };
    autoCheck.full = [autoDesc](const Rect& r) {
        checkbox(kUpdateCheck, r, app.s.updateCheck, tr(L"Check for updates when the launcher starts"), autoDesc);
    };
    Row status;
    const std::wstring mine = widen(SIM_QVFR_VERSION);
    switch (app.update) {
    case Update::Off: status.label = tr(L"Not checked"); break;
    case Update::Checking: status.label = tr(L"Checking…"); break;
    case Update::UpToDate:
        status.label = tr(L"Up to date");
        status.desc = trf(L"The newest release is {version}.", {{L"version", app.release.tag}});
        break;
    case Update::Available:
        status.label = trf(L"{version} is available", {{L"version", app.release.tag}});
        status.desc = trf(L"You have {version}.", {{L"version", mine}});
        if (app.installing)
            status.desc = app.installProgress.step + L"… " + std::to_wstring(static_cast<int>(app.installProgress.share * 100)) + L"%";
        else if (!app.installError.empty())
            status.desc = trf(L"The install stopped: {error}. Nothing was changed.", {{L"error", app.installError}});
        else if (!installable())
            status.desc += L" " + std::wstring(tr(L"Download it by hand: this copy can’t install it itself (a development "
                                                  L"build, or the release has no checked zip)."));
        break;
    case Update::Dev:
        status.label = trf(L"Newest release: {version}", {{L"version", app.release.tag}});
        status.desc = trf(L"This is a development build ({version}).", {{L"version", mine}});
        break;
    case Update::Failed: status.label = tr(L"Couldn’t check"); status.desc = app.updateError + L"."; break;
    }
    // The buttons in a row of their own under it (the status's text keeps the row's width): Install (primary) where
    // this copy can install the release, Download (its page; the primary without Install), Check now. None while
    // installing.
    std::vector<Row> updateRows = {autoCheck, status};
    if (!app.installing) {
        const bool install = installable();
        const bool download = (app.update == Update::Available || app.update == Update::Dev) && trusted_release_page(app.release.url);
        const std::wstring installLabel = app.installError.empty() ? trf(L"Install {version}", {{L"version", app.release.tag}})
                                                                   : std::wstring(tr(L"Try again"));
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
                add(kUpdateDownload, tr(L"Download"), !install);
            add(kUpdateNow, tr(L"Check now"), false);
        };
        updateRows.push_back(actions);
    }
    h += section(x, y + h, w, tr(L"Updates"), updateRows,
                 tr(L"Install keeps your settings, profiles and changed files; Download opens the release’s page."), draw);
    // The same notice as the README's: unofficial, at the user's own risk.
    const std::wstring notice =
        tr(L"Sim-QVFR is not made, approved or sanctioned by Kunos Simulazioni or the developers of the games it "
           L"supports. It hooks the game's code and changes how it renders, so a game update can break it, and "
           L"it can crash the game. Use it in offline sessions only, and please don't report problems you see "
           L"while using it to the game's developers.");
    const std::wstring warning = tr(L"Experimental and unofficial. Use at your own risk.");
    const float nh = app.canvas.measure_height(notice, t::kCallout, w);
    if (draw) {
        app.canvas.text(warning, t::kHeadline,
                        {x, y + h + t::kSpace6, w, 16}, col(app.c->label));
        reader_text({x, y + h + t::kSpace6, w, 16 + t::kSpace2 + nh}, warning + L" " + notice);
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
        reader_text({x, y + h + t::kSpace6, w, 16 + t::kSpace2 + vh}, L"NVIDIA DLSS. " + nvidia);
        app.canvas.text(nvidia, t::kCallout, {x, y + h + t::kSpace6 + 16 + t::kSpace2, w, vh + 2},
                        col(app.c->label_secondary), Align::Leading, false, true);
    }
    h += t::kSpace6 + 16 + t::kSpace2 + vh;
#endif
#ifdef SIM_QVFR_FFX
    // AMD's FSR SDK licence: redistributions reproduce AMD's notices (licenses\AMD-FidelityFX-SDK-LICENSE.md, beside
    // AMD's signed DLLs in layer\). AMD's names stand on their own, as NVIDIA's do.
    const std::wstring amd =
        L"In Le Mans Ultimate on a PC without an NVIDIA RTX graphics card, Sim-QVFR uses AMD FSR (experimental) through "
        L"the AMD FSR SDK and includes AMD’s amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll, under "
        L"AMD’s licence (licenses\\AMD-FidelityFX-SDK-LICENSE.md). AMD, FSR and FidelityFX are trademarks of Advanced Micro "
        L"Devices, Inc. Sim-QVFR is not sponsored or endorsed by AMD.";
    const float ah = app.canvas.measure_height(amd, t::kCallout, w);
    if (draw) {
        app.canvas.text(L"AMD FSR", t::kHeadline, {x, y + h + t::kSpace6, w, 16}, col(app.c->label));
        reader_text({x, y + h + t::kSpace6, w, 16 + t::kSpace2 + ah}, L"AMD FSR. " + amd);
        app.canvas.text(amd, t::kCallout, {x, y + h + t::kSpace6 + 16 + t::kSpace2, w, ah + 2},
                        col(app.c->label_secondary), Align::Leading, false, true);
    }
    h += t::kSpace6 + 16 + t::kSpace2 + ah;
#endif
    return h;
}

}  // namespace simqvfr::launcher::ui::win

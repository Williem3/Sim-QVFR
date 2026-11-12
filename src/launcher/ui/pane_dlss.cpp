// The DLSS pane: Cheeky Foveated DLSS ([cheeky]) for a game with DLSS of its own, Sim-QVFR's own DLSS ([dlss]) for a
// game that ships none (GameFeatures::sideLoadedDlss: LMU).
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../preflight.h"
#include "../settings.h"
#include "../../engine/dlss_settings.h"
#include "../../engine/nvapi_gpus.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

bool own_dlss_available(std::wstring* why) {
    std::wstring reason;
    if (!game_has(&games::GameFeatures::sideLoadedDlss))
        reason = not_yet();
    else if (app.s.mode != RenderMode::Qvfr)
        reason = L"Available in Quad View Foveated Rendering.";
    else if (!nvapi::two_views_gpu())
        reason = L"Needs an NVIDIA RTX graphics card (20 series or newer): none was found.";
    if (why)
        *why = reason;
    return reason.empty();
}

bool own_nr_available(std::wstring* why) {
    std::wstring reason;
    if (!own_dlss_available(&reason))
        ;  // DLSS's own reason
    else if (!app.s.dlss.on)
        reason = L"Needs DLSS on, above: it works from DLSS’s depth and motion.";
    else if (app.s.nr.folder.empty())
        reason = L"Choose your nvngx_dlssnr.dll below first.";
    else if (GetFileAttributesW((app.s.nr.folder + L"\\nvngx_dlssnr.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        reason = L"nvngx_dlssnr.dll isn’t in the folder chosen for it.";
    if (why)
        *why = reason;
    return reason.empty();
}

namespace {

// DLSS-NR ([dlss_nr]): NVIDIA's neural rendering on the views Sim-QVFR's own DLSS works on (engine/nr_views.h).
float own_nr_pane(float x, float y, float w, bool draw) {
    float h = 0;
    std::wstring why;
    const bool available = own_nr_available(&why);
    const NrSettings& n = app.s.nr;
    Row use;
    const std::wstring useDesc =
        available ? L"NVIDIA’s DLSS neural rendering (DLSS-NR) on " + game_name() + L"’s views: its model reworks lighting and "
                    L"materials from the picture, its depth and its motion. Experimental, and heavy: about 2 ms of GPU time "
                    L"and 130 MB of video memory for every million pixels it works on, per view (RTX 5070 Ti). It turns "
                    L"itself off if frames fall under 10 a second with it on."
                  : why;
    use.fullHeight = [useDesc](float width) { return checkbox_height(useDesc, width); };
    use.full = [useDesc, available, on = n.on](const Rect& r) { checkbox(kNrOn, r, on, L"Use DLSS-NR", useDesc, !available); };
    const std::wstring dll = n.folder.empty() ? std::wstring() : n.folder + L"\\nvngx_dlssnr.dll";
    const bool found = !dll.empty() && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::wstring version = found ? file_version(dll) : std::wstring();
    Row file;
    file.label = L"nvngx_dlssnr.dll";
    file.desc = n.folder.empty() ? std::wstring(L"Not chosen. Sim-QVFR includes none: it is NVIDIA’s, and comes only with games "
                                               L"that ship DLSS-NR. Choose the one in such a game’s folder.")
                : found ? (version.empty() ? std::wstring(L"Found") : L"Version " + version) + L", in " + n.folder + L"."
                        : L"Not found in " + n.folder + L".";
    file.controlW = button_width(L"Choose…");
    file.controlH = t::kControlHeight;
    file.control = [](const Rect& r) { button(kNrFolder, r, L"Choose…", false); };
    h += section(x, y, w, L"DLSS-NR (experimental)", {use, file},
                 L"On and off apply while the game runs; another nvngx_dlssnr.dll at the next launch.", draw, Applies::Live) +
         t::kSpace6;
    const bool off = !available || !n.on;
    std::wstring styles[std::size(kNrStyles)], presets[std::size(kNrPresets)];
    for (size_t i = 0; i < std::size(kNrStyles); ++i)
        styles[i] = kNrStyles[i].name;
    for (size_t i = 0; i < std::size(kNrPresets); ++i)
        presets[i] = kNrPresets[i].name;
    Row style = choice_row(kNrStyle, L"Style", styles, static_cast<int>(std::size(styles)), dlss_choice_index(kNrStyles, n.style), off);
    style.desc = L"The look the model aims for.";
    Row preset = choice_row(kNrPreset, L"Preset", presets, static_cast<int>(std::size(presets)),
                            dlss_choice_index(kNrPresets, n.preset), off);
    preset.desc = L"The model: A is the original, B a faster one in newer DLLs (310.9.16 on). Default is the DLL’s own "
                  L"choice. A change rebuilds DLSS-NR: a short stutter.";
    Row mask;
    const std::wstring maskDesc = L"Lets the model find what it treats differently (skin) by itself.";
    mask.fullHeight = [maskDesc](float width) { return checkbox_height(maskDesc, width); };
    mask.full = [maskDesc, off, on = n.autoMask](const Rect& r) { checkbox(kNrAutoMask, r, on, L"Auto mask", maskDesc, off); };
    h += section(x, y + h, w, L"The model",
                 {slider_row(kNrIntensity, L"Intensity", L"How much of the model’s change you get: 0% is the picture as it came.",
                             n.intensity, L"", off),
                  style, preset,
                  slider_row(kNrLocalTone, L"Local tone", L"The strength of its local contrast and lighting changes.", n.localTone,
                             L"", off),
                  slider_row(kNrLocalStructure, L"Local structure", L"The strength of its changes to fine structure.",
                             n.localStructure, L"", off),
                  mask},
                 L"These follow at once, without a stutter (but Preset).", draw, Applies::Live) +
         t::kSpace6;
    const std::wstring orders[] = {L"After DLSS", L"Before DLSS"};
    Row order = choice_row(kNrOrder, L"Runs", orders, 2, n.beforeDlss ? 1 : 0, off);
    order.desc = L"After DLSS works on the finished, anti-aliased views. Before DLSS works on the views as drawn, before "
                 L"anti-aliasing and upscaling: fewer pixels when the focus views are upscaled, and DLSS then smooths its "
                 L"result.";
    Row periphery;
    const std::wstring peripheryDesc = L"Off, only the focus views are processed: where you look. On, the periphery too, at "
                                       L"its own working size and coverage below. It adds about 2 ms of GPU time per eye at "
                                       L"least, however small you make it.";
    periphery.fullHeight = [peripheryDesc](float width) { return checkbox_height(peripheryDesc, width); };
    periphery.full = [peripheryDesc, off, on = n.periphery](const Rect& r) {
        checkbox(kNrPeriphery, r, on, L"Periphery too", peripheryDesc, off);
    };
    const bool noPeriphery = off || !n.periphery;
    h += section(x, y + h, w, L"Where and how much",
                 {order,
                  slider_row(kNrFocusScale, L"Focus working size",
                             L"What the model works at, a share of the focus view. Lower costs less; the picture "
                             L"keeps its own detail.",
                             n.focusScale, L"", off),
                  slider_row(kNrLimit, L"Working size limit",
                             L"The most one view’s model works on, in megapixels, whatever the sizes above. Each megapixel "
                             L"costs about 2 ms of GPU time and 130 MB of video memory, per view.",
                             n.workingLimit, L"", off, L" MP", true),
                  slider_row(kNrEdgeBlend, L"Edge blend",
                             L"The band over which the processed area fades into what isn’t processed around it.", n.edgeBlend,
                             L"", off),
                  periphery,
                  slider_row(kNrPeripheryScale, L"Periphery working size", L"As the focus working size, for the periphery.",
                             n.peripheryScale, L"", noPeriphery),
                  slider_row(kNrPeripheryCoverage, L"Periphery covered",
                             L"How much of each eye’s view the model covers, around the focus area.",
                             n.peripheryCoverage, L"", noPeriphery)},
                 L"Working sizes, coverage, order and the periphery rebuild DLSS-NR once you stop changing them: a short "
                 L"stutter.",
                 draw, Applies::Live);
    return h;
}

// Sim-QVFR's own DLSS ([dlss]): NVIDIA's DLSS through NGX on the game's views, the game's code unchanged.
float own_dlss_pane(float x, float y, float w, bool draw) {
    float h = 0;
    std::wstring why;
    const bool available = own_dlss_available(&why);
    const DlssSettings& d = app.s.dlss;
    Row use;
    const std::wstring useDesc =
        available ? L"NVIDIA’s DLSS on " + game_name() + L"’s views, run by Sim-QVFR through NVIDIA’s NGX (" + short_name() +
                        L" has none of its own) and changing none of its code: DLAA on the periphery, DLAA or an upscale "
                        L"on the focus views. With it on, the game’s own MSAA can go off."
                  : why;
    use.fullHeight = [useDesc](float width) { return checkbox_height(useDesc, width); };
    use.full = [useDesc, available, on = d.on](const Rect& r) { checkbox(kDlssOn, r, on, L"Use DLSS", useDesc, !available); };
    // The DLL: the one Sim-QVFR includes (NVIDIA's, beside the layer), or another the player chose (Use included goes back).
    const std::wstring dll = dlss_dll_path(d.folder, layer_folder());
    const std::wstring version = file_version(dll);
    const bool found = !dll.empty() && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool chosen = !d.folder.empty();
    const std::wstring named = version.empty() ? std::wstring(L"Found") : L"Version " + version;
    Row file;
    file.label = L"nvngx_dlss.dll";
    if (!chosen)
        file.desc = found ? named + L", included with Sim-QVFR. Choose another (a newer one from a game, say) to use it "
                                    L"instead."
                          : std::wstring(L"Missing from Sim-QVFR’s folder: install Sim-QVFR again, or choose one from a "
                                         L"game (DLSS 310 or newer).");
    else
        file.desc = found ? named + L", in " + d.folder + L", your choice."
                          : L"Not found in " + d.folder + L": DLSS takes the one Sim-QVFR includes instead.";
    const float chooseW = button_width(L"Choose…"), includedW = chosen ? button_width(L"Use included") : 0.0f;
    file.controlW = chosen ? includedW + t::kSpace3 + chooseW : chooseW;
    file.controlH = t::kControlHeight;
    file.control = [chosen, chooseW, includedW](const Rect& r) {
        if (chosen)
            button(kDlssIncluded, {r.x, r.y, includedW, r.h}, L"Use included", false);
        button(kDlssFolder, {r.right() - chooseW, r.y, chooseW, r.h}, L"Choose…", false);
    };
    h += section(x, y, w, L"DLSS", {use, file},
                 L"On and off apply while the game runs; another nvngx_dlss.dll at the next launch. Needs an NVIDIA RTX "
                 L"graphics card, Quad View Foveated Rendering and multi-view rendering.",
                 draw, Applies::Live) +
         t::kSpace6;
    const bool off = !available || !d.on;
    std::wstring modes[std::size(kDlssFocusModes)], focusPresets[std::size(kDlssFocusPresets)],
        eyePresets[std::size(kDlssEyePresets)], sharpenings[std::size(kDlssTextureSharpening)];
    for (size_t i = 0; i < std::size(kDlssTextureSharpening); ++i)
        sharpenings[i] = kDlssTextureSharpening[i].name;
    for (size_t i = 0; i < std::size(kDlssFocusModes); ++i)
        modes[i] = kDlssFocusModes[i].name;
    for (size_t i = 0; i < std::size(kDlssFocusPresets); ++i)
        focusPresets[i] = kDlssFocusPresets[i].name;
    for (size_t i = 0; i < std::size(kDlssEyePresets); ++i)
        eyePresets[i] = kDlssEyePresets[i].name;
    Row mode = choice_row(kDlssFocusMode, L"Focus views", modes, static_cast<int>(std::size(modes)),
                          dlss_choice_index(kDlssFocusModes, d.focusScale, true), off);
    mode.desc = L"DLAA draws them at their size. Ultra quality, Quality, Balanced and Performance draw them at 77, 67, 58 "
                L"or 50% each way and upscale them: less GPU time, softer distant textures. DLSS has no ultra quality "
                L"model of its own: 77% runs its quality one.";
    Row focusPreset = choice_row(kDlssFocusPreset, L"Focus preset", focusPresets, static_cast<int>(std::size(focusPresets)),
                                 dlss_choice_index(kDlssFocusPresets, d.focusPreset), off);
    focusPreset.desc = L"Default is NVIDIA’s choice for the mode: K, and M at Performance, which costs more.";
    Row eyePreset = choice_row(kDlssEyePreset, L"Periphery preset", eyePresets, static_cast<int>(std::size(eyePresets)),
                               dlss_choice_index(kDlssEyePresets, d.eyePreset), off);
    eyePreset.desc = L"K is the steadier. F costs about half as much with more aliasing: with eye tracking it stays out of "
                     L"where you look; K suits a fixed focus.";
    h += section(x, y + h, w, L"DLSS on the views", {mode, focusPreset, eyePreset},
                 L"Each change rebuilds DLSS in the running game: a short stutter.", draw, Applies::Live) +
         t::kSpace6;
    Row sharpening = choice_row(kDlssSharpening, L"Texture sharpening", sharpenings,
                                static_cast<int>(std::size(sharpenings)),
                                dlss_choice_index(kDlssTextureSharpening, d.textureSharpening, true), off);
    sharpening.desc = L"Samples textures sharper where DLSS works (the mip bias DLSS asks for). Focus only acts on the focus "
                      L"area, Focus and periphery on the periphery too, where it shows most. Subtle; if fine patterns "
                      L"shimmer or show moiré, go back a step.";
    h += section(x, y + h, w, L"Textures", {sharpening},
                 L"On at launch, its changes apply at once, without a stutter. Turned on from Off while the game runs, it "
                 L"starts at the next launch (off, it costs nothing).",
                 draw, Applies::Live) +
         t::kSpace6;
    Row cars;
    const std::wstring carsDesc =
        L"Gives DLSS each other car’s own motion, so cars passing, alongside or ahead stay clean instead of smearing. "
        L"Turn it off for more headroom: in a field of cars it costs about 0.5 ms of CPU and 0.2 to 0.5 ms of GPU a "
        L"frame.";
    cars.fullHeight = [carsDesc](float width) { return checkbox_height(carsDesc, width); };
    cars.full = [carsDesc, off, on = d.carVectors](const Rect& r) {
        checkbox(kDlssCarVectors, r, on, L"Other cars’ motion", carsDesc, off);
    };
    h += section(x, y + h, w, L"Advanced", {cars},
                 L"Applies at the next launch, with DLSS on at launch; with DLSS off it costs nothing.", draw,
                 Applies::NextLaunch) +
         t::kSpace6;
    h += own_nr_pane(x, y + h, w, draw);
    return h;
}

}  // namespace

float dlss_pane(float x, float y, float w, bool draw) {
    float h = 0;
    if (game_has(&games::GameFeatures::sideLoadedDlss))
        return own_dlss_pane(x, y, w, draw);
    if (!game_has(&games::GameFeatures::foveatedDlss)) {
        Row note;
        note.label = L"Cheeky Foveated DLSS";
        note.desc = not_yet() + L" The launch leaves it off for this game.";
        note.disabled = true;
        h += section(x, y, w, L"Foveated DLSS", {note}, L"", draw);
        return h;
    }
    // Cheeky's author asked for this credit, with the version, wherever the UI controls Cheeky.
    const std::wstring credit = cheeky_version_label(app.runtime.cheekyVersion) +
                                (app.runtime.cheekyVersion.empty() ? L"  ·  its version shows after a launch with it" : L"");
    if (draw)
        app.canvas.text(credit, t::kSubheadline, {x + t::kSpace3, y, w - t::kSpace3, 16}, col(app.c->label_secondary));
    h += 16 + t::kSpace4;
    const std::wstring runtime = cheeky_runtime_path(app.ini, layer_folder());
    const bool found = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
    const CheekyLayer layer = cheeky_layer_state();
    const auto badge_row = [](const std::wstring& label, const std::wstring& desc, int tone, const std::wstring& text) {
        Row row;
        row.label = label;
        row.desc = desc;
        row.controlW = badge_width(text);
        row.controlH = 20;
        row.control = [tone, text](const Rect& r) { badge(r.x, r.y, tone, text); };
        return row;
    };
    Row use;
    const std::wstring useDesc = L"Loads Cheeky’s DLSS runtime into " + game_name() + L", so the outer views can use a "
                                 L"cheaper DLSS pass.";
    use.fullHeight = [useDesc](float width) { return checkbox_height(useDesc, width); };
    use.full = [useDesc](const Rect& r) { checkbox(kCheekyUse, r, app.s.cheeky.load, L"Use Cheeky Foveated DLSS", useDesc); };
    Row rt;
    rt.label = L"Runtime";
    rt.desc = (found ? L"" : L"Not found: ") + runtime;
    rt.controlW = button_width(L"Choose…");
    rt.controlH = t::kControlHeight;
    rt.control = [](const Rect& r) { button(kCheekyRuntime, r, L"Choose…", false); };
    const Row xr = layer == CheekyLayer::Enabled
                       ? badge_row(L"OpenXR layer", L"Cheeky’s layer tells its runtime the frame timing it needs.", 0, L"Installed")
                   : layer == CheekyLayer::Disabled
                       ? badge_row(L"OpenXR layer", L"Registered but turned off: run CheekyOpenXRSetup.exe again.", 1, L"Turned off")
                       : badge_row(L"OpenXR layer", L"Install it with CheekyOpenXRSetup.exe from Cheeky’s release.", 2, L"Not installed");
    const std::wstring& status = app.runtime.cheekyStatus;
    const std::wstring last = status == L"running"   ? L"Cheeky ran at the last launch."
                              : status == L"missing" ? L"At the last launch, Cheeky’s runtime was not found."
                              : status == L"refused" ? L"At the last launch, Cheeky did not start: see its log next to its runtime."
                                                     : L"";
    h += section(x, y + h, w, L"Cheeky Foveated DLSS", {use, rt, xr},
                 L"Put the CheekyFoveatedDLSS folder from Cheeky’s Standalone ZIP next to sim-qvfr.dll, or choose "
                 L"its runtime. " + last,
                 draw, Applies::NextLaunch) + t::kSpace6;
    const bool outer = app.s.cheeky.outerDlaa;
    Row on, cheap;
    const std::wstring onDesc = L"Off leaves ACE’s own DLSS on every view.";
    on.fullHeight = [onDesc](float width) { return checkbox_height(onDesc, width); };
    on.full = [onDesc](const Rect& r) { checkbox(kCheekyOn, r, app.s.cheeky.processing, L"Foveated DLSS", onDesc); };
    const std::wstring cheapDesc = L"The outer views get Cheeky’s own DLAA pass at a lower resolution instead of ACE’s DLSS.";
    cheap.fullHeight = [cheapDesc](float width) { return checkbox_height(cheapDesc, width); };
    cheap.full = [cheapDesc](const Rect& r) { checkbox(kCheekyOuter, r, app.s.cheeky.outerDlaa, L"Cheaper outer views", cheapDesc); };
    std::wstring presetNames[std::size(kCheekyPresets)];
    for (size_t i = 0; i < std::size(kCheekyPresets); ++i)
        presetNames[i] = kCheekyPresets[i].name;
    const float presetW = popup_width(presetNames, static_cast<int>(std::size(presetNames)), 140);
    const auto preset_row = [presetW](Ctl id, const std::wstring& label, const std::wstring& desc, int value, bool disabled) {
        Row row;
        row.label = label;
        row.desc = desc;
        row.disabled = disabled;
        row.controlW = presetW;
        row.controlH = t::kControlHeight;
        row.control = [id, value, disabled](const Rect& r) { popup_button(id, r, cheeky_preset_name(value), disabled); };
        return row;
    };
    h += section(x, y + h, w, L"Processing",
                 {on, cheap,
                  slider_row(kCheekyScale, L"Outer DLAA scale", L"That pass’s resolution, a share of ACE’s render size.",
                             app.s.cheeky.outerScale, L"", !outer),
                  preset_row(kCheekyOuterPreset, L"Outer preset", L"", app.s.cheeky.outerPreset, !outer),
                  preset_row(kCheekyFocusPreset, L"Focus preset", L"DLSS preset for the sharp focus views.",
                             app.s.cheeky.focusPreset, false)},
                 L"In Quad View Foveated Rendering, Sim-QVFR tells Cheeky which "
                 L"views are outer and which are focus.",
                 draw, Applies::Live);
    h += t::kSpace6;
    Row fix, nr;
    const std::wstring fixDesc = L"Cheeky’s workaround for smearing with some presets. Adds GPU work.";
    fix.fullHeight = [fixDesc](float width) { return checkbox_height(fixDesc, width); };
    fix.full = [fixDesc](const Rect& r) { checkbox(kCheekyMotionFix, r, app.s.cheeky.motionFix, L"Fix motion-vector blur", fixDesc); };
    const std::wstring nrDesc = L"NVIDIA’s DLSS-NR (DLSS 5) on the focus views. Needs nvngx_dlssnr.dll next to the game.";
    nr.fullHeight = [nrDesc](float width) { return checkbox_height(nrDesc, width); };
    nr.full = [nrDesc](const Rect& r) { checkbox(kCheekyNr, r, app.s.cheeky.nr, L"DLSS-NR", nrDesc); };
    const std::wstring nrPresets[] = {kCheekyNrPresets[0], kCheekyNrPresets[1], kCheekyNrPresets[2]};
    const std::wstring nrPasses[] = {L"1 pass", L"2 passes", L"3 passes"};
    std::vector<Row> experimental = {
        slider_row(kCheekySupersampling, L"Focus supersampling",
                   L"Runs the focus views’ DLSS above their size for extra sharpness, at a GPU cost.",
                   app.s.cheeky.supersampling, L""),
        fix, nr,
        choice_row(kCheekyNrPreset, L"DLSS-NR preset", nrPresets, 3, std::clamp(app.s.cheeky.nrPreset, 0, 2), !app.s.cheeky.nr),
        choice_row(kCheekyNrPasses, L"DLSS-NR passes", nrPasses, 3, std::clamp(app.s.cheeky.nrPasses, 1, 3) - 1,
                   !app.s.cheeky.nr)};
    h += section(x, y + h, w, L"Experimental: focus views", experimental,
                 L"Cheeky applies these to the focus views only. Each extra DLSS-NR pass costs about "
                 L"one more NR evaluation.",
                 draw, Applies::Live);
    h += t::kSpace6;
    Row fgOn;
    const bool fgHere = game_has(&games::GameFeatures::frameGeneration);
    const std::wstring fgDesc = !fgHere ? not_yet()
                                : L"Cheeky’s DLSS-G adds generated frames between ACE’s, guided by its depth and motion. "
                                  L"Turn OpenXR Toolkit’s Turbo mode and other frame generation (VectorXR’s) off.";
    fgOn.fullHeight = [fgDesc](float width) { return checkbox_height(fgDesc, width); };
    fgOn.full = [fgDesc, fgHere](const Rect& r) {
        checkbox(kFgOn, r, app.s.cheeky.fg && fgHere, L"Frame generation", fgDesc, !fgHere);
    };
    const bool fgOff = !app.s.cheeky.fg || !fgHere;
    Row fgBorder;
    const std::wstring borderDesc = L"Outlines the region in the headset, to check that it follows your eyes.";
    fgBorder.fullHeight = [borderDesc](float width) { return checkbox_height(borderDesc, width); };
    fgBorder.full = [borderDesc](const Rect& r) {
        checkbox(kFgBorder, r, app.s.cheeky.fgBorder, L"Show the center region", borderDesc);
    };
    const std::wstring multipliers[] = {kFgMultipliers[0], kFgMultipliers[1], kFgMultipliers[2]};
    const std::wstring quarters[] = {kFgQuarters[0], kFgQuarters[1], kFgQuarters[2], kFgQuarters[3]};
    h += section(x, y + h, w, L"Frame generation (experimental)",
                 {fgOn,
                  choice_row(kFgMultiplier, L"Multiplier", multipliers, 3, std::clamp(app.s.cheeky.fgMultiplier - 2, 0, 2), fgOff),
                  slider_row(kFgSize, L"Center region", L"Where frames are generated at full quality, a share of each eye.",
                             app.s.cheeky.fgSize, L"", fgOff),
                  choice_row(kFgScale, L"Center FG scale", quarters, 4, std::clamp(app.s.cheeky.fgScale / 25 - 1, 0, 3), fgOff),
                  choice_row(kFgPeriphery, L"Periphery FG scale", quarters, 4,
                             std::clamp(app.s.cheeky.fgPeriphery / 25 - 1, 0, 3), fgOff),
                  [&] {
                      const std::wstring names[3] = {kCheekyFgReleaseNames[0], kCheekyFgReleaseNames[1],
                                                     kCheekyFgReleaseNames[2]};
                      Row release = choice_row(kFgRelease, L"Release game", names, 3,
                                               cheeky_fg_release_index(app.s.cheeky.fgRelease), fgOff);
                      release.desc = L"When the game starts its next frame. Never: CPU and GPU overlap (most frames). "
                                     L"Capture: after the GPU finishes. Generated: after the generated frame.";
                      return release;
                  }(),
                  fgBorder},
                 L"Lower scales use less GPU time and memory.",
                 draw, Applies::Live);
    const std::wstring license = L"Cheeky Foveated DLSS is made by ClarkCheekyKent and is free software under the "
                                 L"GNU GPL v3 (github.com/ClarkCheekyKent/CheekyFoveatedDLSS).";
    const float lh = app.canvas.measure_height(license, t::kCallout, w);
    if (draw)
        app.canvas.text(license, t::kCallout, {x, y + h + t::kSpace6, w, lh + 2}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    h += t::kSpace6 + lh;
    return h;
}

}  // namespace simqvfr::launcher::ui::win

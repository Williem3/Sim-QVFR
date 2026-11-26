// The Picture tab (docs/ui/OVERHAUL_PLAN.md §2): Resolution, Foveation, Preview, View & helmet, Rendering, and the
// DLSS cards, each only where the game has what it holds. Dependent settings hide (values kept) while what they depend
// on is off or doesn't apply (§3), and each card's ⋯ menu resets it.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../preflight.h"
#include "../settings.h"
#include "../../dev_build.h"
#include "../../engine/dlss_settings.h"
#include "../../engine/nr_settings.h"
#include "../../engine/nvapi_gpus.h"
#include "../../games/game.h"
#include "card.h"
#include "controls.h"
#include "form.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

// No NVIDIA RTX card here (AMD, Intel, an older NVIDIA): the game's upscaler is AMD FSR (engine/dlss_views.h: the layer
// runs FSR where DLSS can't), with the same [dlss] settings, its card in DLSS's place.
// Development builds only: SIM_QVFR_LAUNCHER_GPU=fsr shows the FSR card on an RTX PC (to check it).
bool fsr_pc() {
    if constexpr (kDevBuild) {
        wchar_t forced[8] = {};
        if (GetEnvironmentVariableW(L"SIM_QVFR_LAUNCHER_GPU", forced, 8) && _wcsicmp(forced, L"fsr") == 0)
            return true;
    }
    return nvapi::two_views_gpu() == nullptr;
}

const wchar_t* upscaler_name() { return fsr_pc() ? L"FSR" : L"DLSS"; }

bool own_upscaler_available(std::wstring* why) {
    std::wstring reason;
    if (!game_has(&games::GameFeatures::sideLoadedDlss))
        reason = not_yet();
    if (why)
        *why = reason;
    return reason.empty();
}

bool own_dlss_available(std::wstring* why) {
    std::wstring reason;
    if (!own_upscaler_available(&reason))
        ;  // the game's or the mode's reason
    else if (fsr_pc())
        reason = tr(L"Needs an NVIDIA RTX graphics card (20 series or newer): none was found.");
    if (why)
        *why = reason;
    return reason.empty();
}

bool multi_view_game() {
    return game_has(&games::GameFeatures::multiView) || game_has(&games::GameFeatures::layerMultiView);
}

std::wstring multi_view_way(bool full) {
    // What the layer picks at the game's device (games/multi_view.h; scssoftware/corner.cpp): NVIDIA's where the card
    // has it, else instancing.
    if (const nvapi::Gpu* gpu = nvapi::two_views_gpu()) {
        if (!full)
            return tr(L"NVIDIA’s multi-view rendering");
        return trf(L"NVIDIA’s multi-view rendering, on your {gpu}",
                   {{L"gpu", std::wstring(gpu->name.begin(), gpu->name.end())}});
    }
    return full ? tr(L"Sim-QVFR’s own multi-view rendering (instancing), since no NVIDIA RTX card (20 series or "
                     L"newer) was found")
                : tr(L"Sim-QVFR’s multi-view rendering");
}

bool own_nr_available(std::wstring* why) {
    std::wstring reason;
    if (!own_dlss_available(&reason))
        ;  // DLSS's own reason
    else if (!app.s.dlss.on)
        reason = tr(L"Needs DLSS on: it works from DLSS’s depth and motion.");
    else if (app.s.nr.folder.empty())
        reason = tr(L"Choose your nvngx_dlssnr.dll first.");
    else if (GetFileAttributesW((app.s.nr.folder + L"\\nvngx_dlssnr.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        reason = tr(L"nvngx_dlssnr.dll isn’t in the folder chosen for it.");
    if (why)
        *why = reason;
    return reason.empty();
}

namespace {

// What the running game started with in QVFR: live resolution goes down from it and back up to it, not above (its
// periphery and focus view sizes, percent of the native eye). False without a QVFR game running with Sim-QVFR.
bool running_launch(LaunchSizes& launch) {
    if (running_live_ini(runtime_info_path()).empty())
        return false;
    launch = load_launch_sizes(runtime_info_path());
    return launch.known && launch.outer > 0 && launch.focusWidth > 0 &&
           launch.focusHeight > 0;
}

// The headset menu's Auto-fit to headset rate (quad/qvfr_optimizer.h): QVFR on every path but the D3D11 layer's
// (layer/menu.cpp: the truck simulators have no optimizer).
bool auto_fit_game() {
    return game_has(&games::GameFeatures::liveResolution) && !game_has(&games::GameFeatures::layerMultiView);
}

template <size_t N>
std::vector<std::wstring> names_of(const DlssChoice (&list)[N]) {
    std::vector<std::wstring> out;
    for (const DlssChoice& c : list)
        out.push_back(tr(c.name));  // the tables' names are marked with I18N
    return out;
}

std::vector<std::wstring> items_of(Ctl id) {
    std::vector<std::wstring> items;
    choice_items(id, items);
    return items;
}

// ---- Resolution --------------------------------------------------------------------------------

Card resolution_card() {
    // The truck simulators' QVFR (the D3D11 layer's, GameFeatures::layerMultiView): every size live, inside the game's
    // own render size (scssoftware/corner.h corner_set_live), no launch ceiling.
    const bool layerSizes = game_has(&games::GameFeatures::layerMultiView);
    Card c{Tab::Picture, tr(L"Resolution"), layerSizes ? Applies::Live : Applies::LiveLowered};
    c.body = [](Body& b) {
        const Readouts ro = readouts(app.s, app.runtime);
        const bool known = app.runtime.known();
        b.slider({kOuter, tr(L"Periphery resolution"), app.s.outer, L"%", known ? px(ro.outer) : L""});
        b.slider({kInner, tr(L"Focus density"), app.s.inner, L"%", known ? px(ro.focus) : L""});
        if (game_has(&games::GameFeatures::sharpening))
            b.slider({kSharpen, tr(L"Sharpening"), app.s.sharpen, L"%", app.s.sharpen == 0 ? tr(L"Off") : L""});
        if (game_has(&games::GameFeatures::peripheryTextureMatch))
            b.slider({kTextureMatch, tr(L"Periphery texture match"), app.s.textureMatch, L"%",
                      app.s.textureMatch == 0 ? tr(L"Off") : L""});
        if (game_has(&games::GameFeatures::objectDetail))
            b.slider({kObjectDetail, tr(L"Object detail"), app.s.objectDetail});
        b.note(known ? trf(L"Percent of the headset’s {width} × {height} per eye; the periphery and the focus are set "
                           L"apart.",
                           {{L"width", std::to_wstring(app.runtime.width)},
                            {L"height", std::to_wstring(app.runtime.height)}})
                     : std::wstring(tr(L"Percent of the headset’s resolution; pixel sizes appear after the first "
                                       L"launch.")));
        if (game_has(&games::GameFeatures::layerMultiView)) {
            b.note(tr(L"Changes apply while you drive, inside the game’s own render size: the focus and the periphery "
                      L"share it, so a large focus at a high Focus density leaves less room for the periphery."));
            return;
        }
        if (!game_has(&games::GameFeatures::liveResolution))
            return;
        // The live ceiling (its badge's "Now when lowered"): said before the launch, with numbers while it runs.
        LaunchSizes launch;
        if (running_launch(launch)) {
            const int density = std::min(launch.focusWidth * 100 / std::max(1, app.s.width),
                                          launch.focusHeight * 100 / std::max(1, app.s.height));
            b.note(trf(L"This launch allows up to Periphery resolution {periphery} and Focus density {density} at this "
                       L"focus size. Higher applies at the next launch.",
                       {{L"periphery", std::to_wstring(launch.outer) + L"%"},
                        {L"density", std::to_wstring(density) + L"%"}}));
        } else {
            b.note(tr(L"Live changes go down from what you launch with, and back up to it: launch with your highest "
                      L"settings, then lower them in the headset."));
        }
        if (auto_fit_game())
            b.note(tr(L"In the headset, Auto-fit to headset rate sets these and the focus size to fill your GPU’s "
                      L"frame budget while you drive (Right starts it, Left undoes it)."));
    };
    c.reset = [](Settings& s) {
        const Settings d;
        s.outer = d.outer;
        s.inner = d.inner;
        s.sharpen = d.sharpen;
        s.textureMatch = d.textureMatch;
        s.objectDetail = d.objectDetail;
    };
    return c;
}

// ---- Color ---------------------------------------------------------------------------------------

// Colour correction (quad/color_grade.h): the headset image's exposure, brightness, contrast and saturation, the
// periphery and the focus views alike; the headset menu's Color page holds the same.
Card color_card() {
    Card c{Tab::Picture, tr(L"Color"), Applies::Live};
    c.body = [](Body& b) {
        const ColorGrade& g = app.s.grade;
        b.toggle(kGradeOn, tr(L"Color correction"), g.on,
                 g.on ? tr(L"The headset image’s exposure, brightness, contrast and saturation.") : tr(L"Off"));
        b.reveal(kGradeOn, g.on, tr(L"Color correction"),
                 {tr(L"Exposure"), tr(L"Brightness"), tr(L"Contrast"), tr(L"Saturation")}, [](Body& r) {
            const ColorGrade& cg = app.s.grade;
            r.slider({kGradeExposure, tr(L"Exposure"), cg.exposure, L" EV", L"", true});
            r.sliders({kGradeBrightness, tr(L"Brightness"), cg.brightness},
                      {kGradeContrast, tr(L"Contrast"), cg.contrast});
            r.slider({kGradeSaturation, tr(L"Saturation"), cg.saturation, L"%",
                      cg.saturation == -100 ? tr(L"Grey") : L""});
            r.note(tr(L"0 leaves a value as the game draws it. The periphery and the focus area change alike; menus "
                      L"and loading screens show as the game draws them."));
        });
    };
    c.reset = [](Settings& s) { s.grade = Settings{}.grade; };
    return c;
}

// ---- Game Lighting Enhancement -----------------------------------------------------------------

// Game Lighting Enhancement (quad/car_lighting.h; "Night cars" before 2026-11-23, its ini keys still car_light*): LMU's
// cars relit at night; the headset menu's Color page, Game Lighting Enhancement, holds
// the same. The switch takes effect at the next launch (it hooks the game's shaders as they're made); the rest changes
// while you drive.
Card night_cars_card() {
    Card c{Tab::Picture, tr(L"Game Lighting Enhancement (experimental)"), Applies::Live};
    c.body = [](Body& b) {
        const CarLighting& l = app.s.carLight;
        b.toggle(kNightOn, tr(L"Game Lighting Enhancement"), l.on,
                 l.on ? tr(L"Relights the cars at night: highlights from the floodlights and headlights, shading in the "
                           L"creases.")
                      : tr(L"Off"));
        b.reveal(kNightOn, l.on, tr(L"Game Lighting Enhancement"),
                 {tr(L"Quality"), tr(L"Fill light"), tr(L"Ambient occlusion"), tr(L"Highlights"), tr(L"Paint gloss"),
                  tr(L"Smoothing"), tr(L"Floodlight shadows"), tr(L"Paint detail"), tr(L"Lacquer reflections"),
                  tr(L"Headlight highlights"), tr(L"Shadows on the ground"), tr(L"Night vision color")},
                 [](Body& r) {
            const CarLighting& cl = app.s.carLight;
            std::vector<std::wstring> items;
            const int selected = choice_items(kNightQuality, items);
            r.segments(kNightQuality, tr(L"Quality"), items, selected);
            r.sliders({kNightFill, tr(L"Fill light"), cl.fill, L"%"}, {kNightOcclusion, tr(L"Ambient occlusion"), cl.occlusion, L"%"});
            r.sliders({kNightHighlights, tr(L"Highlights"), cl.highlights, L"%"}, {kNightGloss, tr(L"Paint gloss"), cl.gloss, L"%"});
            r.slider({kNightSmooth, tr(L"Smoothing"), cl.smooth, L" cm", cl.smooth == 0 ? tr(L"Off") : L""});
            r.toggle(kNightShadows, tr(L"Floodlight shadows"), cl.shadows,
                     tr(L"Parts of a car hidden from a floodlight lose its light and highlight."));
            r.toggle(kNightDetail, tr(L"Paint detail"), cl.detail,
                     tr(L"The paint’s orange peel and the carbon’s weave in the highlights."));
            r.toggle(kNightReflections, tr(L"Lacquer reflections"), cl.reflections,
                     tr(L"What’s around the car mirrored in its paint, where both eyes can see it."));
            r.toggle(kNightHeadlights, tr(L"Headlight highlights"), cl.headlights,
                     tr(L"Other cars’ headlights shine on the paint as the floodlights do."));
            r.toggle(kNightGround, tr(L"Shadows on the ground"), cl.ground,
                     tr(L"The cars’ shadows on the track and walls around them."));
            r.toggle(kNightVision, tr(L"Night vision color"), cl.nightVision,
                     tr(L"The dark loses its colour and turns a little blue, as eyes see at night."));
            r.note(tr(L"Quality sets how finely each effect is worked out, not which are on: Low takes a quarter to a "
                      L"third less GPU time than High. Game Lighting Enhancement itself starts at the next launch; "
                      L"everything else changes while you drive."));
        });
    };
    c.reset = [](Settings& s) { s.carLight = Settings{}.carLight; };
    return c;
}

// ---- Foveation ---------------------------------------------------------------------------------

void outline_color_row(Body& b) {
    b.custom(32, [](const Rect& r) {
        const std::wstring label = tr(L"Outline color");
        const float sw = segmented_width();
        // Cut with an ellipsis before the help icon and the segments (help, hits and labels keep the full text).
        const float labelW = std::max(0.0f, std::min(r.w / 2, r.w - sw - 30));
        const std::wstring shown = fit(label, t::kBody, labelW);
        app.canvas.text(shown, t::kBody, {r.x, r.y + 6, r.w / 2, 20}, col(app.c->label));
        help_icon(label, r.x + std::ceil(app.canvas.measure(shown, t::kBody)) + 6, r.y + 6);
        segmented(kColor, {r.right() - sw, r.y, sw, 32}, static_cast<int>(app.s.color), false);
        app.drawnLabels.push_back({label, app.drawingSection, r});
    });
}

Card foveation_card() {
    Card c{Tab::Picture, tr(L"Foveation"), Applies::Live};
    if (eye_warning())
        c.chip = tr(L"Eye tracker not detected");
    c.body = [](Body& b) {
        const Settings& s = app.s;
        b.segments(kMode, tr(L"Foveated mode"), items_of(kMode), s.eyeTracking ? 0 : 1);
        if (eye_warning())
            b.note(tr(L"The focus region stays centred until a tracker connects."));
        b.sliders({kWidth, tr(L"Focus width"), s.width}, {kHeight, tr(L"Focus height"), s.height});
        // A focus larger than the running game's launch renders at a lower density until the next launch.
        if (LaunchSizes launch; game_has(&games::GameFeatures::liveResolution) && running_launch(launch) &&
                                (s.inner * s.width > launch.focusWidth * 100 + 50 ||
                                 s.inner * s.height > launch.focusHeight * 100 + 50))
            b.note(tr(L"Larger than this launch allows at your Focus density: it renders less dense until the next "
                      L"launch."));
        b.segments(kShape, tr(L"Shape"), items_of(kShape), s.shape.custom ? 1 : 0);
        if (!s.shape.custom) {
            b.slider({kCorner, tr(L"Corner radius"), s.corner});
            b.hidden(tr(L"Shape"), tr(L"Custom shape"), {tr(L"Adjust point"), tr(L"Up/Down"), tr(L"Left/Right")});
        } else {  // the outline's points: drawn on the preview, the picked one's place here (as the headset menu)
            b.note(trf(L"Click inside the focus area in the preview to add a point (up to {max}), drag a point to move "
                       L"it, right-click it to remove it, Shift+drag to move the whole shape. Trace your windscreen, then "
                       L"move it with Whole shape below, or the focus area with the position sliders.",
                       {{L"max", std::to_wstring(kFocusPointsMax)}}));
            std::vector<std::wstring> names;
            const int picked = choice_items(kShapePointPick, names);
            b.popup(kShapePointPick, tr(L"Adjust point"), names[static_cast<size_t>(picked)], names);
            b.sliders({kShapePointY, tr(L"Up/Down"), *slider_value(kShapePointY), L""},
                      {kShapePointX, tr(L"Left/Right"), *slider_value(kShapePointX), L""});
            b.actions({{kShapeRemove, tr(L"Remove point")},
                       {kShapeReset, tr(L"Start again")}});
            b.hidden(tr(L"Shape"), tr(L"Rectangle"), {tr(L"Corner radius")});
        }
        b.slider({kFeather, tr(L"Feather"), s.feather});
        b.note(tr(L"See the edge of the sharp area? Raise Feather, or make the focus larger."));
        // The headset's runtime composed the views at the last launch (Pimax, Varjo; native quad views): it places the
        // focus area in its own focus region, so the position sliders would do nothing. The custom shape still shows.
        const bool runtimePlaces = game_has(&games::GameFeatures::nativeQuadViews) && app.runtime.quadViewsUsed;
        if (runtimePlaces)
            b.note(tr(L"Your headset’s software composes the views (Quad-Views Foveated Rendering in Pimax Play or Varjo "
                      L"Base) and places the focus area itself, so the position sliders aren’t offered. The shape and "
                      L"Feather still apply."));
        if (s.eyeTracking) {
            if (!runtimePlaces)
                b.sliders({kFocusOffset, tr(L"Gaze offset"), s.offset.gaze, L"°"},
                          {kFocusOffsetX, tr(L"Horizontal gaze offset"), s.offset.gazeX, L"°"});
            b.slider({kGazeDeadzone, tr(L"Gaze dead-zone"), s.gazeDeadzone, L"°", L"", true});
            b.hidden(tr(L"Foveated mode"), tr(L"Fixed"), {tr(L"Vertical position"), tr(L"Horizontal position")});
        } else {
            if (!runtimePlaces)
                b.sliders({kFocusOffset, tr(L"Vertical position"), s.offset.fixed, L"°"},
                          {kFocusOffsetX, tr(L"Horizontal position"), s.offset.fixedX, L"°"});
            b.hidden(tr(L"Foveated mode"), tr(L"Eye tracked"),
                     {tr(L"Gaze dead-zone"), tr(L"Gaze offset"), tr(L"Horizontal gaze offset")});
        }
        if (game_has(&games::GameFeatures::focusCorridor)) {  // road focus (LMU), fixed foveation only
            b.divider();
            if (s.eyeTracking) {
                b.note(tr(L"Road focus is for fixed foveation: it’s off while the focus follows your eyes."));
                b.hidden(tr(L"Foveated mode"), tr(L"Fixed"), {tr(L"Road focus"), tr(L"Road width")});
            } else {
                b.toggle(kCorridorOn, tr(L"Road focus"), s.corridor,
                         s.corridor ? tr(L"Full detail along the road ahead and in your car; the scenery beside and above "
                                         L"the road from the periphery.")
                                    : tr(L"Off"));
                b.reveal(kCorridorOn, s.corridor, tr(L"Road focus"),
                         {tr(L"Cockpit preset"), tr(L"Road width"), tr(L"Road height"), tr(L"Road widening"),
                          tr(L"Road edge blend"),
                          tr(L"Show Road focus area")},
                         [](Body& r) {
                    std::vector<std::wstring> presets;
                    const int preset = choice_items(kCorridorPreset, presets);
                    r.segments(kCorridorPreset, tr(L"Cockpit preset"), presets, preset);
                    r.sliders({kCorridorWidth, tr(L"Road width"), app.s.corridorWidth, L" m"},
                              {kCorridorHeight, tr(L"Road height"), app.s.corridorHeight, L" m", L"", true});
                    r.sliders({kCorridorSpread, tr(L"Road widening"), app.s.corridorSpread, L"\u00b0"},
                              {kCorridorBlend, tr(L"Road edge blend"), app.s.corridorBlend, L" m",
                               app.s.corridorBlend == 0 ? tr(L"Off") : L"", true});
                    const bool show = app.s.corridorShow;
                    r.toggle(kCorridorShow, tr(L"Show Road focus area"), show,
                             show ? tr(L"What Road focus takes from the periphery tinted blue in the headset.")
                                  : tr(L"Off"));
                });
            }
        }
        b.divider();
        b.toggle(kOutline, tr(L"Show focus outline"), s.outline,
                 s.outline ? tr(L"Its edge in the headset, to see where it is.") : tr(L"Off"));
        b.reveal(kOutline, s.outline, tr(L"Show focus outline"), {tr(L"Outline color")}, outline_color_row);
    };
    c.reset = [](Settings& s) {
        const Settings d;
        s.eyeTracking = d.eyeTracking;
        s.width = d.width;
        s.height = d.height;
        s.shape = d.shape;
        s.corner = d.corner;
        s.feather = d.feather;
        s.offset = d.offset;
        s.gazeDeadzone = d.gazeDeadzone;
        s.outline = d.outline;
        s.color = d.color;
        s.corridor = d.corridor;
        s.corridorWidth = d.corridorWidth;
        s.corridorHeight = d.corridorHeight;
        s.corridorPreset = d.corridorPreset;
        s.corridorSpread = d.corridorSpread;
        s.corridorBlend = d.corridorBlend;
        s.corridorShow = d.corridorShow;
    };
    return c;
}

// ---- GPU regulator -----------------------------------------------------------------------------

// A game whose adapter makes QVFR and times its eye passes (GameFeatures::gpuRegulator: rFactor 2). The set
// Periphery resolution and Focus density are the most it uses; it lowers them 1% at a time while the GPU time per frame runs
// over the first limit and raises them again while it runs under the second.
Card regulator_card() {
    Card c{Tab::Picture, tr(L"GPU regulator"), Applies::Live};
    c.body = [](Body& b) {
        const GpuRegulatorSettings& r = app.s.regulator;
        b.toggle(kRegFocus, tr(L"Regulate focus density"), r.focus,
                 r.focus ? tr(L"Lowered and raised 1% at a time, never above the set density.") : tr(L"Off"));
        b.toggle(kRegPeriphery, tr(L"Regulate periphery resolution"), r.periphery,
                 r.periphery ? tr(L"Lowered and raised 1% at a time, never above the set resolution.") : tr(L"Off"));
        b.toggle(kRegRequireGpuBusy, tr(L"Require high GPU load"), r.requireGpuBusy,
                 r.requireGpuBusy ? tr(L"Lower only while GPU load is at least 90%.") : tr(L"Use GPU frame time alone."));
        b.sliders({kRegDecrease, tr(L"Lower above"), r.decreaseAbove, L" ms", L"", true},
                  {kRegIncrease, tr(L"Raise below"), r.increaseBelow, L" ms", L"", true});
        b.sliders({kRegLowerFrames, tr(L"Frames before lowering"), r.lowerFrames, L" frames"},
                  {kRegRaiseFrames, tr(L"Frames before raising"), r.raiseFrames, L" frames"});
        b.note(r.increaseBelow > r.decreaseAbove
                   ? tr(L"Raise below is held at Lower above: nothing is raised while the frames are over the limit.")
                   : r.on() ? tr(L"GPU time of the eye passes per frame, as the log’s scene span. Between the two limits nothing changes.")
                            : tr(L"Switch on what to regulate: the sliders below act once one is on."));
    };
    c.reset = [](Settings& s) { s.regulator = GpuRegulatorSettings{}; };
    return c;
}

// ---- Preview -----------------------------------------------------------------------------------

Card preview_card() {
    Card c{Tab::Picture, tr(L"Preview")};
    c.body = [](Body& b) {
        const float w = b.width();
        // One eye, or both: the crop's outer side is each eye's temple side, so it mirrors.
        b.segments(kPreviewEye, tr(L"Eye"), {tr(L"Left"), tr(L"Both"), tr(L"Right")}, std::clamp(app.previewEye, 0, 2));
        b.custom(preview(0, 0, w, false), [](const Rect& r) { preview(r.x, r.y, r.w, true); });
        b.custom(16, [](const Rect& r) { preview_legend(r.x, r.y, r.w, true); });
        b.divider();
        b.custom(readouts_card(0, 0, w, false), [](const Rect& r) { readouts_card(r.x, r.y, r.w, true); });
    };
    return c;
}

// ---- View & helmet -----------------------------------------------------------------------------

void helmet_rows(Body& c) {
    const HelmetSettings& h = app.s.helmet;
    c.slider({kHelmetRim, tr(L"Rim"), h.rim});
    c.sliders({kHelmetCorners, tr(L"Corners"), h.corners}, {kHelmetLight, tr(L"Lighting"), h.light});
    c.more(tr(L"your own images"),
           {tr(L"Visor"), tr(L"Size"), tr(L"Height"), tr(L"Depth"), tr(L"Detail"), tr(L"Helmet images")}, [](Body& m) {
        const HelmetSettings& hs = app.s.helmet;
        m.buttons(tr(L"Helmet images"), tr(L"top.png, bottom.png and visor.png in the helmet folder."),
                  {{kHelmetFolder, tr(L"Show folder")}});
        m.segments(kVisor, tr(L"Visor"), items_of(kVisor), static_cast<int>(hs.visor));
        m.sliders({kHelmetSize, tr(L"Size"), hs.size, L"°"}, {kHelmetHeight, tr(L"Height"), hs.height, L"°"});
        m.slider({kHelmetDepth, tr(L"Depth"), hs.depth, L" cm"});
        m.segments(kHelmetDetail, tr(L"Detail"), items_of(kHelmetDetail), hs.detail ? 1 : 0);
    });
}

Card view_card() {
    Card c{Tab::Picture, tr(L"View & helmet"), Applies::Live};
    c.body = [](Body& b) {
        const bool crop = game_has(&games::GameFeatures::fovCrop), helmet = game_has(&games::GameFeatures::helmet);
        if (crop) {
            const Settings& s = app.s;
            b.toggle(kCropOn, tr(L"Crop the view"), s.crop,
                     s.crop ? tr(L"Skips the edges of each eye that a helmet covers.") : tr(L"Off"));
            b.reveal(kCropOn, s.crop, tr(L"Crop the view"),
                     {tr(L"Top"), tr(L"Bottom"), tr(L"Outer side (temple)"), tr(L"Inner side (nose)")}, [](Body& r) {
                const Settings& cs = app.s;
                r.sliders({kCropTop, tr(L"Top"), cs.cropTop}, {kCropBottom, tr(L"Bottom"), cs.cropBottom});
                r.sliders({kCropOuter, tr(L"Outer side (temple)"), cs.cropOuter},
                          {kCropInner, tr(L"Inner side (nose)"), cs.cropInner});
            });
        }
        if (crop && helmet)
            b.divider();
        if (helmet) {
            const bool on = app.s.helmet.on;
            b.toggle(kHelmetOn, tr(L"Show a helmet"), on,
                     on ? tr(L"A visor opening around the view, lit by the scene.") : tr(L"Off"));
            b.reveal(kHelmetOn, on, tr(L"Show a helmet"),
                     {tr(L"Rim"), tr(L"Corners"), tr(L"Lighting"), tr(L"Visor"), tr(L"Size"), tr(L"Height"),
                      tr(L"Depth"), tr(L"Detail"), tr(L"Helmet images")},
                     helmet_rows);
        }
    };
    c.reset = [](Settings& s) {
        const Settings d;
        s.crop = d.crop;
        s.cropTop = d.cropTop;
        s.cropBottom = d.cropBottom;
        s.cropOuter = d.cropOuter;
        s.cropInner = d.cropInner;
        HelmetSettings& h = s.helmet;  // the settings the card shows; the images' own keys stay as they are
        const HelmetSettings dh;
        h.on = dh.on;
        h.rim = dh.rim;
        h.corners = dh.corners;
        h.light = dh.light;
        h.visor = dh.visor;
        h.size = dh.size;
        h.height = dh.height;
        h.depth = dh.depth;
        h.detail = dh.detail;
    };
    return c;
}

// ---- Rendering ---------------------------------------------------------------------------------

// How a multi-view game (LMU, ATS, ETS2) draws its focus views; DLSS and Foveated DLSS are their own cards, each with
// its switch. The runtime composing the views (Pimax, Varjo) has no switch: it is automatic ([experiment]
// native_quad_views, the report bundle says what the last launch did).
bool rendering_shown() { return multi_view_game(); }

Card rendering_card() {
    Card c{Tab::Picture, tr(L"Rendering")};
    c.body = [](Body& b) {
        b.note(trf(L"Each eye’s pass also draws its focus view, by {way}.", {{L"way", multi_view_way(true)}}));
        if (game_has(&games::GameFeatures::layerMultiView))  // ATS, ETS2: the path starts with the first eye pass
            b.note(tr(L"The menus and the garage show as without Sim-QVFR: the focus view starts with the first frame "
                      L"that draws the world for each eye."));
    };
    return c;
}

// ---- DLSS and DLSS-NR (Sim-QVFR's own: LMU) ----------------------------------------------------

void dlss_rows(Body& b) {
    {
        const DlssSettings& d = app.s.dlss;
        const std::wstring dll = dlss_dll_path(d.folder, layer_folder()), version = file_version(dll);
        const bool found = !dll.empty() && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES, chosen = !d.folder.empty();
        std::wstring text;
        if (!found)
            text = !chosen ? tr(L"Missing from this copy of Sim-QVFR, so DLSS can’t run: install Sim-QVFR again, or "
                                L"turn DLSS off.")
                           : tr(L"Not where you chose it: the included one is used.");
        else if (!chosen)
            text = version.empty() ? std::wstring(tr(L"Found, included with Sim-QVFR."))
                                   : trf(L"Version {version}, included with Sim-QVFR.", {{L"version", version}});
        else
            text = version.empty() ? std::wstring(tr(L"Found, your choice."))
                                   : trf(L"Version {version}, your choice.", {{L"version", version}});
        std::vector<std::pair<Ctl, std::wstring>> buttons;
        if (chosen)
            buttons.push_back({kDlssIncluded, tr(L"Use included")});
        buttons.push_back({kDlssFolder, tr(L"Choose…")});
        b.buttons(L"nvngx_dlss.dll", text, buttons, Applies::NextLaunch);
        b.popup(kDlssFocusMode, tr(L"Focus views"),
                tr(kDlssFocusModes[dlss_choice_index(kDlssFocusModes, d.focusScale, true)].name),
                names_of(kDlssFocusModes));
        b.segments(kDlssFocusPreset, tr(L"Focus preset"), names_of(kDlssFocusPresets),
                   dlss_choice_index(kDlssFocusPresets, d.focusPreset));
        b.segments(kDlssEyePreset, tr(L"Periphery preset"), names_of(kDlssEyePresets),
                   dlss_choice_index(kDlssEyePresets, d.eyePreset));
        b.popup(kDlssSharpening, tr(L"Texture sharpening"),
                tr(kDlssTextureSharpening[dlss_choice_index(kDlssTextureSharpening, d.textureSharpening, true)].name),
                names_of(kDlssTextureSharpening));
        b.segments(kDlssGlow, tr(L"Light glow"), names_of(kDlssLightGlow),
                   dlss_choice_index(kDlssLightGlow, d.lightGlow, true));
        b.more(tr(L"advanced options"), {tr(L"Other cars’ motion")}, [](Body& m) {
            m.toggle(kDlssCarVectors, tr(L"Other cars’ motion"), app.s.dlss.carVectors,
                     tr(L"Keeps passing cars clean; turn off for headroom."), false, Applies::NextLaunch);
        });
        b.note(tr(L"A change rebuilds DLSS in a running game: a short stutter."));
    }
}

// Sim-QVFR's own DLSS (LMU): its switch, or why it can't be used; its options under the switch while it is on.
Card dlss_card() {
    Card c{Tab::Picture, L"DLSS", Applies::Live};
    c.body = [](Body& b) {
        std::wstring why;
        if (!own_dlss_available(&why)) {
            b.note(why);
            return;
        }
        const bool on = app.s.dlss.on;
        b.toggle(kDlssOn, tr(L"Use DLSS"), on,
                 on ? tr(L"DLAA on the periphery, DLAA or an upscale on the focus.") : tr(L"Off"));
        b.reveal(kDlssOn, on, tr(L"Use DLSS"),
                 {L"nvngx_dlss.dll", tr(L"Focus views"), tr(L"Focus preset"), tr(L"Periphery preset"),
                  tr(L"Texture sharpening"), tr(L"Other cars’ motion"), L"nvngx_dlssnr.dll", tr(L"Use DLSS-NR")},
                 dlss_rows);
    };
    c.reset = [](Settings& s) { s.dlss = DlssSettings{}; };  // its switch and options; the DLL back to the included one
    return c;
}

void nr_rows(Body& c) {
    const NrSettings& n = app.s.nr;
    c.slider({kNrIntensity, tr(L"Intensity"), n.intensity});
    c.popup(kNrStyle, tr(L"Style"), tr(kNrStyles[dlss_choice_index(kNrStyles, n.style)].name), names_of(kNrStyles));
    c.popup(kNrPreset, tr(L"Preset"), tr(kNrPresets[dlss_choice_index(kNrPresets, n.preset)].name),
            names_of(kNrPresets));
    c.slider({kNrFocusScale, tr(L"Focus working size"), n.focusScale});
    c.toggle(kNrPeriphery, tr(L"Periphery too"), n.periphery,
             n.periphery ? tr(L"At least about 2 ms of GPU time more per eye.") : tr(L"Off: only the focus views"));
    c.reveal(kNrPeriphery, n.periphery, tr(L"Periphery too"),
             {tr(L"Periphery working size"), tr(L"Periphery covered")}, [](Body& p) {
        p.slider({kNrPeripheryScale, tr(L"Periphery working size"), app.s.nr.peripheryScale});
        p.slider({kNrPeripheryCoverage, tr(L"Periphery covered"), app.s.nr.peripheryCoverage});
    });
    c.more(tr(L"tuning"),
           {tr(L"Local tone"), tr(L"Local structure"), tr(L"Auto mask"), tr(L"Runs"), tr(L"Working size limit"),
            tr(L"Edge blend")}, [](Body& m) {
        const NrSettings& nn = app.s.nr;
        m.sliders({kNrLocalTone, tr(L"Local tone"), nn.localTone},
                  {kNrLocalStructure, tr(L"Local structure"), nn.localStructure});
        m.toggle(kNrAutoMask, tr(L"Auto mask"), nn.autoMask,
                 tr(L"The model finds what it treats differently (skin) itself."));
        m.segments(kNrOrder, tr(L"Runs"), items_of(kNrOrder), nn.beforeDlss ? 1 : 0);
        m.sliders({kNrLimit, tr(L"Working size limit"), nn.workingLimit, L" MP", L"", true},
                  {kNrEdgeBlend, tr(L"Edge blend"), nn.edgeBlend});
    });
    c.note(tr(L"Preset, working sizes, order and the periphery rebuild DLSS-NR: a short stutter."));
}

Card nr_card() {
    Card c{Tab::Picture, L"DLSS-NR", Applies::Live};
    c.follows = true;  // under DLSS, whose switch shows it
    c.body = [](Body& b) {
        const NrSettings& n = app.s.nr;
        const std::wstring dll = n.folder.empty() ? std::wstring() : n.folder + L"\\nvngx_dlssnr.dll";
        const bool found = !dll.empty() && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
        const std::wstring version = found ? file_version(dll) : std::wstring();
        const std::wstring text =
            n.folder.empty() ? std::wstring(tr(L"Not included with Sim-QVFR: choose the one in a game that ships "
                                               L"DLSS-NR."))
            : found          ? (version.empty() ? std::wstring(tr(L"Found."))
                                                : trf(L"Version {version}.", {{L"version", version}}))
                             : std::wstring(tr(L"Not found where you chose it."));
        b.buttons(L"nvngx_dlssnr.dll", text, {{kNrFolder, tr(L"Choose…")}}, Applies::NextLaunch);
        std::wstring why;
        const bool available = own_nr_available(&why);
        if (available)
            b.toggle(kNrOn, tr(L"Use DLSS-NR"), n.on,
                     n.on ? tr(L"Experimental and heavy: about 2 ms of GPU time a megapixel, per view.") : tr(L"Off"));
        else if (!n.folder.empty())
            b.note(why);
        b.reveal(kNrOn, available && n.on, tr(L"Use DLSS-NR"),
                 {tr(L"Intensity"), tr(L"Style"), tr(L"Preset"), tr(L"Focus working size"), tr(L"Periphery too"),
                  tr(L"Periphery working size"), tr(L"Periphery covered"), tr(L"Local tone"), tr(L"Local structure"),
                  tr(L"Auto mask"), tr(L"Runs"), tr(L"Working size limit"), tr(L"Edge blend")},
                 nr_rows);
    };
    c.reset = [](Settings& s) {  // its options; it stays on, and its DLL stays the player's
        const bool on = s.nr.on;
        const std::wstring folder = s.nr.folder;
        s.nr = NrSettings{};
        s.nr.on = on;
        s.nr.folder = folder;
    };
    return c;
}

// ---- AMD FSR (LMU, a PC without an NVIDIA RTX card: DLSS's place, the same [dlss] settings) ---------

// Which FSR this PC gets, from AMD's own query (engine/fsr_probe.h), said in a sentence.
std::wstring fsr_offer_text() {
    const fsr::Offer& o = app.fsrOffer;
    if (app.fsrProbing || !o.checked)
        return tr(L"Asking AMD’s FSR which version your graphics card gets…");
    if (!o.why.empty())
        return tr(o.why);  // engine/fsr_probe.h's reason, marked there
    const std::wstring name = fsr::offer_name(o);
    return o.major >= 4 ? trf(L"{fsr} on your {gpu}: AMD’s machine-learned upscaler. Included with Sim-QVFR.",
                              {{L"fsr", name}, {L"gpu", o.gpu}})
                        : trf(L"{fsr} on your {gpu}: AMD’s analytic upscaler. Included with Sim-QVFR.",
                              {{L"fsr", name}, {L"gpu", o.gpu}});
}

void fsr_rows(Body& b) {
    const DlssSettings& d = app.s.dlss;
    b.note(fsr_offer_text());
    b.popup(kDlssFocusMode, tr(L"Focus views"),
            tr(kFsrFocusModes[dlss_choice_index(kFsrFocusModes, d.focusScale, true)].name), names_of(kFsrFocusModes));
    b.popup(kDlssSharpening, tr(L"Texture sharpening"),
            tr(kDlssTextureSharpening[dlss_choice_index(kDlssTextureSharpening, d.textureSharpening, true)].name),
            names_of(kDlssTextureSharpening));
    b.more(tr(L"advanced options"), {tr(L"Other cars’ motion")}, [](Body& m) {
        m.toggle(kDlssCarVectors, tr(L"Other cars’ motion"), app.s.dlss.carVectors,
                 tr(L"Keeps passing cars clean; turn off for headroom."), false, Applies::NextLaunch);
    });
    b.note(tr(L"A change rebuilds FSR in a running game: a short stutter."));
}

// AMD FSR in DLSS's place: its switch is DLSS's ([dlss] on), its options DLSS's but the presets (FSR has none).
Card fsr_card() {
    Card c{Tab::Picture, tr(L"FSR (experimental)"), Applies::Live};
    c.body = [](Body& b) {
        std::wstring why;
        if (!own_upscaler_available(&why)) {
            b.note(why);
            return;
        }
        b.note(tr(L"Experimental: Sim-QVFR’s FSR is built like its DLSS, but without AMD hardware to test on so far it "
                  L"has only run as FSR 3.1 on an NVIDIA card. FSR 4 is untested."));
        const bool on = app.s.dlss.on;
        b.toggle(kDlssOn, tr(L"Use FSR"), on,
                 on ? tr(L"Native AA on the periphery, native AA or an upscale on the focus.") : tr(L"Off"));
        b.reveal(kDlssOn, on, tr(L"Use FSR"),
                 {tr(L"Focus views"), tr(L"Texture sharpening"), tr(L"Other cars’ motion")},
                 fsr_rows);
    };
    c.reset = [](Settings& s) {  // its switch and options; DLSS's own DLL choice kept
        const std::wstring folder = s.dlss.folder;
        s.dlss = DlssSettings{};
        s.dlss.folder = folder;
    };
    return c;
}

// ---- Cheeky Foveated DLSS and its frame generation (ACE, ACC) ----------------------------------

void cheeky_rows(Body& b) {
    {
        const CheekySettings& k = app.s.cheeky;
        const std::wstring runtime = cheeky_runtime_path(app.ini, layer_folder());
        const bool found = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        b.buttons(tr(L"Runtime"),
                  found ? tr(L"Found.")
                        : tr(L"Not installed: Cheeky is a separate download (see Launch checks’ How to install it), or "
                             L"choose where it is."),
                  {{kCheekyRuntime, tr(L"Choose…")}}, Applies::NextLaunch);
        const CheekyLayer layer = cheeky_layer_state();
        b.info(tr(L"OpenXR layer"), layer == CheekyLayer::Enabled    ? tr(L"Installed")
                                    : layer == CheekyLayer::Disabled ? tr(L"Turned off")
                                                                     : tr(L"Not installed"));
        if (layer != CheekyLayer::Enabled)
            b.note(layer == CheekyLayer::Disabled
                       ? tr(L"Registered but turned off: run CheekyOpenXRSetup.exe again.")
                       : tr(L"Install it with CheekyOpenXRSetup.exe from Cheeky’s release."));
        const std::wstring& status = app.runtime.cheekyStatus;
        if (status == L"missing")
            b.note(tr(L"At the last launch, Cheeky’s runtime was not found."));
        else if (status == L"refused")
            b.note(tr(L"At the last launch, Cheeky did not start: see its log next to its runtime."));
        b.divider();
        b.toggle(kCheekyOn, tr(L"Foveated DLSS"), k.processing, tr(L"Off leaves the game’s own DLSS on every view."));
        b.toggle(kCheekyOuter, tr(L"Cheaper outer views"), k.outerDlaa,
                 k.outerDlaa ? tr(L"Cheeky’s own DLAA pass on the periphery, at a lower resolution.") : tr(L"Off"));
        b.reveal(kCheekyOuter, k.outerDlaa, tr(L"Cheaper outer views"),
                 {tr(L"Outer DLAA scale"), tr(L"Outer preset")}, [](Body& r) {
            r.slider({kCheekyScale, tr(L"Outer DLAA scale"), app.s.cheeky.outerScale});
            r.popup(kCheekyOuterPreset, tr(L"Outer preset"), tr(cheeky_preset_name(app.s.cheeky.outerPreset)),
                    items_of(kCheekyOuterPreset));
        });
        b.popup(kCheekyFocusPreset, tr(L"Focus preset"), tr(cheeky_preset_name(k.focusPreset)),
                items_of(kCheekyFocusPreset));
        b.more(tr(L"experimental options"),
               {tr(L"Focus supersampling"), tr(L"Fix motion-vector blur"), L"DLSS-NR", tr(L"DLSS-NR preset"),
                tr(L"DLSS-NR passes")},
               [](Body& m) {
                   const CheekySettings& ck = app.s.cheeky;
                   m.slider({kCheekySupersampling, tr(L"Focus supersampling"), ck.supersampling});
                   m.toggle(kCheekyMotionFix, tr(L"Fix motion-vector blur"), ck.motionFix,
                            tr(L"Cheeky’s workaround for smearing; adds GPU work."));
                   m.toggle(kCheekyNr, L"DLSS-NR", ck.nr,
                            ck.nr ? tr(L"On the focus views (nvngx_dlssnr.dll next to the game).") : tr(L"Off"));
                   m.reveal(kCheekyNr, ck.nr, L"DLSS-NR", {tr(L"DLSS-NR preset"), tr(L"DLSS-NR passes")}, [](Body& r) {
                       r.popup(kCheekyNrPreset, tr(L"DLSS-NR preset"),
                               items_of(kCheekyNrPreset)[std::clamp(app.s.cheeky.nrPreset, 0, 2)],
                               items_of(kCheekyNrPreset));
                       r.popup(kCheekyNrPasses, tr(L"DLSS-NR passes"),
                               items_of(kCheekyNrPasses)[std::clamp(app.s.cheeky.nrPasses - 1, 0, 2)],
                               items_of(kCheekyNrPasses));
                   });
               });
        if (!game_has(&games::GameFeatures::frameGeneration))
            b.note(trf(L"Frame generation: {reason}", {{L"reason", not_yet()}}));
    }
}

// Cheeky Foveated DLSS (ACE, ACC): its credit, its switch, the rest under the switch while it is on, its licence.
Card cheeky_card() {
    Card c{Tab::Picture, tr(L"Foveated DLSS"), Applies::Live};  // its switch: the next launch
    c.body = [](Body& b) {
        // Cheeky's author asked for this credit, with the version, wherever the UI controls Cheeky.
        b.note(cheeky_version_label(app.runtime.cheekyVersion) +
               (app.runtime.cheekyVersion.empty()
                    ? L"  ·  " + std::wstring(tr(L"its version shows after a launch with it"))
                    : std::wstring()));
        const bool on = app.s.cheeky.load;
        b.toggle(kCheekyUse, tr(L"Use Cheeky Foveated DLSS"), on,
                 on ? tr(L"Cheeky’s DLSS runtime in the game: a cheaper periphery.") : tr(L"Off"),
                 false, Applies::NextLaunch);
        b.reveal(kCheekyUse, on, tr(L"Use Cheeky Foveated DLSS"),
                 {tr(L"Runtime"), tr(L"OpenXR layer"), tr(L"Foveated DLSS"), tr(L"Cheaper outer views"),
                  tr(L"Outer DLAA scale"), tr(L"Outer preset"), tr(L"Focus preset"), tr(L"Focus supersampling"),
                  tr(L"Fix motion-vector blur"), L"DLSS-NR", tr(L"DLSS-NR preset"), tr(L"DLSS-NR passes"),
                  tr(L"Frame generation")},
                 cheeky_rows);
        b.note(tr(L"Cheeky Foveated DLSS is made by ClarkCheekyKent and is free software under the GNU GPL v3 "
                  L"(github.com/ClarkCheekyKent/CheekyFoveatedDLSS)."));
    };
    c.reset = [](Settings& s) {  // its switch and processing options; frame generation is its own card
        const CheekySettings d;
        CheekySettings& k = s.cheeky;
        k.load = d.load;
        k.processing = d.processing;
        k.outerDlaa = d.outerDlaa;
        k.outerScale = d.outerScale;
        k.outerPreset = d.outerPreset;
        k.focusPreset = d.focusPreset;
        k.supersampling = d.supersampling;
        k.motionFix = d.motionFix;
        k.nr = d.nr;
        k.nrPreset = d.nrPreset;
        k.nrPasses = d.nrPasses;
    };
    return c;
}

void frame_generation_rows(Body& b) {
    {
        const CheekySettings& k = app.s.cheeky;
        std::vector<std::wstring> items;
        b.segments(kFgMultiplier, tr(L"Multiplier"), items_of(kFgMultiplier), choice_items(kFgMultiplier, items));
        b.slider({kFgSize, tr(L"Center region"), k.fgSize});
        b.segments(kFgScale, tr(L"Center FG scale"), items_of(kFgScale), choice_items(kFgScale, items));
        b.segments(kFgPeriphery, tr(L"Periphery FG scale"), items_of(kFgPeriphery), choice_items(kFgPeriphery, items));
        b.toggle(kFgBorder, tr(L"Show the center region"), k.fgBorder,
                 tr(L"Outlines it in the headset, to check it follows your eyes."));
        b.more(tr(L"timing"), {tr(L"Release game")}, [](Body& m) {
            std::vector<std::wstring> releases;
            const int i = choice_items(kFgRelease, releases);
            m.popup(kFgRelease, tr(L"Release game"), releases[static_cast<size_t>(i)], releases);
        });
        b.note(tr(L"Turn OpenXR Toolkit’s Turbo mode and other frame generation (VectorXR’s) off. Lower scales use "
                  L"less GPU time and memory."));
    }
}

// Cheeky's frame generation (ACE): its switch, its options under it while it is on. It follows Foveated DLSS, whose
// switch shows it.
Card frame_generation_card() {
    Card c{Tab::Picture, tr(L"Frame generation"), Applies::Live};
    c.follows = true;
    c.body = [](Body& b) {
        const bool on = app.s.cheeky.fg;
        b.toggle(kFgOn, tr(L"Frame generation"), on,
                 on ? tr(L"Generated frames between the game’s, from its depth and motion.") : tr(L"Off"));
        b.reveal(kFgOn, on, tr(L"Frame generation"),
                 {tr(L"Multiplier"), tr(L"Center region"), tr(L"Center FG scale"), tr(L"Periphery FG scale"),
                  tr(L"Show the center region"), tr(L"Release game")},
                 frame_generation_rows);
    };
    c.reset = [](Settings& s) {  // its switch and options
        const CheekySettings d;
        CheekySettings& k = s.cheeky;
        k.fg = d.fg;
        k.fgMultiplier = d.fgMultiplier;
        k.fgSize = d.fgSize;
        k.fgScale = d.fgScale;
        k.fgPeriphery = d.fgPeriphery;
        k.fgBorder = d.fgBorder;
        k.fgRelease = d.fgRelease;
    };
    return c;
}

// The cards this game and these settings show, in reading order.
std::vector<Card> picture_cards() {
    std::vector<Card> cards = {resolution_card(), foveation_card(), preview_card()};
    if (game_has(&games::GameFeatures::gpuRegulator))
        cards.push_back(regulator_card());
    if (game_has(&games::GameFeatures::sharpening))  // every game whose compositor runs; after Foveation, as in the headset
        cards.push_back(color_card());
    if (game_has(&games::GameFeatures::carLighting))  // LMU: after Color, as in the headset menu's Color page
        cards.push_back(night_cars_card());
    if (game_has(&games::GameFeatures::fovCrop) || game_has(&games::GameFeatures::helmet))
        cards.push_back(view_card());
    if (rendering_shown())
        cards.push_back(rendering_card());
    // Each feature's card holds its switch; what its switch turns on follows straight under it.
    if (game_has(&games::GameFeatures::sideLoadedDlss) && fsr_pc()) {
        cards.push_back(fsr_card());  // no NVIDIA RTX card: AMD FSR, the same settings
    } else if (game_has(&games::GameFeatures::sideLoadedDlss)) {
        cards.push_back(dlss_card());
        if (own_dlss_available() && app.s.dlss.on)
            cards.push_back(nr_card());
    }
    if (game_has(&games::GameFeatures::foveatedDlss)) {
        cards.push_back(cheeky_card());
        if (game_has(&games::GameFeatures::frameGeneration) && app.s.cheeky.load)
            cards.push_back(frame_generation_card());
    }
    return cards;
}

}  // namespace

float picture_page(float x, float y, float w, bool draw) { return card_grid(picture_cards(), x, y, w, draw); }

}  // namespace simqvfr::launcher::ui::win

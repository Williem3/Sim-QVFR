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
    else if (app.s.mode != RenderMode::Qvfr)
        reason = L"Available in Quad View Foveated Rendering.";
    if (why)
        *why = reason;
    return reason.empty();
}

bool own_dlss_available(std::wstring* why) {
    std::wstring reason;
    if (!own_upscaler_available(&reason))
        ;  // the game's or the mode's reason
    else if (fsr_pc())
        reason = L"Needs an NVIDIA RTX graphics card (20 series or newer): none was found.";
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
            return L"NVIDIA’s multi-view rendering";
        return L"NVIDIA’s multi-view rendering, on your " + std::wstring(gpu->name.begin(), gpu->name.end());
    }
    return full ? L"Sim-QVFR’s own multi-view rendering (instancing), since no NVIDIA RTX card (20 series or newer) was found"
                : L"Sim-QVFR’s multi-view rendering";
}

bool own_nr_available(std::wstring* why) {
    std::wstring reason;
    if (!own_dlss_available(&reason))
        ;  // DLSS's own reason
    else if (!app.s.dlss.on)
        reason = L"Needs DLSS on: it works from DLSS’s depth and motion.";
    else if (app.s.nr.folder.empty())
        reason = L"Choose your nvngx_dlssnr.dll first.";
    else if (GetFileAttributesW((app.s.nr.folder + L"\\nvngx_dlssnr.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        reason = L"nvngx_dlssnr.dll isn’t in the folder chosen for it.";
    if (why)
        *why = reason;
    return reason.empty();
}

namespace {

bool qvfr() { return app.s.mode == RenderMode::Qvfr; }

std::wstring percent(double v) { return std::to_wstring(static_cast<int>(std::lround(v * 100))) + L"%"; }

// What the running game started with in QVFR: live resolution goes down from it and back up to it, not above (its
// periphery and focus view sizes, percent of the native eye). False without a QVFR game running with Sim-QVFR.
bool running_launch(LaunchSizes& launch) {
    if (running_live_ini(runtime_info_path()).empty())
        return false;
    launch = load_launch_sizes(runtime_info_path());
    return launch.known && launch.mode == RenderMode::Qvfr && launch.outer > 0 && launch.focusWidth > 0 &&
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
        out.push_back(c.name);
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
    Card c{Tab::Picture, L"Resolution", !qvfr() ? Applies::NextLaunch : layerSizes ? Applies::Live : Applies::LiveLowered};
    c.body = [](Body& b) {
        const Readouts ro = readouts(app.s, app.runtime);
        const bool known = app.runtime.known();
        b.slider({kOuter, L"Periphery resolution", app.s.outer, L"%", known ? px(ro.outer) : L""});
        if (qvfr()) {
            b.slider({kInner, L"Focus density", app.s.inner, L"%", known ? px(ro.focus) : L""});
            if (game_has(&games::GameFeatures::sharpening))
                b.slider({kSharpen, L"Sharpening", app.s.sharpen, L"%", app.s.sharpen == 0 ? L"Off" : L""});
            if (game_has(&games::GameFeatures::objectDetail))
                b.slider({kObjectDetail, L"Object detail", app.s.objectDetail});
        } else {
            b.note(L"In Two-loop the focus density (" + percent(ro.focusDensity) +
                   L") follows from Periphery resolution and the focus size. Focus density, sharpening and object detail are "
                   L"set in Quad View Foveated Rendering.");
            b.hidden(L"Periphery resolution", L"Quad View Foveated Rendering", {L"Focus density", L"Sharpening", L"Object detail"});
        }
        b.note(known ? L"Percent of the headset’s " + std::to_wstring(app.runtime.width) + L" × " +
                           std::to_wstring(app.runtime.height) + L" per eye; the periphery and the focus are set apart."
                     : L"Percent of the headset’s resolution; pixel sizes appear after the first launch.");
        if (qvfr() && game_has(&games::GameFeatures::layerMultiView)) {
            b.note(L"Changes apply while you drive, inside the game’s own render size: the focus and the periphery share "
                   L"it, so a large focus at a high Focus density leaves less room for the periphery.");
            return;
        }
        if (!qvfr() || !game_has(&games::GameFeatures::liveResolution))
            return;
        // The live ceiling (its badge's "Now when lowered"): said before the launch, with numbers while it runs.
        LaunchSizes launch;
        if (running_launch(launch)) {
            const int density = std::min(launch.focusWidth * 100 / std::max(1, app.s.width),
                                          launch.focusHeight * 100 / std::max(1, app.s.height));
            b.note(L"This launch allows up to Periphery resolution " + std::to_wstring(launch.outer) +
                   L"% and Focus density " + std::to_wstring(density) +
                   L"% at this focus size. Higher applies at the next launch.");
        } else {
            b.note(L"Live changes go down from what you launch with, and back up to it: launch with your highest "
                   L"settings, then lower them in the headset.");
        }
        if (auto_fit_game())
            b.note(L"In the headset, Auto-fit to headset rate sets these and the focus size to fill your GPU’s frame "
                   L"budget while you drive (Right starts it, Left undoes it).");
    };
    c.reset = [](Settings& s) {
        const Settings d;
        s.outer = d.outer;
        s.inner = d.inner;
        s.sharpen = d.sharpen;
        s.objectDetail = d.objectDetail;
    };
    return c;
}

// ---- Foveation ---------------------------------------------------------------------------------

void outline_color_row(Body& b) {
    b.custom(32, [](const Rect& r) {
        app.canvas.text(L"Outline color", t::kBody, {r.x, r.y + 6, r.w / 2, 20}, col(app.c->label));
        help_icon(L"Outline color", r.x + std::ceil(app.canvas.measure(L"Outline color", t::kBody)) + 6, r.y + 6);
        const float sw = segmented_width();
        segmented(kColor, {r.right() - sw, r.y, sw, 32}, static_cast<int>(app.s.color), false);
        app.drawnLabels.push_back({L"Outline color", app.drawingSection, r});
    });
}

Card foveation_card() {
    Card c{Tab::Picture, L"Foveation", Applies::Live};
    if (eye_warning())
        c.chip = L"Eye tracker not detected";
    c.body = [](Body& b) {
        const Settings& s = app.s;
        b.segments(kMode, L"Foveated mode", items_of(kMode), s.eyeTracking ? 0 : 1);
        if (eye_warning())
            b.note(L"The focus region stays centred until a tracker connects.");
        b.sliders({kWidth, L"Focus width", s.width}, {kHeight, L"Focus height", s.height});
        // A focus larger than the running game's launch renders at a lower density until the next launch.
        if (LaunchSizes launch; qvfr() && game_has(&games::GameFeatures::liveResolution) && running_launch(launch) &&
                                (s.inner * s.width > launch.focusWidth * 100 + 50 ||
                                 s.inner * s.height > launch.focusHeight * 100 + 50))
            b.note(L"Larger than this launch allows at your Focus density: it renders less dense until the next launch.");
        b.segments(kShape, L"Shape", items_of(kShape), s.shape.corners ? 1 : 0);
        if (!s.shape.corners) {
            b.slider({kCorner, L"Corner radius", s.corner});
            b.hidden(L"Shape", L"Custom corners", {L"Top left in", L"Top right in", L"Bottom left in", L"Bottom right in"});
        } else {  // each corner's two insets, in the corners' order on the box
            const auto corner = [](Ctl id, const wchar_t* label) { return SliderSpec{id, label, *slider_value(id)}; };
            b.sliders(corner(kShapeTopLeftIn, L"Top left in"), corner(kShapeTopLeftDown, L"Top left down"));
            b.sliders(corner(kShapeTopRightIn, L"Top right in"), corner(kShapeTopRightDown, L"Top right down"));
            b.sliders(corner(kShapeBottomLeftIn, L"Bottom left in"), corner(kShapeBottomLeftUp, L"Bottom left up"));
            b.sliders(corner(kShapeBottomRightIn, L"Bottom right in"), corner(kShapeBottomRightUp, L"Bottom right up"));
            b.hidden(L"Shape", L"Rectangle", {L"Corner radius"});
        }
        b.slider({kFeather, L"Feather", s.feather});
        b.note(L"See the edge of the sharp area? Raise Feather, or make the focus larger.");
        if (s.eyeTracking) {
            b.slider({kFocusOffset, L"Gaze offset", s.offset.gaze, L"°"});
            b.slider({kGazeDeadzone, L"Gaze dead-zone", s.gazeDeadzone, L"°", L"", true});
        } else {
            b.slider({kFocusOffset, L"Vertical position", s.offset.fixed, L"°"});
            b.hidden(L"Foveated mode", L"Eye tracked", {L"Gaze dead-zone", L"Gaze offset"});
        }
        b.divider();
        b.toggle(kOutline, L"Show focus outline", s.outline,
                 s.outline ? L"Its edge in the headset, to see where it is." : L"Off");
        b.reveal(kOutline, s.outline, L"Show focus outline", {L"Outline color"}, outline_color_row);
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
    };
    return c;
}

// ---- Preview -----------------------------------------------------------------------------------

Card preview_card() {
    Card c{Tab::Picture, L"Preview"};
    c.body = [](Body& b) {
        const float w = b.width();
        // One eye, or both: the crop's outer side is each eye's temple side, so it mirrors.
        b.segments(kPreviewEye, L"Eye", {L"Left", L"Both", L"Right"}, std::clamp(app.previewEye, 0, 2));
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
    c.slider({kHelmetRim, L"Rim", h.rim});
    c.sliders({kHelmetCorners, L"Corners", h.corners}, {kHelmetLight, L"Lighting", h.light});
    c.more(L"your own images", {L"Visor", L"Size", L"Height", L"Depth", L"Detail", L"Helmet images"}, [](Body& m) {
        const HelmetSettings& hs = app.s.helmet;
        m.buttons(L"Helmet images", L"top.png, bottom.png and visor.png in the helmet folder.", {{kHelmetFolder, L"Show folder"}});
        m.segments(kVisor, L"Visor", items_of(kVisor), static_cast<int>(hs.visor));
        m.sliders({kHelmetSize, L"Size", hs.size, L"°"}, {kHelmetHeight, L"Height", hs.height, L"°"});
        m.slider({kHelmetDepth, L"Depth", hs.depth, L" cm"});
        m.segments(kHelmetDetail, L"Detail", items_of(kHelmetDetail), hs.detail ? 1 : 0);
    });
}

Card view_card() {
    Card c{Tab::Picture, L"View & helmet", Applies::Live};
    c.body = [](Body& b) {
        const bool crop = game_has(&games::GameFeatures::fovCrop), helmet = game_has(&games::GameFeatures::helmet);
        if (!qvfr()) {
            b.note(L"The crop and the helmet are available in Quad View Foveated Rendering.");
            return;
        }
        if (crop) {
            const Settings& s = app.s;
            b.toggle(kCropOn, L"Crop the view", s.crop,
                     s.crop ? L"Skips the edges of each eye that a helmet covers." : L"Off");
            b.reveal(kCropOn, s.crop, L"Crop the view", {L"Top", L"Bottom", L"Outer side (temple)", L"Inner side (nose)"}, [](Body& r) {
                const Settings& cs = app.s;
                r.sliders({kCropTop, L"Top", cs.cropTop}, {kCropBottom, L"Bottom", cs.cropBottom});
                r.sliders({kCropOuter, L"Outer side (temple)", cs.cropOuter}, {kCropInner, L"Inner side (nose)", cs.cropInner});
            });
        }
        if (crop && helmet)
            b.divider();
        if (helmet) {
            const bool on = app.s.helmet.on;
            b.toggle(kHelmetOn, L"Show a helmet", on,
                     on ? L"A visor opening around the view, lit by the scene." : L"Off");
            b.reveal(kHelmetOn, on, L"Show a helmet",
                     {L"Rim", L"Corners", L"Lighting", L"Visor", L"Size", L"Height", L"Depth", L"Detail", L"Helmet images"},
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

// The method (Two-loop only) and how a multi-view game (LMU, ATS, ETS2) draws its focus views; DLSS and Foveated DLSS
// are their own cards, each with its switch. The runtime composing the views (Pimax, Varjo) has no switch: it is
// automatic ([experiment] native_quad_views, the report bundle says what the last launch did).
bool rendering_shown() {
    return method_shown() || multi_view_game();
}

Card rendering_card() {
    Card c{Tab::Picture, L"Rendering"};
    c.body = [](Body& b) {
        if (method_shown()) {  // Two-loop is legacy: the method shows only to leave it
            b.popup(kMethod, L"Rendering method", kMethodNames[qvfr() ? 0 : 1], items_of(kMethod), Applies::NextLaunch);
            b.note(L"Two-loop renders each headset frame twice. Switch to Quad View Foveated Rendering; Two-loop won’t be "
                   L"offered again.");
        }
        if (multi_view_game()) {
            if (!qvfr()) {
                b.note(L"The focus view is drawn in Quad View Foveated Rendering.");
            } else {
                b.note(L"Each eye’s pass also draws its focus view, by " + multi_view_way(true) + L".");
                if (game_has(&games::GameFeatures::layerMultiView))  // ATS, ETS2: the path starts with the first eye pass
                    b.note(L"The menus and the garage show as without Sim-QVFR: the focus view starts with the first frame "
                           L"that draws the world for each eye.");
            }
        }
    };
    c.reset = [](Settings& s) {
        const Settings d;
        s.mode = d.mode;
    };
    return c;
}

// ---- DLSS and DLSS-NR (Sim-QVFR's own: LMU) ----------------------------------------------------

void dlss_rows(Body& b) {
    {
        const DlssSettings& d = app.s.dlss;
        const std::wstring dll = dlss_dll_path(d.folder, layer_folder()), version = file_version(dll);
        const bool found = !dll.empty() && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES, chosen = !d.folder.empty();
        const std::wstring named = version.empty() ? std::wstring(L"Found") : L"Version " + version;
        const std::wstring text = !chosen ? (found ? named + L", included with Sim-QVFR."
                                                   : std::wstring(L"Missing from this copy of Sim-QVFR, so DLSS can’t run: install "
                                                                  L"Sim-QVFR again, or turn DLSS off."))
                                          : (found ? named + L", your choice." : std::wstring(L"Not where you chose it: the included one is used."));
        std::vector<std::pair<Ctl, std::wstring>> buttons;
        if (chosen)
            buttons.push_back({kDlssIncluded, L"Use included"});
        buttons.push_back({kDlssFolder, L"Choose…"});
        b.buttons(L"nvngx_dlss.dll", text, buttons, Applies::NextLaunch);
        b.popup(kDlssFocusMode, L"Focus views", kDlssFocusModes[dlss_choice_index(kDlssFocusModes, d.focusScale, true)].name,
                names_of(kDlssFocusModes));
        b.segments(kDlssFocusPreset, L"Focus preset", names_of(kDlssFocusPresets), dlss_choice_index(kDlssFocusPresets, d.focusPreset));
        b.segments(kDlssEyePreset, L"Periphery preset", names_of(kDlssEyePresets), dlss_choice_index(kDlssEyePresets, d.eyePreset));
        b.popup(kDlssSharpening, L"Texture sharpening",
                kDlssTextureSharpening[dlss_choice_index(kDlssTextureSharpening, d.textureSharpening, true)].name,
                names_of(kDlssTextureSharpening));
        b.more(L"advanced options", {L"Other cars’ motion"}, [](Body& m) {
            m.toggle(kDlssCarVectors, L"Other cars’ motion", app.s.dlss.carVectors,
                     L"Keeps passing cars clean; turn off for headroom.", false, Applies::NextLaunch);
        });
        b.note(L"A change rebuilds DLSS in a running game: a short stutter.");
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
        b.toggle(kDlssOn, L"Use DLSS", on,
                 on ? L"DLAA on the periphery, DLAA or an upscale on the focus." : L"Off");
        b.reveal(kDlssOn, on, L"Use DLSS",
                 {L"nvngx_dlss.dll", L"Focus views", L"Focus preset", L"Periphery preset", L"Texture sharpening", L"Other cars’ motion",
                  L"nvngx_dlssnr.dll", L"Use DLSS-NR"},
                 dlss_rows);
    };
    c.reset = [](Settings& s) { s.dlss = DlssSettings{}; };  // its switch and options; the DLL back to the included one
    return c;
}

void nr_rows(Body& c) {
    const NrSettings& n = app.s.nr;
    c.slider({kNrIntensity, L"Intensity", n.intensity});
    c.popup(kNrStyle, L"Style", kNrStyles[dlss_choice_index(kNrStyles, n.style)].name, names_of(kNrStyles));
    c.popup(kNrPreset, L"Preset", kNrPresets[dlss_choice_index(kNrPresets, n.preset)].name, names_of(kNrPresets));
    c.slider({kNrFocusScale, L"Focus working size", n.focusScale});
    c.toggle(kNrPeriphery, L"Periphery too", n.periphery,
             n.periphery ? L"At least about 2 ms of GPU time more per eye." : L"Off: only the focus views");
    c.reveal(kNrPeriphery, n.periphery, L"Periphery too", {L"Periphery working size", L"Periphery covered"}, [](Body& p) {
        p.slider({kNrPeripheryScale, L"Periphery working size", app.s.nr.peripheryScale});
        p.slider({kNrPeripheryCoverage, L"Periphery covered", app.s.nr.peripheryCoverage});
    });
    c.more(L"tuning", {L"Local tone", L"Local structure", L"Auto mask", L"Runs", L"Working size limit", L"Edge blend"}, [](Body& m) {
        const NrSettings& nn = app.s.nr;
        m.sliders({kNrLocalTone, L"Local tone", nn.localTone}, {kNrLocalStructure, L"Local structure", nn.localStructure});
        m.toggle(kNrAutoMask, L"Auto mask", nn.autoMask, L"The model finds what it treats differently (skin) itself.");
        m.segments(kNrOrder, L"Runs", items_of(kNrOrder), nn.beforeDlss ? 1 : 0);
        m.sliders({kNrLimit, L"Working size limit", nn.workingLimit, L" MP", L"", true}, {kNrEdgeBlend, L"Edge blend", nn.edgeBlend});
    });
    c.note(L"Preset, working sizes, order and the periphery rebuild DLSS-NR: a short stutter.");
}

Card nr_card() {
    Card c{Tab::Picture, L"DLSS-NR", Applies::Live};
    c.follows = true;  // under DLSS, whose switch shows it
    c.body = [](Body& b) {
        const NrSettings& n = app.s.nr;
        const std::wstring dll = n.folder.empty() ? std::wstring() : n.folder + L"\\nvngx_dlssnr.dll";
        const bool found = !dll.empty() && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
        const std::wstring version = found ? file_version(dll) : std::wstring();
        const std::wstring text = n.folder.empty() ? L"Not included with Sim-QVFR: choose the one in a game that ships DLSS-NR."
                                  : found          ? (version.empty() ? std::wstring(L"Found.") : L"Version " + version + L".")
                                                   : std::wstring(L"Not found where you chose it.");
        b.buttons(L"nvngx_dlssnr.dll", text, {{kNrFolder, L"Choose…"}}, Applies::NextLaunch);
        std::wstring why;
        const bool available = own_nr_available(&why);
        if (available)
            b.toggle(kNrOn, L"Use DLSS-NR", n.on,
                     n.on ? L"Experimental and heavy: about 2 ms of GPU time a megapixel, per view." : L"Off");
        else if (!n.folder.empty())
            b.note(why);
        b.reveal(kNrOn, available && n.on, L"Use DLSS-NR",
                 {L"Intensity", L"Style", L"Preset", L"Focus working size", L"Periphery too", L"Periphery working size",
                  L"Periphery covered", L"Local tone", L"Local structure", L"Auto mask", L"Runs", L"Working size limit",
                  L"Edge blend"},
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
        return L"Asking AMD’s FSR which version your graphics card gets…";
    if (!o.why.empty())
        return o.why;
    const std::wstring name = fsr::offer_name(o);
    return name + L" on your " + o.gpu + (o.major >= 4 ? L": AMD’s machine-learned upscaler." : L": AMD’s analytic upscaler.") +
           L" Included with Sim-QVFR.";
}

void fsr_rows(Body& b) {
    const DlssSettings& d = app.s.dlss;
    b.note(fsr_offer_text());
    b.popup(kDlssFocusMode, L"Focus views", kFsrFocusModes[dlss_choice_index(kFsrFocusModes, d.focusScale, true)].name,
            names_of(kFsrFocusModes));
    b.popup(kDlssSharpening, L"Texture sharpening",
            kDlssTextureSharpening[dlss_choice_index(kDlssTextureSharpening, d.textureSharpening, true)].name,
            names_of(kDlssTextureSharpening));
    b.more(L"advanced options", {L"Other cars’ motion"}, [](Body& m) {
        m.toggle(kDlssCarVectors, L"Other cars’ motion", app.s.dlss.carVectors,
                 L"Keeps passing cars clean; turn off for headroom.", false, Applies::NextLaunch);
    });
    b.note(L"A change rebuilds FSR in a running game: a short stutter.");
}

// AMD FSR in DLSS's place: its switch is DLSS's ([dlss] on), its options DLSS's but the presets (FSR has none).
Card fsr_card() {
    Card c{Tab::Picture, L"FSR (experimental)", Applies::Live};
    c.body = [](Body& b) {
        std::wstring why;
        if (!own_upscaler_available(&why)) {
            b.note(why);
            return;
        }
        b.note(L"Experimental: Sim-QVFR’s FSR is built like its DLSS, but without AMD hardware to test on so far it has only "
               L"run as FSR 3.1 on an NVIDIA card. FSR 4 is untested.");
        const bool on = app.s.dlss.on;
        b.toggle(kDlssOn, L"Use FSR", on, on ? L"Native AA on the periphery, native AA or an upscale on the focus." : L"Off");
        b.reveal(kDlssOn, on, L"Use FSR", {L"Focus views", L"Texture sharpening", L"Other cars’ motion"}, fsr_rows);
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
        b.buttons(L"Runtime",
                  found ? L"Found." : L"Not installed: Cheeky is a separate download (see Launch checks’ How to install it), or choose where it is.",
                  {{kCheekyRuntime, L"Choose…"}}, Applies::NextLaunch);
        const CheekyLayer layer = cheeky_layer_state();
        b.info(L"OpenXR layer", layer == CheekyLayer::Enabled ? L"Installed" : layer == CheekyLayer::Disabled ? L"Turned off" : L"Not installed");
        if (layer != CheekyLayer::Enabled)
            b.note(layer == CheekyLayer::Disabled ? L"Registered but turned off: run CheekyOpenXRSetup.exe again."
                                                  : L"Install it with CheekyOpenXRSetup.exe from Cheeky’s release.");
        const std::wstring& status = app.runtime.cheekyStatus;
        if (status == L"missing")
            b.note(L"At the last launch, Cheeky’s runtime was not found.");
        else if (status == L"refused")
            b.note(L"At the last launch, Cheeky did not start: see its log next to its runtime.");
        b.divider();
        b.toggle(kCheekyOn, L"Foveated DLSS", k.processing, L"Off leaves the game’s own DLSS on every view.");
        b.toggle(kCheekyOuter, L"Cheaper outer views", k.outerDlaa,
                 k.outerDlaa ? L"Cheeky’s own DLAA pass on the periphery, at a lower resolution." : L"Off");
        b.reveal(kCheekyOuter, k.outerDlaa, L"Cheaper outer views", {L"Outer DLAA scale", L"Outer preset"}, [](Body& r) {
            r.slider({kCheekyScale, L"Outer DLAA scale", app.s.cheeky.outerScale});
            r.popup(kCheekyOuterPreset, L"Outer preset", cheeky_preset_name(app.s.cheeky.outerPreset), items_of(kCheekyOuterPreset));
        });
        b.popup(kCheekyFocusPreset, L"Focus preset", cheeky_preset_name(k.focusPreset), items_of(kCheekyFocusPreset));
        b.more(L"experimental options", {L"Focus supersampling", L"Fix motion-vector blur", L"DLSS-NR", L"DLSS-NR preset", L"DLSS-NR passes"},
               [](Body& m) {
                   const CheekySettings& ck = app.s.cheeky;
                   m.slider({kCheekySupersampling, L"Focus supersampling", ck.supersampling});
                   m.toggle(kCheekyMotionFix, L"Fix motion-vector blur", ck.motionFix, L"Cheeky’s workaround for smearing; adds GPU work.");
                   m.toggle(kCheekyNr, L"DLSS-NR", ck.nr,
                            ck.nr ? L"On the focus views (nvngx_dlssnr.dll next to the game)." : L"Off");
                   m.reveal(kCheekyNr, ck.nr, L"DLSS-NR", {L"DLSS-NR preset", L"DLSS-NR passes"}, [](Body& r) {
                       r.popup(kCheekyNrPreset, L"DLSS-NR preset", items_of(kCheekyNrPreset)[std::clamp(app.s.cheeky.nrPreset, 0, 2)],
                               items_of(kCheekyNrPreset));
                       r.popup(kCheekyNrPasses, L"DLSS-NR passes", items_of(kCheekyNrPasses)[std::clamp(app.s.cheeky.nrPasses - 1, 0, 2)],
                               items_of(kCheekyNrPasses));
                   });
               });
        if (!game_has(&games::GameFeatures::frameGeneration))
            b.note(L"Frame generation: " + not_yet());
    }
}

// Cheeky Foveated DLSS (ACE, ACC): its credit, its switch, the rest under the switch while it is on, its licence.
Card cheeky_card() {
    Card c{Tab::Picture, L"Foveated DLSS", Applies::Live};  // its switch: the next launch
    c.body = [](Body& b) {
        // Cheeky's author asked for this credit, with the version, wherever the UI controls Cheeky.
        b.note(cheeky_version_label(app.runtime.cheekyVersion) +
               (app.runtime.cheekyVersion.empty() ? L"  ·  its version shows after a launch with it" : L""));
        const bool on = app.s.cheeky.load;
        b.toggle(kCheekyUse, L"Use Cheeky Foveated DLSS", on,
                 on ? L"Cheeky’s DLSS runtime in the game: a cheaper periphery." : L"Off",
                 false, Applies::NextLaunch);
        b.reveal(kCheekyUse, on, L"Use Cheeky Foveated DLSS",
                 {L"Runtime", L"OpenXR layer", L"Foveated DLSS", L"Cheaper outer views", L"Outer DLAA scale", L"Outer preset",
                  L"Focus preset", L"Focus supersampling", L"Fix motion-vector blur", L"DLSS-NR", L"DLSS-NR preset", L"DLSS-NR passes",
                  L"Frame generation"},
                 cheeky_rows);
        b.note(L"Cheeky Foveated DLSS is made by ClarkCheekyKent and is free software under the GNU GPL v3 "
               L"(github.com/ClarkCheekyKent/CheekyFoveatedDLSS).");
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
        b.segments(kFgMultiplier, L"Multiplier", items_of(kFgMultiplier), choice_items(kFgMultiplier, items));
        b.slider({kFgSize, L"Center region", k.fgSize});
        b.segments(kFgScale, L"Center FG scale", items_of(kFgScale), choice_items(kFgScale, items));
        b.segments(kFgPeriphery, L"Periphery FG scale", items_of(kFgPeriphery), choice_items(kFgPeriphery, items));
        b.toggle(kFgBorder, L"Show the center region", k.fgBorder, L"Outlines it in the headset, to check it follows your eyes.");
        b.more(L"timing", {L"Release game"}, [](Body& m) {
            std::vector<std::wstring> releases;
            const int i = choice_items(kFgRelease, releases);
            m.popup(kFgRelease, L"Release game", releases[static_cast<size_t>(i)], releases);
        });
        b.note(L"Turn OpenXR Toolkit’s Turbo mode and other frame generation (VectorXR’s) off. Lower scales use less GPU time "
               L"and memory.");
    }
}

// Cheeky's frame generation (ACE): its switch, its options under it while it is on. It follows Foveated DLSS, whose
// switch shows it.
Card frame_generation_card() {
    Card c{Tab::Picture, L"Frame generation", Applies::Live};
    c.follows = true;
    c.body = [](Body& b) {
        const bool on = app.s.cheeky.fg;
        b.toggle(kFgOn, L"Frame generation", on,
                 on ? L"Generated frames between the game’s, from its depth and motion." : L"Off");
        b.reveal(kFgOn, on, L"Frame generation",
                 {L"Multiplier", L"Center region", L"Center FG scale", L"Periphery FG scale", L"Show the center region", L"Release game"},
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

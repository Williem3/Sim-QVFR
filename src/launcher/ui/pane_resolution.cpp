// The Resolution pane: per-eye resolution, object detail, NVIDIA multi-view rendering, the FOV crop and the helmet
// overlay.
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>

#include "../settings.h"
#include "../../engine/nvapi_gpus.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

float resolution_pane(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool qvfr = app.s.mode == RenderMode::Qvfr;
    float h = 0;
    const bool known = app.runtime.known();
    const std::wstring footer =
        known ? L"Percent of the headset’s recommended resolution (" + std::to_wstring(app.runtime.width) + L" × " +
                    std::to_wstring(app.runtime.height) + L" px per eye). Outer and inner are set independently."
              : L"Percent of the headset’s recommended resolution. Pixel sizes appear after the first launch. Outer and inner are "
                L"set independently.";
    std::vector<Row> rows = {
        slider_row(kOuter, L"Outer resolution", L"The full field of view, rendered at lower density.", app.s.outer,
                   known ? px(ro.outer) : L""),
        slider_row(kInner, L"Inner resolution",
                   qvfr ? L"Pixel density inside the focus region."
                        : L"Set by outer resolution and foveated size in Two-loop Foveated Interlaced.",
                   qvfr ? app.s.inner : static_cast<int>(std::lround(ro.focusDensity * 100)),
                   known && qvfr ? px(ro.focus) : L"", !qvfr),
    };
    h += section(x, y, w, L"Per-eye resolution", rows, footer, draw, qvfr ? Applies::LiveLowered : Applies::NextLaunch);
    // Object detail (X-Plane): the LOD the focus views need, or less of it for main-thread time.
    if (game_has(&games::GameFeatures::objectDetail)) {
        h += t::kSpace6;
        h += section(x, y + h, w, L"Scenery",
                     {slider_row(kObjectDetail, L"Object detail",
                                 qvfr ? L"Tree and object detail where you look, against CPU time."
                                      : L"Available in Quad View Foveated Rendering.",
                                 app.s.objectDetail, L"", !qvfr)},
                     L"100% keeps trees and objects as detailed in the focus region as at its density; 0% is the "
                     L"game’s own detail for the outer resolution.",
                     draw, Applies::Live);
    }
    // NVIDIA multi-view rendering (LMU): each eye's pass draws its focus view too. Only on a GPU that has it (asked of
    // the driver once); the layer checks again at launch.
    if (game_has(&games::GameFeatures::multiView)) {
        h += t::kSpace6;
        const nvapi::Gpu* gpu = nvapi::two_views_gpu();
        std::wstring desc;
        if (!qvfr) {
            desc = L"Available in Quad View Foveated Rendering.";
        } else if (gpu) {
            desc = L"Each eye’s pass also draws its focus view, so the game prepares its scene twice a frame instead of four "
                   L"times.";
        } else {
            std::wstring found;
            for (const nvapi::Gpu& g : nvapi::gpus())
                found += (found.empty() ? L" This PC has " : L", ") + std::wstring(g.name.begin(), g.name.end());
            desc = L"Needs an NVIDIA RTX 20 series or newer graphics card." + found + (found.empty() ? L"" : L".");
        }
        // Without such a GPU it shows off and can't be chosen, even if the ini has multi_view=1 (each launch's copy has
        // it off then: main.cpp).
        const bool unavailable = !qvfr || !gpu;
        const bool shown = app.s.multiView && gpu;
        Row on;
        on.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
        on.full = [desc, unavailable, shown](const Rect& r) {
            checkbox(kMultiView, r, shown, L"Multi-view rendering", desc, unavailable);
        };
        h += section(x, y + h, w, L"NVIDIA multi-view rendering", {on},
                     gpu ? L"Saves CPU and GPU time every frame (about 2 ms of each in testing). Off renders every view on "
                           L"its own, as on any graphics card."
                         : L"Off renders every view on its own, as on any graphics card.",
                     draw, Applies::NextLaunch);
    }
    // The FOV crop (each side on its own, QVFR only) and the helmet overlay: left out for a game without them.
    if (game_has(&games::GameFeatures::fovCrop)) {
        h += t::kSpace6;
        Row cropOn;
        const std::wstring cropDesc = qvfr ? L"Skips the edges of each eye that a helmet covers, so they are never rendered."
                                           : L"Available in Quad View Foveated Rendering.";
        cropOn.fullHeight = [cropDesc](float width) { return checkbox_height(cropDesc, width); };
        cropOn.full = [cropDesc](const Rect& r) { checkbox(kCropOn, r, app.s.crop, L"Crop the view", cropDesc, false); };
        const bool cropOff = !qvfr || !app.s.crop;
        h += section(x, y + h, w, L"Field of view crop",
                     {cropOn, slider_row(kCropTop, L"Top", L"", app.s.cropTop, L"", cropOff),
                      slider_row(kCropBottom, L"Bottom", L"", app.s.cropBottom, L"", cropOff),
                      slider_row(kCropOuter, L"Outer side", L"Toward the temple, mirrored for each eye.", app.s.cropOuter, L"",
                                 cropOff),
                      slider_row(kCropInner, L"Inner side", L"Toward the nose.", app.s.cropInner, L"", cropOff)},
                     L"Share of each eye’s view removed at that side; the headset shows black there.",
                     draw, Applies::Live);
    }
    if (game_has(&games::GameFeatures::helmet)) {
        h += t::kSpace6;
        Row helmetOn;
        const std::wstring helmetDesc = qvfr ? L"Draws a helmet’s visor opening around the view, lit by the scene."
                                             : L"Available in Quad View Foveated Rendering.";
        helmetOn.fullHeight = [helmetDesc](float width) { return checkbox_height(helmetDesc, width); };
        helmetOn.full = [helmetDesc](const Rect& r) {
            checkbox(kHelmetOn, r, app.s.helmet.on, L"Show a helmet", helmetDesc, false);
        };
        const bool helmetOff = !qvfr || !app.s.helmet.on;
        h += section(x, y + h, w, L"Helmet overlay",
                     {helmetOn,
                      slider_row(kHelmetRim, L"Rim", L"How far the padding reaches into the view.", app.s.helmet.rim, L"",
                                 helmetOff),
                      slider_row(kHelmetCorners, L"Corners", L"Rounds the visor opening.", app.s.helmet.corners, L"",
                                 helmetOff),
                      slider_row(kHelmetLight, L"Lighting", L"How much the scene lights the helmet.", app.s.helmet.light, L"",
                                 helmetOff),
                      [&] {
                          const std::wstring names[3] = {kVisorPositionNames[0], kVisorPositionNames[1], kVisorPositionNames[2]};
                          Row visor = choice_row(kVisor, L"Visor", names, 3, static_cast<int>(app.s.helmet.visor), helmetOff);
                          visor.desc = L"Your images only. Ctrl+Alt+V in the game steps it.";
                          return visor;
                      }(),
                      slider_row(kHelmetSize, L"Size", L"Your images only: how wide they are, in degrees of view.",
                                 app.s.helmet.size, L"", helmetOff, L"°"),
                      slider_row(kHelmetHeight, L"Height", L"Your images only: degrees above straight ahead.",
                                 app.s.helmet.height, L"", helmetOff, L"°"),
                      slider_row(kHelmetDepth, L"Depth",
                                 L"Your images only: how far in front of your eyes, in cm. 0 is far away, with no depth.",
                                 app.s.helmet.depth, L"", helmetOff, L" cm"),
                      [&] {
                          const std::wstring names[2] = {L"Performance (2048)", L"Detail (4096)"};
                          Row detail = choice_row(kHelmetDetail, L"Detail", names, 2, app.s.helmet.detail ? 1 : 0, helmetOff);
                          detail.desc = L"Your images only: Detail is sharper, Performance costs less GPU and memory.";
                          return detail;
                      }()},
                     L"Your own helmet: top.png, bottom.png and visor.png in the helmet folder next to the layer.",
                     draw, Applies::Live);
    }
    return h;
}

}  // namespace simqvfr::launcher::ui::win

// The Resolution pane: per-eye resolution, object detail, sharpening, multi-view rendering, the FOV crop and the helmet overlay.
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
    // Sharpening (every game's compositor): AMD's CAS as the compositor resamples the views (compose_shader.h), on the periphery's copy when
    // the runtime composes.
    if (game_has(&games::GameFeatures::sharpening)) {
        h += t::kSpace6;
        h += section(x, y + h, w, L"Image",
                     {slider_row(kSharpen, L"Sharpening",
                                 qvfr ? L"AMD’s Contrast Adaptive Sharpening as Sim-QVFR puts the views together: more "
                                        L"where the image is soft, less at edges already sharp."
                                      : L"Available in Quad View Foveated Rendering.",
                                 app.s.sharpen, app.s.sharpen == 0 ? L"Off" : L"", !qvfr)},
                     L"0% is off. The soft outer resolution gains the most from it, and shimmers more with it.", draw,
                     Applies::Live);
    }
    // Multi-view rendering (LMU): each eye's pass draws and finishes its focus view (two passes instead of four). On any
    // graphics card: NVIDIA's multi-view rendering on an RTX 20 series or newer (asked of the driver once), Sim-QVFR's own
    // (instancing) on others; the layer checks the game's device at launch.
    if (game_has(&games::GameFeatures::multiView)) {
        h += t::kSpace6;
        const nvapi::Gpu* gpu = nvapi::two_views_gpu();
        std::wstring desc;
        if (!qvfr) {
            desc = L"Available in Quad View Foveated Rendering.";
        } else {
            desc = L"Each eye’s pass also draws and finishes its focus view, so the game renders only its own two eye passes "
                   L"a frame instead of four, and Sim-QVFR changes none of the game’s code.";
            desc += gpu ? L" On this graphics card with NVIDIA’s multi-view rendering."
                        : L" On this graphics card with Sim-QVFR’s own (each draw drawn twice by instancing).";
        }
        const bool unavailable = !qvfr;
        const bool shown = app.s.multiView;
        Row on;
        on.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
        on.full = [desc, unavailable, shown](const Rect& r) {
            checkbox(kMultiView, r, shown, L"Multi-view rendering", desc, unavailable);
        };
        h += section(x, y + h, w, L"Multi-view rendering", {on},
                     L"On by default. Saves CPU and GPU time every frame (in testing about 1.5 ms of CPU, and 1 ms of GPU "
                     L"with NVIDIA’s or 0.65 ms with Sim-QVFR’s own). Off renders every view in a pass of its own, "
                     L"changing the game’s code to do it.",
                     draw, Applies::NextLaunch);
    }
    // The runtime composing the views (LMU on a Pimax or Varjo runtime: layer/native_quad.cpp): on by default, used
    // only where the runtime offers quad views. The description ends with what the last launch found.
    if (game_has(&games::GameFeatures::nativeQuadViews)) {
        h += t::kSpace6;
        std::wstring desc;
        if (!qvfr) {
            desc = L"Available in Quad View Foveated Rendering.";
        } else {
            desc = L"On a Pimax or Varjo runtime, the runtime puts the focus region on the display at its full "
                   L"resolution: sharper than Sim-QVFR can compose it. On other runtimes (SteamVR) nothing changes.";
            const RuntimeInfo& rt = app.runtime;
            if (rt.quadViewsUsed)
                desc += L" Last launch: the runtime composed the views.";
            else if (rt.quadViews == L"offered")
                desc += L" Last launch: the runtime offers it, but it wasn’t used (off, frame generation, or see the log).";
            else if (rt.quadViews == L"none")
                desc += L" Last launch: this runtime doesn’t offer it, so Sim-QVFR composed the views.";
            else if (rt.quadViews == L"unknown")
                desc += L" Last launch: another OpenXR layer sits between Sim-QVFR and the runtime, so it wasn’t tried.";
        }
        const bool unavailable = !qvfr;
        const bool shown = app.s.nativeQuad != 0;
        Row on;
        on.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
        on.full = [desc, unavailable, shown](const Rect& r) {
            checkbox(kNativeQuad, r, shown, L"Let the runtime compose (Pimax, Varjo)", desc, unavailable);
        };
        h += section(x, y + h, w, L"Runtime composition", {on},
                     L"With the runtime composing, its own quad view settings (Pimax Play’s resolution and focus area "
                     L"sliders) don’t apply: the resolution and focus region set here do, and change while you drive. Off "
                     L"with frame generation (Cheeky’s or VectorXR’s), which needs one composed image.",
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

// The Foveation and Focus overlay panes, and the side panel's foveation preview, legend and readouts.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "../launch_plan.h"
#include "../settings.h"
#include "../../games/game.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

float foveation_pane(float x, float y, float w, bool draw) {
    const bool qvfr = app.s.mode == RenderMode::Qvfr;
    float h = 0;
    if (eye_warning()) {
        if (draw)
            badge(x, y, 1, L"Eye tracker not detected. The focus region stays centered until one connects.", w);
        h += 20 + t::kSpace6;
    }
    Row method, mode;
    method.label = L"Rendering method";
    {
        const std::wstring names[2] = {kMethodNames[0], kMethodNames[1]};
        method.controlW = popup_width(names, 2, 180);
    }
    method.controlH = t::kControlHeight;
    method.control = [](const Rect& r) { popup_button(kMethod, r, kMethodNames[app.s.mode == RenderMode::Qvfr ? 0 : 1], false); };
    mode.label = L"Foveated mode";
    mode.desc = app.s.eyeTracking ? L"The focus region follows your gaze." : L"The focus region stays at the center of each lens.";
    {
        const std::wstring names[2] = {kModeNames[0], kModeNames[1]};
        mode.controlW = popup_width(names, 2, 140);
    }
    mode.controlH = t::kControlHeight;
    mode.control = [](const Rect& r) { popup_button(kMode, r, kModeNames[app.s.eyeTracking ? 0 : 1], false); };
    h += section(x, y + h, w, L"Mode", method_shown() ? std::vector<Row>{method, mode} : std::vector<Row>{mode},
                 qvfr ? L"Eye tracked or fixed: whether the sharp focus region follows your eyes."
                      : L"Two-loop Foveated Interlaced is legacy: it renders each headset frame twice. Switch to Quad View "
                        L"Foveated Rendering; Two-loop won't be offered again.",
                 draw, method_shown() ? Applies::NextLaunch : Applies::Live);
    h += t::kSpace6;
    h += section(x, y + h, w, L"Focus region",
                 {slider_row(kWidth, L"Foveated width", L"", app.s.width, L""),
                  slider_row(kHeight, L"Foveated height", L"", app.s.height, L""),
                  slider_row(kCorner, L"Corner radius",
                             app.s.shape.corners ? L"Box shape only." : L"Rounds the corners of the focus region.",
                             app.s.corner, L"", app.s.shape.corners),
                  slider_row(kFeather, L"Feather", L"Softens the edge of the focus region.", app.s.feather, L""),
                  app.s.eyeTracking
                      ? slider_row(kFocusOffset, L"Gaze offset",
                                   L"Moves the focus region above (+) or below (−) where you look, if it sits off.",
                                   app.s.offset.gaze, L"", false, L"°")
                      : slider_row(kFocusOffset, L"Vertical position",
                                   L"Raises (+) or lowers (−) the fixed focus region, e.g. toward the road or the dash.",
                                   app.s.offset.fixed, L"", false, L"°"),
                  slider_row(kGazeDeadzone, L"Gaze dead-zone",
                             L"How far your gaze moves before the focus region follows. Higher holds it steadier "
                             L"against eye-tracker jitter (shimmer with MSAA); 0 follows every sample.",
                             app.s.gazeDeadzone, L"", !app.s.eyeTracking, L"°", true)},
                 L"Share of the outer view that the focus view covers. Fixed and eye-tracked modes keep their own "
                 L"vertical offset.",
                 draw, Applies::Live);
    h += t::kSpace6;
    {
        const std::wstring names[2] = {kShapeNames[0], kShapeNames[1]};
        Row shape = choice_row(kShape, L"Shape", names, 2, app.s.shape.corners ? 1 : 0, false);
        shape.desc = L"Box: a rectangle, with the corner radius above. Corners: move each corner in on its own.";
        const bool off = !app.s.shape.corners;
        const auto corner = [&](Ctl id, const wchar_t* label) {
            return slider_row(id, label, L"", *slider_value(id), L"", off);
        };
        h += section(x, y + h, w, L"Focus shape",
                     {shape, corner(kShapeTopLeftIn, L"Top left in"), corner(kShapeTopLeftDown, L"Top left down"),
                      corner(kShapeTopRightIn, L"Top right in"), corner(kShapeTopRightDown, L"Top right down"),
                      corner(kShapeBottomLeftIn, L"Bottom left in"), corner(kShapeBottomLeftUp, L"Bottom left up"),
                      corner(kShapeBottomRightIn, L"Bottom right in"),
                      corner(kShapeBottomRightUp, L"Bottom right up")},
                     L"Pull the top corners in for a trapezoid: more of the dash sharp, at the same cost.",
                     draw, Applies::Live);
    }
    return h;
}

float overlay_pane(float x, float y, float w, bool draw) {
    float h = 0;
    Row show;
    const std::wstring desc = L"Draws a colored border around the focus region.";
    show.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
    show.full = [desc](const Rect& r) { checkbox(kOutline, r, app.s.outline, L"Show focus outline", desc); };
    Row color;
    color.label = L"Outline color";
    color.disabled = !app.s.outline;
    color.controlW = segmented_width();
    color.controlH = t::kControlHeight;
    color.control = [](const Rect& r) { segmented(kColor, r, static_cast<int>(app.s.color), !app.s.outline); };
    h += section(x, y, w, L"Focus overlay",
                 {show, color},
                 L"The outline is drawn into the headset image, to show where the focus region is.", draw, Applies::Live);
    return h;
}

// FoveationPreview (one eye, a schematic) at (x, y), width w. Returns its height.
float preview(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool known = app.runtime.known();
    // The whole eye (the FOV crop is drawn over it); without a recommended size, the kit's example
    // aspect (2584 x 2160) shapes the drawing.
    const Size outer = known ? ro.runtime : Size{2584, 2160};
    const float W = 320, H = std::round(W * outer.height / std::max(1, outer.width)), k = w / W;
    const float frameH = H * k;
    if (draw) {
        const Rect frame{x, y, w, frameH};
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        const auto R = [&](float rx, float ry, float rw, float rh) { return Rect{x + rx * k, y + ry * k, rw * k, rh * k}; };
        const bool tfi = app.s.mode == RenderMode::TwoLoop;
        const bool eye = app.s.eyeTracking && !eye_warning();
        const float fw = W * app.s.width / 100, fh = H * app.s.height / 100;
        // Centred on the lens in both modes (eye tracked: looking straight ahead), moved by the vertical
        // offset, roughly: about 100 degrees of view top to bottom.
        const float gx = 0.5f, gy = 0.5f - app.s.offset.for_mode(app.s.eyeTracking) / 100.0f;
        // The kept view (the left eye: its outer side is on the left); the focus region stays inside it.
        const bool cropHere = game_has(&games::GameFeatures::fovCrop);  // none for a game without the crop
        const float cl = W * static_cast<float>(cropHere && app.s.crop_width() < 1 ? app.s.cropOuter : 0) / 100,
                    cr = W * static_cast<float>(cropHere && app.s.crop_width() < 1 ? app.s.cropInner : 0) / 100,
                    ct = H * static_cast<float>(cropHere && app.s.crop_height() < 1 ? app.s.cropTop : 0) / 100,
                    cb = H * static_cast<float>(cropHere && app.s.crop_height() < 1 ? app.s.cropBottom : 0) / 100;
        const float fx = std::clamp(gx * W - fw / 2, cl, std::max(cl, W - cr - fw)),
                    fy = std::clamp(gy * H - fh / 2, ct, std::max(ct, H - cb - fh));
        // Grid cell size shows pixel density: 8 in the periphery, 8 / ratio in the focus region.
        const double ratio = tfi ? ro.focusDensity * 100 / app.s.outer : ro.focusDensity * 100 / app.s.outer;
        const float pc = 8, fc = std::clamp(static_cast<float>(pc / std::max(0.01, ratio)), 2.0f, 8.0f);
        app.canvas.fill(R(0, 0, W, H), col(app.c->preview_periphery));
        for (float gxl = 0; gxl <= W; gxl += pc)
            app.canvas.line(x + gxl * k, y, x + gxl * k, y + frameH, col(app.c->preview_canvas), 0.6f * k);
        for (float gyl = 0; gyl <= H; gyl += pc)
            app.canvas.line(x, y + gyl * k, x + w, y + gyl * k, col(app.c->preview_canvas), 0.6f * k);
        if (tfi)
            for (float iy = 0; iy < H; iy += 4)
                app.canvas.fill(R(0, iy, W, 2), col(app.c->preview_canvas, 0.55f));
        // Feather: the focus region fades in over feather % of its size, as the compositor blends it;
        // its corners rounded by corner % of half the shorter side, or with the corner shape, each corner
        // pulled in (focus_shape.h; its feather drawn as the quadrilateral shrunk toward its middle).
        const float band = std::min(fw, fh) * app.s.feather / 100.0f;
        const float corner = app.s.shape.corners ? 0.0f : std::min(fw, fh) / 2 * app.s.corner / 100.0f;
        const auto quad = [&](float in) {  // the corner shape, `in` from its edges across and down
            ComPtr<ID2D1PathGeometry> path;
            app.canvas.factory()->CreatePathGeometry(&path);
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            const float sx = (fw - 2 * in) / fw, sy = (fh - 2 * in) / fh;
            for (int i = 0; i < kFocusCorners; ++i) {
                float u, v;
                focus_corner_point(app.s.shape, static_cast<FocusCorner>(i), u, v);
                const D2D1_POINT_2F p{x + (fx + fw / 2 + (u - 0.5f) * fw * sx) * k, y + (fy + fh / 2 + (v - 0.5f) * fh * sy) * k};
                if (i == 0)
                    sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
                else
                    sink->AddLine(p);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            return path;
        };
        const int steps = band > 0.5f ? 6 : 1;
        for (int i = 0; i < steps; ++i) {
            const float in = band * i / steps, alpha = steps == 1 ? 1.0f : static_cast<float>(i + 1) / steps;
            if (app.s.shape.corners)
                app.canvas.fill(quad(in).Get(), col(app.c->preview_focus, alpha));
            else
                app.canvas.fill(R(fx + in, fy + in, fw - 2 * in, fh - 2 * in), col(app.c->preview_focus, alpha),
                                std::max(0.0f, corner - in) * k);
        }
        // The focus region's finer grid (the kit's focus pattern), on its own grid origin, inside its shape.
        ComPtr<ID2D1Geometry> shape;
        if (app.s.shape.corners) {
            shape = quad(0);
        } else {
            ComPtr<ID2D1RoundedRectangleGeometry> box;
            app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(R(fx, fy, fw, fh).d2d(), corner * k, corner * k), &box);
            shape = box;
        }
        ComPtr<ID2D1Layer> focusLayer;
        rt->CreateLayer(&focusLayer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), focusLayer.Get());
        for (float lx = fx; lx <= fx + fw; lx += fc)
            app.canvas.line(x + lx * k, y + fy * k, x + lx * k, y + (fy + fh) * k, col(app.c->preview_periphery), 0.4f * k);
        for (float ly = fy; ly <= fy + fh; ly += fc)
            app.canvas.line(x + fx * k, y + ly * k, x + (fx + fw) * k, y + ly * k, col(app.c->preview_periphery), 0.4f * k);
        rt->PopLayer();
        if (app.s.outline && app.s.shape.corners)
            app.canvas.stroke(quad(1).Get(), col(overlay(static_cast<int>(app.s.color))), 2 * k);
        else if (app.s.outline)
            app.canvas.stroke(R(fx, fy, fw, fh), col(overlay(static_cast<int>(app.s.color))), 2 * k, corner * k);
        // The cropped edges, as the headset shows them; with the helmet, its rim reaches into the view.
        const bool helmet = app.s.mode == RenderMode::Qvfr && app.s.helmet.on && game_has(&games::GameFeatures::helmet);
        const float rim = helmet ? H * app.s.helmet.rim / 100.0f : 0;
        const Color cropped = col(app.c->preview_canvas, helmet ? 1.0f : 0.9f);
        app.canvas.fill(R(0, 0, W, ct + rim), cropped);
        app.canvas.fill(R(0, H - cb - rim, W, cb + rim), cropped);
        app.canvas.fill(R(0, ct, cl + rim, H - ct - cb), cropped);
        app.canvas.fill(R(W - cr - rim, ct, cr + rim, H - ct - cb), cropped);
        if (eye) {
            const float cx = x + gx * W * k, cy = y + gy * H * k;
            const Color label = col(app.c->preview_label);
            app.canvas.circle(cx, cy, 5 * k, label, false, 1.2f * k);
            app.canvas.line(cx - 9 * k, cy, cx - 5 * k, cy, label, 1.2f * k);
            app.canvas.line(cx + 5 * k, cy, cx + 9 * k, cy, label, 1.2f * k);
            app.canvas.line(cx, cy - 9 * k, cx, cy - 5 * k, label, 1.2f * k);
            app.canvas.line(cx, cy + 5 * k, cx, cy + 9 * k, label, 1.2f * k);
        }
        const t::TypeStyle tiny{9.5f * k, 12 * k, 400, false};
        if (known) {
            app.canvas.text(L"Focus " + dims(ro.focus), tiny, R(fx + 6, fy + 4, W - fx - 6, 12), col(app.c->preview_label), Align::Leading, false);
            app.canvas.text(L"Outer " + dims(ro.outer), tiny, R(cl + 8, H - cb - 17, W - cl - cr - 16, 12), col(app.c->preview_label),
                            Align::Leading, false);
        }
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
    }
    return frameH;
}

// What the foveation preview's two marks mean. Returns its height.
float preview_legend(float x, float y, float w, bool draw) {
    if (draw) {
        const Rect focusKey{x, y + 3, 10, 10};
        app.canvas.fill(focusKey, col(app.c->preview_focus), 2);
        app.canvas.stroke(focusKey, col(app.c->accent), 1, 2);
        const std::wstring focusText = app.s.eyeTracking && !eye_warning() ? L"Focus, follows your eyes" : L"Focus, fixed";
        const float fw = std::ceil(app.canvas.measure(focusText, t::kSubheadline));
        app.canvas.text(focusText, t::kSubheadline, {x + 16, y, fw + 2, 16}, col(app.c->label_secondary));
        const float cx = x + 16 + fw + t::kSpace4;
        app.canvas.fill({cx, y + 3, 10, 10}, col(app.c->preview_canvas), 2);
        app.canvas.stroke({cx, y + 3, 10, 10}, col(app.c->label_muted), 1, 2);
        app.canvas.text(L"Not rendered", t::kSubheadline, {cx + 16, y, std::max(0.0f, x + w - cx - 16), 16}, col(app.c->label_secondary));
    }
    return 16;
}

// A label and its value on one line, the value trailing. Returns the line's height.
float readout(float x, float y, float w, const std::wstring& name, const std::wstring& value, bool draw,
              const std::wstring& of) {
    if (draw) {
        app.canvas.text(name, t::kBody, {x, y, w / 2, 20}, col(app.c->label_secondary));
        const float ow = of.empty() ? 0 : std::ceil(app.canvas.measure(of, t::kBody)) + 2;
        app.canvas.text(value, t::kBody, {x + w / 2, y, w / 2 - ow - (of.empty() ? 0 : 4), 20}, col(app.c->label), Align::Trailing);
        if (!of.empty())
            app.canvas.text(of, t::kBody, {x + w - ow, y, ow, 20}, col(app.c->label_muted), Align::Trailing);
    }
    return 20;
}

namespace {

// A share as a bar under a readout. Returns its height.
float meter(float x, float y, float w, double share, uint32_t color, bool draw) {
    if (draw) {
        app.canvas.fill({x, y, w, 6}, col(app.c->segment_track), 3);
        const float f = static_cast<float>(std::clamp(share, 0.0, 1.0));
        if (f > 0)
            app.canvas.fill({x, y, std::max(6.0f, w * f), 6}, col(color), 3);
    }
    return 6;
}

}  // namespace

// An upper-case group label (the sidebar's, a card's). Returns its height.
float eyebrow(float x, float y, float w, const std::wstring& text, bool draw) {
    if (draw) {
        std::wstring upper = text;
        for (wchar_t& ch : upper)
            ch = static_cast<wchar_t>(towupper(ch));
        app.canvas.text(upper, t::kFootnote, {x, y, w, t::kFootnote.lineHeight}, col(app.c->label_muted));
    }
    return t::kFootnote.lineHeight;
}

// The foveation preview's numbers: each view's size, then the pixels and VRAM against what the headset would draw.
float readouts_card(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool known = app.runtime.known();
    const std::wstring dash = L"—";
    float h = eyebrow(x, y, w, L"Per eye", draw) + t::kSpace2;
    h += readout(x, y + h, w, L"Headset", known ? dims(ro.runtime) : dash, draw) + 6;
    h += readout(x, y + h, w, L"Outer view", known ? dims(ro.outer) : dash, draw) + 6;
    h += readout(x, y + h, w, L"Focus view", known ? dims(ro.focus) : dash, draw) + 6;
    h += readout(x, y + h, w, L"Center sharpness", ro.centerPpd > 0 ? std::to_wstring(std::lround(ro.centerPpd)) + L" PPD" : dash,
                 draw);
    h += t::kSpace3;
    if (draw)
        app.canvas.line(x, y + h, x + w, y + h, col(app.c->separator), 1);
    h += t::kSpace3;
    char mp[24], of[24];
    std::snprintf(mp, sizeof(mp), "%.2f M", ro.pixels / 1e6);
    std::snprintf(of, sizeof(of), "of %.2f M", ro.runtimePixels / 1e6);
    h += readout(x, y + h, w, L"Pixels per frame", known ? widen(mp) : dash, draw, known ? widen(of) : L"") + 6;
    h += meter(x, y + h, w, known && ro.runtimePixels > 0 ? ro.pixels / ro.runtimePixels : 0, app.c->accent_fill, draw) + 6;
    if (known) {
        const int savedPercent = static_cast<int>(std::lround(ro.saved * 100));
        const std::wstring saved = savedPercent >= 0 ? std::to_wstring(savedPercent) + L"% fewer pixels than full resolution"
                                                     : std::to_wstring(-savedPercent) + L"% more pixels than full resolution";
        if (draw)
            app.canvas.text(saved, t::kCallout, {x, y + h, w, 17}, col(savedPercent >= 0 ? app.c->success : app.c->warning));
        h += 17;
    }
    // VRAM, scaled from the last QVFR launch's measurement (settings.h's vram_estimate).
    const VramEstimate vram = vram_estimate(app.s, ro, app.runtime);
    h += t::kSpace4;
    char gb[24], budget[24] = "";
    std::snprintf(gb, sizeof(gb), "%.1f GB", vram.mb / 1024);
    if (vram.budgetMb > 0)
        std::snprintf(budget, sizeof(budget), "of %.1f GB", vram.budgetMb / 1024);
    const bool vramKnown = known && vram.mb > 0;
    h += readout(x, y + h, w, L"VRAM, estimated", vramKnown ? widen(gb) : dash, draw, vramKnown ? widen(budget) : L"") + 6;
    const double share = vram.budgetMb > 0 ? vram.mb / vram.budgetMb : 0;
    h += meter(x, y + h, w, vramKnown ? share : 0,
               share > 0.9 ? app.c->danger : share > 0.75 ? app.c->warning : app.c->accent_fill, draw);
    return h;
}

}  // namespace simqvfr::launcher::ui::win

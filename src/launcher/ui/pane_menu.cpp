// The placement preview (the Headset tab's Placement card: the settings menu and the overlay, dragged where they
// float, with its caption and readouts), and the old pages' preview column.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

#include <windows.h>

#include "../launch_plan.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

namespace {

// Angular size of a panel: its height in degrees for a width in degrees, at any distance.
float panel_height_deg(int widthDeg, float aspect) {
    constexpr float kRad = 3.14159265f / 180;
    return 2 * std::atan(std::tan(widthDeg * kRad / 2) * aspect) / kRad;
}

float menu_height_deg(int widthDeg) { return panel_height_deg(widthDeg, kMenuAspect); }

float osd_height_deg(int widthDeg) { return panel_height_deg(widthDeg, static_cast<float>(kOsdHeight) / kOsdWidth); }

std::wstring degrees(int v, const wchar_t* plus, const wchar_t* minus) {
    return v == 0 ? L"0°" : std::to_wstring(std::abs(v)) + L"° " + (v > 0 ? plus : minus);
}

}  // namespace

// The settings menu's place (Headset menu pane) at (x, y), width w: a kRefFovX x kRefFovY view in
// degrees with the panel, drawn in its own colours, where it floats. Returns its height.
float placement_preview(float x, float y, float w, bool draw) {
    const float k = w / kRefFovX, frameH = std::round(kRefFovY * k);
    const Rect frame{x, y, w, frameH};
    const float pw = app.s.menuWidth * k, ph = menu_height_deg(app.s.menuWidth) * k;
    const float pcx = x + (kRefFovX / 2 + app.s.menuYaw) * k, pcy = y + (kRefFovY / 2 - app.s.menuPitch) * k;
    const Rect panel{pcx - pw / 2, pcy - ph / 2, pw, ph};
    if (draw) {
        app.placeFrame = frame;
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        app.canvas.fill(frame, col(app.c->preview_periphery));
        // A 10° grid, with the horizon and straight ahead stronger.
        for (float d = 10; d < kRefFovX; d += 10)
            app.canvas.line(x + d * k, y, x + d * k, y + frameH, col(app.c->preview_canvas), 0.6f);
        for (float d = 10; d < kRefFovY; d += 10)
            app.canvas.line(x, y + d * k, x + w, y + d * k, col(app.c->preview_canvas), 0.6f);
        const float cx = x + w / 2, cy = y + frameH / 2;
        const Color axis = col(app.c->preview_label, 0.35f);
        app.canvas.line(cx, y, cx, y + frameH, axis, 1);
        app.canvas.line(x, cy, x + w, cy, axis, 1);
        app.canvas.circle(cx, cy, 5, col(app.c->preview_label, 0.8f), false, 1.2f);
        // The panel as the headset shows it: title, resolution line, the settings rows (the first
        // selected) and the hint, in its own colours (in panel pixels, 1024 x 972).
        const bool active = app.dragging == kMenuPanel;
        app.canvas.fill({panel.x, panel.y + 2, panel.w, panel.h}, {0, 0.35f}, 4);
        app.canvas.fill(panel, col(kMenuBackground, 0.92f), 4);
        const float m = pw * 36 / 1024, u = ph / 972;
        const auto bar = [&](float bx, float by, float bw, float bh, Color c) {
            app.canvas.fill({panel.x + bx, panel.y + by * u, bw, std::max(1.5f, bh * u)}, c, 1);
        };
        bar(m, 30, pw * 0.42f, 34, col(kMenuLabel));
        bar(m, 100, pw * 0.36f, 22, col(kMenuDim));
        bar(pw / 2, 100, pw * 0.3f, 22, col(kMenuLabel, 0.8f));
        for (int i = 0; i < 11; ++i) {
            const float ry = 200 + 50.0f * i;
            if (i == 0)
                app.canvas.fill({panel.x + m * 0.55f, panel.y + (ry - 8) * u, pw - 1.1f * m, 50 * u}, col(kMenuHighlight), 1);
            bar(m, ry + 8, pw * (0.22f + 0.04f * (i % 3)), 18, col(kMenuLabel, 0.85f));
            const float vw = pw * 0.12f;
            bar(pw - m - vw, ry + 8, vw, 18, col(kMenuLabel, 0.85f));
        }
        bar(m, 880, pw * 0.6f, 14, col(kMenuDim, 0.7f));
        app.canvas.stroke(panel, col(active ? app.c->accent : app.c->preview_label, active ? 1 : 0.45f), active ? 1.5f : 0.75f, 4);
        if (focused(kMenuPanel))
            focus_ring(panel, 4);
        // The performance overlay, when it is on: four number tiles, the frame-time strip and its cells.
        Rect card{};
        if (app.s.osd.enabled) {
            const float cw = app.s.osd.width * k, ch = osd_height_deg(app.s.osd.width) * k;
            const float ccx = x + (kRefFovX / 2 + app.s.osd.yaw) * k, ccy = y + (kRefFovY / 2 - app.s.osd.pitch) * k;
            card = {ccx - cw / 2, ccy - ch / 2, cw, ch};
            const bool held = app.dragging == kOsdPanel;
            const float r = std::max(2.0f, cw * 32 / kOsdWidth), cu = cw / kOsdWidth, cv = ch / kOsdHeight;
            app.canvas.fill({card.x, card.y + 2, card.w, card.h}, {0, 0.35f}, r);
            app.canvas.fill(card, col(kOsdCard, 0.9f), r);
            const auto block = [&](float bx, float by, float bw, float bh, Color c) {
                app.canvas.fill({card.x + bx * cu, card.y + by * cv, std::max(1.0f, bw * cu), std::max(1.0f, bh * cv)}, c, 1);
            };
            for (int i = 0; i < 4; ++i) {
                const float tx = 36 + i * 238.0f + (i ? 24 : 0);
                block(tx, 30, 90, 14, col(kMenuDim, 0.8f));
                block(tx, 58, 110.0f + 20 * (i % 2), 40, col(kMenuLabel));
                block(tx, 126, 120, 14, col(i == 3 ? kOsdYellow : kOsdGreen, 0.9f));
            }
            block(36, 166, kOsdWidth - 72, 60, col(kOsdTrack));
            block(44, 184, kOsdWidth - 88, 36, col(kOsdGreen, 0.75f));
            for (int i = 0; i < 6; ++i) {
                const float cx2 = 36 + (i % 3) * 317.0f, cy2 = 244 + (i / 3) * 76.0f;
                block(cx2, cy2, 100, 12, col(kMenuDim, 0.8f));
                block(cx2, cy2 + 30, 150, 22, i % 3 == 1 || i == 5 ? col(kOsdGreen, 0.55f) : col(kMenuLabel, 0.85f));
            }
            app.canvas.stroke(card, col(held ? app.c->accent : app.c->preview_label, held ? 1 : 0.45f), held ? 1.5f : 0.75f, r);
            if (focused(kOsdPanel))
                focus_ring(card, r);
        }
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
        app.hits.push_back({kMenuPanel, frame, 0});  // empty space: the panel's centre jumps there
        app.hits.push_back({kMenuPanel, panel, 1});  // the panel: dragged from where it is held
        if (app.s.osd.enabled)
            app.hits.push_back({kOsdPanel, card, 1});  // the overlay, over the menu
    }
    return frameH;
}

// How to use the placement preview. Returns its height.
float placement_caption(float x, float y, float w, bool draw) {
    const std::wstring caption = app.s.osd.enabled ? L"Drag the menu or the overlay to place it. Headsets’ fields of view vary."
                                                   : L"Drag the panel to place it. Headsets’ fields of view vary.";
    const float ch = app.canvas.measure_height(caption, t::kSubheadline, w);
    if (draw)
        app.canvas.text(caption, t::kSubheadline, {x, y, w, ch + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    return ch;
}

// The settings menu's place and size, in numbers.
float placement_readouts(float x, float y, float w, bool draw) {
    char size[48], metres[48], distance[16];
    std::snprintf(size, sizeof(size), "%d° × %d°", app.s.menuWidth, static_cast<int>(std::lround(menu_height_deg(app.s.menuWidth))));
    std::snprintf(metres, sizeof(metres), "%.2f m", 2 * kMenuDistance * std::tan(app.s.menuWidth * 3.14159265 / 360));
    std::snprintf(distance, sizeof(distance), "%.1f m", kMenuDistance);
    float h = eyebrow(x, y, w, L"Settings menu", draw) + t::kSpace2;
    h += readout(x, y + h, w, L"Horizontal", degrees(app.s.menuYaw, L"right", L"left"), draw) + 6;
    h += readout(x, y + h, w, L"Vertical", degrees(app.s.menuPitch, L"up", L"down"), draw) + 6;
    h += readout(x, y + h, w, L"Panel size", widen(size), draw) + 6;
    h += readout(x, y + h, w, L"Panel width", widen(metres), draw) + 6;
    h += readout(x, y + h, w, L"Distance", widen(distance), draw);
    return h;
}

namespace {

using Content = std::function<float(float, float, float, bool)>;

// A card in the preview column: a title (and a pill at its end) over `content`. Returns its height.
float preview_card(float x, float y, float w, const std::wstring& title, const std::wstring& pill, const Content& content,
                   bool draw) {
    const float pad = t::kSpace4, head = title.empty() ? 0 : 20 + t::kSpace3, radius = t::kRadiusGroup;
    const float ch = content(x + pad, y + pad + head, w - 2 * pad, false);
    const float h = pad + head + ch + pad;
    if (draw) {
        app.canvas.fill({x, y, w, h}, col(app.c->group_bg), radius);  // flat, as the panes' cards
        if (!title.empty()) {
            app.canvas.text(title, t::kHeadline, {x + pad, y + pad, w - 2 * pad, 20}, col(app.c->label));
            if (!pill.empty()) {
                const float pw = std::ceil(app.canvas.measure(pill, t::kSubheadline)) + 18;
                const Rect pr{x + w - pad - pw, y + pad, pw, 20};
                app.canvas.fill(pr, col(app.c->control_bg), t::kRadiusControl);
                app.canvas.text(pill, t::kSubheadline, pr, col(app.c->label_secondary), Align::Center);
            }
        }
        content(x + pad, y + pad + head, w - 2 * pad, true);
    }
    return h;
}

// `frame` drawn in the preview's dark well, then `after` under it.
Content in_well(Content frame, Content after = nullptr) {
    return [frame, after](float x, float y, float w, bool draw) {
        const float pad = t::kSpace3, fh = frame(x + pad, y + pad, w - 2 * pad, false);
        if (draw) {
            app.canvas.fill({x, y, w, fh + 2 * pad}, col(app.c->preview_canvas), t::kRadiusGroup);
            frame(x + pad, y + pad, w - 2 * pad, true);
        }
        float h = fh + 2 * pad;
        if (after)
            h += t::kSpace3 + after(x, y + h + t::kSpace3, w, draw);
        return h;
    };
}

}  // namespace

// The old pages' preview column: the foveation preview, a card, with a second card of numbers under it.
float side_preview(float x, float y, float w, bool draw) {
    float h = 0;
    h += preview_card(x, y, w, L"Preview", L"Left eye", in_well(preview, preview_legend), draw);
    h += t::kSpace4;
    return h + preview_card(x, y + h, w, L"", L"", readouts_card, draw);
}

}  // namespace simqvfr::launcher::ui::win

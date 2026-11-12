// Panels: a modal card over the dimmed window, with its title and a close button, for what doesn't fit a card: Report an
// issue (the Get help card opens it), a game's own colour (Custom… on the Games card), Manage profiles (the profile
// menu) and About (the version in the rail). Escape or the close button closes it; its content scrolls when it is
// taller than the window.
#include <algorithm>
#include <cmath>
#include <string>

#include <windows.h>

#include "../../games/game.h"
#include "controls.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

std::wstring panel_title(PanelKind kind) {
    switch (kind) {
    case PanelKind::Report: return L"Report an issue";
    case PanelKind::Color: return L"Colour for " + std::wstring(color_game().name);
    case PanelKind::Profiles: return L"Manage profiles";
    case PanelKind::About: return L"About";
    default: return L"";
    }
}

float panel_content(PanelKind kind, float x, float y, float w, bool draw) {
    switch (kind) {
    case PanelKind::Report: return report_pane(x, y, w, draw);
    case PanelKind::Color: return color_panel(x, y, w, draw);
    case PanelKind::Profiles: return profiles_pane(x, y, w, draw);
    case PanelKind::About: return about_pane(x, y, w, draw);
    default: return 0;
    }
}

namespace {

// A panel's width: the forms' (Report, Manage profiles), About's text, the colour wheel's.
float panel_width(PanelKind kind) {
    switch (kind) {
    case PanelKind::Color: return 440;
    case PanelKind::About: return 560;
    default: return 620;
    }
}

}  // namespace

void open_panel(PanelKind kind) {
    app.popup.open = false;
    close_search();
    app.panel = kind;
    app.panelScroll = 0;
    if (kind == PanelKind::Report)
        reload_runtime();  // the causes and the report from the latest session
    if (kind == PanelKind::Color)
        load_color_edit();
    if (kind == PanelKind::Profiles) {
        app.profileMessage.clear();
        refresh_profiles();
    }
    app.focus = kPanelClose;
    app.focusOption = -1;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void close_panel() {
    if (app.panel == PanelKind::None)
        return;
    app.panel = PanelKind::None;
    app.panelBody = {};
    if (app.reportEdit && GetFocus() == app.reportEdit)
        SetFocus(app.hwnd);
    app.focus = kLaunch;
    app.focusOption = -1;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The open panel, over everything but a sheet and the pop-up menu: the window dimmed, its card in the middle (the
// window's background behind its sections), its header, its content clipped and scrolled. Only it answers.
void panel_overlay() {
    if (app.panel == PanelKind::None)
        return;
    Canvas& cv = app.canvas;
    const float W = cv.width(), H = cv.height();
    cv.fill({0, 0, W, H}, {0, app.dark ? 0.55f : 0.32f});
    app.hits.clear();
    app.drawnLabels.clear();
    const float w = std::min(panel_width(app.panel), W - 32), pad = t::kSpace6, headerH = 60;
    const float inner = w - 2 * pad;
    const float contentH = panel_content(app.panel, 0, 0, inner, false);
    const float h = std::min(headerH + contentH + pad, H - 48);
    const Rect card{std::round((W - w) / 2), std::round(std::max(24.0f, (H - h) / 2)), w, h};
    cv.fill(card, col(app.panel == PanelKind::Color ? app.c->surface : app.c->bg), t::kRadiusGroup);  // forms: their sections on it
    cv.stroke(card, col(app.c->field_border), 1, t::kRadiusGroup);
    cv.text(panel_title(app.panel), t::kTitle3, {card.x + pad, card.y + 20, inner - 40, t::kTitle3.lineHeight}, col(app.c->label));
    // Its close button: an ✕.
    const Rect close{card.right() - pad - 28, card.y + 16, 28, 28};
    if (pointer_in(close))
        cv.fill(close, col(app.c->control_hover), t::kRadiusControl);
    cv.path("M4 4L12 12M12 4L4 12", {close.x + 6, close.y + 6, 16, 16}, 16, col(app.c->label), 1.6f);
    if (focused(kPanelClose))
        focus_ring(close, t::kRadiusControl);
    app.hits.push_back({kPanelClose, close, -1, L"Close (Esc)"});
    // The content, scrolled.
    const Rect body{card.x + pad, card.y + headerH, inner, h - headerH - pad};
    app.panelBody = body;
    app.panelScrollMax = std::max(0.0f, contentH - body.h);
    app.panelScroll = std::clamp(app.panelScroll, 0.0f, app.panelScrollMax);
    cv.push_clip(body);
    panel_content(app.panel, body.x, body.y - app.panelScroll, body.w, true);
    cv.pop_clip();
}

}  // namespace simqvfr::launcher::ui::win

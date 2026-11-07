// The kit's controls (controls.h).
#include <algorithm>
#include <cmath>
#include <string>

#include <windows.h>

#include "../settings.h"
#include "controls.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

// ---- Controls ----------------------------------------------------------------------------------

void focus_ring(const Rect& r, float radius) {
    // A 2px ring 3px outside the control (:focus-visible).
    app.canvas.stroke({r.x - 5, r.y - 5, r.w + 10, r.h + 10}, col(app.c->focus_ring), 2, radius + 5);
}

bool focused(Ctl id) { return app.keyboard && app.focus == id && !app.popup.open; }

// The pointer is over `r` (hover states), with no menu open and nothing dragged.
bool pointer_in(const Rect& r) {
    if (app.sheet.open && !app.drawingSheet)
        return false;  // under the sheet
    return app.hover != kNone && !app.popup.open && app.dragging == kNone && r.contains(app.mouseX, app.mouseY);
}

namespace {

// `a` moved toward `b` by `f` (0..1), per channel.
uint32_t mix(uint32_t a, uint32_t b, float f) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xff), cb = static_cast<float>((b >> shift) & 0xff);
        out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * f)) << shift;
    }
    return out;
}

}  // namespace

float badge_width(const std::wstring& text) { return 7 + 12 + 5 + std::ceil(app.canvas.measure(text, kBadgeStyle)) + 10; }

float button_width(const std::wstring& label, const t::TypeStyle& style) {
    return std::ceil(app.canvas.measure(label, style)) + 2 * t::kSpace4;
}

// A button; `icon`: a 16-box stroke path drawn before the label.
void button(Ctl id, const Rect& r, const std::wstring& label, Btn style, const char* icon) {
    const bool down = app.pressed == id && r.contains(app.mouseX, app.mouseY), over = pointer_in(r);
    const float radius = t::kRadiusControl;
    Color bg{0, 0}, fg = col(app.c->label);
    switch (style) {
    case Btn::Primary:
        bg = col(down ? mix(app.c->accent_fill, 0, 0.12f) : over ? mix(app.c->accent_fill, 0xffffff, 0.1f) : app.c->accent_fill);
        fg = col(app.c->on_accent);
        break;
    case Btn::Secondary: bg = col(down ? mix(app.c->control_hover, 0, 0.08f) : over ? app.c->control_hover : app.c->control_bg); break;
    case Btn::Ghost:
        bg = over || down ? col(app.dark ? 0xffffff : 0, down ? 0.1f : 0.06f) : Color{0, 0};
        fg = col(app.c->heading);
        break;
    case Btn::Tint:
        bg = col(app.c->accent_fill, down ? 0.3f : over ? 0.24f : 0.16f);
        fg = col(app.c->accent);
        break;
    case Btn::Support:
        bg = col(down ? mix(app.c->support_fill, 0, 0.12f) : over ? mix(app.c->support_fill, 0xffffff, 0.1f) : app.c->support_fill);
        fg = col(app.c->on_accent);
        break;
    case Btn::Danger:
        bg = over || down ? col(app.c->danger, down ? 0.18f : 0.12f) : Color{0, 0};
        fg = col(app.c->danger);
        break;
    }
    if (bg.alpha > 0)
        app.canvas.fill(r, bg, radius);
    const t::TypeStyle textStyle = style == Btn::Primary && r.h >= t::kControlHeightLarge ? kPrimaryLabel : kButtonLabel;
    if (icon) {
        const float tw = std::ceil(app.canvas.measure(label, textStyle)), iw = 16, gap = 8;
        const float x0 = r.x + (r.w - tw - iw - gap) / 2;
        app.canvas.path(icon, {x0, r.y + (r.h - iw) / 2, iw, iw}, 16, fg, 1.6f, icon == kIconPlay || icon == kIconHeart);
        app.canvas.text(label, textStyle, {x0 + iw + gap, r.y, tw + 2, r.h}, fg);
    } else {
        app.canvas.text(label, textStyle, r, fg, Align::Center);
    }
    if (focused(id))
        focus_ring(r, radius);
    app.hits.push_back({id, r});
}

void button(Ctl id, const Rect& r, const std::wstring& label, bool primary) {
    button(id, r, label, primary ? Btn::Primary : Btn::Secondary);
}

float popup_width(const std::wstring* items, int count, float minWidth) {
    float w = 0;
    for (int i = 0; i < count; ++i)
        w = std::max(w, app.canvas.measure(items[i], t::kBody));
    return std::max(minWidth, std::ceil(w) + t::kSpace3 + 32);
}

void popup_button(Ctl id, const Rect& r, const std::wstring& value, bool disabled) {
    const bool open = app.popup.open && app.popup.owner == id;
    app.canvas.fill(r, col(!disabled && (open || pointer_in(r)) ? app.c->control_hover : app.c->control_bg, disabled ? 0.5f : 1),
                    t::kRadiusControl);
    if (!app.dark)
        app.canvas.stroke(r, col(app.c->control_border, disabled ? 0.5f : 1), 1, t::kRadiusControl);
    app.canvas.text(value, t::kBody, {r.x + t::kSpace3, r.y, r.w - t::kSpace3 - 30, r.h},
                    col(disabled ? app.c->label_tertiary : app.c->label));
    // Chevron 8 x 12, 12px from the trailing edge.
    app.canvas.path("M1.5 4.25L4 1.75l2.5 2.5M1.5 7.75L4 10.25l2.5-2.5", {r.right() - 20, r.y + (r.h - 12) / 2, 8, 12},
                    8, col(disabled ? app.c->label_tertiary : app.c->label_secondary), 1.5f);
    if (focused(id))
        focus_ring(r, t::kRadiusControl);
    app.hits.push_back({id, r});
}

namespace {

// The value column of a slider: the value, and a detail line under it (a pixel size).
constexpr float kSliderValueW = 112;

}  // namespace

// The kit's PercentSlider: track, value readout (in `unit`) and an optional detail line under the value. Height 20
// (36 with detail). `tenths`: the value is in tenths, shown with one decimal ("1.5°").
void slider(Ctl id, const Rect& r, int value, Range range, const std::wstring& detail, bool disabled,
            const wchar_t* unit, bool tenths) {
    const float trackW = r.w - kSliderValueW - t::kSpace5;
    const float cy = r.y + (detail.empty() ? 10.0f : 18.0f);
    const Rect track{r.x, cy - 10, trackW, 20};
    const float knob = 18, travel = track.w - knob;
    const float f = range.max > range.min ? static_cast<float>(value - range.min) / (range.max - range.min) : 0;
    const float kx = track.x + travel * f;
    const Color fill = col(disabled ? app.c->label_tertiary : app.c->accent_fill);
    app.canvas.fill({track.x, cy - 2, track.w, 4}, col(app.c->segment_track), 2);
    // Filled from the range's start, or from 0 when it spans both signs (degrees left and right).
    const float zx = range.min < 0 && range.max > 0 ? track.x + travel * (0.0f - range.min) / (range.max - range.min) : track.x;
    const float kc = kx + knob / 2;
    app.canvas.fill({std::min(zx, kc), cy - 2, std::abs(kc - zx), 4}, fill, 2);
    const Rect k{kx, cy - knob / 2, knob, knob};
    if (!disabled)
        app.canvas.fill({k.x, k.y + 1, k.w, k.h}, {0, app.dark ? 0.5f : 0.2f}, knob / 2);  // its drop
    app.canvas.fill(k, col(disabled ? app.c->label_tertiary : app.c->knob), knob / 2);
    if (!app.dark && !disabled)
        app.canvas.stroke(k, col(app.c->control_border), 1, knob / 2);
    if (focused(id))
        app.canvas.circle(k.x + knob / 2, k.y + knob / 2, knob / 2 + 4, col(app.c->focus_ring), false, 2);
    const float vx = track.right() + t::kSpace5;
    const std::wstring text = (tenths ? gaze_deadzone_text(value) : std::to_wstring(value)) + unit;
    if (detail.empty()) {
        app.canvas.text(text, t::kValue, {vx, cy - 10, kSliderValueW, 20}, col(disabled ? app.c->label_tertiary : app.c->label),
                        Align::Trailing);
    } else {
        app.canvas.text(text, t::kValue, {vx, r.y, kSliderValueW, 18}, col(disabled ? app.c->label_tertiary : app.c->label),
                        Align::Trailing);
        app.canvas.text(detail, t::kSubheadline, {vx - 40, r.y + 19, kSliderValueW + 40, 16},
                        col(disabled ? app.c->label_tertiary : app.c->label_muted), Align::Trailing);
    }
    if (!disabled)
        app.hits.push_back({id, track});
}

float checkbox_height(const std::wstring& desc, float width) {
    const float text = t::kBody.lineHeight +
                       (desc.empty() ? 0 : 2 + app.canvas.measure_height(desc, t::kCallout, width - kSwitchW - t::kSpace5));
    return std::max(text, kSwitchH);
}

// The switch itself (44 x 26): the accent when on, with its knob at the trailing end.
void switch_track(const Rect& s, bool on, bool disabled) {
    const float alpha = disabled ? 0.4f : 1;
    app.canvas.fill(s, col(on ? app.c->accent_fill : app.c->switch_off, alpha), s.h / 2);
    const float knob = s.h - 6, kx = on ? s.right() - 3 - knob : s.x + 3;
    app.canvas.fill({kx, s.y + 4, knob, knob}, {0, disabled ? 0.0f : 0.3f}, knob / 2);
    app.canvas.fill({kx, s.y + 3, knob, knob}, col(0xffffff, disabled ? 0.6f : 1), knob / 2);
}

// A setting that is on or off: its label (and description) leading, a switch trailing; the whole row toggles it.
void checkbox(Ctl id, const Rect& r, bool checked, const std::wstring& label, const std::wstring& desc,
              bool disabled) {
    const float tw = r.w - kSwitchW - t::kSpace5;
    const float descH = desc.empty() ? 0 : app.canvas.measure_height(desc, t::kCallout, tw);
    const float textH = t::kBody.lineHeight + (desc.empty() ? 0 : 2 + descH);
    const float ty = r.y + (r.h - textH) / 2;
    app.canvas.text(label, t::kBody, {r.x, ty, tw, t::kBody.lineHeight}, col(disabled ? app.c->label_tertiary : app.c->label));
    if (!desc.empty())
        app.canvas.text(desc, t::kCallout, {r.x, ty + t::kBody.lineHeight + 2, tw, descH + 2},
                        col(disabled ? app.c->label_tertiary : app.c->label_secondary), Align::Leading, false, true);
    const Rect s{r.right() - kSwitchW, r.y + (r.h - kSwitchH) / 2, kSwitchW, kSwitchH};
    switch_track(s, checked, disabled);
    if (disabled)
        return;  // no hit: it does nothing
    if (focused(id))
        focus_ring(s, kSwitchH / 2);
    app.hits.push_back({id, r});
    help_icon(label, r.x + std::min(tw - 14, std::ceil(app.canvas.measure(label, t::kBody)) + 6), ty);
}

float segmented_width() {
    float w = 4;
    for (const auto& name : kColorNames)
        w += t::kSpace3 * 2 + 10 + 6 + app.canvas.measure(name, t::kBody) + 2;
    return std::ceil(w - 2);
}

uint32_t overlay(int i) { return i == 0 ? app.c->overlay_teal : i == 1 ? app.c->overlay_magenta : app.c->overlay_green; }

void segmented(Ctl id, const Rect& r, int selected, bool disabled) {
    app.canvas.fill(r, col(app.c->segment_track), t::kRadiusControl);
    float x = r.x + 2;
    for (int i = 0; i < 3; ++i) {
        const float w = t::kSpace3 * 2 + 10 + 6 + app.canvas.measure(kColorNames[i], t::kBody);
        const Rect item{x, r.y + 2, w, r.h - 4};
        if (i == selected) {
            if (!disabled)
                app.canvas.fill({item.x, item.y + 1, item.w, item.h}, {0, app.dark ? 0.35f : 0.12f}, t::kRadiusControl - 2);
            app.canvas.fill(item, col(app.c->segment_selected, disabled ? 0.35f : 1), t::kRadiusControl - 2);
        } else if (!disabled && pointer_in(item)) {
            app.canvas.fill(item, col(app.c->segment_selected, 0.35f), t::kRadiusControl - 2);
        }
        app.canvas.circle(item.x + t::kSpace3 + 5, item.y + item.h / 2, 5, col(overlay(i), disabled ? 0.4f : 1), true);
        app.canvas.text(kColorNames[i], t::kBody, {item.x + t::kSpace3 + 16, item.y, w - t::kSpace3 - 16, item.h},
                        col(disabled ? app.c->label_tertiary : app.c->label));
        if (i == selected && focused(id))
            focus_ring(item, t::kRadiusControl - 2);
        if (!disabled)
            app.hits.push_back({id, item, i});
        x += w + 2;
    }
}

namespace {

void glyph(int tone, float x, float y, Color c) {
    const Rect r{x, y, 12, 12};
    if (tone == 0) {  // success: circle + check
        app.canvas.path("M3.75 6.1l1.5 1.5 3-3.2", r, 12, c, 1.3f);
        app.canvas.circle(x + 6, y + 6, 5, c, false, 1.3f);
    } else if (tone == 1) {  // warning: triangle + mark
        app.canvas.path("M6 1.5l5 9H1zM6 5v2.2", r, 12, c, 1.3f);
        app.canvas.circle(x + 6, y + 8.8f, 0.6f, c, true);
    } else if (tone == 2) {  // danger: circle + x
        app.canvas.circle(x + 6, y + 6, 5, c, false, 1.3f);
        app.canvas.path("M4.25 4.25l3.5 3.5M7.75 4.25l-3.5 3.5", r, 12, c, 1.3f);
    } else {
        app.canvas.circle(x + 6, y + 6, 2.5f, c, true);
    }
}

}  // namespace

// StatusBadge: tone 0 success, 1 warning, 2 danger, 3 neutral. Returns its width.
float badge(float x, float y, int tone, const std::wstring& text, float maxWidth) {
    const float w = std::min(maxWidth, badge_width(text));
    const uint32_t bg[] = {app.c->success_bg, app.c->warning_bg, app.c->danger_bg, app.c->segment_track};
    const uint32_t fg[] = {app.c->success, app.c->warning, app.c->danger, app.c->label};
    app.canvas.fill({x, y - 1, w, 22}, col(bg[tone]), 11);
    glyph(tone, x + 8, y + 4, col(fg[tone]));
    app.canvas.text(text, kBadgeStyle, {x + 25, y - 1, w - 25 - 9, 22}, col(fg[tone]));
    return w;
}

}  // namespace simqvfr::launcher::ui::win

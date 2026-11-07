// A pane's cards and their rows (form.h).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>

#include "../settings.h"
#include "form.h"
#include "window_state.h"
#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace {

// Inside a card: rows are padded 18 across and 14 down.
constexpr float kRowPadX = 18, kRowPadY = 14;

// The width of a row's label column: the space the control leaves, or (wide rows) a column of its own.
float label_width(const Row& row, float inner) {
    return row.wide ? std::clamp(inner * 0.36f, 140.0f, 230.0f) : inner - row.controlW - t::kSpace5;
}

float row_height(const Row& row, float w) {
    const float inner = w - 2 * kRowPadX;
    if (row.full)
        return std::max(t::kRowMinHeight, row.fullHeight(inner) + 2 * kRowPadY);
    const float textW = label_width(row, inner);
    float textH = t::kBody.lineHeight;
    if (!row.desc.empty())
        textH += 2 + app.canvas.measure_height(row.desc, t::kCallout, textW);
    return std::max(t::kRowMinHeight, std::max(textH, row.controlH) + 2 * kRowPadY);
}

// The tag after a section's title: "Live" or "Live when lowered" (green), or "Next launch" (grey), 18 high; under the
// pointer, what it means (its help, as an ⓘ's).
void applies_tag(float x, float y, Applies applies) {
    const bool live = applies != Applies::NextLaunch;
    const std::wstring text = applies == Applies::Live ? L"Live" : applies == Applies::LiveLowered ? L"Live when lowered" : L"Next launch";
    const Rect r{x, y, std::ceil(app.canvas.measure(text, kBadgeStyle)) + 16, 18};
    app.canvas.fill(r, col(live ? app.c->success_bg : app.c->segment_track), 9);
    app.canvas.text(text, kBadgeStyle, r, col(live ? app.c->success : app.c->label_secondary), Align::Center);
    int index = 0;
    while (text != kHelp[index].label)
        ++index;
    app.hits.push_back({kHelpIcon, r, index});
    if (app.tip == index)
        app.tipAnchor = r;
}

}  // namespace

// FormSection at (x, y), width w: its heading (and when its settings apply), a card of rows, a footer under it.
// Returns its height.
float section(float x, float y, float w, const std::wstring& title, const std::vector<Row>& rows, const std::wstring& footer,
              bool draw, Applies applies) {
    float h = 0;
    if (!title.empty()) {
        if (draw) {
            app.canvas.text(title, t::kHeadline, {x + 2, y, w - 2, t::kHeadline.lineHeight}, col(app.c->heading));
            // Then its ⓘ (the section's reference text, kHelp by its title) and when its settings apply.
            float tx = x + 2 + std::ceil(app.canvas.measure(title, t::kHeadline)) + 6;
            if (help_for(title)) {
                help_icon(title, tx, y + (t::kHeadline.lineHeight - t::kBody.lineHeight) / 2);
                tx += 14 + 4;
            }
            if (applies != Applies::None)
                applies_tag(tx + 2, y + (t::kHeadline.lineHeight - 18) / 2, applies);
        }
        h += t::kHeadline.lineHeight + t::kSpace2;
    }
    float boxH = 0;
    for (const Row& row : rows)
        boxH += row_height(row, w);
    const Rect box{x, y + h, w, boxH};
    if (draw) {
        app.canvas.fill(box, col(app.c->group_bg), t::kRadiusGroup);
        app.canvas.stroke(box, col(app.c->card_border), 1, t::kRadiusGroup);
        float ry = box.y;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& row = rows[i];
            const float rh = row_height(row, w);
            if (i > 0)
                app.canvas.line(box.x + kRowPadX, ry, box.right() - 1, ry, col(app.c->separator), 1);
            const Rect inner{box.x + kRowPadX, ry + kRowPadY, w - 2 * kRowPadX, rh - 2 * kRowPadY};
            if (row.full) {
                const float fh = row.fullHeight(inner.w);
                row.full({inner.x, inner.y + (inner.h - fh) / 2, inner.w, fh});
            } else {
                const float textW = label_width(row, inner.w);
                float textH = t::kBody.lineHeight;
                const float descH = row.desc.empty() ? 0 : app.canvas.measure_height(row.desc, t::kCallout, textW);
                if (!row.desc.empty())
                    textH += 2 + descH;
                const float ty = inner.y + (inner.h - textH) / 2;
                const Color labelColor = col(row.disabled ? app.c->label_tertiary : app.c->label);
                app.canvas.text(row.label, t::kBody, {inner.x, ty, textW, t::kBody.lineHeight}, labelColor);
                help_icon(row.label, inner.x + std::min(textW - 13, std::ceil(app.canvas.measure(row.label, t::kBody)) + 6), ty);
                if (!row.desc.empty())
                    app.canvas.text(row.desc, t::kCallout, {inner.x, ty + t::kBody.lineHeight + 2, textW, descH + 2},
                                    col(row.disabled ? app.c->label_tertiary : app.c->label_secondary), Align::Leading, false, true);
                if (row.control && row.wide) {
                    const float cx = inner.x + textW + t::kSpace5;
                    row.control({cx, inner.y + (inner.h - row.controlH) / 2, inner.right() - cx, row.controlH});
                } else if (row.control) {  // a row of text alone has none
                    row.control({inner.right() - row.controlW, inner.y + (inner.h - row.controlH) / 2, row.controlW, row.controlH});
                }
            }
            ry += rh;
        }
    }
    h += boxH;
    if (!footer.empty()) {
        const float fw = w - 4, fh = app.canvas.measure_height(footer, t::kSubheadline, fw);
        if (draw)
            app.canvas.text(footer, t::kSubheadline, {x + 2, y + h + t::kSpace2, fw, fh + 2}, col(app.c->label_muted),
                            Align::Leading, false, true);
        h += t::kSpace2 + fh;
    }
    return h;
}

std::wstring px(Size s) { return std::to_wstring(s.width) + L" × " + std::to_wstring(s.height) + L" px"; }

std::wstring dims(Size s) { return std::to_wstring(s.width) + L" × " + std::to_wstring(s.height); }

// A row with a popup button: `names[selected]` of `count` choices.
Row choice_row(Ctl id, const std::wstring& label, const std::wstring* names, int count, int selected, bool disabled) {
    Row row;
    row.label = label;
    row.disabled = disabled;
    row.controlW = popup_width(names, count, 140);
    row.controlH = t::kControlHeight;
    const std::wstring value = names[selected];
    row.control = [id, value, disabled](const Rect& r) { popup_button(id, r, value, disabled); };
    return row;
}

Row slider_row(Ctl id, const std::wstring& label, const std::wstring& desc, int value, const std::wstring& detail,
               bool disabled, const wchar_t* unit, bool tenths) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.wide = true;
    row.controlH = detail.empty() ? 20.0f : 36.0f;
    row.disabled = disabled;
    const Range range = slider_range(id);
    row.control = [=](const Rect& r) { slider(id, r, value, range, detail, disabled, unit, tenths); };
    return row;
}

// A layer nothing here can change: a badge saying so.
Row locked_row(const std::wstring& label, const std::wstring& desc, const std::wstring& text) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = badge_width(text);
    row.controlH = 20;
    row.control = [text](const Rect& r) { badge(r.x, r.y, 3, text); };
    return row;
}

// The current pane's content at (x, y), width w. Returns its height.
// ---- Diagnostics -------------------------------------------------------------------------------
// The last session's measurements (runtime.ini's session_* keys, written by the layer every ~10 s while ACE
// runs), for users to read and to screenshot or copy when asking for help; the CPU capture switch.

// A read-only row: its label (and a line under it) with the value on the right.
Row value_row(const std::wstring& label, const std::wstring& desc, const std::wstring& value) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = std::ceil(app.canvas.measure(value, t::kValue)) + 2;
    row.controlH = 16;
    row.control = [value](const Rect& r) { app.canvas.text(value, t::kValue, r, col(app.c->label), Align::Trailing); };
    return row;
}

// `format` with one number, at whatever length it comes to (a fixed buffer too short for a description made
// swprintf_s end the launcher: the Diagnostics pane's first version).
std::wstring formatted(const wchar_t* format, double v) {
    const int n = _scwprintf(format, v);
    if (n <= 0)
        return {};
    std::wstring text(static_cast<size_t>(n), L'\0');
    swprintf_s(text.data(), static_cast<size_t>(n) + 1, format, v);
    return text;
}

}  // namespace simqvfr::launcher::ui::win

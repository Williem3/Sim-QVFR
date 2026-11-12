// Cards (card.h): their rows, their header and menu, the grid, the disclosures' reveal and Reset this card.
#include "card.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>

#include "controls.h"

namespace simqvfr::launcher::ui::win {

namespace {

constexpr float kRowGap = 14;      // between rows
constexpr float kLineH = 20;       // a label's line
constexpr float kTrackGap = 6;     // label line to slider track
constexpr float kChoiceH = 32;     // a segmented control or pop-up button
constexpr float kHalfGap = 20;     // between two sliders side by side
constexpr float kHeaderH = 50;     // a card's header, to its rule
constexpr float kPad = 16;         // a card's padding

std::wstring value_text(const SliderSpec& s) {
    return (s.tenths ? gaze_deadzone_text(s.value) : std::to_wstring(s.value)) + s.unit;
}

// A slider's label line and track in `r` (its width), at y.
void draw_slider(const SliderSpec& s, float x, float y, float w) {
    Canvas& cv = app.canvas;
    const std::wstring value = value_text(s);
    const float vw = std::ceil(cv.measure(value, t::kValue)) + 2;
    const float dw = s.detail.empty() ? 0 : std::ceil(cv.measure(s.detail, t::kCallout)) + 8;
    const float lw = std::max(0.0f, w - vw - dw - 8);
    cv.text(s.label, t::kBody, {x, y, lw, kLineH}, col(app.c->label));
    help_icon(s.label, x + std::min(lw - 14, std::ceil(cv.measure(s.label, t::kBody)) + 6), y);
    if (dw > 0)
        cv.text(s.detail, t::kCallout, {x + w - vw - dw, y, dw - 6, kLineH}, col(app.c->label_muted), Align::Trailing);
    cv.text(value, t::kValue, {x + w - vw, y, vw, kLineH}, col(app.c->label), Align::Trailing);
    slider_track(s.id, {x, y + kLineH + kTrackGap, w, 20}, s.value, slider_range(s.id), false);
}

// The ease of a reveal at `t` (0..1): fast at first, settling.
float ease(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

bool animations_on() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

// How far `parent`'s rows have slid open (1: all the way, or not animating).
float reveal_progress(Ctl parent) {
    const auto it = app.revealStart.find(parent);
    if (it == app.revealStart.end())
        return 1;
    const float t = static_cast<float>(GetTickCount64() - it->second) / static_cast<float>(kRevealMs);
    if (t >= 1) {
        app.revealStart.erase(it);
        return 1;
    }
    return ease(std::max(0.0f, t));
}

}  // namespace

void start_reveal(Ctl parent) {
    if (!animations_on())  // Windows' "Show animations" off: the rows appear at once
        return;
    app.revealStart[parent] = GetTickCount64();
    SetTimer(app.hwnd, kRevealTimer, 16, nullptr);
}

bool revealing() {
    const unsigned long long now = GetTickCount64();
    return std::any_of(app.revealStart.begin(), app.revealStart.end(),
                       [now](const auto& r) { return now - r.second < kRevealMs; });
}

// ---- Body --------------------------------------------------------------------------------------

void Body::gap() {
    if (!first_)
        h_ += kRowGap;
    first_ = false;
}

void Body::record(const std::wstring& label, const Rect& r) {
    if (draw_ && !label.empty())
        app.drawnLabels.push_back({label, app.drawingSection, r});
}

void Body::slider(const SliderSpec& s) {
    gap();
    const float y = y_ + h_, h = kLineH + kTrackGap + 20;
    if (draw_) {
        draw_slider(s, x_, y, w_);
        record(s.label, {x_, y, w_, h});
    }
    h_ += h;
}

void Body::sliders(const SliderSpec& a, const SliderSpec& b) {
    gap();
    const float y = y_ + h_, h = kLineH + kTrackGap + 20, cw = (w_ - kHalfGap) / 2;
    if (draw_) {
        draw_slider(a, x_, y, cw);
        draw_slider(b, x_ + cw + kHalfGap, y, cw);
        record(a.label, {x_, y, cw, h});
        record(b.label, {x_ + cw + kHalfGap, y, cw, h});
    }
    h_ += h;
}

void Body::toggle(Ctl id, const std::wstring& label, bool on, const std::wstring& help, bool disabled, Applies applies) {
    gap();
    const float rh = checkbox_height(help, w_), y = y_ + h_;
    if (draw_) {
        checkbox(id, {x_, y, w_, rh}, on, label, help, disabled);
        if (applies != Applies::None) {  // its tag after the label (and its ⓘ)
            const float lw = std::ceil(app.canvas.measure(label, t::kBody)) + (help_for(label) ? 22.0f : 8.0f);
            applies_tag(x_ + lw, y + (kLineH - 18) / 2, applies);
        }
    }
    h_ += rh;
}

void Body::segments(Ctl id, const std::wstring& label, const std::vector<std::wstring>& items, int selected) {
    gap();
    const float cw = segmented_choice_width(items), lw = std::ceil(app.canvas.measure(label, t::kBody)) + 26;
    const bool oneLine = lw + cw <= w_;
    const float y = y_ + h_, h = oneLine ? kChoiceH : kLineH + 6 + kChoiceH;
    if (draw_) {
        const float ly = oneLine ? y + (kChoiceH - kLineH) / 2 : y;
        app.canvas.text(label, t::kBody, {x_, ly, oneLine ? w_ - cw : w_, kLineH}, col(app.c->label));
        help_icon(label, x_ + std::ceil(app.canvas.measure(label, t::kBody)) + 6, ly);
        const Rect r = oneLine ? Rect{x_ + w_ - cw, y, cw, kChoiceH} : Rect{x_, y + kLineH + 6, w_, kChoiceH};
        segmented_choice(id, r, items, selected, false);
        record(label, {x_, y, w_, h});
    }
    h_ += h;
}

void Body::popup(Ctl id, const std::wstring& label, const std::wstring& value, const std::vector<std::wstring>& items,
                 Applies applies) {
    gap();
    const float cw = popup_width(items.data(), static_cast<int>(items.size()), 120),
                lw = std::ceil(app.canvas.measure(label, t::kBody)) + 26 + (applies != Applies::None ? 90.0f : 0.0f);
    const bool oneLine = lw + cw <= w_;
    const float y = y_ + h_, h = oneLine ? kChoiceH : kLineH + 6 + kChoiceH;
    if (draw_) {
        const float ly = oneLine ? y + (kChoiceH - kLineH) / 2 : y;
        app.canvas.text(label, t::kBody, {x_, ly, oneLine ? w_ - cw : w_, kLineH}, col(app.c->label));
        const float tw = std::ceil(app.canvas.measure(label, t::kBody));
        help_icon(label, x_ + tw + 6, ly);
        if (applies != Applies::None)
            applies_tag(x_ + tw + (help_for(label) ? 22.0f : 8.0f), ly + (kLineH - 18) / 2, applies);
        const Rect r = oneLine ? Rect{x_ + w_ - cw, y, cw, kChoiceH} : Rect{x_, y + kLineH + 6, cw, kChoiceH};
        popup_button(id, r, value, false);
        record(label, {x_, y, w_, h});
    }
    h_ += h;
}

void Body::buttons(const std::wstring& label, const std::wstring& text, const std::vector<std::pair<Ctl, std::wstring>>& list,
                   Applies applies) {
    gap();
    float bw = 0;
    for (const auto& [id, name] : list)
        bw += button_width(name) + (bw > 0 ? t::kSpace2 : 0);
    const float tw = std::max(80.0f, w_ - bw - 12);
    const float th = text.empty() ? 0 : app.canvas.measure_height(text, t::kCallout, tw);
    const float h = std::max(kLineH + (th > 0 ? 2 + th : 0), t::kControlHeight), y = y_ + h_;
    if (draw_) {
        app.canvas.text(label, t::kBody, {x_, y, tw, kLineH}, col(app.c->label));
        const float lw = std::ceil(app.canvas.measure(label, t::kBody));
        help_icon(label, x_ + lw + 6, y);
        if (applies != Applies::None)
            applies_tag(x_ + lw + (help_for(label) ? 22.0f : 8.0f), y + (kLineH - 18) / 2, applies);
        if (th > 0)
            app.canvas.text(text, t::kCallout, {x_, y + kLineH + 2, tw, th + 2}, col(app.c->label_secondary), Align::Leading,
                            false, true);
        float bx = x_ + w_ - bw;
        for (const auto& [id, name] : list) {
            const float w = button_width(name);
            button(id, {bx, y + (h - t::kControlHeight) / 2, w, t::kControlHeight}, name, false);
            bx += w + t::kSpace2;
        }
        record(label, {x_, y, w_, h});
    }
    h_ += h;
}

void Body::info(const std::wstring& label, const std::wstring& value) {
    gap();
    const float y = y_ + h_;
    if (draw_)
        reader_text({x_, y_ + h_, w_, kLineH}, label + L": " + value);
    if (draw_) {
        const float vw = std::ceil(app.canvas.measure(value, t::kValue)) + 2;
        app.canvas.text(label, t::kBody, {x_, y, w_ - vw - 8, kLineH}, col(app.c->label_secondary));
        app.canvas.text(value, t::kValue, {x_ + w_ - vw, y, vw, kLineH}, col(app.c->label), Align::Trailing);
        record(label, {x_, y, w_, kLineH});
    }
    h_ += kLineH;
}

// A row with its label and a line under it leading, and something `tw` x `th` trailing (painted at its rect).
void Body::trailing(const std::wstring& label, const std::wstring& desc, float tw, float th,
                    const std::function<void(const Rect&)>& paint, const std::wstring& spoken) {
    gap();
    const float textW = std::max(60.0f, w_ - tw - 12);
    const float dh = desc.empty() ? 0 : app.canvas.measure_height(desc, t::kCallout, textW);
    const float h = std::max(kLineH + (dh > 0 ? 2 + dh : 0), th), y = y_ + h_;
    if (draw_) {
        // Read before its control: what the row says (its value), or its line under the label.
        reader_text({x_, y, w_, h}, !spoken.empty() ? spoken : desc.empty() ? L"" : label + L". " + desc);
        app.canvas.text(label, t::kBody, {x_, y, textW, kLineH}, col(app.c->label));
        help_icon(label, x_ + std::min(textW - 14, std::ceil(app.canvas.measure(label, t::kBody)) + 6), y);
        if (dh > 0)
            app.canvas.text(desc, t::kCallout, {x_, y + kLineH + 2, textW, dh + 2}, col(app.c->label_secondary), Align::Leading,
                            false, true);
        paint({x_ + w_ - tw, y + (std::min(h, std::max(th, kLineH)) - th) / 2, tw, th});
        record(label, {x_, y, w_, h});
    }
    h_ += h;
}

void Body::row(const std::wstring& label, const std::wstring& desc, const std::wstring& value) {
    const float vw = value.empty() ? 0 : std::ceil(app.canvas.measure(value, t::kValue)) + 2;
    if (vw > w_ * 0.45f) {  // a long value (a comparison): on its own line under the label, the line under it after
        gap();
        const float y = y_ + h_;
        const float vh = app.canvas.measure_height(value, t::kValue, w_);
        const float dh = desc.empty() ? 0 : app.canvas.measure_height(desc, t::kCallout, w_);
        const float h = kLineH + 2 + vh + (dh > 0 ? 2 + dh : 0);
        if (draw_) {
            reader_text({x_, y, w_, h}, label + L": " + value + (desc.empty() ? L"" : L". " + desc));
            app.canvas.text(label, t::kBody, {x_, y, w_, kLineH}, col(app.c->label));
            help_icon(label, x_ + std::ceil(app.canvas.measure(label, t::kBody)) + 6, y);
            app.canvas.text(value, t::kValue, {x_, y + kLineH + 2, w_, vh + 2}, col(app.c->label), Align::Leading, false, true);
            if (dh > 0)
                app.canvas.text(desc, t::kCallout, {x_, y + kLineH + 4 + vh, w_, dh + 2}, col(app.c->label_secondary),
                                Align::Leading, false, true);
            record(label, {x_, y, w_, h});
        }
        h_ += h;
        return;
    }
    trailing(
        label, desc, vw, kLineH,
        [value](const Rect& r) {
            if (!value.empty())
                app.canvas.text(value, t::kValue, r, col(app.c->label), Align::Trailing);
        },
        label + (value.empty() ? L"" : L": " + value) + (desc.empty() ? L"" : L". " + desc));
}

void Body::status(const std::wstring& label, const std::wstring& desc, int tone, const std::wstring& text) {
    trailing(
        label, desc, badge_width(text), 20, [tone, text](const Rect& r) { badge(r.x, r.y, tone, text); },
        label + L": " + text + (desc.empty() ? L"" : L". " + desc));
}

void Body::action(const std::wstring& label, const std::wstring& desc, Ctl id, const std::wstring& button, int option,
                  Btn style) {
    trailing(label, desc, button_width(button), t::kControlHeight, [=](const Rect& r) {
        ::simqvfr::launcher::ui::win::button(id, r, button, style, nullptr, option);
    });
}

void Body::actions(const std::vector<ButtonSpec>& list) {
    gap();
    float x = 0, y = 0;
    std::vector<Rect> rects;
    for (const ButtonSpec& b : list) {
        const float bw = button_width(b.label) + (b.icon ? 24.0f : 0.0f);
        if (x > 0 && x + bw > w_) {
            x = 0;
            y += t::kControlHeight + t::kSpace2;
        }
        rects.push_back({x_ + x, y_ + h_ + y, bw, t::kControlHeight});
        x += bw + t::kSpace2;
    }
    if (draw_)
        for (size_t i = 0; i < list.size(); ++i) {
            button(list[i].id, rects[i], list[i].label, list[i].style, list[i].icon);
            record(list[i].label, rects[i]);  // Find a setting finds a button (Report an issue…)
        }
    h_ += y + t::kControlHeight;
}

void Body::note(const std::wstring& text) {
    gap();
    const float th = app.canvas.measure_height(text, t::kCallout, w_);
    if (draw_) {
        reader_text({x_, y_ + h_, w_, th}, text);
        app.canvas.text(text, t::kCallout, {x_, y_ + h_, w_, th + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    }
    h_ += th;
}

void Body::chip(int tone, const std::wstring& text) {
    gap();
    if (draw_) {
        reader_text({x_, y_ + h_, w_, 22}, text);
        badge(x_, y_ + h_ + 1, tone, text, w_);
    }
    h_ += 22;
}

void Body::group(const std::wstring& text) {
    gap();
    if (draw_) {
        reader_text({x_, y_ + h_, w_, t::kFootnote.lineHeight}, text, 3);
        app.canvas.text(text, t::kFootnote, {x_, y_ + h_, w_, t::kFootnote.lineHeight}, col(app.c->label_muted));
    }
    h_ += t::kFootnote.lineHeight;
}

void Body::divider() {
    h_ += kRowGap;
    if (draw_)
        app.canvas.line(x_, y_ + h_ - 0.5f, x_ + w_, y_ + h_ - 0.5f, col(app.c->line), 1);
    first_ = false;
}

void Body::custom(float h, const std::function<void(const Rect&)>& paint) {
    gap();
    if (draw_)
        paint({x_, y_ + h_, w_, h});
    h_ += h;
}

void Body::reveal(Ctl parent, bool on, const std::wstring& parentLabel, const std::vector<std::wstring>& hiddenLabels,
                  const std::function<void(Body&)>& rows) {
    if (draw_) {  // it just turned on (here, the headset menu, a profile): its rows slide open
        const auto last = app.revealLast.find(parent);
        if (on && last != app.revealLast.end() && !last->second)
            start_reveal(parent);
        app.revealLast[parent] = on;
    }
    if (!on) {  // hidden, values kept: Find a setting points at the parent (its row, just drawn)
        if (draw_) {
            Rect parentRow{x_, y_, w_, h_};
            for (auto it = app.drawnLabels.rbegin(); it != app.drawnLabels.rend(); ++it)
                if (it->label == parentLabel) {
                    parentRow = it->r;
                    break;
                }
            for (const std::wstring& l : hiddenLabels)
                app.drawnLabels.push_back({l, app.drawingSection, parentRow, parentLabel});
        }
        return;
    }
    const float top = y_ + h_ + kRowGap;
    Body measure(x_, top, w_, false);
    rows(measure);
    const float p = reveal_progress(parent), shown = std::floor(measure.height() * p);
    if (draw_ && shown > 0) {
        const bool clip = p < 1;
        if (clip)
            app.canvas.push_clip({x_ - 12, top - 4, w_ + 24, shown + 4});
        Body body(x_, top, w_, true);
        rows(body);
        if (clip)
            app.canvas.pop_clip();
    }
    h_ += kRowGap + shown;
    first_ = false;
}

void Body::hidden(const std::wstring& rowLabel, const std::wstring& shownWith, const std::vector<std::wstring>& labels) {
    if (!draw_)
        return;
    Rect row{x_, y_, w_, h_};
    for (auto it = app.drawnLabels.rbegin(); it != app.drawnLabels.rend(); ++it)
        if (it->label == rowLabel) {
            row = it->r;
            break;
        }
    for (const std::wstring& l : labels)
        app.drawnLabels.push_back({l, app.drawingSection, row, shownWith});
}

void Body::more(const std::wstring& label, const std::vector<std::wstring>& hiddenLabels, const std::function<void(Body&)>& rows) {
    gap();
    const std::wstring key = app.drawingSection + L"/" + label;
    const bool open = app.moreOpen.count(key) > 0;
    const float y = y_ + h_, h = 24;
    if (draw_) {
        const std::wstring text = (open ? L"Less: " : L"More: ") + label;
        const float tw = std::ceil(app.canvas.measure(text, t::kBodyMedium));
        const Rect r{x_ - 4, y, tw + 8 + 18, h};
        const int index = static_cast<int>(app.moreKeys.size());
        app.moreKeys.push_back(key);
        if (pointer_in(r))
            app.canvas.fill(r, col(app.c->control_hover), t::kRadiusControl);
        app.canvas.text(text, t::kBodyMedium, {x_, y, tw + 2, h}, col(app.c->accent));
        app.canvas.path(open ? "M1.5 7.5L5 4l3.5 3.5" : "M1.5 4.5L5 8l3.5-3.5", {x_ + tw + 6, y + (h - 12) / 2, 10, 12}, 10,
                        col(app.c->accent), 1.5f);
        if (focused(kMore) && app.focusOption == index)
            focus_ring(r, t::kRadiusControl);
        app.hits.push_back({kMore, r, index, L"", text, open ? 1 : 0});
        if (!open)
            for (const std::wstring& l : hiddenLabels)
                app.drawnLabels.push_back({l, app.drawingSection, {x_, y, w_, h}, L"", key});
    }
    h_ += h;
    if (!open)
        return;
    Body body(x_, y_ + h_ + kRowGap, w_, draw_);
    rows(body);
    h_ += kRowGap + body.height();
}

// ---- Cards and the grid ------------------------------------------------------------------------

namespace {

// Measured as drawn: under its title, so its Mores (remembered by "title/label") are open or closed as they will draw.
float card_height(const Card& card, float w) {
    const std::wstring section = app.drawingSection;
    app.drawingSection = card.title;
    Body body(0, 0, w - 2 * kPad, false);
    card.body(body);
    app.drawingSection = section;
    return kHeaderH + 2 + kPad + body.height() + kPad;
}

// A card at r: its header (shape, title, chip; when it applies and its ⋯ menu at the end), its rule, its body.
void draw_card(const Card& card, const Rect& r, int index) {
    Canvas& cv = app.canvas;
    cv.fill(r, col(app.c->surface), t::kRadiusGroup);
    const float hy = r.y + 14;
    {  // read as a heading: its title, its chip, when it applies
        std::wstring spoken = card.title;
        if (!card.chip.empty())
            spoken += L", " + card.chip;
        if (card.applies != Applies::None)
            spoken += card.applies == Applies::Live ? L", live" : card.applies == Applies::LiveLowered ? L", live when lowered"
                                                                                                      : L", next launch";
        reader_text({r.x, r.y, r.w, kHeaderH}, spoken, 2);
    }
    section_shape(card.section, r.x + kPad + 5, hy + 11, 10);
    float right = r.right() - kPad;
    if (card.reset) {  // the ⋯ menu
        const Rect m{right - 28, hy - 3, 28, 28};
        const bool open = app.popup.open && app.popup.owner == kCardMenu && !app.popup.actions.empty() &&
                          app.popup.actions[0] == index;
        if (open || pointer_in(m))
            cv.fill(m, col(app.c->control_hover), t::kRadiusControl);
        for (int i = -1; i <= 1; ++i)
            cv.circle(m.x + 14 + i * 5.5f, m.y + 14, 1.6f, col(app.c->label_secondary), true);
        if (focused(kCardMenu) && app.focusOption == index)
            focus_ring(m, t::kRadiusControl);
        app.hits.push_back({kCardMenu, m, index, L"Card menu: reset this card"});
        right = m.x - 8;
    }
    if (card.applies != Applies::None) {
        const std::wstring text = card.applies == Applies::Live          ? L"Live"
                                  : card.applies == Applies::LiveLowered ? L"Live when lowered"
                                                                         : L"Next launch";
        const float tw = std::ceil(cv.measure(text, kBadgeStyle)) + 16;
        applies_tag(right - tw, hy + 2, card.applies);
        right -= tw + 8;
    }
    const float tx = r.x + kPad + 18;
    const float titleW = std::ceil(cv.measure(card.title, t::kTitle2));
    cv.text(card.title, t::kTitle2, {tx, hy, std::max(0.0f, right - tx), 22}, col(app.c->label));
    app.drawnLabels.push_back({card.title, card.title, {r.x, r.y, r.w, kHeaderH}});
    if (!card.chip.empty() && tx + titleW + 12 < right)
        badge(tx + titleW + 12, hy + 1, card.chipTone, card.chip, right - (tx + titleW + 12));
    cv.fill({r.x, r.y + kHeaderH, r.w, 2}, col(section_color(card.section)));  // the rule in the section's colour
    app.drawingSection = card.title;
    Body body(r.x + kPad, r.y + kHeaderH + 2 + kPad, r.w - 2 * kPad, true);
    card.body(body);
    app.drawingSection.clear();
}

}  // namespace

float card_grid(const std::vector<Card>& cards, float x, float y, float w, bool draw) {
    // As many columns of at least kCardMinW as fit, up to four: 3 at 1440 px, 2 at 1024 to 1280, 4 at 1920.
    constexpr float kCardMinW = 360;
    const float gap = 16;
    const int cols = std::clamp(static_cast<int>((w + gap) / (kCardMinW + gap)), 1, 4);
    const float cw = std::floor((w - gap * (cols - 1)) / cols);
    std::vector<float> colY(static_cast<size_t>(cols), 0.0f);
    size_t last = 0;
    bool any = false;
    for (const Card& card : cards) {
        const size_t c = card.follows && any ? last : static_cast<size_t>(std::min_element(colY.begin(), colY.end()) - colY.begin());
        last = c;
        any = true;
        const float h = card_height(card, cw);
        if (draw) {
            const int index = static_cast<int>(app.cardResets.size());
            app.cardTitles.push_back(card.title);
            app.cardResets.push_back(card.reset);
            draw_card(card, {x + c * (cw + gap), y + colY[c], cw, h}, index);
        }
        colY[c] += h + gap;
    }
    return std::max(0.0f, *std::max_element(colY.begin(), colY.end()) - gap);
}

void reset_card(int index) {
    if (index < 0 || index >= static_cast<int>(app.cardResets.size()) || !app.cardResets[static_cast<size_t>(index)])
        return;
    const std::wstring title = app.cardTitles[static_cast<size_t>(index)];
    const auto reset = app.cardResets[static_cast<size_t>(index)];
    Sheet s;
    s.title = L"Reset " + title + L"?";
    s.text = L"Its settings go back to their defaults. The other cards stay as they are.";
    s.buttons = {L"Cancel", L"Reset"};
    s.focus = 0;
    s.danger = true;
    s.done = [reset](int button, const std::wstring&) {
        if (button != 1)
            return;
        reset(app.s);
        changed();  // saved, and sent to a running game, as any change
    };
    open_sheet(std::move(s));
}

}  // namespace simqvfr::launcher::ui::win

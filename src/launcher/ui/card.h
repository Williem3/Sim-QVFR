// Cards (docs/ui/OVERHAUL_PLAN.md §2, §3): a card's header (its section's shape and colour, its title, when its
// settings apply, its ⋯ menu with Reset this card) over a body of compact rows, laid out in a grid of columns. Body
// lays rows out top to bottom, measuring or drawing; reveal() shows a parent's dependent rows with a short animation
// when it turns on, and hidden() records the labels a parent hides, for Find a setting.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "controls.h"
#include "form.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

// One slider of a row: its control, label, value, unit and the detail after the value (a pixel size).
struct SliderSpec {
    Ctl id;
    std::wstring label;
    int value;
    const wchar_t* unit = L"%";
    std::wstring detail;
    bool tenths = false;  // the value is in tenths ("1.5°")
};

class Body {
public:
    Body(float x, float y, float w, bool draw) : x_(x), y_(y), w_(w), draw_(draw) {}
    float height() const { return h_; }
    bool drawing() const { return draw_; }
    float width() const { return w_; }

    // A slider: the label and its value on a line, the track under it.
    void slider(const SliderSpec& s);
    // Two sliders side by side (Width and Height; a crop's two sides).
    void sliders(const SliderSpec& a, const SliderSpec& b);
    // A switch: the label and one line of help (or what it hides, when off) leading, the switch trailing.
    void toggle(Ctl id, const std::wstring& label, bool on, const std::wstring& help, bool disabled = false,
                Applies applies = Applies::None);
    // A choice of 2-4 short items: the label and a segmented control, on one line when they fit.
    void segments(Ctl id, const std::wstring& label, const std::vector<std::wstring>& items, int selected);
    // A choice from a list: the label and a pop-up button.
    void popup(Ctl id, const std::wstring& label, const std::wstring& value, const std::vector<std::wstring>& items,
               Applies applies = Applies::None);
    // A label with one line of text under it and buttons trailing (a DLL: Choose…).
    void buttons(const std::wstring& label, const std::wstring& text, const std::vector<std::pair<Ctl, std::wstring>>& list,
                 Applies applies = Applies::None);
    // A label and a value, the value trailing.
    void info(const std::wstring& label, const std::wstring& value);
    // A label, a line under it (wrapped) and, trailing, a value (tabular), a status badge or a button.
    void row(const std::wstring& label, const std::wstring& desc, const std::wstring& value);
    void status(const std::wstring& label, const std::wstring& desc, int tone, const std::wstring& text);
    void action(const std::wstring& label, const std::wstring& desc, Ctl id, const std::wstring& button, int option = -1,
                Btn style = Btn::Secondary);
    // Buttons in a row, wrapping onto the next line when they don't fit.
    struct ButtonSpec {
        Ctl id;
        std::wstring label;
        Btn style = Btn::Secondary;
        const char* icon = nullptr;
    };
    void actions(const std::vector<ButtonSpec>& list);
    // Muted text, wrapped (a note, why something isn't available).
    void note(const std::wstring& text);
    // A status chip (badge's tones).
    void chip(int tone, const std::wstring& text);
    // A heading inside the card (a group of its rows), upper case.
    void group(const std::wstring& text);
    // Space and a thin line between groups of rows.
    void divider();
    // Anything else, `h` high, drawn by `paint` at its rect.
    void custom(float h, const std::function<void(const Rect&)>& paint);
    // `parent`'s dependent rows: on, they show (sliding open for a moment after it turned on); off, nothing, and their
    // labels are recorded as hidden by `parentLabel` (Find a setting points at the parent).
    void reveal(Ctl parent, bool on, const std::wstring& parentLabel, const std::vector<std::wstring>& hiddenLabels,
                const std::function<void(Body&)>& rows);
    // Labels a choice hides (Gaze dead-zone in Fixed mode), recorded at the row labelled `rowLabel` for Find a setting,
    // which says they show when `shownWith` is chosen.
    void hidden(const std::wstring& rowLabel, const std::wstring& shownWith, const std::vector<std::wstring>& labels);
    // Rows most people never change, behind "More: <label>" (open or closed, remembered per card); closed, their labels
    // are recorded for Find a setting, which opens it.
    void more(const std::wstring& label, const std::vector<std::wstring>& hiddenLabels, const std::function<void(Body&)>& rows);

    // A label and a line under it leading, and anything `tw` x `th` trailing, painted at its rect (a layer's switch and
    // moves).
    // `spoken`: what a screen reader reads for the row (empty: the label and its line, when it has one).
    void trailing(const std::wstring& label, const std::wstring& desc, float tw, float th,
                  const std::function<void(const Rect&)>& paint, const std::wstring& spoken = {});

private:
    void gap();
    void record(const std::wstring& label, const Rect& r);
    float x_, y_, w_;
    bool draw_;
    float h_ = 0;
    bool first_ = true;
};

// A card: its section, title, when its settings apply (None: per row), its body, and what Reset this card does
// (nullptr: no reset).
struct Card {
    Tab section;
    std::wstring title;
    Applies applies = Applies::None;
    std::function<void(Body&)> body;
    std::function<void(Settings&)> reset;
    std::wstring chip;  // a warning beside the title (Eye tracker not detected), if any
    int chipTone = 1;
};

// The cards in columns (as many of at least 360 px as fit in `w`, up to four), each in the shortest column, in order,
// from (x, y) across `w`. Returns the height of the tallest column.
float card_grid(const std::vector<Card>& cards, float x, float y, float w, bool draw);

// The cards each Game tab's part makes (pane_checks.cpp, pane_diagnostics.cpp, pane_report.cpp, pane_games.cpp,
// pane_layers.cpp).
Card checks_card();
Card diagnostics_card();
Card help_card();
Card games_card();
Card options_card();  // the game's own options (LMU's anti-cheat); only for a game with them
Card layers_card();

// The card menu's Reset this card: asks first, then the card's settings go back to their defaults (a change like any
// other: saved and sent to a running game).
void reset_card(int index);

// Turning a parent on starts its rows' reveal (input.cpp); the timer repaints while one runs.
void start_reveal(Ctl parent);
bool revealing();

}  // namespace simqvfr::launcher::ui::win

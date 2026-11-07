// A pane's cards: rows of a label, a description and a control (Row), drawn as a titled section with a
// footer and what a change applies to (form.cpp).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../settings.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

// ---- Form layout -------------------------------------------------------------------------------

// When a section's settings reach the game: at once while it runs (Live), or when it next starts (NextLaunch); QVFR's
// resolution live as it goes down, or back up to the size the game started with, and above that at the next launch
// (LiveLowered: the views can't grow past their launch size). The section's title carries it as a tag (section).
enum class Applies { None, Live, LiveLowered, NextLaunch };

// One FormRow inside a section box: label (+ description) leading, the control trailing.
struct Row {
    std::wstring label, desc;
    float controlW = 0, controlH = 0;
    bool disabled = false;
    bool wide = false;  // the control takes the row's width after a label column (sliders)
    std::function<void(const Rect&)> control;  // drawn at its rect
    std::function<float(float)> fullHeight;    // full-width rows (switches): height for a width
    std::function<void(const Rect&)> full;
};

// form.cpp
float section(float x, float y, float w, const std::wstring& title, const std::vector<Row>& rows, const std::wstring& footer,
              bool draw, Applies applies = Applies::None);
std::wstring px(Size s);
std::wstring dims(Size s);
Row choice_row(Ctl id, const std::wstring& label, const std::wstring* names, int count, int selected, bool disabled);
Row slider_row(Ctl id, const std::wstring& label, const std::wstring& desc, int value, const std::wstring& detail,
               bool disabled = false, const wchar_t* unit = L"%", bool tenths = false);
Row locked_row(const std::wstring& label, const std::wstring& desc, const std::wstring& text);
Row value_row(const std::wstring& label, const std::wstring& desc, const std::wstring& value);
std::wstring formatted(const wchar_t* format, double v);

}  // namespace simqvfr::launcher::ui::win

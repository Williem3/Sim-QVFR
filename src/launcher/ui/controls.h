// The kit's controls, drawn into the canvas with their hit rectangles: buttons, popup buttons, sliders,
// checkboxes and switches, segmented controls, glyphs and badges, and the focus ring (controls.cpp).
#pragma once

#include <string>
#include <vector>

#include "../settings.h"
#include "window_state.h"

namespace simqvfr::launcher::ui::win {

// Button labels; the Launch button's (Jost, uppercase).
inline constexpr t::TypeStyle kButtonLabel = t::kButton;

inline constexpr t::TypeStyle kPrimaryLabel = t::kLaunch;

// StatusBadge text; measure with it when sizing one.
inline constexpr t::TypeStyle kBadgeStyle = t::kBadge;

// Secondary: a filled surface-2 button. Primary: blue (one per view). Ghost: no fill until the pointer is over it.
// Tint: blue at 16% with blue text (an action that adds). Danger: red text, no fill. Support: secondary, with a red
// heart (Support on Ko-fi). Launch: red with the play triangle, the only red action.
enum class Btn { Secondary, Primary, Ghost, Tint, Danger, Support, Launch };

// Button glyphs (16 x 16, stroked).
inline constexpr const char* kIconPlus = "M8 3.5v9M3.5 8h9";

inline constexpr const char* kIconPaste = "M6 2.5h4v2H6zM4.75 3.5H3.5v10h9v-10h-1.25";

inline constexpr const char* kIconReset = "M2.75 8C2.75 10.9 5.1 13.25 8 13.25S13.25 10.9 13.25 8 10.9 2.75 8 2.75C6.3 2.75 4.8 3.55 3.85 4.8M3.5 2.25V5.25H6.5";

inline constexpr const char* kIconPlay = "M5 3.25v9.5L12.5 8z";

inline constexpr const char* kIconHeart =
    "M8 13.5C8 13.5 2 9.9 2 5.9 2 4 3.4 2.75 5 2.75 6.3 2.75 7.4 3.5 8 4.6 8.6 3.5 9.7 2.75 11 2.75 12.6 2.75 14 4 14 5.9 14 9.9 8 13.5 8 13.5Z";

// A switch row: the label (and its description) leading, the switch trailing (a 44 x 24 rounded rectangle).
inline constexpr float kSwitchW = t::kSwitchWidth, kSwitchH = t::kSwitchHeight;

// controls.cpp
void focus_ring(const Rect& r, float radius);
bool focused(Ctl id);
bool pointer_in(const Rect& r);
float badge_width(const std::wstring& text);
float button_width(const std::wstring& label, const t::TypeStyle& style = kButtonLabel);
void button(Ctl id, const Rect& r, const std::wstring& label, Btn style, const char* icon = nullptr);
void button(Ctl id, const Rect& r, const std::wstring& label, bool primary);
float popup_width(const std::wstring* items, int count, float minWidth);
void popup_button(Ctl id, const Rect& r, const std::wstring& value, bool disabled);
void slider(Ctl id, const Rect& r, int value, Range range, const std::wstring& detail, bool disabled,
            const wchar_t* unit = L"%", bool tenths = false);
void slider_track(Ctl id, const Rect& track, int value, Range range, bool disabled);
float segmented_choice_width(const std::vector<std::wstring>& items);
void segmented_choice(Ctl id, const Rect& r, const std::vector<std::wstring>& items, int selected, bool disabled);
float checkbox_height(const std::wstring& desc, float width);
void switch_track(const Rect& s, bool on, bool disabled);
void checkbox(Ctl id, const Rect& r, bool checked, const std::wstring& label, const std::wstring& desc,
              bool disabled = false);
float segmented_width();
uint32_t overlay(int i);
void segmented(Ctl id, const Rect& r, int selected, bool disabled);
float badge(float x, float y, int tone, const std::wstring& text, float maxWidth = 10000);

}  // namespace simqvfr::launcher::ui::win

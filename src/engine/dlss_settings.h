// Sim-QVFR's own DLSS for a game that ships none (engine/dlss_views.h): the player's settings, [dlss] in sim-qvfr.ini,
// shared by the launcher's DLSS pane, the headset menu's DLSS page and the game's adapter (at launch and live). A game
// has them with games::GameFeatures::sideLoadedDlss. The inputs' diagnostics stay the game's [experiment] keys.
// Before [dlss], LMU had these as [experiment] lmu_dlss, lmu_dlss_folder, lmu_dlss_focus_scale, lmu_dlss_preset and
// lmu_dlss_eye_preset: each is read from there while [dlss] lacks it.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <string>

#include <windows.h>

#include "../ini_put.h"
#include "dlss_math.h"

namespace simqvfr {

struct DlssSettings {
    bool on = false;       // on: DLSS on the eyes and the focus views (live)
    std::wstring folder;   // folder: where nvngx_dlss.dll is (empty: beside the game's exe; next launch)
    int focusScale = 100;  // focus_scale: the focus views' mode, percent of their size each way (kDlssFocusModes; live)
    int focusPreset = 0;   // focus_preset: their NGX preset, 0 NGX's default for the mode (kDlssFocusPresets; live)
    int eyePreset = 11;    // eye_preset: the eyes' (the periphery's) NGX preset, K or F (kDlssEyePresets; live)
    bool operator==(const DlssSettings&) const = default;
};

// The choices offered, each a value and its name. NGX's presets by number (dlss_math.h preset_name): F 6, K 11, M 13.
struct DlssChoice {
    int value;
    const wchar_t* name;
};
inline constexpr DlssChoice kDlssFocusModes[] = {
    {100, L"DLAA"}, {77, L"Ultra quality"}, {67, L"Quality"}, {58, L"Balanced"}, {50, L"Performance"}};
// The focus views' preset: NGX's default for the mode (K; M at Performance), or either.
inline constexpr DlssChoice kDlssFocusPresets[] = {{0, L"Default"}, {11, L"K"}, {13, L"M"}};
// The eyes': K, the steadier, or F, the older model at about half the cost with more aliasing (eye tracking keeps that
// out of where you look; K suits a fixed focus).
inline constexpr DlssChoice kDlssEyePresets[] = {{11, L"K (steadier)"}, {6, L"F (cheaper)"}};

// `value`'s place in `list`: the exact one, else (`nearest`) the closest value, else the first.
template <size_t N>
int dlss_choice_index(const DlssChoice (&list)[N], int value, bool nearest = false) {
    int best = 0;
    for (size_t i = 0; i < N; ++i) {
        if (list[i].value == value)
            return static_cast<int>(i);
        if (nearest && std::abs(list[i].value - value) < std::abs(list[best].value - value))
            best = static_cast<int>(i);
    }
    return best;
}

// The value `step` places on from `value`'s (no wrap: the ends hold).
template <size_t N>
int dlss_choice_step(const DlssChoice (&list)[N], int value, int step) {
    int at = dlss_choice_index(list, value, true) + step;
    at = at < 0 ? 0 : at >= static_cast<int>(N) ? static_cast<int>(N) - 1 : at;
    return list[at].value;
}

template <size_t N>
const wchar_t* dlss_choice_name(const DlssChoice (&list)[N], int value) {
    return list[dlss_choice_index(list, value, true)].name;
}

// A preset as the ini holds it: its letter, "" for 0 (NGX's default).
inline const wchar_t* dlss_preset_letter(int preset) {
    static const wchar_t* const letters[] = {L"",  L"A", L"B", L"C", L"D", L"E", L"F", L"G",
                                             L"H", L"I", L"J", L"K", L"L", L"M", L"N", L"O"};
    return preset >= 0 && preset <= 15 ? letters[preset] : L"";
}

namespace dlss_detail {

// [section] key, or nothing (`found` false) when the file has no such key.
inline std::wstring read_text(const wchar_t* ini, const wchar_t* section, const wchar_t* key, bool& found) {
    wchar_t text[MAX_PATH] = {};
    GetPrivateProfileStringW(section, key, L"\x01", text, MAX_PATH, ini);
    found = !(text[0] == L'\x01' && text[1] == 0);
    return found ? std::wstring(text) : std::wstring();
}

// [dlss] key, else the old [experiment] key (an ini from before [dlss]).
inline std::wstring read_either(const wchar_t* ini, const wchar_t* key, const wchar_t* oldKey, bool& found) {
    std::wstring text = read_text(ini, L"dlss", key, found);
    if (!found && oldKey)
        text = read_text(ini, L"experiment", oldKey, found);
    return text;
}

}  // namespace dlss_detail

inline DlssSettings read_dlss_settings(const wchar_t* ini) {
    DlssSettings s;
    if (!ini)
        return s;
    using dlss_detail::read_either;
    bool found = false;
    const std::wstring on = read_either(ini, L"on", L"lmu_dlss", found);
    s.on = found && !on.empty() && _wtoi(on.c_str()) != 0;  // lmu_dlss=1 (the eyes only, once) reads as on
    s.folder = read_either(ini, L"folder", L"lmu_dlss_folder", found);
    const std::wstring scale = read_either(ini, L"focus_scale", L"lmu_dlss_focus_scale", found);
    if (found && !scale.empty())  // a value between the modes snaps to the nearest
        s.focusScale = kDlssFocusModes[dlss_choice_index(kDlssFocusModes, _wtoi(scale.c_str()), true)].value;
    const std::wstring focusPreset = read_either(ini, L"focus_preset", L"lmu_dlss_preset", found);
    const int focus = dlss::preset_from_text(focusPreset.c_str(), 0);
    s.focusPreset = kDlssFocusPresets[dlss_choice_index(kDlssFocusPresets, focus)].value;  // another: NGX's default
    const std::wstring eyePreset = read_either(ini, L"eye_preset", L"lmu_dlss_eye_preset", found);
    int eye = dlss::preset_from_text(eyePreset.c_str(), -1);
    if (eye < 0)  // the old empty lmu_dlss_eye_preset: as the focus views' (NGX's default for DLAA: K)
        eye = focus == 6 ? 6 : 11;
    s.eyePreset = eye == 6 ? 6 : 11;
    return s;
}

inline bool write_dlss_settings(const wchar_t* ini, const DlssSettings& s) {
    bool ok = ini_put(ini, L"dlss", L"on", s.on ? 1 : 0);
    ok &= ini_put(ini, L"dlss", L"folder", s.folder.c_str());
    ok &= ini_put(ini, L"dlss", L"focus_scale", kDlssFocusModes[dlss_choice_index(kDlssFocusModes, s.focusScale, true)].value);
    const int focus = kDlssFocusPresets[dlss_choice_index(kDlssFocusPresets, s.focusPreset)].value;
    ok &= ini_put(ini, L"dlss", L"focus_preset", focus ? dlss_preset_letter(focus) : L"");
    ok &= ini_put(ini, L"dlss", L"eye_preset", dlss_preset_letter(s.eyePreset == 6 ? 6 : 11));
    return ok;
}

}  // namespace simqvfr

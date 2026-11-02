// The Cheeky Foveated DLSS settings ace-quadview exposes ([cheeky] in ace-quadview.ini), shared by the
// layer (which applies them to Cheeky's runtime with its "set" command, live), the launcher's Foveated
// DLSS pane and the in-headset menu's Foveated DLSS page. Cheeky keeps its own CheekyFoveatedDLSS.ini;
// these few keys are applied over it at every start and change.
//
// In QVFR the layer also sets Cheeky's quad view roles: ACE evaluates the four views' DLSS in view
// order (outer left, outer right, focus left, focus right), which is Cheeky's role order 1-4.
#pragma once

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include <windows.h>

namespace aceqv {

struct CheekySettings {
    bool load = false;        // enabled: load Cheeky's runtime (applies at the next launch)
    bool processing = true;   // processing: Cheeky's Enabled (live)
    bool outerDlaa = true;    // outer_dlaa: PeripheralDlaa, the outer views' own cheaper DLAA pass
    int outerScale = 75;      // outer_scale: PeripheralDlaaScale, percent
    int outerPreset = 5;      // outer_preset: PeripheralDlaaPreset (no game default)
    int focusPreset = 0;      // focus_preset: CenterPreset (0: the game's)
    // Experimental, the focus views only (Cheeky's quad mode): DLSS above their output size, the
    // motion-vector blur workaround, and DLSS-NR (needs NVIDIA's nvngx_dlssnr.dll next to the game).
    int supersampling = 100;  // center_supersampling: CenterSupersampling, percent
    bool motionFix = false;   // fix_motion_blur: FixMotionBlur
    bool nr = false;          // nr: NrEnabled
    int nrPreset = 0;         // nr_preset: NrPreset (kCheekyNrPresets)
    int nrPasses = 1;         // nr_passes: NrPasses
    // Frame generation: Cheeky's DLSS-G on the finished image, guided by the outer views' depth and motion,
    // foveated (a centre region at full method, simple warping around it). Needs OpenXR Toolkit's Turbo
    // mode off and no other frame generation (VectorXR's).
    bool fg = false;          // fg: FgEnabled
    int fgMultiplier = 2;     // fg_multiplier: FgMultiplier (2..4)
    int fgSize = 30;          // fg_size: FgWidth/FgHeight, the centre region, percent of the eye
    int fgScale = 50;         // fg_scale: FgResolutionPercent, the centre's FG resolution (25, 50, 75, 100)
    int fgPeriphery = 50;     // fg_periphery: FgPeripheralPercent, the periphery's (25, 50, 75, 100)
    bool fgBorder = false;    // fg_border: FgShowBorder, outlines the centre region (to check it follows the eyes)
    bool operator==(const CheekySettings&) const = default;
};

inline constexpr int kCheekyFgMultiplierMax = 4, kCheekyFgSizeMin = 10, kCheekyFgSizeMax = 100, kCheekyFgSizeStep = 5;
// FG resolutions are quarters (Cheeky's choices).
inline int cheeky_fg_quarter(int percent) { return std::clamp((percent + 12) / 25 * 25, 25, 100); }

inline constexpr int kCheekyScaleMin = 20, kCheekyScaleMax = 100, kCheekyScaleStep = 5;
inline constexpr int kCheekySupersamplingMin = 100, kCheekySupersamplingMax = 200, kCheekySupersamplingStep = 5;
inline constexpr int kCheekyNrPassesMax = 3;
inline constexpr const wchar_t* kCheekyNrPresets[] = {L"Default", L"A (Original)", L"B (Faster)"};
inline constexpr int kCheekyNrPresetMax = static_cast<int>(std::size(kCheekyNrPresets)) - 1;

// Cheeky's preset choices (its overlay's), value and name. The outer views have no game default.
struct CheekyPreset {
    int value;
    const wchar_t* name;
};
inline constexpr CheekyPreset kCheekyPresets[] = {{0, L"Game default"}, {5, L"E (fastest)"}, {10, L"J"},
                                                  {11, L"K"},           {12, L"L"},             {13, L"M"}};

inline const wchar_t* cheeky_preset_name(int value) {
    for (const CheekyPreset& p : kCheekyPresets)
        if (p.value == value)
            return p.name;
    return L"?";
}

// The preset `step` places along the list from `value` (wrapping), skipping the game default for the
// outer views.
inline int cheeky_preset_step(int value, int step, bool allowDefault) {
    const int first = allowDefault ? 0 : 1, count = static_cast<int>(std::size(kCheekyPresets)) - first;
    int at = 0;
    for (int i = 0; i < count; ++i)
        if (kCheekyPresets[first + i].value == value)
            at = i;
    return kCheekyPresets[first + ((at + step) % count + count) % count].value;
}

inline int cheeky_valid_preset(int value, bool allowDefault) {
    for (const CheekyPreset& p : kCheekyPresets)
        if (p.value == value && (allowDefault || p.value != 0))
            return value;
    return allowDefault ? 0 : 5;
}

inline CheekySettings read_cheeky_settings(const wchar_t* ini) {
    CheekySettings c;
    const auto get = [ini](const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(L"cheeky", key, fallback, ini));
    };
    c.load = get(L"enabled", 0) != 0;
    c.processing = get(L"processing", 1) != 0;
    c.outerDlaa = get(L"outer_dlaa", 1) != 0;
    c.outerScale = std::clamp(get(L"outer_scale", c.outerScale), kCheekyScaleMin, kCheekyScaleMax);
    c.outerPreset = cheeky_valid_preset(get(L"outer_preset", c.outerPreset), false);
    c.focusPreset = cheeky_valid_preset(get(L"focus_preset", c.focusPreset), true);
    c.supersampling = std::clamp(get(L"center_supersampling", c.supersampling), kCheekySupersamplingMin,
                                 kCheekySupersamplingMax);
    c.motionFix = get(L"fix_motion_blur", 0) != 0;
    c.nr = get(L"nr", 0) != 0;
    c.nrPreset = std::clamp(get(L"nr_preset", c.nrPreset), 0, kCheekyNrPresetMax);
    c.nrPasses = std::clamp(get(L"nr_passes", c.nrPasses), 1, kCheekyNrPassesMax);
    c.fg = get(L"fg", 0) != 0;
    c.fgMultiplier = std::clamp(get(L"fg_multiplier", c.fgMultiplier), 2, kCheekyFgMultiplierMax);
    c.fgSize = std::clamp(get(L"fg_size", c.fgSize), kCheekyFgSizeMin, kCheekyFgSizeMax);
    c.fgScale = cheeky_fg_quarter(get(L"fg_scale", c.fgScale));
    c.fgPeriphery = cheeky_fg_quarter(get(L"fg_periphery", c.fgPeriphery));
    c.fgBorder = get(L"fg_border", 0) != 0;
    return c;
}

// The live keys (everything but `load`), or with `load` the enabled key as well.
inline bool write_cheeky_settings(const wchar_t* ini, const CheekySettings& c, bool load) {
    const auto put = [ini](const wchar_t* key, int value) {
        return WritePrivateProfileStringW(L"cheeky", key, std::to_wstring(value).c_str(), ini) != 0;
    };
    bool ok = true;
    if (load)
        ok &= put(L"enabled", c.load ? 1 : 0);
    ok &= put(L"processing", c.processing ? 1 : 0);
    ok &= put(L"outer_dlaa", c.outerDlaa ? 1 : 0);
    ok &= put(L"outer_scale", std::clamp(c.outerScale, kCheekyScaleMin, kCheekyScaleMax));
    ok &= put(L"outer_preset", cheeky_valid_preset(c.outerPreset, false));
    ok &= put(L"focus_preset", cheeky_valid_preset(c.focusPreset, true));
    ok &= put(L"center_supersampling", std::clamp(c.supersampling, kCheekySupersamplingMin, kCheekySupersamplingMax));
    ok &= put(L"fix_motion_blur", c.motionFix ? 1 : 0);
    ok &= put(L"nr", c.nr ? 1 : 0);
    ok &= put(L"nr_preset", std::clamp(c.nrPreset, 0, kCheekyNrPresetMax));
    ok &= put(L"nr_passes", std::clamp(c.nrPasses, 1, kCheekyNrPassesMax));
    ok &= put(L"fg", c.fg ? 1 : 0);
    ok &= put(L"fg_multiplier", std::clamp(c.fgMultiplier, 2, kCheekyFgMultiplierMax));
    ok &= put(L"fg_size", std::clamp(c.fgSize, kCheekyFgSizeMin, kCheekyFgSizeMax));
    ok &= put(L"fg_scale", cheeky_fg_quarter(c.fgScale));
    ok &= put(L"fg_periphery", cheeky_fg_quarter(c.fgPeriphery));
    ok &= put(L"fg_border", c.fgBorder ? 1 : 0);
    return ok;
}

// Cheeky's "set" lines for these settings and, for QVFR, the quad view roles. `gaze`: QVFR's focus follows
// the eyes ([quad] gaze), so Cheeky's FG region follows them too (CenterMode 1, its OpenXR layer's eye
// gaze; game-composited quad views use it only for FG). Otherwise the region sits at each eye's forward point.
inline std::string cheeky_set_lines(const CheekySettings& c, bool quadRoles, bool gaze = false) {
    char scale[16], supersampling[16];
    snprintf(scale, sizeof(scale), "%.2f", std::clamp(c.outerScale, kCheekyScaleMin, kCheekyScaleMax) / 100.0);
    snprintf(supersampling, sizeof(supersampling), "%.2f",
             std::clamp(c.supersampling, kCheekySupersamplingMin, kCheekySupersamplingMax) / 100.0);
    std::string s = "Enabled=" + std::to_string(c.processing ? 1 : 0) + "\nPeripheralDlaa=" +
                    std::to_string(c.outerDlaa ? 1 : 0) + "\nPeripheralDlaaScale=" + scale +
                    "\nPeripheralDlaaPreset=" + std::to_string(cheeky_valid_preset(c.outerPreset, false)) +
                    "\nCenterPreset=" + std::to_string(cheeky_valid_preset(c.focusPreset, true)) +
                    "\nCenterSupersampling=" + supersampling + "\nFixMotionBlur=" + std::to_string(c.motionFix ? 1 : 0) +
                    "\nNrEnabled=" + std::to_string(c.nr ? 1 : 0) +
                    "\nNrPreset=" + std::to_string(std::clamp(c.nrPreset, 0, kCheekyNrPresetMax)) +
                    "\nNrPasses=" + std::to_string(std::clamp(c.nrPasses, 1, kCheekyNrPassesMax));
    // Frame generation as tested in ACE: DLSS-G centre, warped periphery, foveated on its own region, and
    // the game released once its colour is captured (the default holds it until generation, which
    // deadlocked here: DLSS-G shares ACE's queue).
    char fgSize[16];
    snprintf(fgSize, sizeof(fgSize), "%.2f", std::clamp(c.fgSize, kCheekyFgSizeMin, kCheekyFgSizeMax) / 100.0);
    s += "\nFgEnabled=" + std::to_string(c.fg ? 1 : 0) +
         "\nFgMultiplier=" + std::to_string(std::clamp(c.fgMultiplier, 2, kCheekyFgMultiplierMax)) +
         "\nFgCenterMethod=0\nFgPeripheralMethod=1\nFgFoveated=1\nFgMatchSrRegion=0\nFgReleasePoint=8" +
         "\nFgXOffset=0\nFgYOffset=0\nCenterMode=" + std::to_string(gaze ? 1 : 0) +
         "\nFgWidth=" + fgSize + "\nFgHeight=" + fgSize +
         "\nFgResolutionPercent=" + std::to_string(cheeky_fg_quarter(c.fgScale)) +
         "\nFgPeripheralPercent=" + std::to_string(cheeky_fg_quarter(c.fgPeriphery)) +
         "\nFgShowBorder=" + std::to_string(c.fgBorder ? 1 : 0);
    if (quadRoles)
        s += "\nManualQuadViewAssignment=1\nQuadSavedSourceCount=4\nQuadView1Role=1\nQuadView2Role=2"
             "\nQuadView3Role=3\nQuadView4Role=4";
    return s;
}

// The credit Cheeky's author asked for wherever the UI controls Cheeky: "CheekyFoveatedDLSS - v0.6.3"
// (Cheeky reports "0.6.3-standalone"; the host suffix is dropped). Empty `version`: not known yet.
inline std::wstring cheeky_version_label(const std::wstring& version) {
    const std::wstring v = version.substr(0, version.find(L'-'));
    return v.empty() ? L"CheekyFoveatedDLSS" : L"CheekyFoveatedDLSS - v" + v;
}

}  // namespace aceqv

// NVIDIA's DLSS neural rendering ("DLSS-NR") on Sim-QVFR's own DLSS views (engine/nr_views.h): the player's settings,
// [dlss_nr] in sim-qvfr.ini, shared by the launcher's DLSS pane, the headset menu's DLSS-NR page and the game's adapter
// (at launch and live). A game has them with games::GameFeatures::sideLoadedDlss; they work only with [dlss] on.
// Pure arithmetic and the ini: no Direct3D (engine/nr_math.h has the regions).
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>

#include <windows.h>

#include "../ini_put.h"
#include "dlss_settings.h"

namespace simqvfr {

struct NrSettings {
    bool on = false;      // on (live)
    // folder: where the player's nvngx_dlssnr.dll is (a game that ships DLSS-NR has one). Sim-QVFR includes none: it is
    // not NVIDIA's public SDK's, so not ours to give out. Next launch.
    std::wstring folder;
    int preset = 0;       // preset: the runtime's model, 0 its default, 1 A, 2 B (kNrPresets; live, a new feature)
    int style = 0;        // style: 0 standard, 1 natural, 2 cinematic (kNrStyles; live)
    int intensity = 100;  // intensity: percent, 0 the picture as it came .. 100 (live)
    int localTone = 100;       // local_tone: percent, 0..200 (live)
    int localStructure = 100;  // local_structure: percent, 0..200 (live)
    bool autoMask = false;     // auto_mask: the runtime's own mask of what it treats differently (skin) (live)
    // periphery: the eyes' views too (the periphery), not the focus views alone (live, new features).
    bool periphery = false;
    // focus_working, periphery_working: what the model works at, percent of the region's size each way (kNrScaleMin..100).
    // Its change of the picture is put onto the full-size picture, so a smaller scale costs the change's fine detail,
    // not the picture's (live, new features).
    int focusScale = 100;
    int peripheryScale = 50;
    // working_limit: the most one view's model works on, in tenths of a megapixel (kNrLimitMin..kNrLimitMax), whatever
    // the view's size and the percents above: a working size over it is lowered to it. The model takes about 2 ms of
    // GPU time and 130 MB of video memory a megapixel, plus 300 MB a view (RTX 5070 Ti, runtime 310.8). The first
    // headset run had none of this: two 5.4-megapixel focus views, 2 GB and 21 ms a frame on a full card (live, new
    // features).
    int workingLimit = 10;
    // periphery_coverage: how much of each eye's view the model covers, percent of its size each way, about the focus
    // view (kNrCoverageMin..100: all of it) (live, new features).
    int peripheryCoverage = 100;
    // edge_blend: the band along the processed region's edge over which it fades to the picture as it came, percent of
    // the region's size (0..kNrEdgeMax): the focus view's edge when the periphery isn't processed, the covered
    // region's edge when it is smaller than the view (live).
    int edgeBlend = 10;
    // before_dlss: the model runs on the views as drawn, before DLSS anti-aliases or upscales them (fewer pixels under
    // a focus upscale); off: on DLSS's output (live, new features).
    bool beforeDlss = false;
    // exposure: the scene's brightness as the model gets it, percent. The model works on 0..1; the game's scene is HDR
    // before its tone mapping, so it goes in through a curve that keeps highlights (and back out through its inverse)
    // (live).
    int exposure = 100;
    // input_gamma: the model gets the scene display-encoded (sRGB's curve), as a finished frame is; off: linear (live).
    bool inputGamma = true;
    // auto_exposure: the scene is brought to a photograph's exposure before the model gets it (its mean brightness to
    // middle grey, smoothed over frames, one value for every view), whatever the game's own scale; `exposure` then
    // multiplies that. Off: `exposure` alone (live).
    bool autoExposure = true;
    // max_gain: the most the model may brighten or darken a pixel, percent (kNrGainMin..kNrGainMax; 300: three times
    // either way). Its change is taken as a ratio of its output to its input, as the display would show them, and
    // the scene is multiplied by it: a ratio near white stays a ratio (the first two headset runs put its output back
    // through the curve's inverse, which made every small change near white many times larger: blown highlights, and
    // its frame-to-frame variation a flicker) (live).
    int maxGain = 300;
    // Diagnostics, [experiment] dlss_nr_save=N (not written back): N sets of pictures into the log folder, the first
    // half a second after it starts and then every ten: what the model got, what it gave back and the composed result,
    // as PNG (each read back from the GPU: a hitch a set).
    int saveFrames = 0;
    bool operator==(const NrSettings&) const = default;
};

inline constexpr DlssChoice kNrPresets[] = {{0, L"Default"}, {1, L"A (original)"}, {2, L"B (faster)"}};
inline constexpr DlssChoice kNrStyles[] = {{0, L"Standard"}, {1, L"Natural"}, {2, L"Cinematic"}};
inline constexpr int kNrScaleMin = 25, kNrCoverageMin = 20, kNrEdgeMax = 30, kNrExposureMin = 25, kNrExposureMax = 400;
inline constexpr int kNrLimitMin = 3, kNrLimitMax = 40;  // tenths of a megapixel a view
inline constexpr int kNrGainMin = 110, kNrGainMax = 1000;

// The settings held to their ranges (an ini edited by hand).
inline NrSettings nr_clamped(NrSettings s) {
    s.preset = kNrPresets[dlss_choice_index(kNrPresets, s.preset)].value;
    s.style = kNrStyles[dlss_choice_index(kNrStyles, s.style)].value;
    s.intensity = std::clamp(s.intensity, 0, 100);
    s.localTone = std::clamp(s.localTone, 0, 200);
    s.localStructure = std::clamp(s.localStructure, 0, 200);
    s.focusScale = std::clamp(s.focusScale, kNrScaleMin, 100);
    s.peripheryScale = std::clamp(s.peripheryScale, kNrScaleMin, 100);
    s.workingLimit = std::clamp(s.workingLimit, kNrLimitMin, kNrLimitMax);
    s.peripheryCoverage = std::clamp(s.peripheryCoverage, kNrCoverageMin, 100);
    s.edgeBlend = std::clamp(s.edgeBlend, 0, kNrEdgeMax);
    s.exposure = std::clamp(s.exposure, kNrExposureMin, kNrExposureMax);
    s.maxGain = std::clamp(s.maxGain, kNrGainMin, kNrGainMax);
    s.saveFrames = std::clamp(s.saveFrames, 0, 50);
    return s;
}

inline NrSettings read_nr_settings(const wchar_t* ini) {
    NrSettings s;
    if (!ini)
        return s;
    const auto number = [ini](const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(L"dlss_nr", key, fallback, ini));
    };
    s.on = number(L"on", 0) != 0;
    wchar_t folder[MAX_PATH] = {};
    GetPrivateProfileStringW(L"dlss_nr", L"folder", L"", folder, MAX_PATH, ini);
    s.folder = folder;
    s.preset = number(L"preset", s.preset);
    s.style = number(L"style", s.style);
    s.intensity = number(L"intensity", s.intensity);
    s.localTone = number(L"local_tone", s.localTone);
    s.localStructure = number(L"local_structure", s.localStructure);
    s.autoMask = number(L"auto_mask", 0) != 0;
    s.periphery = number(L"periphery", 0) != 0;
    s.focusScale = number(L"focus_working", s.focusScale);
    s.peripheryScale = number(L"periphery_working", s.peripheryScale);
    s.workingLimit = number(L"working_limit", s.workingLimit);
    s.peripheryCoverage = number(L"periphery_coverage", s.peripheryCoverage);
    s.edgeBlend = number(L"edge_blend", s.edgeBlend);
    s.beforeDlss = number(L"before_dlss", 0) != 0;
    s.exposure = number(L"exposure", s.exposure);
    s.inputGamma = number(L"input_gamma", 1) != 0;
    s.autoExposure = number(L"auto_exposure", 1) != 0;
    s.maxGain = number(L"max_gain", s.maxGain);
    s.saveFrames = static_cast<int>(GetPrivateProfileIntW(L"experiment", L"dlss_nr_save", 0, ini));
    return nr_clamped(s);
}

inline bool write_nr_settings(const wchar_t* ini, const NrSettings& given) {
    const NrSettings s = nr_clamped(given);
    bool ok = ini_put(ini, L"dlss_nr", L"on", s.on ? 1 : 0);
    ok &= ini_put(ini, L"dlss_nr", L"folder", s.folder.c_str());
    ok &= ini_put(ini, L"dlss_nr", L"preset", s.preset);
    ok &= ini_put(ini, L"dlss_nr", L"style", s.style);
    ok &= ini_put(ini, L"dlss_nr", L"intensity", s.intensity);
    ok &= ini_put(ini, L"dlss_nr", L"local_tone", s.localTone);
    ok &= ini_put(ini, L"dlss_nr", L"local_structure", s.localStructure);
    ok &= ini_put(ini, L"dlss_nr", L"auto_mask", s.autoMask ? 1 : 0);
    ok &= ini_put(ini, L"dlss_nr", L"periphery", s.periphery ? 1 : 0);
    ok &= ini_put(ini, L"dlss_nr", L"focus_working", s.focusScale);
    ok &= ini_put(ini, L"dlss_nr", L"periphery_working", s.peripheryScale);
    ok &= ini_put(ini, L"dlss_nr", L"working_limit", s.workingLimit);
    ok &= ini_put(ini, L"dlss_nr", L"periphery_coverage", s.peripheryCoverage);
    ok &= ini_put(ini, L"dlss_nr", L"edge_blend", s.edgeBlend);
    ok &= ini_put(ini, L"dlss_nr", L"before_dlss", s.beforeDlss ? 1 : 0);
    ok &= ini_put(ini, L"dlss_nr", L"exposure", s.exposure);
    ok &= ini_put(ini, L"dlss_nr", L"input_gamma", s.inputGamma ? 1 : 0);
    ok &= ini_put(ini, L"dlss_nr", L"auto_exposure", s.autoExposure ? 1 : 0);
    ok &= ini_put(ini, L"dlss_nr", L"max_gain", s.maxGain);
    return ok;
}

}  // namespace simqvfr

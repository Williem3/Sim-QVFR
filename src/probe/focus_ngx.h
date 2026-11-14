// P2a: NVIDIA DLSS for the focus views, at the NGX API (the driver's _nvngx.dll exports, which the game's own NGX
// library calls with its arguments unchanged). Detours on CreateFeature / EvaluateFeature read what the game sends
// (logged, and the latest evaluation of each eye kept); our own SuperSampling features, one per focus view, are made
// and evaluated through the same exports inside the game's NGX session. The game's calls pass unchanged, but for the
// eyes' preset override (set_presets) at its feature creation.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace simqvfr::focus_ngx {

// One of the game's DLSS evaluations, as its parameter bag held it.
struct EyeEval {
    uint64_t sequence = 0;  // evaluations seen before this one (0: none kept)
    ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr, *exposure = nullptr;
    ID3D12Resource *transparency = nullptr, *bias = nullptr;
    std::string colorName, depthName, motionName, outputName, exposureName;
    float jitter[2] = {}, motionScale[2] = {1, 1};
    float preExposure = 0, exposureScale = 0, sharpness = 0, frameTime = 0;
    bool hasPreExposure = false, hasExposureScale = false, hasFrameTime = false;
    int reset = 0;
    unsigned renderW = 0, renderH = 0;
    unsigned colorBase[2] = {}, depthBase[2] = {}, motionBase[2] = {}, outputBase[2] = {};
};

// The game's SuperSampling feature as it was made (the last one), if the detour saw it.
struct CreateInfo {
    bool seen = false;
    unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;
    int quality = 0, flags = 0;
    unsigned presets[6] = {};  // DLAA, Quality, Balanced, Performance, UltraPerformance, UltraQuality
    bool presetSet[6] = {};
};

// Hooks the exports once _nvngx.dll is loaded (cheap to call again; true once installed).
bool install();
// At instance creation: NVIDIA's NGX core (_nvngx.dll, from the folder the driver registers) loaded and detoured, so the
// game's own load gets the same module and its feature creation is seen (its create flags, presets).
void preload();
bool installed();
// NGX render presets by number (0: the game's own): `eye` for the game's own feature (its eyes, the periphery), set at its
// creation in place of the game's; `focus` for our focus features. While running too: the eyes' feature is then
// replaced at its next evaluation by one of ours with the same sizes, mode and flags and the new preset (the game's
// own handle stays the game's), and the focus features are made again by their owner (focus_preset).
void set_presets(int eye, int focus);
// The focus features' preset as last set: a change while running has the caller make its features again.
int focus_preset();
// jitterX: the share of the game's horizontal jitter NGX is told (the game's eyes and our focus features). ACE's
// single-pass stereo applies half the horizontal jitter it reports (its NDC jitter is over the double-wide target):
// 0.5 tells NGX the shift really drawn; 1 leaves the game's value. peripheryDlaa: the game's eyes at DLAA in every
// in-game DLSS mode (its optimal-settings query answered with render = output, its feature made as DLAA).
void set_options(float jitterX, bool peripheryDlaa);
float jitter_x();
// What runs, for the headset menu. Not seen until the game has made its DLSS feature.
struct State {
    bool seen = false, forcedDlaa = false;
    int gameQuality = 0;        // the in-game mode as the game asked (NGX's PerfQuality value)
    unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;  // the eyes' feature as made
    int eyePreset = 0, focusPreset = 0;  // as used (the game's own preset when not overridden)
    bool focusRunning = false;
};
State state();
void note_focus_running(bool running);
bool latest(int eye, EyeEval& out);  // eye 0 / 1 by the colour subrect (x > 0: the right eye)
CreateInfo create_info();
uint64_t game_evaluations();

struct Feature;
// Our SuperSampling feature at the focus size (render = output: DLAA), with the game's flags, quality and presets.
// Recorded into `list`; nullptr (result logged) when NGX refuses.
// outWidth / outHeight (0: as the render size): the output size of a feature that upscales, its mode by the ratio.
Feature* make(ID3D12GraphicsCommandList* list, unsigned width, unsigned height, int extraFlags = 0, unsigned outWidth = 0,
              unsigned outHeight = 0);
void release(Feature* f);

struct Inputs {
    ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr, *exposure = nullptr;
    unsigned base[2] = {};  // every input's and the output's subrect base (one eye of a side-by-side target)
    unsigned outBase[2] = {};  // the output's own base when `ownOutBase` (a feature that upscales)
    bool ownOutBase = false;
    unsigned width = 0, height = 0;
    float jitter[2] = {}, motionScale[2] = {1, 1};
    const EyeEval* game = nullptr;  // pre-exposure, exposure scale, sharpness, frame time copied as the game set them
    bool reset = false;
};
// Records our evaluation into `list` (inputs in shader-resource states, output in UNORDERED_ACCESS). False when NGX
// refuses (logged a few times).
bool evaluate(ID3D12GraphicsCommandList* list, Feature* f, const Inputs& in);

}  // namespace simqvfr::focus_ngx

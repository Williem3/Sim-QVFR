// DLSS called by us through NVIDIA's NGX on a game's D3D11 device, for a game that ships no DLSS of its own
// (docs/recon-lmu.md "DLSS for LMU at the API level"): a view's image anti-aliased at its own size (DLAA), or upscaled
// from a smaller render (super resolution), from its colour, depth and motion vectors. Built only where CMake's
// NGX_SDK_DIR names NVIDIA's NGX SDK (the SDK is not in this repository); elsewhere every call answers that it is
// unavailable. tests/ngx_dlss_check.cpp is the standalone hardware check of the same calls.
#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace simqvfr::ngx {

// This build has the NGX SDK linked in.
bool built_in();
// NGX started on `device` with a project id of ours, nvngx_dlss.dll looked for in `dllFolder` (and beside the game's
// exe), NGX's own files in `dataFolder`. False with the reason in `why` (not built in, no RTX GPU, no DLL, an old
// driver). Once per device; a second call answers as the first did.
bool start(ID3D11Device* device, const wchar_t* dllFolder, const wchar_t* dataFolder, std::string& why);

struct Feature;
// A DLSS feature for a view rendered at up to renderWidth x renderHeight and shown at outputWidth x outputHeight: DLAA
// when they are the same, else super resolution (its quality mode the nearest to the ratio: ultra quality 77%, quality
// 67%, balanced 58%, performance 50%, ultra performance 33%; any ratio between works, the mode only names it). HDR
// colour in, NGX's own exposure, motion vectors at the render size. `preset`: NGX's render preset number for whichever
// mode (0: its default). Null with NGX's result in `result`.
Feature* create(ID3D11DeviceContext* context, unsigned renderWidth, unsigned renderHeight, unsigned outputWidth,
                unsigned outputHeight, bool depthInverted, int preset, unsigned& result);
void release(Feature* feature);

struct Evaluation {
    ID3D11Resource* colour = nullptr;   // the view at (colourX, colourY) of this texture, width x height
    unsigned colourX = 0, colourY = 0;
    ID3D11Resource* depth = nullptr;    // the view's own, width x height (R32F)
    ID3D11Resource* vectors = nullptr;  // the view's own, width x height (RG16F): to where the pixel was, in pixels
    ID3D11Resource* output = nullptr;   // the view's own, at the feature's output size
    unsigned width = 0, height = 0;     // this frame's render size (at most the feature's)
    float jitterX = 0, jitterY = 0;     // this frame's projection jitter, in render pixels
    bool reset = false;                 // no history (the first frame, a cut)
};
bool evaluate(ID3D11DeviceContext* context, Feature* feature, const Evaluation& e, unsigned& result);
// Every feature must be released first.
void shutdown(ID3D11Device* device);

}  // namespace simqvfr::ngx

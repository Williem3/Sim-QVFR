// NVIDIA's DLSS neural rendering ("DLSS-NR", NGX feature 18, nvngx_dlssnr.dll) called by us for a D3D11 game. The
// runtime works on D3D12 only, so this side owns a D3D12 device on the game's adapter: a view's inputs and output are
// textures both devices share (made on D3D12, opened on the game's D3D11 device), and a fence both share orders the
// work on the GPU without the CPU waiting: D3D11 fills the inputs and signals, D3D12 waits, evaluates and signals,
// D3D11 waits before it reads the output.
// NGX is started as engine/ngx_dlss.cpp starts it for DLSS, by NVIDIA's documented call with a project id of ours.
// The feature's parameter names ("DLSSNR.*") are the runtime's own; NVIDIA publishes no header for them, and
// tests/ngx_dlssnr_check.cpp is the check of each one used here against the runtime itself. nvngx_dlssnr.dll comes with
// games that ship DLSS-NR; it is the player's own copy, never part of this repository or of a release.
// Built only where CMake's NGX_SDK_DIR names NVIDIA's NGX SDK; elsewhere every call answers that it is unavailable.
#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IUnknown;

namespace simqvfr::ngxnr {

bool built_in();
// A D3D12 device on `device`'s adapter, the shared fence, and NGX started on it with nvngx_dlssnr.dll looked for in
// `dllFolder`; NGX's own files in `dataFolder`. False with the reason in `why`. Once per session; a second call answers
// as the first did (shutdown() forgets).
bool start(ID3D11Device* device, const wchar_t* dllFolder, const wchar_t* dataFolder, std::string& why);

// A view's feature at width x height (what the model works at) and its four shared textures at that size.
struct View;
// `preset`: the runtime's model (0 its default, 1 A, 2 B). Null with NGX's or Direct3D's result in `result`.
View* create(unsigned width, unsigned height, int preset, unsigned& result);
// The view is given up: nothing waits, and nothing of it is let go of yet on either device. Its feature and textures,
// and `keep` (the caller's own Direct3D 11 objects made on those textures: views, the textures they draw from), are
// destroyed by a later collect(), once both devices are through everything queued that touches them: the D3D12 queue
// by the evaluations' fence, the game's device by a second fence it signals on `context` here, after all it has queued.
// Letting go of either side under queued work is what must never happen: NVIDIA's driver answers with an error and a
// hung device (event 153, tests/nr_views_check.cpp part 3), and the first headset run ended in a Windows bugcheck right
// after a release that waited 5 s and then destroyed regardless.
void release(ID3D11DeviceContext* context, View* view, IUnknown* const* keep = nullptr, unsigned count = 0);
// Once a frame, between frames: destroys the released views whose work the GPU has finished.
void collect();
// Released views not destroyed yet (checks).
unsigned pending_releases();

// The view's textures on the game's device. D3D11 draws the three inputs and reads the output.
struct Textures {
    ID3D11Texture2D* colour = nullptr;   // RGBA16F, values 0..1 (the model clips what is above)
    ID3D11Texture2D* depth = nullptr;    // R32F
    ID3D11Texture2D* vectors = nullptr;  // RG16F: to where the texel was, as a fraction of the view (x right, y down)
    ID3D11Texture2D* output = nullptr;   // RGBA16F
};
Textures textures(View* view);

// The model's settings: taken at each evaluation (a changed one needs no new feature).
struct Evaluation {
    float intensity = 1;       // 0 the picture as it came .. 1
    float localTone = 1;       // 0..2
    float localStructure = 1;  // 0..2
    unsigned style = 0;        // 0 standard, 1 natural, 2 cinematic
    bool autoMask = false;
    bool depthInverted = false;
    bool reset = false;        // no history (the first frame, a cut, a region that jumped)
};
// The inputs as D3D11 has drawn them on `context` up to this call go through the model; what `context` does after this
// call sees the output. Nothing waits on the CPU. False with the result in `result` (the output then is not to be used).
bool evaluate(ID3D11DeviceContext* context, View* view, const Evaluation& e, unsigned& result);
// evaluate()'s `result` when nothing was submitted because the GPU has kBacklog evaluations of ours still to do: the
// view stays as it came this frame.
inline constexpr unsigned kBehind = 0x4e520001;
inline constexpr unsigned kBacklog = 24;  // six frames of four views

// This process's video memory on the game's graphics card, in MB: in use, and what Windows lets it use before it
// starts moving memory off the card. False: not known.
bool video_memory(double& usedMb, double& budgetMb);
// What a view worked at width x height takes, MB: its feature and four textures (measured with runtime 310.8.0.0,
// tests/ngx_dlssnr_check.cpp: 360 MB at 0.6 megapixels, 574 at 2.4, 979 at 5.4).
inline double view_memory_mb(unsigned width, unsigned height) { return 300.0 + 130.0 * (width * static_cast<double>(height) / 1e6); }

// The GPU time of `view`'s evaluations since the last call, in ms each on average (0: none measured yet).
double take_gpu_ms(View* view);

// The session's end, on the thread of the game's immediate `context`. Flushes it and waits a bounded time for both
// devices to finish what was queued, destroys the released views, shuts NGX on D3D12 down and releases the D3D12
// device. If they don't finish in that time, nothing is destroyed: the objects are left to the process's end (a leak,
// logged) rather than destroyed under work still queued.
void shutdown(ID3D11DeviceContext* context);

// The nvngx_dlssnr.dll NGX loaded into this process (its full path), empty while none is.
std::wstring loaded_dll();

}  // namespace simqvfr::ngxnr

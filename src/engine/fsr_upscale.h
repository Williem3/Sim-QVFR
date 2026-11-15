// AMD FSR upscaling (FSR 4 where AMD's runtime offers it, else FSR 3.1) called by us for a D3D11 game that ships none,
// next to the side-loaded DLSS (engine/dlss_views.h picks one; docs/recon-lmu.md "AMD FSR upscaling for LMU at the API
// level"). AMD's runtime works on D3D12 only, so this side owns a D3D12 device on the game's adapter, as DLSS-NR's
// (engine/ngx_nr.h) does: a view's inputs and output are textures both devices share (made on D3D12, opened on the
// game's D3D11 device) and a shared fence orders the work on the GPU without the CPU waiting: D3D11 fills the inputs and
// signals, D3D12 waits, runs FSR and signals, D3D11 waits before it reads the output.
// The runtime is AMD's signed amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll, loaded at run time
// (nothing of them linked); the provider (FSR 4.x, 3.1.x) is the one AMD's version query offers on this GPU.
// Built only where CMake's FFX_SDK_DIR names AMD's FSR SDK headers (not in this repository); elsewhere every call
// answers that it is unavailable.
#pragma once

#include <string>

#include <dxgiformat.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IUnknown;

namespace simqvfr::fsr {

bool built_in();
// The PCI vendor id of `device`'s adapter (0x1002 AMD, 0x10de NVIDIA, 0x8086 Intel); 0 when not known.
unsigned adapter_vendor(ID3D11Device* device);
inline constexpr unsigned kVendorAmd = 0x1002;

// Which of the runtime's providers: the newest it offers on this GPU (FSR 4 on the GPUs it supports, else 3.1), or the
// newest FSR 3.x even where 4 is offered (an A/B).
enum class Provider { Newest = 0, Fsr3 = 3 };

// A D3D12 device on `device`'s adapter, the shared fences, and AMD's runtime loaded: amd_fidelityfx_upscaler_dx12.dll
// and amd_fidelityfx_loader_dx12.dll looked for in `dllFolder` (the player's choice, may be empty), then beside the
// layer, then beside the game's exe (both from one folder). False with the reason in `why`. Once per session; a second
// call answers as the first did (shutdown() forgets).
bool start(ID3D11Device* device, const wchar_t* dllFolder, Provider provider, std::string& why);
// The providers the runtime offers on this GPU, as it names them ("FSR 4.1.1, FSR 3.1.5, ..."), and the one a context
// was made with (empty until one was).
std::string providers();
std::string provider_in_use();
// The folder the runtime was loaded from (empty while it isn't).
std::wstring loaded_folder();

// A view's FSR context, rendered at up to renderWidth x renderHeight and shown at outputWidth x outputHeight (the same:
// FSR's native AA; smaller: upscaling at that ratio, any ratio), and its four shared textures: colour and output in
// `format` (the game's scene format), depth R32F, motion vectors RG16F. Null with the runtime's or Direct3D's result
// in `result`.
struct View;
View* create(unsigned renderWidth, unsigned renderHeight, unsigned outputWidth, unsigned outputHeight, DXGI_FORMAT format,
             bool depthInverted, bool depthInfinite, unsigned& result);

// The view's textures on the game's device. D3D11 copies or draws the three inputs and reads the output.
struct Textures {
    ID3D11Texture2D* colour = nullptr;   // the view as drawn, at its render size (from the top-left)
    ID3D11Texture2D* depth = nullptr;    // R32F, render size
    ID3D11Texture2D* vectors = nullptr;  // RG16F, render size: to where the texel was, in pixels (x right, y down)
    ID3D11Texture2D* output = nullptr;   // the output size
};
Textures textures(View* view);

struct Evaluation {
    unsigned width = 0, height = 0;  // this frame's render size (at most the context's)
    float jitterX = 0, jitterY = 0;  // this frame's projection jitter, in render pixels (as handed to NGX)
    bool reset = false;              // no history (the first frame, a cut)
    float cameraNear = 0.1f, cameraFar = 10000.0f;  // metres
    float fovY = 1.5f;               // radians, the view's vertical field of view
    float frameMs = 11.1f;           // since the last frame
};
// The inputs as D3D11 has drawn them on `context` up to this call go through FSR; what `context` does after this call
// sees the output. Nothing waits on the CPU. False with the result in `result` (the output then is not to be used).
bool evaluate(ID3D11DeviceContext* context, View* view, const Evaluation& e, unsigned& result);
// evaluate()'s `result` when nothing was submitted because the GPU has kBacklog evaluations of ours still to do.
inline constexpr unsigned kBehind = 0x46535201;
inline constexpr unsigned kBacklog = 24;  // six frames of four views

// The view is given up: nothing waits, nothing of it is let go of yet on either device. Its context and textures, and
// `keep` (the caller's own Direct3D 11 objects on those textures), are destroyed by a later collect(), once both devices
// are through everything queued that touches them (engine/ngx_nr.h release(): letting go under queued work hangs
// NVIDIA's driver).
void release(ID3D11DeviceContext* context, View* view, IUnknown* const* keep = nullptr, unsigned count = 0);
// Once a frame, between frames: destroys the released views whose work the GPU has finished.
void collect();
unsigned pending_releases();

// Our Direct3D 12 device is lost: no evaluation succeeds until shutdown() and a new start().
bool device_lost();
// The GPU time of `view`'s evaluations since the last call, in ms each on average (0: none measured yet).
double take_gpu_ms(View* view);

// The session's end, on the thread of the game's immediate `context`: both devices given a bounded time to finish,
// every view destroyed, the runtime's DLLs left loaded, the D3D12 device released. If they don't finish in time,
// nothing is destroyed (left to the process's end, logged).
void shutdown(ID3D11DeviceContext* context);

}  // namespace simqvfr::fsr

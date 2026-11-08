#include "four_passes.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "../../probe/run_exit.h"
#include "../../sha256_file.h"
#include "abi.h"
#include "focus_math.h"
#include "game.h"

namespace lmu {
namespace {

using RenderSceneFn = void (*)(void* arg, char laterEye);
using TargetFn = void* (*)(void* renderer);
using SurfaceFn = void* (*)(void* texture, unsigned usage);
using D3DObjectFn = void* (*)(void* surface);
using HistoryCtorFn = void (*)(void* record);
using SetDeltaTimeFn = long (*)(void* postEffect, float seconds);
using MaskDrawFn = void (*)(void* mask, void* camera);

RenderSceneFn originalRenderScene = nullptr;
SetDeltaTimeFn originalSetDeltaTime = nullptr;
MaskDrawFn originalMaskDraw = nullptr;
bool installed = false;
FourPassesRun cfg;           // the run's settings (four_passes.h)
bool qvfrMode = false;       // QVFR: four passes every frame, composed (no samples)
bool focusMode = false;      // passes 2-3 are focus views (else copies of the eyes)

// The focus box (QVFR: live, from the settings and the gaze; under liveMutex): its share of the eye's tangent width and
// height (focusFractionY <= 0: as the width), and each eye's centre as tangents of its view (gazeValid false: the
// eye's forward axis).
std::mutex liveMutex;
double focusFraction = 0.3;
double focusFractionY = 0;
float gazeTan[2][2] = {};
bool gazeValid[2] = {};

// The eye loop's pass count, switched per frame: two (the stock loop, full frame rate) except in the short windows
// before each sample, where four. It's the immediate of the tail stub's last `cmp edi, n`, in our own stub memory
// (read-write-execute), written on the render thread between frames.
std::uint8_t* countImmediate = nullptr;
int passTarget = 2;  // the count in effect this frame
unsigned long long sceneFrames = 0;  // frames that rendered the 3D scene (the eye loop ran)
bool done = false;
int exitCountdown = -1;
std::vector<double> results;  // per sample and eye: focus / eye median (focus), differing pixels % (copies)

void set_pass_count(int n) {
    if (!countImmediate || passTarget == n)
        return;
    *countImmediate = static_cast<std::uint8_t>(n);
    FlushInstructionCache(GetCurrentProcess(), countImmediate, 1);
    passTarget = n;
}

// Passes 2-3 render with YEBIS's frame time held near zero, so its auto exposure (one state for both eyes) isn't
// advanced again by the extra passes: they use the exposure their eye just had, and a focus view can't pull it toward
// its box. Not 0: YEBIS treats <= 0 as paused and divides by it elsewhere (docs/recon-lmu.md "[Post-effect time]").
constexpr float kHeldDeltaTime = 1e-5f;
bool holdExposure = true;
bool inLaterPass = false;  // passes 2-3 are rendering (render thread)
int heldThisFrame = 0;
float lastGameDeltaTime = 0;

long set_delta_time(void* postEffect, float seconds) {
    if (inLaterPass && holdExposure) {
        lastGameDeltaTime = seconds;
        ++heldThisFrame;
        seconds = kHeldDeltaTime;
    }
    return originalSetDeltaTime(postEffect, seconds);
}

// The hidden-area (lens) mask is the eye's: in a focus view it would black out the view's own corners (the first focus
// run's rounded rectangle; catalogue row 13). Focus passes skip it.
int maskSkippedThisFrame = 0;

void mask_draw(void* mask, void* camera) {
    if (focusMode && inLaterPass) {
        ++maskSkippedThisFrame;
        return;
    }
    originalMaskDraw(mask, camera);
}

// Passes 2-3's camera history records (the game keeps two on its stack, one per eye).
alignas(16) std::uint8_t history[2][kCameraHistorySize];

// The render thread's view of the frame (render_scene and frame_end both run on it; checked).
unsigned long renderThread = 0;
int pass = -1;  // this frame's pass, 0-3
int passesThisFrame = 0;
int eyeOfPass[4] = {-1, -1, -1, -1};
unsigned long long frames = 0, fourPassFrames = 0, comparisons = 0;
bool armed = false;     // copies: copy each pass's image aside this frame
unsigned captured = 0;  // copies: bit p, pass p's image copied
ID3D11Texture2D* copies[4] = {};
bool loggedTarget = false;

std::uintptr_t base() { return simqvfr::module_base(); }

ID3D11Texture2D* d3d_texture(void* textureObject) {
    void* surface =
        textureObject ? reinterpret_cast<SurfaceFn>(base() + kTextureSurface)(textureObject, kRenderTargetUsage) : nullptr;
    auto* object = surface ? static_cast<IUnknown*>(reinterpret_cast<D3DObjectFn>(base() + kSurfaceD3D)(surface)) : nullptr;
    if (!object)
        return nullptr;
    ID3D11Texture2D* result = nullptr;
    ID3D11View* view = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11View), reinterpret_cast<void**>(&view)))) {
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        view->Release();
        if (resource) {
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&result));
            resource->Release();
        }
    } else {
        object->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&result));
    }
    return result;  // AddRef'd
}

// The texture the pass just finished in (the current eye target), AddRef'd; null if it can't be had.
ID3D11Texture2D* current_target(int& eye) {
    void* renderer = *reinterpret_cast<void**>(base() + kRenderer);
    if (!renderer)
        return nullptr;
    eye = *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewEye);
    return d3d_texture(reinterpret_cast<TargetFn>(base() + kGetCurrentEyeTarget)(renderer));
}

// Eye e's render texture (the eye-texture array), AddRef'd.
ID3D11Texture2D* eye_texture(int e) { return d3d_texture(*reinterpret_cast<void**>(base() + kEyeTextures + e * 8)); }

// `dst` (created or re-created to match) = `src`, on the immediate context.
bool copy_texture(ID3D11Texture2D*& dst, ID3D11Texture2D* src) {
    if (!src)
        return false;
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);
    if (!loggedTarget) {
        loggedTarget = true;
        simqvfr::logf("lmu four passes: eye target %ux%u format %d samples %u mips %u", desc.Width, desc.Height,
                      static_cast<int>(desc.Format), desc.SampleDesc.Count, desc.MipLevels);
    }
    ID3D11Device* device = nullptr;
    src->GetDevice(&device);
    if (dst) {
        D3D11_TEXTURE2D_DESC have;
        dst->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
            dst->Release();
            dst = nullptr;
        }
    }
    if (!dst) {
        D3D11_TEXTURE2D_DESC aside = desc;
        aside.MipLevels = 1;
        aside.ArraySize = 1;
        aside.Usage = D3D11_USAGE_DEFAULT;
        aside.BindFlags = 0;
        aside.CPUAccessFlags = 0;
        aside.MiscFlags = 0;
        if (desc.SampleDesc.Count != 1 || FAILED(device->CreateTexture2D(&aside, nullptr, &dst)))
            dst = nullptr;
    }
    bool ok = false;
    if (dst) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, nullptr);
        context->Release();
        ok = true;
    }
    device->Release();
    return ok;
}

// The other way: `dst` (an existing texture) = `src`.
void copy_back(ID3D11Texture2D* dst, ID3D11Texture2D* src) {
    if (!dst || !src)
        return;
    ID3D11Device* device = nullptr;
    dst->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, nullptr);
    context->Release();
    device->Release();
}

// ---- Focus views ----

// The projection slots' 4x4 (focus_math.h): the camera update copies its eye's two into the camera each pass.
float* slot_matrix(int eye, int kind) {
    return reinterpret_cast<float*>(base() + (kind ? kEyeProjectionFar : kEyeProjection) + eye * kEyeSlotStride +
                                    kSlotMatrix);
}

FocusBox boxes[2];
float saved[2][2][16];
bool swapped[2] = {};
bool shapeLogged = false;

bool swap_in_focus(int eye) {
    float eyeM[2][16], focusM[2][16];
    FocusBox box[2];
    double fx, fy, cx = 0, cy = 0;
    {
        std::lock_guard guard(liveMutex);
        fx = focusFraction;
        fy = focusFractionY > 0 ? focusFractionY : focusFraction;
        if (gazeValid[eye]) {
            cx = gazeTan[eye][0];
            cy = gazeTan[eye][1];
        }
    }
    for (int k = 0; k < 2; ++k)
        std::memcpy(eyeM[k], slot_matrix(eye, k), sizeof(eyeM[k]));
    // A share of the eye's tangent span: on the gaze, kept inside the eye (QVFR), else on the eye's forward axis (gates).
    clamp_focus_centre(eyeM[0], fx, fy, cx, cy);
    bool ok = true;
    for (int k = 0; k < 2; ++k)
        ok = focus_projection(eyeM[k], fx, fy, cx, cy, box[k], focusM[k]) && ok;
    if (!ok) {
        if (!shapeLogged) {
            shapeLogged = true;
            const float* m = eyeM[0];
            simqvfr::logf("lmu focus: eye %d's projection isn't the shape expected (m0 %g m5 %g m8 %g m9 %g m11 %g "
                          "m15 %g): the focus views render as copies",
                          eye, m[0], m[5], m[8], m[9], m[11], m[15]);
        }
        return false;
    }
    boxes[eye] = box[0];
    for (int k = 0; k < 2; ++k) {
        std::memcpy(saved[eye][k], eyeM[k], sizeof(saved[eye][k]));
        std::memcpy(slot_matrix(eye, k), focusM[k], sizeof(focusM[k]));
    }
    swapped[eye] = true;
    return true;
}

void restore_projection(int eye) {
    if (!swapped[eye])
        return;
    for (int k = 0; k < 2; ++k)
        std::memcpy(slot_matrix(eye, k), saved[eye][k], sizeof(saved[eye][k]));
    swapped[eye] = false;
}

ID3D11Texture2D* eyeSave[2] = {};     // the eyes' images while the focus views use their targets
ID3D11Texture2D* focusImage[2] = {};  // the focus views' images
bool focusFrameComplete = false;

void focus_after_pass(int p) {
    if (p == 1) {
        ID3D11Texture2D* e0 = eye_texture(0);
        ID3D11Texture2D* e1 = eye_texture(1);
        const bool saved0 = copy_texture(eyeSave[0], e0), saved1 = copy_texture(eyeSave[1], e1);
        if (e0)
            e0->Release();
        if (e1)
            e1->Release();
        focusFrameComplete = false;
        if (saved0 && saved1)
            swap_in_focus(0);
    } else if (p == 2) {
        ID3D11Texture2D* t = eye_texture(0);
        copy_texture(focusImage[0], t);
        if (t)
            t->Release();
        restore_projection(0);
        if (eyeSave[0] && eyeSave[1])
            swap_in_focus(1);
    } else if (p == 3) {
        ID3D11Texture2D* t0 = eye_texture(0);
        ID3D11Texture2D* t1 = eye_texture(1);
        copy_texture(focusImage[1], t1);
        restore_projection(1);
        // The eyes' own images back where the game submits them from.
        copy_back(t0, eyeSave[0]);
        copy_back(t1, eyeSave[1]);
        if (t0)
            t0->Release();
        if (t1)
            t1->Release();
        focusFrameComplete = true;
    }
}

// ---- QVFR: the atlas ----
// All four views of a frame in one texture for the layer's compositor: the eyes in the top row (eye e at x = e W), their
// focus views below. Typeless: the game's images copy in, and the compositor reads it in the swapchain's format.
ID3D11Texture2D* atlas = nullptr;
unsigned viewW = 0, viewH = 0;  // every view's size (the eye targets')
DXGI_FORMAT atlasFormat = DXGI_FORMAT_UNKNOWN;
bool eyesInAtlas = false;   // this frame's eyes are in the top row while the focus views use their targets
bool frameFocusOk = false;  // this frame's focus passes rendered with their focus projections
bool atlasReady = false;    // this frame's four views are in the atlas, until the layer takes it
unsigned long long composedFrames = 0, refusedFrames = 0;
bool atlasRefusalLogged = false, atlasThreadLogged = false;

DXGI_FORMAT typeless_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default:
        return f;
    }
}

bool is_view(ID3D11Texture2D* t) {
    if (!t)
        return false;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    return d.Width == viewW && d.Height == viewH && typeless_of(d.Format) == atlasFormat && d.SampleDesc.Count == 1;
}

// The atlas for views the size and format of `eyeTexture` (made again when they change).
bool ensure_atlas(ID3D11Texture2D* eyeTexture) {
    D3D11_TEXTURE2D_DESC desc;
    eyeTexture->GetDesc(&desc);
    const DXGI_FORMAT format = typeless_of(desc.Format);
    if (atlas && viewW == desc.Width && viewH == desc.Height && atlasFormat == format)
        return true;
    if (atlas) {
        atlas->Release();
        atlas = nullptr;
    }
    viewW = viewH = 0;
    if (desc.SampleDesc.Count != 1 || desc.Width > 8192 || desc.Height > 8192) {
        simqvfr::logf("lmu qvfr: the eye target is %ux%u with %u samples: no atlas, not composed", desc.Width, desc.Height,
                      desc.SampleDesc.Count);
        return false;
    }
    D3D11_TEXTURE2D_DESC a{};
    a.Width = 2 * desc.Width;
    a.Height = 2 * desc.Height;
    a.MipLevels = 1;
    a.ArraySize = 1;
    a.Format = format;
    a.SampleDesc = {1, 0};
    a.Usage = D3D11_USAGE_DEFAULT;
    a.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device* device = nullptr;
    eyeTexture->GetDevice(&device);
    const HRESULT hr = device->CreateTexture2D(&a, nullptr, &atlas);
    device->Release();
    if (FAILED(hr)) {
        atlas = nullptr;
        simqvfr::logf("lmu qvfr: the atlas %ux%u (format %d) couldn't be made (0x%08lx): not composed", a.Width, a.Height,
                      static_cast<int>(format), static_cast<unsigned long>(hr));
        return false;
    }
    viewW = desc.Width;
    viewH = desc.Height;
    atlasFormat = format;
    simqvfr::logf("lmu qvfr: atlas %ux%u, format %d (views %ux%u, the eye targets' format %d)", a.Width, a.Height,
                  static_cast<int>(format), viewW, viewH, static_cast<int>(desc.Format));
    return true;
}

// The view `src` into the atlas at (x, y), or (atlas_out) the atlas's view at (x, y) into `dst`: immediate context.
void atlas_in(ID3D11Texture2D* src, unsigned x, unsigned y) {
    ID3D11Device* device = nullptr;
    atlas->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX whole{0, 0, 0, viewW, viewH, 1};
    context->CopySubresourceRegion(atlas, 0, x, y, 0, src, 0, &whole);
    context->Release();
    device->Release();
}

void atlas_out(ID3D11Texture2D* dst, unsigned x, unsigned y) {
    ID3D11Device* device = nullptr;
    atlas->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX box{x, y, 0, x + viewW, y + viewH, 1};
    context->CopySubresourceRegion(dst, 0, 0, 0, 0, atlas, 0, &box);
    context->Release();
    device->Release();
}

// The frame's record for the layer (adapter.h Views4Frame: tangents left, right, up, down; the focus views off-centre,
// unturned). Under liveMutex.
simqvfr::Views4Frame latestRecord{};
bool haveRecord = false;

void store_record() {
    simqvfr::Views4Frame f{};
    for (int e = 0; e < 2; ++e) {
        const FocusBox& b = boxes[e];
        const float outer[4] = {b.eyeTan[0], b.eyeTan[1], b.eyeTan[3], b.eyeTan[2]};
        const float focus[4] = {b.focusTan[0], b.focusTan[1], b.focusTan[3], b.focusTan[2]};
        std::memcpy(f.outerTan[e], outer, sizeof(outer));
        std::memcpy(f.focusTan[e], focus, sizeof(focus));
        f.turn[e] = {0, 0, 0, 1};
    }
    std::lock_guard guard(liveMutex);
    latestRecord = f;
    haveRecord = true;
}

// QVFR, after pass p: the eyes go into the atlas's top row while the focus views render into their targets (pass 2 =
// focus 0 in eye 0's, pass 3 = focus 1 in eye 1's), each focus view into the row below, then the eyes back for the
// game's HUD and submit.
void qvfr_after_pass(int p) {
    if (p == 1) {
        eyesInAtlas = frameFocusOk = false;
        ID3D11Texture2D* e0 = eye_texture(0);
        ID3D11Texture2D* e1 = eye_texture(1);
        if (e0 && e1 && ensure_atlas(e0) && is_view(e1)) {
            atlas_in(e0, 0, 0);
            atlas_in(e1, viewW, 0);
            eyesInAtlas = true;
            frameFocusOk = swap_in_focus(0);
        }
        if (e0)
            e0->Release();
        if (e1)
            e1->Release();
    } else if (p == 2 && eyesInAtlas) {
        ID3D11Texture2D* t = eye_texture(0);
        if (frameFocusOk && is_view(t))
            atlas_in(t, 0, viewH);
        else
            frameFocusOk = false;
        if (t)
            t->Release();
        restore_projection(0);
        if (frameFocusOk)
            frameFocusOk = swap_in_focus(1);
    } else if (p == 3 && eyesInAtlas) {
        ID3D11Texture2D* t0 = eye_texture(0);
        ID3D11Texture2D* t1 = eye_texture(1);
        if (frameFocusOk && is_view(t1))
            atlas_in(t1, viewW, viewH);
        else
            frameFocusOk = false;
        restore_projection(1);
        if (is_view(t0))
            atlas_out(t0, 0, 0);
        if (is_view(t1))
            atlas_out(t1, viewW, 0);
        if (t0)
            t0->Release();
        if (t1)
            t1->Release();
        eyesInAtlas = false;
        atlasReady = frameFocusOk;
        if (atlasReady)
            store_record();
    }
}

// ---- Copies ----

void copy_aside(int p) {
    int eye = -1;
    ID3D11Texture2D* source = current_target(eye);
    if (!source) {
        simqvfr::logf("lmu copies: pass %d: the eye target's texture can't be had; not compared", p);
        return;
    }
    if (copy_texture(copies[p], source))
        captured |= 1u << p;
    source->Release();
}

void render_scene(void* arg, char laterEye) {
    renderThread = GetCurrentThreadId();
    pass = laterEye ? pass + 1 : 0;
    const int p = pass;
    ++passesThisFrame;
    if (p == 0)
        atlasReady = false;  // a new frame: its own four views, or none
    inLaterPass = p >= 2;
    originalRenderScene(arg, laterEye);
    inLaterPass = false;
    if (p < 4) {
        void* renderer = *reinterpret_cast<void**>(base() + kRenderer);
        eyeOfPass[p] = renderer ? *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewEye)
                                : -1;
        if (qvfrMode)
            qvfr_after_pass(p);
        else if (focusMode && passTarget == 4)
            focus_after_pass(p);
        else if (armed)
            copy_aside(p);
    }
}

std::wstring captures_folder() {
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

// Half size, for looking at: the full images are 50 MB each.
std::vector<std::uint8_t> half(const std::vector<std::uint8_t>& px, unsigned w, unsigned h, unsigned& hw, unsigned& hh) {
    hw = w / 2;
    hh = h / 2;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(hw) * hh * 4);
    for (unsigned y = 0; y < hh; ++y)
        for (unsigned x = 0; x < hw; ++x)
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](unsigned xx, unsigned yy) {
                    return static_cast<unsigned>(px[(static_cast<std::size_t>(yy) * w + xx) * 4 + c]);
                };
                out[(static_cast<std::size_t>(y) * hw + x) * 4 + c] = static_cast<std::uint8_t>(
                    (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
            }
    return out;
}

void compare_copies() {
    std::vector<std::uint8_t> px[4];
    unsigned w[4] = {}, h[4] = {};
    for (int p = 0; p < 4; ++p)
        if (!simqvfr::d3d11_read_bgra8(copies[p], px[p], w[p], h[p])) {
            simqvfr::logf("lmu copies: pass %d's image can't be read back; not compared", p);
            return;
        }
    ++comparisons;
    const std::wstring dir = captures_folder();
    for (int eye = 0; eye < 2; ++eye) {
        const int a = eye, b = eye + 2;
        if (w[a] != w[b] || h[a] != h[b]) {
            simqvfr::logf("lmu copies: frame %llu: eye %d is %ux%u, pass %d %ux%u: not compared", frames, eye, w[a], h[a], b,
                          w[b], h[b]);
            continue;
        }
        unsigned long long differing = 0, sum = 0;
        long long signedSum[3] = {};  // pass minus eye, per channel (B, G, R): one sign everywhere is an exposure shift
        int maxDiff = 0;
        unsigned x0 = w[a], y0 = h[a], x1 = 0, y1 = 0;
        std::vector<std::uint8_t> diff(px[a].size(), 0);
        for (unsigned y = 0; y < h[a]; ++y)
            for (unsigned x = 0; x < w[a]; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * w[a] + x) * 4;
                int d = 0;
                for (int c = 0; c < 3; ++c) {
                    const int s = static_cast<int>(px[b][i + c]) - static_cast<int>(px[a][i + c]);
                    signedSum[c] += s;
                    d = (std::max)(d, std::abs(s));
                }
                if (d) {
                    ++differing;
                    sum += d;
                    maxDiff = (std::max)(maxDiff, d);
                    x0 = (std::min)(x0, x);
                    y0 = (std::min)(y0, y);
                    x1 = (std::max)(x1, x);
                    y1 = (std::max)(y1, y);
                }
                const auto v = static_cast<std::uint8_t>(d ? (std::min)(255, 64 + d * 16) : 0);
                diff[i] = diff[i + 1] = diff[i + 2] = v;
                diff[i + 3] = 255;
            }
        const double total = static_cast<double>(w[a]) * h[a];
        results.push_back(100.0 * differing / total);
        if (differing == 0)
            simqvfr::logf("lmu copies: frame %llu: pass %d equals eye %d pixel for pixel (%ux%u, eyes rendered %d/%d)", frames,
                          b, eye, w[a], h[a], eyeOfPass[a], eyeOfPass[b]);
        else
            simqvfr::logf("lmu copies: frame %llu: pass %d DIFFERS from eye %d: %llu pixels (%.3f%%), max %d, mean %.2f where "
                          "different, within x %u-%u y %u-%u (%ux%u; eyes rendered %d/%d); pass minus eye summed over "
                          "the image R %+lld G %+lld B %+lld",
                          frames, b, eye, differing, 100.0 * differing / total, maxDiff,
                          static_cast<double>(sum) / differing, x0, x1, y0, y1, w[a], h[a], eyeOfPass[a], eyeOfPass[b],
                          signedSum[2], signedSum[1], signedSum[0]);
        // Images: on the first comparison, and the first time a pair differs.
        static bool wroteDiff[2] = {};
        if (comparisons == 1 || (differing && !wroteDiff[eye])) {
            wchar_t name[96];
            unsigned hw = 0, hh = 0;
            // The half image first: its size comes back through hw/hh, and argument order is unspecified (the first
            // build wrote 0-byte eye images, sized 0x0).
            const std::vector<std::uint8_t> eyeHalf = half(px[a], w[a], h[a], hw, hh);
            swprintf_s(name, L"\\lmu-copies-frame%llu-eye%d.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, eyeHalf, hw, hh);
            const std::vector<std::uint8_t> passHalf = half(px[b], w[b], h[b], hw, hh);
            swprintf_s(name, L"\\lmu-copies-frame%llu-pass%d.png", frames, b);
            simqvfr::write_png_bgra(dir + name, passHalf, hw, hh);
            if (differing) {
                wroteDiff[eye] = true;
                swprintf_s(name, L"\\lmu-copies-frame%llu-diff%d.png", frames, eye);
                simqvfr::write_png_bgra(dir + name, diff, w[a], h[a]);
            }
            simqvfr::logf("lmu copies: images in %ls (half size; the diff map full size, brighter = larger difference)",
                          dir.c_str());
        }
    }
}

double linear(std::uint8_t v) {  // sRGB-encoded 8-bit to linear
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// The focus view against its eye's crop: linear luminance in a grid of cells over the box, focus / eye per cell.
void compare_focus() {
    ++comparisons;
    const std::wstring dir = captures_folder();
    for (int eye = 0; eye < 2; ++eye) {
        const FocusBox& b = boxes[eye];
        std::vector<std::uint8_t> ep, fp;
        unsigned ew = 0, eh = 0, fw = 0, fh = 0;
        if (!b.valid || !simqvfr::d3d11_read_bgra8(eyeSave[eye], ep, ew, eh) ||
            !simqvfr::d3d11_read_bgra8(focusImage[eye], fp, fw, fh)) {
            simqvfr::logf("lmu focus: frame %llu: eye %d's images can't be read back; not compared", frames, eye);
            continue;
        }
        constexpr int kCells = 16;
        std::vector<double> ratios;
        double sumE = 0, sumF = 0;
        for (int cy = 1; cy < kCells - 1; ++cy)  // the outer ring left out: edge pixels and the box's own border
            for (int cx = 1; cx < kCells - 1; ++cx) {
                double le = 0, lf = 0;
                int ne = 0, nf = 0;
                const double u0 = static_cast<double>(cx) / kCells, u1 = static_cast<double>(cx + 1) / kCells;
                const double v0 = static_cast<double>(cy) / kCells, v1 = static_cast<double>(cy + 1) / kCells;
                const auto lum = [](const std::vector<std::uint8_t>& px, std::size_t i) {
                    return 0.0722 * linear(px[i]) + 0.7152 * linear(px[i + 1]) + 0.2126 * linear(px[i + 2]);
                };
                for (unsigned y = static_cast<unsigned>(v0 * fh); y < static_cast<unsigned>(v1 * fh); y += 2)
                    for (unsigned x = static_cast<unsigned>(u0 * fw); x < static_cast<unsigned>(u1 * fw); x += 2) {
                        lf += lum(fp, (static_cast<std::size_t>(y) * fw + x) * 4);
                        ++nf;
                    }
                const double ex0 = (b.rect[0] + (b.rect[2] - b.rect[0]) * u0) * ew, ex1 = (b.rect[0] + (b.rect[2] - b.rect[0]) * u1) * ew;
                const double ey0 = (b.rect[1] + (b.rect[3] - b.rect[1]) * v0) * eh, ey1 = (b.rect[1] + (b.rect[3] - b.rect[1]) * v1) * eh;
                for (unsigned y = static_cast<unsigned>(ey0); y < static_cast<unsigned>(ey1) && y < eh; ++y)
                    for (unsigned x = static_cast<unsigned>(ex0); x < static_cast<unsigned>(ex1) && x < ew; ++x) {
                        le += lum(ep, (static_cast<std::size_t>(y) * ew + x) * 4);
                        ++ne;
                    }
                if (ne && nf) {
                    le /= ne;
                    lf /= nf;
                    sumE += le;
                    sumF += lf;
                    if (le > 0.002)
                        ratios.push_back(lf / le);
                }
            }
        std::sort(ratios.begin(), ratios.end());
        if (!ratios.empty())
            results.push_back(ratios[ratios.size() / 2]);
        const auto pct = [&](double q) { return ratios.empty() ? 0.0 : ratios[static_cast<std::size_t>(q * (ratios.size() - 1))]; };
        simqvfr::logf("lmu focus: frame %llu: eye %d: focus / eye linear luminance over the box %.3f; per cell median %.3f, "
                      "p10 %.3f, p90 %.3f (%zu cells); box in the eye x %.3f-%.3f y %.3f-%.3f of %ux%u; eye tangents "
                      "l %.3f r %.3f d %.3f u %.3f, focus l %.3f r %.3f d %.3f u %.3f; focus image %ux%u",
                      frames, eye, sumE > 0 ? sumF / sumE : 0.0, pct(0.5), pct(0.1), pct(0.9), ratios.size(), b.rect[0],
                      b.rect[2], b.rect[1], b.rect[3], ew, eh, b.eyeTan[0], b.eyeTan[1], b.eyeTan[2], b.eyeTan[3],
                      b.focusTan[0], b.focusTan[1], b.focusTan[2], b.focusTan[3], fw, fh);
        if (comparisons == 1 && eye == 0) {  // full-size images for tools/focus_compare.py (once: ~2 s each)
            wchar_t name[96];
            swprintf_s(name, L"\\lmu-focus-frame%llu-eye%d.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, ep, ew, eh);
            swprintf_s(name, L"\\lmu-focus-frame%llu-focus%d.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, fp, fw, fh);
            simqvfr::logf("lmu focus: images in %ls: python tools/focus_compare.py <eye png> <focus png> %.5f %.5f %.5f %.5f",
                          dir.c_str(), b.rect[0], b.rect[1], b.rect[2], b.rect[3]);
        }
    }
}

const char* tag() { return qvfrMode ? "lmu qvfr" : focusMode ? "lmu focus" : "lmu copies"; }

}  // namespace

bool four_passes_start(const FourPassesRun& run) {
    if (installed)
        return true;
    cfg = run;
    cfg.count = std::clamp(cfg.count, 1, 16);
    cfg.every = (std::max)(cfg.every, 1);
    cfg.warmup = std::clamp(cfg.warmup, 1, 600);
    qvfrMode = run.qvfr;
    focusMode = run.focus || run.qvfr;
    holdExposure = run.holdExposure;
    four_passes_set_box(run.fraction, run.fractionY);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!simqvfr::games::supported_build(kGame, simqvfr::sha256_file(exe))) {
        simqvfr::logf("%s: not the build its addresses are from; not installed", tag());
        return false;
    }
    struct Anchor {
        std::uintptr_t rva;
        const std::uint8_t* bytes;
        std::size_t size;
        const char* name;
    };
    const Anchor anchors[] = {
        {kEyeLoopHead, kEyeLoopHeadAnchor, sizeof(kEyeLoopHeadAnchor), "eye loop head"},
        {kEyeLoopTail, kEyeLoopTailAnchor, sizeof(kEyeLoopTailAnchor), "eye loop tail"},
        {kCameraHistoryCtor, kCameraHistoryCtorAnchor, sizeof(kCameraHistoryCtorAnchor), "camera history constructor"},
        {kRenderScene, kRenderSceneAnchor, sizeof(kRenderSceneAnchor), "scene render"},
        {kGetCurrentEyeTarget, kGetCurrentEyeTargetAnchor, sizeof(kGetCurrentEyeTargetAnchor), "current eye target"},
        {kTextureSurface, kTextureSurfaceAnchor, sizeof(kTextureSurfaceAnchor), "texture surface"},
        {kSurfaceD3D, kSurfaceD3DAnchor, sizeof(kSurfaceD3DAnchor), "surface's D3D11 object"},
        {kPostEffectSetDeltaTime, kPostEffectSetDeltaTimeAnchor, sizeof(kPostEffectSetDeltaTimeAnchor),
         "post-effect frame time"},
    };
    for (const Anchor& a : anchors)
        if (!simqvfr::code_matches(a.rva, a.bytes, a.size)) {
            simqvfr::logf("%s: the %s isn't this build's; not installed", tag(), a.name);
            return false;
        }
    for (auto& record : history)
        reinterpret_cast<HistoryCtorFn>(base() + kCameraHistoryCtor)(record);
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        simqvfr::logf("%s: MH_Initialize failed", tag());
        return false;
    }
    void* target = reinterpret_cast<void*>(base() + kRenderScene);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&render_scene), reinterpret_cast<void**>(&originalRenderScene)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        simqvfr::logf("%s: the scene render couldn't be hooked; not installed", tag());
        return false;
    }
    void* deltaTime = reinterpret_cast<void*>(base() + kPostEffectSetDeltaTime);
    if (MH_CreateHook(deltaTime, reinterpret_cast<void*>(&set_delta_time),
                      reinterpret_cast<void**>(&originalSetDeltaTime)) != MH_OK ||
        MH_EnableHook(deltaTime) != MH_OK) {
        MH_DisableHook(target);
        simqvfr::logf("%s: the post-effect frame time couldn't be hooked; not installed", tag());
        return false;
    }
    if (focusMode) {
        void* mask = reinterpret_cast<void*>(base() + kHiddenAreaDraw);
        if (!simqvfr::code_matches(kHiddenAreaDraw, kHiddenAreaDrawAnchor, sizeof(kHiddenAreaDrawAnchor)) ||
            MH_CreateHook(mask, reinterpret_cast<void*>(&mask_draw), reinterpret_cast<void**>(&originalMaskDraw)) != MH_OK ||
            MH_EnableHook(mask) != MH_OK) {
            MH_DisableHook(deltaTime);
            MH_DisableHook(target);
            simqvfr::log_line("lmu focus: the hidden-area mask's draw isn't this build's or couldn't be hooked; not installed");
            return false;
        }
    }
    // The head first: with two passes `edi & 1` is `edi`, so it changes nothing until the tail counts to four.
    // Head: the camera update gets the eye as edi & 1 (passes 2-3 set up eyes 0-1's cameras, not eyes 2-3).
    static const std::uint8_t head[] = {0x44, 0x8b, 0xc7,        // mov r8d, edi
                                        0x41, 0x83, 0xe0, 0x01,  // and r8d, 1
                                        0xb2, 0x01};             // mov dl, 1
    // Tail: count to four; from pass 2 on, rbx points at our history records instead of past the game's two.
    std::uint8_t tail[] = {0xff, 0xc7,                          // inc edi
                           0x83, 0xff, 0x02,                    // cmp edi, 2
                           0x75, 0x0c,                          // jne add
                           0x48, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rbx, history
                           0xeb, 0x04,                          // jmp cmp4
                           0x48, 0x83, 0xc3, 0x6c,              // add: add rbx, 0x6c
                           0x83, 0xff, 0x04};                   // cmp4: cmp edi, 4 (the game's jl follows)
    const auto records = reinterpret_cast<std::uint64_t>(&history[0][0]);
    std::memcpy(tail + 9, &records, 8);
    if (!simqvfr::code_redirect(kEyeLoopHead, kEyeLoopHeadAnchor, sizeof(kEyeLoopHeadAnchor), head, sizeof(head))) {
        MH_DisableHook(target);
        simqvfr::logf("%s: the eye loop head couldn't be patched; not installed", tag());
        return false;
    }
    if (!simqvfr::code_redirect(kEyeLoopTail, kEyeLoopTailAnchor, sizeof(kEyeLoopTailAnchor), tail, sizeof(tail))) {
        simqvfr::logf("%s: the eye loop tail couldn't be patched; two passes as before", tag());
        return false;
    }
    // The tail's stub (the jump at the site) ends `cmp edi, 4`: its immediate is the pass count. Start at two.
    const auto* site = reinterpret_cast<const std::uint8_t*>(base() + kEyeLoopTail);
    std::int32_t rel = 0;
    std::memcpy(&rel, site + 1, 4);
    auto* stub = const_cast<std::uint8_t*>(site + 5 + rel);
    if (site[0] == 0xE9 && stub[23] == 0x83 && stub[24] == 0xff && stub[25] == 0x04) {
        countImmediate = stub + 25;
        passTarget = 4;
        if (!qvfrMode)  // QVFR: four every frame
            set_pass_count(2);
    } else if (!qvfrMode) {
        simqvfr::logf("%s: the loop tail's stub isn't where expected: four passes every frame", tag());
    }
    installed = true;
    if (qvfrMode) {
        std::lock_guard guard(liveMutex);
        simqvfr::logf("lmu qvfr: on: the eye loop runs four times every frame, passes 2-3 render focus views (a %.0f%% x "
                      "%.0f%% box of each eye's tangent span, on the gaze) into the eye targets, set aside into the atlas the "
                      "layer composes; post-effect frame time held in passes 2-3: %s; hidden-area mask off in them",
                      focusFraction * 100, (focusFractionY > 0 ? focusFractionY : focusFraction) * 100,
                      holdExposure ? "yes" : "no");
        return true;
    }
    simqvfr::logf("%s: samples: %d, the first %d scene frames in, then every %d; four passes for %d frames before each "
                  "(else two, full frame rate)%s",
                  tag(), cfg.count, cfg.first, cfg.every, cfg.warmup,
                  cfg.exitWhenDone ? "; the game is asked to close after the last (exit_when_done=1)" : "");
    if (focusMode)
        simqvfr::logf("lmu focus: on: the eye loop runs four times, passes 2-3 render focus views (a %.0f%% box of each "
                      "eye's tangent span, centred on its forward axis) into the eye targets, set aside before the game "
                      "submits; the eyes are shown as normal; frame time held: %s",
                      cfg.fraction * 100, holdExposure ? "yes" : "no");
    else
        simqvfr::logf("lmu copies: on: the eye loop runs four times, passes 2-3 render eyes 0-1 again and are compared "
                      "with them; passes 2-3's post-effect frame time %s",
                      holdExposure ? "held near zero (lmu_copies_hold_exposure=1)" : "as the game sets it (hold off)");
    return true;
}

void four_passes_frame_end() {
    if (!installed)
        return;
    ++frames;
    // Never leave a focus projection in an eye's slot past the frame (a frame that stopped short of pass 3).
    for (int e = 0; e < 2; ++e)
        if (swapped[e]) {
            restore_projection(e);
            simqvfr::logf("lmu focus: frame %llu: eye %d's projection was still the focus box at the frame's end; restored",
                          frames, e);
        }
    const int passes = passesThisFrame;
    passesThisFrame = 0;
    if (passes >= 2)
        ++sceneFrames;
    if (passes >= 4)
        ++fourPassFrames;
    if (fourPassFrames == 1 && passes >= 4)
        simqvfr::logf("%s: frame %llu: first four-pass frame, eyes rendered %d %d %d %d; post-effect frame time "
                      "held in passes 2-3: %s (%d calls this frame, the game's %.5f s -> %.5f s); hidden-area mask "
                      "skipped in %d draws",
                      tag(), frames, eyeOfPass[0], eyeOfPass[1], eyeOfPass[2], eyeOfPass[3], holdExposure ? "yes" : "no",
                      heldThisFrame, lastGameDeltaTime, kHeldDeltaTime, maskSkippedThisFrame);
    const int maskSkipped = maskSkippedThisFrame;
    maskSkippedThisFrame = 0;
    const int held = heldThisFrame;
    heldThisFrame = 0;

    if (qvfrMode) {
        if (passes < 4)
            atlasReady = false;  // no four views this frame (menus, loading): the layer submits the eyes as they are
        if (eyesInAtlas) {
            eyesInAtlas = false;
            simqvfr::logf("lmu qvfr: frame %llu stopped after %d passes with the eyes set aside", frames, passes);
        }
        if (frames % 1800 == 0)
            simqvfr::logf("lmu qvfr: frame %llu: %llu scene frames, %llu four-pass, %llu composed, %llu not (the eye images "
                          "unlike the views); last frame %d passes, frame time held in %d calls, mask skipped in %d; "
                          "views %ux%u",
                          frames, sceneFrames, fourPassFrames, composedFrames, refusedFrames, passes, held, maskSkipped,
                          viewW, viewH);
        return;
    }

    // The sample: due once the scene has run `first + k * every` frames, after `warmup` four-pass frames.
    const unsigned long long dueAt = cfg.first + static_cast<unsigned long long>(comparisons) * cfg.every;
    const bool onRenderThread = GetCurrentThreadId() == renderThread;
    if (!done && passes >= 4 && sceneFrames >= dueAt) {
        if (focusMode) {
            if (focusFrameComplete) {
                if (onRenderThread)
                    compare_focus();
                else {
                    simqvfr::log_line("lmu focus: xrEndFrame isn't on the render thread; images not read back");
                    ++comparisons;
                }
            }
        } else if (armed) {
            armed = false;
            if (!onRenderThread) {
                simqvfr::log_line("lmu copies: xrEndFrame isn't on the render thread; images not read back");
                ++comparisons;
            } else if (captured == 0xf) {
                compare_copies();
            } else {
                simqvfr::logf("lmu copies: frame %llu: only passes 0x%x copied aside (%d passes); not compared", frames,
                              captured, passes);
                ++comparisons;
            }
        } else {
            armed = true;  // the next frame's passes are copied aside
            captured = 0;
        }
    }
    if (!done && comparisons >= static_cast<unsigned long long>(cfg.count)) {
        done = true;
        double lo = 1e9, hi = -1e9;
        for (double r : results) {
            lo = (std::min)(lo, r);
            hi = (std::max)(hi, r);
        }
        if (focusMode)
            simqvfr::logf("lmu focus: done: %llu samples; focus / eye brightness (median per cell) %.3f-%.3f: %s", comparisons,
                          results.empty() ? 0.0 : lo, results.empty() ? 0.0 : hi,
                          !results.empty() && lo >= 0.97 && hi <= 1.03 ? "within 3%" : "OUT OF RANGE (or not measured)");
        else
            simqvfr::logf("lmu copies: done: %llu samples; pixels differing %.3f-%.3f%%", comparisons,
                          results.empty() ? 0.0 : lo, results.empty() ? 0.0 : hi);
        exitCountdown = cfg.exitWhenDone ? 45 : -1;  // a few frames at two passes first
    }
    // Next frame's pass count: four from `warmup` frames before the sample until it's taken, else two.
    const unsigned long long nextDue = cfg.first + static_cast<unsigned long long>(comparisons) * cfg.every;
    set_pass_count(!done && sceneFrames + cfg.warmup >= nextDue ? 4 : 2);
    if (exitCountdown > 0 && --exitCountdown == 0)
        simqvfr::request_game_exit(focusMode ? "the focus gate has its samples" : "the copies gate has its samples");

    if (frames % 1800 == 0)
        simqvfr::logf("%s: frame %llu: %llu scene frames, %llu four-pass; last frame %d passes, eyes %d %d %d %d, "
                      "frame time held in %d calls, mask skipped in %d",
                      tag(), frames, sceneFrames, fourPassFrames, passes, eyeOfPass[0], eyeOfPass[1], eyeOfPass[2],
                      eyeOfPass[3], held, maskSkipped);
}

void four_passes_set_gaze(const float (&tan)[2][2], const bool (&valid)[2]) {
    std::lock_guard guard(liveMutex);
    for (int e = 0; e < 2; ++e) {
        gazeValid[e] = valid[e] && std::isfinite(tan[e][0]) && std::isfinite(tan[e][1]);
        gazeTan[e][0] = tan[e][0];
        gazeTan[e][1] = tan[e][1];
    }
}

void four_passes_set_box(double fractionX, double fractionY) {
    std::lock_guard guard(liveMutex);
    focusFraction = std::clamp(fractionX, 0.05, 1.0);
    focusFractionY = fractionY > 0 ? std::clamp(fractionY, 0.05, 1.0) : 0.0;
}

bool four_passes_record(simqvfr::Views4Frame& frame) {
    std::lock_guard guard(liveMutex);
    if (!haveRecord)
        return false;
    frame = latestRecord;
    return true;
}

ID3D11Texture2D* four_passes_atlas(ID3D11Texture2D* const (&eyes)[2], float (&uv)[4][4]) {
    if (!qvfrMode || !atlasReady || !atlas)
        return nullptr;
    atlasReady = false;  // once per frame
    if (!is_view(eyes[0]) || !is_view(eyes[1])) {
        ++refusedFrames;
        if (!atlasRefusalLogged) {
            atlasRefusalLogged = true;
            D3D11_TEXTURE2D_DESC d{};
            if (eyes[0])
                eyes[0]->GetDesc(&d);
            simqvfr::logf("lmu qvfr: the game's eye image is %ux%u format %d, the views %ux%u format %d: not composed "
                          "(the headset shows the eyes)",
                          d.Width, d.Height, static_cast<int>(d.Format), viewW, viewH, static_cast<int>(atlasFormat));
        }
        return nullptr;
    }
    if (GetCurrentThreadId() != renderThread && !atlasThreadLogged) {
        atlasThreadLogged = true;
        simqvfr::log_line("lmu qvfr: xrEndFrame isn't on the thread that rendered the views (the atlas copies race)");
    }
    // The eyes again, as the game submitted them: with what it drew after its eye loop (the HUD).
    atlas_in(eyes[0], 0, 0);
    atlas_in(eyes[1], viewW, 0);
    for (int e = 0; e < 2; ++e) {
        const float outer[4] = {0.5f * e, 0.0f, 0.5f, 0.5f}, focus[4] = {0.5f * e, 0.5f, 0.5f, 0.5f};
        std::memcpy(uv[e], outer, sizeof(outer));
        std::memcpy(uv[e + 2], focus, sizeof(focus));
    }
    if (composedFrames++ == 0)
        simqvfr::logf("lmu qvfr: frame %llu: the first four views handed to the compositor", frames);
    return atlas;
}

void four_passes_view_size(int (&size)[2]) {
    size[0] = static_cast<int>(viewW);
    size[1] = static_cast<int>(viewH);
}

}  // namespace lmu

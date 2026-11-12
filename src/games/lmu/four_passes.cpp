#include "four_passes.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
#include "../../quad/d3d11_view_resample.h"
#include "../../sha256_file.h"
#include "abi.h"
#include "../../engine/dlss_math.h"
#include "../../engine/dlss_views.h"
#include "../../engine/ngx_dlss.h"
#include "../../engine/nvapi_gpus.h"
#include "focus_math.h"
#include "game.h"
#include "glare_share.h"
#include "mvr_pass.h"
#include "mvr_probe.h"
#include "tag_viewport.h"

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
bool glareHooked = false;    // QVFR: the focus views can take their eye's glare (glare_share.h)
// The frame capture hotkey (d3d11_capture_hotkey=1): the arm last acted on (capture_views), and as this frame's pass 0
// read it (render_scene), so the views and the glare read back pass by pass are of one frame.
std::uint64_t seenCaptureArm = 0;
std::uint64_t frameCaptureArm = 0;
bool capture_due(std::uint64_t arm) { return arm != seenCaptureArm && arm != 0 && arm != 0xFFFFFFFFull; }

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
bool inPass = false;       // a pass of the eye loop is rendering (render thread)
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
// run's rounded rectangle; catalogue row 13). Focus passes skip it. The four passes' hook; the multi-view path handles
// the mask's draw at the d3d11 level instead (mvr_pass.h mvr_mask_stale).
int maskSkippedThisFrame = 0;

// QVFR: the mask's own FOV. VR_Init turns the runtime's mesh (tangents) into screen space with the FOV it just located,
// so the mask fits only that FOV: after a live FOV crop change it would black out what the lens shows. Under liveMutex,
// except maskStale (render thread reads).
float locatedTan[2][4] = {};  // the latest located FOV per eye: tangents left, right, down, up
bool locatedValid = false;
float maskTan[2][4] = {};
bool maskKnown[2] = {};
std::atomic<bool> maskStale{false};
bool maskStaleLogged = false;
int maskStaleThisFrame = 0;

void mask_draw(void* mask, void* camera) {
    if (focusMode && inLaterPass) {
        ++maskSkippedThisFrame;
        return;
    }
    if (qvfrMode && maskStale.load(std::memory_order_relaxed)) {
        ++maskStaleThisFrame;
        return;
    }
    // With multi-view rendering its twin draws it into the focus view too, where that view sees the hidden area (R6a).
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

// The multi-view path's eye images, without calling the game's code: LMU's submit copies each eye's image into that eye's
// swapchain image (CopySubresourceRegion), so at xrEndFrame the copy into the submitted image names eye e's
// (learn_eye_images, mvr_pass.h mvr_copy_source). Held (AddRef'd) until another is seen. Null until the first.
ID3D11Texture2D* eyeImage[2] = {};
bool eyeImagesLogged = false, eyeImagesChecked = false;

void learn_eye_images(ID3D11Texture2D* const (&eyes)[2]) {
    for (int e = 0; e < 2; ++e) {
        ID3D11Resource* src = mvr_copy_source(eyes[e]);
        ID3D11Texture2D* t = nullptr;
        if (!src || FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))))
            continue;
        if (t == eyeImage[e]) {
            t->Release();
            continue;
        }
        if (eyeImage[e])
            eyeImage[e]->Release();
        eyeImage[e] = t;  // our reference
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        simqvfr::logf("lmu qvfr: frame %llu: eye %d's image %ux%u format %d, %s from its copy into the swapchain image "
                      "(no call into the game)",
                      frames, e, d.Width, d.Height, static_cast<int>(d.Format), eyeImagesLogged ? "again" : "found");
    }
    eyeImagesLogged = eyeImagesLogged || (eyeImage[0] && eyeImage[1]);
}

// The game's eye image e, AddRef'd (null if not known): on the multi-view path the one its copy named, else the game's.
ID3D11Texture2D* game_eye_image(int e) {
    if (!cfg.trackerDrives)
        return eye_texture(e);
    if (eyeImage[e])
        eyeImage[e]->AddRef();
    return eyeImage[e];
}

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
ClipRemap uiRemap[2];  // each focus view from its eye in clip space (the VR UI's matrices; focus_math.h)
float saved[2][2][16];
bool swapped[2] = {};
bool shapeLogged = false;

// MVR (lmu_mvr): both eyes' focus projections taken once at pass 0, so the focus view drawn in its eye's pass and the
// one pass 2/3 renders use the same box (the gaze moves between them).
float frozenFocus[2][2][16];
FocusBox frozenBox[2];
ClipRemap frozenRemap[2];
bool frozen[2] = {};

bool focus_matrices(int eye, float (&eyeM)[2][16], float (&focusM)[2][16], FocusBox (&box)[2]);

bool swap_in_focus(int eye) {
    float eyeM[2][16], focusM[2][16];
    FocusBox box[2];
    if (frozen[eye]) {
        for (int k = 0; k < 2; ++k)
            std::memcpy(eyeM[k], slot_matrix(eye, k), sizeof(eyeM[k]));
        std::memcpy(focusM, frozenFocus[eye], sizeof(focusM));
        box[0] = box[1] = frozenBox[eye];
    } else if (!focus_matrices(eye, eyeM, focusM, box)) {
        return false;
    }
    boxes[eye] = box[0];
    uiRemap[eye] = clip_remap(eyeM[0], focusM[0]);
    for (int k = 0; k < 2; ++k) {
        std::memcpy(saved[eye][k], eyeM[k], sizeof(saved[eye][k]));
        std::memcpy(slot_matrix(eye, k), focusM[k], sizeof(focusM[k]));
    }
    swapped[eye] = true;
    return true;
}

// Eye `eye`'s projections (both slots) and its focus box's: from the live box and the gaze.
bool focus_matrices(int eye, float (&eyeM)[2][16], float (&focusM)[2][16], FocusBox (&box)[2]) {
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
    // DLAA (engine/dlss_views.h): the eye's projection carries this frame's jitter and its DLAA output doesn't. The box's eye
    // tangents and place in the eye's image as the output has them: the frame's record (what the layer composes and
    // submits the periphery with) and the focus post's glare mapping read the output. The matrices stay as rendered.
    double shiftX, shiftY;
    if (ok && simqvfr::dlss_eye_clip_shift(eye, shiftX, shiftY))
        for (int k = 0; k < 2; ++k)
            simqvfr::dlss::unshift_box(box[k], shiftX, shiftY);
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
    return true;
}

// MVR, at pass 0: both eyes' focus boxes for this frame (frozen until it ends).
void freeze_focus() {
    for (int e = 0; e < 2; ++e) {
        float eyeM[2][16];
        FocusBox box[2];
        frozen[e] = focus_matrices(e, eyeM, frozenFocus[e], box);
        if (frozen[e]) {
            frozenBox[e] = box[0];
            frozenRemap[e] = clip_remap(eyeM[0], frozenFocus[e][0]);
        }
    }
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

// ---- QVFR: what the four passes cost ----
// CPU: each pass's time on the render thread, wall clock and the thread's own CPU (its cycle counter), and this code's
// own work in its hooks. GPU: timestamp queries around this code's copies, read back frames later without stalling.
// Averages logged every 900 frames while the scene runs (four_passes_frame_end).
double qpcPerMs = 0, tscPerMs = 0;
long long calibQpc = 0;
unsigned long long calibTsc = 0;

double now_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (qpcPerMs == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcPerMs = static_cast<double>(f.QuadPart) / 1000.0;
    }
    return static_cast<double>(t.QuadPart) / qpcPerMs;
}

unsigned long long thread_cycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    // The cycle counter's rate (TSC): calibrated against QPC over the first second.
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (calibQpc == 0) {
        calibQpc = t.QuadPart;
        calibTsc = __rdtsc();
    } else if (tscPerMs == 0 && qpcPerMs > 0 && static_cast<double>(t.QuadPart - calibQpc) > 1000 * qpcPerMs) {
        tscPerMs = static_cast<double>(__rdtsc() - calibTsc) / (static_cast<double>(t.QuadPart - calibQpc) / qpcPerMs);
    }
    return c;
}

struct CostWindow {
    double passWallMs[4] = {}, passCpuCycles[4] = {}, hookCycles = 0;
    double gpuMs[4] = {};  // spans: eyes set aside, focus 0 copied, focus 1 copied + eyes back, atlas filled
    unsigned frames = 0, gpuFrames = 0;
} costWindow;

// GPU spans of one frame: a disjoint query around timestamp pairs, a ring of frames read back when done.
struct GpuSpans {
    static constexpr int kSlots = 4, kSpans = 4;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Query* disjoint[kSlots] = {};
    ID3D11Query* stamp[kSlots][kSpans][2] = {};
    bool used[kSlots][kSpans] = {}, pending[kSlots] = {};
    int slot = 0;
    bool open = false, failed = false;

    bool ready(ID3D11Texture2D* any) {
        if (context)
            return true;
        if (failed || !any)
            return false;
        ID3D11Device* device = nullptr;
        any->GetDevice(&device);
        D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
        bool ok = true;
        for (int s = 0; s < kSlots; ++s) {
            ok = ok && SUCCEEDED(device->CreateQuery(&dj, &disjoint[s]));
            for (int k = 0; k < kSpans; ++k)
                for (int w = 0; w < 2; ++w)
                    ok = ok && SUCCEEDED(device->CreateQuery(&ts, &stamp[s][k][w]));
        }
        if (ok)
            device->GetImmediateContext(&context);
        device->Release();
        failed = !ok;
        if (!ok)
            simqvfr::log_line("lmu qvfr cost: GPU timestamp queries unavailable: copies not timed");
        return ok;
    }
    void collect(int s) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (context->GetData(disjoint[s], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;  // not done yet (or lost when the slot is reused)
        pending[s] = false;
        if (dj.Disjoint || dj.Frequency == 0)
            return;
        double ms[kSpans] = {};
        for (int k = 0; k < kSpans; ++k) {
            UINT64 b = 0, e = 0;
            if (!used[s][k])
                continue;
            if (context->GetData(stamp[s][k][0], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context->GetData(stamp[s][k][1], &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || e < b)
                return;
            ms[k] = static_cast<double>(e - b) * 1000.0 / static_cast<double>(dj.Frequency);
        }
        for (int k = 0; k < kSpans; ++k)
            costWindow.gpuMs[k] += ms[k];
        ++costWindow.gpuFrames;
    }
    void begin_frame() {
        if (open)
            end_frame();
        for (int s = 0; s < kSlots; ++s)
            if (pending[s])
                collect(s);
        slot = (slot + 1) % kSlots;
        pending[slot] = false;
        for (bool& u : used[slot])
            u = false;
        context->Begin(disjoint[slot]);
        open = true;
    }
    void mark(int span, int which) {
        if (!open)
            return;
        context->End(stamp[slot][span][which]);
        used[slot][span] = true;
    }
    void end_frame() {
        if (!open)
            return;
        context->End(disjoint[slot]);
        open = false;
        pending[slot] = true;
    }
} gpuSpans;

// ---- QVFR: live resolution ----
// Each pass renders into the top-left of its launch-size target at the eye render size its camera copied at the pass's
// head (kEyeRenderSize, which VR_UpdateEyes sets once per frame from the render scale). So the size and the scale (which
// the pass's post-processing reads) are set together at the end of the pass before: the focus share after pass 1, the
// outer share after pass 3 (and so for the next frame's VR_UpdateEyes), each the game's own scale times the share
// (four_passes_set_live_scales).
// Two sizes per pass (focus_math.h final_extent): sceneW/sceneH, what it rendered of the main targets (the scene, the
// resolve, YEBIS: glare share and MVR's side-by-side viewports work there); passW/passH, the part of its eye target
// that holds its view after the final blit or FSR stretched the main target over it (the copies, the atlas regions, the
// driver tags and the VR UI). The same at the game's render scale 1; below it (FSR's presets) the eye target's part is
// larger by recommended / main. extentW/extentH: passW/passH unrounded, for the layer's record.
float liveScale[2] = {1, 1};  // outer, focus (under liveMutex)
float liveAxes[2][2] = {{1, 1}, {1, 1}};  // per axis (perAxis): outer, focus; width, height shares (under liveMutex)
float gameScale = 0;          // the game's own render scale (its setting; what the launch-size targets were made with)
float lastWritten = 0;        // what this code last wrote to it
unsigned passW[4] = {}, passH[4] = {};
unsigned sceneW[4] = {}, sceneH[4] = {};
double extentW[4] = {}, extentH[4] = {};
bool liveLogged = false, stretchLogged = false;

// The runtime's recommended size as the game took it (VR_Init).
void real_recommended(std::uint32_t (&out)[2]) {
    const auto* rec = reinterpret_cast<const std::uint32_t*>(base() + kRecommendedSize);
    out[0] = rec[0];
    out[1] = rec[1];
}

// The main targets' size: the recommended size at the game's own render scale, as cubeDX11_CreateVRMainTargets makes
// them; 0 x 0 before the game's scale is known.
void main_size(unsigned (&out)[2]) {
    out[0] = out[1] = 0;
    if (gameScale <= 0)
        return;
    std::uint32_t rec[2];
    real_recommended(rec);
    out[0] = scaled_size(rec[0], gameScale);
    out[1] = scaled_size(rec[1], gameScale);
}

// LMU's FSR 1 is on (its UpscaleMethod): each view's post-processing ends in FSR's upscale, not in the final blit.
bool fsr_on() {
    const auto* renderer = *reinterpret_cast<const std::uint8_t* const*>(base() + kRenderer);
    if (!renderer)
        return false;
    const auto* fsr = *reinterpret_cast<const std::uint8_t* const*>(renderer + kRendererFsr);
    return fsr && *reinterpret_cast<const int*>(fsr + kFsrMethod) != 0;
}

// Pass p's part of its eye target from what it rendered of the main targets (passW from sceneW), at most the target.
void set_final_size(int p) {
    unsigned m[2];
    main_size(m);
    std::uint32_t rec[2];
    real_recommended(rec);
    extentW[p] = final_extent(sceneW[p], m[0], rec[0]);
    extentH[p] = final_extent(sceneH[p], m[1], rec[1]);
    passW[p] = final_texels(extentW[p], viewW);
    passH[p] = final_texels(extentH[p], viewH);
    if (viewW && extentW[p] > viewW)
        extentW[p] = viewW;
    if (viewH && extentH[p] > viewH)
        extentH[p] = viewH;
}

int probe_active();  // the render scale probe owns the scale (below)

// At a pass's start: the size its camera copied (what it renders), and the game's own scale (first seen, or changed by
// the game: its video settings).
void qvfr_before_pass(int p) {
    if (p >= 4)
        return;
    const float scale = *reinterpret_cast<const float*>(base() + kRenderScale);
    if (!probe_active() && (gameScale <= 0 || (lastWritten > 0 && scale != lastWritten))) {
        if (gameScale > 0)
            simqvfr::logf("lmu qvfr: frame %llu: the game's render scale changed (%.3f -> %.3f): live scales from it",
                          frames, gameScale, scale);
        gameScale = scale;
        lastWritten = 0;
        stretchLogged = false;
    }
    const auto* size = reinterpret_cast<const float*>(base() + kEyeRenderSize);
    unsigned m[2];
    main_size(m);
    sceneW[p] = size[0] >= 1 ? static_cast<unsigned>(size[0]) : 1u;
    sceneH[p] = size[1] >= 1 ? static_cast<unsigned>(size[1]) : 1u;
    if (m[0] && m[1]) {
        sceneW[p] = std::min(sceneW[p], m[0]);
        sceneH[p] = std::min(sceneH[p], m[1]);
    }
    set_final_size(p);
    if (!stretchLogged && m[0] && viewW) {
        stretchLogged = true;
        std::uint32_t rec[2];
        real_recommended(rec);
        if (m[0] != rec[0] || m[1] != rec[1])
            simqvfr::logf("lmu qvfr: frame %llu: the game's render scale is %.3f (FSR %s): its main targets are %ux%u, "
                          "stretched over the %ux%u eye targets by %s; each view is taken from the part of its eye target "
                          "it covers (pass %d: %ux%u rendered, %ux%u in the eye target)",
                          frames, gameScale, fsr_on() ? "on" : "off", m[0], m[1], viewW, viewH,
                          fsr_on() ? "FSR's upscale" : "the final blit", p, sceneW[p], sceneH[p], passW[p], passH[p]);
    }
}

// The next passes' size: render scale and eye render size, the game's own scale times the outer (which = 0) or focus
// share. At the end of pass 1 (focus) and pass 3 (outer: also what the next frame's VR_UpdateEyes computes).
void set_next_passes(int which) {
    if (probe_active() || gameScale <= 0)
        return;
    // The focus passes beside their eyes (perAxis): their post-processing at the one scale that covers them (YEBIS
    // processes the top-left at the render scale on both axes, whatever the render size says: 2026-11-08 09:28 run),
    // their own width and height recorded (the region their eye's pass drew; they skip the scene, gate D).
    if (cfg.perAxis && which == 1) {
        float sw, sh, share;
        {
            std::lock_guard guard(liveMutex);
            sw = liveAxes[1][0];
            sh = liveAxes[1][1];
            share = liveScale[1];
        }
        const float written = gameScale * share;
        *reinterpret_cast<float*>(base() + kRenderScale) = written;
        lastWritten = written;
        std::uint32_t rec[2];
        real_recommended(rec);
        auto* size = reinterpret_cast<float*>(base() + kEyeRenderSize);
        size[0] = static_cast<float>(scaled_size(rec[0], gameScale * sw));
        size[1] = static_cast<float>(scaled_size(rec[1], gameScale * sh));
        return;
    }
    float share;
    {
        std::lock_guard guard(liveMutex);
        share = liveScale[which];
    }
    const float written = gameScale * share;
    *reinterpret_cast<float*>(base() + kRenderScale) = written;
    lastWritten = written;
    const auto* recommended = reinterpret_cast<const std::uint32_t*>(base() + kRecommendedSize);
    auto* size = reinterpret_cast<float*>(base() + kEyeRenderSize);
    size[0] = static_cast<float>(scaled_size(recommended[0], written));
    size[1] = static_cast<float>(scaled_size(recommended[1], written));
}

// ---- MVR (lmu_mvr, mvr_pass.h): passes 0-1 draw their focus views beside the eye ----

bool mvrInstalled = false;
// The eye passes' room check (mvr_before_pass). The first scene frame after menus or loading refuses by design: its
// passes still render at the game's own size (the live resolution is written at its pass 1's end). A refusal on a frame
// after a scene frame is not expected, so the two are logged apart, once each, and counted for the fallback's summary.
bool mvrRoomLogged[2] = {};  // [0] the first scene frame after others, [1] a later one
bool lastFrameScene = false;  // the frame before rendered the scene (its passes at the live resolution)
unsigned long long mvrRoomRefused = 0, mvrRoomRefusedLater = 0, mvrNoBox = 0;
char mvrLastRefusal[160] = {};  // the last refusal's sizes
// Two-pass frames without both focus finals, by the first reason found (after_two_passes); and those with both.
unsigned long long finalsNoBlit = 0, finalsNoBox = 0, finalsOutside = 0, finalsNoViews = 0, finalsBoth = 0;
int mvrSamples = 0;
bool mvrSampleFrame = false;  // this frame is a gate sample
bool mvrFrame = true;         // this frame uses MVR (false: an A/B run's frame with the four passes alone)
// R4a (FourPassesRun::focusFinal, docs/recon-lmu.md "R4 plan"): this frame runs the game's two eye passes only, its
// focus views the focus finals beside the eyes (mvr_focus_final); the atlas takes them from there.
bool twoPassFrame = false;
bool atlasFromFinals = false;
bool frameFinalsOk = false;          // this frame's focus finals both made (after_two_passes)
unsigned long long noFinalsRun = 0;  // two-pass frames in a row without them (R5's fallback)
bool singleSampleRefusalLogged = false;  // MSAA off: the fallback refused (it would hook the game's code), said once
unsigned finalAt[2][2] = {};  // each focus final's top-left in its eye's texture
bool finalsMissingLogged = false, twoPassLogged = false, countLogged = false;
unsigned long long twoPassFrames = 0, finalsMissing = 0;

// lmu_mvr_backend_ab's side of this frame: 0 NVIDIA's single-pass stereo, 1 instancing.
int backendSide = 0;
bool backendAbOk = false, backendAbLogged = false;
// lmu_car_vectors_ab's: 0 with the other cars' motion, 1 without; the cars' motion went on at the install.
int carSide = 0;
bool carAbLogged = false, carsOn = false;
bool car_ab_on() { return cfg.carAb > 0 && carsOn; }

// The A/B side of this frame: lmu_focus_final_ab's (0 two passes, 1 four), lmu_mvr_backend_ab's (0 NVIDIA's, 1
// instancing) or lmu_mvr_ab's (0 MVR, 1 without).
int ab_side() {
    return cfg.focusFinalAb > 0 ? (twoPassFrame ? 0 : 1)
           : cfg.backendAb > 0  ? backendSide
           : cfg.carAb > 0      ? carSide
                                : (mvrFrame ? 0 : 1);
}
bool ab_run() {
    return cfg.mvrAb > 0 || cfg.focusFinalAb > 0 || (cfg.backendAb > 0 && backendAbOk) || car_ab_on();
}

// An A/B run (FourPassesRun::mvrAb): each side's CPU per pass since the last cost line, in thread cycles.
struct AbWindow {
    double cpu[2][4] = {};  // [0] with MVR, [1] without
    unsigned frames[2] = {};
} ab;

// Gate D (lmu_mvr=2): passes 2-3 skip the scene work before their post-processing (abi.h [MVR]) while skipScene is set
// (render thread only: Cull_Item is also called from elsewhere).
using ViewSceneFn = void (*)(void*, void*, void*, unsigned, char);
using SortMeshBucketFn = std::uintptr_t (*)(void*, void*, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
using CullItemFn = void (*)(void*, void*, void*, float);
using CullSceneGraphFn = void (*)(void*, void*, void*);
ViewSceneFn originalViewScene = nullptr;
SortMeshBucketFn originalSortMeshBucket = nullptr;
CullItemFn originalCullItem = nullptr;
CullSceneGraphFn originalCullSceneGraph = nullptr;
bool skipScene = false;
unsigned long long skipped[4] = {};  // view scenes, sort buckets, cull items, scene graph culls: since the last log
unsigned long long passesSkipped = 0, passesNotSkipped = 0;

bool skipping() { return skipScene && GetCurrentThreadId() == renderThread; }

void view_scene(void* renderer, void* scene, void* view, unsigned flags, char last) {
    if (skipping()) {
        ++skipped[0];
        return;
    }
    originalViewScene(renderer, scene, view, flags, last);
}

std::uintptr_t sort_mesh_bucket(void* bucket, void* scene, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c,
                                std::uintptr_t d) {
    if (skipping()) {
        ++skipped[1];
        return 0;
    }
    return originalSortMeshBucket(bucket, scene, a, b, c, d);
}

void cull_item(void* item, void* view, void* p, float distance) {
    if (skipping()) {
        ++skipped[2];
        return;
    }
    originalCullItem(item, view, p, distance);
}

void cull_scene_graph(void* graph, void* view, void* p) {
    if (skipping()) {
        ++skipped[3];
        return;
    }
    originalCullSceneGraph(graph, view, p);
}

bool install_skip() {
    struct Hook {
        std::uintptr_t rva;
        const std::uint8_t* anchor;
        std::size_t size;
        void* detour;
        void** original;
    };
    const Hook hooks[] = {
        {kViewScene, kViewSceneAnchor, sizeof(kViewSceneAnchor), reinterpret_cast<void*>(&view_scene),
         reinterpret_cast<void**>(&originalViewScene)},
        {kSortMeshBucket, kSortMeshBucketAnchor, sizeof(kSortMeshBucketAnchor), reinterpret_cast<void*>(&sort_mesh_bucket),
         reinterpret_cast<void**>(&originalSortMeshBucket)},
        {kCullItem, kCullItemAnchor, sizeof(kCullItemAnchor), reinterpret_cast<void*>(&cull_item),
         reinterpret_cast<void**>(&originalCullItem)},
        {kCullSceneGraph, kCullSceneGraphAnchor, sizeof(kCullSceneGraphAnchor), reinterpret_cast<void*>(&cull_scene_graph),
         reinterpret_cast<void**>(&originalCullSceneGraph)},
    };
    for (const Hook& h : hooks)
        if (!simqvfr::code_matches(h.rva, h.anchor, h.size))
            return false;
    for (const Hook& h : hooks) {
        void* target = reinterpret_cast<void*>(base() + h.rva);
        if (MH_CreateHook(target, h.detour, h.original) != MH_OK || MH_EnableHook(target) != MH_OK)
            return false;
    }
    return true;
}

// Before pass p: the eye's size (qvfr_before_pass took it), its focus view's (as set_next_passes will set pass 2-3's),
// this frame's remap (frozen at pass 0), and whether this frame is a gate sample.
// MVR's context hooks on `device` (at the first pass, or with the pass tracker at the session's first frame end, so menus
// are seen too). False: MVR off for the session.
ID3D11Device* sessionDevice = nullptr;  // four_passes_device (not referenced: the session keeps it)
void pass_begin(int p);
void pass_end(int p);
void panel_draw(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start);
bool install_mvr(ID3D11Device* device) {
    mvr_focus_replay(cfg.focusReplay);  // before the install: the draws hooked for it only
    mvr_ui_mips(cfg.uiMips);
    // Texture sharpening's pixel shader hook only when it's on at launch (no cost otherwise; its three settings are live
    // from then on) and where DLSS can run: a build with NGX, an NVIDIA GPU that draws two views, the passes the
    // tracker drives (DLSS's only way in).
    mvr_mip_bias_hooks(cfg.textureSharpening && simqvfr::ngx::built_in() && simqvfr::nvapi::two_views_gpu() != nullptr &&
                       cfg.trackerDrives);
    // Other cars' motion: the player's where DLSS can run (as texture sharpening's), the diagnostics' as they say; their
    // hooks only when on (the tracking before the slots: they need it).
    const bool cars = cfg.carTrack && (cfg.carForced || (simqvfr::ngx::built_in() &&
                                                         simqvfr::nvapi::two_views_gpu() != nullptr && cfg.trackerDrives));
    mvr_car_track(cars);
    mvr_car_slots(cars ? cfg.carSlots : 0);
    mvr_car_slot_images(cars && cfg.carForced);  // a freeze each: never the player's
    carsOn = cars && cfg.carSlots >= 2;  // the A/B measures what players get (DLSS taking the motion)
    mvrInstalled = device && mvr_pass_install(device);
    if (!mvrInstalled) {
        cfg.mvr = false;
        return false;
    }
    if (cfg.passTracker) {
        mvr_track_passes(true);
        if (cfg.trackerDrives) {
            mvr_track_drive(&pass_begin, &pass_end);
            mvr_mask_stale(&maskStale);  // R6c: the lens mask's draw dropped while stale
            if (qvfrMode)
                mvr_panel_draws(&panel_draw);  // R6b: the menus at native from the VR UI panel's draws
        }
        simqvfr::log_line(cfg.trackerDrives
                              ? "lmu mvr: pass tracker drives the eye passes (lmu_pass_tracker=2): their bounds from the d3d11 "
                                "calls alone; the game's scene render, eye loop, post-effect time, scene skip, glare and "
                                "driver tag hooks not installed"
                              : "lmu mvr: pass tracker on (lmu_pass_tracker=1, observe only): the eye passes as the d3d11 "
                                "calls show them, compared with the game's in the log");
    }
    if (cfg.focusReplay)
        simqvfr::log_line("lmu mvr: focus post on (lmu_focus_replay=1): after each eye pass's YEBIS copy and composite, "
                          "the same for its focus view, with the eye's glare and exposure; its final beside the eye");
    if (cfg.focusFinal)
        simqvfr::logf("lmu mvr: focus finals on (lmu_focus_final=1): the game's two eye passes, the focus views from "
                      "beside the eyes%s",
                      cfg.focusFinalAb > 0 ? (", A/B: " + std::to_string(cfg.focusFinalAb) +
                                              " frames like that, then as many with the four passes" +
                                              (cfg.focusFinalAbPlain ? " as plain QVFR (no multi-view)" : " (gate D)") +
                                              ", in turn")
                                                 .c_str()
                                           : "");
    return true;
}

void mvr_before_pass(int p) {
    if (!mvrInstalled) {
        ID3D11Texture2D* t = eye_texture(0);
        if (!t)
            return;
        ID3D11Device* device = nullptr;
        t->GetDevice(&device);
        t->Release();
        const bool ok = install_mvr(device);
        if (device)
            device->Release();
        if (!ok)
            return;
    }
    bool sample = false;
    if (p == 0) {
        mvrFrame = cfg.mvrAb <= 0 || (sceneFrames / static_cast<unsigned long long>(cfg.mvrAb)) % 2 == 0;
        mvr_frame_mode(mvrFrame ? 0 : 1);
        if (cfg.focusFinal) {  // R4a: two passes (or, in an A/B run, every other N frames four)
            twoPassFrame =
                cfg.trackerDrives ||  // R4b: the game's own two passes (no loop patch)
                (countImmediate &&
                 (cfg.focusFinalAb <= 0 || (sceneFrames / static_cast<unsigned long long>(cfg.focusFinalAb)) % 2 == 0));
            if (!countImmediate && !cfg.trackerDrives && !countLogged) {
                countLogged = true;
                simqvfr::log_line("lmu mvr: focus finals: the eye loop's count can't be set (its stub isn't where "
                                  "expected): four passes as before");
            }
            set_pass_count(twoPassFrame ? 2 : 4);
            mvr_frame_mode(twoPassFrame ? 0 : 1);
            if (cfg.backendAb > 0 && cfg.trackerDrives) {  // NVIDIA's or instancing this frame (between passes)
                if (!backendAbLogged) {
                    backendAbLogged = true;
                    backendAbOk = mvr_probe_backend() == MvrBackend::nvapi && mvr_probe_instanced();
                    if (backendAbOk)
                        simqvfr::logf("lmu mvr: backend A/B: %d frames with NVIDIA's multi-view rendering, then %d "
                                      "with instancing, in turn (lmu_mvr_backend_ab)",
                                      cfg.backendAb, cfg.backendAb);
                    else
                        simqvfr::logf("lmu mvr: lmu_mvr_backend_ab=%d ignored: it needs NVIDIA's multi-view rendering "
                                      "and the instanced twins (an RTX 20 series or newer, without "
                                      "multi_view_software=1)",
                                      cfg.backendAb);
                }
                if (backendAbOk && !mvr_broken()) {
                    backendSide = static_cast<int>((sceneFrames / static_cast<unsigned long long>(cfg.backendAb)) % 2);
                    if (mvr_instanced() != (backendSide == 1))
                        mvr_use_instanced(backendSide == 1, true);
                    mvr_frame_mode(backendSide);
                }
            }
            if (car_ab_on()) {  // the other cars' motion this frame or not (between frames)
                if (!carAbLogged) {
                    carAbLogged = true;
                    simqvfr::logf("lmu mvr: other cars' motion A/B: %d frames with it, then %d without, in turn "
                                  "(lmu_car_vectors_ab)",
                                  cfg.carAb, cfg.carAb);
                }
                carSide = static_cast<int>((sceneFrames / static_cast<unsigned long long>(cfg.carAb)) % 2);
                mvr_car_active(carSide == 0);
                mvr_frame_mode(carSide);
            }
            // R5c: the four-pass side as plain QVFR (no multi-view: the branch below for every pass) or gate D.
            if (cfg.focusFinalAbPlain && !twoPassFrame)
                mvrFrame = false;
            mvr_keep_focus(cfg.mvrSkip && !twoPassFrame && mvrFrame);  // gate D's kept regions only for passes 2-3
        }
        freeze_focus();
        const auto due = static_cast<unsigned long long>(cfg.first);
        sample = cfg.mvrCompare && mvrFrame && !twoPassFrame && mvrSamples < cfg.count && sceneFrames >= due &&
                 (sceneFrames - due) % static_cast<unsigned long long>(cfg.every) == 0;
        mvrSamples += sample ? 1 : 0;
        mvrSampleFrame = sample;
    }
    if (!mvrFrame) {  // an A/B run's other side: the eyes alone, passes 2-3 render their scene
        skipScene = false;
        mvr_pass_begin(p, nullptr, false, false);
        return;
    }
    if (p >= 2) {  // gate D: the scene is skipped when the eye's pass drew this focus view (its region kept)
        skipScene = cfg.mvrSkip && mvr_focus_ready(p - 2);
        ++(skipScene ? passesSkipped : passesNotSkipped);
        mvr_pass_begin(p, nullptr, false, skipScene);
        return;
    }
    MvrPassView v;
    bool draw = false;
    if (p < 2 && frozen[p] && gameScale > 0) {
        const ClipRemap& r = frozenRemap[p];
        v.eye = p;
        v.remap[0] = static_cast<float>(r.a);
        v.remap[1] = static_cast<float>(r.b);
        v.remap[2] = static_cast<float>(r.c);
        v.remap[3] = static_cast<float>(r.d);
        v.eyeW = sceneW[p];  // its corner of the main targets, where the focus view goes beside it
        v.eyeH = sceneH[p];
        v.finalEyeW = passW[p];  // that corner in its final (stretched by the game's blit below render scale 1)
        v.finalEyeH = passH[p];
        v.box = frozenBox[p];
        float shareW, shareH;
        {
            std::lock_guard guard(liveMutex);
            shareW = cfg.perAxis ? liveAxes[1][0] : liveScale[1];
            shareH = cfg.perAxis ? liveAxes[1][1] : liveScale[1];
        }
        std::uint32_t recommended[2];
        real_recommended(recommended);
        v.focusW = scaled_size(recommended[0], gameScale * shareW);
        v.focusH = scaled_size(recommended[1], gameScale * shareH);
        // DLSS super resolution on the focus view (engine/dlss_views.h dlss_focus_scale): the scene draws it smaller in
        // the same corner, DLSS upscales it into its region before the post; 1 (its size) unless that's on and working.
        const double srScale = simqvfr::dlss_focus_scale();
        v.focusRenderW = simqvfr::dlss::render_size(v.focusW, srScale);
        v.focusRenderH = simqvfr::dlss::render_size(v.focusH, srScale);
        // DLAA (engine/dlss_views.h): the focus view's own jitter, through its remap (in the pixels it's drawn at); the
        // eye's is in its projection. Both are what the overlays after the post are drawn moved back by
        // (MvrPassView::overlayShift).
        const float remapB = v.remap[1], remapD = v.remap[3];
        simqvfr::dlss_focus_jitter(p, v.focusRenderW, v.focusRenderH, v.remap[1], v.remap[3]);
        v.overlayShift[1][0] = v.remap[1] - remapB;
        v.overlayShift[1][1] = v.remap[3] - remapD;
        simqvfr::dlss_eye_clip_shift(p, v.overlayShift[0][0], v.overlayShift[0][1]);
        simqvfr::dlss_mip_bias(p, v.mipBias[1], v.mipBias[0]);
        if ((v.mipBias[0] != 0 || v.mipBias[1] != 0) && !mvr_mip_bias_ready()) {  // turned on while the game runs
            static bool logged = false;
            if (!logged) {
                logged = true;
                simqvfr::log_line("lmu mvr: texture sharpening turned on, but it was off at launch (its hook and shader "
                                  "twins weren't made): it starts at the next launch");
            }
        }
        // Side by side in the main targets (the scene's), and the focus final beside the eye's corner of its final.
        unsigned room[2];
        main_size(room);
        if (!room[0] || !room[1]) {
            room[0] = viewW;
            room[1] = viewH;
        }
        draw = v.eyeW && v.focusW && (!room[0] || (v.eyeW + v.focusW <= room[0] && v.focusH <= room[1])) &&
               (!viewW || (v.finalEyeW + v.focusW <= viewW && v.focusH <= viewH));
        if (!draw) {
            const int later = lastFrameScene ? 1 : 0;
            ++mvrRoomRefused;
            mvrRoomRefusedLater += later;
            std::snprintf(mvrLastRefusal, sizeof(mvrLastRefusal),
                          "frame %llu pass %d: eye %ux%u, focus %ux%u, targets %ux%u, the eye %ux%u of its %ux%u final",
                          frames, p, v.eyeW, v.eyeH, v.focusW, v.focusH, room[0], room[1], v.finalEyeW, v.finalEyeH, viewW,
                          viewH);
            if (!mvrRoomLogged[later]) {
                mvrRoomLogged[later] = true;
                simqvfr::logf("lmu mvr: frame %llu: eye %ux%u and focus %ux%u don't fit side by side in %ux%u targets (the "
                              "eye %ux%u of its %ux%u final)%s: %s",
                              frames, v.eyeW, v.eyeH, v.focusW, v.focusH, room[0], room[1], v.finalEyeW, v.finalEyeH, viewW,
                              viewH,
                              later ? ", on a frame after a scene frame (its passes at the live resolution: not expected)"
                                    : ", the first scene frame after menus or loading (its passes at the game's own size "
                                      "until the live resolution applies: expected)",
                              twoPassFrame ? "no focus views this frame" : "the focus views from passes 2-3 only");
            }
        }
    } else {
        ++mvrNoBox;  // no focus box this frame (its projection, or the game's scale not known yet)
    }
    mvr_pass_begin(p, draw ? &v : nullptr, sample);
}

// The view `src` (its top-left w x h) into the atlas at (x, y), or (atlas_out) the atlas's at (x, y) into `dst`.
void atlas_in(ID3D11Texture2D* src, unsigned x, unsigned y, unsigned w, unsigned h) {
    ID3D11Device* device = nullptr;
    atlas->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX box{0, 0, 0, std::min(w, viewW), std::min(h, viewH), 1};
    context->CopySubresourceRegion(atlas, 0, x, y, 0, src, 0, &box);
    context->Release();
    device->Release();
}

// `src`'s w x h at (sx, sy) into the atlas at (x, y) (R4a: a focus final beside its eye).
void atlas_in_from(ID3D11Texture2D* src, unsigned sx, unsigned sy, unsigned x, unsigned y, unsigned w, unsigned h) {
    ID3D11Device* device = nullptr;
    atlas->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX box{sx, sy, 0, sx + std::min(w, viewW), sy + std::min(h, viewH), 1};
    context->CopySubresourceRegion(atlas, 0, x, y, 0, src, 0, &box);
    context->Release();
    device->Release();
}

void atlas_out(ID3D11Texture2D* dst, unsigned x, unsigned y, unsigned w, unsigned h) {
    ID3D11Device* device = nullptr;
    atlas->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX box{x, y, 0, x + std::min(w, viewW), y + std::min(h, viewH), 1};
    context->CopySubresourceRegion(dst, 0, 0, 0, 0, atlas, 0, &box);
    context->Release();
    device->Release();
}

// `dst` = the top-left w x h of `src` (same size and format family).
void copy_corner(ID3D11Texture2D* dst, ID3D11Texture2D* src, unsigned w, unsigned h) {
    ID3D11Device* device = nullptr;
    dst->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D11_BOX box{0, 0, 0, w, h, 1};
    context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &box);
    context->Release();
    device->Release();
}

// R3's gate (lmu_focus_replay): each eye pass's focus final (the focus view beside the eye in the eye's final,
// mvr_focus_final) kept aside before pass 2 renders into the eye target over it, put back after pass 3 so the VR UI is
// doubled onto it, compared with passes 2-3's at the samples (compare_focus_final).
ID3D11Texture2D* mvrFinal[2] = {};
bool mvrFinalKept[2] = {};
unsigned mvrFinalAt[2][4] = {};  // x, y, width, height in the eye target

void keep_focus_finals(ID3D11Texture2D* const (&eyes)[2]) {
    for (int e = 0; e < 2; ++e) {
        mvrFinalKept[e] = false;
        unsigned x = 0, y = 0, w = 0, h = 0;
        if (!eyes[e] || !mvr_focus_final(e, x, y, w, h))
            continue;
        D3D11_TEXTURE2D_DESC d;
        eyes[e]->GetDesc(&d);
        if (x + w > d.Width || y + h > d.Height || d.SampleDesc.Count != 1)
            continue;
        if (mvrFinal[e]) {
            D3D11_TEXTURE2D_DESC have;
            mvrFinal[e]->GetDesc(&have);
            if (have.Width != w || have.Height != h || have.Format != d.Format) {
                mvrFinal[e]->Release();
                mvrFinal[e] = nullptr;
            }
        }
        ID3D11Device* device = nullptr;
        eyes[e]->GetDevice(&device);
        if (!mvrFinal[e]) {
            D3D11_TEXTURE2D_DESC c = d;
            c.Width = w;
            c.Height = h;
            c.MipLevels = c.ArraySize = 1;
            c.Usage = D3D11_USAGE_DEFAULT;
            c.BindFlags = 0;
            c.CPUAccessFlags = c.MiscFlags = 0;
            device->CreateTexture2D(&c, nullptr, &mvrFinal[e]);
        }
        if (mvrFinal[e]) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
            context->CopySubresourceRegion(mvrFinal[e], 0, 0, 0, 0, eyes[e], 0, &box);
            context->Release();
            mvrFinalKept[e] = true;
            const unsigned at[4] = {x, y, w, h};
            std::memcpy(mvrFinalAt[e], at, sizeof(at));
        }
        device->Release();
    }
}

void put_back_focus_finals(ID3D11Texture2D* const (&eyes)[2]) {
    for (int e = 0; e < 2; ++e) {
        if (!mvrFinalKept[e] || !eyes[e] || !mvrFinal[e])
            continue;
        ID3D11Device* device = nullptr;
        eyes[e]->GetDevice(&device);
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        context->CopySubresourceRegion(eyes[e], 0, mvrFinalAt[e][0], mvrFinalAt[e][1], 0, mvrFinal[e], 0, nullptr);
        context->Release();
        device->Release();
    }
}

// The focus views' own images, render targets: the game's VR UI (the HUD, the in-car menus) is drawn into the eyes after
// the eye loop, and into these too (vrui_draw). The atlas gets them at xrEndFrame.
ID3D11Texture2D* focusTex[2] = {};
ID3D11RenderTargetView* focusRtv[2] = {};  // made in vrui_draw, like the game's own eye RTVs

void release_focus_textures() {
    for (int e = 0; e < 2; ++e) {
        if (focusRtv[e])
            focusRtv[e]->Release();
        if (focusTex[e])
            focusTex[e]->Release();
        focusRtv[e] = nullptr;
        focusTex[e] = nullptr;
    }
}

bool ensure_focus_textures(ID3D11Texture2D* eyeTexture) {
    D3D11_TEXTURE2D_DESC desc;
    eyeTexture->GetDesc(&desc);
    if (focusTex[0] && focusTex[1]) {
        D3D11_TEXTURE2D_DESC have;
        focusTex[0]->GetDesc(&have);
        if (have.Width == desc.Width && have.Height == desc.Height && have.Format == desc.Format)
            return true;
    }
    release_focus_textures();
    if (desc.SampleDesc.Count != 1)
        return false;
    D3D11_TEXTURE2D_DESC f = desc;
    f.MipLevels = 1;
    f.ArraySize = 1;
    f.Usage = D3D11_USAGE_DEFAULT;
    f.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    f.CPUAccessFlags = 0;
    f.MiscFlags = 0;
    ID3D11Device* device = nullptr;
    eyeTexture->GetDevice(&device);
    bool ok = true;
    for (int e = 0; e < 2; ++e)
        ok = ok && SUCCEEDED(device->CreateTexture2D(&f, nullptr, &focusTex[e]));
    device->Release();
    if (!ok) {
        release_focus_textures();
        simqvfr::logf("lmu qvfr: the focus images (%ux%u, format %d) couldn't be made: not composed", f.Width, f.Height,
                      static_cast<int>(f.Format));
    }
    return ok;
}

// ---- QVFR: the menus at native resolution ----
// A frame without the eye loop (the main menu, loading) is the game's eye images as they are, at the views' size: its
// UI as soft as the periphery. Instead each eye's image is scaled up to the native eye into an image of ours before the
// game draws its UI, the UI panel drawn into that at native after (as into the focus views), and the layer composes a
// menu atlas of the two (the focus views the same pixels). Kept for the session (the compositor holds views of them).
bool menuNative = false;        // FourPassesRun::menuNative
double layerShare[2] = {1, 1};  // the views' layer in native eye widths and heights (FourPassesRun)
simqvfr::D3D11ViewResample menuResample;
bool menuResampleTried = false;
ID3D11Texture2D* menuTex[2] = {};
ID3D11RenderTargetView* menuRtv[2] = {};
ID3D11Texture2D* menuAtlas = nullptr;
unsigned menuW = 0, menuH = 0;
bool menuReady = false;  // this frame's menu is in the menu atlas, until the layer takes it
float menuUv[4][4] = {};
unsigned long long menuFrames = 0;
bool menuRefusalLogged = false;

// ---- QVFR: the driver tags under live resolution (tag_viewport.h) ----
// The tags are drawn after each pass's post-processing with a viewport of the whole eye target; a pass rendered smaller
// (live resolution) gets them too far out and too large. For that draw the viewport is cut to what the pass rendered.
using LabelsDrawFn = void (*)(void* camera, void* labels);
LabelsDrawFn originalLabelsDraw = nullptr;
bool labelsHooked = false;
ID3D11DeviceContext* labelsContext = nullptr;  // the game's immediate context (from an eye target's device)
unsigned long long labelsCut = 0, labelsKept = 0;
bool labelsCutLogged = false, labelsKeptLogged = false, labelsResetLogged = false;

// The size of the render target bound as the first, 0 x 0 if none.
void bound_target_size(ID3D11DeviceContext* context, unsigned& w, unsigned& h) {
    w = h = 0;
    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv)
        return;
    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    rtv->Release();
    if (!resource)
        return;
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        w = d.Width;
        h = d.Height;
        texture->Release();
    }
    resource->Release();
}

void labels_draw(void* camera, void* labels) {
    const int p = pass;
    if (!qvfrMode || !inPass || p < 0 || p >= 4 || GetCurrentThreadId() != renderThread) {
        originalLabelsDraw(camera, labels);
        return;
    }
    if (!labelsContext) {
        if (ID3D11Texture2D* t = eye_texture(0)) {
            ID3D11Device* device = nullptr;
            t->GetDevice(&device);
            device->GetImmediateContext(&labelsContext);
            device->Release();
            t->Release();
        }
        if (!labelsContext) {
            originalLabelsDraw(camera, labels);
            return;
        }
    }
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    labelsContext->RSGetViewports(&n, vps);
    unsigned tw = 0, th = 0;
    bound_target_size(labelsContext, tw, th);
    TagViewport cut{};
    const bool cutting = n >= 1 && tw == viewW && th == viewH &&
                         tag_viewport_cut({vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height}, viewW, viewH,
                                          passW[p], passH[p], cut);
    if (!cutting) {
        ++labelsKept;
        if (!labelsKeptLogged && n >= 1 && (passW[p] < viewW || passH[p] < viewH)) {
            labelsKeptLogged = true;
            simqvfr::logf("lmu qvfr: driver tags: frame %llu pass %d: viewport %.0fx%.0f at %.0f,%.0f on a %ux%u target "
                          "(views %ux%u, the pass rendered %ux%u): left as the game set it",
                          frames, p, vps[0].Width, vps[0].Height, vps[0].TopLeftX, vps[0].TopLeftY, tw, th, viewW, viewH,
                          passW[p], passH[p]);
        }
        originalLabelsDraw(camera, labels);
        return;
    }
    D3D11_VIEWPORT v = vps[0];
    v.Width = cut.width;
    v.Height = cut.height;
    labelsContext->RSSetViewports(1, &v);  // through MVR's hook when it runs: its second viewport follows
    originalLabelsDraw(camera, labels);
    D3D11_VIEWPORT after[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT m = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    labelsContext->RSGetViewports(&m, after);
    if (!labelsResetLogged && (m < 1 || after[0].Width != v.Width || after[0].Height != v.Height)) {
        labelsResetLogged = true;
        simqvfr::logf("lmu qvfr: driver tags: frame %llu pass %d: the draw set its own viewport (%.0fx%.0f): the cut had no "
                      "effect",
                      frames, p, m ? after[0].Width : 0.0f, m ? after[0].Height : 0.0f);
    }
    // The game's viewports back: MVR doubles one viewport itself, so with it the game's one is restored.
    labelsContext->RSSetViewports(cfg.mvr ? 1 : n, vps);
    ++labelsCut;
    if (!labelsCutLogged) {
        labelsCutLogged = true;
        simqvfr::logf("lmu qvfr: driver tags: frame %llu pass %d: drawn in the %.0fx%.0f the pass rendered, not the whole "
                      "%ux%u target",
                      frames, p, cut.width, cut.height, viewW, viewH);
    }
}

// ---- QVFR: the VR UI in the focus views ----
// VR_Present draws the HUD canvas onto a curved panel in each eye's final with the eye's projection x view (kVruiDraw);
// the same draw again into the focus images, with that matrix remapped to the focus view's clip space (uiRemap).
using VruiDrawFn = void (*)(void* vrui, ID3D11DeviceContext* context, const void* eyes);
using VruiDrawEyeFn = void (*)(void* vrui, ID3D11DeviceContext* context, const void* eyes, unsigned eye);
VruiDrawFn originalVruiDraw = nullptr;
bool vruiHooked = false;
bool focusUiPending = false;  // this frame's focus images wait for the UI
bool uiScaleOnly = false;     // R4a: the eyes' panels cut to the rendered part, no focus images to draw into
unsigned long long uiDrawn = 0, uiSkipped = 0;
bool uiRefusalLogged = false;

// RTVs on `textures` like the game's eye RTV `like`; false (none made) when they can't be.
bool make_rtvs(ID3D11DeviceContext* context, ID3D11RenderTargetView* like, ID3D11Texture2D* const (&textures)[2],
               ID3D11RenderTargetView* (&out)[2]) {
    for (auto*& view : out)
        if (view) {
            view->Release();
            view = nullptr;
        }
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    like->GetDesc(&d);
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    bool ok = d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D;
    for (int e = 0; e < 2; ++e)
        ok = ok && textures[e] && SUCCEEDED(device->CreateRenderTargetView(textures[e], &d, &out[e]));
    device->Release();
    if (!ok)
        for (auto*& view : out)
            if (view) {
                view->Release();
                view = nullptr;
            }
    return ok;
}

// Both eyes' panels drawn with `block` (the eyes block, its matrices as wanted) into `rtvs`, at a w x h viewport when
// w > 0 (else the one set); the game's eye RTVs, viewport and bound targets put back.
void draw_panels(void* vrui, ID3D11DeviceContext* context, const std::uint8_t* block, ID3D11RenderTargetView* const (&rtvs)[2],
                 int w, int h) {
    auto** rtv = reinterpret_cast<ID3D11RenderTargetView**>(static_cast<std::uint8_t*>(vrui) + kVruiEyeRtv);
    int* viewport = reinterpret_cast<int*>(static_cast<std::uint8_t*>(vrui) + kVruiViewport);
    const int keptViewport[2] = {viewport[0], viewport[1]};
    ID3D11RenderTargetView* keptRtv = nullptr;
    ID3D11DepthStencilView* keptDsv = nullptr;
    context->OMGetRenderTargets(1, &keptRtv, &keptDsv);
    ID3D11RenderTargetView* const gameRtv[2] = {rtv[0], rtv[1]};
    rtv[0] = rtvs[0];
    rtv[1] = rtvs[1];
    if (w > 0 && h > 0) {
        viewport[0] = w;
        viewport[1] = h;
    }
    const auto drawEye = reinterpret_cast<VruiDrawEyeFn>(base() + kVruiDrawEye);
    drawEye(vrui, context, block, 0);
    drawEye(vrui, context, block, 1);
    rtv[0] = gameRtv[0];
    rtv[1] = gameRtv[1];
    viewport[0] = keptViewport[0];
    viewport[1] = keptViewport[1];
    context->OMSetRenderTargets(1, &keptRtv, keptDsv);
    if (keptRtv)
        keptRtv->Release();
    if (keptDsv)
        keptDsv->Release();
}

// The menus at native resolution (below): before and after the game's own UI draw in a frame without the eye loop.
bool menu_background(ID3D11DeviceContext* context, ID3D11RenderTargetView* like);
void menu_ui(void* vrui, ID3D11DeviceContext* context, const void* eyes);

// The VR UI object's RTV of eye 0, when it has both eyes' (menu_background's `like`).
ID3D11RenderTargetView* vrui_eye_rtv(void* vrui) {
    auto** rtv = reinterpret_cast<ID3D11RenderTargetView**>(static_cast<std::uint8_t*>(vrui) + kVruiEyeRtv);
    return rtv[0] && rtv[1] ? rtv[0] : nullptr;
}

void focus_ui(void* vrui, ID3D11DeviceContext* context, const void* eyes) {
    auto** rtv = reinterpret_cast<ID3D11RenderTargetView**>(static_cast<std::uint8_t*>(vrui) + kVruiEyeRtv);
    const auto refuse = [](const char* why) {
        ++uiSkipped;
        if (!uiRefusalLogged) {
            uiRefusalLogged = true;
            simqvfr::logf("lmu qvfr: frame %llu: the VR UI isn't drawn into the focus views (%s): the HUD and menus are "
                          "missing inside the focus box",
                          frames, why);
        }
    };
    if (!rtv[0] || !rtv[1] || !focusTex[0] || !focusTex[1])
        return refuse("no targets");
    if ((!focusRtv[0] || !focusRtv[1]) && !make_rtvs(context, rtv[0], focusTex, focusRtv))
        return refuse("its render target view doesn't fit the focus images");
    std::uint8_t block[kVruiEyesSize];
    std::memcpy(block, eyes, sizeof(block));
    int layout = 0;
    for (int e = 0; e < 2; ++e) {
        float m[16];
        std::uint8_t* at = block + kVruiEyeMatrix + e * kEyeSlotStride;
        std::memcpy(m, at, sizeof(m));
        layout = clip_layout(m);
        if (!layout) {
            if (!uiRefusalLogged)
                simqvfr::logf("lmu qvfr: eye %d's UI matrix: %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g", e, m[0],
                              m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            return refuse("its matrix isn't a projection x view of the shape expected");
        }
        remap_clip(m, uiRemap[e], layout);
        std::memcpy(at, m, sizeof(m));
    }
    draw_panels(vrui, context, block, focusRtv, 0, 0);  // at the viewport vrui_draw set (the focus views' size)
    if (uiDrawn++ == 0)
        simqvfr::logf("lmu qvfr: frame %llu: the VR UI drawn into the focus views too (mode %d; the UI matrix keeps its clip "
                      "outputs in its %s)",
                      frames, *static_cast<const int*>(eyes), layout == 1 ? "columns" : "rows");
}

void vrui_draw(void* vrui, ID3D11DeviceContext* context, const void* eyes) {
    // With live resolution the panels go into the part of each target the frame rendered: the panel draws' viewport
    // (the eye targets' size, kVruiViewport) set to it for this call. Frames without four views keep the game's.
    const bool pending = focusUiPending && vrui && context && eyes;
    const bool scaleOnly = uiScaleOnly && vrui && context && eyes;
    uiScaleOnly = false;
    int* viewport = vrui ? reinterpret_cast<int*>(static_cast<std::uint8_t*>(vrui) + kVruiViewport) : nullptr;
    const int kept[2] = {viewport ? viewport[0] : 0, viewport ? viewport[1] : 0};
    const bool scaled = (pending || scaleOnly) && viewW &&
                        (passW[0] < viewW || passH[0] < viewH || passW[2] < viewW || passH[2] < viewH);
    if (scaled) {
        viewport[0] = static_cast<int>(passW[0]);
        viewport[1] = static_cast<int>(passH[0]);
    }
    // A frame without the eye loop (the main menu, loading): the eyes scaled up to native first, the UI drawn into those
    // at native after the game's own draw (menu_background, menu_ui).
    const bool menu = !pending && qvfrMode && menuNative && passesThisFrame == 0 && vrui && context && eyes &&
                      menu_background(context, vrui_eye_rtv(vrui));
    originalVruiDraw(vrui, context, eyes);
    if (menu)
        menu_ui(vrui, context, eyes);
    if (pending) {
        const unsigned long long c0 = thread_cycles();
        focusUiPending = false;
        if (scaled) {
            viewport[0] = static_cast<int>(passW[2]);
            viewport[1] = static_cast<int>(passH[2]);
        }
        focus_ui(vrui, context, eyes);
        costWindow.hookCycles += static_cast<double>(thread_cycles() - c0);
    }
    if (scaled) {
        viewport[0] = kept[0];
        viewport[1] = kept[1];
    }
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
    // The sizes rendered, as views4_live_one_size lays them out (each view half its layer at most: 0.5 = all of it): each
    // view's exact part of its eye target (extentW: the stretched one below the game's render scale 1).
    f.live = true;
    for (int v = 0; v < 4; ++v) {
        f.width[v] = f.renderWidth[v] = viewW ? 0.5 * extentW[v] / viewW : 0.5;
        f.height[v] = f.renderHeight[v] = viewH ? 0.5 * extentH[v] / viewH : 0.5;
    }
    std::lock_guard guard(liveMutex);
    latestRecord = f;
    haveRecord = true;
}

// The menu images (native: w x h, the eye texture `like`'s format) and the menu atlas (2w x h); false if they can't be.
bool ensure_menu(ID3D11DeviceContext* context, ID3D11Texture2D* like, ID3D11RenderTargetView* likeRtv, unsigned w,
                 unsigned h) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!menuResample.ready() && !menuResampleTried) {
        menuResampleTried = true;
        if (!menuResample.initialize(device))
            simqvfr::log_line("lmu qvfr: the menu's scaler couldn't be made: menus at the views' size");
    }
    D3D11_TEXTURE2D_DESC d;
    like->GetDesc(&d);
    bool ok = menuResample.ready();
    if (ok && !(menuTex[0] && menuTex[1] && menuAtlas && menuRtv[0] && menuRtv[1] && menuW == w && menuH == h)) {
        for (int e = 0; e < 2; ++e) {
            if (menuRtv[e])
                menuRtv[e]->Release();
            if (menuTex[e])
                menuTex[e]->Release();
            menuRtv[e] = nullptr;
            menuTex[e] = nullptr;
        }
        if (menuAtlas)
            menuAtlas->Release();
        menuAtlas = nullptr;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = w;
        t.Height = h;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Format = d.Format;
        t.SampleDesc = {1, 0};
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int e = 0; e < 2; ++e)
            ok = ok && SUCCEEDED(device->CreateTexture2D(&t, nullptr, &menuTex[e]));
        D3D11_TEXTURE2D_DESC a = t;
        a.Width = 2 * w;
        a.Format = typeless_of(d.Format);
        a.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&a, nullptr, &menuAtlas)) && make_rtvs(context, likeRtv, menuTex, menuRtv);
        menuW = ok ? w : 0;
        menuH = ok ? h : 0;
        simqvfr::logf(ok ? "lmu qvfr: menu images %ux%u (the native eye; the views %ux%u), menu atlas %ux%u"
                         : "lmu qvfr: the menu images %ux%u couldn't be made (the views %ux%u, atlas %ux%u): menus at the "
                           "views' size",
                      w, h, d.Width, d.Height, 2 * w, h);
    }
    device->Release();
    return ok;
}

bool menu_background(ID3D11DeviceContext* context, ID3D11RenderTargetView* like) {
    ID3D11Texture2D* eyes[2] = {game_eye_image(0), game_eye_image(1)};
    bool ok = like && eyes[0] && eyes[1];
    D3D11_TEXTURE2D_DESC d{};
    unsigned w = 0, h = 0;
    if (ok) {
        eyes[0]->GetDesc(&d);
        // The native eye: the size the layer offered over the layer's share of it.
        std::uint32_t recommended[2];
        real_recommended(recommended);
        w = std::min(8192u, static_cast<unsigned>(std::lround(recommended[0] / layerShare[0])));
        h = std::min(8192u, static_cast<unsigned>(std::lround(recommended[1] / layerShare[1])));
        ok = d.SampleDesc.Count == 1 && (w > d.Width || h > d.Height);  // else the eyes are as sharp already
    }
    ok = ok && ensure_menu(context, eyes[0], like, w, h);
    if (ok) {
        // The same tangents both sides: a straight bilinear scale of the whole eye image.
        const simqvfr::ViewMapping same{{-1, 1, 1, -1}, {-1, 1, 1, -1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}};
        for (int e = 0; e < 2; ++e)
            ok = ok && menuResample.resample_target(eyes[e], {0, 0, static_cast<int>(d.Width), static_cast<int>(d.Height)},
                                                    menuTex[e], {0, 0, static_cast<int>(menuW), static_cast<int>(menuH)},
                                                    same);
        if (!ok && !menuRefusalLogged) {
            menuRefusalLogged = true;
            simqvfr::log_line("lmu qvfr: the menu background couldn't be scaled up: menus at the views' size");
        }
    }
    for (ID3D11Texture2D* t : eyes)
        if (t)
            t->Release();
    return ok;
}

// A menu frame's record: the eyes' own projections, each focus view its box's part of the eye (the same pixels), and
// the views' places in the menu atlas. False: the eyes as they are.
bool menu_record(simqvfr::Views4Frame& f, float (&uv)[4][4]) {
    double fx, fy, cx[2] = {}, cy[2] = {};
    {
        std::lock_guard guard(liveMutex);
        fx = focusFraction;
        fy = focusFractionY > 0 ? focusFractionY : focusFraction;
        for (int e = 0; e < 2; ++e)
            if (gazeValid[e]) {
                cx[e] = gazeTan[e][0];
                cy[e] = gazeTan[e][1];
            }
    }
    f = {};
    for (int e = 0; e < 2; ++e) {
        float eyeM[16], focusM[16];
        std::memcpy(eyeM, slot_matrix(e, 0), sizeof(eyeM));
        FocusBox box;
        clamp_focus_centre(eyeM, fx, fy, cx[e], cy[e]);
        if (!focus_projection(eyeM, fx, fy, cx[e], cy[e], box, focusM))
            return false;
        const float outer[4] = {box.eyeTan[0], box.eyeTan[1], box.eyeTan[3], box.eyeTan[2]};
        const float focus[4] = {box.focusTan[0], box.focusTan[1], box.focusTan[3], box.focusTan[2]};
        std::memcpy(f.outerTan[e], outer, sizeof(outer));
        std::memcpy(f.focusTan[e], focus, sizeof(focus));
        f.turn[e] = {0, 0, 0, 1};
        const float o[4] = {0.5f * e, 0.0f, 0.5f, 1.0f};
        const float b[4] = {0.5f * e + 0.5f * static_cast<float>(box.rect[0]), static_cast<float>(box.rect[1]),
                            0.5f * static_cast<float>(box.rect[2] - box.rect[0]), static_cast<float>(box.rect[3] - box.rect[1])};
        std::memcpy(uv[e], o, sizeof(o));
        std::memcpy(uv[e + 2], b, sizeof(b));
    }
    f.live = true;
    for (int v = 0; v < 4; ++v) {
        f.width[v] = f.renderWidth[v] = 0.5;
        f.height[v] = f.renderHeight[v] = 1.0;
    }
    return true;
}

// The menu images (their UI drawn) into the menu atlas, the record stored: the layer composes the menu (four_passes_atlas).
void menu_publish(const simqvfr::Views4Frame& f, const float (&uv)[4][4]) {
    ID3D11Device* device = nullptr;
    menuAtlas->GetDevice(&device);
    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    for (int e = 0; e < 2; ++e)
        immediate->CopySubresourceRegion(menuAtlas, 0, e * menuW, 0, 0, menuTex[e], 0, nullptr);
    immediate->Release();
    device->Release();
    std::memcpy(menuUv, uv, sizeof(uv));
    {
        std::lock_guard guard(liveMutex);
        latestRecord = f;
        haveRecord = true;
    }
    menuReady = true;
}

// The four passes' hook (vrui_draw): the panels drawn again into the menu images at native, by the game's drawEye.
void menu_ui(void* vrui, ID3D11DeviceContext* context, const void* eyes) {
    simqvfr::Views4Frame f{};
    float uv[4][4];
    if (!menu_record(f, uv))
        return;  // the eyes as they are
    std::uint8_t block[kVruiEyesSize];
    std::memcpy(block, eyes, sizeof(block));
    draw_panels(vrui, context, block, menuRtv, static_cast<int>(menuW), static_cast<int>(menuH));
    menu_publish(f, uv);
}

// R6b, the multi-view path: the menus at native from the d3d11 calls alone (mvr_pass.h mvr_panel_draws), no call into
// the game's code. In a frame without passes, before the first VR UI panel draw into an eye's image the eyes are scaled
// up into the menu images (menu_background); after each panel draw the same draw into that eye's menu image at its size
// (mvr_redraw_panel); at the frame's end, both eyes' drawn, the atlas and the record (menu_finish).
bool menuFrameStarted = false, menuBackgroundOk = false, menuPanelDrawn[2] = {};
unsigned long long menuPanelReplays = 0, menuPanelMisses = 0;
bool menuPanelLogged = false;

void panel_draw(ID3D11DeviceContext* context, bool after, unsigned count, unsigned start) {
    if (!qvfrMode || !menuNative || passesThisFrame != 0)
        return;
    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv)
        return;
    ID3D11Resource* target = nullptr;
    rtv->GetResource(&target);
    int eye = -1;
    for (int e = 0; e < 2; ++e) {
        ID3D11Texture2D* t = game_eye_image(e);
        if (t && target && static_cast<ID3D11Resource*>(t) == target)
            eye = e;
        if (t)
            t->Release();
    }
    if (target)
        target->Release();
    if (eye >= 0 && !after && !menuFrameStarted) {
        menuFrameStarted = true;
        menuBackgroundOk = menu_background(context, rtv);
    } else if (eye >= 0 && after && menuBackgroundOk && menuRtv[eye]) {
        mvr_redraw_panel(context, menuRtv[eye], menuW, menuH, count, start);
        menuPanelDrawn[eye] = true;
        ++menuPanelReplays;
        if (!menuPanelLogged) {
            menuPanelLogged = true;
            simqvfr::logf("lmu qvfr: frame %llu: the VR UI's panel drawn again into the menu images at %ux%u (from its "
                          "d3d11 draw)",
                          frames, menuW, menuH);
        }
    } else if (eye < 0 && after) {
        ++menuPanelMisses;  // a panel into something else than an eye's image
    }
    rtv->Release();
}

// At the frame's end (xrEndFrame, before the layer takes the atlas): a menu frame whose panels were drawn into both menu
// images is published.
void menu_finish() {
    const bool both = menuPanelDrawn[0] && menuPanelDrawn[1];
    menuFrameStarted = menuBackgroundOk = menuPanelDrawn[0] = menuPanelDrawn[1] = false;
    if (!both || !menuAtlas)
        return;
    simqvfr::Views4Frame f{};
    float uv[4][4];
    if (menu_record(f, uv))
        menu_publish(f, uv);
}

// QVFR, after pass p: the eyes go into the atlas's top row while the focus views render into their targets (pass 2 =
// focus 0 in eye 0's, pass 3 = focus 1 in eye 1's), each focus view into its own image, then the eyes back for the
// game's VR UI and submit.
// R4a, after pass 1 of a two-pass frame: the eye loop ends here; the eyes stay in their targets with the focus finals
// beside them (the VR UI is doubled onto those after the loop). The frame's record and sizes from the focus finals;
// the atlas takes them at xrEndFrame (four_passes_atlas). No focus finals: no four views this frame.
void after_two_passes() {
    eyesInAtlas = frameFocusOk = false;
    atlasFromFinals = false;
    ID3D11Texture2D* e0 = game_eye_image(0);
    ID3D11Texture2D* e1 = game_eye_image(1);
    // The multi-view path, once: the copies' eye images are the targets the passes' final blits drew into.
    if (cfg.trackerDrives && !eyeImagesChecked && e0 && e1 && mvr_eye_final(0) && mvr_eye_final(1)) {
        eyeImagesChecked = true;
        const bool same = mvr_eye_final(0) == e0 && mvr_eye_final(1) == e1;
        simqvfr::logf("lmu qvfr: frame %llu: the eye images from the submit's copies %s the final blits' targets "
                      "(passes 0 and 1)",
                      frames, same ? "are" : "are NOT");
    }
    if (e0 && e1 && ensure_atlas(e0) && is_view(e1)) {
        for (int q = 0; q < 2; ++q)  // the first frame's sizes were taken before the atlas knew the views
            set_final_size(q);
        bool ok = true;
        for (int e = 0; e < 2 && ok; ++e) {
            unsigned x = 0, y = 0, w = 0, h = 0;
            const bool final = mvr_focus_final(e, x, y, w, h);
            ok = final && frozen[e] && x + w <= viewW && y + h <= viewH && w && h;
            if (!ok)  // the fallback's summary: the first reason found
                ++(!final ? finalsNoBlit : !frozen[e] ? finalsNoBox : finalsOutside);
            if (ok) {
                finalAt[e][0] = x;
                finalAt[e][1] = y;
                passW[2 + e] = w;  // the focus final: the focus image 1:1 beside the eye
                passH[2 + e] = h;
                extentW[2 + e] = w;
                extentH[2 + e] = h;
                boxes[e] = frozenBox[e];
            }
        }
        frameFinalsOk = ok;
        if (ok) {
            ++finalsBoth;
            atlasReady = atlasFromFinals = true;
            uiScaleOnly = vruiHooked;  // the VR UI's eye viewports still cut for live resolution; no focus_ui
            store_record();
            if (gpuSpans.ready(e0))
                gpuSpans.begin_frame();
        } else {
            ++finalsMissing;
            if (!finalsMissingLogged) {
                finalsMissingLogged = true;
                simqvfr::logf("lmu mvr: frame %llu: two passes without both focus finals: no four views this frame (the eyes "
                              "as they are)",
                              frames);
            }
        }
    } else {
        ++finalsNoViews;  // the eye images not found, or not the views' size
    }
    if (e0)
        e0->Release();
    if (e1)
        e1->Release();
}

void qvfr_after_pass(int p) {
    if (p == 1 && twoPassFrame) {
        after_two_passes();
        return;
    }
    if (p == 1) {
        eyesInAtlas = frameFocusOk = false;
        ID3D11Texture2D* e0 = eye_texture(0);
        ID3D11Texture2D* e1 = eye_texture(1);
        if (e0 && e1 && ensure_atlas(e0) && ensure_focus_textures(e0) && is_view(e1)) {
            for (int q = 0; q < 2; ++q)  // the first frame's sizes were taken before the atlas knew the views
                set_final_size(q);
            const bool timed = gpuSpans.ready(e0);
            if (timed) {
                gpuSpans.begin_frame();
                gpuSpans.mark(0, 0);
            }
            if (cfg.focusReplay) {
                ID3D11Texture2D* const eyes[2] = {e0, e1};
                keep_focus_finals(eyes);
            }
            atlas_in(e0, 0, 0, passW[0], passH[0]);
            atlas_in(e1, viewW, 0, passW[1], passH[1]);
            if (timed)
                gpuSpans.mark(0, 1);
            eyesInAtlas = true;
            frameFocusOk = swap_in_focus(0);
        }
        if (e0)
            e0->Release();
        if (e1)
            e1->Release();
    } else if (p == 2 && eyesInAtlas) {
        ID3D11Texture2D* t = eye_texture(0);
        gpuSpans.mark(1, 0);
        if (frameFocusOk && is_view(t))
            copy_corner(focusTex[0], t, passW[2], passH[2]);
        else
            frameFocusOk = false;
        gpuSpans.mark(1, 1);
        if (t)
            t->Release();
        restore_projection(0);
        if (frameFocusOk)
            frameFocusOk = swap_in_focus(1);
    } else if (p == 3 && eyesInAtlas) {
        ID3D11Texture2D* t0 = eye_texture(0);
        ID3D11Texture2D* t1 = eye_texture(1);
        gpuSpans.mark(2, 0);
        if (frameFocusOk && is_view(t1))
            copy_corner(focusTex[1], t1, passW[3], passH[3]);
        else
            frameFocusOk = false;
        restore_projection(1);
        if (is_view(t0))
            atlas_out(t0, 0, 0, passW[0], passH[0]);
        if (is_view(t1))
            atlas_out(t1, viewW, 0, passW[1], passH[1]);
        if (cfg.focusReplay) {  // R3: the focus finals back beside the eyes, for the VR UI's doubling
            ID3D11Texture2D* const eyes[2] = {is_view(t0) ? t0 : nullptr, is_view(t1) ? t1 : nullptr};
            put_back_focus_finals(eyes);
        }
        gpuSpans.mark(2, 1);
        if (t0)
            t0->Release();
        if (t1)
            t1->Release();
        eyesInAtlas = false;
        atlasReady = frameFocusOk;
        focusUiPending = atlasReady && vruiHooked;
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

// The work around one eye pass: render_scene's (the game's scene render, hooked), or with R4b's pass tracker driving
// (FourPassesRun::trackerDrives) the d3d11 bounds' (mvr_pass.h mvr_track_drive): pass_begin before the pass's draws,
// pass_end after them, on the render thread.
double passWall0 = 0;
unsigned long long passCpu0 = 0;
int trackedEyeLogged = 0;                    // tracked passes that rendered another eye: the first 5 logged
unsigned long long trackedEyeMismatches = 0;  // and counted

void pass_begin(int p) {
    renderThread = GetCurrentThreadId();
    pass = p;
    ++passesThisFrame;
    if (p == 0) {
        atlasReady = menuReady = false;  // a new frame: its own four views, or none
        frameFinalsOk = false;
    }
    unsigned long long hook0 = thread_cycles();
    if (qvfrMode)
        qvfr_before_pass(p);  // live resolution: the size this pass renders at
    if (qvfrMode && cfg.mvr && p < 4)
        mvr_before_pass(p);
    inLaterPass = p >= 2;
    // The frame capture hotkey as this frame's first pass sees it: its views (capture_views) and, read back pass by
    // pass, its glare (glare_share_capture_frame) come from the same frame.
    if (qvfrMode && p == 0) {
        frameCaptureArm = simqvfr::d3d11_capture_armed();
        if (glareHooked && capture_due(frameCaptureArm))
            glare_share_capture_frame();
    }
    // Which view YEBIS's glare belongs to: an eye's is kept; a focus view's (its projection swapped in) replaced by it.
    // Its region is the view's own width and height over its target's (MVR's side by side too: glare_math.h), both in
    // the main targets YEBIS runs in; YEBIS's scale (the render scale over the targets') only logged and checked to
    // cover it.
    if (glareHooked && p < 4) {
        unsigned m[2];
        main_size(m);
        if (!m[0] || !m[1]) {
            m[0] = viewW;
            m[1] = viewH;
        }
        glare_share_pass(p, p >= 2 && eyesInAtlas && frameFocusOk ? &boxes[p - 2] : nullptr, sceneW[p], sceneH[p], m[0],
                         m[1], gameScale > 0 ? *reinterpret_cast<const float*>(base() + kRenderScale) / gameScale : 0.0);
    }
    passWall0 = now_ms();
    passCpu0 = thread_cycles();
    costWindow.hookCycles += static_cast<double>(passCpu0 - hook0);
    inPass = true;
}

void pass_end(int p) {
    const double wall0 = passWall0;
    const unsigned long long cpu0 = passCpu0;
    inPass = false;
    if (qvfrMode && cfg.mvr && p < 4)
        mvr_pass_end(p);
    skipScene = false;
    const unsigned long long cpu1 = thread_cycles();
    if (glareHooked)
        glare_share_pass(-1, nullptr);
    if (qvfrMode && p < 4) {  // what the game's own pass cost (four_passes_frame_end logs the averages)
        costWindow.passWallMs[p] += now_ms() - wall0;
        costWindow.passCpuCycles[p] += static_cast<double>(cpu1 - cpu0);
        if (cfg.mvr && ab_run())
            ab.cpu[ab_side()][p] += static_cast<double>(cpu1 - cpu0);
    }
    inLaterPass = false;
    if (p < 4) {
        void* renderer = *reinterpret_cast<void**>(base() + kRenderer);
        eyeOfPass[p] = renderer ? *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewEye)
                                : -1;
        if (cfg.trackerDrives && p < 2 && eyeOfPass[p] != p) {
            ++trackedEyeMismatches;
            if (trackedEyeLogged < 5) {
                ++trackedEyeLogged;
                simqvfr::logf("lmu mvr: pass tracker: frame %llu: the tracked pass %d rendered the game's eye %d (%llu "
                              "such passes so far)",
                              frames, p, eyeOfPass[p], trackedEyeMismatches);
            }
        }
        if (qvfrMode) {
            qvfr_after_pass(p);
            if (p == 1 || p == 3)  // live resolution: the next passes' size, before their cameras copy it
                set_next_passes(p == 1 && !twoPassFrame ? 1 : 0);
            costWindow.hookCycles += static_cast<double>(thread_cycles() - cpu1);
        } else if (focusMode && passTarget == 4)
            focus_after_pass(p);
        else if (armed)
            copy_aside(p);
    }
}

void render_scene(void* arg, char laterEye) {
    const int p = laterEye ? pass + 1 : 0;
    pass_begin(p);
    originalRenderScene(arg, laterEye);
    pass_end(p);
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

// Gate D (lmu_mvr=2) at a sample: each focus view as composed (its pass's post-processing on the region its eye's pass
// drew) against the box's crop of its eye: linear luminance per cell, focus / eye; both saved for looking at.
void compare_mvr_focus() {
    const std::wstring dir = captures_folder();
    for (int eye = 0; eye < 2; ++eye) {
        const FocusBox& b = boxes[eye];
        ID3D11Texture2D* et = eye_texture(eye);
        std::vector<std::uint8_t> ep, fp;
        unsigned ew = 0, eh = 0, fw = 0, fh = 0;
        const bool read = b.valid && et && focusTex[eye] && simqvfr::d3d11_read_bgra8(et, ep, ew, eh) &&
                          simqvfr::d3d11_read_bgra8(focusTex[eye], fp, fw, fh);
        if (et)
            et->Release();
        const unsigned vw = std::min(passW[eye], ew), vh = std::min(passH[eye], eh);  // the eye's corner
        const unsigned cw = std::min(passW[2 + eye], fw), ch = std::min(passH[2 + eye], fh);  // the focus view's
        if (!read || !vw || !vh || !cw || !ch) {
            simqvfr::logf("lmu mvr: frame %llu: eye %d's images can't be read back; not compared", frames, eye);
            continue;
        }
        const auto lum = [](const std::vector<std::uint8_t>& px, std::size_t i) {
            return 0.0722 * linear(px[i]) + 0.7152 * linear(px[i + 1]) + 0.2126 * linear(px[i + 2]);
        };
        constexpr int kCells = 16;
        std::vector<double> ratios;
        for (int cy = 1; cy < kCells - 1; ++cy)
            for (int cx = 1; cx < kCells - 1; ++cx) {
                double le = 0, lf = 0;
                int ne = 0, nf = 0;
                const double u0 = static_cast<double>(cx) / kCells, u1 = static_cast<double>(cx + 1) / kCells;
                const double v0 = static_cast<double>(cy) / kCells, v1 = static_cast<double>(cy + 1) / kCells;
                for (unsigned y = static_cast<unsigned>(v0 * ch); y < static_cast<unsigned>(v1 * ch); y += 2)
                    for (unsigned x = static_cast<unsigned>(u0 * cw); x < static_cast<unsigned>(u1 * cw); x += 2) {
                        lf += lum(fp, (static_cast<std::size_t>(y) * fw + x) * 4);
                        ++nf;
                    }
                const double ex0 = (b.rect[0] + (b.rect[2] - b.rect[0]) * u0) * vw;
                const double ex1 = (b.rect[0] + (b.rect[2] - b.rect[0]) * u1) * vw;
                const double ey0 = (b.rect[1] + (b.rect[3] - b.rect[1]) * v0) * vh;
                const double ey1 = (b.rect[1] + (b.rect[3] - b.rect[1]) * v1) * vh;
                for (unsigned y = static_cast<unsigned>(ey0); y < static_cast<unsigned>(ey1) && y < vh; ++y)
                    for (unsigned x = static_cast<unsigned>(ex0); x < static_cast<unsigned>(ex1) && x < vw; ++x) {
                        le += lum(ep, (static_cast<std::size_t>(y) * ew + x) * 4);
                        ++ne;
                    }
                if (ne && nf && le / ne > 0.002)
                    ratios.push_back((lf / nf) / (le / ne));
            }
        std::sort(ratios.begin(), ratios.end());
        const auto pct = [&](double q) {
            return ratios.empty() ? 0.0 : ratios[static_cast<std::size_t>(q * static_cast<double>(ratios.size() - 1))];
        };
        simqvfr::logf("lmu mvr: frame %llu: focus %d as composed (its post-processing on the region pass %d drew) vs its "
                      "eye's crop: focus / eye luminance per cell median %.3f (10%% %.3f, 90%% %.3f, %zu cells)",
                      frames, eye, eye, pct(0.5), pct(0.1), pct(0.9), ratios.size());
        // The focus view and the eye's crop of the box (its own pixels), for looking at.
        std::vector<std::uint8_t> focusOnly(static_cast<std::size_t>(cw) * ch * 4);
        for (unsigned y = 0; y < ch; ++y)
            std::memcpy(focusOnly.data() + static_cast<std::size_t>(y) * cw * 4, fp.data() + static_cast<std::size_t>(y) * fw * 4,
                        static_cast<std::size_t>(cw) * 4);
        const unsigned bx0 = static_cast<unsigned>(b.rect[0] * vw), by0 = static_cast<unsigned>(b.rect[1] * vh);
        const unsigned bx1 = std::min(vw, static_cast<unsigned>(b.rect[2] * vw)), by1 = std::min(vh, static_cast<unsigned>(b.rect[3] * vh));
        if (bx1 > bx0 && by1 > by0) {
            const unsigned bw = bx1 - bx0, bh = by1 - by0;
            std::vector<std::uint8_t> crop(static_cast<std::size_t>(bw) * bh * 4);
            for (unsigned y = 0; y < bh; ++y)
                std::memcpy(crop.data() + static_cast<std::size_t>(y) * bw * 4,
                            ep.data() + (static_cast<std::size_t>(by0 + y) * ew + bx0) * 4, static_cast<std::size_t>(bw) * 4);
            wchar_t name[96];
            swprintf_s(name, L"\\lmu-mvr-frame%llu-eye%d-crop.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, crop, bw, bh);
        }
        wchar_t name[96];
        swprintf_s(name, L"\\lmu-mvr-frame%llu-focus%d-composed.png", frames, eye);
        simqvfr::write_png_bgra(dir + name, focusOnly, cw, ch);
    }
}

// R3's gate (lmu_focus_replay with gate D's samples): each eye's focus final (its pass's focus post, the final blit and
// the overlays doubled beside the eye; the VR UI after the passes) against pass 2/3's own focus view as composed (its
// post-processing, blit, driver tags and the VR UI by focus_ui). Expected: aligned 0,0, brightness ~1.00; the driver
// tags drawn at the eye's scale (remapped) where pass 2/3 draws them at its own. The first images compared for each eye
// saved (final, pass, and their difference x8), and up to two later ones that differ (over 0.5% of pixels > 4 off,
// or any pixel > 32 off: a driver tag's edge).
bool finalSaved[2] = {};
int finalDiffersSaved[2] = {};
void compare_focus_final() {
    const std::wstring dir = captures_folder();
    for (int eye = 0; eye < 2; ++eye) {
        if (!mvrFinalKept[eye]) {
            simqvfr::logf("lmu mvr: frame %llu: focus final %d: none this frame (no final blit replayed); not compared", frames,
                          eye);
            continue;
        }
        ID3D11Texture2D* et = eye_texture(eye);
        std::vector<std::uint8_t> ep, pp;
        unsigned ew = 0, eh = 0, pw = 0, ph = 0;
        const bool read = et && focusTex[eye] && simqvfr::d3d11_read_bgra8(et, ep, ew, eh) &&
                          simqvfr::d3d11_read_bgra8(focusTex[eye], pp, pw, ph);
        if (et)
            et->Release();
        const unsigned* at = mvrFinalAt[eye];
        if (!read || at[0] + at[2] > ew || at[1] + at[3] > eh) {
            simqvfr::logf("lmu mvr: frame %llu: focus final %d: the images can't be read back; not compared", frames, eye);
            continue;
        }
        const unsigned w = std::min({at[2], pw, passW[2 + eye]}), h = std::min({at[3], ph, passH[2 + eye]});
        const auto crop = [&](const std::vector<std::uint8_t>& px, unsigned stride, unsigned x0, unsigned y0) {
            std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
            for (unsigned y = 0; y < h; ++y)
                std::memcpy(out.data() + static_cast<std::size_t>(y) * w * 4,
                            px.data() + (static_cast<std::size_t>(y0 + y) * stride + x0) * 4, static_cast<std::size_t>(w) * 4);
            return out;
        };
        const std::vector<std::uint8_t> a = crop(pp, pw, 0, 0), b = crop(ep, ew, at[0], at[1]);
        const MvrComparison c = mvr_compare_bgra8(a, b, w, h);
        simqvfr::logf("lmu mvr: frame %llu: focus final %d (beside eye %d at %u,%u, %ux%u) vs pass %d's own (%ux%u), %ux%u "
                      "compared: %saligned at %+d,%+d px; brightness %.4f (cell median %.4f); mean luminance difference "
                      "%.2f%%, %.2f%% of pixels >10%%; bytes: largest %d, mean %.2f, %.2f%% of pixels >4",
                      frames, eye, eye, at[0], at[1], at[2], at[3], 2 + eye, passW[2 + eye], passH[2 + eye], w, h,
                      c.ok ? "" : "(too small) ", c.shiftX, c.shiftY, c.brightness, c.cellMedian, 100 * c.meanDiff,
                      c.differingPct, c.maxByte, c.meanByte, c.bytesOverPct);
        const bool differs = c.ok && (c.bytesOverPct > 0.5 || c.maxByte > 32) && finalDiffersSaved[eye] < 2;
        if (!finalSaved[eye] || differs) {
            finalDiffersSaved[eye] += finalSaved[eye] ? 1 : 0;
            finalSaved[eye] = true;
            std::vector<std::uint8_t> diff(a.size());
            for (std::size_t i = 0; i < a.size(); ++i)
                diff[i] = i % 4 == 3 ? 255
                                     : static_cast<std::uint8_t>(std::min(255, 8 * std::abs(static_cast<int>(a[i]) -
                                                                                             static_cast<int>(b[i]))));
            wchar_t name[96];
            swprintf_s(name, L"\\lmu-final-frame%llu-focus%d-final.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, b, w, h);
            swprintf_s(name, L"\\lmu-final-frame%llu-focus%d-pass.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, a, w, h);
            swprintf_s(name, L"\\lmu-final-frame%llu-focus%d-diff8.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, diff, w, h);
            simqvfr::logf("lmu mvr: images in %ls (lmu-final-frame%llu-focus%d-final/pass/diff8.png)", dir.c_str(), frames,
                          eye);
        }
    }
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

// ---- QVFR diagnostic: the render scale changed live (FourPassesRun::renderScaleProbe) ----
// Scene frames 600-719 at the probe's share of the scale, the images saved at 660; then the scale as it was.
constexpr unsigned long long kProbeStart = 600, kProbeShot = 60, kProbeEnd = 120;  // the last two after the start
float probeOldScale = 0;
int probeStage = 0;  // 0 waiting, 1 scaled, 2 saved, 3 done
unsigned long long probeFrom = 0;

int probe_active() { return probeStage == 1 || probeStage == 2; }

void probe_save(ID3D11Texture2D* texture, const wchar_t* what) {
    std::vector<std::uint8_t> px;
    unsigned w = 0, h = 0;
    if (!texture || !simqvfr::d3d11_read_bgra8(texture, px, w, h)) {
        simqvfr::logf("lmu render scale probe: the %ls image can't be read back", what);
        return;
    }
    // How much of it the frame drew: the bounding box of pixels that aren't black.
    unsigned x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (unsigned y = 0; y < h; y += 2)
        for (unsigned x = 0; x < w; x += 2) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            if (px[i] > 4 || px[i + 1] > 4 || px[i + 2] > 4) {
                x0 = (std::min)(x0, x);
                y0 = (std::min)(y0, y);
                x1 = (std::max)(x1, x);
                y1 = (std::max)(y1, y);
            }
        }
    wchar_t name[96];
    swprintf_s(name, L"\\lmu-scale-probe-frame%llu-%ls.png", frames, what);
    simqvfr::write_png_bgra(captures_folder() + name, px, w, h);
    simqvfr::logf("lmu render scale probe: %ls image %ux%u saved (%ls); not black within x %u-%u y %u-%u", what, w, h, name + 1,
                  x0, x1, y0, y1);
}

void render_scale_probe(int passes) {
    if (!cfg.renderScaleProbe || probeStage == 3)
        return;
    auto* scale = reinterpret_cast<float*>(base() + kRenderScale);
    if (probeStage == 0) {
        if (sceneFrames < kProbeStart || passes < 4)
            return;
        probeOldScale = *scale;
        *scale = probeOldScale * static_cast<float>(cfg.renderScaleProbe) / 100.0f;
        probeFrom = sceneFrames;
        probeStage = 1;
        simqvfr::logf("lmu render scale probe: frame %llu: render scale %.3f -> %.3f for %llu scene frames (views %ux%u)",
                      frames, probeOldScale, *scale, kProbeEnd, viewW, viewH);
    } else if (probeStage == 1 && sceneFrames >= probeFrom + kProbeShot && passes >= 4) {
        probeStage = 2;
        ID3D11Texture2D* e0 = eye_texture(0);
        if (e0) {
            D3D11_TEXTURE2D_DESC d;
            e0->GetDesc(&d);
            simqvfr::logf("lmu render scale probe: frame %llu: the eye target is %ux%u (the views at launch %ux%u)", frames,
                          d.Width, d.Height, viewW, viewH);
        }
        probe_save(e0, L"eye0");
        if (e0)
            e0->Release();
        probe_save(focusTex[0], L"focus0");
    } else if (probeStage >= 1 && sceneFrames >= probeFrom + kProbeEnd) {
        *scale = probeOldScale;
        probeStage = 3;
        simqvfr::logf("lmu render scale probe: frame %llu: render scale back to %.3f", frames, probeOldScale);
    }
}

// ---- QVFR: the four views at the frame capture hotkey ----
// [experiment] d3d11_capture_hotkey=1: each press saves the swapchains' images (probe/d3d11_capture.h); the next composed
// frame's four views are saved too, each as far as it rendered, and the log gives each focus view's box in its eye:
// what tools/halo_profile.py and tools/focus_compare.py measure.
void capture_views() {
    const std::uint64_t arm = frameCaptureArm;  // as this frame's pass 0 saw it (render_scene)
    if (arm == seenCaptureArm)
        return;
    seenCaptureArm = arm;
    if (arm == 0 || arm == 0xFFFFFFFFull)  // no session, or the hotkey not pressed yet
        return;
    std::vector<std::uint8_t> px;
    unsigned w = 0, h = 0;
    if (!simqvfr::d3d11_read_bgra8(atlas, px, w, h) || w < 2 * viewW || h < 2 * viewH) {
        simqvfr::logf("lmu qvfr: frame capture: frame %llu's views couldn't be read back", frames);
        return;
    }
    const std::wstring dir = captures_folder();
    const wchar_t* names[4] = {L"eye0", L"eye1", L"focus0", L"focus1"};
    std::wstring paths[4];
    for (int v = 0; v < 4; ++v) {
        const unsigned x0 = (v % 2) * viewW, y0 = (v / 2) * viewH;
        const unsigned cw = std::clamp(passW[v], 1u, viewW), ch = std::clamp(passH[v], 1u, viewH);
        std::vector<std::uint8_t> view(static_cast<std::size_t>(cw) * ch * 4);
        for (unsigned y = 0; y < ch; ++y)
            std::memcpy(view.data() + static_cast<std::size_t>(y) * cw * 4,
                        px.data() + (static_cast<std::size_t>(y0 + y) * w + x0) * 4, static_cast<std::size_t>(cw) * 4);
        wchar_t name[64];
        swprintf_s(name, L"lmu-qvfr-frame%llu-%ls.png", frames, names[v]);
        paths[v] = (std::filesystem::path(dir) / name).wstring();
        if (!simqvfr::write_png_bgra(paths[v], view, cw, ch))
            simqvfr::logf("lmu qvfr: frame capture: %ls not written", paths[v].c_str());
    }
    for (int e = 0; e < 2; ++e) {
        const double* r = boxes[e].rect;
        simqvfr::logf("lmu qvfr: frame capture %llu: eye %d with focus view %d (glare share %s): python "
                      "tools/halo_profile.py --eye-image \"%ls\" --focus-image \"%ls\" --box %.5f %.5f %.5f %.5f",
                      frames, e, e, glare_share_on() ? "on" : "off", paths[e].c_str(), paths[e + 2].c_str(), r[0], r[1],
                      r[2], r[3]);
    }
    wchar_t stem[64];
    swprintf_s(stem, L"lmu-qvfr-frame%llu", frames);
    glare_share_save_capture(dir, stem);
}

}  // namespace

// The eye loop four times: the camera history records, the scene render and post-effect time hooks, the loop head and
// tail patches (the tail's count immediate). At start, or later when the multi-view path falls back
// (fallback_to_four_passes: between frames, on the render thread, so the eye loop isn't running). False: not installed
// (anything done undone that can be).
bool install_four_pass_hooks() {
    for (auto& record : history)
        reinterpret_cast<HistoryCtorFn>(base() + kCameraHistoryCtor)(record);
    void* target = reinterpret_cast<void*>(base() + kRenderScene);
    void* deltaTime = reinterpret_cast<void*>(base() + kPostEffectSetDeltaTime);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&render_scene), reinterpret_cast<void**>(&originalRenderScene)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        simqvfr::logf("%s: the scene render couldn't be hooked; not installed", tag());
        return false;
    }
    if (MH_CreateHook(deltaTime, reinterpret_cast<void*>(&set_delta_time),
                      reinterpret_cast<void**>(&originalSetDeltaTime)) != MH_OK ||
        MH_EnableHook(deltaTime) != MH_OK) {
        MH_DisableHook(target);
        simqvfr::logf("%s: the post-effect frame time couldn't be hooked; not installed", tag());
        return false;
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
        MH_DisableHook(deltaTime);
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
    return true;
}

// The lens mask's draw hooked (the four passes: focus passes skip it; a stale mask skipped). Before the eye loop is changed.
bool install_mask_hook() {
    void* mask = reinterpret_cast<void*>(base() + kHiddenAreaDraw);
    if (!simqvfr::code_matches(kHiddenAreaDraw, kHiddenAreaDrawAnchor, sizeof(kHiddenAreaDrawAnchor)) ||
        MH_CreateHook(mask, reinterpret_cast<void*>(&mask_draw), reinterpret_cast<void**>(&originalMaskDraw)) != MH_OK ||
        MH_EnableHook(mask) != MH_OK) {
        simqvfr::log_line("lmu focus: the hidden-area mask's draw isn't this build's or couldn't be hooked; not installed");
        return false;
    }
    return true;
}

// The VR UI's draw hooked (the four passes: into the focus views too; menus at native).
void install_vrui_hook() {
    void* vrui = reinterpret_cast<void*>(base() + kVruiDraw);
    vruiHooked = simqvfr::code_matches(kVruiDraw, kVruiDrawAnchor, sizeof(kVruiDrawAnchor)) &&
                 simqvfr::code_matches(kVruiDrawEye, kVruiDrawEyeAnchor, sizeof(kVruiDrawEyeAnchor)) &&
                 MH_CreateHook(vrui, reinterpret_cast<void*>(&vrui_draw), reinterpret_cast<void**>(&originalVruiDraw)) ==
                     MH_OK &&
                 MH_EnableHook(vrui) == MH_OK;
    if (!vruiHooked)
        simqvfr::log_line("lmu qvfr: the VR UI's draw isn't this build's or couldn't be hooked: the HUD and menus are "
                          "missing inside the focus box");
}

// QVFR with the four passes: the driver tags' viewport cut and the focus views' glare from their eyes'.
void install_four_pass_extras() {
    // The driver tags in the part of each target the pass rendered (live resolution); without it a pass rendered
    // smaller draws them too far out and too large.
    void* labelsDraw = reinterpret_cast<void*>(base() + kLabelsDraw);
    labelsHooked = simqvfr::code_matches(kLabelsDraw, kLabelsDrawAnchor, sizeof(kLabelsDrawAnchor)) &&
                   MH_CreateHook(labelsDraw, reinterpret_cast<void*>(&labels_draw),
                                 reinterpret_cast<void**>(&originalLabelsDraw)) == MH_OK &&
                   MH_EnableHook(labelsDraw) == MH_OK;
    if (!labelsHooked)
        simqvfr::log_line("lmu qvfr: the driver tags' draw isn't this build's or couldn't be hooked: with live "
                          "resolution below 100% they show a second time in the periphery");
    // The focus views' glare from their eye's (glare_share.h); hooked even when off at launch, for the live switch.
    glareHooked = glare_share_start();
    if (glareHooked)
        glare_share_set(cfg.glareShare);
    simqvfr::logf("lmu qvfr: glare share: %s", !glareHooked     ? "not available (each view keeps its own glare)"
                                               : cfg.glareShare ? "on: the focus views take their eye's glare (bloom, "
                                                                  "star, ghosts) through the box; lmu_glare_share=0 "
                                                                  "turns it off"
                                                                : "off at launch (lmu_glare_share=0): each view's own "
                                                                  "glare");
}

// R5: the multi-view path (the tracker driving the game's two passes) gives way to the four passes without MVR, for
// the rest of the session: at xrCreateSession when the device can't do it, or at a frame end (render thread, between
// frames) when MVR fails while running. Logged once with `why`.
bool fellBack = false;

// What the multi-view path did before it stopped, step by step, so a log shows which one the focus views didn't get past:
// the eye passes' room check and focus box (mvr_before_pass), drawing the focus view, the focus post's copy, composite
// and final blit (mvr_pass), and the frame's two focus finals (after_two_passes). Session totals.
void log_multi_view_summary() {
    const MvrPassStats s = mvr_pass_stats();
    const MvrProbeStats probe = mvr_probe_stats();
    const auto u = [](unsigned long long v) { return v; };
    simqvfr::logf("lmu mvr: before it stopped: %llu two-pass frames, %llu with both focus finals; without them: %llu no "
                  "final blit beside an eye, %llu no focus box, %llu a final outside the image, %llu the eye images not "
                  "the views; eye passes: %llu refused for room (%llu on a frame after a scene frame)%s%s, %llu without a "
                  "focus box, %llu drew their focus view (%llu twin binds, %llu stereo switches refused); focus post: %llu "
                  "copies, %llu composites (%llu by their bindings), %llu final blits, %llu steps failed, %llu passes "
                  "without the composite after the copy, %llu draws into the resolve's target by another shader; YEBIS's "
                  "copy, composite and final blit pixel shaders marked %llu, %llu and %llu times",
                  twoPassFrames, finalsBoth, finalsNoBlit, finalsNoBox, finalsOutside, finalsNoViews, mvrRoomRefused,
                  mvrRoomRefusedLater, mvrLastRefusal[0] ? "; the last: " : "", mvrLastRefusal, mvrNoBox,
                  u(s.passesWithFocus), u(s.twinBinds), u(s.stereoErrors), u(s.focusCopies), u(s.focusComposites),
                  u(s.focusCompositesByBindings), u(s.focusBlits), u(s.focusFailures), u(s.focusNoComposite),
                  u(s.focusUnknownComposites), u(probe.yebisCopies), u(probe.yebisComposites), u(probe.finalBlits));
}

void fallback_to_four_passes(const char* why) {
    if (fellBack || !cfg.trackerDrives)
        return;
    fellBack = true;
    if (twoPassFrames)  // it ran (not the session start's fallback, where the device can't)
        log_multi_view_summary();
    mvr_track_drive(nullptr, nullptr);
    mvr_track_passes(false);
    mvr_focus_replay(false);
    mvr_panel_draws(nullptr);  // the menus by the VR UI's hook from now on
    mvr_mask_stale(nullptr);   // and the mask by its own
    cfg.trackerDrives = cfg.focusFinal = cfg.focusReplay = cfg.passTracker = false;
    cfg.mvr = cfg.mvrSkip = false;
    twoPassFrame = false;
    menuFrameStarted = menuBackgroundOk = menuPanelDrawn[0] = menuPanelDrawn[1] = false;
    if (focusMode && !install_mask_hook()) {
        simqvfr::logf("lmu mvr: frame %llu: multi-view rendering stopped (%s) and the lens mask couldn't be hooked for the "
                      "four passes: the eyes only (no focus views) for the rest of the session",
                      frames, why);
        return;
    }
    if (!install_four_pass_hooks()) {
        simqvfr::logf("lmu mvr: frame %llu: multi-view rendering stopped (%s) and the four passes couldn't be installed: "
                      "the eyes only (no focus views) for the rest of the session",
                      frames, why);
        return;
    }
    if (qvfrMode) {
        install_vrui_hook();
        install_four_pass_extras();
    }
    simqvfr::logf("lmu mvr: frame %llu: multi-view rendering stopped (%s): the four passes from the next frame (the focus "
                  "views rendered by passes 2-3), for the rest of the session",
                  frames, why);
}

bool four_passes_start(const FourPassesRun& run) {
    if (installed)
        return true;
    cfg = run;
    cfg.count = std::clamp(cfg.count, 1, 16);
    cfg.every = (std::max)(cfg.every, 1);
    cfg.warmup = std::clamp(cfg.warmup, 1, 600);
    qvfrMode = run.qvfr;
    focusMode = run.focus || run.qvfr;
    menuNative = run.qvfr && run.menuNative && run.layerWidth > 0 && run.layerHeight > 0;
    layerShare[0] = run.layerWidth;
    layerShare[1] = run.layerHeight;
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
    // R4b (cfg.trackerDrives): the game's own two eye passes, found at the d3d11 level, and (R6) the lens mask and the VR
    // UI from their d3d11 draws: no game code hooked. The four passes' hooks (the mask's, install_four_pass_hooks, the VR
    // UI's, the extras) only if it falls back to them (fallback_to_four_passes).
    const bool driven = cfg.trackerDrives;
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        simqvfr::logf("%s: MH_Initialize failed", tag());
        return false;
    }
    if (driven) {
        installed = true;
        mvr_time_hooks(cfg.mvrTiming);
        if (qvfrMode) {
            std::lock_guard guard(liveMutex);
            simqvfr::logf("lmu qvfr: on with multi-view rendering and the pass tracker: the game's own two eye passes draw "
                          "the focus views beside the eyes, a %.0f%% x %.0f%% box of each eye's tangent span on the gaze; "
                          "game code hooked: none (the lens mask and the VR UI handled at their d3d11 draws); the eye "
                          "loop, the scene render, the post-effect time, the scene skip, glare share, the driver tags, the "
                          "mask and the VR UI hooked only if it falls back to the four passes",
                          focusFraction * 100, (focusFractionY > 0 ? focusFractionY : focusFraction) * 100);
        }
        return true;
    }
    if (focusMode && !install_mask_hook())  // first: nothing of the eye loop is changed before it is in
        return false;
    if (!install_four_pass_hooks())
        return false;
    installed = true;
    if (qvfrMode)
        install_vrui_hook();
    if (qvfrMode && cfg.mvr && cfg.mvrSkip) {  // gate D: passes 2-3 may skip the scene; mvr_pass keeps the focus regions
        cfg.mvrSkip = install_skip();
        mvr_keep_focus(cfg.mvrSkip);
        if (cfg.mvrAb > 0)
            simqvfr::logf("lmu mvr: A/B run: %d frames with MVR, then %d with the four passes alone, in turn (lmu_mvr_ab)",
                          cfg.mvrAb, cfg.mvrAb);
        mvr_time_hooks(cfg.mvrTiming);
        simqvfr::logf("lmu mvr: gate D: %s", cfg.mvrSkip ? "passes 2-3 skip the scene once their eye's pass drew the focus "
                                                          "view, their post-processing on that region"
                                                        : "the scene work's functions aren't this build's or couldn't be "
                                                          "hooked: passes 2-3 render the scene as before");
    }
    if (qvfrMode) {
        install_four_pass_extras();
        std::lock_guard guard(liveMutex);
        simqvfr::logf("lmu qvfr: on: the eye loop runs four times every frame, passes 2-3 render focus views (a %.0f%% x "
                      "%.0f%% box of each eye's tangent span, on the gaze) into the eye targets, set aside for the layer "
                      "to compose; post-effect frame time held in passes 2-3: %s; hidden-area mask off in them; the VR UI "
                      "drawn into them: %s",
                      focusFraction * 100, (focusFractionY > 0 ? focusFractionY : focusFraction) * 100,
                      holdExposure ? "yes" : "no", vruiHooked ? "yes" : "no");
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

void four_passes_device(ID3D11Device* device) {
    sessionDevice = device;
    // R5: the device decides; the game is still in its menus, the eye loop not yet run.
    if (installed && cfg.trackerDrives && !mvr_probe_usable())
        fallback_to_four_passes("the game's device can't draw two views a pass (no NVIDIA multi-view rendering, and no "
                                "vertex shader picking the viewport), or multi-view's shader probe is off");
    else if (installed && cfg.trackerDrives)
        simqvfr::logf("lmu qvfr: multi-view rendering by %s%s", mvr_backend_name(mvr_probe_backend()),
                      mvr_probe_backend() == MvrBackend::nvapi && mvr_probe_instanced()
                          ? "; instancing takes over if the driver ever refuses it"
                          : "");
}

void four_passes_frame_end() {
    if (!installed)
        return;
    ++frames;
    if (cfg.mvr && cfg.passTracker && !mvrInstalled && sessionDevice)  // the tracker sees the menus too
        if (!install_mvr(sessionDevice))
            fallback_to_four_passes("multi-view's context hooks couldn't be installed");
    simqvfr::dlss_frame_end(frames);
    if (cfg.mvr) {
        mvr_frame_end(frames);
        frozen[0] = frozen[1] = false;
        if (cfg.mvrSkip && mvrSampleFrame && atlasReady) {
            compare_mvr_focus();
            if (cfg.focusReplay)
                compare_focus_final();
        }
        mvrSampleFrame = false;
        if (cfg.mvrSkip && frames % 900 == 0) {
            simqvfr::logf("lmu mvr: passes 2-3 skipped the scene in %llu passes (%llu not: the focus view not drawn by its "
                          "eye's pass); skipped since the last: %llu view scenes, %llu sort buckets, %llu culled items, "
                          "%llu scene graph culls",
                          passesSkipped, passesNotSkipped, skipped[0], skipped[1], skipped[2], skipped[3]);
            for (unsigned long long& n : skipped)
                n = 0;
        }
    }
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
    lastFrameScene = passes >= 2;  // the next frame's room check: its passes at the live resolution
    if (passes >= 4)
        ++fourPassFrames;
    if (twoPassFrame && passes >= 2 && twoPassFrames++ == 0)
        simqvfr::logf("%s: frame %llu: first two-pass frame (focus finals), eyes rendered %d %d", tag(), frames, eyeOfPass[0],
                      eyeOfPass[1]);
    // R5: the multi-view path's health, between frames on the render thread; it gives way to the four passes when MVR
    // fails while running (fallback_to_four_passes).
    if (cfg.trackerDrives) {
        if (passes >= 1)
            noFinalsRun = frameFinalsOk ? 0 : noFinalsRun + 1;
        // FSR on: each eye's post-processing ends in FSR's upscale (compute) instead of the final blit, which the focus
        // final beside the eye is drawn with; the four passes take FSR's upscale for every view.
        const char* why = mvr_broken()           ? "the driver refused NVIDIA's multi-view rendering (and instancing "
                                                   "couldn't take over)"
                          : fsr_on()             ? "LMU's FSR upscaling is on: it replaces the final blit the focus views "
                                                   "are drawn beside the eyes with"
                          : noFinalsRun >= 90     ? "no focus views made in 90 frames in a row"
                          : cfg.fallbackAt > 0 && sceneFrames >= static_cast<unsigned long long>(cfg.fallbackAt)
                              ? "forced by lmu_mvr_fallback_at"
                              : nullptr;
        // MSAA off (mvr_pass.h mvr_single_sample_passes): the passes found on the scene's single-sample target, the
        // focus views not made. The four passes would hook the game's code: the eyes alone instead, said once.
        if (why && noFinalsRun >= 90 && !mvr_broken() && !fsr_on() && mvr_single_sample_passes()) {
            if (!singleSampleRefusalLogged) {
                singleSampleRefusalLogged = true;
                simqvfr::logf("lmu mvr: frame %llu: no focus views made in 90 frames with MSAA off (the passes found on "
                              "the scene's single-sample target): not falling back to the four passes, which hook the "
                              "game's code; the eyes as they are until they come",
                              frames);
            }
            why = nullptr;
        }
        if (why)
            fallback_to_four_passes(why);
    }
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

    const int fullPasses = twoPassFrame ? 2 : 4;  // R4a: a two-pass frame is complete after pass 1
    if (qvfrMode) {
        if (passes < fullPasses) {
            atlasReady = false;  // no four views this frame (menus, loading): the layer submits the eyes as they are
            seenCaptureArm = simqvfr::d3d11_capture_armed();  // a frame capture now has no views to save (not later)
            // ... so they render at the game's own scale, filling their targets (the next scene pass sets it again).
            if (!probe_active() && gameScale > 0 && lastWritten != gameScale) {
                *reinterpret_cast<float*>(base() + kRenderScale) = gameScale;
                lastWritten = gameScale;
            }
        }
        render_scale_probe(passes);
        if (eyesInAtlas) {
            eyesInAtlas = false;
            simqvfr::logf("lmu qvfr: frame %llu stopped after %d passes with the eyes set aside", frames, passes);
        }
        if (focusUiPending) {  // VR_Present didn't draw its UI this frame
            focusUiPending = false;
            ++uiSkipped;
        }
        uiScaleOnly = false;
        if (cfg.trackerDrives) {
            menu_finish();  // R6b: before the layer takes the atlas (views4_submit, after this)
            maskStaleThisFrame += mvr_mask_dropped();
        }
        const bool stale = maskStale.load(std::memory_order_relaxed);
        if (stale != maskStaleLogged) {
            maskStaleLogged = stale;
            if (stale)
                simqvfr::logf("lmu qvfr: frame %llu: the located FOV no longer matches the lens mask's (a live FOV crop "
                              "change): the mask is off (%d draws skipped this frame)",
                              frames, maskStaleThisFrame);
            else
                simqvfr::logf("lmu qvfr: frame %llu: the located FOV matches the lens mask's again: the mask is on", frames);
        }
        maskStaleThisFrame = 0;
        if (glareHooked)
            glare_share_frame_end(frames);
        // Every 900 frames while the scene runs: the sizes the passes rendered at (a performance comparison needs them),
        // and what the frames since cost: each pass's wall and CPU time, this code's own CPU, the GPU time of its copies.
        if (passes >= fullPasses) {
            ++costWindow.frames;
            if (cfg.mvr && ab_run())
                ++ab.frames[ab_side()];
        }
        if (passes >= fullPasses && frames % 900 == 0) {
            unsigned mainWH[2];
            main_size(mainWH);
            std::uint32_t rec[2];
            real_recommended(rec);
            if (mainWH[0] && (mainWH[0] != rec[0] || mainWH[1] != rec[1]))  // render scale not 1: scene and eye target apart
                simqvfr::logf("lmu qvfr: frame %llu: passes rendered at %ux%u %ux%u %ux%u %ux%u in %ux%u main targets "
                              "(render scale %.3f, FSR %s), in the %ux%u eye targets %ux%u %ux%u %ux%u %ux%u",
                              frames, sceneW[0], sceneH[0], sceneW[1], sceneH[1], sceneW[2], sceneH[2], sceneW[3], sceneH[3],
                              mainWH[0], mainWH[1], gameScale, fsr_on() ? "on" : "off", viewW, viewH, passW[0], passH[0],
                              passW[1], passH[1], passW[2], passH[2], passW[3], passH[3]);
            else
                simqvfr::logf("lmu qvfr: frame %llu: passes rendered at %ux%u %ux%u %ux%u %ux%u in %ux%u targets", frames,
                              passW[0], passH[0], passW[1], passH[1], passW[2], passH[2], passW[3], passH[3], viewW, viewH);
            const CostWindow& w = costWindow;
            if (w.frames && tscPerMs > 0) {
                const double n = w.frames, g = w.gpuFrames ? w.gpuFrames : 1;
                const auto cpu = [&](double cycles) { return cycles / tscPerMs / n; };
                simqvfr::logf("lmu qvfr cost: %u frames: passes wall %.2f %.2f %.2f %.2f ms, CPU %.2f %.2f %.2f %.2f ms "
                              "(eyes %.2f, focus views %.2f ms CPU per frame); this code's CPU %.3f ms per frame; GPU of "
                              "its copies %.3f + %.3f + %.3f + %.3f = %.3f ms per frame (%u frames timed: eyes aside, focus 0, "
                              "focus 1 and eyes back, atlas)",
                              w.frames, w.passWallMs[0] / n, w.passWallMs[1] / n, w.passWallMs[2] / n, w.passWallMs[3] / n,
                              cpu(w.passCpuCycles[0]), cpu(w.passCpuCycles[1]), cpu(w.passCpuCycles[2]),
                              cpu(w.passCpuCycles[3]), cpu(w.passCpuCycles[0] + w.passCpuCycles[1]),
                              cpu(w.passCpuCycles[2] + w.passCpuCycles[3]), cpu(w.hookCycles), w.gpuMs[0] / g,
                              w.gpuMs[1] / g, w.gpuMs[2] / g, w.gpuMs[3] / g,
                              (w.gpuMs[0] + w.gpuMs[1] + w.gpuMs[2] + w.gpuMs[3]) / g, w.gpuFrames);
            }
            if (cfg.mvr && cfg.focusFinalAb > 0 && ab.frames[0] && ab.frames[1] && tscPerMs > 0) {
                double c[2][4];
                for (int m = 0; m < 2; ++m)
                    for (int q = 0; q < 4; ++q)
                        c[m][q] = ab.cpu[m][q] / tscPerMs / ab.frames[m];
                const double two = c[0][0] + c[0][1] + c[0][2] + c[0][3], four = c[1][0] + c[1][1] + c[1][2] + c[1][3];
                simqvfr::logf("lmu mvr A/B (focus finals%s): CPU per pass with two passes %.2f %.2f ms (%.2f; %u frames), "
                              "with four %.2f %.2f %.2f %.2f ms (%.2f; %u frames): %+.2f ms per frame; %llu two-pass frames "
                              "without both focus finals so far",
                              cfg.focusFinalAbPlain ? " vs plain QVFR" : " vs gate D", c[0][0], c[0][1], two, ab.frames[0],
                              c[1][0], c[1][1], c[1][2], c[1][3], four, ab.frames[1], two - four, finalsMissing);
            }
            if (cfg.mvr && cfg.backendAb > 0 && backendAbOk && ab.frames[0] && ab.frames[1] && tscPerMs > 0) {
                double c[2][2];
                for (int m = 0; m < 2; ++m)
                    for (int q = 0; q < 2; ++q)
                        c[m][q] = ab.cpu[m][q] / tscPerMs / ab.frames[m];
                const MvrPassStats ms = mvr_pass_stats();
                simqvfr::logf("lmu mvr A/B (NVIDIA's vs instancing): CPU per pass with NVIDIA's %.2f %.2f "
                              "ms (%.2f; %u frames), with instancing %.2f %.2f ms (%.2f; %u frames): instancing %+.2f ms "
                              "per frame; %llu draws doubled so far, %llu into the eye alone",
                              c[0][0], c[0][1], c[0][0] + c[0][1], ab.frames[0], c[1][0], c[1][1], c[1][0] + c[1][1],
                              ab.frames[1], c[1][0] + c[1][1] - c[0][0] - c[0][1],
                              static_cast<unsigned long long>(ms.doubledDraws),
                              static_cast<unsigned long long>(ms.eyeOnlyDraws));
            }
            if (cfg.mvr && car_ab_on() && ab.frames[0] && ab.frames[1] && tscPerMs > 0) {
                double c[2][2];
                for (int m = 0; m < 2; ++m)
                    for (int q = 0; q < 2; ++q)
                        c[m][q] = ab.cpu[m][q] / tscPerMs / ab.frames[m];
                simqvfr::logf("lmu mvr A/B (other cars' motion): CPU per pass with it %.2f %.2f ms (%.2f; %u frames), "
                              "without %.2f %.2f ms (%.2f; %u frames): the cars' motion %+.3f ms per frame (the hooks' "
                              "detours on both sides)",
                              c[0][0], c[0][1], c[0][0] + c[0][1], ab.frames[0], c[1][0], c[1][1], c[1][0] + c[1][1],
                              ab.frames[1], c[0][0] + c[0][1] - c[1][0] - c[1][1]);
            }
            if (cfg.mvr && cfg.mvrAb > 0 && ab.frames[0] && ab.frames[1] && tscPerMs > 0) {
                double c[2][4], eyes[2], focus[2];
                for (int m = 0; m < 2; ++m) {
                    for (int q = 0; q < 4; ++q)
                        c[m][q] = ab.cpu[m][q] / tscPerMs / ab.frames[m];
                    eyes[m] = c[m][0] + c[m][1];
                    focus[m] = c[m][2] + c[m][3];
                }
                simqvfr::logf("lmu mvr A/B: CPU per pass with MVR %.2f %.2f %.2f %.2f ms (eyes %.2f, focus views %.2f; %u "
                              "frames), without %.2f %.2f %.2f %.2f ms (eyes %.2f, focus views %.2f; %u frames): MVR %+.2f ms "
                              "on the eyes, %+.2f on the focus views, %+.2f per frame",
                              c[0][0], c[0][1], c[0][2], c[0][3], eyes[0], focus[0], ab.frames[0], c[1][0], c[1][1], c[1][2],
                              c[1][3], eyes[1], focus[1], ab.frames[1], eyes[0] - eyes[1], focus[0] - focus[1],
                              eyes[0] + focus[0] - eyes[1] - focus[1]);
            }
            ab = AbWindow{};
            costWindow = CostWindow{};
        }
        if (frames % 1800 == 0)
            simqvfr::logf("lmu qvfr: frame %llu: %llu scene frames, %llu four-pass, %llu composed, %llu not (the eye images "
                          "unlike the views); VR UI in the focus views %llu, not %llu; last frame %d passes, frame time "
                          "held in %d calls, mask skipped in %d; views %ux%u; driver tag draws cut %llu, kept %llu; tracked "
                          "passes on another eye %llu",
                          frames, sceneFrames, fourPassFrames, composedFrames, refusedFrames, uiDrawn, uiSkipped, passes,
                          held, maskSkipped, viewW, viewH, labelsCut, labelsKept, trackedEyeMismatches);
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
    if (cfg.trackerDrives)
        learn_eye_images(eyes);  // this frame's submit copies: the next frame's eye images
    if (qvfrMode && menuReady && menuAtlas) {  // a menu frame, at native (menu_ui)
        menuReady = false;
        std::memcpy(uv, menuUv, sizeof(uv));
        if (menuFrames++ == 0)
            simqvfr::logf("lmu qvfr: frame %llu: the first menu frame composed at %ux%u (its UI drawn at that size)", frames,
                          menuW, menuH);
        return menuAtlas;
    }
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
    // The eyes again, as the game submitted them: with what it drew after its eye loop (the VR UI); the focus views with
    // the UI drawn into them too (vrui_draw).
    // Each only as far as its pass rendered (live resolution: the top-left of the launch-size targets).
    const unsigned long long c0 = thread_cycles();
    gpuSpans.mark(3, 0);
    atlas_in(eyes[0], 0, 0, passW[0], passH[0]);
    atlas_in(eyes[1], viewW, 0, passW[1], passH[1]);
    if (atlasFromFinals) {  // R4a: the focus finals beside the eyes, the VR UI doubled onto them
        atlasFromFinals = false;
        atlas_in_from(eyes[0], finalAt[0][0], finalAt[0][1], 0, viewH, passW[2], passH[2]);
        atlas_in_from(eyes[1], finalAt[1][0], finalAt[1][1], viewW, viewH, passW[3], passH[3]);
    } else {
        atlas_in(focusTex[0], 0, viewH, passW[2], passH[2]);
        atlas_in(focusTex[1], viewW, viewH, passW[3], passH[3]);
    }
    gpuSpans.mark(3, 1);
    gpuSpans.end_frame();
    costWindow.hookCycles += static_cast<double>(thread_cycles() - c0);
    // Each view's exact part of the atlas cell (extentW: below the game's render scale 1, the stretched part).
    const double cw = 0.5 / viewW, ch = 0.5 / viewH;
    for (int e = 0; e < 2; ++e) {
        const float outer[4] = {0.5f * e, 0.0f, static_cast<float>(cw * extentW[e]), static_cast<float>(ch * extentH[e])};
        const float focus[4] = {0.5f * e, 0.5f, static_cast<float>(cw * extentW[e + 2]),
                                static_cast<float>(ch * extentH[e + 2])};
        std::memcpy(uv[e], outer, sizeof(outer));
        std::memcpy(uv[e + 2], focus, sizeof(focus));
    }
    if (composedFrames++ == 0)
        simqvfr::logf("lmu qvfr: frame %llu: the first four views handed to the compositor", frames);
    const bool live = passW[0] != viewW || passW[2] != viewW || passH[0] != viewH || passH[2] != viewH;
    if (live != liveLogged) {
        liveLogged = live;
        simqvfr::logf("lmu qvfr: frame %llu: views rendered at outer %ux%u, focus %ux%u of %ux%u%s", frames, passW[0],
                      passH[0], passW[2], passH[2], viewW, viewH, live ? " (live resolution)" : " (the launch size)");
    }
    capture_views();
    return atlas;
}

void four_passes_note_views(const XrView* views, uint32_t count) {
    if (!views || count < 2)
        return;
    std::lock_guard guard(liveMutex);
    bool stale = false;
    for (int e = 0; e < 2; ++e) {
        const XrFovf& f = views[e].fov;
        const float t[4] = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleDown), std::tan(f.angleUp)};
        std::memcpy(locatedTan[e], t, sizeof(t));
        if (maskKnown[e])
            for (int k = 0; k < 4; ++k)
                stale = stale || std::fabs(t[k] - maskTan[e][k]) > 2e-4f;
    }
    locatedValid = true;
    maskStale.store(stale, std::memory_order_relaxed);
}

void four_passes_mask_taken(uint32_t viewIndex) {
    if (viewIndex >= 2)
        return;
    std::lock_guard guard(liveMutex);
    maskKnown[viewIndex] = locatedValid;
    std::memcpy(maskTan[viewIndex], locatedTan[viewIndex], sizeof(maskTan[viewIndex]));
    const float* t = maskTan[viewIndex];
    simqvfr::logf("lmu qvfr: eye %u's lens mask fetched: built with the FOV located last (tangents l %.3f r %.3f d %.3f "
                  "u %.3f)%s",
                  viewIndex, t[0], t[1], t[2], t[3], locatedValid ? "" : " -- none located yet: the mask is always drawn");
}

void four_passes_set_live_scales(double outer, double focus) {
    const float o = static_cast<float>(std::clamp(outer, 0.1, 1.0)), f = static_cast<float>(std::clamp(focus, 0.1, 1.0));
    std::lock_guard guard(liveMutex);
    if (o == liveScale[0] && f == liveScale[1])
        return;
    liveScale[0] = o;
    liveScale[1] = f;
    simqvfr::logf("lmu qvfr: live resolution: outer views at %.0f%%, focus views at %.0f%% of the launch size per axis "
                  "(from the next frame)",
                  o * 100, f * 100);
}

void four_passes_set_live_axes(double outerWidth, double outerHeight, double focusWidth, double focusHeight) {
    const auto share = [](double v) { return static_cast<float>(std::clamp(v, 0.05, 1.0)); };
    const float a[2][2] = {{share(outerWidth), share(outerHeight)}, {share(focusWidth), share(focusHeight)}};
    std::lock_guard guard(liveMutex);
    if (std::memcmp(a, liveAxes, sizeof(a)) == 0)
        return;
    std::memcpy(liveAxes, a, sizeof(a));
    if (cfg.perAxis)
        simqvfr::logf("lmu qvfr: live resolution per axis: outer views at %.0f%% x %.0f%%, focus views at %.0f%% x %.0f%% "
                      "of the target (from the next frame)",
                      a[0][0] * 100, a[0][1] * 100, a[1][0] * 100, a[1][1] * 100);
}

void four_passes_view_size(int (&size)[2]) {
    size[0] = static_cast<int>(viewW);
    size[1] = static_cast<int>(viewH);
}

}  // namespace lmu

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
#include <string>
#include <vector>

#include "../../engine/code_patch.h"
#include "../../log.h"
#include "../../probe/d3d11_capture.h"
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

RenderSceneFn originalRenderScene = nullptr;
SetDeltaTimeFn originalSetDeltaTime = nullptr;
bool installed = false;
bool focusMode = false;      // passes 2-3 are focus views (else copies of the eyes)
double focusFraction = 0.3;  // the focus box: share of the eye's tangent width and height

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

// Passes 2-3's camera history records (the game keeps two on its stack, one per eye).
alignas(16) std::uint8_t history[2][kCameraHistorySize];

// The render thread's view of the frame (render_scene and frame_end both run on it; checked).
unsigned long renderThread = 0;
int pass = -1;  // this frame's pass, 0-3
int passesThisFrame = 0;
int eyeOfPass[4] = {-1, -1, -1, -1};
unsigned long long frames = 0, fourPassFrames = 0, comparisons = 0, nextCompare = 0;
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
    bool ok = true;
    for (int k = 0; k < 2; ++k) {
        std::memcpy(eyeM[k], slot_matrix(eye, k), sizeof(eyeM[k]));
        // As ACC: a share of the eye's tangent span, centred on the eye's forward axis (gaze and offsets come later).
        ok = focus_projection(eyeM[k], focusFraction, focusFraction, 0, 0, box[k], focusM[k]) && ok;
    }
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
    inLaterPass = p >= 2;
    originalRenderScene(arg, laterEye);
    inLaterPass = false;
    if (p < 4) {
        void* renderer = *reinterpret_cast<void**>(base() + kRenderer);
        eyeOfPass[p] = renderer ? *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewEye)
                                : -1;
        if (focusMode)
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

const char* tag() { return focusMode ? "lmu focus" : "lmu copies"; }

}  // namespace

bool four_passes_start(bool focus, bool hold, double fraction) {
    if (installed)
        return true;
    focusMode = focus;
    holdExposure = hold;
    focusFraction = std::clamp(fraction, 0.05, 1.0);
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
    installed = true;
    if (focusMode)
        simqvfr::logf("lmu focus: on: the eye loop runs four times, passes 2-3 render focus views (a %.0f%% box of each "
                      "eye's tangent span, centred on its forward axis) into the eye targets, set aside before the game "
                      "submits; the eyes are shown as normal. Focus and eye compared 300 frames into four-pass rendering, "
                      "then every 3600 frames (four times at most); frame time held: %s",
                      focusFraction * 100, holdExposure ? "yes" : "no");
    else
        simqvfr::logf("lmu copies: on: the eye loop runs four times, passes 2-3 render eyes 0-1 again; their images are "
                      "compared 300 frames into four-pass rendering, then every 3600 frames (four times at most); passes "
                      "2-3's post-effect frame time %s",
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
    if (passes >= 4)
        ++fourPassFrames;
    if (fourPassFrames == 1 && passes >= 4)
        simqvfr::logf("%s: frame %llu: first four-pass frame, eyes rendered %d %d %d %d; post-effect frame time "
                      "held in passes 2-3: %s (%d calls this frame, the game's %.5f s -> %.5f s)",
                      tag(), frames, eyeOfPass[0], eyeOfPass[1], eyeOfPass[2], eyeOfPass[3], holdExposure ? "yes" : "no",
                      heldThisFrame, lastGameDeltaTime, kHeldDeltaTime);
    const int held = heldThisFrame;
    heldThisFrame = 0;
    const bool due = passes >= 4 && fourPassFrames >= 300 && comparisons < 4 && frames >= nextCompare;
    if (focusMode) {
        if (due && focusFrameComplete) {
            if (GetCurrentThreadId() != renderThread)
                simqvfr::log_line("lmu focus: xrEndFrame isn't on the render thread; images not read back");
            else
                compare_focus();
            nextCompare = frames + 3600;
        }
    } else {
        if (armed) {
            armed = false;
            if (GetCurrentThreadId() != renderThread)
                simqvfr::log_line("lmu copies: xrEndFrame isn't on the render thread; images not read back");
            else if (captured == 0xf)
                compare_copies();
            else
                simqvfr::logf("lmu copies: frame %llu: only passes 0x%x copied aside (%d passes); not compared", frames,
                              captured, passes);
        }
        if (due) {
            armed = true;
            captured = 0;
            nextCompare = frames + 3600;
        }
    }
    if (frames % 1800 == 0)
        simqvfr::logf("%s: frame %llu: %llu four-pass frames so far; last frame %d passes, eyes %d %d %d %d, "
                      "frame time held in %d calls",
                      tag(), frames, fourPassFrames, passes, eyeOfPass[0], eyeOfPass[1], eyeOfPass[2], eyeOfPass[3], held);
}

}  // namespace lmu

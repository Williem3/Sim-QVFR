#include "four_passes.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>

#include <algorithm>
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
#include "game.h"

namespace lmu {
namespace {

using RenderSceneFn = void (*)(void* arg, char laterEye);
using TargetFn = void* (*)(void* renderer);
using SurfaceFn = void* (*)(void* texture, unsigned usage);
using D3DObjectFn = void* (*)(void* surface);
using HistoryCtorFn = void (*)(void* record);

RenderSceneFn originalRenderScene = nullptr;
bool installed = false;

// Passes 2-3's camera history records (the game keeps two on its stack, one per eye).
alignas(16) std::uint8_t history[2][kCameraHistorySize];

// The render thread's view of the frame (render_scene and frame_end both run on it; checked).
unsigned long renderThread = 0;
int pass = -1;           // this frame's pass, 0-3
int passesThisFrame = 0;
int eyeOfPass[4] = {-1, -1, -1, -1};
unsigned long long frames = 0, fourPassFrames = 0, comparisons = 0, nextCompare = 0;
bool armed = false;      // copy each pass's image aside this frame
unsigned captured = 0;   // bit p: pass p's image copied
ID3D11Texture2D* copies[4] = {};
bool loggedTarget = false;

std::uintptr_t base() { return simqvfr::module_base(); }

// The texture the pass just finished in (the current eye target's D3D11 texture), AddRef'd; null if it can't be had.
ID3D11Texture2D* eye_target(int& eye) {
    void* renderer = *reinterpret_cast<void**>(base() + kRenderer);
    if (!renderer)
        return nullptr;
    eye = *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewEye);
    void* texture = reinterpret_cast<TargetFn>(base() + kGetCurrentEyeTarget)(renderer);
    void* surface = texture ? reinterpret_cast<SurfaceFn>(base() + kTextureSurface)(texture, kRenderTargetUsage) : nullptr;
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
    return result;
}

void copy_aside(int p) {
    int eye = -1;
    ID3D11Texture2D* source = eye_target(eye);
    if (!source) {
        simqvfr::logf("lmu copies: pass %d: the eye target's texture can't be had; not compared", p);
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    if (!loggedTarget) {
        loggedTarget = true;
        simqvfr::logf("lmu copies: eye target %ux%u format %d samples %u mips %u", desc.Width, desc.Height,
                      static_cast<int>(desc.Format), desc.SampleDesc.Count, desc.MipLevels);
    }
    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    if (copies[p]) {
        D3D11_TEXTURE2D_DESC have;
        copies[p]->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
            copies[p]->Release();
            copies[p] = nullptr;
        }
    }
    if (!copies[p]) {
        D3D11_TEXTURE2D_DESC aside = desc;
        aside.MipLevels = 1;
        aside.ArraySize = 1;
        aside.Usage = D3D11_USAGE_DEFAULT;
        aside.BindFlags = 0;
        aside.CPUAccessFlags = 0;
        aside.MiscFlags = 0;
        if (desc.SampleDesc.Count != 1 || FAILED(device->CreateTexture2D(&aside, nullptr, &copies[p])))
            copies[p] = nullptr;
    }
    if (copies[p]) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        context->CopySubresourceRegion(copies[p], 0, 0, 0, 0, source, 0, nullptr);
        context->Release();
        captured |= 1u << p;
    }
    device->Release();
    source->Release();
}

void render_scene(void* arg, char laterEye) {
    renderThread = GetCurrentThreadId();
    pass = laterEye ? pass + 1 : 0;
    const int p = pass;
    ++passesThisFrame;
    originalRenderScene(arg, laterEye);
    if (p < 4) {
        void* renderer = *reinterpret_cast<void**>(base() + kRenderer);
        eyeOfPass[p] = renderer ? *reinterpret_cast<const int*>(static_cast<const std::uint8_t*>(renderer) + kRendererViewEye)
                                : -1;
        if (armed)
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

void compare() {
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
        int maxDiff = 0;
        unsigned x0 = w[a], y0 = h[a], x1 = 0, y1 = 0;
        std::vector<std::uint8_t> diff(px[a].size(), 0);
        for (unsigned y = 0; y < h[a]; ++y)
            for (unsigned x = 0; x < w[a]; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * w[a] + x) * 4;
                int d = 0;
                for (int c = 0; c < 3; ++c)
                    d = (std::max)(d, std::abs(static_cast<int>(px[a][i + c]) - static_cast<int>(px[b][i + c])));
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
                          "different, within x %u-%u y %u-%u (%ux%u; eyes rendered %d/%d)",
                          frames, b, eye, differing, 100.0 * differing / total, maxDiff,
                          static_cast<double>(sum) / differing, x0, x1, y0, y1, w[a], h[a], eyeOfPass[a], eyeOfPass[b]);
        // Images: on the first comparison, and the first time a pair differs.
        static bool wroteDiff[2] = {};
        if (comparisons == 1 || (differing && !wroteDiff[eye])) {
            wchar_t name[96];
            unsigned hw = 0, hh = 0;
            swprintf_s(name, L"\\lmu-copies-frame%llu-eye%d.png", frames, eye);
            simqvfr::write_png_bgra(dir + name, half(px[a], w[a], h[a], hw, hh), hw, hh);
            swprintf_s(name, L"\\lmu-copies-frame%llu-pass%d.png", frames, b);
            simqvfr::write_png_bgra(dir + name, half(px[b], w[b], h[b], hw, hh), hw, hh);
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

}  // namespace

bool four_passes_start_copies() {
    if (installed)
        return true;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!simqvfr::games::supported_build(kGame, simqvfr::sha256_file(exe))) {
        simqvfr::log_line("lmu copies: not the build its addresses are from; not installed");
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
    };
    for (const Anchor& a : anchors)
        if (!simqvfr::code_matches(a.rva, a.bytes, a.size)) {
            simqvfr::logf("lmu copies: the %s isn't this build's; not installed", a.name);
            return false;
        }
    for (auto& record : history)
        reinterpret_cast<HistoryCtorFn>(base() + kCameraHistoryCtor)(record);
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        simqvfr::log_line("lmu copies: MH_Initialize failed");
        return false;
    }
    void* target = reinterpret_cast<void*>(base() + kRenderScene);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&render_scene), reinterpret_cast<void**>(&originalRenderScene)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        simqvfr::log_line("lmu copies: the scene render couldn't be hooked; not installed");
        return false;
    }
    // The head first: with two passes `edi & 1` is `edi`, so it changes nothing until the tail counts to four.
    // Head: the camera update gets the eye as edi & 1 (passes 2-3 set up eyes 0-1's cameras, not eyes 2-3).
    static const std::uint8_t head[] = {0x44, 0x8b, 0xc7,        // mov r8d, edi
                                        0x41, 0x83, 0xe0, 0x01,  // and r8d, 1
                                        0xb2, 0x01};             // mov dl, 1
    // Tail: count to four; from pass 2 on, rbx points at our history records instead of past the game's two.
    std::uint8_t tail[] = {0xff, 0xc7,                    // inc edi
                           0x83, 0xff, 0x02,              // cmp edi, 2
                           0x75, 0x0c,                    // jne add
                           0x48, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rbx, history
                           0xeb, 0x04,                    // jmp cmp4
                           0x48, 0x83, 0xc3, 0x6c,        // add: add rbx, 0x6c
                           0x83, 0xff, 0x04};             // cmp4: cmp edi, 4 (the game's jl follows)
    const auto records = reinterpret_cast<std::uint64_t>(&history[0][0]);
    std::memcpy(tail + 9, &records, 8);
    if (!simqvfr::code_redirect(kEyeLoopHead, kEyeLoopHeadAnchor, sizeof(kEyeLoopHeadAnchor), head, sizeof(head))) {
        MH_DisableHook(target);
        simqvfr::log_line("lmu copies: the eye loop head couldn't be patched; not installed");
        return false;
    }
    if (!simqvfr::code_redirect(kEyeLoopTail, kEyeLoopTailAnchor, sizeof(kEyeLoopTailAnchor), tail, sizeof(tail))) {
        simqvfr::log_line("lmu copies: the eye loop tail couldn't be patched; two passes as before");
        return false;
    }
    installed = true;
    simqvfr::log_line("lmu copies: on: the eye loop runs four times, passes 2-3 render eyes 0-1 again; their images are "
                      "compared 300 frames into four-pass rendering, then every 3600 frames (four times at most)");
    return true;
}

void four_passes_frame_end() {
    if (!installed)
        return;
    ++frames;
    const int passes = passesThisFrame;
    passesThisFrame = 0;
    if (passes >= 4)
        ++fourPassFrames;
    if (fourPassFrames == 1 && passes >= 4)
        simqvfr::logf("lmu copies: frame %llu: first four-pass frame, eyes rendered %d %d %d %d", frames, eyeOfPass[0],
                      eyeOfPass[1], eyeOfPass[2], eyeOfPass[3]);
    if (armed) {
        armed = false;
        if (GetCurrentThreadId() != renderThread)
            simqvfr::log_line("lmu copies: xrEndFrame isn't on the render thread; images not read back");
        else if (captured == 0xf)
            compare();
        else
            simqvfr::logf("lmu copies: frame %llu: only passes 0x%x copied aside (%d passes); not compared", frames, captured,
                          passes);
    }
    if (passes >= 4 && fourPassFrames >= 300 && comparisons < 4 && frames >= nextCompare) {
        armed = true;
        captured = 0;
        nextCompare = frames + 3600;
    }
    if (frames % 1800 == 0)
        simqvfr::logf("lmu copies: frame %llu: %llu four-pass frames so far; last frame %d passes, eyes %d %d %d %d", frames,
                      fourPassFrames, passes, eyeOfPass[0], eyeOfPass[1], eyeOfPass[2], eyeOfPass[3]);
}

}  // namespace lmu

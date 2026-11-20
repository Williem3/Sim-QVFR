#include "fixation.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <windows.h>

#include "../../dev_build.h"
#include "../../engine/dlss_math.h"
#include "../../log.h"
#include "../../quad/d3d11_fixation.h"

namespace scs {

bool fixation_depth_terms(const float (&m)[4][4], bool rows, double& a, double& b) {
    // z and w as functions of the position (x, y, z, 1).
    double z[4], w[4];
    for (int i = 0; i < 4; ++i) {
        z[i] = rows ? m[2][i] : m[i][2];
        w[i] = rows ? m[3][i] : m[i][3];
    }
    const double ww = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
    if (!(ww > 1e-12) || !std::isfinite(ww))
        return false;
    const double s = (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / ww;
    double residual = 0, zz = 0;
    for (int i = 0; i < 3; ++i) {
        residual += (z[i] - s * w[i]) * (z[i] - s * w[i]);
        zz += z[i] * z[i];
    }
    // Within a thousandth of z's own size (float matrices); z exactly 0 (an infinite far plane) is s = 0.
    if (!(residual <= 1e-6 * zz + 1e-14 * ww))
        return false;  // z isn't w scaled: not the world -> clip matrix of a perspective view
    const double t = z[3] - s * w[3];
    double nearM = 0, farM = 0;
    bool infinite = false;
    if (!std::isfinite(t) || !simqvfr::dlss::near_far(s, t, nearM, farM, infinite) || nearM > 5)
        return false;
    a = s;
    b = t;
    return true;
}

namespace {

using Microsoft::WRL::ComPtr;

std::mutex mutex;  // the render thread's calls against fixation_note_fov and fixation_stop
simqvfr::D3D11Fixation tracker;
bool wanted = true, started = false;
// Whether the read aims the boxes ([experiment] scs_fixation_converge; Ctrl+Alt+V toggles it live in dev builds).
std::atomic<bool> converge{true};
bool vKeyDown = false;
ComPtr<ID3D11Device> device;
float fov[2][4] = {{-1, 1, -1, 1}, {-1, 1, -1, 1}};
ComPtr<ID3D11Texture2D> depth;  // the left eye's G-buffer depth while its pass runs
bool leftPass = false;
std::uint64_t frames = 0, drawsThisPass = 0, leftPasses = 0;

// The depth target has no shader binding (ATS 2026-11-19: D32_FLOAT_S8X24_UINT, bind 0x40): a whole copy of it (d3d11
// copies depth only whole) into a texture of ours of its typeless format, every kDepthCopyEvery left passes.
constexpr std::uint64_t kDepthCopyEvery = 4;
ComPtr<ID3D11Texture2D> depthCopy;
D3D11_TEXTURE2D_DESC depthCopyDesc{};
std::uint64_t depthCopies = 0;
bool copyFailedSaid = false;

DXGI_FORMAT typeless_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// The texture the probe reads for this pass: the game's when a shader can read it, else this pass's copy of it (null:
// not this pass, or no copy).
ID3D11Texture2D* readable_depth(ID3D11DeviceContext* context) {
    D3D11_TEXTURE2D_DESC d;
    depth->GetDesc(&d);
    if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        return depth.Get();
    if (leftPasses % kDepthCopyEvery != 0)
        return nullptr;
    const DXGI_FORMAT f = typeless_of(d.Format);
    if (f == DXGI_FORMAT_UNKNOWN || d.ArraySize != 1 || d.MipLevels != 1)
        return nullptr;
    if (!depthCopy || depthCopyDesc.Width != d.Width || depthCopyDesc.Height != d.Height || depthCopyDesc.Format != f ||
        depthCopyDesc.SampleDesc.Count != d.SampleDesc.Count) {
        depthCopy.Reset();
        D3D11_TEXTURE2D_DESC c = d;
        c.Format = f;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        c.MiscFlags = 0;
        c.CPUAccessFlags = 0;
        c.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device->CreateTexture2D(&c, nullptr, &depthCopy))) {
            if (!copyFailedSaid)
                simqvfr::logf("scs fixation: no texture for the depth's copy (%ux%u format %d): no reads", d.Width,
                              d.Height, static_cast<int>(f));
            copyFailedSaid = true;
            return nullptr;
        }
        depthCopyDesc = c;
        simqvfr::logf("scs fixation: the depth target can't be read by a shader (format %d, bind 0x%x): copied whole "
                      "into one of ours (%ux%u, %.0f MB) every %llu frames",
                      static_cast<int>(d.Format), d.BindFlags, d.Width, d.Height,
                      d.Width * static_cast<double>(d.Height) * (f == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4) / 1048576.0,
                      static_cast<unsigned long long>(kDepthCopyEvery));
    }
    context->CopyResource(depthCopy.Get(), depth.Get());
    ++depthCopies;
    return depthCopy.Get();
}

// ---- The scene matrix's depth terms ----
// The regular readback: a ring of 64-byte staging buffers, each with the frame it was queued in.
constexpr int kRing = 3;
ComPtr<ID3D11Buffer> staging[kRing];
std::uint64_t stagedAt[kRing] = {};
std::uint64_t lastCopy = 0;
double terms[2] = {};
bool haveTerms = false;
std::uint64_t matrices = 0, rejected = 0, attempts = 0;
constexpr std::uint64_t kCopyEvery = 30;  // frames between matrix copies (the planes hardly change)
// The world's terms only. The cab's first few draws have their own (ATS 2026-11-19: 0.1-15 m against the world's
// 0.2-4000 m) into the same depth, but the world's put both eyes' boxes together on the dash and on the road alike
// (headset A/B 2026-11-19); the cab's didn't. A set whose far plane is within kWorldFarM is passed over.
constexpr double kWorldFarM = 100;
int termsSaid = 0;

void take_matrices(ID3D11DeviceContext* context) {
    for (int i = 0; i < kRing; ++i) {
        if (!stagedAt[i])
            continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (context->Map(staging[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK)
            continue;
        float v[4][4];
        std::memcpy(v, m.pData, sizeof(v));
        context->Unmap(staging[i].Get(), 0);
        stagedAt[i] = 0;
        double a = 0, b = 0;
        const bool ok = fixation_depth_terms(v, true, a, b) || fixation_depth_terms(v, false, a, b);
        ++(ok ? matrices : rejected);
        double nearM = 0, farM = 0;
        bool infinite = false;
        if (!ok || !simqvfr::dlss::near_far(a, b, nearM, farM, infinite) || (!infinite && farM < kWorldFarM))
            continue;
        const bool changed = !haveTerms || std::fabs(terms[0] - a) > 1e-6 * (1 + std::fabs(a)) ||
                             std::fabs(terms[1] - b) > 1e-6 * (1 + std::fabs(b));
        if (changed && termsSaid < 4) {
            ++termsSaid;
            simqvfr::logf("scs fixation: the scene's depth runs %.3f m to %s%.0f m (%s; depth = %.6f + %.6f / w)", nearM,
                          infinite ? "past " : "", farM, b > 0 ? "reversed" : "0 near", a, b);
        }
        terms[0] = a;
        terms[1] = b;
        haveTerms = true;
    }
}

// Rows 4-7 of the vertex shader's slot-0 range copied into `dst`. False: no such range.
bool copy_rows(ID3D11DeviceContext1* context, ID3D11Buffer* dst) {
    ID3D11Buffer* buf = nullptr;
    UINT first = 0, count = 0;
    context->VSGetConstantBuffers1(0, 1, &buf, &first, &count);
    if (!buf)
        return false;
    D3D11_BUFFER_DESC d;
    buf->GetDesc(&d);
    const UINT total = d.ByteWidth / 16;
    if (!count)
        count = total;
    const bool ok = first + 8 <= total && count >= 8;
    if (ok) {
        const D3D11_BOX box{(first + 4) * 16, 0, 0, (first + 8) * 16, 1, 1};
        context->CopySubresourceRegion(dst, 0, 0, 0, 0, buf, 0, &box);
    }
    buf->Release();
    return ok;
}

}  // namespace

void fixation_wanted(bool on, bool aim) {
    wanted = on;
    converge.store(aim, std::memory_order_relaxed);
}

void fixation_start(ID3D11Device* d) {
    std::lock_guard guard(mutex);
    if (started || !d || !wanted)
        return;
    device = d;
    D3D11_BUFFER_DESC desc{64, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    bool ok = true;
    for (int i = 0; i < kRing && ok; ++i) {
        ok = SUCCEEDED(d->CreateBuffer(&desc, nullptr, &staging[i]));
        stagedAt[i] = 0;
    }
    started = ok && tracker.start(d, "scs fixation");
    haveTerms = false;
    termsSaid = 0;
    frames = lastCopy = matrices = rejected = attempts = leftPasses = depthCopies = 0;
    if (started)
        simqvfr::logf("scs fixation: the boxes %s ([experiment] scs_fixation_converge%s)",
                      converge.load() ? "aimed at the depth read" : "parallel (reads only)",
                      simqvfr::kDevBuild ? "; Ctrl+Alt+V toggles it" : "");
    if (!started) {
        for (auto& s : staging)
            s.Reset();
        device.Reset();
    }
}

void fixation_stop() {
    std::lock_guard guard(mutex);
    if (!started)
        return;
    started = false;
    tracker.stop();
    for (auto& s : staging)
        s.Reset();
    depth.Reset();
    depthCopy.Reset();
    device.Reset();
}

void fixation_note_fov(const float (&f)[2][4]) {
    std::lock_guard guard(mutex);
    std::memcpy(fov, f, sizeof(fov));
}

void fixation_scene_draw(ID3D11DeviceContext1* context) {
    if (!started || !leftPass || !context)
        return;
    ++drawsThisPass;
    // A draw well into the pass (the world's, past the cab's first few draws and the clears), another each time (a
    // shader whose rows 4-7 aren't its matrix is passed over the next time), at most once in kCopyEvery frames.
    if (drawsThisPass != 96 + (attempts * 53) % 400 || (lastCopy && frames < lastCopy + kCopyEvery))
        return;
    std::lock_guard guard(mutex);
    int slot = -1;
    for (int i = 0; i < kRing; ++i)
        if (!stagedAt[i]) {
            slot = i;
            break;
        }
    if (slot >= 0 && copy_rows(context, staging[slot].Get())) {
        stagedAt[slot] = frames + 1;
        lastCopy = frames;
        ++attempts;
    }
}

void fixation_gbuffer_begin(ID3D11DepthStencilView* dsv) {
    leftPass = false;
    drawsThisPass = 0;
    if (!started || !dsv)
        return;
    ComPtr<ID3D11Resource> r;
    dsv->GetResource(&r);
    depth.Reset();
    leftPass = r && SUCCEEDED(r.As(&depth));
    if (leftPass)
        ++leftPasses;
}

void fixation_gbuffer_end(ID3D11DeviceContext* context, const float (&centre)[2], float fx, float fy, unsigned eyeW,
                          unsigned eyeH) {
    const bool was = leftPass;
    leftPass = false;
    if (!was || !started || !haveTerms || !depth || !eyeW || !eyeH || !context) {
        depth.Reset();
        return;
    }
    ID3D11Texture2D* readable = readable_depth(context);
    if (!readable) {
        depth.Reset();
        return;
    }
    float f[4];
    {
        std::lock_guard guard(mutex);
        std::memcpy(f, fov[0], sizeof(f));
    }
    const int rect[4] = {0, 0, std::max(1, static_cast<int>(fx * eyeW)), std::max(1, static_cast<int>(fy * eyeH))};
    const int px = std::clamp(static_cast<int>(centre[0] * rect[2]), 0, rect[2] - 1);
    const int py = std::clamp(static_cast<int>(centre[1] * rect[3]), 0, rect[3] - 1);
    const double tx = f[0] + centre[0] * (f[1] - f[0]), ty = f[3] - centre[1] * (f[3] - f[2]);
    tracker.request(readable, rect, px, py, std::max(2, rect[2] / 150), tx, ty, terms[0], terms[1]);
    depth.Reset();
}

void fixation_frame_end() {
    if (!started)
        return;
    ++frames;
    if constexpr (simqvfr::kDevBuild) {  // Ctrl+Alt+V: the boxes parallel <-> aimed (a live A/B)
        const bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
        const bool down = chord && (GetAsyncKeyState('V') & 0x8000);
        if (down && !vKeyDown) {
            const bool aim = !converge.load();
            converge.store(aim);
            simqvfr::logf("scs fixation: Ctrl+Alt+V: the boxes %s", aim ? "aimed at the depth read" : "parallel");
        }
        vKeyDown = down;
    }
    {
        std::lock_guard guard(mutex);
        if (device) {
            ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            take_matrices(context.Get());
        }
    }
    tracker.frame_end();
    if (frames % 900 == 0)
        simqvfr::logf("scs fixation: scene matrices read %llu, not a perspective view %llu; depth copies %llu",
                      static_cast<unsigned long long>(matrices), static_cast<unsigned long long>(rejected),
                      static_cast<unsigned long long>(depthCopies));
}

double fixation_m() { return converge.load(std::memory_order_relaxed) ? tracker.metres() : 0.0; }

}  // namespace scs

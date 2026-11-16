// Samples (replay_internal.h has the file map): at a sample frame, every focus twin and its eye reference copied to
// readback, then off the game's threads saved as pictures in logs\replay and compared through the focus remap.
#include "replay_internal.h"
#include "replay_shaders.h"  // kSlotBytes

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "../log.h"
#include "d3d11_capture.h"  // write_png_bgra

namespace simqvfr::replay_detail {
namespace {

// R11G11B10_FLOAT: 6-bit-mantissa 11-bit floats (red, green) and a 5-bit-mantissa 10-bit float (blue), 5-bit exponents.
float unpack_small_float(uint32_t bits, int mantissaBits) {
    const uint32_t mantissa = bits & ((1u << mantissaBits) - 1), exponent = bits >> mantissaBits;
    if (exponent == 0)
        return std::ldexp(static_cast<float>(mantissa), -14 - mantissaBits);
    if (exponent == 31)
        return 65504.0f;
    return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissaBits),
                      static_cast<int>(exponent) - 15);
}
float half_to_float(uint16_t h) {
    const uint32_t sign = h >> 15, exponent = (h >> 10) & 31, mantissa = h & 1023;
    const float v = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                    : exponent == 31 ? 65504.0f
                                     : std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
    return sign ? -v : v;
}

bool colour_supported(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM;
}
bool depth_supported(DXGI_FORMAT f) { return f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_FLOAT; }

// A readback as floats: 1 per pixel (depth) or 3 (colour, as stored).
std::vector<float> read_back(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f, DXGI_FORMAT format,
                             bool depth) {
    const UINT w = f.Footprint.Width, h = f.Footprint.Height;
    std::vector<float> out(size_t{w} * h * (depth ? 1 : 3));
    void* p = nullptr;
    if (FAILED(readback->Map(0, nullptr, &p)))
        return {};
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(p) + f.Offset + size_t{y} * f.Footprint.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            const size_t i = size_t{y} * w + x;
            if (depth) {
                std::memcpy(&out[i], row + size_t{x} * 4, 4);
            } else if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
                uint32_t v;
                std::memcpy(&v, row + size_t{x} * 4, 4);
                out[i * 3 + 0] = unpack_small_float(v & 0x7ff, 6);
                out[i * 3 + 1] = unpack_small_float((v >> 11) & 0x7ff, 6);
                out[i * 3 + 2] = unpack_small_float((v >> 22) & 0x3ff, 5);
            } else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                uint16_t v[4];
                std::memcpy(v, row + size_t{x} * 8, 8);
                for (int c = 0; c < 3; ++c) out[i * 3 + c] = half_to_float(v[c]);
            } else {
                const uint8_t* v = row + size_t{x} * 4;
                const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
                for (int c = 0; c < 3; ++c) out[i * 3 + c] = v[bgra ? 2 - c : c] / 255.0f;
            }
        }
    }
    readback->Unmap(0, nullptr);
    return out;
}

void save_picture(const std::wstring& path, const std::vector<float>& v, UINT w, UINT h, bool depth, float scale) {
    std::vector<uint8_t> bgra(size_t{w} * h * 4);
    for (size_t i = 0; i < size_t{w} * h; ++i) {
        uint8_t rgb[3];
        if (depth) {
            const float d = v[i];
            rgb[0] = rgb[1] = rgb[2] =
                static_cast<uint8_t>(std::lround((d <= 0 ? 0.0f : std::sqrt(std::min(1.0f, d * scale))) * 255));
        } else {
            for (int c = 0; c < 3; ++c) {
                if (scale < 0) {  // already display values (8-bit, tonemapped): written as they are
                    rgb[c] = static_cast<uint8_t>(std::lround(std::clamp(v[i * 3 + c], 0.0f, 1.0f) * 255));
                    continue;
                }
                const float x = std::max(0.0f, v[i * 3 + c] * scale);
                rgb[c] = static_cast<uint8_t>(std::lround(std::pow(x / (1 + x), 1 / 2.2f) * 255));
            }
        }
        bgra[i * 4 + 0] = rgb[2];
        bgra[i * 4 + 1] = rgb[1];
        bgra[i * 4 + 2] = rgb[0];
        bgra[i * 4 + 3] = 255;
    }
    write_png_bgra(path, bgra, w, h);
}

struct SampleTarget {
    std::string name;
    bool depth = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> focusRead, eyeRead;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT focusFoot{}, eyeFoot{};
    UINT fw = 0, fh = 0, ew = 0, eh = 0;  // per eye
};
struct SampleJob {
    uint64_t fenceValue = 0, frame = 0;
    double box = 0;
    bool shadowOn = true;  // R2c shadow A/B: whether this sample frame's focus lighting used our deferred shadow
    uint32_t phase = 0;    // replay_diag: the sample's index (0-based)
    ComPtr<ID3D12Resource> constantsRead;
    std::vector<SampleTarget> targets;
    std::vector<ComPtr<IUnknown>> keep;  // the copy list and twins until the copy is done
    struct Extra {  // whole textures saved as pictures only (the atmosphere sky maps: ours vs the game's)
        std::string name;
        ComPtr<ID3D12Resource> read;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    std::vector<Extra> extras;
};

// The focus pixel (u, v) of eye `eye`, as a fractional pixel position in the eye reference.
struct Mapper {
    double cx[2], cy[2], box;
    UINT fw, fh, ew, eh;
    void eye_pixel(int eye, double u, double v, double& x, double& y) const {
        const double nx = (u + 0.5) / fw * 2 - 1, ny = 1 - (v + 0.5) / fh * 2;
        x = (cx[eye] + nx * box + 1) * 0.5 * ew - 0.5;
        y = (1 - (cy[eye] + ny * box)) * 0.5 * eh - 0.5;
    }
};

// Bilinear in the eye reference, `channels` floats per pixel; false at the image edge or across sky / no sky.
bool bilinear(const std::vector<float>& img, UINT stride, int eye, UINT ew, UINT eh, double x, double y, int channels,
              const std::vector<float>* depthForSky, float* out) {
    const long px = static_cast<long>(std::floor(x)), py = static_cast<long>(std::floor(y));
    if (px < 1 || py < 1 || px + 2 >= static_cast<long>(ew) || py + 2 >= static_cast<long>(eh))
        return false;
    const auto idx = [&](long xx, long yy) {
        return static_cast<size_t>(yy) * stride + static_cast<size_t>(eye) * ew + static_cast<size_t>(xx);
    };
    if (depthForSky) {
        const auto& d = *depthForSky;
        const bool s00 = d[idx(px, py)] <= 0, s10 = d[idx(px + 1, py)] <= 0, s01 = d[idx(px, py + 1)] <= 0,
                   s11 = d[idx(px + 1, py + 1)] <= 0;
        if (s00 != s11 || s10 != s01 || s00 != s10)
            return false;  // an edge against the sky: no bilinear value
    }
    const double tx = x - px, ty = y - py;
    for (int c = 0; c < channels; ++c) {
        const float a = img[idx(px, py) * channels + c], b = img[idx(px + 1, py) * channels + c];
        const float d0 = img[idx(px, py + 1) * channels + c], d1 = img[idx(px + 1, py + 1) * channels + c];
        out[c] = static_cast<float>((a * (1 - tx) + b * tx) * (1 - ty) + (d0 * (1 - tx) + d1 * tx) * ty);
    }
    return true;
}

// Off the game's threads: waits for the copy, saves every twin pair and compares them through the remap.
void focus_save(std::shared_ptr<SampleJob> job) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    focus.fence->SetEventOnCompletion(job->fenceValue, event);
    WaitForSingleObject(event, 10000);
    CloseHandle(event);
    float rows[16] = {};
    void* p = nullptr;
    if (job->constantsRead && SUCCEEDED(job->constantsRead->Map(0, nullptr, &p))) {
        std::memcpy(rows, static_cast<const uint8_t*>(p) + 128 * 16, sizeof(rows));
        job->constantsRead->Unmap(0, nullptr);
    }
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"replay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ReplayFocusCompare result;
    std::vector<float> eyeDepth;
    UINT eyeDepthW = 0;
    double bestShare = -1;
    double cx[2] = {-rows[2], -rows[10]}, cy[2] = {-rows[6], -rows[14]};
    for (const SampleTarget& t : job->targets) {
        const Mapper m{{cx[0], cx[1]}, {cy[0], cy[1]}, job->box, t.fw, t.fh, t.ew, t.eh};
        const std::vector<float> f = read_back(t.focusRead.Get(), t.focusFoot, t.format, t.depth);
        const std::vector<float> e = read_back(t.eyeRead.Get(), t.eyeFoot, t.format, t.depth);
        if (f.empty() || e.empty())
            continue;
        // Picture scale: depth by the reference's 99th percentile; colour by its median (to ~0.18).
        std::vector<float> sorted;
        for (size_t i = 0; i < e.size(); i += t.depth ? 7 : 21)
            if (e[i] > 0) sorted.push_back(e[i]);
        std::sort(sorted.begin(), sorted.end());
        float scale = 1;
        if (!sorted.empty())
            scale = t.depth ? 1.0f / std::max(1e-6f, sorted[sorted.size() * 99 / 100])
                            : 0.18f / std::max(1e-4f, sorted[sorted.size() / 2]);
        if (t.name.rfind("post", 0) == 0)
            scale = -1;  // display values: raw
        wchar_t name[160];
        std::swprintf(name, 160, L"frame-%llu-%hs-focus.png", static_cast<unsigned long long>(job->frame), t.name.c_str());
        save_picture((dir / name).wstring(), f, t.fw * 2, t.fh, t.depth, scale);
        std::swprintf(name, 160, L"frame-%llu-%hs-eye.png", static_cast<unsigned long long>(job->frame), t.name.c_str());
        save_picture((dir / name).wstring(), e, t.ew * 2, t.eh, t.depth, scale);
        const UINT fStride = t.fw * 2, eStride = t.ew * 2;
        if (t.depth && bestShare < 0) {  // the largest depth twin (sorted first)
            eyeDepth = e;
            eyeDepthW = t.ew;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    uint64_t compared = 0, agree = 0;
                    for (int eye = 0; eye < 2; ++eye)
                        for (UINT v = 2; v + 2 < t.fh; v += 2)
                            for (UINT u = 2; u + 2 < t.fw; u += 2) {
                                double x, y;
                                m.eye_pixel(eye, u, v, x, y);
                                float b;
                                const float a = f[size_t{v} * fStride + static_cast<size_t>(eye) * t.fw + u];
                                if (!bilinear(e, eStride, eye, t.ew, t.eh, x + dx, y + dy, 1, &e, &b))
                                    continue;
                                if (a <= 0 && b <= 0)
                                    continue;  // sky in both
                                ++compared;
                                if (std::fabs(a - b) <= 0.01f * std::max(a, b))
                                    ++agree;
                            }
                    const double share = compared ? static_cast<double>(agree) / static_cast<double>(compared) : 0;
                    if (share > bestShare) {
                        bestShare = share;
                        result.pixels = compared;
                        result.agree = share;
                        result.shiftX = dx;
                        result.shiftY = dy;
                    }
                }
        } else if (!t.depth && (result.colourPixels == 0 || t.name.rfind("post", 0) == 0)) {  // the largest colour twin; P1 post
            const ReplayFocusCompare kept = result;  // the post comparison logs only
            const bool skyMask = eyeDepthW == t.ew && !eyeDepth.empty();
            uint64_t compared = 0, agree = 0;
            for (int eye = 0; eye < 2; ++eye)
                for (UINT v = 2; v + 2 < t.fh; v += 2)
                    for (UINT u = 2; u + 2 < t.fw; u += 2) {
                        double x, y;
                        m.eye_pixel(eye, u, v, x, y);
                        float b[3];
                        if (!bilinear(e, eStride, eye, t.ew, t.eh, x + result.shiftX, y + result.shiftY, 3,
                                      skyMask ? &eyeDepth : nullptr, b))
                            continue;
                        const float* a = &f[(size_t{v} * fStride + static_cast<size_t>(eye) * t.fw + u) * 3];
                        ++compared;
                        bool same = true;
                        for (int c = 0; c < 3; ++c)
                            same = same && std::fabs(a[c] - b[c]) <= 0.03f * std::max(std::fabs(a[c]), std::fabs(b[c])) + 0.003f;
                        if (same)
                            ++agree;
                    }
            result.colourPixels = compared;
            result.colourAgree = compared ? static_cast<double>(agree) / static_cast<double>(compared) : 0;
            // At the eye's pixel size: k x k focus pixels (one eye pixel's footprint) averaged, compared with the eye
            // at the block's centre.
            const UINT k = std::max(1u, static_cast<UINT>(std::lround(t.fw / std::max(1.0, t.ew * job->box))));
            uint64_t eyeCompared = 0, eyeAgree = 0, hoodAgree = 0;
            for (int eye = 0; eye < 2; ++eye)
                for (UINT v0 = k; v0 + 2 * k <= t.fh; v0 += k)
                    for (UINT u0 = k; u0 + 2 * k <= t.fw; u0 += k) {
                        double x, y;
                        m.eye_pixel(eye, u0 + (k - 1) * 0.5, v0 + (k - 1) * 0.5, x, y);
                        float b[3];
                        if (!bilinear(e, eStride, eye, t.ew, t.eh, x + result.shiftX, y + result.shiftY, 3,
                                      skyMask ? &eyeDepth : nullptr, b))
                            continue;
                        float a[3] = {};
                        for (UINT v = v0; v < v0 + k; ++v)
                            for (UINT u = u0; u < u0 + k; ++u)
                                for (int c = 0; c < 3; ++c)
                                    a[c] += f[(size_t{v} * fStride + static_cast<size_t>(eye) * t.fw + u) * 3 + c];
                        ++eyeCompared;
                        bool same = true;
                        for (int c = 0; c < 3; ++c) {
                            a[c] /= static_cast<float>(k * k);
                            same = same && std::fabs(a[c] - b[c]) <= 0.03f * std::max(std::fabs(a[c]), std::fabs(b[c])) + 0.003f;
                        }
                        if (same)
                            ++eyeAgree;
                        // Within the eye's 3x3 neighbourhood around the mapped position (bilinear() checked the edges).
                        const long px = static_cast<long>(std::floor(x + result.shiftX + 0.5));
                        const long py = static_cast<long>(std::floor(y + result.shiftY + 0.5));
                        bool inHood = true;
                        for (int c = 0; c < 3 && inHood; ++c) {
                            float lo = 1e30f, hi = -1e30f;
                            for (long dy = -1; dy <= 1; ++dy)
                                for (long dx = -1; dx <= 1; ++dx) {
                                    const long qx = std::clamp(px + dx, 0L, static_cast<long>(t.ew) - 1);
                                    const long qy = std::clamp(py + dy, 0L, static_cast<long>(t.eh) - 1);
                                    const float q = e[(static_cast<size_t>(qy) * eStride + static_cast<size_t>(eye) * t.ew +
                                                       static_cast<size_t>(qx)) * 3 + c];
                                    lo = std::min(lo, q);
                                    hi = std::max(hi, q);
                                }
                            const float tol = 0.03f * std::max(std::fabs(lo), std::fabs(hi)) + 0.003f;
                            inHood = a[c] >= lo - tol && a[c] <= hi + tol;
                        }
                        if (inHood)
                            ++hoodAgree;
                    }
            if (focus.settings.diag) {
                static const char* const kPhase[4] = {"as set", "our fog OFF", "our 3D atmosphere ON",
                                                      "our 3D atmosphere + sky maps ON"};
                logd("replay focus: sample frame %llu: diagnostic phase: %s", static_cast<unsigned long long>(job->frame),
                     kPhase[std::min<uint32_t>(3, job->phase)]);
            }
            if (focus.settings.atmosphere == 2)
                logd("replay focus: sample frame %llu: atmosphere A/B: the focus views %s our atmosphere maps",
                     static_cast<unsigned long long>(job->frame), job->shadowOn ? "used" : "did NOT use");
            if (focus.settings.transparents == 2)
                logd("replay focus: sample frame %llu: transparents A/B: %s",
                     static_cast<unsigned long long>(job->frame), job->shadowOn ? "replayed" : "NOT replayed");
            if (focus.settings.shadows == 2)
                logd("replay focus: sample frame %llu: shadow A/B: the focus lighting %s our deferred shadow",
                     static_cast<unsigned long long>(job->frame), job->shadowOn ? "used" : "did NOT use");
            result.colourEyePixels = eyeCompared;
            result.colourEyeAgree = eyeCompared ? static_cast<double>(eyeAgree) / static_cast<double>(eyeCompared) : 0;
            result.colourHoodAgree = eyeCompared ? static_cast<double>(hoodAgree) / static_cast<double>(eyeCompared) : 0;
            logd("replay focus: sample frame %llu: colour %s at the eye's pixel size (%ux%u focus pixels each): %llu "
                 "compared, %.1f%% agree; %.1f%% within the eye's 3x3 neighbourhood (edge-tolerant)",
                 static_cast<unsigned long long>(job->frame), t.name.c_str(), k, k,
                 static_cast<unsigned long long>(eyeCompared), result.colourEyeAgree * 100, result.colourHoodAgree * 100);
            logd("replay focus: sample frame %llu: colour %s: %llu pixels compared, %.1f%% agree (every channel within 3%%)",
                 static_cast<unsigned long long>(job->frame), t.name.c_str(), static_cast<unsigned long long>(compared),
                 result.colourAgree * 100);
            if (t.name.rfind("post", 0) == 0)
                result = kept;  // the scene colour's result stays the one reported
        }
    }
    for (const SampleJob::Extra& x : job->extras) {  // pictures only, each at its own median exposure
        const std::vector<float> v = read_back(x.read.Get(), x.foot, x.format, false);
        if (v.empty())
            continue;
        std::vector<float> sorted;
        for (size_t i = 0; i < v.size(); i += 7)
            if (v[i] > 0) sorted.push_back(v[i]);
        std::sort(sorted.begin(), sorted.end());
        const float scale = sorted.empty() ? 1.0f : 0.18f / std::max(1e-6f, sorted[sorted.size() / 2]);
        double mean[3] = {};
        for (size_t i = 0; i < v.size() / 3; ++i)
            for (int c = 0; c < 3; ++c) mean[c] += v[i * 3 + c];
        const double n = static_cast<double>(std::max<size_t>(1, v.size() / 3));
        double alpha = -1;
        if (x.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {  // the alpha (cloud coverage / transmittance)
            void* m = nullptr;
            if (SUCCEEDED(x.read->Map(0, nullptr, &m))) {
                double sum = 0;
                for (UINT y = 0; y < x.foot.Footprint.Height; ++y)
                    for (UINT xx = 0; xx < x.foot.Footprint.Width; ++xx) {
                        uint16_t h;
                        std::memcpy(&h, static_cast<const uint8_t*>(m) + x.foot.Offset + size_t{y} * x.foot.Footprint.RowPitch +
                                            size_t{xx} * 8 + 6,
                                    2);
                        sum += half_to_float(h);
                    }
                x.read->Unmap(0, nullptr);
                alpha = sum / (static_cast<double>(x.foot.Footprint.Width) * x.foot.Footprint.Height);
            }
        }
        if (alpha >= 0)
            logd("replay focus: sample frame %llu: %s alpha mean %.4f", static_cast<unsigned long long>(job->frame),
                 x.name.c_str(), alpha);
        logd("replay focus: sample frame %llu: %s %ux%u mean (%.4f, %.4f, %.4f), median %.4f",
             static_cast<unsigned long long>(job->frame), x.name.c_str(), x.foot.Footprint.Width, x.foot.Footprint.Height,
             mean[0] / n, mean[1] / n, mean[2] / n, sorted.empty() ? 0.0f : sorted[sorted.size() / 2]);
        wchar_t name[160];
        std::swprintf(name, 160, L"frame-%llu-%hs.png", static_cast<unsigned long long>(job->frame), x.name.c_str());
        save_picture((dir / name).wstring(), v, x.foot.Footprint.Width, x.foot.Footprint.Height, false, scale);
    }
    AcquireSRWLockExclusive(&focus.compareLock);
    result.sample = focus.compare.sample + 1;
    focus.compare = result;
    ReleaseSRWLockExclusive(&focus.compareLock);
    logd("replay focus: sample frame %llu: depth %llu pixels compared, %.1f%% agree within 1%% at the best shift (%d, %d) "
         "eye pixels; eye centres (%.4f, %.4f) and (%.4f, %.4f); images in logs\\replay",
         static_cast<unsigned long long>(job->frame), static_cast<unsigned long long>(result.pixels), result.agree * 100,
         result.shiftX, result.shiftY, cx[0], cy[0], cx[1], cy[1]);
    focus.saving = false;
}

}  // namespace

// Under focus.lock, on the submit thread before the next frame's clear: every twin of a readable format and the
// sample's constants copied to readback, then saved off-thread.
void focus_copy_sample() {
    focus.copyPending = false;
    auto job = std::make_shared<SampleJob>();
    job->frame = focus.copyFrame.load();
    job->box = focus.settings.boxFraction;
    job->shadowOn = (focus.samplesTaken.load() + 1) % 2 == 0;  // counted at the sample frame's end: k + 1 here
    job->phase = focus.samplesTaken.load() - 1;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List> l;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&l))))
        return;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    const auto buffer = [&](UINT64 bytes) {
        ComPtr<ID3D12Resource> out;
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&out));
        return out;
    };
    inReplay = true;
    // Depth twins first (the colour comparison skips sky edges by the depth).
    std::vector<const Twin*> order;
    for (const auto& [game, t] : focus.twins)
        if (t.gameDesc.SampleDesc.Count == 1 &&
            (t.depth ? depth_supported(t.gameDesc.Format) : colour_supported(t.viewFormat)))
            order.push_back(&t);
    std::stable_sort(order.begin(), order.end(), [](const Twin* a, const Twin* b) {
        // depth first, then the largest, then ACE's scene targets by name (same-size flagsDepth, oit*, velocity)
        if (a->depth != b->depth)
            return a->depth;
        if (a->eyeW != b->eyeW)
            return a->eyeW > b->eyeW;
        const auto scene = [](const Twin* t) { return t->name == "msaaDepth" || t->name == "msaaColor"; };
        return scene(a) && !scene(b);
    });
    for (const Twin* t : order) {
        SampleTarget s;
        s.name = t->name;
        s.depth = t->depth;
        s.format = t->depth ? DXGI_FORMAT_R32_FLOAT : t->viewFormat;
        s.fw = t->focusW;
        s.fh = t->focusH;
        s.ew = t->eyeW;
        s.eh = t->eyeH;
        const D3D12_RESOURCE_DESC fd = t->focus->GetDesc(), ed = t->eye->GetDesc();
        UINT64 fb = 0, eb = 0;
        device->GetCopyableFootprints(&fd, 0, 1, 0, &s.focusFoot, nullptr, nullptr, &fb);
        device->GetCopyableFootprints(&ed, 0, 1, 0, &s.eyeFoot, nullptr, nullptr, &eb);
        s.focusRead = buffer(fb);
        s.eyeRead = buffer(eb);
        if (!s.focusRead || !s.eyeRead)
            continue;
        const D3D12_RESOURCE_STATES state = t->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        const std::tuple<ID3D12Resource*, ID3D12Resource*, D3D12_PLACED_SUBRESOURCE_FOOTPRINT> pairs[2] = {
            {t->focus.Get(), s.focusRead.Get(), s.focusFoot}, {t->eye.Get(), s.eyeRead.Get(), s.eyeFoot}};
        for (const auto& [twin, read, foot] : pairs) {
            barrier(l.Get(), twin, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{read, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = foot;
            D3D12_TEXTURE_COPY_LOCATION src{twin, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), twin, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            job->keep.emplace_back(twin);
        }
        job->targets.push_back(std::move(s));
    }
    if (focus.skyCloudViews && focus.skyClouds) {  // our focus clouds (the sky patch), as a picture with its alpha logged
        SampleJob::Extra x;
        x.name = "clouds-ours";
        const D3D12_RESOURCE_DESC d = focus.skyClouds->GetDesc();
        x.format = d.Format;
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
        x.read = buffer(bytes);
        if (x.read) {
            barrier(l.Get(), focus.skyClouds.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = x.foot;
            D3D12_TEXTURE_COPY_LOCATION src{focus.skyClouds.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), focus.skyClouds.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            job->keep.emplace_back(focus.skyClouds.Get());
            job->extras.push_back(std::move(x));
        }
    }
    if (focus.atmoMade) {  // the sky maps of eye 0 (ours resampled, the game's): pictures for the sky investigation
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        for (int m : {2, 3}) {
            for (int which = 0; which < 2; ++which) {
                ID3D12Resource* res = which ? focus.atmoGame[m] : focus.atmo[m].Get();
                if (!res)
                    continue;
                SampleJob::Extra x;
                x.name = std::string(m == 2 ? "skyIn" : "skyOut") + (which ? "-game" : "-ours");
                const D3D12_RESOURCE_DESC d = res->GetDesc();
                x.format = d.Format;
                UINT64 bytes = 0;
                device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
                x.read = buffer(bytes);
                if (!x.read)
                    continue;
                const D3D12_RESOURCE_STATES state = which ? read : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint = x.foot;
                D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                src.SubresourceIndex = 0;
                l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
                job->keep.emplace_back(res);
                job->extras.push_back(std::move(x));
            }
        }
    }
    {  // MSAA: our refraction source's mip 0 (the opaque scene, resolved at the blur) and our resolved colour (the post's)
        const std::tuple<ID3D12Resource*, const char*, D3D12_RESOURCE_STATES> extra[2] = {
            {focus.blur.Get(), "blur0-ours", D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
            {focus.resolvedColour.Get(), "colour-resolved", D3D12_RESOURCE_STATE_RENDER_TARGET}};
        for (const auto& [res, nm, state] : extra) {
            if (!res || !focus.resolvedColour)
                continue;
            SampleJob::Extra x;
            x.name = nm;
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            x.format = d.Format;
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
            x.read = buffer(bytes);
            if (!x.read)
                continue;
            barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = x.foot;
            D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            job->keep.emplace_back(res);
            job->extras.push_back(std::move(x));
        }
    }
    if (focus.dlssReady && focus.dlssOut && focus.packAa) {  // P2a: our DLSS image and packed motion vectors
        for (int which = 0; which < 2; ++which) {
            ID3D12Resource* res = which ? focus.packAa.Get() : focus.dlssOut.Get();
            if (which == 1 && focus.ngxAuto[0])
                res = focus.dlssOutAuto.Get();
            else if (which == 1 && focus.casDone)
                res = focus.casOut.Get();
            const D3D12_RESOURCE_STATES state =
                which && !focus.ngxAuto[0] && !focus.casDone ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                             : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            SampleJob::Extra x;
            x.name = which ? (focus.ngxAuto[0] ? "dlss-auto" : focus.casDone ? "dlss-cas" : "motion-ours") : "dlss-ours";
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            x.format = d.Format;
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&d, 0, 1, 0, &x.foot, nullptr, nullptr, &bytes);
            x.read = buffer(bytes);
            if (!x.read)
                continue;
            barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{x.read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = x.foot;
            D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            job->keep.emplace_back(res);
            job->extras.push_back(std::move(x));
        }
    }
    if (focus.postReady && focus.postLdr && focus.postGameFxaa) {  // P1: our tonemapped focus vs the game's eyes
        const Twin* colourTwin = nullptr;
        for (const auto& [game, t] : focus.twins)
            if (!t.depth && t.name == "msaaColor" && t.gameDesc.SampleDesc.Count == 1)
                colourTwin = &t;
        if (!colourTwin && focus.resolvedColour)
            colourTwin = &focus.resolvedColourTwin;
        const std::pair<ID3D12Resource*, const char*> posts[3] = {
            {focus.postLdr.Get(), "post"}, {focus.postAlt[0].Get(), "post-nodlss"},
            {focus.postAlt[1].Get(), focus.ngxAuto[0] ? "post-auto" : "post-nocas"}};
        for (const auto& [postRes, postName] : posts) {
        if (colourTwin && postRes) {
            SampleTarget s;
            s.name = postName;
            s.format = focus.postGameFxaaFormat;
            s.fw = focus.postW / 2;
            s.fh = focus.postH;
            s.ew = colourTwin->eyeW;
            s.eh = colourTwin->eyeH;
            const D3D12_RESOURCE_DESC fd = postRes->GetDesc(), ed = focus.postGameFxaa->GetDesc();
            UINT64 fb = 0, eb = 0;
            device->GetCopyableFootprints(&fd, 0, 1, 0, &s.focusFoot, nullptr, nullptr, &fb);
            device->GetCopyableFootprints(&ed, 0, 1, 0, &s.eyeFoot, nullptr, nullptr, &eb);
            s.focusRead = buffer(fb);
            s.eyeRead = buffer(eb);
            if (s.focusRead && s.eyeRead && ed.Width == UINT64{2} * s.ew && ed.Height == s.eh) {
                // ours rests as a shader resource, the game's fxaa_texture as a render target (end of its post list)
                const std::tuple<ID3D12Resource*, ID3D12Resource*, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_RESOURCE_STATES>
                    pairs[2] = {{postRes, s.focusRead.Get(), s.focusFoot, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
                                {focus.postGameFxaa.Get(), s.eyeRead.Get(), s.eyeFoot, D3D12_RESOURCE_STATE_RENDER_TARGET}};
                for (const auto& [res, read, foot, state] : pairs) {
                    barrier(l.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION dst{read, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                    dst.PlacedFootprint = foot;
                    D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                    src.SubresourceIndex = 0;
                    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    barrier(l.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
                    job->keep.emplace_back(res);
                }
                job->targets.push_back(std::move(s));
            }
        }
        }
    }
    const uint32_t slot = focus.sampleSlot.load();
    if (slot != UINT32_MAX) {
        job->constantsRead = buffer(kSlotBytes);
        barrier(l.Get(), focus.constants.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(job->constantsRead.Get(), 0, focus.constants.Get(), UINT64{slot} * kSlotBytes, kSlotBytes);
        barrier(l.Get(), focus.constants.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    l->Close();
    inReplay = false;
    if (job->targets.empty()) {
        logd("replay focus: no twin of a format the sample can read (multisampled, or not depth / colour)");
        return;
    }
    ID3D12CommandList* lists[] = {l.Get()};
    executeOriginal(focus.queue.Get(), 1, lists);
    job->fenceValue = focus.fenceValue.fetch_add(1) + 1;
    focus.queue->Signal(focus.fence.Get(), job->fenceValue);
    job->keep.emplace_back(l.Get());
    job->keep.emplace_back(allocator.Get());
    focus.saving = true;
    std::thread(focus_save, job).detach();
}

}  // namespace simqvfr::replay_detail

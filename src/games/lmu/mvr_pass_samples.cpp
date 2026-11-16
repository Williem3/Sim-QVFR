// Sample images of LMU's multi-view path (mvr_pass_internal.h): focus regions copied and read back, compared
// (gate C, mvr_compare), tonemapped for looking at; the car slots' gate image.
#include "mvr_pass_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include "../../log.h"
#include "../../probe/d3d11_capture.h"

namespace lmu::mvr_detail {

// `dst` (made or re-made at w x h in `src`'s format) = src's region at (x, y).
bool copy_region(ComPtr<ID3D11Texture2D>& dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w, unsigned h) {
    ComPtr<ID3D11Texture2D> source;
    if (!src || FAILED(src->QueryInterface(IID_PPV_ARGS(&source))) || !w || !h)
        return false;
    D3D11_TEXTURE2D_DESC d;
    source->GetDesc(&d);
    if (x + w > d.Width || y + h > d.Height || d.SampleDesc.Count != 1)
        return false;
    if (dst) {
        D3D11_TEXTURE2D_DESC have;
        dst->GetDesc(&have);
        if (have.Width != w || have.Height != h || have.Format != d.Format)
            dst.Reset();
    }
    if (!dst) {
        D3D11_TEXTURE2D_DESC c = d;
        c.Width = w;
        c.Height = h;
        c.MipLevels = 1;
        c.ArraySize = 1;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = 0;
        c.CPUAccessFlags = 0;
        c.MiscFlags = 0;
        ComPtr<ID3D11Device> device;
        g.context->GetDevice(&device);
        if (FAILED(device->CreateTexture2D(&c, nullptr, &dst)))
            return false;
    }
    const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    g.context->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
    return true;
}

bool read_halves(ID3D11Texture2D* texture, std::vector<uint16_t>& out, unsigned& w, unsigned& h) {
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS)
        return false;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&s, nullptr, &staging)))
        return false;
    g.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    w = d.Width;
    h = d.Height;
    out.resize(static_cast<size_t>(w) * h * 4);
    for (unsigned y = 0; y < h; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, static_cast<const uint8_t*>(map.pData) + y * map.RowPitch,
                    static_cast<size_t>(w) * 8);
    g.context->Unmap(staging.Get(), 0);
    return true;
}

namespace {

float half_to_float(uint16_t v) {
    const uint32_t sign = (v >> 15) & 1, exponent = (v >> 10) & 0x1f, mantissa = v & 0x3ff;
    float f;
    if (exponent == 0)
        f = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        f = mantissa ? 0.0f : 65504.0f;  // NaN as 0, infinity as the largest half
    else
        f = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -f : f;
}

std::vector<float> luminance(const std::vector<uint16_t>& px, unsigned w, unsigned h) {
    std::vector<float> l(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < l.size(); ++i)
        l[i] = std::max(0.0f, 0.2126f * half_to_float(px[i * 4]) + 0.7152f * half_to_float(px[i * 4 + 1]) +
                                  0.0722f * half_to_float(px[i * 4 + 2]));
    return l;
}

}  // namespace

// For looking at: x / (1 + x), gamma 2.2, BGRA.
std::vector<uint8_t> tonemapped(const std::vector<uint16_t>& px, unsigned w, unsigned h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = std::max(0.0f, half_to_float(px[i * 4 + c]));
            out[i * 4 + 2 - c] = static_cast<uint8_t>(std::lround(255.0f * std::pow(v / (1.0f + v), 1.0f / 2.2f)));
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

// Car slots' gate image: the pass's views (the eye, its focus view beside it) as drawn, each car part's pixels in a colour
// of its slot over the scene darkened; the share of the views' pixels that are car parts and how many parts, logged.
void save_slot_image(int eye, ID3D11Resource* scene) {
    const UINT fw = g.drawFocus ? (g.view.focusRenderW ? g.view.focusRenderW : g.view.focusW) : 0;
    const UINT fh = g.drawFocus ? (g.view.focusRenderH ? g.view.focusRenderH : g.view.focusH) : 0;
    const UINT w = std::min(g.view.eyeW + fw, g.slotW), h = std::min(std::max(g.view.eyeH, fh), g.slotH);
    if (!w || !h || !scene || !g.slotTex || g.slotSamples > 1 || tr.sceneDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return;  // (the gate image is of a single-sample slot target: MSAA off)
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    const auto staged = [&](ID3D11Resource* src, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& out) {
        const D3D11_TEXTURE2D_DESC t = {w, h, 1, 1, format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        if (FAILED(device->CreateTexture2D(&t, nullptr, &out)))
            return false;
        const D3D11_BOX box = {0, 0, 0, w, h, 1};
        g.context->CopySubresourceRegion(out.Get(), 0, 0, 0, 0, src, 0, &box);
        return true;
    };
    ComPtr<ID3D11Texture2D> slots, colour;
    if (!staged(g.slotTex.Get(), DXGI_FORMAT_R16_UINT, slots) || !staged(scene, DXGI_FORMAT_R16G16B16A16_FLOAT, colour))
        return;
    D3D11_MAPPED_SUBRESOURCE ms{}, mc{};
    if (FAILED(g.context->Map(slots.Get(), 0, D3D11_MAP_READ, 0, &ms)))
        return;
    if (FAILED(g.context->Map(colour.Get(), 0, D3D11_MAP_READ, 0, &mc))) {
        g.context->Unmap(slots.Get(), 0);
        return;
    }
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    std::vector<uint8_t> seen(kCarSlots + 1, 0);
    size_t carPixels = 0, viewPixels = 0, parts = 0;
    for (UINT y = 0; y < h; ++y) {
        const auto* sr = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(ms.pData) + y * ms.RowPitch);
        const auto* cr = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mc.pData) + y * mc.RowPitch);
        for (UINT x = 0; x < w; ++x) {
            const bool inView = (x < g.view.eyeW && y < g.view.eyeH) || (x >= g.view.eyeW && y < fh);
            uint8_t* o = bgra.data() + (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const float v = std::max(0.0f, half_to_float(cr[x * 4 + c]));
                o[2 - c] = static_cast<uint8_t>(std::lround(110.0f * std::pow(v / (1.0f + v), 1.0f / 2.2f)));
            }
            o[3] = 255;
            if (!inView)
                continue;
            ++viewPixels;
            const uint16_t word = sr[x];
            if (word == 0 || word > kCarSlots)
                continue;
            ++carPixels;
            if (!seen[word]) {
                seen[word] = 1;
                ++parts;
            }
            const uint32_t hash = word * 2654435761u;  // a colour per slot
            o[0] = static_cast<uint8_t>(96 + (hash >> 8) % 160);
            o[1] = static_cast<uint8_t>(96 + (hash >> 16) % 160);
            o[2] = static_cast<uint8_t>(96 + (hash >> 24) % 160);
        }
    }
    g.context->Unmap(colour.Get(), 0);
    g.context->Unmap(slots.Get(), 0);
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wchar_t name[96];
    swprintf_s(name, L"lmu-car-slots-frame%llu-eye%d.png", g.framesSinceInstall, eye);
    simqvfr::write_png_bgra((dir / name).wstring(), bgra, w, h);
    ++g.stats.slotImages;
    simqvfr::logf("lmu mvr: car slots: gate image %ls (%ux%u, eye %d%s): %.2f%% of the views' pixels car parts, %zu "
                  "parts; %llu slot views bound in the pass",
                  (dir / name).c_str(), w, h, eye, fw ? " and its focus view" : "",
                  viewPixels ? 100.0 * static_cast<double>(carPixels) / static_cast<double>(viewPixels) : 0.0, parts,
                  static_cast<unsigned long long>(g.slotViewsThisPass));
}

void compare_sample(unsigned long long frame) {
    ++g.stats.samples;
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int eye = 0; eye < 2; ++eye) {
        std::vector<uint16_t> a, b;
        unsigned aw = 0, ah = 0, bw = 0, bh = 0;
        if (!g.have[eye] || !g.have[2 + eye] || !read_halves(g.mvrImage[eye].Get(), a, aw, ah) ||
            !read_halves(g.passImage[eye].Get(), b, bw, bh) || aw != bw || ah != bh) {
            simqvfr::logf("lmu mvr: frame %llu: eye %d's focus images not both copied (%d %d); not compared", frame, eye,
                          g.have[eye], g.have[2 + eye]);
            continue;
        }
        const MvrComparison c = mvr_compare(a, b, aw, ah);
        simqvfr::logf("lmu mvr: frame %llu: focus %d drawn in pass %d (MVR) vs pass %d, %ux%u: aligned at %+d,%+d px; "
                      "brightness %.4f (cell median %.4f); mean difference %.2f%%; %.2f%% of pixels differ >10%%",
                      frame, eye, eye, 2 + eye, aw, ah, c.shiftX, c.shiftY, c.brightness, c.cellMedian, 100 * c.meanDiff,
                      c.differingPct);
        if (g.stats.samples == 1) {
            wchar_t name[96];
            swprintf_s(name, L"lmu-mvr-frame%llu-focus%d-mvr.png", frame, eye);
            simqvfr::write_png_bgra((dir / name).wstring(), tonemapped(a, aw, ah), aw, ah);
            swprintf_s(name, L"lmu-mvr-frame%llu-focus%d-pass.png", frame, eye);
            simqvfr::write_png_bgra((dir / name).wstring(), tonemapped(b, bw, bh), bw, bh);
            simqvfr::logf("lmu mvr: images in %ls (lmu-mvr-frame%llu-focus*-mvr/pass.png)", dir.c_str(), frame);
        }
    }
}

}  // namespace lmu::mvr_detail

namespace lmu {

using namespace mvr_detail;

namespace {

// mvr_compare's measures on two luminance images.
MvrComparison compare_luminance(const std::vector<float>& la, const std::vector<float>& lb, unsigned w, unsigned h,
                                int range) {
    MvrComparison c;
    const int border = range + 8;
    double best = -1, bestMeanA = 0;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            double diff = 0, sumA = 0;
            for (int y = border; y < static_cast<int>(h) - border; y += 4)
                for (int x = border; x < static_cast<int>(w) - border; x += 4) {
                    const float va = la[static_cast<size_t>(y) * w + x];
                    const float vb = lb[static_cast<size_t>(y + dy) * w + (x + dx)];
                    diff += std::fabs(va - vb);
                    sumA += va;
                }
            if (best < 0 || diff < best) {
                best = diff;
                bestMeanA = sumA;
                c.shiftX = dx;
                c.shiftY = dy;
            }
        }
    c.meanDiff = bestMeanA > 0 ? best / bestMeanA : 0;
    double sumA = 0, sumB = 0;
    size_t differing = 0, counted = 0;
    constexpr int kCells = 16;
    std::vector<double> cellA(kCells * kCells, 0), cellB(kCells * kCells, 0);
    for (int y = border; y < static_cast<int>(h) - border; ++y)
        for (int x = border; x < static_cast<int>(w) - border; ++x) {
            const float va = la[static_cast<size_t>(y) * w + x];
            const float vb = lb[static_cast<size_t>(y + c.shiftY) * w + (x + c.shiftX)];
            sumA += va;
            sumB += vb;
            ++counted;
            if (std::fabs(va - vb) > 0.1f * std::max(va, vb) && std::max(va, vb) > 1e-4f)
                ++differing;
            const int cell = (y * kCells / static_cast<int>(h)) * kCells + x * kCells / static_cast<int>(w);
            cellA[cell] += va;
            cellB[cell] += vb;
        }
    c.brightness = sumA > 0 ? sumB / sumA : 0;
    std::vector<double> ratios;
    for (int i = 0; i < kCells * kCells; ++i)
        if (cellA[i] > 1e-6)
            ratios.push_back(cellB[i] / cellA[i]);
    std::sort(ratios.begin(), ratios.end());
    c.cellMedian = ratios.empty() ? 0 : ratios[ratios.size() / 2];
    c.differingPct = counted ? 100.0 * static_cast<double>(differing) / static_cast<double>(counted) : 0;
    c.ok = true;
    return c;
}

float srgb_to_linear(uint8_t v) {
    const float x = static_cast<float>(v) / 255.0f;
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

MvrComparison mvr_compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, unsigned w, unsigned h,
                          int range) {
    if (a.size() != static_cast<size_t>(w) * h * 4 || b.size() != a.size() || w < 64 || h < 64)
        return {};
    return compare_luminance(luminance(a, w, h), luminance(b, w, h), w, h, range);
}

MvrComparison mvr_compare_bgra8(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, unsigned w, unsigned h,
                                int range) {
    if (a.size() != static_cast<size_t>(w) * h * 4 || b.size() != a.size() || w < 64 || h < 64)
        return {};
    float table[256];
    for (int i = 0; i < 256; ++i)
        table[i] = srgb_to_linear(static_cast<uint8_t>(i));
    const auto lum = [&](const std::vector<uint8_t>& px) {
        std::vector<float> l(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < l.size(); ++i)
            l[i] = 0.0722f * table[px[i * 4]] + 0.7152f * table[px[i * 4 + 1]] + 0.2126f * table[px[i * 4 + 2]];
        return l;
    };
    MvrComparison c = compare_luminance(lum(a), lum(b), w, h, range);
    // The bytes themselves at that offset (colour channels; alpha left out).
    const int border = range + 8;
    double sum = 0;
    size_t counted = 0, over = 0;
    for (int y = border; y < static_cast<int>(h) - border; ++y)
        for (int x = border; x < static_cast<int>(w) - border; ++x) {
            const size_t ia = (static_cast<size_t>(y) * w + x) * 4;
            const size_t ib = (static_cast<size_t>(y + c.shiftY) * w + (x + c.shiftX)) * 4;
            int most = 0;
            for (int k = 0; k < 3; ++k) {
                const int d = std::abs(static_cast<int>(a[ia + k]) - static_cast<int>(b[ib + k]));
                most = std::max(most, d);
                sum += d;
            }
            c.maxByte = std::max(c.maxByte, most);
            over += most > 4 ? 1 : 0;
            ++counted;
        }
    c.meanByte = counted ? sum / (3.0 * static_cast<double>(counted)) : 0;
    c.bytesOverPct = counted ? 100.0 * static_cast<double>(over) / static_cast<double>(counted) : 0;
    return c;
}

}  // namespace lmu

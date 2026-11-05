#include "helmet_pieces.h"

#include <algorithm>
#include <cmath>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "../log.h"
#include "helmet.h"

namespace simqvfr {

bool decode_helmet_piece(HelmetPiece which, const std::wstring& path, std::vector<uint8_t>& pixels, uint32_t& width,
                         uint32_t& height, HelmetPieceInfo& info) {
    using Microsoft::WRL::ComPtr;
    info = HelmetPieceInfo{};
    pixels.clear();
    width = height = 0;
    // Decode with WIC as straight-alpha RGBA.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                     &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                            0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            pixels.resize(static_cast<size_t>(w) * h * 4);
            if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())))
                pixels.clear();
            width = w;
            height = h;
        }
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    if (pixels.empty()) {
        logf("compositor: helmet image %ls could not be read", path.c_str());
        return false;
    }
    // Where the piece's opaque part ends down its middle column: the top piece from its top row, the
    // bottom piece from its bottom row (for the crop hint).
    if (which != HelmetPiece::Visor) {
        const bool fromTop = which == HelmetPiece::Top;
        uint32_t rows = 0;
        while (rows < height) {
            const uint32_t y = fromTop ? rows : height - 1 - rows;
            if (pixels[(static_cast<size_t>(y) * width + width / 2) * 4 + 3] < 128)
                break;
            ++rows;
        }
        info.edge = rows ? static_cast<double>(rows) / height : -1;
    }
    info.aspect = static_cast<double>(height) / width;
    // Whether it has (almost) no transparency, and its white point: the linear brightness its brightest
    // 1% reach (for a visor drawn as a tint, that counts as clear).
    {
        size_t seeThrough = 0;
        size_t histogram[256] = {};
        for (size_t i = 0; i < pixels.size(); i += 4) {
            seeThrough += pixels[i + 3] < 250;
            ++histogram[std::max({pixels[i], pixels[i + 1], pixels[i + 2]})];
        }
        const size_t count = pixels.size() / 4;
        info.opaque = seeThrough * 200 < count;
        size_t above = 0;
        int level = 255;
        for (; level > 0; --level) {
            above += histogram[level];
            if (above * 100 >= count)
                break;
        }
        const double encoded = level / 255.0;
        const double linear = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        info.whitePoint = std::max(0.05, linear);
    }
    // The rows it draws on (any pixel more than faintly opaque) and, as glass, the rows it tints (any pixel
    // passing less than 98.5% of the scene): the helmet layer leaves the clear ones out (helmet_bands).
    {
        double linearOf[256];  // sRGB to linear, once per value
        for (int c = 0; c < 256; ++c) {
            const double e = c / 255.0;
            linearOf[c] = e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
        }
        info.rows.assign(kHelmetRows, false);
        info.tintRows.assign(kHelmetRows, false);
        for (uint32_t y = 0; y < height; ++y) {
            bool drawn = false, tints = false;
            for (uint32_t x = 0; x < width && !(drawn && tints); ++x) {
                const uint8_t* px = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                if (px[3] <= 2)
                    continue;
                drawn = true;
                const double lum = 0.2126 * linearOf[px[0]] + 0.7152 * linearOf[px[1]] + 0.0722 * linearOf[px[2]];
                tints = tints || lum / info.whitePoint < 0.985;
            }
            // Every bin the image row spans (several for an image shorter than kHelmetRows).
            const size_t first = static_cast<size_t>(y) * kHelmetRows / height,
                         last = std::max(first, static_cast<size_t>(y + 1) * kHelmetRows / height - 1);
            for (size_t bin = first; bin <= last && bin < kHelmetRows; ++bin) {
                info.rows[bin] = info.rows[bin] || drawn;
                info.tintRows[bin] = info.tintRows[bin] || tints;
            }
        }
    }
    // Premultiplied, so filtering never blends in the colour of transparent pixels (no fringes at the edges).
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            pixels[i + c] = static_cast<uint8_t>((pixels[i + c] * pixels[i + 3] + 127) / 255);
    return true;
}

bool HelmetPieceSet::load_helmet_piece(HelmetPiece which, const std::wstring& path) {
    if (!pieces_ready())
        return false;
    const int p = static_cast<int>(which);
    info_[p] = HelmetPieceInfo{};
    loaded_[p] = false;
    drop_piece(p);
    if (path.empty())
        return true;
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    HelmetPieceInfo info;
    if (!decode_helmet_piece(which, path, pixels, width, height, info) || !upload_piece(p, pixels, width, height))
        return false;
    info_[p] = std::move(info);
    loaded_[p] = true;
    logf("compositor: helmet image %ls, %ux%u%s, white point %.2f", path.c_str(), width, height,
         info_[p].opaque ? ", opaque" : "", info_[p].whitePoint);
    return true;
}

unsigned HelmetPieceSet::helmet_piece_mask() const {
    unsigned mask = 0;
    for (unsigned p = 0; p < 3; ++p)
        mask |= loaded_[p] ? 1u << p : 0u;
    return mask;
}

double HelmetPieceSet::helmet_aspect() const {
    for (int p = 0; p < 3; ++p)
        if (loaded_[p])
            return info_[p].aspect;
    return 1;
}

double HelmetPieceSet::helmet_edge(HelmetPiece piece) const {
    const int p = static_cast<int>(piece);
    return loaded_[p] ? info_[p].edge : -1;
}

bool HelmetPieceSet::visor_tints(int visorBlend) const {
    const int v = static_cast<int>(HelmetPiece::Visor);
    return loaded_[v] && (visorBlend == 1 || (visorBlend == 0 && info_[v].opaque));
}

std::vector<bool> HelmetPieceSet::helmet_rows(double visorOffset, int visorBlend) const {
    std::vector<bool> rows(kHelmetRows, false);
    for (const HelmetPiece piece : {HelmetPiece::Top, HelmetPiece::Bottom}) {
        const int p = static_cast<int>(piece);
        if (loaded_[p])
            for (int i = 0; i < kHelmetRows; ++i)
                rows[i] = rows[i] || info_[p].rows[i];
    }
    const int v = static_cast<int>(HelmetPiece::Visor);
    if (loaded_[v]) {
        const std::vector<bool>& drawn = visor_tints(visorBlend) ? info_[v].tintRows : info_[v].rows;
        for (int i = 0; i < kHelmetRows; ++i) {  // image row v shows the visor's row v + offset (none past its ends)
            const double at = (i + 0.5) / kHelmetRows + visorOffset;
            if (at >= 0 && at < 1)
                rows[i] = rows[i] || drawn[static_cast<size_t>(at * kHelmetRows)];
        }
    }
    return rows;
}

void HelmetPieceSet::clear_pieces() {
    for (int p = 0; p < 3; ++p) {
        info_[p] = HelmetPieceInfo{};
        loaded_[p] = false;
    }
}

}  // namespace simqvfr

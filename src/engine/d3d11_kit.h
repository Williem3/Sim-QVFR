// Small Direct3D 11 pieces the views' upscalers share (dlss_views.cpp, nr_views.cpp): the tagged log line, the
// once-a-session give-up, and the plain resources both make the same way.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>

#include "../log.h"

namespace simqvfr {

// A log line with a run's tag in front ("lmu dlss: ..."), cut at 1023 characters as each file's own did.
inline void log_tagged_v(const char* tag, const char* format, va_list args) {
    char text[1024];
    std::vsnprintf(text, sizeof(text), format, args);
    simqvfr::logf("%s: %s", tag, text);
}

inline void log_tagged(const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_tagged_v(tag, format, args);
    va_end(args);
}

// Off for the session: said once, the first time.
inline void give_up_once(bool& off, const char* tag, const char* why) {
    if (!off)
        log_tagged(tag, "off for this session: %s", why);
    off = true;
}

// A single-sample, one-mip 2D texture in default memory (the old one let go of first).
inline bool make_texture2d(ID3D11Device* device, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture, unsigned w, unsigned h,
                           DXGI_FORMAT format, UINT bind) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    texture.Reset();
    return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &texture));
}

// A constant buffer in default memory (written with UpdateSubresource).
inline bool make_constant_buffer(ID3D11Device* device, UINT bytes, Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &buffer));
}

// Bilinear, clamped at the edges, no mip blending.
inline bool make_linear_clamp_sampler(ID3D11Device* device, Microsoft::WRL::ComPtr<ID3D11SamplerState>& sampler) {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateSamplerState(&sd, &sampler));
}

}  // namespace simqvfr

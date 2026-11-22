#pragma once

// RF2's per-draw pieces for an eye pass's draws: the two views' viewports and scissors in the scene target, and the
// focus view's clip remap in VS b13 (the twin's constant buffer: lmu/mvr_probe.h).

#include <d3d11.h>
#include <wrl/client.h>

#include "render_backend.h"

namespace simqvfr::games::rf2 {

// The outer view's and the focus view's viewports and scissors for a draw whose own viewport is `v` (depth range kept).
struct DrawAtlas {
    D3D11_VIEWPORT viewport[2]{};
    D3D11_RECT scissor[2]{};
};
inline DrawAtlas make_draw_atlas(const D3D11_VIEWPORT& v, const AtlasLayout& layout) {
    DrawAtlas a{};
    a.viewport[0] = {0, 0, static_cast<float>(layout.outerW), static_cast<float>(layout.outerH), v.MinDepth, v.MaxDepth};
    a.viewport[1] = {static_cast<float>(layout.focus_x()), 0, static_cast<float>(layout.focusW),
                     static_cast<float>(layout.focusH), v.MinDepth, v.MaxDepth};
    a.scissor[0] = {0, 0, static_cast<LONG>(layout.outerW), static_cast<LONG>(layout.outerH)};
    a.scissor[1] = {static_cast<LONG>(layout.focus_x()), 0, static_cast<LONG>(layout.width), static_cast<LONG>(layout.focusH)};
    return a;
}

// The focus view's clip remap for the twins (VS b13), on the context's device; made again when the device changes.
class RemapBuffer {
public:
    bool upload_and_bind(ID3D11DeviceContext* c, const float remap[4]) {
        if (!c || !ensure(c))
            return false;
        c->UpdateSubresource(buffer_.Get(), 0, nullptr, remap, 0, 0);
        c->VSSetConstantBuffers(13, 1, buffer_.GetAddressOf());
        return true;
    }
    bool bind(ID3D11DeviceContext* c) {
        if (!c || !buffer_)
            return false;
        c->VSSetConstantBuffers(13, 1, buffer_.GetAddressOf());
        return true;
    }

private:
    bool ensure(ID3D11DeviceContext* c) {
        Microsoft::WRL::ComPtr<ID3D11Device> d;
        c->GetDevice(&d);
        if (!d)
            return false;
        if (buffer_ && device_.Get() == d.Get())
            return true;
        buffer_.Reset();
        device_ = d;
        const float identity[4] = {1, 0, 1, 0};
        const D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA init = {identity, 0, 0};
        return SUCCEEDED(d->CreateBuffer(&bd, &init, &buffer_));
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
};

// The game shader's twin stored on it as private data (lmu/mvr_probe.h: kMvrTwinGuid, kMvrInstancedTwinGuid).
inline Microsoft::WRL::ComPtr<ID3D11VertexShader> twin_of(ID3D11VertexShader* original, const GUID& guid) {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> result;
    if (!original)
        return result;
    IUnknown* unknown = nullptr;
    UINT bytes = sizeof(unknown);
    if (SUCCEEDED(original->GetPrivateData(guid, &bytes, &unknown)) && unknown) {
        unknown->QueryInterface(IID_PPV_ARGS(&result));
        unknown->Release();
    }
    return result;
}

}  // namespace simqvfr::games::rf2

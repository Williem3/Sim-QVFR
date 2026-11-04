#include "d3d11_compositor.h"

#include <d3dcompiler.h>

#include "../log.h"
#include "compose_shader.h"

namespace aceqv {
namespace {

bool is_typeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool D3D11Compositor::initialize(ID3D11Device* device, DXGI_FORMAT viewFormat) {
    shutdown();
    Com<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(kComposeShader, sizeof(kComposeShader) - 1, "quad-compose", nullptr, nullptr, "VS", "vs_5_0", 0, 0,
                          &vs, &errors)) ||
        FAILED(D3DCompile(kComposeShader, sizeof(kComposeShader) - 1, "quad-compose", nullptr, nullptr, "PS", "ps_5_0", 0, 0,
                          &ps, &errors))) {
        logf("d3d11 compositor: shader compile failed: %s",
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    D3D11_BUFFER_DESC cb{kComposeConstants * sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                         D3D11_CPU_ACCESS_WRITE};
    D3D11_SAMPLER_DESC sampler{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                               D3D11_TEXTURE_ADDRESS_CLAMP};
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_)) ||
        FAILED(device->CreateBuffer(&cb, nullptr, &constants_[0])) ||
        FAILED(device->CreateBuffer(&cb, nullptr, &constants_[1])) ||
        FAILED(device->CreateSamplerState(&sampler, &sampler_)) || FAILED(device->CreateDeferredContext(0, &deferred_))) {
        log_line("d3d11 compositor: making its shaders, buffers or deferred context failed");
        shutdown();
        return false;
    }
    device_ = device;
    device->GetImmediateContext(&immediate_);
    viewFormat_ = viewFormat;
    ready_ = true;
    logf("d3d11 compositor: ready (view format %d)", viewFormat);
    return true;
}

ID3D11ShaderResourceView* D3D11Compositor::source(ID3D11Texture2D* texture) {
    auto& view = sources_[texture];
    if (!view) {
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{is_typeless(d.Format) ? viewFormat_ : d.Format, D3D11_SRV_DIMENSION_TEXTURE2D};
        v.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(texture, &v, &view)))
            view.Reset();
    }
    return view.Get();
}

ID3D11RenderTargetView* D3D11Compositor::target(ID3D11Texture2D* texture) {
    auto& view = targets_[texture];
    if (!view) {
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        D3D11_RENDER_TARGET_VIEW_DESC v{is_typeless(d.Format) ? viewFormat_ : d.Format, D3D11_RTV_DIMENSION_TEXTURE2D};
        if (FAILED(device_->CreateRenderTargetView(texture, &v, &view)))
            view.Reset();
    }
    return view.Get();
}

bool D3D11Compositor::compose(ID3D11Texture2D* views, ID3D11Texture2D* output, const EyeComposition (&eyes)[2]) {
    if (!ready_ || !views || !output)
        return false;
    ID3D11ShaderResourceView* srv = source(views);
    ID3D11RenderTargetView* rtv = target(output);
    if (!srv || !rtv)
        return false;
    D3D11_TEXTURE2D_DESC outDesc;
    output->GetDesc(&outDesc);
    ID3D11DeviceContext* c = deferred_.Get();
    ID3D11ShaderResourceView* sources[2] = {srv, srv};  // t0 outer, t1 focus: both views are in the one image
    ID3D11SamplerState* sampler = sampler_.Get();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs_.Get(), nullptr, 0);
    c->PSSetShader(ps_.Get(), nullptr, 0);
    c->PSSetShaderResources(0, 2, sources);
    c->PSSetSamplers(0, 1, &sampler);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const float eyeWidth = static_cast<float>(outDesc.Width) / 2.0f;
    for (int eye = 0; eye < 2; ++eye) {
        float constants[kComposeConstants] = {};
        compose_constants(eyes[eye], eye, constants);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(c->Map(constants_[eye].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            Com<ID3D11CommandList> discard;
            c->FinishCommandList(FALSE, &discard);
            return false;
        }
        std::memcpy(mapped.pData, constants, sizeof(constants));
        c->Unmap(constants_[eye].Get(), 0);
        ID3D11Buffer* buffer = constants_[eye].Get();
        c->PSSetConstantBuffers(0, 1, &buffer);
        // The FOV crop draws the eye into the top left of its half, at the same density (as the D3D12 compositor).
        const EyeComposition& e = eyes[eye];
        const float w = eyeWidth * (e.outputSize[0] > 0 ? e.outputSize[0] : 1.0f),
                    h = static_cast<float>(outDesc.Height) * (e.outputSize[1] > 0 ? e.outputSize[1] : 1.0f);
        const D3D11_VIEWPORT viewport{eye * eyeWidth, 0, w, h, 0, 1};
        c->RSSetViewports(1, &viewport);
        c->Draw(3, 0);
    }
    Com<ID3D11CommandList> list;
    if (FAILED(c->FinishCommandList(FALSE, &list)))
        return false;
    immediate_->ExecuteCommandList(list.Get(), TRUE);  // the game's state as it was
    return true;
}

void D3D11Compositor::shutdown() {
    ready_ = false;
    sources_.clear();
    targets_.clear();
    deferred_.Reset();
    immediate_.Reset();
    vs_.Reset();
    ps_.Reset();
    constants_[0].Reset();
    constants_[1].Reset();
    sampler_.Reset();
    device_.Reset();
}

}  // namespace aceqv

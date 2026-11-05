#include "d3d11_compositor.h"

#include <cmath>
#include <cstring>

#include <d3dcompiler.h>

#include "../log.h"

namespace simqvfr {
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
    Com<ID3DBlob> vs, ps, lightPs, helmetPs, errors;
    const auto compile = [&](const char* entry, const char* profile, Com<ID3DBlob>& blob) {
        return SUCCEEDED(D3DCompile(kComposeShader, sizeof(kComposeShader) - 1, "quad-compose", nullptr, nullptr, entry,
                                    profile, 0, 0, &blob, &errors));
    };
    if (!compile("VS", "vs_5_0", vs) || !compile("PS", "ps_5_0", ps) || !compile("LightPS", "ps_5_0", lightPs) ||
        !compile("HelmetLayerPS", "ps_5_0", helmetPs)) {
        logf("d3d11 compositor: shader compile failed: %s",
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    D3D11_BUFFER_DESC cb{kComposeConstants * sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                         D3D11_CPU_ACCESS_WRITE};
    D3D11_SAMPLER_DESC sampler{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                               D3D11_TEXTURE_ADDRESS_CLAMP};
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    // The light pass: the new estimate weighted by the blend factor, the last frames' by its inverse.
    D3D11_BLEND_DESC blend = {};
    D3D11_RENDER_TARGET_BLEND_DESC& b = blend.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = TRUE;
    D3D11_TEXTURE2D_DESC lightDesc{4, 2, 1, 1, kLightFormat, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    D3D11_TEXTURE2D_DESC readbackDesc = lightDesc;
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.BindFlags = 0;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_)) &&
              SUCCEEDED(device->CreatePixelShader(lightPs->GetBufferPointer(), lightPs->GetBufferSize(), nullptr, &lightPs_)) &&
              SUCCEEDED(device->CreatePixelShader(helmetPs->GetBufferPointer(), helmetPs->GetBufferSize(), nullptr,
                                                  &helmetPs_)) &&
              SUCCEEDED(device->CreateSamplerState(&sampler, &sampler_)) &&
              SUCCEEDED(device->CreateBlendState(&blend, &lightBlend_)) &&
              SUCCEEDED(device->CreateRasterizerState(&raster, &scissored_)) &&
              SUCCEEDED(device->CreateTexture2D(&lightDesc, nullptr, &light_)) &&
              SUCCEEDED(device->CreateShaderResourceView(light_.Get(), nullptr, &lightSrv_)) &&
              SUCCEEDED(device->CreateRenderTargetView(light_.Get(), nullptr, &lightRtv_)) &&
              SUCCEEDED(device->CreateDeferredContext(0, &deferred_));
    for (auto& buffer : constants_)
        ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &buffer));
    if (!ok) {
        log_line("d3d11 compositor: making its shaders, buffers, light texture or deferred context failed");
        shutdown();
        return false;
    }
    if (FAILED(device->CreateTexture2D(&readbackDesc, nullptr, &lightReadback_)))
        lightReadback_.Reset();  // no light in the log; the overlay itself is unaffected
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

ID3D11RenderTargetView* D3D11Compositor::target(ID3D11Texture2D* texture, DXGI_FORMAT typelessAs) {
    auto& view = targets_[{texture, typelessAs}];
    if (!view) {
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        D3D11_RENDER_TARGET_VIEW_DESC v{is_typeless(d.Format) ? typelessAs : d.Format, D3D11_RTV_DIMENSION_TEXTURE2D};
        if (FAILED(device_->CreateRenderTargetView(texture, &v, &view)))
            view.Reset();
    }
    return view.Get();
}

bool D3D11Compositor::set_constants(int buffer, const float (&constants)[kComposeConstants]) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(deferred_->Map(constants_[buffer].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, constants, sizeof(constants));
    deferred_->Unmap(constants_[buffer].Get(), 0);
    ID3D11Buffer* b = constants_[buffer].Get();
    deferred_->PSSetConstantBuffers(0, 1, &b);
    return true;
}

bool D3D11Compositor::discard() {
    Com<ID3D11CommandList> dropped;
    deferred_->FinishCommandList(FALSE, &dropped);
    return false;
}

bool D3D11Compositor::finish() {
    Com<ID3D11CommandList> list;
    if (FAILED(deferred_->FinishCommandList(FALSE, &list)))
        return false;
    immediate_->ExecuteCommandList(list.Get(), TRUE);  // the game's state as it was
    return true;
}

void D3D11Compositor::bind_sources(ID3D11ShaderResourceView* views) {
    ID3D11ShaderResourceView* sources[6] = {views, views, lightSrv_.Get(), pieces_[0].Get(), pieces_[1].Get(),
                                            pieces_[2].Get()};
    deferred_->PSSetShaderResources(0, 6, sources);
}

bool D3D11Compositor::record_light(ID3D11ShaderResourceView* views, const float (&source0)[4], const float (&source1)[4]) {
    ID3D11DeviceContext* c = deferred_.Get();
    ID3D11ShaderResourceView* none[6] = {};
    c->PSSetShaderResources(0, 6, none);  // the light texture is the target here
    c->PSSetShaderResources(0, 1, &views);
    ID3D11RenderTargetView* rtv = lightRtv_.Get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const float factor = lightFresh_ ? 1.0f : kLightBlend, factors[4] = {factor, factor, factor, factor};
    c->OMSetBlendState(lightBlend_.Get(), factors, 0xffffffff);
    const D3D11_VIEWPORT viewport{0, 0, 4, 2, 0, 1};
    c->RSSetViewports(1, &viewport);
    c->PSSetShader(lightPs_.Get(), nullptr, 0);
    float constants[kComposeConstants] = {};
    light_constants(source0, source1, constants);
    if (!set_constants(kLightBuffer, constants))
        return false;
    c->Draw(3, 0);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ID3D11RenderTargetView* noTarget = nullptr;
    c->OMSetRenderTargets(1, &noTarget, nullptr);  // the light texture is read from here on
    if (lightWanted_ && lightReadback_ && !lightCopied_) {  // for read_light
        c->CopyResource(lightReadback_.Get(), light_.Get());
        lightCopied_ = true;
        lightWanted_ = false;
    }
    lightFresh_ = false;
    return true;
}

bool D3D11Compositor::compose(ID3D11Texture2D* views, ID3D11Texture2D* output, const EyeComposition (&eyes)[2]) {
    if (!ready_ || !views || !output)
        return false;
    ID3D11ShaderResourceView* srv = source(views);
    ID3D11RenderTargetView* rtv = target(output, viewFormat_);
    if (!srv || !rtv)
        return false;
    D3D11_TEXTURE2D_DESC outDesc;
    output->GetDesc(&outDesc);
    ID3D11DeviceContext* c = deferred_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs_.Get(), nullptr, 0);
    c->PSSetSamplers(0, 1, &sampler);
    if ((eyes[0].helmet > 0 || eyes[1].helmet > 0) && !record_light(srv, eyes[0].outerSource, eyes[1].outerSource))
        return discard();
    c->PSSetShader(ps_.Get(), nullptr, 0);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    bind_sources(srv);  // t0 outer, t1 focus: both views are in the one image
    const float eyeWidth = static_cast<float>(outDesc.Width) / 2.0f;
    const unsigned pieceMask = helmet_piece_mask();
    for (int eye = 0; eye < 2; ++eye) {
        const EyeComposition& e = eyes[eye];
        float constants[kComposeConstants] = {};
        compose_constants(e, eye, constants);
        helmet_constants(e, pieceMask, visor_tints(e.visorBlend), visor_white_point(), constants);
        if (!set_constants(eye, constants))
            return discard();
        // The FOV crop draws the eye into the top left of its half, at the same density (as the D3D12 compositor).
        const float w = eyeWidth * (e.outputSize[0] > 0 ? e.outputSize[0] : 1.0f),
                    h = static_cast<float>(outDesc.Height) * (e.outputSize[1] > 0 ? e.outputSize[1] : 1.0f);
        const D3D11_VIEWPORT viewport{eye * eyeWidth, 0, w, h, 0, 1};
        c->RSSetViewports(1, &viewport);
        c->Draw(3, 0);
    }
    return finish();
}

bool D3D11Compositor::draw_helmet_layer(ID3D11Texture2D* views, ID3D11Texture2D* target_, const HelmetLayerDraw& d) {
    if (!ready_ || !views || !target_ || !has_helmet_pieces())
        return false;
    ID3D11ShaderResourceView* srv = source(views);
    ID3D11RenderTargetView* rtv = target(target_, kHelmetLayerFormat);
    if (!srv || !rtv)
        return false;
    D3D11_TEXTURE2D_DESC desc;
    target_->GetDesc(&desc);
    ID3D11DeviceContext* c = deferred_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs_.Get(), nullptr, 0);
    c->PSSetSamplers(0, 1, &sampler);
    if (!record_light(srv, d.outerSource[0], d.outerSource[1]))
        return discard();
    c->PSSetShader(helmetPs_.Get(), nullptr, 0);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    bind_sources(srv);
    float constants[kComposeConstants] = {};
    helmet_layer_constants(d, helmet_piece_mask(), visor_tints(d.visorBlend), visor_white_point(), constants);
    if (!set_constants(kLayerBuffer, constants))
        return discard();
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    c->RSSetViewports(1, &viewport);
    c->RSSetState(scissored_.Get());
    // Only the bands the pieces draw on (the rest of the image is never submitted).
    const std::vector<HelmetBand> whole = {{0, 1}};
    for (const HelmetBand& band : d.bands.empty() ? whole : d.bands) {
        const D3D11_RECT scissor{0, static_cast<LONG>(std::floor(band.top * desc.Height)), static_cast<LONG>(desc.Width),
                                 static_cast<LONG>(std::ceil(band.bottom * desc.Height))};
        c->RSSetScissorRects(1, &scissor);
        c->Draw(3, 0);
    }
    return finish();
}

bool D3D11Compositor::read_light(float (&rgb)[2][4][3]) {
    if (!lightCopied_ || !lightReadback_ || !immediate_)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(immediate_->Map(lightReadback_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        return false;  // not copied yet (DXGI_ERROR_WAS_STILL_DRAWING): asked again later
    lightCopied_ = false;
    for (int eye = 0; eye < 2; ++eye)
        for (int side = 0; side < 4; ++side) {
            uint16_t texel[4];
            std::memcpy(texel, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(eye) * mapped.RowPitch +
                                   static_cast<size_t>(side) * sizeof(texel),
                        sizeof(texel));
            for (int ch = 0; ch < 3; ++ch)
                rgb[eye][side][ch] = half_to_float(texel[ch]);
        }
    immediate_->Unmap(lightReadback_.Get(), 0);
    return true;
}

bool D3D11Compositor::upload_piece(int which, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
    const D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                    D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA init{pixels.data(), width * 4};
    Com<ID3D11Texture2D> texture;
    Com<ID3D11ShaderResourceView> view;
    if (FAILED(device_->CreateTexture2D(&desc, &init, &texture)) ||
        FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr, &view)))
        return false;
    pieces_[which] = view;
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
    lightPs_.Reset();
    helmetPs_.Reset();
    for (auto& buffer : constants_)
        buffer.Reset();
    sampler_.Reset();
    lightBlend_.Reset();
    scissored_.Reset();
    light_.Reset();
    lightReadback_.Reset();
    lightSrv_.Reset();
    lightRtv_.Reset();
    lightFresh_ = true;
    lightWanted_ = lightCopied_ = false;
    for (auto& piece : pieces_)
        piece.Reset();
    clear_pieces();
    device_.Reset();
}

}  // namespace simqvfr

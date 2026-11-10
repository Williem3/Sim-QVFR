#include "focus_post.h"

#include <d3dcompiler.h>

#include <algorithm>

namespace lmu {
namespace {

// Full-screen triangles whose outputs match YEBIS's own vertex shaders' (a TEXCOORD per register, then the position), so
// the game's pixel shaders take them: its copy reads TEXCOORD0; its composite TEXCOORD0 (scene), TEXCOORD1 (glare) and
// TEXCOORD3 (the 1x1 exposure, any UV). float4 outputs keep each semantic in a register of its own.
constexpr char kShader[] = R"(
cbuffer Rects : register(b12) { float4 sceneRect; };  // xy: UV offset, zw: UV size
void corner(uint id, out float4 pos, out float2 uv) {
    float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);
    pos = float4(p, 0, 1);
    uv = float2((p.x + 1) * 0.5, (1 - p.y) * 0.5);
}
struct CopyOut { float4 uv : TEXCOORD0; float4 pos : SV_Position; };
CopyOut copy_vs(uint id : SV_VertexID) {
    CopyOut o; float2 uv; corner(id, o.pos, uv);
    o.uv = float4(sceneRect.xy + uv * sceneRect.zw, 0, 0);
    return o;
}
struct CompositeOut {
    float4 scene : TEXCOORD0; float4 glare : TEXCOORD1; float4 unused2 : TEXCOORD2; float4 exposure : TEXCOORD3;
    float4 unused4 : TEXCOORD4; float4 pos : SV_Position;
};
CompositeOut composite_vs(uint id : SV_VertexID) {
    CompositeOut o; float2 uv; corner(id, o.pos, uv);
    o.scene = float4(uv, 0, 0); o.glare = float4(uv, 0, 0); o.unused2 = 0; o.exposure = float4(0.5, 0.5, 0, 0);
    o.unused4 = 0;
    return o;
}
)";

template <class T> using Com = Microsoft::WRL::ComPtr<T>;

bool compile_vs(ID3D11Device* device, const char* entry, Com<ID3D11VertexShader>& out) {
    Com<ID3DBlob> code, errors;
    return SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "focus-post", nullptr, nullptr, entry, "vs_5_0", 0, 0, &code,
                                &errors)) &&
           SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out));
}

}  // namespace

bool FocusPost::initialize(ID3D11Device* device) {
    if (ready_)
        return true;
    if (!device || FAILED(device->CreateDeferredContext(0, &deferred_)) || !compile_vs(device, "copy_vs", copyVs_) ||
        !compile_vs(device, "composite_vs", compositeVs_))
        return false;
    const D3D11_BUFFER_DESC rd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bs = {};
    bs.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC ds = {};
    if (FAILED(device->CreateBuffer(&rd, nullptr, &rects_)) || FAILED(device->CreateRasterizerState(&rs, &raster_)) ||
        FAILED(device->CreateBlendState(&bs, &blend_)) || FAILED(device->CreateDepthStencilState(&ds, &depth_)) ||
        !resample_.initialize(device))
        return false;
    device_ = device;
    ready_ = true;
    return true;
}

bool FocusPost::ensure(Image& image, unsigned width, unsigned height, DXGI_FORMAT format) {
    if (image.texture && image.width == width && image.height == height)
        return true;
    image = Image{};
    const D3D11_TEXTURE2D_DESC td = {width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (!width || !height || FAILED(device_->CreateTexture2D(&td, nullptr, &image.texture)) ||
        FAILED(device_->CreateShaderResourceView(image.texture.Get(), nullptr, &image.srv)) ||
        FAILED(device_->CreateRenderTargetView(image.texture.Get(), nullptr, &image.rtv))) {
        image = Image{};
        return false;
    }
    image.width = width;
    image.height = height;
    return true;
}

ID3D11Buffer* FocusPost::snapshot(ID3D11DeviceContext* context, Com<ID3D11Buffer>& keep) {
    Com<ID3D11Buffer> bound;
    context->PSGetConstantBuffers(0, 1, &bound);
    if (!bound)
        return nullptr;
    D3D11_BUFFER_DESC d;
    bound->GetDesc(&d);
    D3D11_BUFFER_DESC have = {};
    if (keep)
        keep->GetDesc(&have);
    if (!keep || have.ByteWidth != d.ByteWidth) {
        keep.Reset();
        const D3D11_BUFFER_DESC kd = {d.ByteWidth, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (FAILED(device_->CreateBuffer(&kd, nullptr, &keep)))
            return nullptr;
    }
    context->CopyResource(keep.Get(), bound.Get());
    return keep.Get();
}

bool FocusPost::run(ID3D11DeviceContext* context) {
    Com<ID3D11CommandList> list;
    if (FAILED(deferred_->FinishCommandList(FALSE, &list)))
        return false;
    context->ExecuteCommandList(list.Get(), TRUE);  // the game's state as it was
    return true;
}

bool FocusPost::after_copy(ID3D11DeviceContext* context, int eye, ID3D11ShaderResourceView* scene, unsigned sceneWidth,
                           unsigned sceneHeight, const FocusPostRegion& focus) {
    if (!ready_ || eye < 0 || eye > 1 || !scene || !sceneWidth || !sceneHeight || !focus.width || !focus.height ||
        focus.x + focus.width > sceneWidth || focus.y + focus.height > sceneHeight)
        return false;
    View& v = views_[eye];
    v.sceneReady = false;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11SamplerState> sampler;
    context->PSGetShader(&ps, nullptr, nullptr);
    context->PSGetSamplers(0, 1, &sampler);
    ID3D11Buffer* constants = snapshot(context, copyConstants_);
    if (!ps || !constants || !ensure(v.scene, focus.width, focus.height, DXGI_FORMAT_R16G16B16A16_FLOAT))
        return false;
    const float rect[4] = {static_cast<float>(focus.x) / sceneWidth, static_cast<float>(focus.y) / sceneHeight,
                           static_cast<float>(focus.width) / sceneWidth, static_cast<float>(focus.height) / sceneHeight};
    context->UpdateSubresource(rects_.Get(), 0, nullptr, rect, 0, 0);
    ID3D11DeviceContext* d = deferred_.Get();
    d->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->VSSetShader(copyVs_.Get(), nullptr, 0);
    d->VSSetConstantBuffers(12, 1, rects_.GetAddressOf());
    d->PSSetShader(ps.Get(), nullptr, 0);
    d->PSSetConstantBuffers(0, 1, &constants);
    d->PSSetSamplers(0, 1, sampler.GetAddressOf());
    d->PSSetShaderResources(0, 1, &scene);
    d->RSSetState(raster_.Get());
    d->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    d->OMSetDepthStencilState(depth_.Get(), 0);
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(focus.width), static_cast<float>(focus.height), 0, 1};
    d->RSSetViewports(1, &vp);
    d->OMSetRenderTargets(1, v.scene.rtv.GetAddressOf(), nullptr);
    d->Draw(3, 0);
    if (!run(context))
        return false;
    v.sceneReady = true;
    v.sceneWidth = sceneWidth;
    v.sceneHeight = sceneHeight;
    return true;
}

bool FocusPost::after_composite(ID3D11DeviceContext* context, int eye, const simqvfr::ViewRegion& glareRegion,
                                const simqvfr::ViewMapping& mapping) {
    if (!ready_ || eye < 0 || eye > 1)
        return false;
    View& v = views_[eye];
    if (!v.sceneReady)
        return false;
    v.sceneReady = false;
    Com<ID3D11PixelShader> ps;
    ID3D11SamplerState* samplers[3] = {};
    ID3D11ShaderResourceView* srvs[3] = {};
    context->PSGetShader(&ps, nullptr, nullptr);
    context->PSGetSamplers(0, 3, samplers);
    context->PSGetShaderResources(0, 3, srvs);
    const auto release = [&] {
        for (auto* s : samplers)
            if (s)
                s->Release();
        for (auto* s : srvs)
            if (s)
                s->Release();
    };
    ID3D11Buffer* constants = snapshot(context, compositeConstants_);
    bool ok = ps && constants && srvs[1] && srvs[2] && glareRegion.width > 0 && glareRegion.height > 0;
    // The glare image at the glare's density: the eye's glare covers its view at width x (glare texture / scene).
    unsigned glareW = 0, glareH = 0;
    if (ok) {
        Com<ID3D11Resource> resource;
        srvs[1]->GetResource(&resource);
        Com<ID3D11Texture2D> texture;
        ok = resource && SUCCEEDED(resource.As(&texture));
        if (ok) {
            D3D11_TEXTURE2D_DESC td;
            texture->GetDesc(&td);
            glareW = std::max(1u, (v.scene.width * td.Width + v.sceneWidth / 2) / v.sceneWidth);
            glareH = std::max(1u, (v.scene.height * td.Height + v.sceneHeight / 2) / v.sceneHeight);
            ok = ensure(v.glare, glareW, glareH, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                 resample_.resample_target(srvs[1], td.Width, td.Height, glareRegion, v.glare.texture.Get(),
                                           {0, 0, static_cast<int>(glareW), static_cast<int>(glareH)}, mapping);
        }
    }
    ok = ok && ensure(v.focus, v.scene.width, v.scene.height, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (ok) {
        ID3D11DeviceContext* d = deferred_.Get();
        d->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d->VSSetShader(compositeVs_.Get(), nullptr, 0);
        d->PSSetShader(ps.Get(), nullptr, 0);
        d->PSSetConstantBuffers(0, 1, &constants);
        d->PSSetSamplers(0, 3, samplers);
        ID3D11ShaderResourceView* inputs[3] = {v.scene.srv.Get(), v.glare.srv.Get(), srvs[2]};
        d->PSSetShaderResources(0, 3, inputs);
        d->RSSetState(raster_.Get());
        d->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
        d->OMSetDepthStencilState(depth_.Get(), 0);
        const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(v.focus.width), static_cast<float>(v.focus.height), 0, 1};
        d->RSSetViewports(1, &vp);
        d->OMSetRenderTargets(1, v.focus.rtv.GetAddressOf(), nullptr);
        d->Draw(3, 0);
        ok = run(context);
    }
    release();
    if (ok) {
        v.image = v.focus.texture;
        ++v.composites;
    }
    return ok;
}

void FocusPost::shutdown() {
    for (View& v : views_)
        v = View{};
    resample_.shutdown();
    deferred_.Reset();
    copyVs_.Reset();
    compositeVs_.Reset();
    rects_.Reset();
    copyConstants_.Reset();
    compositeConstants_.Reset();
    raster_.Reset();
    blend_.Reset();
    depth_.Reset();
    device_.Reset();
    ready_ = false;
}

}  // namespace lmu

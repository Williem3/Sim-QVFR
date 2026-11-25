#pragma once

// RF2's composition: at an eye's resolve, the resolved scene (its outer view at the top left, its focus view at the top
// right: render_backend.h AtlasLayout) is redrawn over the whole resolved target as the finished eye, the outer view
// stretched over it and the focus view inside its box, feathered, rounded and outlined as the ini says. Called from the
// resolve's observer (context_hooks): its own calls on the context go straight through.

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "../../log.h"
#include "render_backend.h"

namespace simqvfr::games::rf2 {

class Compositor {
public:
    // The focus area's look (the ini's feather_percent, corner_radius_percent, debug_border and debug_border_color),
    // live from the layer's thread; compose() uses the latest on the render thread. `feather`: the blend band inside
    // the box's edge, a share of the box (0..0.5). `cornerRadius`: a share of half the box's shorter side (0 square).
    // `outline`: a thin line in `color` (linear RGBA) at the box's edge.
    void set_look(double feather, double cornerRadius, bool outline, const float (&color)[4]) {
        Look next;
        next.feather = static_cast<float>(std::clamp(feather, 0.0, 0.5));
        next.corner = static_cast<float>(std::clamp(cornerRadius, 0.0, 1.0));
        next.outline = outline ? 0.008f : 0.0f;
        std::memcpy(next.color, color, sizeof(next.color));
        std::lock_guard<std::mutex> lock(lookMutex_);
        if (lookSet_ && std::memcmp(&next, &look_, sizeof(Look)) == 0)
            return;
        simqvfr::logf("rf2 look: feather %.0f%%, corner radius %.0f%%, outline %s", next.feather * 100, next.corner * 100,
                      outline ? "on" : "off");
        lookSet_ = true;
        look_ = next;
    }

    // `resolved` (subresource `sub`): the eye's resolved scene, the layout's size, single-sampled; `format`: the resolve's
    // (a typed format, for the views). `focusRect`: the eye's box as shares of the image.
    bool compose(ID3D11DeviceContext* c, ID3D11Resource* resolved, UINT sub, DXGI_FORMAT format, int eye,
                 const float (&focusRect)[4], const AtlasLayout& layout) {
        using Microsoft::WRL::ComPtr;
        if (!c || !resolved || sub || eye < 0 || eye > 1 || !layout.valid())
            return false;
        ComPtr<ID3D11Texture2D> source;
        if (FAILED(resolved->QueryInterface(IID_PPV_ARGS(&source))))
            return false;
        D3D11_TEXTURE2D_DESC td{};
        source->GetDesc(&td);
        if (td.Width != layout.width || td.Height != layout.height || td.SampleDesc.Count != 1 || td.ArraySize != 1)
            return false;
        ComPtr<ID3D11Device> device;
        c->GetDevice(&device);
        if (!ensure(device.Get(), td, format))
            return false;

        c->CopyResource(source_.Get(), resolved);
        const SavedState saved(c);

        ID3D11RenderTargetView* target = tempRtv_.Get();
        c->OMSetRenderTargets(1, &target, nullptr);
        c->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        c->OMSetDepthStencilState(nullptr, 0);
        c->RSSetState(nullptr);
        const D3D11_VIEWPORT full{0, 0, static_cast<float>(layout.width), static_cast<float>(layout.height), 0, 1};
        c->RSSetViewports(1, &full);
        c->RSSetScissorRects(0, nullptr);
        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs_.Get(), nullptr, 0);
        c->PSSetShader(ps_.Get(), nullptr, 0);
        c->GSSetShader(nullptr, nullptr, 0);
        c->HSSetShader(nullptr, nullptr, 0);
        c->DSSetShader(nullptr, nullptr, 0);
        ID3D11ShaderResourceView* srv = sourceSrv_.Get();
        ID3D11SamplerState* sampler = sampler_.Get();
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetSamplers(0, 1, &sampler);
        Look look;
        {
            std::lock_guard<std::mutex> lock(lookMutex_);
            look = look_;
        }
        const float w = static_cast<float>(layout.width), h = static_cast<float>(layout.height);
        // The focus box (lo, hi), the focus view's tile and the outer view's size, as shares of the scene target; the
        // look: feather, outline width, corner radius (x, y, z), the outline's colour; the target's texel and size.
        const float constants[24] = {focusRect[0], focusRect[1], focusRect[2], focusRect[3],
                                     static_cast<float>(layout.focus_x()) / w, 0.0f, static_cast<float>(layout.focusW) / w,
                                     static_cast<float>(layout.focusH) / h,
                                     static_cast<float>(layout.outerW) / w, static_cast<float>(layout.outerH) / h, 0.0f, 0.0f,
                                     look.feather, look.outline, look.corner, 0.0f,
                                     look.color[0], look.color[1], look.color[2], look.color[3],
                                     1.0f / w, 1.0f / h, w, h};
        c->UpdateSubresource(focusBoxCb_.Get(), 0, nullptr, constants, 0, 0);
        ID3D11Buffer* focusBox = focusBoxCb_.Get();
        c->PSSetConstantBuffers(13, 1, &focusBox);
        c->Draw(3, 0);
        ID3D11ShaderResourceView* nil = nullptr;
        c->PSSetShaderResources(0, 1, &nil);
        c->CopyResource(resolved, temp_.Get());
        saved.restore(c);
        ++frames_[eye];
        return true;
    }

    uint64_t frames(int eye) const { return eye >= 0 && eye < 2 ? frames_[eye] : 0; }

private:
    // The context state the composition changes, put back after it.
    struct SavedState {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
        FLOAT blendFactor[4]{};
        UINT sampleMask = 0, stencilRef = 0, viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE,
             scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs;
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
        Microsoft::WRL::ComPtr<ID3D11GeometryShader> gs;
        Microsoft::WRL::ComPtr<ID3D11HullShader> hs;
        Microsoft::WRL::ComPtr<ID3D11DomainShader> ds;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        Microsoft::WRL::ComPtr<ID3D11Buffer> psCb13;

        explicit SavedState(ID3D11DeviceContext* c) {
            ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                rtv[i].Attach(rtvs[i]);
            c->OMGetBlendState(&blend, blendFactor, &sampleMask);
            c->OMGetDepthStencilState(&depth, &stencilRef);
            c->RSGetState(&rs);
            c->RSGetViewports(&viewports, vp);
            c->RSGetScissorRects(&scissors, sc);
            c->IAGetInputLayout(&layout);
            c->IAGetPrimitiveTopology(&topology);
            c->VSGetShader(&vs, nullptr, nullptr);
            c->PSGetShader(&ps, nullptr, nullptr);
            c->GSGetShader(&gs, nullptr, nullptr);
            c->HSGetShader(&hs, nullptr, nullptr);
            c->DSGetShader(&ds, nullptr, nullptr);
            c->PSGetShaderResources(0, 1, &srv);
            c->PSGetSamplers(0, 1, &sampler);
            c->PSGetConstantBuffers(13, 1, &psCb13);
        }
        void restore(ID3D11DeviceContext* c) const {
            ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
            UINT count = 0;
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                rtvs[i] = rtv[i].Get();
                if (rtvs[i])
                    count = i + 1;
            }
            c->OMSetRenderTargets(count, rtvs, dsv.Get());
            c->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
            c->OMSetDepthStencilState(depth.Get(), stencilRef);
            c->RSSetState(rs.Get());
            c->RSSetViewports(viewports, vp);
            c->RSSetScissorRects(scissors, sc);
            c->IASetInputLayout(layout.Get());
            c->IASetPrimitiveTopology(topology);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            c->GSSetShader(gs.Get(), nullptr, 0);
            c->HSSetShader(hs.Get(), nullptr, 0);
            c->DSSetShader(ds.Get(), nullptr, 0);
            ID3D11ShaderResourceView* s = srv.Get();
            ID3D11SamplerState* smp = sampler.Get();
            ID3D11Buffer* cb = psCb13.Get();
            c->PSSetShaderResources(0, 1, &s);
            c->PSSetSamplers(0, 1, &smp);
            c->PSSetConstantBuffers(13, 1, &cb);
        }
    };

    bool ensure(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& target, DXGI_FORMAT format) {
        using Microsoft::WRL::ComPtr;
        if (!device)
            return false;
        if (device != device_.Get()) {  // the renderer's device changed (context_hooks::retarget): everything made again
            device_ = device;
            vs_.Reset();
            ps_.Reset();
            focusBoxCb_.Reset();
            sampler_.Reset();
            temp_.Reset();
        }
        if (!vs_ || !ps_) {
            static constexpr char shader[] = R"HLSL(
Texture2D<float4> Atlas : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer FocusBox : register(b13) { float4 FocusLoHi; float4 FocusTile; float4 OuterTile; float4 Look; float4 LookColor; float4 Target; };
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut VS(uint id : SV_VertexID) { VOut o; float2 p=float2((id<<1)&2,id&2); o.uv=p; o.pos=float4(p.x*2-1,1-p.y*2,0,1); return o; }
// How far inside the focus box a point is (f: 0..1 across it), as a share of the box; negative outside. Square corners:
// the nearer edge's share of its own axis. Rounded (Look.z: the radius as a share of half the shorter side): the
// distance to the rounded edge over the shorter side, measured in the box's pixels so the corners are circular.
float inside_box(float2 f, float2 size) {
 if (Look.z <= 0) { float2 e=min(f,1-f); return min(e.x,e.y); }
 float2 hs=0.5*size; float r=Look.z*min(hs.x,hs.y);
 float2 q=abs((f-0.5)*size)-hs+r;
 float d=length(max(q,0))+min(max(q.x,q.y),0)-r;
 return -d/min(size.x,size.y);
}
float4 PS(VOut i) : SV_Target {
 float2 texel=Target.xy;
 float2 ouv=clamp(i.uv*OuterTile.xy,texel*0.5,OuterTile.xy-texel*0.5);
 float4 outer=Atlas.SampleLevel(LinearClamp,ouv,0);
 float2 lo=FocusLoHi.xy,hi=FocusLoHi.zw;
 bool inside=all(i.uv>=lo)&&all(i.uv<=hi);
 if(inside){ float2 local=(i.uv-lo)/(hi-lo);
  float e=inside_box(local,(hi-lo)*Target.zw);
  if(e<0) return outer;
  float w=saturate(e/max(Look.x,1e-4)); w=w*w*(3-2*w);
  float2 uv=float2(FocusTile.x+local.x*FocusTile.z,local.y*FocusTile.w);
  uv=clamp(uv,float2(FocusTile.x,0)+texel*0.5,float2(FocusTile.x+FocusTile.z,FocusTile.w)-texel*0.5);
  float4 color=lerp(outer,Atlas.SampleLevel(LinearClamp,uv,0),w);
  if(e<Look.y) color=LookColor;
  return color; }
 return outer;
})HLSL";
            ComPtr<ID3DBlob> vsCode, psCode, errors;
            const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
            if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "rf2_outer_focus", nullptr, nullptr, "VS", "vs_5_0", flags, 0,
                                  &vsCode, &errors))) {
                simqvfr::logf("rf2 compose: VS compile failed: %s",
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
                return false;
            }
            errors.Reset();
            if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "rf2_outer_focus", nullptr, nullptr, "PS", "ps_5_0", flags, 0,
                                  &psCode, &errors))) {
                simqvfr::logf("rf2 compose: PS compile failed: %s",
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
                return false;
            }
            if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)) ||
                FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_)))
                return false;
            const D3D11_BUFFER_DESC cbd = {96, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (FAILED(device->CreateBuffer(&cbd, nullptr, &focusBoxCb_)))
                return false;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &sampler_)))
                return false;
        }
        if (!temp_ || width_ != target.Width || height_ != target.Height || format_ != target.Format || view_ != format) {
            D3D11_TEXTURE2D_DESC td = target;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.SampleDesc = {1, 0};
            td.Usage = D3D11_USAGE_DEFAULT;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            temp_.Reset();
            tempRtv_.Reset();
            source_.Reset();
            sourceSrv_.Reset();
            const DXGI_FORMAT viewFormat = format != DXGI_FORMAT_UNKNOWN ? format : target.Format;
            D3D11_RENDER_TARGET_VIEW_DESC rd{viewFormat, D3D11_RTV_DIMENSION_TEXTURE2D};
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{viewFormat, D3D11_SRV_DIMENSION_TEXTURE2D};
            sd.Texture2D.MipLevels = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &temp_)) ||
                FAILED(device->CreateRenderTargetView(temp_.Get(), &rd, &tempRtv_))) {
                temp_.Reset();
                return false;
            }
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &source_)) ||
                FAILED(device->CreateShaderResourceView(source_.Get(), &sd, &sourceSrv_))) {
                temp_.Reset();
                return false;
            }
            width_ = target.Width;
            height_ = target.Height;
            format_ = target.Format;
            view_ = format;
        }
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> focusBoxCb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> tempRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceSrv_;
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN, view_ = DXGI_FORMAT_UNKNOWN;
    uint64_t frames_[2]{};

    struct Look {
        float feather = 0.0f, outline = 0.0f, corner = 0.0f;
        float color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    };
    std::mutex lookMutex_;
    Look look_;
    bool lookSet_ = false;
};

}  // namespace simqvfr::games::rf2

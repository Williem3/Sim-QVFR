#pragma once

#include <algorithm>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <windows.h>
#include <wrl/client.h>

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
        if (lookSet_ && std::memcmp(&next, &look_, sizeof(Look)) == 0) return;
        simqvfr::logf("rf2 look: feather %.0f%%, corner radius %.0f%%, outline %s", next.feather * 100,
                      next.corner * 100, outline ? "on" : "off");
        lookSet_ = true;
        look_ = next;
    }

    bool compose(ID3D11DeviceContext* c, ID3D11Resource* resolved, UINT sub, int eye,
                 const float (&focusRect)[2][4], const AtlasLayout& layout, bool& replaying) {
        using Microsoft::WRL::ComPtr;
        if (!c || !resolved || sub || eye < 0 || eye > 1) return false;
        ComPtr<ID3D11Texture2D> source;
        if (FAILED(resolved->QueryInterface(IID_PPV_ARGS(&source)))) return false;
        D3D11_TEXTURE2D_DESC td{};
        source->GetDesc(&td);
        if (td.Width != 3184 || td.Height != 3096 || td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            td.SampleDesc.Count != 1) return false;
        ComPtr<ID3D11Device> device;
        c->GetDevice(&device);
        if (!ensure(device.Get(), td)) return false;

        c->CopyResource(source_.Get(), resolved);
        ComPtr<ID3D11RenderTargetView> oldRtv;
        ComPtr<ID3D11DepthStencilView> oldDsv;
        c->OMGetRenderTargets(1, &oldRtv, &oldDsv);
        ComPtr<ID3D11BlendState> oldBlend;
        FLOAT bf[4]{};
        UINT mask = 0;
        c->OMGetBlendState(&oldBlend, bf, &mask);
        ComPtr<ID3D11DepthStencilState> oldDepth;
        UINT stencil = 0;
        c->OMGetDepthStencilState(&oldDepth, &stencil);
        ComPtr<ID3D11RasterizerState> oldRs;
        c->RSGetState(&oldRs);
        D3D11_VIEWPORT oldVp[16]{};
        UINT nVp = 16;
        c->RSGetViewports(&nVp, oldVp);
        D3D11_RECT oldSc[16]{};
        UINT nSc = 16;
        c->RSGetScissorRects(&nSc, oldSc);
        ComPtr<ID3D11InputLayout> oldLayout;
        c->IAGetInputLayout(&oldLayout);
        D3D11_PRIMITIVE_TOPOLOGY oldTopo{};
        c->IAGetPrimitiveTopology(&oldTopo);
        ComPtr<ID3D11VertexShader> oldVs;
        ComPtr<ID3D11PixelShader> oldPs;
        ComPtr<ID3D11GeometryShader> oldGs;
        ComPtr<ID3D11HullShader> oldHs;
        ComPtr<ID3D11DomainShader> oldDs;
        c->VSGetShader(&oldVs, nullptr, nullptr);
        c->PSGetShader(&oldPs, nullptr, nullptr);
        c->GSGetShader(&oldGs, nullptr, nullptr);
        c->HSGetShader(&oldHs, nullptr, nullptr);
        c->DSGetShader(&oldDs, nullptr, nullptr);
        ComPtr<ID3D11ShaderResourceView> oldSrv;
        c->PSGetShaderResources(0, 1, &oldSrv);
        ComPtr<ID3D11SamplerState> oldSampler;
        c->PSGetSamplers(0, 1, &oldSampler);
        ComPtr<ID3D11Buffer> oldPsCb13;
        c->PSGetConstantBuffers(13, 1, &oldPsCb13);

        const bool was = replaying;
        replaying = true;
        ID3D11RenderTargetView* target = tempRtv_.Get();
        c->OMSetRenderTargets(1, &target, nullptr);
        c->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        c->OMSetDepthStencilState(nullptr, 0);
        c->RSSetState(nullptr);
        const D3D11_VIEWPORT full{0, 0, 3184.0f, 3096.0f, 0, 1};
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
        // The focus box (lo, hi), the focus view's tile and the outer view's size, as shares of the scene target; then
        // the look: feather, outline width, corner radius (x, y, z), and the outline's colour.
        Look look;
        {
            std::lock_guard<std::mutex> lock(lookMutex_);
            look = look_;
        }
        const float constants[20] = {focusRect[eye][0], focusRect[eye][1], focusRect[eye][2], focusRect[eye][3],
                                     static_cast<float>(layout.focus_x()) / kSceneWidth, 0.0f,
                                     static_cast<float>(layout.focusW) / kSceneWidth,
                                     static_cast<float>(layout.focusH) / kSceneHeight,
                                     static_cast<float>(layout.outerW) / kSceneWidth,
                                     static_cast<float>(layout.outerH) / kSceneHeight, 0.0f, 0.0f,
                                     look.feather, look.outline, look.corner, 0.0f,
                                     look.color[0], look.color[1], look.color[2], look.color[3]};
        c->UpdateSubresource(focusBoxCb_.Get(), 0, nullptr, constants, 0, 0);
        ID3D11Buffer* focusBox = focusBoxCb_.Get();
        c->PSSetConstantBuffers(13, 1, &focusBox);
        c->Draw(3, 0);
        ID3D11ShaderResourceView* nil = nullptr;
        c->PSSetShaderResources(0, 1, &nil);
        c->CopyResource(resolved, temp_.Get());

        ID3D11RenderTargetView* restoreRtv = oldRtv.Get();
        c->OMSetRenderTargets(1, &restoreRtv, oldDsv.Get());
        c->OMSetBlendState(oldBlend.Get(), bf, mask);
        c->OMSetDepthStencilState(oldDepth.Get(), stencil);
        c->RSSetState(oldRs.Get());
        if (nVp) c->RSSetViewports(nVp, oldVp);
        if (nSc) c->RSSetScissorRects(nSc, oldSc);
        c->IASetInputLayout(oldLayout.Get());
        c->IASetPrimitiveTopology(oldTopo);
        c->VSSetShader(oldVs.Get(), nullptr, 0);
        c->PSSetShader(oldPs.Get(), nullptr, 0);
        c->GSSetShader(oldGs.Get(), nullptr, 0);
        c->HSSetShader(oldHs.Get(), nullptr, 0);
        c->DSSetShader(oldDs.Get(), nullptr, 0);
        ID3D11Buffer* restoreCb = oldPsCb13.Get();
        c->PSSetConstantBuffers(13, 1, &restoreCb);
        ID3D11ShaderResourceView* restoreSrv = oldSrv.Get();
        ID3D11SamplerState* restoreSampler = oldSampler.Get();
        c->PSSetShaderResources(0, 1, &restoreSrv);
        c->PSSetSamplers(0, 1, &restoreSampler);
        replaying = was;
        ++frames_[eye];
        return true;
    }

    uint64_t frames(int eye) const { return eye >= 0 && eye < 2 ? frames_[eye] : 0; }

private:
    bool ensure(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& target) {
        using Microsoft::WRL::ComPtr;
        if (!device) return false;
        if (!vs_ || !ps_) {
            static constexpr char shader[] = R"HLSL(
Texture2D<float4> Atlas : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer FocusBox : register(b13) { float4 FocusLoHi; float4 FocusTile; float4 OuterTile; float4 Look; float4 LookColor; };
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
 float2 texel=float2(1.0/3184.0,1.0/3096.0);
 float2 ouv=clamp(i.uv*OuterTile.xy,texel*0.5,OuterTile.xy-texel*0.5);
 float4 outer=Atlas.SampleLevel(LinearClamp,ouv,0);
 float2 lo=FocusLoHi.xy,hi=FocusLoHi.zw;
 bool inside=all(i.uv>=lo)&&all(i.uv<=hi);
 if(inside){ float2 local=(i.uv-lo)/(hi-lo);
  float e=inside_box(local,(hi-lo)*float2(3184.0,3096.0));
  if(e<0) return outer;
  float w=saturate(e/max(Look.x,1e-4)); w=w*w*(3-2*w);
  float2 uv=float2(FocusTile.x+local.x*FocusTile.z,local.y*FocusTile.w);
  uv=clamp(uv,float2(FocusTile.x,0)+texel*0.5,float2(FocusTile.x+FocusTile.z,FocusTile.w)-texel*0.5);
  float4 color=lerp(outer,Atlas.SampleLevel(LinearClamp,uv,0),w);
  if(e<Look.y) color=LookColor;
  return color; }
 return outer;
})HLSL";
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                               LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
            static CompileFn compile = module ? reinterpret_cast<CompileFn>(GetProcAddress(module, "D3DCompile")) : nullptr;
            if (!compile) { simqvfr::log_line("rf2 outer+focus: d3dcompiler_47.dll / D3DCompile unavailable"); return false; }
            ComPtr<ID3DBlob> vsCode, psCode, errors;
            const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
            HRESULT hr = compile(shader, sizeof(shader)-1, "rf2_outer_focus", nullptr, nullptr, "VS", "vs_5_0", flags, 0, &vsCode, &errors);
            if (FAILED(hr)) { simqvfr::logf("rf2 outer+focus: VS compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown"); return false; }
            errors.Reset();
            hr = compile(shader, sizeof(shader)-1, "rf2_outer_focus", nullptr, nullptr, "PS", "ps_5_0", flags, 0, &psCode, &errors);
            if (FAILED(hr)) { simqvfr::logf("rf2 outer+focus: PS compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown"); return false; }
            if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)) ||
                FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_))) return false;
            const D3D11_BUFFER_DESC cbd = {80, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (FAILED(device->CreateBuffer(&cbd, nullptr, &focusBoxCb_))) return false;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &sampler_))) return false;
        }
        if (!temp_ || !source_ || format_ != target.Format) {
            D3D11_TEXTURE2D_DESC td = target;
            td.MipLevels=1; td.ArraySize=1; td.SampleDesc.Count=1; td.SampleDesc.Quality=0;
            td.Usage=D3D11_USAGE_DEFAULT; td.CPUAccessFlags=0; td.MiscFlags=0;
            temp_.Reset(); tempRtv_.Reset(); source_.Reset(); sourceSrv_.Reset();
            td.BindFlags=D3D11_BIND_RENDER_TARGET;
            if (FAILED(device->CreateTexture2D(&td,nullptr,&temp_)) || FAILED(device->CreateRenderTargetView(temp_.Get(),nullptr,&tempRtv_))) return false;
            td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&td,nullptr,&source_)) || FAILED(device->CreateShaderResourceView(source_.Get(),nullptr,&sourceSrv_))) return false;
            format_=target.Format;
        }
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> focusBoxCb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> tempRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceSrv_;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
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

#include "d3d11_view_resample.h"

#include <cstring>

#include <d3dcompiler.h>

#include "../log.h"

namespace simqvfr {
namespace {

constexpr char kShader[] = R"(
cbuffer Mapping : register(b0) {
    float4 focusTan;  // left, right, up, down
    float4 eyeTan;
    float4 rot0, rot1, rot2;  // rows: the focus view's frame -> the eye's
    float4 dstRegion;  // x, y, width, height (texels)
    float4 srcRegion;
    float4 srcInvSize;  // 1 / the source texture's width, height
};
Texture2D<float4> src : register(t0);
SamplerState linearClamp : register(s0);
RWTexture2D<float4> dst : register(u0);

// The eye's value in the direction of the focus view's point f (0-1 across its region).
float4 eye_value(float2 f) {
    const float3 a = float3(lerp(focusTan.x, focusTan.y, f.x), lerp(focusTan.z, focusTan.w, f.y), -1);
    const float3 b = float3(dot(rot0.xyz, a), dot(rot1.xyz, a), dot(rot2.xyz, a));
    const float2 t = b.xy / max(-b.z, 1e-6);
    const float2 o = float2((t.x - eyeTan.x) / (eyeTan.y - eyeTan.x), (eyeTan.z - t.y) / (eyeTan.z - eyeTan.w));
    // Kept half a texel inside the eye's region, so the bilinear taps never reach a neighbouring view.
    const float2 p = srcRegion.xy + clamp(o * srcRegion.zw, 0.5, srcRegion.zw - 0.5);
    return src.SampleLevel(linearClamp, p * srcInvSize.xy, 0);
}

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)dstRegion.z || id.y >= (uint)dstRegion.w)
        return;
    dst[uint2(dstRegion.xy) + id.xy] = eye_value((float2(id.xy) + 0.5) / dstRegion.zw);
}

float4 VS(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target {
    return eye_value((pos.xy - dstRegion.xy) / dstRegion.zw);
}
)";

constexpr unsigned kConstants = 32;  // floats

DXGI_FORMAT typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    default:
        return f;
    }
}

}  // namespace

bool D3D11ViewResample::initialize(ID3D11Device* device) {
    shutdown();
    Com<ID3DBlob> cs, vs, ps, errors;
    const auto compile = [&](const char* entry, const char* target, Com<ID3DBlob>& out) {
        errors.Reset();
        if (SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "view-resample", nullptr, nullptr, entry, target, 0, 0, &out,
                                 &errors)))
            return true;
        logf("view resample: %s compile failed: %s", entry,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    };
    if (!compile("CS", "cs_5_0", cs) || !compile("VS", "vs_5_0", vs) || !compile("PS", "ps_5_0", ps))
        return false;
    D3D11_BUFFER_DESC cb{kConstants * sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                         D3D11_CPU_ACCESS_WRITE};
    D3D11_SAMPLER_DESC sampler{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                               D3D11_TEXTURE_ADDRESS_CLAMP};
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    raster.DepthClipEnable = TRUE;
    if (FAILED(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &cs_)) ||
        FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_)) ||
        FAILED(device->CreateRasterizerState(&raster, &raster_)) ||
        FAILED(device->CreateBuffer(&cb, nullptr, &constants_)) || FAILED(device->CreateSamplerState(&sampler, &sampler_)) ||
        FAILED(device->CreateDeferredContext(0, &deferred_))) {
        log_line("view resample: making its shaders, states, buffer or deferred context failed");
        shutdown();
        return false;
    }
    device_ = device;
    device->GetImmediateContext(&immediate_);
    ready_ = true;
    return true;
}

ID3D11ShaderResourceView* D3D11ViewResample::source(ID3D11Texture2D* texture) {
    auto& view = sources_[texture];
    if (!view) {
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{typed(d.Format), D3D11_SRV_DIMENSION_TEXTURE2D};
        v.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(texture, &v, &view)))
            view.Reset();
    }
    return view.Get();
}

ID3D11UnorderedAccessView* D3D11ViewResample::target(ID3D11Texture2D* texture) {
    auto& view = targets_[texture];
    if (!view) {
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        D3D11_UNORDERED_ACCESS_VIEW_DESC v{typed(d.Format), D3D11_UAV_DIMENSION_TEXTURE2D};
        if (FAILED(device_->CreateUnorderedAccessView(texture, &v, &view)))
            view.Reset();
    }
    return view.Get();
}

ID3D11RenderTargetView* D3D11ViewResample::render_target(ID3D11Texture2D* texture) {
    auto& view = renderTargets_[texture];
    if (!view) {
        D3D11_TEXTURE2D_DESC d;
        texture->GetDesc(&d);
        D3D11_RENDER_TARGET_VIEW_DESC v{typed(d.Format), D3D11_RTV_DIMENSION_TEXTURE2D};
        if (FAILED(device_->CreateRenderTargetView(texture, &v, &view)))
            view.Reset();
    }
    return view.Get();
}

// A texture holding a copy of `src`'s region (the render-target path reads its own destination's texture).
ID3D11Texture2D* D3D11ViewResample::scratch(ID3D11Texture2D* src, const ViewRegion& region) {
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC have{};
    if (scratch_)
        scratch_->GetDesc(&have);
    if (!scratch_ || have.Format != d.Format || have.Width < UINT(region.width) || have.Height < UINT(region.height)) {
        scratch_.Reset();
        scratchView_.Reset();
        D3D11_TEXTURE2D_DESC s{UINT(region.width), UINT(region.height), 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE};
        if (FAILED(device_->CreateTexture2D(&s, nullptr, &scratch_)))
            return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{typed(d.Format), D3D11_SRV_DIMENSION_TEXTURE2D};
        v.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(scratch_.Get(), &v, &scratchView_))) {
            scratch_.Reset();
            return nullptr;
        }
    }
    return scratch_.Get();
}

bool D3D11ViewResample::constants(const ViewMapping& m, const ViewRegion& srcRegion, const ViewRegion& dstRegion,
                                 UINT srcWidth, UINT srcHeight) {
    float c[kConstants] = {};
    std::memcpy(c, m.focusTan, sizeof(m.focusTan));
    std::memcpy(c + 4, m.eyeTan, sizeof(m.eyeTan));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            c[8 + row * 4 + col] = m.rotation[row * 3 + col];
    const float dr[4] = {float(dstRegion.x), float(dstRegion.y), float(dstRegion.width), float(dstRegion.height)};
    const float sr[4] = {float(srcRegion.x), float(srcRegion.y), float(srcRegion.width), float(srcRegion.height)};
    std::memcpy(c + 20, dr, sizeof(dr));
    std::memcpy(c + 24, sr, sizeof(sr));
    c[28] = 1.0f / float(srcWidth);
    c[29] = 1.0f / float(srcHeight);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(deferred_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, c, sizeof(c));
    deferred_->Unmap(constants_.Get(), 0);
    return true;
}

bool D3D11ViewResample::execute() {
    Com<ID3D11CommandList> list;
    if (FAILED(deferred_->FinishCommandList(FALSE, &list)))
        return false;
    immediate_->ExecuteCommandList(list.Get(), TRUE);  // the game's state as it was
    return true;
}

static bool regions_ok(const ViewRegion& a, const ViewRegion& b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0;
}

bool D3D11ViewResample::resample(ID3D11Texture2D* src, const ViewRegion& srcRegion, ID3D11Texture2D* dst,
                                const ViewRegion& dstRegion, const ViewMapping& m) {
    if (!ready_ || !src || !dst || src == dst || !regions_ok(srcRegion, dstRegion))
        return false;
    // The textures are the game's and may come and go (pooled render targets): a bounded set of views.
    if (sources_.size() + targets_.size() + renderTargets_.size() > 32)
        forget_views();
    ID3D11ShaderResourceView* srv = source(src);
    ID3D11UnorderedAccessView* uav = target(dst);
    if (!srv || !uav)
        return false;
    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    if (!constants(m, srcRegion, dstRegion, sd.Width, sd.Height))
        return false;
    deferred_->CSSetShader(cs_.Get(), nullptr, 0);
    deferred_->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    deferred_->CSSetShaderResources(0, 1, &srv);
    deferred_->CSSetSamplers(0, 1, sampler_.GetAddressOf());
    deferred_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    deferred_->Dispatch((dstRegion.width + 7) / 8, (dstRegion.height + 7) / 8, 1);
    return execute();
}

void D3D11ViewResample::draw(ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv, const ViewRegion& dstRegion) {
    const D3D11_VIEWPORT vp{float(dstRegion.x), float(dstRegion.y), float(dstRegion.width), float(dstRegion.height), 0, 1};
    deferred_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred_->VSSetShader(vs_.Get(), nullptr, 0);
    deferred_->PSSetShader(ps_.Get(), nullptr, 0);
    deferred_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    deferred_->PSSetShaderResources(0, 1, &srv);
    deferred_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    deferred_->RSSetState(raster_.Get());
    deferred_->RSSetViewports(1, &vp);
    deferred_->OMSetRenderTargets(1, &rtv, nullptr);
    deferred_->Draw(3, 0);
}

bool D3D11ViewResample::resample_target(ID3D11Texture2D* src, const ViewRegion& srcRegion, ID3D11Texture2D* dst,
                                       const ViewRegion& dstRegion, const ViewMapping& m) {
    if (!ready_ || !src || !dst || !regions_ok(srcRegion, dstRegion))
        return false;
    if (sources_.size() + targets_.size() + renderTargets_.size() > 32)
        forget_views();
    ID3D11RenderTargetView* rtv = render_target(dst);
    if (!rtv)
        return false;
    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    // Another texture the shaders can read: straight from it (the one scratch texture would be re-made at every switch
    // of format, AO's R8 and bloom's R11G11B10 in one ACC frame). LMU's eye glare copies have no shader binding.
    if (src != dst && (sd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        ID3D11ShaderResourceView* srv = source(src);
        if (!srv || !constants(m, srcRegion, dstRegion, sd.Width, sd.Height))
            return false;
        draw(srv, rtv, dstRegion);
        return execute();
    }
    ID3D11Texture2D* copy = scratch(src, srcRegion);
    if (!copy)
        return false;
    D3D11_TEXTURE2D_DESC cd;
    copy->GetDesc(&cd);
    const ViewRegion inCopy{0, 0, srcRegion.width, srcRegion.height};
    if (!constants(m, inCopy, dstRegion, cd.Width, cd.Height))
        return false;
    const D3D11_BOX box{UINT(srcRegion.x), UINT(srcRegion.y), 0, UINT(srcRegion.x + srcRegion.width),
                        UINT(srcRegion.y + srcRegion.height), 1};
    deferred_->CopySubresourceRegion(copy, 0, 0, 0, 0, src, 0, &box);
    draw(scratchView_.Get(), rtv, dstRegion);
    return execute();
}

bool D3D11ViewResample::resample_target(ID3D11ShaderResourceView* src, unsigned srcWidth, unsigned srcHeight,
                                       const ViewRegion& srcRegion, ID3D11Texture2D* dst, const ViewRegion& dstRegion,
                                       const ViewMapping& m) {
    if (!ready_ || !src || !dst || !srcWidth || !srcHeight || !regions_ok(srcRegion, dstRegion))
        return false;
    if (sources_.size() + targets_.size() + renderTargets_.size() > 32)
        forget_views();
    ID3D11RenderTargetView* rtv = render_target(dst);
    if (!rtv || !constants(m, srcRegion, dstRegion, srcWidth, srcHeight))
        return false;
    draw(src, rtv, dstRegion);
    return execute();
}

void D3D11ViewResample::forget_views() {
    sources_.clear();
    targets_.clear();
    renderTargets_.clear();
}

void D3D11ViewResample::shutdown() {
    forget_views();
    scratchView_.Reset();
    scratch_.Reset();
    sampler_.Reset();
    constants_.Reset();
    raster_.Reset();
    ps_.Reset();
    vs_.Reset();
    cs_.Reset();
    deferred_.Reset();
    immediate_.Reset();
    device_.Reset();
    ready_ = false;
}

}  // namespace simqvfr

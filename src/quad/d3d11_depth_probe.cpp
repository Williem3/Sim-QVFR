#include "d3d11_depth_probe.h"

#include <cstring>

#include <d3dcompiler.h>

#include "../log.h"

namespace simqvfr {
namespace {

// One thread per point: the depth at its texel (clamped to the texture), into the output buffer.
constexpr char kShader[] = R"(
Texture2D<float> depthTexture : register(t0);
RWBuffer<float> values : register(u0);
cbuffer Points : register(b0) { int4 points[9]; };
[numthreads(16, 1, 1)]
void CS(uint i : SV_DispatchThreadID) {
    if (i >= 9) return;
    uint w, h;
    depthTexture.GetDimensions(w, h);
    int2 p = clamp(points[i].xy, int2(0, 0), int2(int(w) - 1, int(h) - 1));
    values[i] = depthTexture.Load(int3(p, 0));
}
)";

// The shader-readable format of a depth texture's (typeless) format: its depth channel as a float.
DXGI_FORMAT depth_view_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace

bool D3D11DepthProbe::initialize(ID3D11Device* device) {
    shutdown();
    if (!device)
        return false;
    Com<ID3DBlob> cs, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "depth-probe", nullptr, nullptr, "CS", "cs_5_0", 0, 0, &cs,
                          &errors))) {
        logf("depth probe: compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    D3D11_BUFFER_DESC cb{16 * kPoints, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    D3D11_BUFFER_DESC out{16 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 16;
    D3D11_BUFFER_DESC staging{16 * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    bool ok = SUCCEEDED(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &cs_)) &&
              SUCCEEDED(device->CreateBuffer(&cb, nullptr, &constants_)) &&
              SUCCEEDED(device->CreateBuffer(&out, nullptr, &output_)) &&
              SUCCEEDED(device->CreateUnorderedAccessView(output_.Get(), &uav, &outputView_)) &&
              SUCCEEDED(device->CreateDeferredContext(0, &deferred_));
    for (int i = 0; ok && i < kRing; ++i)
        ok = SUCCEEDED(device->CreateBuffer(&staging, nullptr, &staging_[i]));
    if (!ok) {
        log_line("depth probe: making its shader, buffers or deferred context failed");
        shutdown();
        return false;
    }
    device_ = device;
    device->GetImmediateContext(&immediate_);
    ready_ = true;
    return true;
}

ID3D11ShaderResourceView* D3D11DepthProbe::view_of(ID3D11Texture2D* depth) {
    if (depth == viewTexture_ && view_)
        return view_.Get();
    view_.Reset();
    viewTexture_ = depth;
    D3D11_TEXTURE2D_DESC d;
    depth->GetDesc(&d);
    const DXGI_FORMAT f = depth_view_format(d.Format);
    if (f == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        if (!refusedLogged_)
            logf("depth probe: the depth texture (%ux%u, format %d, %u samples, bind 0x%x) can't be read by a shader",
                 d.Width, d.Height, static_cast<int>(d.Format), d.SampleDesc.Count, d.BindFlags);
        refusedLogged_ = true;
        return nullptr;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = f;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(depth, &srv, &view_))) {
        if (!refusedLogged_)
            logf("depth probe: no shader view of the depth texture (format %d)", static_cast<int>(d.Format));
        refusedLogged_ = true;
        return nullptr;
    }
    return view_.Get();
}

bool D3D11DepthProbe::request(ID3D11Texture2D* depth, const int (&points)[kPoints][2]) {
    if (!ready_ || !depth)
        return false;
    ID3D11ShaderResourceView* srv = view_of(depth);
    if (!srv)
        return false;
    const int slot = static_cast<int>(requests_ % kRing);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(deferred_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return false;
    int data[kPoints][4] = {};
    for (int i = 0; i < kPoints; ++i) {
        data[i][0] = points[i][0];
        data[i][1] = points[i][1];
    }
    std::memcpy(m.pData, data, sizeof(data));
    deferred_->Unmap(constants_.Get(), 0);
    ID3D11Buffer* cb = constants_.Get();
    ID3D11UnorderedAccessView* uav = outputView_.Get();
    deferred_->CSSetShader(cs_.Get(), nullptr, 0);
    deferred_->CSSetConstantBuffers(0, 1, &cb);
    deferred_->CSSetShaderResources(0, 1, &srv);
    deferred_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    deferred_->Dispatch(1, 1, 1);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;
    deferred_->CSSetShaderResources(0, 1, &noSrv);
    deferred_->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    deferred_->CopyResource(staging_[slot].Get(), output_.Get());
    Com<ID3D11CommandList> list;
    if (FAILED(deferred_->FinishCommandList(FALSE, &list)))
        return false;
    immediate_->ExecuteCommandList(list.Get(), TRUE);  // the game's state as it was
    queuedAt_[slot] = ++requests_;
    return true;
}

bool D3D11DepthProbe::latest(float (&values)[kPoints], int& age) {
    if (!ready_)
        return false;
    // The newest queued read the GPU has finished, newer than the last one handed out.
    for (std::uint64_t n = requests_; n > read_ && n + kRing > requests_; --n) {
        const int slot = static_cast<int>((n - 1) % kRing);
        if (queuedAt_[slot] != n)
            continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (immediate_->Map(staging_[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK)
            continue;  // not yet (DXGI_ERROR_WAS_STILL_DRAWING): an older one may be
        std::memcpy(values, m.pData, sizeof(values));
        immediate_->Unmap(staging_[slot].Get(), 0);
        age = static_cast<int>(requests_ - n);
        read_ = n;
        return true;
    }
    return false;
}

void D3D11DepthProbe::shutdown() {
    ready_ = false;
    view_.Reset();
    viewTexture_ = nullptr;
    for (auto& s : staging_)
        s.Reset();
    outputView_.Reset();
    output_.Reset();
    constants_.Reset();
    cs_.Reset();
    deferred_.Reset();
    immediate_.Reset();
    device_.Reset();
    requests_ = read_ = 0;
    for (auto& q : queuedAt_)
        q = 0;
    refusedLogged_ = false;
}

}  // namespace simqvfr

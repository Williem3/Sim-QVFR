// The focus corridor's shaders (games/lmu/corridor_shader.h; [experiment] lmu_corridor_ab, lmu_corridor_show) on a WARP
// device, on scenes ray-cast here in LMU's depth convention (reversed: depth = a + b / z): the copy takes the eye's
// depth over the box from where the box sits in the target; the road pass (on a grid of half the copy each way) finds
// the road beneath every texel and the road's centre; the mask keeps the corridor and gives every other focus texel the viewport's single depth. Scenes: a
// flat road between tree walls 15 m out (the walls, the sky and a near dash never kept); the road climbing a 12 degree
// hill 40 m ahead with a wall across it at 150 m (the hill's road kept all the way up, which a level corridor loses
// past ~57 m; the wall above 3.5 m not); the car beside a wall 4 m to its right with the road to its left (the corridor
// moves left to it: road 14 m left kept). Then the fill on the flat road: the eye's colour and depth into the masked
// texels, the focus view kept deep inside the corridor, a blend within the feather of its edge.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "games/lmu/corridor_shader.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kA = -1.29938e-05f, kB = 0.130002f;  // LMU's depth terms (the capture's projection)
constexpr float kL = -0.45f, kR = 0.45f, kU = 0.30f, kD = -0.35f;  // the focus box's tangents
constexpr float kEyeHeight = 1.3f, kHalf = 10, kTall = 3.5f, kStart = 3, kReach = 6, kDash = 0.8f;
constexpr float kDashBelow = -0.25f;  // the dash fills the view below this tangent
constexpr float kFeather = 1;         // the fill's blend at the corridor's edge (m)
constexpr UINT kCW = 160, kCH = 120;  // the copy (the eye's box region)
constexpr UINT kFW = 320, kFH = 240;  // the focus viewport
constexpr UINT kGW = (kCW + 1) / 2, kGH = (kCH + 1) / 2;  // the road grid: half the copy each way

// What a ray hit: 0 sky, 1 ground, 2 a wall (vertical), 3 the dash. Its point: x right, y up from the eye, z ahead.
struct Hit {
    int what = 0;
    float x = 0, y = 0, z = 1e30f;
};

// A scene: the ground's height above the car's road plane at (x, z), and its walls (x = const or z = const planes,
// each with its extent and height above the ground there).
struct Wall {
    bool across;  // a z = const wall (across the view), else x = const
    float at, from, to, height;
};
struct Scene {
    std::function<float(float, float)> ground;
    std::vector<Wall> walls;
};

Hit cast(const Scene& s, float tx, float ty) {
    Hit h;
    if (ty < kDashBelow) {
        h.what = 3;
        h.z = kDash;
        h.x = tx * kDash;
        h.y = ty * kDash;
        return h;
    }
    // The ground: march out, then bisect.
    float prev = 0, z = 0.5f;
    const auto above = [&](float zz) { return ty * zz - (s.ground(tx * zz, zz) - kEyeHeight); };
    while (z < 2000) {
        if (above(z) < 0) {
            float lo = prev, hi = z;
            for (int i = 0; i < 40; ++i) {
                const float mid = 0.5f * (lo + hi);
                (above(mid) < 0 ? hi : lo) = mid;
            }
            h = {1, tx * hi, ty * hi, hi};
            break;
        }
        prev = z;
        z += (std::max)(0.05f, z * 0.01f);
    }
    for (const Wall& w : s.walls) {
        float zz;
        if (w.across) {
            zz = w.at;
        } else {
            if (tx == 0 || (w.at > 0) != (tx > 0))
                continue;
            zz = w.at / tx;
        }
        if (zz <= 0 || zz >= h.z)
            continue;
        const float x = tx * zz, y = ty * zz;
        const float along = w.across ? x : zz;
        const float base = s.ground(x, zz) - kEyeHeight;
        if (along >= w.from && along <= w.to && y >= base && y <= base + w.height)
            h = {2, x, y, zz};
    }
    return h;
}

float depth_of(float z) { return z > 1e29f ? 0.0f : kA + kB / z; }

template <typename T>
bool readback(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* resource, std::vector<T>& out,
              size_t count, UINT width) {
    ComPtr<ID3D11Resource> staging;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11Buffer> buf;
    UINT rows = 1;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        rows = d.Height;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> s;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &s)))
            return false;
        staging = s;
    } else if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buf)))) {
        D3D11_BUFFER_DESC d;
        buf->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        d.StructureByteStride = 0;
        ComPtr<ID3D11Buffer> s;
        if (FAILED(device->CreateBuffer(&d, nullptr, &s)))
            return false;
        staging = s;
    } else {
        return false;
    }
    context->CopyResource(staging.Get(), resource);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;
    out.resize(count);
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * width], static_cast<const char*>(m.pData) + y * m.RowPitch,
                    width * sizeof(T));
    context->Unmap(staging.Get(), 0);
    return true;
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> copyPs, maskPs, fillOutsidePs, fillEdgePs;
    ComPtr<ID3D11BlendState> fillBlend, fillOpaque;
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11ComputeShader> classify, columns, centre, margin;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> always;
};

// The fill's results: each focus texel's colour (RGBA) and depth after it, the eye's colour green, the focus view red.
struct Filled {
    std::vector<float> colour, depth;
};

// The scene through the pipeline: the road pass (centre smoothing 1: at once) on its depth as the copy, then the mask,
// then with `filled` the fill. Out: each focus texel masked or kept, what it shows, and the road's centre.
// The corridor's rows as margin_main gathered them (the band DLSS takes) and as the margin map holds them: [top, bottom).
// The car kept sharp (corridor_shader.h fill.x): whatever lies nearer than this, metres; 0 none.
float gCarM = 0;
// The corridor's widening with distance (corridor_shader.h focus.w): the tangent of its angle either side; 0 straight.
float gSpreadTan = 0;
// The corridor edge's blend growing with distance (corridor_shader.h ground.w): metres more a metre away.
float gFeatherGrowth = 0;
// Show Road focus area (corridor_shader.h view.x): the fill's eye colour tinted.
float gCoverage = 0;
// Past a crest, the steepest grade the road may fall by out of sight (corridor_shader.h view.y): the game's default.
float gDropTan = 0.15f;

struct Rows {
    unsigned gpuTop = 0, gpuBottom = 0, mapTop = 0, mapBottom = 0;
    unsigned blend = 0;  // the map's texels within the edge's blend (0 < margin < 1 blend width)
    bool read = false;
} lastRows;

bool run_scene(Gpu& g, const Scene& scene, std::vector<bool>& masked, float& centre, std::vector<Hit>& hits,
               Filled* filled = nullptr) {
    std::vector<float> copy(kCW * kCH);
    for (UINT y = 0; y < kCH; ++y)
        for (UINT x = 0; x < kCW; ++x) {
            const float tx = kL + (kR - kL) * (x + 0.5f) / kCW, ty = kU + (kD - kU) * (y + 0.5f) / kCH;
            copy[y * kCW + x] = depth_of(cast(scene, tx, ty).z);
        }
    ID3D11Device* d = g.device.Get();
    ID3D11DeviceContext* c = g.context.Get();
    const D3D11_TEXTURE2D_DESC copyDesc = {kCW, kCH, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                           D3D11_BIND_SHADER_RESOURCE, 0, 0};
    const D3D11_SUBRESOURCE_DATA copyData = {copy.data(), kCW * sizeof(float), 0};
    ComPtr<ID3D11Texture2D> copyTex, ground, road;
    ComPtr<ID3D11ShaderResourceView> copySrv, groundSrv, centreSrv, roadSrv;
    ComPtr<ID3D11UnorderedAccessView> groundUav, sumsUav, centreUav, roadUav, marginUav;
    ComPtr<ID3D11Texture2D> marginTex;
    ComPtr<ID3D11ShaderResourceView> marginSrv;
    const D3D11_TEXTURE2D_DESC md = {kCW, kCH, 1, 1, DXGI_FORMAT_R16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ComPtr<ID3D11Buffer> sums, centreBuf;
    const D3D11_TEXTURE2D_DESC gd = {kGW, kGH, 1, 1, DXGI_FORMAT_R32G32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    const D3D11_TEXTURE2D_DESC rd = {kGW, kGH, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    const D3D11_BUFFER_DESC sd = {kGW * 2 * sizeof(float), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                  D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 2 * sizeof(float)};
    const D3D11_BUFFER_DESC cd = {sizeof(float), D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                                  D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(float)};
    const float zero = 0;
    const D3D11_SUBRESOURCE_DATA zeroData = {&zero, 0, 0};
    if (FAILED(d->CreateTexture2D(&copyDesc, &copyData, &copyTex)) ||
        FAILED(d->CreateShaderResourceView(copyTex.Get(), nullptr, &copySrv)) ||
        FAILED(d->CreateTexture2D(&gd, nullptr, &ground)) ||
        FAILED(d->CreateUnorderedAccessView(ground.Get(), nullptr, &groundUav)) ||
        FAILED(d->CreateShaderResourceView(ground.Get(), nullptr, &groundSrv)) ||
        FAILED(d->CreateTexture2D(&rd, nullptr, &road)) || FAILED(d->CreateUnorderedAccessView(road.Get(), nullptr, &roadUav)) ||
        FAILED(d->CreateShaderResourceView(road.Get(), nullptr, &roadSrv)) ||
        FAILED(d->CreateTexture2D(&md, nullptr, &marginTex)) ||
        FAILED(d->CreateUnorderedAccessView(marginTex.Get(), nullptr, &marginUav)) ||
        FAILED(d->CreateShaderResourceView(marginTex.Get(), nullptr, &marginSrv)) ||
        FAILED(d->CreateBuffer(&sd, nullptr, &sums)) || FAILED(d->CreateUnorderedAccessView(sums.Get(), nullptr, &sumsUav)) ||
        FAILED(d->CreateBuffer(&cd, &zeroData, &centreBuf)) ||
        FAILED(d->CreateUnorderedAccessView(centreBuf.Get(), nullptr, &centreUav)) ||
        FAILED(d->CreateShaderResourceView(centreBuf.Get(), nullptr, &centreSrv)))
        return false;
    float k[40] = {0, 0, kCW, kCH, kL, kR, kU, kD, kA, kB, kStart, kTall, 0, 1, 0, kEyeHeight,
                   kHalf, 0, kCW, kCH, std::cos(35.0f * 3.14159265f / 180), 1.0f, kReach, gFeatherGrowth,
                   gCarM, kFeather, kFW, kFH, 0, 0, 0, gSpreadTan, 0, 0, 0, 0, gCoverage, gDropTan, 0, 0};
    const uint32_t stamp = 7;  // the mask's stamp this pass (the fill finds its texels by it)
    std::memcpy(&k[30], &stamp, sizeof(stamp));
    c->UpdateSubresource(g.cb.Get(), 0, nullptr, k, 0, 0);
    // The road pass: every grid texel classified, then the columns, then the centre.
    c->CSSetShader(g.classify.Get(), nullptr, 0);
    c->CSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    c->CSSetShaderResources(0, 1, copySrv.GetAddressOf());
    c->CSSetUnorderedAccessViews(3, 1, roadUav.GetAddressOf(), nullptr);
    c->Dispatch((kGW + 7) / 8, (kGH + 7) / 8, 1);
    ID3D11UnorderedAccessView* noUav = nullptr;
    c->CSSetUnorderedAccessViews(3, 1, &noUav, nullptr);
    c->CSSetShader(g.columns.Get(), nullptr, 0);
    c->CSSetShaderResources(3, 1, roadSrv.GetAddressOf());
    ID3D11UnorderedAccessView* uavs[3] = {groundUav.Get(), sumsUav.Get(), centreUav.Get()};
    c->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    c->Dispatch((kGW + 63) / 64, 1, 1);
    c->CSSetShader(g.centre.Get(), nullptr, 0);
    c->Dispatch(1, 1, 1);
    // The margin map, from the road's and the centre's results.
    ID3D11UnorderedAccessView* noUavs3[3] = {};
    c->CSSetUnorderedAccessViews(0, 3, noUavs3, nullptr);
    ID3D11ShaderResourceView* marginIn[3] = {copySrv.Get(), groundSrv.Get(), centreSrv.Get()};
    c->CSSetShaderResources(0, 3, marginIn);
    // With the rows' buffer (raw, cleared to 0).
    const D3D11_BUFFER_DESC rowsDesc = {8, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    D3D11_UNORDERED_ACCESS_VIEW_DESC rowsView{};
    rowsView.Format = DXGI_FORMAT_R32_TYPELESS;
    rowsView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    rowsView.Buffer.NumElements = 2;
    rowsView.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11Buffer> rowsBuf;
    ComPtr<ID3D11UnorderedAccessView> rowsUav;
    if (FAILED(d->CreateBuffer(&rowsDesc, nullptr, &rowsBuf)) ||
        FAILED(d->CreateUnorderedAccessView(rowsBuf.Get(), &rowsView, &rowsUav)))
        return false;
    const UINT zero4[4] = {};
    c->ClearUnorderedAccessViewUint(rowsUav.Get(), zero4);
    ID3D11UnorderedAccessView* marginUavs[2] = {marginUav.Get(), rowsUav.Get()};
    c->CSSetUnorderedAccessViews(4, 2, marginUavs, nullptr);
    c->CSSetShader(g.margin.Get(), nullptr, 0);
    c->Dispatch((kCW + 7) / 8, (kCH + 7) / 8, 1);
    ID3D11UnorderedAccessView* noUav4[2] = {};
    c->CSSetUnorderedAccessViews(4, 2, noUav4, nullptr);
    {
        std::vector<uint32_t> rowsOut;
        std::vector<uint16_t> map;
        lastRows = {};
        if (readback(d, c, rowsBuf.Get(), rowsOut, 2, 2) && readback(d, c, marginTex.Get(), map, kCW * kCH, kCW)) {
            lastRows.read = true;
            lastRows.gpuTop = rowsOut[1] ? kCH - rowsOut[0] : 0;
            lastRows.gpuBottom = rowsOut[1];
            unsigned top = kCH, bottom = 0;
            for (UINT y = 0; y < kCH; ++y)
                for (UINT x = 0; x < kCW; ++x) {
                    const uint16_t h = map[y * kCW + x];
                    if (!(h & 0x8000) && (h & 0x7fff)) {  // above 0 (a half's sign and magnitude)
                        top = (std::min)(top, static_cast<unsigned>(y));
                        bottom = (std::max)(bottom, static_cast<unsigned>(y) + 1);
                        lastRows.blend += (h & 0x7fff) < 0x3c00 ? 1 : 0;  // below 1.0
                    }
                }
            lastRows.mapTop = bottom ? top : 0;
            lastRows.mapBottom = bottom;
        }
    }
    ID3D11UnorderedAccessView* noUavs[3] = {};
    c->CSSetUnorderedAccessViews(0, 3, noUavs, nullptr);
    std::vector<float> centreOut;
    if (!readback(d, c, centreBuf.Get(), centreOut, 1, 1))
        return false;
    centre = centreOut[0];
    // The mask over a reversed depth target cleared far (0).
    const D3D11_TEXTURE2D_DESC depthDesc = {kFW, kFH, 1, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_DEPTH_STENCIL, 0, 0};
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(d->CreateTexture2D(&depthDesc, nullptr, &depthTex)) ||
        FAILED(d->CreateDepthStencilView(depthTex.Get(), &dsvDesc, &dsv)))
        return false;
    c->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    const D3D11_TEXTURE2D_DESC stampDesc = {kFW, kFH, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ComPtr<ID3D11Texture2D> stampTex;
    ComPtr<ID3D11UnorderedAccessView> stampUav;
    ComPtr<ID3D11ShaderResourceView> stampSrv;
    if (FAILED(d->CreateTexture2D(&stampDesc, nullptr, &stampTex)) ||
        FAILED(d->CreateUnorderedAccessView(stampTex.Get(), nullptr, &stampUav)) ||
        FAILED(d->CreateShaderResourceView(stampTex.Get(), nullptr, &stampSrv)))
        return false;
    const D3D11_VIEWPORT vp{0, 0, kFW, kFH, 1, 1};  // the nearest (reversed): one depth
    c->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, dsv.Get(), 0, 1, stampUav.GetAddressOf(), nullptr);
    c->OMSetDepthStencilState(g.always.Get(), 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.vs.Get(), nullptr, 0);
    c->PSSetShader(g.maskPs.Get(), nullptr, 0);
    c->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    ID3D11ShaderResourceView* srvs[7] = {copySrv.Get(), groundSrv.Get(), centreSrv.Get(), nullptr, nullptr, nullptr,
                                         marginSrv.Get()};
    c->PSSetShaderResources(0, 7, srvs);
    c->PSSetSamplers(0, 1, g.linear.GetAddressOf());
    c->RSSetState(g.raster.Get());
    c->RSSetViewports(1, &vp);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {};
    c->PSSetShaderResources(0, 3, none);
    std::vector<float> depth;
    if (!readback(d, c, depthTex.Get(), depth, kFW * kFH, kFW))
        return false;
    if (filled) {
        // The eye's colour green, the focus view red; the mask's stamps as it left them.
        c->OMSetRenderTargets(0, nullptr, nullptr);  // the stamps' UAV unbound before they're read
        std::vector<float> green(kCW * kCH * 4), red(kFW * kFH * 4);
        for (size_t i = 0; i < green.size(); i += 4) {
            green[i + 1] = 1;
            green[i + 3] = 1;
        }
        for (size_t i = 0; i < red.size(); i += 4) {
            red[i] = 1;
            red[i + 3] = 1;
        }
        const D3D11_TEXTURE2D_DESC eyeDesc = {kCW, kCH, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                              D3D11_BIND_SHADER_RESOURCE, 0, 0};
        const D3D11_SUBRESOURCE_DATA eyeData = {green.data(), kCW * 4 * sizeof(float), 0};
        const D3D11_TEXTURE2D_DESC focusDesc = {kFW, kFH, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                                D3D11_BIND_RENDER_TARGET, 0, 0};
        const D3D11_SUBRESOURCE_DATA focusData = {red.data(), kFW * 4 * sizeof(float), 0};
        ComPtr<ID3D11Texture2D> eyeTex, focusTex;
        ComPtr<ID3D11ShaderResourceView> eyeSrv;
        ComPtr<ID3D11RenderTargetView> focusRtv;
        if (FAILED(d->CreateTexture2D(&eyeDesc, &eyeData, &eyeTex)) ||
            FAILED(d->CreateShaderResourceView(eyeTex.Get(), nullptr, &eyeSrv)) ||
            FAILED(d->CreateTexture2D(&focusDesc, &focusData, &focusTex)) ||
            FAILED(d->CreateRenderTargetView(focusTex.Get(), nullptr, &focusRtv)))
            return false;
        ID3D11ShaderResourceView* fillSrvs[7] = {copySrv.Get(), groundSrv.Get(), centreSrv.Get(), nullptr, stampSrv.Get(),
                                                 eyeSrv.Get(), marginSrv.Get()};
        c->PSSetShaderResources(0, 7, fillSrvs);
        c->PSSetSamplers(0, 1, g.linear.GetAddressOf());
        const D3D11_VIEWPORT fvp{0, 0, kFW, kFH, 0, 1};
        c->RSSetViewports(1, &fvp);
        c->OMSetRenderTargets(1, focusRtv.GetAddressOf(), dsv.Get());
        c->OMSetDepthStencilState(g.always.Get(), 0);
        c->OMSetBlendState(g.fillOpaque.Get(), nullptr, 0xffffffff);
        c->PSSetShader(g.fillOutsidePs.Get(), nullptr, 0);
        c->Draw(3, 0);
        c->OMSetRenderTargets(1, focusRtv.GetAddressOf(), nullptr);
        c->OMSetBlendState(g.fillBlend.Get(), nullptr, 0xffffffff);
        c->PSSetShader(g.fillEdgePs.Get(), nullptr, 0);
        c->Draw(3, 0);
        c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* none6[6] = {};
        c->PSSetShaderResources(0, 6, none6);
        if (!readback(d, c, focusTex.Get(), filled->colour, kFW * kFH * 4, kFW * 4) ||
            !readback(d, c, depthTex.Get(), filled->depth, kFW * kFH, kFW))
            return false;
    }
    masked.resize(kFW * kFH);
    hits.resize(kFW * kFH);
    for (UINT y = 0; y < kFH; ++y)
        for (UINT x = 0; x < kFW; ++x) {
            masked[y * kFW + x] = depth[y * kFW + x] == 1.0f;
            const float tx = kL + (kR - kL) * (x + 0.5f) / kFW, ty = kU + (kD - kU) * (y + 0.5f) / kFH;
            hits[y * kFW + x] = cast(scene, tx, ty);
        }
    return true;
}

// The share of the texels `which` picks that are kept (not masked); -1 with none picked.
double kept_share(const std::vector<bool>& masked, const std::vector<Hit>& hits, const std::function<bool(const Hit&)>& which) {
    int n = 0, kept = 0;
    for (size_t i = 0; i < hits.size(); ++i)
        if (which(hits[i])) {
            ++n;
            kept += masked[i] ? 0 : 1;
        }
    return n ? static_cast<double>(kept) / n : -1;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    Gpu g;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.device,
                                 nullptr, &g.context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    const auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& code) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(lmu::kCorridorShaders, sizeof(lmu::kCorridorShaders) - 1, "corridor", nullptr,
                                      nullptr, entry, target, 0, 0, &code, &errors);
        if (FAILED(hr) && errors)
            std::printf("%s: %s\n", entry, static_cast<const char*>(errors->GetBufferPointer()));
        return SUCCEEDED(hr);
    };
    ComPtr<ID3DBlob> vsCode, copyCode, copyMsCode, maskCode, showCode, columnsCode, centreCode, classifyCode;
    ComPtr<ID3DBlob> fillOutsideCode, fillEdgeCode, colourMsCode, colourSsCode, marginCode;
    check(compile("vs_main", "vs_5_0", vsCode) && compile("copy_ss", "ps_5_0", copyCode) &&
              compile("copy_ms", "ps_5_0", copyMsCode) && compile("mask_main", "ps_5_0", maskCode) &&
              compile("show_main", "ps_5_0", showCode) && compile("columns_main", "cs_5_0", columnsCode) &&
              compile("centre_main", "cs_5_0", centreCode) && compile("classify_main", "cs_5_0", classifyCode) &&
              compile("margin_main", "cs_5_0", marginCode) &&
              compile("fill_outside_main", "ps_5_0", fillOutsideCode) && compile("fill_edge_main", "ps_5_0", fillEdgeCode) &&
              compile("copy_colour_ms", "ps_5_0", colourMsCode) && compile("copy_colour_ss", "ps_5_0", colourSsCode),
          "the corridor's shaders compile");
    if (failures)
        return test_result();
    ID3D11Device* d = g.device.Get();
    d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g.vs);
    d->CreatePixelShader(copyCode->GetBufferPointer(), copyCode->GetBufferSize(), nullptr, &g.copyPs);
    d->CreatePixelShader(maskCode->GetBufferPointer(), maskCode->GetBufferSize(), nullptr, &g.maskPs);
    d->CreatePixelShader(fillOutsideCode->GetBufferPointer(), fillOutsideCode->GetBufferSize(), nullptr, &g.fillOutsidePs);
    d->CreatePixelShader(fillEdgeCode->GetBufferPointer(), fillEdgeCode->GetBufferSize(), nullptr, &g.fillEdgePs);
    {
        D3D11_BLEND_DESC fb{};
        fb.RenderTarget[0].BlendEnable = TRUE;
        fb.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        fb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        fb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        fb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        fb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        fb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        fb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        d->CreateBlendState(&fb, &g.fillBlend);
        D3D11_BLEND_DESC fo{};
        fo.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        d->CreateBlendState(&fo, &g.fillOpaque);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        d->CreateSamplerState(&sd, &g.linear);
    }
    d->CreateComputeShader(columnsCode->GetBufferPointer(), columnsCode->GetBufferSize(), nullptr, &g.columns);
    d->CreateComputeShader(classifyCode->GetBufferPointer(), classifyCode->GetBufferSize(), nullptr, &g.classify);
    d->CreateComputeShader(marginCode->GetBufferPointer(), marginCode->GetBufferSize(), nullptr, &g.margin);
    d->CreateComputeShader(centreCode->GetBufferPointer(), centreCode->GetBufferSize(), nullptr, &g.centre);
    const D3D11_BUFFER_DESC cbDesc = {40 * sizeof(float), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    d->CreateBuffer(&cbDesc, nullptr, &g.cb);
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    d->CreateRasterizerState(&rs, &g.raster);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d->CreateDepthStencilState(&ds, &g.always);

    // The copy shader: the box region of a target, texel for texel.
    {
        constexpr UINT kTW = 240, kTH = 180, kBX = 37, kBY = 21;
        std::vector<float> eye(kTW * kTH);
        for (UINT i = 0; i < eye.size(); ++i)
            eye[i] = static_cast<float>(i % 977) / 977.0f;
        const D3D11_TEXTURE2D_DESC eyeDesc = {kTW, kTH, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                              D3D11_BIND_SHADER_RESOURCE, 0, 0};
        const D3D11_SUBRESOURCE_DATA eyeData = {eye.data(), kTW * sizeof(float), 0};
        const D3D11_TEXTURE2D_DESC outDesc = {kCW, kCH, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                              D3D11_BIND_RENDER_TARGET, 0, 0};
        ComPtr<ID3D11Texture2D> eyeTex, outTex;
        ComPtr<ID3D11ShaderResourceView> eyeSrv;
        ComPtr<ID3D11RenderTargetView> outRtv;
        d->CreateTexture2D(&eyeDesc, &eyeData, &eyeTex);
        d->CreateShaderResourceView(eyeTex.Get(), nullptr, &eyeSrv);
        d->CreateTexture2D(&outDesc, nullptr, &outTex);
        d->CreateRenderTargetView(outTex.Get(), nullptr, &outRtv);
        float k[40] = {kBX, kBY, kCW, kCH};
        k[18] = kCW;  // the copy's size: the box's own texels (no focus view: fill.z 0)
        k[19] = kCH;
        ID3D11DeviceContext* c = g.context.Get();
        c->UpdateSubresource(g.cb.Get(), 0, nullptr, k, 0, 0);
        const D3D11_VIEWPORT vp{0, 0, kCW, kCH, 0, 1};
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(g.vs.Get(), nullptr, 0);
        c->PSSetShader(g.copyPs.Get(), nullptr, 0);
        c->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
        c->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
        c->PSSetShaderResources(0, 1, eyeSrv.GetAddressOf());
        c->RSSetState(g.raster.Get());
        c->RSSetViewports(1, &vp);
        c->Draw(3, 0);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        std::vector<float> out;
        bool same = readback(d, c, outTex.Get(), out, kCW * kCH, kCW);
        for (UINT y = 0; same && y < kCH; ++y)
            for (UINT x = 0; same && x < kCW; ++x)
                same = out[y * kCW + x] == eye[(kBY + y) * kTW + kBX + x];
        check(same, "copy: the box region of the eye's depth, texel for texel, from its corner in the target");
        // With a focus view (kCW x kCH at 80,60 in the same target): its depth where it drew it, the eye's where this
        // pass's mask stamped its texels (stamp 9 on every 7th texel; 8 elsewhere, an older pass's).
        {
            constexpr UINT kFX = 80, kFY = 60;
            constexpr uint32_t kStamp = 9;
            std::vector<uint32_t> stampData(kCW * kCH);
            for (UINT i = 0; i < stampData.size(); ++i)
                stampData[i] = i % 7 == 0 ? kStamp : 8;
            const D3D11_TEXTURE2D_DESC stampDesc = {kCW, kCH, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                                    D3D11_BIND_SHADER_RESOURCE, 0, 0};
            const D3D11_SUBRESOURCE_DATA stampInit = {stampData.data(), kCW * sizeof(uint32_t), 0};
            ComPtr<ID3D11Texture2D> stampTex;
            ComPtr<ID3D11ShaderResourceView> stampSrv;
            d->CreateTexture2D(&stampDesc, &stampInit, &stampTex);
            d->CreateShaderResourceView(stampTex.Get(), nullptr, &stampSrv);
            k[26] = kCW;  // the focus view's size and corner
            k[27] = kCH;
            k[28] = kFX;
            k[29] = kFY;
            std::memcpy(&k[30], &kStamp, sizeof(kStamp));
            c->UpdateSubresource(g.cb.Get(), 0, nullptr, k, 0, 0);
            ID3D11ShaderResourceView* in[5] = {eyeSrv.Get(), nullptr, nullptr, nullptr, stampSrv.Get()};
            c->PSSetShaderResources(0, 5, in);
            c->OMSetRenderTargets(1, outRtv.GetAddressOf(), nullptr);
            c->Draw(3, 0);
            c->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11ShaderResourceView* none5[5] = {};
            c->PSSetShaderResources(0, 5, none5);
            std::vector<float> both;
            bool right = readback(d, c, outTex.Get(), both, kCW * kCH, kCW);
            unsigned fromFocus = 0, fromEye = 0;
            for (UINT y = 0; right && y < kCH; ++y)
                for (UINT x = 0; right && x < kCW; ++x) {
                    const bool masked = (y * kCW + x) % 7 == 0;
                    const float want = masked ? eye[(kBY + y) * kTW + kBX + x] : eye[(kFY + y) * kTW + kFX + x];
                    right = both[y * kCW + x] == want;
                    (masked ? fromEye : fromFocus) += right ? 1 : 0;
                }
            std::printf("     copy with a focus view: %u texels from it, %u from the eye where the mask skipped it\n",
                        fromFocus, fromEye);
            check(right && fromFocus > 0 && fromEye > 0,
                  "copy: the focus view's depth where it drew it, the eye's where this pass's mask skipped it");
        }
    }

    std::vector<bool> masked;
    std::vector<Hit> hits;
    float centre = 0;
    char what[200];

    // 1. A flat road between tree walls 15 m out.
    {
        const Scene flat{[](float, float) { return 0.0f; },
                         {{false, -15, 0, 3000, 12}, {false, 15, 0, 3000, 12}}};
        check(run_scene(g, flat, masked, centre, hits), "flat road: through the pipeline");
        const double road = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 1 && h.z > kStart + 1 && std::fabs(h.x) < kHalf - 1;
        });
        const double walls = kept_share(masked, hits, [](const Hit& h) { return h.what == 2 && h.y + kEyeHeight > kTall + 1; });
        const double sky = kept_share(masked, hits, [](const Hit& h) { return h.what == 0; });
        const double dash = kept_share(masked, hits, [](const Hit& h) { return h.what == 3; });
        std::printf("     flat: road kept %.3f, walls above 4.5 m %.3f, sky %.3f, dash %.3f; centre %.2f m\n", road, walls,
                    sky, dash, centre);
        check(road > 0.97 && walls < 0.03 && sky == 0 && dash < 0.03 && std::fabs(centre) < 1,
              "flat road: the road kept, the walls' upper parts, the sky and the dash not; the centre on the head's line");
        std::printf("     flat: the corridor's rows gathered %u-%u, in the margin map %u-%u (of %u)\n", lastRows.gpuTop,
                    lastRows.gpuBottom, lastRows.mapTop, lastRows.mapBottom, kCH);
        check(lastRows.read && lastRows.gpuBottom > lastRows.gpuTop && lastRows.gpuTop == lastRows.mapTop &&
                  lastRows.gpuBottom == lastRows.mapBottom,
              "flat road: the corridor's rows gathered for DLSS's band are the margin map's");
        // The car kept sharp (whatever lies nearer than the corridor's start): the dash kept too, the rest as before.
        gCarM = kStart;
        check(run_scene(g, flat, masked, centre, hits), "flat road, the car kept: through the pipeline");
        gCarM = 0;
        const double carDash = kept_share(masked, hits, [](const Hit& h) { return h.what == 3; });
        const double carSky = kept_share(masked, hits, [](const Hit& h) { return h.what == 0; });
        const double carWalls = kept_share(masked, hits, [](const Hit& h) { return h.what == 2 && h.y + kEyeHeight > kTall + 1; });
        std::printf("     flat, the car kept: dash %.3f, sky %.3f, walls above 4.5 m %.3f\n", carDash, carSky, carWalls);
        check(carDash > 0.95 && carSky == 0 && carWalls < 0.03,
              "flat road, the car kept: the dash kept, the sky and the walls' upper parts still not");
    }
    // 1a. The edge's blend growing with distance: more of the map within the blend, the corridor itself the same.
    {
        const Scene flat{[](float, float) { return 0.0f; }, {{false, -12, 0, 3000, 8}, {false, 12, 0, 3000, 8}}};
        check(run_scene(g, flat, masked, centre, hits), "blend, fixed: through the pipeline");
        const Rows fixedRows = lastRows;
        gFeatherGrowth = 0.03f;
        check(run_scene(g, flat, masked, centre, hits), "blend, growing: through the pipeline");
        gFeatherGrowth = 0;
        std::printf("     blend: %u texels within the edge's blend fixed at 1 m, %u growing 3%% with distance; rows %u-%u, %u-%u\n",
                    fixedRows.blend, lastRows.blend, fixedRows.mapTop, fixedRows.mapBottom, lastRows.mapTop,
                    lastRows.mapBottom);
        // (Only the real edges widen: the road surface stays out of the blend, checked with the fill below. A first
        // version let the far road itself slide into the blend: 4.8 times the texels here, a blurred far road in game.)
        check(lastRows.blend > fixedRows.blend && lastRows.blend < fixedRows.blend * 2 && lastRows.mapTop == fixedRows.mapTop &&
                  lastRows.mapBottom == fixedRows.mapBottom,
              "blend: wider far away with the growth (its edges only), the corridor's rows the same");
    }
    // 1b. A bend: the ground's road leaving the head's line to the right past 40 m (25 m across at 100 m), a 20 m tall wall
    // across at 200 m. Straight, the far road falls outside; widening 15 degrees with distance, it stays in, the wall's
    // upper part still out.
    {
        const auto bendAt = [](float z) { return z > 40 ? (z - 40) * (z - 40) / 144.0f : 0.0f; };
        const Scene flat{[](float, float) { return 0.0f; }, {{true, 200, -300, 300, 20}}};
        const auto bendRoad = [&](const Hit& h) {
            return h.what == 1 && h.z > 70 && h.z < 105 && std::fabs(h.x - bendAt(h.z)) < 3;
        };
        check(run_scene(g, flat, masked, centre, hits), "bend, straight: through the pipeline");
        const double straight = kept_share(masked, hits, bendRoad);
        gSpreadTan = static_cast<float>(std::tan(15.0 * 3.14159265 / 180));
        check(run_scene(g, flat, masked, centre, hits), "bend, widening: through the pipeline");
        gSpreadTan = 0;
        const double widened = kept_share(masked, hits, bendRoad);
        const double wallTop = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 2 && h.y + kEyeHeight > kTall + 1;
        });
        std::printf("     bend: its road 70-105 m out kept %.3f straight, %.3f widening 15 degrees; the wall above 4.5 m %.3f\n",
                    straight, widened, wallTop);
        check(straight < 0.5 && widened > 0.95 && wallTop < 0.05,
              "bend: the road bending away kept with the corridor widening, not without; the wall's upper part still out");
    }
    // 2. The road climbing 12 degrees from 40 m out, a wall across it at 150 m (20 m tall).
    {
        const float slope = std::tan(12.0f * 3.14159265f / 180);
        const Scene hill{[slope](float, float z) { return z > 40 ? (z - 40) * slope : 0.0f; },
                         {{true, 150, -60, 60, 20}}};
        check(run_scene(g, hill, masked, centre, hits), "hill: through the pipeline");
        const double climb = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 1 && h.z > 60 && std::fabs(h.x) < kHalf - 1;
        });
        const double wallTop = kept_share(masked, hits, [slope](const Hit& h) {
            return h.what == 2 && h.y + kEyeHeight - (150 - 40) * slope > kTall + 1;
        });
        std::printf("     hill: its road past 60 m kept %.3f (a level corridor: none past ~57 m), the wall across it above "
                    "4.5 m %.3f\n",
                    climb, wallTop);
        std::snprintf(what, sizeof(what), "hill: the climbing road kept (%.3f), the wall's upper part not (%.3f)", climb,
                      wallTop);
        check(climb > 0.95 && wallTop < 0.05, what);
    }
    // 2a. Downhill over a crest (Spa after Kemmel: Les Combes down to Rivage): the road falls 6% from 50 m to a dip at
    // 150 m and climbs 6% beyond it (checked out to 400 m), a car (1.4 m tall, 2 m wide) on the far climb at 220 m.
    // Before the drop (the floor 1 m below the last road seen) the far road showed as a strip too thin to read as road,
    // so the car and a third of it were measured against the crest: 0.000 and 0.662 kept. And a rounded crest falling
    // away (the road dropping 1 m by 40 m past it, 4 m by 80 m), a car on it at 75 m.
    {
        const Scene dip{[](float, float z) {
                            return z < 50 ? 0.0f : z < 150 ? -(z - 50) * 0.06f : -6.0f + (z - 150) * 0.06f;
                        },
                        {{true, 220, -1, 1, 1.4f}}};
        check(run_scene(g, dip, masked, centre, hits), "dip: through the pipeline");
        const double farRoad = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 1 && h.z > 160 && h.z < 400 && std::fabs(h.x) < kHalf - 1;
        });
        const double car = kept_share(masked, hits, [](const Hit& h) { return h.what == 2 && h.z > 200; });
        std::printf("     dip: the road past the dip kept %.3f, the car on it %.3f\n", farRoad, car);
        gDropTan = 0;  // the floor as before: 1 m below the last road seen
        check(run_scene(g, dip, masked, centre, hits), "dip, no drop: through the pipeline");
        gDropTan = 0.15f;
        const double farRoadOld = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 1 && h.z > 160 && h.z < 400 && std::fabs(h.x) < kHalf - 1;
        });
        const double carOld = kept_share(masked, hits, [](const Hit& h) { return h.what == 2 && h.z > 200; });
        std::printf("     dip, no drop: the road past the dip kept %.3f, the car on it %.3f\n", farRoadOld, carOld);
        std::snprintf(what, sizeof(what), "dip: the road and the car beyond it kept (%.3f, %.3f; without the drop %.3f, %.3f)",
                      farRoad, car, farRoadOld, carOld);
        check(farRoad > 0.95 && car > 0.95, what);
        const Scene crest{[](float, float z) { return z < 40 ? 0.0f : -(z - 40) * (z - 40) / 1600.0f; },
                          {{true, 75, -1, 1, 1.4f}}};
        check(run_scene(g, crest, masked, centre, hits), "crest: through the pipeline");
        const double fallRoad = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 1 && h.z > 50 && std::fabs(h.x) < kHalf - 1;
        });
        // Its middle (its two outer columns are edge texels at this test's resolution, as off centre's 0.6%).
        const double carOn = kept_share(masked, hits, [](const Hit& h) { return h.what == 2 && std::fabs(h.x) < 0.8f; });
        std::printf("     crest: the road falling away past 50 m kept %.3f, the car on it %.3f\n", fallRoad, carOn);
        check(fallRoad > 0.95 && carOn > 0.95, "crest: the road falling away and the car on it kept");
    }
    // 3. The car 4 m left of a wall on its right, the road and the verge out to 30 m on its left.
    {
        const Scene offCentre{[](float, float) { return 0.0f; }, {{false, 4, 0, 3000, 8}, {false, -30, 0, 3000, 12}}};
        check(run_scene(g, offCentre, masked, centre, hits), "off centre: through the pipeline");
        const double left14 = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 1 && h.z > 15 && h.x < -12 && h.x > -14;  // 7-9 m from the centre: clear of its edge
        });
        const double left20 = kept_share(masked, hits, [](const Hit& h) {
            return h.what == 1 && h.z > 15 && h.x < -18;
        });
        std::printf("     off centre: centre %.2f m (reach %.0f), road 12-14 m left kept %.3f, beyond 18 m %.3f\n", centre,
                    kReach, left14, left20);
        // The ground the box sees 10-80 m ahead (16 m either side at most) spans the box's left edge (0.45 z) or 16 m to
        // the wall 4 m right: its mean by area is about 5.3 m left.
        check(centre < -4.5f && centre > -kReach - 0.01f && left14 > 0.95 && left20 < 0.05,
              "off centre: the corridor moves to the road on the left (~5.3 m), 12-14 m left kept, 18 m not");
    }
    // 4. The fill on the flat road.
    {
        const Scene flat{[](float, float) { return 0.0f; },
                         {{false, -15, 0, 3000, 12}, {false, 15, 0, 3000, 12}}};
        Filled f;
        check(run_scene(g, flat, masked, centre, hits, &f), "fill: through the pipeline");
        int maskedN = 0, maskedGreen = 0, maskedDepth = 0, deepN = 0, deepRed = 0, edgeN = 0, edgeMixed = 0, junk = 0;
        for (UINT y = 0; y < kFH; ++y)
            for (UINT x = 0; x < kFW; ++x) {
                const size_t i = y * kFW + x;
                const float r = f.colour[4 * i], gr = f.colour[4 * i + 1];
                const Hit& h = hits[i];
                junk += std::fabs(r + gr - 1) > 0.01f ? 1 : 0;  // a blend of the two: red + green = 1
                if (masked[i]) {
                    ++maskedN;
                    maskedGreen += gr > 0.99f ? 1 : 0;
                    maskedDepth += f.depth[i] < 1.0f ? 1 : 0;  // the eye's depth, not the mask's
                    continue;
                }
                if (h.what != 1)
                    continue;
                const float m = (std::min)((std::min)(h.z - kStart, kHalf - std::fabs(h.x)), kTall - (h.y + kEyeHeight));
                if (m > kFeather + 0.5f) {
                    ++deepN;
                    deepRed += r > 0.99f ? 1 : 0;
                } else if (m > 0.2f && m < kFeather - 0.2f && h.z < 40) {
                    // (closer than 40 m: past it a copy texel spans more than the band tested here)
                    ++edgeN;
                    edgeMixed += r > 0.02f && r < 0.98f ? 1 : 0;
                }
            }
        std::printf("     fill: masked texels green %d / %d, their depth the eye's %d / %d; deep in the corridor red %d / "
                    "%d; within the feather blended %d / %d; texels neither red, green nor a blend %d\n",
                    maskedGreen, maskedN, maskedDepth, maskedN, deepRed, deepN, edgeMixed, edgeN, junk);
        check(maskedN > 0 && maskedGreen == maskedN && maskedDepth == maskedN,
              "fill: every masked texel takes the eye's colour and depth");
        check(deepN > 0 && deepRed >= deepN * 0.99, "fill: the focus view kept deep inside the corridor");
        check(edgeN > 0 && edgeMixed >= edgeN * 0.9, "fill: a blend within the feather of the corridor's edge");
        check(junk == 0, "fill: nothing but the two views and their blends");
        // The edge's blend growing with distance: the road far ahead, well inside the corridor's sides and top, stays the
        // focus view's (the road surface itself is a metre over the corridor's floor: that bound keeps the near blend).
        Filled grown;
        gFeatherGrowth = 0.03f;
        check(run_scene(g, flat, masked, centre, hits, &grown), "fill, the blend growing: through the pipeline");
        gFeatherGrowth = 0;
        int farN = 0, farRed = 0;
        for (UINT y = 0; y < kFH; ++y)
            for (UINT x = 0; x < kFW; ++x) {
                const size_t i = y * kFW + x;
                const Hit& h = hits[i];
                if (masked[i] || h.what != 1 || h.z < 30)
                    continue;
                const float blend = kFeather + 0.03f * std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
                const float m = (std::min)(kHalf - std::fabs(h.x), kTall - (h.y + kEyeHeight));
                if (m > blend + 0.5f) {
                    ++farN;
                    farRed += grown.colour[4 * i] > 0.99f ? 1 : 0;
                }
            }
        std::printf("     fill, the blend growing: the road 30 m and further, inside the grown blend, the focus view's %d / %d\n",
                    farRed, farN);
        check(farN > 0 && farRed >= farN * 0.99, "fill, the blend growing: the far road stays the focus view's");
        // Show Road focus area: the masked texels take the eye's colour tinted (green 1 x 0.4), the corridor as ever.
        Filled tinted;
        gCoverage = 1;
        check(run_scene(g, flat, masked, centre, hits, &tinted), "fill, the coverage shown: through the pipeline");
        gCoverage = 0;
        int tintN = 0, tintOk = 0, keptN = 0, keptRed = 0;
        for (size_t i = 0; i < hits.size(); ++i) {
            const float r = tinted.colour[4 * i], gr = tinted.colour[4 * i + 1];
            if (masked[i]) {
                ++tintN;
                tintOk += std::fabs(gr - 0.4f) < 0.01f && r < 0.01f ? 1 : 0;
            } else if (r > 0.99f) {
                ++keptRed;
            }
            keptN += masked[i] ? 0 : 1;
        }
        std::printf("     fill, the coverage shown: masked texels tinted %d / %d; kept texels red %d / %d\n", tintOk, tintN,
                    keptRed, keptN);
        check(tintN > 0 && tintOk == tintN && keptRed > keptN / 2,
              "fill, the coverage shown: what comes from the periphery tinted, the corridor the focus view's");
    }
    return test_result();
}

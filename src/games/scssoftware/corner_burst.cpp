// The burst recorder: the released images and each replay's output read back and written as PNGs (dev builds'
// Ctrl+Alt+B; corner_internal.h).
#include "corner_internal.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <thread>

#include "../../log.h"
#include "../../probe/d3d11_capture.h"

namespace scs {
namespace detail {

// A burst's record of a replay: the focus region of its first target, and (the burst's first frame) a log line naming
// the pass: its target, shader and inputs.
void shoot_stage(const D3D11_RECT& f, const char* inputs) {
    const int stage = g.stage++;
    ID3D11Texture2D* target = g.boundRt[0];
    if (!target)
        return;
    D3D11_TEXTURE2D_DESC d;
    target->GetDesc(&d);
    const int eye = current_eye();
    if (g.stats.frames == g.burstStartFrame) {
        ID3D11PixelShader* ps = nullptr;
        g.context->PSGetShader(&ps, nullptr, nullptr);
        simqvfr::logf("scs focus: burst stage %02d eye %d: target %ux%u format %d, shader %p, inputs%s", stage, eye, d.Width,
                      d.Height, static_cast<int>(d.Format), static_cast<void*>(ps), inputs);
        if (ps)
            ps->Release();
    }
    if (d.SampleDesc.Count != 1)
        return;
    // The focus region, and the eye's own picture of the box (the game's draw just before): the same pass, both views.
    const float* b = g.box[std::min(std::max(eye, 0), 1)];
    const LONG ex = std::lround(g.fx * d.Width * b[0]), ey = std::lround(g.fy * d.Height * b[1]);
    const D3D11_RECT rects[2] = {f, {ex, ey, ex + std::lround(g.fx * d.Width * beta_x()), ey + std::lround(g.fy * d.Height * beta_y())}};
    const bool isFloat = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                         d.Format == DXGI_FORMAT_R16_FLOAT || d.Format == DXGI_FORMAT_R32_FLOAT ||
                         d.Format == DXGI_FORMAT_R16G16_FLOAT || d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    for (int k = 0; k < 2; ++k) {
        const D3D11_RECT& r = rects[k];
        const UINT w = static_cast<UINT>(r.right - r.left), h = static_cast<UINT>(r.bottom - r.top);
        if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top || static_cast<UINT>(r.right) > d.Width ||
            static_cast<UINT>(r.bottom) > d.Height)
            continue;
        State::Crop* crop = nullptr;
        for (State::Crop& e : g.crops)
            if (e.desc.Width == w && e.desc.Height == h && e.desc.Format == d.Format)
                crop = &e;
        if (!crop) {
            State::Crop e;
            e.desc = {w, h, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
            if (FAILED(g.device->CreateTexture2D(&e.desc, nullptr, &e.tex)))
                continue;
            g.crops.push_back(e);
            crop = &g.crops.back();
        }
        const D3D11_BOX box = {static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0, static_cast<UINT>(r.right),
                               static_cast<UINT>(r.bottom), 1};
        g.context->CopySubresourceRegion(crop->tex.Get(), 0, 0, 0, 0, target, 0, &box);
        wchar_t tag[64];
        swprintf_s(tag, L"stage-eye%d-s%02d-%ux%u-fmt%d%ls-%ls", eye, stage, d.Width, d.Height, static_cast<int>(d.Format),
                   isFloat ? L"-squashed" : L"", k ? L"eyebox" : L"focus");
        g.shootSquash = isFloat;
        shoot(crop->tex.Get(), 0, 0, false, false, tag);
        g.shootSquash = false;
    }
}

ID3D11ShaderResourceView* source_view(ID3D11Texture2D* image, const D3D11_TEXTURE2D_DESC& d) {
    for (State::Source& s : g.sources)
        if (s.image == image)
            return s.view.Get();
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = srgb_view(d.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    State::Source s;
    s.image = image;
    if (FAILED(g.device->CreateShaderResourceView(image, &sv, &s.view)))
        return nullptr;
    g.sources.push_back(s);
    return g.sources.back().view.Get();
}

// The whole image drawn at a quarter of its size into `burstTex`, copied into a new staging texture: one shot.
void shoot(ID3D11Texture2D* image, int chain, uint32_t index, bool after, bool scaled, const wchar_t* tag) {
    two_views(false);
    D3D11_TEXTURE2D_DESC d;
    image->GetDesc(&d);
    ID3D11ShaderResourceView* srv = source_view(image, d);
    if (!srv || !blit_resources())
        return;
    const UINT w = std::max(1u, d.Width / 4), h = std::max(1u, d.Height / 4);
    if (!g.burstTex || g.burstDesc.Width != w || g.burstDesc.Height != h) {
        g.burstTex.Reset();
        g.burstRtv.Reset();
        D3D11_TEXTURE2D_DESC sd = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.burstTex)) ||
            FAILED(g.device->CreateRenderTargetView(g.burstTex.Get(), &rv, &g.burstRtv)))
            return;
        g.burstDesc = sd;
    }
    State::Shot shot;
    D3D11_TEXTURE2D_DESC st = g.burstDesc;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.device->CreateTexture2D(&st, nullptr, &shot.staging)))
        return;
    ID3D11DeviceContext1* c = g.context.Get();
    const ch::Bypass bypass;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> psSrv;
    ComPtr<ID3D11SamplerState> samp;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[kMaxViews];
    UINT vpCount = kMaxViews;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    FLOAT bf[4];
    UINT mask;
    ComPtr<ID3D11DepthStencilState> depth;
    UINT ref;
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &psSrv);
    c->PSGetSamplers(0, 1, &samp);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    c->RSGetViewports(&vpCount, vp);
    c->RSGetState(&raster);
    c->OMGetBlendState(&blend, bf, &mask);
    c->OMGetDepthStencilState(&depth, &ref);
    c->IAGetInputLayout(&layout);
    c->IAGetPrimitiveTopology(&topo);
    ID3D11RenderTargetView* out[] = {g.burstRtv.Get()};
    c->OMSetRenderTargets(1, out, nullptr);
    const D3D11_VIEWPORT v = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    c->RSSetViewports(1, &v);
    c->RSSetState(g.raster.Get());
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.blitVs.Get(), nullptr, 0);
    c->PSSetShader(g.shootSquash && g.squashPs ? g.squashPs.Get() : g.blitPs.Get(), nullptr, 0);
    ID3D11SamplerState* s[] = {g.linear.Get()};
    c->PSSetShaderResources(0, 1, &srv);
    c->PSSetSamplers(0, 1, s);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none[] = {nullptr};
    c->PSSetShaderResources(0, 1, none);
    c->CopyResource(shot.staging.Get(), g.burstTex.Get());
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    for (ID3D11RenderTargetView* r : rtvs)
        if (r)
            r->Release();
    if (dsv)
        dsv->Release();
    if (vpCount)
        c->RSSetViewports(vpCount, vp);
    c->RSSetState(raster.Get());
    c->OMSetBlendState(blend.Get(), bf, mask);
    c->OMSetDepthStencilState(depth.Get(), ref);
    c->IASetInputLayout(layout.Get());
    c->IASetPrimitiveTopology(topo);
    c->VSSetShader(vs.Get(), nullptr, 0);
    c->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* put[] = {psSrv.Get()};
    ID3D11SamplerState* putS[] = {samp.Get()};
    c->PSSetShaderResources(0, 1, put);
    c->PSSetSamplers(0, 1, putS);
    shot.frame = g.stats.frames;
    shot.tag = tag ? tag : L"";
    shot.w = w;
    shot.h = h;
    shot.chain = chain;
    shot.index = index;
    shot.after = after;
    shot.cornered = g.active;
    shot.scaled = scaled;
    g.shots.push_back(shot);
}

// After the burst: every shot read back (render thread), written as PNG on a thread of its own.
void write_burst() {
    struct Pixels {
        std::vector<uint8_t> bgra;
        unsigned w = 0, h = 0;
        std::wstring name;
    };
    std::vector<Pixels> all;
    for (State::Shot& s : g.shots) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
            continue;
        Pixels p;
        p.w = s.w;
        p.h = s.h;
        p.bgra.resize(static_cast<size_t>(p.w) * p.h * 4);
        for (unsigned y = 0; y < p.h; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
            uint8_t* o = p.bgra.data() + static_cast<size_t>(y) * p.w * 4;
            for (unsigned x = 0; x < p.w; ++x) {
                o[x * 4 + 0] = row[x * 4 + 2];
                o[x * 4 + 1] = row[x * 4 + 1];
                o[x * 4 + 2] = row[x * 4 + 0];
                o[x * 4 + 3] = 255;
            }
        }
        g.context->Unmap(s.staging.Get(), 0);
        wchar_t name[96];
        swprintf_s(name, L"f%03llu-eye%d-%ls-img%u%ls%ls.png", static_cast<unsigned long long>(s.frame - g.burstStartFrame),
                   s.chain, s.after ? L"2after" : L"1before", s.index, s.cornered ? L"" : L"-nocorner",
                   s.after && !s.scaled ? L"-NOTSCALED" : L"");
        if (!s.tag.empty())
            swprintf_s(name, L"f%03llu-%ls.png", static_cast<unsigned long long>(s.frame - g.burstStartFrame), s.tag.c_str());
        p.name = name;
        all.push_back(std::move(p));
    }
    g.shots.clear();
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t folder[64];
    swprintf_s(folder, L"scs-burst-%02u%02u%02u", t.wHour, t.wMinute, t.wSecond);
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures" / folder;
    simqvfr::logf("scs corner: burst of %zu images read back; writing them to %ls", all.size(), dir.c_str());
    std::thread([all = std::move(all), dir]() {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        size_t written = 0;
        for (const auto& p : all)
            written += simqvfr::write_png_bgra((dir / p.name).wstring(), p.bgra, p.w, p.h) ? 1 : 0;
        simqvfr::logf("scs corner: burst written: %zu of %zu PNGs in %ls", written, all.size(), dir.c_str());
    }).detach();
}

}  // namespace detail

}  // namespace scs

// The post census of LMU's multi-view path (mvr_pass.h mvr_post_census; diagnostics, log only): what the eye's
// post-processing draws with (mvr_pass_internal.h PostCensus).
#include "mvr_pass_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "../../log.h"
#include "../../probe/d3d11_capture.h"
#include "yebis_shaders.h"

namespace lmu::mvr_detail {
namespace {

// `bytes` of `buffer` from `offset`, read back through a staging copy (a stall: census frames only).
bool census_read(ID3D11Buffer* buffer, UINT offset, UINT bytes, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!buffer || !bytes)
        return false;
    D3D11_BUFFER_DESC bd;
    buffer->GetDesc(&bd);
    if (offset >= bd.ByteWidth)
        return false;
    bytes = (std::min)(bytes, bd.ByteWidth - offset);
    ComPtr<ID3D11Device> device;
    g.context->GetDevice(&device);
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = (bytes + 15) & ~15u;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(device->CreateBuffer(&sd, nullptr, &staging)))
        return false;
    const D3D11_BOX box{offset, 0, 0, offset + bytes, 1, 1};
    const bool replaying = g.replaying;
    g.replaying = true;  // our own calls: the hooks pass them on
    g.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buffer, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    const bool mapped = SUCCEEDED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
    if (mapped) {
        out.assign(static_cast<const std::uint8_t*>(m.pData), static_cast<const std::uint8_t*>(m.pData) + bytes);
        g.context->Unmap(staging.Get(), 0);
    }
    g.replaying = replaying;
    return mapped;
}

std::string census_floats(const std::vector<std::uint8_t>& bytes, std::size_t count) {
    std::string s;
    const std::size_t n = (std::min)(count, bytes.size() / 4);
    for (std::size_t i = 0; i < n; ++i) {
        float f;
        std::memcpy(&f, bytes.data() + i * 4, 4);
        char v[24];
        std::snprintf(v, sizeof(v), "%s%.5g", i ? (i % 4 ? "," : " | ") : "", f);
        s += v;
    }
    return s;
}

template <typename Shader>
uint32_t census_checksum(Shader* shader) {
    uint32_t c[4] = {};
    UINT size = sizeof(c);
    return shader && SUCCEEDED(shader->GetPrivateData(kShaderChecksumGuid, &size, c)) && size == sizeof(c) ? c[0] : 0;
}

// Level 2: `cb`'s bound range (num constants from first; 0: the whole buffer from first) saved for offline reading.
void census_save(ID3D11Buffer* cb, UINT first, UINT num, const char* stage, UINT slot) {
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    const UINT offset = first * 16;
    if (offset >= bd.ByteWidth)
        return;
    const UINT bytes = (std::min)(num ? num * 16 : bd.ByteWidth - offset, bd.ByteWidth - offset);
    std::vector<std::uint8_t> data;
    if (!census_read(cb, offset, bytes, data))
        return;
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"census";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wchar_t name[64];
    swprintf_s(name, L"c%llu_d%d_%hs%u.bin", census.taken, census.draws - 1, stage, slot);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir / name).c_str(), L"wb") == 0 && f) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
}

// The constant buffers 0-3 of a stage: their first 8 float4s from the bound first constant.
template <typename Get>
std::string census_cbs(Get get, const char* stage) {
    std::string s;
    for (UINT slot = 0; slot < 4; ++slot) {
        ID3D11Buffer* cb = nullptr;
        UINT first = 0, num = 0;
        get(slot, &cb, &first, &num);
        if (!cb)
            continue;
        if (census.dumpThis)
            census_save(cb, first, num, stage, slot);
        std::vector<std::uint8_t> bytes;
        if (census_read(cb, first * 16, 128, bytes)) {
            char head[48];
            std::snprintf(head, sizeof(head), " cb%u@%u: ", slot, first);
            s += head + census_floats(bytes, 32);
        }
        cb->Release();
    }
    return s;
}

void census_draw(ID3D11DeviceContext* context, const char* call, long long a, long long b) {
    if (census.draws++ >= kCensusDraws)
        return;
    ComPtr<ID3D11VertexShader> vs;
    context->VSGetShader(&vs, nullptr, nullptr);
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    ID3D11RenderTargetView* rtvs[2] = {};
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(2, rtvs, &dsv);
    std::string targets;
    for (ID3D11RenderTargetView* rtv : rtvs) {
        if (!rtv)
            continue;
        ComPtr<ID3D11Resource> r;
        rtv->GetResource(&r);
        targets += (targets.empty() ? "" : "+") + describe(r.Get());
        rtv->Release();
    }
    std::string inputs;
    ID3D11ShaderResourceView* srvs[4] = {};
    context->PSGetShaderResources(0, 4, srvs);
    for (int i = 0; i < 4; ++i) {
        if (!srvs[i])
            continue;
        ComPtr<ID3D11Resource> r;
        srvs[i]->GetResource(&r);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        srvs[i]->GetDesc(&vd);
        char mip[16] = "";
        if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && vd.Texture2D.MostDetailedMip)
            std::snprintf(mip, sizeof(mip), " mip%u", vd.Texture2D.MostDetailedMip);
        inputs += " t" + std::to_string(i) + " " + describe(r.Get()) + mip;
        srvs[i]->Release();
    }
    char head[400];
    std::snprintf(head, sizeof(head),
                  "lmu mvr: post census %llu: #%d %s(%lld,%lld) vs %08x ps %08x > %s%s; vp %u [%.1f,%.1f %.1fx%.1f] sc %u "
                  "[%ld,%ld,%ld,%ld]; inputs%s",
                  census.taken, census.draws - 1, call, a, b, census_checksum(vs.Get()), census_checksum(ps.Get()),
                  targets.empty() ? "-" : targets.c_str(), dsv ? " +depth" : "", g.vpCount, g.vps[0].TopLeftX,
                  g.vps[0].TopLeftY, g.vps[0].Width, g.vps[0].Height, g.scCount, g.scs[0].left, g.scs[0].top,
                  g.scs[0].right, g.scs[0].bottom, inputs.empty() ? " none" : inputs.c_str());
    // The first vertices of slot 0 (a full-screen quad's UVs, if baked in), from the draw's first vertex.
    std::string verts;
    ComPtr<ID3D11Buffer> vb;
    UINT stride = 0, offset = 0;
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (vb && stride && stride <= 64) {
        const bool indexed = std::strcmp(call, "DrawIndexed") == 0;
        const UINT first = indexed ? 0 : static_cast<UINT>(b);  // DrawIndexed: base vertex unknown here, from 0
        std::vector<std::uint8_t> bytes;
        if (census_read(vb.Get(), offset + first * stride, stride * 4, bytes)) {
            char s[32];
            std::snprintf(s, sizeof(s), " vb stride %u:", stride);
            verts = s;
            for (std::size_t v = 0; v * stride < bytes.size(); ++v) {
                std::vector<std::uint8_t> one(bytes.begin() + v * stride,
                                              bytes.begin() + (std::min)(bytes.size(), (v + 1) * stride));
                verts += " [" + census_floats(one, 8) + "]";
            }
        }
    }
    // The bound first constants (PSSetConstantBuffers1: YEBIS's $Globals is one 64 KB buffer at offsets), through
    // ID3D11DeviceContext1; without it, from offset 0.
    ComPtr<ID3D11DeviceContext1> c1;
    context->QueryInterface(IID_PPV_ARGS(&c1));
    const std::string vcb = census_cbs([&](UINT s, ID3D11Buffer** cb, UINT* f, UINT* n) {
        if (c1)
            c1->VSGetConstantBuffers1(s, 1, cb, f, n);
        else
            context->VSGetConstantBuffers(s, 1, cb);
    }, "vs");
    const std::string pcb = census_cbs([&](UINT s, ID3D11Buffer** cb, UINT* f, UINT* n) {
        if (c1)
            c1->PSGetConstantBuffers1(s, 1, cb, f, n);
        else
            context->PSGetConstantBuffers(s, 1, cb);
    }, "ps");
    simqvfr::logf("%s;%s", head, verts.empty() ? " no vertex buffer" : verts.c_str());
    if (!vcb.empty())
        simqvfr::logf("lmu mvr: post census %llu: #%d vs constants:%s", census.taken, census.draws - 1, vcb.c_str());
    if (!pcb.empty())
        simqvfr::logf("lmu mvr: post census %llu: #%d ps constants:%s", census.taken, census.draws - 1, pcb.c_str());
}

// Level 3: the whole pass from its start, one line a draw (shaders, targets, viewport, scissor and whether the
// rasterizer uses it), and every bound constant buffer of both stages (slots 0-13) saved once per distinct content as
// logs\census\c<N>_<hash>.bin, named in the line: an offline diff between eye sizes finds what follows the size.
constexpr int kCensusWholeDraws = 2500;

std::uint64_t census_hash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : bytes)
        h = (h ^ b) * 1099511628211ull;
    return h;
}

template <typename Get>
std::string census_refs(Get get, const char* stage) {
    std::string s;
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
        ID3D11Buffer* cb = nullptr;
        UINT first = 0, num = 0;
        get(slot, &cb, &first, &num);
        if (!cb)
            continue;
        D3D11_BUFFER_DESC bd;
        cb->GetDesc(&bd);
        const UINT offset = first * 16;
        std::vector<std::uint8_t> data;
        if (offset < bd.ByteWidth &&
            census_read(cb, offset, (std::min)(num ? num * 16 : bd.ByteWidth - offset, bd.ByteWidth - offset), data)) {
            const std::uint64_t h = census_hash(data);
            if (censusSaved.insert(h).second) {
                const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"census";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                wchar_t name[64];
                swprintf_s(name, L"c%llu_%016llx.bin", census.taken, static_cast<unsigned long long>(h));
                FILE* f = nullptr;
                if (_wfopen_s(&f, (dir / name).c_str(), L"wb") == 0 && f) {
                    std::fwrite(data.data(), 1, data.size(), f);
                    std::fclose(f);
                }
            }
            char ref[48];
            std::snprintf(ref, sizeof(ref), " %s%u=%016llx", stage, slot, static_cast<unsigned long long>(h));
            s += ref;
        }
        cb->Release();
    }
    return s;
}

void census_draw_whole(ID3D11DeviceContext* context, const char* call, long long a, long long b) {
    if (census.draws++ >= kCensusWholeDraws)
        return;
    ComPtr<ID3D11VertexShader> vs;
    context->VSGetShader(&vs, nullptr, nullptr);
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    ID3D11RenderTargetView* rtvs[2] = {};
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(2, rtvs, &dsv);
    std::string targets;
    for (ID3D11RenderTargetView* rtv : rtvs) {
        if (!rtv)
            continue;
        ComPtr<ID3D11Resource> r;
        rtv->GetResource(&r);
        targets += (targets.empty() ? "" : "+") + describe(r.Get());
        rtv->Release();
    }
    if (dsv) {
        ComPtr<ID3D11Resource> r;
        dsv->GetResource(&r);
        targets += " z " + describe(r.Get());
    }
    ComPtr<ID3D11RasterizerState> rs;
    context->RSGetState(&rs);
    D3D11_RASTERIZER_DESC rd{};
    if (rs)
        rs->GetDesc(&rd);
    ComPtr<ID3D11DeviceContext1> c1;
    context->QueryInterface(IID_PPV_ARGS(&c1));
    const std::string refs =
        census_refs([&](UINT s, ID3D11Buffer** cb, UINT* f, UINT* n) {
            if (c1)
                c1->VSGetConstantBuffers1(s, 1, cb, f, n);
            else
                context->VSGetConstantBuffers(s, 1, cb);
        }, "vs") +
        census_refs([&](UINT s, ID3D11Buffer** cb, UINT* f, UINT* n) {
            if (c1)
                c1->PSGetConstantBuffers1(s, 1, cb, f, n);
            else
                context->PSGetConstantBuffers(s, 1, cb);
        }, "ps");
    // Inside a pass the rects as the hooks hold them (cut); outside, as the context holds them (the game's).
    UINT vpCount = g.vpCount, scCount = g.scCount;
    D3D11_VIEWPORT vp = g.vps[0];
    D3D11_RECT sc = g.scs[0];
    if (g.pass < 0) {
        D3D11_VIEWPORT vps[kMaxRects] = {};
        D3D11_RECT scs[kMaxRects] = {};
        vpCount = kMaxRects;
        context->RSGetViewports(&vpCount, vps);
        scCount = kMaxRects;
        context->RSGetScissorRects(&scCount, scs);
        vp = vps[0];
        sc = scs[0];
    }
    simqvfr::logf("lmu mvr: pass census %llu: #%d %s %s(%lld,%lld) vs %08x ps %08x > %s; vp %u [%.1f,%.1f %.1fx%.1f] sc %u%s "
                  "[%ld,%ld,%ld,%ld];%s",
                  census.taken, census.draws - 1, g.pass < 0 ? "before" : g.afterResolve ? "post" : "scene", call, a, b,
                  census_checksum(vs.Get()), census_checksum(ps.Get()), targets.empty() ? "-" : targets.c_str(), vpCount,
                  vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, scCount, rd.ScissorEnable ? " on" : " off", sc.left, sc.top,
                  sc.right, sc.bottom, refs.c_str());
}

// Level 5: on the first census, after each full-size draw of pass 0's post (and once of the scene before it), the
// target saved as logs\captures\census<N>-d<draw>-<what>.png: which step of the eye's post-processing makes its image.
int censusSnaps = 0;

void census_snap(ID3D11Resource* resource, const char* what) {
    ComPtr<ID3D11Texture2D> t;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&t))) || censusSnaps >= 16)
        return;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    if (d.Width != sizeCut.fullW || d.Height != sizeCut.fullH || d.SampleDesc.Count != 1)
        return;
    const bool replaying = g.replaying;
    g.replaying = true;
    std::vector<uint8_t> bgra;
    unsigned w = 0, h = 0;
    std::vector<uint16_t> halves;
    bool ok = false;
    if (read_halves(t.Get(), halves, w, h)) {
        bgra = tonemapped(halves, w, h);
        ok = true;
    } else {
        ok = simqvfr::d3d11_read_bgra8(t.Get(), bgra, w, h);
    }
    g.replaying = replaying;
    if (!ok) {
        simqvfr::logf("lmu mvr: pass census %llu: #%d %s: %ux%u f%d can't be read back", census.taken, census.draws - 1,
                      what, d.Width, d.Height, static_cast<int>(d.Format));
        return;
    }
    ++censusSnaps;
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    wchar_t name[96];
    swprintf_s(name, L"census%llu-d%d-%hs.png", census.taken, census.draws - 1, what);
    simqvfr::write_png_bgra((dir / name).wstring(), bgra, w, h);
    simqvfr::logf("lmu mvr: pass census %llu: #%d %s saved (%ls, %ux%u f%d)", census.taken, census.draws - 1, what, name, w,
                  h, static_cast<int>(d.Format));
}

// Before a census draw (level 5): the post's first draw's inputs, the scene as the post finds it.
void census_snap_before(ID3D11DeviceContext* context) {
    if (census.level < 5 || census.dumps != 1 || g.pass != 0 || !g.afterResolve || censusSnaps)
        return;
    ID3D11ShaderResourceView* srvs[2] = {};
    context->PSGetShaderResources(0, 2, srvs);
    for (int i = 0; i < 2; ++i) {
        if (!srvs[i])
            continue;
        ComPtr<ID3D11Resource> r;
        srvs[i]->GetResource(&r);
        srvs[i]->Release();
        census_snap(r.Get(), i ? "input1-before-post" : "input0-before-post");
    }
}

}  // namespace

// After a census draw (level 5): its target, if full-size.
void census_after(ID3D11DeviceContext* context) {
    if (census.level < 5 || census.dumps != 1 || !census.active || g.pass != 0 || !g.afterResolve)
        return;
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> r;
    if (rtv)
        rtv->GetResource(&r);
    census_snap(r.Get(), "after");
}

void census_any(ID3D11DeviceContext* context, const char* call, long long a, long long b) {
    if (census.level >= 3) {
        census_draw_whole(context, call, a, b);
        census_snap_before(context);
    }
    else
        census_draw(context, call, a, b);
}

}  // namespace lmu::mvr_detail

namespace lmu {

using namespace mvr_detail;

void mvr_post_census(int level) {
    census = PostCensus{};
    census.level = level;
    if (level)
        simqvfr::log_line("lmu mvr: post census on (lmu_post_census): every draw after tracked pass 0's resolve logged with "
                          "its viewport, inputs, first vertices and constants, every 900 frames and at each new eye size");
}

}  // namespace lmu

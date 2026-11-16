// Offline check of the shared D3D11 state guard (engine/d3d11_state.h) on a WARP device. A known full state (A) is
// bound, then for each part: the guard taken, another full state (B) bound over everything and drawn with, the guard
// let go. Every piece the part covers must be A's again (ranged constant buffers with their ranges, the output merger's
// UAVs after its targets, stream-out, the predicate, 0 viewports set back or left per the option), everything else
// B's as drawn; the setters a blit's spec makes are counted, and every object's reference count is as it started.
#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/d3d11_state.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;
namespace d3s = simqvfr::d3d11_state;

namespace {

constexpr char kShaders[] = R"(
struct VSIn { float3 pos : POSITION; };
float4 vs_main(VSIn i) : SV_Position { return float4(i.pos, 1); }
float4 ps_main(float4 p : SV_Position) : SV_Target { return 1; }
RWTexture2D<float> u : register(u0);
[numthreads(1, 1, 1)] void cs_main() { u[uint2(0, 0)] = 1; }
struct V { float4 pos : SV_Position; };
[maxvertexcount(3)] void gs_main(triangle V i[3], inout TriangleStream<V> s) { for (int k = 0; k < 3; ++k) s.Append(i[k]); }
struct Hc { float e[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct Cp { float4 pos : POS; };
Hc hsc(InputPatch<Cp, 3> p) { Hc o; o.e[0] = o.e[1] = o.e[2] = 1; o.inside = 1; return o; }
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("hsc")] Cp hs_main(InputPatch<Cp, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }
[domain("tri")] float4 ds_main(Hc c, float3 w : SV_DomainLocation, const OutputPatch<Cp, 3> p) : SV_Position {
    return p[0].pos * w.x + p[1].pos * w.y + p[2].pos * w.z;
}
)";

using G = simqvfr::D3D11StateGuard;
constexpr UINT kCbs = G::kCbs, kSrvs = G::kSrvs, kSamps = G::kSamplers, kUavs = G::kUavs, kRects = G::kRects;

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<ID3D11DeviceContext1> context1;
std::vector<IUnknown*> objects;  // every object made, for the reference counts
std::vector<ComPtr<ID3DBlob>> code;

template <class T> T* keep(ComPtr<T>& p) {
    objects.push_back(p.Get());
    return p.Get();
}
ID3DBlob* compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3DCompile(kShaders, sizeof(kShaders) - 1, "d3d11_state_test", nullptr, nullptr, entry, target, 0, 0, &blob,
                          &errors))) {
        std::printf("compile %s: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    code.push_back(blob);
    return blob.Get();
}
ULONG refs(IUnknown* u) {
    u->AddRef();
    return u->Release();
}

ComPtr<ID3D11Texture2D> texture(UINT bind, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    const D3D11_TEXTURE2D_DESC d = {16, 16, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&d, nullptr, &t);
    keep(t);
    return t;
}
ComPtr<ID3D11Buffer> buffer(UINT bytes, UINT bind) {
    const D3D11_BUFFER_DESC d = {bytes, D3D11_USAGE_DEFAULT, bind, 0, 0, 0};
    ComPtr<ID3D11Buffer> b;
    device->CreateBuffer(&d, nullptr, &b);
    keep(b);
    return b;
}

// One full state: every part the guard knows, its own objects (states with descriptions of their own: d3d11 hands out
// one object per description).
struct Full {
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    ComPtr<ID3D11Buffer> vb[2], ib;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11Buffer> cb[3][3];  // per stage, slots 0-2
    ComPtr<ID3D11ShaderResourceView> srv[3][3];
    ComPtr<ID3D11SamplerState> samp[3][2];
    ComPtr<ID3D11UnorderedAccessView> csUav[2];
    ComPtr<ID3D11RenderTargetView> rtv[2];
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> omUav;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Buffer> so[2];
    ComPtr<ID3D11Predicate> predicate;
    UINT viewports = 3, scissors = 3;
    bool second = false;  // B: other descriptions, GS/HS/DS null, two targets and no output merger UAV
};

void make(Full& f, bool second) {
    f.second = second;
    const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, second ? 0u : 4u,
                                              D3D11_INPUT_PER_VERTEX_DATA, 0};
    ID3DBlob* vsCode = compile("vs_main", "vs_5_0");
    device->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &f.layout);
    keep(f.layout);
    f.topology = second ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    for (auto& b : f.vb)
        b = buffer(256, D3D11_BIND_VERTEX_BUFFER);
    f.ib = buffer(256, D3D11_BIND_INDEX_BUFFER);
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &f.vs);
    keep(f.vs);
    ID3DBlob* c = compile("ps_main", "ps_5_0");
    device->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &f.ps);
    keep(f.ps);
    c = compile("cs_main", "cs_5_0");
    device->CreateComputeShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &f.cs);
    keep(f.cs);
    if (!second) {
        c = compile("gs_main", "gs_5_0");
        device->CreateGeometryShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &f.gs);
        keep(f.gs);
        c = compile("hs_main", "hs_5_0");
        device->CreateHullShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &f.hs);
        keep(f.hs);
        c = compile("ds_main", "ds_5_0");
        device->CreateDomainShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &f.ds);
        keep(f.ds);
    }
    for (auto& stage : f.cb)
        for (auto& b : stage)
            b = buffer(64 * 1024, D3D11_BIND_CONSTANT_BUFFER);
    for (auto& stage : f.srv)
        for (auto& v : stage) {
            ComPtr<ID3D11Texture2D> t = texture(D3D11_BIND_SHADER_RESOURCE);
            device->CreateShaderResourceView(t.Get(), nullptr, &v);
            keep(v);
        }
    float bias = second ? 4.0f : 0.0f;  // (no description shared with A's)
    for (auto& stage : f.samp)
        for (auto& s : stage) {
            D3D11_SAMPLER_DESC d = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                    D3D11_TEXTURE_ADDRESS_CLAMP, bias, 1, D3D11_COMPARISON_NEVER, {}, 0, D3D11_FLOAT32_MAX};
            bias += 0.25f;
            device->CreateSamplerState(&d, &s);
            keep(s);
        }
    for (auto& u : f.csUav) {
        ComPtr<ID3D11Texture2D> t = texture(D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R32_FLOAT);
        device->CreateUnorderedAccessView(t.Get(), nullptr, &u);
        keep(u);
    }
    for (auto& r : f.rtv) {
        ComPtr<ID3D11Texture2D> t = texture(D3D11_BIND_RENDER_TARGET);
        device->CreateRenderTargetView(t.Get(), nullptr, &r);
        keep(r);
    }
    {
        ComPtr<ID3D11Texture2D> t = texture(D3D11_BIND_DEPTH_STENCIL, DXGI_FORMAT_D24_UNORM_S8_UINT);
        device->CreateDepthStencilView(t.Get(), nullptr, &f.dsv);
        keep(f.dsv);
        ComPtr<ID3D11Texture2D> u = texture(D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R32_FLOAT);
        device->CreateUnorderedAccessView(u.Get(), nullptr, &f.omUav);
        keep(f.omUav);
    }
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = second ? 0x7 : 0xf;
    bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    device->CreateBlendState(&bd, &f.blend);
    keep(f.blend);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = !second;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = second ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_LESS;
    dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    device->CreateDepthStencilState(&dd, &f.depth);
    keep(f.depth);
    const D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, second ? D3D11_CULL_NONE : D3D11_CULL_FRONT, FALSE, 0, 0, 0, TRUE, TRUE,
                                      FALSE, FALSE};
    device->CreateRasterizerState(&rd, &f.raster);
    keep(f.raster);
    for (auto& b : f.so)
        b = buffer(1024, D3D11_BIND_STREAM_OUTPUT);
    const D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    device->CreatePredicate(&qd, &f.predicate);
    keep(f.predicate);
    context->Begin(f.predicate.Get());  // issued (nothing drawn), so a predicated draw doesn't wait on it
    context->End(f.predicate.Get());
    f.viewports = f.scissors = second ? 1 : 3;
}

void bind(const Full& f) {
    ID3D11DeviceContext* c = context.Get();
    c->IASetInputLayout(f.layout.Get());
    c->IASetPrimitiveTopology(f.topology);
    ID3D11Buffer* vbs[2] = {f.vb[0].Get(), f.vb[1].Get()};
    const UINT strides[2] = {12, f.second ? 16u : 24u}, offsets[2] = {0, f.second ? 0u : 32u};
    c->IASetVertexBuffers(f.second ? 0 : 3, 2, vbs, strides, offsets);
    c->IASetIndexBuffer(f.ib.Get(), f.second ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, f.second ? 0 : 8);
    c->VSSetShader(f.vs.Get(), nullptr, 0);
    c->PSSetShader(f.ps.Get(), nullptr, 0);
    c->CSSetShader(f.cs.Get(), nullptr, 0);
    c->GSSetShader(f.gs.Get(), nullptr, 0);
    c->HSSetShader(f.hs.Get(), nullptr, 0);
    c->DSSetShader(f.ds.Get(), nullptr, 0);
    for (int s = 0; s < 3; ++s) {
        ID3D11Buffer* b[3] = {f.cb[s][0].Get(), f.cb[s][1].Get(), f.cb[s][2].Get()};
        // A: slot 1 a range of its buffer (as the SCS games bind theirs), the others whole. B: all whole.
        const UINT first[3] = {0, f.second ? 0u : 16u, 0}, num[3] = {4096, f.second ? 4096u : 32u, 4096};
        ID3D11ShaderResourceView* v[3] = {f.srv[s][0].Get(), f.srv[s][1].Get(), f.srv[s][2].Get()};
        ID3D11SamplerState* sm[2] = {f.samp[s][0].Get(), f.samp[s][1].Get()};
        if (s == 0) {
            context1->VSSetConstantBuffers1(0, 3, b, first, num);
            c->VSSetShaderResources(0, 3, v);
            c->VSSetSamplers(0, 2, sm);
        } else if (s == 1) {
            context1->PSSetConstantBuffers1(0, 3, b, first, num);
            c->PSSetShaderResources(0, 3, v);
            c->PSSetSamplers(0, 2, sm);
        } else {
            context1->CSSetConstantBuffers1(0, 3, b, first, num);
            c->CSSetShaderResources(0, 3, v);
            c->CSSetSamplers(0, 2, sm);
        }
    }
    ID3D11UnorderedAccessView* cu[2] = {f.csUav[0].Get(), f.csUav[1].Get()};
    c->CSSetUnorderedAccessViews(f.second ? 1 : 0, 2, cu, nullptr);
    ID3D11RenderTargetView* r[2] = {f.rtv[0].Get(), f.rtv[1].Get()};
    if (f.second) {
        c->OMSetRenderTargets(2, r, nullptr);
    } else {  // one target, a UAV in the slot after it (UAVStartSlot 1)
        ID3D11UnorderedAccessView* u = f.omUav.Get();
        c->OMSetRenderTargetsAndUnorderedAccessViews(1, r, f.dsv.Get(), 1, 1, &u, nullptr);
    }
    const FLOAT factor[4] = {f.second ? 0.5f : 0.25f, 0.1f, 0.2f, 0.3f};
    c->OMSetBlendState(f.blend.Get(), factor, f.second ? 0xff : 0xf0f0f0f0);
    c->OMSetDepthStencilState(f.depth.Get(), f.second ? 3 : 7);
    c->RSSetState(f.raster.Get());
    D3D11_VIEWPORT vp[3];
    D3D11_RECT sc[3];
    for (int i = 0; i < 3; ++i) {
        vp[i] = {static_cast<float>(i), f.second ? 1.0f : 2.0f, 8, 8, 0, 1};
        sc[i] = {i, f.second ? 1 : 2, 8 + i, 9};
    }
    c->RSSetViewports(f.viewports, f.viewports ? vp : nullptr);
    c->RSSetScissorRects(f.scissors, f.scissors ? sc : nullptr);
    ID3D11Buffer* so[2] = {f.so[0].Get(), f.so[1].Get()};
    const UINT soOffsets[2] = {0, 0};
    c->SOSetTargets(2, so, soOffsets);
    c->SetPredication(f.predicate.Get(), f.second ? FALSE : TRUE);
}

// The context's state as read back (raw pointers: the objects stay alive in their Full).
struct Snap {
    void* layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    void* vb[G::kBuffers];
    UINT strides[G::kBuffers], offsets[G::kBuffers];
    void* ib;
    DXGI_FORMAT ibFormat;
    UINT ibOffset;
    void *vs, *ps, *cs, *gs, *hs, *ds;
    void* cb[3][kCbs];
    UINT first[3][kCbs], num[3][kCbs];
    void* srv[3][kSrvs];
    void* samp[3][kSamps];
    void* csUav[kUavs];
    void* rtv[8];
    void* dsv;
    void* omUav[kUavs];
    void* blend;
    FLOAT factor[4];
    UINT mask;
    void* depth;
    UINT ref;
    void* raster;
    UINT viewports, scissors;
    D3D11_VIEWPORT vp[kRects];
    D3D11_RECT sc[kRects];
    void* so[4];
    void* predicate;
    BOOL predicateValue;
};
template <class T> void* raw(T* p) {
    if (p)
        p->Release();
    return p;
}
template <class T, size_t N> void raw_all(T* (&in)[N], void** out) {
    for (size_t i = 0; i < N; ++i)
        out[i] = raw(in[i]);
}
Snap snap() {
    ID3D11DeviceContext* c = context.Get();
    Snap s{};
    ID3D11InputLayout* l;
    c->IAGetInputLayout(&l);
    s.layout = raw(l);
    c->IAGetPrimitiveTopology(&s.topology);
    ID3D11Buffer* vb[G::kBuffers];
    c->IAGetVertexBuffers(0, G::kBuffers, vb, s.strides, s.offsets);
    raw_all(vb, s.vb);
    ID3D11Buffer* ib;
    c->IAGetIndexBuffer(&ib, &s.ibFormat, &s.ibOffset);
    s.ib = raw(ib);
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11ComputeShader* cs;
    ID3D11GeometryShader* gs;
    ID3D11HullShader* hs;
    ID3D11DomainShader* ds;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->CSGetShader(&cs, nullptr, nullptr);
    c->GSGetShader(&gs, nullptr, nullptr);
    c->HSGetShader(&hs, nullptr, nullptr);
    c->DSGetShader(&ds, nullptr, nullptr);
    s.vs = raw(vs), s.ps = raw(ps), s.cs = raw(cs), s.gs = raw(gs), s.hs = raw(hs), s.ds = raw(ds);
    ID3D11Buffer* cb[kCbs];
    ID3D11ShaderResourceView* srv[kSrvs];
    ID3D11SamplerState* samp[kSamps];
    context1->VSGetConstantBuffers1(0, kCbs, cb, s.first[0], s.num[0]);
    raw_all(cb, s.cb[0]);
    context1->PSGetConstantBuffers1(0, kCbs, cb, s.first[1], s.num[1]);
    raw_all(cb, s.cb[1]);
    context1->CSGetConstantBuffers1(0, kCbs, cb, s.first[2], s.num[2]);
    raw_all(cb, s.cb[2]);
    c->VSGetShaderResources(0, kSrvs, srv);
    raw_all(srv, s.srv[0]);
    c->PSGetShaderResources(0, kSrvs, srv);
    raw_all(srv, s.srv[1]);
    c->CSGetShaderResources(0, kSrvs, srv);
    raw_all(srv, s.srv[2]);
    c->VSGetSamplers(0, kSamps, samp);
    raw_all(samp, s.samp[0]);
    c->PSGetSamplers(0, kSamps, samp);
    raw_all(samp, s.samp[1]);
    c->CSGetSamplers(0, kSamps, samp);
    raw_all(samp, s.samp[2]);
    ID3D11UnorderedAccessView* uav[kUavs];
    c->CSGetUnorderedAccessViews(0, kUavs, uav);
    raw_all(uav, s.csUav);
    ID3D11RenderTargetView* rtv[8];
    ID3D11DepthStencilView* dsv;
    c->OMGetRenderTargetsAndUnorderedAccessViews(8, rtv, &dsv, 0, kUavs, uav);
    raw_all(rtv, s.rtv);
    raw_all(uav, s.omUav);
    s.dsv = raw(dsv);
    ID3D11BlendState* blend;
    c->OMGetBlendState(&blend, s.factor, &s.mask);
    s.blend = raw(blend);
    ID3D11DepthStencilState* depth;
    c->OMGetDepthStencilState(&depth, &s.ref);
    s.depth = raw(depth);
    ID3D11RasterizerState* raster;
    c->RSGetState(&raster);
    s.raster = raw(raster);
    s.viewports = s.scissors = kRects;
    c->RSGetViewports(&s.viewports, s.vp);
    c->RSGetScissorRects(&s.scissors, s.sc);
    ID3D11Buffer* so[4];
    c->SOGetTargets(4, so);
    raw_all(so, s.so);
    ID3D11Predicate* p;
    c->GetPredication(&p, &s.predicateValue);
    s.predicate = raw(p);
    return s;
}

bool same_mem(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) == 0; }
bool in(d3s::Slots r, UINT i) { return i >= r.first && i < r.first + r.count; }

// `got` against what `spec` leaves: its parts and slots A's, everything else B's as drawn. False, with the first piece
// that differs in `what`.
bool as_expected(const Snap& got, const Snap& A, const Snap& B, const d3s::Spec& spec, const char*& what) {
    const uint32_t p = spec.parts | ((spec.parts & d3s::kOMUavs) ? d3s::kOMTargets : 0);
    const auto want = [&](uint32_t part) -> const Snap& { return (p & part) ? A : B; };
    what = "input layout";
    if (got.layout != want(d3s::kInputLayout).layout || got.topology != want(d3s::kInputLayout).topology)
        return false;
    what = "vertex and index buffers";
    const Snap& ia = want(d3s::kIABuffers);
    if (!same_mem(got.vb, ia.vb, sizeof(got.vb)) || !same_mem(got.strides, ia.strides, sizeof(got.strides)) ||
        !same_mem(got.offsets, ia.offsets, sizeof(got.offsets)) || got.ib != ia.ib || got.ibFormat != ia.ibFormat ||
        got.ibOffset != ia.ibOffset)
        return false;
    what = "shaders";
    if (got.vs != want(d3s::kVS).vs || got.ps != want(d3s::kPS).ps || got.cs != want(d3s::kCS).cs)
        return false;
    what = "GS/HS/DS";
    const Snap& g = want(d3s::kGsHsDs);
    if (got.gs != g.gs || got.hs != g.hs || got.ds != g.ds)
        return false;
    const d3s::Slots cbs[3] = {spec.vsCb, spec.psCb, spec.csCb}, srvs[3] = {spec.vsSrv, spec.psSrv, spec.csSrv},
                     samps[3] = {spec.vsSamplers, spec.psSamplers, spec.csSamplers};
    for (int s = 0; s < 3; ++s) {
        what = "constant buffers (and their ranges)";
        for (UINT i = 0; i < kCbs; ++i) {
            const Snap& w = in(cbs[s], i) ? A : B;
            if (got.cb[s][i] != w.cb[s][i] ||
                (w.cb[s][i] && (got.first[s][i] != w.first[s][i] || got.num[s][i] != w.num[s][i])))
                return false;
        }
        what = "shader resources";
        for (UINT i = 0; i < kSrvs; ++i)
            if (got.srv[s][i] != (in(srvs[s], i) ? A : B).srv[s][i])
                return false;
        what = "samplers";
        for (UINT i = 0; i < kSamps; ++i)
            if (got.samp[s][i] != (in(samps[s], i) ? A : B).samp[s][i])
                return false;
    }
    what = "compute UAVs";
    for (UINT i = 0; i < kUavs; ++i)
        if (got.csUav[i] != (in(spec.csUavs, i) ? A : B).csUav[i])
            return false;
    what = "render targets";
    const Snap& om = want(d3s::kOMTargets);
    if (!same_mem(got.rtv, om.rtv, sizeof(got.rtv)) || got.dsv != om.dsv)
        return false;
    // (the targets alone: whether UAVs after them stay is d3d11's)
    what = "output merger UAVs";
    if (((p & d3s::kOMUavs) || !(p & d3s::kOMTargets)) && !same_mem(got.omUav, want(d3s::kOMUavs).omUav, sizeof(got.omUav)))
        return false;
    what = "blend";
    const Snap& bl = want(d3s::kBlend);
    if (got.blend != bl.blend || !same_mem(got.factor, bl.factor, sizeof(got.factor)) || got.mask != bl.mask)
        return false;
    what = "depth-stencil";
    if (got.depth != want(d3s::kDepth).depth || got.ref != want(d3s::kDepth).ref)
        return false;
    what = "rasterizer";
    if (got.raster != want(d3s::kRSState).raster)
        return false;
    // A saved count of 0: set back (emptyRects) or not set at all (B's as drawn).
    what = "viewports";
    const Snap& vp = (p & d3s::kViewports) && (A.viewports || spec.emptyRects) ? A : B;
    if (got.viewports != vp.viewports || !same_mem(got.vp, vp.vp, sizeof(D3D11_VIEWPORT) * got.viewports))
        return false;
    what = "scissors";
    const Snap& sc = (p & d3s::kScissors) && (A.scissors || spec.emptyRects) ? A : B;
    if (got.scissors != sc.scissors || !same_mem(got.sc, sc.sc, sizeof(D3D11_RECT) * got.scissors))
        return false;
    what = "stream-out";
    if (!same_mem(got.so, want(d3s::kStreamOut).so, sizeof(got.so)))
        return false;
    what = "predicate";
    return got.predicate == want(d3s::kPredication).predicate &&
           got.predicateValue == want(d3s::kPredication).predicateValue;
}

// Counts the guard's setters (a policy like LMU's RealCalls), straight on to the context.
struct Counting : d3s::DirectCalls {
    static inline int sets = 0;
    using D = d3s::DirectCalls;
    static void IASetInputLayout(C* c, ID3D11InputLayout* l) { ++sets, D::IASetInputLayout(c, l); }
    static void IASetPrimitiveTopology(C* c, D3D11_PRIMITIVE_TOPOLOGY t) { ++sets, D::IASetPrimitiveTopology(c, t); }
    static void IASetVertexBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* o) {
        ++sets, D::IASetVertexBuffers(c, s, n, b, st, o);
    }
    static void IASetIndexBuffer(C* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT o) { ++sets, D::IASetIndexBuffer(c, b, f, o); }
    static void VSSetShader(C* c, ID3D11VertexShader* s, ID3D11ClassInstance* const* i, UINT n) { ++sets, D::VSSetShader(c, s, i, n); }
    static void PSSetShader(C* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* i, UINT n) { ++sets, D::PSSetShader(c, s, i, n); }
    static void CSSetShader(C* c, ID3D11ComputeShader* s, ID3D11ClassInstance* const* i, UINT n) { ++sets, D::CSSetShader(c, s, i, n); }
    static void GSSetShader(C* c, ID3D11GeometryShader* s) { ++sets, D::GSSetShader(c, s); }
    static void HSSetShader(C* c, ID3D11HullShader* s) { ++sets, D::HSSetShader(c, s); }
    static void DSSetShader(C* c, ID3D11DomainShader* s) { ++sets, D::DSSetShader(c, s); }
    static void VSSetConstantBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b) { ++sets, D::VSSetConstantBuffers(c, s, n, b); }
    static void PSSetConstantBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b) { ++sets, D::PSSetConstantBuffers(c, s, n, b); }
    static void CSSetConstantBuffers(C* c, UINT s, UINT n, ID3D11Buffer* const* b) { ++sets, D::CSSetConstantBuffers(c, s, n, b); }
    static void VSSetConstantBuffers1(C1* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        ++sets, D::VSSetConstantBuffers1(c, s, n, b, f, k);
    }
    static void PSSetConstantBuffers1(C1* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        ++sets, D::PSSetConstantBuffers1(c, s, n, b, f, k);
    }
    static void CSSetConstantBuffers1(C1* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        ++sets, D::CSSetConstantBuffers1(c, s, n, b, f, k);
    }
    static void VSSetShaderResources(C* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { ++sets, D::VSSetShaderResources(c, s, n, v); }
    static void PSSetShaderResources(C* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { ++sets, D::PSSetShaderResources(c, s, n, v); }
    static void CSSetShaderResources(C* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { ++sets, D::CSSetShaderResources(c, s, n, v); }
    static void VSSetSamplers(C* c, UINT s, UINT n, ID3D11SamplerState* const* v) { ++sets, D::VSSetSamplers(c, s, n, v); }
    static void PSSetSamplers(C* c, UINT s, UINT n, ID3D11SamplerState* const* v) { ++sets, D::PSSetSamplers(c, s, n, v); }
    static void CSSetSamplers(C* c, UINT s, UINT n, ID3D11SamplerState* const* v) { ++sets, D::CSSetSamplers(c, s, n, v); }
    static void CSSetUnorderedAccessViews(C* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* u, const UINT* k) {
        ++sets, D::CSSetUnorderedAccessViews(c, s, n, u, k);
    }
    static void OMSetRenderTargets(C* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
        ++sets, D::OMSetRenderTargets(c, n, r, d);
    }
    static void OMSetRenderTargetsAndUnorderedAccessViews(C* c, UINT n, ID3D11RenderTargetView* const* r,
                                                          ID3D11DepthStencilView* d, UINT us, UINT un,
                                                          ID3D11UnorderedAccessView* const* u, const UINT* k) {
        ++sets, D::OMSetRenderTargetsAndUnorderedAccessViews(c, n, r, d, us, un, u, k);
    }
    static void OMSetBlendState(C* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) { ++sets, D::OMSetBlendState(c, b, f, m); }
    static void OMSetDepthStencilState(C* c, ID3D11DepthStencilState* d, UINT r) { ++sets, D::OMSetDepthStencilState(c, d, r); }
    static void RSSetState(C* c, ID3D11RasterizerState* r) { ++sets, D::RSSetState(c, r); }
    static void RSSetViewports(C* c, UINT n, const D3D11_VIEWPORT* v) { ++sets, D::RSSetViewports(c, n, v); }
    static void RSSetScissorRects(C* c, UINT n, const D3D11_RECT* r) { ++sets, D::RSSetScissorRects(c, n, r); }
    static void SOSetTargets(C* c, UINT n, ID3D11Buffer* const* b, const UINT* o) { ++sets, D::SOSetTargets(c, n, b, o); }
    static void SetPredication(C* c, ID3D11Predicate* p, BOOL v) { ++sets, D::SetPredication(c, p, v); }
};

Full a, b;
Snap snapB;

// One case: A bound, the guard taken with `spec`, everything cleared and B bound and drawn, the guard let go: what the
// spec covers must be A's again, everything else B's. Returns the setters the guard made.
int run_case(const char* name, const d3s::Spec& spec, bool plainContext = false, const Full& first = a) {
    context->ClearState();
    bind(first);
    const Snap snapA = snap();
    Counting::sets = 0;
    const auto scribble = [] {
        context->ClearState();
        bind(b);
        context->Draw(3, 0);
    };
    if (plainContext) {  // the guard asks for D3D11.1 itself (ranged buffers)
        const d3s::Guard<Counting> guard(context.Get(), spec);
        scribble();
    } else {  // the D3D11.1 context, as the SCS games pass theirs
        const d3s::Guard<Counting> guard(context1.Get(), spec);
        scribble();
    }
    const Snap got = snap();
    const char* piece = "";
    char what[256];
    const bool ok = as_expected(got, snapA, snapB, spec, piece);
    if (ok)
        std::snprintf(what, sizeof(what), "%s: put back, everything else left as drawn (%d setters)", name, Counting::sets);
    else
        std::snprintf(what, sizeof(what), "%s: %s not as expected", name, piece);
    check(ok, what);
    return Counting::sets;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 &context)) ||
        FAILED(context.As(&context1))) {
        std::puts("no WARP D3D11.1 device");
        return 2;
    }
    if (!compile("vs_main", "vs_5_0"))
        return 2;
    make(a, false);
    make(b, true);
    context->ClearState();
    bind(b);
    snapB = snap();
    {
        context->ClearState();
        bind(a);
        const Snap snapA = snap();
        check(snapA.vs != snapB.vs && snapA.blend != snapB.blend && snapA.samp[1][0] != snapB.samp[1][0] && snapA.gs &&
                  !snapB.gs && snapA.omUav[1] && snapA.first[1][1] == 16 && snapA.num[1][1] == 32 && snapA.viewports == 3 &&
                  snapA.predicate && snapA.predicateValue && snapA.layout != snapB.layout && snapA.raster != snapB.raster,
              "states A and B differ in every part (A: a ranged b1, an output merger UAV at u1, GS/HS/DS, 3 viewports)");
    }
    context->ClearState();
    context->Flush();
    std::vector<ULONG> before;
    for (IUnknown* u : objects)
        before.push_back(refs(u));

    // Each part alone.
    const struct {
        const char* name;
        uint32_t part;
    } parts[] = {{"input layout", d3s::kInputLayout}, {"vertex/index buffers", d3s::kIABuffers}, {"VS", d3s::kVS},
                 {"PS", d3s::kPS}, {"CS", d3s::kCS}, {"GS/HS/DS", d3s::kGsHsDs}, {"render targets", d3s::kOMTargets},
                 {"targets and UAVs", d3s::kOMUavs}, {"blend", d3s::kBlend}, {"depth-stencil", d3s::kDepth},
                 {"rasterizer", d3s::kRSState}, {"viewports", d3s::kViewports}, {"scissors", d3s::kScissors},
                 {"stream-out", d3s::kStreamOut}, {"predicate", d3s::kPredication}};
    for (const auto& p : parts) {
        d3s::Spec s;
        s.parts = p.part;
        run_case(p.name, s);
        if (p.part == d3s::kVS || p.part == d3s::kPS || p.part == d3s::kCS) {
            s.instances = false;  // the shader alone (the SCS games')
            char name[64];
            std::snprintf(name, sizeof(name), "%s without class instances", p.name);
            run_case(name, s);
        }
    }
    // The slot groups, part of a stage's slots and all of them; ranged buffers through either context.
    for (const d3s::Slots r : {d3s::Slots{1, 1}, d3s::Slots{0, 3}, d3s::Slots{0, 64}}) {
        char name[64];
        d3s::Spec s;
        s.vsCb = s.psCb = s.csCb = r;
        std::snprintf(name, sizeof(name), "constant buffers %u+%u (D3D11.1 context)", r.first, r.count);
        run_case(name, s);
        std::snprintf(name, sizeof(name), "constant buffers %u+%u (plain context)", r.first, r.count);
        run_case(name, s, true);
        s = {};
        s.vsSrv = s.psSrv = s.csSrv = r;
        std::snprintf(name, sizeof(name), "shader resources %u+%u", r.first, r.count);
        run_case(name, s);
        s = {};
        s.vsSamplers = s.psSamplers = s.csSamplers = r;
        std::snprintf(name, sizeof(name), "samplers %u+%u", r.first, r.count);
        run_case(name, s);
        s = {};
        s.csUavs = r;
        std::snprintf(name, sizeof(name), "compute UAVs %u+%u", r.first, r.count);
        run_case(name, s);
    }
    // DLSS's (dlss_views.cpp full_state): everything, its slots, unbound first.
    {
        d3s::Spec s;
        s.parts = d3s::kAll;
        s.vsCb = s.psCb = s.csCb = {0, kCbs};
        s.vsSrv = s.psSrv = s.csSrv = {0, kSrvs};
        s.vsSamplers = s.psSamplers = s.csSamplers = {0, kSamps};
        s.csUavs = {0, kUavs};
        s.unbindFirst = true;
        // 2 at the start (the predicate and stream-out cleared); then 5 unbinds, IA 4, shaders 6, buffers 3, samplers
        // 3, RS 3, blend and depth, OM, CS UAVs, SRVs 3, stream-out, the predicate: as its own guard made.
        check(run_case("everything, unbound first (DLSS's)", s, true) == 35, "DLSS's: 35 setters, as its own guard made");
        s.unbindFirst = false;
        check(run_case("everything, not unbound first", s) == 30, "everything not unbound first: the 5 unbinds fewer");
    }
    // An SCS blit's (corner_focus stretch_into_twin): 15 setters; the game's 0 viewports and scissors left unset
    // (emptyRects off: 13 setters), or set back as 0 (on: 15).
    {
        d3s::Spec s;
        s.parts = d3s::kInputLayout | d3s::kVS | d3s::kPS | d3s::kGsHsDs | d3s::kOMTargets | d3s::kBlend | d3s::kDepth |
                  d3s::kRSState | d3s::kViewports | d3s::kScissors;
        s.psSrv = s.psSamplers = {0, 1};
        s.instances = false;
        s.emptyRects = false;
        check(run_case("an SCS blit's", s) == 15, "an SCS blit's: 15 setters, as its own code made");
        Full none = a;
        none.viewports = none.scissors = 0;
        check(run_case("an SCS blit's, the game with 0 viewports and scissors", s, false, none) == 13,
              "0 viewports and scissors, emptyRects off: RSSetViewports and RSSetScissorRects not called (13 setters)");
        s.emptyRects = true;
        check(run_case("0 viewports and scissors, emptyRects on", s, false, none) == 15,
              "0 viewports and scissors, emptyRects on: both set back as 0 (15 setters)");
    }
    // LMU's final blit's (focus_post.cpp blit_final): the PS with instances, t0, b0 ranged through a plain context.
    {
        d3s::Spec s;
        s.parts = d3s::kPS | d3s::kViewports | d3s::kScissors;
        s.psSrv = s.psCb = {0, 1};
        check(run_case("LMU's final blit's", s, true) == 5, "LMU's final blit's: 5 setters, as its own code made");
    }

    context->ClearState();
    context->Flush();
    bool same = true;
    for (size_t i = 0; i < objects.size(); ++i)
        same = same && refs(objects[i]) == before[i];
    char what[128];
    std::snprintf(what, sizeof(what), "every object's reference count as before the guards (%zu objects)", objects.size());
    check(same, what);
    return test_result();
}

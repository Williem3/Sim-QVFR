// Offline check of the second-view vertex shader edits (engine/dxbc_patch.h) on a WARP device: shaders compiled here
// are converted, created, and their outputs streamed out. add_second_view (NVIDIA's): SV_Position exactly as before,
// NV_XYZW_RIGHT the clip remap of it, NV_VIEWPORT_MASK its value. add_instanced_view (any GPU): drawn with twice the
// instances, view 0 the original's SV_Position bit for bit in viewport 0, view 1 its remap in viewport 1, the game's own
// instance kept; one rasterized pair on a debug device. The checksum is checked against the compiler's.
//
//   sim_qvfr_tests dxbc_patch_test [folder of a game's VS_*.dxbc]   (also every shader there both ways; the game's code: local only)
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/dxbc_patch.h"

using Microsoft::WRL::ComPtr;
namespace dxbc = simqvfr::dxbc;

namespace {

int failures = 0;
D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_WARP;  // --hardware: this PC's GPU

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::vector<uint8_t> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, "main", profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        std::printf("     compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return {};
    }
    const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {p, p + code->GetBufferSize()};
}

// A matrix in b0, the position from it; position at o0, or o1 (`late`) after a texcoord; `pieces`: xy and zw apart.
const char* kShader = R"(
cbuffer C : register(b0) { float4x4 m; float4 k; }
struct Out {
#ifdef LATE
    float4 t : TEXCOORD0;
#endif
    float4 pos : SV_Position;
#ifndef LATE
    float4 t : TEXCOORD0;
#endif
};
Out main(float4 p : POSITION) {
    Out o;
    float4 c = mul(p, m);
#ifdef PIECES
    o.pos.xy = c.xy * k.x;
    o.pos.zw = c.zw + k.y;
#else
    o.pos = c;
#endif
    o.t = p * 2;
    return o;
}
)";

const char* kPassThrough = "float4 main(float4 p : POSITION) : SV_Position { return p; }";

std::string with_defines(const char* defines) { return std::string(defines) + kShader; }

// N texcoord outputs after the position (N + 1 registers).
std::string many_outputs(int n) {
    std::string s = "struct Out { float4 pos : SV_Position;";
    for (int i = 0; i < n; ++i)
        s += " float4 t" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";";
    s += " }; Out main(float4 p : POSITION) { Out o; o.pos = p;";
    for (int i = 0; i < n; ++i)
        s += " o.t" + std::to_string(i) + " = p * " + std::to_string(i + 2) + ";";
    return s + " return o; }";
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

struct Streamed {
    bool ok = false;
    float pos[3][4] = {}, right[3][4] = {};
    uint32_t mask[3] = {};
};

// Draws three points through `vs` with stream output of its position, and for a converted shader the second view's
// position and the mask; returns what was written.
Streamed stream(Gpu& g, const std::vector<uint8_t>& vs, bool converted, const float (&m)[16], const float (&k)[4],
                const float (&remap)[4], const float (&points)[3][4]) {
    Streamed s;
    ComPtr<ID3D11VertexShader> shader;
    if (FAILED(g.device->CreateVertexShader(vs.data(), vs.size(), nullptr, &shader)))
        return s;
    std::vector<D3D11_SO_DECLARATION_ENTRY> decl = {{0, "SV_POSITION", 0, 0, 4, 0}};
    UINT stride = 16;
    if (converted) {
        decl.push_back({0, dxbc::kRightSemantic, 0, 0, 4, 0});
        decl.push_back({0, dxbc::kMaskSemantic, 0, 0, 1, 0});
        stride = 36;
    }
    ComPtr<ID3D11GeometryShader> so;
    const HRESULT hr = g.device->CreateGeometryShaderWithStreamOutput(vs.data(), vs.size(), decl.data(),
                                                                      static_cast<UINT>(decl.size()), &stride, 1,
                                                                      D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &so);
    if (FAILED(hr)) {
        std::printf("     stream output shader not created (0x%08lx, %s)\n", static_cast<unsigned long>(hr),
                    converted ? "converted" : "original");
        char dump[MAX_PATH] = {};
        if (GetEnvironmentVariableA("DXBC_PATCH_DUMP", dump, MAX_PATH)) {  // the shader, for D3DDisassemble
            FILE* f = nullptr;
            if (fopen_s(&f, dump, "wb") == 0 && f) {
                std::fwrite(vs.data(), 1, vs.size(), f);
                std::fclose(f);
            }
        }
        return s;
    }
    const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                              D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    if (FAILED(g.device->CreateInputLayout(&element, 1, vs.data(), vs.size(), &layout)))
        return s;
    D3D11_BUFFER_DESC bd = {sizeof(points), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA init = {points, 0, 0};
    ComPtr<ID3D11Buffer> vb, cb, remapCb, out, staging;
    float constants[20];
    std::memcpy(constants, m, sizeof(m));
    std::memcpy(constants + 16, k, sizeof(k));
    const D3D11_SUBRESOURCE_DATA cInit = {constants, 0, 0}, rInit = {remap, 0, 0};
    D3D11_BUFFER_DESC cd = {sizeof(constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC rd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC od = {stride * 3, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
    D3D11_BUFFER_DESC sd = {stride * 3, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    if (FAILED(g.device->CreateBuffer(&bd, &init, &vb)) || FAILED(g.device->CreateBuffer(&cd, &cInit, &cb)) ||
        FAILED(g.device->CreateBuffer(&rd, &rInit, &remapCb)) || FAILED(g.device->CreateBuffer(&od, nullptr, &out)) ||
        FAILED(g.device->CreateBuffer(&sd, nullptr, &staging)))
        return s;
    const UINT vbStride = 16, offset = 0;
    g.context->IASetInputLayout(layout.Get());
    g.context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &vbStride, &offset);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.context->VSSetShader(shader.Get(), nullptr, 0);
    g.context->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.context->VSSetConstantBuffers(13, 1, remapCb.GetAddressOf());
    g.context->GSSetShader(so.Get(), nullptr, 0);
    g.context->PSSetShader(nullptr, nullptr, 0);
    g.context->SOSetTargets(1, out.GetAddressOf(), &offset);
    g.context->Draw(3, 0);
    ID3D11Buffer* none = nullptr;
    g.context->SOSetTargets(1, &none, &offset);
    g.context->CopyResource(staging.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return s;
    for (int v = 0; v < 3; ++v) {
        const auto* row = reinterpret_cast<const uint8_t*>(map.pData) + v * stride;
        std::memcpy(s.pos[v], row, 16);
        if (converted) {
            std::memcpy(s.right[v], row + 16, 16);
            std::memcpy(&s.mask[v], row + 32, 4);
        }
    }
    g.context->Unmap(staging.Get(), 0);
    s.ok = true;
    return s;
}

bool close(float a, float b) { return std::fabs(a - b) <= 1e-5f * (1 + std::fabs(a)); }

// One shader through the edit and the stream output, against the unconverted one.
void run_case(Gpu& g, const char* name, const std::vector<uint8_t>& original, uint32_t expectedPositionRegister) {
    char what[160];
    if (original.empty()) {
        std::snprintf(what, sizeof(what), "%s: compiled", name);
        check(false, what);
        return;
    }
    const dxbc::SecondView sv = dxbc::add_second_view(original.data(), original.size());
    std::snprintf(what, sizeof(what), "%s: converted (SV_Position o%u, %d writes; right o%u, mask o%u)%s%s", name,
                  sv.positionRegister, sv.positionWrites, sv.rightRegister, sv.maskRegister,
                  sv.error.empty() ? "" : ": ", sv.error.c_str());
    check(!sv.bytecode.empty() && sv.positionRegister == expectedPositionRegister, what);
    if (sv.bytecode.empty())
        return;
    std::snprintf(what, sizeof(what), "%s: the converted container's checksum", name);
    check(dxbc::checksum_ok(sv.bytecode.data(), sv.bytecode.size()), what);

    const float m[16] = {1.5f, 0.1f, 0, 0, 0.2f, 1.25f, 0, 0, 0.3f, -0.4f, 1.01f, 1, 0.5f, 0.25f, -0.1f, 0};
    const float k[4] = {0.75f, 0.125f, 0, 0};
    const float remap[4] = {2.5f, -0.5f, 3.0f, 0.75f};  // a, b, c, d
    const float points[3][4] = {{0.1f, 0.2f, 0.9f, 1}, {-0.7f, 0.4f, 2.5f, 1}, {0.3f, -0.6f, 7.0f, 1}};
    const Streamed before = stream(g, original, false, m, k, remap, points);
    const Streamed after = stream(g, sv.bytecode, true, m, k, remap, points);
    std::snprintf(what, sizeof(what), "%s: both created and streamed out", name);
    check(before.ok && after.ok, what);
    if (!before.ok || !after.ok)
        return;
    bool samePosition = true, rightOk = true, masksOk = true;
    for (int v = 0; v < 3; ++v) {
        samePosition = samePosition && std::memcmp(before.pos[v], after.pos[v], 16) == 0;
        const float* p = after.pos[v];
        const float expect[4] = {remap[0] * p[0] + remap[1] * p[3], remap[2] * p[1] + remap[3] * p[3], p[2], p[3]};
        for (int c = 0; c < 4; ++c)
            rightOk = rightOk && close(after.right[v][c], expect[c]);
        masksOk = masksOk && after.mask[v] == 0x00020001;
    }
    std::snprintf(what, sizeof(what), "%s: SV_Position bit-identical to the original's", name);
    check(samePosition, what);
    std::snprintf(what, sizeof(what), "%s: NV_XYZW_RIGHT = (a x + b w, c y + d w, z, w); e.g. %.4f %.4f %.4f %.4f", name,
                  after.right[1][0], after.right[1][1], after.right[1][2], after.right[1][3]);
    check(rightOk, what);
    std::snprintf(what, sizeof(what), "%s: NV_VIEWPORT_MASK 0x00020001 (view 0 in viewport 0, view 1 in 1)", name);
    check(masksOk, what);
    const dxbc::SecondView again = dxbc::add_second_view(sv.bytecode.data(), sv.bytecode.size());
    std::snprintf(what, sizeof(what), "%s: a converted shader isn't converted again", name);
    check(again.bytecode.empty() && again.error == "already converted", what);
}

// ---- add_instanced_view: the vendor-neutral two views (instancing and SV_ViewportArrayIndex) ----

// What a draw streamed out: per vertex in draw order (instance by instance) its SV_Position, its viewport index (a
// converted shader's) and one more float4 output (`extra`, e.g. TEXCOORD) if asked.
struct Out {
    bool ok = false;
    std::vector<std::array<float, 4>> pos, extra;
    std::vector<uint32_t> vp;
};

struct DrawSpec {
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    std::vector<uint8_t> vertexData;  // slot 0
    UINT vertexStride = 16;
    std::vector<uint8_t> instanceData;  // slot 1 (per-instance elements), empty: none
    UINT instanceStride = 16;
    UINT vertices = 3, instances = 1;
    std::vector<ComPtr<ID3D11Buffer>> cbs;  // slots 0.., the remap at 13 if given
    const char* extra = nullptr;            // a float4 output to stream too
};

Out stream_out(Gpu& g, const std::vector<uint8_t>& vs, const DrawSpec& d, bool viewportIndex) {
    Out o;
    std::vector<D3D11_SO_DECLARATION_ENTRY> decl = {{0, "SV_POSITION", 0, 0, 4, 0}};
    UINT stride = 16;
    if (d.extra) {
        decl.push_back({0, d.extra, 0, 0, 4, 0});
        stride += 16;
    }
    if (viewportIndex) {
        decl.push_back({0, "SV_ViewportArrayIndex", 0, 0, 1, 0});
        stride += 4;
    }
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11GeometryShader> so;
    ComPtr<ID3D11InputLayout> layout;
    HRESULT hr = g.device->CreateVertexShader(vs.data(), vs.size(), nullptr, &shader);
    if (SUCCEEDED(hr))
        hr = g.device->CreateGeometryShaderWithStreamOutput(vs.data(), vs.size(), decl.data(),
                                                            static_cast<UINT>(decl.size()), &stride, 1,
                                                            D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &so);
    if (SUCCEEDED(hr) && !d.elements.empty())
        hr = g.device->CreateInputLayout(d.elements.data(), static_cast<UINT>(d.elements.size()), vs.data(), vs.size(),
                                         &layout);
    if (FAILED(hr)) {
        std::printf("     not created (0x%08lx)\n", static_cast<unsigned long>(hr));
        return o;
    }
    const UINT total = d.vertices * d.instances;
    ComPtr<ID3D11Buffer> vb, ib, out, staging;
    const auto buffer = [&](const std::vector<uint8_t>& bytes, ComPtr<ID3D11Buffer>& b) {
        if (bytes.empty())
            return true;
        D3D11_BUFFER_DESC bd = {static_cast<UINT>(bytes.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA init = {bytes.data(), 0, 0};
        return SUCCEEDED(g.device->CreateBuffer(&bd, &init, &b));
    };
    D3D11_BUFFER_DESC od = {stride * total, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
    D3D11_BUFFER_DESC sd = {stride * total, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    if (!buffer(d.vertexData, vb) || !buffer(d.instanceData, ib) || FAILED(g.device->CreateBuffer(&od, nullptr, &out)) ||
        FAILED(g.device->CreateBuffer(&sd, nullptr, &staging)))
        return o;
    ID3D11Buffer* vbs[2] = {vb.Get(), ib.Get()};
    const UINT strides[2] = {d.vertexStride, d.instanceStride}, offsets[2] = {0, 0}, zero = 0;
    g.context->IASetInputLayout(layout.Get());
    g.context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.context->VSSetShader(shader.Get(), nullptr, 0);
    std::vector<ID3D11Buffer*> cbs;
    for (const ComPtr<ID3D11Buffer>& b : d.cbs)
        cbs.push_back(b.Get());
    if (!cbs.empty())
        g.context->VSSetConstantBuffers(0, static_cast<UINT>(cbs.size()), cbs.data());
    g.context->GSSetShader(so.Get(), nullptr, 0);
    g.context->PSSetShader(nullptr, nullptr, 0);
    g.context->SOSetTargets(1, out.GetAddressOf(), &zero);
    if (d.instances == 1)
        g.context->Draw(d.vertices, 0);
    else
        g.context->DrawInstanced(d.vertices, d.instances, 0, 0);
    ID3D11Buffer* none = nullptr;
    g.context->SOSetTargets(1, &none, &zero);
    g.context->GSSetShader(nullptr, nullptr, 0);
    g.context->CopyResource(staging.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return o;
    for (UINT v = 0; v < total; ++v) {
        const auto* row = static_cast<const uint8_t*>(map.pData) + v * stride;
        std::array<float, 4> p{}, e{};
        std::memcpy(p.data(), row, 16);
        o.pos.push_back(p);
        if (d.extra) {
            std::memcpy(e.data(), row + 16, 16);
            o.extra.push_back(e);
        }
        if (viewportIndex) {
            uint32_t vp = 0;
            std::memcpy(&vp, row + stride - 4, 4);
            o.vp.push_back(vp);
        }
    }
    g.context->Unmap(staging.Get(), 0);
    o.ok = true;
    return o;
}

ComPtr<ID3D11Buffer> constant_buffer(Gpu& g, const void* data, UINT size) {
    D3D11_BUFFER_DESC bd = {size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
    ComPtr<ID3D11Buffer> b;
    g.device->CreateBuffer(&bd, &init, &b);
    return b;
}

template <class T>
std::vector<uint8_t> bytes_of(const T* p, size_t n) {
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    return {b, b + n * sizeof(T)};
}

bool same_or_nan(float a, float b) { return (std::isnan(a) && std::isnan(b)) || close(a, b); }

// Instance by instance of the original (o, `instances` of them) against the converted (c, twice as many): view 0 of
// each the original's position bit for bit and viewport 0, view 1 its remap and viewport 1; `extra` equal too.
bool views_match(const Out& o, const Out& c, UINT vertices, UINT instances, const float (&remap)[4], bool extra,
                 std::string* why = nullptr) {
    if (!o.ok || !c.ok || c.pos.size() != o.pos.size() * 2 || c.vp.size() != c.pos.size())
        return false;
    for (UINT i = 0; i < instances; ++i)
        for (UINT view = 0; view < 2; ++view)
            for (UINT v = 0; v < vertices; ++v) {
                const std::array<float, 4>& p = o.pos[i * vertices + v];
                const size_t at = (static_cast<size_t>(i) * 2 + view) * vertices + v;
                const std::array<float, 4>& q = c.pos[at];
                bool ok = c.vp[at] == view;
                if (view == 0) {
                    ok = ok && std::memcmp(p.data(), q.data(), 16) == 0;
                } else {
                    const float expect[4] = {remap[0] * p[0] + remap[1] * p[3], remap[2] * p[1] + remap[3] * p[3], p[2], p[3]};
                    for (int k = 0; k < 4; ++k)
                        ok = ok && same_or_nan(q[k], expect[k]);
                }
                if (extra)
                    ok = ok && std::memcmp(o.extra[i * vertices + v].data(), c.extra[at].data(), 16) == 0;
                if (!ok) {
                    if (why) {
                        char s[200];
                        std::snprintf(s, sizeof(s), "instance %u view %u vertex %u: %g %g %g %g (vp %u) for %g %g %g %g", i,
                                      view, v, q[0], q[1], q[2], q[3], c.vp[at], p[0], p[1], p[2], p[3]);
                        *why = s;
                    }
                    return false;
                }
            }
    return true;
}

// One compiled shader both ways: converted, checksum, original x1 against converted x2 (the kShader inputs).
void instanced_case(Gpu& g, const char* name, const std::vector<uint8_t>& original, uint32_t expectedPositionRegister) {
    char what[200];
    const dxbc::InstancedView iv = dxbc::add_instanced_view(original.data(), original.size());
    std::snprintf(what, sizeof(what), "instanced %s: converted (SV_Position o%u, %d writes; viewport index o%u, instance v%u)%s%s",
                  name, iv.positionRegister, iv.positionWrites, iv.viewportRegister, iv.instanceRegister,
                  iv.error.empty() ? "" : ": ", iv.error.c_str());
    check(!iv.bytecode.empty() && iv.positionRegister == expectedPositionRegister && !iv.hadInstance, what);
    if (iv.bytecode.empty())
        return;
    std::snprintf(what, sizeof(what), "instanced %s: checksum", name);
    check(dxbc::checksum_ok(iv.bytecode.data(), iv.bytecode.size()), what);
    const float constants[20] = {1.5f, 0.1f, 0, 0, 0.2f, 1.25f, 0, 0, 0.3f, -0.4f, 1.01f, 1, 0.5f, 0.25f, -0.1f, 0,
                                 0.75f, 0.125f, 0, 0};
    const float remap[4] = {2.5f, -0.5f, 3.0f, 0.75f};
    const float points[3][4] = {{0.1f, 0.2f, 0.9f, 1}, {-0.7f, 0.4f, 2.5f, 1}, {0.3f, -0.6f, 7.0f, 1}};
    DrawSpec d;
    d.elements = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    d.vertexData = bytes_of(&points[0][0], 12);
    d.cbs.resize(14);
    d.cbs[0] = constant_buffer(g, constants, sizeof(constants));
    d.cbs[13] = constant_buffer(g, remap, sizeof(remap));
    const Out o = stream_out(g, original, d, false);
    d.instances = 2;
    const Out c = stream_out(g, iv.bytecode, d, true);
    std::string why;
    std::snprintf(what, sizeof(what), "instanced %s: two instances: view 0 the original's position (viewport 0), view 1 its "
                  "remap (viewport 1)", name);
    check(views_match(o, c, 3, 1, remap, false, &why), (std::string(what) + (why.empty() ? "" : ": " + why)).c_str());
    const dxbc::InstancedView again = dxbc::add_instanced_view(iv.bytecode.data(), iv.bytecode.size());
    std::snprintf(what, sizeof(what), "instanced %s: a converted shader isn't converted again", name);
    check(again.bytecode.empty() && !again.error.empty(), what);
}

void instanced_cases(Gpu& g, const std::vector<uint8_t>& plain, const std::vector<uint8_t>& pixel) {
    char what[200];
    D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
    const bool feature = SUCCEEDED(g.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3))) &&
                         o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
    check(feature, "the device lets a vertex shader pick the viewport (D3D11_OPTIONS3 VPAndRTArrayIndexFromAnyShaderFeedingRasterizer)");
    instanced_case(g, "vs_4_0", plain, 0);
    instanced_case(g, "vs_5_0", compile(with_defines("").c_str(), "vs_5_0"), 0);
    instanced_case(g, "vs_4_0 position in o1", compile(with_defines("#define LATE\n").c_str(), "vs_4_0"), 1);
    instanced_case(g, "vs_4_0 position in pieces", compile(with_defines("#define PIECES\n").c_str(), "vs_4_0"), 0);
    instanced_case(g, "vs_5_0 position in pieces", compile(with_defines("#define PIECES\n").c_str(), "vs_5_0"), 0);
    instanced_case(g, "vs_4_0 no temps", compile(kPassThrough, "vs_4_0"), 0);
    instanced_case(g, "vs_4_0 15 outputs (one register free)", compile(many_outputs(14).c_str(), "vs_4_0"), 0);
    {
        const std::vector<uint8_t> full = compile(many_outputs(15).c_str(), "vs_4_0");
        const dxbc::InstancedView none = dxbc::add_instanced_view(full.data(), full.size());
        check(!full.empty() && none.bytecode.empty() && none.error == "no free output register",
              "instanced vs_4_0 16 outputs: refused (no register free)");
        const dxbc::InstancedView ps = dxbc::add_instanced_view(pixel.data(), pixel.size());
        check(ps.bytecode.empty() && !ps.error.empty(), "instanced: a pixel shader is refused");
        const std::vector<uint8_t> picks = compile(
            "struct O { float4 p : SV_Position; uint vp : SV_ViewportArrayIndex; }; O main(float4 p : POSITION) { O o; "
            "o.p = p; o.vp = 1; return o; }",
            "vs_5_0");
        const dxbc::InstancedView own = dxbc::add_instanced_view(picks.data(), picks.size());
        check(!picks.empty() && own.bytecode.empty(), "instanced: a shader that picks its viewport itself is refused");
    }

    // A shader that reads SV_InstanceID: it gets the game's own instance (id >> 1) in both views.
    {
        const char* source = R"(
struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };
O main(float4 p : POSITION, uint id : SV_InstanceID) {
    O o; o.pos = p + float4(0.25 * id, -0.125 * id, 0, 0); o.t = float4(id, id * 2 + 1, 0, 1); return o; })";
        for (const char* profile : {"vs_4_0", "vs_5_0"}) {
            const std::vector<uint8_t> original = compile(source, profile);
            const dxbc::InstancedView iv = dxbc::add_instanced_view(original.data(), original.size());
            std::snprintf(what, sizeof(what), "instanced %s reading SV_InstanceID: converted (v%u, %d reads)%s%s", profile,
                          iv.instanceRegister, iv.instanceReads, iv.error.empty() ? "" : ": ", iv.error.c_str());
            check(!iv.bytecode.empty() && iv.hadInstance && iv.instanceReads > 0, what);
            if (iv.bytecode.empty())
                continue;
            const float remap[4] = {1.5f, 0.25f, 2.0f, -0.5f};
            const float points[3][4] = {{0.1f, 0.2f, 0.5f, 1}, {-0.7f, 0.4f, 0.25f, 2}, {0.3f, -0.6f, 0.75f, 1}};
            DrawSpec d;
            d.elements = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            d.vertexData = bytes_of(&points[0][0], 12);
            d.cbs.resize(14);
            d.cbs[13] = constant_buffer(g, remap, sizeof(remap));
            d.extra = "TEXCOORD";
            d.instances = 3;
            const Out o = stream_out(g, original, d, false);
            d.instances = 6;
            const Out c = stream_out(g, iv.bytecode, d, true);
            std::string why;
            std::snprintf(what, sizeof(what), "instanced %s reading SV_InstanceID: 3 instances drawn as 6: each pair the "
                          "game's instance (its texcoord the same), views 0 and 1", profile);
            check(views_match(o, c, 3, 3, remap, true, &why), (std::string(what) + (why.empty() ? "" : ": " + why)).c_str());
        }
    }

    // Per-instance vertex data: with the step rate doubled in the layout (the caller's part), each pair reads its instance's.
    {
        const char* source = R"(
struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };
O main(float4 p : POSITION, float4 offset : OFFSET) { O o; o.pos = p + offset; o.t = offset; return o; })";
        const std::vector<uint8_t> original = compile(source, "vs_4_0");
        const dxbc::InstancedView iv = dxbc::add_instanced_view(original.data(), original.size());
        if (!iv.bytecode.empty()) {
            const float remap[4] = {2.0f, 0.5f, 1.5f, -0.25f};
            const float points[3][4] = {{0.1f, 0.2f, 0.5f, 1}, {-0.7f, 0.4f, 0.25f, 1}, {0.3f, -0.6f, 0.75f, 1}};
            const float offsets[4][4] = {{0.5f, 0, 0, 0}, {0, 0.5f, 0, 0}, {-0.5f, 0, 0, 0}, {0, -0.25f, 0, 0}};
            DrawSpec d;
            d.elements = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                          {"OFFSET", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
            d.vertexData = bytes_of(&points[0][0], 12);
            d.instanceData = bytes_of(&offsets[0][0], 16);
            d.cbs.resize(14);
            d.cbs[13] = constant_buffer(g, remap, sizeof(remap));
            d.extra = "TEXCOORD";
            d.instances = 4;
            const Out o = stream_out(g, original, d, false);
            d.elements[1].InstanceDataStepRate = 2;
            d.instances = 8;
            const Out c = stream_out(g, iv.bytecode, d, true);
            std::string why;
            check(views_match(o, c, 3, 4, remap, true, &why),
                  ("instanced: per-instance data with the layout's step rate doubled: each pair its instance's data" +
                   (why.empty() ? std::string() : ": " + why))
                      .c_str());
        } else {
            check(false, ("instanced: per-instance shader converted: " + iv.error).c_str());
        }
    }

    // Rasterized on a debug device: a triangle drawn twice as an instanced pair into two viewports of one target, a pixel
    // shader linked to the original's outputs; view 1 lands where the remap puts it; the debug layer has nothing to say.
    {
        Gpu d;
        const bool debug = SUCCEEDED(D3D11CreateDevice(nullptr, driverType, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                                       nullptr, 0, D3D11_SDK_VERSION, &d.device, nullptr, &d.context));
        if (!debug && FAILED(D3D11CreateDevice(nullptr, driverType, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &d.device, nullptr, &d.context))) {
            check(false, "instanced raster: a WARP device");
            return;
        }
        ComPtr<ID3D11InfoQueue> queue;
        if (debug)
            d.device.As(&queue);
        const char* vsSource = R"(
struct O { float4 pos : SV_Position; float4 colour : COLOR0; };
O main(float4 p : POSITION) { O o; o.pos = p; o.colour = float4(1, 0.5, 0.25, 1); return o; })";
        const char* psSource = "float4 main(float4 pos : SV_Position, float4 colour : COLOR0) : SV_Target { return colour; }";
        const std::vector<uint8_t> vsCode = compile(vsSource, "vs_4_0"), psCode = compile(psSource, "ps_4_0");
        const dxbc::InstancedView iv = dxbc::add_instanced_view(vsCode.data(), vsCode.size());
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                                  D3D11_INPUT_PER_VERTEX_DATA, 0};
        // The layout made with the game's original shader (as the game makes it), used with the converted one.
        const bool made = !iv.bytecode.empty() &&
                          SUCCEEDED(d.device->CreateVertexShader(iv.bytecode.data(), iv.bytecode.size(), nullptr, &vs)) &&
                          SUCCEEDED(d.device->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps)) &&
                          SUCCEEDED(d.device->CreateInputLayout(&element, 1, vsCode.data(), vsCode.size(), &layout));
        check(made, "instanced raster: the converted shader created, the layout made with the original's bytecode");
        if (!made)
            return;
        const UINT w = 64, h = 32;
        D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ComPtr<ID3D11Texture2D> target, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        d.device->CreateTexture2D(&td, nullptr, &target);
        d.device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.device->CreateTexture2D(&td, nullptr, &staging);
        // A small triangle around the eye's centre; the remap moves it right by half the focus viewport (b = 0.5 w).
        const float tri[3][4] = {{-0.25f, -0.25f, 0.5f, 1}, {0.0f, 0.25f, 0.5f, 1}, {0.25f, -0.25f, 0.5f, 1}};
        const float remap[4] = {1.0f, 0.5f, 1.0f, 0.0f};
        D3D11_BUFFER_DESC bd = {sizeof(tri), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA init = {tri, 0, 0};
        ComPtr<ID3D11Buffer> vb;
        d.device->CreateBuffer(&bd, &init, &vb);
        Gpu& dg = d;
        ComPtr<ID3D11Buffer> rb = constant_buffer(dg, remap, sizeof(remap));
        const D3D11_VIEWPORT vps[2] = {{0, 0, 32, 32, 0, 1}, {32, 0, 32, 32, 0, 1}};
        const float clear[4] = {0, 0, 0, 0};
        const UINT stride = 16, offset = 0;
        d.context->ClearRenderTargetView(rtv.Get(), clear);
        d.context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        d.context->RSSetViewports(2, vps);
        d.context->IASetInputLayout(layout.Get());
        d.context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.context->VSSetShader(vs.Get(), nullptr, 0);
        d.context->VSSetConstantBuffers(13, 1, rb.GetAddressOf());
        d.context->PSSetShader(ps.Get(), nullptr, 0);
        d.context->DrawInstanced(3, 2, 0, 0);
        d.context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE map;
        uint32_t px[h][w] = {};
        if (SUCCEEDED(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            for (UINT y = 0; y < h; ++y)
                std::memcpy(px[y], static_cast<const uint8_t*>(map.pData) + y * map.RowPitch, w * 4);
            d.context->Unmap(staging.Get(), 0);
        }
        // View 0: centred in 0..32 (x 12..20 at mid height); view 1: moved by 0.5 NDC = 8 px: 32 + 16 + 8 = x 52-ish.
        const uint32_t lit = px[18][16], left1 = px[18][32 + 16], right1 = px[18][32 + 16 + 8];
        std::snprintf(what, sizeof(what), "instanced raster: view 0 at the eye's centre (%08x), view 1 in viewport 1 moved by "
                      "the remap (%08x there, %08x where it would be unmoved)", lit, right1, left1);
        const auto colour = [](uint32_t c) {  // (1, 0.5, 0.25, 1) within a step a channel (GPUs round 0.5 either way)
            const int expect[4] = {0xff, 0x80, 0x40, 0xff};
            for (int k = 0; k < 4; ++k)
                if (std::abs(static_cast<int>((c >> (8 * k)) & 0xff) - expect[k]) > 1)
                    return false;
            return true;
        };
        check(colour(lit) && colour(right1) && left1 == 0 && px[18][8] == 0, what);
        if (queue) {
            UINT64 errors = 0;
            const UINT64 n = queue->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                queue->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> buf(len);
                auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
                if (SUCCEEDED(queue->GetMessage(i, m, &len)) &&
                    (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                    ++errors;
                    std::printf("     debug layer: %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
                }
            }
            std::snprintf(what, sizeof(what), "instanced raster: the debug layer reports no errors (%llu)",
                          static_cast<unsigned long long>(errors));
            check(errors == 0, what);
        } else {
            std::printf("     (no debug layer on this machine: linkage not checked)\n");
        }
    }
}

// A game's vertex shaders from a folder (VS_*.dxbc): each converted, created, and drawn both ways with inputs made from
// its input signature and pseudo-random vertex and constant data; view 0 must be its own position bit for bit.
void real_shaders(Gpu& g, const char* folder) {
    namespace fs = std::filesystem;
    unsigned files = 0, converted = 0, created = 0, matched = 0, skipped = 0;
    std::map<std::string, unsigned> refused;
    std::vector<std::string> mismatches;
    uint32_t seed = 12345;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    std::vector<float> constants(4096 * 4);
    for (float& f : constants)
        f = static_cast<float>(next() % 2000) / 1000.0f - 1.0f;
    const float remap[4] = {3.2f, -0.4f, 1.7f, 0.2f};
    std::vector<ComPtr<ID3D11Buffer>> cbs(14);
    for (int s = 0; s < 13; ++s)
        cbs[s] = constant_buffer(g, constants.data(), static_cast<UINT>(constants.size() * 4));
    cbs[13] = constant_buffer(g, remap, sizeof(remap));
    for (const auto& entry : fs::directory_iterator(folder)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("VS_", 0) != 0 || entry.path().extension() != ".dxbc")
            continue;
        ++files;
        std::vector<uint8_t> code;
        if (FILE* f = nullptr; _wfopen_s(&f, entry.path().c_str(), L"rb") == 0 && f) {
            std::fseek(f, 0, SEEK_END);
            code.resize(static_cast<size_t>(std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            code.resize(std::fread(code.data(), 1, code.size(), f));
            std::fclose(f);
        }
        const dxbc::InstancedView iv = dxbc::add_instanced_view(code.data(), code.size());
        if (iv.bytecode.empty()) {
            ++refused[iv.error];
            continue;
        }
        ++converted;
        ComPtr<ID3D11VertexShader> vs;
        if (FAILED(g.device->CreateVertexShader(iv.bytecode.data(), iv.bytecode.size(), nullptr, &vs))) {
            mismatches.push_back(name + ": not created");
            continue;
        }
        ++created;
        // Inputs from the original's signature: one buffer, a 16-byte element each, float / uint / sint by its type.
        ComPtr<ID3D11ShaderReflection> reflection;
        if (FAILED(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)))) {
            ++skipped;
            continue;
        }
        D3D11_SHADER_DESC sd{};
        reflection->GetDesc(&sd);
        DrawSpec d;
        std::vector<std::string> names(sd.InputParameters);
        UINT offset = 0;
        std::vector<uint8_t> kinds;
        for (UINT i = 0; i < sd.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            reflection->GetInputParameterDesc(i, &p);
            if (p.SystemValueType != D3D_NAME_UNDEFINED)
                continue;
            names[i] = p.SemanticName;
            const DXGI_FORMAT format = p.ComponentType == D3D_REGISTER_COMPONENT_UINT32   ? DXGI_FORMAT_R32G32B32A32_UINT
                                       : p.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                                                                                          : DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.elements.push_back({names[i].c_str(), p.SemanticIndex, format, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
            kinds.push_back(format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 0 : 1);
            offset += 16;
        }
        d.vertexStride = offset ? offset : 16;
        d.vertices = 3;
        d.vertexData.resize(static_cast<size_t>(d.vertexStride) * d.vertices);
        for (UINT v = 0; v < d.vertices; ++v)
            for (size_t e = 0; e < kinds.size(); ++e)
                for (int k = 0; k < 4; ++k) {
                    uint8_t* at = d.vertexData.data() + v * d.vertexStride + e * 16 + k * 4;
                    if (kinds[e]) {
                        const uint32_t u = next() % 4;  // indices kept small
                        std::memcpy(at, &u, 4);
                    } else {
                        const float f = k == 3 ? 1.0f : static_cast<float>(next() % 2000) / 1000.0f - 1.0f;
                        std::memcpy(at, &f, 4);
                    }
                }
        d.cbs = cbs;
        const Out o = stream_out(g, code, d, false);
        d.instances = 2;
        const Out c = stream_out(g, iv.bytecode, d, true);
        std::string why;
        if (views_match(o, c, 3, 1, remap, false, &why))
            ++matched;
        else if (mismatches.size() < 8)
            mismatches.push_back(name + (o.ok && c.ok ? ": " + why : ": not streamed"));
    }
    std::string why;
    for (const auto& [reason, n] : refused)
        why += (why.empty() ? "" : ", ") + reason + " x" + std::to_string(n);
    char what[400];
    std::snprintf(what, sizeof(what), "the folder's %u vertex shaders: %u converted (refused: %s), %u created, %u matched "
                  "both views (%u skipped: no reflection)",
                  files, converted, why.empty() ? "none" : why.c_str(), created, matched, skipped);
    for (const std::string& m : mismatches)
        std::printf("     %s\n", m.c_str());
    check(files > 0 && created == converted && matched + skipped == created, what);
}

// ---- add_focus_mip_bias: a pixel shader's samples biased inside a rectangle ----

// A 256x128 texture whose mip k is all one colour (R = 40 k + 20, G = 200 - 40 k), point-filtered, over a 64x32 target:
// four texels a pixel each way, so mip 2 (R 100); with a bias of -1, mip 1 (R 60).
const char* kMipVs = R"(
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O main(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";
const char* kMipPsHead = R"(
Texture2D t : register(t0);
SamplerState s : register(s0);
SamplerComparisonState cs : register(s1);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
)";
uint8_t mip_red(int level) { return static_cast<uint8_t>(40 * level + 20); }

struct MipScene {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> point;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    bool ok = false;
};

MipScene mip_scene(Gpu& g) {
    MipScene m;
    const std::vector<uint8_t> vsCode = compile(kMipVs, "vs_4_0");
    D3D11_TEXTURE2D_DESC td = {256, 128, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE, 0, 0};
    std::vector<std::vector<uint32_t>> levels;
    levels.reserve(16);  // init holds pointers into them
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    for (UINT w = 256, h = 128, level = 0; w || h; w /= 2, h /= 2, ++level) {
        const UINT lw = std::max(w, 1u), lh = std::max(h, 1u);
        const uint32_t green = static_cast<uint32_t>(200 - 20 * static_cast<int>(level)) & 0xff;
        const uint32_t rgba = mip_red(static_cast<int>(level)) | (green << 8) | 0xff000000u;
        levels.emplace_back(size_t{lw} * lh, rgba);
        init.push_back({levels.back().data(), lw * 4, 0});
        if (w <= 1 && h <= 1)
            break;
    }
    td.MipLevels = static_cast<UINT>(init.size());
    ComPtr<ID3D11Texture2D> texture;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_TEXTURE2D_DESC rd = {64, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    if (vsCode.empty() || FAILED(g.device->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &m.vs)) ||
        FAILED(g.device->CreateTexture2D(&td, init.data(), &texture)) ||
        FAILED(g.device->CreateShaderResourceView(texture.Get(), nullptr, &m.srv)) ||
        FAILED(g.device->CreateSamplerState(&sd, &m.point)) || FAILED(g.device->CreateTexture2D(&rd, nullptr, &m.target)) ||
        FAILED(g.device->CreateRenderTargetView(m.target.Get(), nullptr, &m.rtv)))
        return m;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.BindFlags = 0;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    m.ok = SUCCEEDED(g.device->CreateTexture2D(&rd, nullptr, &m.staging));
    return m;
}

// Draws the target full through `ps` with b13 = (rect, (inside, outside)); each pixel's RGBA (64x32), empty when not
// drawn.
std::vector<uint8_t> mip_draw(Gpu& g, MipScene& m, const std::vector<uint8_t>& ps, const float (&rect)[4], float inside,
                              float outside = 0) {
    ComPtr<ID3D11PixelShader> shader;
    if (!m.ok || FAILED(g.device->CreatePixelShader(ps.data(), ps.size(), nullptr, &shader)))
        return {};
    const float constants[8] = {rect[0], rect[1], rect[2], rect[3], inside, outside, 0, 0};
    ComPtr<ID3D11Buffer> cb = constant_buffer(g, constants, sizeof(constants));
    const D3D11_VIEWPORT viewport = {0, 0, 64, 32, 0, 1};
    const float clear[4] = {0, 0, 0, 0};
    g.context->ClearRenderTargetView(m.rtv.Get(), clear);
    g.context->OMSetRenderTargets(1, m.rtv.GetAddressOf(), nullptr);
    g.context->RSSetViewports(1, &viewport);
    g.context->IASetInputLayout(nullptr);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(m.vs.Get(), nullptr, 0);
    g.context->PSSetShader(shader.Get(), nullptr, 0);
    g.context->PSSetShaderResources(0, 1, m.srv.GetAddressOf());
    g.context->PSSetSamplers(0, 1, m.point.GetAddressOf());
    g.context->PSSetConstantBuffers(13, 1, cb.GetAddressOf());
    g.context->Draw(3, 0);
    g.context->CopyResource(m.staging.Get(), m.target.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.context->Map(m.staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return {};
    std::vector<uint8_t> red(64 * 32 * 4);
    for (int y = 0; y < 32; ++y)
        std::memcpy(red.data() + y * 64 * 4, static_cast<const uint8_t*>(map.pData) + y * map.RowPitch, 64 * 4);
    g.context->Unmap(m.staging.Get(), 0);
    return red;
}

// Pixels whose channel `c` isn't `want`, in x0 <= x < x1 (all rows).
int off(const std::vector<uint8_t>& px, int x0, int x1, int c, uint8_t want) {
    int n = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = x0; x < x1; ++x)
            n += px[(y * 64 + x) * 4 + c] != want;
    return n;
}

// One pixel shader: the original everywhere mip 2; converted, mip 1 inside the right half (x >= 32) and mip 2 outside;
// with an empty rectangle, mip 2 everywhere; with -2 inside and -1 outside, mip 0 and mip 1. `levelChannel` (blue): a
// SampleLevel at mip 3 the bias mustn't touch.
void mip_case(Gpu& g, MipScene& m, const char* name, const char* body, const char* profile, int samples, int leftAlone,
              bool levelChannel = false) {
    const std::string source = std::string(kMipPsHead) + body;
    const std::vector<uint8_t> original = compile(source.c_str(), profile);
    const dxbc::FocusMipBias b = dxbc::add_focus_mip_bias(original.data(), original.size());
    const std::string what = std::string("focus mip bias, ") + name;
    if (original.empty() || b.bytecode.empty()) {
        check(false, (what + ": converted (" + b.error + ")").c_str());
        return;
    }
    check(dxbc::checksum_ok(b.bytecode.data(), b.bytecode.size()) && b.samples == samples && b.leftAlone == leftAlone,
          (what + ": converted, checksum, " + std::to_string(b.samples) + " biased, " + std::to_string(b.leftAlone) +
           " left alone").c_str());
    const float right[4] = {32, 0, 64, 32}, none[4] = {0, 0, 0, 0};
    const std::vector<uint8_t> before = mip_draw(g, m, original, right, -1);
    const std::vector<uint8_t> after = mip_draw(g, m, b.bytecode, right, -1);
    const std::vector<uint8_t> empty = mip_draw(g, m, b.bytecode, none, -1);
    const std::vector<uint8_t> both = mip_draw(g, m, b.bytecode, right, -2, -1);
    const bool drawn = !before.empty() && !after.empty() && !empty.empty() && !both.empty();
    check(drawn && off(before, 0, 64, 0, mip_red(2)) == 0, (what + ": the original samples mip 2 everywhere").c_str());
    check(drawn && off(after, 0, 32, 0, mip_red(2)) == 0 && off(after, 32, 64, 0, mip_red(1)) == 0,
          (what + ": converted, mip 1 inside the rectangle and mip 2 outside").c_str());
    check(drawn && off(empty, 0, 64, 0, mip_red(2)) == 0, (what + ": an empty rectangle changes nothing").c_str());
    check(drawn && off(both, 0, 32, 0, mip_red(1)) == 0 && off(both, 32, 64, 0, mip_red(0)) == 0,
          (what + ": -2 inside and -1 outside, mip 0 and mip 1").c_str());
    if (levelChannel)
        check(drawn && off(after, 0, 64, 2, mip_red(3)) == 0, (what + ": SampleLevel stays at mip 3").c_str());
}

void mip_bias_cases(Gpu& g) {
    MipScene m = mip_scene(g);
    check(m.ok, "focus mip bias: the mip texture and target");
    mip_case(g, m, "ps_4_0 SV_Position not read (as LMU's)", "float4 main(O i) : SV_Target { return t.Sample(s, i.uv); }",
             "ps_4_0", 1, 0);
    mip_case(g, m, "ps_4_0 SV_Position.x read",
             "float4 main(O i) : SV_Target { return t.Sample(s, i.uv) + (i.pos.x < -1 ? 1 : 0); }", "ps_4_0", 1, 0);
    mip_case(g, m, "ps_5_0 sample with an offset",
             "float4 main(O i) : SV_Target { return t.Sample(s, i.uv, int2(1, 0)); }", "ps_5_0", 1, 0);
    mip_case(g, m, "ps_4_0 two samples, a SampleLevel",
             "float4 main(O i) : SV_Target { float4 a = t.Sample(s, i.uv); float4 c = t.Sample(s, i.uv.yx);"
             " return float4(a.r, c.g, t.SampleLevel(s, i.uv, 3).r, 1); }",
             "ps_4_0", 2, 1, true);

    const auto refused = [&](const char* name, const char* body, const char* profile, const char* error) {
        const std::string source = std::string(kMipPsHead) + body;
        const std::vector<uint8_t> code = compile(source.c_str(), profile);
        const dxbc::FocusMipBias b = dxbc::add_focus_mip_bias(code.data(), code.size());
        check(!code.empty() && b.bytecode.empty() && b.error == error,
              (std::string("focus mip bias refuses ") + name + " (" + b.error + ")").c_str());
    };
    refused("a shader without SV_Position", "float4 main(float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }",
            "ps_4_0", "no SV_Position input");
    refused("one with nothing to bias", "float4 main(O i) : SV_Target { return t.SampleLevel(s, i.uv, 1); }", "ps_4_0",
            "no plain samples");
    refused("one using b13", "cbuffer C : register(b13) { float4 k; } float4 main(O i) : SV_Target { return t.Sample(s, i.uv) * k; }",
            "ps_4_0", "the bias's constant buffer slot is in use");
    const std::vector<uint8_t> vs = compile(kMipVs, "vs_4_0");
    const dxbc::FocusMipBias v = dxbc::add_focus_mip_bias(vs.data(), vs.size());
    check(v.bytecode.empty() && !v.error.empty(), "focus mip bias refuses a vertex shader");
}

// A game's pixel shaders (PS_*.dxbc, e.g. tools/rdc/view_census.py RDC_PS_BYTECODE; local only): each converted or
// refused for a reason, and every converted one created by the device.
void real_pixel_shaders(Gpu& g, const char* folder) {
    namespace fs = std::filesystem;
    unsigned files = 0, converted = 0, created = 0, samples = 0;
    std::map<std::string, unsigned> refused;
    std::vector<std::string> failed;
    for (const auto& entry : fs::directory_iterator(folder)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("PS_", 0) != 0 || entry.path().extension() != ".dxbc")
            continue;
        ++files;
        std::vector<uint8_t> code;
        if (FILE* f = nullptr; _wfopen_s(&f, entry.path().c_str(), L"rb") == 0 && f) {
            std::fseek(f, 0, SEEK_END);
            code.resize(static_cast<size_t>(std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            code.resize(std::fread(code.data(), 1, code.size(), f));
            std::fclose(f);
        }
        const dxbc::FocusMipBias b = dxbc::add_focus_mip_bias(code.data(), code.size());
        if (b.bytecode.empty()) {
            ++refused[b.error];
            continue;
        }
        ++converted;
        samples += static_cast<unsigned>(b.samples);
        ComPtr<ID3D11PixelShader> ps;
        if (SUCCEEDED(g.device->CreatePixelShader(b.bytecode.data(), b.bytecode.size(), nullptr, &ps)))
            ++created;
        else
            failed.push_back(name);
    }
    if (!files)
        return;
    std::printf("     game pixel shaders: %u, converted %u (%u samples biased), created %u\n", files, converted, samples,
                created);
    for (const auto& [why, n] : refused)
        std::printf("     refused %u: %s\n", n, why.c_str());
    for (const std::string& f : failed)
        std::printf("     not created: %s\n", f.c_str());
    check(converted > 0 && created == converted, "focus mip bias: every converted game pixel shader is created");
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--hardware") == 0) {  // this PC's GPU instead of WARP
        driverType = D3D_DRIVER_TYPE_HARDWARE;
        --argc;
        ++argv;
    }
    Gpu g;
    if (FAILED(D3D11CreateDevice(nullptr, driverType, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.device,
                                 nullptr, &g.context))) {
        std::printf("FAIL no %s device\n", driverType == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware");
        return 1;
    }
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(g.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
            adapter->GetDesc(&ad);
        std::printf("     device: %ls\n", ad.Description);
    }
    const std::vector<uint8_t> plain = compile(with_defines("").c_str(), "vs_4_0");
    check(!plain.empty() && dxbc::checksum_ok(plain.data(), plain.size()), "checksum: the compiler's own matches");
    std::vector<uint8_t> broken = plain;
    if (!broken.empty())
        broken[100] ^= 1;
    check(!broken.empty() && !dxbc::checksum_ok(broken.data(), broken.size()), "checksum: a changed byte fails");

    run_case(g, "vs_4_0", plain, 0);
    run_case(g, "vs_5_0", compile(with_defines("").c_str(), "vs_5_0"), 0);
    run_case(g, "vs_4_0 position in o1", compile(with_defines("#define LATE\n").c_str(), "vs_4_0"), 1);
    run_case(g, "vs_4_0 position written in pieces", compile(with_defines("#define PIECES\n").c_str(), "vs_4_0"), 0);
    run_case(g, "vs_5_0 position written in pieces", compile(with_defines("#define PIECES\n").c_str(), "vs_5_0"), 0);
    run_case(g, "vs_4_0 no temps", compile(kPassThrough, "vs_4_0"), 0);
    run_case(g, "vs_4_0 14 outputs (the last two registers free)", compile(many_outputs(13).c_str(), "vs_4_0"), 0);

    const std::vector<uint8_t> full = compile(many_outputs(14).c_str(), "vs_4_0");
    const dxbc::SecondView none = dxbc::add_second_view(full.data(), full.size());
    check(!full.empty() && none.bytecode.empty() && none.error == "no free output register",
          "vs_4_0 15 outputs: refused (no two registers free)");
    const std::vector<uint8_t> pixel = compile("float4 main() : SV_Target { return 1; }", "ps_4_0");
    const dxbc::SecondView ps = dxbc::add_second_view(pixel.data(), pixel.size());
    check(ps.bytecode.empty() && !ps.error.empty(), "a pixel shader is refused");

    instanced_cases(g, plain, pixel);
    mip_bias_cases(g);
    if (argc > 1) {  // a game's shaders: vertex ones (logs\shaders of a probe run with lmu_mvr_dump=1) both ways, pixel
        real_shaders(g, argv[1]);  // ones through the focus mip bias
        real_pixel_shaders(g, argv[1]);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

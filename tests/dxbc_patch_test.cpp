// Offline check of the second-view vertex shader edit (engine/dxbc_patch.h) on a WARP device: shaders compiled here
// are converted, created, and their outputs streamed out: SV_Position exactly as before, NV_XYZW_RIGHT the clip
// remap of it, NV_VIEWPORT_MASK its value. The checksum is checked against the compiler's.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/dxbc_patch.h"

using Microsoft::WRL::ComPtr;
namespace dxbc = simqvfr::dxbc;

namespace {

int failures = 0;

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

}  // namespace

int main() {
    Gpu g;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.device,
                                 nullptr, &g.context))) {
        std::printf("FAIL no WARP device\n");
        return 1;
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

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

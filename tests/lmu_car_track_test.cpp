// Offline check of LMU's car tracking (games/lmu/mvr_pass.h mvr_car_track, car_motion.h) on a WARP device (--hardware:
// the GPU's): a vertex shader's worldMatrix found by its reflection; with the hooks on, an eye pass's draws keyed by their buffers and index
// range, the matrix read as the game writes the per-draw buffer (Map, Unmap) at the shader's offset, the static world
// (identity) told apart, both eye passes one frame, the frame before's matrix there the next frame; draws outside the
// passes, without a depth target or with a shader that reads no worldMatrix left out; the game's calls passed on.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "games/lmu/mvr_pass.h"
#include "games/lmu/mvr_probe.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

int fake_two_views(ID3D11DeviceContext*, bool) { return 0; }

ComPtr<ID3DBlob> compile(const char* source) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "t", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &errors))) {
        std::printf("FAIL compile: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

// LMU's per-draw buffer as these tests lay it out: a float4 before the matrix (so the offset matters), the matrix.
struct PerDraw {
    float pad[4] = {9, 9, 9, 9};  // not an identity: read here by mistake, the static world would look like a part
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

}  // namespace

// --scan <folder>: where each vertex shader's bytecode there (*.dxbc, as lmu_mvr_dump saves or a capture's export)
// reads its worldMatrix, as counts per slot and offset (the game's code stays local: nothing of it in the repository).
int scan(const char* folder) {
    std::map<std::pair<unsigned, unsigned>, int> places;
    int none = 0, files = 0;
    WIN32_FIND_DATAA f;
    const std::string pattern = std::string(folder) + "\\*.dxbc";
    HANDLE h = FindFirstFileA(pattern.c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("FAIL no .dxbc files in %s\n", folder);
        return 1;
    }
    do {
        std::FILE* file = nullptr;
        if (fopen_s(&file, (std::string(folder) + "\\" + f.cFileName).c_str(), "rb") != 0 || !file)
            continue;
        std::vector<char> code;
        char buffer[65536];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
            code.insert(code.end(), buffer, buffer + n);
        std::fclose(file);
        ++files;
        unsigned slot = 0, offset = 0;
        if (lmu::mvr_world_matrix_place(code.data(), code.size(), slot, offset))
            ++places[{slot, offset}];
        else
            ++none;
    } while (FindNextFileA(h, &f));
    FindClose(h);
    std::printf("%d shaders: %d read no worldMatrix\n", files, none);
    for (const auto& [at, n] : places)
        std::printf("  slot %u offset %u: %d\n", at.first, at.second, n);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "--scan") == 0)
        return scan(argv[2]);
    // --hardware: the GPU's own driver instead of WARP (d3d11 may take other paths for Map there). --timing: the hooks'
    // own cost per draw (best with --hardware).
    bool hardware = false, timing = false;
    for (int i = 1; i < argc; ++i) {
        hardware = hardware || std::strcmp(argv[i], "--hardware") == 0;
        timing = timing || std::strcmp(argv[i], "--timing") == 0;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr,
                                 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::printf("FAIL no %s device\n", hardware ? "hardware" : "WARP");
        return 1;
    }

    // The worldMatrix's place, from the reflection (the probe marks each game shader with it: kVsWorldMatrixGuid).
    const ComPtr<ID3DBlob> worldCode = compile(
        "cbuffer GlobalCBFloats_Draw_0 : register(b3) { float4 pad; float4x4 worldMatrix; };\n"
        "cbuffer View : register(b0) { float4x4 viewProj; };\n"
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "    return mul(mul(float4(id, id * 0.5, 0, 1), worldMatrix), viewProj); }\n");
    const ComPtr<ID3DBlob> plainCode = compile(
        "cbuffer View : register(b0) { float4x4 viewProj; };\n"
        "float4 main(uint id : SV_VertexID) : SV_Position { return mul(float4(id, 0, 0, 1), viewProj); }\n");
    if (!worldCode || !plainCode)
        return 1;
    unsigned slot = 99, offset = 99;
    check(lmu::mvr_world_matrix_place(worldCode->GetBufferPointer(), worldCode->GetBufferSize(), slot, offset) &&
              slot == 3 && offset == 16,
          "worldMatrix found by the reflection: slot 3, offset 16");
    unsigned s2 = 0, o2 = 0;
    check(!lmu::mvr_world_matrix_place(plainCode->GetBufferPointer(), plainCode->GetBufferSize(), s2, o2),
          "a shader without one: none");
    ComPtr<ID3D11VertexShader> worldVs, plainVs;
    device->CreateVertexShader(worldCode->GetBufferPointer(), worldCode->GetBufferSize(), nullptr, &worldVs);
    device->CreateVertexShader(plainCode->GetBufferPointer(), plainCode->GetBufferSize(), nullptr, &plainVs);
    const UINT place[2] = {slot, offset};
    if (!worldVs || !plainVs || FAILED(worldVs->SetPrivateData(lmu::kVsWorldMatrixGuid, sizeof(place), place))) {
        std::printf("FAIL shaders\n");
        return 1;
    }

    // The hooks: the ten as always, DrawIndexed and car tracking's five (VSSetConstantBuffers, Map, Unmap, the input
    // assembler's two).
    // The game's resources: the per-draw buffer, a target with depth, two meshes' buffers, a ring (a dynamic vertex
    // buffer the game refills each frame: its draws' base vertex moves), one index buffer.
    ComPtr<ID3D11Buffer> perDraw, viewCb, vbA, vbB, ring, ib;
    {
        const D3D11_BUFFER_DESC d = {sizeof(PerDraw), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                                     D3D11_CPU_ACCESS_WRITE, 0, 0};
        device->CreateBuffer(&d, nullptr, &perDraw);
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const D3D11_BUFFER_DESC v = {sizeof(identity), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA init = {identity, 0, 0};
        device->CreateBuffer(&v, &init, &viewCb);
        const float verts[32] = {};
        const D3D11_BUFFER_DESC vd = {sizeof(verts), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA vi = {verts, 0, 0};
        device->CreateBuffer(&vd, &vi, &vbA);
        device->CreateBuffer(&vd, &vi, &vbB);
        const D3D11_BUFFER_DESC rd = {sizeof(verts), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE,
                                      0, 0};
        device->CreateBuffer(&rd, nullptr, &ring);
        const uint16_t indices[12] = {0, 1, 2, 0, 2, 3, 1, 2, 3, 0, 1, 3};
        const D3D11_BUFFER_DESC id = {sizeof(indices), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA ii = {indices, 0, 0};
        device->CreateBuffer(&id, &ii, &ib);
    }
    ComPtr<ID3D11Texture2D> color, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    {
        D3D11_TEXTURE2D_DESC t = {32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET, 0, 0};
        device->CreateTexture2D(&t, nullptr, &color);
        t.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        t.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        device->CreateTexture2D(&t, nullptr, &depth);
        if (color)
            device->CreateRenderTargetView(color.Get(), nullptr, &rtv);
        if (depth)
            device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    }
    if (!perDraw || !viewCb || !vbA || !vbB || !ring || !ib || !rtv || !dsv) {
        std::printf("FAIL resources\n");
        return 1;
    }
    const D3D11_VIEWPORT vp = {0, 0, 32, 32, 0, 1};
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* const cbs[4] = {viewCb.Get(), nullptr, nullptr, perDraw.Get()};

    // --timing: the calls car tracking hooks, as LMU makes them before each draw (the per-draw buffer written, the
    // vertex shader's buffers and the mesh's bound; no draw), timed before the install and after it.
    const auto time_calls = [&]() {
        constexpr int kCalls = 20000;
        double best = 1e30;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        for (int round = 0; round < 7; ++round) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < kCalls; ++i) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(context->Map(perDraw.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    PerDraw pd;
                    pd.world[12] = static_cast<float>(i);
                    std::memcpy(m.pData, &pd, sizeof(pd));
                    context->Unmap(perDraw.Get(), 0);
                }
                context->VSSetConstantBuffers(0, 4, cbs);
                const UINT stride = 16, vbOffset = 0;
                ID3D11Buffer* const vb = (i & 1) ? vbA.Get() : vbB.Get();
                context->IASetVertexBuffers(0, 1, &vb, &stride, &vbOffset);
                context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
            }
            QueryPerformanceCounter(&t1);
            context->Flush();
            best = (std::min)(best, 1e9 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart) /
                                        kCalls);
        }
        return best;  // ns per draw's five calls
    };
    const double untimed = timing ? time_calls() : 0;

    // The hooks: the ten as always, DrawIndexed and car tracking's five (VSSetConstantBuffers, Map, Unmap, the input
    // assembler's two).
    lmu::mvr_set_two_views(&fake_two_views);
    lmu::mvr_car_track(true);
    check(lmu::mvr_pass_install(device.Get()), "the immediate context hooked");
    char what[240];
    const lmu::MvrPassStats installed = lmu::mvr_pass_stats();
    std::snprintf(what, sizeof(what), "%llu entries hooked (sixteen with car tracking)",
                  static_cast<unsigned long long>(installed.contextEntries));
    check(installed.contextEntries == 16, what);

    int mapsOk = 0, maps = 0;
    // A draw as LMU makes it: the per-draw buffer written (Map discard, Unmap), the mesh's buffers bound, DrawIndexed.
    const auto draw = [&](const float* world, ID3D11Buffer* vb, UINT start, UINT count, INT base = 0) {
        D3D11_MAPPED_SUBRESOURCE m{};
        ++maps;
        if (SUCCEEDED(context->Map(perDraw.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData) {
            ++mapsOk;
            PerDraw pd;
            if (world)
                std::memcpy(pd.world, world, sizeof(pd.world));
            std::memcpy(m.pData, &pd, sizeof(pd));
            context->Unmap(perDraw.Get(), 0);
        }
        const UINT stride = 16, vbOffset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &vbOffset);
        context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->DrawIndexed(count, start, base);
    };
    const float moved5[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 0, 0, 1};  // a car part 5 along x
    const float moved7[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, 0, 0, 1};  // the next frame
    // A frame's two eye passes: the static world twice (identity; two index ranges), a car part between, and a part
    // drawn from the ring at this frame's place in it (`ringBase`).
    const auto frame = [&](const float* part, INT ringBase) {
        for (int pass = 0; pass < 2; ++pass) {
            lmu::mvr_pass_begin(pass, nullptr, false, false);
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
            context->VSSetShader(worldVs.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 4, cbs);
            draw(nullptr, vbA.Get(), 0, 3);
            draw(part, vbB.Get(), 0, 6);
            draw(part, ring.Get(), 6, 6, ringBase);
            draw(nullptr, vbA.Get(), 3, 3);
            lmu::mvr_pass_end(pass);
        }
    };

    frame(moved5, 0);
    lmu::MvrPassStats s = lmu::mvr_pass_stats();
    lmu::CarMotionStats c = lmu::mvr_car_stats();
    std::snprintf(what, sizeof(what),
                  "frame 1: the per-draw buffer learned at its first draw (%llu), matrices read after (%llu of 8), "
                  "3 identity draws (%llu), 4 of the two parts (%llu), first seen (%llu), from the ring (%llu)",
                  static_cast<unsigned long long>(s.carBufferLearned), static_cast<unsigned long long>(s.carMatrixReads),
                  static_cast<unsigned long long>(c.identityDraws), static_cast<unsigned long long>(c.partDraws),
                  static_cast<unsigned long long>(c.newKeys), static_cast<unsigned long long>(s.carDynamicVb));
    check(s.carBufferLearned == 1 && s.carMatrixReads == 7 && c.identityDraws == 3 && c.partDraws == 4 &&
              c.newKeys == 2 && c.matched == 0 && c.frames == 1 && s.carOtherOffset == 0 && s.carNoMatrix == 0 &&
              s.carDynamicVb == 2,
          what);

    frame(moved7, 4);  // the ring refilled: its part 4 vertices further on
    s = lmu::mvr_pass_stats();
    c = lmu::mvr_car_stats();
    std::snprintf(what, sizeof(what),
                  "frame 2: both parts matched to the frame before in both eyes (%llu), the ring's though its base vertex "
                  "moved; two keys held (%zu), 7 identity draws in all (%llu)",
                  static_cast<unsigned long long>(c.matched), c.live, static_cast<unsigned long long>(c.identityDraws));
    check(c.matched == 4 && c.newKeys == 2 && c.live == 2 && c.identityDraws == 7 && c.partDraws == 8 && c.frames == 2 &&
              s.carMatrixReads == 15 && s.carDynamicVb == 4,
          what);

    // Left out: a draw outside the passes, one without a depth target, one whose shader reads no worldMatrix.
    const lmu::CarMotionStats before = c;
    draw(moved7, vbB.Get(), 0, 6);
    lmu::mvr_pass_begin(0, nullptr, false, false);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->VSSetShader(worldVs.Get(), nullptr, 0);
    draw(moved7, vbB.Get(), 0, 6);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    context->VSSetShader(plainVs.Get(), nullptr, 0);
    draw(moved7, vbB.Get(), 0, 6);
    c = lmu::mvr_car_stats();
    check(c.draws == before.draws && c.frames == 3, "outside a pass, without depth, a shader without a worldMatrix: not counted");
    // A shader reading it at another offset than the learned one: counted apart, left out.
    const UINT elsewhere[2] = {slot, offset + 64};
    worldVs->SetPrivateData(lmu::kVsWorldMatrixGuid, sizeof(elsewhere), elsewhere);
    lmu::mvr_twins_changed();  // the entry cache looks again
    context->VSSetShader(worldVs.Get(), nullptr, 0);
    draw(moved7, vbB.Get(), 0, 6);
    lmu::mvr_pass_end(0);
    s = lmu::mvr_pass_stats();
    c = lmu::mvr_car_stats();
    check(s.carOtherOffset == 1 && c.draws == before.draws, "a shader with the matrix at another offset: counted apart");
    check(maps > 0 && mapsOk == maps, "every Map of the game's passed on and returned its memory");
    context->Flush();
    if (timing) {  // the per-draw buffer learned by now: each Unmap of it copies the matrix, as in the game
        lmu::mvr_time_hooks(false);
        const double after = time_calls();
        std::printf("timing: a draw's five calls (Map, Unmap, VSSetConstantBuffers, IASetVertexBuffers, IASetIndexBuffer) "
                    "%.1f ns before the install, %.1f ns after: %.1f ns per call hooked; at run 20's 15,000-36,000 calls "
                    "per frame %.3f-%.3f ms\n",
                    untimed, after, (after - untimed) / 5, (after - untimed) / 5 * 15000 / 1e6,
                    (after - untimed) / 5 * 36000 / 1e6);
    }

    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

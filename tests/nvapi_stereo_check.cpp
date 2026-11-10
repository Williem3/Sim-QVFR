// Hardware check of NVIDIA single-pass stereo (engine/nvapi_stereo.h) with converted shaders (engine/dxbc_patch.h).
// Not part of the test suite (no _test suffix): it needs an NVIDIA GPU, and its optional shader folder holds a game's
// shaders (tools/rdc/view_census.py RDC_BYTECODE: keep it local).
//
//   nvapi_stereo_check.exe [folder of .dxbc vertex shaders]
//   nvapi_stereo_check.exe --timing      (only 4. below)
//
// 1. NVAPI's support flags on the default adapter.
// 2. Every shader in the folder: its checksum, the conversion, the converted checksum, created by D3D11 and by NVAPI
//    with the custom semantics.
// 3. Two-view draws: one triangle through a converted pass-through shader into a 256x128 target with two viewports,
//    against a control with the mode off: the layer's routing (independent viewport mask 0x00020001: view 1 in the
//    right viewport of the same target), the mask swapped, and the classic one (view 1 a render target slice on).
// 4. (--timing) The render thread's CPU for the eye-pass hooks' work, and for how a hook is reached (a MinHook jump, a
//    vtable copy).
// 5. The layer's routing again with the device on a vtable copy (engine/object_vtable.h), as the probe leaves the
//    game's: NVAPI's flags, its shader creation and the draw as on the original.
// 6. What d3d11 does to the immediate context's vtable: the entries it rewrites between idle and busy and as multithread
//    protection goes on, and what a hook on one function an entry held misses then.
// 7. The layer's own context hooks installed on this device (games/lmu/mvr_pass.h): every function the entries hold.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <MinHook.h>

#include "engine/dxbc_patch.h"
#include "engine/nvapi_gpus.h"
#include "engine/nvapi_stereo.h"
#include "engine/object_vtable.h"
#include "games/lmu/mvr_pass.h"

using Microsoft::WRL::ComPtr;
namespace dxbc = simqvfr::dxbc;
namespace nvapi = simqvfr::nvapi;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::vector<uint8_t> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "check", nullptr, nullptr, "main", profile, 0, 0, &code, &errors)))
        return {};
    const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {p, p + code->GetBufferSize()};
}

void shaders(ID3D11Device* device, const std::filesystem::path& folder) {
    int total = 0, sumOk = 0, converted = 0, convertedSum = 0, created = 0, nvCreated = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() != ".dxbc")
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<uint8_t> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ++total;
        sumOk += dxbc::checksum_ok(code.data(), code.size()) ? 1 : 0;
        const dxbc::SecondView sv = dxbc::add_second_view(code.data(), code.size());
        if (sv.bytecode.empty()) {
            std::printf("     %s: not converted: %s\n", entry.path().filename().string().c_str(), sv.error.c_str());
            continue;
        }
        ++converted;
        convertedSum += dxbc::checksum_ok(sv.bytecode.data(), sv.bytecode.size()) ? 1 : 0;
        ComPtr<ID3D11VertexShader> plain, nv;
        const HRESULT hr = device->CreateVertexShader(sv.bytecode.data(), sv.bytecode.size(), nullptr, &plain);
        created += SUCCEEDED(hr) ? 1 : 0;
        const int status = nvapi::create_second_view_shader(device, sv, &nv);
        nvCreated += status == 0 && nv ? 1 : 0;
        if (FAILED(hr) || status != 0)
            std::printf("     %s: D3D11 0x%08lx, NVAPI %d (%s)\n", entry.path().filename().string().c_str(),
                        static_cast<unsigned long>(hr), status, nvapi::status_name(status));
    }
    char what[200];
    std::snprintf(what, sizeof(what), "%d shaders: %d checksums match, %d converted (%d checksums), %d created by D3D11, "
                  "%d by NVAPI", total, sumOk, converted, convertedSum, created, nvCreated);
    check(total > 0 && sumOk == total && converted == total && convertedSum == total && created == total &&
              nvCreated == total, what);
}

// What one half of one slice holds after a draw: lit pixels and their bounds.
struct Half {
    int count = 0, minX = 1 << 30, maxX = -1, minY = 1 << 30, maxY = -1;
    int cx() const { return count ? (minX + maxX) / 2 : -1; }
    int cy() const { return count ? (minY + maxY) / 2 : -1; }
};

struct Result {
    bool ok = false;      // drawn, read back, device not removed
    Half half[2][2];      // [slice][left, right]
};

struct Variant {
    const char* name;
    bool twoViews, independentMasks;
    unsigned targetOffset, slices;
    bool viewportMask = true;
    uint32_t mask = 0x00020001;
};

// One triangle through a converted pass-through shader into a 256x128 target of `slices` slices, two viewports (left
// and right half). View 1 is the remap a = c = 0.5, b = d = 0.4: half size, centred at NDC (0.4, 0.4) of its own
// viewport, so wherever it lands it can't hide inside view 0's triangle.
Result draw(ID3D11Device* device, ID3D11DeviceContext* context, const Variant& v) {
    Result r;
    const std::vector<uint8_t> vsCode =
        compile("float4 main(float4 p : POSITION) : SV_Position { return p; }", "vs_5_0");
    const std::vector<uint8_t> psCode = compile("float4 main() : SV_Target { return 1; }", "ps_5_0");
    dxbc::SecondViewOptions options;
    options.viewportMask = v.viewportMask;
    options.mask = v.mask;
    const dxbc::SecondView sv = dxbc::add_second_view(vsCode.data(), vsCode.size(), options);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (sv.bytecode.empty() || nvapi::create_second_view_shader(device, sv, &vs) != 0 ||
        FAILED(device->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps)))
        return r;
    const float points[3][4] = {{-0.8f, -0.8f, 0.5f, 1}, {0, 0.8f, 0.5f, 1}, {0.8f, -0.8f, 0.5f, 1}};
    const float remap[4] = {0.5f, 0.4f, 0.5f, 0.4f};
    D3D11_BUFFER_DESC bd = {sizeof(points), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA vInit = {points, 0, 0}, cInit = {remap, 0, 0};
    ComPtr<ID3D11Buffer> vb, cb;
    const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                              D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    D3D11_TEXTURE2D_DESC td = {256, 128, 1, v.slices, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.ArraySize = v.slices;
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    if (FAILED(device->CreateBuffer(&bd, &vInit, &vb)) || FAILED(device->CreateBuffer(&cd, &cInit, &cb)) ||
        FAILED(device->CreateInputLayout(&element, 1, vsCode.data(), vsCode.size(), &layout)) ||
        FAILED(device->CreateTexture2D(&td, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target.Get(), &rd, &rtv)) ||
        FAILED(device->CreateRasterizerState(&rs, &raster)))
        return r;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging)))
        return r;
    const float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(rtv.Get(), clear);
    const D3D11_VIEWPORT viewports[2] = {{0, 0, 128, 128, 0, 1}, {128, 0, 128, 128, 0, 1}};
    const UINT stride = 16, offset = 0;
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(2, viewports);
    context->RSSetState(raster.Get());
    context->IASetInputLayout(layout.Get());
    context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->VSSetConstantBuffers(13, 1, cb.GetAddressOf());
    context->PSSetShader(ps.Get(), nullptr, 0);
    if (v.twoViews && nvapi::set_two_views(context, true, v.independentMasks, v.targetOffset) != 0)
        return r;
    context->Draw(3, 0);
    if (v.twoViews)
        nvapi::set_two_views(context, false);
    context->CopyResource(staging.Get(), target.Get());
    for (unsigned slice = 0; slice < v.slices && slice < 2; ++slice) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(context->Map(staging.Get(), slice, D3D11_MAP_READ, 0, &map)))
            return r;
        for (int y = 0; y < 128; ++y) {
            const auto* row = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
            for (int x = 0; x < 256; ++x) {
                if (row[x * 4] < 128)
                    continue;
                Half& h = r.half[slice][x < 128 ? 0 : 1];
                ++h.count;
                h.minX = std::min(h.minX, x);
                h.maxX = std::max(h.maxX, x);
                h.minY = std::min(h.minY, y);
                h.maxY = std::max(h.maxY, y);
            }
        }
        context->Unmap(staging.Get(), slice);
    }
    r.ok = SUCCEEDED(device->GetDeviceRemovedReason());
    return r;
}

void print(const Variant& v, const Result& r) {
    std::printf("     %-48s%s", v.name, r.ok ? "" : " (not drawn or device removed)");
    for (unsigned s = 0; s < v.slices && s < 2; ++s)
        for (int h = 0; h < 2; ++h)
            std::printf(" | s%u %s %4d px c(%d,%d)", s, h ? "R" : "L", r.half[s][h].count, r.half[s][h].cx(),
                        r.half[s][h].cy());
    std::printf("\n");
}

// View 1 where it should be: a quarter of view 0's area, centred at (90, 38) of its half (NDC 0.4, 0.4 of 128 px).
bool view1_at(const Half& h, int baseX, int area0) {
    return h.count > area0 / 6 && h.count < area0 / 3 && std::abs(h.cx() - (baseX + 90)) <= 4 &&
           std::abs(h.cy() - 38) <= 4;
}

// 4. (--timing) The render thread's CPU for what the layer's eye-pass hooks add, on this GPU: passes of 400 small draws,
//    each with its own vertex shader bind (four shaders in turn), as the game's eye pass does (~400 binds, ~22 stereo
//    switches per pass in the 2026-11-08 runs). Thread cycles (busy time, not waits) per pass, converted at the TSC rate.
const GUID kTwinGuid = {0x6d1c4a52, 0x3b8e, 0x4f61, {0x9a, 0x27, 0x5e, 0x10, 0xc3, 0x7b, 0x44, 0xd9}};

double cycles_per_ms() {
    static double rate = 0;
    if (rate == 0) {
        ULONG64 c0 = 0, c1 = 0;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        do {
            QueryPerformanceCounter(&t1);
        } while (t1.QuadPart - t0.QuadPart < f.QuadPart / 5);
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        rate = static_cast<double>(c1 - c0) / (1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / f.QuadPart);
    }
    return rate;
}

void timing(ID3D11Device* device, ID3D11DeviceContext* context) {
    constexpr int kShaders = 4, kDraws = 400, kPasses = 300;
    const char* vsSource[kShaders] = {
        "float4 main(float4 p : POSITION) : SV_Position { return p; }",
        "float4 main(float4 p : POSITION) : SV_Position { return p * float4(0.9, 0.9, 1, 1); }",
        "float4 main(float4 p : POSITION) : SV_Position { return p * float4(0.8, 0.8, 1, 1); }",
        "float4 main(float4 p : POSITION) : SV_Position { return p * float4(0.7, 0.7, 1, 1); }"};
    ComPtr<ID3D11VertexShader> plain[kShaders], twin[kShaders];
    std::vector<uint8_t> first;
    for (int i = 0; i < kShaders; ++i) {
        const std::vector<uint8_t> code = compile(vsSource[i], "vs_5_0");
        const dxbc::SecondView sv = dxbc::add_second_view(code.data(), code.size());
        if (code.empty() || sv.bytecode.empty() ||
            FAILED(device->CreateVertexShader(code.data(), code.size(), nullptr, &plain[i])) ||
            nvapi::create_second_view_shader(device, sv, &twin[i]) != 0) {
            check(false, "timing: shaders");
            return;
        }
        plain[i]->SetPrivateDataInterface(kTwinGuid, twin[i].Get());
        if (i == 0)
            first = code;
    }
    const std::vector<uint8_t> psCode = compile("float4 main() : SV_Target { return 1; }", "ps_5_0");
    // Big enough that the GPU has work in flight at each switch (a pipeline drain would show as GPU time).
    const float points[3][4] = {{-1, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}, {3, -1, 0.5f, 1}};
    const float remap[4] = {1, 0, 1, 0};
    D3D11_BUFFER_DESC bd = {sizeof(points), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA vInit = {points, 0, 0}, cInit = {remap, 0, 0};
    const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                              D3D11_INPUT_PER_VERTEX_DATA, 0};
    D3D11_TEXTURE2D_DESC td = {512, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    D3D11_TEXTURE2D_DESC dd = {512, 256, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_DEPTH_STENCIL, 0, 0};
    ComPtr<ID3D11Buffer> vb, cb;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Texture2D> target, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Query> done, disjoint, stamps[2];
    const D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}, dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0},
                           tq = {D3D11_QUERY_TIMESTAMP, 0};
    device->CreateQuery(&dq, &disjoint);
    device->CreateQuery(&tq, &stamps[0]);
    device->CreateQuery(&tq, &stamps[1]);
    if (FAILED(device->CreateBuffer(&bd, &vInit, &vb)) || FAILED(device->CreateBuffer(&cd, &cInit, &cb)) ||
        FAILED(device->CreateInputLayout(&element, 1, first.data(), first.size(), &layout)) ||
        FAILED(device->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps)) ||
        FAILED(device->CreateTexture2D(&td, nullptr, &target)) || FAILED(device->CreateTexture2D(&dd, nullptr, &depth)) ||
        FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)) ||
        FAILED(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv)) || FAILED(device->CreateQuery(&qd, &done))) {
        check(false, "timing: resources");
        return;
    }
    const D3D11_VIEWPORT viewports[2] = {{0, 0, 256, 256, 0, 1}, {256, 0, 256, 256, 0, 1}};
    const UINT stride = 16, offset = 0;
    const auto setup = [&](bool two) {
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        context->RSSetViewports(two ? 2 : 1, viewports);
        context->IASetInputLayout(layout.Get());
        context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetConstantBuffers(13, 1, cb.GetAddressOf());
        context->PSSetShader(ps.Get(), nullptr, 0);
    };
    const auto wait = [&] {
        context->End(done.Get());
        context->Flush();
        BOOL finished = FALSE;
        while (context->GetData(done.Get(), &finished, sizeof(finished), 0) != S_OK || !finished)
            Sleep(0);
    };
    const auto lookup = [&](ID3D11VertexShader* vs) {  // mvr_pass.cpp's twin_of
        ComPtr<ID3D11VertexShader> t;
        IUnknown* unknown = nullptr;
        UINT size = sizeof(unknown);
        if (SUCCEEDED(vs->GetPrivateData(kTwinGuid, &size, &unknown)) && unknown) {
            unknown->QueryInterface(IID_PPV_ARGS(&t));
            unknown->Release();
        }
        return t;
    };
    // switchEvery: draws between stereo switches (0: none); off draws use the plain shader with one viewport.
    struct Case {
        const char* name;
        bool twins, stereo, lookups;
        int switchEvery;
        bool viewportsWithSwitch;
    };
    const Case cases[] = {
        {"plain shaders, stereo off (the game alone)", false, false, false, 0, false},
        {"twins, stereo on all pass", true, true, false, 0, false},
        {"twins + a GetPrivateData lookup per bind", true, true, true, 0, false},
        {"twins, a stereo switch every 18 draws (~22 per pass)", true, true, false, 18, false},
        {"... + viewports reset at each switch", true, true, false, 18, true},
        {"... + lookups (the layer today)", true, true, true, 18, true},
    };
    const double rate = cycles_per_ms();
    double base = 0, baseGpu = 0;
    for (const Case& c : cases) {
        wait();
        setup(c.stereo);
        bool on = false;
        if (c.stereo)
            on = nvapi::set_two_views(context, true) == 0;
        ULONG64 total = 0;
        double gpuMs = 0;
        int gpuPasses = 0;
        for (int pass = 0; pass < kPasses; ++pass) {
            const bool timed = pass % 20 == 10;  // one GPU-timed pass in each batch, read back at the batch's wait
            if (timed) {
                context->Begin(disjoint.Get());
                context->End(stamps[0].Get());
            }
            ULONG64 c0 = 0, c1 = 0;
            QueryThreadCycleTime(GetCurrentThread(), &c0);
            for (int d = 0; d < kDraws; ++d) {
                if (c.switchEvery && d % c.switchEvery == 0) {
                    on = !on;
                    nvapi::set_two_views(context, on);
                    if (c.viewportsWithSwitch)
                        context->RSSetViewports(on ? 2 : 1, viewports);
                }
                ID3D11VertexShader* game = plain[d % kShaders].Get();
                ID3D11VertexShader* bind = game;
                if (c.twins && on)
                    bind = c.lookups ? lookup(game).Get() : twin[d % kShaders].Get();
                context->VSSetShader(bind, nullptr, 0);
                context->Draw(3, 0);
            }
            QueryThreadCycleTime(GetCurrentThread(), &c1);
            total += c1 - c0;
            if (timed) {
                context->End(stamps[1].Get());
                context->End(disjoint.Get());
            }
            if (pass % 20 == 19) {
                wait();  // keep the queue short: no blocking inside the timed loop
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                UINT64 t0 = 0, t1 = 0;
                if (context->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_OK && !dj.Disjoint && dj.Frequency &&
                    context->GetData(stamps[0].Get(), &t0, sizeof(t0), 0) == S_OK &&
                    context->GetData(stamps[1].Get(), &t1, sizeof(t1), 0) == S_OK && t1 > t0) {
                    gpuMs += 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(dj.Frequency);
                    ++gpuPasses;
                }
            }
        }
        if (on)
            nvapi::set_two_views(context, false);
        wait();
        const double ms = static_cast<double>(total) / kPasses / rate;
        const double gpu = gpuPasses ? gpuMs / gpuPasses : 0;
        if (base == 0) {
            base = ms;
            baseGpu = gpu;
        }
        std::printf("     %-56s CPU %.3f ms per pass (%+.3f), GPU %.3f (%+.3f)\n", c.name, ms, ms - base, gpu,
                    gpu - baseGpu);
    }
    // The switch alone, no draws between.
    ULONG64 c0 = 0, c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    for (int i = 0; i < 2000; ++i)
        nvapi::set_two_views(context, (i & 1) == 0);
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    std::printf("     %-56s %.2f us per call\n", "SetSinglePassStereoMode alone", 1000.0 * (c1 - c0) / 2000 / rate);
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    for (int i = 0; i < 20000; ++i)
        lookup(plain[i % kShaders].Get());
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    std::printf("     %-56s %.3f us per call\n", "GetPrivateData + QueryInterface (twin_of)", 1000.0 * (c1 - c0) / 20000 / rate);
    wait();
    check(SUCCEEDED(device->GetDeviceRemovedReason()), "timing: device fine afterwards");

    // How a hook is reached: the same pass (a VSSetShader bind and a draw, 400 times) with VSSetShader passed straight
    // on by a detour reached by a MinHook jump in d3d11.dll's code (the layer's), or through a vtable copy (not for the
    // context: d3d11 rewrites its commands' entries, which a copy freezes, slowing every call); and no hook. Rounds in a
    // rotating order, the median pass of each.
    using VSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*,
                                                   ID3D11ClassInstance* const*, UINT);
    static VSSetShaderFn viaJump = nullptr, viaCopy = nullptr;
    struct Detours {
        static void STDMETHODCALLTYPE jump(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* i,
                                           UINT n) {
            viaJump(c, vs, i, n);
        }
        static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* i,
                                           UINT n) {
            viaCopy(c, vs, i, n);
        }
    };
    constexpr unsigned kVs = lmu::context_slot::kVSSetShader;
    void* const vsSetShader = (*reinterpret_cast<void***>(context))[kVs];
    constexpr int kModes = 3;
    std::vector<double> passes[kModes];
    for (int round = 0; round < 40; ++round)
        for (int k = 0; k < kModes; ++k) {
            const int mode = (k + round) % kModes;
            simqvfr::ObjectVtable copy;
            bool hooked = true;
            if (mode == 1) {
                const MH_STATUS init = MH_Initialize();
                hooked = (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) &&
                         MH_CreateHook(vsSetShader, reinterpret_cast<void*>(&Detours::jump),
                                       reinterpret_cast<void**>(&viaJump)) == MH_OK &&
                         MH_EnableHook(vsSetShader) == MH_OK;
            }
            if (mode == 2)
                hooked = copy.install(context, simqvfr::kD3D11ContextSlots,
                                      {{kVs, reinterpret_cast<void*>(&Detours::copy), reinterpret_cast<void**>(&viaCopy)}});
            if (!hooked) {
                check(false, "timing: a hook for the comparison");
                return;
            }
            wait();
            setup(false);
            for (int pass = 0; pass < 40; ++pass) {
                ULONG64 p0 = 0, p1 = 0;
                QueryThreadCycleTime(GetCurrentThread(), &p0);
                for (int d = 0; d < kDraws; ++d) {
                    context->VSSetShader(plain[d % kShaders].Get(), nullptr, 0);
                    context->Draw(3, 0);
                }
                QueryThreadCycleTime(GetCurrentThread(), &p1);
                passes[mode].push_back(static_cast<double>(p1 - p0) / rate);
                if (pass % 20 == 19)
                    wait();
            }
            copy.remove();
            if (mode == 1)
                MH_RemoveHook(vsSetShader);
        }
    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const char* names[kModes] = {"no hook", "VSSetShader through a MinHook jump (the layer's)",
                                 "VSSetShader through a vtable copy (not for the context)"};
    double ms[kModes];
    for (int mode = 0; mode < kModes; ++mode)
        ms[mode] = median(passes[mode]);
    for (int mode = 0; mode < kModes; ++mode)
        std::printf("     %-64s CPU %.4f ms per pass (%+.4f; %+.1f ns per bind and draw)\n", names[mode], ms[mode],
                    ms[mode] - ms[0], 1e6 * (ms[mode] - ms[0]) / kDraws);
    char what[200];
    // 0.8-2.6 ns on an RTX 5070 Ti (2026-11-09, by the CPU's clock state): under 10 us a frame at LMU's ~1,700 calls.
    std::snprintf(what, sizeof(what), "timing: the layer's hook costs at most 5 ns per bind and draw (%+.1f)",
                  1e6 * (ms[1] - ms[0]) / kDraws);
    check(ms[1] - ms[0] <= 5e-6 * kDraws, what);
    check(SUCCEEDED(device->GetDeviceRemovedReason()), "timing: device fine after the hook comparison");
}

// The device's shader creation through a vtable copy, as the probe leaves the game's device. Counted: it passes on.
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                         ID3D11VertexShader**);
CreateVertexShaderFn realCreateVertexShader = nullptr;
int creates = 0;
HRESULT STDMETHODCALLTYPE counted_create(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* l,
                                         ID3D11VertexShader** vs) {
    ++creates;
    return realCreateVertexShader(d, code, size, l, vs);
}

// RSSetViewports as one MinHook hook on the function its entry holds with multithread protection off (the layer's
// hooks before 2026-11-09): counted, to show what such a hook misses once protection is on.
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
RSSetViewportsFn realRSSetViewports = nullptr;
int viewportSets = 0;
void STDMETHODCALLTYPE counted_viewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps) {
    ++viewportSets;
    realRSSetViewports(c, n, vps);
}

}  // namespace

int main(int argc, char** argv) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("FAIL no hardware D3D11 device\n");
        return 1;
    }
    const nvapi::Support s = nvapi::query(device.Get());
    char what[200];
    std::snprintf(what, sizeof(what), "NVAPI: loaded %d, single pass stereo %d, XYZW %d, multi-view %d (status %d %s)",
                  s.loaded, s.singlePassStereo, s.xyzw, s.multiView, s.status, nvapi::status_name(s.status));
    check(s.loaded && s.xyzw, what);
    for (const nvapi::Gpu& gpu : nvapi::gpus())
        std::printf("     GPU %s: architecture 0x%x, two views %d\n", gpu.name.c_str(), gpu.architecture, gpu.twoViews);
    check(nvapi::two_views_gpu() != nullptr, "nvapi_gpus: a GPU with two views found without a device (the launcher's check)");
    if (!s.xyzw)
        return 1;
    if (argc > 1 && std::strcmp(argv[1], "--timing") == 0) {
        timing(device.Get(), context.Get());
        std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
    }
    if (argc > 1)
        shaders(device.Get(), argv[1]);

    const Variant control = {"control (mode off)", false, false, 0, 1};
    const Result c = draw(device.Get(), context.Get(), control);
    print(control, c);
    const int area0 = c.half[0][0].count;
    std::snprintf(what, sizeof(what), "control: view 0 only, %d px in the left half", area0);
    check(c.ok && area0 > 3000 && c.half[0][1].count == 0, what);
    // Measured 2026-11-08 (RTX 5070 Ti): with the independent mask NV_VIEWPORT_MASK's low 16 bits route view 0 and
    // its high 16 bits view 1; without it both views follow the low bits; NVAPI's second-mask type isn't read.
    const Variant viewports = {"independent mask 0x00020001 (the layer's)", true, true, 0, 1, true, 0x00020001};
    const Variant swapped = {"independent mask 0x00010002 (swapped)", true, true, 0, 1, true, 0x00010002};
    const Variant slices = {"no mask, view 1 a slice further", true, false, 1, 2, false, 0};
    const Result r = draw(device.Get(), context.Get(), viewports);
    print(viewports, r);
    check(r.ok && std::abs(r.half[0][0].count - area0) <= area0 / 50 && view1_at(r.half[0][1], 128, area0),
          "two views into two viewports of one target: view 0 unchanged, view 1 in the right half");
    const Result rs = draw(device.Get(), context.Get(), swapped);
    print(swapped, rs);
    check(rs.ok && view1_at(rs.half[0][0], 0, area0) && std::abs(rs.half[0][1].count - area0) <= area0 / 50,
          "the mask's halves route the views: swapped, view 1 left and view 0 right");
    const Result rl = draw(device.Get(), context.Get(), slices);
    print(slices, rl);
    check(rl.ok && std::abs(rl.half[0][0].count - area0) <= area0 / 50 && view1_at(rl.half[1][0], 0, area0),
          "two views into two slices: view 1 in slice 1");

    // 5. The device on a vtable copy, as the probe leaves the game's (engine/object_vtable.h): NVAPI's flags, its shader
    //    creation and the layer's two-view draw as on the original.
    {
        simqvfr::ObjectVtable deviceCopy;
        const bool copied = deviceCopy.install(device.Get(), simqvfr::kD3D11DeviceSlots,
                                               {{12, reinterpret_cast<void*>(&counted_create),
                                                 reinterpret_cast<void**>(&realCreateVertexShader)}});
        std::snprintf(what, sizeof(what), "the device on a vtable copy (%zu entries)", deviceCopy.slots());
        check(copied, what);
        const nvapi::Support sc = nvapi::query(device.Get());
        check(sc.loaded && sc.xyzw && sc.singlePassStereo == s.singlePassStereo,
              "the device on a copy: NVAPI's flags as before");
        Variant onCopy = viewports;
        onCopy.name = "independent mask 0x00020001, device on a copy";
        const Result rc = draw(device.Get(), context.Get(), onCopy);
        print(onCopy, rc);
        std::snprintf(what, sizeof(what),
                      "the device on a copy: the two-view draw lands as before (NVAPI's shader creation called the "
                      "device's entry %d times)",
                      creates);
        check(rc.ok && std::abs(rc.half[0][0].count - r.half[0][0].count) <= area0 / 50 &&
                  std::abs(rc.half[0][1].count - r.half[0][1].count) <= area0 / 50 && view1_at(rc.half[0][1], 128, area0),
              what);
        deviceCopy.remove();
    }

    // 6. What d3d11 does to the immediate context's vtable (it lives inside the context): the entries it rewrites as the
    //    context goes idle (a flush, the GPU done) and busy (a command), and as multithread protection goes on; and what
    //    a MinHook hook on one function an entry held misses then (why the layer hooks every function each entry is seen
    //    holding, games/lmu/mvr_pass.cpp). 2026-11-09, RTX 5070 Ti: 24 entries (every command's) between idle and busy,
    //    130 with protection, all of the layer's ten among those. Once protection has been on, the entries hold other
    //    plain functions than at first; a hook on one of those saw no call with protection on (its locking function calls
    //    the first plain one instead).
    {
        void** vt = *reinterpret_cast<void***>(context.Get());
        const std::size_t n = simqvfr::kD3D11ContextSlots;
        ComPtr<ID3D11Query> done;
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11Multithread> mt;
        const D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        const D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const float zero[4] = {};
        const auto idle = [&] {
            context->End(done.Get());
            context->Flush();
            BOOL finished = FALSE;
            while (context->GetData(done.Get(), &finished, sizeof(finished), 0) != S_OK || !finished)
                Sleep(0);
        };
        // The entries that differ between two snapshots, and how many of them are the layer's.
        const auto rewritten = [&](const std::vector<void*>& a, const std::vector<void*>& b, int& ours) {
            std::string list;
            int count = 0;
            ours = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (a[i] != b[i]) {
                    ++count;
                    if (list.size() < 200)
                        list += " " + std::to_string(i);
                    for (unsigned slot : lmu::context_slot::kHooked)
                        ours += i == slot ? 1 : 0;
                }
            return std::to_string(count) + (count ? ":" + list : "");
        };
        if (SUCCEEDED(device->CreateQuery(&qd, &done)) && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &buffer)) &&
            SUCCEEDED(context.As(&mt))) {
            const BOOL was = mt->SetMultithreadProtected(FALSE);
            idle();
            const std::vector<void*> idleSet(vt, vt + n);
            context->UpdateSubresource(buffer.Get(), 0, nullptr, zero, 0, 0);
            const std::vector<void*> busySet(vt, vt + n);
            idle();
            const std::vector<void*> idleAgain(vt, vt + n);
            int ours = 0;
            std::printf("     entries d3d11 rewrites between idle and busy: %s\n",
                        rewritten(idleSet, busySet, ours).c_str());
            check(idleAgain == idleSet && idleSet != busySet, "d3d11 rewrites its context's vtable between idle and busy");
            void* const firstPlain = vt[lmu::context_slot::kRSSetViewports];
            mt->SetMultithreadProtected(TRUE);
            const std::vector<void*> lockedSet(vt, vt + n);
            const std::string list = rewritten(idleAgain, lockedSet, ours);
            std::printf("     entries d3d11 rewrites as multithread protection goes on: %s\n", list.c_str());
            std::snprintf(what, sizeof(what), "multithread protection on: d3d11 rewrites %d of the layer's eleven entries",
                          ours);
            check(ours > 0, what);
            mt->SetMultithreadProtected(FALSE);
            // The way before: one hook, on the function RSSetViewports' entry holds now (protection off, after it was on).
            void* const plain = vt[lmu::context_slot::kRSSetViewports];
            const D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
            const MH_STATUS init = MH_Initialize();
            const bool hooked = (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) &&
                                MH_CreateHook(plain, reinterpret_cast<void*>(&counted_viewports),
                                              reinterpret_cast<void**>(&realRSSetViewports)) == MH_OK &&
                                MH_EnableHook(plain) == MH_OK;
            context->RSSetViewports(1, &vp);
            const int off = viewportSets;
            mt->SetMultithreadProtected(TRUE);
            context->RSSetViewports(1, &vp);
            const int on = viewportSets - off;
            std::printf("     one hook on RSSetViewports' plain function (%s the first one): it sees %d of 1 call with "
                        "protection off, %d of 1 with it on\n",
                        plain == firstPlain ? "still" : "not", off, on);
            check(hooked, "one hook on RSSetViewports' plain function");
            mt->SetMultithreadProtected(was);
            if (hooked)
                MH_RemoveHook(plain);
        } else {
            check(false, "the context's vtable: a query, a buffer and its multithread protection");
        }
    }

    // 7. The layer's own install on this device (games/lmu/mvr_pass.h), with the focus post (DrawIndexed too): every
    //    function the eleven entries are seen holding hooked; with multithread protection on, the entries hold none it
    //    hasn't seen.
    {
        lmu::mvr_focus_replay(true);
        const bool installed = lmu::mvr_pass_install(device.Get());
        const uint64_t hookedNow = lmu::mvr_pass_stats().contextFunctions;
        std::snprintf(what, sizeof(what),
                      "the layer's install: %llu functions hooked for %llu entries (eleven with the focus post; two each "
                      "at least), %llu of them ResolveSubresource's",
                      static_cast<unsigned long long>(hookedNow),
                      static_cast<unsigned long long>(lmu::mvr_pass_stats().contextEntries),
                      static_cast<unsigned long long>(lmu::mvr_pass_stats().resolveFunctions));
        check(installed && lmu::mvr_pass_stats().contextEntries == 11 && hookedNow >= 22, what);
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(context.As(&mt))) {
            const BOOL was = mt->SetMultithreadProtected(TRUE);
            lmu::mvr_pass_begin(0, nullptr, false);  // a pass's start hooks any function not seen before
            lmu::mvr_pass_end(0);
            context->Flush();
            lmu::mvr_pass_begin(0, nullptr, false);
            lmu::mvr_pass_end(0);
            mt->SetMultithreadProtected(was);
            check(lmu::mvr_pass_stats().contextFunctions == hookedNow,
                  "the layer's install: protection on, idle and busy: no function it hadn't hooked");
        }
        check(SUCCEEDED(device->GetDeviceRemovedReason()), "the layer's install: device fine");
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

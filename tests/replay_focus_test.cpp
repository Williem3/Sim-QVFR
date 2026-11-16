// Offline check of the focus replay (probe/replay_probe.h, mode 3, every stereo list) on a WARP device: a miniature of
// ACE's single-pass-stereo scene (two viewports side by side, the eye from SV_InstanceID routed by SV_ViewportArrayIndex,
// projections read from root parameter 1 in ACE's pass-constant layout, reverse-Z): a depth-only list, a colour list
// (R11G11B10_FLOAT, depth test EQUAL, a colour that is a smooth function of world position) and a half-width pass with
// targets of its own (ACE's `flags` pass), in one batch. The probe replays them for the focus views into twins of their
// targets, the constants remapped off-axis, and at sample frames also into eye-size references, then compares depth and
// colour through the remap. Twins follow each game target: none is remade when lists of different widths alternate, and
// each is remade once when the game makes its targets again at another size (frame 2).
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include "games/ace/stereo_flag.h"
#include "probe/replay_probe.h"
#include "xr.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// ACE's projection rows: x (sx, 0, ox, 0), y (0, sy, oy, 0), z (0, 0, n, f) reverse-Z, w (0, 0, -1, 0).
void put_projection(float* rows, int base, float sx, float ox, float sy, float oy) {
    float* r = rows + base * 4;
    const float m[16] = {sx, 0, ox, 0, 0, sy, oy, 0, 0, 0, 6.6667e-6f, 0.1f, 0, 0, -1, 0};
    std::memcpy(r, m, sizeof(m));
}

constexpr char kShader[] = R"(
cbuffer PassConstants : register(b1) { float4 rows[128]; };
struct Out { float4 pos : SV_Position; float3 world : WORLD; uint vp : SV_ViewportArrayIndex; };
static const float3 kVerts[12] = {
    // a floor from 1.5 m to 30 m ahead, 1 m below the eye
    float3(-10, -1, -1.5), float3(10, -1, -1.5), float3(-10, -1, -30),
    float3(10, -1, -1.5), float3(10, -1, -30), float3(-10, -1, -30),
    // a wall 5 m ahead, slanted
    float3(-1, -1, -4), float3(1, -1, -6), float3(-1, 1, -4),
    float3(1, -1, -6), float3(1, 1, -6), float3(-1, 1, -4)};
Out vs(uint v : SV_VertexID, uint eye : SV_InstanceID) {
    const float4 p = float4(kVerts[v], 1);
    const uint b = eye == 0 ? 13 : 95;
    Out o;
    o.pos = float4(dot(rows[b], p), dot(rows[b + 1], p), dot(rows[b + 2], p), dot(rows[b + 3], p));
    o.world = kVerts[v];
    o.vp = eye;
    return o;
}
float4 ps(Out i) : SV_Target {
    const float3 w = i.world;
    // Slow enough that bilinear between eye pixels holds at 30 m (one eye pixel spans ~0.3 m of floor there).
    return float4(0.5 + 0.4 * sin(w.x * 0.4), 0.5 + 0.4 * sin(w.z * 0.25), 0.5 + 0.4 * cos((w.x + w.y) * 0.3), 1) * 2.0;
}
)";

}  // namespace

// R2b's CPU math: the focus inverse projection for ACE's tile shaders (out = M v, M's columns in the registers). A focus
// view point (u, v) unprojected with it and projected with the eye's projection must land at the eye's centre + (u, v)
// times the box.
void check_inverse_projection() {
    const double sx = 0.8159, ox = -0.16857, sy = 1.016, oy = 0.02, box = 0.2;
    const double p[4][4] = {{sx, 0, ox, 0}, {0, sy, oy, 0}, {0, 0, 6.6667e-6, 0.1}, {0, 0, -1, 0}};  // clip = P view
    // inverse(P) by the probe's own convention check: build M = inverse(P) numerically (Gauss-Jordan on doubles).
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = j < 4 ? p[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
    }
    float eye[16], focus[16];
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            eye[k * 4 + i] = static_cast<float>(a[i][k + 4]);  // registers = columns
    check(simqvfr::replay_focus_inverse_projection(eye, box, focus), "focus inverse projection computed");
    double worst = 0;
    for (double u : {-0.9, -0.3, 0.0, 0.5, 0.95})
        for (double v : {-0.8, 0.0, 0.7})
            for (double z : {0.05, 0.002}) {  // reverse-Z depths
                const double clip[4] = {u, v, z, 1};
                double view[4] = {};
                for (int i = 0; i < 4; ++i)
                    for (int k = 0; k < 4; ++k) view[i] += focus[k * 4 + i] * clip[k];
                double e[4] = {};
                for (int i = 0; i < 4; ++i)
                    for (int k = 0; k < 4; ++k) e[i] += p[i][k] * view[k];
                const double ex = e[0] / e[3], ey = e[1] / e[3];
                worst = std::max({worst, std::fabs(ex - (-ox + u * box)), std::fabs(ey - (-oy + v * box))});
            }
    std::printf("inverse projection: worst NDC error %.2e\n", worst);
    check(worst < 1e-4, "focus NDC unprojects to the eye's centre + focus NDC x box");
}

int SIM_QVFR_TEST_MAIN() {
    check_inverse_projection();
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring logPath = std::wstring(temp) + L"simqvfr-replay-focus-test.log";
    DeleteFileW(logPath.c_str());
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", logPath.c_str());
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::puts("no WARP D3D12 device");
        return 2;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (!options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation) {
        std::puts("skipped: no viewport index from the vertex shader on this device");
        return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    simqvfr::ReplayFocusSettings settings;
    settings.boxFraction = 0.25;
    settings.pixelScale = 2.0;
    settings.saveFirst = 0;
    settings.saveEvery = 2;
    settings.saveCount = 2;
    settings.allTargets = true;
    settings.fog = true;  // compiles the fog-constants shader (no fog passes here: nothing else changes)
    simqvfr::ace_known_build_for_tests(true);  // (this test exe is not ACE)
    check(simqvfr::replay_probe_start(device.Get(), 3, queue.Get(), settings), "probe installs in mode 3");

    // The "game": root 0 unused, root 1 the pass constants; depth-only, colour and half-width pipelines, reverse-Z.
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {1, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error, vs, ps;
    ComPtr<ID3D12RootSignature> root;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "stereo-scene", nullptr, nullptr, "vs", "vs_5_1", 0, 0, &vs, &error)) ||
        FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "stereo-scene", nullptr, nullptr, "ps", "ps_5_1", 0, 0, &ps, &error))) {
        std::printf("shaders: %s\n", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> depthPipeline, colourPipeline, halfPipeline;
    check(SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&depthPipeline))), "game depth pipeline");
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    check(SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&colourPipeline))), "game colour pipeline");
    // A half-width pass with targets of its own (ACE's `flags` pass): R8G8B8A8 and its own depth, depth written.
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    check(SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&halfPipeline))), "game half-width pipeline");

    // Pass constants in ACE's layout: left eye (asymmetric, centre to the right), right eye mirrored.
    float rows[512] = {};
    for (int base : {13, 17, 21, 25})
        put_projection(rows, base, 0.8159f, -0.16857f, 1.016f, 0.02f);
    for (int base : {95, 103, 107})
        put_projection(rows, base, 0.8159f, 0.16857f, 1.016f, 0.02f);
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bufferDesc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> constants;
    device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&constants));
    void* mapped = nullptr;
    constants->Map(0, nullptr, &mapped);
    std::memcpy(mapped, rows, sizeof(rows));
    constants->Unmap(0, nullptr);

    // The game's targets, their views created after the probe (it learns formats and samples from them).
    UINT eyeW = 256, eyeH = 200;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2}, rtvHeapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    ComPtr<ID3D12DescriptorHeap> dsvHeap, rtvHeap;
    device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap));
    device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE halfDsv{dsv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};
    const D3D12_CPU_DESCRIPTOR_HANDLE halfRtv{rtv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    ComPtr<ID3D12Resource> depth, colour, halfDepth, halfColour;
    const auto make = [&](ComPtr<ID3D12Resource>& r, UINT width, DXGI_FORMAT format, bool isDepth,
                          D3D12_CPU_DESCRIPTOR_HANDLE view) {
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, eyeH, 1, 1, format, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN,
                              isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        r.Reset();  // the queue is idle between frames here
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                        isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        &clear, IID_PPV_ARGS(&r));
        if (isDepth)
            device->CreateDepthStencilView(r.Get(), nullptr, view);
        else
            device->CreateRenderTargetView(r.Get(), nullptr, view);
    };
    const auto make_targets = [&] {
        make(depth, eyeW * 2, DXGI_FORMAT_D32_FLOAT, true, dsv);
        make(colour, eyeW * 2, DXGI_FORMAT_R11G11B10_FLOAT, false, rtv);
        make(halfDepth, eyeW, DXGI_FORMAT_D32_FLOAT, true, halfDsv);  // half width: eyeW / 2 per eye
        make(halfColour, eyeW, DXGI_FORMAT_R8G8B8A8_UNORM, false, halfRtv);
    };
    make_targets();

    ComPtr<ID3D12CommandAllocator> allocators[3];
    ComPtr<ID3D12GraphicsCommandList> lists[3];
    for (int i = 0; i < 3; ++i) {
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i]));
        lists[i]->Close();
    }
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fenceValue = 0;

    // One frame of the "game": the stereo depth list, the stereo colour list and a half-width stereo pass, one batch.
    const auto frame = [&] {
        for (int i = 0; i < 3; ++i) {
            const float w = static_cast<float>(i == 2 ? eyeW / 2 : eyeW), h = static_cast<float>(eyeH);
            const D3D12_VIEWPORT views[2] = {{0, 0, w, h, 0, 1}, {w, 0, w, h, 0, 1}};
            const D3D12_RECT rects[2] = {{0, 0, LONG(w), LONG(h)}, {LONG(w), 0, LONG(2 * w), LONG(h)}};
            ID3D12GraphicsCommandList* l = lists[i].Get();
            allocators[i]->Reset();
            l->Reset(allocators[i].Get(), nullptr);
            const float black[4] = {};
            if (i == 0) {
                l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                l->ClearRenderTargetView(rtv, black, 0, nullptr);
            } else if (i == 2) {
                l->ClearDepthStencilView(halfDsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                l->ClearRenderTargetView(halfRtv, black, 0, nullptr);
            }
            l->SetGraphicsRootSignature(root.Get());
            l->SetGraphicsRootConstantBufferView(1, constants->GetGPUVirtualAddress());
            if (i == 0)
                l->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            else if (i == 1)
                l->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            else
                l->OMSetRenderTargets(1, &halfRtv, FALSE, &halfDsv);
            l->RSSetViewports(2, views);
            l->RSSetScissorRects(2, rects);
            l->SetPipelineState(i == 0 ? depthPipeline.Get() : i == 1 ? colourPipeline.Get() : halfPipeline.Get());
            l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            l->DrawInstanced(12, 2, 0, 0);
            l->Close();
        }
        ID3D12CommandList* batch[] = {lists[0].Get(), lists[1].Get(), lists[2].Get()};
        queue->ExecuteCommandLists(3, batch);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, 10000);
        simqvfr::replay_probe_frame_end();
    };

    simqvfr::ReplayFocusCompare compare;
    const auto wait_compare = [&](uint64_t sample) {
        for (int i = 0; i < 200 && compare.sample < sample; ++i) {
            Sleep(50);
            compare = simqvfr::replay_probe_focus_compare();
        }
        std::printf("compare %llu: depth %llu pixels, %.2f%% agree, shift (%d, %d); colour %llu pixels, %.2f%% agree\n",
                    static_cast<unsigned long long>(compare.sample), static_cast<unsigned long long>(compare.pixels),
                    compare.agree * 100, compare.shiftX, compare.shiftY,
                    static_cast<unsigned long long>(compare.colourPixels), compare.colourAgree * 100);
        std::printf("compare %llu: colour at the eye's pixel size %llu pixels, %.2f%% agree, %.2f%% in the neighbourhood\n",
                    static_cast<unsigned long long>(compare.sample),
                    static_cast<unsigned long long>(compare.colourEyePixels), compare.colourEyeAgree * 100,
                    compare.colourHoodAgree * 100);
    };
    // Samples at frames 0 and 2, each copied at the next frame's first replay.
    for (int f = 0; f < 4; ++f) {
        if (f == 2) {
            wait_compare(1);
            check(compare.sample == 1 && compare.pixels > 2000, "first sample compared");
            check(compare.agree > 0.97 && compare.shiftX == 0 && compare.shiftY == 0,
                  "depth: focus replay and eye reference agree through the remap, at shift (0, 0)");
            check(compare.colourPixels > 2000 && compare.colourAgree > 0.95, "colour: they agree too");
            check(compare.colourEyePixels > 100 && compare.colourEyeAgree > 0.95, "colour at the eye's pixel size too");
            check(compare.colourHoodAgree >= compare.colourEyeAgree, "the neighbourhood test is at least as tolerant");
            eyeW = 320;
            eyeH = 240;
            make_targets();
        }
        frame();
    }
    frame();  // frame 4 gets the second sample (frame 2) copied, before its clear
    const simqvfr::ReplayProbeStats stats = simqvfr::replay_probe_take_stats();
    std::printf("lists replayed %llu, executed %llu; twins made %llu, made again %llu\n",
                static_cast<unsigned long long>(stats.listsReplayed), static_cast<unsigned long long>(stats.focusReplays),
                static_cast<unsigned long long>(stats.twinsMade), static_cast<unsigned long long>(stats.twinsRemade));
    check(stats.listsReplayed == 15 && stats.focusReplays == 15, "every frame's three stereo lists replayed and executed");
    // Twins are made once per game target (4) and again once when the game makes its targets again (4): never per list.
    check(stats.twinsMade == 8 && stats.twinsRemade == 4,
          "twins follow each game target: none remade between the full- and half-width lists");
    wait_compare(2);
    check(compare.sample == 2, "the second sample, after the game's targets were made again, was compared");
    check(compare.pixels > 3000, "at the new size (more focus pixels)");
    check(compare.agree > 0.97 && compare.shiftX == 0 && compare.shiftY == 0, "depth agrees at shift (0, 0)");
    check(compare.colourAgree > 0.95, "colour agrees");
    check(SUCCEEDED(device->GetDeviceRemovedReason()), "the device is fine");
    CloseHandle(event);
    std::printf("%s\n", failures ? "replay focus test FAILED" : "replay focus test passed");
    return failures ? 1 : 0;
}

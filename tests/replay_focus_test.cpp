// Offline check of the focus depth replay (probe/replay_probe.h, mode 3) on a WARP device: a miniature of ACE's
// single-pass-stereo depth pass (depth only, two viewports side by side, the eye from SV_InstanceID routed by
// SV_ViewportArrayIndex, projections read from root parameter 1 in ACE's pass-constant layout, reverse-Z) is recorded,
// replayed by the probe for both focus views into its own target with the off-axis remap, and executed after the
// game's list. At the sample frame the probe also replays it unchanged into its eye reference and compares the two
// through the remap: nearly every pixel must agree, at shift (0, 0).
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <dxgi1_4.h>
#include <wrl/client.h>

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
struct Out { float4 pos : SV_Position; uint vp : SV_ViewportArrayIndex; };
static const float3 kVerts[12] = {
    // a floor from 1.5 m to 30 m ahead, 1 m below the eye
    float3(-10, -1, -1.5), float3(10, -1, -1.5), float3(-10, -1, -30),
    float3(10, -1, -1.5), float3(10, -1, -30), float3(-10, -1, -30),
    // a wall 5 m ahead, slanted
    float3(-1, -1, -4), float3(1, -1, -6), float3(-1, 1, -4),
    float3(1, -1, -6), float3(1, 1, -6), float3(-1, 1, -4)};
Out main(uint v : SV_VertexID, uint eye : SV_InstanceID) {
    const float4 p = float4(kVerts[v], 1);
    const uint b = eye == 0 ? 13 : 95;
    Out o;
    o.pos = float4(dot(rows[b], p), dot(rows[b + 1], p), dot(rows[b + 2], p), dot(rows[b + 3], p));
    o.vp = eye;
    return o;
}
)";

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", (std::wstring(temp) + L"simqvfr-replay-focus-test.log").c_str());
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
    check(simqvfr::replay_probe_start(device.Get(), 3, queue.Get(), settings), "probe installs in mode 3");

    // The "game": root 0 unused, root 1 the pass constants; a depth-only pipeline, GREATER (reverse-Z).
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {1, 0};
    const D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error, vs;
    ComPtr<ID3D12RootSignature> root;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "stereo-depth", nullptr, nullptr, "main", "vs_5_1", 0, 0, &vs, &error))) {
        std::printf("vertex shader: %s\n", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
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
    ComPtr<ID3D12PipelineState> pipeline;
    check(SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline))), "game pipeline");

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

    // The game's depth, its view created after the probe (the probe learns format and samples from it). At frame 2 the
    // game makes it again at another size (a resolution change): the probe must make its targets again.
    UINT eyeW = 256, eyeH = 200;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap));
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    ComPtr<ID3D12Resource> depth;
    const auto make_depth = [&] {
        D3D12_RESOURCE_DESC depthDesc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, eyeW * 2, eyeH, 1, 1, DXGI_FORMAT_D32_FLOAT,
                                      {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        depth.Reset();  // the queue is idle between frames here
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                        IID_PPV_ARGS(&depth));
        device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    };
    make_depth();

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fenceValue = 0;

    simqvfr::ReplayFocusCompare compare;
    const auto wait_compare = [&](uint64_t sample) {
        for (int i = 0; i < 200 && compare.sample < sample; ++i) {
            Sleep(50);
            compare = simqvfr::replay_probe_focus_compare();
        }
        std::printf("compare %llu: %llu pixels, %.2f%% agree, shift (%d, %d)\n", static_cast<unsigned long long>(compare.sample),
                    static_cast<unsigned long long>(compare.pixels), compare.agree * 100, compare.shiftX, compare.shiftY);
    };
    // Samples at frames 0 and 2, each copied at the next frame's first replay.
    for (int frame = 0; frame < 4; ++frame) {
        if (frame == 2) {
            wait_compare(1);
            check(compare.sample == 1 && compare.pixels > 2000, "first sample compared");
            check(compare.agree > 0.97 && compare.shiftX == 0 && compare.shiftY == 0,
                  "focus replay and eye reference agree through the remap, at shift (0, 0)");
            eyeW = 320;
            eyeH = 240;
            make_depth();
        }
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        list->SetGraphicsRootSignature(root.Get());
        list->SetGraphicsRootConstantBufferView(1, constants->GetGPUVirtualAddress());
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const D3D12_VIEWPORT views[2] = {{0, 0, float(eyeW), float(eyeH), 0, 1}, {float(eyeW), 0, float(eyeW), float(eyeH), 0, 1}};
        const D3D12_RECT rects[2] = {{0, 0, LONG(eyeW), LONG(eyeH)}, {LONG(eyeW), 0, LONG(2 * eyeW), LONG(eyeH)}};
        list->RSSetViewports(2, views);
        list->RSSetScissorRects(2, rects);
        list->SetPipelineState(pipeline.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(12, 2, 0, 0);
        check(SUCCEEDED(list->Close()), "the game's stereo depth list closes");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, 10000);
        simqvfr::replay_probe_frame_end();
    }
    const simqvfr::ReplayProbeStats stats = simqvfr::replay_probe_take_stats();
    check(stats.listsReplayed == 4 && stats.focusReplays == 4, "every frame's depth list replayed and executed");
    wait_compare(2);
    check(compare.sample == 2, "the second sample, after the game's depth was made again, was compared");
    check(compare.pixels > 3000, "at the new size (more focus pixels)");
    check(compare.agree > 0.97 && compare.shiftX == 0 && compare.shiftY == 0, "and agrees at shift (0, 0)");
    check(SUCCEEDED(device->GetDeviceRemovedReason()), "the device is fine");
    CloseHandle(event);
    std::printf("%s\n", failures ? "replay focus test FAILED" : "replay focus test passed");
    return failures ? 1 : 0;
}

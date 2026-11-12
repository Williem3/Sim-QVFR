// Offline check of the pass census on a WARP device: draws in known quarters and dispatches of
// known sizes must be counted per pipeline state, and a pipeline that only draws into quarters
// 0-1 flagged as "eyes only".
#include <cstdio>
#include <fstream>
#include <string>

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "probe/pass_census.h"
#include "xr.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors))) {
        std::printf("compile %s failed: %s\n", target, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // The log goes to a file of the test's own (opened at its first line), read back at the end.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring ownLog = std::wstring(temp) + L"simqvfr-pass-census-test.log";
    DeleteFileW(ownLog.c_str());
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", ownLog.c_str());
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::puts("no WARP D3D12 device");
        return 2;
    }
    if (!simqvfr::pass_census_start(device.Get()))
        return 3;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> rootBlob;
    ComPtr<ID3D12RootSignature> root;
    D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, nullptr);
    device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&root));

    const auto cs = compile("[numthreads(1,1,1)] void main() {}", "cs_5_0");
    const auto vs = compile("float4 main(uint id : SV_VertexID) : SV_Position { return float4(id, 0, 0, 1); }", "vs_5_0");
    const auto ps = compile("float4 main() : SV_Target { return 1; }", "ps_5_0");
    if (!cs || !vs || !ps)
        return 4;
    D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc{};
    computeDesc.pRootSignature = root.Get();
    computeDesc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> compute;
    device->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(&compute));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx{};
    gfx.pRootSignature = root.Get();
    gfx.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    gfx.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    gfx.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gfx.SampleMask = ~0u;
    gfx.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gfx.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gfx.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gfx.NumRenderTargets = 1;
    gfx.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gfx.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> allViews, eyesOnly;
    device->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(&allViews));
    device->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(&eyesOnly));
    if (!compute || !allViews || !eyesOnly)
        return 5;
    eyesOnly->SetName(L"eyes only pipeline");
    compute->SetName(L"test compute");

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));

    const int left[4] = {0, 64, 128, 192}, width[4] = {64, 64, 64, 64};
    simqvfr::pass_census_set_view_columns(left, width);
    simqvfr::pass_census_watch("eyes only pipeline,test compute");
    simqvfr::pass_census_set_uav_barriers(true);  // recording a UAV barrier after each dispatch must stay valid
    simqvfr::pass_census_arm(1, 1);
    simqvfr::pass_census_frame_end();  // frame 1: counting starts
    list->Dispatch(1, 1, 1);         // no pipeline yet: counted under (none)
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(compute.Get());
    list->Dispatch(4, 2, 1);
    list->Dispatch(4, 2, 1);
    list->Dispatch(8, 1, 1);
    list->SetGraphicsRootSignature(root.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(allViews.Get());
    for (const float x : {0.0f, 128.0f}) {  // quarters 0 and 2
        const D3D12_VIEWPORT v{x, 0, 64, 64, 0, 1};
        list->RSSetViewports(1, &v);
        list->DrawInstanced(3, 1, 0, 0);
    }
    const D3D12_VIEWPORT half{0, 0, 128, 64, 0, 1};
    list->RSSetViewports(1, &half);
    list->DrawIndexedInstanced(3, 1, 0, 0, 0);  // no index buffer needed to record
    list->SetPipelineState(eyesOnly.Get());
    const D3D12_VIEWPORT q1{64, 0, 64, 64, 0, 1};
    list->RSSetViewports(1, &q1);
    list->DrawInstanced(3, 1, 0, 0);
    list->Close();
    simqvfr::pass_census_frame_end();  // frame 2: report

    char logPath[MAX_PATH];
    GetEnvironmentVariableA("SIM_QVFR_LOG", logPath, MAX_PATH);
    std::ifstream log(logPath);
    std::string line;
    bool computeOk = false, allViewsOk = false, eyesOnlyOk = false, noneOk = false, falseFlag = false;
    int watched = 0, watchedDispatches = 0;
    while (std::getline(log, line)) {
        if (line.find("pass census: watched eyes only pipeline draw viewport=64,0 64x64") != std::string::npos &&
            line.find("stack: sim_qvfr_tests.exe+") != std::string::npos)
            ++watched;
        if (line.find("pass census: watched test compute dispatch ") != std::string::npos &&
            line.find("stack: sim_qvfr_tests.exe+") != std::string::npos)
            ++watchedDispatches;
        if (line.find("pass census: cs") == std::string::npos && line.find("pass census: gfx") == std::string::npos)
            continue;
        std::puts(line.c_str());
        if (line.find("\"test compute\" dispatches 3.0 dims 4x2x1:2.0 8x1x1:1.0 lists direct") != std::string::npos)
            computeOk = true;
        if (line.find("draws 1.0 0.0 1.0 0.0 | 1.0 0.0 0.0 0.0") != std::string::npos)
            allViewsOk = line.find("eyes only\"") == std::string::npos;
        if (line.find("\"eyes only pipeline\" draws 0.0 1.0 0.0 0.0 | 0.0 0.0 0.0 0.0 (indirect 0.0)  <-- eyes only") !=
            std::string::npos)
            eyesOnlyOk = true;
        if (line.find("draws 1.0 0.0 1.0 0.0") != std::string::npos && line.find("<-- eyes only") != std::string::npos)
            falseFlag = true;
        if (line.find("cs 0000000000000000 \"(none)\" dispatches 1.0 dims 1x1x1:1.0") != std::string::npos)
            noneOk = true;
    }
    std::printf("compute %s, all-views gfx %s, eyes-only gfx %s, no-pipeline dispatch %s, false flag %s\n",
                computeOk ? "ok" : "WRONG", allViewsOk ? "ok" : "WRONG", eyesOnlyOk ? "ok" : "WRONG",
                noneOk ? "ok" : "WRONG", falseFlag ? "WRONG" : "none");
    std::printf("watched draw lines %d (expected 1), watched dispatch lines %d (expected 3)\n", watched, watchedDispatches);
    const bool ok = computeOk && allViewsOk && eyesOnlyOk && noneOk && !falseFlag && watched == 1 && watchedDispatches == 3;
    std::printf("RESULT: %s\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}

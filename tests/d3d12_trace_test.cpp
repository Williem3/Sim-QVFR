// Offline check of the D3D12 swapchain-write trace on a WARP device: a registered "swapchain image"
// bound through OMSetRenderTargets and BeginRenderPass must be reported, an unregistered one not.
#include <cstdio>
#include <fstream>
#include <string>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include "probe/d3d12_trace.h"
#include "xr.h"

using Microsoft::WRL::ComPtr;

int main() {
    // The log goes to a file of the test's own (opened at its first line), read back at the end.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring ownLog = std::wstring(temp) + L"simqvfr-d3d12-trace-test.log";
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
    if (!simqvfr::d3d12_trace_start(device.Get()))
        return 3;

    auto make_texture = [&](UINT width) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        ComPtr<ID3D12Resource> texture;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&texture));
        return texture;
    };
    ComPtr<ID3D12Resource> swapchainImage = make_texture(256), other = make_texture(128);
    ComPtr<ID3D12Resource> internal = make_texture(2048), internalAgain = make_texture(2048);
    simqvfr::d3d12_trace_register_image(swapchainImage.Get(), 256, 64, 1);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE swapchainRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE otherRtv{swapchainRtv.ptr +
                                         device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    D3D12_CPU_DESCRIPTOR_HANDLE internalRtv{otherRtv.ptr +
                                            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    device->CreateRenderTargetView(swapchainImage.Get(), nullptr, swapchainRtv);
    device->CreateRenderTargetView(other.Get(), nullptr, otherRtv);
    device->CreateRenderTargetView(internal.Get(), nullptr, internalRtv);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));

    list->OMSetRenderTargets(1, &otherRtv, FALSE, nullptr);  // not a swapchain image: no report
    list->OMSetRenderTargets(1, &swapchainRtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{128, 0, 128, 64, 0, 1};
    list->RSSetViewports(1, &viewport);
    D3D12_RENDER_PASS_RENDER_TARGET_DESC pass{swapchainRtv};
    pass.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
    pass.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
    list->BeginRenderPass(1, &pass, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    list->EndRenderPass();
    list->OMSetRenderTargets(1, &internalRtv, FALSE, nullptr);  // first bind: logged with a stack
    list->OMSetRenderTargets(1, &internalRtv, FALSE, nullptr);  // repeat outside a sequence window: silent
    list->Close();

    char logPath[MAX_PATH];
    GetEnvironmentVariableA("SIM_QVFR_LOG", logPath, MAX_PATH);
    std::ifstream log(logPath);
    std::string line;
    int omHits = 0, passHits = 0, viewportLines = 0, falseHits = 0, allocations = 0, firstBinds = 0, repeatBinds = 0;
    while (std::getline(log, line)) {
        if (line.find("via=OMSetRenderTargets target=256x64 image=1") != std::string::npos) ++omHits;
        if (line.find("via=BeginRenderPass target=256x64 image=1") != std::string::npos) ++passHits;
        if (line.find("viewport=128,0 128x64") != std::string::npos) ++viewportLines;
        if (line.find("target=128x") != std::string::npos || line.find(" 128x64") != std::string::npos &&
                                                                   line.find("viewport") == std::string::npos)
            ++falseHits;
        if (line.find("texture allocation 2048x64") != std::string::npos && line.find("stack:") != std::string::npos)
            ++allocations;
        if (line.find("render target first bind 2048x64") != std::string::npos) ++firstBinds;
        if (line.find("rt bind 2048x64") != std::string::npos) ++repeatBinds;
    }
    std::printf("OMSetRenderTargets hits=%d BeginRenderPass hits=%d viewport lines=%d false hits=%d\n", omHits, passHits,
                viewportLines, falseHits);
    std::printf("allocations=%d (expected 1: same description twice) first binds=%d repeat binds=%d (expected 0)\n",
                allocations, firstBinds, repeatBinds);
    const bool ok = omHits == 1 && passHits == 1 && viewportLines == 1 && falseHits == 0 && allocations == 1 &&
                    firstBinds == 1 && repeatBinds == 0;
    std::printf("RESULT: %s\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}

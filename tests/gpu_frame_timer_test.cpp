// Offline check of the GPU frame timer (probe/gpu_frame_timer.h) on a WARP device: frames with
// heavy GPU work between their begin and rendered marks must time longer than empty frames, every
// finished frame must be counted, and a frame without a rendered mark must not be.
#include <cstdio>
#include <string>

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "probe/gpu_frame_timer.h"

using Microsoft::WRL::ComPtr;
using namespace simqvfr;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    failures += !ok;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // The log goes to a file of the test's own (opened at its first line), read back at the end.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring logPath = std::wstring(temp) + L"simqvfr-gpu-timer-test.log";
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
    D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return 2;
    expect(gpu_frame_timer_start(device.Get(), queue.Get()), "timer starts on a direct queue");

    // Heavy work: clears of a large render target.
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 2048;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               nullptr, IID_PPV_ARGS(&target))) ||
        FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvs))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return 2;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    list->Close();
    uint64_t fenceValue = 0;
    const auto wait_idle = [&] {
        queue->Signal(fence.Get(), ++fenceValue);
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fenceValue, done);
        WaitForSingleObject(done, 10000);
        CloseHandle(done);
    };
    const auto frame = [&](bool heavy, bool rendered) {
        gpu_frame_timer_mark(GpuMark::kBegin);
        if (heavy) {
            allocator->Reset();
            list->Reset(allocator.Get(), nullptr);
            const float colour[4] = {0.2f, 0.4f, 0.6f, 1};
            for (int i = 0; i < 40; ++i)
                list->ClearRenderTargetView(rtv, colour, 0, nullptr);
            list->Close();
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
        }
        if (rendered) {
            gpu_frame_timer_mark(GpuMark::kRendered);
            gpu_frame_timer_mark(GpuMark::kComposed);
        }
        wait_idle();  // the allocator is reused next frame
    };

    for (int i = 0; i < 6; ++i)
        frame(false, true);
    frame(false, false);  // no rendered mark: not counted
    gpu_frame_timer_mark(GpuMark::kBegin);  // ends the last frame so it is harvested
    const GpuFrameStats empty = gpu_frame_timer_collect();
    std::printf("empty frames: %zu timed, p50 %.3f ms, interval p50 %.3f ms\n", empty.frames, empty.renderP50, empty.intervalP50);
    expect(empty.frames == 6, "every finished frame with a rendered mark is counted");

    for (int i = 0; i < 6; ++i)
        frame(true, true);
    gpu_frame_timer_mark(GpuMark::kBegin);
    const GpuFrameStats heavy = gpu_frame_timer_collect();
    std::printf("heavy frames: %zu timed, p50 %.3f ms, p90 %.3f ms, compose %.3f ms, interval p50 %.3f ms, skipped %llu\n",
                heavy.frames, heavy.renderP50, heavy.renderP90, heavy.composeAvg, heavy.intervalP50,
                static_cast<unsigned long long>(heavy.skipped));
    expect(heavy.frames == 6 && heavy.skipped == 0, "heavy frames are all timed");
    expect(heavy.renderP50 > empty.renderP50 * 2 && heavy.renderP50 > 0.05, "heavy GPU work times longer than none");
    expect(heavy.intervalP50 > 0, "consecutive frames give an interval");

    // The log keeps its own samples: the overlay's two collects above did not take them, so the report
    // logs all twelve timed frames (into SIM_QVFR_LOG, read back).
    gpu_frame_timer_report();
    std::string logged;
    if (FILE* f = _wfopen(logPath.c_str(), L"rb")) {
        char buffer[4096];
        for (size_t n; (n = fread(buffer, 1, sizeof(buffer), f)) > 0;)
            logged.append(buffer, n);
        fclose(f);
    }
    const bool logsAll = logged.find("over 12 frames") != std::string::npos;
    expect(logsAll, "the log's report has every frame, whatever the overlay collected");
    if (!logsAll)
        std::printf("     logged: %s\n", logged.c_str());
    const GpuFrameStats after = gpu_frame_timer_collect();
    expect(after.frames == 0, "and the report leaves the overlay's window alone");
    gpu_frame_timer_stop();
    std::printf("RESULT: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}

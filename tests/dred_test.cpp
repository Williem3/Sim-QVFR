// Offline check of the DRED report: enable DRED, create a WARP device, run a named command list
// with a marker, remove the device on purpose (ID3D12Device5::RemoveDevice) and check the log.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>

#include "probe/dred.h"
#include "xr.h"

int main() {
    // The log goes to a file of the test's own (opened at its first line), read back at the end.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring ownLog = std::wstring(temp) + L"simqvfr-dred-test.log";
    DeleteFileW(ownLog.c_str());
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", ownLog.c_str());
    char logPath[MAX_PATH] = {};
    GetEnvironmentVariableA("SIM_QVFR_LOG", logPath, MAX_PATH);
    if (!simqvfr::dred_enable()) {
        std::puts("RESULT: FAIL (dred_enable)");
        return 1;
    }
    const bool debugLayer = simqvfr::d3d12_debug_enable();
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device5* device = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::puts("RESULT: FAIL (no WARP device with ID3D12Device5)");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    queue->SetName(L"test queue");
    list->SetName(L"test list");
    // ACE's failure: resetting an allocator while its command list is still recording.
    simqvfr::d3d12_debug_attach(device);
    const HRESULT earlyReset = allocator->Reset();
    static const char marker[] = "views4 test pass";
    list->SetMarker(0, marker, sizeof(marker));
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    device->RemoveDevice();
    const bool removed = simqvfr::dred_report_if_removed(device);

    std::ifstream in(logPath);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    const bool header = log.find("dred: DEVICE REMOVED") != std::string::npos;
    const bool crumbs = log.find("command lists in the breadcrumb log") != std::string::npos ||
                        log.find("no breadcrumbs") != std::string::npos;
    const bool debugError = log.find("d3d12 debug ERROR") != std::string::npos;
    std::printf("removed=%d report header=%d breadcrumb section=%d; debug layer=%d early reset hr=0x%08lx "
                "logged error=%d\n",
                removed, header, crumbs, debugLayer, static_cast<unsigned long>(earlyReset), debugError);
    const bool ok = removed && header && crumbs && (!debugLayer || debugError);
    std::printf("RESULT: %s\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}

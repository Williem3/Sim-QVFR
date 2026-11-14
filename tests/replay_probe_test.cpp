// Offline check of the re-emission probe (probe/replay_probe.h) on a WARP device: a command list with enough draws and a
// wide viewport is recorded and re-emitted at Close (never executed), one with too few draws or a narrow viewport is not;
// the real list still executes and its draw lands in the target (the hooks forward every call).
#include <cstdio>
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

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", (std::wstring(temp) + L"simqvfr-replay-probe-test.log").c_str());
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::puts("no WARP D3D12 device");
        return 2;
    }
    {  // a pipeline-state stream as d3dx12's subobject helpers lay it out: the shaders found by walking it, not by
       // values that equal a shader type's number elsewhere (the sample mask 1 = VS, rasterizer fields 2 = PS)
        struct alignas(void*) Root { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature* v; };
        struct alignas(void*) Mask { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; UINT v; };
        struct alignas(void*) Shader { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; };
        struct alignas(void*) Raster { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RASTERIZER_DESC v; };
        struct alignas(void*) Formats { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RT_FORMAT_ARRAY v; };
        struct Stream {
            Root root;
            Mask mask;
            Raster raster;
            Shader vs;
            Formats formats;
            Shader ps;
        } stream{};
        static const uint8_t vsBytes[8] = {}, psBytes[8] = {};
        stream.root = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, nullptr};
        stream.mask = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, 1};
        stream.raster.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        stream.raster.v.FillMode = D3D12_FILL_MODE_WIREFRAME;  // 2
        stream.raster.v.CullMode = D3D12_CULL_MODE_FRONT;      // 2
        stream.vs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {vsBytes, sizeof(vsBytes)}};
        stream.formats.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
        stream.formats.v.NumRenderTargets = 1;
        stream.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {psBytes, sizeof(psBytes)}};
        const D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
        D3D12_SHADER_BYTECODE vs{}, ps{};
        check(simqvfr::replay_probe_stream_shaders(&desc, vs, ps) && vs.pShaderBytecode == vsBytes &&
                  vs.BytecodeLength == sizeof(vsBytes) && ps.pShaderBytecode == psBytes && ps.BytecodeLength == sizeof(psBytes),
              "pipeline stream: the vertex and pixel shaders found by walking the subobjects");
        struct alignas(void*) Unknown { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; UINT v; };
        struct { Shader vs; Unknown odd; Shader ps; } partial{};
        partial.vs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {vsBytes, sizeof(vsBytes)}};
        partial.odd = {static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(99), 2};
        partial.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {psBytes, sizeof(psBytes)}};
        const D3D12_PIPELINE_STATE_STREAM_DESC partialDesc{sizeof(partial), &partial};
        check(simqvfr::replay_probe_stream_shaders(&partialDesc, vs, ps) && vs.pShaderBytecode == vsBytes && !ps.pShaderBytecode,
              "pipeline stream: an unknown subobject type ends the walk (nothing after it guessed)");
    }
    simqvfr::replay_probe_set_thresholds(4, 256.0f);
    check(simqvfr::replay_probe_start(device.Get(), 1), "probe installs");

    // A root signature with one root CBV and 4 root constants. Draws are recorded without a pipeline (nothing validates
    // at record time without the debug layer); lists with draws are never executed, only the last, draw-free one is.
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {1, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, error;
    ComPtr<ID3D12RootSignature> rootSignature;
    check(SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) &&
              SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                    IID_PPV_ARGS(&rootSignature))),
          "root signature");

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();

    const auto record = [&](float width, int draws) {
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        list->SetGraphicsRootSignature(rootSignature.Get());
        const D3D12_VIEWPORT viewports[2] = {{0, 0, width, 128, 0, 1}, {width, 0, width, 128, 0, 1}};
        list->RSSetViewports(2, viewports);
        const D3D12_RECT scissors[2] = {{0, 0, 64, 64}, {64, 0, 128, 64}};
        list->RSSetScissorRects(2, scissors);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vb{0, 0, 0};
        list->IASetVertexBuffers(0, 1, &vb);
        list->IASetIndexBuffer(nullptr);
        list->OMSetStencilRef(1);
        for (int i = 0; i < draws; ++i) {
            const UINT constants[4] = {UINT(i), 1, 2, 3};
            list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
            list->SetGraphicsRoot32BitConstant(1, UINT(i), 3);
            list->SetGraphicsRootConstantBufferView(0, 0);
            if (i % 2)
                list->DrawInstanced(3, 1, 0, 0);
            else
                list->DrawIndexedInstanced(3, 2, 0, 0, 0);
        }
        return list->Close();
    };

    simqvfr::replay_probe_take_stats();
    check(SUCCEEDED(record(512.0f, 10)), "a scene-like list closes");
    simqvfr::ReplayProbeStats s = simqvfr::replay_probe_take_stats();
    check(s.listsSeen == 1 && s.listsReplayed == 1, "it is recorded and re-emitted");
    check(s.drawsReplayed == 10, "all 10 draws re-emitted");
    // calls: root sig, viewports, scissors, topology, VB, IB, stencil + 10 x (constants, constant, CBV, draw)
    check(s.callsReplayed == 7 + 40, "every recorded call re-emitted");
    check(s.replayMs >= 0 && s.hookMs > 0 && s.gameMs > 0, "timings are counted");

    check(SUCCEEDED(record(128.0f, 10)), "a narrow list closes");
    s = simqvfr::replay_probe_take_stats();
    check(s.listsSeen == 1 && s.listsReplayed == 0, "a narrow viewport does not qualify");

    check(SUCCEEDED(record(512.0f, 2)), "a short list closes");
    s = simqvfr::replay_probe_take_stats();
    check(s.listsSeen == 1 && s.listsReplayed == 0, "too few draws do not qualify");

    // The real list still works: it executes (state only, no draws) and the queue drains.
    check(SUCCEEDED(record(512.0f, 0)), "an empty list closes");
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence.Get(), 1);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, done);
    check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 && SUCCEEDED(device->GetDeviceRemovedReason()),
          "the game's list executes unharmed");
    CloseHandle(done);

    std::printf("%s\n", failures ? "replay probe test FAILED" : "replay probe test passed");
    return failures ? 1 : 0;
}

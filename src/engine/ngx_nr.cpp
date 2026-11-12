#include "ngx_nr.h"

#include <windows.h>
#include <d3d11_4.h>

#ifdef SIM_QVFR_NGX
#include <cctype>
#include <cstdio>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#include "../log.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr::ngxnr {

struct View {
    unsigned width = 0, height = 0;
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    ComPtr<ID3D12Resource> colour, depth, vectors, output;
    ComPtr<ID3D11Texture2D> colour11, depth11, vectors11, output11;
    // Its evaluations' GPU time: two timestamps a submission, read once that submission is through.
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> times;
    UINT64 timedAt = 0;  // the fence value of the submission whose timestamps `times` holds (0: none)
    double msSum = 0;
    unsigned msCount = 0;
};

namespace {

// NGX feature 18: the neural rendering runtime (NVSDK_NGX_Feature_Reserved18 in the SDK's public header).
constexpr NVSDK_NGX_Feature kNeuralRendering = static_cast<NVSDK_NGX_Feature>(18);
// The project id engine/ngx_dlss.cpp starts NGX with: ours, a custom engine's.
constexpr const char* kProjectId = "5b1c0f1e-6a57-4c1b-9d0e-51a0c7f3d2a4";

struct Allocator {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 busyUntil = 0;  // the fence value its last list is through at
};

struct State {
    bool started = false, tried = false;
    std::string failure;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12GraphicsCommandList> list;  // closed between uses
    std::vector<Allocator> allocators;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Fence> fence11;
    UINT64 value = 0;
    HANDLE event = nullptr;
    UINT64 timestampFrequency = 0;
} s;

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string line = message ? message : "";
    std::string lower = line;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("error") == std::string::npos && lower.find("warn") == std::string::npos && lower.find("fail") == std::string::npos)
        return;
    if (lower.find("failed to load ngxcore") != std::string::npos || lower.find("from cache") != std::string::npos)
        return;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    simqvfr::logf("ngx nr: %s", line.c_str());
}

bool fail(std::string& why, const char* what, HRESULT hr) {
    char text[200];
    std::snprintf(text, sizeof(text), "%s (0x%08x)", what, static_cast<unsigned>(hr));
    why = s.failure = text;
    return false;
}

// An allocator no submission still uses, its list ready to record.
bool begin_list() {
    const UINT64 done = s.fence->GetCompletedValue();
    Allocator* free = nullptr;
    for (Allocator& a : s.allocators)
        if (a.busyUntil <= done) {
            free = &a;
            break;
        }
    if (!free) {
        Allocator a;
        if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.allocator))))
            return false;
        s.allocators.push_back(a);
        free = &s.allocators.back();
    }
    if (FAILED(free->allocator->Reset()))
        return false;
    free->busyUntil = UINT64_MAX;  // until end_list
    if (!s.list)
        return SUCCEEDED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, free->allocator.Get(), nullptr, IID_PPV_ARGS(&s.list)));
    return SUCCEEDED(s.list->Reset(free->allocator.Get(), nullptr));
}

// The list submitted; the fence signalled with a new value after it (returned; 0: the submission failed).
UINT64 end_list() {
    const bool closed = SUCCEEDED(s.list->Close());
    UINT64 at = 0;
    if (closed) {
        ID3D12CommandList* lists[] = {s.list.Get()};
        s.queue->ExecuteCommandLists(1, lists);
        at = ++s.value;
        s.queue->Signal(s.fence.Get(), at);
    }
    for (Allocator& a : s.allocators)
        if (a.busyUntil == UINT64_MAX)
            a.busyUntil = at;
    return at;
}

bool wait_for(UINT64 value, DWORD ms) {
    if (s.fence->GetCompletedValue() >= value)
        return true;
    if (FAILED(s.fence->SetEventOnCompletion(value, s.event)))
        return false;
    return WaitForSingleObject(s.event, ms) == WAIT_OBJECT_0;
}

void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, 0, from, to};
    s.list->ResourceBarrier(1, &b);
}

// A texture both devices use: made here, opened on the game's device. `output`: the model writes it (D3D11 only reads
// it); else D3D11 draws it.
bool shared(unsigned w, unsigned h, DXGI_FORMAT format, bool output, ComPtr<ID3D12Resource>& r12, ComPtr<ID3D11Texture2D>& r11,
            HRESULT& hr) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1, d.SampleDesc.Count = 1, d.Format = format;
    // A render target either way: the game's device opens no shared texture that is only the model's to write
    // (E_INVALIDARG, tests/nr_views_check.cpp).
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
              (output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    const char* step = "made on Direct3D 12";
    hr = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r12));
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr)) {
        step = "given a shared handle";
        hr = s.device->CreateSharedHandle(r12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    }
    if (SUCCEEDED(hr)) {
        step = "opened on the game's device";
        hr = s.device11->OpenSharedResource1(handle, IID_PPV_ARGS(&r11));
    }
    if (handle)
        CloseHandle(handle);
    if (FAILED(hr))
        simqvfr::logf("ngx nr: a shared %ux%u texture (format %d, %s) couldn't be %s (0x%08x)", w, h, static_cast<int>(format),
                      output ? "the model's output" : "an input", step, static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}

// The last timed submission's timestamps into the view's sums, once it is through.
void collect_time(View* v) {
    if (!v->timedAt || !s.timestampFrequency || s.fence->GetCompletedValue() < v->timedAt)
        return;
    v->timedAt = 0;
    UINT64* stamps = nullptr;
    const D3D12_RANGE read{0, 2 * sizeof(UINT64)};
    if (FAILED(v->times->Map(0, &read, reinterpret_cast<void**>(&stamps))) || !stamps)
        return;
    if (stamps[1] > stamps[0]) {
        v->msSum += 1000.0 * static_cast<double>(stamps[1] - stamps[0]) / static_cast<double>(s.timestampFrequency);
        ++v->msCount;
    }
    const D3D12_RANGE none{0, 0};
    v->times->Unmap(0, &none);
}

}  // namespace

bool built_in() { return true; }

bool start(ID3D11Device* device, const wchar_t* dllFolder, const wchar_t* dataFolder, std::string& why) {
    if (s.tried) {
        why = s.failure;
        return s.started;
    }
    s.tried = true;
    if (!dllFolder || !*dllFolder) {
        why = s.failure = "no folder with nvngx_dlssnr.dll was chosen";
        return false;
    }
    if (GetFileAttributesW((std::wstring(dllFolder) + L"\\nvngx_dlssnr.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        why = s.failure = "nvngx_dlssnr.dll isn't in the folder chosen for it";
        return false;
    }
    HRESULT hr = device ? device->QueryInterface(IID_PPV_ARGS(&s.device11)) : E_POINTER;
    if (FAILED(hr))
        return fail(why, "the game's Direct3D 11 device has no shared fences (Windows 10 1703 or newer needed)", hr);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(hr = device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(hr = dxgi->GetAdapter(&adapter)))
        return fail(why, "the game's graphics adapter couldn't be found", hr);
    if (FAILED(hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device))))
        return fail(why, "no Direct3D 12 device on the game's graphics card", hr);
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HANDLE fenceHandle = nullptr;
    if (FAILED(hr = s.device->CreateCommandQueue(&q, IID_PPV_ARGS(&s.queue))) ||
        FAILED(hr = s.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.fence))) ||
        FAILED(hr = s.device->CreateSharedHandle(s.fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
        shutdown();
        s.tried = true;
        return fail(why, "the Direct3D 12 queue and its fence couldn't be made", hr);
    }
    hr = s.device11->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&s.fence11));
    CloseHandle(fenceHandle);
    if (FAILED(hr)) {
        shutdown();
        s.tried = true;
        return fail(why, "the game's device couldn't open the shared fence", hr);
    }
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.queue->GetTimestampFrequency(&s.timestampFrequency);
    CreateDirectoryW(dataFolder, nullptr);
    const wchar_t* paths[1] = {dllFolder};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    const NVSDK_NGX_Result r =
        NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataFolder, s.device.Get(), &info);
    if (NVSDK_NGX_FAILED(r)) {
        ComPtr<ID3D12Device> keep = s.device;  // not NGX's to shut down: it never started
        s.device.Reset();
        shutdown();
        s.tried = true;
        return fail(why, "NGX didn't start on Direct3D 12", static_cast<HRESULT>(r));
    }
    s.started = true;
    return true;
}

View* create(unsigned width, unsigned height, int preset, unsigned& result) {
    result = static_cast<unsigned>(NVSDK_NGX_Result_Fail);
    if (!s.started || !width || !height)
        return nullptr;
    auto* v = new View;
    v->width = width;
    v->height = height;
    HRESULT hr = S_OK;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 2 * sizeof(UINT64), bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1, bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = shared(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false, v->colour, v->colour11, hr) &&
              shared(width, height, DXGI_FORMAT_R32_FLOAT, false, v->depth, v->depth11, hr) &&
              shared(width, height, DXGI_FORMAT_R16G16_FLOAT, false, v->vectors, v->vectors11, hr) &&
              shared(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true, v->output, v->output11, hr) &&
              SUCCEEDED(hr = s.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&v->queries))) &&
              SUCCEEDED(hr = s.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                               nullptr, IID_PPV_ARGS(&v->times)));
    if (!ok) {
        result = static_cast<unsigned>(hr);
        release(v);
        return nullptr;
    }
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_AllocateParameters(&v->params);
    if (NVSDK_NGX_SUCCEED(r) && v->params) {
        NVSDK_NGX_Parameter* p = v->params;
        for (const char* k : {"Width", "OutWidth", "DLSSNR.Width"})
            p->Set(k, width);
        for (const char* k : {"Height", "OutHeight", "DLSSNR.Height"})
            p->Set(k, height);
        p->Set("DLSSNR.Enabled", 1u);
        p->Set("CreationNodeMask", 1u);
        p->Set("VisibilityNodeMask", 1u);
        p->Set("DLSSNR.Hint.Render.Preset", static_cast<unsigned>(preset < 0 ? 0 : preset));
        p->Set("DLSSNR.Intensity", 1.0f);
        p->Set("DLSSNR.LocalToneStrength", 1.0f);
        p->Set("DLSSNR.LocalStructureStrength", 1.0f);
        p->Set("DLSSNR.UseAutoMask", 0u);
        p->Set("DLSSNR.ControlMask", static_cast<void*>(nullptr));
        p->Set("DLSSNR.Style", 0u);
        r = NVSDK_NGX_Result_Fail;
        if (begin_list()) {
            r = NVSDK_NGX_D3D12_CreateFeature(s.list.Get(), kNeuralRendering, p, &v->handle);
            const UINT64 at = end_list();
            if (NVSDK_NGX_SUCCEED(r) && (!at || !wait_for(at, 30000)))
                r = NVSDK_NGX_Result_Fail;
        }
    }
    result = static_cast<unsigned>(r);
    if (NVSDK_NGX_FAILED(r) || !v->handle) {
        release(v);
        return nullptr;
    }
    NVSDK_NGX_Parameter* p = v->params;
    p->Set("DLSSNR.Color", v->colour.Get());
    p->Set("DLSSNR.Output", v->output.Get());
    p->Set("DLSSNR.Depth", v->depth.Get());
    p->Set("DLSSNR.MVec", v->vectors.Get());
    for (const char* k : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
                          "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY", "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY"})
        p->Set(k, 0u);
    for (const char* k : {"DLSSNR.ColorSubrectWidth", "DLSSNR.OutputSubrectWidth", "DLSSNR.DepthSubrectWidth", "DLSSNR.MVecSubrectWidth"})
        p->Set(k, width);
    for (const char* k :
         {"DLSSNR.ColorSubrectHeight", "DLSSNR.OutputSubrectHeight", "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectHeight"})
        p->Set(k, height);
    // The vectors are fractions of the view: times its size, the texels the runtime works in.
    p->Set("DLSSNR.MVecScaleX", static_cast<float>(width));
    p->Set("DLSSNR.MVecScaleY", static_cast<float>(height));
    return v;
}

void release(View* v) {
    if (!v)
        return;
    if (s.queue && s.fence && s.value)  // nothing of it still in flight
        wait_for(s.value, 5000);
    if (v->handle)
        NVSDK_NGX_D3D12_ReleaseFeature(v->handle);
    if (v->params)
        NVSDK_NGX_D3D12_DestroyParameters(v->params);
    delete v;
}

Textures textures(View* v) {
    Textures t;
    if (v) {
        t.colour = v->colour11.Get();
        t.depth = v->depth11.Get();
        t.vectors = v->vectors11.Get();
        t.output = v->output11.Get();
    }
    return t;
}

bool evaluate(ID3D11DeviceContext* context, View* v, const Evaluation& e, unsigned& result) {
    result = static_cast<unsigned>(NVSDK_NGX_Result_Fail);
    if (!s.started || !context || !v || !v->handle)
        return false;
    ComPtr<ID3D11DeviceContext4> context4;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&context4));
    if (FAILED(hr) || FAILED(hr = s.device->GetDeviceRemovedReason())) {
        result = static_cast<unsigned>(hr);
        return false;
    }
    collect_time(v);
    if (!begin_list())
        return false;
    // D3D11's draws into the inputs first: the queue waits for this value before the list below runs.
    const UINT64 inputs = ++s.value;
    context4->Signal(s.fence11.Get(), inputs);
    context->Flush();
    s.queue->Wait(s.fence.Get(), inputs);

    NVSDK_NGX_Parameter* p = v->params;
    p->Set("DLSSNR.Intensity", e.intensity);
    p->Set("DLSSNR.LocalToneStrength", e.localTone);
    p->Set("DLSSNR.LocalStructureStrength", e.localStructure);
    p->Set("DLSSNR.Style", e.style);
    p->Set("DLSSNR.UseAutoMask", e.autoMask ? 1u : 0u);
    p->Set("DLSSNR.DepthInverted", e.depthInverted ? 1u : 0u);
    p->Set("DLSSNR.Reset", e.reset ? 1u : 0u);
    ID3D12Resource* const inputsOf[] = {v->colour.Get(), v->depth.Get(), v->vectors.Get()};
    for (ID3D12Resource* r : inputsOf)
        barrier(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(v->output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool timed = !v->timedAt;  // the last timed one has been read
    if (timed)
        s.list->EndQuery(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_EvaluateFeature(s.list.Get(), v->handle, p, nullptr);
    if (timed) {
        s.list->EndQuery(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        s.list->ResolveQueryData(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, v->times.Get(), 0);
    }
    for (ID3D12Resource* r12 : inputsOf)
        barrier(r12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(v->output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    // Submitted whatever NGX answered: the queue's wait above must be followed by a signal D3D11 can wait for.
    const UINT64 done = end_list();
    if (!done)
        return false;
    if (timed)
        v->timedAt = done;
    context4->Wait(s.fence11.Get(), done);
    result = static_cast<unsigned>(r);
    return NVSDK_NGX_SUCCEED(r);
}

double take_gpu_ms(View* v) {
    if (!v)
        return 0;
    collect_time(v);
    const double ms = v->msCount ? v->msSum / v->msCount : 0;
    v->msSum = 0;
    v->msCount = 0;
    return ms;
}

void shutdown() {
    if (s.queue && s.fence && s.event) {
        const UINT64 at = ++s.value;
        if (SUCCEEDED(s.queue->Signal(s.fence.Get(), at)))
            wait_for(at, 5000);
    }
    if (s.started && s.device)
        NVSDK_NGX_D3D12_Shutdown1(s.device.Get());
    if (s.event)
        CloseHandle(s.event);
    s = State{};
}

}  // namespace simqvfr::ngxnr

#else  // no NGX SDK in this build

namespace simqvfr::ngxnr {

struct View {};

bool built_in() { return false; }
bool start(ID3D11Device*, const wchar_t*, const wchar_t*, std::string& why) {
    why = "this build has no NGX SDK (CMake: NGX_SDK_DIR)";
    return false;
}
View* create(unsigned, unsigned, int, unsigned& result) {
    result = 0;
    return nullptr;
}
void release(View*) {}
Textures textures(View*) { return {}; }
bool evaluate(ID3D11DeviceContext*, View*, const Evaluation&, unsigned& result) {
    result = 0;
    return false;
}
double take_gpu_ms(View*) { return 0; }
void shutdown() {}

}  // namespace simqvfr::ngxnr

#endif

namespace simqvfr::ngxnr {

std::wstring loaded_dll() {
    const HMODULE dll = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (!dll)
        return {};
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(dll, path, MAX_PATH);
    return n == 0 || n == MAX_PATH ? std::wstring() : std::wstring(path, n);
}

}  // namespace simqvfr::ngxnr

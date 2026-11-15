// AMD FSR upscaling on our own D3D12 device for a D3D11 game (fsr_upscale.h). The bridge is DLSS-NR's
// (engine/ngx_nr.cpp): shared textures, shared fences, releases deferred until both devices are through.
#include "fsr_upscale.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <vector>

#include "../log.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr::fsr {

unsigned adapter_vendor(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc)))
        return 0;
    return desc.VendorId;
}

}  // namespace simqvfr::fsr

#ifdef SIM_QVFR_FFX
#include <d3d12.h>
#include <dxgi1_4.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

namespace simqvfr::fsr {

struct View {
    unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;
    ffxContext context = nullptr;
    ComPtr<ID3D12Resource> colour, depth, vectors, output;
    ComPtr<ID3D11Texture2D> colour11, depth11, vectors11, output11;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> times;
    UINT64 timedAt = 0;
    UINT64 lastSubmit = 0;
    unsigned long long releasedAt = 0;
    UINT64 releaseValue = 0;
    std::vector<ComPtr<IUnknown>> keep;
    double msSum = 0;
    unsigned msCount = 0;
};

namespace {

struct Allocator {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 busyUntil = 0;
};

struct State {
    bool started = false, tried = false;
    std::string failure;
    HMODULE loader = nullptr, upscaler = nullptr;
    std::wstring folder;
    ffxFunctions ffx{};
    Provider provider = Provider::Newest;
    uint64_t overrideId = 0;  // the provider asked for (0: the runtime's own choice, the newest)
    std::string providers, inUse;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::vector<Allocator> allocators;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Fence> fence11;
    UINT64 value = 0;
    ComPtr<ID3D12Fence> releaseFence;
    ComPtr<ID3D11Fence> releaseFence11;
    UINT64 releaseValue = 0;
    HANDLE event = nullptr;
    UINT64 timestampFrequency = 0;
    std::vector<View*> released;
    unsigned long long collects = 0;
    std::deque<UINT64> outstanding;
} s;

constexpr unsigned long long kReleaseFrames = 8;

std::atomic<int> messagesLogged{0};

// The runtime's own warnings and errors, the first few dozen a session.
void on_message(uint32_t type, const wchar_t* message) {
    if (messagesLogged.fetch_add(1) >= 40)
        return;
    std::wstring line = message ? message : L"";
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r'))
        line.pop_back();
    simqvfr::logf("fsr: the runtime says (%s): %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", line.c_str());
}

bool fail(std::string& why, const char* what, HRESULT hr) {
    char text[240];
    std::snprintf(text, sizeof(text), "%s (0x%08x)", what, static_cast<unsigned>(hr));
    why = s.failure = text;
    return false;
}

bool wait_for(UINT64 value, DWORD ms) {
    if (s.fence->GetCompletedValue() >= value)
        return true;
    if (FAILED(s.fence->SetEventOnCompletion(value, s.event)))
        return false;
    return WaitForSingleObject(s.event, ms) == WAIT_OBJECT_0;
}

void destroy(View* v) {
    if (v->context && s.ffx.DestroyContext)
        s.ffx.DestroyContext(&v->context, nullptr);
    delete v;
}

void discard(View* v) {
    if (s.queue && s.fence && v->lastSubmit && !wait_for(v->lastSubmit, 30000)) {
        s.released.push_back(v);
        return;
    }
    destroy(v);
}

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
    free->busyUntil = UINT64_MAX;
    if (!s.list)
        return SUCCEEDED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, free->allocator.Get(), nullptr, IID_PPV_ARGS(&s.list)));
    return SUCCEEDED(s.list->Reset(free->allocator.Get(), nullptr));
}

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

void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, 0, from, to};
    s.list->ResourceBarrier(1, &b);
}

// A typed format for a shared texture of the scene's (a typeless scene: its float or unorm reading).
DXGI_FORMAT typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

// A texture both devices use: made here, opened on the game's device. A render target either way (the game's device
// opens no shared texture that isn't: tests/nr_views_check.cpp); the output also an unordered-access target for FSR.
bool shared(unsigned w, unsigned h, DXGI_FORMAT format, bool output, ComPtr<ID3D12Resource>& r12, ComPtr<ID3D11Texture2D>& r11,
            HRESULT& hr) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1, d.SampleDesc.Count = 1, d.Format = format;
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
        simqvfr::logf("fsr: a shared %ux%u texture (format %d, %s) couldn't be %s (0x%08x)", w, h, static_cast<int>(format),
                      output ? "FSR's output" : "an input", step, static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}

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

// The folder this module (the layer, or a test program) was loaded from.
std::wstring own_folder() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&own_folder), &self))
        return {};
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return {};
    return std::filesystem::path(std::wstring(path, n)).parent_path().wstring();
}

std::wstring exe_folder() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return {};
    return std::filesystem::path(std::wstring(path, n)).parent_path().wstring();
}

constexpr const wchar_t* kUpscalerDll = L"amd_fidelityfx_upscaler_dx12.dll";
constexpr const wchar_t* kLoaderDll = L"amd_fidelityfx_loader_dx12.dll";

bool has_both(const std::wstring& folder) {
    return !folder.empty() &&
           GetFileAttributesW((folder + L"\\" + kUpscalerDll).c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW((folder + L"\\" + kLoaderDll).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// AMD's two DLLs from the first folder holding both: the upscaler first and by its full path, so the loader finds it
// already loaded and takes no other copy (its lookup: a loaded module, the exe's folder, its own folder).
bool load_runtime(const wchar_t* dllFolder, std::string& why) {
    std::vector<std::wstring> folders;
    if (dllFolder && *dllFolder)
        folders.emplace_back(dllFolder);
    folders.push_back(own_folder());
    folders.push_back(exe_folder());
    for (const std::wstring& f : folders) {
        if (!has_both(f))
            continue;
        const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
        s.upscaler = LoadLibraryExW((f + L"\\" + kUpscalerDll).c_str(), nullptr, flags);
        s.loader = s.upscaler ? LoadLibraryExW((f + L"\\" + kLoaderDll).c_str(), nullptr, flags) : nullptr;
        if (!s.loader) {
            const DWORD error = GetLastError();
            if (s.upscaler)
                FreeLibrary(s.upscaler);
            s.upscaler = nullptr;
            char text[200];
            std::snprintf(text, sizeof(text), "AMD's FSR DLLs in the folder found couldn't be loaded (error %lu)", error);
            why = s.failure = text;
            return false;
        }
        ffxLoadFunctions(&s.ffx, s.loader);
        if (!s.ffx.CreateContext || !s.ffx.DestroyContext || !s.ffx.Query || !s.ffx.Dispatch) {
            why = s.failure = "amd_fidelityfx_loader_dx12.dll lacks the FSR API's functions";
            return false;
        }
        s.folder = f;
        return true;
    }
    why = s.failure = dllFolder && *dllFolder
                          ? "amd_fidelityfx_upscaler_dx12.dll and amd_fidelityfx_loader_dx12.dll aren't both in the folder "
                            "chosen for them, beside Sim-QVFR or beside the game"
                          : "amd_fidelityfx_upscaler_dx12.dll and amd_fidelityfx_loader_dx12.dll aren't both beside Sim-QVFR "
                            "or beside the game";
    return false;
}

// "FSR 3.1.5" -> 3, 1, 5 (what a name holds of a version; missing parts 0).
void version_of(const char* name, int (&v)[3]) {
    v[0] = v[1] = v[2] = 0;
    if (!name)
        return;
    const char* p = name;
    while (*p && (*p < '0' || *p > '9'))
        ++p;
    for (int i = 0; i < 3 && *p; ++i) {
        v[i] = std::atoi(p);
        while (*p >= '0' && *p <= '9')
            ++p;
        if (*p != '.')
            break;
        ++p;
    }
}

// The runtime's providers on this device, logged by name; the one Provider asks for chosen (0: no override).
void choose_provider() {
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = s.device.Get();
    q.outputCount = &count;
    if (s.ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0 || count > 64) {
        s.providers = "(the runtime lists none)";
        return;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (s.ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK) {
        s.providers = "(the runtime's list couldn't be read)";
        return;
    }
    s.providers.clear();
    int best[3] = {-1, 0, 0};
    for (uint64_t i = 0; i < count; ++i) {
        const char* name = names[i] ? names[i] : "?";
        if (!s.providers.empty())
            s.providers += ", ";
        s.providers += name;
        int v[3];
        version_of(name, v);
        if (s.provider == Provider::Fsr3 && v[0] == 3 &&
            (v[0] > best[0] || (v[0] == best[0] && (v[1] > best[1] || (v[1] == best[1] && v[2] > best[2]))))) {
            best[0] = v[0], best[1] = v[1], best[2] = v[2];
            s.overrideId = ids[i];
        }
    }
    if (s.provider == Provider::Fsr3 && !s.overrideId)
        simqvfr::logf("fsr: FSR 3.x asked for, but the runtime offers none on this GPU (%s): its newest instead",
                      s.providers.c_str());
}

}  // namespace

bool built_in() { return true; }

bool start(ID3D11Device* device, const wchar_t* dllFolder, Provider provider, std::string& why) {
    if (s.tried) {
        why = s.failure;
        return s.started;
    }
    s.tried = true;
    s.provider = provider;
    HRESULT hr = device ? device->QueryInterface(IID_PPV_ARGS(&s.device11)) : E_POINTER;
    if (FAILED(hr))
        return fail(why, "the game's Direct3D 11 device has no shared fences (Windows 10 1703 or newer needed)", hr);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(hr = device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(hr = dxgi->GetAdapter(&adapter)))
        return fail(why, "the game's graphics adapter couldn't be found", hr);
    if (!s.upscaler && !load_runtime(dllFolder, why))
        return false;
    if (FAILED(hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device))))
        return fail(why, "no Direct3D 12 device on the game's graphics card", hr);
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HANDLE fenceHandle = nullptr;
    if (FAILED(hr = s.device->CreateCommandQueue(&q, IID_PPV_ARGS(&s.queue))) ||
        FAILED(hr = s.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.fence))) ||
        FAILED(hr = s.device->CreateSharedHandle(s.fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
        shutdown(nullptr);
        s.tried = true;
        return fail(why, "the Direct3D 12 queue and its fence couldn't be made", hr);
    }
    hr = s.device11->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&s.fence11));
    CloseHandle(fenceHandle);
    fenceHandle = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = s.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.releaseFence))) &&
        SUCCEEDED(hr = s.device->CreateSharedHandle(s.releaseFence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
        hr = s.device11->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&s.releaseFence11));
        CloseHandle(fenceHandle);
    }
    if (FAILED(hr)) {
        shutdown(nullptr);
        s.tried = true;
        return fail(why, "the game's device couldn't open the shared fence", hr);
    }
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.queue->GetTimestampFrequency(&s.timestampFrequency);
    choose_provider();
    s.started = true;
    return true;
}

std::string providers() { return s.providers; }
std::string provider_in_use() { return s.inUse; }
std::wstring loaded_folder() { return s.upscaler ? s.folder : std::wstring(); }

View* create(unsigned renderWidth, unsigned renderHeight, unsigned outputWidth, unsigned outputHeight, DXGI_FORMAT format,
             bool depthInverted, bool depthInfinite, unsigned& result) {
    result = FFX_API_RETURN_ERROR;
    if (!s.started || !renderWidth || !renderHeight || !outputWidth || !outputHeight)
        return nullptr;
    auto* v = new View;
    v->width = renderWidth, v->height = renderHeight, v->outWidth = outputWidth, v->outHeight = outputHeight;
    const DXGI_FORMAT colour = typed(format);
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
    const bool ok = shared(renderWidth, renderHeight, colour, false, v->colour, v->colour11, hr) &&
                    shared(renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT, false, v->depth, v->depth11, hr) &&
                    shared(renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT, false, v->vectors, v->vectors11, hr) &&
                    shared(outputWidth, outputHeight, colour, true, v->output, v->output11, hr) &&
                    SUCCEEDED(hr = s.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&v->queries))) &&
                    SUCCEEDED(hr = s.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bd,
                                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&v->times)));
    if (!ok) {
        result = static_cast<unsigned>(hr);
        discard(v);
        return nullptr;
    }
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // HDR linear colour (the scene before the game's tone mapping), FSR's own exposure, vectors at the render size.
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                 (depthInverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) | (depthInfinite ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0u);
    desc.maxRenderSize = {renderWidth, renderHeight};
    desc.maxUpscaleSize = {outputWidth, outputHeight};
    desc.fpMessage = &on_message;
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s.device.Get();
    ffxOverrideVersion over{};
    over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    over.versionId = s.overrideId;
    desc.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    backend.header.pNext = s.overrideId ? &over.header : nullptr;
    ffxReturnCode_t r = s.ffx.CreateContext(&v->context, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK) {  // an older runtime than these headers: without the version this build was made with
        v->context = nullptr;
        desc.header.pNext = &backend.header;
        const ffxReturnCode_t again = s.ffx.CreateContext(&v->context, &desc.header, nullptr);
        if (again == FFX_API_RETURN_OK)
            simqvfr::logf("fsr: the runtime refused this build's FSR API version (%u.%u.%u, result %u): made without it",
                          FFX_UPSCALER_VERSION_MAJOR, FFX_UPSCALER_VERSION_MINOR, FFX_UPSCALER_VERSION_PATCH, r);
        r = again;
    }
    result = r;
    if (r != FFX_API_RETURN_OK || !v->context) {
        v->context = nullptr;
        discard(v);
        return nullptr;
    }
    if (s.inUse.empty()) {
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (s.ffx.Query(&v->context, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
            s.inUse = pv.versionName;
        else
            s.inUse = "(not named)";
    }
    return v;
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
    result = FFX_API_RETURN_ERROR;
    if (!s.started || !context || !v || !v->context)
        return false;
    ComPtr<ID3D11DeviceContext4> context4;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&context4));
    if (FAILED(hr) || FAILED(hr = s.device->GetDeviceRemovedReason())) {
        result = static_cast<unsigned>(hr);
        return false;
    }
    collect_time(v);
    const UINT64 reached = s.fence->GetCompletedValue();
    while (!s.outstanding.empty() && s.outstanding.front() <= reached)
        s.outstanding.pop_front();
    if (s.outstanding.size() >= kBacklog) {
        result = kBehind;
        return false;
    }
    if (!begin_list())
        return false;
    // D3D11's copies and draws into the inputs first: the queue waits for this value before the list below runs.
    const UINT64 inputs = ++s.value;
    context4->Signal(s.fence11.Get(), inputs);
    context->Flush();
    s.queue->Wait(s.fence.Get(), inputs);

    ID3D12Resource* const inputsOf[] = {v->colour.Get(), v->depth.Get(), v->vectors.Get()};
    for (ID3D12Resource* r : inputsOf)
        barrier(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(v->output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool timed = !v->timedAt;
    if (timed)
        s.list->EndQuery(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = s.list.Get();
    d.color = ffxApiGetResourceDX12(v->colour.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(v->depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(v->vectors.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(v->output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // Vectors from this frame's (unjittered) pixel to where it was, in render pixels: FSR's own convention.
    d.motionVectorScale = {1.0f, 1.0f};
    d.jitterOffset = {e.jitterX, e.jitterY};
    d.renderSize = {e.width ? e.width : v->width, e.height ? e.height : v->height};
    d.upscaleSize = {v->outWidth, v->outHeight};
    d.enableSharpening = false;  // Sim-QVFR's own CAS sharpens where the player asks
    d.sharpness = 0;
    d.frameTimeDelta = e.frameMs;
    d.preExposure = 1.0f;
    d.reset = e.reset;
    d.cameraNear = e.cameraNear;
    d.cameraFar = e.cameraFar;
    d.cameraFovAngleVertical = e.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    const ffxReturnCode_t r = s.ffx.Dispatch(&v->context, &d.header);
    if (timed) {
        s.list->EndQuery(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        s.list->ResolveQueryData(v->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, v->times.Get(), 0);
    }
    for (ID3D12Resource* r12 : inputsOf)
        barrier(r12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(v->output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    // Submitted whatever FSR answered: the queue's wait above must be followed by a signal D3D11 can wait for.
    const UINT64 done = end_list();
    if (!done)
        return false;
    if (timed)
        v->timedAt = done;
    v->lastSubmit = done;
    s.outstanding.push_back(done);
    context4->Wait(s.fence11.Get(), done);
    result = r;
    return r == FFX_API_RETURN_OK;
}

void release(ID3D11DeviceContext* context, View* v, IUnknown* const* keep, unsigned count) {
    if (!v)
        return;
    for (unsigned i = 0; keep && i < count; ++i)
        if (keep[i])
            v->keep.emplace_back(keep[i]);
    ComPtr<ID3D11DeviceContext4> context4;
    if (context && s.releaseFence11 && !device_lost() && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
        v->releaseValue = ++s.releaseValue;
        context4->Signal(s.releaseFence11.Get(), v->releaseValue);
    } else {
        v->releaseValue = UINT64_MAX;
    }
    v->releasedAt = s.collects;
    s.released.push_back(v);
}

void collect() {
    ++s.collects;
    if (s.released.empty() || !s.fence || !s.releaseFence)
        return;
    const UINT64 done = s.fence->GetCompletedValue(), let = s.releaseFence->GetCompletedValue();
    if (done == UINT64_MAX || let == UINT64_MAX)
        return;
    for (auto it = s.released.begin(); it != s.released.end();) {
        View* v = *it;
        if (done >= v->lastSubmit && let >= v->releaseValue && s.collects >= v->releasedAt + kReleaseFrames) {
            destroy(v);
            it = s.released.erase(it);
        } else {
            ++it;
        }
    }
}

unsigned pending_releases() { return static_cast<unsigned>(s.released.size()); }

bool device_lost() { return s.device && FAILED(s.device->GetDeviceRemovedReason()); }

double take_gpu_ms(View* v) {
    if (!v)
        return 0;
    collect_time(v);
    const double ms = v->msCount ? v->msSum / v->msCount : 0;
    v->msSum = 0;
    v->msCount = 0;
    return ms;
}

void shutdown(ID3D11DeviceContext* context) {
    const bool lost = device_lost();
    bool idle = true;
    if (!lost && s.queue && s.fence && s.event) {
        ComPtr<ID3D11DeviceContext4> context4;
        UINT64 let = 0;
        if (context && s.releaseFence11 && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            let = ++s.releaseValue;
            context4->Signal(s.releaseFence11.Get(), let);
            context->Flush();
        } else if (!s.released.empty()) {
            idle = false;
        }
        const UINT64 at = ++s.value;
        idle = idle && SUCCEEDED(s.queue->Signal(s.fence.Get(), at)) && wait_for(at, 3000);
        if (idle && let) {
            idle = SUCCEEDED(s.releaseFence->SetEventOnCompletion(let, s.event)) && WaitForSingleObject(s.event, 3000) == WAIT_OBJECT_0;
            idle = idle && s.releaseFence->GetCompletedValue() != UINT64_MAX;
        }
        idle = idle && s.fence->GetCompletedValue() != UINT64_MAX;
    }
    if (lost)
        simqvfr::logf("fsr: its Direct3D 12 device is lost: everything of it released (%u released views among it)",
                      static_cast<unsigned>(s.released.size()));
    const HMODULE loader = s.loader, upscaler = s.upscaler;
    const ffxFunctions ffx = s.ffx;
    const std::wstring folder = s.folder;
    if (!idle) {
        simqvfr::logf("fsr: the GPU didn't finish FSR's work within 3 s: its Direct3D 12 objects (%u released views among them) "
                      "are left as they are, not destroyed",
                      static_cast<unsigned>(s.released.size()));
        (void)s.device.Detach();
        (void)s.device11.Detach();
        (void)s.queue.Detach();
        (void)s.list.Detach();
        (void)s.fence.Detach();
        (void)s.fence11.Detach();
        (void)s.releaseFence.Detach();
        (void)s.releaseFence11.Detach();
        for (Allocator& a : s.allocators)
            (void)a.allocator.Detach();
        for (View* v : s.released)
            for (ComPtr<IUnknown>& k : v->keep)
                (void)k.Detach();
    } else {
        for (View* v : s.released)
            destroy(v);
        s.released.clear();
        if (s.event)
            CloseHandle(s.event);
    }
    s = State{};
    // AMD's DLLs stay loaded for the process (a later session takes them again; unloading a runtime that may keep
    // threads or callbacks is not ours to risk).
    s.loader = loader;
    s.upscaler = upscaler;
    s.ffx = ffx;
    s.folder = folder;
}

}  // namespace simqvfr::fsr

#else  // no FSR SDK headers in this build

namespace simqvfr::fsr {

struct View {};

bool built_in() { return false; }
bool start(ID3D11Device*, const wchar_t*, Provider, std::string& why) {
    why = "this build has no FSR SDK (CMake: FFX_SDK_DIR)";
    return false;
}
std::string providers() { return {}; }
std::string provider_in_use() { return {}; }
std::wstring loaded_folder() { return {}; }
View* create(unsigned, unsigned, unsigned, unsigned, DXGI_FORMAT, bool, bool, unsigned& result) {
    result = 0;
    return nullptr;
}
Textures textures(View*) { return {}; }
bool evaluate(ID3D11DeviceContext*, View*, const Evaluation&, unsigned& result) {
    result = 0;
    return false;
}
void release(ID3D11DeviceContext*, View*, IUnknown* const*, unsigned) {}
void collect() {}
unsigned pending_releases() { return 0; }
bool device_lost() { return false; }
double take_gpu_ms(View*) { return 0; }
void shutdown(ID3D11DeviceContext*) {}

}  // namespace simqvfr::fsr

#endif

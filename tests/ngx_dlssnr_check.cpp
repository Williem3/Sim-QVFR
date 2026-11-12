// Hardware check of NVIDIA's DLSS neural rendering runtime (nvngx_dlssnr.dll, "DLSS-NR") called by us on D3D12, outside
// any game: does it start for a process that is no NVIDIA-registered title, by which route, which of its settings
// change the picture, and what does a view cost? Not part of the test suite (no _test suffix): it needs an NVIDIA RTX
// GPU, the NGX SDK (CMake: NGX_SDK_DIR) and a folder holding nvngx_dlssnr.dll, which no SDK of NVIDIA's carries (a game
// that ships DLSS-NR has it); it is never ours to distribute.
//
//   sim_qvfr_tests ngx_dlssnr_check <folder with nvngx_dlssnr.dll> [sdk|direct] [application id, hex]
//
// Routes:
//   sdk     NGX's documented start with our own project id (as engine/ngx_dlss.cpp does for DLSS), the folder given as
//           a feature path, feature 18 made through the SDK's library.
//   direct  the DLL's own NVSDK_NGX_D3D12_* exports (its Init_Ext takes an application id: the third argument, 0
//           unless given).
// The parameter names are the runtime's own ("DLSSNR.*"); NVIDIA documents none of them publicly.
// Set NGX_LOG=1 for NGX's own log lines.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    failures += ok ? 0 : 1;
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::printf("  ngx: %s", message);
}

constexpr NVSDK_NGX_Feature kNeuralRendering = static_cast<NVSDK_NGX_Feature>(18);
constexpr const char* kProjectId = "5b1c0f1e-6a57-4c1b-9d0e-51a0c7f3d2a4";  // engine/ngx_dlss.cpp's

using AllocateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn =
    NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using InitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);

struct Runtime {
    AllocateFn allocate = nullptr;
    DestroyFn destroy = nullptr;
    CreateFn create = nullptr;
    EvaluateFn evaluate = nullptr;
    ReleaseFn release = nullptr;
};

NVSDK_NGX_Result NVSDK_CONV sdk_create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature f, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** h) {
    return NVSDK_NGX_D3D12_CreateFeature(list, f, p, h);
}
NVSDK_NGX_Result NVSDK_CONV sdk_evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p,
                                         PFN_NVSDK_NGX_ProgressCallback cb) {
    return NVSDK_NGX_D3D12_EvaluateFeature(list, h, p, cb);
}

ComPtr<ID3D12Device> device;
ComPtr<IDXGIAdapter3> adapter3;
// This process's video memory in use on the GPU, MB; what the last run() took at its most (its textures and feature).
double vram_mb() {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!adapter3 || FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return 0;
    return static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
}
double lastVramMb = 0;
ComPtr<ID3D12CommandQueue> queue;
ComPtr<ID3D12CommandAllocator> allocator;
ComPtr<ID3D12GraphicsCommandList> list;
ComPtr<ID3D12Fence> fence;
UINT64 fenceValue = 0;
HANDLE fenceEvent = nullptr;

bool flush() {
    if (FAILED(list->Close()))
        return false;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++fenceValue);
    fence->SetEventOnCompletion(fenceValue, fenceEvent);
    const bool done = WaitForSingleObject(fenceEvent, 60000) == WAIT_OBJECT_0;
    allocator->Reset();
    list->Reset(allocator.Get(), nullptr);
    return done && SUCCEEDED(device->GetDeviceRemovedReason());
}

void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, 0, from, to};
    list->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes, d.Height = 1, d.DepthOrArraySize = 1, d.MipLevels = 1, d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d,
                                    type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&r));
    return r;
}

ComPtr<ID3D12Resource> texture(UINT w, UINT h, DXGI_FORMAT format, bool output) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1, d.SampleDesc.Count = 1, d.Format = format;
    d.Flags = output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                    output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                    IID_PPV_ARGS(&r));
    return r;
}

// A scene of sorts, linear HDR: a sky gradient over a ground with a checker of detail, a bright disc, a skin-toned
// ellipse. `out`: the RGB as stored (halves read back), w*h*3.
void fill_colour(uint16_t* row, UINT y, UINT w, UINT h, std::vector<float>* out) {
    using DirectX::PackedVector::XMConvertFloatToHalf;
    using DirectX::PackedVector::XMConvertHalfToFloat;
    for (UINT x = 0; x < w; ++x) {
        const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
        float rgb[3];
        if (v < 0.45f) {
            rgb[0] = 0.25f + 0.3f * v, rgb[1] = 0.45f + 0.3f * v, rgb[2] = 0.9f - 0.4f * v;
        } else {
            const float detail = ((x / 9 + y / 9) % 2) * 0.06f;
            rgb[0] = 0.18f + detail + 0.1f * u, rgb[1] = 0.2f + detail, rgb[2] = 0.17f + detail;
        }
        const float sx = (u - 0.75f) / 0.05f, sy = (v - 0.15f) / 0.05f;
        if (sx * sx + sy * sy < 1)
            rgb[0] = 6.0f, rgb[1] = 5.5f, rgb[2] = 4.5f;
        const float dx = (u - 0.4f) / 0.12f, dy = (v - 0.6f) / 0.2f;
        if (dx * dx + dy * dy < 1)
            rgb[0] = 0.63f + 0.1f * dx, rgb[1] = 0.4f + 0.08f * dx, rgb[2] = 0.28f + 0.05f * dx;
        for (int c = 0; c < 3; ++c) {
            row[x * 4 + c] = XMConvertFloatToHalf(rgb[c]);
            if (out)
                (*out)[(size_t(y) * w + x) * 3 + c] = XMConvertHalfToFloat(row[x * 4 + c]);
        }
        row[x * 4 + 3] = XMConvertFloatToHalf(1.0f);
    }
}

// The texture filled (colour: the scene; depth: 0.5; vectors: zero) and left readable by compute shaders.
std::vector<ComPtr<ID3D12Resource>> uploads;
void upload(ID3D12Resource* r, std::vector<float>* colourOut) {
    const D3D12_RESOURCE_DESC desc = r->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    ComPtr<ID3D12Resource> up = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
    unsigned char* data = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&data));
    std::memset(data, 0, size_t(bytes));
    const UINT w = static_cast<UINT>(desc.Width), h = desc.Height;
    if (colourOut)
        colourOut->assign(size_t(w) * h * 3, 0.0f);
    for (UINT y = 0; y < h; ++y) {
        unsigned char* row = data + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            fill_colour(reinterpret_cast<uint16_t*>(row), y, w, h, colourOut);
        else if (desc.Format == DXGI_FORMAT_R32_FLOAT)
            for (UINT x = 0; x < w; ++x)
                reinterpret_cast<float*>(row)[x] = 0.5f;
    }
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = up.Get(), src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, src.PlacedFootprint = fp;
    dst.pResource = r, dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    uploads.push_back(up);
}

bool read_rgb(ID3D12Resource* output, std::vector<float>& rgb) {
    const D3D12_RESOURCE_DESC desc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    ComPtr<ID3D12Resource> back = buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = output, src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = back.Get(), dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, dst.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!flush())
        return false;
    unsigned char* data = nullptr;
    if (FAILED(back->Map(0, nullptr, reinterpret_cast<void**>(&data))))
        return false;
    const UINT w = static_cast<UINT>(desc.Width), h = desc.Height;
    rgb.resize(size_t(w) * h * 3);
    for (UINT y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<const uint16_t*>(data + fp.Offset + size_t(y) * fp.Footprint.RowPitch);
        for (UINT x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                rgb[(size_t(y) * w + x) * 3 + c] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + c]);
    }
    back->Unmap(0, nullptr);
    return true;
}

struct Tuning {
    const char* name;
    float intensity = 1, tone = 1, structure = 1;
    unsigned autoMask = 0, style = 0, preset = 0;
    unsigned flags = 0;  // "DLSS.Feature.Create.Flags" (NVSDK_NGX_DLSS_Feature_Flags: 1 IsHDR)
    // Set after the feature is made, before its evaluations (< 0: not): do the settings follow without a new feature?
    float lateIntensity = -1, lateTone = -1, lateStructure = -1;
    int lateStyle = -1;
};

struct Diff {
    double mean = 0;
    float max = 0;
    unsigned nonFinite = 0;
};

// Over the region (x0, y0, w, h) of two images `stride` wide.
Diff diff(const std::vector<float>& a, const std::vector<float>& b, UINT stride, UINT x0, UINT y0, UINT w, UINT h) {
    Diff d;
    for (UINT y = y0; y < y0 + h; ++y)
        for (UINT x = x0; x < x0 + w; ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = (size_t(y) * stride + x) * 3 + c;
                if (!std::isfinite(a[i]) || !std::isfinite(b[i])) {
                    ++d.nonFinite;
                    continue;
                }
                const float e = std::fabs(a[i] - b[i]);
                d.mean += e;
                d.max = std::max(d.max, e);
            }
    d.mean /= double(w) * h * 3;
    return d;
}

// One feature w x h, working on the region at (x0, y0) of textures tw x th. `out`: the output read back after `frames`
// evaluations; `ms`: the median wall time of an evaluation, submitted and waited for (an idle GPU: a floor).
bool run(const Runtime& rt, const Tuning& t, UINT w, UINT h, UINT tw, UINT th, UINT x0, UINT y0, int frames, std::vector<float>* in,
         std::vector<float>* out, double* ms, double* createMs) {
    const double vramBefore = vram_mb();
    ComPtr<ID3D12Resource> colour = texture(tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    ComPtr<ID3D12Resource> output = texture(tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    ComPtr<ID3D12Resource> vectors = texture(tw, th, DXGI_FORMAT_R16G16_FLOAT, false);
    ComPtr<ID3D12Resource> depth = texture(tw, th, DXGI_FORMAT_R32_FLOAT, false);
    if (!colour || !output || !vectors || !depth)
        return false;
    upload(colour.Get(), in);
    upload(vectors.Get(), nullptr);
    upload(depth.Get(), nullptr);
    if (!flush())
        return false;
    uploads.clear();

    NVSDK_NGX_Parameter* p = nullptr;
    if (NVSDK_NGX_FAILED(rt.allocate(&p)) || !p)
        return false;
    for (const char* k : {"Width", "Height", "OutWidth", "OutHeight", "DLSSNR.Width", "DLSSNR.Height"})
        p->Set(k, std::strstr(k, "Height") ? h : w);
    p->Set("DLSSNR.Enabled", 1u);
    p->Set("CreationNodeMask", 1u);
    p->Set("VisibilityNodeMask", 1u);
    p->Set("DLSSNR.Hint.Render.Preset", t.preset);
    p->Set("DLSSNR.Intensity", t.intensity);
    p->Set("DLSSNR.LocalToneStrength", t.tone);
    p->Set("DLSSNR.LocalStructureStrength", t.structure);
    p->Set("DLSSNR.UseAutoMask", t.autoMask);
    p->Set("DLSSNR.ControlMask", static_cast<void*>(nullptr));
    p->Set("DLSSNR.Style", t.style);
    p->Set("DLSS.Feature.Create.Flags", t.flags);
    NVSDK_NGX_Handle* handle = nullptr;
    const auto c0 = std::chrono::steady_clock::now();
    const NVSDK_NGX_Result made = rt.create(list.Get(), kNeuralRendering, p, &handle);
    bool ok = NVSDK_NGX_SUCCEED(made) && handle && flush();
    if (createMs)
        *createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    if (!ok) {
        std::printf("  %s: feature 18 at %ux%u not made (0x%08x)\n", t.name, w, h, unsigned(made));
        rt.destroy(p);
        return false;
    }
    p->Set("DLSSNR.Color", colour.Get());
    p->Set("DLSSNR.Output", output.Get());
    p->Set("DLSSNR.Depth", depth.Get());
    p->Set("DLSSNR.MVec", vectors.Get());
    for (const char* resource : {"Color", "Output", "Depth", "MVec"}) {
        const std::string prefix = std::string("DLSSNR.") + resource + "Subrect";
        p->Set((prefix + "BaseX").c_str(), x0);
        p->Set((prefix + "BaseY").c_str(), y0);
        p->Set((prefix + "Width").c_str(), w);
        p->Set((prefix + "Height").c_str(), h);
    }
    p->Set("DLSSNR.MVecScaleX", float(w));
    p->Set("DLSSNR.MVecScaleY", float(h));
    p->Set("DLSSNR.DepthInverted", 0u);
    if (t.lateIntensity >= 0)
        p->Set("DLSSNR.Intensity", t.lateIntensity);
    if (t.lateTone >= 0)
        p->Set("DLSSNR.LocalToneStrength", t.lateTone);
    if (t.lateStructure >= 0)
        p->Set("DLSSNR.LocalStructureStrength", t.lateStructure);
    if (t.lateStyle >= 0)
        p->Set("DLSSNR.Style", static_cast<unsigned>(t.lateStyle));
    std::vector<double> times;
    for (int frame = 0; frame < frames && ok; ++frame) {
        p->Set("DLSSNR.Reset", frame == 0 ? 1u : 0u);
        const auto t0 = std::chrono::steady_clock::now();
        const NVSDK_NGX_Result r = rt.evaluate(list.Get(), handle, p, nullptr);
        ok = NVSDK_NGX_SUCCEED(r) && flush();
        if (!ok)
            std::printf("  %s: evaluation %d failed (0x%08x, device 0x%08x)\n", t.name, frame, unsigned(r),
                        unsigned(device->GetDeviceRemovedReason()));
        if (frame >= 2)
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    lastVramMb = vram_mb() - vramBefore;
    if (ok && ms && !times.empty()) {
        std::sort(times.begin(), times.end());
        *ms = times[times.size() / 2];
    }
    if (ok && out)
        ok = read_rgb(output.Get(), *out);
    rt.release(handle);
    rt.destroy(p);
    return ok;
}

bool nvidia_device() {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.VendorId == 0x10de && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            std::printf("GPU: %ls\n", d.Description);
            adapter.As(&adapter3);
            return true;
        }
    }
    return false;
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: ngx_dlssnr_check <folder with nvngx_dlssnr.dll> [sdk|direct] [application id, hex]\n");
        return 2;
    }
    const std::wstring folder = argv[1];
    const bool direct = argc > 2 && std::wcscmp(argv[2], L"direct") == 0;
    const unsigned long long applicationId = argc > 3 ? std::wcstoull(argv[3], nullptr, 16) : 0;
    const bool log = GetEnvironmentVariableW(L"NGX_LOG", nullptr, 0) > 0;

    check(nvidia_device(), "a D3D12 device on an NVIDIA GPU");
    if (!device)
        return 1;
    D3D12_COMMAND_QUEUE_DESC q{};
    device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring data = std::wstring(temp) + L"sim-qvfr-ngx-nr-check";
    CreateDirectoryW(data.c_str(), nullptr);

    Runtime rt;
    HMODULE module = nullptr;
    if (!direct) {
        const wchar_t* paths[] = {folder.c_str()};
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
        if (log) {
            info.LoggingInfo.LoggingCallback = ngx_log;
            info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
        }
        const NVSDK_NGX_Result r =
            NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data.c_str(), device.Get(), &info);
        std::printf("route sdk: NGX start with our project id: 0x%08x\n", unsigned(r));
        check(NVSDK_NGX_SUCCEED(r), "NGX starts on D3D12 with a project id");
        if (NVSDK_NGX_FAILED(r))
            return 1;
        rt.allocate = NVSDK_NGX_D3D12_AllocateParameters;
        rt.destroy = NVSDK_NGX_D3D12_DestroyParameters;
        rt.create = sdk_create;
        rt.evaluate = sdk_evaluate;
        rt.release = NVSDK_NGX_D3D12_ReleaseFeature;
    } else {
        module = LoadLibraryExW((folder + L"\\nvngx_dlssnr.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        check(module != nullptr, "nvngx_dlssnr.dll loads");
        if (!module)
            return 1;
        const auto init = reinterpret_cast<InitExtFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init_Ext"));
        rt.allocate = reinterpret_cast<AllocateFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
        rt.destroy = reinterpret_cast<DestroyFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
        rt.create = reinterpret_cast<CreateFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
        rt.evaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        rt.release = reinterpret_cast<ReleaseFn>(GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
        std::printf("route direct: exports Init_Ext %d, AllocateParameters %d, DestroyParameters %d, CreateFeature %d, "
                    "EvaluateFeature %d, ReleaseFeature %d\n",
                    init != nullptr, rt.allocate != nullptr, rt.destroy != nullptr, rt.create != nullptr, rt.evaluate != nullptr,
                    rt.release != nullptr);
        check(init && rt.allocate && rt.destroy && rt.create && rt.evaluate && rt.release, "the DLL exports NGX's D3D12 calls");
        if (!(init && rt.allocate && rt.destroy && rt.create && rt.evaluate && rt.release))
            return 1;
        const NVSDK_NGX_Result r = init(applicationId, data.c_str(), device.Get(), NVSDK_NGX_Version_API, nullptr);
        std::printf("route direct: Init_Ext with application id 0x%llx: 0x%08x\n", applicationId, unsigned(r));
        check(NVSDK_NGX_SUCCEED(r), "the DLL's Init_Ext succeeds");
        if (NVSDK_NGX_FAILED(r))
            return 1;
    }

    // 1. Does it run, and does it change the picture?
    constexpr UINT size = 512;
    std::vector<float> original, baseline;
    double ms = 0, createMs = 0;
    const bool ran = run(rt, Tuning{"baseline"}, size, size, size, size, 0, 0, 6, &original, &baseline, &ms, &createMs);
    check(ran, "feature 18 made and evaluated at 512x512");
    if (!ran)
        return 1;
    const Diff toInput = diff(baseline, original, size, 0, 0, size, size);
    std::printf("  baseline: mean |output - input| %.5f, max %.3f, non-finite %u; feature made in %.0f ms\n", toInput.mean, toInput.max,
                toInput.nonFinite, createMs);
    check(toInput.nonFinite == 0, "the output is finite");
    check(toInput.mean > 1e-4, "the output differs from the input");

    // 2. Which settings change the picture (against the baseline: intensity, tone and structure 1, style and preset 0).
    const Tuning tunings[] = {
        {"intensity 0", 0},          {"intensity 0.5", 0.5f},     {"local tone 0", 1, 0},        {"local tone 2", 1, 2},
        {"local structure 0", 1, 1, 0}, {"local structure 2", 1, 1, 2}, {"auto mask on", 1, 1, 1, 1}, {"style 1", 1, 1, 1, 0, 1},
        {"style 2", 1, 1, 1, 0, 2},  {"preset 1 (A)", 1, 1, 1, 0, 0, 1}, {"preset 2 (B)", 1, 1, 1, 0, 0, 2},
    };
    for (const Tuning& t : tunings) {
        std::vector<float> out;
        double tms = 0;
        if (!run(rt, t, size, size, size, size, 0, 0, 6, nullptr, &out, &tms, nullptr)) {
            std::printf("  %-18s did not run\n", t.name);
            continue;
        }
        const Diff b = diff(out, baseline, size, 0, 0, size, size), i = diff(out, original, size, 0, 0, size, size);
        std::printf("  %-18s vs baseline %.5f (max %.3f), vs input %.5f, %.2f ms\n", t.name, b.mean, b.max, i.mean, tms);
    }
    std::fflush(stdout);

    // 2b. Values above 1 (the scene's disc is 6.0, 5.5, 4.5): what comes back, plain and with the HDR flag, at intensity
    // 0 (the picture passed through) and 1.
    for (const Tuning& t : {Tuning{"plain, intensity 0", 0}, Tuning{"HDR flag, intensity 0", 0, 1, 1, 0, 0, 0, 1},
                            Tuning{"plain, intensity 1"}, Tuning{"HDR flag, intensity 1", 1, 1, 1, 0, 0, 0, 1}}) {
        std::vector<float> out;
        if (!run(rt, t, size, size, size, size, 0, 0, 6, nullptr, &out, nullptr, nullptr)) {
            std::printf("  %-22s did not run\n", t.name);
            continue;
        }
        const size_t disc = (size_t(size * 0.15f) * size + size_t(size * 0.75f)) * 3, ground = (size_t(size * 0.9f) * size + 40) * 3;
        const Diff i = diff(out, original, size, 0, 0, size, size);
        std::printf("  %-22s disc %.3f %.3f %.3f (in %.1f %.1f %.1f), ground %.4f %.4f %.4f (in %.4f %.4f %.4f), vs input %.5f max %.3f\n",
                    t.name, out[disc], out[disc + 1], out[disc + 2], original[disc], original[disc + 1], original[disc + 2],
                    out[ground], out[ground + 1], out[ground + 2], original[ground], original[ground + 1], original[ground + 2],
                    i.mean, i.max);
    }
    std::fflush(stdout);

    // 2c. The settings changed on a feature already made (made at the baseline's): the same picture as a feature made
    // with them? (Then a slider needs no new feature.)
    {
        struct Late {
            Tuning made, late;
        };
        Tuning li{"late intensity 0.5"}, lt{"late local tone 0"}, ls{"late structure 2"}, ly{"late style 2"};
        li.lateIntensity = 0.5f, lt.lateTone = 0, ls.lateStructure = 2, ly.lateStyle = 2;
        const Late cases[] = {{Tuning{"intensity 0.5", 0.5f}, li}, {Tuning{"local tone 0", 1, 0}, lt},
                              {Tuning{"local structure 2", 1, 1, 2}, ls}, {Tuning{"style 2", 1, 1, 1, 0, 2}, ly}};
        for (const Late& k : cases) {
            std::vector<float> made, late;
            if (!run(rt, k.made, size, size, size, size, 0, 0, 6, nullptr, &made, nullptr, nullptr) ||
                !run(rt, k.late, size, size, size, size, 0, 0, 6, nullptr, &late, nullptr, nullptr))
                continue;
            const Diff same = diff(late, made, size, 0, 0, size, size), base = diff(late, baseline, size, 0, 0, size, size);
            std::printf("  %-20s vs made with it %.5f, vs baseline %.5f: %s\n", k.late.name, same.mean, base.mean,
                        same.mean < 1e-4 ? "follows" : base.mean < 1e-4 ? "IGNORED after creation" : "differs from both");
        }
        std::fflush(stdout);
    }

    // 3. Cost by size (square views; an idle GPU, submit and wait included).
    // Video memory: what a view takes (its four textures and its feature), by this process's usage before and after.
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            std::printf("  video memory: this process uses %.0f MB of a %.0f MB budget before the sizes below\n",
                        info.CurrentUsage / 1048576.0, info.Budget / 1048576.0);
    }
    struct Size {
        UINT w, h;
    };
    for (const Size s : {Size{768, 768}, Size{1024, 1024}, Size{1536, 1536}, Size{2048, 2048}, Size{2560, 2560}, Size{2595, 2084}}) {
        double tms = 0, cms = 0;
        if (run(rt, Tuning{"size"}, s.w, s.h, s.w, s.h, 0, 0, 12, nullptr, nullptr, &tms, &cms))
            std::printf("  %ux%u: %.2f ms an evaluation (%.2f ms a megapixel), feature made in %.0f ms, %.0f MB of video memory "
                        "(%.0f MB a megapixel)\n",
                        s.w, s.h, tms, tms / (s.w * s.h / 1e6), cms, lastVramMb, lastVramMb / (s.w * s.h / 1e6));
        else
            std::printf("  %ux%u: did not run\n", s.w, s.h);
        std::fflush(stdout);
    }

    // 4. A region of wider textures: the feature 512x512 at (640, 128) of 1536x768 ones. The output outside the region
    // must stay as it was (zero), the inside must be the picture.
    {
        std::vector<float> in, out;
        const bool sub = run(rt, Tuning{"region"}, size, size, 1536, 768, 640, 128, 6, &in, &out, nullptr, nullptr);
        check(sub, "a feature on a region of wider textures runs");
        if (sub) {
            const std::vector<float> zero(out.size(), 0.0f);
            const Diff inside = diff(out, in, 1536, 640, 128, size, size);
            const Diff left = diff(out, zero, 1536, 0, 0, 640, 768), insideZero = diff(out, zero, 1536, 640, 128, size, size);
            std::printf("  region: inside vs its input %.5f, inside vs zero %.5f, left of it vs zero %.5f\n", inside.mean,
                        insideZero.mean, left.mean);
            check(insideZero.mean > 0.05, "the region was written");
            check(left.mean < 1e-6, "nothing outside the region was written");
            check(inside.mean < 0.25, "the region holds its own part of the picture");
        }
    }

    if (module)
        std::printf("note: route direct leaves the runtime loaded until the process ends\n");
    else
        NVSDK_NGX_D3D12_Shutdown1(device.Get());
    CloseHandle(fenceEvent);
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}

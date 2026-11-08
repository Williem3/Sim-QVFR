// LMU's MVR probe (mvr_probe.h).
#include "mvr_probe.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "../../engine/dxbc_patch.h"
#include "../../engine/nvapi_stereo.h"
#include "../../log.h"
#include "MinHook.h"

namespace lmu {
namespace {

namespace dxbc = simqvfr::dxbc;
namespace nvapi = simqvfr::nvapi;

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
                                                    IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                    ID3D11DeviceContext**);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                         ID3D11VertexShader**);
constexpr int kCreateVertexShaderSlot = 12;  // ID3D11Device's vtable: IUnknown (3), the resources and views (4-11), then this

CreateDeviceFn originalCreateDevice = nullptr;
CreateDeviceAndSwapChainFn originalCreateDeviceAndSwapChain = nullptr;
CreateVertexShaderFn originalCreateVertexShader = nullptr;
std::atomic<bool> shaderHookTried{false};
std::atomic<bool> supportLogged{false};
bool dump = false;
bool underRenderDoc = false;
std::atomic<bool> nvapiUsable{false};

std::mutex statsMutex;
MvrProbeStats stats;
uint64_t loggedCreated = ~0ull;
int64_t lastLogQpc = 0;
int failuresLogged = 0;
thread_local bool inProbe = false;  // NVAPI's own shader creation must not be examined again

double ms_since(const LARGE_INTEGER& from) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(now.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

void log_support(ID3D11Device* device, const char* when) {
    if (supportLogged.exchange(true))
        return;
    if (underRenderDoc) {
        simqvfr::logf("lmu mvr probe: RenderDoc wraps the device (%s): NVAPI skipped, conversions counted only", when);
        return;
    }
    const nvapi::Support s = nvapi::query(device);
    nvapiUsable = s.loaded && s.xyzw;
    simqvfr::logf("lmu mvr probe: the game's device (%s): NVAPI loaded %d, single pass stereo %d, XYZW %d, multi-view %d "
                  "(%d %s)%s", when, s.loaded, s.singlePassStereo, s.xyzw, s.multiView, s.status,
                  nvapi::status_name(s.status), nvapiUsable ? "" : "; NVAPI creation skipped");
}

void save(const uint8_t* code, size_t size) {
    if (size < 20)
        return;
    uint32_t id[4] = {};
    std::memcpy(id, code + 4, 16);  // the container's checksum names it
    char name[64];
    std::snprintf(name, sizeof(name), "VS_%08x%08x.dxbc", id[0], id[1]);
    const std::filesystem::path dir = std::filesystem::path(simqvfr::log_directory()) / L"shaders";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / name;
    if (std::filesystem::exists(path, ec))
        return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        std::fwrite(code, 1, size, f);
        std::fclose(f);
        std::lock_guard guard(statsMutex);
        ++stats.dumped;
    }
}

void examine(ID3D11Device* device, const uint8_t* code, size_t size) {
    inProbe = true;
    log_support(device, "first vertex shader");
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const dxbc::SecondView sv = dxbc::add_second_view(code, size);
    const double convertMs = ms_since(start);
    int status = 0;
    double nvapiMs = 0;
    bool tried = false, created = false;
    if (!sv.bytecode.empty() && nvapiUsable) {
        Microsoft::WRL::ComPtr<ID3D11VertexShader> shader;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        status = nvapi::create_second_view_shader(device, sv, &shader);
        nvapiMs = ms_since(t);
        tried = true;
        created = status == 0 && shader;
    }
    if (dump)
        save(code, size);
    {
        std::lock_guard guard(statsMutex);
        ++stats.created;
        stats.convertMs += convertMs;
        stats.maxConvertMs = std::max(stats.maxConvertMs, convertMs);
        stats.nvapiMs += nvapiMs;
        if (sv.bytecode.empty())
            ++stats.refused[sv.error];
        else
            ++stats.converted;
        if (tried)
            ++(created ? stats.nvapiCreated : stats.nvapiFailed);
        if ((sv.bytecode.empty() || (tried && !created)) && failuresLogged < 8) {
            ++failuresLogged;
            uint32_t id[2] = {};
            if (size >= 12)
                std::memcpy(id, code + 4, 8);
            simqvfr::logf("lmu mvr probe: vertex shader %08x%08x (%zu bytes): %s", id[0], id[1], size,
                          sv.bytecode.empty() ? sv.error.c_str() : nvapi::status_name(status));
        }
    }
    inProbe = false;
}

HRESULT STDMETHODCALLTYPE create_vertex_shader(ID3D11Device* device, const void* code, SIZE_T size,
                                               ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader) {
    const HRESULT hr = originalCreateVertexShader(device, code, size, linkage, shader);
    if (SUCCEEDED(hr) && code && size && !inProbe)
        examine(device, static_cast<const uint8_t*>(code), size);
    return hr;
}

void hook_shaders(ID3D11Device* device, const char* when) {
    if (!device || shaderHookTried.exchange(true))
        return;
    void* target = (*reinterpret_cast<void***>(device))[kCreateVertexShaderSlot];
    const bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&create_vertex_shader),
                                  reinterpret_cast<void**>(&originalCreateVertexShader)) == MH_OK &&
                    MH_EnableHook(target) == MH_OK;
    simqvfr::logf("lmu mvr probe: vertex shader creation %s (%s)", ok ? "hooked" : "NOT hooked", when);
}

HRESULT WINAPI create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                             const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk, ID3D11Device** device,
                             D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    const HRESULT hr = originalCreateDevice(adapter, type, software, flags, levels, count, sdk, device, level, context);
    if (SUCCEEDED(hr) && device && *device)
        hook_shaders(*device, "at D3D11CreateDevice");
    return hr;
}

HRESULT WINAPI create_device_and_swap_chain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                            const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk,
                                            const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain,
                                            ID3D11Device** device, D3D_FEATURE_LEVEL* level,
                                            ID3D11DeviceContext** context) {
    const HRESULT hr = originalCreateDeviceAndSwapChain(adapter, type, software, flags, levels, count, sdk, desc,
                                                        swapChain, device, level, context);
    if (SUCCEEDED(hr) && device && *device)
        hook_shaders(*device, "at D3D11CreateDeviceAndSwapChain");
    return hr;
}

}  // namespace

bool mvr_probe_install(const wchar_t* ini) {
    dump = ini && GetPrivateProfileIntW(L"experiment", L"lmu_mvr_dump", 0, ini) != 0;
    underRenderDoc = GetModuleHandleW(L"renderdoc.dll") != nullptr;
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        simqvfr::log_line("lmu mvr probe: MH_Initialize failed; probe off");
        return false;
    }
    void* target = nullptr;
    const bool device = MH_CreateHookApiEx(L"d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&create_device),
                                           reinterpret_cast<void**>(&originalCreateDevice), &target) == MH_OK &&
                        MH_EnableHook(target) == MH_OK;
    const bool chain = MH_CreateHookApiEx(L"d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                                          reinterpret_cast<void*>(&create_device_and_swap_chain),
                                          reinterpret_cast<void**>(&originalCreateDeviceAndSwapChain), &target) == MH_OK &&
                       MH_EnableHook(target) == MH_OK;
    simqvfr::logf("lmu mvr probe: on (log only)%s; device creation %s%s", dump ? ", shaders dumped to logs\\shaders" : "",
                  device && chain ? "hooked" : "not hooked (the session's device instead)",
                  underRenderDoc ? "; under RenderDoc" : "");
    return true;
}

void mvr_probe_device(ID3D11Device* device) {
    log_support(device, "xrCreateSession");
    hook_shaders(device, "at xrCreateSession: shaders created before it aren't seen");
}

void mvr_probe_frame_end() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    std::lock_guard guard(statsMutex);
    if (stats.created == loggedCreated || now.QuadPart - lastLogQpc < 5 * freq.QuadPart)
        return;
    loggedCreated = stats.created;
    lastLogQpc = now.QuadPart;
    std::string refused;
    for (const auto& [why, n] : stats.refused)
        refused += (refused.empty() ? "" : ", ") + why + " x" + std::to_string(n);
    const double n = stats.created ? static_cast<double>(stats.created) : 1.0;
    const double tried = static_cast<double>(stats.nvapiCreated + stats.nvapiFailed);
    simqvfr::logf("lmu mvr probe: %llu vertex shaders created, %llu converted%s%s%s; NVAPI created %llu, failed %llu; "
                  "conversion %.3f ms avg (max %.3f), NVAPI creation %.3f ms avg%s",
                  static_cast<unsigned long long>(stats.created), static_cast<unsigned long long>(stats.converted),
                  refused.empty() ? "" : " (refused: ", refused.c_str(), refused.empty() ? "" : ")",
                  static_cast<unsigned long long>(stats.nvapiCreated), static_cast<unsigned long long>(stats.nvapiFailed),
                  stats.convertMs / n, stats.maxConvertMs, tried > 0 ? stats.nvapiMs / tried : 0.0,
                  dump ? (", " + std::to_string(stats.dumped) + " dumped").c_str() : "");
}

MvrProbeStats mvr_probe_stats() {
    std::lock_guard guard(statsMutex);
    return stats;
}

}  // namespace lmu

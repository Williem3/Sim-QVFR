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
#include <unordered_map>
#include <vector>

#include "../../engine/dlss_settings.h"
#include "../../engine/dxbc_patch.h"
#include "../../engine/nvapi_stereo.h"
#include "../../engine/object_vtable.h"
#include "../../log.h"
#include "MinHook.h"
#include "yebis_shaders.h"

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
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                        ID3D11PixelShader**);
using CreateInputLayoutFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*,
                                                        SIZE_T, ID3D11InputLayout**);
// ID3D11Device's vtable: IUnknown (3), the resources and views (3-10), CreateInputLayout, CreateVertexShader, the
// geometry shaders (13-14), CreatePixelShader.
constexpr int kCreateInputLayoutSlot = 11, kCreateVertexShaderSlot = 12, kCreatePixelShaderSlot = 15;

CreateDeviceFn originalCreateDevice = nullptr;
CreateDeviceAndSwapChainFn originalCreateDeviceAndSwapChain = nullptr;
CreateVertexShaderFn originalCreateVertexShader = nullptr;
CreatePixelShaderFn originalCreatePixelShader = nullptr;
CreateInputLayoutFn originalCreateInputLayout = nullptr;
std::atomic<bool> shaderHookTried{false};
std::atomic<bool> supportLogged{false};
bool dump = false;
bool attach = false;  // lmu_mvr=1: each game shader gets its converted twin as private data (kMvrTwinGuid)
bool makeBiasTwins = false;  // texture sharpening on at launch ([dlss] texture_sharpening): pixel shaders' mip bias twins
bool markChecksums = false;  // with the focus post: every shader's checksum as private data (the traces' names)
bool underRenderDoc = false;
bool forceInstanced = false;  // [experiment] multi_view_software=1: instancing even where NVIDIA's is there
std::atomic<bool> nvapiUsable{false};
std::atomic<bool> instancedUsable{false};  // the device lets a vertex shader pick the viewport (D3D11_OPTIONS3)
std::atomic<uint64_t> perInstanceLayouts{0};

std::mutex statsMutex;
MvrProbeStats stats;
uint64_t loggedCreated = ~0ull;
int64_t lastLogQpc = 0;
int failuresLogged = 0;
thread_local bool inProbe = false;  // NVAPI's own shader creation must not be examined again

// The game creates each of its ~135 vertex shaders many times (3,810 creations in a race, 2026-11-08): one conversion
// and one NVAPI shader per distinct bytecode, by its checksum (null: refused or not created).
std::mutex twinMutex;
std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11VertexShader>> twins, instancedTwins;
std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11PixelShader>> biasTwins;  // the pixel shaders' (or null)
std::atomic<uint64_t> pixelGeneration{0};

uint64_t id_of(const uint8_t* code, size_t size) {
    uint64_t id = 0;
    if (size >= 12)
        std::memcpy(&id, code + 4, 8);
    return id;
}

double ms_since(const LARGE_INTEGER& from) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(now.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

void log_support(ID3D11Device* device, const char* when) {
    if (supportLogged.exchange(true))
        return;
    // Instancing: a vertex shader may pick the viewport (D3D11.3; RenderDoc passes the device's answer on).
    D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
    instancedUsable = device &&
                      SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3))) &&
                      o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
    const char* instanced = instancedUsable ? "a vertex shader may pick the viewport (instancing possible)"
                                            : "a vertex shader may NOT pick the viewport (no instancing)";
    if (underRenderDoc) {
        simqvfr::logf("lmu mvr probe: RenderDoc wraps the device (%s): NVAPI skipped; %s", when, instanced);
        return;
    }
    const nvapi::Support s = nvapi::query(device);
    nvapiUsable = s.loaded && s.xyzw && !forceInstanced;
    simqvfr::logf("lmu mvr probe: the game's device (%s): NVAPI loaded %d, single pass stereo %d, XYZW %d, multi-view %d "
                  "(%d %s)%s; %s",
                  when, s.loaded, s.singlePassStereo, s.xyzw, s.multiView, s.status, nvapi::status_name(s.status),
                  nvapiUsable          ? ""
                  : s.loaded && s.xyzw ? "; NVAPI not used (multi_view_software=1)"
                                       : "; NVAPI creation skipped",
                  instanced);
}

void save(const uint8_t* code, size_t size, const char* stage = "VS") {
    if (size < 20)
        return;
    uint32_t id[4] = {};
    std::memcpy(id, code + 4, 16);  // the container's checksum names it
    char name[64];
    std::snprintf(name, sizeof(name), "%s_%08x%08x.dxbc", stage, id[0], id[1]);
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

// The instanced twin (dxbc::add_instanced_view) made by D3D11 itself, or null with the reason counted.
Microsoft::WRL::ComPtr<ID3D11VertexShader> make_instanced(ID3D11Device* device, const uint8_t* code, size_t size,
                                                          uint64_t id) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const dxbc::InstancedView iv = dxbc::add_instanced_view(code, size);
    Microsoft::WRL::ComPtr<ID3D11VertexShader> twin;
    HRESULT hr = E_FAIL;
    if (!iv.bytecode.empty())
        hr = originalCreateVertexShader(device, iv.bytecode.data(), iv.bytecode.size(), nullptr, &twin);
    const double ms = ms_since(start);
    std::lock_guard guard(statsMutex);
    stats.instancedMs += ms;
    if (iv.bytecode.empty()) {
        ++stats.instancedRefused[iv.error];
    } else {
        ++stats.instancedConverted;
        ++(SUCCEEDED(hr) && twin ? stats.instancedCreated : stats.instancedFailed);
    }
    if ((iv.bytecode.empty() || FAILED(hr)) && failuresLogged < 8) {
        ++failuresLogged;
        uint32_t name[2] = {};
        std::memcpy(name, &id, 8);
        simqvfr::logf("lmu mvr probe: vertex shader %08x%08x (%zu bytes): no instanced twin: %s", name[0], name[1], size,
                      iv.bytecode.empty() ? iv.error.c_str() : "D3D11 didn't create it");
    }
    if (FAILED(hr))
        twin.Reset();
    return twin;
}

void examine(ID3D11Device* device, const uint8_t* code, size_t size, ID3D11VertexShader* original) {
    inProbe = true;
    log_support(device, "first vertex shader");
    const uint64_t id = id_of(code, size);
    Microsoft::WRL::ComPtr<ID3D11VertexShader> twin, instancedTwin;
    bool known = false;
    {
        std::lock_guard guard(twinMutex);
        if (const auto it = twins.find(id); it != twins.end()) {
            twin = it->second;
            known = true;
        }
        if (const auto it = instancedTwins.find(id); it != instancedTwins.end())
            instancedTwin = it->second;
    }
    if (known) {  // seen before: its twins (or their refusal) again
        if (attach && twin && original)
            original->SetPrivateDataInterface(kMvrTwinGuid, twin.Get());
        if (attach && instancedTwin && original)
            original->SetPrivateDataInterface(kMvrInstancedTwinGuid, instancedTwin.Get());
        std::lock_guard guard(statsMutex);
        ++stats.created;
        ++stats.repeats;
        stats.attached += attach && (twin || instancedTwin) && original ? 1 : 0;
        inProbe = false;
        return;
    }
    // Instancing's twin wherever the device can: the way itself on other GPUs, the fallback on NVIDIA's.
    if (attach && instancedUsable)
        instancedTwin = make_instanced(device, code, size, id);
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const dxbc::SecondView sv = dxbc::add_second_view(code, size);
    const double convertMs = ms_since(start);
    int status = 0;
    double nvapiMs = 0;
    bool tried = false, created = false;
    if (!sv.bytecode.empty() && nvapiUsable) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        status = nvapi::create_second_view_shader(device, sv, &twin);
        nvapiMs = ms_since(t);
        tried = true;
        created = status == 0 && twin;
        if (!created)
            twin.Reset();
    }
    {
        std::lock_guard guard(twinMutex);
        twins[id] = twin;
        instancedTwins[id] = instancedTwin;
    }
    if (attach && twin && original)
        original->SetPrivateDataInterface(kMvrTwinGuid, twin.Get());
    if (attach && instancedTwin && original)
        original->SetPrivateDataInterface(kMvrInstancedTwinGuid, instancedTwin.Get());
    if (dump)
        save(code, size);
    {
        std::lock_guard guard(statsMutex);
        ++stats.created;
        stats.attached += attach && (twin || instancedTwin) && original ? 1 : 0;
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
            uint32_t name[2] = {};
            std::memcpy(name, &id, 8);
            simqvfr::logf("lmu mvr probe: vertex shader %08x%08x (%zu bytes): %s", name[0], name[1], size,
                          sv.bytecode.empty() ? sv.error.c_str() : nvapi::status_name(status));
        }
    }
    inProbe = false;
}

std::atomic<uint64_t> shaderGeneration{0};

// The game's device points at a vtable copy with our shader creation (engine/object_vtable.h): d3d11.dll's code stays
// as it is. D3D11's device creation is hooked only from xrCreateInstance to xrCreateSession (the game creates its first
// shaders before its session), then removed.
simqvfr::ObjectVtable deviceVtable;
ID3D11Device* hookedDevice = nullptr;  // not referenced: compared only
void* deviceCreation[2] = {};          // D3D11CreateDevice, D3D11CreateDeviceAndSwapChain while hooked

HRESULT STDMETHODCALLTYPE create_vertex_shader(ID3D11Device* device, const void* code, SIZE_T size,
                                               ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader) {
    const HRESULT hr = originalCreateVertexShader(device, code, size, linkage, shader);
    if (SUCCEEDED(hr) && code && size && !inProbe)
        examine(device, static_cast<const uint8_t*>(code), size, shader ? *shader : nullptr);
    uint32_t checksum[4];
    if (SUCCEEDED(hr) && markChecksums && !inProbe && shader && *shader && dxbc_checksum(code, size, checksum))
        (*shader)->SetPrivateData(kShaderChecksumGuid, sizeof(checksum), checksum);  // the overlay trace's names
    // The lens mask's and the VR UI panel's: their draws handled at the d3d11 level (mvr_pass.cpp, R6).
    if (const VsRole role = SUCCEEDED(hr) && !inProbe && shader && *shader ? vs_role(code, size) : VsRole::none;
        role != VsRole::none) {
        const UINT value = static_cast<UINT>(role);
        (*shader)->SetPrivateData(kVsRoleGuid, sizeof(value), &value);
        std::lock_guard guard(statsMutex);
        ++(role == VsRole::lensMask ? stats.lensMasks : stats.vruiPanels);
    }
    if (SUCCEEDED(hr))  // after its twin is attached: a lookup that sees the new count sees the twin too
        shaderGeneration.fetch_add(1, std::memory_order_release);
    return hr;
}

// An input layout with per-instance data (instancing draws every instance twice, the instanced twin reading the game's
// as instance >> 1): its twin with every per-instance step rate doubled, so a pair reads its instance's data, made from
// the same shader bytecode and attached for mvr_pass's doubled draws.
HRESULT STDMETHODCALLTYPE create_input_layout(ID3D11Device* device, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count,
                                              const void* code, SIZE_T size, ID3D11InputLayout** layout) {
    const HRESULT hr = originalCreateInputLayout(device, elements, count, code, size, layout);
    if (FAILED(hr) || !layout || !*layout || !elements || !attach || inProbe)
        return hr;
    log_support(device, "first input layout");  // a layout may come before the first vertex shader
    if (!instancedUsable)
        return hr;
    bool perInstance = false;
    for (UINT i = 0; i < count; ++i)
        perInstance = perInstance || (elements[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA &&
                                      elements[i].InstanceDataStepRate > 0);
    {
        std::lock_guard guard(statsMutex);
        ++stats.layouts;
    }
    if (!perInstance)
        return hr;
    std::vector<D3D11_INPUT_ELEMENT_DESC> doubled(elements, elements + count);
    for (D3D11_INPUT_ELEMENT_DESC& e : doubled)
        if (e.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA)
            e.InstanceDataStepRate *= 2;  // 0 (never steps) stays 0
    Microsoft::WRL::ComPtr<ID3D11InputLayout> twin;
    if (SUCCEEDED(originalCreateInputLayout(device, doubled.data(), count, code, size, &twin)) && twin) {
        (*layout)->SetPrivateDataInterface(kMvrInstancedLayoutGuid, twin.Get());
        perInstanceLayouts.fetch_add(1, std::memory_order_release);
        std::lock_guard guard(statsMutex);
        ++stats.instancedLayouts;
    }
    return hr;
}

// YEBIS's copy and composite marked with their role (yebis_shaders.h), for the focus post (mvr_pass.cpp, focus_post.h).
HRESULT STDMETHODCALLTYPE create_pixel_shader(ID3D11Device* device, const void* code, SIZE_T size,
                                              ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) {
    const HRESULT hr = originalCreatePixelShader(device, code, size, linkage, shader);
    if (FAILED(hr) || !shader || !*shader)
        return hr;
    const YebisRole role = yebis_role(code, size);
    if (role != YebisRole::none) {
        const UINT value = static_cast<UINT>(role);
        (*shader)->SetPrivateData(kYebisRoleGuid, sizeof(value), &value);
        std::lock_guard guard(statsMutex);
        ++(role == YebisRole::copy ? stats.yebisCopies : role == YebisRole::composite ? stats.yebisComposites : stats.finalBlits);
    }
    if (markChecksums) {  // the focus post's diagnostics (lmu_focus_replay): which shaders the post-processing runs
        uint32_t checksum[4];
        if (dxbc_checksum(code, size, checksum))
            (*shader)->SetPrivateData(kShaderChecksumGuid, sizeof(checksum), checksum);
        if (dump)
            save(static_cast<const uint8_t*>(code), size, "PS");
    }
    // Texture sharpening (mvr_pass.cpp): the mip bias twin, one per distinct bytecode, made when it is first seen.
    if (attach && makeBiasTwins && !inProbe && role == YebisRole::none && code && size) {
        const auto* bytes = static_cast<const uint8_t*>(code);
        const uint64_t id = id_of(bytes, size);
        Microsoft::WRL::ComPtr<ID3D11PixelShader> twin;
        bool seen = false;
        {
            std::lock_guard guard(twinMutex);
            if (const auto it = biasTwins.find(id); it != biasTwins.end()) {
                twin = it->second;
                seen = true;
            }
        }
        std::string refusedWhy;
        bool failed = false;
        if (!seen) {
            const dxbc::FocusMipBias b = dxbc::add_focus_mip_bias(bytes, size);
            if (b.bytecode.empty())
                refusedWhy = b.error;
            else if (FAILED(originalCreatePixelShader(device, b.bytecode.data(), b.bytecode.size(), nullptr, &twin)))
                failed = true;
            std::lock_guard guard(twinMutex);
            biasTwins[id] = twin;
        }
        if (twin)
            (*shader)->SetPrivateDataInterface(kMvrMipBiasTwinGuid, twin.Get());
        std::lock_guard guard(statsMutex);
        ++stats.pixelCreated;
        stats.pixelRepeats += seen ? 1 : 0;
        stats.pixelTwinned += twin ? 1 : 0;
        stats.pixelFailed += failed ? 1 : 0;
        if (!refusedWhy.empty())
            ++stats.pixelRefused[refusedWhy];
    }
    pixelGeneration.fetch_add(1, std::memory_order_release);  // after its twin is attached (as for vertex shaders)
    return hr;
}

void hook_shaders(ID3D11Device* device, const char* when) {
    if (!device || shaderHookTried.exchange(true))
        return;
    const bool ok = deviceVtable.install(device, simqvfr::kD3D11DeviceSlots,
                                         {{kCreateInputLayoutSlot, reinterpret_cast<void*>(&create_input_layout),
                                           reinterpret_cast<void**>(&originalCreateInputLayout)},
                                          {kCreateVertexShaderSlot, reinterpret_cast<void*>(&create_vertex_shader),
                                           reinterpret_cast<void**>(&originalCreateVertexShader)},
                                          {kCreatePixelShaderSlot, reinterpret_cast<void*>(&create_pixel_shader),
                                           reinterpret_cast<void**>(&originalCreatePixelShader)}});
    if (ok)
        hookedDevice = device;
    simqvfr::logf("lmu mvr probe: input layout, vertex and pixel shader creation %s (%s; the device on a vtable copy, %zu "
                  "entries)",
                  ok ? "hooked" : "NOT hooked", when, deviceVtable.slots());
}

void unhook_device_creation() {
    int removed = 0, hooked = 0;
    for (void*& target : deviceCreation)
        if (target) {
            ++hooked;
            removed += MH_RemoveHook(target) == MH_OK ? 1 : 0;
            target = nullptr;
        }
    if (hooked)
        simqvfr::logf("lmu mvr probe: device creation unhooked (%d of %d)", removed, hooked);
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

bool mvr_probe_install(const wchar_t* ini, bool attachTwins, bool nameShaders) {
    dump = ini && GetPrivateProfileIntW(L"experiment", L"lmu_mvr_dump", 0, ini) != 0;
    attach = attachTwins;
    // Texture sharpening's twins only when it's on at launch (as its hook, four_passes.cpp install_mvr).
    makeBiasTwins = attach && ini && simqvfr::read_dlss_settings(ini).textureSharpening != 0;
    forceInstanced = ini && GetPrivateProfileIntW(L"experiment", L"multi_view_software", 0, ini) != 0;
    markChecksums =
        attach && (nameShaders || (ini && GetPrivateProfileIntW(L"experiment", L"lmu_focus_replay", 0, ini) != 0));
    underRenderDoc = GetModuleHandleW(L"renderdoc.dll") != nullptr;
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        simqvfr::log_line("lmu mvr probe: MH_Initialize failed; probe off");
        return false;
    }
    const bool device = MH_CreateHookApiEx(L"d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&create_device),
                                           reinterpret_cast<void**>(&originalCreateDevice), &deviceCreation[0]) == MH_OK &&
                        MH_EnableHook(deviceCreation[0]) == MH_OK;
    const bool chain = MH_CreateHookApiEx(L"d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                                          reinterpret_cast<void*>(&create_device_and_swap_chain),
                                          reinterpret_cast<void**>(&originalCreateDeviceAndSwapChain),
                                          &deviceCreation[1]) == MH_OK &&
                       MH_EnableHook(deviceCreation[1]) == MH_OK;
    simqvfr::logf("lmu mvr probe: on (%s)%s%s%s; device creation %s%s",
                  attach ? "each shader's twin attached for lmu_mvr" : "log only",
                  forceInstanced ? "; instancing, not NVIDIA's multi-view rendering (multi_view_software=1)" : "",
                  dump ? (markChecksums ? ", vertex and pixel shaders dumped to logs\\shaders" : ", shaders dumped to logs\\shaders")
                       : "",
                  markChecksums ? "; shaders named by checksum for the log's traces" : "",
                  device && chain ? "hooked" : "not hooked (the session's device instead)",
                  underRenderDoc ? "; under RenderDoc" : "");
    return true;
}

void mvr_probe_device(ID3D11Device* device) {
    unhook_device_creation();
    log_support(device, "xrCreateSession");
    if (hookedDevice && device != hookedDevice)
        simqvfr::log_line("lmu mvr probe: the session's device isn't the first one created (the one hooked): its "
                          "shaders aren't seen");
    hook_shaders(device, "at xrCreateSession: shaders created before it aren't seen");
}

void mvr_probe_frame_end() {
    // The device's vtable is d3d11's and never changes (unlike the context's): its pointer replaced means another tool.
    static bool copyLostLogged = false;
    if (hookedDevice && !copyLostLogged && !deviceVtable.installed()) {
        copyLostLogged = true;
        simqvfr::log_line("lmu mvr probe: the device's vtable pointer was replaced since (another tool?): vertex shaders "
                          "created from now on get no twin");
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    std::lock_guard guard(statsMutex);
    if (stats.created == loggedCreated || now.QuadPart - lastLogQpc < 5 * freq.QuadPart)
        return;
    loggedCreated = stats.created;
    lastLogQpc = now.QuadPart;
    std::string refused, instancedRefused;
    for (const auto& [why, n] : stats.refused)
        refused += (refused.empty() ? "" : ", ") + why + " x" + std::to_string(n);
    for (const auto& [why, n] : stats.instancedRefused)
        instancedRefused += (instancedRefused.empty() ? "" : ", ") + why + " x" + std::to_string(n);
    const double n = stats.created ? static_cast<double>(stats.created) : 1.0;
    const double tried = static_cast<double>(stats.nvapiCreated + stats.nvapiFailed);
    double instancedTried = static_cast<double>(stats.instancedConverted);
    for (const auto& [why, count] : stats.instancedRefused)
        instancedTried += static_cast<double>(count);
    simqvfr::logf("lmu mvr probe: %llu vertex shaders created (%llu distinct), %llu converted%s%s%s; NVAPI created %llu, "
                  "failed %llu; instanced twins %llu converted%s%s%s, %llu created, %llu failed (%.3f ms avg); %llu game "
                  "shaders given twins; input layouts %llu (%llu with per-instance data, twinned); conversion %.3f ms avg "
                  "(max %.3f), NVAPI creation %.3f ms avg%s; YEBIS's copy and composite pixel shaders marked %llu and %llu "
                  "times",
                  static_cast<unsigned long long>(stats.created),
                  static_cast<unsigned long long>(stats.created - stats.repeats),
                  static_cast<unsigned long long>(stats.converted),
                  refused.empty() ? "" : " (refused: ", refused.c_str(), refused.empty() ? "" : ")",
                  static_cast<unsigned long long>(stats.nvapiCreated), static_cast<unsigned long long>(stats.nvapiFailed),
                  static_cast<unsigned long long>(stats.instancedConverted),
                  instancedRefused.empty() ? "" : " (refused: ", instancedRefused.c_str(), instancedRefused.empty() ? "" : ")",
                  static_cast<unsigned long long>(stats.instancedCreated),
                  static_cast<unsigned long long>(stats.instancedFailed),
                  instancedTried > 0 ? stats.instancedMs / instancedTried : 0.0,
                  static_cast<unsigned long long>(stats.attached), static_cast<unsigned long long>(stats.layouts),
                  static_cast<unsigned long long>(stats.instancedLayouts), stats.convertMs / n, stats.maxConvertMs,
                  tried > 0 ? stats.nvapiMs / tried : 0.0,
                  dump ? (", " + std::to_string(stats.dumped) + " dumped").c_str() : "",
                  static_cast<unsigned long long>(stats.yebisCopies), static_cast<unsigned long long>(stats.yebisComposites));
    if (stats.pixelCreated) {
        std::string pixelRefused;
        for (const auto& [why, count] : stats.pixelRefused)
            pixelRefused += (pixelRefused.empty() ? "" : ", ") + why + " x" + std::to_string(count);
        simqvfr::logf("lmu mvr probe: %llu pixel shaders created (%llu distinct), %llu given a mip bias twin (texture "
                      "sharpening)%s%s%s, %llu twins not created",
                      static_cast<unsigned long long>(stats.pixelCreated),
                      static_cast<unsigned long long>(stats.pixelCreated - stats.pixelRepeats),
                      static_cast<unsigned long long>(stats.pixelTwinned), pixelRefused.empty() ? "" : " (refused: ",
                      pixelRefused.c_str(), pixelRefused.empty() ? "" : ")",
                      static_cast<unsigned long long>(stats.pixelFailed));
    }
}

bool mvr_probe_usable() { return attach && (nvapiUsable || instancedUsable); }

MvrBackend mvr_probe_backend() {
    return !attach ? MvrBackend::none
           : nvapiUsable ? MvrBackend::nvapi
           : instancedUsable ? MvrBackend::instanced
                             : MvrBackend::none;
}

bool mvr_probe_instanced() { return attach && instancedUsable; }

uint64_t mvr_probe_instanced_layouts() { return perInstanceLayouts.load(std::memory_order_acquire); }

const char* mvr_backend_name(MvrBackend backend) {
    return backend == MvrBackend::nvapi       ? "NVIDIA's multi-view rendering"
           : backend == MvrBackend::instanced ? "instancing (each draw twice, the vertex shader picking the viewport)"
                                              : "none";
}

MvrProbeStats mvr_probe_stats() {
    std::lock_guard guard(statsMutex);
    return stats;
}

uint64_t mvr_shader_generation() { return shaderGeneration.load(std::memory_order_acquire); }
uint64_t mvr_pixel_shader_generation() { return pixelGeneration.load(std::memory_order_acquire); }

}  // namespace lmu

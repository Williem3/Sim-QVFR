// AMD FSR upscaling on our own D3D12 device for a D3D11 game (fsr_upscale.h), on the bridge DLSS-NR shares
// (engine/d3d12_bridge.h): shared textures, shared fences, releases deferred until both devices are through.
#include "fsr_upscale.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
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
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <cwchar>
#include <string>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

#include "d3d12_bridge.h"

namespace simqvfr::fsr {

struct View : BridgeView {
    unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;
    ffxContext context = nullptr;
};

namespace {

// AMD's runtime: loaded once, kept for the process (shutdown()).
struct Runtime {
    HMODULE loader = nullptr, upscaler = nullptr;
    std::wstring folder;
    ffxFunctions ffx{};
} rt;

void destroy(BridgeView* bv) {
    auto* v = static_cast<View*>(bv);
    if (v->context && rt.ffx.DestroyContext)
        rt.ffx.DestroyContext(&v->context, nullptr);
    delete v;
}

D3D12Bridge bridge("fsr", "FSR's output", destroy);

struct State {
    Provider provider = Provider::Newest;
    uint64_t overrideId = 0;  // the provider asked for (0: the runtime's own choice, the newest)
    std::string providers, inUse;
} s;

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

// Opens `path` without write or delete sharing (so it can't be swapped between this check and the load) and checks
// its Authenticode signature is valid and AMD's: the DLLs can come from the game's folder. Once per load, no UI,
// revocation from the cache only (no network stall). INVALID_HANDLE_VALUE (and `why`) when it isn't; the caller
// closes the handle once the DLL is loaded.
HANDLE open_signed_by_amd(const std::wstring& path, std::string& why) {
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        why = "couldn't be opened (error " + std::to_string(GetLastError()) + ")";
        return file;
    }
    WINTRUST_FILE_INFO info{sizeof(info)};
    info.pcwszFilePath = path.c_str();
    info.hFile = file;
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trust = WinVerifyTrust(nullptr, &action, &data);
    wchar_t signer[256] = L"";
    if (trust == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sgnr = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        if (sgnr && sgnr->csCertChain && sgnr->pasCertChain && sgnr->pasCertChain[0].pCert)
            CertGetNameStringW(sgnr->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, signer, 256);
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    if (trust == ERROR_SUCCESS && std::wcsstr(signer, L"Advanced Micro Devices"))
        return file;
    CloseHandle(file);
    char text[160];
    if (trust != ERROR_SUCCESS)
        std::snprintf(text, sizeof(text), "has no valid signature (WinVerifyTrust 0x%08lx)", static_cast<unsigned long>(trust));
    else
        std::snprintf(text, sizeof(text), "isn't signed by AMD (signer \"%ls\")", signer);
    why = text;
    return INVALID_HANDLE_VALUE;
}

// AMD's two DLLs from the first folder holding both, signed by AMD: the upscaler first and by its full path, so the
// loader finds it already loaded and takes no other copy (its lookup: a loaded module, the exe's folder, its own folder).
bool load_runtime(const wchar_t* dllFolder, std::string& why) {
    std::vector<std::wstring> folders;
    if (dllFolder && *dllFolder)
        folders.emplace_back(dllFolder);
    folders.push_back(own_folder());
    folders.push_back(exe_folder());
    std::string refused;  // the last unsigned pair's reason, if no folder had a signed one
    for (const std::wstring& f : folders) {
        if (!has_both(f))
            continue;
        const std::wstring upscalerPath = f + L"\\" + kUpscalerDll, loaderPath = f + L"\\" + kLoaderDll;
        std::string bad;
        const HANDLE upscalerFile = open_signed_by_amd(upscalerPath, bad);
        const HANDLE loaderFile =
            upscalerFile != INVALID_HANDLE_VALUE ? open_signed_by_amd(loaderPath, bad) : INVALID_HANDLE_VALUE;
        if (loaderFile == INVALID_HANDLE_VALUE) {
            if (upscalerFile != INVALID_HANDLE_VALUE)
                CloseHandle(upscalerFile);
            refused = std::string(upscalerFile != INVALID_HANDLE_VALUE ? "amd_fidelityfx_loader_dx12.dll "
                                                                       : "amd_fidelityfx_upscaler_dx12.dll ") +
                      bad;
            logf("fsr: refused AMD's FSR DLLs in %ls: %s", f.c_str(), refused.c_str());
            continue;
        }
        // (the handles, shared for reading only, keep both files as checked until loaded)
        const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
        rt.upscaler = LoadLibraryExW(upscalerPath.c_str(), nullptr, flags);
        rt.loader = rt.upscaler ? LoadLibraryExW(loaderPath.c_str(), nullptr, flags) : nullptr;
        const DWORD error = GetLastError();
        CloseHandle(upscalerFile);
        CloseHandle(loaderFile);
        if (!rt.loader) {
            if (rt.upscaler)
                FreeLibrary(rt.upscaler);
            rt.upscaler = nullptr;
            char text[200];
            std::snprintf(text, sizeof(text), "AMD's FSR DLLs in the folder found couldn't be loaded (error %lu)", error);
            why = bridge.failure = text;
            return false;
        }
        ffxLoadFunctions(&rt.ffx, rt.loader);
        if (!rt.ffx.CreateContext || !rt.ffx.DestroyContext || !rt.ffx.Query || !rt.ffx.Dispatch) {
            // Unloaded and cleared: rt.upscaler set would read as a loaded runtime next time.
            FreeLibrary(rt.loader);
            FreeLibrary(rt.upscaler);
            rt.loader = rt.upscaler = nullptr;
            rt.ffx = {};
            why = bridge.failure = "amd_fidelityfx_loader_dx12.dll lacks the FSR API's functions";
            return false;
        }
        rt.folder = f;
        return true;
    }
    if (!refused.empty()) {
        why = bridge.failure = "AMD's FSR DLLs weren't loaded: " + refused;
        return false;
    }
    why = bridge.failure = dllFolder && *dllFolder
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
    q.device = bridge.device();
    q.outputCount = &count;
    if (rt.ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0 || count > 64) {
        s.providers = "(the runtime lists none)";
        return;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (rt.ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK) {
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
    if (bridge.tried) {
        why = bridge.failure;
        return bridge.started;
    }
    bridge.tried = true;
    s.provider = provider;
    ComPtr<IDXGIAdapter> adapter;
    if (!bridge.open_game_device(device, adapter, why))
        return false;
    // AMD's DLLs before our device: checked and loaded first.
    if (!rt.upscaler && !load_runtime(dllFolder, why))
        return false;
    if (!bridge.create_device(adapter.Get(), why))
        return false;
    choose_provider();
    bridge.started = true;
    return true;
}

std::string providers() { return s.providers; }
std::string provider_in_use() { return s.inUse; }
std::wstring loaded_folder() { return rt.upscaler ? rt.folder : std::wstring(); }

View* create(unsigned renderWidth, unsigned renderHeight, unsigned outputWidth, unsigned outputHeight, DXGI_FORMAT format,
             bool depthInverted, bool depthInfinite, unsigned& result) {
    result = FFX_API_RETURN_ERROR;
    if (!bridge.started || !renderWidth || !renderHeight || !outputWidth || !outputHeight)
        return nullptr;
    auto* v = new View;
    v->width = renderWidth, v->height = renderHeight, v->outWidth = outputWidth, v->outHeight = outputHeight;
    const DXGI_FORMAT colour = typed(format);
    HRESULT hr = S_OK;
    if (!bridge.make_view(v, renderWidth, renderHeight, colour, outputWidth, outputHeight, colour, hr)) {
        result = static_cast<unsigned>(hr);
        bridge.discard(v);
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
    backend.device = bridge.device();
    ffxOverrideVersion over{};
    over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    over.versionId = s.overrideId;
    desc.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    backend.header.pNext = s.overrideId ? &over.header : nullptr;
    ffxReturnCode_t r = rt.ffx.CreateContext(&v->context, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK) {  // an older runtime than these headers: without the version this build was made with
        v->context = nullptr;
        desc.header.pNext = &backend.header;
        const ffxReturnCode_t again = rt.ffx.CreateContext(&v->context, &desc.header, nullptr);
        if (again == FFX_API_RETURN_OK)
            simqvfr::logf("fsr: the runtime refused this build's FSR API version (%u.%u.%u, result %u): made without it",
                          FFX_UPSCALER_VERSION_MAJOR, FFX_UPSCALER_VERSION_MINOR, FFX_UPSCALER_VERSION_PATCH, r);
        r = again;
    }
    result = r;
    if (r != FFX_API_RETURN_OK || !v->context) {
        v->context = nullptr;
        bridge.discard(v);
        return nullptr;
    }
    if (s.inUse.empty()) {
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (rt.ffx.Query(&v->context, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
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
    if (!bridge.started || !context || !v || !v->context)
        return false;
    D3D12Bridge::Evaluating ev;
    if (!bridge.begin_evaluation(context, v, kBacklog, kBehind, result, ev))
        return false;
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = bridge.list();
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
    const ffxReturnCode_t r = rt.ffx.Dispatch(&v->context, &d.header);
    // Submitted whatever FSR answered.
    if (!bridge.end_evaluation(v, ev))
        return false;
    result = r;
    return r == FFX_API_RETURN_OK;
}

void release(ID3D11DeviceContext* context, View* v, IUnknown* const* keep, unsigned count) { bridge.release(context, v, keep, count); }

void collect() { bridge.collect(); }

unsigned pending_releases() { return bridge.released(); }

bool device_lost() { return bridge.device_lost(); }

double take_gpu_ms(View* v) { return bridge.take_gpu_ms(v); }

void shutdown(ID3D11DeviceContext* context) {
    // AMD's DLLs stay loaded for the process (rt: a later session takes them again; unloading a runtime that may keep
    // threads or callbacks is not ours to risk).
    if (bridge.shutdown(context, "FSR", ""))
        bridge.reset();
    s = State{};
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

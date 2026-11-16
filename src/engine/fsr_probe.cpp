// AMD's provider query outside the game (fsr_probe.h).
#include "fsr_probe.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
#include <vector>

#ifdef SIM_QVFR_FFX
#include <d3d12.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <cwchar>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#endif

using Microsoft::WRL::ComPtr;

namespace simqvfr::fsr {

namespace {

// "3.1.5", "FSR 4.1.1 *" -> 3 1, 4 1.
void version_of(const char* name, int& major, int& minor) {
    major = minor = 0;
    const char* p = name ? name : "";
    while (*p && (*p < '0' || *p > '9'))
        ++p;
    major = std::atoi(p);
    while (*p >= '0' && *p <= '9')
        ++p;
    if (*p == '.')
        minor = std::atoi(p + 1);
}

#ifdef SIM_QVFR_FFX
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

// Opens `path` without write or delete sharing (so it can't be swapped before the load) when its Authenticode
// signature is valid and AMD's; INVALID_HANDLE_VALUE otherwise. Once, no UI, revocation from the cache only (no
// network stall). The caller closes the handle once the DLL is loaded.
HANDLE open_signed_by_amd(const std::wstring& path) {
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return file;
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
    return INVALID_HANDLE_VALUE;
}
#endif

}  // namespace

Offer probe_offer(const std::wstring& folder) {
    Offer o;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
        o.why = L"No graphics card was found.";
        return o;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    o.gpu = desc.Description;
    o.vendor = desc.VendorId;
#ifdef SIM_QVFR_FFX
    const std::wstring upscaler = folder + L"\\amd_fidelityfx_upscaler_dx12.dll", loader = folder + L"\\amd_fidelityfx_loader_dx12.dll";
    if (GetFileAttributesW(upscaler.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(loader.c_str()) == INVALID_FILE_ATTRIBUTES) {
        o.why = L"AMD’s FSR files are missing from this copy of Sim-QVFR.";
        return o;
    }
    // Only AMD-signed copies, held open as checked until loaded.
    const HANDLE upscalerFile = open_signed_by_amd(upscaler);
    const HANDLE loaderFile = upscalerFile != INVALID_HANDLE_VALUE ? open_signed_by_amd(loader) : INVALID_HANDLE_VALUE;
    if (loaderFile == INVALID_HANDLE_VALUE) {
        if (upscalerFile != INVALID_HANDLE_VALUE)
            CloseHandle(upscalerFile);
        o.why = L"AMD’s FSR files in this copy of Sim-QVFR aren’t signed by AMD.";
        return o;
    }
    // The upscaler first, by its full path: the loader then takes it, not a copy elsewhere. Both stay loaded (the
    // launcher asks once).
    const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    const HMODULE effect = LoadLibraryExW(upscaler.c_str(), nullptr, flags);
    const HMODULE module = effect ? LoadLibraryExW(loader.c_str(), nullptr, flags) : nullptr;
    CloseHandle(upscalerFile);
    CloseHandle(loaderFile);
    ffxFunctions ffx{};
    if (module)
        ffxLoadFunctions(&ffx, module);
    if (!ffx.Query) {
        if (module)
            FreeLibrary(module);
        if (effect)
            FreeLibrary(effect);
        o.why = L"AMD’s FSR files couldn’t be loaded.";
        return o;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        o.why = L"The graphics card has no Direct3D 12.";
        return o;
    }
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = device.Get();
    q.outputCount = &count;
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0 || count > 64) {
        o.why = L"AMD’s FSR offers nothing on this graphics card.";
        return o;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK) {
        o.why = L"AMD’s FSR couldn’t list what it offers.";
        return o;
    }
    for (uint64_t i = 0; i < count; ++i) {
        int major = 0, minor = 0;
        version_of(names[i], major, minor);
        if (!o.names.empty())
            o.names += L", ";
        for (const char* p = names[i] ? names[i] : "?"; *p; ++p)
            o.names += static_cast<wchar_t>(static_cast<unsigned char>(*p));
        if (major > o.major || (major == o.major && minor > o.minor))
            o.major = major, o.minor = minor;
    }
    o.checked = true;
#else
    (void)folder;
    o.why = L"This build has no AMD FSR.";
#endif
    return o;
}

}  // namespace simqvfr::fsr

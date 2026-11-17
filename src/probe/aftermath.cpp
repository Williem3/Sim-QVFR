#include "probe/aftermath.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "log.h"

#if defined(SIM_QVFR_AFTERMATH)
#include "GFSDK_Aftermath.h"
#include "GFSDK_Aftermath_GpuCrashDump.h"
#include "GFSDK_Aftermath_GpuCrashDumpDecoding.h"
#endif

namespace simqvfr::aftermath {

#if !defined(SIM_QVFR_AFTERMATH)

bool built() { return false; }
bool enable_dumps() { return false; }
bool attach(ID3D11Device*, ID3D11DeviceContext*, bool) { return false; }
void mark(uint64_t) {}
bool marking() { return false; }
void set_marker_names(void (*)(uint64_t, char*, unsigned)) {}
void after_removal(unsigned) {}
void detach() {}

#else

namespace {

// The library's entry points, from the SDK's DLL (loaded at run time: a layer that links it can't load without it).
struct Api {
    decltype(&GFSDK_Aftermath_EnableGpuCrashDumps) enableDumps = nullptr;
    decltype(&GFSDK_Aftermath_GetCrashDumpStatus) dumpStatus = nullptr;
    decltype(&GFSDK_Aftermath_DX11_Initialize) dx11Initialize = nullptr;
    decltype(&GFSDK_Aftermath_DX11_CreateContextHandle) createContext = nullptr;
    decltype(&GFSDK_Aftermath_ReleaseContextHandle) releaseContext = nullptr;
    decltype(&GFSDK_Aftermath_SetEventMarker) setMarker = nullptr;
    decltype(&GFSDK_Aftermath_GetData) getData = nullptr;
    decltype(&GFSDK_Aftermath_GetDeviceStatus) deviceStatus = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_CreateDecoder) createDecoder = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_DestroyDecoder) destroyDecoder = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo) dumpDevice = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfoCount) markerCount = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfo) markerInfo = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount) shaderCount = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo) shaderInfo = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GetPageFaultInfo) pageFault = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GenerateJSON) generateJson = nullptr;
    decltype(&GFSDK_Aftermath_GpuCrashDump_GetJSON) getJson = nullptr;
};
Api api;
bool loaded = false, dumpsOn = false;
GFSDK_Aftermath_ContextHandle context = nullptr;
std::atomic<bool> markers{false};
void (*namer)(uint64_t, char*, unsigned) = nullptr;
std::atomic<unsigned> dumps{0};

std::string name_of(uint64_t code) {
    char text[160] = "";
    if (namer)
        namer(code, text, sizeof(text));
    else
        snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(code));
    return text;
}

const char* result_text(GFSDK_Aftermath_Result r) {
    switch (r) {
        case GFSDK_Aftermath_Result_Success: return "success";
        case GFSDK_Aftermath_Result_FAIL_AlreadyInitialized: return "already initialised";
        case GFSDK_Aftermath_Result_FAIL_InvalidAdapter: return "not an NVIDIA adapter";
        case GFSDK_Aftermath_Result_FAIL_DriverInitFailed: return "the driver's init failed";
        case GFSDK_Aftermath_Result_FAIL_DriverVersionNotSupported: return "driver version not supported";
        case GFSDK_Aftermath_Result_FAIL_D3dDllInterceptionNotSupported: return "a D3D interception tool (PIX?) is active";
        case GFSDK_Aftermath_Result_FAIL_Disabled: return "disabled in the registry (ForceOff)";
        default: return "failed";
    }
}

const char* status_text(GFSDK_Aftermath_Device_Status s) {
    switch (s) {
        case GFSDK_Aftermath_Device_Status_Active: return "active";
        case GFSDK_Aftermath_Device_Status_Timeout: return "TIMEOUT (a GPU hang)";
        case GFSDK_Aftermath_Device_Status_OutOfMemory: return "out of memory";
        case GFSDK_Aftermath_Device_Status_PageFault: return "PAGE FAULT";
        case GFSDK_Aftermath_Device_Status_Stopped: return "stopped";
        case GFSDK_Aftermath_Device_Status_Reset: return "reset";
        case GFSDK_Aftermath_Device_Status_DmaFault: return "DMA fault";
        case GFSDK_Aftermath_Device_Status_DeviceRemovedNoGpuFault: return "removed, no GPU fault";
        default: return "unknown";
    }
}

template <typename F>
bool load(HMODULE lib, F& fn, const char* name) {
    fn = reinterpret_cast<F>(GetProcAddress(lib, name));
    return fn != nullptr;
}

bool load_library() {
    if (loaded)
        return true;
    HMODULE lib = LoadLibraryW(SIM_QVFR_AFTERMATH_DLL);
    if (!lib) {
        logf("aftermath: %ls didn't load (error %lu)", SIM_QVFR_AFTERMATH_DLL, GetLastError());
        return false;
    }
    loaded = load(lib, api.enableDumps, "GFSDK_Aftermath_EnableGpuCrashDumps") &&
             load(lib, api.dumpStatus, "GFSDK_Aftermath_GetCrashDumpStatus") &&
             load(lib, api.dx11Initialize, "GFSDK_Aftermath_DX11_Initialize") &&
             load(lib, api.createContext, "GFSDK_Aftermath_DX11_CreateContextHandle") &&
             load(lib, api.releaseContext, "GFSDK_Aftermath_ReleaseContextHandle") &&
             load(lib, api.setMarker, "GFSDK_Aftermath_SetEventMarker") &&
             load(lib, api.getData, "GFSDK_Aftermath_GetData") &&
             load(lib, api.deviceStatus, "GFSDK_Aftermath_GetDeviceStatus") &&
             load(lib, api.createDecoder, "GFSDK_Aftermath_GpuCrashDump_CreateDecoder") &&
             load(lib, api.destroyDecoder, "GFSDK_Aftermath_GpuCrashDump_DestroyDecoder") &&
             load(lib, api.dumpDevice, "GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo") &&
             load(lib, api.markerCount, "GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfoCount") &&
             load(lib, api.markerInfo, "GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfo") &&
             load(lib, api.shaderCount, "GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount") &&
             load(lib, api.shaderInfo, "GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo") &&
             load(lib, api.pageFault, "GFSDK_Aftermath_GpuCrashDump_GetPageFaultInfo") &&
             load(lib, api.generateJson, "GFSDK_Aftermath_GpuCrashDump_GenerateJSON") &&
             load(lib, api.getJson, "GFSDK_Aftermath_GpuCrashDump_GetJSON");
    if (!loaded)
        log_line("aftermath: the library lacks an entry point this build uses; off");
    return loaded;
}

bool write_file(const std::wstring& path, const void* data, size_t bytes) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return false;
    fwrite(data, 1, bytes, f);
    fclose(f);
    return true;
}

// Aftermath's own thread, once the dump is ready: written, decoded into the log, its JSON beside it.
void GFSDK_AFTERMATH_CALL on_dump(const void* dump, const uint32_t bytes, void*) {
    const unsigned n = ++dumps;
    const std::wstring base = log_directory() + L"\\gpu-crash-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(n);
    const bool saved = write_file(base + L".nv-gpudmp", dump, bytes);
    logf("aftermath: GPU crash dump %u (%u bytes) %s %ls.nv-gpudmp", n, bytes, saved ? "written to" : "NOT written to",
         base.c_str());
    GFSDK_Aftermath_GpuCrashDump_Decoder decoder = nullptr;
    if (api.createDecoder(GFSDK_Aftermath_Version_API, dump, bytes, &decoder) != GFSDK_Aftermath_Result_Success) {
        log_line("aftermath: the dump couldn't be decoded");
        return;
    }
    GFSDK_Aftermath_GpuCrashDump_DeviceInfo device{};
    if (api.dumpDevice(decoder, &device) == GFSDK_Aftermath_Result_Success)
        logf("aftermath: the device: %s (adapter reset %d, engine reset %d)", status_text(device.status),
             device.adapterReset ? 1 : 0, device.engineReset ? 1 : 0);
    GFSDK_Aftermath_GpuCrashDump_PageFaultInfo fault{};
    if (api.pageFault(decoder, &fault) == GFSDK_Aftermath_Result_Success)
        logf("aftermath: a page fault at GPU address %016llx (fault type %u, access %u, engine %u, client %u, %u resources)",
             static_cast<unsigned long long>(fault.faultingGpuVA), static_cast<unsigned>(fault.faultType),
             static_cast<unsigned>(fault.accessType), static_cast<unsigned>(fault.engine), static_cast<unsigned>(fault.client),
             fault.resourceInfoCount);
    uint32_t count = 0;
    if (api.markerCount(decoder, &count) == GFSDK_Aftermath_Result_Success && count) {
        GFSDK_Aftermath_GpuCrashDump_EventMarkerInfo info[16]{};
        const uint32_t take = count < 16 ? count : 16;
        if (api.markerInfo(decoder, take, info) == GFSDK_Aftermath_Result_Success)
            for (uint32_t i = 0; i < take; ++i)
                logf("aftermath: context %llu (status %u, type %u): the last marker the GPU reached: %s",
                     static_cast<unsigned long long>(info[i].contextId), static_cast<unsigned>(info[i].contextStatus),
                     static_cast<unsigned>(info[i].contextType),
                     name_of(reinterpret_cast<uint64_t>(info[i].markerData)).c_str());
    } else {
        log_line("aftermath: no markers in the dump (on D3D11 the driver takes them only while NVIDIA's Aftermath monitor "
                 "runs)");
    }
    uint32_t shaders = 0;
    if (api.shaderCount(decoder, &shaders) == GFSDK_Aftermath_Result_Success)
        logf("aftermath: %u shaders active at the fault", shaders);
    if (shaders) {
        GFSDK_Aftermath_GpuCrashDump_ShaderInfo info[16]{};
        const uint32_t take = shaders < 16 ? shaders : 16;
        if (api.shaderInfo(decoder, take, info) == GFSDK_Aftermath_Result_Success)
            for (uint32_t i = 0; i < take; ++i)
                logf("aftermath:   shader %016llx, type %u%s", static_cast<unsigned long long>(info[i].shaderHash),
                     static_cast<unsigned>(info[i].shaderType), info[i].isInternal ? ", the driver's own" : "");
    }
    uint32_t jsonBytes = 0;
    if (api.generateJson(decoder, GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO,
                         GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE, nullptr, nullptr, nullptr, nullptr,
                         &jsonBytes) == GFSDK_Aftermath_Result_Success &&
        jsonBytes) {
        std::string json(jsonBytes, '\0');
        if (api.getJson(decoder, jsonBytes, json.data()) == GFSDK_Aftermath_Result_Success) {
            if (!json.empty() && json.back() == '\0')
                json.pop_back();
            write_file(base + L".json", json.data(), json.size());
            logf("aftermath: decoded as %ls.json", base.c_str());
        }
    }
    api.destroyDecoder(decoder);
}

}  // namespace

bool built() { return true; }

bool enable_dumps() {
    if (dumpsOn)
        return true;
    if (!load_library())
        return false;
    const GFSDK_Aftermath_Result r = api.enableDumps(GFSDK_Aftermath_Version_API, GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_DX,
                                                     GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default, &on_dump, nullptr,
                                                     nullptr, nullptr, nullptr);
    dumpsOn = r == GFSDK_Aftermath_Result_Success;
    logf("aftermath: GPU crash dumps %s (%s, 0x%08x)", dumpsOn ? "on" : "OFF", result_text(r), static_cast<unsigned>(r));
    return dumpsOn;
}

bool attach(ID3D11Device* device, ID3D11DeviceContext* immediate, bool shaderErrors) {
    if (!loaded || context || !device || !immediate)
        return context != nullptr;
    // Markers only by default: the shader error reporting changes how the GPU runs shaders, and the hang under
    // study needs the GPU's normal timing (two runs with it on didn't hang, 2026-11-17).
    const uint32_t flags = GFSDK_Aftermath_FeatureFlags_EnableMarkers |
                           (shaderErrors ? GFSDK_Aftermath_FeatureFlags_EnableShaderErrorReporting : 0);
    const GFSDK_Aftermath_Result r = api.dx11Initialize(GFSDK_Aftermath_Version_API, flags, device);
    if (r != GFSDK_Aftermath_Result_Success && r != GFSDK_Aftermath_Result_FAIL_AlreadyInitialized) {
        logf("aftermath: not on the game's device (%s, 0x%08x)", result_text(r), static_cast<unsigned>(r));
        return false;
    }
    const GFSDK_Aftermath_Result c = api.createContext(immediate, &context);
    if (c != GFSDK_Aftermath_Result_Success) {
        logf("aftermath: no context handle (0x%08x)", static_cast<unsigned>(c));
        context = nullptr;
        return false;
    }
    markers = true;
    logf("aftermath: on the game's device, markers on its immediate context%s (they reach the dump only while NVIDIA's "
         "Aftermath monitor runs)", shaderErrors ? ", shader error reporting on" : "");
    return true;
}

void mark(uint64_t code) {
    if (markers.load(std::memory_order_relaxed))
        api.setMarker(context, reinterpret_cast<const void*>(code), 0);
}

bool marking() { return markers.load(std::memory_order_relaxed); }

void set_marker_names(void (*name)(uint64_t, char*, unsigned)) { namer = name; }

void after_removal(unsigned waitMs) {
    if (!loaded)
        return;
    GFSDK_Aftermath_Device_Status status = GFSDK_Aftermath_Device_Status_Unknown;
    if (context && api.deviceStatus(&status) == GFSDK_Aftermath_Result_Success)
        logf("aftermath: the device's state: %s", status_text(status));
    if (context) {
        GFSDK_Aftermath_ContextData data{};
        const GFSDK_Aftermath_Result r = api.getData(1, &context, &data);
        if (r == GFSDK_Aftermath_Result_Success)
            logf("aftermath: the last marker the GPU reached on the game's context: %s (context status %u)",
                 data.markerData ? name_of(reinterpret_cast<uint64_t>(data.markerData)).c_str() : "none",
                 static_cast<unsigned>(data.status));
        else
            logf("aftermath: no marker data (0x%08x)", static_cast<unsigned>(r));
    }
    if (!dumpsOn)
        return;
    const ULONGLONG start = GetTickCount64();
    GFSDK_Aftermath_CrashDump_Status s = GFSDK_Aftermath_CrashDump_Status_Unknown;
    while (GetTickCount64() - start < waitMs) {
        api.dumpStatus(&s);
        if (s == GFSDK_Aftermath_CrashDump_Status_Finished || s == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed)
            break;
        Sleep(50);
    }
    logf("aftermath: the crash dump's state after %.1f s: %u (4 = written, 2 = failed)", (GetTickCount64() - start) / 1000.0,
         static_cast<unsigned>(s));
}

void detach() {
    markers = false;
    if (context) {
        api.releaseContext(context);
        context = nullptr;
    }
}

#endif

}  // namespace simqvfr::aftermath

// The NVIDIA GPUs in this machine and NVAPI's loader (nvapi_gpus.h). The only file that includes nvapi_interface.h
// (its table has external linkage).
#include "engine/nvapi_gpus.h"

#include <cstring>
#include <mutex>

#include <windows.h>

#pragma warning(push, 0)
#include "../third_party/nvapi/nvapi.h"
#include "../third_party/nvapi/nvapi_interface.h"
#pragma warning(pop)

namespace simqvfr::nvapi {

namespace {

struct Loader {
    void* (*queryInterface)(unsigned int) = nullptr;
    int status = NVAPI_LIBRARY_NOT_FOUND;
};

const Loader& loader() {
    static Loader l;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE dll = LoadLibraryW(L"nvapi64.dll");  // the driver's; kept loaded
        if (!dll)
            return;
        auto queryInterface = reinterpret_cast<void* (*)(unsigned int)>(GetProcAddress(dll, "nvapi_QueryInterface"));
        if (!queryInterface) {
            l.status = NVAPI_NO_IMPLEMENTATION;
            return;
        }
        void* initialize = nullptr;
        for (const NVAPI_INTERFACE_TABLE& entry : nvapi_interface_table)
            if (std::strcmp(entry.func, "NvAPI_Initialize") == 0)
                initialize = queryInterface(entry.id);
        l.status = initialize ? reinterpret_cast<decltype(&NvAPI_Initialize)>(initialize)() : NVAPI_NO_IMPLEMENTATION;
        if (l.status == NVAPI_OK)
            l.queryInterface = queryInterface;
    });
    return l;
}

std::vector<Gpu> ask() {
    std::vector<Gpu> out;
    auto enumerate = reinterpret_cast<decltype(&NvAPI_EnumPhysicalGPUs)>(function("NvAPI_EnumPhysicalGPUs"));
    auto fullName = reinterpret_cast<decltype(&NvAPI_GPU_GetFullName)>(function("NvAPI_GPU_GetFullName"));
    auto archInfo = reinterpret_cast<decltype(&NvAPI_GPU_GetArchInfo)>(function("NvAPI_GPU_GetArchInfo"));
    if (!enumerate || !archInfo)
        return out;
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
    NvU32 count = 0;
    if (enumerate(handles, &count) != NVAPI_OK)
        return out;
    for (NvU32 i = 0; i < count && i < NVAPI_MAX_PHYSICAL_GPUS; ++i) {
        Gpu gpu;
        NvAPI_ShortString name = {};
        if (fullName && fullName(handles[i], name) == NVAPI_OK)
            gpu.name = name;
        NV_GPU_ARCH_INFO arch = {};
        arch.version = NV_GPU_ARCH_INFO_VER;
        if (archInfo(handles[i], &arch) == NVAPI_OK) {
            gpu.architecture = arch.architecture;
            gpu.implementation = arch.implementation;
        }
        gpu.twoViews = offers_multi_view(gpu.architecture, gpu.implementation, gpu.name);
        out.push_back(gpu);
    }
    return out;
}

}  // namespace

void* function(const char* name, int* status) {
    const Loader& l = loader();
    if (status)
        *status = l.status;
    if (!l.queryInterface || !name)
        return nullptr;
    for (const NVAPI_INTERFACE_TABLE& entry : nvapi_interface_table)
        if (std::strcmp(entry.func, name) == 0)
            return l.queryInterface(entry.id);
    return nullptr;
}

bool has_two_views(unsigned architecture) {
    return architecture >= NV_GPU_ARCHITECTURE_TU100 && architecture < 0xE0000000u;
}

bool offers_multi_view(unsigned architecture, unsigned implementation, const std::string& name) {
    if (!has_two_views(architecture))
        return false;
    if (architecture == NV_GPU_ARCHITECTURE_TU100 &&
        (implementation == NV_GPU_ARCH_IMPLEMENTATION_TU116 || implementation == NV_GPU_ARCH_IMPLEMENTATION_TU117))
        return false;  // GTX 16 series
    return name.find("GTX") == std::string::npos;
}

const std::vector<Gpu>& gpus() {
    static std::vector<Gpu> found;
    static std::once_flag once;
    std::call_once(once, [] { found = ask(); });
    return found;
}

const Gpu* two_views_gpu() {
    for (const Gpu& gpu : gpus())
        if (gpu.twoViews)
            return &gpu;
    return nullptr;
}

}  // namespace simqvfr::nvapi

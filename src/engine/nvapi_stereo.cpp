// NVIDIA single-pass stereo (nvapi_stereo.h).
#include "engine/nvapi_stereo.h"

#include <cstring>
#include <mutex>

#include <windows.h>

#pragma warning(push, 0)
#include "../third_party/nvapi/nvapi.h"
#include "../third_party/nvapi/nvapi_interface.h"
#pragma warning(pop)

namespace simqvfr::nvapi {

namespace {

struct Api {
    bool ok = false;
    decltype(&NvAPI_D3D_QuerySinglePassStereoSupport) querySps = nullptr;
    decltype(&NvAPI_D3D_QueryMultiViewSupport) queryMultiView = nullptr;
    decltype(&NvAPI_D3D_SetSinglePassStereoMode) setSps = nullptr;
    decltype(&NvAPI_D3D11_CreateVertexShaderEx) createVs = nullptr;
    int initStatus = NVAPI_LIBRARY_NOT_FOUND;
};

void* function(void* (*queryInterface)(unsigned int), const char* name) {
    for (const NVAPI_INTERFACE_TABLE& entry : nvapi_interface_table)
        if (std::strcmp(entry.func, name) == 0)
            return queryInterface(entry.id);
    return nullptr;
}

Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE dll = LoadLibraryW(L"nvapi64.dll");
        if (!dll)
            return;
        auto queryInterface = reinterpret_cast<void* (*)(unsigned int)>(GetProcAddress(dll, "nvapi_QueryInterface"));
        if (!queryInterface) {
            a.initStatus = NVAPI_NO_IMPLEMENTATION;
            return;
        }
        auto initialize = reinterpret_cast<decltype(&NvAPI_Initialize)>(function(queryInterface, "NvAPI_Initialize"));
        a.initStatus = initialize ? initialize() : NVAPI_NO_IMPLEMENTATION;
        if (a.initStatus != NVAPI_OK)
            return;
        a.querySps = reinterpret_cast<decltype(a.querySps)>(function(queryInterface, "NvAPI_D3D_QuerySinglePassStereoSupport"));
        a.queryMultiView = reinterpret_cast<decltype(a.queryMultiView)>(function(queryInterface, "NvAPI_D3D_QueryMultiViewSupport"));
        a.setSps = reinterpret_cast<decltype(a.setSps)>(function(queryInterface, "NvAPI_D3D_SetSinglePassStereoMode"));
        a.createVs = reinterpret_cast<decltype(a.createVs)>(function(queryInterface, "NvAPI_D3D11_CreateVertexShaderEx"));
        a.ok = a.querySps && a.setSps && a.createVs;
    });
    return a;
}

}  // namespace

Support query(ID3D11Device* device) {
    Support s;
    Api& a = api();
    s.status = a.initStatus;
    s.loaded = a.initStatus == NVAPI_OK;
    if (!a.ok || !device)
        return s;
    NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS sps = {};
    sps.version = NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS_VER;
    s.status = a.querySps(device, &sps);
    if (s.status == NVAPI_OK) {
        s.singlePassStereo = sps.bSinglePassStereoSupported != 0;
        s.xyzw = sps.bSinglePassStereoXYZWSupported != 0;
    }
    if (a.queryMultiView) {
        NV_QUERY_MULTIVIEW_SUPPORT_PARAMS mv = {};
        mv.version = NV_QUERY_MULTIVIEW_SUPPORT_PARAMS_VER;
        if (a.queryMultiView(device, &mv) == NVAPI_OK)
            s.multiView = mv.bMultiViewSupported != 0;
    }
    return s;
}

int create_second_view_shader(ID3D11Device* device, const dxbc::SecondView& converted, ID3D11VertexShader** shader) {
    Api& a = api();
    if (!a.ok)
        return a.initStatus != NVAPI_OK ? a.initStatus : NVAPI_NO_IMPLEMENTATION;
    if (converted.bytecode.empty())
        return NVAPI_INVALID_ARGUMENT;
    NV_CUSTOM_SEMANTIC semantics[2] = {};
    NvU32 count = 0;
    const auto add = [&](NV_CUSTOM_SEMANTIC_TYPE type, const char* name, NvU32 reg, NvU32 mask) {
        NV_CUSTOM_SEMANTIC& s = semantics[count++];
        s.version = NV_CUSTOM_SEMANTIC_VERSION;
        s.NVCustomSemanticType = type;
        strncpy_s(s.NVCustomSemanticNameString, name, _TRUNCATE);
        s.RegisterSpecified = TRUE;
        s.RegisterNum = reg;
        s.RegisterMask = mask;
    };
    add(NV_XYZW_RIGHT_SEMANTIC, dxbc::kRightSemantic, converted.rightRegister, 0xf);
    if (converted.hasMask)
        add(NV_VIEWPORT_MASK_SEMANTIC, dxbc::kMaskSemantic, converted.maskRegister, 0x1);
    NvAPI_D3D11_CREATE_VERTEX_SHADER_EX ex = {};
    ex.version = NVAPI_D3D11_CREATEVERTEXSHADEREX_VERSION;
    ex.NumCustomSemantics = count;
    ex.pCustomSemantics = semantics;
    return a.createVs(device, converted.bytecode.data(), converted.bytecode.size(), nullptr, &ex, shader);
}

int set_two_views(ID3D11DeviceContext* context, bool on, bool independentMasks, unsigned targetOffset) {
    Api& a = api();
    if (!a.ok)
        return a.initStatus != NVAPI_OK ? a.initStatus : NVAPI_NO_IMPLEMENTATION;
    return a.setSps(context, on ? 2 : 1, on ? targetOffset : 0, on && independentMasks ? 1 : 0);
}

const char* status_name(int status) {
    switch (status) {
        case NVAPI_OK: return "NVAPI_OK";
        case NVAPI_ERROR: return "NVAPI_ERROR";
        case NVAPI_LIBRARY_NOT_FOUND: return "NVAPI_LIBRARY_NOT_FOUND";
        case NVAPI_NO_IMPLEMENTATION: return "NVAPI_NO_IMPLEMENTATION";
        case NVAPI_API_NOT_INITIALIZED: return "NVAPI_API_NOT_INITIALIZED";
        case NVAPI_INVALID_ARGUMENT: return "NVAPI_INVALID_ARGUMENT";
        case NVAPI_NVIDIA_DEVICE_NOT_FOUND: return "NVAPI_NVIDIA_DEVICE_NOT_FOUND";
        case NVAPI_NOT_SUPPORTED: return "NVAPI_NOT_SUPPORTED";
        case NVAPI_INCOMPATIBLE_STRUCT_VERSION: return "NVAPI_INCOMPATIBLE_STRUCT_VERSION";
        default: return "NvAPI_Status (see nvapi_lite_common.h)";
    }
}

}  // namespace simqvfr::nvapi

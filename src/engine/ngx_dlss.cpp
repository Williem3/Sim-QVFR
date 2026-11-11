#include "ngx_dlss.h"

#include <windows.h>
#include <d3d11.h>

#ifdef SIM_QVFR_NGX
#include <cctype>
#include <cstdio>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "../log.h"

namespace simqvfr::ngx {

struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
};

namespace {

// A project id of our own (a GUID, custom engine): Sim-QVFR has no NVIDIA application id.
constexpr const char* kProjectId = "5b1c0f1e-6a57-4c1b-9d0e-51a0c7f3d2a4";

bool started = false, tried = false;
std::string failure;
NVSDK_NGX_Parameter* params = nullptr;
// While a feature is made: the tag NGX's lines give its mode ("(DLAA)", "(Perf)"), so its line on the preset it took
// for that mode is logged (it writes one for every mode).
const char* creatingMode = nullptr;

// NGX's own lines into our log: its errors and warnings, and the preset it took for a feature being made (it writes
// hundreds of lines a feature otherwise).
void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string line = message ? message : "";
    std::string lower = line;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (const size_t at = creatingMode ? line.find(creatingMode) : std::string::npos;
        at != std::string::npos &&
        (lower.find("using app hint preset") != std::string::npos || lower.find("using title default preset") != std::string::npos)) {
        std::string preset = line.substr(at);
        while (!preset.empty() && (preset.back() == '\n' || preset.back() == '\r' || preset.back() == ' '))
            preset.pop_back();
        simqvfr::logf("ngx: %s", preset.c_str());
        return;
    }
    if (lower.find("error") == std::string::npos && lower.find("warn") == std::string::npos && lower.find("fail") == std::string::npos)
        return;
    // Not failures: its first two guesses for its core before the driver's own, and model downloads it has no copy of.
    if (lower.find("failed to load ngxcore") != std::string::npos || lower.find("from cache") != std::string::npos)
        return;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    simqvfr::logf("ngx: %s", line.c_str());
}

}  // namespace

bool built_in() { return true; }

bool start(ID3D11Device* device, const wchar_t* dllFolder, const wchar_t* dataFolder, std::string& why) {
    if (tried) {
        why = failure;
        return started;
    }
    tried = true;
    CreateDirectoryW(dataFolder, nullptr);
    const wchar_t* paths[] = {dllFolder};
    NVSDK_NGX_FeatureCommonInfo info{};
    if (dllFolder && *dllFolder) {
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
    }
    info.LoggingInfo.LoggingCallback = ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    char text[160];
    NVSDK_NGX_Result r =
        NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataFolder, device, &info);
    if (NVSDK_NGX_FAILED(r)) {
        std::snprintf(text, sizeof(text), "NGX didn't start (0x%08x)", static_cast<unsigned>(r));
        why = failure = text;
        return false;
    }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
    int available = 0, needsDriver = 0, initResult = 0;
    if (NVSDK_NGX_SUCCEED(r) && params) {
        params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
    }
    if (!available) {
        std::snprintf(text, sizeof(text), "DLSS isn't available (parameters 0x%08x, feature 0x%08x%s): nvngx_dlss.dll missing?",
                      static_cast<unsigned>(r), static_cast<unsigned>(initResult), needsDriver ? ", a newer driver needed" : "");
        why = failure = text;
        NVSDK_NGX_D3D11_Shutdown1(device);
        params = nullptr;
        return false;
    }
    started = true;
    return true;
}

Feature* create(ID3D11DeviceContext* context, unsigned renderWidth, unsigned renderHeight, unsigned outputWidth,
                unsigned outputHeight, bool depthInverted, int preset, unsigned& result) {
    result = static_cast<unsigned>(NVSDK_NGX_Result_Fail);
    if (!started || !params || !renderWidth || !renderHeight || renderWidth > outputWidth || renderHeight > outputHeight)
        return nullptr;
    NVSDK_NGX_DLSS_Create_Params c{};
    c.Feature.InWidth = renderWidth;
    c.Feature.InHeight = renderHeight;
    c.Feature.InTargetWidth = outputWidth;
    c.Feature.InTargetHeight = outputHeight;
    // The mode nearest the ratio (it names the preset NGX picks; the sizes are what it works at). NGX refuses its
    // UltraQuality mode for super resolution (0xbad00010, unsupported: LMU run 2026-11-11 00:08 and ngx_dlss_check),
    // so a render above quality's 67% (Sim-QVFR's "ultra quality", 77%) runs in the quality mode, which takes it.
    const double ratio = static_cast<double>(renderWidth) / outputWidth;
    c.Feature.InPerfQualityValue = renderWidth == outputWidth && renderHeight == outputHeight ? NVSDK_NGX_PerfQuality_Value_DLAA
                                   : ratio >= 0.62 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                   : ratio >= 0.54 ? NVSDK_NGX_PerfQuality_Value_Balanced
                                   : ratio >= 0.42 ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                   : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    c.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                             NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                             (depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
        NVSDK_NGX_Parameter_SetUI(params, hint, static_cast<unsigned>(preset));
    switch (c.Feature.InPerfQualityValue) {
    case NVSDK_NGX_PerfQuality_Value_DLAA: creatingMode = "(DLAA)"; break;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality: creatingMode = "(UltraQuality)"; break;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality: creatingMode = "(Quality)"; break;
    case NVSDK_NGX_PerfQuality_Value_Balanced: creatingMode = "(Balanced)"; break;
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: creatingMode = "(Perf)"; break;
    default: creatingMode = "(UltraPerf)"; break;
    }
    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context, &handle, params, &c);
    creatingMode = nullptr;
    result = static_cast<unsigned>(r);
    if (NVSDK_NGX_FAILED(r) || !handle)
        return nullptr;
    auto* f = new Feature;
    f->handle = handle;
    return f;
}

void release(Feature* feature) {
    if (!feature)
        return;
    if (feature->handle)
        NVSDK_NGX_D3D11_ReleaseFeature(feature->handle);
    delete feature;
}

bool evaluate(ID3D11DeviceContext* context, Feature* feature, const Evaluation& v, unsigned& result) {
    result = static_cast<unsigned>(NVSDK_NGX_Result_Fail);
    if (!started || !params || !feature || !feature->handle)
        return false;
    NVSDK_NGX_D3D11_DLSS_Eval_Params e{};
    e.Feature.pInColor = v.colour;
    e.Feature.pInOutput = v.output;
    e.pInDepth = v.depth;
    e.pInMotionVectors = v.vectors;
    e.InJitterOffsetX = v.jitterX;
    e.InJitterOffsetY = v.jitterY;
    e.InRenderSubrectDimensions = {v.width, v.height};
    e.InColorSubrectBase = {v.colourX, v.colourY};
    e.InReset = v.reset ? 1 : 0;
    e.InMVScaleX = e.InMVScaleY = 1;
    e.InPreExposure = e.InExposureScale = 1;
    const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(context, feature->handle, params, &e);
    result = static_cast<unsigned>(r);
    return NVSDK_NGX_SUCCEED(r);
}

void shutdown(ID3D11Device* device) {
    if (started)
        NVSDK_NGX_D3D11_Shutdown1(device);
    started = tried = false;  // a later session starts it again
    failure.clear();
    params = nullptr;
}

}  // namespace simqvfr::ngx

#else  // no NGX SDK in this build

namespace simqvfr::ngx {

struct Feature {};

bool built_in() { return false; }
bool start(ID3D11Device*, const wchar_t*, const wchar_t*, std::string& why) {
    why = "this build has no NGX SDK (CMake: NGX_SDK_DIR)";
    return false;
}
Feature* create(ID3D11DeviceContext*, unsigned, unsigned, unsigned, unsigned, bool, int, unsigned& result) {
    result = 0;
    return nullptr;
}
void release(Feature*) {}
bool evaluate(ID3D11DeviceContext*, Feature*, const Evaluation&, unsigned& result) {
    result = 0;
    return false;
}
void shutdown(ID3D11Device*) {}

}  // namespace simqvfr::ngx

#endif

#include "focus_ngx.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#include <MinHook.h>

#include "../log.h"

#if defined(SIM_QVFR_NGX)
#include <nvsdk_ngx_params.h>
#endif

namespace simqvfr::focus_ngx {

#if !defined(SIM_QVFR_NGX)

// Built without NVIDIA's NGX headers: no focus DLSS.
bool install() { return false; }
bool installed() { return false; }
void preload() {}
bool latest(int, EyeEval&) { return false; }
CreateInfo create_info() { return {}; }
uint64_t game_evaluations() { return 0; }
struct Feature {};
Feature* make(ID3D12GraphicsCommandList*, unsigned, unsigned, int) { return nullptr; }
void release(Feature*) {}
bool evaluate(ID3D12GraphicsCommandList*, Feature*, const Inputs&) { return false; }

#else

namespace {

using Bag = NVSDK_NGX_Parameter;
using CreateFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, Bag*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const Bag*, void*);
using AllocateFn = NVSDK_NGX_Result (*)(Bag**);
using DestroyFn = NVSDK_NGX_Result (*)(Bag*);
using ReleaseFn = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);

CreateFn createOriginal = nullptr;
EvaluateFn evaluateOriginal = nullptr;
AllocateFn allocateFn = nullptr;
DestroyFn destroyFn = nullptr;
ReleaseFn releaseFn = nullptr;
std::atomic<bool> hooked{false};
bool tried = false;
SRWLOCK lock = SRWLOCK_INIT;
CreateInfo made;
EyeEval eyes[2];
std::atomic<uint64_t> evaluations{0};
int logged = 0;  // whole evaluations logged
std::atomic<int> eyePreset{0}, focusPreset{0};  // set_presets: 0 the game's own

const char* const kPresetKeys[6] = {
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};

bool ok(NVSDK_NGX_Result r) { return (static_cast<unsigned>(r) & 0xfff00000u) != 0xbad00000u; }

std::string name_of(ID3D12Resource* r) {
    if (!r)
        return "-";
    wchar_t name[96] = {};
    UINT size = sizeof(name) - sizeof(wchar_t);
    if (FAILED(r->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)) || !name[0])
        return "unnamed";
    std::string out;
    for (const wchar_t* c = name; *c; ++c)
        out += (std::iswalnum(*c) || *c == L'_' || *c == L'.') ? static_cast<char>(*c) : '_';
    return out;
}

std::string describe(ID3D12Resource* r) {
    if (!r)
        return "-";
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s %llux%u fmt %d x%u", name_of(r).c_str(), static_cast<unsigned long long>(d.Width),
                  d.Height, static_cast<int>(d.Format), d.SampleDesc.Count);
    return buf;
}

ID3D12Resource* get_res(const Bag* b, const char* k) {
    ID3D12Resource* r = nullptr;
    return ok(b->Get(k, &r)) ? r : nullptr;
}
unsigned get_ui(const Bag* b, const char* k) {
    unsigned v = 0;
    return ok(b->Get(k, &v)) ? v : 0;
}
int get_i(const Bag* b, const char* k, bool* has = nullptr) {
    int v = 0;
    const bool h = ok(b->Get(k, &v));
    if (has)
        *has = h;
    return h ? v : 0;
}
float get_f(const Bag* b, const char* k, bool* has = nullptr) {
    float v = 0;
    const bool h = ok(b->Get(k, &v));
    if (has)
        *has = h;
    return h ? v : 0;
}

NVSDK_NGX_Result createHook(ID3D12GraphicsCommandList* l, NVSDK_NGX_Feature feature, Bag* bag, NVSDK_NGX_Handle** out) {
    if (feature == NVSDK_NGX_Feature_SuperSampling && bag) {
        CreateInfo c;
        c.seen = true;
        c.width = get_ui(bag, NVSDK_NGX_Parameter_Width);
        c.height = get_ui(bag, NVSDK_NGX_Parameter_Height);
        c.outWidth = get_ui(bag, NVSDK_NGX_Parameter_OutWidth);
        c.outHeight = get_ui(bag, NVSDK_NGX_Parameter_OutHeight);
        c.quality = get_i(bag, NVSDK_NGX_Parameter_PerfQualityValue);
        c.flags = get_i(bag, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
        for (int i = 0; i < 6; ++i) {
            unsigned v = 0;
            c.presetSet[i] = ok(bag->Get(kPresetKeys[i], &v));
            c.presets[i] = v;
        }
        AcquireSRWLockExclusive(&lock);
        made = c;
        ReleaseSRWLockExclusive(&lock);
        logf("focus dlss: the game made a DLSS feature: render %ux%u, output %ux%u, quality %d, create flags 0x%x, "
             "presets DLAA %u Q %u B %u P %u UP %u UQ %u",
             c.width, c.height, c.outWidth, c.outHeight, c.quality, c.flags, c.presets[0], c.presets[1], c.presets[2],
             c.presets[3], c.presets[4], c.presets[5]);
        if (c.width != c.outWidth || c.height != c.outHeight)
            logf("focus dlss: the game's mode upscales its eyes (%ux%u -> %ux%u): the periphery is not DLAA in this mode; the "
                 "focus views are drawn from its render-size targets",
                 c.width, c.height, c.outWidth, c.outHeight);
        // The eyes' (the periphery's) preset override: the game's feature made with ours for every mode (the one change
        // to the game's NGX calls; `made` keeps the game's own for our focus features).
        if (const int preset = eyePreset.load(); preset > 0) {
            for (const char* key : kPresetKeys)
                bag->Set(key, static_cast<unsigned>(preset));
            logf("focus dlss: the game's feature (the eyes, the periphery) made with preset %d instead of its own", preset);
        }
    }
    return createOriginal(l, feature, bag, out);
}

NVSDK_NGX_Result evaluateHook(ID3D12GraphicsCommandList* l, const NVSDK_NGX_Handle* h, const Bag* bag, void* callback) {
    if (bag) {
        EyeEval e;
        e.color = get_res(bag, NVSDK_NGX_Parameter_Color);
        e.motion = get_res(bag, NVSDK_NGX_Parameter_MotionVectors);
        if (e.color && e.motion) {  // a DLSS (SuperSampling) evaluation
            e.depth = get_res(bag, NVSDK_NGX_Parameter_Depth);
            e.output = get_res(bag, NVSDK_NGX_Parameter_Output);
            e.exposure = get_res(bag, NVSDK_NGX_Parameter_ExposureTexture);
            e.transparency = get_res(bag, NVSDK_NGX_Parameter_TransparencyMask);
            e.bias = get_res(bag, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
            e.colorName = name_of(e.color);
            e.depthName = name_of(e.depth);
            e.motionName = name_of(e.motion);
            e.outputName = name_of(e.output);
            e.exposureName = name_of(e.exposure);
            e.jitter[0] = get_f(bag, NVSDK_NGX_Parameter_Jitter_Offset_X);
            e.jitter[1] = get_f(bag, NVSDK_NGX_Parameter_Jitter_Offset_Y);
            bool has = false;
            e.motionScale[0] = get_f(bag, NVSDK_NGX_Parameter_MV_Scale_X, &has);
            if (!has)
                e.motionScale[0] = 1;
            e.motionScale[1] = get_f(bag, NVSDK_NGX_Parameter_MV_Scale_Y, &has);
            if (!has)
                e.motionScale[1] = 1;
            e.preExposure = get_f(bag, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &e.hasPreExposure);
            e.exposureScale = get_f(bag, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &e.hasExposureScale);
            e.sharpness = get_f(bag, NVSDK_NGX_Parameter_Sharpness);
            e.frameTime = get_f(bag, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &e.hasFrameTime);
            e.reset = get_i(bag, NVSDK_NGX_Parameter_Reset);
            e.renderW = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
            e.renderH = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height);
            e.colorBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X);
            e.colorBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
            e.depthBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
            e.depthBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
            e.motionBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);
            e.motionBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
            e.outputBase[0] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
            e.outputBase[1] = get_ui(bag, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
            const uint64_t n = evaluations.fetch_add(1);
            e.sequence = n + 1;
            // The right eye: the colour subrect starts past x 0 (one double-wide target); with separate targets per eye
            // (both at 0) the order in the frame decides.
            const int eye = e.colorBase[0] > 0 ? 1 : (e.colorBase[0] == 0 && e.outputBase[0] > 0 ? 1 : 0);
            bool logIt = false;
            AcquireSRWLockExclusive(&lock);
            eyes[eye] = e;
            if (logged < 4) {
                ++logged;
                logIt = true;
            }
            ReleaseSRWLockExclusive(&lock);
            if (logIt) {
                logf("focus dlss: game evaluation %llu (eye %d): colour %s base %u,%u; depth %s base %u,%u; motion %s base "
                     "%u,%u scale %.4g,%.4g; output %s base %u,%u; render %ux%u",
                     static_cast<unsigned long long>(n), eye, describe(e.color).c_str(), e.colorBase[0], e.colorBase[1],
                     describe(e.depth).c_str(), e.depthBase[0], e.depthBase[1], describe(e.motion).c_str(),
                     e.motionBase[0], e.motionBase[1], e.motionScale[0], e.motionScale[1], describe(e.output).c_str(),
                     e.outputBase[0], e.outputBase[1], e.renderW, e.renderH);
                logf("focus dlss:   jitter %.4f,%.4f; exposure texture %s; pre-exposure %s%.4g; exposure scale %s%.4g; "
                     "sharpness %.3g; frame time %s%.3g ms; reset %d; transparency mask %s; bias mask %s",
                     e.jitter[0], e.jitter[1], describe(e.exposure).c_str(), e.hasPreExposure ? "" : "(unset) ",
                     e.preExposure, e.hasExposureScale ? "" : "(unset) ", e.exposureScale, e.sharpness,
                     e.hasFrameTime ? "" : "(unset) ", e.frameTime, e.reset, describe(e.transparency).c_str(),
                     describe(e.bias).c_str());
            }
        }
    }
    return evaluateOriginal(l, h, bag, callback);
}

}  // namespace

struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    Bag* bag = nullptr;
};

bool installed() { return hooked.load(); }

bool install() {
    if (hooked.load())
        return true;
    HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
    if (!ngx)
        return false;
    AcquireSRWLockExclusive(&lock);
    if (tried) {
        ReleaseSRWLockExclusive(&lock);
        return hooked.load();
    }
    tried = true;
    ReleaseSRWLockExclusive(&lock);
    void* create = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
    void* evaluate = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    allocateFn = reinterpret_cast<AllocateFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
    destroyFn = reinterpret_cast<DestroyFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_DestroyParameters"));
    releaseFn = reinterpret_cast<ReleaseFn>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!create || !evaluate || !allocateFn || !destroyFn || !releaseFn) {
        log_line("focus dlss: _nvngx.dll lacks a D3D12 export (create / evaluate / parameters / release); no focus DLSS");
        return false;
    }
    if (const MH_STATUS init = MH_Initialize(); init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        logf("focus dlss: MinHook: %s; no focus DLSS", MH_StatusToString(init));
        return false;
    }
    MH_STATUS s = MH_CreateHook(create, reinterpret_cast<void*>(&createHook), reinterpret_cast<void**>(&createOriginal));
    if (s == MH_OK) {
        s = MH_CreateHook(evaluate, reinterpret_cast<void*>(&evaluateHook), reinterpret_cast<void**>(&evaluateOriginal));
        if (s != MH_OK)
            MH_RemoveHook(create);
    }
    if (s == MH_OK && (s = MH_QueueEnableHook(create)) == MH_OK && (s = MH_QueueEnableHook(evaluate)) == MH_OK)
        s = MH_ApplyQueued();
    if (s != MH_OK) {
        MH_RemoveHook(create);
        MH_RemoveHook(evaluate);
        logf("focus dlss: could not detour NGX's exports (%s); no focus DLSS", MH_StatusToString(s));
        return false;
    }
    hooked = true;
    log_line("focus dlss: NGX's D3D12 create / evaluate exports detoured (_nvngx.dll)");
    return true;
}

void preload() {
    if (hooked.load() || GetModuleHandleW(L"_nvngx.dll")) {
        install();
        return;
    }
    wchar_t folder[MAX_PATH] = {};
    DWORD size = sizeof(folder);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                     nullptr, folder, &size) != ERROR_SUCCESS) {
        log_line("focus dlss: no NGX core path registered by the driver; detours wait for the game's own load");
        return;
    }
    const std::wstring path = std::wstring(folder) + L"\\_nvngx.dll";
    if (!LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        logf("focus dlss: NGX core not loaded early (error %lu); detours wait for the game's own load", GetLastError());
        return;
    }
    if (install())
        log_line("focus dlss: NGX core loaded early from the driver's folder; the game's DLSS creation will be seen");
}

bool latest(int eye, EyeEval& out) {
    if (eye < 0 || eye > 1)
        return false;
    AcquireSRWLockShared(&lock);
    out = eyes[eye];
    ReleaseSRWLockShared(&lock);
    return out.sequence != 0;
}

CreateInfo create_info() {
    AcquireSRWLockShared(&lock);
    const CreateInfo c = made;
    ReleaseSRWLockShared(&lock);
    return c;
}

uint64_t game_evaluations() { return evaluations.load(); }

void set_presets(int eye, int focus) {
    eyePreset.store(eye);
    focusPreset.store(focus);
}

Feature* make(ID3D12GraphicsCommandList* list, unsigned width, unsigned height, int extraFlags) {
    if (!hooked.load())
        return nullptr;
    const CreateInfo c = create_info();
    auto* f = new Feature;
    NVSDK_NGX_Result r = allocateFn(&f->bag);
    if (ok(r) && f->bag) {
        Bag* b = f->bag;
        b->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        b->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        b->Set(NVSDK_NGX_Parameter_Width, width);
        b->Set(NVSDK_NGX_Parameter_Height, height);
        b->Set(NVSDK_NGX_Parameter_OutWidth, width);
        b->Set(NVSDK_NGX_Parameter_OutHeight, height);
        // DLAA (render = output); the game's flags when its creation was seen, else HDR colour, reverse depth and
        // motion vectors at render size (ACE's targets: R11G11B10 linear, reverse-Z).
        b->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));
        const int flags = (c.seen ? c.flags
                                  : (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                     NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)) |
                          extraFlags;
        b->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
        b->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
        const int ownPreset = focusPreset.load();  // the focus views' preset override (0: the game's presets)
        for (int i = 0; i < 6; ++i) {
            if (ownPreset > 0)
                b->Set(kPresetKeys[i], static_cast<unsigned>(ownPreset));
            else if (c.presetSet[i])
                b->Set(kPresetKeys[i], c.presets[i]);
        }
        r = createOriginal(list, NVSDK_NGX_Feature_SuperSampling, b, &f->handle);
        logf("focus dlss: our feature %ux%u (DLAA, flags 0x%x, preset %s%d, the game's creation %s): result 0x%08x", width,
             height, flags, ownPreset > 0 ? "ours " : "the game's ", ownPreset > 0 ? ownPreset : static_cast<int>(c.presets[0]),
             c.seen ? "seen" : "not seen", static_cast<unsigned>(r));
    } else {
        logf("focus dlss: NGX parameters not allocated (0x%08x)", static_cast<unsigned>(r));
    }
    if (!ok(r) || !f->handle) {
        release(f);
        return nullptr;
    }
    return f;
}

void release(Feature* f) {
    if (!f)
        return;
    if (f->handle && releaseFn)
        releaseFn(f->handle);
    if (f->bag && destroyFn)
        destroyFn(f->bag);
    delete f;
}

bool evaluate(ID3D12GraphicsCommandList* list, Feature* f, const Inputs& in) {
    if (!f || !f->bag || !f->handle)
        return false;
    Bag* b = f->bag;
    b->Set(NVSDK_NGX_Parameter_Color, in.color);
    b->Set(NVSDK_NGX_Parameter_Output, in.output);
    b->Set(NVSDK_NGX_Parameter_Depth, in.depth);
    b->Set(NVSDK_NGX_Parameter_MotionVectors, in.motion);
    b->Set(NVSDK_NGX_Parameter_ExposureTexture, in.exposure);
    b->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, in.jitter[0]);
    b->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, in.jitter[1]);
    b->Set(NVSDK_NGX_Parameter_MV_Scale_X, in.motionScale[0]);
    b->Set(NVSDK_NGX_Parameter_MV_Scale_Y, in.motionScale[1]);
    b->Set(NVSDK_NGX_Parameter_Reset, in.reset ? 1 : 0);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X})
        b->Set(k, in.base[0]);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                          NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y})
        b->Set(k, in.base[1]);
    b->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, in.width);
    b->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, in.height);
    if (in.game) {
        if (in.game->hasPreExposure)
            b->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, in.game->preExposure);
        if (in.game->hasExposureScale)
            b->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, in.game->exposureScale);
        b->Set(NVSDK_NGX_Parameter_Sharpness, in.game->sharpness);
        if (in.game->hasFrameTime)
            b->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, in.game->frameTime);
    }
    const NVSDK_NGX_Result r = evaluateOriginal(list, f->handle, b, nullptr);
    static std::atomic<int> failures{0};
    if (!ok(r) && failures.fetch_add(1) < 3)
        logf("focus dlss: our evaluation refused: 0x%08x", static_cast<unsigned>(r));
    return ok(r);
}

#endif

}  // namespace simqvfr::focus_ngx

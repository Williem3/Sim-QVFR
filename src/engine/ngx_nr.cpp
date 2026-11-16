#include "ngx_nr.h"

#include <windows.h>
#include <d3d11_4.h>

#ifdef SIM_QVFR_NGX
#include <cctype>
#include <cstdio>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#include "../log.h"
#include "d3d12_bridge.h"
#include "ngx_common.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr::ngxnr {

struct View : BridgeView {
    unsigned width = 0, height = 0;
    int preset = 0;                    // what it was asked for (a creation still on the GPU is matched by it)
    unsigned long long askedAt = 0;    // collect()'s count when create() last asked for it while it was being made
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
};

namespace {

// NGX feature 18: the neural rendering runtime (NVSDK_NGX_Feature_Reserved18 in the SDK's public header).
constexpr NVSDK_NGX_Feature kNeuralRendering = static_cast<NVSDK_NGX_Feature>(18);

void destroy(BridgeView* bv) {
    auto* v = static_cast<View*>(bv);
    if (v->handle)
        NVSDK_NGX_D3D12_ReleaseFeature(v->handle);
    if (v->params)
        NVSDK_NGX_D3D12_DestroyParameters(v->params);
    delete v;
}

D3D12Bridge bridge("ngx nr", "the model's output", destroy);

struct State {
    ComPtr<IDXGIAdapter3> adapter;
    std::vector<View*> creating;  // feature created, its creation list not through on the GPU yet (create)
} s;

// create() waits this long for the feature's creation list on the render thread; longer and the view is answered
// kCreating and handed over by a later create() once the GPU is through (a stalled GPU must not freeze the headset).
constexpr DWORD kCreateWaitMs = 500;
// A creation nobody asked for again in this many collect() calls (the size changed meanwhile) is given up.
constexpr unsigned long long kCreatingFrames = 120;

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string line = message ? message : "";
    if (!ngx_line_logged(ngx_lower(line)))
        return;
    ngx_trim_line_end(line);
    simqvfr::logf("ngx nr: %s", line.c_str());
}

}  // namespace

bool built_in() { return true; }

bool start(ID3D11Device* device, const wchar_t* dllFolder, const wchar_t* dataFolder, std::string& why) {
    if (bridge.tried) {
        why = bridge.failure;
        return bridge.started;
    }
    bridge.tried = true;
    if (!dllFolder || !*dllFolder) {
        why = bridge.failure = "no folder with nvngx_dlssnr.dll was chosen";
        return false;
    }
    if (GetFileAttributesW((std::wstring(dllFolder) + L"\\nvngx_dlssnr.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        why = bridge.failure = "nvngx_dlssnr.dll isn't in the folder chosen for it";
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (!bridge.open_game_device(device, adapter, why) || !bridge.create_device(adapter.Get(), why))
        return false;
    adapter.As(&s.adapter);
    CreateDirectoryW(dataFolder, nullptr);
    const wchar_t* paths[1] = {dllFolder};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    const NVSDK_NGX_Result r =
        NVSDK_NGX_D3D12_Init_with_ProjectID(kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataFolder, bridge.device(), &info);
    if (NVSDK_NGX_FAILED(r)) {
        ComPtr<ID3D12Device> keep = bridge.take_device();  // not NGX's to shut down: it never started
        shutdown(nullptr);
        bridge.tried = true;
        return bridge.fail(why, "NGX didn't start on Direct3D 12", static_cast<HRESULT>(r));
    }
    bridge.started = true;
    return true;
}

namespace {

// A created feature's evaluation inputs: set once its creation list is through.
View* finish(View* v) {
    const unsigned width = v->width, height = v->height;
    NVSDK_NGX_Parameter* p = v->params;
    p->Set("DLSSNR.Color", v->colour.Get());
    p->Set("DLSSNR.Output", v->output.Get());
    p->Set("DLSSNR.Depth", v->depth.Get());
    p->Set("DLSSNR.MVec", v->vectors.Get());
    for (const char* k : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
                          "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY", "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY"})
        p->Set(k, 0u);
    for (const char* k : {"DLSSNR.ColorSubrectWidth", "DLSSNR.OutputSubrectWidth", "DLSSNR.DepthSubrectWidth", "DLSSNR.MVecSubrectWidth"})
        p->Set(k, width);
    for (const char* k :
         {"DLSSNR.ColorSubrectHeight", "DLSSNR.OutputSubrectHeight", "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectHeight"})
        p->Set(k, height);
    // The vectors are fractions of the view: times its size, the texels the runtime works in.
    p->Set("DLSSNR.MVecScaleX", static_cast<float>(width));
    p->Set("DLSSNR.MVecScaleY", static_cast<float>(height));
    return v;
}

}  // namespace

View* create(unsigned width, unsigned height, int preset, unsigned& result) {
    result = static_cast<unsigned>(NVSDK_NGX_Result_Fail);
    if (!bridge.started || !width || !height)
        return nullptr;
    // A creation of this kind already on the GPU: handed over once through, never used before (no half-made view out).
    for (auto it = s.creating.begin(); it != s.creating.end(); ++it) {
        View* c = *it;
        if (c->width != width || c->height != height || c->preset != preset)
            continue;
        const UINT64 done = bridge.completed();
        if (done == UINT64_MAX) {  // a lost device: the creation is as good as failed
            s.creating.erase(it);
            bridge.discard(c);
            return nullptr;
        }
        if (done < c->lastSubmit) {
            c->askedAt = bridge.frames();
            result = kCreating;
            return nullptr;
        }
        s.creating.erase(it);
        result = static_cast<unsigned>(NVSDK_NGX_Result_Success);
        return finish(c);
    }
    auto* v = new View;
    v->width = width;
    v->height = height;
    v->preset = preset;
    HRESULT hr = S_OK;
    if (!bridge.make_view(v, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, hr)) {
        result = static_cast<unsigned>(hr);
        bridge.discard(v);
        return nullptr;
    }
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_AllocateParameters(&v->params);
    if (NVSDK_NGX_SUCCEED(r) && v->params) {
        NVSDK_NGX_Parameter* p = v->params;
        for (const char* k : {"Width", "OutWidth", "DLSSNR.Width"})
            p->Set(k, width);
        for (const char* k : {"Height", "OutHeight", "DLSSNR.Height"})
            p->Set(k, height);
        p->Set("DLSSNR.Enabled", 1u);
        p->Set("CreationNodeMask", 1u);
        p->Set("VisibilityNodeMask", 1u);
        p->Set("DLSSNR.Hint.Render.Preset", static_cast<unsigned>(preset < 0 ? 0 : preset));
        p->Set("DLSSNR.Intensity", 1.0f);
        p->Set("DLSSNR.LocalToneStrength", 1.0f);
        p->Set("DLSSNR.LocalStructureStrength", 1.0f);
        p->Set("DLSSNR.UseAutoMask", 0u);
        p->Set("DLSSNR.ControlMask", static_cast<void*>(nullptr));
        p->Set("DLSSNR.Style", 0u);
        r = NVSDK_NGX_Result_Fail;
        bool late = false;
        if (bridge.begin_list()) {
            r = NVSDK_NGX_D3D12_CreateFeature(bridge.list(), kNeuralRendering, p, &v->handle);
            const UINT64 at = bridge.end_list();
            v->lastSubmit = at ? at : bridge.last_value();
            if (NVSDK_NGX_SUCCEED(r) && !at)
                r = NVSDK_NGX_Result_Fail;
            else if (NVSDK_NGX_SUCCEED(r) && !bridge.wait_for(at, kCreateWaitMs))
                late = true;
        }
        if (late && v->handle) {
            simqvfr::logf("ngx nr: a %ux%u feature's creation isn't through on the GPU after %u ms: handed over on a later frame",
                          width, height, static_cast<unsigned>(kCreateWaitMs));
            v->askedAt = bridge.frames();
            s.creating.push_back(v);
            result = kCreating;
            return nullptr;
        }
    }
    result = static_cast<unsigned>(r);
    if (NVSDK_NGX_FAILED(r) || !v->handle) {
        bridge.discard(v);
        return nullptr;
    }
    return finish(v);
}

void release(ID3D11DeviceContext* context, View* v, IUnknown* const* keep, unsigned count) { bridge.release(context, v, keep, count); }

void collect() {
    const unsigned long long frame = bridge.count_frame();
    // Creations nobody asks for any more (the size or preset changed): released as any other view.
    for (auto it = s.creating.begin(); it != s.creating.end();) {
        if (frame >= (*it)->askedAt + kCreatingFrames) {
            bridge.discard(*it);
            it = s.creating.erase(it);
        } else {
            ++it;
        }
    }
    bridge.destroy_finished();
}

unsigned pending_releases() { return bridge.released() + static_cast<unsigned>(s.creating.size()); }

bool device_lost() { return bridge.device_lost(); }

void lose_device_for_tests() {
    ComPtr<ID3D12Device5> device5;
    if (bridge.device() && SUCCEEDED(bridge.device()->QueryInterface(IID_PPV_ARGS(&device5))))
        device5->RemoveDevice();
}

bool video_memory(double& usedMb, double& budgetMb) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!s.adapter || FAILED(s.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;
    usedMb = static_cast<double>(info.CurrentUsage) / 1048576.0;
    budgetMb = static_cast<double>(info.Budget) / 1048576.0;
    return true;
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
    result = static_cast<unsigned>(NVSDK_NGX_Result_Fail);
    if (!bridge.started || !context || !v || !v->handle)
        return false;
    D3D12Bridge::Evaluating ev;
    if (!bridge.begin_evaluation(context, v, kBacklog, kBehind, result, ev))
        return false;
    NVSDK_NGX_Parameter* p = v->params;
    p->Set("DLSSNR.Intensity", e.intensity);
    p->Set("DLSSNR.LocalToneStrength", e.localTone);
    p->Set("DLSSNR.LocalStructureStrength", e.localStructure);
    p->Set("DLSSNR.Style", e.style);
    p->Set("DLSSNR.UseAutoMask", e.autoMask ? 1u : 0u);
    p->Set("DLSSNR.DepthInverted", e.depthInverted ? 1u : 0u);
    p->Set("DLSSNR.Reset", e.reset ? 1u : 0u);
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_EvaluateFeature(bridge.list(), v->handle, p, nullptr);
    // Submitted whatever NGX answered.
    if (!bridge.end_evaluation(v, ev))
        return false;
    result = static_cast<unsigned>(r);
    return NVSDK_NGX_SUCCEED(r);
}

double take_gpu_ms(View* v) { return bridge.take_gpu_ms(v); }

void shutdown(ID3D11DeviceContext* context) {
    for (View* v : s.creating)  // creations still on the GPU: waited for and destroyed with the released views
        bridge.park(v);
    s.creating.clear();
    if (!bridge.shutdown(context, "DLSS-NR", "; DLSS-NR stays off until the game is restarted")) {
        (void)s.adapter.Detach();
        s = State{};
        // No restart in this process: the device still held would come back from D3D12CreateDevice (one a process and
        // adapter) and NGX would start a second time on a device it was never shut down on.
        bridge.tried = true;
        bridge.failure = "an earlier session's DLSS-NR work never finished on the GPU: off until the game is restarted";
        return;
    }
    if (bridge.started && bridge.device())
        NVSDK_NGX_D3D12_Shutdown1(bridge.device());
    bridge.reset();
    s = State{};
}

}  // namespace simqvfr::ngxnr

#else  // no NGX SDK in this build

namespace simqvfr::ngxnr {

struct View {};

bool built_in() { return false; }
bool start(ID3D11Device*, const wchar_t*, const wchar_t*, std::string& why) {
    why = "this build has no NGX SDK (CMake: NGX_SDK_DIR)";
    return false;
}
View* create(unsigned, unsigned, int, unsigned& result) {
    result = 0;
    return nullptr;
}
void release(ID3D11DeviceContext*, View*, IUnknown* const*, unsigned) {}
void collect() {}
unsigned pending_releases() { return 0; }
bool video_memory(double&, double&) { return false; }
bool device_lost() { return false; }
void lose_device_for_tests() {}
Textures textures(View*) { return {}; }
bool evaluate(ID3D11DeviceContext*, View*, const Evaluation&, unsigned& result) {
    result = 0;
    return false;
}
double take_gpu_ms(View*) { return 0; }
void shutdown(ID3D11DeviceContext*) {}

}  // namespace simqvfr::ngxnr

#endif

namespace simqvfr::ngxnr {

std::wstring loaded_dll() {
    const HMODULE dll = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (!dll)
        return {};
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(dll, path, MAX_PATH);
    return n == 0 || n == MAX_PATH ? std::wstring() : std::wstring(path, n);
}

}  // namespace simqvfr::ngxnr

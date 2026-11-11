// Hardware check of DLSS called by us through NGX on D3D11, outside any game: can a process that ships no DLSS of its
// own create DLSS features and evaluate them, and what do four views a frame cost? Not part of the test suite (no
// _test suffix): it needs an NVIDIA RTX GPU, the NGX SDK (CMake: NGX_SDK_DIR) and a folder holding nvngx_dlss.dll.
//
//   ngx_dlss_check.exe <folder with nvngx_dlss.dll> [frames]
//
// 1. NGX starts with a project id (custom engine) and reports DLSS as available.
// 2. Per case (render size -> output size, quality mode, preset): four features as QVFR would hold them (two periphery
//    views, two focus views), evaluated once each per frame; the GPU time per frame from timestamp queries and the CPU
//    time of the calls. Inputs: HDR colour (RGBA16F noise), depth (R32F), motion vectors (RG16F, zero).
// 3. Sub-rectangles: a view read from a region of a wider colour/depth/vector texture and written into a region of a
//    wider output (the multi-view layout: an eye and its focus view side by side in one target).
// Set NGX_LOG=1 for NGX's own log lines.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::printf("  ngx: %s", message);
}

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;

ComPtr<ID3D11Texture2D> texture(UINT w, UINT h, DXGI_FORMAT format, UINT bind, bool noise) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.Format = format;
    d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    if (!noise) {
        device->CreateTexture2D(&d, nullptr, &t);
        return t;
    }
    // Noise over a gradient: every texel differs from its neighbours (the worst case for nothing in particular, but
    // not a flat image either). RGBA16F or R32F only.
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    D3D11_SUBRESOURCE_DATA init{};
    std::vector<uint16_t> half;
    std::vector<float> flt;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        half.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < half.size(); i += 4) {
            float base = float(i / 4 % w) / w;
            for (int c = 0; c < 3; ++c) half[i + c] = DirectX::PackedVector::XMConvertFloatToHalf(base + rnd() * 0.5f);
            half[i + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
        }
        init.pSysMem = half.data(), init.SysMemPitch = w * 8;
    } else {
        flt.resize(size_t(w) * h);
        for (float& f : flt) f = 0.2f + rnd() * 0.7f;
        init.pSysMem = flt.data(), init.SysMemPitch = w * 4;
    }
    device->CreateTexture2D(&d, &init, &t);
    return t;
}

// The share of non-zero colour values on one row of a region: did the evaluation write there?
float written(ID3D11Texture2D* tex, UINT x, UINT y, UINT w) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Width = w, d.Height = 1, d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> row;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &row))) return -1;
    D3D11_BOX box{x, y, 0, x + w, y + 1, 1};
    context->CopySubresourceRegion(row.Get(), 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(row.Get(), 0, D3D11_MAP_READ, 0, &m))) return -1;
    auto* v = static_cast<const uint16_t*>(m.pData);
    UINT set = 0;
    for (UINT i = 0; i < w; ++i) set += (v[i * 4] & 0x7fff) || (v[i * 4 + 1] & 0x7fff) || (v[i * 4 + 2] & 0x7fff);
    context->Unmap(row.Get(), 0);
    return float(set) / w;
}

struct View {
    UINT rw, rh, ow, oh;  // render and output size
    NVSDK_NGX_Handle* handle = nullptr;
    ComPtr<ID3D11Texture2D> colour, depth, vectors, output;
};

struct Case {
    const char* name;
    NVSDK_NGX_PerfQuality_Value quality;
    int preset;
    View views[4];
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

const UINT kTargets = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

bool create(View& v, NVSDK_NGX_Parameter* params, NVSDK_NGX_PerfQuality_Value quality, int preset, bool outSubrects) {
    NVSDK_NGX_DLSS_Create_Params c{};
    c.Feature.InWidth = v.rw, c.Feature.InHeight = v.rh;
    c.Feature.InTargetWidth = v.ow, c.Feature.InTargetHeight = v.oh;
    c.Feature.InPerfQualityValue = quality;
    c.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    c.InEnableOutputSubrects = outSubrects;
    // Every mode's hint, as the module sets them (engine/ngx_dlss.cpp): the one for this feature's mode is the one used.
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
        NVSDK_NGX_Parameter_SetUI(params, hint, static_cast<unsigned>(preset));
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context.Get(), &v.handle, params, &c);
    if (NVSDK_NGX_FAILED(r)) {
        std::printf("  create %ux%u -> %ux%u: 0x%08x\n", v.rw, v.rh, v.ow, v.oh, unsigned(r));
        return false;
    }
    return true;
}

void run_case(Case& k, NVSDK_NGX_Parameter* params, int frames) {
    bool made = true;
    for (View& v : k.views) {
        made = made && create(v, params, k.quality, k.preset, false);
        v.colour = texture(v.rw, v.rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
        v.depth = texture(v.rw, v.rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
        v.vectors = texture(v.rw, v.rh, DXGI_FORMAT_R16G16_FLOAT, kTargets, false);
        v.output = texture(v.ow, v.oh, DXGI_FORMAT_R16G16B16A16_FLOAT, kTargets, false);
        const float zero[4]{};
        ComPtr<ID3D11RenderTargetView> rtv;
        device->CreateRenderTargetView(v.vectors.Get(), nullptr, &rtv);
        context->ClearRenderTargetView(rtv.Get(), zero);
    }
    std::string label = std::string(k.name) + ": four features made";
    check(made, label.c_str());
    if (made) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt{D3D11_QUERY_TIMESTAMP, 0};
        ComPtr<ID3D11Query> disjoint, t0, t1;
        device->CreateQuery(&qd, &disjoint), device->CreateQuery(&qt, &t0), device->CreateQuery(&qt, &t1);
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        std::vector<double> gpu, cpu;
        bool ok = true;
        for (int f = 0; f < frames; ++f) {
            context->Begin(disjoint.Get());
            context->End(t0.Get());
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            for (View& v : k.views) {
                NVSDK_NGX_D3D11_DLSS_Eval_Params e{};
                e.Feature.pInColor = v.colour.Get(), e.Feature.pInOutput = v.output.Get();
                e.pInDepth = v.depth.Get(), e.pInMotionVectors = v.vectors.Get();
                e.InJitterOffsetX = (f % 8) / 8.0f - 0.5f, e.InJitterOffsetY = (f % 3) / 3.0f - 0.5f;
                e.InRenderSubrectDimensions = {v.rw, v.rh};
                e.InReset = f == 0, e.InMVScaleX = 1, e.InMVScaleY = 1, e.InPreExposure = 1, e.InExposureScale = 1;
                ok = ok && NVSDK_NGX_SUCCEED(NGX_D3D11_EVALUATE_DLSS_EXT(context.Get(), v.handle, params, &e));
            }
            QueryPerformanceCounter(&b);
            context->End(t1.Get());
            context->End(disjoint.Get());
            context->Flush();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 s0 = 0, s1 = 0;
            while (context->GetData(disjoint.Get(), &dj, sizeof dj, 0) == S_FALSE) Sleep(0);
            while (context->GetData(t0.Get(), &s0, sizeof s0, 0) == S_FALSE) Sleep(0);
            while (context->GetData(t1.Get(), &s1, sizeof s1, 0) == S_FALSE) Sleep(0);
            if (f >= frames / 5 && !dj.Disjoint) {
                gpu.push_back(double(s1 - s0) * 1000.0 / double(dj.Frequency));
                cpu.push_back(double(b.QuadPart - a.QuadPart) * 1000.0 / double(freq.QuadPart));
            }
        }
        label = std::string(k.name) + ": every evaluation succeeded";
        check(ok, label.c_str());
        float w = written(k.views[2].output.Get(), 0, k.views[2].oh / 2, k.views[2].ow);
        label = std::string(k.name) + ": the output was written";
        check(w > 0.9f, label.c_str());
        std::printf("     %-34s GPU %.2f ms per frame (%.2f per view), CPU %.3f ms per frame, %zu frames\n", k.name,
                    median(gpu), median(gpu) / 4, median(cpu), gpu.size());
    }
    for (View& v : k.views) {
        if (v.handle) NVSDK_NGX_D3D11_ReleaseFeature(v.handle);
        v = View{v.rw, v.rh, v.ow, v.oh};
    }
}

// Two features of one kind (a pair of views: the eyes, or the focus views) timed alone: GPU ms per frame for the pair,
// median after a warm-up. -1: not made (NGX refused the preset or the size).
double time_pair(NVSDK_NGX_Parameter* params, NVSDK_NGX_PerfQuality_Value quality, int preset, UINT rw, UINT rh, UINT ow,
                 UINT oh, int frames) {
    View views[2] = {{rw, rh, ow, oh}, {rw, rh, ow, oh}};
    bool made = true;
    for (View& v : views) {
        made = made && create(v, params, quality, preset, false);
        v.colour = texture(v.rw, v.rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
        v.depth = texture(v.rw, v.rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
        v.vectors = texture(v.rw, v.rh, DXGI_FORMAT_R16G16_FLOAT, kTargets, false);
        v.output = texture(v.ow, v.oh, DXGI_FORMAT_R16G16B16A16_FLOAT, kTargets, false);
        const float zero[4]{};
        ComPtr<ID3D11RenderTargetView> rtv;
        device->CreateRenderTargetView(v.vectors.Get(), nullptr, &rtv);
        context->ClearRenderTargetView(rtv.Get(), zero);
    }
    double result = -1;
    if (made) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt{D3D11_QUERY_TIMESTAMP, 0};
        ComPtr<ID3D11Query> disjoint, t0, t1;
        device->CreateQuery(&qd, &disjoint), device->CreateQuery(&qt, &t0), device->CreateQuery(&qt, &t1);
        std::vector<double> gpu;
        bool ok = true;
        for (int f = 0; f < frames; ++f) {
            context->Begin(disjoint.Get());
            context->End(t0.Get());
            for (View& v : views) {
                NVSDK_NGX_D3D11_DLSS_Eval_Params e{};
                e.Feature.pInColor = v.colour.Get(), e.Feature.pInOutput = v.output.Get();
                e.pInDepth = v.depth.Get(), e.pInMotionVectors = v.vectors.Get();
                e.InJitterOffsetX = (f % 8) / 8.0f - 0.5f, e.InJitterOffsetY = (f % 3) / 3.0f - 0.5f;
                e.InRenderSubrectDimensions = {v.rw, v.rh};
                e.InReset = f == 0, e.InMVScaleX = 1, e.InMVScaleY = 1, e.InPreExposure = 1, e.InExposureScale = 1;
                ok = ok && NVSDK_NGX_SUCCEED(NGX_D3D11_EVALUATE_DLSS_EXT(context.Get(), v.handle, params, &e));
            }
            context->End(t1.Get());
            context->End(disjoint.Get());
            context->Flush();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 s0 = 0, s1 = 0;
            while (context->GetData(disjoint.Get(), &dj, sizeof dj, 0) == S_FALSE) Sleep(0);
            while (context->GetData(t0.Get(), &s0, sizeof s0, 0) == S_FALSE) Sleep(0);
            while (context->GetData(t1.Get(), &s1, sizeof s1, 0) == S_FALSE) Sleep(0);
            if (f >= frames / 5 && !dj.Disjoint)
                gpu.push_back(double(s1 - s0) * 1000.0 / double(dj.Frequency));
        }
        result = ok ? median(gpu) : -1;
    }
    for (View& v : views)
        if (v.handle) NVSDK_NGX_D3D11_ReleaseFeature(v.handle);
    return result;
}

// An eye (ew x eh) with its focus view (fw x fh) to its right in one set of textures: the focus view evaluated from
// its region, written into the same region of a wide output whose eye part must stay untouched.
void run_subrects(NVSDK_NGX_Parameter* params, UINT ew, UINT eh, UINT fw, UINT fh) {
    View v{fw, fh, fw, fh};
    UINT W = ew + fw, H = std::max(eh, fh);
    bool made = create(v, params, NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_Default, true);
    check(made, "sub-rectangles: feature made with output sub-rectangles");
    if (!made) return;
    v.colour = texture(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
    v.depth = texture(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
    v.vectors = texture(W, H, DXGI_FORMAT_R16G16_FLOAT, kTargets, false);
    v.output = texture(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, kTargets, false);
    const float zero[4]{};
    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateRenderTargetView(v.vectors.Get(), nullptr, &rtv);
    context->ClearRenderTargetView(rtv.Get(), zero);
    rtv.Reset();
    device->CreateRenderTargetView(v.output.Get(), nullptr, &rtv);
    context->ClearRenderTargetView(rtv.Get(), zero);
    bool ok = true;
    for (int f = 0; f < 8; ++f) {
        NVSDK_NGX_D3D11_DLSS_Eval_Params e{};
        e.Feature.pInColor = v.colour.Get(), e.Feature.pInOutput = v.output.Get();
        e.pInDepth = v.depth.Get(), e.pInMotionVectors = v.vectors.Get();
        e.InRenderSubrectDimensions = {fw, fh};
        e.InColorSubrectBase = e.InDepthSubrectBase = e.InMVSubrectBase = e.InOutputSubrectBase = {ew, 0};
        e.InReset = f == 0, e.InMVScaleX = 1, e.InMVScaleY = 1, e.InPreExposure = 1, e.InExposureScale = 1;
        ok = ok && NVSDK_NGX_SUCCEED(NGX_D3D11_EVALUATE_DLSS_EXT(context.Get(), v.handle, params, &e));
    }
    check(ok, "sub-rectangles: every evaluation succeeded");
    float focus = written(v.output.Get(), ew, fh / 2, fw), eye = written(v.output.Get(), 0, fh / 2, ew);
    std::printf("     written: focus region %.3f, eye region %.3f\n", focus, eye);
    check(focus > 0.9f, "sub-rectangles: the focus region of the output was written");
    check(eye == 0.0f, "sub-rectangles: the eye region of the output was left alone");
    NVSDK_NGX_D3D11_ReleaseFeature(v.handle);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: ngx_dlss_check <folder with nvngx_dlss.dll> [frames]\n");
        return 2;
    }
    int frames = argc > 2 ? std::max(20, _wtoi(argv[2])) : 200;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("no D3D11 hardware device\n");
        return 2;
    }

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring data = std::wstring(temp) + L"sim-qvfr-ngx-check";
    CreateDirectoryW(data.c_str(), nullptr);
    const wchar_t* paths[] = {argv[1]};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths, info.PathListInfo.Length = 1;
    if (std::getenv("NGX_LOG")) {
        info.LoggingInfo.LoggingCallback = ngx_log;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    }
    // A project id of our own (a GUID, custom engine): no NVIDIA application id.
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("5b1c0f1e-6a57-4c1b-9d0e-51a0c7f3d2a4", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                             "1.0", data.c_str(), device.Get(), &info);
    std::printf("NGX init: 0x%08x\n", unsigned(r));
    check(NVSDK_NGX_SUCCEED(r), "NGX starts on a D3D11 device with a project id");
    if (NVSDK_NGX_FAILED(r)) return 1;

    NVSDK_NGX_Parameter* params = nullptr;
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
    check(NVSDK_NGX_SUCCEED(r) && params, "capability parameters");
    if (!params) return 1;
    int available = 0, needsDriver = 0, initResult = 0;
    params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
    std::printf("DLSS available %d, needs a newer driver %d, feature init result 0x%08x\n", available, needsDriver,
                unsigned(initResult));
    check(available != 0, "DLSS is available to this process");

    if (available) {
        // Sizes as QVFR runs LMU on this machine's headset: periphery views ~1600x1280, focus views ~1280x1300.
        const NVSDK_NGX_PerfQuality_Value dlaa = NVSDK_NGX_PerfQuality_Value_DLAA, quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        Case cases[] = {
            {"DLAA, default preset", dlaa, NVSDK_NGX_DLSS_Hint_Render_Preset_Default,
             {{1600, 1280, 1600, 1280}, {1600, 1280, 1600, 1280}, {1280, 1300, 1280, 1300}, {1280, 1300, 1280, 1300}}},
            {"DLAA, preset K", dlaa, NVSDK_NGX_DLSS_Hint_Render_Preset_K,
             {{1600, 1280, 1600, 1280}, {1600, 1280, 1600, 1280}, {1280, 1300, 1280, 1300}, {1280, 1300, 1280, 1300}}},
            {"DLAA, preset M", dlaa, NVSDK_NGX_DLSS_Hint_Render_Preset_M,
             {{1600, 1280, 1600, 1280}, {1600, 1280, 1600, 1280}, {1280, 1300, 1280, 1300}, {1280, 1300, 1280, 1300}}},
            {"Quality (67%), default preset", quality, NVSDK_NGX_DLSS_Hint_Render_Preset_Default,
             {{1072, 858, 1600, 1280}, {1072, 858, 1600, 1280}, {858, 871, 1280, 1300}, {858, 871, 1280, 1300}}},
            {"Quality (67%), preset K", quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K,
             {{1072, 858, 1600, 1280}, {1072, 858, 1600, 1280}, {858, 871, 1280, 1300}, {858, 871, 1280, 1300}}},
            {"DLAA, default preset, large views", dlaa, NVSDK_NGX_DLSS_Hint_Render_Preset_Default,
             {{2478, 1990, 2478, 1990}, {2478, 1990, 2478, 1990}, {1600, 1284, 1600, 1284}, {1600, 1284, 1600, 1284}}},
        };
        for (Case& k : cases) run_case(k, params, frames);
        run_subrects(params, 1600, 1280, 1280, 1300);

        // Per pair and preset at LMU run 14's sizes (eyes 1038x834, focus views 1947x1563 drawn at 50%): what each
        // preset costs the eyes (the periphery) and the focus views apart. Hinted for every mode at once, as the
        // module does; 0 is NGX's default for the mode (K for DLAA, quality and balanced; M for performance).
        struct Preset {
            const char* name;
            int value;
        };
        const Preset presets[] = {{"default", NVSDK_NGX_DLSS_Hint_Render_Preset_Default}, {"E", NVSDK_NGX_DLSS_Hint_Render_Preset_E},
                                  {"F", NVSDK_NGX_DLSS_Hint_Render_Preset_F}, {"J", NVSDK_NGX_DLSS_Hint_Render_Preset_J},
                                  {"K", NVSDK_NGX_DLSS_Hint_Render_Preset_K}, {"L", NVSDK_NGX_DLSS_Hint_Render_Preset_L},
                                  {"M", NVSDK_NGX_DLSS_Hint_Render_Preset_M}};
        const NVSDK_NGX_PerfQuality_Value perf = NVSDK_NGX_PerfQuality_Value_MaxPerf;
        std::printf("\nper pair, GPU ms per frame for two views (run 14's sizes; -1: not made):\n");
        std::printf("     %-8s %12s %14s %14s %14s\n", "preset", "eyes DLAA", "focus DLAA", "focus 67%", "focus 50%");
        for (const Preset& p : presets) {
            const double eyes = time_pair(params, dlaa, p.value, 1038, 834, 1038, 834, frames);
            const double focusDlaa = time_pair(params, dlaa, p.value, 1947, 1563, 1947, 1563, frames);
            const double focusQuality = time_pair(params, quality, p.value, 1304, 1047, 1947, 1563, frames);
            const double focusPerf = time_pair(params, perf, p.value, 974, 782, 1947, 1563, frames);
            std::printf("     %-8s %12.2f %14.2f %14.2f %14.2f\n", p.name, eyes, focusDlaa, focusQuality, focusPerf);
        }
    }
    context->ClearState();
    context->Flush();
    NVSDK_NGX_D3D11_Shutdown1(device.Get());
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

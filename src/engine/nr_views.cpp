// DLSS-NR on the views (nr_views.h).
#include "nr_views.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <vector>

#include "../log.h"
#include "../probe/d3d11_capture.h"
#include "ngx_nr.h"
#include "nr_math.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr {
namespace {

// Two full-screen triangles' worth of work a view:
//   prepare (at the working size): the region's colour through the curve into the model's colour input, the view's
//     depth and motion vectors for the region's texels into its other two;
//   compose (at the region's size): the picture as it came times the model's change of it (the ratio of its output to
//     its input as a display shows them, both sampled at the working size, held to a most), faded to the picture as it
//     came along the edges marked.
// Before them, once a frame: meter (the picture's log brightness at 64x64, averaged down its mips) and adapt (that
// mean blended into the one value every view's exposure comes from).
constexpr char kShaders[] = R"(
cbuffer C : register(b0) {
    float4 region;   // the region in the view: x, y, width, height (the colour's pixels)
    float4 view;     // the view's size in the colour's pixels; the guides' size
    float4 working;  // the working size; the exposure; 1: sRGB's curve
    float4 motion;   // xy: how far the region moved in the view since the frame before, as a fraction of the region;
                     // z: adapt's share of this frame's brightness
    float4 edges;    // 1 where that side fades: left, top, right, bottom
    float4 blend;    // x: the band, a fraction of the region; y: 1 with the exposure metered; z: the most gain;
                     // w: show's picture (0 the model's input, 1 its output, 2 the picture bound, through the curve)
};
Texture2D<float4> sourceIn : register(t0);   // the region as it came
Texture2D<float> depthIn : register(t1);     // prepare: the view's depth
Texture2D<float2> vectorsIn : register(t2);  // prepare: the view's motion vectors
Texture2D<float4> modelOut : register(t3);   // compose: the model's output
Texture2D<float4> modelIn : register(t4);    // compose: the model's input
Texture2D<float4> adaptedIn : register(t5);  // the scene's mean log brightness, smoothed (1x1)
Texture2D<float4> meterIn : register(t6);    // adapt: this frame's log brightness (its last mip the mean)
SamplerState linearClamp : register(s0);

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 to_srgb(float3 c) { return lerp(12.92 * c, 1.055 * pow(max(c, 1e-8), 1 / 2.4) - 0.055, step(0.0031308, c)); }
float3 from_srgb(float3 c) { return lerp(c / 12.92, pow(max((c + 0.055) / 1.055, 1e-8), 2.4), step(0.04045, c)); }
float luminance(float3 c) { return dot(max(c, 0), float3(0.2126, 0.7152, 0.0722)); }
// Metered: the scene's mean brightness to middle grey (0.18 after the curve below), whatever the game's scale.
float exposure() { return working.z * (blend.y > 0.5 ? 0.22 / max(exp(adaptedIn.Load(int3(0, 0, 0)).r), 1e-5) : 1); }
float3 encode(float3 c) {
    c = max(c, 0) * exposure();
    c = c / (1 + max(c.r, max(c.g, c.b)));
    return working.w > 0.5 ? to_srgb(c) : c;
}
// What the model got or gave back, as a display shows it.
float3 shown(float3 e) {
    e = saturate(e);
    return working.w > 0.5 ? from_srgb(e) : e;
}
float4 meter(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy / 64, o = 0.25 / 64;
    float l = log(luminance(sourceIn.SampleLevel(linearClamp, uv + o * float2(-1, -1), 0).rgb) + 1e-4) +
              log(luminance(sourceIn.SampleLevel(linearClamp, uv + o * float2(1, -1), 0).rgb) + 1e-4) +
              log(luminance(sourceIn.SampleLevel(linearClamp, uv + o * float2(-1, 1), 0).rgb) + 1e-4) +
              log(luminance(sourceIn.SampleLevel(linearClamp, uv + o * float2(1, 1), 0).rgb) + 1e-4);
    return float4(l / 4, 0, 0, 1);
}
float4 adapt(float4 pos : SV_Position) : SV_Target { return float4(meterIn.Load(int3(0, 0, 6)).r, 0, 0, motion.z); }
struct Prepared {
    float4 colour : SV_Target0;
    float depth : SV_Target1;
    float2 motion : SV_Target2;
};
Prepared prepare(float4 pos : SV_Position) {
    float2 uv = pos.xy / working.xy;
    float2 at = (region.xy + uv * region.zw) / view.xy;  // in the view
    int2 texel = clamp(int2(at * view.zw), int2(0, 0), int2(view.zw) - 1);
    Prepared o;
    // Four taps a texel: a region worked at a smaller size is averaged into it, not picked from.
    float2 q = 0.25 / working.xy;
    float3 came = (sourceIn.SampleLevel(linearClamp, uv + q * float2(-1, -1), 0).rgb + sourceIn.SampleLevel(linearClamp, uv + q * float2(1, -1), 0).rgb +
                   sourceIn.SampleLevel(linearClamp, uv + q * float2(-1, 1), 0).rgb + sourceIn.SampleLevel(linearClamp, uv + q * float2(1, 1), 0).rgb) / 4;
    o.colour = float4(encode(came), 1);
    o.depth = depthIn.Load(int3(texel, 0));
    o.motion = vectorsIn.Load(int3(texel, 0)) / view.zw * (view.xy / region.zw) + motion.xy;
    return o;
}
float fade(float t, float on) {
    float s = saturate(t / max(blend.x, 1e-6));
    return on > 0.5 && blend.x > 0 ? s * s * (3 - 2 * s) : 1;
}
float4 compose(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy / region.zw;
    float4 came = sourceIn.Load(int3(pos.xy, 0));
    float3 got = shown(modelIn.SampleLevel(linearClamp, uv, 0).rgb), gave = shown(modelOut.SampleLevel(linearClamp, uv, 0).rgb);
    float3 gain = clamp((gave + 0.01) / (got + 0.01), 1 / blend.z, blend.z);
    float3 made = came.rgb * gain;
    float w = fade(uv.x, edges.x) * fade(uv.y, edges.y) * fade(1 - uv.x, edges.z) * fade(1 - uv.y, edges.w);
    return float4(lerp(came.rgb, made, w), came.a);
}
float4 show(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy / working.xy;
    float3 c = blend.w < 0.5 ? modelIn.SampleLevel(linearClamp, uv, 0).rgb
               : blend.w < 1.5 ? modelOut.SampleLevel(linearClamp, uv, 0).rgb : encode(sourceIn.SampleLevel(linearClamp, uv, 0).rgb);
    return float4(saturate(c), 1);
}
)";

struct Constants {
    float region[4], view[4], working[4], motion[4], edges[4], blend[4];
};

struct Slot {
    // What it was made for.
    nr::Region region;  // its size; x and y as last run
    unsigned workingW = 0, workingH = 0;
    int preset = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool made = false, refused = false;
    ngxnr::View* view = nullptr;
    ComPtr<ID3D11RenderTargetView> colourRtv, depthRtv, vectorsRtv, resultRtv;
    ComPtr<ID3D11ShaderResourceView> colourSrv, outputSrv, sourceSrv, resultSrv;
    unsigned long long runs = 0;  // evaluations of this feature
    ComPtr<ID3D11Texture2D> source, result;
    unsigned long long ranFrame = 0;  // the frame it last ran in (history: the frame before)
    bool before = false;              // it ran before DLSS then
    double msSum = 0;
    unsigned msCount = 0;
};

struct State {
    const char* tag = "dlss-nr";
    NrSettings cfg;
    bool off = false;  // gave up for the session (the log says why)
    bool ready = false;
    unsigned long long frame = 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> prepare, compose, meter, adapt, show;
    // The exposure's meter: this frame's log brightness (64x64, mips), and the one smoothed value every view uses.
    ComPtr<ID3D11Texture2D> meterTex, adaptedTex, adaptedRead;
    ComPtr<ID3D11RenderTargetView> meterRtv, adaptedRtv;
    ComPtr<ID3D11ShaderResourceView> meterSrv, adaptedSrv;
    ComPtr<ID3D11BlendState> adaptBlend;
    unsigned long long meterFrame = 0;
    bool adapted = false, readPending = false;
    double sceneMean = 0;  // the scene's mean brightness as last read back (0: not yet)
    // Diagnostics (NrSettings::saveFrames).
    int savesLeft = 0, saved = 0;
    ComPtr<ID3D11Texture2D> showTex;
    ComPtr<ID3D11RenderTargetView> showRtv;
    unsigned showW = 0, showH = 0;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearClamp;
    Slot slots[4];
    int createFailures = 0, failuresInARow = 0;
    // The breaker: frames in a row that took over kSlowFrame with DLSS-NR's evaluations in them.
    std::chrono::steady_clock::time_point lastFrameEnd;
    bool haveLastFrameEnd = false, ranThisFrame = false, madeThisFrame = false;
    int slowFrames = 0;
    unsigned long long behind = 0;  // evaluations left out since the last line: the GPU had a backlog of ours
    unsigned limitLogged[4][2] = {};  // the limited working size last said for the view
    bool memoryLogged = false, smallLogged[4] = {};
    int restarts = 0;  // times our device was lost and DLSS-NR started again this session
    unsigned long long evaluations = 0, failures = 0, remade = 0, statsFrame = 0, evaluationsTotal = 0;
    bool firstLogged[4] = {};
    std::mutex mutex;
    bool livePending = false;
    NrSettings live;
    std::chrono::steady_clock::time_point liveAt;
} d;

void say(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    simqvfr::logf("%s: %s", d.tag ? d.tag : "dlss-nr", text);
}

void give_up(const char* why) {
    if (!d.off)
        say("off for this session: %s", why);
    d.off = true;
}

// The slot's view given up (ngx_nr.h release): with its feature go the slot's own views and textures, all kept until
// both devices are through what is queued on them. On the game's immediate context (the render thread).
void release_slot(Slot& s) {
    if (s.view) {
        IUnknown* const keep[] = {s.colourRtv.Get(), s.depthRtv.Get(),  s.vectorsRtv.Get(), s.resultRtv.Get(), s.colourSrv.Get(),
                                  s.outputSrv.Get(), s.sourceSrv.Get(), s.source.Get(),     s.result.Get(), s.resultSrv.Get()};
        ComPtr<ID3D11DeviceContext> context;
        if (d.device)
            d.device->GetImmediateContext(&context);
        ngxnr::release(context.Get(), s.view, keep, static_cast<unsigned>(std::size(keep)));
    }
    s = Slot{};
}

const char* style_name(int style) { return style == 1 ? "natural" : style == 2 ? "cinematic" : "standard"; }
const char* preset_name(int preset) { return preset == 1 ? "A" : preset == 2 ? "B" : "the runtime's default"; }

void describe(const NrSettings& c, char* text, size_t size) {
    char periphery[96] = "the focus views only";
    if (c.periphery)
        std::snprintf(periphery, sizeof(periphery), "the focus views and %d%% of the eyes' views (worked at %d%%)", c.peripheryCoverage,
                      c.peripheryScale);
    std::snprintf(text, size,
                  "%s, the focus views worked at %d%%, at most %.1f megapixels a view, %s DLSS; preset %s, style %s, intensity "
                  "%d%%, local tone %d%%, local structure %d%%, auto mask %s, edge blend %d%%, exposure %s%d%%, input %s, gain "
                  "up to %.1fx",
                  periphery, c.focusScale, c.workingLimit / 10.0, c.beforeDlss ? "before" : "after", preset_name(c.preset),
                  style_name(c.style), c.intensity,
                  c.localTone, c.localStructure, c.autoMask ? "on" : "off", c.edgeBlend, c.autoExposure ? "metered x " : "", c.exposure,
                  c.inputGamma ? "sRGB-encoded" : "linear", c.maxGain / 100.0);
}

ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShaders, std::strlen(kShaders), "dlss-nr", nullptr, nullptr, entry, profile, 0, 0, &code, &errors))) {
        say("shader %s: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

bool initialize(ID3D11DeviceContext* context) {
    context->GetDevice(&d.device);
    if (!d.device) {
        give_up("the context has no device");
        return false;
    }
    const auto vs = compile("vs", "vs_5_0"), prepare = compile("prepare", "ps_5_0"), compose = compile("compose", "ps_5_0");
    const auto meter = compile("meter", "ps_5_0"), adapt = compile("adapt", "ps_5_0"), show = compile("show", "ps_5_0");
    D3D11_TEXTURE2D_DESC md{};
    md.Width = md.Height = 64;
    md.MipLevels = 7;
    md.ArraySize = 1;
    md.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    md.SampleDesc.Count = 1;
    md.Usage = D3D11_USAGE_DEFAULT;
    md.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    D3D11_TEXTURE2D_DESC ad = md;
    ad.Width = ad.Height = 1;
    ad.MipLevels = 1;
    ad.MiscFlags = 0;
    D3D11_TEXTURE2D_DESC rd = ad;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.BindFlags = 0;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_RENDER_TARGET_VIEW_DESC mrd{};
    mrd.Format = md.Format;
    mrd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!meter || !adapt || !show ||
        FAILED(d.device->CreatePixelShader(meter->GetBufferPointer(), meter->GetBufferSize(), nullptr, &d.meter)) ||
        FAILED(d.device->CreatePixelShader(adapt->GetBufferPointer(), adapt->GetBufferSize(), nullptr, &d.adapt)) ||
        FAILED(d.device->CreatePixelShader(show->GetBufferPointer(), show->GetBufferSize(), nullptr, &d.show)) ||
        FAILED(d.device->CreateTexture2D(&md, nullptr, &d.meterTex)) || FAILED(d.device->CreateTexture2D(&ad, nullptr, &d.adaptedTex)) ||
        FAILED(d.device->CreateTexture2D(&rd, nullptr, &d.adaptedRead)) ||
        FAILED(d.device->CreateRenderTargetView(d.meterTex.Get(), &mrd, &d.meterRtv)) ||
        FAILED(d.device->CreateShaderResourceView(d.meterTex.Get(), nullptr, &d.meterSrv)) ||
        FAILED(d.device->CreateRenderTargetView(d.adaptedTex.Get(), nullptr, &d.adaptedRtv)) ||
        FAILED(d.device->CreateShaderResourceView(d.adaptedTex.Get(), nullptr, &d.adaptedSrv)) ||
        FAILED(d.device->CreateBlendState(&blend, &d.adaptBlend))) {
        give_up("its exposure meter couldn't be made");
        return false;
    }
    d.adapted = d.readPending = false;
    d.meterFrame = 0;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (!vs || !prepare || !compose ||
        FAILED(d.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d.vs)) ||
        FAILED(d.device->CreatePixelShader(prepare->GetBufferPointer(), prepare->GetBufferSize(), nullptr, &d.prepare)) ||
        FAILED(d.device->CreatePixelShader(compose->GetBufferPointer(), compose->GetBufferSize(), nullptr, &d.compose)) ||
        FAILED(d.device->CreateBuffer(&bd, nullptr, &d.constants)) || FAILED(d.device->CreateSamplerState(&sd, &d.linearClamp))) {
        give_up("its shaders couldn't be made");
        return false;
    }
    std::string why;
    const std::wstring data = (std::filesystem::path(simqvfr::log_directory()) / L"ngx-nr").wstring();
    if (!ngxnr::start(d.device.Get(), d.cfg.folder.c_str(), data.c_str(), why)) {
        give_up(why.c_str());
        return false;
    }
    say("NGX started on a Direct3D 12 device beside the game's (nvngx_dlssnr.dll from %ls)", d.cfg.folder.c_str());
    d.ready = true;
    return true;
}

bool make(ComPtr<ID3D11Texture2D>& texture, unsigned w, unsigned h, DXGI_FORMAT format, UINT bind) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    texture.Reset();
    return SUCCEEDED(d.device->CreateTexture2D(&td, nullptr, &texture));
}

// The view's feature and textures for a region this size, worked at ww x wh (made again when any of it changes).
bool ensure(Slot& s, int index, const nr::Region& region, unsigned ww, unsigned wh, DXGI_FORMAT format) {
    if (s.made && s.region.width == region.width && s.region.height == region.height && s.workingW == ww && s.workingH == wh &&
        s.preset == d.cfg.preset && s.format == format)
        return s.view != nullptr;
    if (s.view)
        ++d.remade;
    release_slot(s);
    s.made = true;
    s.region = region;
    s.workingW = ww;
    s.workingH = wh;
    s.preset = d.cfg.preset;
    s.format = format;
    unsigned result = 0;
    // Room on the card first: a feature made into a full card has Windows move memory off it every frame (frames of
    // hundreds of ms).
    double used = 0, budget = 0;
    const double needs = ngxnr::view_memory_mb(ww, wh);
    if (ngxnr::video_memory(used, budget) && used + needs > budget) {
        if (!d.memoryLogged)
            say("view %d: not made: worked at %ux%u it needs about %.0f MB of video memory, and the game uses %.0f of the %.0f MB "
                "Windows gives it. Lower the working size or its limit, or the game's resolution",
                index, ww, wh, needs, used, budget);
        d.memoryLogged = true;
        s.refused = true;
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    s.view = ngxnr::create(ww, wh, d.cfg.preset, result);
    if (!s.view && d.cfg.preset != 0) {  // a runtime without that model: its default
        unsigned again = 0;
        s.view = ngxnr::create(ww, wh, 0, again);
        if (s.view)
            say("view %d: preset %s refused (0x%08x): made with the runtime's default instead", index, preset_name(d.cfg.preset), result);
        else
            result = again;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    bool ok = s.view != nullptr;
    if (ok) {
        const ngxnr::Textures t = ngxnr::textures(s.view);
        ok = SUCCEEDED(d.device->CreateRenderTargetView(t.colour, nullptr, &s.colourRtv)) &&
             SUCCEEDED(d.device->CreateRenderTargetView(t.depth, nullptr, &s.depthRtv)) &&
             SUCCEEDED(d.device->CreateRenderTargetView(t.vectors, nullptr, &s.vectorsRtv)) &&
             SUCCEEDED(d.device->CreateShaderResourceView(t.colour, nullptr, &s.colourSrv)) &&
             SUCCEEDED(d.device->CreateShaderResourceView(t.output, nullptr, &s.outputSrv)) &&
             make(s.source, region.width, region.height, format, D3D11_BIND_SHADER_RESOURCE) &&
             make(s.result, region.width, region.height, format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
             SUCCEEDED(d.device->CreateShaderResourceView(s.source.Get(), nullptr, &s.sourceSrv)) &&
             SUCCEEDED(d.device->CreateShaderResourceView(s.result.Get(), nullptr, &s.resultSrv)) &&
             SUCCEEDED(d.device->CreateRenderTargetView(s.result.Get(), nullptr, &s.resultRtv));
    }
    if (!ok) {
        say("view %d's feature for a %ux%u region worked at %ux%u (format %d) couldn't be made (%s, 0x%08x)", index, region.width,
            region.height, ww, wh, static_cast<int>(format), s.view ? "its Direct3D 11 views" : "NGX or the shared textures", result);
        if (s.view) {  // made, but its Direct3D 11 views weren't: given up as any other
            Slot gone;
            gone.view = s.view;
            release_slot(gone);
        }
        s.view = nullptr;
        s.refused = true;
        if (++d.createFailures >= 4)
            give_up("the features can't be made");
        return false;
    }
    d.madeThisFrame = true;
    double usedNow = 0, budgetNow = 0;
    const bool memory = ngxnr::video_memory(usedNow, budgetNow);
    say("view %d's feature made in %.0f ms: a %ux%u region worked at %ux%u (%.2f megapixels), preset %s; video memory %.0f of %.0f MB",
        index, ms, region.width, region.height, ww, wh, ww * static_cast<double>(wh) / 1e6, preset_name(d.cfg.preset),
        memory ? usedNow : 0.0, memory ? budgetNow : 0.0);
    return true;
}

void draw(ID3D11DeviceContext* context, ID3D11PixelShader* ps, unsigned w, unsigned h) {
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    context->RSSetViewports(1, &vp);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(d.vs.Get(), nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, d.constants.GetAddressOf());
    context->PSSetSamplers(0, 1, d.linearClamp.GetAddressOf());
    context->Draw(3, 0);
}

// Diagnostics (NrSettings::saveFrames): what the model got, what it gave back and the composed result through the same
// curve, each drawn into an 8-bit picture at the working size, read back and written as PNG into the log folder.
void save_pictures(ID3D11DeviceContext* context, Slot& s, Constants c) {
    if (!d.showTex || d.showW != s.workingW || d.showH != s.workingH) {
        d.showRtv.Reset();
        if (!make(d.showTex, s.workingW, s.workingH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET) ||
            FAILED(d.device->CreateRenderTargetView(d.showTex.Get(), nullptr, &d.showRtv))) {
            d.savesLeft = 0;
            return;
        }
        d.showW = s.workingW;
        d.showH = s.workingH;
    }
    --d.savesLeft;
    ++d.saved;
    static const wchar_t* const names[3] = {L"model-got", L"model-gave", L"result"};
    const std::filesystem::path folder = simqvfr::log_directory();
    for (int mode = 0; mode < 3; ++mode) {
        c.blend[3] = static_cast<float>(mode);
        context->UpdateSubresource(d.constants.Get(), 0, nullptr, &c, 0, 0);
        context->OMSetRenderTargets(1, d.showRtv.GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* in[6] = {s.resultSrv.Get(), nullptr, nullptr, s.outputSrv.Get(), s.colourSrv.Get(), d.adaptedSrv.Get()};
        context->PSSetShaderResources(0, 6, in);
        draw(context, d.show.Get(), s.workingW, s.workingH);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* const none[6] = {};
        context->PSSetShaderResources(0, 6, none);
        std::vector<uint8_t> pixels;
        unsigned w = 0, h = 0;
        wchar_t name[64];
        std::swprintf(name, std::size(name), L"dlss-nr-%02d-%ls.png", d.saved, names[mode]);
        if (d3d11_read_bgra8(d.showTex.Get(), pixels, w, h))
            write_png_bgra((folder / name).wstring(), pixels, w, h);
    }
    say("pictures %d saved (dlss-nr-%02d-model-got / -model-gave / -result .png, %ux%u, in the log folder): the scene's mean "
        "brightness %.4f as last read",
        d.saved, d.saved, s.workingW, s.workingH, d.sceneMean);
}

// Does `l` need features made again (or released), against the settings in use?
bool live_remakes(const NrSettings& l) {
    const NrSettings& c = d.cfg;
    return l.on != c.on || l.preset != c.preset || l.periphery != c.periphery || l.focusScale != c.focusScale ||
           l.workingLimit != c.workingLimit ||
           l.peripheryScale != c.peripheryScale || l.peripheryCoverage != c.peripheryCoverage || l.beforeDlss != c.beforeDlss;
}

constexpr auto kLiveSettle = std::chrono::milliseconds(500);
// The breaker (nr_frame_end): this many frames in a row over this long, each with DLSS-NR evaluated in it, and it turns
// itself off for the session. The first headset run sat at 300 ms a frame until the player found the switch.
constexpr auto kSlowFrame = std::chrono::milliseconds(100);
constexpr int kSlowFrames = 20;

}  // namespace

void nr_start(const char* tag, const NrSettings& settings) {
    {
        std::lock_guard guard(d.mutex);
        d.livePending = false;
    }
    d.tag = tag ? tag : "dlss-nr";
    d.cfg = nr_clamped(settings);
    d.savesLeft = d.cfg.saveFrames;
    d.saved = 0;
    d.off = false;
    if (d.cfg.on && !ngxnr::built_in()) {
        say("asked for, but this build has no NGX SDK (CMake: NGX_SDK_DIR): off");
        d.cfg.on = false;
    }
    if (d.cfg.on) {
        char text[512];
        describe(d.cfg, text, sizeof(text));
        say("DLSS-NR on %s", text);
    }
}

bool nr_wanted() { return d.cfg.on && !d.off; }
bool nr_view_wanted(int index) { return nr_wanted() && index >= 0 && index < 4 && (index >= 2 || d.cfg.periphery); }
bool nr_before_dlss() { return d.cfg.beforeDlss; }

void nr_live(const NrSettings& settings) {
    std::lock_guard guard(d.mutex);
    d.live = nr_clamped(settings);
    d.livePending = true;
    d.liveAt = std::chrono::steady_clock::now();
}

bool nr_run_view(ID3D11DeviceContext* context, const NrView& v) {
    if (!context || !v.scene || !v.depth || !v.vectors || !v.width || !v.height || !v.guideWidth || !v.guideHeight ||
        !nr_view_wanted(v.index))
        return false;
    if (!d.ready && !initialize(context))
        return false;
    const bool eye = v.index < 2;
    const nr::Region region = eye && d.cfg.peripheryCoverage < 100
                                  ? nr::covered(v.width, v.height, v.focusKnown ? v.focusU : 0.5, v.focusKnown ? v.focusV : 0.5,
                                                d.cfg.peripheryCoverage)
                                  : nr::Region{0, 0, v.width, v.height};
    const int scale = eye ? d.cfg.peripheryScale : d.cfg.focusScale;
    unsigned ww = nr::working(region.width, scale), wh = nr::working(region.height, scale);
    const unsigned askedW = ww, askedH = wh;
    if (nr::limit_working(ww, wh, d.cfg.workingLimit / 10.0) && (d.limitLogged[v.index][0] != ww || d.limitLogged[v.index][1] != wh)) {
        d.limitLogged[v.index][0] = ww, d.limitLogged[v.index][1] = wh;
        say("view %d: %d%% of its %ux%u region would be %ux%u (%.2f megapixels): worked at %ux%u, the limit of %.1f megapixels a "
            "view (working_limit)",
            v.index, scale, region.width, region.height, askedW, askedH, askedW * static_cast<double>(askedH) / 1e6, ww, wh,
            d.cfg.workingLimit / 10.0);
    }
    if (ww < nr::kMinWorking || wh < nr::kMinWorking) {  // a view smaller than the model can work at: left as it came
        if (!d.smallLogged[v.index])
            say("view %d: its %ux%u region is under the %ux%u the model needs: not processed", v.index, region.width, region.height,
                nr::kMinWorking, nr::kMinWorking);
        d.smallLogged[v.index] = true;
        return false;
    }
    Slot& s = d.slots[v.index];
    if (s.refused && s.region.width == region.width && s.region.height == region.height && s.workingW == ww && s.workingH == wh &&
        s.preset == d.cfg.preset && s.format == v.format)
        return false;
    if (!ensure(s, v.index, region, ww, wh, v.format)) {
        ++d.failures;
        return false;
    }
    // History: the frame before, the same side of DLSS, and the region no further than a quarter of itself from where
    // it was (its move goes into the vectors).
    const double movedX = (static_cast<double>(region.x) - s.region.x) / region.width;
    const double movedY = (static_cast<double>(region.y) - s.region.y) / region.height;
    const bool history = !v.reset && s.ranFrame + 1 == d.frame && s.before == d.cfg.beforeDlss && std::fabs(movedX) < 0.25 &&
                         std::fabs(movedY) < 0.25;
    s.region = region;
    if (!d.firstLogged[v.index]) {
        d.firstLogged[v.index] = true;
        say("view %d first run: %ux%u at (%u, %u) of its %ux%u, worked at %ux%u, guides %ux%u, depth %s, %s DLSS", v.index,
            region.width, region.height, region.x, region.y, v.width, v.height, ww, wh, v.guideWidth, v.guideHeight,
            v.depthInverted ? "reversed" : "0 near", d.cfg.beforeDlss ? "before" : "after");
    }

    Constants c{};
    c.region[0] = static_cast<float>(region.x), c.region[1] = static_cast<float>(region.y);
    c.region[2] = static_cast<float>(region.width), c.region[3] = static_cast<float>(region.height);
    c.view[0] = static_cast<float>(v.width), c.view[1] = static_cast<float>(v.height);
    c.view[2] = static_cast<float>(v.guideWidth), c.view[3] = static_cast<float>(v.guideHeight);
    c.working[0] = static_cast<float>(ww), c.working[1] = static_cast<float>(wh);
    c.working[2] = d.cfg.exposure / 100.0f;
    c.working[3] = d.cfg.inputGamma ? 1.0f : 0.0f;
    c.motion[0] = history ? static_cast<float>(movedX) : 0.0f;
    c.motion[1] = history ? static_cast<float>(movedY) : 0.0f;
    // A side fades where what lies beyond it isn't processed: a focus view's sides without the periphery, a covered
    // part's sides inside its eye's view.
    const bool focusFades = !eye && !d.cfg.periphery;
    c.edges[0] = focusFades || (eye && region.x > 0) ? 1.0f : 0.0f;
    c.edges[1] = focusFades || (eye && region.y > 0) ? 1.0f : 0.0f;
    c.edges[2] = focusFades || (eye && region.x + region.width < v.width) ? 1.0f : 0.0f;
    c.edges[3] = focusFades || (eye && region.y + region.height < v.height) ? 1.0f : 0.0f;
    c.blend[0] = d.cfg.edgeBlend / 100.0f;
    c.blend[1] = d.cfg.autoExposure ? 1.0f : 0.0f;
    c.blend[2] = d.cfg.maxGain / 100.0f;
    const bool meterNow = d.cfg.autoExposure && d.meterFrame != d.frame;  // once a frame: every view the same exposure
    c.motion[2] = d.adapted ? 0.05f : 1.0f;
    context->UpdateSubresource(d.constants.Get(), 0, nullptr, &c, 0, 0);

    const D3D11_BOX box{v.x + region.x, v.y + region.y, 0, v.x + region.x + region.width, v.y + region.y + region.height, 1};
    context->CopySubresourceRegion(s.source.Get(), 0, 0, 0, 0, v.scene, 0, &box);
    ID3D11ShaderResourceView* const none[7] = {};
    if (meterNow) {
        d.meterFrame = d.frame;
        context->OMSetRenderTargets(1, d.meterRtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, s.sourceSrv.GetAddressOf());
        draw(context, d.meter.Get(), 64, 64);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->GenerateMips(d.meterSrv.Get());
        context->OMSetRenderTargets(1, d.adaptedRtv.GetAddressOf(), nullptr);
        context->OMSetBlendState(d.adaptBlend.Get(), nullptr, 0xffffffff);
        ID3D11ShaderResourceView* metered[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, d.meterSrv.Get()};
        context->PSSetShaderResources(0, 7, metered);
        draw(context, d.adapt.Get(), 1, 1);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(0, 7, none);
        d.adapted = true;
    }
    ID3D11RenderTargetView* inputs[3] = {s.colourRtv.Get(), s.depthRtv.Get(), s.vectorsRtv.Get()};
    context->OMSetRenderTargets(3, inputs, nullptr);
    ID3D11ShaderResourceView* prepared[6] = {s.sourceSrv.Get(), v.depth, v.vectors, nullptr, nullptr, d.adaptedSrv.Get()};
    context->PSSetShaderResources(0, 6, prepared);
    draw(context, d.prepare.Get(), ww, wh);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->PSSetShaderResources(0, 7, none);

    ngxnr::Evaluation e;
    e.intensity = d.cfg.intensity / 100.0f;
    e.localTone = d.cfg.localTone / 100.0f;
    e.localStructure = d.cfg.localStructure / 100.0f;
    e.style = static_cast<unsigned>(d.cfg.style);
    e.autoMask = d.cfg.autoMask;
    e.depthInverted = v.depthInverted;
    e.reset = !history;
    unsigned result = 0;
    const bool done = ngxnr::evaluate(context, s.view, e, result);
    s.ranFrame = d.frame;
    s.before = d.cfg.beforeDlss;
    if (!done && result == ngxnr::kBehind) {  // not submitted: the view stays as it came, and has no history from here
        ++d.behind;
        s.ranFrame = 0;
        return false;
    }
    d.ranThisFrame = true;
    if (!done) {
        if (d.failures < 5)
            say("frame %llu view %d: the evaluation failed (0x%08x)", d.frame, v.index, result);
        ++d.failures;
        s.ranFrame = 0;  // no history from it
        if (++d.failuresInARow >= 8)
            give_up("its evaluations keep failing");
        return false;
    }
    d.failuresInARow = 0;
    context->OMSetRenderTargets(1, s.resultRtv.GetAddressOf(), nullptr);
    ID3D11ShaderResourceView* composed[6] = {s.sourceSrv.Get(), nullptr, nullptr, s.outputSrv.Get(), s.colourSrv.Get(), d.adaptedSrv.Get()};
    context->PSSetShaderResources(0, 6, composed);
    draw(context, d.compose.Get(), region.width, region.height);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->PSSetShaderResources(0, 7, none);
    context->CopySubresourceRegion(v.scene, 0, v.x + region.x, v.y + region.y, 0, s.result.Get(), 0, nullptr);
    ++d.evaluations;
    ++d.evaluationsTotal;
    ++s.runs;
    if (d.savesLeft > 0 && v.index == 2 && s.runs % 600 == 30)
        save_pictures(context, s, c);
    return true;
}

void nr_frame_end(unsigned long long frame) {
    ngxnr::collect();  // released views whose work the GPU has finished
    // The breaker: frames that crawl with DLSS-NR's work in them. Not the frame a feature was made in.
    {
        const auto now = std::chrono::steady_clock::now();
        const bool slow = d.haveLastFrameEnd && d.ranThisFrame && !d.madeThisFrame && now - d.lastFrameEnd > kSlowFrame;
        d.slowFrames = slow ? d.slowFrames + 1 : d.ranThisFrame ? 0 : d.slowFrames;
        d.lastFrameEnd = now;
        d.haveLastFrameEnd = true;
        d.ranThisFrame = d.madeThisFrame = false;
        if (d.slowFrames >= kSlowFrames && d.cfg.on && !d.off) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "%d frames in a row took over %lld ms with it on: the GPU can't carry it at these sizes (lower the "
                          "working size or its limit)",
                          kSlowFrames, static_cast<long long>(kSlowFrame.count()));
            give_up(text);
        }
    }
    if (d.off) {  // given up: its features go (released; destroyed once the GPU is through them)
        for (Slot& s : d.slots)
            if (s.view)
                release_slot(s);
    }
    // Our Direct3D 12 device lost (a hang): everything of it is dropped, and DLSS-NR starts again on a new device, twice
    // a session at most (a third loss leaves it off until it is switched off and on).
    if (d.ready && ngxnr::device_lost()) {
        for (Slot& s : d.slots)
            release_slot(s);
        ComPtr<ID3D11DeviceContext> context;
        if (d.device)
            d.device->GetImmediateContext(&context);
        ngxnr::shutdown(context.Get());
        d.ready = false;
        d.createFailures = d.failuresInARow = d.slowFrames = 0;
        const bool again = d.restarts < 2;
        ++d.restarts;
        say("its Direct3D 12 device was lost: %s", again ? "starting again on a new one" : "off until it is switched off and on again");
        d.off = !again;
    }
    NrSettings live;
    bool take = false;
    {
        std::lock_guard guard(d.mutex);
        if (d.livePending) {
            live = d.live;
            // A change that makes features again waits until the settings have held (a slider stepped through); the
            // model's own settings follow at once.
            const bool settled = !live_remakes(live) || std::chrono::steady_clock::now() - d.liveAt >= kLiveSettle;
            if (settled) {
                d.livePending = false;
                take = true;
            } else {
                NrSettings now = live;
                now.on = d.cfg.on, now.preset = d.cfg.preset, now.periphery = d.cfg.periphery;
                now.focusScale = d.cfg.focusScale, now.peripheryScale = d.cfg.peripheryScale;
                now.peripheryCoverage = d.cfg.peripheryCoverage, now.beforeDlss = d.cfg.beforeDlss;
                now.workingLimit = d.cfg.workingLimit;
                live = now;
                take = !(now == d.cfg);
            }
        }
    }
    if (take && !(live == d.cfg)) {
        const bool was = d.cfg.on, remakes = live_remakes(live);
        live.folder = d.cfg.folder;  // another folder: the next launch
        d.cfg = live;
        if (was && !live.on) {
            for (Slot& s : d.slots)
                release_slot(s);
            say("live: off (the features released)");
        } else if (live.on) {
            if (!was) {
                d.off = false;  // on again: a session's earlier refusal is tried once more
                d.createFailures = d.failuresInARow = d.slowFrames = 0;
            }
            if (remakes) {  // new sizes: a refusal for memory tried again
                d.memoryLogged = false;
                std::memset(d.smallLogged, 0, sizeof(d.smallLogged));
                for (Slot& s : d.slots)
                    if (s.refused)
                        s = Slot{};  // no view in it
            }
            if (remakes && !live.periphery)
                for (int i = 0; i < 2; ++i)
                    release_slot(d.slots[i]);
            char text[512];
            describe(d.cfg, text, sizeof(text));
            say("live: %s%s", was ? "" : "on: ", text);
        }
    }
    if (!d.cfg.on)
        return;
    ++d.frame;
    if (frame - d.statsFrame >= 900) {
        d.statsFrame = frame;
        // The exposure meter's value, copied out now and read at the next line (ready by then: nothing waits).
        if (d.adaptedTex && d.adaptedRead && d.device && d.adapted) {
            ComPtr<ID3D11DeviceContext> context;
            d.device->GetImmediateContext(&context);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (d.readPending && SUCCEEDED(context->Map(d.adaptedRead.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                const uint16_t h = *static_cast<const uint16_t*>(m.pData);
                const int e = (h >> 10) & 0x1f, mant = h & 0x3ff;
                const double f = e == 0 ? std::ldexp(mant, -24) : e == 31 ? 0.0 : std::ldexp(mant | 0x400, e - 25);
                d.sceneMean = std::exp((h & 0x8000) ? -f : f);
                context->Unmap(d.adaptedRead.Get(), 0);
            }
            context->CopyResource(d.adaptedRead.Get(), d.adaptedTex.Get());
            d.readPending = true;
        }
        if (d.evaluations || d.failures || d.behind) {
            char views[4][64];
            for (int i = 0; i < 4; ++i) {
                const Slot& s = d.slots[i];
                const double ms = s.view ? ngxnr::take_gpu_ms(s.view) : 0;
                if (s.view && ms > 0)
                    std::snprintf(views[i], sizeof(views[i]), "%ux%u worked at %ux%u %.2f ms", s.region.width, s.region.height,
                                  s.workingW, s.workingH, ms);
                else if (s.view)
                    std::snprintf(views[i], sizeof(views[i]), "%ux%u worked at %ux%u (not timed yet)", s.region.width,
                                  s.region.height, s.workingW, s.workingH);
                else
                    std::snprintf(views[i], sizeof(views[i]), "-");
            }
            char scene[96] = "";
            if (d.cfg.autoExposure && d.sceneMean > 0)
                std::snprintf(scene, sizeof(scene), "; the scene's mean brightness %.4f, so its exposure x%.2f", d.sceneMean,
                              0.22 / d.sceneMean * d.cfg.exposure / 100.0);
            say("%llu evaluations, %llu failed, %llu left out (the GPU was behind), %llu features made again since the last line; GPU "
                "time an evaluation: eyes %s | %s, focus views %s | %s%s",
                d.evaluations, d.failures, d.behind, d.remade, views[0], views[1], views[2], views[3], scene);
        }
        d.evaluations = d.failures = d.remade = d.behind = 0;
    }
}

void nr_shutdown() {
    const bool had = d.ready;
    for (Slot& s : d.slots)
        release_slot(s);
    if (d.ready) {
        ComPtr<ID3D11DeviceContext> context;
        if (d.device)
            d.device->GetImmediateContext(&context);
        ngxnr::shutdown(context.Get());
    }
    d.ready = false;
    d.vs.Reset();
    d.prepare.Reset();
    d.compose.Reset();
    d.constants.Reset();
    d.linearClamp.Reset();
    d.meter.Reset(), d.adapt.Reset(), d.show.Reset();
    d.meterTex.Reset(), d.adaptedTex.Reset(), d.adaptedRead.Reset(), d.showTex.Reset();
    d.meterRtv.Reset(), d.adaptedRtv.Reset(), d.showRtv.Reset();
    d.meterSrv.Reset(), d.adaptedSrv.Reset();
    d.adaptBlend.Reset();
    d.adapted = d.readPending = false;
    d.showW = d.showH = 0;
    d.device.Reset();
    std::memset(d.firstLogged, 0, sizeof(d.firstLogged));
    std::memset(d.limitLogged, 0, sizeof(d.limitLogged));
    std::memset(d.smallLogged, 0, sizeof(d.smallLogged));
    d.restarts = 0;
    d.memoryLogged = d.haveLastFrameEnd = false;
    d.slowFrames = 0;
    if (had)
        say("shut down (features released, NGX on Direct3D 12 stopped)");
}

unsigned long long nr_evaluations_total() { return d.evaluationsTotal; }
unsigned nr_pending_releases() { return ngxnr::pending_releases(); }

bool nr_view_for_checks(int index, unsigned region[4], unsigned working[2]) {
    if (index < 0 || index >= 4 || !d.slots[index].view)
        return false;
    const Slot& s = d.slots[index];
    region[0] = s.region.x, region[1] = s.region.y, region[2] = s.region.width, region[3] = s.region.height;
    working[0] = s.workingW, working[1] = s.workingH;
    return true;
}

}  // namespace simqvfr

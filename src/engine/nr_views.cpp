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
#include <mutex>

#include "../log.h"
#include "ngx_nr.h"
#include "nr_math.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr {
namespace {

// Two full-screen triangles' worth of work a view:
//   prepare (at the working size): the region's colour through the curve into the model's colour input, the view's
//     depth and motion vectors for the region's texels into its other two;
//   compose (at the region's size): the picture as it came plus the model's change of it (its output less its input,
//     both sampled at the working size), back through the curve, faded to the picture as it came along the edges marked.
constexpr char kShaders[] = R"(
cbuffer C : register(b0) {
    float4 region;   // the region in the view: x, y, width, height (the colour's pixels)
    float4 view;     // the view's size in the colour's pixels; the guides' size
    float4 working;  // the working size; the exposure; 1: sRGB's curve
    float4 motion;   // xy: how far the region moved in the view since the frame before, as a fraction of the region
    float4 edges;    // 1 where that side fades: left, top, right, bottom
    float4 blend;    // x: the band, a fraction of the region
};
Texture2D<float4> sourceIn : register(t0);   // the region as it came
Texture2D<float> depthIn : register(t1);     // prepare: the view's depth
Texture2D<float2> vectorsIn : register(t2);  // prepare: the view's motion vectors
Texture2D<float4> modelOut : register(t3);   // compose: the model's output
Texture2D<float4> modelIn : register(t4);    // compose: the model's input
SamplerState linearClamp : register(s0);

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 to_srgb(float3 c) { return lerp(12.92 * c, 1.055 * pow(max(c, 1e-8), 1 / 2.4) - 0.055, step(0.0031308, c)); }
float3 from_srgb(float3 c) { return lerp(c / 12.92, pow(max((c + 0.055) / 1.055, 1e-8), 2.4), step(0.04045, c)); }
float3 encode(float3 c) {
    c = max(c, 0) * working.z;
    c = c / (1 + max(c.r, max(c.g, c.b)));
    return working.w > 0.5 ? to_srgb(c) : c;
}
float3 decode(float3 e) {
    e = saturate(e);
    if (working.w > 0.5)
        e = from_srgb(e);
    float m = min(max(e.r, max(e.g, e.b)), 0.9995);
    return e / (1 - m) / working.z;
}
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
    o.colour = float4(encode(sourceIn.SampleLevel(linearClamp, uv, 0).rgb), 1);
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
    float3 change = modelOut.SampleLevel(linearClamp, uv, 0).rgb - modelIn.SampleLevel(linearClamp, uv, 0).rgb;
    float3 made = decode(encode(came.rgb) + change);
    float w = fade(uv.x, edges.x) * fade(uv.y, edges.y) * fade(1 - uv.x, edges.z) * fade(1 - uv.y, edges.w);
    return float4(lerp(came.rgb, made, w), came.a);
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
    ComPtr<ID3D11ShaderResourceView> colourSrv, outputSrv, sourceSrv;
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
    ComPtr<ID3D11PixelShader> prepare, compose;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearClamp;
    Slot slots[4];
    int createFailures = 0, failuresInARow = 0;
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

void release_slot(Slot& s) {
    if (s.view)
        ngxnr::release(s.view);
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
                  "%s, the focus views worked at %d%%, %s DLSS; preset %s, style %s, intensity %d%%, local tone %d%%, local "
                  "structure %d%%, auto mask %s, edge blend %d%%, exposure %d%%, input %s",
                  periphery, c.focusScale, c.beforeDlss ? "before" : "after", preset_name(c.preset), style_name(c.style), c.intensity,
                  c.localTone, c.localStructure, c.autoMask ? "on" : "off", c.edgeBlend, c.exposure,
                  c.inputGamma ? "sRGB-encoded" : "linear");
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
             make(s.result, region.width, region.height, format, D3D11_BIND_RENDER_TARGET) &&
             SUCCEEDED(d.device->CreateShaderResourceView(s.source.Get(), nullptr, &s.sourceSrv)) &&
             SUCCEEDED(d.device->CreateRenderTargetView(s.result.Get(), nullptr, &s.resultRtv));
    }
    if (!ok) {
        say("view %d's feature for a %ux%u region worked at %ux%u (format %d) couldn't be made (%s, 0x%08x)", index, region.width,
            region.height, ww, wh, static_cast<int>(format), s.view ? "its Direct3D 11 views" : "NGX or the shared textures", result);
        if (s.view)
            ngxnr::release(s.view);
        s.view = nullptr;
        s.refused = true;
        if (++d.createFailures >= 4)
            give_up("the features can't be made");
        return false;
    }
    say("view %d's feature made in %.0f ms: a %ux%u region worked at %ux%u (%.2f megapixels), preset %s", index, ms, region.width,
        region.height, ww, wh, ww * static_cast<double>(wh) / 1e6, preset_name(d.cfg.preset));
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

// Does `l` need features made again (or released), against the settings in use?
bool live_remakes(const NrSettings& l) {
    const NrSettings& c = d.cfg;
    return l.on != c.on || l.preset != c.preset || l.periphery != c.periphery || l.focusScale != c.focusScale ||
           l.peripheryScale != c.peripheryScale || l.peripheryCoverage != c.peripheryCoverage || l.beforeDlss != c.beforeDlss;
}

constexpr auto kLiveSettle = std::chrono::milliseconds(500);

}  // namespace

void nr_start(const char* tag, const NrSettings& settings) {
    {
        std::lock_guard guard(d.mutex);
        d.livePending = false;
    }
    d.tag = tag ? tag : "dlss-nr";
    d.cfg = nr_clamped(settings);
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
    const unsigned ww = nr::working(region.width, scale), wh = nr::working(region.height, scale);
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
    context->UpdateSubresource(d.constants.Get(), 0, nullptr, &c, 0, 0);

    const D3D11_BOX box{v.x + region.x, v.y + region.y, 0, v.x + region.x + region.width, v.y + region.y + region.height, 1};
    context->CopySubresourceRegion(s.source.Get(), 0, 0, 0, 0, v.scene, 0, &box);
    ID3D11ShaderResourceView* const none[3] = {};
    ID3D11RenderTargetView* inputs[3] = {s.colourRtv.Get(), s.depthRtv.Get(), s.vectorsRtv.Get()};
    context->OMSetRenderTargets(3, inputs, nullptr);
    ID3D11ShaderResourceView* prepared[3] = {s.sourceSrv.Get(), v.depth, v.vectors};
    context->PSSetShaderResources(0, 3, prepared);
    draw(context, d.prepare.Get(), ww, wh);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->PSSetShaderResources(0, 3, none);

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
    ID3D11ShaderResourceView* composed[5] = {s.sourceSrv.Get(), nullptr, nullptr, s.outputSrv.Get(), s.colourSrv.Get()};
    context->PSSetShaderResources(0, 5, composed);
    draw(context, d.compose.Get(), region.width, region.height);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* const none5[5] = {};
    context->PSSetShaderResources(0, 5, none5);
    context->CopySubresourceRegion(v.scene, 0, v.x + region.x, v.y + region.y, 0, s.result.Get(), 0, nullptr);
    ++d.evaluations;
    ++d.evaluationsTotal;
    return true;
}

void nr_frame_end(unsigned long long frame) {
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
                d.createFailures = d.failuresInARow = 0;
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
        if (d.evaluations || d.failures) {
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
            say("%llu evaluations, %llu failed, %llu features made again since the last line; GPU time an evaluation: eyes %s | %s, "
                "focus views %s | %s",
                d.evaluations, d.failures, d.remade, views[0], views[1], views[2], views[3]);
        }
        d.evaluations = d.failures = d.remade = 0;
    }
}

void nr_shutdown() {
    const bool had = d.ready;
    for (Slot& s : d.slots)
        release_slot(s);
    if (d.ready)
        ngxnr::shutdown();
    d.ready = false;
    d.vs.Reset();
    d.prepare.Reset();
    d.compose.Reset();
    d.constants.Reset();
    d.linearClamp.Reset();
    d.device.Reset();
    std::memset(d.firstLogged, 0, sizeof(d.firstLogged));
    if (had)
        say("shut down (features released, NGX on Direct3D 12 stopped)");
}

unsigned long long nr_evaluations_total() { return d.evaluationsTotal; }

bool nr_view_for_checks(int index, unsigned region[4], unsigned working[2]) {
    if (index < 0 || index >= 4 || !d.slots[index].view)
        return false;
    const Slot& s = d.slots[index];
    region[0] = s.region.x, region[1] = s.region.y, region[2] = s.region.width, region[3] = s.region.height;
    working[0] = s.workingW, working[1] = s.workingH;
    return true;
}

}  // namespace simqvfr

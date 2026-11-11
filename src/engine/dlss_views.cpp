// DLAA at the API level (dlss_views.h).
#include "dlss_views.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#include "../log.h"
#include "dlss_math.h"
#include "ngx_dlss.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr {
namespace {

using dlss::M4;
namespace ngx = simqvfr::ngx;

// One full-screen triangle; the pixel shader resolves the view's depth (sample 0 of the scene's multisample depth, or
// the texel of a single-sample one) and writes its motion vector: the pixel reprojected into the frame before.
constexpr char kVectorsShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;    // this frame's clip (as rendered) -> the frame before's (without its jitter): the world
    row_major float4x4 R2;   // the same for what is fixed to the player's seat (the head's motion alone)
    float4 js;               // this frame's clip shift x, y; the view's width, height
    float4 origin;           // the view's top-left in the depth texture; the projection's depth terms
    float4 extra;            // x: 1 with the car's box on
    row_major float4x4 T;    // this frame's clip -> the tracking space (through the head's view)
    float4 head;             // the eye's position in the tracking space
    float4 boxMin;           // the car's box about it: x right, y up, z back
    float4 boxMax;
};
#ifdef MS
Texture2DMS<float> depthIn : register(t0);
#else
Texture2D<float> depthIn : register(t0);
#endif
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
bool in_car(float2 ndc, float d) {
    if (extra.x <= 0)
        return false;
    float4 q = mul(float4(ndc, d, 1), T);
    if (abs(q.w) < 1e-9)
        return false;
    float3 rel = q.xyz / q.w - head.xyz;
    return all(rel > boxMin.xyz) && all(rel < boxMax.xyz);
}
struct Out {
    float depth : SV_Target0;
    float2 motion : SV_Target1;
};
Out ps(float4 pos : SV_Position) {
    int2 p = int2(pos.xy) + int2(origin.xy);
#ifdef MS
    float d = depthIn.Load(p, 0);
#else
    float d = depthIn.Load(int3(p, 0));
#endif
    float2 ndc = float2(pos.x / js.z * 2 - 1, 1 - pos.y / js.w * 2);
    float4 before = in_car(ndc, d) ? mul(float4(ndc, d, 1), R2) : mul(float4(ndc, d, 1), R);
    Out o;
    o.depth = d;
    o.motion = before.w > 1e-6 ? (before.xy / before.w - (ndc - js.xy)) * float2(0.5 * js.z, -0.5 * js.w) : float2(0, 0);
    return o;
}
)";

// The gate: over a 64x64 grid, 16 points a cell, apart for the world (first target) and what is taken for the player's
// car (in its box, its vector the head's motion alone; with the box off: nearer than 8 m, the world's vectors; second
// target), each point looked up in the frame before at its own jitter (that frame was rendered with it). Its colour,
// both frames after the same small blur (a 3x3 tent: four bilinear taps half a texel off): how much the frame differs
// from the one before where the point stood (x), where its vector says it was (y), a pixel off that (z, four ways); the
// points counted (w). Its depth (third and fourth targets), which detail finer than a pixel doesn't scramble as it does
// the colour: whether the frame before's depth where the vector says the point was is the depth the reprojection
// predicts for it (within 2%), and whether it is this frame's where the point stood; for points within 1 km. Third
// target: the world's summed vector length in pixels, its points whose depth agrees along the vectors, standing; the
// car's summed length. Fourth: the car's agreeing along the vectors, standing; the points depth judged, the world's and
// the car's.
constexpr char kCheckShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;
    row_major float4x4 R2;
    float4 js;
    float4 origin;           // z, w: the projection's depth terms (depth = z + w / distance)
    float4 extra;            // x: 1 with the car's box on; y, z: the frame before's clip shift (its jitter as rendered)
    row_major float4x4 T;    // this frame's clip -> the tracking space (through the head's view)
    float4 head;             // the eye's position in the tracking space
    float4 boxMin;           // the car's box about it: x right, y up, z back
    float4 boxMax;
};
Texture2D<float4> nowIn : register(t0);
Texture2D<float4> beforeIn : register(t1);
Texture2D<float> depthIn : register(t2);
Texture2D<float> depthBeforeIn : register(t3);
SamplerState linearClamp : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float lum(float3 c) { return log2(1 + dot(max(c, 0), float3(0.2126, 0.7152, 0.0722))); }
// Whether `predicted` lies within the depths about uv (the four texels a bilinear lookup there would blend), 2% wider:
// a point between texels on a slope (the road far ahead changes depth faster than 2% a pixel in a small view) and an
// edge the jitter moved (the point's own side among them) both agree.
bool agrees(float2 uv, float predicted) {
    float4 seen = depthBeforeIn.GatherRed(linearClamp, uv);
    float lo = min(min(seen.x, seen.y), min(seen.z, seen.w)), hi = max(max(seen.x, seen.y), max(seen.z, seen.w));
    float slack = 0.02 * max(abs(predicted), 1e-7);
    return predicted >= lo - slack && predicted <= hi + slack;
}
float tent(Texture2D<float4> t, float2 uv) {
    float2 h = 0.5 / js.zw;
    float3 c = t.SampleLevel(linearClamp, uv + float2(-h.x, -h.y), 0).rgb + t.SampleLevel(linearClamp, uv + float2(h.x, -h.y), 0).rgb +
               t.SampleLevel(linearClamp, uv + float2(-h.x, h.y), 0).rgb + t.SampleLevel(linearClamp, uv + h, 0).rgb;
    return lum(0.25 * c);
}
float around(Texture2D<float4> t, float2 uv, float v) {
    float2 px = 1 / js.zw;
    return 0.25 * (abs(v - tent(t, uv + float2(px.x, 0))) + abs(v - tent(t, uv - float2(px.x, 0))) +
                   abs(v - tent(t, uv + float2(0, px.y))) + abs(v - tent(t, uv - float2(0, px.y))));
}
bool where(float2 ndc, float d, float4x4 m, out float2 uv, out float predicted) {
    float4 b = mul(float4(ndc, d, 1), m);
    uv = float2(0, 0);
    predicted = 0;
    if (b.w <= 1e-6)
        return false;
    float2 n = b.xy / b.w + extra.yz;
    uv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    predicted = b.z / b.w;
    return all(abs(n) < 1);
}
bool in_car(float2 ndc, float d) {
    if (extra.x <= 0)
        return false;
    float4 q = mul(float4(ndc, d, 1), T);
    if (abs(q.w) < 1e-9)
        return false;
    float3 rel = q.xyz / q.w - head.xyz;
    return all(rel > boxMin.xyz) && all(rel < boxMax.xyz);
}
struct Out {
    float4 world : SV_Target0;
    float4 car : SV_Target1;
    float4 more : SV_Target2;
    float4 carDepth : SV_Target3;
};
Out ps(float4 pos : SV_Position) {
    Out o;
    o.world = 0;
    o.car = 0;
    o.more = 0;
    o.carDepth = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            float2 at = (floor(pos.xy) + (float2(i, j) + 0.5) / 4) / 64;
            int2 p = int2(at * js.zw);
            float2 c = (float2(p) + 0.5) / js.zw;
            float2 ndc = float2(c.x * 2 - 1, 1 - c.y * 2);
            float d = depthIn.Load(int3(p, 0));
            float2 a;
            float away = d - origin.z;
            float metres = abs(away) > 1e-9 ? origin.w / away : 1e9;
            bool car = extra.x > 0 ? in_car(ndc, d) : metres > 0 && metres < 8;
            bool on;
            float predicted;
            if (car && extra.x > 0)
                on = where(ndc, d, R2, a, predicted);
            else
                on = where(ndc, d, R, a, predicted);
            if (!on)
                continue;
            float now = tent(nowIn, c);
            float4 pt = float4(abs(now - tent(beforeIn, c)), abs(now - tent(beforeIn, a)), around(beforeIn, a, now), 1);
            float len = length((a - c) * js.zw);
            // Depth judges points within 1 km: farther (the sky at the far plane, depth 0) nothing moves with the
            // car's travel, and a reversed depth that near 0 is below what a 2% tolerance can tell from rounding.
            float judged = metres > 0 && metres < 1000 ? 1 : 0;
            float alongOk = judged * (agrees(a, predicted) ? 1 : 0);
            float stillOk = judged * (agrees(c, d) ? 1 : 0);
            if (car) {
                o.car += pt;
                o.more.w += len;
                o.carDepth.xyw += float3(alongOk, stillOk, judged);
            } else {
                o.world += pt;
                o.more.xyz += float3(len, alongOk, stillOk);
                o.carDepth.z += judged;
            }
        }
    return o;
}
)";

// A focus view drawn smaller that DLSS didn't upscale this frame: its render stretched over its region (bilinear).
constexpr char kStretchShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 R;
    row_major float4x4 R2;
    float4 js;               // z, w: the region's width, height
};
Texture2D<float4> src : register(t0);
SamplerState linearClamp : register(s0);
float4 ps(float4 pos : SV_Position) : SV_Target { return src.SampleLevel(linearClamp, pos.xy / js.zw, 0); }
)";

struct Constants {
    float r[16], r2[16], js[4], origin[4], extra[4], t[16], head[4], boxMin[4], boxMax[4];
};

// The car's box for a view this frame (dlss_math.h in_car_box): off without the eye's pose.
struct CarTest {
    bool on = false;
    M4 clipToTracking = dlss::identity();
    double head[3] = {};
};

struct Jitter {
    bool on = false;
    double sx = 0, sy = 0;  // pixels the image moves (x right, y down)
    double cx = 0, cy = 0;  // the clip shift for it
};

struct Slot {  // a view: eye 0, eye 1, focus 0, focus 1
    unsigned width = 0, height = 0;        // what it's drawn at (its inputs)
    unsigned outWidth = 0, outHeight = 0;  // what DLSS makes of it (the same: DLAA)
    int preset = 0;                        // NGX's render preset it was made with (DlssRun::preset, eyePreset)
    bool inverted = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ngx::Feature* feature = nullptr;
    bool refused = false;  // the feature couldn't be made at this size
    ComPtr<ID3D11Texture2D> depth, vectors, output, colour[2], depthBefore;
    ComPtr<ID3D11RenderTargetView> depthRtv, vectorsRtv;
    ComPtr<ID3D11ShaderResourceView> depthSrv, colourSrv[2], depthBeforeSrv;
    int colourAt = 0;
    unsigned long long colourFrame[2] = {}, depthBeforeFrame = 0;
    M4 beforeUnjittered[2] = {dlss::identity(), dlss::identity()};  // through the world's view; through the head's
    double beforeShift[2] = {};  // the frame before's clip shift as rendered (its jitter): where its colour sits
    unsigned long long beforeFrame = 0;
    bool haveBefore = false;
    unsigned long long runs = 0;  // frames this view went through DLAA at this size
};

struct State {
    DlssRun cfg;
    bool off = false;  // gave up for the session (the log says why)
    bool ready = false;
    unsigned long long frame = 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> vectorsMs, vectorsSs, check, stretch;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearClamp;
    ComPtr<ID3D11Texture2D> checkTarget[4], checkRead[4];
    ComPtr<ID3D11RenderTargetView> checkRtv[4];
    // The scene's depth target as a shader resource.
    ID3D11Resource* depthFor = nullptr;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    bool depthMs = false, depthRefusedLogged = false;
    Slot slots[4];
    int createFailures = 0;
    bool firstLogged = false;
    // Super resolution on the focus views (DlssRun::focusScale): off for the session once NGX refuses a feature for it.
    bool focusSrOff = false;
    unsigned phases = dlss::kJitterPhases;  // the jitter sequence's length (longer for a smaller render)
    // The stretch for a focus view DLSS didn't upscale (stretch_focus): its render and its region, at these sizes.
    ComPtr<ID3D11Texture2D> stretchIn, stretchOut;
    ComPtr<ID3D11ShaderResourceView> stretchSrv;
    ComPtr<ID3D11RenderTargetView> stretchRtv;
    unsigned stretchSize[4] = {};
    DXGI_FORMAT stretchFormat = DXGI_FORMAT_UNKNOWN;
    bool stretchRefusedLogged = false;
    // Jitter (the mutex: xrLocateViews may run on another thread than the passes).
    std::mutex mutex;
    unsigned phase = 0;
    bool lastFrameOk = false;       // both eyes were evaluated in the frame before: jitter may go on
    unsigned eyeW[2] = {}, eyeH[2] = {};
    Jitter eye[2], focus[2];
    XrPosef pose[2] = {};  // the eyes' poses as last located (the tracking space)
    bool poseValid[2] = {};
    // Does the located FOV's shift reach the game's projection, and with which sign? Per axis, over the first frames
    // with jitter: the projection's third-row step against the step asked for.
    double signSame[2] = {}, signOpposite[2] = {}, signMoved[2] = {};
    double lastThird[2][2] = {}, lastAsked[2][2] = {};
    bool haveThird[2] = {};
    int signSamples = 0;
    bool signDecided = false, jitterDead = false;
    int sign[2] = {1, 1};
    // This frame, and since the last log line.
    bool evaluated[2] = {};
    unsigned long long evaluations = 0, failures = 0, recreated = 0, jitteredFrames = 0, statsFrame = 0, stretched = 0;
    unsigned long long evaluationsTotal = 0, stretchedTotal = 0;
    PassMatricesFn matrices = nullptr;  // the game's (dlss_set_matrices)
} d;

// A log line with the run's tag in front ("lmu dlss: ...").
void say(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    simqvfr::logf("%s: %s", d.cfg.tag ? d.cfg.tag : "dlss", text);
}

void give_up(const char* why) {
    if (!d.off)
        say("off for this session: %s", why);
    d.off = true;
}

ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* profile, const D3D_SHADER_MACRO* macros) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "dlss", macros, nullptr, entry, profile, 0, 0, &code, &errors))) {
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
    const D3D_SHADER_MACRO ms[] = {{"MS", "1"}, {nullptr, nullptr}};
    const auto vs = compile(kVectorsShader, "vs", "vs_5_0", nullptr);
    const auto psMs = compile(kVectorsShader, "ps", "ps_5_0", ms);
    const auto psSs = compile(kVectorsShader, "ps", "ps_5_0", nullptr);
    const auto check = compile(kCheckShader, "ps", "ps_5_0", nullptr);
    const auto stretch = compile(kStretchShader, "ps", "ps_5_0", nullptr);
    if (!vs || !psMs || !psSs || !check || !stretch ||
        FAILED(d.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d.vs)) ||
        FAILED(d.device->CreatePixelShader(psMs->GetBufferPointer(), psMs->GetBufferSize(), nullptr, &d.vectorsMs)) ||
        FAILED(d.device->CreatePixelShader(psSs->GetBufferPointer(), psSs->GetBufferSize(), nullptr, &d.vectorsSs)) ||
        FAILED(d.device->CreatePixelShader(check->GetBufferPointer(), check->GetBufferSize(), nullptr, &d.check)) ||
        FAILED(d.device->CreatePixelShader(stretch->GetBufferPointer(), stretch->GetBufferSize(), nullptr, &d.stretch))) {
        give_up("its shaders couldn't be made");
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    bool ok = SUCCEEDED(d.device->CreateBuffer(&bd, nullptr, &d.constants)) &&
              SUCCEEDED(d.device->CreateSamplerState(&sd, &d.linearClamp));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    for (int i = 0; i < 4 && ok; ++i) {
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        td.CPUAccessFlags = 0;
        ok = SUCCEEDED(d.device->CreateTexture2D(&td, nullptr, &d.checkTarget[i])) &&
             SUCCEEDED(d.device->CreateRenderTargetView(d.checkTarget[i].Get(), nullptr, &d.checkRtv[i]));
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = ok && SUCCEEDED(d.device->CreateTexture2D(&td, nullptr, &d.checkRead[i]));
    }
    if (!ok) {
        give_up("its buffers couldn't be made");
        return false;
    }
    std::string why;
    const std::wstring data = (std::filesystem::path(simqvfr::log_directory()) / L"ngx").wstring();
    if (!ngx::start(d.device.Get(), d.cfg.dllFolder.c_str(), data.c_str(), why)) {
        give_up(why.c_str());
        return false;
    }
    char focusMode[96] = "";
    if (d.cfg.views >= 2)
        std::snprintf(focusMode, sizeof(focusMode), " and the focus views (%s, %d%%)", dlss::focus_mode_name(d.cfg.focusScale),
                      d.cfg.focusScale);
    say("NGX started on the game's device (nvngx_dlss.dll from %ls); views: the eyes%s; jitter %s (%u phases); "
                  "presets: the eyes %s, the focus views %s",
                  d.cfg.dllFolder.empty() ? L"the game's folder" : d.cfg.dllFolder.c_str(), focusMode,
                  d.cfg.jitter ? "on" : "off", d.phases,
                  dlss::preset_name(d.cfg.eyePreset >= 0 ? d.cfg.eyePreset : d.cfg.preset), dlss::preset_name(d.cfg.preset));
    d.ready = true;
    return true;
}

// The scene's depth target as a shader resource (its first sample read by the vectors' shader).
bool depth_view(ID3D11DepthStencilView* dsv, unsigned& width, unsigned& height) {
    ComPtr<ID3D11Resource> resource;
    if (dsv)
        dsv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)))
        return false;
    D3D11_TEXTURE2D_DESC td;
    texture->GetDesc(&td);
    width = td.Width;
    height = td.Height;
    if (resource.Get() == d.depthFor)
        return d.depthSrv != nullptr;
    d.depthFor = resource.Get();
    d.depthSrv.Reset();
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    switch (td.Format) {
    case DXGI_FORMAT_R32_TYPELESS: format = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R24G8_TYPELESS: format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS: format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R16_TYPELESS: format = DXGI_FORMAT_R16_UNORM; break;
    default: break;
    }
    d.depthMs = td.SampleDesc.Count > 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = format;
    sd.ViewDimension = d.depthMs ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (format == DXGI_FORMAT_UNKNOWN || !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        FAILED(d.device->CreateShaderResourceView(resource.Get(), &sd, &d.depthSrv))) {
        d.depthSrv.Reset();
        if (!d.depthRefusedLogged) {
            d.depthRefusedLogged = true;
            say("the scene's depth target can't be read (%ux%u format %d x%u, bind flags 0x%x)", td.Width,
                          td.Height, static_cast<int>(td.Format), td.SampleDesc.Count, td.BindFlags);
        }
        return false;
    }
    say("depth from the scene's depth target (%ux%u format %d x%u)", td.Width, td.Height,
                  static_cast<int>(td.Format), td.SampleDesc.Count);
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

// The view's feature and textures at these sizes, drawn at w x h and made into ow x oh (made again when they change: a
// live resolution change, super resolution starting or stopping).
bool ensure(ID3D11DeviceContext* context, Slot& s, int index, unsigned w, unsigned h, unsigned ow, unsigned oh,
            DXGI_FORMAT format, bool inverted) {
    const int preset = index < 2 && d.cfg.eyePreset >= 0 ? d.cfg.eyePreset : d.cfg.preset;  // the eyes are views 0-1
    if (s.width == w && s.height == h && s.outWidth == ow && s.outHeight == oh && s.format == format &&
        s.inverted == inverted && s.preset == preset)
        return s.feature != nullptr;
    if (s.feature) {
        ngx::release(s.feature);
        ++d.recreated;
    }
    s = Slot{};
    s.width = w;
    s.height = h;
    s.outWidth = ow;
    s.outHeight = oh;
    s.preset = preset;
    s.format = format;
    s.inverted = inverted;
    const bool upscale = ow != w || oh != h;
    const UINT targets = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bool ok = make(s.depth, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
              make(s.vectors, w, h, DXGI_FORMAT_R16G16_FLOAT, targets) && make(s.output, ow, oh, format, targets) &&
              SUCCEEDED(d.device->CreateRenderTargetView(s.depth.Get(), nullptr, &s.depthRtv)) &&
              SUCCEEDED(d.device->CreateRenderTargetView(s.vectors.Get(), nullptr, &s.vectorsRtv)) &&
              SUCCEEDED(d.device->CreateShaderResourceView(s.depth.Get(), nullptr, &s.depthSrv));
    for (int i = 0; i < 2 && ok && d.cfg.gate; ++i)
        ok = make(s.colour[i], w, h, format, D3D11_BIND_SHADER_RESOURCE) &&
             SUCCEEDED(d.device->CreateShaderResourceView(s.colour[i].Get(), nullptr, &s.colourSrv[i]));
    if (ok && d.cfg.gate)  // the gate's depth test: the frame before's depth
        ok = make(s.depthBefore, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE) &&
             SUCCEEDED(d.device->CreateShaderResourceView(s.depthBefore.Get(), nullptr, &s.depthBeforeSrv));
    unsigned result = 0;
    if (ok)
        s.feature = ngx::create(context, w, h, ow, oh, inverted, preset, result);
    if (!s.feature) {
        s.refused = true;
        say("view %d's feature at %ux%u%s (preset %s, format %d) couldn't be made (%s, NGX 0x%08x)", index, w, h,
            upscale ? " for an upscale" : "", dlss::preset_name(preset), static_cast<int>(format),
            ok ? "NGX refused" : "its textures", result);
        if (upscale) {  // the focus views go back to their size and DLAA (from the next frame: dlss_focus_scale)
            if (!d.focusSrOff)
                say("super resolution on the focus views off for this session: they're drawn at their size with DLAA");
            d.focusSrOff = true;
        } else if (++d.createFailures >= 4) {
            give_up("the features can't be made");
        }
        return false;
    }
    // NGX's own line on the preset it took for the feature's mode follows (ngx_dlss.cpp), "ngx: (DLAA) Using App hint
    // Preset F" or "... using title default Preset K".
    if (upscale)
        say("view %d's DLSS feature made at %ux%u up to %ux%u (%s, preset %s, format %d, depth %s)", index, w, h, ow, oh,
            dlss::focus_mode_name(d.cfg.focusScale), dlss::preset_name(preset), static_cast<int>(format),
            inverted ? "inverted" : "0 near");
    else
        say("view %d's DLAA feature made at %ux%u (preset %s, format %d, depth %s)", index, w, h, dlss::preset_name(preset),
            static_cast<int>(format), inverted ? "inverted" : "0 near");
    return true;
}

void to_floats(const M4& m, float* out) {
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<float>(m[i]);
}

// The pass's view and projection, from the game (dlss_set_matrices): a rigid view and a projection of the shape.
bool pass_matrices(int eye, M4& view, M4& projection) {
    float v[16], p[16];
    if (!d.matrices || !d.matrices(eye, v, p))
        return false;
    view = dlss::from_floats(v);
    projection = dlss::from_floats(p);
    return dlss::projection(projection) && dlss::rigid(view);
}

// The eye's jitter as it reached the projection: the third row's step from the frame before against the step asked for.
void note_projection(int eye, const M4& projection, const Jitter& asked) {
    if (d.signDecided || d.jitterDead)
        return;
    const double third[2] = {projection[8], projection[9]};
    const double want[2] = {asked.on ? asked.cx : 0, asked.on ? asked.cy : 0};
    if (d.haveThird[eye] && (asked.on || d.lastAsked[eye][0] != 0 || d.lastAsked[eye][1] != 0)) {
        for (int a = 0; a < 2; ++a) {
            const double moved = third[a] - d.lastThird[eye][a], step = want[a] - d.lastAsked[eye][a];
            d.signSame[a] += (moved - step) * (moved - step);
            d.signOpposite[a] += (moved + step) * (moved + step);
            d.signMoved[a] += moved * moved;
        }
        ++d.signSamples;
    }
    d.haveThird[eye] = true;
    for (int a = 0; a < 2; ++a) {
        d.lastThird[eye][a] = third[a];
        d.lastAsked[eye][a] = want[a];
    }
    if (d.signSamples < 64)
        return;
    if (d.signMoved[0] < 1e-14 && d.signMoved[1] < 1e-14) {
        d.jitterDead = true;
        say("the located FOV's jitter doesn't reach the game's projection (its third row stood still "
                      "over %d frames): jitter off",
                      d.signSamples);
        return;
    }
    d.signDecided = true;
    for (int a = 0; a < 2; ++a)
        d.sign[a] = d.signSame[a] <= d.signOpposite[a] ? 1 : -1;
    say("the jitter reaches the game's projection: x %s (misfit %.3g as asked, %.3g negated), y %s (%.3g, "
                  "%.3g), over %d frames",
                  d.sign[0] > 0 ? "as asked" : "NEGATED", d.signSame[0], d.signOpposite[0],
                  d.sign[1] > 0 ? "as asked" : "NEGATED", d.signSame[1], d.signOpposite[1], d.signSamples);
}

// The immediate context's state the module's draws and NGX's evaluation may change, read before the work and put back
// after it, call by call. (Not a context state swap, ID3D11DeviceContext1::SwapDeviceContextState: in the run of
// 2026-11-10 15:53 the multi-view pass tracker then saw each eye pass end right after its resolve and no focus view
// was made.) Everything here happens inside the caller's window, in which its hooks pass calls through.
class StateGuard {
public:
    explicit StateGuard(ID3D11DeviceContext* c) : c_(c) {
        c->IAGetInputLayout(&layout_);
        c->IAGetPrimitiveTopology(&topology_);
        c->IAGetVertexBuffers(0, kBuffers, vb_, strides_, offsets_);
        c->IAGetIndexBuffer(&ib_, &ibFormat_, &ibOffset_);
        vsInstances_ = psInstances_ = csInstances_ = kInstances;
        c->VSGetShader(&vs_, vsI_, &vsInstances_);
        c->PSGetShader(&ps_, psI_, &psInstances_);
        c->CSGetShader(&cs_, csI_, &csInstances_);
        c->GSGetShader(&gs_, nullptr, nullptr);
        c->HSGetShader(&hs_, nullptr, nullptr);
        c->DSGetShader(&ds_, nullptr, nullptr);
        c->VSGetConstantBuffers(0, kCbs, vsCb_);
        c->PSGetConstantBuffers(0, kCbs, psCb_);
        c->CSGetConstantBuffers(0, kCbs, csCb_);
        c->VSGetShaderResources(0, kSrvs, vsSrv_);
        c->PSGetShaderResources(0, kSrvs, psSrv_);
        c->CSGetShaderResources(0, kSrvs, csSrv_);
        c->VSGetSamplers(0, kSamplers, vsSamp_);
        c->PSGetSamplers(0, kSamplers, psSamp_);
        c->CSGetSamplers(0, kSamplers, csSamp_);
        c->CSGetUnorderedAccessViews(0, kUavs, csUav_);
        c->OMGetRenderTargets(kTargets, rtv_, &dsv_);
        c->OMGetBlendState(&blend_, blendFactor_, &sampleMask_);
        c->OMGetDepthStencilState(&depthState_, &stencilRef_);
        c->RSGetState(&raster_);
        viewports_ = scissors_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetViewports(&viewports_, vp_);
        c->RSGetScissorRects(&scissors_, sc_);
    }
    ~StateGuard() {
        ID3D11DeviceContext* c = c_;
        // Ours and NGX's inputs and outputs off first, so nothing put back meets a texture still bound the other way.
        ID3D11ShaderResourceView* noSrv[kSrvs] = {};
        ID3D11UnorderedAccessView* noUav[kUavs] = {};
        c->OMSetRenderTargets(0, nullptr, nullptr);
        c->CSSetUnorderedAccessViews(0, kUavs, noUav, nullptr);
        c->VSSetShaderResources(0, kSrvs, noSrv);
        c->PSSetShaderResources(0, kSrvs, noSrv);
        c->CSSetShaderResources(0, kSrvs, noSrv);
        c->IASetInputLayout(layout_);
        c->IASetPrimitiveTopology(topology_);
        c->IASetVertexBuffers(0, kBuffers, vb_, strides_, offsets_);
        c->IASetIndexBuffer(ib_, ibFormat_, ibOffset_);
        c->VSSetShader(vs_, vsI_, vsInstances_);
        c->PSSetShader(ps_, psI_, psInstances_);
        c->CSSetShader(cs_, csI_, csInstances_);
        c->GSSetShader(gs_, nullptr, 0);
        c->HSSetShader(hs_, nullptr, 0);
        c->DSSetShader(ds_, nullptr, 0);
        c->VSSetConstantBuffers(0, kCbs, vsCb_);
        c->PSSetConstantBuffers(0, kCbs, psCb_);
        c->CSSetConstantBuffers(0, kCbs, csCb_);
        c->VSSetSamplers(0, kSamplers, vsSamp_);
        c->PSSetSamplers(0, kSamplers, psSamp_);
        c->CSSetSamplers(0, kSamplers, csSamp_);
        c->RSSetState(raster_);
        c->RSSetViewports(viewports_, vp_);
        c->RSSetScissorRects(scissors_, sc_);
        c->OMSetBlendState(blend_, blendFactor_, sampleMask_);
        c->OMSetDepthStencilState(depthState_, stencilRef_);
        c->OMSetRenderTargets(kTargets, rtv_, dsv_);
        c->CSSetUnorderedAccessViews(0, kUavs, csUav_, nullptr);
        c->VSSetShaderResources(0, kSrvs, vsSrv_);
        c->PSSetShaderResources(0, kSrvs, psSrv_);
        c->CSSetShaderResources(0, kSrvs, csSrv_);
        release(layout_), release(ib_), release(vs_), release(ps_), release(cs_), release(gs_), release(hs_), release(ds_);
        release(dsv_), release(blend_), release(depthState_), release(raster_);
        release_all(vb_), release_all(vsCb_), release_all(psCb_), release_all(csCb_), release_all(vsSrv_);
        release_all(psSrv_), release_all(csSrv_), release_all(vsSamp_), release_all(psSamp_), release_all(csSamp_);
        release_all(csUav_), release_all(rtv_);
        for (UINT i = 0; i < vsInstances_ && i < kInstances; ++i)
            release(vsI_[i]);
        for (UINT i = 0; i < psInstances_ && i < kInstances; ++i)
            release(psI_[i]);
        for (UINT i = 0; i < csInstances_ && i < kInstances; ++i)
            release(csI_[i]);
    }
    StateGuard(const StateGuard&) = delete;
    StateGuard& operator=(const StateGuard&) = delete;

private:
    static constexpr UINT kBuffers = 4, kCbs = 8, kSrvs = 32, kSamplers = 8, kUavs = 8, kTargets = 8, kInstances = 256;
    template <class T> static void release(T*& p) {
        if (p)
            p->Release();
        p = nullptr;
    }
    template <class T, size_t N> static void release_all(T* (&a)[N]) {
        for (T*& p : a)
            release(p);
    }
    ID3D11DeviceContext* c_;
    ID3D11InputLayout* layout_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb_[kBuffers] = {};
    UINT strides_[kBuffers] = {}, offsets_[kBuffers] = {};
    ID3D11Buffer* ib_ = nullptr;
    DXGI_FORMAT ibFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset_ = 0;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11GeometryShader* gs_ = nullptr;
    ID3D11HullShader* hs_ = nullptr;
    ID3D11DomainShader* ds_ = nullptr;
    ID3D11ClassInstance* vsI_[kInstances] = {};
    ID3D11ClassInstance* psI_[kInstances] = {};
    ID3D11ClassInstance* csI_[kInstances] = {};
    UINT vsInstances_ = 0, psInstances_ = 0, csInstances_ = 0;
    ID3D11Buffer* vsCb_[kCbs] = {};
    ID3D11Buffer* psCb_[kCbs] = {};
    ID3D11Buffer* csCb_[kCbs] = {};
    ID3D11ShaderResourceView* vsSrv_[kSrvs] = {};
    ID3D11ShaderResourceView* psSrv_[kSrvs] = {};
    ID3D11ShaderResourceView* csSrv_[kSrvs] = {};
    ID3D11SamplerState* vsSamp_[kSamplers] = {};
    ID3D11SamplerState* psSamp_[kSamplers] = {};
    ID3D11SamplerState* csSamp_[kSamplers] = {};
    ID3D11UnorderedAccessView* csUav_[kUavs] = {};
    ID3D11RenderTargetView* rtv_[kTargets] = {};
    ID3D11DepthStencilView* dsv_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    FLOAT blendFactor_[4] = {};
    UINT sampleMask_ = 0xffffffff;
    ID3D11DepthStencilState* depthState_ = nullptr;
    UINT stencilRef_ = 0;
    ID3D11RasterizerState* raster_ = nullptr;
    UINT viewports_ = 0, scissors_ = 0;
    D3D11_VIEWPORT vp_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_RECT sc_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
};

void draw_triangle(ID3D11DeviceContext* context, ID3D11PixelShader* ps, unsigned w, unsigned h) {
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    context->RSSetViewports(1, &vp);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(d.vs.Get(), nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, d.constants.GetAddressOf());
    context->Draw(3, 0);
}

// The gate: this frame against the one before, where it stood and where the vectors say it was (its depth and its
// colour), read back and logged.
void check_vectors(ID3D11DeviceContext* context, Slot& s, int index, bool car) {
    ID3D11RenderTargetView* rtvs[4] = {d.checkRtv[0].Get(), d.checkRtv[1].Get(), d.checkRtv[2].Get(), d.checkRtv[3].Get()};
    context->OMSetRenderTargets(4, rtvs, nullptr);
    ID3D11ShaderResourceView* srvs[4] = {s.colourSrv[s.colourAt].Get(), s.colourSrv[1 - s.colourAt].Get(), s.depthSrv.Get(),
                                         s.depthBeforeSrv.Get()};
    context->PSSetShaderResources(0, 4, srvs);
    context->PSSetSamplers(0, 1, d.linearClamp.GetAddressOf());
    draw_triangle(context, d.check.Get(), 64, 64);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none[4] = {};
    context->PSSetShaderResources(0, 4, none);
    double sum[4][4] = {};
    for (int t = 0; t < 4; ++t) {
        context->CopyResource(d.checkRead[t].Get(), d.checkTarget[t].Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(d.checkRead[t].Get(), 0, D3D11_MAP_READ, 0, &m)))
            return;
        for (int y = 0; y < 64; ++y) {
            const auto* row = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch);
            for (int x = 0; x < 64 * 4; ++x)
                sum[t][x & 3] += row[x];
        }
        context->Unmap(d.checkRead[t].Get(), 0);
    }
    // [0]: the world, [1]: what is taken for the player's car: colour difference standing, along the vectors, a pixel
    // off them, the points; [2]: the world's summed vector length, depth agreeing along the vectors, standing, the car's
    // summed length; [3]: the car's depth agreeing along the vectors, standing, the points depth judged (within 1 km),
    // the world's and the car's.
    char text[2][400];
    for (int t = 0; t < 2; ++t) {
        const double n = sum[t][3];
        if (n < 1) {
            std::snprintf(text[t], sizeof(text[t]), "no points");
            continue;
        }
        const double still = sum[t][0] / n, along = sum[t][1] / n, off = sum[t][2] / n;
        const double length = (t ? sum[2][3] : sum[2][0]) / n;
        const double judged = t ? sum[3][3] : sum[3][2];
        char depth[160];
        if (judged < 1) {
            std::snprintf(depth, sizeof(depth), "depth: none within 1 km");
        } else {
            const double depthAlong = (t ? sum[3][0] : sum[2][1]) / judged, depthStill = (t ? sum[3][1] : sum[2][2]) / judged;
            std::snprintf(depth, sizeof(depth), "depth (%.0f within 1 km) agrees along the vectors at %.1f%%, standing %.1f%%%s",
                          judged, 100 * depthAlong, 100 * depthStill, dlss::gate_depth_verdict(depthStill, depthAlong));
        }
        std::snprintf(text[t], sizeof(text[t]),
                      "%.0f points (mean %.2f px); %s; colour difference %.5f standing, %.5f along the vectors, %.5f a "
                      "pixel off them%s",
                      n, length, depth, still, along, off, dlss::gate_verdict(still, along, off));
    }
    if (car)
        say("gate: frame %llu view %d (%ux%u): the world: %s; the car (its box, moved with the head "
                      "alone): %s",
                      d.frame, index, s.width, s.height, text[0], text[1]);
    else
        say("gate: frame %llu view %d (%ux%u): the world beyond 8 m: %s; nearer (the car, the world's "
                      "vectors): %s",
                      d.frame, index, s.width, s.height, text[0], text[1]);
}

// One view: its depth and vectors drawn, its part of the resolved scene (w x h at (x, y)) through DLSS and back into
// place at ow x oh (DLAA: the same). False: not done (the view as it was drawn).
bool run_view(ID3D11DeviceContext* context, ID3D11Resource* resolved, DXGI_FORMAT format, int index, unsigned x, unsigned y,
              unsigned w, unsigned h, unsigned ow, unsigned oh, const M4 rendered[2], const M4 unjittered[2],
              const Jitter& jitter, double shiftX, double shiftY, bool inverted, const double depthTerms[2], CarTest car) {
    Slot& s = d.slots[index];
    if (!ensure(context, s, index, w, h, ow, oh, format, inverted)) {
        ++d.failures;
        return false;
    }
    const bool history = s.haveBefore && s.beforeFrame + 1 == d.frame;
    const int use = 0;  // [0] through the world's view, [1] through the head's (the car)
    // The car's box needs this view's clip -> tracking space: the inverse of the head's view x projection as rendered.
    car.on = car.on && dlss::invert(rendered[1], car.clipToTracking);
    M4 r[2] = {dlss::identity(), dlss::identity()};
    bool ok = history;
    for (int k = 0; k < 2 && ok; ++k)
        ok = dlss::reprojection(rendered[k], s.beforeUnjittered[k], r[k]);
    if (!ok)
        r[0] = r[1] = dlss::identity();
    Constants c{};
    to_floats(r[use], c.r);
    to_floats(r[1 - use], c.r2);
    // Without history the vectors come out as the jitter alone (nothing moved).
    c.js[0] = ok ? static_cast<float>(jitter.on ? shiftX : 0) : 0;
    c.js[1] = ok ? static_cast<float>(jitter.on ? shiftY : 0) : 0;
    c.js[2] = static_cast<float>(w);
    c.js[3] = static_cast<float>(h);
    c.origin[0] = static_cast<float>(x);
    c.origin[1] = static_cast<float>(y);
    c.origin[2] = static_cast<float>(depthTerms[0]);  // depth = [0] + [1] / distance
    c.origin[3] = static_cast<float>(depthTerms[1]);
    c.extra[0] = car.on ? 1.0f : 0.0f;
    c.extra[1] = static_cast<float>(s.beforeShift[0]);  // the gate: the frame before's colour sits at its own jitter
    c.extra[2] = static_cast<float>(s.beforeShift[1]);
    to_floats(car.clipToTracking, c.t);
    for (int i = 0; i < 3; ++i)
        c.head[i] = static_cast<float>(car.head[i]);
    // The box in the tracking space's axes: x right, y up, z back (forward is -z).
    c.boxMin[0] = static_cast<float>(-d.cfg.carBox[0]);
    c.boxMax[0] = static_cast<float>(d.cfg.carBox[1]);
    c.boxMin[1] = static_cast<float>(-d.cfg.carBox[2]);
    c.boxMax[1] = static_cast<float>(d.cfg.carBox[3]);
    c.boxMin[2] = static_cast<float>(-d.cfg.carBox[4]);
    c.boxMax[2] = static_cast<float>(d.cfg.carBox[5]);
    context->UpdateSubresource(d.constants.Get(), 0, nullptr, &c, 0, 0);
    ID3D11RenderTargetView* rtvs[2] = {s.depthRtv.Get(), s.vectorsRtv.Get()};
    context->OMSetRenderTargets(2, rtvs, nullptr);
    context->PSSetShaderResources(0, 1, d.depthSrv.GetAddressOf());
    draw_triangle(context, d.depthMs ? d.vectorsMs.Get() : d.vectorsSs.Get(), w, h);
    if (!ok || !d.cfg.vectors) {
        const float zero[4] = {};
        context->ClearRenderTargetView(s.vectorsRtv.Get(), zero);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);

    if (d.cfg.gate) {  // the frame as the game made it, kept for the next frame's check
        s.colourAt = 1 - s.colourAt;
        const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
        context->CopySubresourceRegion(s.colour[s.colourAt].Get(), 0, 0, 0, 0, resolved, 0, &box);
        s.colourFrame[s.colourAt] = d.frame;
        // The 60th frame of the view (a second into the scene), then every 300th.
        if (ok && (index == 0 || index == 2) && (s.runs == 60 || (s.runs > 60 && s.runs % 300 == 0)) &&
            s.colourFrame[1 - s.colourAt] + 1 == d.frame && s.depthBeforeFrame + 1 == d.frame) {
            check_vectors(context, s, index, car.on);
        }
        context->CopyResource(s.depthBefore.Get(), s.depth.Get());  // this frame's depth, for the next frame's check
        s.depthBeforeFrame = d.frame;
    }

    ++s.runs;
    ngx::Evaluation e;
    e.colour = resolved;
    e.colourX = x;
    e.colourY = y;
    e.depth = s.depth.Get();
    e.vectors = s.vectors.Get();
    e.output = s.output.Get();
    e.width = w;
    e.height = h;
    e.jitterX = jitter.on ? static_cast<float>(d.cfg.flipX ? -jitter.sx : jitter.sx) : 0;
    e.jitterY = jitter.on ? static_cast<float>(d.cfg.flipY ? -jitter.sy : jitter.sy) : 0;
    e.reset = !history;
    unsigned result = 0;
    const bool done = ngx::evaluate(context, s.feature, e, result);
    if (done) {
        context->CopySubresourceRegion(resolved, 0, x, y, 0, s.output.Get(), 0, nullptr);  // ow x oh, from (x, y)
        ++d.evaluations;
        ++d.evaluationsTotal;
    } else {
        if (d.failures < 5)
            say("frame %llu view %d: the evaluation failed (NGX 0x%08x)", d.frame, index, result);
        ++d.failures;
    }
    s.beforeUnjittered[0] = unjittered[0];
    s.beforeUnjittered[1] = unjittered[1];
    s.beforeShift[0] = jitter.on ? shiftX : 0;
    s.beforeShift[1] = jitter.on ? shiftY : 0;
    s.beforeFrame = d.frame;
    s.haveBefore = true;
    return done;
}

// A focus view drawn smaller (w x h at (x, 0)) that DLSS didn't upscale this frame: stretched over its region (ow x oh
// from there), so the post and the headset get it in place, soft, not a small image in the region's corner with the
// last frame's around it. Under its own state guard (the game's state back after it).
void stretch_focus(ID3D11DeviceContext* context, ID3D11Resource* resolved, unsigned x, unsigned w, unsigned h, unsigned ow,
                   unsigned oh) {
    if (!d.device || !d.vs || !d.stretch || !d.constants || !d.linearClamp || !resolved || !w || !h)
        return;
    ComPtr<ID3D11Texture2D> scene;
    if (FAILED(resolved->QueryInterface(IID_PPV_ARGS(&scene))))
        return;
    D3D11_TEXTURE2D_DESC sd;
    scene->GetDesc(&sd);
    if (sd.SampleDesc.Count != 1 || x + ow > sd.Width || oh > sd.Height || w > ow || h > oh)
        return;
    const unsigned size[4] = {w, h, ow, oh};
    if (std::memcmp(size, d.stretchSize, sizeof(size)) != 0 || d.stretchFormat != sd.Format) {
        std::memcpy(d.stretchSize, size, sizeof(size));
        d.stretchFormat = sd.Format;
        d.stretchSrv.Reset();
        d.stretchRtv.Reset();
        if (!make(d.stretchIn, w, h, sd.Format, D3D11_BIND_SHADER_RESOURCE) ||
            !make(d.stretchOut, ow, oh, sd.Format, D3D11_BIND_RENDER_TARGET) ||
            FAILED(d.device->CreateShaderResourceView(d.stretchIn.Get(), nullptr, &d.stretchSrv)) ||
            FAILED(d.device->CreateRenderTargetView(d.stretchOut.Get(), nullptr, &d.stretchRtv))) {
            d.stretchSrv.Reset();
            d.stretchRtv.Reset();
            if (!d.stretchRefusedLogged)
                say("a focus view's stretch (%ux%u to %ux%u, format %d) couldn't be made", w, h, ow, oh,
                    static_cast<int>(sd.Format));
            d.stretchRefusedLogged = true;
        }
    }
    if (!d.stretchSrv || !d.stretchRtv)
        return;
    StateGuard guard(context);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_BOX box{x, 0, 0, x + w, h, 1};
    context->CopySubresourceRegion(d.stretchIn.Get(), 0, 0, 0, 0, resolved, 0, &box);
    Constants c{};
    c.js[2] = static_cast<float>(ow);
    c.js[3] = static_cast<float>(oh);
    context->UpdateSubresource(d.constants.Get(), 0, nullptr, &c, 0, 0);
    context->OMSetRenderTargets(1, d.stretchRtv.GetAddressOf(), nullptr);
    context->PSSetShaderResources(0, 1, d.stretchSrv.GetAddressOf());
    context->PSSetSamplers(0, 1, d.linearClamp.GetAddressOf());
    draw_triangle(context, d.stretch.Get(), ow, oh);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->CopySubresourceRegion(resolved, 0, x, 0, 0, d.stretchOut.Get(), 0, nullptr);
    ++d.stretched;
    ++d.stretchedTotal;
}

// Runs `f` on the way out of a scope.
template <class F> struct OnExit {
    F f;
    ~OnExit() { f(); }
};
template <class F> OnExit(F) -> OnExit<F>;

}  // namespace

void dlss_start(const DlssRun& run) {
    d.cfg = run;
    d.off = d.focusSrOff = false;
    d.cfg.focusScale = (std::max)(33, (std::min)(100, run.focusScale));
    // The jitter's sequence: as long as the smallest render asks (the eyes and the focus views share its phase).
    d.phases = d.cfg.views >= 2 ? dlss::jitter_phases(d.cfg.focusScale / 100.0) : dlss::kJitterPhases;
    if (run.views && !ngx::built_in()) {
        say("asked for, but this build has no NGX SDK (CMake: NGX_SDK_DIR): off");
        d.cfg.views = 0;
    }
    char focusMode[160] = "";
    if (d.cfg.views >= 2 && d.cfg.focusScale < 100)
        std::snprintf(focusMode, sizeof(focusMode), ", DLSS %s on the focus views (drawn at %d%% of their size each way, "
                      "upscaled)", dlss::focus_mode_name(d.cfg.focusScale), d.cfg.focusScale);
    else if (d.cfg.views >= 2)
        std::snprintf(focusMode, sizeof(focusMode), " and the focus views");
    if (d.cfg.views)
        say("DLAA on the eyes%s after each eye pass's scene (its multisample resolve, or with MSAA off "
                      "before the post-processing reads it) (jitter %s, vectors %s, depth "
                      "%s, the car %s: a box %.2f m left, %.2f right, %.2f down, %.2f up, %.2f front, %.2f back of the "
                      "head%s%s; presets: the eyes %s, the focus views %s)",
                      focusMode, d.cfg.jitter ? "on" : "off",
                      d.cfg.vectors ? "from depth" : "zero", d.cfg.depthReversed ? "reversed" : "0 near",
                      d.cfg.car ? "on" : "off", d.cfg.carBox[0], d.cfg.carBox[1], d.cfg.carBox[2], d.cfg.carBox[3],
                      d.cfg.carBox[4], d.cfg.carBox[5], d.cfg.viewInverse ? ", view matrix inverted" : "",
                      d.cfg.gate ? ", gate on" : "",
                      dlss::preset_name(d.cfg.eyePreset >= 0 ? d.cfg.eyePreset : d.cfg.preset), dlss::preset_name(d.cfg.preset));
}

bool dlss_wanted() { return d.cfg.views > 0 && !d.off; }

void dlss_jitter_views(XrView* views, uint32_t count) {
    if (!dlss_wanted() || !views || count < 2)
        return;
    std::lock_guard guard(d.mutex);
    for (int e = 0; e < 2; ++e) {  // the eyes in the tracking space: the car's vectors (the head's motion alone)
        const XrQuaternionf& o = views[e].pose.orientation;
        d.pose[e] = views[e].pose;
        d.poseValid[e] = std::fabs(o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w - 1.0f) < 0.01f;
    }
    d.eye[0].on = d.eye[1].on = false;
    if (!d.cfg.jitter || d.jitterDead || !d.lastFrameOk)
        return;
    ++d.phase;
    double sx, sy;
    dlss::jitter_pixels(d.phase, sx, sy, d.phases);
    for (int e = 0; e < 2; ++e) {
        if (!d.eyeW[e] || !d.eyeH[e])
            continue;
        Jitter j;
        j.on = true;
        j.sx = sx;
        j.sy = sy;
        dlss::clip_shift_for(sx, sy, d.eyeW[e], d.eyeH[e], j.cx, j.cy);
        XrFovf& fov = views[e].fov;
        dlss::Tangents t{std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleDown), std::tan(fov.angleUp)};
        dlss::shift_tangents(t, j.cx, j.cy);
        fov.angleLeft = static_cast<float>(std::atan(t.left));
        fov.angleRight = static_cast<float>(std::atan(t.right));
        fov.angleDown = static_cast<float>(std::atan(t.down));
        fov.angleUp = static_cast<float>(std::atan(t.up));
        d.eye[e] = j;
    }
}

double dlss_focus_scale() {
    if (!dlss_wanted() || !d.ready || d.cfg.views < 2 || d.cfg.focusScale >= 100 || d.focusSrOff)
        return 1.0;
    return d.cfg.focusScale / 100.0;
}

void dlss_focus_jitter(int eye, unsigned focusWidth, unsigned focusHeight, float& b, float& dd) {
    if (eye < 0 || eye > 1)
        return;
    std::lock_guard guard(d.mutex);
    d.focus[eye].on = false;
    if (!dlss_wanted() || d.cfg.views < 2 || !d.cfg.jitter || d.jitterDead || !d.lastFrameOk || !focusWidth || !focusHeight)
        return;
    Jitter j;
    j.on = true;
    dlss::jitter_pixels(d.phase, j.sx, j.sy, d.phases);
    dlss::clip_shift_for(j.sx, j.sy, focusWidth, focusHeight, j.cx, j.cy);
    b += static_cast<float>(j.cx);
    dd += static_cast<float>(j.cy);
    d.focus[eye] = j;
}

bool dlss_eye_clip_shift(int eye, double& cx, double& cy) {
    cx = cy = 0;
    if (eye < 0 || eye > 1 || !dlss_wanted())
        return false;
    std::lock_guard guard(d.mutex);
    if (!d.eye[eye].on)
        return false;
    cx = d.eye[eye].cx * d.sign[0];
    cy = d.eye[eye].cy * d.sign[1];
    return true;
}

void dlss_after_resolve(ID3D11DeviceContext* context, ID3D11Resource* resolved, ID3D11DepthStencilView* depth,
                        const DlssPass& pass) {
    if (!context || !resolved || pass.eye < 0 || pass.eye > 1 || !pass.eyeWidth || !pass.eyeHeight)
        return;
    // The focus view's size, and what it was drawn at (smaller: super resolution). Drawn smaller, it is upscaled here or,
    // on any way out without that, stretched (the region would otherwise hold a small image in its corner).
    const unsigned fw = pass.focusWidth, fh = pass.focusHeight;
    const unsigned rw = pass.focusRenderWidth ? (std::min)(pass.focusRenderWidth, fw) : fw;
    const unsigned rh = pass.focusRenderHeight ? (std::min)(pass.focusRenderHeight, fh) : fh;
    bool focusLeft = pass.focus && fw && fh && (rw < fw || rh < fh);
    const OnExit stretchLeft{[&] {
        if (focusLeft)
            stretch_focus(context, resolved, pass.eyeWidth, rw, rh, fw, fh);
    }};
    if (!dlss_wanted())
        return;
    if (!d.ready && !initialize(context))
        return;
    ComPtr<ID3D11Texture2D> scene;
    if (FAILED(resolved->QueryInterface(IID_PPV_ARGS(&scene))))
        return;
    D3D11_TEXTURE2D_DESC sd;
    scene->GetDesc(&sd);
    unsigned depthW = 0, depthH = 0;
    if (!depth_view(depth, depthW, depthH)) {
        if (!depth && !d.depthRefusedLogged) {
            d.depthRefusedLogged = true;
            say("no depth target was bound with the pass's scene");
        }
        ++d.failures;
        return;
    }
    M4 view, projection;
    if (!pass_matrices(pass.eye, view, projection)) {
        give_up("the game's view and projection for the pass aren't a rigid view and a projection (its log lines say more)");
        return;
    }
    Jitter eyeJitter, focusJitter;
    XrPosef pose{};
    bool poseValid = false;
    {
        std::lock_guard guard(d.mutex);
        d.eyeW[pass.eye] = pass.eyeWidth;
        d.eyeH[pass.eye] = pass.eyeHeight;
        eyeJitter = d.eye[pass.eye];
        focusJitter = d.focus[pass.eye];
        pose = d.pose[pass.eye];
        poseValid = d.poseValid[pass.eye];
    }
    note_projection(pass.eye, projection, eyeJitter);
    // The eye's jitter as it reached the projection (the game's sign for the located FOV; the focus view's is ours).
    eyeJitter.sx *= d.sign[0];
    eyeJitter.cx *= d.sign[0];
    eyeJitter.sy *= d.sign[1];
    eyeJitter.cy *= d.sign[1];
    // The game's matrix is world -> view as given (LMU's: run 3's gate; DlssRun::viewInverse: the camera's world matrix).
    if (d.cfg.viewInverse) {
        M4 viewInverse;
        if (!dlss::invert(view, viewInverse))
            return;
        view = viewInverse;
    }
    // The scene's depth: reversed (the eye's projection with its depth turned round) unless told otherwise.
    const bool inverted = d.cfg.depthReversed;
    const M4 used = inverted ? dlss::reversed_depth(projection) : projection;
    const double depthTerms[2] = {used[10], used[14]};  // depth = m10 + m14 / distance (w = z)
    // Two ways a pixel got here from the frame before: with the world (the game's view matrix: the car's travel and
    // the head's motion), and with the player's seat (the eye's pose in the tracking space: the head's motion alone).
    // What lies in the car's box about the head takes the second (the shader tests each pixel's place in the tracking
    // space).
    CarTest car;
    car.on = poseValid && d.cfg.car;
    M4 head = view;
    if (car.on) {
        const double q[4] = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
        const double t[3] = {pose.position.x, pose.position.y, pose.position.z};
        head = dlss::head_view(q, t);
        for (int i = 0; i < 3; ++i)
            car.head[i] = t[i];
    }
    const M4 vp[2] = {dlss::mul(view, used), dlss::mul(head, used)};
    const bool room = pass.eyeWidth <= sd.Width && pass.eyeHeight <= sd.Height && pass.eyeWidth <= depthW &&
                      pass.eyeHeight <= depthH;
    // The focus view: its region in the scene (DLSS's output), what it was drawn at in the depth.
    const bool focus = d.cfg.views >= 2 && pass.focus && fw && fh && pass.eyeWidth + fw <= sd.Width && fh <= sd.Height &&
                       pass.eyeWidth + rw <= depthW && rh <= depthH;
    if (!room) {
        ++d.failures;
        return;
    }
    if (!d.firstLogged) {
        d.firstLogged = true;
        char drawn[64] = "";
        if (focus && (rw < fw || rh < fh))
            std::snprintf(drawn, sizeof(drawn), " (drawn at %ux%u of its %ux%u)", rw, rh, fw, fh);
        say("first pass: eye %d %ux%u%s%s in a %ux%u scene (format %d), depth %ux%u x%s", pass.eye,
                      pass.eyeWidth, pass.eyeHeight, focus ? " with its focus view beside" : "", drawn, sd.Width, sd.Height,
                      static_cast<int>(sd.Format), depthW, depthH, d.depthMs ? "multisample" : "1");
    }

    StateGuard guard(context);  // the game's state back when this returns
    // A plain pipeline for our draws: the game's other stages, states and scissors out of the way.
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);

    {  // the eye: rendered with its jitter; without it the clip shift is taken off
        const M4 off = dlss::clip_shift(eyeJitter.on ? -eyeJitter.cx : 0, eyeJitter.on ? -eyeJitter.cy : 0);
        const M4 unjittered[2] = {dlss::mul(vp[0], off), dlss::mul(vp[1], off)};
        run_view(context, resolved, sd.Format, pass.eye, 0, 0, pass.eyeWidth, pass.eyeHeight, pass.eyeWidth,
                 pass.eyeHeight, vp, unjittered, eyeJitter, eyeJitter.cx, eyeJitter.cy, inverted, depthTerms, car);
    }
    if (focus && !d.off) {  // the focus view: the eye's clip through its remap (which carries its own jitter)
        const M4 remap = dlss::clip_remap_matrix(pass.remap[0], pass.remap[1], pass.remap[2], pass.remap[3]);
        const M4 plain = dlss::clip_remap_matrix(pass.remap[0], pass.remap[1] - (focusJitter.on ? focusJitter.cx : 0),
                                                 pass.remap[2], pass.remap[3] - (focusJitter.on ? focusJitter.cy : 0));
        const M4 rendered[2] = {dlss::mul(vp[0], remap), dlss::mul(vp[1], remap)};
        const M4 unjittered[2] = {dlss::mul(vp[0], plain), dlss::mul(vp[1], plain)};
        if (run_view(context, resolved, sd.Format, 2 + pass.eye, pass.eyeWidth, 0, rw, rh, fw, fh, rendered, unjittered,
                     focusJitter, focusJitter.cx, focusJitter.cy, inverted, depthTerms, car))
            focusLeft = false;
    }

    d.evaluated[pass.eye] = true;
    d.jitteredFrames += eyeJitter.on && pass.eye == 0 ? 1 : 0;
}

void dlss_shutdown() {
    if (!d.device)
        return;
    for (Slot& s : d.slots) {
        if (s.feature)
            ngx::release(s.feature);
        s = Slot{};
    }
    if (d.ready)
        ngx::shutdown(d.device.Get());
    d.ready = false;
    d.depthFor = nullptr;
    d.depthSrv.Reset();
    d.vs.Reset();
    d.vectorsMs.Reset();
    d.vectorsSs.Reset();
    d.check.Reset();
    d.stretch.Reset();
    d.stretchIn.Reset();
    d.stretchOut.Reset();
    d.stretchSrv.Reset();
    d.stretchRtv.Reset();
    std::memset(d.stretchSize, 0, sizeof(d.stretchSize));
    d.constants.Reset();
    d.linearClamp.Reset();
    for (int i = 0; i < 4; ++i) {
        d.checkTarget[i].Reset();
        d.checkRead[i].Reset();
        d.checkRtv[i].Reset();
    }
    d.device.Reset();
    {
        std::lock_guard guard(d.mutex);
        d.lastFrameOk = false;
        d.eye[0] = d.eye[1] = d.focus[0] = d.focus[1] = Jitter{};
    }
    d.firstLogged = false;
    say("shut down (features released, NGX stopped)");
}

void dlss_set_matrices(PassMatricesFn fn) { d.matrices = fn; }

unsigned long long dlss_evaluations_total() { return d.evaluationsTotal; }
unsigned long long dlss_stretched_total() { return d.stretchedTotal; }

void dlss_frame_end(unsigned long long frame) {
    if (!d.cfg.views)
        return;
    {
        std::lock_guard guard(d.mutex);
        d.lastFrameOk = d.evaluated[0] && d.evaluated[1] && !d.off;
        d.evaluated[0] = d.evaluated[1] = false;
    }
    ++d.frame;
    if (frame - d.statsFrame >= 900) {
        d.statsFrame = frame;
        if (d.evaluations || d.failures || d.stretched) {
            char focusSizes[2][48];
            for (int i = 0; i < 2; ++i) {
                const Slot& s = d.slots[2 + i];
                if (s.outWidth != s.width || s.outHeight != s.height)
                    std::snprintf(focusSizes[i], sizeof(focusSizes[i]), "%ux%u up to %ux%u", s.width, s.height, s.outWidth,
                                  s.outHeight);
                else
                    std::snprintf(focusSizes[i], sizeof(focusSizes[i]), "%ux%u", s.width, s.height);
            }
            say("%llu evaluations, %llu failed, %llu features made again, %llu frames with jitter, %llu focus views "
                "stretched since the last line; views %ux%u %ux%u, focus %s %s; jitter sign %s",
                d.evaluations, d.failures, d.recreated, d.jitteredFrames, d.stretched, d.slots[0].width, d.slots[0].height,
                d.slots[1].width, d.slots[1].height, focusSizes[0], focusSizes[1],
                d.jitterDead ? "none (it doesn't reach the projection)" : !d.signDecided ? "not settled yet"
                : d.sign[0] > 0 && d.sign[1] > 0 ? "as asked" : "negated on an axis");
        }
        d.evaluations = d.failures = d.recreated = d.jitteredFrames = d.stretched = 0;
    }
}

}  // namespace simqvfr

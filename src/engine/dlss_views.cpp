// DLAA at the API level (dlss_views.h).
#include "dlss_views.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "../log.h"
#include "d3d11_kit.h"
#include "d3d11_state.h"
#include "dlss_math.h"
#include "dlss_views_shaders.h"
#include "fsr_upscale.h"
#include "ngx_dlss.h"
#include "nr_math.h"
#include "nr_views.h"

using Microsoft::WRL::ComPtr;

namespace simqvfr {
namespace {

using dlss::M4;
namespace ngx = simqvfr::ngx;

struct Constants {
    float r[16], r2[16], js[4], origin[4], extra[4], t[16], head[4], boxMin[4], boxMax[4];
    float inv[16], before[16], objects[4];  // moving objects (DlssPass::objectSlots)
    float band[4];  // a band of the view's rows: its top, the view's height (0: none), its move since the frame before
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

struct Slot {  // a view: eye 0, eye 1, focus 0, focus 1; focus 0 and 1 over a band of their rows (DlssPass::focusBandTop)
    unsigned width = 0, height = 0;        // what it's drawn at (its inputs)
    unsigned outWidth = 0, outHeight = 0;  // what DLSS makes of it (the same: DLAA)
    int preset = 0;                        // NGX's render preset it was made with (DlssRun::preset, eyePreset)
    bool inverted = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ngx::Feature* feature = nullptr;
    fsr::View* fsrView = nullptr;  // AMD FSR's context instead (DlssRun::upscaler): its shared textures are depth,
                                   // vectors, output and colourIn
    int backend = -1;              // the upscaler it was made by (State::backend)
    bool infinite = false;         // FSR: made for an infinite far plane
    ComPtr<ID3D11Texture2D> colourIn;  // FSR: the view's colour copied in (FSR reads no sub-rectangle of the scene)
    bool refused = false;  // the feature couldn't be made at this size
    ComPtr<ID3D11Texture2D> depth, vectors, output, colour[2], depthBefore;
    ComPtr<ID3D11RenderTargetView> depthRtv, vectorsRtv;
    ComPtr<ID3D11ShaderResourceView> depthSrv, colourSrv[2], depthBeforeSrv;
    ComPtr<ID3D11ShaderResourceView> vectorsSrv;  // DLSS-NR's guide (nr_views.h)
    int colourAt = 0;
    unsigned long long colourFrame[2] = {}, depthBeforeFrame = 0;
    M4 beforeUnjittered[2] = {dlss::identity(), dlss::identity()};  // through the world's view; through the head's
    double beforeShift[2] = {};  // the frame before's clip shift as rendered (its jitter): where its colour sits
    unsigned long long beforeFrame = 0;
    bool haveBefore = false;
    unsigned beforeBandTop = 0;  // a band's top row in the frame before (its vectors carry the move)
    unsigned long long runs = 0;  // frames this view went through DLAA at this size
    // A live size change waiting to settle (ensure): the size asked and since when (GetTickCount64), and whether this
    // frame's view was left as drawn for it (not a failure).
    unsigned waitW = 0, waitH = 0, waitOW = 0, waitOH = 0;
    unsigned long long waitSince = 0;
    bool waiting = false;
};

// A view's feature is made again for a new size only once that size has held this long. Making one takes 40-250 ms on
// the game's render thread (2026-11-16, LMU, 2000-2600 px focus views), and every step of the headset menu's Focus
// density or size made both focus views' again: a 80-330 ms freeze per press, more with a key held. Settled, a run of
// steps costs one. Until then the view shows as drawn (no DLAA for that moment; super resolution: stretched).
constexpr unsigned long long kResizeSettleMs = 500;
// One eye's passes in a row without the game's matrices before DLSS is off for the session (two seconds at 90 Hz;
// the other eye's passes don't reset it): one without them is left as drawn.
constexpr unsigned kMatricesMissingLimit = 180;

struct State {
    DlssRun cfg;
    bool off = false;  // gave up for the session (the log says why)
    bool ready = false;
    unsigned long long frame = 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> vectorsMs, vectorsSs, check, stretch, bandOutside;
    // The vectors with our single-sample depth copy beside multisample object slots; the copy's and the measurement's.
    ComPtr<ID3D11PixelShader> vectorsCopyMs, depthCopyMs, depthCopySs, measureMs, measureSs, keepFarMs, keepFarSs;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearClamp;
    ComPtr<ID3D11Texture2D> checkTarget[4], checkRead[4];
    ComPtr<ID3D11RenderTargetView> checkRtv[4];
    // The scene's depth target as a shader resource.
    ID3D11Resource* depthFor = nullptr;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    bool depthMs = false, depthRefusedLogged = false;
    // What this pass's vectors read as the depth (run_view): the depth target's view, or the copy made before the
    // transparents (dlss_depth_copy_list), single-sample.
    ID3D11ShaderResourceView* depthIn = nullptr;
    bool depthInMs = false;
    // The scene's depth as it stood before the transparents that write depth (dlss_depth_copy_list): our copy (the
    // depth target's size, R32 float), the depth target it copies (raw, compared only) and the list that makes it.
    struct DepthCopy {
        ComPtr<ID3D11DeviceContext> deferred;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        unsigned texW = 0, texH = 0;
        ComPtr<ID3D11CommandList> list;
        ID3D11Resource* source = nullptr;  // the list's depth target
        unsigned w = 0, h = 0;             // the part it copies (from the top-left)
        bool ms = false;
        bool refusedLogged = false;
        // The measurement (DlssRun::depthMeasure): every 8th texel, before and after, read back a frame or more later.
        ComPtr<ID3D11Texture2D> grid, gridRead[2];
        ComPtr<ID3D11RenderTargetView> gridRtv;
        unsigned gridW = 0, gridH = 0;
        struct Taken {
            bool pending = false;
            int eye = 0;
            unsigned long long frame = 0;
            unsigned eyeW = 0, eyeH = 0, focusW = 0, focusH = 0;  // the regions (the focus view right of the eye)
            double terms[2] = {};                                 // depth = [0] + [1] / distance
            double nearM = 0;                                     // DlssRun::depthCopyNear then
        } taken[2];
        unsigned long long nextFrame = 300;
        int nextEye = 0;
        unsigned long long used = 0, measured = 0;  // passes whose vectors read the copy; only measured it
    } copy;
    Slot slots[6];
    int createFailures = 0;
    bool firstLogged = false;
    bool dllNamed = false;  // the nvngx_dlss.dll NGX loaded has been logged
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
    // A focus view's rows outside its DLAA band (band_outside): the eye's region under it copied in, the rows drawn
    // into the focus view's size, copied back; at these sizes. Since the last log line: bands run, their rows' share.
    ComPtr<ID3D11Texture2D> bandIn, bandOut;
    ComPtr<ID3D11ShaderResourceView> bandSrv;
    ComPtr<ID3D11RenderTargetView> bandRtv;
    unsigned bandSize[4] = {};
    DXGI_FORMAT bandFormat = DXGI_FORMAT_UNKNOWN;
    bool bandRefusedLogged = false;
    unsigned long long bands = 0, bandFallbacks = 0, bandDirect = 0, composed = 0;
    ComPtr<ID3D11BlendState> bandBlend;    // the eye's colour by its share (alpha), the target's alpha kept
    ID3D11Resource* bandScene = nullptr;   // the scene texture bandSceneRtv views (raw, compared only)
    ComPtr<ID3D11RenderTargetView> bandSceneRtv;
    double bandShare = 0;
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
    // Each eye's focus view processed this frame, and in the frame before (its jitter may go on).
    bool focusEvaluated[2] = {}, focusOk[2] = {};
    int jitterHeldLogged = 0;  // "no jitter until it is processed" lines so far (a few)
    bool jitterHeld = false;   // in a stretch of frames with a view failing (logged at its start)
    unsigned long long failuresAtFrameEnd = 0;  // `failures` at the last frame end: a frame's own failures
    unsigned long long resizeWaitsAtFrameEnd = 0;  // `resizeWaits` likewise: a view left as drawn holds the jitter too
    unsigned long long evaluations = 0, failures = 0, recreated = 0, jitteredFrames = 0, statsFrame = 0, stretched = 0;
    unsigned long long resizeWaits = 0;  // views left as drawn while a new size settled (ensure, kResizeSettleMs)
    unsigned long long resizedFrame = 0;  // the frame a view's feature was last made again for a new size: one a frame
    unsigned long long objectViews = 0;  // views whose vectors took moving objects' motion (DlssPass::objectSlots)
    unsigned long long evaluationsTotal = 0, stretchedTotal = 0;
    PassMatricesFn matrices = nullptr;  // the game's (dlss_set_matrices)
    unsigned matricesMissing[2] = {};   // per eye, its passes in a row given none (kMatricesMissingLimit: off)
    // The player's latest change (dlss_live; under the mutex), taken at the next frame end.
    bool livePending = false;
    DlssLive live;
    std::chrono::steady_clock::time_point liveAt;  // the last dlss_live
    unsigned liveChanges = 0;                      // dlss_live calls since the last apply
    // The upscaler running the views (DlssRun::upscaler as resolved for the GPU): 0 NGX, 1 AMD FSR; -1 none started.
    int backend = -1;
    unsigned vendor = 0;  // the game's adapter's PCI vendor id
    // FSR's camera inputs: the time between frames, the far plane's kind (made into its contexts).
    double frameMs = 11.1;
    std::chrono::steady_clock::time_point lastFrameEnd{};
    bool cameraInfinite = false;
    bool fsrProviderNamed = false;
    bool ngxRefused = false;  // NGX didn't start on this device: FSR in its place (fsrWhereNoDlss)
} d;

// The camera AMD FSR takes per view (NGX takes none of it).
struct Camera {
    float nearM = 0.1f, farM = 10000.0f, fovY = 1.5f;
};

const char* log_tag() { return d.cfg.tag ? d.cfg.tag : "dlss"; }

// A log line with the run's tag in front ("lmu dlss: ...").
void say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_tagged_v(log_tag(), format, args);
    va_end(args);
}

void give_up(const char* why) { give_up_once(d.off, log_tag(), why); }

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
    const auto bandOutside = compile(kBandOutsideShader, "ps", "ps_5_0", nullptr);
    const D3D_SHADER_MACRO bothMs[] = {{"MS", "1"}, {"DEPTH_MS", "1"}, {nullptr, nullptr}};
    const auto psBoth = compile(kVectorsShader, "ps", "ps_5_0", bothMs);
    const auto copyMs = compile(kDepthCopyShader, "ps", "ps_5_0", ms);
    const auto copySs = compile(kDepthCopyShader, "ps", "ps_5_0", nullptr);
    const auto measureMs = compile(kDepthMeasureShader, "ps", "ps_5_0", ms);
    const auto measureSs = compile(kDepthMeasureShader, "ps", "ps_5_0", nullptr);
    const auto keepFarMs = compile(kDepthKeepFarShader, "ps", "ps_5_0", ms);
    const auto keepFarSs = compile(kDepthKeepFarShader, "ps", "ps_5_0", nullptr);
    const auto made = [&](const ComPtr<ID3DBlob>& code, ComPtr<ID3D11PixelShader>& ps) {
        return code && SUCCEEDED(d.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
    };
    if (!made(psMs, d.vectorsCopyMs) || !made(copyMs, d.depthCopyMs) || !made(copySs, d.depthCopySs) ||
        !made(measureMs, d.measureMs) || !made(measureSs, d.measureSs) || !made(keepFarMs, d.keepFarMs) ||
        !made(keepFarSs, d.keepFarSs) || !made(bandOutside, d.bandOutside)) {
        give_up("its shaders couldn't be made");
        return false;
    }
    if (!vs || !psBoth || !psSs || !check || !stretch ||
        FAILED(d.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d.vs)) ||
        FAILED(d.device->CreatePixelShader(psBoth->GetBufferPointer(), psBoth->GetBufferSize(), nullptr, &d.vectorsMs)) ||
        FAILED(d.device->CreatePixelShader(psSs->GetBufferPointer(), psSs->GetBufferSize(), nullptr, &d.vectorsSs)) ||
        FAILED(d.device->CreatePixelShader(check->GetBufferPointer(), check->GetBufferSize(), nullptr, &d.check)) ||
        FAILED(d.device->CreatePixelShader(stretch->GetBufferPointer(), stretch->GetBufferSize(), nullptr, &d.stretch))) {
        give_up("its shaders couldn't be made");
        return false;
    }
    bool ok = make_constant_buffer(d.device.Get(), sizeof(Constants), d.constants) &&
              make_linear_clamp_sampler(d.device.Get(), d.linearClamp);
    {  // band_outside's blend: the eye's colour by its share, the target's alpha left as it was
        D3D11_BLEND_DESC bd{};
        auto& t = bd.RenderTarget[0];
        t.BlendEnable = TRUE;
        t.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ZERO;
        t.DestBlendAlpha = D3D11_BLEND_ONE;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        ok = ok && SUCCEEDED(d.device->CreateBlendState(&bd, &d.bandBlend));
    }
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
    d.vendor = fsr::adapter_vendor(d.device.Get());
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
    return make_texture2d(d.device.Get(), texture, w, h, format, bind);
}

// The upscaler the run asks for on this GPU: AMD FSR when asked, or with fsrWhereNoDlss where NGX can't run (a GPU not
// NVIDIA's, or NGX refused to start).
int wanted_backend() {
    return d.cfg.upscaler == 1 || (d.cfg.fsrWhereNoDlss && (d.vendor != fsr::kVendorNvidia || d.ngxRefused)) ? 1 : 0;
}

// The overlay's name for what runs (dlss_upscaler_badge): 0 DLSS, else FSR's provider major * 100 + minor.
std::atomic<int> badge{0};

const char* backend_name(int backend) { return backend == 1 ? "AMD FSR" : "NVIDIA DLSS"; }

// A view's feature let go of by its upscaler (FSR's once both devices are through its work, with our views of its
// textures), the slot emptied.
void release_slot(Slot& s) {
    if (s.feature)
        ngx::release(s.feature);
    if (s.fsrView) {
        ComPtr<ID3D11DeviceContext> context;
        if (d.device)
            d.device->GetImmediateContext(&context);
        IUnknown* const keep[] = {s.depth.Get(),    s.vectors.Get(),    s.output.Get(),   s.colourIn.Get(),
                                  s.depthRtv.Get(), s.vectorsRtv.Get(), s.depthSrv.Get(), s.vectorsSrv.Get()};
        fsr::release(context.Get(), s.fsrView, keep, static_cast<unsigned>(std::size(keep)));
    }
    s = Slot{};
}

// The upscaler the run asks for, started (once a session each; a second start answers as the first did). False: it
// couldn't start (given up for the session, the log says why).
bool start_backend() {
    const int want = wanted_backend();
    if (d.backend == want)
        return true;
    std::string why;
    char focusMode[96] = "";
    if (d.cfg.views >= 2)
        std::snprintf(focusMode, sizeof(focusMode), " and the focus views (%s, %d%%)",
                      want == 1 && d.cfg.focusScale >= 100 ? "native AA" : dlss::focus_mode_name(d.cfg.focusScale),
                      d.cfg.focusScale);
    if (want == 1) {
        const fsr::Provider provider = d.cfg.fsrProvider == 3 ? fsr::Provider::Fsr3 : fsr::Provider::Newest;
        if (!fsr::start(d.device.Get(), d.cfg.fsrFolder.c_str(), provider, why)) {
            give_up(("AMD FSR: " + why).c_str());
            return false;
        }
        const std::wstring folder = fsr::loaded_folder();
        say("AMD FSR started on our Direct3D 12 device on the game's graphics card (vendor 0x%04x%s; AMD's runtime from %ls); "
            "providers it offers here: %s; asked for: %s; views: the eyes%s; jitter %s (%u phases); NGX's presets don't apply",
            d.vendor, d.cfg.upscaler == 1 ? "" : ", FSR as the GPU is AMD's", folder.c_str(), fsr::providers().c_str(),
            provider == fsr::Provider::Fsr3 ? "the newest FSR 3.x" : "the newest", focusMode, d.cfg.jitter ? "on" : "off",
            d.phases);
    } else {
        const std::wstring data = (std::filesystem::path(simqvfr::log_directory()) / L"ngx").wstring();
        if (!ngx::start(d.device.Get(), d.cfg.dllFolder.c_str(), data.c_str(), why)) {
            if (d.cfg.fsrWhereNoDlss && fsr::built_in() && !d.ngxRefused) {  // an older NVIDIA card: FSR instead
                d.ngxRefused = true;
                say("NGX didn't start (%s): AMD FSR in its place", why.c_str());
                return start_backend();
            }
            give_up(why.c_str());
            return false;
        }
        const std::wstring lookedIn =
            d.cfg.dllFolder.empty() ? std::wstring(L"Sim-QVFR's folder") : d.cfg.dllFolder + L", then Sim-QVFR's folder";
        badge = 0;
        say("NGX started on the game's device (nvngx_dlss.dll looked for in %ls); views: the eyes%s; jitter %s (%u phases); "
            "presets: the eyes %s, the focus views %s",
            lookedIn.c_str(), focusMode, d.cfg.jitter ? "on" : "off", d.phases,
            dlss::preset_name(d.cfg.eyePreset >= 0 ? d.cfg.eyePreset : d.cfg.preset), dlss::preset_name(d.cfg.preset));
    }
    for (Slot& s : d.slots)  // another upscaler's features: made again by this one at their next pass
        if (s.feature || s.fsrView)
            release_slot(s);
    d.backend = want;
    return true;
}

// The view's feature and textures at these sizes, drawn at w x h and made into ow x oh (made again when they change: a
// live resolution change, super resolution starting or stopping, another upscaler).
bool ensure(ID3D11DeviceContext* context, Slot& s, int index, unsigned w, unsigned h, unsigned ow, unsigned oh,
            DXGI_FORMAT format, bool inverted) {
    const bool amd = d.backend == 1;
    // NGX's presets (the eyes are views 0-1); FSR has none.
    const int preset = amd ? 0 : index < 2 && d.cfg.eyePreset >= 0 ? d.cfg.eyePreset : d.cfg.preset;
    const bool infinite = amd && d.cameraInfinite;
    s.waiting = false;
    if (s.width == w && s.height == h && s.outWidth == ow && s.outHeight == oh && s.format == format &&
        s.inverted == inverted && s.preset == preset && s.backend == d.backend && s.infinite == infinite)
        return s.feature != nullptr || s.fsrView != nullptr;
    // Only its size changed (live resolution): made again once the size has settled (kResizeSettleMs).
    if ((s.feature || s.fsrView) && s.format == format && s.inverted == inverted && s.preset == preset &&
        s.backend == d.backend && s.infinite == infinite) {
        const unsigned long long now = GetTickCount64();
        if (s.waitW != w || s.waitH != h || s.waitOW != ow || s.waitOH != oh) {
            s.waitW = w, s.waitH = h, s.waitOW = ow, s.waitOH = oh;
            s.waitSince = now;
        }
        // Settled: one view's feature a frame, so the two focus views' rebuilds are two short hitches, not one long one.
        if (now - s.waitSince < kResizeSettleMs || d.resizedFrame == d.frame) {
            s.waiting = true;
            ++d.resizeWaits;
            return false;
        }
        d.resizedFrame = d.frame;
    }
    if (s.feature || s.fsrView) {
        release_slot(s);
        ++d.recreated;
    }
    s = Slot{};
    const auto note = [&] {  // what the slot is for (kept after a refusal: not tried again at the same sizes)
        s.width = w, s.height = h, s.outWidth = ow, s.outHeight = oh, s.preset = preset, s.format = format;
        s.inverted = inverted, s.backend = d.backend, s.infinite = infinite;
    };
    note();
    const bool upscale = ow != w || oh != h;
    const UINT targets = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    unsigned result = 0;
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);
    bool ok = true;
    if (amd) {  // FSR's context first: our depth, vectors and its output are its shared textures
        s.fsrView = fsr::create(w, h, ow, oh, format, inverted, infinite, result);
        ok = s.fsrView != nullptr;
        if (ok) {
            const fsr::Textures t = fsr::textures(s.fsrView);
            s.depth = t.depth;
            s.vectors = t.vectors;
            s.output = t.output;
            s.colourIn = t.colour;
        }
    } else {
        ok = make(s.depth, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
             make(s.vectors, w, h, DXGI_FORMAT_R16G16_FLOAT, targets) && make(s.output, ow, oh, format, targets);
    }
    const bool textures = ok;
    ok = ok && SUCCEEDED(d.device->CreateRenderTargetView(s.depth.Get(), nullptr, &s.depthRtv)) &&
         SUCCEEDED(d.device->CreateRenderTargetView(s.vectors.Get(), nullptr, &s.vectorsRtv)) &&
         SUCCEEDED(d.device->CreateShaderResourceView(s.depth.Get(), nullptr, &s.depthSrv)) &&
         SUCCEEDED(d.device->CreateShaderResourceView(s.vectors.Get(), nullptr, &s.vectorsSrv));
    for (int i = 0; i < 2 && ok && d.cfg.gate; ++i)
        ok = make(s.colour[i], w, h, format, D3D11_BIND_SHADER_RESOURCE) &&
             SUCCEEDED(d.device->CreateShaderResourceView(s.colour[i].Get(), nullptr, &s.colourSrv[i]));
    if (ok && d.cfg.gate)  // the gate's depth test: the frame before's depth
        ok = make(s.depthBefore, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE) &&
             SUCCEEDED(d.device->CreateShaderResourceView(s.depthBefore.Get(), nullptr, &s.depthBeforeSrv));
    if (ok && !amd)
        s.feature = ngx::create(context, w, h, ow, oh, inverted, preset, result);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    if (amd ? ok : s.feature != nullptr)  // what a live change's stutter is made of: each view's feature made again
        say("view %d's feature made in %.1f ms (%ux%u%s, %s%s)", index,
            1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart), w, h,
            upscale ? " upscaled" : "", amd ? "AMD FSR" : "preset ", amd ? "" : dlss::preset_name(preset));
    int madeWith = preset;  // the preset the feature was made with
    if (!amd && !s.feature && ok && preset != 0) {  // a DLL without that preset: NGX's default for the mode instead
        unsigned again = 0;
        s.feature = ngx::create(context, w, h, ow, oh, inverted, 0, again);
        if (s.feature) {
            say("view %d: NGX refused preset %s (NGX 0x%08x): made with its default for the mode instead", index,
                dlss::preset_name(preset), result);
            madeWith = 0;
        } else {
            result = again;
        }
    }
    if (!(amd ? ok : s.feature != nullptr)) {
        say("view %d's feature at %ux%u%s (preset %s, format %d) couldn't be made (%s, %s 0x%08x)", index, w, h,
            upscale ? " for an upscale" : "", dlss::preset_name(preset), static_cast<int>(format),
            !textures && !amd ? "its textures" : amd ? (textures ? "its views" : "FSR refused") : "NGX refused",
            amd ? "FSR" : "NGX", result);
        if (s.fsrView || s.feature)
            release_slot(s);
        note();
        s.refused = true;
        if (upscale) {  // the focus views go back to their size and DLAA (from the next frame: dlss_focus_scale)
            if (!d.focusSrOff)
                say("super resolution on the focus views off for this session: they're drawn at their size with %s",
                    amd ? "FSR's native AA" : "DLAA");
            d.focusSrOff = true;
        } else if (++d.createFailures >= 4) {
            give_up("the features can't be made");
        }
        return false;
    }
    if (amd) {
        if (upscale)
            say("view %d's FSR context made at %ux%u up to %ux%u (%s, %d%%, format %d, depth %s%s)", index, w, h, ow, oh,
                dlss::focus_mode_name(d.cfg.focusScale), d.cfg.focusScale, static_cast<int>(format),
                inverted ? "inverted" : "0 near", infinite ? ", infinite far plane" : "");
        else
            say("view %d's FSR context made at %ux%u (native AA, format %d, depth %s%s)", index, w, h,
                static_cast<int>(format), inverted ? "inverted" : "0 near", infinite ? ", infinite far plane" : "");
        if (!d.fsrProviderNamed) {
            d.fsrProviderNamed = true;
            say("FSR provider in use: %s", fsr::provider_in_use().c_str());
        }
        int major = 0, minor = 0;
        if (sscanf_s(fsr::provider_in_use().c_str(), "%d.%d", &major, &minor) < 1)
            major = 3, minor = 1;
        badge = major * 100 + minor;
        return true;
    }
    // NGX's own line on the preset it took for the feature's mode follows (ngx_dlss.cpp), "ngx: (DLAA) Using App hint
    // Preset F" or "... using title default Preset K".
    if (upscale)
        say("view %d's DLSS feature made at %ux%u up to %ux%u (%s, preset %s, format %d, depth %s)", index, w, h, ow, oh,
            dlss::focus_mode_name(d.cfg.focusScale), dlss::preset_name(madeWith), static_cast<int>(format),
            inverted ? "inverted" : "0 near");
    else
        say("view %d's DLAA feature made at %ux%u (preset %s, format %d, depth %s)", index, w, h, dlss::preset_name(madeWith),
            static_cast<int>(format), inverted ? "inverted" : "0 near");
    if (!d.dllNamed) {  // which one NGX took: the player's, Sim-QVFR's own, or one beside the game's exe
        d.dllNamed = true;
        const std::wstring dll = ngx::loaded_dll();
        say("nvngx_dlss.dll in use: %ls", dll.empty() ? L"(none loaded in this process)" : dll.c_str());
    }
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
// after it, call by call (d3d11_state.h). (Not a context state swap, ID3D11DeviceContext1::SwapDeviceContextState: in
// the run of 2026-11-10 15:53 the multi-view pass tracker then saw each eye pass end right after its resolve and no
// focus view was made.) Everything here happens inside the caller's window, in which its hooks pass calls through.
// Every slot NGX's evaluation could bind: all API constant buffer and sampler slots, all vertex buffers; 32 of the 128
// resource slots and feature level 11.0's 8 UAV slots. Ours and NGX's inputs and outputs off first (unbindFirst).
d3d11_state::Spec full_state() {
    using G = D3D11StateGuard;
    d3d11_state::Spec s;
    s.parts = d3d11_state::kAll;
    s.vsCb = s.psCb = s.csCb = {0, G::kCbs};
    s.vsSrv = s.psSrv = s.csSrv = {0, G::kSrvs};
    s.vsSamplers = s.psSamplers = s.csSamplers = {0, G::kSamplers};
    s.csUavs = {0, G::kUavs};
    s.instances = true;
    s.emptyRects = true;
    s.unbindFirst = true;
    return s;
}

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
              const Jitter& jitter, double shiftX, double shiftY, bool inverted, const double depthTerms[2], CarTest car,
              const DlssPass& pass, const Camera& camera, unsigned bandHeightOfView = 0) {
    Slot& s = d.slots[index];
    if (!ensure(context, s, index, w, h, ow, oh, format, inverted)) {
        if (!s.waiting)  // a size settling isn't a failure (counted as resizeWaits)
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
    // Moving objects: with the frame before's view, each object surface's point goes back with its object's motion.
    M4 inv = dlss::identity();
    const bool objects = ok && pass.objectSlots && pass.objectMotion && pass.objectCount && dlss::invert(rendered[use], inv);
    to_floats(inv, c.inv);
    to_floats(objects ? s.beforeUnjittered[use] : dlss::identity(), c.before);
    c.objects[0] = objects ? static_cast<float>(pass.objectCount) : 0.0f;
    // A band of the view's rows (y its top): the pixels' place in the whole view, and the band's move since the frame
    // before (the frame before's pixel sits that many rows lower in its band).
    if (bandHeightOfView) {
        c.band[0] = static_cast<float>(y);
        c.band[1] = static_cast<float>(bandHeightOfView);
        c.band[2] = history ? static_cast<float>(static_cast<int>(y) - static_cast<int>(s.beforeBandTop)) : 0.0f;
    }
    if (objects)
        ++d.objectViews;
    context->UpdateSubresource(d.constants.Get(), 0, nullptr, &c, 0, 0);
    ID3D11RenderTargetView* rtvs[2] = {s.depthRtv.Get(), s.vectorsRtv.Get()};
    context->OMSetRenderTargets(2, rtvs, nullptr);
    ID3D11ShaderResourceView* inputs[3] = {d.depthIn, objects ? pass.objectSlots : nullptr,
                                           objects ? pass.objectMotion : nullptr};
    context->PSSetShaderResources(0, 3, inputs);
    // The depth target's own samples with the scene's slots; our copy (single-sample) beside multisample slots; or both
    // single-sample.
    draw_triangle(context, d.depthInMs ? d.vectorsMs.Get() : d.depthMs ? d.vectorsCopyMs.Get() : d.vectorsSs.Get(), w, h);
    if (!ok || !d.cfg.vectors) {
        const float zero[4] = {};
        context->ClearRenderTargetView(s.vectorsRtv.Get(), zero);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none[3] = {};
    context->PSSetShaderResources(0, 3, none);

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

    // DLSS-NR (nr_views.h) on the view's colour as it is in the scene now, cw x ch from (x, y): before DLSS on the view
    // as drawn, else on DLSS's output. An eye's view takes its focus view's place from the remap.
    const auto neural = [&](unsigned cw, unsigned ch) {
        NrView nv;
        nv.index = index;
        nv.scene = resolved;
        nv.format = format;
        nv.x = x, nv.y = y, nv.width = cw, nv.height = ch;
        nv.depth = s.depthSrv.Get();
        nv.vectors = s.vectorsSrv.Get();
        nv.guideWidth = w, nv.guideHeight = h;
        nv.depthInverted = inverted;
        nv.reset = !history;
        if (index < 2 && pass.focus)
            nv.focusKnown = nr::focus_centre(pass.remap, nv.focusU, nv.focusV);
        nr_run_view(context, nv);
    };
    if (nr_view_wanted(index) && nr_before_dlss())
        neural(w, h);

    ++s.runs;
    // The jitter in render pixels, as it reached the projection (x right, y down): NGX's and FSR's convention alike
    // (FSR's sample puts it in as the clip shift 2 jx / w, -2 jy / h: dlss::clip_shift_for's).
    const float jx = jitter.on ? static_cast<float>(d.cfg.flipX ? -jitter.sx : jitter.sx) : 0;
    const float jy = jitter.on ? static_cast<float>(d.cfg.flipY ? -jitter.sy : jitter.sy) : 0;
    unsigned result = 0;
    bool done = false;
    if (s.fsrView) {
        // FSR reads no sub-rectangle of the scene: the view's colour copied into its own texture (the one copy DLSS
        // doesn't make; depth and vectors were drawn into FSR's textures above).
        const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
        context->CopySubresourceRegion(s.colourIn.Get(), 0, 0, 0, 0, resolved, 0, &box);
        fsr::Evaluation fe;
        fe.width = w;
        fe.height = h;
        fe.jitterX = jx;
        fe.jitterY = jy;
        fe.reset = !history;
        fe.cameraNear = camera.nearM;
        fe.cameraFar = camera.farM;
        fe.fovY = camera.fovY;
        fe.frameMs = static_cast<float>(d.frameMs);
        done = fsr::evaluate(context, s.fsrView, fe, result);
    } else {
        ngx::Evaluation e;
        e.colour = resolved;
        e.colourX = x;
        e.colourY = y;
        e.depth = s.depth.Get();
        e.vectors = s.vectors.Get();
        e.output = s.output.Get();
        e.width = w;
        e.height = h;
        e.jitterX = jx;
        e.jitterY = jy;
        e.reset = !history;
        done = ngx::evaluate(context, s.feature, e, result);
    }
    if (done) {
        context->CopySubresourceRegion(resolved, 0, x, y, 0, s.output.Get(), 0, nullptr);  // ow x oh, from (x, y)
        if (nr_view_wanted(index) && !nr_before_dlss())
            neural(ow, oh);
        ++d.evaluations;
        ++d.evaluationsTotal;
    } else {
        if (d.failures < 5)
            say("frame %llu view %d: the evaluation failed (%s 0x%08x)", d.frame, index, s.fsrView ? "FSR" : "NGX", result);
        ++d.failures;
    }
    s.beforeUnjittered[0] = unjittered[0];
    s.beforeUnjittered[1] = unjittered[1];
    s.beforeShift[0] = jitter.on ? shiftX : 0;
    s.beforeShift[1] = jitter.on ? shiftY : 0;
    s.beforeFrame = d.frame;
    s.haveBefore = true;
    s.beforeBandTop = y;
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
    const D3D11StateGuard guard(context, full_state());
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

// A focus view (fw x fh at (x, 0)) after DLSS, where the game drew only part of it at full quality (DlssPass::focusKeep,
// focusBandTop): what isn't kept takes the eye's image (the top-left eyeW x eyeH, DLSS's output) through the focus view's
// clip remap without jitter (remap: a, b, c, d), blended in by the eye's share. Drawn into the scene itself where it
// takes a render target view, else through a copy of the focus view. Inside dlss_after_resolve's state guard. False:
// not drawn (the focus view as DLSS left it).
bool band_outside(ID3D11DeviceContext* context, ID3D11Resource* resolved, DXGI_FORMAT format, unsigned eyeW, unsigned eyeH,
                  unsigned x, unsigned fw, unsigned fh, unsigned top, unsigned bandH, const double remap[4],
                  const DlssPass& pass) {
    if (!d.bandOutside || !d.bandBlend || std::fabs(remap[0]) < 1e-9 || std::fabs(remap[2]) < 1e-9)
        return false;
    // The eye's region under the focus view: its corners through the remap's inverse, 2 pixels wider, in the eye.
    const double ex0 = (-1 - remap[1]) / remap[0], ex1 = (1 - remap[1]) / remap[0];
    const double ey0 = (-1 - remap[3]) / remap[2], ey1 = (1 - remap[3]) / remap[2];
    const auto px = [&](double e) { return (e + 1) * 0.5 * eyeW; };
    const auto py = [&](double e) { return (1 - e) * 0.5 * eyeH; };
    const int left = (std::max)(0, static_cast<int>(std::floor((std::min)(px(ex0), px(ex1)))) - 2);
    const int right = (std::min)(static_cast<int>(eyeW), static_cast<int>(std::ceil((std::max)(px(ex0), px(ex1)))) + 2);
    const int upper = (std::max)(0, static_cast<int>(std::floor((std::min)(py(ey0), py(ey1)))) - 2);
    const int lower = (std::min)(static_cast<int>(eyeH), static_cast<int>(std::ceil((std::max)(py(ey0), py(ey1)))) + 2);
    if (right <= left || lower <= upper)
        return false;
    const unsigned rw = static_cast<unsigned>(right - left), rh = static_cast<unsigned>(lower - upper);
    // The scene as a render target, where it is one (made once per scene texture).
    if (resolved != d.bandScene) {
        d.bandScene = resolved;
        d.bandSceneRtv.Reset();
        ComPtr<ID3D11Texture2D> scene;
        D3D11_TEXTURE2D_DESC sd{};
        if (SUCCEEDED(resolved->QueryInterface(IID_PPV_ARGS(&scene))))
            scene->GetDesc(&sd);
        if (scene && (sd.BindFlags & D3D11_BIND_RENDER_TARGET) && sd.SampleDesc.Count == 1 &&
            FAILED(d.device->CreateRenderTargetView(resolved, nullptr, &d.bandSceneRtv)))
            d.bandSceneRtv.Reset();
    }
    const bool direct = d.bandSceneRtv != nullptr;
    const unsigned size[4] = {rw, rh, direct ? 0u : fw, direct ? 0u : fh};
    if (std::memcmp(size, d.bandSize, sizeof(size)) != 0 || d.bandFormat != format) {
        std::memcpy(d.bandSize, size, sizeof(size));
        d.bandFormat = format;
        d.bandSrv.Reset();
        d.bandRtv.Reset();
        d.bandOut.Reset();
        if (!make(d.bandIn, rw, rh, format, D3D11_BIND_SHADER_RESOURCE) ||
            FAILED(d.device->CreateShaderResourceView(d.bandIn.Get(), nullptr, &d.bandSrv)) ||
            (!direct && (!make(d.bandOut, fw, fh, format, D3D11_BIND_RENDER_TARGET) ||
                         FAILED(d.device->CreateRenderTargetView(d.bandOut.Get(), nullptr, &d.bandRtv))))) {
            d.bandSrv.Reset();
            d.bandRtv.Reset();
            if (!d.bandRefusedLogged)
                say("a focus view's composition after DLSS (%ux%u from the eye's %ux%u, format %d) couldn't be made", fw,
                    fh, rw, rh, static_cast<int>(format));
            d.bandRefusedLogged = true;
        }
    }
    if (!d.bandSrv || (!direct && !d.bandRtv))
        return false;
    const D3D11_BOX box{static_cast<UINT>(left), static_cast<UINT>(upper), 0, static_cast<UINT>(right),
                        static_cast<UINT>(lower), 1};
    context->CopySubresourceRegion(d.bandIn.Get(), 0, 0, 0, 0, resolved, 0, &box);
    if (!direct) {  // the focus view as DLSS left it, blended over in our copy
        const D3D11_BOX focusBox{x, 0, 0, x + fw, fh, 1};
        context->CopySubresourceRegion(d.bandOut.Get(), 0, 0, 0, 0, resolved, 0, &focusBox);
    }
    Constants c{};
    c.r[0] = direct ? static_cast<float>(x) : 0.0f;
    c.r[1] = static_cast<float>(top);
    c.r[2] = static_cast<float>(bandH);
    c.r[3] = pass.focusKeepFeather;
    const bool keep = pass.focusKeep && pass.focusKeepWidth && pass.focusKeepHeight;
    c.r[4] = static_cast<float>(pass.focusKeepWidth);
    c.r[5] = static_cast<float>(pass.focusKeepHeight);
    c.r[6] = keep ? 1.0f : 0.0f;
    c.r[7] = pass.focusStamps ? 1.0f : 0.0f;
    std::memcpy(&c.r[8], &pass.focusStamp, sizeof(pass.focusStamp));
    c.js[0] = static_cast<float>(eyeW);
    c.js[1] = static_cast<float>(eyeH);
    c.js[2] = static_cast<float>(fw);
    c.js[3] = static_cast<float>(fh);
    c.origin[0] = static_cast<float>(left);
    c.origin[1] = static_cast<float>(upper);
    c.origin[2] = static_cast<float>(rw);
    c.origin[3] = static_cast<float>(rh);
    for (int i = 0; i < 4; ++i)
        c.extra[i] = static_cast<float>(remap[i]);
    context->UpdateSubresource(d.constants.Get(), 0, nullptr, &c, 0, 0);
    ID3D11RenderTargetView* target = direct ? d.bandSceneRtv.Get() : d.bandRtv.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    ID3D11ShaderResourceView* inputs[3] = {d.bandSrv.Get(), keep ? pass.focusKeep : nullptr, pass.focusStamps};
    context->PSSetShaderResources(0, 3, inputs);
    context->PSSetSamplers(0, 1, d.linearClamp.GetAddressOf());
    context->OMSetBlendState(d.bandBlend.Get(), nullptr, 0xffffffff);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(d.vs.Get(), nullptr, 0);
    context->PSSetShader(d.bandOutside.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, d.constants.GetAddressOf());
    const D3D11_VIEWPORT vp{direct ? static_cast<float>(x) : 0.0f, 0, static_cast<float>(fw), static_cast<float>(fh), 0, 1};
    context->RSSetViewports(1, &vp);
    context->Draw(3, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none[3] = {};
    context->PSSetShaderResources(0, 3, none);
    if (!direct)
        context->CopySubresourceRegion(resolved, 0, x, 0, 0, d.bandOut.Get(), 0, nullptr);
    d.bandDirect += direct ? 1 : 0;
    return true;
}

// Runs `f` on the way out of a scope.
template <class F> struct OnExit {
    F f;
    ~OnExit() { f(); }
};
template <class F> OnExit(F) -> OnExit<F>;

// The measurement (DlssRun::depthMeasure), every ~2 s, the eyes in turn: every 8th texel of this pass's copy (before
// its transparents) and of the depth target now (after them) into the grid, then to a staging texture read at a later
// frame end (read_depth_measure). Inside dlss_after_resolve's state guard.
void measure_depth(ID3D11DeviceContext* context, int eye, unsigned eyeW, unsigned eyeH, unsigned focusW, unsigned focusH,
                   const double terms[2]) {
    State::DepthCopy& c = d.copy;
    if (d.frame < c.nextFrame || eye != c.nextEye)
        return;
    const int k = c.taken[0].pending ? 1 : 0;
    if (c.taken[k].pending)
        return;
    const unsigned gw = (c.texW + 7) / 8, gh = (c.texH + 7) / 8;
    if (!c.grid || c.gridW != gw || c.gridH != gh) {
        c.grid.Reset();
        c.gridRtv.Reset();
        c.gridRead[0].Reset();
        c.gridRead[1].Reset();
        c.gridW = c.gridH = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = gw, td.Height = gh, td.MipLevels = td.ArraySize = 1, td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32_FLOAT, td.Usage = D3D11_USAGE_STAGING, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!make(c.grid, gw, gh, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_RENDER_TARGET) ||
            FAILED(d.device->CreateRenderTargetView(c.grid.Get(), nullptr, &c.gridRtv)) ||
            FAILED(d.device->CreateTexture2D(&td, nullptr, &c.gridRead[0])) ||
            FAILED(d.device->CreateTexture2D(&td, nullptr, &c.gridRead[1]))) {
            c.grid.Reset();
            c.nextFrame = ~0ull;  // not again this session
            say("the depth measurement's textures couldn't be made: not measured");
            return;
        }
        c.gridW = gw;
        c.gridH = gh;
    }
    const unsigned w = eyeW + focusW, h = (std::max)(eyeH, focusH);
    context->OMSetRenderTargets(1, c.gridRtv.GetAddressOf(), nullptr);
    ID3D11ShaderResourceView* in[2] = {c.srv.Get(), d.depthSrv.Get()};
    context->PSSetShaderResources(0, 2, in);
    draw_triangle(context, d.depthMs ? d.measureMs.Get() : d.measureSs.Get(), (w + 7) / 8, (h + 7) / 8);
    ID3D11ShaderResourceView* none[2] = {};
    context->PSSetShaderResources(0, 2, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(c.gridRead[k].Get(), c.grid.Get());
    State::DepthCopy::Taken& t = c.taken[k];
    t.pending = true;
    t.eye = eye;
    t.frame = d.frame;
    t.eyeW = eyeW, t.eyeH = eyeH, t.focusW = focusW, t.focusH = focusH;
    t.terms[0] = terms[0], t.terms[1] = terms[1];
    t.nearM = d.cfg.depthCopyNear;
    c.nextFrame = d.frame + 180;
    c.nextEye = 1 - eye;
}

// One region's points (x0..x1, y0..y1 in texels) of a read measurement: what the transparents drew nearer, as a
// sentence into `out`.
void depth_measure_region(const char* name, const State::DepthCopy::Taken& t, const float* grid, unsigned pitchFloats,
                          unsigned x0, unsigned x1, unsigned y0, unsigned y1, char* out, size_t outSize) {
    std::vector<float> nearer, behind;
    unsigned points = 0, sky = 0;
    const auto metres = [&](float depth) {  // reversed depth: 0 far (the sky), larger nearer
        const double v = depth - t.terms[0];
        return v > 1e-9 ? t.terms[1] / v : 1e9;
    };
    for (unsigned y = y0 / 8; y < (y1 + 7) / 8 && y < d.copy.gridH; ++y)
        for (unsigned x = x0 / 8; x < (x1 + 7) / 8 && x < d.copy.gridW; ++x) {
            const unsigned px = x * 8 + 4, py = y * 8 + 4;
            if (px < x0 || px >= x1 || py < y0 || py >= y1)
                continue;
            const float before = grid[y * pitchFloats + x * 2], after = grid[y * pitchFloats + x * 2 + 1];
            ++points;
            if (after > before + 1e-7f + 1e-4f * before) {  // drawn nearer after the copy
                nearer.push_back(static_cast<float>(metres(after)));
                const double was = metres(before);
                sky += was >= 1e8 ? 1 : 0;
                behind.push_back(static_cast<float>((std::min)(was, 1e6)));
            }
        }
    if (!points) {
        std::snprintf(out, outSize, "%s: no points", name);
        return;
    }
    if (nearer.empty()) {
        std::snprintf(out, outSize, "%s: %u points, none drawn nearer", name, points);
        return;
    }
    const auto q = [](std::vector<float>& v, double at) {
        const size_t i = (std::min)(v.size() - 1, static_cast<size_t>(at * static_cast<double>(v.size())));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
        return v[i];
    };
    const size_t n = nearer.size();
    size_t beyond = 0;  // drawn nearer, but at or beyond DlssRun::depthCopyNear: left as drawn
    for (const float m : nearer)
        beyond += t.nearM > 0 && m >= t.nearM ? 1 : 0;
    const float n25 = q(nearer, 0.25), n50 = q(nearer, 0.5), n75 = q(nearer, 0.75);
    const float b25 = q(behind, 0.25), b50 = q(behind, 0.5), b75 = q(behind, 0.75);
    std::snprintf(out, outSize,
                  "%s: %.1f%% of %u points drawn nearer, at %.2f m (middle half %.2f-%.2f) where the depth before was "
                  "%.1f m (%.1f-%.1f; %.0f%% sky); %.0f%% of them %.1f m or more away (left as drawn)",
                  name, 100.0 * static_cast<double>(n) / points, points, n50, n25, n75, b50, b25, b75,
                  100.0 * sky / static_cast<double>(n), 100.0 * static_cast<double>(beyond) / static_cast<double>(n),
                  t.nearM);
}

// The frame end: measurements whose copies have arrived, logged.
void read_depth_measure(ID3D11DeviceContext* context) {
    State::DepthCopy& c = d.copy;
    for (int k = 0; k < 2; ++k) {
        State::DepthCopy::Taken& t = c.taken[k];
        if (!t.pending || !c.gridRead[k] || d.frame < t.frame + 2)
            continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(c.gridRead[k].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)))
            continue;
        char eyeText[200], focusText[200] = "";
        const float* grid = static_cast<const float*>(m.pData);
        const unsigned pitch = m.RowPitch / sizeof(float);
        depth_measure_region("eye", t, grid, pitch, 0, t.eyeW, 0, t.eyeH, eyeText, sizeof(eyeText));
        if (t.focusW && t.focusH)
            depth_measure_region("; focus view", t, grid, pitch, t.eyeW, t.eyeW + t.focusW, 0, t.focusH, focusText,
                                 sizeof(focusText));
        context->Unmap(c.gridRead[k].Get(), 0);
        t.pending = false;
        say("depth before the transparents, eye %d, frame %llu (every 8th texel; the copy against the depth at the pass's "
            "end): %s%s",
            t.eye, t.frame, eyeText, focusText);
    }
}

}  // namespace

void dlss_start(const DlssRun& run) {
    {
        std::lock_guard guard(d.mutex);
        d.livePending = false;
    }
    d.cfg = run;
    d.off = d.focusSrOff = false;
    d.cfg.focusScale = (std::max)(33, (std::min)(100, run.focusScale));
    // The jitter's sequence: as long as the smallest render asks (the eyes and the focus views share its phase).
    d.phases = d.cfg.views >= 2 ? dlss::jitter_phases(d.cfg.focusScale / 100.0) : dlss::kJitterPhases;
    if (run.views && !ngx::built_in() && !(fsr::built_in() && (run.upscaler == 1 || run.fsrWhereNoDlss))) {
        say("asked for, but this build has no NGX SDK (CMake: NGX_SDK_DIR): off");
        d.cfg.views = 0;
    }
    if (run.views && (run.upscaler == 1 || run.fsrWhereNoDlss) && !fsr::built_in()) {
        say("AMD FSR asked for, but this build has no FSR SDK (CMake: FFX_SDK_DIR): DLSS instead");
        d.cfg.upscaler = 0;
        d.cfg.fsrWhereNoDlss = false;
    }
    d.backend = d.ready ? d.backend : -1;
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
    if (d.cfg.views && (d.cfg.upscaler == 1 || d.cfg.fsrWhereNoDlss))
        say("the upscaler: %s (AMD's provider: %s)",
            d.cfg.upscaler == 1 ? "AMD FSR" : "NVIDIA DLSS, AMD FSR where DLSS can't run (not an NVIDIA GPU, or NGX refuses)",
            d.cfg.fsrProvider == 3 ? "the newest FSR 3.x" : "the newest it offers");
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
    if (!dlss_wanted() || d.cfg.views < 2 || !d.cfg.jitter || d.jitterDead || !d.lastFrameOk || !d.focusOk[eye] ||
        !focusWidth || !focusHeight)
        return;
    Jitter j;
    j.on = true;
    dlss::jitter_pixels(d.phase, j.sx, j.sy, d.phases);
    dlss::clip_shift_for(j.sx, j.sy, focusWidth, focusHeight, j.cx, j.cy);
    b += static_cast<float>(j.cx);
    dd += static_cast<float>(j.cy);
    d.focus[eye] = j;
}

bool dlss_mip_bias(int eye, float& focus, float& eyeBias) {
    focus = eyeBias = 0;
    if (eye < 0 || eye > 1 || !dlss_wanted() || d.cfg.textureSharpening <= 0)
        return false;
    const double scale = dlss_focus_scale();  // render over output: 1 under DLAA
    std::lock_guard guard(d.mutex);
    if (d.cfg.views >= 2 && d.focusOk[eye])
        focus = static_cast<float>(std::log2(scale) - 1.0);
    if (d.cfg.textureSharpening >= 2 && d.lastFrameOk)
        eyeBias = -1.0f;
    return focus != 0 || eyeBias != 0;
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
    // Whether DLSS processed the eye and its focus view this frame, noted on every way out: the next frame's jitter
    // goes only to views it processed (dlss_frame_end), so a view left as drawn (its feature refused, an evaluation
    // failed) never shows the jitter as a shake.
    bool eyeDone = false, focusDone = false;
    const OnExit noteDone{[&] {
        d.evaluated[pass.eye] = eyeDone;
        d.focusEvaluated[pass.eye] = focusDone;
    }};
    if (!dlss_wanted())
        return;
    if (!d.ready && !initialize(context))
        return;
    if (!start_backend())  // the upscaler asked for (a live change may have switched it)
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
        // A pass without them is left as drawn; only a run of them is the game's way (a game that never gives them).
        ++d.failures;
        if (++d.matricesMissing[pass.eye & 1] >= kMatricesMissingLimit)
            give_up("the game's view and projection for the pass aren't a rigid view and a projection (its log lines say "
                    "more)");
        return;
    }
    d.matricesMissing[pass.eye & 1] = 0;
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
    // FSR's camera: the planes from the depth terms, the vertical FOV from the eye's y scale (the focus view's through
    // its remap). NGX takes none of it.
    Camera eyeCamera, focusCamera;
    {
        double nearM = 0, farM = 0;
        bool infinite = false;
        if (dlss::near_far(depthTerms[0], depthTerms[1], nearM, farM, infinite)) {
            eyeCamera.nearM = focusCamera.nearM = static_cast<float>(nearM);
            eyeCamera.farM = focusCamera.farM = static_cast<float>(farM);
            d.cameraInfinite = infinite;
        }
        eyeCamera.fovY = static_cast<float>(dlss::vertical_fov(projection[5]));
        focusCamera.fovY = static_cast<float>(dlss::vertical_fov(projection[5], pass.remap[2]));
    }
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
    // The depth the vectors and DLSS read: the copy made before the pass's transparents wrote theirs, where this pass
    // made it of this depth target and over both views' parts; else the depth target as it is now.
    State::DepthCopy& dc = d.copy;
    const unsigned copyW = pass.eyeWidth + (focus ? rw : 0), copyH = (std::max)(pass.eyeHeight, focus ? rh : 0u);
    const bool copied = pass.depthCopied && dc.srv && dc.source == d.depthFor && dc.w >= copyW && dc.h >= copyH;
    const bool readCopy = copied && pass.depthCopyRead;
    d.depthIn = readCopy ? dc.srv.Get() : d.depthSrv.Get();
    d.depthInMs = !readCopy && d.depthMs;
    dc.used += readCopy ? 1 : 0;
    dc.measured += copied && !readCopy ? 1 : 0;
    if (!d.firstLogged) {
        d.firstLogged = true;
        char drawn[64] = "";
        if (focus && (rw < fw || rh < fh))
            std::snprintf(drawn, sizeof(drawn), " (drawn at %ux%u of its %ux%u)", rw, rh, fw, fh);
        say("first pass: eye %d %ux%u%s%s in a %ux%u scene (format %d), depth %ux%u x%s", pass.eye,
                      pass.eyeWidth, pass.eyeHeight, focus ? " with its focus view beside" : "", drawn, sd.Width, sd.Height,
                      static_cast<int>(sd.Format), depthW, depthH, d.depthMs ? "multisample" : "1");
    }

    const D3D11StateGuard guard(context, full_state());  // the game's state back when this returns
    // A plain pipeline for our draws: the game's other stages, states and scissors out of the way.
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);

    if (copied && d.cfg.depthMeasure)  // the copy as made, against the depth at the pass's end
        measure_depth(context, pass.eye, pass.eyeWidth, pass.eyeHeight, focus ? rw : 0, focus ? rh : 0, depthTerms);
    if (readCopy && d.cfg.depthCopyNear > 0) {  // the copy only where what was drawn over it is near
        Constants k{};
        k.extra[3] = static_cast<float>(depthTerms[0] + depthTerms[1] / d.cfg.depthCopyNear);
        context->UpdateSubresource(d.constants.Get(), 0, nullptr, &k, 0, 0);
        context->OMSetRenderTargets(1, dc.rtv.GetAddressOf(), nullptr);
        context->PSSetShaderResources(0, 1, d.depthSrv.GetAddressOf());
        draw_triangle(context, d.depthMs ? d.keepFarMs.Get() : d.keepFarSs.Get(), copyW, copyH);
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    {  // the eye: rendered with its jitter; without it the clip shift is taken off
        const M4 off = dlss::clip_shift(eyeJitter.on ? -eyeJitter.cx : 0, eyeJitter.on ? -eyeJitter.cy : 0);
        const M4 unjittered[2] = {dlss::mul(vp[0], off), dlss::mul(vp[1], off)};
        eyeDone = run_view(context, resolved, sd.Format, pass.eye, 0, 0, pass.eyeWidth, pass.eyeHeight, pass.eyeWidth,
                           pass.eyeHeight, vp, unjittered, eyeJitter, eyeJitter.cx, eyeJitter.cy, inverted, depthTerms, car,
                           pass, eyeCamera);
    }
    if (focus && !d.off) {  // the focus view: the eye's clip through its remap (which carries its own jitter)
        const M4 remap = dlss::clip_remap_matrix(pass.remap[0], pass.remap[1], pass.remap[2], pass.remap[3]);
        const M4 plain = dlss::clip_remap_matrix(pass.remap[0], pass.remap[1] - (focusJitter.on ? focusJitter.cx : 0),
                                                 pass.remap[2], pass.remap[3] - (focusJitter.on ? focusJitter.cy : 0));
        const M4 rendered[2] = {dlss::mul(vp[0], remap), dlss::mul(vp[1], remap)};
        const M4 unjittered[2] = {dlss::mul(vp[0], plain), dlss::mul(vp[1], plain)};
        // A band of its rows (DlssPass::focusBandTop): DLAA (not super resolution), NGX, a band smaller than the view.
        const unsigned bandH = pass.focusBandHeight;
        const bool band = bandH && bandH < fh && rw == fw && rh == fh && d.backend == 0;
        if (band) {
            const unsigned top = (std::min)(pass.focusBandTop, fh - bandH);
            focusDone = run_view(context, resolved, sd.Format, 4 + pass.eye, pass.eyeWidth, top, fw, bandH, fw, bandH,
                                 rendered, unjittered, focusJitter, focusJitter.cx, focusJitter.cy, inverted, depthTerms,
                                 car, pass, focusCamera, fh);
            // The other rows: the eye's image as DLSS made it (both unjittered: the focus view's clip from the eye's
            // as rendered is the remap with the eye's jitter and without its own).
            const double plainRemap[4] = {
                pass.remap[0], pass.remap[0] * (eyeJitter.on ? eyeJitter.cx : 0) + pass.remap[1] - (focusJitter.on ? focusJitter.cx : 0),
                pass.remap[2], pass.remap[2] * (eyeJitter.on ? eyeJitter.cy : 0) + pass.remap[3] - (focusJitter.on ? focusJitter.cy : 0)};
            if (focusDone && eyeDone &&
                band_outside(context, resolved, sd.Format, pass.eyeWidth, pass.eyeHeight, pass.eyeWidth, fw, fh, top, bandH,
                             plainRemap, pass)) {
                ++d.bands;
                d.bandShare += static_cast<double>(bandH) / fh;
                d.composed += pass.focusKeep ? 1 : 0;
            } else {
                ++d.bandFallbacks;
            }
        } else {
            focusDone = run_view(context, resolved, sd.Format, 2 + pass.eye, pass.eyeWidth, 0, rw, rh, fw, fh, rendered,
                                 unjittered, focusJitter, focusJitter.cx, focusJitter.cy, inverted, depthTerms, car, pass,
                                 focusCamera);
            // What the game didn't draw at full quality: the eye's image as DLSS made it (DLAA, NGX: as the band).
            if (pass.focusKeep && focusDone && eyeDone && rw == fw && rh == fh && d.backend == 0) {
                const double plainRemap[4] = {
                    pass.remap[0], pass.remap[0] * (eyeJitter.on ? eyeJitter.cx : 0) + pass.remap[1] - (focusJitter.on ? focusJitter.cx : 0),
                    pass.remap[2], pass.remap[2] * (eyeJitter.on ? eyeJitter.cy : 0) + pass.remap[3] - (focusJitter.on ? focusJitter.cy : 0)};
                if (band_outside(context, resolved, sd.Format, pass.eyeWidth, pass.eyeHeight, pass.eyeWidth, fw, fh, 0, 0,
                                 plainRemap, pass))
                    ++d.composed;
                else
                    ++d.bandFallbacks;
            }
        }
        if (focusDone)
            focusLeft = false;
    }
    d.jitteredFrames += eyeJitter.on && pass.eye == 0 ? 1 : 0;
}

ID3D11CommandList* dlss_depth_copy_list(ID3D11DepthStencilView* depth, unsigned width, unsigned height) {
    if (!d.ready || d.off || !d.cfg.views || !depth || !width || !height)
        return nullptr;
    unsigned dw = 0, dh = 0;
    if (!depth_view(depth, dw, dh))
        return nullptr;
    State::DepthCopy& c = d.copy;
    width = (std::min)(width, dw);
    height = (std::min)(height, dh);
    if (!c.texture || c.texW != dw || c.texH != dh) {
        c.texture.Reset();
        c.rtv.Reset();
        c.srv.Reset();
        c.list.Reset();
        c.texW = c.texH = 0;
        if (!make(c.texture, dw, dh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) ||
            FAILED(d.device->CreateRenderTargetView(c.texture.Get(), nullptr, &c.rtv)) ||
            FAILED(d.device->CreateShaderResourceView(c.texture.Get(), nullptr, &c.srv)) ||
            (!c.deferred && FAILED(d.device->CreateDeferredContext(0, &c.deferred)))) {
            c.texture.Reset();
            c.rtv.Reset();
            c.srv.Reset();
            if (!c.refusedLogged) {
                c.refusedLogged = true;
                say("the depth's copy before the transparents couldn't be made (%ux%u): DLSS reads the depth target as it "
                    "is at the pass's end",
                    dw, dh);
            }
            return nullptr;
        }
        c.texW = dw;
        c.texH = dh;
        say("the depth's copy before the transparents made at %ux%u (the depth target's %s)", dw, dh,
            d.depthMs ? "first samples" : "texels");
    }
    if (!c.list || c.source != d.depthFor || c.w != width || c.h != height || c.ms != d.depthMs) {
        c.list.Reset();
        ID3D11DeviceContext* dc = c.deferred.Get();
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        dc->OMSetRenderTargets(1, c.rtv.GetAddressOf(), nullptr);
        dc->PSSetShaderResources(0, 1, d.depthSrv.GetAddressOf());
        dc->RSSetViewports(1, &vp);
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->VSSetShader(d.vs.Get(), nullptr, 0);
        dc->PSSetShader(d.depthMs ? d.depthCopyMs.Get() : d.depthCopySs.Get(), nullptr, 0);
        dc->Draw(3, 0);
        if (FAILED(dc->FinishCommandList(FALSE, &c.list))) {
            c.list.Reset();
            return nullptr;
        }
        c.source = d.depthFor;
        c.w = width;
        c.h = height;
        c.ms = d.depthMs;
    }
    return c.list.Get();
}

void dlss_shutdown() {
    nr_shutdown();  // its features and its views of this device first
    if (!d.device)
        return;
    for (Slot& s : d.slots)
        release_slot(s);
    if (d.ready) {
        ngx::shutdown(d.device.Get());  // each only if it started
        ComPtr<ID3D11DeviceContext> context;
        d.device->GetImmediateContext(&context);
        fsr::shutdown(context.Get());
    }
    d.ready = false;
    d.backend = -1;
    d.fsrProviderNamed = false;
    d.ngxRefused = false;
    d.depthFor = nullptr;
    d.depthSrv.Reset();
    d.depthIn = nullptr;
    d.copy = State::DepthCopy{};
    d.vectorsCopyMs.Reset();
    d.depthCopyMs.Reset();
    d.depthCopySs.Reset();
    d.measureMs.Reset();
    d.measureSs.Reset();
    d.keepFarMs.Reset();
    d.keepFarSs.Reset();
    d.vs.Reset();
    d.vectorsMs.Reset();
    d.vectorsSs.Reset();
    d.check.Reset();
    d.stretch.Reset();
    d.bandOutside.Reset();
    d.bandBlend.Reset();
    d.bandScene = nullptr;
    d.bandSceneRtv.Reset();
    d.bandIn.Reset();
    d.bandOut.Reset();
    d.bandSrv.Reset();
    d.bandRtv.Reset();
    std::memset(d.bandSize, 0, sizeof(d.bandSize));
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
        d.focusOk[0] = d.focusOk[1] = false;
        d.eye[0] = d.eye[1] = d.focus[0] = d.focus[1] = Jitter{};
    }
    d.firstLogged = false;
    say("shut down (features released, NGX and FSR stopped)");
}

void dlss_set_matrices(PassMatricesFn fn) { d.matrices = fn; }

bool dlss_vectors_for_checks(ID3D11DeviceContext* context, int index, std::vector<float>& xy, unsigned& width,
                             unsigned& height) {
    if (!context || index < 0 || index >= static_cast<int>(std::size(d.slots)) || !d.slots[index].vectors)
        return false;
    ID3D11Texture2D* vectors = d.slots[index].vectors.Get();
    D3D11_TEXTURE2D_DESC t;
    vectors->GetDesc(&t);
    t.Usage = D3D11_USAGE_STAGING;
    t.BindFlags = 0;
    t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    t.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&t, nullptr, &staging)))
        return false;
    context->CopyResource(staging.Get(), vectors);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;
    const auto half = [](uint16_t v) {  // R16G16_FLOAT
        const uint32_t sign = v >> 15, exponent = (v >> 10) & 0x1f, mantissa = v & 0x3ff;
        const float f = exponent == 0    ? std::ldexp(static_cast<float>(mantissa), -24)
                        : exponent == 31 ? 0.0f
                                         : std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
        return sign ? -f : f;
    };
    width = t.Width;
    height = t.Height;
    xy.resize(static_cast<size_t>(width) * height * 2);
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
        for (unsigned x = 0; x < width * 2; ++x)
            xy[static_cast<size_t>(y) * width * 2 + x] = half(row[x]);
    }
    context->Unmap(staging.Get(), 0);
    return true;
}

void dlss_live(const DlssLive& live) {
    std::lock_guard guard(d.mutex);
    d.live = live;
    d.livePending = true;
    d.liveAt = std::chrono::steady_clock::now();
    ++d.liveChanges;
}

namespace {

// A change that makes features again (each 40-70 ms on the render thread, two per change) waits until the settings
// have held this long: stepping through the quality modes in the headset menu makes one set, not one per step (run 26a:
// two changes a second apart, ~120 ms each).
auto kLiveSettle = std::chrono::milliseconds(500);

// Does `l` make features again (or release them)?
bool live_remakes(const DlssLive& l) {
    const int views = l.on ? 2 : 0;
    const int scale = (std::max)(33, (std::min)(100, l.focusScale));
    return views != d.cfg.views || scale != d.cfg.focusScale || l.preset != d.cfg.preset || l.eyePreset != d.cfg.eyePreset ||
           l.upscaler != d.cfg.upscaler;
}

// Texture sharpening: no feature to make again, from the next pass.
void apply_live_sharpening(const DlssLive& l) {
    if (l.textureSharpening == d.cfg.textureSharpening)
        return;
    d.cfg.textureSharpening = l.textureSharpening;
    say("live: texture sharpening %s", l.textureSharpening >= 2   ? "on the focus views and the eyes"
                                       : l.textureSharpening == 1 ? "on the focus views"
                                                                  : "off");
}

// The player's change, between frames on the render thread (dlss_frame_end). `changes`: the dlss_live calls it gathers.
void apply_live(const DlssLive& l, unsigned changes) {
    const int views = l.on ? 2 : 0;
    const int scale = (std::max)(33, (std::min)(100, l.focusScale));
    const bool was = d.cfg.views > 0 && !d.off;
    apply_live_sharpening(l);
    if (!live_remakes(l))
        return;
    const bool otherUpscaler = l.upscaler != d.cfg.upscaler;
    if ((!views && d.cfg.views) || otherUpscaler) {  // off (NGX and FSR stay started until the session ends), or
        for (Slot& s : d.slots)                       // the other upscaler's to make
            release_slot(s);
    }
    {
        std::lock_guard guard(d.mutex);  // the jitter reads these on xrLocateViews' thread
        if ((views && !was) || otherUpscaler)
            d.off = false;  // on again: a session's earlier refusal (NGX, the features) is tried once more
        d.cfg.upscaler = l.upscaler;
        d.cfg.views = views;
        d.cfg.focusScale = scale;
        d.cfg.preset = l.preset;
        d.cfg.eyePreset = l.eyePreset;
        d.phases = views >= 2 ? dlss::jitter_phases(scale / 100.0) : dlss::kJitterPhases;
        d.focusSrOff = false;  // a new mode is tried afresh
        if (!views) {
            d.lastFrameOk = false;
            d.focusOk[0] = d.focusOk[1] = false;
            d.eye[0] = d.eye[1] = d.focus[0] = d.focus[1] = Jitter{};
        }
        d.evaluated[0] = d.evaluated[1] = d.focusEvaluated[0] = d.focusEvaluated[1] = false;
    }
    d.createFailures = 0;
    if (otherUpscaler)
        say("live: the upscaler is now %s%s", backend_name(wanted_backend()),
            wanted_backend() != l.upscaler ? " (the GPU is AMD's)" : "");
    if (!views)
        say("live: off (the features released; %u change%s)", changes, changes == 1 ? "" : "s");
    else
        say("live: %s on the eyes (preset %s), the focus views %s (%d%%, preset %s), %u jitter phases (%u change%s "
            "gathered, %lld ms settled)",
            was ? "DLAA" : "on: DLAA", dlss::preset_name(l.eyePreset >= 0 ? l.eyePreset : l.preset),
            dlss::focus_mode_name(scale), scale, dlss::preset_name(l.preset), d.phases, changes,
            changes == 1 ? "" : "s", static_cast<long long>(kLiveSettle.count()));
}

}  // namespace

void dlss_live_settle_for_tests(int ms) { kLiveSettle = std::chrono::milliseconds(ms < 0 ? 0 : ms); }
unsigned long long dlss_evaluations_total() { return d.evaluationsTotal; }
int dlss_upscaler_in_use() { return d.backend; }

const wchar_t* dlss_upscaler_badge() {
    switch (badge.load()) {
    case 0: return L"DLSS";
    case 400: case 401: case 402: case 403: case 404: case 405: return L"FSR4";
    case 301: return L"FSR3.1";
    case 300: return L"FSR3";
    default: return badge.load() >= 400 ? L"FSR4" : badge.load() >= 300 ? L"FSR3" : L"FSR";
    }
}
unsigned long long dlss_stretched_total() { return d.stretchedTotal; }

void dlss_frame_end(unsigned long long frame) {
    DlssLive live;
    bool pending, settled = true;
    unsigned changes = 0;
    {
        std::lock_guard guard(d.mutex);
        pending = d.livePending;
        live = d.live;
        if (pending && live_remakes(live) && std::chrono::steady_clock::now() - d.liveAt < kLiveSettle)
            settled = false;  // kept pending: the features once the player stops changing them
        if (pending && settled) {
            d.livePending = false;
            changes = d.liveChanges;
            d.liveChanges = 0;
        }
    }
    if (pending && settled)
        apply_live(live, changes);
    else if (pending)
        apply_live_sharpening(live);
    nr_frame_end(frame);
    if (d.ready && (d.copy.taken[0].pending || d.copy.taken[1].pending)) {
        ComPtr<ID3D11DeviceContext> context;
        d.device->GetImmediateContext(&context);
        read_depth_measure(context.Get());
    }
    {  // FSR's frame time (its own clock between frame ends), and its released contexts collected
        const auto now = std::chrono::steady_clock::now();
        if (d.lastFrameEnd.time_since_epoch().count())
            d.frameMs = std::clamp(std::chrono::duration<double, std::milli>(now - d.lastFrameEnd).count(), 1.0, 100.0);
        d.lastFrameEnd = now;
        fsr::collect();
    }
    if (!d.cfg.views)
        return;
    {
        // Jitter next frame only where DLSS processed the view this frame: the eyes (one FOV shift for both) if it
        // processed both, each focus view if it processed that one.
        std::lock_guard guard(d.mutex);
        d.lastFrameOk = d.evaluated[0] && d.evaluated[1] && !d.off;
        for (int e = 0; e < 2; ++e)
            d.focusOk[e] = d.focusEvaluated[e] && !d.off;
        // Logged once a stretch of frames in which a view was tried and failed (not a frame without passes: menus,
        // loading), a few stretches a session.
        const bool failedNow = d.failures != d.failuresAtFrameEnd;
        const bool settlingNow = d.resizeWaits != d.resizeWaitsAtFrameEnd;  // a new size waiting (kResizeSettleMs)
        if (failedNow && !d.jitterHeld && d.cfg.jitter && !d.off && d.jitterHeldLogged < 3) {
            ++d.jitterHeldLogged;
            say("frame %llu: a view wasn't processed (eyes %d %d, focus views %d %d): no jitter on it until it is", d.frame,
                d.evaluated[0] ? 1 : 0, d.evaluated[1] ? 1 : 0, d.focusEvaluated[0] ? 1 : 0, d.focusEvaluated[1] ? 1 : 0);
        }
        d.jitterHeld = failedNow || settlingNow || (d.jitterHeld && !(d.lastFrameOk && d.focusOk[0] && d.focusOk[1]));
        d.evaluated[0] = d.evaluated[1] = d.focusEvaluated[0] = d.focusEvaluated[1] = false;
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
            say("%llu evaluations, %llu failed, %llu features made again (%llu views left as drawn while a new size "
                "settled), %llu frames with jitter, %llu focus views "
                "stretched, %llu views with moving objects' motion since the last line; views %ux%u %ux%u, focus %s %s; "
                "jitter sign %s",
                d.evaluations, d.failures, d.recreated, d.resizeWaits, d.jitteredFrames, d.stretched, d.objectViews, d.slots[0].width,
                d.slots[0].height,
                d.slots[1].width, d.slots[1].height, focusSizes[0], focusSizes[1],
                d.jitterDead ? "none (it doesn't reach the projection)" : !d.signDecided ? "not settled yet"
                : d.sign[0] > 0 && d.sign[1] > 0 ? "as asked" : "negated on an axis");
            if (d.bands || d.bandFallbacks || d.composed)
                say("focus views over a band of their rows: %llu (%.0f%% of their rows each); %llu whose rest took the eye's "
                    "image after DLSS (%llu drawn into the scene itself), %llu that kept the pass's own (the band or the eye "
                    "not evaluated); band features %ux%u %ux%u since the last line",
                    d.bands, d.bands ? 100.0 * d.bandShare / d.bands : 0.0, d.composed, d.bandDirect, d.bandFallbacks,
                    d.slots[4].width, d.slots[4].height, d.slots[5].width, d.slots[5].height);
            d.bands = d.bandFallbacks = d.composed = d.bandDirect = 0;
            d.bandShare = 0;
            if (d.copy.used || d.copy.measured)
                say("depth before the transparents: %llu passes read it, %llu only measured it (the A/B's other side) "
                    "since the last line",
                    d.copy.used, d.copy.measured);
            d.copy.used = d.copy.measured = 0;
            if (d.backend == 1) {  // FSR's own GPU time per view (its D3D12 queue's timestamps)
                double ms[4];
                for (int i = 0; i < 4; ++i)
                    ms[i] = fsr::take_gpu_ms(d.slots[i].fsrView);
                say("FSR GPU per evaluation: eyes %.2f %.2f ms, focus views %.2f %.2f ms (%s); frame %.1f ms", ms[0], ms[1], ms[2],
                    ms[3], fsr::provider_in_use().c_str(), d.frameMs);
            }
        }
        d.evaluations = d.failures = d.recreated = d.jitteredFrames = d.stretched = d.objectViews = d.resizeWaits = 0;
    }
    d.failuresAtFrameEnd = d.failures;
    d.resizeWaitsAtFrameEnd = d.resizeWaits;
}

}  // namespace simqvfr

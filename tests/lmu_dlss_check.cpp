// Hardware check of the DLAA module (engine/dlss_views.h) as LMU uses it outside the game: its whole path on a real D3D11 device
// with a stand-in for the game's side. Not part of the test suite (no _test suffix): it needs an NVIDIA RTX GPU, a build
// with the NGX SDK and a folder holding nvngx_dlss.dll.
//
//   lmu_dlss_check.exe <folder with nvngx_dlss.dll> [frames]
//
// Each frame, as the game and the layer's hooks would: the stereo views located (their FOV jittered by the module), the
// projection built from that FOV as LMU builds it, a camera that moves, an eye and its focus view side by side in a
// resolved HDR scene with a multisample depth target, the module called after the "resolve" for both eyes. Checked: the
// evaluations succeed, both views' parts of the scene are rewritten and nothing else, the context's state is as the
// caller left it, the jitter is found in the projection. The module's log lines are printed at the end.
//
// Then a second part with a scene that moves with the camera (a textured floor and ceiling drawn through each view's
// projection as rendered, the camera driving at racing speed): every point stands still in the world, so the vectors
// must explain the frame's change and the gate must say so, at a small eye size (fine detail per pixel) too.
//
// Last, the focus views in DLSS performance mode (drawn at 50% each way, upscaled by DLSS into their region): the
// upscaled view must sit where a full-size render of it without jitter has the world (no shift, no scale), the gate must
// fit at the render size, and a frame in which DLSS gives up must still fill the region (the render stretched).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include "engine/dlss_math.h"
#include "engine/dlss_views.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;

// One row of an RGBA16F texture, as floats (red only).
std::vector<float> row_of(ID3D11Texture2D* texture, unsigned x, unsigned y, unsigned w) {
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    d.Width = w, d.Height = 1, d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> row;
    std::vector<float> out;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &row)))
        return out;
    const D3D11_BOX box{x, y, 0, x + w, y + 1, 1};
    context->CopySubresourceRegion(row.Get(), 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(row.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return out;
    const auto* v = static_cast<const DirectX::PackedVector::HALF*>(m.pData);
    for (unsigned i = 0; i < w; ++i)
        out.push_back(DirectX::PackedVector::XMConvertHalfToFloat(v[i * 4]));
    context->Unmap(row.Get(), 0);
    return out;
}

double difference(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        s += std::fabs(a[i] - b[i]);
    return a.empty() ? 0 : s / a.size();
}

// The second part's scene: for each pixel of a view, the ray through it (the view's clip as rendered -> the world)
// meets a floor at y = 0 or a ceiling at y = 4; its colour is a texture of the hit's place (two octaves, the finer about
// a pixel wide on the floor a few metres off at the small eye size), its depth the hit's through the same matrix. The
// game's convention: depth 1 at the near plane.
constexpr char kSceneShader[] = R"(
cbuffer C : register(b0) {
    row_major float4x4 clipToWorld;
    row_major float4x4 worldToClip;
    row_major float4x4 worldToCamera;  // the camera's view matrix (x right, y up, z forward)
    float4 area;                       // the view's viewport: x, y, width, height
    float4 frame;                      // x: 1 with the window frame fixed to the camera
};
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float hash2(float2 c) { return frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453); }
float value_noise(float2 p) {
    float2 i = floor(p), f = frac(p);
    float2 s = f * f * (3 - 2 * f);
    return lerp(lerp(hash2(i), hash2(i + float2(1, 0)), s.x), lerp(hash2(i + float2(0, 1)), hash2(i + float2(1, 1)), s.x), s.y);
}
struct Out {
    float4 colour : SV_Target0;
    float depth : SV_Depth;
};
Out ps(float4 pos : SV_Position) {
    float2 ndc = float2((pos.x - area.x) / area.z * 2 - 1, 1 - (pos.y - area.y) / area.w * 2);
    float4 a = mul(float4(ndc, 1, 1), clipToWorld);
    float4 b = mul(float4(ndc, 0.01, 1), clipToWorld);
    float3 p0 = a.xyz / a.w;
    float3 dir = normalize(b.xyz / b.w - p0);
    float level = dir.y < 0 ? 0 : 4;
    float t = (level - p0.y) / (abs(dir.y) > 1e-6 ? dir.y : 1e-6);
    t = t > 0 ? t : 1e4;
    // Holes in the ceiling show the sky: depth 0, the far plane (as a cleared reversed depth target has it).
    float3 roof = p0 + dir * t;
    bool sky = dir.y > 0 && frac(roof.x / 12) < 0.6 && frac(roof.z / 12) < 0.6;
    if (sky)
        t = 1e9;
    // Posts 0.4 m wide every 2 m across (the camera drives between two), in rows every 10 m along z: depth that changes
    // as the camera comes nearer (a floor alone keeps each pixel's depth while the camera drives along it).
    if (dir.z > 1e-4) {
        for (int k = 1; k <= 12; ++k) {
            float tk = ((floor(p0.z / 10) + k) * 10 - p0.z) / dir.z;
            if (tk >= t)
                break;
            float3 q = p0 + dir * tk;
            if (q.y > 0 && q.y < 4 && frac((q.x - 101) / 2) < 0.2) {
                t = tk;
                sky = false;
                break;
            }
        }
    }
    // A car's window frame, fixed to the camera 0.4 m ahead (dash, roof and pillars round a window; at the view's edges
    // it reaches 0.56 m aside and 0.44 m up and down, inside the check's car box): what it hides of the world in one
    // frame comes out from behind it in the next.
    float2 tex = 0;
    bool car = false;
    if (frame.x > 0) {
        float3 c0 = mul(float4(p0, 1), worldToCamera).xyz, cd = mul(float4(dir, 0), worldToCamera).xyz;
        float tc = cd.z > 1e-4 ? (0.4 - c0.z) / cd.z : -1;
        float3 h = c0 + cd * tc;
        if (tc > 0 && tc < t && (abs(h.x) > 0.3 || h.y < -0.12 || h.y > 0.17)) {
            t = tc;
            car = true;
            sky = false;
            tex = h.xy * 3;
        }
    }
    float3 hit = p0 + dir * min(t, 1e4);
    float4 c = mul(float4(hit, 1), worldToClip);
    Out o;
    o.depth = sky ? 0 : saturate(c.z / c.w);
    // The floor's x, z; the posts' across and up; the sky's direction (it stands still as the camera drives).
    float2 at = car ? tex : sky ? dir.xz / max(dir.y, 0.05) : float2(hit.x + hit.y, hit.z - hit.y);
    float v = 0.6 * value_noise(at * 20) + 0.4 * value_noise(at * 80 + 17);
    o.colour = float4(v, 0.8 * v + 0.1, 1 - 0.5 * v, 1);
    return o;
}
)";

struct SceneConstants {
    float clipToWorld[16], worldToClip[16], worldToCamera[16], area[4], frame[4];
};

struct Scene {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11DepthStencilState> always;
};

bool make_scene(Scene& s) {
    ComPtr<ID3DBlob> code, errors;
    for (int stage = 0; stage < 2; ++stage) {
        if (FAILED(D3DCompile(kSceneShader, sizeof(kSceneShader) - 1, "check_scene", nullptr, nullptr,
                              stage ? "ps" : "vs", stage ? "ps_5_0" : "vs_5_0", 0, 0, &code, &errors))) {
            std::printf("     the scene's shader: %s\n",
                        errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return false;
        }
        const void* bytes = code->GetBufferPointer();
        const SIZE_T size = code->GetBufferSize();
        const HRESULT made = stage ? device->CreatePixelShader(bytes, size, nullptr, &s.ps)
                                   : device->CreateVertexShader(bytes, size, nullptr, &s.vs);
        if (FAILED(made))
            return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(SceneConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &s.constants)) &&
           SUCCEEDED(device->CreateDepthStencilState(&ds, &s.always));
}

// One view of the scene through `rendered` (the world -> its clip as rendered; `view` the camera's view matrix, for
// the window frame with `frame`): its colour into `colour`, its depth into the multisample depth target (with the
// multisample colour target, which takes the colour too and is not read). No `msColour`: the colour alone, with `blend`
// (and its factor) if given.
bool draw_scene_view(const Scene& s, const simqvfr::dlss::M4& rendered, const float (&view)[16], bool frame, float x, float y,
                     float w, float h, ID3D11RenderTargetView* colour, ID3D11RenderTargetView* msColour,
                     ID3D11DepthStencilView* dsv, ID3D11BlendState* blend = nullptr, const float* blendFactor = nullptr) {
    simqvfr::dlss::M4 inverse;
    if (!simqvfr::dlss::invert(rendered, inverse))
        return false;
    SceneConstants c{};
    for (int i = 0; i < 16; ++i) {
        c.clipToWorld[i] = static_cast<float>(inverse[i]);
        c.worldToClip[i] = static_cast<float>(rendered[i]);
        c.worldToCamera[i] = view[i];
    }
    c.area[0] = x, c.area[1] = y, c.area[2] = w, c.area[3] = h;
    c.frame[0] = frame ? 1.0f : 0.0f;
    context->UpdateSubresource(s.constants.Get(), 0, nullptr, &c, 0, 0);
    ID3D11ShaderResourceView* none[1] = {};
    context->PSSetShaderResources(0, 1, none);
    const D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
    context->RSSetViewports(1, &vp);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s.vs.Get(), nullptr, 0);
    context->PSSetShader(s.ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, s.constants.GetAddressOf());
    context->OMSetDepthStencilState(s.always.Get(), 0);
    if (blend)
        context->OMSetBlendState(blend, blendFactor, 0xffffffff);
    context->OMSetRenderTargets(1, &colour, nullptr);
    context->Draw(3, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    if (msColour) {
        context->OMSetRenderTargets(1, &msColour, dsv);
        context->Draw(3, 0);
    }
    context->OMSetDepthStencilState(nullptr, 0);
    return true;
}

// The stereo views' FOV, jittered by the module, and LMU's projection from it (the ordinary depth convention, as the
// eye's slot holds it).
void eye_projection(const XrFovf& fov, float (&projection)[16]) {
    const double l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), u = std::tan(fov.angleUp),
                 dn = std::tan(fov.angleDown), n = 0.05, fz = 20000;
    for (float& m : projection)
        m = 0;
    projection[0] = static_cast<float>(2 / (r - l));
    projection[5] = static_cast<float>(2 / (u - dn));
    projection[8] = static_cast<float>(-(r + l) / (r - l));
    projection[9] = static_cast<float>(-(u + dn) / (u - dn));
    projection[10] = static_cast<float>(fz / (fz - n));
    projection[11] = 1;
    projection[14] = static_cast<float>(-n * fz / (fz - n));
}

// The game's side's view and projection for the pass about to be called (each frame below sets them), handed to the
// module as a game's matrices callback would (simqvfr::dlss_set_matrices).
float view[16], projection[16];
bool matricesFail = false;  // the game's side has none (the module then gives up: the stretch's check)
bool check_matrices(int, float (&v)[16], float (&p)[16]) {
    std::memcpy(v, view, sizeof(v));
    std::memcpy(p, projection, sizeof(p));
    return !matricesFail;
}

// Mean |a[x] - b[x + dx]| over the row, a few pixels in from its ends.
double shifted_difference(const std::vector<float>& a, const std::vector<float>& b, int dx) {
    double s = 0;
    int n = 0;
    for (int x = 4; x + 4 < static_cast<int>(a.size()) && x + 4 < static_cast<int>(b.size()); ++x) {
        s += std::fabs(a[x] - b[x + dx]);
        ++n;
    }
    return n ? s / n : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: lmu_dlss_check <folder with nvngx_dlss.dll> [frames]\n");
        return 2;
    }
    const int frames = argc > 2 ? (std::max)(120, _wtoi(argv[2])) : 400;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring log = std::wstring(temp) + L"sim-qvfr-lmu-dlss-check.log";
    DeleteFileW(log.c_str());
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", log.c_str());

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::printf("no D3D11 hardware device\n");
        return 2;
    }
    const unsigned eyeW = 1600, eyeH = 1280, focusW = 1280, focusH = 1300, W = eyeW + focusW, H = focusH;

    // The game's side: the resolved scene (noise, rewritten each frame as a resolve would), a multisample depth target.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W, td.Height = H, td.MipLevels = td.ArraySize = 1, td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    std::vector<DirectX::PackedVector::HALF> noise(size_t(W) * H * 4);
    uint32_t seed = 99;
    for (size_t i = 0; i < noise.size(); i += 4) {
        seed = seed * 1664525u + 1013904223u;
        const float v = 0.2f + (seed >> 8) / 16777216.0f;
        noise[i] = noise[i + 1] = noise[i + 2] = DirectX::PackedVector::XMConvertFloatToHalf(v);
        noise[i + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
    }
    ComPtr<ID3D11Texture2D> resolved, other, depth;
    device->CreateTexture2D(&td, nullptr, &resolved);
    device->CreateTexture2D(&td, nullptr, &other);
    ComPtr<ID3D11ShaderResourceView> resolvedSrv;
    device->CreateShaderResourceView(resolved.Get(), nullptr, &resolvedSrv);
    td.SampleDesc.Count = 8;  // the scene's multisample colour target, as the game has bound at its resolve
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> sceneMs;
    device->CreateTexture2D(&td, nullptr, &sceneMs);
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R32_TYPELESS, td.SampleDesc.Count = 8;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&td, nullptr, &depth);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT, dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11RenderTargetView> otherRtv;
    check(resolved && depth && SUCCEEDED(device->CreateDepthStencilView(depth.Get(), &dd, &dsv)) &&
              SUCCEEDED(device->CreateRenderTargetView(sceneMs.Get(), nullptr, &otherRtv)) && resolvedSrv,
          "the stand-in scene and depth targets are made");
    if (!dsv)
        return 1;
    // Reversed depth, near plane 0.05 m: everything 0.8 m away, so the middle of each view is inside the car's box
    // (0.6 m up from the head) and its top rows are outside it: both of the gate's classes get points.
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0625f, 0);

    simqvfr::DlssRun run;
    run.tag = "lmu dlss";  // LMU's, as the log lines below are read
    run.views = 2;
    run.dllFolder = argv[1];
    run.gate = true;
    run.eyePreset = 6;  // the first part: the eyes on preset F (lmu_dlss_eye_preset=F), the focus views on NGX's default
    simqvfr::dlss_start(run);
    check(simqvfr::dlss_wanted(), "the module takes the run (a build with the NGX SDK)");

    simqvfr::dlss_set_matrices(&check_matrices);
    bool stateKept = true, rewritten = true, restLeft = true;
    int stateLost = 0;
    for (int f = 0; f < frames; ++f) {
        XrView views[2]{};
        for (XrView& v : views) {
            v.fov = {-0.9512f, 0.8814f, 0.8290f, -0.8290f};
            v.pose.orientation = {0, std::sin(0.001f * f), 0, std::cos(0.001f * f)};  // the head turning a little
            v.pose.position = {0.03f, 1.2f, 0.0f};
        }
        simqvfr::dlss_jitter_views(views, 2);
        // The camera drives along z and turns a little: a world matrix inverted into the view.
        const double yaw = 0.002 * f, c = std::cos(yaw), s = std::sin(yaw);
        const simqvfr::dlss::M4 world = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 100.0, 1.2, 0.5 * f, 1};
        simqvfr::dlss::M4 v;
        simqvfr::dlss::invert(world, v);
        for (int i = 0; i < 16; ++i)
            view[i] = static_cast<float>(v[i]);
        for (int eye = 0; eye < 2; ++eye) {
            eye_projection(views[eye].fov, projection);

            simqvfr::DlssPass pass;
            pass.eye = eye;
            pass.eyeWidth = eyeW, pass.eyeHeight = eyeH;
            pass.focus = true;
            pass.focusWidth = focusW, pass.focusHeight = focusH;
            pass.remap[0] = 4, pass.remap[1] = -0.3f, pass.remap[2] = 1.6f, pass.remap[3] = -0.08f;
            simqvfr::dlss_focus_jitter(eye, focusW, focusH, pass.remap[1], pass.remap[3]);

            context->UpdateSubresource(resolved.Get(), 0, nullptr, noise.data(), W * 8, 0);
            // The caller's state: a target and a viewport the module must leave bound.
            const D3D11_VIEWPORT vp{3, 5, 123, 45, 0, 1};
            // The caller's state as the game's at its resolve: the scene's multisample colour and depth targets bound,
            // the resolved scene still a pixel shader input from the frame before's post-processing.
            context->OMSetRenderTargets(1, otherRtv.GetAddressOf(), dsv.Get());
            context->PSSetShaderResources(0, 1, resolvedSrv.GetAddressOf());
            context->RSSetViewports(1, &vp);
            const bool last = f == frames - 1 && eye == 1;
            std::vector<float> before[3];
            if (last) {
                before[0] = row_of(resolved.Get(), 0, eyeH / 2, eyeW);
                before[1] = row_of(resolved.Get(), eyeW, focusH / 2, focusW);
                before[2] = row_of(resolved.Get(), 0, eyeH + 10, eyeW);  // under the eye: no view's
            }
            simqvfr::dlss_after_resolve(context.Get(), resolved.Get(), dsv.Get(), pass);
            ComPtr<ID3D11RenderTargetView> rtvNow;
            ComPtr<ID3D11DepthStencilView> dsvNow;
            context->OMGetRenderTargets(1, &rtvNow, &dsvNow);
            UINT count = 1;
            D3D11_VIEWPORT vpNow{};
            context->RSGetViewports(&count, &vpNow);
            ComPtr<ID3D11ShaderResourceView> srvNow;
            context->PSGetShaderResources(0, 1, &srvNow);
            const bool kept = rtvNow.Get() == otherRtv.Get() && dsvNow.Get() == dsv.Get() && srvNow.Get() == resolvedSrv.Get() && count == 1 &&
                              vpNow.Width == 123 && vpNow.TopLeftX == 3;
            if (!kept && stateKept)
                std::printf("     frame %d eye %d: render target %s, depth target %s, %u viewports, the first %gx%g at %g\n", f, eye,
                            rtvNow.Get() == otherRtv.Get() ? "kept" : rtvNow ? "ANOTHER" : "NONE",
                            dsvNow.Get() == dsv.Get() ? (srvNow.Get() == resolvedSrv.Get() ? "kept, input kept" : "kept, INPUT LOST") : "LOST", count, vpNow.Width,
                            vpNow.Height, vpNow.TopLeftX);
            stateKept = stateKept && kept;
            stateLost += kept ? 0 : 1;
            if (last) {
                const double eyeDiff = difference(before[0], row_of(resolved.Get(), 0, eyeH / 2, eyeW));
                const double focusDiff = difference(before[1], row_of(resolved.Get(), eyeW, focusH / 2, focusW));
                const double restDiff = difference(before[2], row_of(resolved.Get(), 0, eyeH + 10, eyeW));
                std::printf("     the last pass changed the eye's row by %.4f, the focus view's by %.4f, the row under the eye "
                            "by %.6f (mean per texel)\n",
                            eyeDiff, focusDiff, restDiff);
                rewritten = eyeDiff > 0.01 && focusDiff > 0.01;
                restLeft = restDiff == 0;
            }
        }
        simqvfr::dlss_frame_end(static_cast<unsigned long long>(f + 1));
    }
    std::printf("     %llu evaluations in %d frames\n", simqvfr::dlss_evaluations_total(), frames);
    check(simqvfr::dlss_wanted(), "the module is still on at the end");
    check(simqvfr::dlss_evaluations_total() == static_cast<unsigned long long>(frames) * 4, "every view of every frame was evaluated");
    std::printf("     the caller's state differed after %d of %d calls\n", stateLost, frames * 2);
    check(stateKept, "the caller's render target, depth target, shader input and viewport are bound as before after every call");
    check(rewritten, "the eye's and the focus view's parts of the scene are rewritten");
    check(restLeft, "the scene outside the views is untouched");
    simqvfr::dlss_shutdown();

    // The second part: the scene that moves with the camera. A small eye (as the game's periphery at 10-20% of native)
    // and a large focus view; the car's box off, so every point takes the world's vectors (the gate's classes then split
    // at 8 m, both through the world). Then the same with the view matrix taken the wrong way round
    // (lmu_dlss_view_inverse): the control, whose vectors the gate must find wrong. Then a car's window frame fixed to
    // the camera, with the car's box about the head (the head still in the tracking space: the frame stands still on
    // screen): the world comes out from behind its edges every frame, as the game's periphery sees it.
    // Then the plain scene again with MSAA off: a single-sample depth target (as LMU makes it then: R32 typeless with a
    // D32 view) and the scene's own colour target, the module called at YEBIS's copy instead of a resolve.
    // Last, the plain scene with the focus views in DLSS performance mode (lmu_dlss_focus_scale=50): drawn at their
    // render size (dlss_focus_scale, asked each frame as the game's side does) in their region's corner.
    const unsigned sceneEyeW = 520, sceneEyeH = 420, sceneFrames = 330;
    Scene scene;
    check(make_scene(scene), "the moving scene's shaders are made");
    ComPtr<ID3D11RenderTargetView> resolvedRtv;
    device->CreateRenderTargetView(resolved.Get(), nullptr, &resolvedRtv);
    // The super resolution part's reference: the focus view drawn at its size without jitter.
    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = focusW, rd.Height = focusH, rd.MipLevels = rd.ArraySize = 1, rd.SampleDesc.Count = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, rd.Usage = D3D11_USAGE_DEFAULT, rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> reference;
    ComPtr<ID3D11RenderTargetView> referenceRtv;
    device->CreateTexture2D(&rd, nullptr, &reference);
    if (reference)
        device->CreateRenderTargetView(reference.Get(), nullptr, &referenceRtv);
    // It is antialiased (16 renders a pixel apart in sub-pixel steps, averaged): what a converged history tends to,
    // where one render's own aliasing would hide how near DLSS gets.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> accumulate;
    device->CreateBlendState(&bd, &accumulate);
    // The upscaled focus view against the reference, shifted by dx, dy pixels (summed over rows across it), and what the
    // stretch left (a frame in which DLSS gave up).
    double placement[5][5] = {}, bilinearDiff = 0;
    bool placed = false, srScaled = true, stretchFilled = false, stretchOff = false;
    unsigned long long stretchedBefore = 0, stretchedAfter = 0;
    double stretchDiff = -1;
    D3D11_TEXTURE2D_DESC sd1{};
    sd1.Width = W, sd1.Height = H, sd1.MipLevels = sd1.ArraySize = 1, sd1.SampleDesc.Count = 1;
    sd1.Format = DXGI_FORMAT_R32_TYPELESS, sd1.Usage = D3D11_USAGE_DEFAULT;
    sd1.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth1;
    ComPtr<ID3D11DepthStencilView> dsv1;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd1{};
    dd1.Format = DXGI_FORMAT_D32_FLOAT, dd1.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    check(SUCCEEDED(device->CreateTexture2D(&sd1, nullptr, &depth1)) &&
              SUCCEEDED(device->CreateDepthStencilView(depth1.Get(), &dd1, &dsv1)),
          "the single-sample depth target (MSAA off) is made");
    // The live part (the player's settings changed while the game runs, dlss_live): DLAA on everything, then off at
    // frame 30, on again at frame 60 with the focus views at quality (67%) and the eyes on F, at frame 90 the focus views
    // at DLAA on preset M, at frame 105 at 77% ("ultra quality"); each change taken at that frame's end.
    unsigned long long liveAt[4] = {};
    double liveScaleOff = -1, liveScaleOn = -1;
    bool liveWantedOff = true;
    for (int part = 0; part < 7; ++part) {
        const bool control = part == 1, frame = part == 3, single = part == 4, sr = part == 5, liveTest = part == 6;
        ID3D11DepthStencilView* const partDsv = single ? dsv1.Get() : dsv.Get();
        ID3D11RenderTargetView* const partColour = single ? resolvedRtv.Get() : otherRtv.Get();
        simqvfr::DlssRun moving = run;
        moving.eyePreset = -1;                // as the focus views' (NGX's default)
        moving.car = part == 2 || part == 3;  // part 2: the box alone (nothing in it), part 3: with the frame in it
        const double box[6] = {0.8, 0.8, 0.6, 0.6, 1.0, 1.0};  // the frame inside, the floor and the posts outside
        for (int i = 0; i < 6; ++i)
            moving.carBox[i] = box[i];
        moving.viewInverse = control;
        moving.focusScale = sr ? 50 : 100;
        simqvfr::dlss_start(moving);
        // The super resolution part drives as the others (its gate at the 60th and 300th frames), then stands still for
        // 90 frames (its history converges over the 32 jitter phases) before the reference frame and the give-up frame.
        const unsigned partFrames = sr ? 400 : liveTest ? 120 : sceneFrames, stopAt = 310;
        for (unsigned f = 0; f < partFrames && scene.ps && resolvedRtv; ++f) {
            if (liveTest) {
                simqvfr::DlssLive change;
                if (f == 30) {
                    change.on = false;
                    simqvfr::dlss_live(change);
                } else if (f == 60) {
                    change.on = true, change.focusScale = 67, change.preset = 0, change.eyePreset = 6;
                    simqvfr::dlss_live(change);
                } else if (f == 90) {
                    change.on = true, change.focusScale = 100, change.preset = 13, change.eyePreset = 11;
                    simqvfr::dlss_live(change);
                } else if (f == 105) {  // "ultra quality": NGX has no such mode for an upscale; its quality mode takes 77%
                    change.on = true, change.focusScale = 77, change.preset = 0, change.eyePreset = 11;
                    simqvfr::dlss_live(change);
                }
                if (f == 31 || f == 61 || f == 90 || f == 119)  // the evaluations so far at each stretch's ends
                    liveAt[f == 31 ? 0 : f == 61 ? 1 : f == 90 ? 2 : 3] = simqvfr::dlss_evaluations_total();
                if (f == 45) {
                    liveScaleOff = simqvfr::dlss_focus_scale();
                    liveWantedOff = simqvfr::dlss_wanted();
                }
                if (f == 75)
                    liveScaleOn = simqvfr::dlss_focus_scale();
            }
            const bool referenceFrame = sr && f == partFrames - 2, giveUpFrame = sr && f == partFrames - 1;
            matricesFail = giveUpFrame;
            if (giveUpFrame)
                stretchedBefore = simqvfr::dlss_stretched_total();
            // The focus view's render size this frame, as the game's side sizes it (its size until NGX has started).
            const double scale = simqvfr::dlss_focus_scale();
            const unsigned rw = simqvfr::dlss::render_size(focusW, scale), rh = simqvfr::dlss::render_size(focusH, scale);
            if (sr && f >= 1 && (rw != focusW / 2 || rh != focusH / 2) && !giveUpFrame)
                srScaled = false;
            XrView views[2]{};
            for (XrView& v : views) {
                v.fov = {-0.9512f, 0.8814f, 0.8290f, -0.8290f};
                v.pose.orientation = {0, 0, 0, 1};
                v.pose.position = {0.03f, 1.2f, 0.0f};
            }
            simqvfr::dlss_jitter_views(views, 2);
            // 0.5 m a frame (45 m/s at 90 fps) along z, turning a little, 1.2 m above the floor.
            const double travel = sr && f > stopAt ? stopAt : static_cast<double>(f);
            const double yaw = 0.002 * travel, c = std::cos(yaw), s = std::sin(yaw);
            const simqvfr::dlss::M4 world = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 100.0, 1.2, 0.5 * travel, 1};
            simqvfr::dlss::M4 v;
            simqvfr::dlss::invert(world, v);
            for (int i = 0; i < 16; ++i)
                view[i] = static_cast<float>(v[i]);
            for (int eye = 0; eye < 2; ++eye) {
                float plain[16];
                eye_projection({-0.9512f, 0.8814f, 0.8290f, -0.8290f}, plain);
                eye_projection(views[eye].fov, projection);
                simqvfr::DlssPass pass;
                pass.eye = eye;
                pass.eyeWidth = sceneEyeW, pass.eyeHeight = sceneEyeH;
                pass.focus = true;
                pass.focusWidth = focusW, pass.focusHeight = focusH;
                pass.focusRenderWidth = rw, pass.focusRenderHeight = rh;
                // As the game's remap is made (from the eye's projection as rendered to the focus view's): the eye's jitter
                // (its NDC shift, the projection's third row) taken back out, so the focus view carries only its own.
                const float shiftX = projection[8] - plain[8], shiftY = projection[9] - plain[9];
                pass.remap[0] = 4, pass.remap[1] = -0.3f - 4 * shiftX, pass.remap[2] = 1.6f, pass.remap[3] = -0.08f - 1.6f * shiftY;
                simqvfr::dlss_focus_jitter(eye, rw, rh, pass.remap[1], pass.remap[3]);
                // The scene as the game draws it: the eye through its projection as rendered, the focus view through the
                // eye's clip remapped (its jitter included), each into its corner of the targets (the focus view at its
                // render size).
                const simqvfr::dlss::M4 eyeClip =
                    simqvfr::dlss::mul(simqvfr::dlss::from_floats(view), simqvfr::dlss::reversed_depth(simqvfr::dlss::from_floats(projection)));
                const simqvfr::dlss::M4 focusClip = simqvfr::dlss::mul(
                    eyeClip, simqvfr::dlss::clip_remap_matrix(pass.remap[0], pass.remap[1], pass.remap[2], pass.remap[3]));
                if (giveUpFrame) {  // a marker the stretch must cover where the focus view's region lies beyond its render
                    const float marker[4] = {7, 7, 7, 1};
                    context->ClearRenderTargetView(resolvedRtv.Get(), marker);
                }
                draw_scene_view(scene, eyeClip, view, frame, 0, 0, static_cast<float>(sceneEyeW),
                                static_cast<float>(sceneEyeH), resolvedRtv.Get(), partColour, partDsv);
                draw_scene_view(scene, focusClip, view, frame, static_cast<float>(sceneEyeW), 0, static_cast<float>(rw),
                                static_cast<float>(rh), resolvedRtv.Get(), partColour, partDsv);
                // The game's state at its resolve (MSAA off: at YEBIS's copy, the scene an input of it), then the module.
                if (single) {
                    ID3D11RenderTargetView* const copyTarget = otherRtv.Get();  // some other target bound, as the copy's
                    context->OMSetRenderTargets(1, &copyTarget, nullptr);
                } else {
                    context->OMSetRenderTargets(1, otherRtv.GetAddressOf(), dsv.Get());
                }
                context->PSSetShaderResources(0, 1, resolvedSrv.GetAddressOf());
                // The baseline: the render upscaled bilinearly (as the stretch does), on the rows compared below.
                std::vector<float> bilinear[7];
                if (referenceFrame && eye == 0) {
                    const auto axis = [](unsigned at, unsigned from, unsigned to, unsigned& i0, unsigned& i1, double& t) {
                        const double p = (std::max)(0.0, (at + 0.5) * from / to - 0.5);
                        i0 = (std::min)(static_cast<unsigned>(p), from - 1);
                        i1 = (std::min)(i0 + 1, from - 1);
                        t = p - std::floor(p);
                    };
                    for (unsigned k = 1; k < 8; ++k) {
                        unsigned y0, y1;
                        double ty;
                        axis(focusH * k / 8, rh, focusH, y0, y1, ty);
                        const std::vector<float> r0 = row_of(resolved.Get(), sceneEyeW, y0, rw),
                                                 r1 = row_of(resolved.Get(), sceneEyeW, y1, rw);
                        for (unsigned x = 0; x < focusW && r0.size() == rw && r1.size() == rw; ++x) {
                            unsigned x0, x1;
                            double tx;
                            axis(x, rw, focusW, x0, x1, tx);
                            const double top = r0[x0] + (r0[x1] - r0[x0]) * tx, bottom = r1[x0] + (r1[x1] - r1[x0]) * tx;
                            bilinear[k - 1].push_back(static_cast<float>(top + (bottom - top) * ty));
                        }
                    }
                }
                simqvfr::dlss_after_resolve(context.Get(), resolved.Get(), partDsv, pass);
                if ((referenceFrame || giveUpFrame) && eye == 0 && referenceRtv && accumulate) {
                    // The focus view without jitter at its size: the eye's clip unjittered through the remap without
                    // the focus view's jitter (as the remap was made before it was added), antialiased.
                    const simqvfr::dlss::M4 plainClip = simqvfr::dlss::mul(
                        simqvfr::dlss::mul(simqvfr::dlss::from_floats(view), simqvfr::dlss::reversed_depth(simqvfr::dlss::from_floats(plain))),
                        simqvfr::dlss::clip_remap_matrix(4, -0.3f, 1.6f, -0.08f));
                    const float zero[4] = {}, sixteenth[4] = {1 / 16.0f, 1 / 16.0f, 1 / 16.0f, 1 / 16.0f};
                    context->ClearRenderTargetView(referenceRtv.Get(), zero);
                    for (unsigned i = 0; i < 16; ++i) {
                        double sx, sy, cx, cy;
                        simqvfr::dlss::jitter_pixels(i, sx, sy, 16);
                        simqvfr::dlss::clip_shift_for(sx, sy, focusW, focusH, cx, cy);
                        draw_scene_view(scene, simqvfr::dlss::mul(plainClip, simqvfr::dlss::clip_shift(cx, cy)), view, false, 0, 0,
                                        static_cast<float>(focusW), static_cast<float>(focusH), referenceRtv.Get(), nullptr,
                                        nullptr, accumulate.Get(), sixteenth);
                    }
                }
                if (referenceFrame && eye == 0 && reference) {
                    // Rows across the upscaled region against the reference's, shifted -2..2 each way.
                    for (unsigned k = 1; k < 8; ++k) {
                        const unsigned y = focusH * k / 8;
                        const std::vector<float> out = row_of(resolved.Get(), sceneEyeW, y, focusW);
                        for (int dy = -2; dy <= 2; ++dy) {
                            const std::vector<float> ref = row_of(reference.Get(), 0, y + dy, focusW);
                            for (int dx = -2; dx <= 2; ++dx)
                                placement[dy + 2][dx + 2] += shifted_difference(out, ref, dx) / 7;
                            if (dy == 0)
                                bilinearDiff += shifted_difference(bilinear[k - 1], ref, 0) / 7;
                        }
                    }
                    placed = true;
                }
                if (giveUpFrame && eye == 0 && reference) {
                    // DLSS gave up in this call (no matrices): the region beyond the render must hold the render
                    // stretched, not the marker, and look like the reference there.
                    const unsigned y = focusH * 7 / 8;
                    const std::vector<float> out = row_of(resolved.Get(), sceneEyeW, y, focusW);
                    stretchFilled = !out.empty();
                    for (float value : out)
                        stretchFilled = stretchFilled && std::fabs(value - 7.0f) > 0.5f;
                    stretchDiff = shifted_difference(out, row_of(reference.Get(), 0, y, focusW), 0);
                }
            }
            if (giveUpFrame) {
                stretchedAfter = simqvfr::dlss_stretched_total();
                stretchOff = !simqvfr::dlss_wanted() && simqvfr::dlss_focus_scale() == 1.0;
            }
            simqvfr::dlss_frame_end(static_cast<unsigned long long>(frames) + part * sceneFrames + f + 1);
        }
        matricesFail = false;
        simqvfr::dlss_shutdown();
    }

    context->ClearState();
    context->Flush();

    std::printf("\nthe module's log:\n");
    std::ifstream in(log);
    std::string line, all;
    while (std::getline(in, line)) {
        std::printf("  %s\n", line.c_str());
        all += line + "\n";
    }
    check(all.find("the jitter reaches the game's projection: x as asked") != std::string::npos &&
              all.find("y as asked") != std::string::npos,
          "the jitter is found in the projection, with the sign asked");
    check(all.find("gate: frame") != std::string::npos, "the gate ran");
    // The eye's view reaches above the box; the focus view (a quarter of the eye's tangent span) may lie all inside.
    const size_t eyeLine = all.find("view 0 (");
    const std::string eyeGate = eyeLine == std::string::npos ? "" : all.substr(eyeLine, all.find('\n', eyeLine) - eyeLine);
    check(!eyeGate.empty() && eyeGate.find("no points") == std::string::npos,
          "the shaders' box test puts the eye view's points both in the car's box and outside it");
    // The moving scene's gate lines (after the second and the third start): the depth verdict of every class (it follows
    // the "standing N%"), and the colour's of the nearer class, whose detail a pixel resolves.
    struct Verdicts {
        int lines = 0, depthFit = 0, depthNot = 0, nearColourFit = 0;
    };
    const auto count = [](const std::string& s, const char* what) {
        int n = 0;
        for (size_t k = s.find(what); k != std::string::npos; k = s.find(what, k + 1))
            ++n;
        return n;
    };
    const auto verdicts = [&](size_t from, size_t to) {
        Verdicts v;
        for (size_t at = all.find("gate: frame", from); at != std::string::npos && at < to; at = all.find("gate: frame", at + 1)) {
            const std::string one = all.substr(at, all.find('\n', at) - at);
            ++v.lines;
            v.depthFit += count(one, "%: fit");
            v.depthNot += count(one, "%: partly") + count(one, "%: WORSE than none");
            const size_t nearer = one.find("nearer");
            v.nearColourFit += nearer != std::string::npos && one.find("a pixel off them: fit", nearer) != std::string::npos;
        }
        return v;
    };
    size_t starts[9] = {all.find("lmu dlss: DLAA on"), std::string::npos, std::string::npos, std::string::npos,
                        std::string::npos, std::string::npos, std::string::npos, std::string::npos, std::string::npos};
    for (int i = 1; i < 8 && starts[i - 1] != std::string::npos; ++i)
        starts[i] = all.find("lmu dlss: DLAA on", starts[i - 1] + 1);
    const auto part_verdicts = [&](int i) {
        return starts[i] == std::string::npos ? Verdicts{} : verdicts(starts[i], starts[i + 1]);
    };
    const Verdicts right = part_verdicts(1), wrong = part_verdicts(2), boxed = part_verdicts(3), framed = part_verdicts(4),
                   single = part_verdicts(5), upscaled = part_verdicts(6);
    std::printf("     the moving scene: %d gate lines, depth fit in %d classes, not in %d, the nearer class's colour fit in %d\n",
                right.lines, right.depthFit, right.depthNot, right.nearColourFit);
    std::printf("     its control (the view matrix inverted): %d gate lines, depth fit in %d classes, not in %d\n", wrong.lines,
                wrong.depthFit, wrong.depthNot);
    std::printf("     with the car's box on, nothing in it: %d gate lines, depth fit in %d classes, not in %d\n", boxed.lines,
                boxed.depthFit, boxed.depthNot);
    std::printf("     behind a window frame (in the car's box): %d gate lines, depth fit in %d classes, not in %d\n",
                framed.lines, framed.depthFit, framed.depthNot);
    check(right.lines >= 4 && right.depthFit == 2 * right.lines && right.depthNot == 0,
          "the moving scene's vectors fit by their depth, in the small eye view too");
    check(right.nearColourFit == right.lines, "the moving scene's nearer points fit by their colour");
    check(wrong.lines >= 4 && wrong.depthNot >= wrong.lines, "the control's wrong vectors are found wrong by their depth");
    check(boxed.lines >= 4 && boxed.depthNot == 0 && framed.lines >= 4 && framed.depthNot == 0,
          "with the car's box on, and behind a window frame in it, the vectors fit by their depth");
    std::printf("     MSAA off (single-sample depth): %d gate lines, depth fit in %d classes, not in %d, the nearer class's "
                "colour fit in %d\n",
                single.lines, single.depthFit, single.depthNot, single.nearColourFit);
    const size_t singleAt = starts[5] == std::string::npos ? starts[5] : all.find("depth from the scene's depth target", starts[5]);
    const std::string singleDepth = singleAt == std::string::npos ? "" : all.substr(singleAt, all.find('\n', singleAt) - singleAt);
    std::printf("     %s\n", singleDepth.c_str());
    check(singleDepth.find(" x1)") != std::string::npos && single.lines >= 4 && single.depthFit == 2 * single.lines &&
              single.depthNot == 0 &&
              single.nearColourFit == single.lines,
          "MSAA off: the depth read from a single-sample target, the vectors fit as with MSAA");

    // The focus views in DLSS performance mode.
    const std::string srPart = starts[6] == std::string::npos ? "" : all.substr(starts[6], starts[7] - starts[6]);
    char made[96];
    std::snprintf(made, sizeof(made), "view 2's DLSS feature made at %ux%u up to %ux%u (performance", focusW / 2, focusH / 2,
                  focusW, focusH);
    char gateView[48];
    std::snprintf(gateView, sizeof(gateView), "view 2 (%ux%u)", focusW / 2, focusH / 2);
    std::printf("     super resolution: %d gate lines, depth fit in %d classes, not in %d\n", upscaled.lines, upscaled.depthFit,
                upscaled.depthNot);
    check(srPart.find("DLSS performance on the focus views") != std::string::npos && srPart.find(made) != std::string::npos &&
              srPart.find("(32 phases)") != std::string::npos,
          "super resolution: the focus views' features made at half size up to their size, 32 jitter phases");
    check(srPart.find("ngx: (Perf) App hint Preset Unspecified or Overridden, using title default Preset M") != std::string::npos,
          "super resolution: NGX's line says it took preset M, its default for performance");

    // The presets per view (the first part: the eyes on F, the focus views on NGX's default), as made and as NGX took them.
    const std::string firstPart = starts[0] == std::string::npos ? "" : all.substr(starts[0], starts[1] - starts[0]);
    char eyeMade[96], focusMade[96];
    std::snprintf(eyeMade, sizeof(eyeMade), "view 0's DLAA feature made at 1600x1280 (preset F");
    std::snprintf(focusMade, sizeof(focusMade), "view 2's DLAA feature made at %ux%u (preset default", focusW, focusH);
    const size_t eyeAt = firstPart.find(eyeMade), focusAt = firstPart.find(focusMade);
    const auto ngx_line_before = [&](size_t at, const char* what) {  // NGX's preset line comes right before ours
        const size_t found = at == std::string::npos ? at : firstPart.rfind("ngx: (DLAA)", at);
        return found != std::string::npos && firstPart.find(what, found) != std::string::npos &&
               firstPart.find(what, found) < at;
    };
    check(firstPart.find("presets: the eyes F, the focus views default") != std::string::npos && eyeAt != std::string::npos &&
              focusAt != std::string::npos,
          "presets: the eyes' features made with F, the focus views' with NGX's default");
    check(ngx_line_before(eyeAt, "Using App hint Preset F") &&
              ngx_line_before(focusAt, "using title default Preset K"),
          "presets: NGX's lines say it took F for the eyes and K (its DLAA default) for the focus views");
    check(srScaled, "super resolution: the focus views asked to be drawn at half size from the frame after NGX started");
    check(upscaled.lines >= 4 && upscaled.depthFit == 2 * upscaled.lines && upscaled.depthNot == 0 &&
              srPart.find(gateView) != std::string::npos,
          "super resolution: the vectors fit by their depth, the focus view's at its render size");
    int bestX = 0, bestY = 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            if (placement[dy + 2][dx + 2] < placement[bestY + 2][bestX + 2])
                bestX = dx, bestY = dy;
    std::printf("     the upscaled focus view against its reference (mean difference per texel, rows across it), shifted:\n");
    for (int dy = -2; dy <= 2; ++dy)
        std::printf("       dy %+d: %.4f %.4f %.4f %.4f %.4f\n", dy, placement[dy + 2][0], placement[dy + 2][1],
                    placement[dy + 2][2], placement[dy + 2][3], placement[dy + 2][4]);
    check(placed && bestX == 0 && bestY == 0 && placement[2][2] < 0.8 * placement[2][4],
          "super resolution: the upscaled focus view sits where the full-size render has the world (no shift, no scale)");
    std::printf("     after 90 frames standing still: DLSS's upscale differs from the full-size render by %.4f (%.0f%% of "
                "the render upscaled bilinearly, %.4f)\n",
                placement[2][2], bilinearDiff > 0 ? 100 * placement[2][2] / bilinearDiff : 0.0, bilinearDiff);
    // Measured 2026-11-10 (RTX 5070 Ti, nvngx_dlss.dll 310.9.1 from ACE): 32% with the inputs right; 54% with the focus view's
    // jitter computed for its full size instead of its render size (a control, the inputs' likeliest mistake).
    check(placed && bilinearDiff > 0 && placement[2][2] < 0.45 * bilinearDiff,
          "super resolution: converged, DLSS's upscale is far nearer the full-size render than a bilinear upscale (jitter "
          "and vectors in render pixels)");
    std::printf("     DLSS given up in a frame: %llu focus views stretched, the row beyond the render %s, differing from the "
                "reference by %.4f\n",
                stretchedAfter - stretchedBefore, stretchFilled ? "filled" : "NOT FILLED", stretchDiff);
    check(stretchedAfter - stretchedBefore == 2 && stretchFilled && stretchDiff >= 0 && stretchDiff < placement[2][4] &&
              stretchOff,
          "super resolution: when DLSS gives up, both focus views are stretched over their region; then their size again");

    // The live part.
    const std::string livePart = starts[7] == std::string::npos ? "" : all.substr(starts[7]);
    char liveFocus[96];
    std::snprintf(liveFocus, sizeof(liveFocus), "view 2's DLSS feature made at %ux%u up to %ux%u (quality, preset default",
                  simqvfr::dlss::render_size(focusW, 0.67), simqvfr::dlss::render_size(focusH, 0.67), focusW, focusH);
    char liveFocusM[80];
    std::snprintf(liveFocusM, sizeof(liveFocusM), "view 2's DLAA feature made at %ux%u (preset M", focusW, focusH);
    char liveEye[80];
    std::snprintf(liveEye, sizeof(liveEye), "view 0's DLAA feature made at %ux%u (preset F", sceneEyeW, sceneEyeH);
    std::printf("     live: evaluations %llu at the off, %llu at the on again, %llu at the switch to DLAA M, %llu at the end; "
                "focus scale %.2f while off, %.2f after\n",
                liveAt[0], liveAt[1], liveAt[2], liveAt[3], liveScaleOff, liveScaleOn);
    check(livePart.find("live: off (the features released)") != std::string::npos && liveAt[1] == liveAt[0] &&
              !liveWantedOff && liveScaleOff == 1.0,
          "live: off takes at the frame's end: no evaluations, the focus views at their size");
    check(livePart.find("live: on: DLAA on the eyes (preset F), the focus views quality (67%, preset default)") != std::string::npos &&
              livePart.find(liveEye) != std::string::npos && livePart.find(liveFocus) != std::string::npos &&
              std::fabs(liveScaleOn - 0.67) < 1e-9 && liveAt[2] - liveAt[1] == 4 * 29,
          "live: on again at quality with F on the eyes: features made with them, every view evaluated each frame");
    char liveFocus77[96];
    std::snprintf(liveFocus77, sizeof(liveFocus77), "view 2's DLSS feature made at %ux%u up to %ux%u (ultra quality",
                  simqvfr::dlss::render_size(focusW, 0.77), simqvfr::dlss::render_size(focusH, 0.77), focusW, focusH);
    check(livePart.find(liveFocusM) != std::string::npos && livePart.find("ngx: (DLAA) Using App hint Preset M") != std::string::npos &&
              liveAt[3] - liveAt[2] == 4 * 29,
          "live: the focus views to DLAA on preset M (NGX took M), then 77%: every view still evaluated each frame");
    check(livePart.find(liveFocus77) != std::string::npos && livePart.find("couldn't be made") == std::string::npos &&
              livePart.find("ngx: (Quality) App hint Preset Unspecified or Overridden, using title default Preset K") != std::string::npos,
          "live: 77% (\"ultra quality\") made in NGX's quality mode, nothing refused");
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

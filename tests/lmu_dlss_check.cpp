// Hardware check of LMU's DLAA module (games/lmu/dlss_views.h) outside the game: its whole path on a real D3D11 device
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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include "games/lmu/dlss_math.h"
#include "games/lmu/dlss_views.h"
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
// multisample colour target, which takes the colour too and is not read).
bool draw_scene_view(const Scene& s, const lmu::dlss::M4& rendered, const float (&view)[16], bool frame, float x, float y,
                     float w, float h, ID3D11RenderTargetView* colour, ID3D11RenderTargetView* msColour,
                     ID3D11DepthStencilView* dsv) {
    lmu::dlss::M4 inverse;
    if (!lmu::dlss::invert(rendered, inverse))
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
    context->OMSetRenderTargets(1, &colour, nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(1, &msColour, dsv);
    context->Draw(3, 0);
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

    lmu::DlssRun run;
    run.views = 2;
    run.dllFolder = argv[1];
    run.gate = true;
    lmu::dlss_start(run);
    check(lmu::dlss_wanted(), "the module takes the run (a build with the NGX SDK)");

    float view[16], projection[16];
    lmu::dlss_test_matrices(view, projection);
    bool stateKept = true, rewritten = true, restLeft = true;
    int stateLost = 0;
    for (int f = 0; f < frames; ++f) {
        XrView views[2]{};
        for (XrView& v : views) {
            v.fov = {-0.9512f, 0.8814f, 0.8290f, -0.8290f};
            v.pose.orientation = {0, std::sin(0.001f * f), 0, std::cos(0.001f * f)};  // the head turning a little
            v.pose.position = {0.03f, 1.2f, 0.0f};
        }
        lmu::dlss_jitter_views(views, 2);
        // The camera drives along z and turns a little: a world matrix inverted into the view.
        const double yaw = 0.002 * f, c = std::cos(yaw), s = std::sin(yaw);
        const lmu::dlss::M4 world = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 100.0, 1.2, 0.5 * f, 1};
        lmu::dlss::M4 v;
        lmu::dlss::invert(world, v);
        for (int i = 0; i < 16; ++i)
            view[i] = static_cast<float>(v[i]);
        for (int eye = 0; eye < 2; ++eye) {
            eye_projection(views[eye].fov, projection);

            lmu::DlssPass pass;
            pass.eye = eye;
            pass.eyeWidth = eyeW, pass.eyeHeight = eyeH;
            pass.focus = true;
            pass.focusWidth = focusW, pass.focusHeight = focusH;
            pass.remap[0] = 4, pass.remap[1] = -0.3f, pass.remap[2] = 1.6f, pass.remap[3] = -0.08f;
            lmu::dlss_focus_jitter(eye, focusW, focusH, pass.remap[1], pass.remap[3]);

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
            lmu::dlss_after_resolve(context.Get(), resolved.Get(), dsv.Get(), pass);
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
        lmu::dlss_frame_end(static_cast<unsigned long long>(f + 1));
    }
    std::printf("     %llu evaluations in %d frames\n", lmu::dlss_evaluations_total(), frames);
    check(lmu::dlss_wanted(), "the module is still on at the end");
    check(lmu::dlss_evaluations_total() == static_cast<unsigned long long>(frames) * 4, "every view of every frame was evaluated");
    std::printf("     the caller's state differed after %d of %d calls\n", stateLost, frames * 2);
    check(stateKept, "the caller's render target, depth target, shader input and viewport are bound as before after every call");
    check(rewritten, "the eye's and the focus view's parts of the scene are rewritten");
    check(restLeft, "the scene outside the views is untouched");
    lmu::dlss_shutdown();

    // The second part: the scene that moves with the camera. A small eye (as the game's periphery at 10-20% of native)
    // and a large focus view; the car's box off, so every point takes the world's vectors (the gate's classes then split
    // at 8 m, both through the world). Then the same with the view matrix taken the wrong way round
    // (lmu_dlss_view_inverse): the control, whose vectors the gate must find wrong. Then a car's window frame fixed to
    // the camera, with the car's box about the head (the head still in the tracking space: the frame stands still on
    // screen): the world comes out from behind its edges every frame, as the game's periphery sees it.
    // Last, the plain scene again with MSAA off: a single-sample depth target (as LMU makes it then: R32 typeless with a
    // D32 view) and the scene's own colour target, the module called at YEBIS's copy instead of a resolve.
    const unsigned sceneEyeW = 520, sceneEyeH = 420, sceneFrames = 330;
    Scene scene;
    check(make_scene(scene), "the moving scene's shaders are made");
    ComPtr<ID3D11RenderTargetView> resolvedRtv;
    device->CreateRenderTargetView(resolved.Get(), nullptr, &resolvedRtv);
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
    for (int part = 0; part < 5; ++part) {
        const bool control = part == 1, frame = part == 3, single = part == 4;
        ID3D11DepthStencilView* const partDsv = single ? dsv1.Get() : dsv.Get();
        ID3D11RenderTargetView* const partColour = single ? resolvedRtv.Get() : otherRtv.Get();
        lmu::DlssRun moving = run;
        moving.car = part == 2 || part == 3;  // part 2: the box alone (nothing in it), part 3: with the frame in it
        const double box[6] = {0.8, 0.8, 0.6, 0.6, 1.0, 1.0};  // the frame inside, the floor and the posts outside
        for (int i = 0; i < 6; ++i)
            moving.carBox[i] = box[i];
        moving.viewInverse = control;
        lmu::dlss_start(moving);
        for (unsigned f = 0; f < sceneFrames && scene.ps && resolvedRtv; ++f) {
            XrView views[2]{};
            for (XrView& v : views) {
                v.fov = {-0.9512f, 0.8814f, 0.8290f, -0.8290f};
                v.pose.orientation = {0, 0, 0, 1};
                v.pose.position = {0.03f, 1.2f, 0.0f};
            }
            lmu::dlss_jitter_views(views, 2);
            // 0.5 m a frame (45 m/s at 90 fps) along z, turning a little, 1.2 m above the floor.
            const double yaw = 0.002 * f, c = std::cos(yaw), s = std::sin(yaw);
            const lmu::dlss::M4 world = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 100.0, 1.2, 0.5 * f, 1};
            lmu::dlss::M4 v;
            lmu::dlss::invert(world, v);
            for (int i = 0; i < 16; ++i)
                view[i] = static_cast<float>(v[i]);
            for (int eye = 0; eye < 2; ++eye) {
                float plain[16];
                eye_projection({-0.9512f, 0.8814f, 0.8290f, -0.8290f}, plain);
                eye_projection(views[eye].fov, projection);
                lmu::DlssPass pass;
                pass.eye = eye;
                pass.eyeWidth = sceneEyeW, pass.eyeHeight = sceneEyeH;
                pass.focus = true;
                pass.focusWidth = focusW, pass.focusHeight = focusH;
                // As the game's remap is made (from the eye's projection as rendered to the focus view's): the eye's jitter
                // (its NDC shift, the projection's third row) taken back out, so the focus view carries only its own.
                const float shiftX = projection[8] - plain[8], shiftY = projection[9] - plain[9];
                pass.remap[0] = 4, pass.remap[1] = -0.3f - 4 * shiftX, pass.remap[2] = 1.6f, pass.remap[3] = -0.08f - 1.6f * shiftY;
                lmu::dlss_focus_jitter(eye, focusW, focusH, pass.remap[1], pass.remap[3]);
                // The scene as the game draws it: the eye through its projection as rendered, the focus view through the
                // eye's clip remapped (its jitter included), each into its corner of the targets.
                const lmu::dlss::M4 eyeClip =
                    lmu::dlss::mul(lmu::dlss::from_floats(view), lmu::dlss::reversed_depth(lmu::dlss::from_floats(projection)));
                const lmu::dlss::M4 focusClip = lmu::dlss::mul(
                    eyeClip, lmu::dlss::clip_remap_matrix(pass.remap[0], pass.remap[1], pass.remap[2], pass.remap[3]));
                draw_scene_view(scene, eyeClip, view, frame, 0, 0, static_cast<float>(sceneEyeW),
                                static_cast<float>(sceneEyeH), resolvedRtv.Get(), partColour, partDsv);
                draw_scene_view(scene, focusClip, view, frame, static_cast<float>(sceneEyeW), 0, static_cast<float>(focusW),
                                static_cast<float>(focusH), resolvedRtv.Get(), partColour, partDsv);
                // The game's state at its resolve (MSAA off: at YEBIS's copy, the scene an input of it), then the module.
                if (single) {
                    ID3D11RenderTargetView* const copyTarget = otherRtv.Get();  // some other target bound, as the copy's
                    context->OMSetRenderTargets(1, &copyTarget, nullptr);
                } else {
                    context->OMSetRenderTargets(1, otherRtv.GetAddressOf(), dsv.Get());
                }
                context->PSSetShaderResources(0, 1, resolvedSrv.GetAddressOf());
                lmu::dlss_after_resolve(context.Get(), resolved.Get(), partDsv, pass);
            }
            lmu::dlss_frame_end(static_cast<unsigned long long>(frames) + part * sceneFrames + f + 1);
        }
        lmu::dlss_shutdown();
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
    size_t starts[7] = {all.find("lmu dlss: DLAA on"), std::string::npos, std::string::npos, std::string::npos,
                        std::string::npos, std::string::npos, std::string::npos};
    for (int i = 1; i < 6 && starts[i - 1] != std::string::npos; ++i)
        starts[i] = all.find("lmu dlss: DLAA on", starts[i - 1] + 1);
    const auto part_verdicts = [&](int i) {
        return starts[i] == std::string::npos ? Verdicts{} : verdicts(starts[i], starts[i + 1]);
    };
    const Verdicts right = part_verdicts(1), wrong = part_verdicts(2), boxed = part_verdicts(3), framed = part_verdicts(4),
                   single = part_verdicts(5);
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
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

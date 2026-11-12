// Hardware check of DLSS-NR on the views (engine/nr_views.h, engine/ngx_nr.h) outside the game: the D3D11 side, the
// D3D12 runtime behind it and the way back, on a real D3D11 device. Not part of the test suite (no _test suffix): it
// needs an NVIDIA RTX GPU, a build with the NGX SDK, a folder holding nvngx_dlss.dll and one holding nvngx_dlssnr.dll
// (the player's own: a game that ships DLSS-NR has it).
//
//   sim_qvfr_tests nr_views_check <folder with nvngx_dlss.dll> <folder with nvngx_dlssnr.dll>
//
// 1. The module alone on a picture with values far above 1 (nr_run_view called as dlss_views.cpp calls it):
//    - intensity 0 gives the picture back, highlights included (the curve in and out, the region's place);
//    - intensity 0 at half the working size gives it back too (only the model's change is taken from the smaller size);
//    - a focus view without the periphery keeps its edge as it came and changes inside (the edge blend);
//    - an eye's view covered in part changes only inside the covered part, which sits about the focus view;
//    - a slider follows a live change without a new feature; a new working size makes one.
// 2. With DLSS (engine/dlss_views.h) on the same device, as LMU runs it: NGX on D3D11 and on D3D12 side by side, every
//    DLSS and DLSS-NR evaluation succeeds, after DLSS and before it, the caller's state kept.
// The modules' log lines are printed at the end.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include "engine/dlss_math.h"
#include "engine/dlss_views.h"
#include "engine/nr_math.h"
#include "engine/nr_views.h"
#include "log.h"

using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    failures += ok ? 0 : 1;
}

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;

// A picture of sorts, linear HDR: sky, a ground with a fine checker, a disc of 6.0, 5.5, 4.5.
void picture(unsigned w, unsigned h, std::vector<uint16_t>& half) {
    half.resize(size_t(w) * h * 4);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float rgb[3];
            if (v < 0.45f) {
                rgb[0] = 0.25f + 0.3f * v, rgb[1] = 0.45f + 0.3f * v, rgb[2] = 0.9f - 0.4f * v;
            } else {
                const float detail = ((x / 3 + y / 3) % 2) * 0.08f;
                rgb[0] = 0.18f + detail + 0.1f * u, rgb[1] = 0.2f + detail, rgb[2] = 0.17f + detail;
            }
            const float sx = (u - 0.6f) / 0.08f, sy = (v - 0.3f) / 0.08f;
            if (sx * sx + sy * sy < 1)
                rgb[0] = 6.0f, rgb[1] = 5.5f, rgb[2] = 4.5f;
            uint16_t* p = &half[(size_t(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c)
                p[c] = XMConvertFloatToHalf(rgb[c]);
            p[3] = XMConvertFloatToHalf(1.0f);
        }
}

// The texture's RGB as floats.
bool read(ID3D11Texture2D* texture, std::vector<float>& rgb) {
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &staging)))
        return false;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;
    rgb.resize(size_t(d.Width) * d.Height * 3);
    for (unsigned y = 0; y < d.Height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
        for (unsigned x = 0; x < d.Width; ++x)
            for (int c = 0; c < 3; ++c)
                rgb[(size_t(y) * d.Width + x) * 3 + c] = XMConvertHalfToFloat(row[x * 4 + c]);
    }
    context->Unmap(staging.Get(), 0);
    return true;
}

struct Diff {
    double mean = 0, max = 0;
};
// Over (x0, y0, w, h) of two pictures `stride` wide, each difference relative to 1 + the value.
Diff diff(const std::vector<float>& a, const std::vector<float>& b, unsigned stride, unsigned x0, unsigned y0, unsigned w, unsigned h) {
    Diff d;
    for (unsigned y = y0; y < y0 + h; ++y)
        for (unsigned x = x0; x < x0 + w; ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = (size_t(y) * stride + x) * 3 + c;
                const double e = std::fabs(a[i] - b[i]) / (1.0 + std::fabs(b[i]));
                d.mean += e;
                d.max = (std::max)(d.max, std::isfinite(e) ? e : 1e9);
            }
    d.mean /= double(w) * h * 3;
    return d;
}

float projection[16], view[16];
bool check_matrices(int, float (&v)[16], float (&p)[16]) {
    std::memcpy(v, view, sizeof(v));
    std::memcpy(p, projection, sizeof(p));
    return true;
}

void eye_projection(const XrFovf& fov, float (&p)[16]) {
    const double l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), u = std::tan(fov.angleUp), dn = std::tan(fov.angleDown),
                 n = 0.05, fz = 20000;
    for (float& m : p)
        m = 0;
    p[0] = static_cast<float>(2 / (r - l));
    p[5] = static_cast<float>(2 / (u - dn));
    p[8] = static_cast<float>(-(r + l) / (r - l));
    p[9] = static_cast<float>(-(u + dn) / (u - dn));
    p[10] = static_cast<float>(fz / (fz - n));
    p[11] = 1;
    p[14] = static_cast<float>(-n * fz / (fz - n));
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: nr_views_check <folder with nvngx_dlss.dll> <folder with nvngx_dlssnr.dll>\n");
        return 2;
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring log = std::wstring(temp) + L"sim-qvfr-nr-views-check.log";
    DeleteFileW(log.c_str());
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", log.c_str());

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                                 &context))) {
        std::printf("no D3D11 hardware device\n");
        return 2;
    }

    // ---- 1. The module alone. A scene 1400x700 holding a view 640x480 at (300, 100).
    const unsigned W = 1400, H = 700, vx = 300, vy = 100, vw = 640, vh = 480;
    std::vector<uint16_t> half;
    picture(W, H, half);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W, td.Height = H, td.MipLevels = td.ArraySize = 1, td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> scene, depth, vectors;
    device->CreateTexture2D(&td, nullptr, &scene);
    td.Width = vw, td.Height = vh, td.Format = DXGI_FORMAT_R32_FLOAT;
    std::vector<float> depthData(size_t(vw) * vh, 0.0625f);
    D3D11_SUBRESOURCE_DATA init{depthData.data(), vw * 4, 0};
    device->CreateTexture2D(&td, &init, &depth);
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    std::vector<uint16_t> zero(size_t(vw) * vh * 2, 0);
    init = {zero.data(), vw * 4, 0};
    device->CreateTexture2D(&td, &init, &vectors);
    ComPtr<ID3D11ShaderResourceView> depthSrv, vectorsSrv;
    check(scene && depth && vectors && SUCCEEDED(device->CreateShaderResourceView(depth.Get(), nullptr, &depthSrv)) &&
              SUCCEEDED(device->CreateShaderResourceView(vectors.Get(), nullptr, &vectorsSrv)),
          "the stand-in scene, depth and vectors are made");
    if (!depthSrv || !vectorsSrv)
        return 1;
    std::vector<float> came;
    const auto reset_scene = [&] { context->UpdateSubresource(scene.Get(), 0, nullptr, half.data(), W * 8, 0); };
    reset_scene();
    read(scene.Get(), came);

    simqvfr::NrSettings base;
    base.on = true;
    base.folder = argv[2];
    base.edgeBlend = 0;
    unsigned long long frame = 0;
    // `frames` frames of view `index` through the module with `settings`; the scene read back after the last.
    const auto run = [&](const simqvfr::NrSettings& settings, int index, int frames, std::vector<float>& out, bool start = true) {
        if (start)
            simqvfr::nr_start("nr check", settings);
        bool ok = true;
        for (int f = 0; f < frames; ++f) {
            reset_scene();
            simqvfr::NrView v;
            v.index = index;
            v.scene = scene.Get();
            v.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            v.x = vx, v.y = vy, v.width = vw, v.height = vh;
            v.depth = depthSrv.Get(), v.vectors = vectorsSrv.Get();
            v.guideWidth = vw, v.guideHeight = vh;
            v.depthInverted = true;
            v.reset = f == 0;
            v.focusKnown = true, v.focusU = 0.7, v.focusV = 0.4;
            ok = simqvfr::nr_run_view(context.Get(), v) && ok;
            simqvfr::nr_frame_end(++frame);
        }
        return read(scene.Get(), out) && ok;
    };

    std::vector<float> out;
    {
        simqvfr::NrSettings s = base;
        s.intensity = 0;
        const bool ran = run(s, 2, 4, out);
        check(ran, "a focus view goes through the model (NGX on D3D12, the shared textures, the fence)");
        if (!ran) {
            std::ifstream in(log);
            for (std::string line; std::getline(in, line);)
                std::printf("  log: %s\n", line.c_str());
            return 1;
        }
        const Diff inside = diff(out, came, W, vx, vy, vw, vh), left = diff(out, came, W, 0, 0, vx, H);
        std::printf("     intensity 0: the view vs the picture as it came: mean %.6f, max %.6f (relative); left of it: max %.6f\n",
                    inside.mean, inside.max, left.max);
        check(inside.max < 0.01, "intensity 0 gives the picture back, its highlights of 6.0 included");
        check(left.max == 0, "the scene outside the view is untouched");

        s.focusScale = 50;
        run(s, 2, 4, out);
        const Diff halved = diff(out, came, W, vx, vy, vw, vh);
        unsigned region[4] = {}, working[2] = {};
        simqvfr::nr_view_for_checks(2, region, working);
        std::printf("     intensity 0 worked at %ux%u: mean %.6f, max %.6f\n", working[0], working[1], halved.mean, halved.max);
        check(working[0] == vw / 2 && working[1] == vh / 2, "focus_scale 50 works at half the view's size each way");
        check(halved.max < 0.01, "at half the working size intensity 0 still gives the full-size picture back");
    }
    std::vector<float> full;
    {
        simqvfr::NrSettings s = base;
        s.edgeBlend = 10;
        run(s, 2, 6, out);
        full = out;
        const Diff edge = diff(out, came, W, vx, vy, vw, 1), middle = diff(out, came, W, vx + vw / 4, vy + vh / 4, vw / 2, vh / 2);
        const Diff sideEdge = diff(out, came, W, vx, vy, 1, vh);
        std::printf("     intensity 100, edge blend 10%%: the top row vs as it came: max %.6f, the left column: max %.6f, the middle: "
                    "mean %.5f\n",
                    edge.max, sideEdge.max, middle.mean);
        check(middle.mean > 0.002, "the model changes the middle of the view");
        check(edge.max < 0.01 && sideEdge.max < 0.01, "the view's edge stays as it came (the edge blend)");
    }
    {  // an eye's view, half covered about a focus view at (0.7, 0.4)
        simqvfr::NrSettings s = base;
        s.periphery = true;
        s.peripheryScale = 50;
        s.peripheryCoverage = 50;
        s.edgeBlend = 10;
        run(s, 0, 6, out);
        unsigned region[4] = {}, working[2] = {};
        const bool have = simqvfr::nr_view_for_checks(0, region, working);
        const simqvfr::nr::Region want = simqvfr::nr::covered(vw, vh, 0.7, 0.4, 50);
        std::printf("     an eye's view covered 50%%: region %ux%u at (%u, %u), worked at %ux%u\n", region[2], region[3], region[0], region[1],
                    working[0], working[1]);
        check(have && region[0] == want.x && region[1] == want.y && region[2] == want.width && region[3] == want.height &&
                  region[2] == vw / 2 && working[0] == vw / 4,
              "the covered part is half the view each way about the focus view, worked at half of that");
        const Diff inside = diff(out, came, W, vx + region[0] + region[2] / 4, vy + region[1] + region[3] / 4, region[2] / 2, region[3] / 2);
        std::vector<float> masked = out;  // the covered part put back: the rest must be the picture as it came
        for (unsigned y = vy + region[1]; y < vy + region[1] + region[3]; ++y)
            for (unsigned x = vx + region[0]; x < vx + region[0] + region[2]; ++x)
                for (int c = 0; c < 3; ++c)
                    masked[(size_t(y) * W + x) * 3 + c] = came[(size_t(y) * W + x) * 3 + c];
        const Diff outside = diff(masked, came, W, 0, 0, W, H);
        std::printf("     inside it: mean %.5f; outside it: max %.6f\n", inside.mean, outside.max);
        check(inside.mean > 0.002, "the model changes the covered part");
        check(outside.max == 0, "the eye's view outside the covered part is untouched");
        // The focus views with the periphery on: no fade at their edge (what lies beyond is processed too).
        run(s, 2, 6, out, false);
        const Diff edge = diff(out, came, W, vx + vw / 4, vy, vw / 2, 2);
        std::printf("     a focus view with the periphery on: its top rows vs as they came: mean %.5f\n", edge.mean);
        check(edge.mean > 0.0005, "with the periphery processed the focus view's edge doesn't fade");
    }
    {  // live changes
        simqvfr::NrSettings s = base;
        s.edgeBlend = 10;
        run(s, 2, 6, out);
        const unsigned long long before = simqvfr::nr_evaluations_total();
        simqvfr::NrSettings live = s;
        live.intensity = 0;
        simqvfr::nr_live(live);
        run(live, 2, 3, out, false);
        const Diff back = diff(out, came, W, vx, vy, vw, vh);
        std::printf("     live intensity 0: the view vs as it came: max %.6f\n", back.max);
        check(back.max < 0.01 && simqvfr::nr_evaluations_total() == before + 3, "a live intensity change follows at the next frame");
        live.focusScale = 75;
        simqvfr::nr_live(live);
        run(live, 2, 2, out, false);
        unsigned region[4] = {}, working[2] = {};
        simqvfr::nr_view_for_checks(2, region, working);
        const bool held = working[0] == vw;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        run(live, 2, 3, out, false);
        simqvfr::nr_view_for_checks(2, region, working);
        std::printf("     live focus_scale 75: worked at %ux%u after it settled\n", working[0], working[1]);
        check(held && working[0] == vw * 3 / 4, "a live working-size change waits until it has settled, then makes a new feature");
        live.on = false;
        simqvfr::nr_live(live);
        simqvfr::nr_frame_end(++frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        simqvfr::nr_frame_end(++frame);
        check(!simqvfr::nr_wanted(), "live off");
    }
    simqvfr::nr_shutdown();

    // ---- 2. With DLSS, as LMU runs it: an eye and its focus view side by side, both orders.
    {
        const unsigned eyeW = 1200, eyeH = 960, focusW = 960, focusH = 980, SW = eyeW + focusW, SH = focusH;
        std::vector<uint16_t> big;
        picture(SW, SH, big);
        td = {};
        td.Width = SW, td.Height = SH, td.MipLevels = td.ArraySize = 1, td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> resolved, other, sceneDepth;
        device->CreateTexture2D(&td, nullptr, &resolved);
        device->CreateTexture2D(&td, nullptr, &other);
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&td, nullptr, &sceneDepth);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT, dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> dsv;
        ComPtr<ID3D11RenderTargetView> otherRtv;
        check(resolved && sceneDepth && SUCCEEDED(device->CreateDepthStencilView(sceneDepth.Get(), &dd, &dsv)) &&
                  SUCCEEDED(device->CreateRenderTargetView(other.Get(), nullptr, &otherRtv)),
              "the stand-in scene with an eye and its focus view is made");
        if (!dsv || !otherRtv)
            return 1;
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0625f, 0);
        std::vector<float> bigCame;
        for (int order = 0; order < 2; ++order) {
            simqvfr::DlssRun dlss;
            dlss.tag = "lmu dlss";
            dlss.views = 2;
            dlss.dllFolder = argv[1];
            dlss.focusScale = order ? 67 : 100;  // before DLSS: under a focus upscale, the fewer pixels
            simqvfr::dlss_start(dlss);
            simqvfr::dlss_set_matrices(&check_matrices);
            simqvfr::NrSettings s = base;
            s.periphery = order == 0;
            s.peripheryScale = 50;
            s.peripheryCoverage = 60;
            s.edgeBlend = 10;
            s.beforeDlss = order == 1;
            simqvfr::nr_start("lmu dlss-nr", s);
            const unsigned long long nrBefore = simqvfr::nr_evaluations_total(), dlssBefore = simqvfr::dlss_evaluations_total();
            const int frames = 40;
            bool stateKept = true;
            for (int f = 0; f < frames; ++f) {
                XrView views[2]{};
                for (XrView& v : views) {
                    v.fov = {-0.9512f, 0.8814f, 0.8290f, -0.8290f};
                    v.pose.orientation = {0, 0, 0, 1};
                    v.pose.position = {0.03f, 1.2f, 0.0f};
                }
                simqvfr::dlss_jitter_views(views, 2);
                const simqvfr::dlss::M4 world = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100.0, 1.2, 0.0, 1};
                simqvfr::dlss::M4 v;
                simqvfr::dlss::invert(world, v);
                for (int i = 0; i < 16; ++i)
                    view[i] = static_cast<float>(v[i]);
                const double scale = simqvfr::dlss_focus_scale();
                for (int eye = 0; eye < 2; ++eye) {
                    eye_projection(views[eye].fov, projection);
                    simqvfr::DlssPass pass;
                    pass.eye = eye;
                    pass.eyeWidth = eyeW, pass.eyeHeight = eyeH;
                    pass.focus = true;
                    pass.focusWidth = focusW, pass.focusHeight = focusH;
                    pass.focusRenderWidth = simqvfr::dlss::render_size(focusW, scale);
                    pass.focusRenderHeight = simqvfr::dlss::render_size(focusH, scale);
                    pass.remap[0] = 4, pass.remap[1] = -0.3f, pass.remap[2] = 1.6f, pass.remap[3] = -0.08f;
                    simqvfr::dlss_focus_jitter(eye, pass.focusRenderWidth, pass.focusRenderHeight, pass.remap[1], pass.remap[3]);
                    context->UpdateSubresource(resolved.Get(), 0, nullptr, big.data(), SW * 8, 0);
                    if (bigCame.empty())
                        read(resolved.Get(), bigCame);
                    const D3D11_VIEWPORT vp{3, 5, 123, 45, 0, 1};
                    context->OMSetRenderTargets(1, otherRtv.GetAddressOf(), dsv.Get());
                    context->RSSetViewports(1, &vp);
                    simqvfr::dlss_after_resolve(context.Get(), resolved.Get(), dsv.Get(), pass);
                    ComPtr<ID3D11RenderTargetView> rtvNow;
                    ComPtr<ID3D11DepthStencilView> dsvNow;
                    context->OMGetRenderTargets(1, &rtvNow, &dsvNow);
                    UINT count = 1;
                    D3D11_VIEWPORT vpNow{};
                    context->RSGetViewports(&count, &vpNow);
                    stateKept = stateKept && rtvNow.Get() == otherRtv.Get() && dsvNow.Get() == dsv.Get() && count == 1 && vpNow.Width == 123;
                }
                simqvfr::dlss_frame_end(static_cast<unsigned long long>(order * 1000 + f + 1));
            }
            const unsigned long long nr = simqvfr::nr_evaluations_total() - nrBefore, ds = simqvfr::dlss_evaluations_total() - dlssBefore;
            std::printf("     %s DLSS%s: %llu DLSS evaluations, %llu DLSS-NR evaluations in %d frames\n", order ? "before" : "after",
                        order ? " (focus views upscaled from 67%)" : " (the eyes' views too)", ds, nr, frames);
            check(ds == static_cast<unsigned long long>(frames) * 4, "every view's DLSS evaluation succeeded beside DLSS-NR");
            check(nr == static_cast<unsigned long long>(frames) * (order ? 2 : 4), "every wanted view's DLSS-NR evaluation succeeded");
            check(stateKept, "the caller's render target, depth target and viewport are bound as before after every call");
            read(resolved.Get(), out);
            const Diff under = diff(out, bigCame, SW, 0, eyeH + 4, eyeW, SH - eyeH - 4);
            check(under.max == 0, "the scene outside the views is untouched");
            unsigned region[4] = {}, working[2] = {};
            if (simqvfr::nr_view_for_checks(2, region, working))
                std::printf("     focus view: region %ux%u worked at %ux%u\n", region[2], region[3], working[0], working[1]);
            if (order == 1)
                check(region[2] == simqvfr::dlss::render_size(focusW, 0.67), "before DLSS the focus view is processed at its render size");
            simqvfr::dlss_shutdown();  // DLSS-NR's with it
        }
    }

    std::ifstream in(log);
    for (std::string line; std::getline(in, line);)
        if (line.find("dlss") != std::string::npos || line.find("nr") != std::string::npos || line.find("ngx") != std::string::npos)
            std::printf("  log: %s\n", line.c_str());
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}

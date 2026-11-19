// Offline check of LMU's night car lighting (games/lmu/car_light.h, quad/car_lighting.h): the settings round-trip
// through the ini and the live word; the light table keeps each floodlight once, per car slot the ones that reach the
// part (nearest first), and the paint flag; the unprojection and the highlight's math; and on a WARP device the pass
// itself: car pixels (the slot target) scaled by the fill and lit by a floodlight's highlight where lacquer mirrors it,
// every other pixel left exactly as it was, and its CPU and GPU timings averaged.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>

#include "engine/dxbc_patch.h"
#include "games/lmu/car_light.h"
#include "quad/car_lighting.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;

namespace {

float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float v;
    if (exponent == 0)
        v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? NAN : INFINITY;
    else
        v = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -v : v;
}
uint16_t float_to_half(float f) {
    // Enough for the test's values (normal, positive, well within range).
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const int exponent = static_cast<int>((bits >> 23) & 0xff) - 127 + 15;
    const uint32_t mantissa = (bits >> 13) & 0x3ff;
    return static_cast<uint16_t>((exponent << 10) | mantissa);
}

// The test's camera: view space looking down +z, row vectors, reversed depth (near n at 1, infinity at 0) as LMU's.
constexpr float kScale = 1.0f, kNear = 0.1f;
void projection(float (&p)[16]) {
    const float m[16] = {kScale, 0, 0, 0, 0, kScale, 0, 0, 0, 0, 0, 1, 0, 0, kNear, 0};
    std::memcpy(p, m, sizeof(m));
}

void put_light(std::vector<uint8_t>& buffer, unsigned i, float x, float y, float z, float radius, float c) {
    const float pos[4] = {x, y, z, radius}, col[4] = {c, c, c, 0.005f};
    std::memcpy(buffer.data() + 16 * i, pos, 16);
    std::memcpy(buffer.data() + 320 + 16 * i, col, 16);
}

void settings_checks() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::wstring ini = std::wstring(dir) + L"lmu_car_light_test.ini";
    DeleteFileW(ini.c_str());
    const simqvfr::CarLighting missing = simqvfr::read_car_lighting(ini.c_str());
    check(!missing.on && missing.fill == 80 && missing.occlusion == 70 && missing.highlights == 100 && missing.gloss == 70 &&
              missing.smooth == 15,
          "settings: the defaults when the keys are missing (off, 80/70/100/70, smoothing 15 cm)");
    simqvfr::CarLighting c;
    c.on = true;
    c.fill = 55;
    c.occlusion = 30;
    c.highlights = 250;  // past its range
    c.gloss = 5;
    c.smooth = 60;  // past its range
    check(simqvfr::write_car_lighting(ini.c_str(), c), "settings: written");
    const simqvfr::CarLighting back = simqvfr::read_car_lighting(ini.c_str());
    check(back.on && back.fill == 55 && back.occlusion == 30 && back.highlights == 200 && back.gloss == 5 && back.smooth == 50,
          "settings: read back, the highlights clamped to 200 and the smoothing to 50 cm");
    check(simqvfr::car_lighting_from_bits(simqvfr::car_lighting_bits(back)) == back, "settings: the live word round-trips");
    simqvfr::set_car_lighting(back);
    check(simqvfr::car_lighting() == back, "settings: the live state holds what was set");
    simqvfr::set_car_lighting(simqvfr::CarLighting{});
    DeleteFileW(ini.c_str());
}

// The comparison image (dev builds' Ctrl+Alt+L): the two views side by side under a bar with the labels, opaque.
void compare_checks() {
    constexpr unsigned w = 64, h = 40;
    std::vector<uint8_t> before(size_t{w} * h * 4), after(size_t{w} * h * 4);
    for (size_t i = 0; i < before.size(); i += 4) {
        before[i] = 40;  // B
        before[i + 3] = 255;
        after[i + 2] = 200;  // R
        after[i + 3] = 255;
    }
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::wstring path = std::wstring(dir) + L"lmu_car_light_compare_test.png";
    DeleteFileW(path.c_str());
    check(lmu::car_light_side_by_side(path, before, after, w, h, L"Before", L"After"), "compare: the side by side written");
    WIN32_FILE_ATTRIBUTE_DATA a{};
    check(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > 100,
          "compare: the PNG is there");
    check(!lmu::car_light_side_by_side(path, before, after, w, h + 1, L"Before", L"After"),
          "compare: refused when the images are smaller than said");
    DeleteFileW(path.c_str());
}

void table_checks() {
    lmu::CarLightTable table(16);
    table.begin_frame();
    std::vector<uint8_t> buffer(640, 0);
    // A car at (0, 0, 0): lights 20 m and 60 m away (radius 125), one 300 m away (out of reach), one invalid (radius 0.1).
    put_light(buffer, 0, 20, 10, 0, 125, 2);
    put_light(buffer, 1, -60, 30, 0, 125, 2);
    put_light(buffer, 2, 300, 30, 0, 125, 2);
    put_light(buffer, 3, 5, 5, 5, 0.1f, 1.4f);
    const lmu::OmniLayout layout{0, 320, 20};
    const float origin[3] = {0, 0, 0};
    table.draw(3, origin, buffer.data(), buffer.size(), layout, 1, true);
    uint32_t w[4];
    table.packed(3, w);
    check(table.lights().size() == 3, "table: the three usable floodlights in the table (the invalid one left out)");
    check((w[0] & 0xff) == 0 && ((w[0] >> 8) & 0xff) == 1 && ((w[0] >> 16) & 0xff) == 0xff,
          "table: slot 3 lists the two that reach it (the far one left out), the rest none");
    check(w[2] == 1, "table: slot 3 is paint");
    // Another part of another car, with one of the same lights: no new light in the table.
    table.draw(7, origin, buffer.data(), buffer.size(), layout, 2, false);
    table.packed(7, w);
    check(table.lights().size() == 3 && (w[0] & 0xffff) == 0x0100 && w[2] == 0,
          "table: the same buffer again (another part): its lights reused, slot 7 not paint");
    check(table.first_slot() == 3 && table.last_slot() == 7, "table: the slots written this frame");
    table.packed(5, w);
    check(w[0] == 0xffffffffu && w[1] == 0xffffffffu && w[2] == 0, "table: a slot not drawn has no lights");
    // A new frame: the slots' lists start again.
    table.begin_frame();
    table.packed(3, w);
    check(w[0] == 0xffffffffu && table.lights().empty() && table.first_slot() > table.last_slot(),
          "table: a new frame starts empty");
    // Nine lights for one part: the eight nearest kept.
    std::vector<uint8_t> many(640, 0);
    for (unsigned i = 0; i < 9; ++i)
        put_light(many, i, 10.0f + 10.0f * i, 0, 0, 200, 1);
    table.draw(1, origin, many.data(), many.size(), layout, 3, false);
    table.packed(1, w);
    bool farthestGone = true;
    for (int k = 0; k < 8; ++k) {
        const uint32_t idx = (w[k / 4] >> (8 * (k % 4))) & 0xff;
        farthestGone = farthestGone && idx != 8 && idx != 0xff;
    }
    check(farthestGone, "table: of nine lights the eight nearest kept");
}

void math_checks() {
    float p[16], inv[16];
    projection(p);
    // Its inverse: x = x'/s, y = y'/s, z = n w'/z'... by construction: [x' y' z' w'] = [s x, s y, n, z].
    const float m[16] = {1 / kScale, 0, 0, 0, 0, 1 / kScale, 0, 0, 0, 0, 0, 1 / kNear, 0, 0, 1, 0};
    std::memcpy(inv, m, sizeof(m));
    float out[3];
    lmu::car_light_unproject(inv, 0.5f, -0.25f, kNear / 5.0f, out);
    check(std::fabs(out[2] - 5) < 1e-4f && std::fabs(out[0] - 2.5f) < 1e-4f && std::fabs(out[1] + 1.25f) < 1e-4f,
          "math: a pixel's depth unprojects to its point (5 m, x = ndc x z)");
    const float n[3] = {0, 0, -1}, eye[3] = {0, 0, -1};
    const float mirror[3] = {0, 0, -1};
    const float off[3] = {0.8f, 0, -0.6f};
    const float peak = lmu::car_light_highlight(n, eye, mirror, 0.3f);
    const float aside = lmu::car_light_highlight(n, eye, off, 0.3f);
    check(peak > 0.2f && aside < peak * 0.05f, "math: the highlight peaks where the lacquer mirrors the light");
    check(lmu::car_light_highlight(n, eye, mirror, 0.08f) > peak, "math: glossier, a brighter, smaller highlight");
}

void pass_checks(bool hardware) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr,
                                 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        check(false, "pass: a D3D11 device");
        return;
    }
    constexpr UINT kW = 64, kH = 32;
    // The scene: 0.2 everywhere; a wall 5 m out across the view; the left half a car part (slot 0, written as 1).
    std::vector<uint16_t> scenePx(size_t{kW} * kH * 4, float_to_half(0.2f));
    for (size_t i = 3; i < scenePx.size(); i += 4)
        scenePx[i] = float_to_half(1.0f);
    D3D11_TEXTURE2D_DESC sd{kW, kH, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA sinit{scenePx.data(), kW * 8};
    ComPtr<ID3D11Texture2D> scene;
    std::vector<float> depthPx(size_t{kW} * kH, kNear / 5.0f);
    D3D11_TEXTURE2D_DESC dd{kW, kH, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA dinit{depthPx.data(), kW * 4};
    ComPtr<ID3D11Texture2D> depth;
    std::vector<uint16_t> slotPx(size_t{kW} * kH, 0);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW / 2; ++x)
            slotPx[size_t{y} * kW + x] = 1;
    D3D11_TEXTURE2D_DESC ld{kW, kH, 1, 1, DXGI_FORMAT_R16_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA linit{slotPx.data(), kW * 2};
    ComPtr<ID3D11Texture2D> slots;
    ComPtr<ID3D11ShaderResourceView> slotView;
    if (FAILED(device->CreateTexture2D(&sd, &sinit, &scene)) || FAILED(device->CreateTexture2D(&dd, &dinit, &depth)) ||
        FAILED(device->CreateTexture2D(&ld, &linit, &slots)) ||
        FAILED(device->CreateShaderResourceView(slots.Get(), nullptr, &slotView))) {
        check(false, "pass: its textures");
        return;
    }
    const auto read = [&]() {
        D3D11_TEXTURE2D_DESC st = sd;
        st.Usage = D3D11_USAGE_STAGING;
        st.BindFlags = 0;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        std::vector<float> px;
        if (FAILED(device->CreateTexture2D(&st, nullptr, &staging)))
            return px;
        context->CopyResource(staging.Get(), scene.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
            return px;
        px.resize(size_t{kW} * kH);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x)
                px[size_t{y} * kW + x] = half_to_float(
                    reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + size_t{y} * m.RowPitch)[x * 4]);
        context->Unmap(staging.Get(), 0);
        return px;
    };
    const auto reset = [&]() { context->UpdateSubresource(scene.Get(), 0, nullptr, scenePx.data(), kW * 8, 0); };

    // One floodlight at the camera, in reach of the wall (the car's part at its middle left).
    lmu::CarLightTable table(16);
    table.begin_frame();
    std::vector<uint8_t> buffer(640, 0);
    put_light(buffer, 0, -2.5f, 0, 0, 50, 2);
    const float origin[3] = {-2.5f, 0, 5};
    table.draw(0, origin, buffer.data(), buffer.size(), lmu::OmniLayout{0, 320, 20}, 1, true);
    lmu::CarLightView view;
    view.eyeW = kW;
    view.eyeH = kH;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(view.view, identity, sizeof(identity));
    projection(view.viewProj);
    lmu::CarLightPass pass;

    simqvfr::CarLighting s;
    s.on = true;
    s.fill = 50;
    s.occlusion = 0;
    s.highlights = 0;
    const bool ran = pass.run(context.Get(), scene.Get(), depth.Get(), slotView.Get(), false, table, view, s, 0);
    check(ran, "pass: made and drawn");
    std::vector<float> px = read();
    if (px.empty()) {
        check(false, "pass: the scene read back");
        return;
    }
    bool carHalved = true, restKept = true;
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const float v = px[size_t{y} * kW + x];
            if (x < kW / 2)
                carHalved = carHalved && std::fabs(v - 0.1f) < 2e-3f;
            else
                restKept = restKept && v == half_to_float(float_to_half(0.2f));
        }
    check(carHalved, "pass: fill 50%: the car's pixels at half");
    check(restKept, "pass: every pixel off the car exactly as it was");

    // Highlights on: the lacquer mirrors the light (at x = -2.5 m) to the camera halfway, x = -1.25 m on the wall 5 m out
    // (pixel 24 of 64 with the test's FOV); far less away from it.
    reset();
    s.fill = 100;
    s.highlights = 100;
    s.gloss = 80;
    pass.run(context.Get(), scene.Get(), depth.Get(), slotView.Get(), false, table, view, s, 0);
    px = read();
    const float atMirror = px[size_t{kH / 2} * kW + 24], aside = px[size_t{kH / 2} * kW + 2];
    char text[160];
    std::snprintf(text, sizeof(text), "pass: a highlight where the light mirrors (%.3f at its middle, %.3f aside, 0.2 base)",
                  atMirror, aside);
    check(atMirror > 0.3f && aside < atMirror && aside >= 0.2f - 2e-3f, text);
    check(px[size_t{kH / 2} * kW + 48] == half_to_float(float_to_half(0.2f)), "pass: highlights only on the car");
    // Mirror-like lacquer (gloss 100) right at the light: a glint held to the cap (1.5 over the paint), not a blowout.
    reset();
    s.gloss = 100;
    pass.run(context.Get(), scene.Get(), depth.Get(), slotView.Get(), false, table, view, s, 0);
    px = read();
    float brightest = 0;
    for (float v : px)
        brightest = (std::max)(brightest, v);
    std::snprintf(text, sizeof(text), "pass: gloss 100: the brightest highlight held to the cap (%.3f, at most 1.7)",
                  brightest);
    check(brightest > 0.3f && brightest <= 1.71f, text);
    s.gloss = 80;

    // A wall nearer than the cockpit's 1 m: left alone.
    reset();
    std::vector<float> nearPx(size_t{kW} * kH, kNear / 0.5f);
    context->UpdateSubresource(depth.Get(), 0, nullptr, nearPx.data(), kW * 4, 0);
    s.fill = 50;
    pass.run(context.Get(), scene.Get(), depth.Get(), slotView.Get(), false, table, view, s, 0);
    px = read();
    check(std::fabs(px[size_t{kH / 2} * kW + 8] - 0.2f) < 2e-3f, "pass: nothing within 1 m (the player's cockpit)");

    // A faceted surface (run 1, 2026-11-18: the triangles showed in the highlights): a cylinder 1 m across, its axis
    // upright 5 m out, made of flat facets 10 cm wide, seen through a narrow FOV (1.6 cm a pixel). Pixel to pixel the
    // highlight jumps at each facet's edge; smoothed over 15 cm it changes gradually.
    {
        lmu::CarLightView narrow = view;
        const float m[16] = {10, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0, 1, 0, 0, kNear, 0};
        std::memcpy(narrow.viewProj, m, sizeof(m));
        const auto profile = [](float x) {  // the facets' z at x: straight between points every 10 cm on the circle
            const float x0 = std::floor(x / 0.1f) * 0.1f, x1 = x0 + 0.1f;
            const auto circle = [](float v) { return 5.0f - std::sqrt((std::max)(0.0f, 1.0f - v * v)); };
            return circle(x0) + (x - x0) / 0.1f * (circle(x1) - circle(x0));
        };
        std::vector<float> facets(size_t{kW} * kH);
        for (UINT x = 0; x < kW; ++x) {
            const float u = ((x + 0.5f) / kW * 2 - 1) / 10;  // x / z along the pixel's ray
            float z = 4;
            for (int i = 0; i < 30; ++i)
                z = profile(u * z);
            for (UINT y = 0; y < kH; ++y)
                facets[size_t{y} * kW + x] = kNear / z;
        }
        context->UpdateSubresource(depth.Get(), 0, nullptr, facets.data(), kW * 4, 0);
        std::vector<uint16_t> allCar(size_t{kW} * kH, 1);
        context->UpdateSubresource(slots.Get(), 0, nullptr, allCar.data(), kW * 2, 0);
        lmu::CarLightTable lit(16);
        lit.begin_frame();
        std::vector<uint8_t> lamp(640, 0);
        put_light(lamp, 0, 0, 0.3f, 0, 50, 2);
        const float centre[3] = {0, 0, 4};
        lit.draw(0, centre, lamp.data(), lamp.size(), lmu::OmniLayout{0, 320, 20}, 1, true);
        const auto worst_step = [&](int smooth) {
            reset();
            simqvfr::CarLighting f;
            f.on = true;
            f.fill = 100;
            f.occlusion = 0;
            f.highlights = 100;
            f.gloss = 40;
            f.smooth = smooth;
            pass.run(context.Get(), scene.Get(), depth.Get(), slotView.Get(), false, lit, narrow, f, 0);
            const std::vector<float> img = read();
            float worst = 0;
            for (UINT x = 4; x + 5 < kW; ++x)
                worst = (std::max)(worst, std::fabs(img[size_t{kH / 2} * kW + x + 1] - img[size_t{kH / 2} * kW + x]));
            return worst;
        };
        const float faceted = worst_step(0), smoothed = worst_step(15);
        std::snprintf(text, sizeof(text),
                      "pass: smoothing hides the facets (largest pixel-to-pixel step %.4f without, %.4f with 15 cm)",
                      faceted, smoothed);
        check(faceted > 0 && smoothed < faceted * 0.5f, text);
        context->UpdateSubresource(slots.Get(), 0, nullptr, slotPx.data(), kW * 2, 0);
        context->UpdateSubresource(depth.Get(), 0, nullptr, nearPx.data(), kW * 4, 0);
    }

    // Timings: averaged over 90 frames.
    for (int f = 0; f < 95; ++f) {
        pass.run(context.Get(), scene.Get(), depth.Get(), slotView.Get(), false, table, view, s, 0);
        context->Flush();
        Sleep(1);
        pass.frame_end(context.Get());
    }
    std::snprintf(text, sizeof(text), "pass: timings averaged (%.3f ms GPU, %.3f ms CPU per frame)", pass.gpu_ms(),
                  pass.cpu_ms());
    check(pass.windows() >= 1 && pass.cpu_ms() > 0 && pass.gpu_ms() >= 0, text);
    check(!pass.failed(), "pass: not failed");
}

// The game's normals (engine/dxbc_patch.h add_slot_output's normal, vertex_normal_rows): a pixel shader with LMU's
// tangent frame inputs (TEXCOORD2-4, the normal in their z) gets render target 2 written with the normal; a vertex
// shader that hands it on that way is recognised, one that doesn't isn't.
ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, entry, target, 0, 0, &code, &errors))) {
        std::printf("compile %s: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

constexpr char kFrameShaders[] = R"(
struct V { float4 pos : SV_Position; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 t4 : TEXCOORD4; };
cbuffer Frame : register(b0) { float4 tangent; float4 binormal; float4 normal; };
V vs_rows(uint id : SV_VertexID, float3 n : NORMAL0) {
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    float3 N = normalize(normal.xyz + n * 0.0001), T = tangent.xyz, B = binormal.xyz;
    o.t2 = float4(T.x, B.x, N.x, 0);
    o.t3 = float4(T.y, B.y, N.y, 0);
    o.t4 = float4(T.z, B.z, N.z, 0);
    return o;
}
V vs_other(uint id : SV_VertexID, float3 n : NORMAL0) {
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.t2 = float4(normalize(normal.xyz + n * 0.0001), 0);
    o.t3 = tangent;
    o.t4 = binormal;
    return o;
}
cbuffer Light : register(b1) { float4 toLight; };
Texture2D aoSpecularMap : register(t0);
SamplerState aoSampler : register(s0);
float4 ps_lit(V i) : SV_Target0 {
    float3 N = float3(i.t2.z, i.t3.z, i.t4.z);  // as LMU's: the normal from the frame's z column
    float ao = aoSpecularMap.Sample(aoSampler, i.t2.xy).y;  // as LMU's: the baked occlusion in .y
    return float4(saturate(dot(N, toLight.xyz)) * ao, i.t2.x, i.t3.y, 1);
}
)";

void normal_checks(bool hardware) {
    const ComPtr<ID3DBlob> rows = compile(kFrameShaders, "vs_rows", "vs_5_0"), other = compile(kFrameShaders, "vs_other", "vs_5_0");
    const ComPtr<ID3DBlob> lit = compile(kFrameShaders, "ps_lit", "ps_5_0");
    if (!rows || !other || !lit) {
        check(false, "normals: the test's shaders compile");
        return;
    }
    const auto* rb = static_cast<const uint8_t*>(rows->GetBufferPointer());
    const auto* ob = static_cast<const uint8_t*>(other->GetBufferPointer());
    check(simqvfr::dxbc::vertex_normal_rows(rb, rows->GetBufferSize()),
          "normals: a vertex shader handing on the normal in TEXCOORD2-4's z is recognised");
    check(!simqvfr::dxbc::vertex_normal_rows(ob, other->GetBufferSize()), "normals: one handing on another layout isn't");
    simqvfr::dxbc::SlotOutputOptions options;
    options.normalReg = 2;
    options.aoResource = 0;  // aoSpecularMap at t0
    const simqvfr::dxbc::SlotOutput twin =
        simqvfr::dxbc::add_slot_output(static_cast<const uint8_t*>(lit->GetBufferPointer()), lit->GetBufferSize(), options);
    check(!twin.bytecode.empty() && twin.normal, "normals: the pixel shader's slot twin writes the normal too");
    check(twin.ao, "normals: and its baked occlusion (the .y of aoSpecularMap's sample) in the w");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr,
                                 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        check(false, "normals: a D3D11 device");
        return;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(device->CreateVertexShader(rb, rows->GetBufferSize(), nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(twin.bytecode.data(), twin.bytecode.size(), nullptr, &ps))) {
        check(false, "normals: the twin is a valid shader");
        return;
    }
    // Targets: colour, the slot (from t127: 7), the normal.
    constexpr UINT kSize = 8;
    const auto target = [&](DXGI_FORMAT f, ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& v) {
        const D3D11_TEXTURE2D_DESC d{kSize, kSize, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
        return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t)) && SUCCEEDED(device->CreateRenderTargetView(t.Get(), nullptr, &v));
    };
    ComPtr<ID3D11Texture2D> colourTex, slotTex, normalTex;
    ComPtr<ID3D11RenderTargetView> colour, slot, normal;
    const uint32_t slotWord = 7;
    D3D11_BUFFER_DESC wd{4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA winit{&slotWord, 4, 0};
    ComPtr<ID3D11Buffer> word;
    ComPtr<ID3D11ShaderResourceView> wordView;
    D3D11_SHADER_RESOURCE_VIEW_DESC wv{};
    wv.Format = DXGI_FORMAT_R32_UINT;
    wv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    wv.Buffer.NumElements = 1;
    const float frame[12] = {1, 0, 0, 0, 0, 0, 1, 0, 0.6f, 0.8f, 0, 0};  // tangent x, binormal z, normal (0.6, 0.8, 0)
    D3D11_BUFFER_DESC cd{sizeof(frame), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA cinit{frame, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    if (!target(DXGI_FORMAT_R8G8B8A8_UNORM, colourTex, colour) || !target(DXGI_FORMAT_R16_UINT, slotTex, slot) ||
        !target(DXGI_FORMAT_R16G16B16A16_UNORM, normalTex, normal) || FAILED(device->CreateBuffer(&wd, &winit, &word)) ||
        FAILED(device->CreateShaderResourceView(word.Get(), &wv, &wordView)) ||
        FAILED(device->CreateBuffer(&cd, &cinit, &cb))) {
        check(false, "normals: the targets");
        return;
    }
    // The baked occlusion: a 1x1 texture whose .y is 0.25 (its x 0.9: the wrong component would show).
    const uint32_t aoTexel = 0xff0040e6u;  // RGBA8: r 0.9, g 0.25, b 0, a 1
    const D3D11_TEXTURE2D_DESC ad{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, 0};
    const D3D11_SUBRESOURCE_DATA ainit{&aoTexel, 4, 0};
    ComPtr<ID3D11Texture2D> aoTex;
    ComPtr<ID3D11ShaderResourceView> aoView;
    ComPtr<ID3D11SamplerState> aoSampler;
    D3D11_SAMPLER_DESC sdsc{};
    sdsc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sdsc.AddressU = sdsc.AddressV = sdsc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateTexture2D(&ad, &ainit, &aoTex)) ||
        FAILED(device->CreateShaderResourceView(aoTex.Get(), nullptr, &aoView)) ||
        FAILED(device->CreateSamplerState(&sdsc, &aoSampler))) {
        check(false, "normals: the occlusion texture");
        return;
    }
    context->PSSetShaderResources(0, 1, aoView.GetAddressOf());
    context->PSSetSamplers(0, 1, aoSampler.GetAddressOf());
    // The vertex shader's NORMAL input: a layout with a zero vertex buffer.
    const D3D11_INPUT_ELEMENT_DESC el{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    const float zeros[9] = {};
    D3D11_BUFFER_DESC vd{sizeof(zeros), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA vinit{zeros, 0, 0};
    ComPtr<ID3D11Buffer> vb;
    if (FAILED(device->CreateInputLayout(&el, 1, rb, rows->GetBufferSize(), &layout)) ||
        FAILED(device->CreateBuffer(&vd, &vinit, &vb))) {
        check(false, "normals: the input layout");
        return;
    }
    const UINT stride = 12, offset = 0;
    context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetShaderResources(127, 1, wordView.GetAddressOf());
    ID3D11RenderTargetView* rtvs[3] = {colour.Get(), slot.Get(), normal.Get()};
    context->OMSetRenderTargets(3, rtvs, nullptr);
    const D3D11_VIEWPORT vp{0, 0, kSize, kSize, 0, 1};
    context->RSSetViewports(1, &vp);
    context->Draw(3, 0);
    D3D11_TEXTURE2D_DESC sd{kSize, kSize, 1, 1, DXGI_FORMAT_R16G16B16A16_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                            D3D11_CPU_ACCESS_READ};
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) {
        check(false, "normals: read back");
        return;
    }
    context->CopyResource(staging.Get(), normalTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        check(false, "normals: read back");
        return;
    }
    uint16_t texel[4];
    std::memcpy(texel, static_cast<const uint8_t*>(m.pData) + 4 * 8 + m.RowPitch * 4, 8);
    context->Unmap(staging.Get(), 0);
    const float nx = texel[0] / 65535.0f * 2 - 1, ny = texel[1] / 65535.0f * 2 - 1, nz = texel[2] / 65535.0f * 2 - 1;
    const float aoRead = texel[3] / 65535.0f;
    char text[160];
    std::snprintf(text, sizeof(text), "normals: render target 2 holds the vertex shader's normal (%.3f %.3f %.3f of 0.6 0.8 0)",
                  nx, ny, nz);
    check(std::fabs(nx - 0.6f) < 0.01f && std::fabs(ny - 0.8f) < 0.01f && std::fabs(nz) < 0.01f, text);
    std::snprintf(text, sizeof(text), "normals: its w holds the baked occlusion (%.3f of 0.251)", aoRead);
    check(std::fabs(aoRead - 64.0f / 255.0f) < 0.01f, text);
}

// --scan <folder>: LMU's shaders saved from a capture (PS_*.dxbc, VS_*.dxbc; local only, never in the repository):
// how many pixel shaders' slot twins write the normal (and are valid shaders on WARP), how many vertex shaders hand it
// on in the layout it reads.
int scan(const char* folder) {
    ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                 nullptr)))
        return 1;
    int ps = 0, twins = 0, normals = 0, aos = 0, invalid = 0, vs = 0, rows = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        const std::string name = entry.path().filename().string();
        std::ifstream f(entry.path(), std::ios::binary);
        const std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (name.rfind("PS_", 0) == 0) {
            ++ps;
            simqvfr::dxbc::SlotOutputOptions options;
            options.normalReg = 2;
            ComPtr<ID3D11ShaderReflection> reflection;
            if (SUCCEEDED(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection))))
                for (const char* aoName : {"aoSpecularMap", "aoSpecMask"}) {
                    D3D11_SHADER_INPUT_BIND_DESC bind{};
                    if (SUCCEEDED(reflection->GetResourceBindingDescByName(aoName, &bind)) && bind.Type == D3D_SIT_TEXTURE) {
                        options.aoResource = bind.BindPoint;
                        break;
                    }
                }
            const simqvfr::dxbc::SlotOutput t = simqvfr::dxbc::add_slot_output(code.data(), code.size(), options);
            if (t.bytecode.empty()) {
                std::printf("  %s: no twin (%s)\n", name.c_str(), t.error.c_str());
                continue;
            }
            ++twins;
            normals += t.normal ? 1 : 0;
            aos += t.ao ? 1 : 0;
            if (options.aoResource != ~0u && !t.ao)
                std::printf("  %s: has an occlusion texture (t%u) but no copy of it%s\n", name.c_str(), options.aoResource,
                            t.normal ? "" : " (no normal either)");
            ComPtr<ID3D11PixelShader> shader;
            if (FAILED(device->CreatePixelShader(t.bytecode.data(), t.bytecode.size(), nullptr, &shader))) {
                ++invalid;
                std::printf("  %s: the twin ISN'T a valid shader\n", name.c_str());
            }
        } else if (name.rfind("VS_", 0) == 0) {
            ++vs;
            const bool r = simqvfr::dxbc::vertex_normal_rows(code.data(), code.size());
            rows += r ? 1 : 0;
            std::printf("  %s: %s\n", name.c_str(), r ? "normal rows" : "another layout");
        }
    }
    std::printf("scan: %d pixel shaders, %d twins (%d write the normal, %d with the baked occlusion, %d invalid); %d "
                "vertex shaders, %d hand on the normal in TEXCOORD2-4's z\n",
                ps, twins, normals, aos, invalid, vs, rows);
    return invalid ? 1 : 0;
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "--scan") == 0)
        return scan(argv[2]);
    const bool hardware = argc > 1 && std::strcmp(argv[1], "--hardware") == 0;
    settings_checks();
    compare_checks();
    table_checks();
    math_checks();
    normal_checks(hardware);
    pass_checks(hardware);
    return test_result();
}

// Offline check of LMU's glare share (games/lmu/glare_share.h, glare_math.h) on a WARP device. YEBIS's glare result is an
// R11G11B10_FLOAT texture whose top-left region a pass uses (viewport scale); a focus pass's is replaced by its eye's,
// resampled through the focus box. Checked here:
// - the region YEBIS uses, from the render texture's size and viewport scale;
// - a halo in the eye's glare lands in the focus view's glare at the same direction and the same angular size (in
//   focus texels: the eye's size times the focus magnification, per axis), the texels beyond the focus region untouched,
//   the focus region's edge carrying the eye's glare at the box edge (no seam), and the game's bound target and viewport
//   as they were (the resample runs between two of YEBIS's draws);
// - the pointer walk from CRenderGlare to the D3D11 texture, on fake objects laid out as abi.h says, refusing nulls,
//   unreadable memory and a texture of another size.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "games/lmu/glare_math.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

// R11G11B10_FLOAT from and to floats (non-negative): the half's bits shifted (5-bit exponent, 6/5-bit mantissa).
std::uint16_t to_half(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
    if (f <= 0 || e <= 0)
        return 0;
    return static_cast<std::uint16_t>((e << 10) | ((x & 0x7fffff) >> 13));
}
float from_half(std::uint32_t h) {
    const int e = (h >> 10) & 0x1f;
    if (e == 0)
        return 0;
    const std::uint32_t x = static_cast<std::uint32_t>(e - 15 + 127) << 23 | (h & 0x3ff) << 13;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
std::uint32_t pack(float r, float g, float b) {
    return (to_half(r) >> 4 & 0x7ff) | (to_half(g) >> 4 & 0x7ff) << 11 | (to_half(b) >> 5 & 0x3ff) << 22;
}
float red(std::uint32_t p) { return from_half((p & 0x7ff) << 4); }

// LMU's eye projection (tests/lmu_focus_test.cpp): row vectors, left-handed, w = z.
void lmu_projection(double l, double r, double d, double u, float (&m)[16]) {
    for (float& v : m)
        v = 0;
    m[0] = static_cast<float>(2 / (r - l));
    m[5] = static_cast<float>(2 / (u - d));
    m[8] = static_cast<float>(-(r + l) / (r - l));
    m[9] = static_cast<float>(-(u + d) / (u - d));
    m[10] = 1.0f;
    m[11] = 1.0f;
    m[14] = -0.05f;
}

constexpr UINT kW = 646, kH = 506;  // YEBIS's glare result for 1614x1266 views (0.4 of the view)
constexpr double kFloor = 0.0078125;  // the glare's floor: 2^-7, exact in R11G11B10 (the far field reads back as it)

ComPtr<ID3D11Texture2D> glare_texture(ID3D11Device* device, const std::vector<std::uint32_t>& texels, UINT bind) {
    D3D11_TEXTURE2D_DESC d{kW, kH, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, bind};
    D3D11_SUBRESOURCE_DATA init{texels.data(), kW * 4};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&d, &init, &t);
    return t;
}

std::vector<std::uint32_t> read_back(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* t) {
    D3D11_TEXTURE2D_DESC d{kW, kH, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    ComPtr<ID3D11Texture2D> back;
    device->CreateTexture2D(&d, nullptr, &back);
    context->CopyResource(back.Get(), t);
    std::vector<std::uint32_t> out(kW * kH);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(context->Map(back.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < kH; ++y)
            std::memcpy(&out[y * kW], static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch, kW * 4);
        context->Unmap(back.Get(), 0);
    }
    return out;
}

// A render texture's UV scale and offset (+0x90): UV per texel of its size, so share / size for a pass at that share.
void uv_scale(float share, unsigned w, unsigned h, float (&out)[4]) {
    out[0] = share / static_cast<float>(w);
    out[1] = share / static_cast<float>(h);
    out[2] = 0;
    out[3] = 0;
}

void region_checks() {
    // The values the 2026-11-08 10:42 run logged at full size: 0.0015 0.0020 (1/646, 1/506), offsets 0.
    float full[4], half[4], forty[4];
    uv_scale(1, kW, kH, full);
    uv_scale(0.5f, kW, kH, half);
    uv_scale(0.4f, kW, kH, forty);
    const float none[4] = {0, 0, 0, 0}, over[4] = {3.0f / kW, 2.0f / kH, 0, 0}, nan[4] = {NAN, 1.0f / kH, 0, 0};
    auto r = lmu::yebis_region(kW, kH, kW, kH, full);
    check(r.x == 0 && r.y == 0 && r.width == 646 && r.height == 506,
          "region: UV scale 1 / size (the run's 0.0015 x 0.0020) is the whole texture, not one texel");
    check(lmu::yebis_region_plausible(r, kW, kH), "region: the whole texture is plausible");
    r = lmu::yebis_region(kW, kH, kW, kH, half);
    check(r.width == 323 && r.height == 253, "region: a pass at 0.5 uses the top-left half (646 x 0.5, 506 x 0.5)");
    r = lmu::yebis_region(kW, kH, kW, kH, forty);
    check(r.width == 258 && r.height == 202, "region: a pass at 0.4 rounds to 258x202");
    r = lmu::yebis_region(kW, kH, kW / 2, kH / 2, full);
    check(r.width == 323 && r.height == 253, "region: a render texture half its texture's size, UV per its texel, is half");
    r = lmu::yebis_region(kW, kH, kW, kH, none);
    check(r.width == 1 && r.height == 1 && !lmu::yebis_region_plausible(r, kW, kH),
          "region: UV scale 0 keeps a texel and is refused as implausible");
    const float asRead[4] = {1, 1, 0, 0};  // the first build's reading: a viewport scale of 1 taken as UV per texel
    r = lmu::yebis_region(kW, kH, kW, kH, asRead);
    check(r.width == 646 && r.height == 506, "region: a UV extent over 1 stops at the texture");
    r = lmu::yebis_region(kW, kH, kW, kH, over);
    check(r.width == 646 && r.height == 506, "region: UV extents 3 and 2 stop at the texture");
    r = lmu::yebis_region(kW, kH, kW, kH, nan);
    check(r.width == 646 && r.height == 506, "region: a non-finite scale is the whole axis");
    const simqvfr::ViewRegion tiny{0, 0, 30, 506};
    check(!lmu::yebis_region_plausible(tiny, kW, kH), "region: under 5% of the texture on an axis is implausible");

    // A pass's view in it (the 10:52 run: textures 646x540 for 1614x1350 targets; focus passes at 1514x1266).
    float full540[4];
    uv_scale(1, 646, 540, full540);
    const simqvfr::ViewRegion whole540 = lmu::yebis_region(646, 540, 646, 540, full540);
    r = lmu::yebis_pass_region(whole540, 1514.0 / 1614, 1266.0 / 1350);
    check(r.width == 606 && r.height == 506, "pass region: a focus pass at 1514x1266 of 1614x1350 is 606x506 of 646x540");
    r = lmu::yebis_pass_region(whole540, 1, 1);
    check(r.width == 646 && r.height == 540, "pass region: a pass filling its target is the whole region");
    r = lmu::yebis_pass_region(whole540, 0, 0);
    check(r.width == 0, "pass region: a share not known yet is no region");
    r = lmu::yebis_pass_region(whole540, 1.2, 1);
    check(r.width == 646, "pass region: a share over 1 stops at the region");
}

void share_checks(ID3D11Device* device, ID3D11DeviceContext* context) {
    // An eye like the cropped one of the 2026-11-08 runs (wide, short), a 25% x 64% box off-centre on the gaze.
    const double l = -1.402, r = 1.213, d = -0.437, u = 0.415;
    float eye[16], focus[16];
    lmu_projection(l, r, d, u, eye);
    double cx = 0.2, cy = 0.05;
    lmu::clamp_focus_centre(eye, 0.25, 0.64, cx, cy);
    lmu::FocusBox box;
    check(lmu::focus_projection(eye, 0.25, 0.64, cx, cy, box, focus), "the focus box is made");
    const simqvfr::ViewMapping m = lmu::glare_mapping(box);
    check(m.focusTan[0] == box.focusTan[0] && m.focusTan[1] == box.focusTan[1] && m.focusTan[2] == box.focusTan[3] &&
              m.focusTan[3] == box.focusTan[2] && m.eyeTan[2] == box.eyeTan[3] && m.eyeTan[3] == box.eyeTan[2] &&
              m.rotation[0] == 1 && m.rotation[4] == 1 && m.rotation[8] == 1 && m.rotation[1] == 0,
          "mapping: tangents reordered to left, right, up, down; no turn");

    // As LMU runs: YEBIS uses its whole glare texture (UV extent 1) and each pass's view is the share of its target the
    // pass rendered (live resolution): the eye at 0.8 here, the focus view at 0.938 x 0.9378 as in the 10:52 run. A halo
    // (a Gaussian, sigma 5 eye texels) at a direction inside the box, near its left edge.
    float full[4];
    uv_scale(1, kW, kH, full);
    const simqvfr::ViewRegion whole = lmu::yebis_region(kW, kH, kW, kH, full);
    const simqvfr::ViewRegion er = lmu::yebis_pass_region(whole, 0.8, 0.8);
    const simqvfr::ViewRegion fr = lmu::yebis_pass_region(whole, 1514.0 / 1614, 1266.0 / 1350);
    check(er.width == 517 && er.height == 405 && fr.width == 606 && fr.height == 475,
          "regions: the eye's at 0.8 (517x405), the focus view's at 0.938 x 0.9378 (606x475)");
    const double tx = box.focusTan[0] + 0.15 * (box.focusTan[1] - box.focusTan[0]);
    const double ty = box.focusTan[2] + 0.6 * (box.focusTan[3] - box.focusTan[2]);
    const double ex = (tx - l) / (r - l) * er.width, ey = (u - ty) / (u - d) * er.height;  // eye texels (rows down)
    const double sigma = 5;
    std::vector<std::uint32_t> eyeTexels(kW * kH, pack(0.5f, 0.5f, 0.5f));  // beyond its region: must never be read
    for (int y = 0; y < er.height; ++y)
        for (int x = 0; x < er.width; ++x) {
            const double dx = x + 0.5 - ex, dy = y + 0.5 - ey;
            const float v = static_cast<float>(kFloor + 2 * std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma)));
            eyeTexels[y * kW + x] = pack(v, v * 0.8f, v * 0.2f);
        }
    // YEBIS's glare texture as the focus pass left it: its own (narrow) glare, here a flat marker, in a target the game
    // has bound with its own viewport.
    const std::uint32_t marker = pack(0.25f, 0.25f, 0.25f);
    std::vector<std::uint32_t> focusTexels(kW * kH, marker);
    auto eyeGlare = glare_texture(device, eyeTexels, 0);
    auto yebis = glare_texture(device, focusTexels, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    D3D11_TEXTURE2D_DESC gd{64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> gameTarget;
    ComPtr<ID3D11RenderTargetView> gameRtv;
    device->CreateTexture2D(&gd, nullptr, &gameTarget);
    device->CreateRenderTargetView(gameTarget.Get(), nullptr, &gameRtv);
    context->OMSetRenderTargets(1, gameRtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT gameVp{3, 4, 50, 40, 0, 1};
    context->RSSetViewports(1, &gameVp);

    simqvfr::D3D11ViewResample resample;
    check(resample.initialize(device), "the resample starts on WARP");
    // As glare_share keeps it: the eye's region copied to the top-left of a texture of its own.
    const bool ran = resample.resample_target(eyeGlare.Get(), er, yebis.Get(), fr, m);
    check(ran, "the eye's glare is resampled into the focus view's (R11G11B10_FLOAT render target)");

    ComPtr<ID3D11RenderTargetView> boundRtv;
    context->OMGetRenderTargets(1, &boundRtv, nullptr);
    UINT nvp = 1;
    D3D11_VIEWPORT vp{};
    context->RSGetViewports(&nvp, &vp);
    check(boundRtv.Get() == gameRtv.Get() && vp.TopLeftX == 3 && vp.Width == 50 && vp.Height == 40,
          "the game's bound target and viewport are as they were");

    const auto out = read_back(device, context, yebis.Get());
    // The halo's centre and width in focus texels (luminance-weighted moments over the focus region, minus the floor).
    double sw = 0, sx = 0, sy = 0, sxx = 0, syy = 0;
    for (int y = 0; y < fr.height; ++y)
        for (int x = 0; x < fr.width; ++x) {
            const double w = (std::max)(0.0, static_cast<double>(red(out[y * kW + x])) - kFloor);
            sw += w;
            sx += w * (x + 0.5);
            sy += w * (y + 0.5);
            sxx += w * (x + 0.5) * (x + 0.5);
            syy += w * (y + 0.5) * (y + 0.5);
        }
    const double mx = sx / sw, my = sy / sw;
    const double vx = std::sqrt(sxx / sw - mx * mx), vy = std::sqrt(syy / sw - my * my);
    // Expected: the same direction in the focus view, the eye's sigma times the focus magnification per axis.
    const double wantX = (tx - box.focusTan[0]) / (box.focusTan[1] - box.focusTan[0]) * fr.width;
    const double wantY = (box.focusTan[3] - ty) / (box.focusTan[3] - box.focusTan[2]) * fr.height;
    const double magX = (static_cast<double>(fr.width) / (box.focusTan[1] - box.focusTan[0])) / (er.width / (r - l));
    const double magY = (static_cast<double>(fr.height) / (box.focusTan[3] - box.focusTan[2])) / (er.height / (u - d));
    std::printf("     halo at %.2f, %.2f (want %.2f, %.2f); sigma %.2f x %.2f focus texels (want %.2f x %.2f: "
                "magnification %.3f x %.3f)\n",
                mx, my, wantX, wantY, vx, vy, sigma * magX, sigma * magY, magX, magY);
    check(std::fabs(mx - wantX) < 1.0 && std::fabs(my - wantY) < 1.0, "the halo lands at its direction in the focus view");
    check(std::fabs(vx / (sigma * magX) - 1) < 0.03 && std::fabs(vy / (sigma * magY) - 1) < 0.03,
          "the halo has the eye's angular size (the eye's sigma times the magnification, per axis, within 3%)");

    // No seam: the focus region's left column is the eye's glare at the box's left edge (the eye texel column there,
    // interpolated), row by row through the halo.
    int seamBad = 0;
    for (int y = 0; y < fr.height; y += 7) {
        const double t = box.focusTan[3] - (y + 0.5) / fr.height * (box.focusTan[3] - box.focusTan[2]);
        const double px = (box.focusTan[0] + 0.5 / fr.width * (box.focusTan[1] - box.focusTan[0]) - l) / (r - l) * er.width;
        const double py = (u - t) / (u - d) * er.height;
        const double dx = px - ex, dy = py - ey;
        const double want = kFloor + 2 * std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
        const double have = red(out[y * kW]);
        if (std::fabs(have - want) > 0.03 * want + 0.004)
            ++seamBad;
    }
    check(seamBad == 0, "the focus region's edge column holds the eye's glare at the box edge");

    bool beyondKept = true;
    const simqvfr::ViewRegion inner = lmu::yebis_pass_region(whole, 0.5, 0.5);  // into a smaller focus region
    std::vector<std::uint32_t> again(kW * kH, marker);
    auto yebis2 = glare_texture(device, again, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    resample.resample_target(eyeGlare.Get(), er, yebis2.Get(), inner, m);
    const auto out2 = read_back(device, context, yebis2.Get());
    for (UINT y = 0; y < kH && beyondKept; ++y)
        for (UINT x = 0; x < kW; ++x)
            if ((static_cast<int>(x) >= inner.width || static_cast<int>(y) >= inner.height) && out2[y * kW + x] != marker) {
                beyondKept = false;
                break;
            }
    check(beyondKept, "texels beyond the focus pass's region are untouched");
}

void read_checks(ID3D11Device* device) {
    std::vector<std::uint32_t> texels(kW * kH, 0);
    auto tex = glare_texture(device, texels, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    // Fake YEBIS objects at abi.h's offsets: CRenderGlare -> CRenderTexture -> IGPUTexture -> the D3D11 resource.
    alignas(16) std::uint8_t gpu[0x100] = {}, surface[0x100] = {}, glare[0x400] = {};
    ID3D11Resource* resource = tex.Get();
    std::memcpy(gpu + lmu::kGpuTextureResource, &resource, sizeof(resource));
    const unsigned size[2] = {kW, kH};
    float scale[4];
    uv_scale(0.5f, kW, kH, scale);
    std::memcpy(surface + lmu::kRenderTextureSize, size, sizeof(size));
    std::memcpy(surface + lmu::kRenderTextureScale, scale, sizeof(scale));
    const void* gpuPtr = gpu;
    std::memcpy(surface + lmu::kRenderTextureGpu, &gpuPtr, sizeof(gpuPtr));
    const void* surfacePtr = surface;
    std::memcpy(glare + lmu::kGlareResult, &surfacePtr, sizeof(surfacePtr));

    lmu::YebisGlare g;
    int rc = lmu::read_yebis_glare(glare, g);
    check(rc == 0 && g.texture == tex.Get() && g.width == kW && g.height == kH && g.textureWidth == kW &&
              g.textureHeight == kH && g.scaleOffset[0] == scale[0],
          "read: the glare result's texture and its size, the render texture's size and UV scale");
    if (g.texture)
        g.texture->Release();

    const unsigned smaller[2] = {kW / 2, kH};
    std::memcpy(surface + lmu::kRenderTextureSize, smaller, sizeof(smaller));
    rc = lmu::read_yebis_glare(glare, g);
    check(rc == 0 && g.width == kW / 2 && g.textureWidth == kW, "read: a render texture smaller than its texture is read");
    if (g.texture)
        g.texture->Release();
    const unsigned bigger[2] = {kW * 2, kH};
    std::memcpy(surface + lmu::kRenderTextureSize, bigger, sizeof(bigger));
    rc = lmu::read_yebis_glare(glare, g);
    check(rc == 3 && !g.texture, "read: a render texture bigger than the texture is refused");
    std::memcpy(surface + lmu::kRenderTextureSize, size, sizeof(size));

    const void* null = nullptr;
    std::memcpy(glare + lmu::kGlareResult, &null, sizeof(null));
    check(lmu::read_yebis_glare(glare, g) == 1, "read: no result surface (glare off) is refused");

    const void* bad = reinterpret_cast<const void*>(0x10);
    std::memcpy(glare + lmu::kGlareResult, &bad, sizeof(bad));
    check(lmu::read_yebis_glare(glare, g) == 2 && !g.texture, "read: unreadable memory on the way is caught, not a crash");
}

}  // namespace

int main() {
    region_checks();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::puts("no WARP D3D11 device");
        return 1;
    }
    share_checks(device.Get(), context.Get());
    read_checks(device.Get());
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

// Offline check of the compositor's sharpening (compose_shader.h's cas_sample, AMD's CAS) on a WARP device: flat
// views stay exactly as they are, the taps never reach a neighbouring view in the image, a view copied texel for texel
// (draw_view_copies) is CAS's sharpening filter to within a step of 255 of a CPU reference, and the periphery's upscale
// comes out steeper at an edge than bilinear's while away from the edge it is unchanged.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "quad/compose_shader.h"
#include "quad/d3d11_compositor.h"

using Microsoft::WRL::ComPtr;
using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// The views' image, as d3d11_compositor_test's: two eye columns of 128, outer views 128 high on top, focus views 64
// high below.
constexpr UINT kW = 256, kH = 192;

uint32_t grey(int v) {
    const uint32_t g = static_cast<uint32_t>(std::clamp(v, 0, 255));
    return 0xff000000u | (g << 16) | (g << 8) | g;
}

struct Image {
    std::vector<uint32_t> px;
    UINT width = 0, height = 0;
    uint32_t at(UINT x, UINT y) const { return px[y * width + x]; }
    int channel(UINT x, UINT y, int c) const { return static_cast<int>((at(x, y) >> (8 * c)) & 0xff); }
};

Image read_image(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Image image;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return image;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return image;
    image.width = desc.Width;
    image.height = desc.Height;
    image.px.resize(static_cast<size_t>(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(&image.px[static_cast<size_t>(y) * desc.Width],
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    return image;
}

ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, UINT w, UINT h, const std::vector<uint32_t>* pixels) {
    D3D11_TEXTURE2D_DESC desc{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    D3D11_SUBRESOURCE_DATA init{pixels ? pixels->data() : nullptr, w * 4};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&desc, pixels ? &init : nullptr, &t);
    return t;
}

// Each eye: the whole view's tangents +-1, a centred 40% focus box, its views where d3d11_compositor_test has them.
void eyes_for(EyeComposition (&eyes)[2], float sharpness) {
    for (int e = 0; e < 2; ++e) {
        eyes[e] = {};
        const float outer[4] = {-1, 1, 1, -1}, focus[4] = {-0.4f, 0.4f, 0.4f, -0.4f};
        std::memcpy(eyes[e].outerTan, outer, sizeof(outer));
        std::memcpy(eyes[e].focusTan, focus, sizeof(focus));
        eyes[e].rotation[0] = eyes[e].rotation[4] = eyes[e].rotation[8] = 1;
        const float outerSrc[4] = {e * 0.5f, 0, 0.5f, 128.0f / kH}, focusSrc[4] = {e * 0.5f, 128.0f / kH, 0.5f, 64.0f / kH};
        std::memcpy(eyes[e].outerSource, outerSrc, sizeof(outerSrc));
        std::memcpy(eyes[e].focusSource, focusSrc, sizeof(focusSrc));
        eyes[e].feather = 0.1f;
        eyes[e].sharpness = sharpness;
    }
}

// CAS's sharpening at texel (x, y) of `v` (one to one: its cross of neighbours, clamped to the region x0..x1, y0..y1),
// as compose_shader.h's cas_sample gives it on a texel centre; `sharpness` 0..1. One channel, 0..255.
int cas_reference(const std::vector<uint32_t>& v, int x, int y, int x0, int y0, int x1, int y1, int c, float sharpness) {
    const auto at = [&](int px, int py) {
        px = std::clamp(px, x0, x1);
        py = std::clamp(py, y0, y1);
        return static_cast<float>((v[static_cast<size_t>(py) * kW + px] >> (8 * c)) & 0xff) / 255.0f;
    };
    const float b = at(x, y - 1), e = at(x - 1, y), f = at(x, y), g = at(x + 1, y), j = at(x, y + 1);
    const float mn = std::min({b, e, f, g, j}), mx = std::max({b, e, f, g, j});
    const float amp = std::sqrt(std::clamp(std::min(mn, 1 - mx) / std::max(mx, 1e-5f), 0.0f, 1.0f));
    const float w = -0.2f * sharpness * amp;
    const float out = std::clamp((f + w * (b + e + g + j)) / (1 + 4 * w), 0.0f, 1.0f);
    return static_cast<int>(std::lround(out * 255));
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::puts("no WARP D3D11 device");
        return 2;
    }
    D3D11Compositor compositor;
    check(compositor.initialize(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM), "the compositor starts on WARP");

    {  // The constants: rot2.w (19) the sharpness, clamped to 0..1.
        EyeComposition c{};
        float constants[kComposeConstants] = {};
        c.sharpness = 0.4f;
        compose_constants(c, 0, constants);
        const bool inRange = constants[19] == 0.4f;
        c.sharpness = 3;
        compose_constants(c, 0, constants);
        check(inRange && constants[19] == 1.0f, "constants: the sharpness in rot2.w, at most 1");
    }

    // 1. Flat mid greys, each view its own (so a tap from the wrong view would show): composed at full sharpening
    // (outer 128 -> 96 texels across, the focus view's 128 texels into a ~38-pixel box: its four-tap branch), every
    // output pixel is its view's grey exactly.
    {
        const int outerGrey[2] = {128, 90}, focusGrey[2] = {60, 170};
        std::vector<uint32_t> pixels(kW * kH);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x)
                pixels[y * kW + x] = grey(y < 128 ? outerGrey[x / 128] : focusGrey[x / 128]);
        ComPtr<ID3D11Texture2D> views = texture(device.Get(), kW, kH, &pixels), output = texture(device.Get(), 192, 96, nullptr);
        EyeComposition eyes[2];
        eyes_for(eyes, 1.0f);
        check(compositor.compose(views.Get(), output.Get(), eyes), "flat: compose, sharpening 100%");
        const Image out = read_image(device.Get(), context.Get(), output.Get());
        for (int e = 0; e < 2; ++e) {
            const UINT x0 = e * 96;
            bool edges = true;  // the outer view along every side of the eye, the rows next to the focus views included
            for (UINT i = 0; i < 96; ++i)
                edges &= out.at(x0, i) == grey(outerGrey[e]) && out.at(x0 + 95, i) == grey(outerGrey[e]) &&
                         out.at(x0 + i, 0) == grey(outerGrey[e]) && out.at(x0 + i, 95) == grey(outerGrey[e]);
            char what[128];
            std::snprintf(what, sizeof(what), "flat, eye %d: the outer view's grey on every edge of the eye (0x%08x)", e,
                          out.at(x0 + 95, 95));
            check(edges, what);
            bool middle = true;
            for (UINT y = 40; y < 56; ++y)
                for (UINT x = x0 + 40; x < x0 + 56; ++x)
                    middle &= out.at(x, y) == grey(focusGrey[e]);
            std::snprintf(what, sizeof(what), "flat, eye %d: the focus view's grey in the box (0x%08x)", e, out.at(x0 + 48, 48));
            check(middle, what);
        }
    }

    // 2. Texel for texel (draw_view_copies, the periphery's copy when the runtime composes): a pattern of mid-tone
    // values, each channel its own, against the CPU's CAS within a step of 255 (the GPU's rounding); 0% is a plain copy.
    {
        std::vector<uint32_t> pixels(kW * kH);
        uint32_t seed = 12345;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return 30 + static_cast<int>((seed >> 24) % 196);  // 30..225
        };
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                // Blocks of 4 with noise on top: edges of every contrast, and flat-ish stretches.
                const int base = ((x / 4 + y / 4) % 3) * 70 + 40;
                const int r = std::clamp(base + next() / 8 - 12, 0, 255), g = std::clamp(255 - base + next() / 8 - 12, 0, 255),
                          b = next();
                pixels[y * kW + x] = 0xff000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
                                     static_cast<uint32_t>(r);
            }
        ComPtr<ID3D11Texture2D> views = texture(device.Get(), kW, kH, &pixels), target = texture(device.Get(), 256, 128, nullptr);
        const float sources[2][4] = {{0, 0, 0.5f, 128.0f / kH}, {0.5f, 0, 0.5f, 128.0f / kH}};
        const int rects[2][4] = {{0, 0, 128, 128}, {128, 0, 128, 128}};
        for (float sharpness : {0.0f, 0.5f, 1.0f}) {
            check(compositor.draw_view_copies(views.Get(), target.Get(), sources, rects, sharpness), "copies: drawn");
            const Image out = read_image(device.Get(), context.Get(), target.Get());
            int worst = 0, changed = 0;
            UINT worstX = 0, worstY = 0;
            for (UINT y = 0; y < 128; ++y)
                for (UINT x = 0; x < 256; ++x) {
                    const int x0 = x < 128 ? 0 : 128;
                    for (int c = 0; c < 3; ++c) {
                        const int want = sharpness > 0
                                             ? cas_reference(pixels, static_cast<int>(x), static_cast<int>(y), x0, 0, x0 + 127, 127, c, sharpness)
                                             : static_cast<int>((pixels[y * kW + x] >> (8 * c)) & 0xff);
                        const int d = std::abs(out.channel(x, y, c) - want);
                        changed += out.channel(x, y, c) != static_cast<int>((pixels[y * kW + x] >> (8 * c)) & 0xff);
                        if (d > worst) {
                            worst = d;
                            worstX = x;
                            worstY = y;
                        }
                    }
                }
            char what[160];
            std::snprintf(what, sizeof(what), "copies at %.0f%%: CAS's filter within one step (worst %d at %u,%u; %d values "
                          "changed)", sharpness * 100, worst, worstX, worstY, changed);
            check(worst <= 1 && (sharpness > 0 ? changed > 10000 : changed == 0), what);
        }
    }

    // 3. The periphery upscaled 2x (128 texels into 256 pixels per eye) across a soft edge at a quarter of the view,
    // outside the focus box: sharpened, the transition is steeper and spreads apart (darker below the edge, lighter
    // above it); away from it the image is the same as bilinear's.
    {
        std::vector<uint32_t> pixels(kW * kH);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const int ex = static_cast<int>(x % 128);  // a ramp over 3 texels from 64 to 192 at x = 31..33
                const int v = ex <= 30 ? 64 : ex >= 34 ? 192 : 64 + (ex - 30) * 32;
                pixels[y * kW + x] = grey(y < 128 ? v : 128);
            }
        ComPtr<ID3D11Texture2D> views = texture(device.Get(), kW, kH, &pixels), output = texture(device.Get(), 512, 256, nullptr);
        EyeComposition eyes[2];
        eyes_for(eyes, 0.0f);
        check(compositor.compose(views.Get(), output.Get(), eyes), "edge: compose, bilinear");
        const Image plain = read_image(device.Get(), context.Get(), output.Get());
        eyes_for(eyes, 1.0f);
        check(compositor.compose(views.Get(), output.Get(), eyes), "edge: compose, sharpening 100%");
        const Image sharp = read_image(device.Get(), context.Get(), output.Get());
        const UINT y = 20;  // above the focus box (it spans rows 77-179)
        int plainSlope = 0, sharpSlope = 0, below = 0, above = 0;
        bool sameAway = true;
        for (UINT x = 1; x < 256; ++x) {
            plainSlope = std::max(plainSlope, plain.channel(x, y, 0) - plain.channel(x - 1, y, 0));
            sharpSlope = std::max(sharpSlope, sharp.channel(x, y, 0) - sharp.channel(x - 1, y, 0));
            if (x >= 50 && x < 64)  // just below the ramp (output 62..68)
                below += plain.channel(x, y, 0) - sharp.channel(x, y, 0);
            if (x > 68 && x <= 80)
                above += sharp.channel(x, y, 0) - plain.channel(x, y, 0);
            if ((x < 40 || (x > 90 && x < 150)) && sharp.at(x, y) != plain.at(x, y))
                sameAway = false;
        }
        char what[160];
        std::snprintf(what, sizeof(what), "edge: steeper sharpened (largest step %d against bilinear's %d)", sharpSlope,
                      plainSlope);
        check(sharpSlope > plainSlope, what);
        std::snprintf(what, sizeof(what), "edge: darker below it (%d in all), lighter above it (%d in all)", below, above);
        check(below > 0 && above > 0, what);
        check(sameAway, "edge: away from it, the same as bilinear's");
    }

    // 4. A supersampled focus view, two texels per output pixel each way (128 x 64 texels into a 64 x 32 box: eyes of
    // 160 x 80 with the 40% box): each pixel is its 2 x 2 texels' average, sharpened against the neighbouring pixels'
    // averages (CAS at the output's pixel scale), to within a step of 255 of a CPU reference; off, the plain average.
    std::vector<uint32_t> pattern(kW * kH);
    {
        uint32_t seed = 777;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<int>((seed >> 24) % 160) + 48;  // 48..207
        };
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const int v = ((x / 3 + y / 2) % 2) * 60 + next() / 2;  // stripes over noise
                pattern[y * kW + x] = grey(v) & 0xffffff00u | static_cast<uint32_t>(std::clamp(v + 20, 0, 255));
            }
    }
    const auto texel = [&](int x, int y, int c) {  // eye 0's focus view: columns 0-127, rows 128-191
        x = std::clamp(x, 0, 127);
        y = std::clamp(y, 0, 63);
        return static_cast<float>((pattern[static_cast<size_t>(128 + y) * kW + x] >> (8 * c)) & 0xff) / 255.0f;
    };
    const auto block = [&](int bx, int by, int c) {  // the average of output pixel (bx, by)'s 2 x 2 texels
        return 0.25f * (texel(2 * bx, 2 * by, c) + texel(2 * bx + 1, 2 * by, c) + texel(2 * bx, 2 * by + 1, c) +
                        texel(2 * bx + 1, 2 * by + 1, c));
    };
    const auto cas_of = [](float e, float b, float d, float f, float h, float sharpness) {
        const float mn = std::min({b, d, e, f, h}), mx = std::max({b, d, e, f, h});
        const float w = -0.2f * sharpness * std::sqrt(std::clamp(std::min(mn, 1 - mx) / std::max(mx, 1e-5f), 0.0f, 1.0f));
        return std::clamp((e + w * (b + d + f + h)) / (1 + 4 * w), 0.0f, 1.0f);
    };
    {
        ComPtr<ID3D11Texture2D> views = texture(device.Get(), kW, kH, &pattern), output = texture(device.Get(), 320, 80, nullptr);
        for (float sharpness : {0.0f, 1.0f}) {
            EyeComposition eyes[2];
            eyes_for(eyes, sharpness);
            for (EyeComposition& e : eyes)
                e.feather = 0.001f;
            check(compositor.compose(views.Get(), output.Get(), eyes), "supersampled: compose");
            const Image out = read_image(device.Get(), context.Get(), output.Get());
            int worst = 0, changed = 0;
            for (int by = 1; by < 31; ++by)  // the box's pixels 48..111 x 24..55, one in from its edge
                for (int bx = 1; bx < 63; ++bx)
                    for (int c = 0; c < 3; ++c) {
                        const float e = block(bx, by, c);
                        const float want = sharpness > 0 ? cas_of(e, block(bx, by - 1, c), block(bx - 1, by, c),
                                                                   block(bx + 1, by, c), block(bx, by + 1, c), sharpness)
                                                         : e;
                        const int got = out.channel(48 + bx, 24 + by, c);
                        worst = std::max(worst, std::abs(got - static_cast<int>(std::lround(want * 255))));
                        changed += std::abs(got - static_cast<int>(std::lround(e * 255))) > 1;
                    }
            char what[160];
            std::snprintf(what, sizeof(what), "supersampled at %.0f%%: each pixel CAS of the 2x2 averages within one step "
                          "(worst %d; %d values off the plain average)", sharpness * 100, worst, changed);
            check(worst <= 1 && (sharpness > 0 ? changed > 2000 : changed == 0), what);
        }
    }

    // 5. The focus layer (the runtime composing), shown at two texels per display pixel (viewTan.z 2): its sharpening is
    // at the display's scale, so the runtime's 2 x 2 average keeps most of CAS's change on those averages; CAS on the
    // texels themselves (viewTan.z 0) keeps much less.
    {
        ComPtr<ID3D11Texture2D> views = texture(device.Get(), kW, kH, &pattern), layer = texture(device.Get(), 256, 64, nullptr);
        const int rects[2][4] = {{0, 0, 128, 64}, {128, 0, 128, 64}};
        double wanted = 0;
        for (int by = 2; by < 30; ++by)
            for (int bx = 2; bx < 62; ++bx)
                for (int c = 0; c < 3; ++c) {
                    const float e = block(bx, by, c);
                    wanted += std::abs(cas_of(e, block(bx, by - 1, c), block(bx - 1, by, c), block(bx + 1, by, c),
                                              block(bx, by + 1, c), 1.0f) - e);
                }
        double kept[2] = {};
        for (int scale = 0; scale < 2; ++scale) {
            EyeComposition eyes[2];
            eyes_for(eyes, 1.0f);
            for (EyeComposition& e : eyes) {
                e.feather = 0.001f;
                e.viewTan[2] = scale ? 2.0f : 0.0f;
            }
            check(compositor.draw_focus_layer(views.Get(), layer.Get(), eyes, rects), "focus layer: drawn, sharpened");
            const Image out = read_image(device.Get(), context.Get(), layer.Get());
            for (int by = 2; by < 30; ++by)
                for (int bx = 2; bx < 62; ++bx)
                    for (int c = 0; c < 3; ++c) {
                        const float avg = 0.25f / 255.0f * static_cast<float>(out.channel(2 * bx, 2 * by, c) + out.channel(2 * bx + 1, 2 * by, c) +
                                                                       out.channel(2 * bx, 2 * by + 1, c) + out.channel(2 * bx + 1, 2 * by + 1, c));
                        kept[scale] += std::abs(avg - block(bx, by, c));
                    }
        }
        char what[192];
        std::snprintf(what, sizeof(what), "focus layer at 2 texels a pixel: the 2x2 averages keep %.0f%% of CAS's change "
                      "on them (sharpened at the display's scale), %.0f%% sharpened at the texels' own", 100 * kept[1] / wanted,
                      100 * kept[0] / wanted);
        check(kept[1] / wanted > 0.6 && kept[1] > 1.3 * kept[0], what);
    }

    compositor.shutdown();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

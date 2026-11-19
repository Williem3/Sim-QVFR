// Offline check of colour correction (quad/color_grade.h, compose_shader.h color_grade): its settings round-trip
// through the ini signed and clamped, the live word and the shader's constants, the headset menu's Color page, and on a
// WARP device the composed eyes (periphery and focus alike) and native quad views' graded periphery copy (GradePS)
// against a CPU reference; off, or on with every value 0, the image is exactly as it was.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "quad/color_grade.h"
#include "quad/compose_shader.h"
#include "quad/d3d11_compositor.h"
#include "quad/settings_menu.h"
#include "test_util.h"

using Microsoft::WRL::ComPtr;
using namespace simqvfr;

namespace {

constexpr UINT kW = 256, kH = 192;  // the views' image, as cas_sharpen_test's: outer views 128 high, focus views 64 below

uint32_t rgb(int r, int g, int b) {
    const auto c = [](int v) { return static_cast<uint32_t>(std::clamp(v, 0, 255)); };
    return 0xff000000u | (c(b) << 16) | (c(g) << 8) | c(r);
}
int channel(uint32_t p, int c) { return static_cast<int>((p >> (8 * c)) & 0xff); }

std::vector<uint32_t> read_image(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint32_t> px;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return px;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return px;
    px.resize(static_cast<size_t>(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(&px[static_cast<size_t>(y) * desc.Width],
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    return px;
}

ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, UINT w, UINT h, const std::vector<uint32_t>* pixels) {
    D3D11_TEXTURE2D_DESC desc{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    D3D11_SUBRESOURCE_DATA init{pixels ? pixels->data() : nullptr, w * 4};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&desc, pixels ? &init : nullptr, &t);
    return t;
}

void eyes_for(EyeComposition (&eyes)[2], const ColorGrade& grade) {
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
        color_grade_constants(grade, eyes[e].color);
    }
}

// compose_shader.h's color_grade on the CPU, one pixel (0..255 each channel, as the UNORM image reads it).
uint32_t grade_reference(uint32_t p, const ColorGrade& g) {
    float k[3];
    color_grade_constants(g, k);
    if (k[0] <= 0)
        return p;
    const double sat1000 = std::floor(k[2] / kColorPackShift);
    const double contrast = (k[2] - sat1000 * kColorPackShift) / 1000.0, saturation = sat1000 / 1000.0;
    double d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = std::pow(std::pow(channel(p, i) / 255.0 * k[0], 1 / 2.2), k[1]);
    const double l = std::clamp(0.2126 * d[0] + 0.7152 * d[1] + 0.0722 * d[2], 0.0, 1.0);
    const double s = l < 0.5 ? 0.5 * std::pow(2 * l, contrast) : 1 - 0.5 * std::pow(2 - 2 * l, contrast);
    double c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = std::pow(std::max(d[i] + (s - l), 0.0), 2.2);
    const double luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    int o[3];
    for (int i = 0; i < 3; ++i)
        o[i] = static_cast<int>(std::lround(std::clamp(luma + (c[i] - luma) * saturation, 0.0, 1.0) * 255));
    return rgb(o[0], o[1], o[2]);
}

int worst_channel(uint32_t a, uint32_t b) {
    int worst = 0;
    for (int c = 0; c < 3; ++c)
        worst = std::max(worst, std::abs(channel(a, c) - channel(b, c)));
    return worst;
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    // ---- Settings ---------------------------------------------------------------------------------------------
    {
        const ColorGrade none;
        check(!none.on && !none.active(), "defaults: off, nothing changed");
        ColorGrade g{true, -7, 12, -30, 45};
        check(color_grade_from_bits(color_grade_bits(g)) == g && color_grade_from_bits(kColorGradeNone) == none &&
                  color_grade_bits(none) == kColorGradeNone,
              "the live word round-trips, negative values included");
        float off[3] = {1, 1, 1};
        color_grade_constants(ColorGrade{true, 0, 0, 0, 0}, off);
        check(off[0] == 0 && off[1] == 0 && off[2] == 0, "on with every value 0: the shader's constants say off");
        float on[3];
        color_grade_constants(ColorGrade{true, 10, 20, -50, -100}, on);
        check(close_to(on[0], 2.0, 1e-6) && close_to(on[1], std::exp2(-0.3), 1e-6) && on[2] == 500.0f,
              "constants: +1 EV doubles, brightness a power, contrast 0.5 and saturation 0 packed as a whole number");
        color_grade_constants(ColorGrade{true, -30, -50, 50, 100}, on);
        check(on[2] == 1500.0f + kColorPackShift * 2000.0f && on[2] == std::floor(on[2]) && on[2] < 16777216.0f,
              "constants: the largest pack is exact in a float");
        check(grade_reference(rgb(0, 0, 0), ColorGrade{true, 0, 50, 0, 0}) == rgb(0, 0, 0) &&
                  grade_reference(rgb(255, 255, 255), ColorGrade{true, 0, 50, 0, 0}) == rgb(255, 255, 255) &&
                  grade_reference(rgb(0, 0, 0), ColorGrade{true, 0, 0, -50, 0}) == rgb(0, 0, 0) &&
                  grade_reference(rgb(255, 255, 255), ColorGrade{true, 0, 0, 50, 0}) == rgb(255, 255, 255) &&
                  channel(grade_reference(rgb(64, 64, 64), ColorGrade{true, 0, 30, 0, 0}), 0) > 64,
              "curves: brightness and contrast keep black and white, brightness lifts a dark grey");
        check(color_exposure_text(5) == L"+0.5 EV" && color_exposure_text(-12) == L"-1.2 EV" &&
                  color_exposure_text(0) == L"0 EV" && color_percent_text(-30) == L"-30%" &&
                  color_percent_text(8) == L"+8%",
              "the values' texts");

        const std::wstring ini = temp_path(L"sim-qvfr-color-grade-test.ini");
        DeleteFileW(ini.c_str());
        check(read_color_grade(ini.c_str()) == none, "a missing key reads as off and 0");
        check(write_color_grade(ini.c_str(), g) && read_color_grade(ini.c_str()) == g,
              "written and read back signed ([quad] color_exposure=-7 etc.)");
        check(ini_signed(ini.c_str(), L"quad", L"color_contrast", 0) == -30,
              "negative values are written as such");
        WritePrivateProfileStringW(L"quad", L"color_saturation", L"-500", ini.c_str());
        WritePrivateProfileStringW(L"quad", L"color_exposure", L"99", ini.c_str());
        const ColorGrade beyond = read_color_grade(ini.c_str());
        check(beyond.saturation == kColorSaturation.min && beyond.exposure == kColorExposure.max, "out of range: clamped");
        DeleteFileW(ini.c_str());
    }

    // ---- The headset menu's Color page -------------------------------------------------------------------------
    {
        SettingsMenu m;
        m.launch.qvfr = true;
        m.launch.sharpening = false;
        bool shown = false;
        for (const MenuPage p : menu_pages(m))
            shown |= p == MenuPage::Color;
        check(!shown, "menu: no Color page for a game without the compositor's features");
        m.launch.sharpening = true;
        const std::vector<MenuPage> pages = menu_pages(m);
        check(pages.size() > 1 && pages[1] == MenuPage::Color, "menu: the Color page after Picture");
        m.page = 1;
        std::vector<MenuItem> items = menu_page_items(m);
        check(items.size() == 3 && items[1] == MenuItem::HeadColor && items[2] == MenuItem::ColorOn,
              "menu: off, only the switch");
        m.selected = 2;
        check(menu_key(m, MenuKey::Right) && m.values.color.on, "menu: Right turns it on");
        items = menu_page_items(m);
        check(items.size() == 7 && items[3] == MenuItem::ColorExposure && items[6] == MenuItem::ColorSaturation,
              "menu: on, its four values under it");
        m.selected = 3;
        check(menu_key(m, MenuKey::Left) && m.values.color.exposure == -1 &&
                  menu_lines(m)[3].value == L"-0.1 EV",
              "menu: exposure steps a tenth of a stop, shown in EV");
        m.selected = 6;
        for (int i = 0; i < 30; ++i)
            menu_key(m, MenuKey::Left);
        check(m.values.color.saturation == kColorSaturation.min && menu_lines(m, false)[6].value == L"-100%",
              "menu: saturation stops at -100%");
        const std::wstring ini = temp_path(L"sim-qvfr-color-grade-menu-test.ini");
        DeleteFileW(ini.c_str());
        write_menu_values(ini.c_str(), m.values, true);
        check(GetPrivateProfileIntW(L"quad", L"color_correction", -1, ini.c_str()) == -1,
              "menu: not written for a game without it");
        write_menu_values(ini.c_str(), m.values, true, false, false, true);
        check(read_menu_values(ini.c_str()).color == m.values.color, "menu: written for a game with it, and read back");
        DeleteFileW(ini.c_str());
    }

    // ---- Rendering (WARP) -------------------------------------------------------------------------------------
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        std::puts("no WARP D3D11 device");
        return 2;
    }
    D3D11Compositor compositor;
    check(compositor.initialize(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM), "the compositor starts on WARP");

    // Flat colours, each view its own: every composed pixel is its view's colour graded, the periphery and the focus
    // area alike; with it off, exactly as it was.
    const uint32_t outerColor[2] = {rgb(150, 90, 40), rgb(60, 120, 200)}, focusColor[2] = {rgb(200, 30, 90), rgb(20, 180, 110)};
    std::vector<uint32_t> pixels(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            pixels[y * kW + x] = y < 128 ? outerColor[x / 128] : focusColor[x / 128];
    ComPtr<ID3D11Texture2D> views = texture(device.Get(), kW, kH, &pixels), output = texture(device.Get(), 192, 96, nullptr);
    const auto composed = [&](const ColorGrade& g, int& worst, int& changed) {
        EyeComposition eyes[2];
        eyes_for(eyes, g);
        worst = 255;
        changed = 0;
        if (!compositor.compose(views.Get(), output.Get(), eyes))
            return false;
        const std::vector<uint32_t> out = read_image(device.Get(), context.Get(), output.Get());
        worst = 0;
        for (int e = 0; e < 2; ++e) {
            // A periphery pixel near the eye's corner and the focus box's centre (inside its feather).
            const uint32_t corner = out[4 * 192 + e * 96 + 4], centre = out[48 * 192 + e * 96 + 48];
            worst = std::max({worst, worst_channel(corner, grade_reference(outerColor[e], g)),
                              worst_channel(centre, grade_reference(focusColor[e], g))});
            changed += (corner != outerColor[e]) + (centre != focusColor[e]);
        }
        return true;
    };
    int worst = 0, changed = 0;
    check(composed(ColorGrade{}, worst, changed) && worst == 0 && changed == 0, "compose: off, every view as it was");
    check(composed(ColorGrade{true, 0, 0, 0, 0}, worst, changed) && worst == 0 && changed == 0,
          "compose: on with every value 0, as it was");
    const ColorGrade looks[] = {{true, 10, 0, 0, 0}, {true, 0, 20, 0, 0}, {true, 0, 0, 40, 0}, {true, 0, 0, 0, -100},
                                {true, -6, -10, 24, 60}};
    const char* lookNames[] = {"compose: +1 EV, periphery and focus alike, to the CPU's within 2 of 255",
                               "compose: brightness +20%, to the CPU's", "compose: contrast +40%, to the CPU's",
                               "compose: saturation -100% (grey), to the CPU's", "compose: all four at once, to the CPU's"};
    for (size_t i = 0; i < std::size(looks); ++i)
        check(composed(looks[i], worst, changed) && worst <= 2 && changed == 4, lookNames[i]);
    {  // grey really is grey
        EyeComposition eyes[2];
        eyes_for(eyes, ColorGrade{true, 0, 0, 0, -100});
        compositor.compose(views.Get(), output.Get(), eyes);
        const std::vector<uint32_t> out = read_image(device.Get(), context.Get(), output.Get());
        const uint32_t p = out[4 * 192 + 4];
        check(std::abs(channel(p, 0) - channel(p, 1)) <= 1 && std::abs(channel(p, 1) - channel(p, 2)) <= 1,
              "compose: saturation -100% leaves no colour");
    }

    // Native quad views' periphery copy: each eye's region drawn graded texel for texel at its place in the pad image.
    {
        std::vector<uint32_t> ramp(kW * kH);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x)
                ramp[y * kW + x] = rgb(static_cast<int>(x), static_cast<int>(y), static_cast<int>((x * 7 + y * 3) & 0xff));
        ComPtr<ID3D11Texture2D> source = texture(device.Get(), kW, kH, &ramp), pad = texture(device.Get(), 200, 100, nullptr);
        ComPtr<ID3D11RenderTargetView> rtv;
        device->CreateRenderTargetView(pad.Get(), nullptr, &rtv);
        const float black[4] = {0, 0, 0, 1};
        context->ClearRenderTargetView(rtv.Get(), black);
        const ColorGrade g{true, 4, -6, 20, 30};
        float constants[3];
        color_grade_constants(g, constants);
        ID3D11Texture2D* const sources[2] = {source.Get(), source.Get()};
        const int from[2][4] = {{10, 20, 60, 50}, {140, 5, 60, 50}}, to[2][2] = {{5, 30}, {105, 30}};
        check(compositor.draw_graded(sources, from, rtv.Get(), to, constants), "graded copy: drawn");
        const std::vector<uint32_t> out = read_image(device.Get(), context.Get(), pad.Get());
        int worstCopy = 0;
        bool outside = true;
        for (int e = 0; e < 2; ++e)
            for (int y = 0; y < from[e][3]; ++y)
                for (int x = 0; x < from[e][2]; ++x) {
                    const uint32_t want = grade_reference(ramp[(from[e][1] + y) * kW + from[e][0] + x], g);
                    worstCopy = std::max(worstCopy, worst_channel(out[(to[e][1] + y) * 200 + to[e][0] + x], want));
                }
        for (int x = 0; x < 200; ++x)  // the rows above and below the eyes stay as cleared
            outside &= out[10 * 200 + x] == rgb(0, 0, 0) && out[90 * 200 + x] == rgb(0, 0, 0);
        check(worstCopy <= 2, "graded copy: texel for texel, each eye's region graded at its place, to the CPU's");
        check(outside, "graded copy: nothing drawn outside the eyes' regions");
    }

    compositor.shutdown();
    return test_result();
}

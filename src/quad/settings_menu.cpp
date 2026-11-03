#include "settings_menu.h"

#include "fov_crop.h"
#include "views4_layout.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

namespace aceqv {
namespace {

struct Spec {
    MenuItem item;
    const wchar_t* label;
    int min, max, step;
};

// The launcher's ranges (kOuterRange, kInnerRange, kSizeRange, kFeatherRange, kCornerRange).
constexpr Spec kSpecs[] = {
    {MenuItem::Periphery, L"Periphery", 10, 100, 5},  {MenuItem::Density, L"Focus density", 50, 200, 5},
    {MenuItem::Width, L"Focus width", 10, 100, 1},     {MenuItem::Height, L"Focus height", 10, 100, 1},
    {MenuItem::Feather, L"Feather", 0, 50, 1},         {MenuItem::Corners, L"Corners", 0, 100, 5},
    {MenuItem::Outline, L"Outline", 0, 1, 1},          {MenuItem::EyeTracking, L"Eye tracking", 0, 1, 1},
    // The performance overlay's place, degrees (the launcher's sliders).
    {MenuItem::Overlay, L"Performance overlay", 0, 1, 1},
    {MenuItem::OverlayYaw, L"Overlay left / right", -kOsdYawMax, kOsdYawMax, 1},
    {MenuItem::OverlayPitch, L"Overlay up / down", -kOsdPitchMax, kOsdPitchMax, 1},
    {MenuItem::OverlayWidth, L"Overlay size", kOsdWidthMin, kOsdWidthMax, 1},
    // The panel's place, degrees (the launcher's kMenuYawRange, kMenuPitchRange, kMenuWidthRange).
    {MenuItem::MenuYaw, L"Menu left / right", -kMenuYawMax, kMenuYawMax, 1},
    {MenuItem::MenuPitch, L"Menu up / down", -kMenuPitchMax, kMenuPitchMax, 1},
    {MenuItem::MenuWidth, L"Menu size", kMenuWidthMin, kMenuWidthMax, 1},
    // The page switch and the Foveated DLSS page (cheeky_settings.h's ranges; presets step through its list).
    {MenuItem::Page, L"Page", 0, 1, 1},
    {MenuItem::CheekyProcessing, L"Foveated DLSS", 0, 1, 1},
    {MenuItem::CheekyOuterDlaa, L"Cheaper outer views", 0, 1, 1},
    {MenuItem::CheekyOuterScale, L"Outer DLAA scale", kCheekyScaleMin, kCheekyScaleMax, kCheekyScaleStep},
    {MenuItem::CheekyOuterPreset, L"Outer preset", 0, 0, 1},
    {MenuItem::CheekyFocusPreset, L"Focus preset", 0, 0, 1},
    {MenuItem::CheekySupersampling, L"Focus supersampling", kCheekySupersamplingMin, kCheekySupersamplingMax,
     kCheekySupersamplingStep},
    {MenuItem::CheekyMotionFix, L"Fix motion-vector blur", 0, 1, 1},
    {MenuItem::CheekyNr, L"DLSS-NR (experimental)", 0, 1, 1},
    {MenuItem::CheekyNrPreset, L"DLSS-NR preset", 0, kCheekyNrPresetMax, 1},
    {MenuItem::CheekyNrPasses, L"DLSS-NR passes", 1, kCheekyNrPassesMax, 1},
    {MenuItem::FgOn, L"Frame generation", 0, 1, 1},
    {MenuItem::FgMultiplier, L"Multiplier", 2, kCheekyFgMultiplierMax, 1},
    {MenuItem::FgSize, L"Center region", kCheekyFgSizeMin, kCheekyFgSizeMax, kCheekyFgSizeStep},
    {MenuItem::FgScale, L"Center FG scale", 25, 100, 25},
    {MenuItem::FgPeriphery, L"Periphery FG scale", 25, 100, 25},
    {MenuItem::FgBorder, L"Show FG region", 0, 1, 1},
    // The View crop page: shares of each eye's view removed at each side (fov_crop.h's kCropMax).
    {MenuItem::Crop, L"Crop the view", 0, 1, 1},
    {MenuItem::CropTop, L"Top", 0, static_cast<int>(kCropMax * 100), 1},
    {MenuItem::CropBottom, L"Bottom", 0, static_cast<int>(kCropMax * 100), 1},
    {MenuItem::CropOuter, L"Outer side (temple)", 0, static_cast<int>(kCropMax * 100), 1},
    {MenuItem::CropInner, L"Inner side (nose)", 0, static_cast<int>(kCropMax * 100), 1},
    {MenuItem::Helmet, L"Helmet overlay", 0, 1, 1},
    {MenuItem::HelmetRim, L"Helmet rim", 0, kHelmetRimMax, 1},
    {MenuItem::HelmetCorners, L"Helmet corners", 0, kHelmetCornersMax, 10},
    {MenuItem::HelmetLight, L"Helmet lighting", 0, kHelmetLightMax, kHelmetLightStep},
    {MenuItem::HelmetDebug, L"Show helmet light", 0, 1, 1},
    {MenuItem::Visor, L"Visor", 0, 2, 1},
    {MenuItem::HelmetSize, L"Helmet size", kHelmetSizeMin, kHelmetSizeMax, 5},
    {MenuItem::HelmetHeight, L"Helmet height", -kHelmetHeightMax, kHelmetHeightMax, 1},
    {MenuItem::HelmetDepth, L"Helmet depth", 0, kHelmetDepthMax, 1},
    {MenuItem::HelmetDetail, L"Helmet detail", 0, 1, 1},
};

const Spec& spec(MenuItem item) {
    for (const Spec& s : kSpecs)
        if (s.item == item)
            return s;
    return kSpecs[0];
}

int* value_of(MenuValues& v, MenuItem item) {
    switch (item) {
    case MenuItem::Periphery: return &v.periphery;
    case MenuItem::Density: return &v.density;
    case MenuItem::Width: return &v.width;
    case MenuItem::Height: return &v.height;
    case MenuItem::Feather: return &v.feather;
    case MenuItem::Corners: return &v.corners;
    case MenuItem::MenuYaw: return &v.placement.yaw;
    case MenuItem::MenuPitch: return &v.placement.pitch;
    case MenuItem::MenuWidth: return &v.placement.width;
    case MenuItem::OverlayYaw: return &v.overlay.yaw;
    case MenuItem::OverlayPitch: return &v.overlay.pitch;
    case MenuItem::OverlayWidth: return &v.overlay.width;
    case MenuItem::CropTop: return &v.cropTop;
    case MenuItem::CropBottom: return &v.cropBottom;
    case MenuItem::CropOuter: return &v.cropOuter;
    case MenuItem::CropInner: return &v.cropInner;
    case MenuItem::HelmetRim: return &v.helmet.rim;
    case MenuItem::HelmetCorners: return &v.helmet.corners;
    case MenuItem::HelmetLight: return &v.helmet.light;
    case MenuItem::HelmetSize: return &v.helmet.size;
    case MenuItem::HelmetHeight: return &v.helmet.height;
    default: return nullptr;
    }
}

int read_int(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini));
}

bool write(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int value) {
    return WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), ini) != 0;
}

}  // namespace

std::vector<MenuItem> menu_items(bool qvfr) {
    std::vector<MenuItem> items;
    if (qvfr)
        items = {MenuItem::Periphery, MenuItem::Density};
    items.insert(items.end(), {MenuItem::Width, MenuItem::Height, MenuItem::Feather, MenuItem::Corners,
                               MenuItem::Outline, MenuItem::EyeTracking});
    return items;
}

std::vector<MenuPage> menu_pages(const SettingsMenu& menu) {
    std::vector<MenuPage> pages = {MenuPage::Settings};
    if (menu.launch.qvfr)
        pages.insert(pages.end(), {MenuPage::Crop, MenuPage::Helmet});
    if (menu.cheeky.available)
        pages.insert(pages.end(), {MenuPage::Dlss, MenuPage::Fg});
    pages.push_back(MenuPage::Headset);
    return pages;
}

MenuPage menu_page(const SettingsMenu& menu) {
    const std::vector<MenuPage> pages = menu_pages(menu);
    return pages[std::clamp(menu.page, 0, static_cast<int>(pages.size()) - 1)];
}

std::vector<MenuItem> menu_page_items(const SettingsMenu& menu) {
    std::vector<MenuItem> items = {MenuItem::Page};
    const MenuPage page = menu_page(menu);
    if (page == MenuPage::Headset) {
        items.insert(items.end(), {MenuItem::Overlay, MenuItem::OverlayYaw, MenuItem::OverlayPitch, MenuItem::OverlayWidth,
                                   MenuItem::MenuYaw, MenuItem::MenuPitch, MenuItem::MenuWidth});
    } else if (page == MenuPage::Fg) {
        items.insert(items.end(), {MenuItem::FgOn, MenuItem::FgMultiplier, MenuItem::FgSize, MenuItem::FgScale,
                                   MenuItem::FgPeriphery, MenuItem::FgBorder});
    } else if (page == MenuPage::Crop) {
        items.push_back(MenuItem::Crop);
        if (menu.values.crop)
            items.insert(items.end(), {MenuItem::CropTop, MenuItem::CropBottom, MenuItem::CropOuter, MenuItem::CropInner});
    } else if (page == MenuPage::Helmet) {
        items.push_back(MenuItem::Helmet);
        if (menu.values.helmet.on && menu.launch.helmetPieces)
            items.insert(items.end(), {MenuItem::Visor, MenuItem::HelmetSize, MenuItem::HelmetHeight, MenuItem::HelmetDepth,
                                       MenuItem::HelmetDetail, MenuItem::HelmetLight, MenuItem::HelmetDebug});
        else if (menu.values.helmet.on)
            items.insert(items.end(), {MenuItem::HelmetRim, MenuItem::HelmetCorners, MenuItem::HelmetLight, MenuItem::HelmetDebug});
    } else if (page == MenuPage::Dlss) {
        items.insert(items.end(), {MenuItem::CheekyProcessing, MenuItem::CheekyOuterDlaa, MenuItem::CheekyOuterScale,
                                   MenuItem::CheekyOuterPreset, MenuItem::CheekyFocusPreset, MenuItem::CheekySupersampling,
                                   MenuItem::CheekyMotionFix, MenuItem::CheekyNr, MenuItem::CheekyNrPreset,
                                   MenuItem::CheekyNrPasses});
    } else {
        const std::vector<MenuItem> method = menu_items(menu.launch.qvfr);
        items.insert(items.end(), method.begin(), method.end());
    }
    return items;
}

namespace {

std::wstring page_name(const SettingsMenu& menu, MenuPage page) {
    switch (page) {
    case MenuPage::Dlss: return L"Foveated DLSS";
    case MenuPage::Fg: return L"Frame generation";
    case MenuPage::Headset: return L"Overlay and menu";
    case MenuPage::Crop: return L"View crop";
    case MenuPage::Helmet: return L"Helmet";
    default: return menu.launch.qvfr ? L"QVFR" : L"Two-loop";
    }
}

}  // namespace

std::wstring menu_title(const SettingsMenu& menu) {
    const MenuPage page = menu_page(menu);
    return page == MenuPage::Settings ? page_name(menu, page) + L" settings" : page_name(menu, page);
}

std::wstring menu_subtitle(const SettingsMenu& menu) {
    const MenuPage page = menu_page(menu);
    return page == MenuPage::Dlss || page == MenuPage::Fg ? cheeky_version_label(menu.cheeky.version) : L"";
}

bool menu_key(SettingsMenu& menu, MenuKey key) {
    const int pages = static_cast<int>(menu_pages(menu).size());
    menu.page = std::clamp(menu.page, 0, pages - 1);
    const std::vector<MenuItem> items = menu_page_items(menu);
    const int n = static_cast<int>(items.size());
    menu.selected = std::clamp(menu.selected, 0, n - 1);
    if (key == MenuKey::Up || key == MenuKey::Down) {
        menu.selected = (menu.selected + (key == MenuKey::Down ? 1 : n - 1)) % n;
        return false;
    }
    const MenuItem item = items[menu.selected];
    if (item == MenuItem::Page) {  // the next or previous page, with the switch still selected
        menu.page = (menu.page + (key == MenuKey::Right ? 1 : pages - 1)) % pages;
        menu.selected = 0;
        return false;
    }
    CheekySettings& c = menu.cheeky.values;
    const int step = key == MenuKey::Right ? 1 : -1;
    switch (item) {
    case MenuItem::CheekyProcessing: c.processing = !c.processing; return true;
    case MenuItem::CheekyOuterDlaa: c.outerDlaa = !c.outerDlaa; return true;
    case MenuItem::CheekyOuterScale: {
        const int before = c.outerScale;
        c.outerScale = std::clamp(c.outerScale + step * kCheekyScaleStep, kCheekyScaleMin, kCheekyScaleMax);
        return c.outerScale != before;
    }
    case MenuItem::CheekyOuterPreset: c.outerPreset = cheeky_preset_step(c.outerPreset, step, false); return true;
    case MenuItem::CheekyFocusPreset: c.focusPreset = cheeky_preset_step(c.focusPreset, step, true); return true;
    case MenuItem::CheekySupersampling: {
        const int before = c.supersampling;
        c.supersampling = std::clamp(c.supersampling + step * kCheekySupersamplingStep, kCheekySupersamplingMin,
                                     kCheekySupersamplingMax);
        return c.supersampling != before;
    }
    case MenuItem::CheekyMotionFix: c.motionFix = !c.motionFix; return true;
    case MenuItem::CheekyNr: c.nr = !c.nr; return true;
    case MenuItem::CheekyNrPreset: c.nrPreset = ((c.nrPreset + step) % (kCheekyNrPresetMax + 1) + kCheekyNrPresetMax + 1) %
                                                (kCheekyNrPresetMax + 1);
        return true;
    case MenuItem::CheekyNrPasses: {
        const int before = c.nrPasses;
        c.nrPasses = std::clamp(c.nrPasses + step, 1, kCheekyNrPassesMax);
        return c.nrPasses != before;
    }
    case MenuItem::HelmetDepth: {  // far (0), then 3 cm on: 1 cm a step up close, 5 cm from 20 cm
        int& d = menu.values.helmet.depth;
        const int before = d;
        if (step > 0)
            d = d == 0 ? kHelmetDepthMin : std::min(kHelmetDepthMax, d + (d >= 20 ? 5 : 1));
        else
            d = d <= kHelmetDepthMin ? 0 : d - (d > 20 ? 5 : 1);
        return d != before;
    }
    case MenuItem::Visor: {  // closed, half, open (no wrap)
        const int before = static_cast<int>(menu.values.helmet.visor);
        menu.values.helmet.visor = static_cast<VisorPosition>(std::clamp(before + step, 0, 2));
        return static_cast<int>(menu.values.helmet.visor) != before;
    }
    case MenuItem::FgOn: c.fg = !c.fg; return true;
    case MenuItem::FgBorder: c.fgBorder = !c.fgBorder; return true;
    case MenuItem::FgMultiplier:
    case MenuItem::FgSize:
    case MenuItem::FgScale:
    case MenuItem::FgPeriphery: {
        const Spec& sp = spec(item);
        int& v = item == MenuItem::FgMultiplier ? c.fgMultiplier
                 : item == MenuItem::FgSize     ? c.fgSize
                 : item == MenuItem::FgScale    ? c.fgScale
                                                : c.fgPeriphery;
        const int before = v;
        v = std::clamp(v + step * sp.step, sp.min, sp.max);
        return v != before;
    }
    default: break;
    }
    if (item == MenuItem::Outline || item == MenuItem::EyeTracking || item == MenuItem::Overlay || item == MenuItem::Crop ||
        item == MenuItem::Helmet || item == MenuItem::HelmetDebug || item == MenuItem::HelmetDetail) {
        bool& flag = item == MenuItem::Outline       ? menu.values.outline
                     : item == MenuItem::EyeTracking ? menu.values.eyeTracking
                     : item == MenuItem::Crop        ? menu.values.crop
                     : item == MenuItem::Helmet      ? menu.values.helmet.on
                     : item == MenuItem::HelmetDebug ? menu.values.helmet.debugLight
                     : item == MenuItem::HelmetDetail ? menu.values.helmet.detail
                                                     : menu.values.overlay.enabled;
        flag = !flag;
        return true;
    }
    const Spec& s = spec(item);
    int* v = value_of(menu.values, item);
    const int before = *v;
    *v = std::clamp(*v + (key == MenuKey::Right ? s.step : -s.step), s.min, s.max);
    return *v != before;
}

MenuValues read_menu_values(const wchar_t* ini) {
    MenuValues v;
    v.width = std::clamp(read_int(ini, L"experiment", L"focus_percent", v.width), 10, 100);
    v.height = std::clamp(read_int(ini, L"experiment", L"focus_height_percent", v.width), 10, 100);
    const int eyeScale = read_int(ini, L"experiment", L"eye_scale", v.periphery);
    const int periphery = read_int(ini, L"experiment", L"periphery_scale", 0);
    v.periphery = std::clamp(periphery ? periphery : eyeScale, 10, 100);
    const int density = read_int(ini, L"experiment", L"focus_density", 0);
    v.density = std::clamp(density ? density : static_cast<int>(std::lround(100.0 * eyeScale / v.width)), 50, 200);
    v.feather = std::clamp(read_int(ini, L"quad", L"feather_percent", v.feather), 0, 50);
    v.corners = std::clamp(read_int(ini, L"quad", L"corner_radius_percent", v.corners), 0, 100);
    v.outline = read_int(ini, L"quad", L"debug_border", 0) != 0;
    v.eyeTracking = read_int(ini, L"quad", L"gaze", 1) != 0;
    v.overlay = read_osd_placement(ini);
    v.placement = read_menu_placement(ini);
    const int cropMax = static_cast<int>(kCropMax * 100);
    v.crop = read_int(ini, L"crop", L"enabled", 0) != 0;
    v.cropTop = std::clamp(read_int(ini, L"crop", L"top", 0), 0, cropMax);
    v.cropBottom = std::clamp(read_int(ini, L"crop", L"bottom", 0), 0, cropMax);
    v.cropOuter = std::clamp(read_int(ini, L"crop", L"outer", 0), 0, cropMax);
    v.cropInner = std::clamp(read_int(ini, L"crop", L"inner", 0), 0, cropMax);
    v.helmet = read_helmet_settings(ini);
    return v;
}

bool write_menu_values(const wchar_t* ini, const MenuValues& v, bool qvfr) {
    bool ok = true;
    if (qvfr) {
        ok &= write(ini, L"experiment", L"periphery_scale", v.periphery);
        ok &= write(ini, L"experiment", L"focus_density", v.density);
        ok &= write(ini, L"crop", L"enabled", v.crop ? 1 : 0);
        ok &= write(ini, L"crop", L"top", v.cropTop);
        ok &= write(ini, L"crop", L"bottom", v.cropBottom);
        ok &= write(ini, L"crop", L"outer", v.cropOuter);
        ok &= write(ini, L"crop", L"inner", v.cropInner);
        ok &= write_helmet_settings(ini, v.helmet);
    }
    ok &= write(ini, L"experiment", L"focus_percent", v.width);
    ok &= write(ini, L"experiment", L"focus_height_percent", v.height);
    ok &= write(ini, L"quad", L"feather_percent", v.feather);
    ok &= write(ini, L"quad", L"corner_radius_percent", v.corners);
    ok &= write(ini, L"quad", L"debug_border", v.outline ? 1 : 0);
    ok &= write(ini, L"quad", L"gaze", v.eyeTracking ? 1 : 0);
    ok &= write_osd_placement(ini, v.overlay);
    ok &= write(ini, L"menu", L"yaw", v.placement.yaw);
    ok &= write(ini, L"menu", L"pitch", v.placement.pitch);
    ok &= write(ini, L"menu", L"width", v.placement.width);
    return ok;
}

// The launch layout from the launch sizes (percent of the native eye), as the layer has it.
Views4Layout launch_layout(const MenuLaunch& l) {
    return views4_layout(l.outer / 100, l.focusWidth / 100, l.focusHeight / 100, l.cropWidth / 100, l.cropHeight / 100);
}

MenuFit menu_fit(const SettingsMenu& menu) {
    const MenuValues& v = menu.values;
    MenuFit f;
    // The focus views' size against the native eye: density times the box (the layer's views4_sizes).
    f.wantFocusWidth = v.density * v.width / 100.0;
    f.wantFocusHeight = v.density * v.height / 100.0;
    f.periphery = v.periphery;
    f.focusWidth = f.wantFocusWidth;
    f.focusHeight = f.wantFocusHeight;
    f.densityX = f.densityY = v.density;
    if (!menu.launch.qvfr)
        return f;
    // The layer's own rule (views4_fit), in percent.
    const Views4Fit fit = views4_fit(launch_layout(menu.launch), v.periphery / 100.0, f.wantFocusWidth / 100,
                                     f.wantFocusHeight / 100, v.crop_width() / 100, v.crop_height() / 100);
    f.periphery = fit.outer * 100;
    f.focusWidth = fit.focus * 100;
    f.focusHeight = fit.focusHeight * 100;
    f.peripheryCapped = fit.outerCapped;
    f.peripheryByRoom = fit.outerByRoom;
    f.focusCapped = fit.focusCapped;
    f.densityX = f.focusWidth * 100 / v.width;
    f.densityY = f.focusHeight * 100 / v.height;
    return f;
}

bool menu_over_launch(const SettingsMenu& menu) {
    const MenuFit f = menu_fit(menu);
    return f.peripheryCapped || f.focusCapped;
}

namespace {

// "12° right", "4° down", "0°".
std::wstring degrees(int v, const wchar_t* plus, const wchar_t* minus) {
    return v == 0 ? L"0\u00b0" : std::to_wstring(v < 0 ? -v : v) + L"\u00b0 " + (v > 0 ? plus : minus);
}

std::wstring percent(double v) { return std::to_wstring(static_cast<int>(std::lround(v))) + L"%"; }

int px(int native, double percentOfNative) { return static_cast<int>(std::lround(native * percentOfNative / 100)); }

// A size in percent of the native eye, in pixels ("2232x1751"), or empty when the native size is unknown.
std::wstring pixels(const MenuLaunch& l, double width, double height) {
    if (l.nativeWidth <= 0 || l.nativeHeight <= 0)
        return L"";
    return std::to_wstring(px(l.nativeWidth, width)) + L"x" + std::to_wstring(px(l.nativeHeight, height));
}

// ACE's double-wide target for views of these sizes (percent of the native eye; `outer` the
// periphery's density, over `cropWidth` x `cropHeight` percent of the eye): the row of four views, as
// tall as the taller pair.
std::wstring target(const MenuLaunch& l, double outer, double focusWidth, double focusHeight, double cropWidth,
                    double cropHeight) {
    if (l.nativeWidth <= 0 || l.nativeHeight <= 0)
        return L"?";
    const int w = 2 * (px(l.nativeWidth, outer * cropWidth / 100) + px(l.nativeWidth, focusWidth));
    const int h = std::max(px(l.nativeHeight, outer * cropHeight / 100), px(l.nativeHeight, focusHeight));
    wchar_t mpx[16];
    swprintf_s(mpx, L"%.1f", static_cast<double>(w) * h / 1e6);
    return std::to_wstring(w) + L"x" + std::to_wstring(h) + L" (" + mpx + L" MP)";
}

}  // namespace

MenuResolution menu_resolution(const SettingsMenu& menu) {
    MenuResolution r;
    const MenuLaunch& l = menu.launch;
    const MenuPage page = menu_page(menu);
    if (!l.qvfr || (page != MenuPage::Settings && page != MenuPage::Crop && menu.cheeky.available))
        return r;
    const MenuFit f = menu_fit(menu);
    const MenuValues& v = menu.values;
    r.shown = true;
    r.max = target(l, l.outer, l.focusWidth, l.focusHeight, l.cropWidth, l.cropHeight);
    r.current = target(l, f.periphery, f.focusWidth, f.focusHeight, v.crop_width(), v.crop_height());
    r.capped = f.peripheryCapped || f.focusCapped;
    if (r.capped)
        r.asked = target(l, v.periphery, f.wantFocusWidth, f.wantFocusHeight, v.crop_width(), v.crop_height());
    return r;
}

MenuStats menu_stats(const SettingsMenu& menu) {
    MenuStats s;
    const MenuLaunch& l = menu.launch;
    const MenuFit f = menu_fit(menu);
    // Density in effect (QVFR: the fit's, horizontal; Two-loop: eye_scale over the box's share).
    const double density = l.qvfr ? f.densityX : l.twoLoopScale * 100 / menu.values.width;
    if (l.nativePpd > 0)
        s.centerPpd = static_cast<int>(std::lround(l.nativePpd * density / 100));
    // Rendered against both native eyes, in pixels when the native size is known (as the launcher).
    double rendered, native;
    if (l.nativeWidth > 0 && l.nativeHeight > 0) {
        const auto area = [&](double w, double h) {
            return static_cast<double>(px(l.nativeWidth, w)) * px(l.nativeHeight, h);
        };
        rendered = l.qvfr ? 2 * (area(f.periphery * menu.values.crop_width() / 100, f.periphery * menu.values.crop_height() / 100) +
                                 area(f.focusWidth, f.focusHeight))
                          : 4 * area(l.twoLoopScale, l.twoLoopScale);
        native = 2.0 * l.nativeWidth * l.nativeHeight;
    } else {
        rendered = l.qvfr ? 2 * (f.periphery * f.periphery * menu.values.crop_width() * menu.values.crop_height() / 1e4 +
                                 f.focusWidth * f.focusHeight)
                          : 4 * l.twoLoopScale * l.twoLoopScale;
        native = 2 * 100.0 * 100.0;
    }
    s.savedPercent = static_cast<int>(std::lround((1 - rendered / native) * 100));
    return s;
}

std::vector<std::wstring> menu_notes(const SettingsMenu& menu) {
    std::vector<std::wstring> notes;
    const MenuPage page = menu_page(menu);
    if (page == MenuPage::Fg) {
        if (menu.cheeky.values.fg)
            notes.push_back(L"Needs OpenXR Toolkit's Turbo mode and other frame generation off");
        return notes;
    }
    if (page == MenuPage::Dlss || page == MenuPage::Helmet)
        return notes;
    if (page == MenuPage::Crop) {
        // The FOV follows at once; the views' pixels only without DLSS (and up to the launch size).
        if (menu.launch.dlssHeld)
            notes.push_back(L"DLSS is on: the view crops now, its pixel savings apply at the next launch");
        if (menu.values.crop_width() > menu.launch.cropWidth + 0.5 || menu.values.crop_height() > menu.launch.cropHeight + 0.5)
            notes.push_back(L"Less crop than at launch: the periphery is softer until the next launch");
        return notes;
    }
    if (menu.launch.qvfr && menu.launch.dlssHeld)
        notes.push_back(L"DLSS is on: resolution changes apply at the next launch");
    const MenuFit f = menu_fit(menu);
    const MenuLaunch& l = menu.launch;
    const bool known = l.nativeWidth > 0 && l.nativeHeight > 0;
    if (f.focusCapped)
        notes.push_back(known ? L"Focus area needs " + pixels(l, f.wantFocusWidth, f.wantFocusHeight) + L" px, " +
                                    pixels(l, f.focusWidth, f.focusHeight) + L" fits: lower Focus density, width or height"
                              : L"Focus area is too large for this launch: lower Focus density, width or height");
    // The periphery's views at a density, over the FOV crop's share of the eye.
    const auto periphery = [&](double density) {
        return pixels(l, density * menu.values.crop_width() / 100, density * menu.values.crop_height() / 100);
    };
    if (f.peripheryCapped && f.peripheryByRoom)
        notes.push_back(L"Periphery needs " + (known ? periphery(menu.values.periphery) + L" px, the focus area leaves " +
                                                           periphery(f.periphery)
                                                     : std::wstring(L"more room")) +
                        L": lower Focus density or size to make room");
    else if (f.peripheryCapped)
        notes.push_back(L"Periphery above " + percent(f.periphery) + L" does not fit this launch: relaunch to use it");
    return notes;
}

std::vector<MenuLine> menu_lines(const SettingsMenu& menu) {
    std::vector<MenuLine> lines;
    const std::vector<MenuItem> items = menu_page_items(menu);
    MenuValues v = menu.values;
    const CheekySettings& c = menu.cheeky.values;
    const MenuFit fit = menu_fit(menu);
    for (size_t i = 0; i < items.size(); ++i) {
        MenuLine line;
        line.label = spec(items[i]).label;
        line.selected = static_cast<int>(i) == menu.selected;
        if (items[i] == MenuItem::Page)
            line.value = page_name(menu, menu_page(menu));
        else if (items[i] == MenuItem::FgOn)
            line.value = c.fg ? L"On" : L"Off";
        else if (items[i] == MenuItem::FgMultiplier)
            line.value = std::to_wstring(c.fgMultiplier) + L"x";
        else if (items[i] == MenuItem::FgSize)
            line.value = std::to_wstring(c.fgSize) + L"%";
        else if (items[i] == MenuItem::FgScale)
            line.value = std::to_wstring(c.fgScale) + L"%";
        else if (items[i] == MenuItem::FgPeriphery)
            line.value = std::to_wstring(c.fgPeriphery) + L"%";
        else if (items[i] == MenuItem::FgBorder)
            line.value = c.fgBorder ? L"On" : L"Off";
        else if (items[i] == MenuItem::CheekyProcessing)
            line.value = c.processing ? L"On" : L"Off";
        else if (items[i] == MenuItem::CheekyOuterDlaa)
            line.value = c.outerDlaa ? L"On" : L"Off";
        else if (items[i] == MenuItem::CheekyOuterScale)
            line.value = std::to_wstring(c.outerScale) + L"%";
        else if (items[i] == MenuItem::CheekyOuterPreset)
            line.value = cheeky_preset_name(c.outerPreset);
        else if (items[i] == MenuItem::CheekyFocusPreset)
            line.value = cheeky_preset_name(c.focusPreset);
        else if (items[i] == MenuItem::CheekySupersampling)
            line.value = std::to_wstring(c.supersampling) + L"%";
        else if (items[i] == MenuItem::CheekyMotionFix)
            line.value = c.motionFix ? L"On" : L"Off";
        else if (items[i] == MenuItem::CheekyNr)
            line.value = c.nr ? L"On" : L"Off";
        else if (items[i] == MenuItem::CheekyNrPreset)
            line.value = kCheekyNrPresets[std::clamp(c.nrPreset, 0, kCheekyNrPresetMax)];
        else if (items[i] == MenuItem::CheekyNrPasses)
            line.value = std::to_wstring(c.nrPasses);
        else if (items[i] == MenuItem::Outline)
            line.value = v.outline ? L"On" : L"Off";
        else if (items[i] == MenuItem::EyeTracking)
            line.value = v.eyeTracking ? L"On" : L"Off";
        else if (items[i] == MenuItem::Overlay)
            line.value = v.overlay.enabled ? L"On" : L"Off";
        else if (items[i] == MenuItem::Crop)
            line.value = v.crop ? L"On" : L"Off";
        else if (items[i] == MenuItem::Helmet)
            line.value = v.helmet.on ? L"On" : L"Off";
        else if (items[i] == MenuItem::HelmetDebug)
            line.value = v.helmet.debugLight ? L"On" : L"Off";
        else if (items[i] == MenuItem::Visor)
            line.value = kVisorPositionNames[static_cast<int>(v.helmet.visor)];
        else if (items[i] == MenuItem::HelmetSize)
            line.value = std::to_wstring(v.helmet.size) + L"\u00b0";
        else if (items[i] == MenuItem::HelmetHeight)
            line.value = degrees(v.helmet.height, L"up", L"down");
        else if (items[i] == MenuItem::HelmetDetail)
            line.value = kHelmetDetailNames[v.helmet.detail ? 1 : 0];
        else if (items[i] == MenuItem::HelmetDepth)
            line.value = v.helmet.depth ? std::to_wstring(v.helmet.depth) + L" cm" : L"Far";
        else if (items[i] == MenuItem::OverlayYaw)
            line.value = degrees(v.overlay.yaw, L"right", L"left");
        else if (items[i] == MenuItem::OverlayPitch)
            line.value = degrees(v.overlay.pitch, L"up", L"down");
        else if (items[i] == MenuItem::OverlayWidth)
            line.value = std::to_wstring(v.overlay.width) + L"\u00b0";
        else if (items[i] == MenuItem::MenuYaw)
            line.value = degrees(v.placement.yaw, L"right", L"left");
        else if (items[i] == MenuItem::MenuPitch)
            line.value = degrees(v.placement.pitch, L"up", L"down");
        else if (items[i] == MenuItem::MenuWidth)
            line.value = std::to_wstring(v.placement.width) + L"\u00b0";
        else
            line.value = std::to_wstring(*value_of(v, items[i])) + L"%";
        // What is in effect when the asked value does not fit the launch size.
        if (items[i] == MenuItem::Periphery && fit.peripheryCapped) {
            line.capped = true;
            line.value += L"  (" + percent(fit.periphery) + L" now)";
        } else if (items[i] == MenuItem::Density && fit.focusCapped) {
            line.capped = true;
            line.value += std::lround(fit.densityX) == std::lround(fit.densityY)
                              ? L"  (" + percent(fit.densityX) + L" now)"
                              : L"  (" + percent(fit.densityX) + L" x " + percent(fit.densityY) + L" now)";
        }
        lines.push_back(line);
    }
    return lines;
}

MenuPlacement read_menu_placement(const wchar_t* ini) {
    // As strings: GetPrivateProfileInt reads negative values as 0.
    const auto read = [ini](const wchar_t* key, int fallback) {
        wchar_t text[16] = {};
        GetPrivateProfileStringW(L"menu", key, L"", text, 16, ini);
        return text[0] ? _wtoi(text) : fallback;
    };
    MenuPlacement p;
    p.yaw = std::clamp(read(L"yaw", p.yaw), -kMenuYawMax, kMenuYawMax);
    p.pitch = std::clamp(read(L"pitch", p.pitch), -kMenuPitchMax, kMenuPitchMax);
    p.width = std::clamp(read(L"width", p.width), kMenuWidthMin, kMenuWidthMax);
    return p;
}

MenuPose menu_pose(const MenuPlacement& p, float aspect) {
    constexpr double kRad = 3.14159265358979 / 180;
    const double yaw = p.yaw * kRad, pitch = p.pitch * kRad;
    MenuPose pose;
    pose.position[0] = static_cast<float>(kMenuDistance * std::sin(yaw) * std::cos(pitch));
    pose.position[1] = static_cast<float>(kMenuDistance * std::sin(pitch));
    pose.position[2] = static_cast<float>(-kMenuDistance * std::cos(yaw) * std::cos(pitch));
    // Turn about Y by -yaw, then about X by pitch: the quad's +Z (its front) points back at the eye.
    const double hy = -yaw / 2, hp = pitch / 2;
    pose.orientation[0] = static_cast<float>(std::cos(hy) * std::sin(hp));
    pose.orientation[1] = static_cast<float>(std::sin(hy) * std::cos(hp));
    pose.orientation[2] = static_cast<float>(-std::sin(hy) * std::sin(hp));
    pose.orientation[3] = static_cast<float>(std::cos(hy) * std::cos(hp));
    pose.width = static_cast<float>(2 * kMenuDistance * std::tan(p.width * kRad / 2));
    pose.height = pose.width * aspect;
    return pose;
}

}  // namespace aceqv

#include "settings_menu.h"

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
    // The panel's place, degrees (the launcher's kMenuYawRange, kMenuPitchRange, kMenuWidthRange).
    {MenuItem::MenuYaw, L"Menu left / right", -kMenuYawMax, kMenuYawMax, 1},
    {MenuItem::MenuPitch, L"Menu up / down", -kMenuPitchMax, kMenuPitchMax, 1},
    {MenuItem::MenuWidth, L"Menu size", kMenuWidthMin, kMenuWidthMax, 1},
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
                               MenuItem::Outline, MenuItem::EyeTracking, MenuItem::MenuYaw, MenuItem::MenuPitch,
                               MenuItem::MenuWidth});
    return items;
}

bool menu_key(SettingsMenu& menu, MenuKey key) {
    const std::vector<MenuItem> items = menu_items(menu.launch.qvfr);
    const int n = static_cast<int>(items.size());
    menu.selected = std::clamp(menu.selected, 0, n - 1);
    if (key == MenuKey::Up || key == MenuKey::Down) {
        menu.selected = (menu.selected + (key == MenuKey::Down ? 1 : n - 1)) % n;
        return false;
    }
    const MenuItem item = items[menu.selected];
    if (item == MenuItem::Outline || item == MenuItem::EyeTracking) {
        bool& flag = item == MenuItem::Outline ? menu.values.outline : menu.values.eyeTracking;
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
    v.placement = read_menu_placement(ini);
    return v;
}

bool write_menu_values(const wchar_t* ini, const MenuValues& v, bool qvfr) {
    bool ok = true;
    if (qvfr) {
        ok &= write(ini, L"experiment", L"periphery_scale", v.periphery);
        ok &= write(ini, L"experiment", L"focus_density", v.density);
    }
    ok &= write(ini, L"experiment", L"focus_percent", v.width);
    ok &= write(ini, L"experiment", L"focus_height_percent", v.height);
    ok &= write(ini, L"quad", L"feather_percent", v.feather);
    ok &= write(ini, L"quad", L"corner_radius_percent", v.corners);
    ok &= write(ini, L"quad", L"debug_border", v.outline ? 1 : 0);
    ok &= write(ini, L"quad", L"gaze", v.eyeTracking ? 1 : 0);
    ok &= write(ini, L"menu", L"yaw", v.placement.yaw);
    ok &= write(ini, L"menu", L"pitch", v.placement.pitch);
    ok &= write(ini, L"menu", L"width", v.placement.width);
    return ok;
}

// The launch layout from the launch sizes (percent of the native eye), as the layer has it.
Views4Layout launch_layout(const MenuLaunch& l) { return views4_layout(l.outer / 100, l.focusWidth / 100, l.focusHeight / 100); }

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
                                     f.wantFocusHeight / 100);
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

// ACE's double-wide target for views of these sizes (percent of the native eye): the row of four
// views, as tall as the taller pair.
std::wstring target(const MenuLaunch& l, double outer, double focusWidth, double focusHeight) {
    if (l.nativeWidth <= 0 || l.nativeHeight <= 0)
        return L"?";
    const int w = 2 * (px(l.nativeWidth, outer) + px(l.nativeWidth, focusWidth));
    const int h = std::max(px(l.nativeHeight, outer), px(l.nativeHeight, focusHeight));
    wchar_t mpx[16];
    swprintf_s(mpx, L"%.1f", static_cast<double>(w) * h / 1e6);
    return std::to_wstring(w) + L"x" + std::to_wstring(h) + L" (" + mpx + L" MP)";
}

}  // namespace

MenuResolution menu_resolution(const SettingsMenu& menu) {
    MenuResolution r;
    const MenuLaunch& l = menu.launch;
    if (!l.qvfr)
        return r;
    const MenuFit f = menu_fit(menu);
    r.shown = true;
    r.max = target(l, l.outer, l.focusWidth, l.focusHeight);
    r.current = target(l, f.periphery, f.focusWidth, f.focusHeight);
    r.capped = f.peripheryCapped || f.focusCapped;
    if (r.capped)
        r.asked = target(l, menu.values.periphery, f.wantFocusWidth, f.wantFocusHeight);
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
        rendered = l.qvfr ? 2 * (area(f.periphery, f.periphery) + area(f.focusWidth, f.focusHeight))
                          : 4 * area(l.twoLoopScale, l.twoLoopScale);
        native = 2.0 * l.nativeWidth * l.nativeHeight;
    } else {
        rendered = l.qvfr ? 2 * (f.periphery * f.periphery + f.focusWidth * f.focusHeight)
                          : 4 * l.twoLoopScale * l.twoLoopScale;
        native = 2 * 100.0 * 100.0;
    }
    s.savedPercent = static_cast<int>(std::lround((1 - rendered / native) * 100));
    return s;
}

std::vector<std::wstring> menu_notes(const SettingsMenu& menu) {
    std::vector<std::wstring> notes;
    const MenuFit f = menu_fit(menu);
    const MenuLaunch& l = menu.launch;
    const bool known = l.nativeWidth > 0 && l.nativeHeight > 0;
    if (f.focusCapped)
        notes.push_back(known ? L"Focus area needs " + pixels(l, f.wantFocusWidth, f.wantFocusHeight) + L" px, " +
                                    pixels(l, f.focusWidth, f.focusHeight) + L" fits: lower Focus density, width or height"
                              : L"Focus area is too large for this launch: lower Focus density, width or height");
    if (f.peripheryCapped && f.peripheryByRoom)
        notes.push_back(L"Periphery needs " + (known ? pixels(l, menu.values.periphery, menu.values.periphery) + L" px, the focus area leaves " +
                                                           pixels(l, f.periphery, f.periphery)
                                                     : std::wstring(L"more room")) +
                        L": lower Focus density or size to make room");
    else if (f.peripheryCapped)
        notes.push_back(L"Periphery above " + percent(f.periphery) + L" does not fit this launch: relaunch to use it");
    return notes;
}

std::vector<MenuLine> menu_lines(const SettingsMenu& menu) {
    std::vector<MenuLine> lines;
    const std::vector<MenuItem> items = menu_items(menu.launch.qvfr);
    MenuValues v = menu.values;
    const MenuFit fit = menu_fit(menu);
    for (size_t i = 0; i < items.size(); ++i) {
        MenuLine line;
        line.label = spec(items[i]).label;
        line.selected = static_cast<int>(i) == menu.selected;
        if (items[i] == MenuItem::Outline)
            line.value = v.outline ? L"On" : L"Off";
        else if (items[i] == MenuItem::EyeTracking)
            line.value = v.eyeTracking ? L"On" : L"Off";
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

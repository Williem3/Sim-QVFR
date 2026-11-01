#include "settings.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include <windows.h>

namespace aceqv::launcher {
namespace {

// The views4 call sites of the tested QVFR configuration (recon "Darkness fixed").
constexpr wchar_t kQvfrCallers[] = L"init,draw,tiles,slots,cameras,fog,consts,final,oitpass";

int read_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

std::wstring read_string(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[256] = {};
    GetPrivateProfileStringW(section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)), ini.c_str());
    return buffer;
}

int clamp_to(int v, Range r) { return std::clamp(v, r.min, r.max); }

bool write(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(section, key, value.c_str(), ini.c_str()) != 0;
}

}  // namespace

const wchar_t* color_name(OutlineColor color) {
    switch (color) {
    case OutlineColor::Magenta: return L"magenta";
    case OutlineColor::Green: return L"green";
    default: return L"teal";
    }
}

Settings load_settings(const std::wstring& ini) {
    Settings s;
    const std::wstring mode = read_string(ini, L"experiment", L"mode");
    s.mode = _wcsicmp(mode.c_str(), L"quad") == 0 ? RenderMode::TwoLoop : RenderMode::Qvfr;
    s.width = clamp_to(read_int(ini, L"experiment", L"focus_percent", s.width), kSizeRange);
    s.height = clamp_to(read_int(ini, L"experiment", L"focus_height_percent", s.width), kSizeRange);
    const int eyeScale = read_int(ini, L"experiment", L"eye_scale", s.outer);
    const int periphery = read_int(ini, L"experiment", L"periphery_scale", 0);
    s.outer = clamp_to(s.mode == RenderMode::Qvfr && periphery ? periphery : eyeScale, kOuterRange);
    // Unset, the layer derives the density from eye_scale (equal quarters); show that value.
    const int density = read_int(ini, L"experiment", L"focus_density", 0);
    s.inner = clamp_to(density ? density : static_cast<int>(std::lround(100.0 * eyeScale / s.width)), kInnerRange);
    s.feather = clamp_to(read_int(ini, L"quad", L"feather_percent", s.feather), kFeatherRange);
    s.corner = clamp_to(read_int(ini, L"quad", L"corner_radius_percent", s.corner), kCornerRange);
    s.eyeTracking = read_int(ini, L"quad", L"gaze", 1) != 0;
    s.outline = read_int(ini, L"quad", L"debug_border", 0) != 0;
    const std::wstring color = read_string(ini, L"quad", L"debug_border_color");
    s.color = _wcsicmp(color.c_str(), L"magenta") == 0 ? OutlineColor::Magenta
              : _wcsicmp(color.c_str(), L"green") == 0 ? OutlineColor::Green
                                                       : OutlineColor::Teal;
    return s;
}

bool save_settings(const std::wstring& ini, const Settings& in) {
    Settings s = in;
    s.outer = clamp_to(s.outer, kOuterRange);
    s.inner = clamp_to(s.inner, kInnerRange);
    s.width = clamp_to(s.width, kSizeRange);
    s.height = clamp_to(s.height, kSizeRange);
    s.feather = clamp_to(s.feather, kFeatherRange);
    s.corner = clamp_to(s.corner, kCornerRange);
    const auto n = [](int v) { return std::to_wstring(v); };
    bool ok = true;
    if (s.mode == RenderMode::Qvfr) {
        ok &= write(ini, L"experiment", L"mode", L"views4");
        ok &= write(ini, L"experiment", L"views4_callers", kQvfrCallers);
        ok &= write(ini, L"experiment", L"views4_compose", L"1");
        ok &= write(ini, L"experiment", L"views4_show_copies", L"0");
        ok &= write(ini, L"experiment", L"periphery_scale", n(s.outer));
        ok &= write(ini, L"experiment", L"focus_density", n(s.inner));
    } else {
        ok &= write(ini, L"experiment", L"mode", L"quad");
    }
    ok &= write(ini, L"experiment", L"eye_scale", n(s.outer));  // Two-loop's resolution; QVFR's fallback
    ok &= write(ini, L"experiment", L"focus_percent", n(s.width));
    ok &= write(ini, L"experiment", L"focus_height_percent", n(s.height));
    ok &= write(ini, L"quad", L"feather_percent", n(s.feather));
    ok &= write(ini, L"quad", L"corner_radius_percent", n(s.corner));
    ok &= write(ini, L"quad", L"gaze", s.eyeTracking ? L"1" : L"0");
    ok &= write(ini, L"quad", L"debug_border", s.outline ? L"1" : L"0");
    ok &= write(ini, L"quad", L"debug_border_color", color_name(s.color));
    return ok;
}

RuntimeInfo load_runtime_info(const std::wstring& path) {
    RuntimeInfo r;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return r;
    r.width = read_int(path, L"runtime", L"recommended_width", 0);
    r.height = read_int(path, L"runtime", L"recommended_height", 0);
    r.maxWidth = read_int(path, L"runtime", L"max_width", 0);
    r.maxHeight = read_int(path, L"runtime", L"max_height", 0);
    const int eye = read_int(path, L"runtime", L"eye_tracking", -1);
    r.eyeTracker = eye == 1 ? EyeTracker::Detected : eye == 0 ? EyeTracker::Missing : EyeTracker::Unknown;
    return r;
}

Readouts readouts(const Settings& s, const RuntimeInfo& runtime) {
    Readouts r;
    const auto scaled = [](int base, double fraction) { return static_cast<int>(std::lround(base * fraction)); };
    r.outer = {scaled(runtime.width, s.outer / 100.0), scaled(runtime.height, s.outer / 100.0)};
    if (s.mode == RenderMode::Qvfr) {
        // The kit's QVFR.viewPixels: focus px = recommended x inner % x foveated size %.
        r.focus = {scaled(runtime.width, s.inner / 100.0 * s.width / 100.0),
                   scaled(runtime.height, s.inner / 100.0 * s.height / 100.0)};
        r.target = {2 * (r.outer.width + r.focus.width), std::max(r.outer.height, r.focus.height)};
        r.pixels = 2.0 * (static_cast<double>(r.outer.width) * r.outer.height +
                          static_cast<double>(r.focus.width) * r.focus.height);
        r.focusDensity = s.inner / 100.0;
    } else {
        // Two frames per headset frame, both at the outer size; frame B spans the foveated size.
        r.focus = r.outer;
        r.target = {2 * r.outer.width, r.outer.height};
        r.pixels = 4.0 * r.outer.width * r.outer.height;
        r.focusDensity = static_cast<double>(s.outer) / s.width;
    }
    // ACE's double-wide target must fit the runtime's maximum width, so each eye gets half.
    const int eyeWidth = r.target.width / 2;
    r.overRuntimeLimit = (runtime.maxWidth > 0 && eyeWidth > runtime.maxWidth / 2) ||
                         (runtime.maxHeight > 0 && r.target.height > runtime.maxHeight);
    return r;
}

}  // namespace aceqv::launcher

#include "settings.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <filesystem>
#include <string>

#include <windows.h>

namespace aceqv::launcher {
namespace {

// The views4 call sites of the tested QVFR configuration (recon "Darkness fixed").
constexpr wchar_t kQvfrCallers[] = L"init,draw,tiles,slots,cameras,fog,consts,final,oitpass,dlss";

int read_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

std::wstring read_string(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[256] = {};
    GetPrivateProfileStringW(section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)), ini.c_str());
    return buffer;
}

// Signed: GetPrivateProfileInt reads negative values as 0.
int read_signed(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    const std::wstring text = read_string(ini, section, key);
    return text.empty() ? fallback : _wtoi(text.c_str());
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
    s.menuYaw = clamp_to(read_signed(ini, L"menu", L"yaw", s.menuYaw), kMenuYawRange);
    s.menuPitch = clamp_to(read_signed(ini, L"menu", L"pitch", s.menuPitch), kMenuPitchRange);
    s.menuWidth = clamp_to(read_signed(ini, L"menu", L"width", s.menuWidth), kMenuWidthRange);
    s.cheeky = read_cheeky_settings(ini.c_str());
    s.osd = read_osd_placement(ini.c_str());
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
    ok &= write_cheeky_settings(ini.c_str(), s.cheeky, true);
    return save_live_settings(ini, s) && ok;
}

bool save_live_settings(const std::wstring& ini, const Settings& in) {
    Settings s = in;
    s.width = clamp_to(s.width, kSizeRange);
    s.height = clamp_to(s.height, kSizeRange);
    s.feather = clamp_to(s.feather, kFeatherRange);
    s.corner = clamp_to(s.corner, kCornerRange);
    const auto n = [](int v) { return std::to_wstring(v); };
    bool ok = true;
    if (s.mode == RenderMode::Qvfr) {  // QVFR resizes its views live, up to their launch size
        ok &= write(ini, L"experiment", L"periphery_scale", n(clamp_to(s.outer, kOuterRange)));
        ok &= write(ini, L"experiment", L"focus_density", n(clamp_to(s.inner, kInnerRange)));
    }
    ok &= write(ini, L"experiment", L"focus_percent", n(s.width));
    ok &= write(ini, L"experiment", L"focus_height_percent", n(s.height));
    ok &= write(ini, L"quad", L"feather_percent", n(s.feather));
    ok &= write(ini, L"quad", L"corner_radius_percent", n(s.corner));
    ok &= write(ini, L"quad", L"gaze", s.eyeTracking ? L"1" : L"0");
    ok &= write(ini, L"quad", L"debug_border", s.outline ? L"1" : L"0");
    ok &= write(ini, L"quad", L"debug_border_color", color_name(s.color));
    ok &= write(ini, L"menu", L"yaw", n(clamp_to(s.menuYaw, kMenuYawRange)));
    ok &= write(ini, L"menu", L"pitch", n(clamp_to(s.menuPitch, kMenuPitchRange)));
    ok &= write(ini, L"menu", L"width", n(clamp_to(s.menuWidth, kMenuWidthRange)));
    ok &= write_cheeky_settings(ini.c_str(), s.cheeky, false);  // the layer applies them to Cheeky live
    ok &= write_osd_placement(ini.c_str(), s.osd);
    return ok;
}

std::wstring cheeky_runtime_path(const std::wstring& ini, const std::wstring& layerFolder) {
    const auto absolute = [&](const std::wstring& path) {
        const std::filesystem::path p(path);
        return (p.is_absolute() ? p : std::filesystem::path(layerFolder) / p).lexically_normal().wstring();
    };
    const std::wstring runtime = read_string(ini, L"cheeky", L"runtime");
    if (!runtime.empty())
        return absolute(runtime);
    std::wstring configDir = read_string(ini, L"cheeky", L"config_dir");
    if (configDir.empty())
        configDir = L"CheekyFoveatedDLSS";
    return (std::filesystem::path(absolute(configDir)) / L"CheekyFoveatedDLSSRuntime.dll").wstring();
}

bool save_cheeky_runtime(const std::wstring& ini, const std::wstring& runtime) {
    return write(ini, L"cheeky", L"runtime", runtime);
}

CheekyLayer cheeky_layer_state() {
    // Implicit layers: value name = manifest path, data 0 = enabled (the OpenXR loader's convention).
    CheekyLayer state = CheekyLayer::Missing;
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        wchar_t name[1024];
        for (DWORD i = 0;; ++i) {
            DWORD nameLength = static_cast<DWORD>(std::size(name)), data = 1, dataSize = sizeof(data), type = 0;
            if (RegEnumValueW(key, i, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize) !=
                ERROR_SUCCESS)
                break;
            std::wstring lower(name, nameLength);
            for (wchar_t& ch : lower)
                ch = static_cast<wchar_t>(towlower(ch));
            if (lower.find(L"xr_apilayer_cheeky") == std::wstring::npos)
                continue;
            if (type == REG_DWORD && data == 0)
                state = CheekyLayer::Enabled;
            else if (state == CheekyLayer::Missing)
                state = CheekyLayer::Disabled;
        }
        RegCloseKey(key);
    }
    return state;
}

std::wstring running_live_ini(const std::wstring& runtimeInfo) {
    const std::wstring ini = read_string(runtimeInfo, L"runtime", L"live_ini");
    const DWORD pid = static_cast<DWORD>(read_int(runtimeInfo, L"runtime", L"live_pid", 0));
    if (ini.empty() || pid == 0)
        return {};
    // That process, still running, and still ACE (not a reused id).
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    DWORD code = 0;
    wchar_t path[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
                         QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    const std::wstring exe = path;
    const std::wstring ace = L"\\AssettoCorsaEVO.exe";
    if (!running || exe.size() < ace.size() || _wcsicmp(exe.c_str() + exe.size() - ace.size(), ace.c_str()) != 0)
        return {};
    return ini;
}

LaunchSizes load_launch_sizes(const std::wstring& runtimeInfo) {
    LaunchSizes l;
    const std::wstring mode = read_string(runtimeInfo, L"runtime", L"launch_mode");
    l.known = !mode.empty();
    l.mode = _wcsicmp(mode.c_str(), L"quad") == 0 ? RenderMode::TwoLoop : RenderMode::Qvfr;
    l.outer = read_int(runtimeInfo, L"runtime", L"launch_outer", 0);
    l.focusWidth = read_int(runtimeInfo, L"runtime", L"launch_focus_width", 0);
    l.focusHeight = read_int(runtimeInfo, L"runtime", L"launch_focus_height", 0);
    return l;
}

bool fits_launch(const Settings& s, const LaunchSizes& launch) {
    if (!launch.known || s.mode != launch.mode)
        return false;
    if (s.mode == RenderMode::TwoLoop)
        return s.outer == launch.outer;
    // The focus views' size against the native eye, as the layer computes it (percent, rounded there).
    const double focusWidth = s.inner * s.width / 100.0, focusHeight = s.inner * s.height / 100.0;
    return s.outer <= launch.outer && focusWidth <= launch.focusWidth + 0.5 && focusHeight <= launch.focusHeight + 0.5;
}


RuntimeInfo load_runtime_info(const std::wstring& path) {
    RuntimeInfo r;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return r;
    r.width = read_int(path, L"runtime", L"recommended_width", 0);
    r.height = read_int(path, L"runtime", L"recommended_height", 0);
    r.maxWidth = read_int(path, L"runtime", L"max_width", 0);
    r.maxHeight = read_int(path, L"runtime", L"max_height", 0);
    r.centerPpd = _wtof(read_string(path, L"runtime", L"native_center_ppd").c_str());
    const int eye = read_int(path, L"runtime", L"eye_tracking", -1);
    r.eyeTracker = eye == 1 ? EyeTracker::Detected : eye == 0 ? EyeTracker::Missing : EyeTracker::Unknown;
    r.cheekyStatus = read_string(path, L"runtime", L"cheeky_status");
    r.cheekyVersion = read_string(path, L"runtime", L"cheeky_version");
    return r;
}

Readouts readouts(const Settings& s, const RuntimeInfo& runtime) {
    Readouts r;
    const auto scaled = [](int base, double fraction) { return static_cast<int>(std::lround(base * fraction)); };
    const auto area = [](Size v) { return static_cast<double>(v.width) * v.height; };
    r.outer = {scaled(runtime.width, s.outer / 100.0), scaled(runtime.height, s.outer / 100.0)};
    if (s.mode == RenderMode::Qvfr) {
        // The kit's QVFR.viewPixels: focus px = recommended x inner % x foveated size %.
        r.focus = {scaled(runtime.width, s.inner / 100.0 * s.width / 100.0),
                   scaled(runtime.height, s.inner / 100.0 * s.height / 100.0)};
        r.target = {2 * (r.outer.width + r.focus.width), std::max(r.outer.height, r.focus.height)};
        r.focusDensity = s.inner / 100.0;
    } else {
        // Two frames per headset frame, both at the outer size; frame B spans the foveated size.
        r.focus = r.outer;
        r.target = {2 * r.outer.width, r.outer.height};
        r.focusDensity = static_cast<double>(s.outer) / s.width;
    }
    // ACE's double-wide target must fit the runtime's maximum width, so each eye gets half.
    const int eyeWidth = r.target.width / 2;
    const int maxEyeWidth = runtime.maxWidth > 0 ? std::max(64, runtime.maxWidth / 2) : 0;
    const int maxEyeHeight = runtime.maxHeight > 0 ? std::max(64, runtime.maxHeight) : 0;
    const double sx = maxEyeWidth > 0 && eyeWidth > maxEyeWidth ? static_cast<double>(maxEyeWidth) / eyeWidth : 1.0;
    const double sy = maxEyeHeight > 0 && r.target.height > maxEyeHeight ? static_cast<double>(maxEyeHeight) / r.target.height : 1.0;
    r.overRuntimeLimit = sx < 1.0 || sy < 1.0;
    if (r.overRuntimeLimit) {
        // The layer clamps the offered eye to the maximum and every view keeps its share of it, so all
        // of them shrink by the same factor on that axis.
        const auto shrink = [&](Size v) { return Size{scaled(v.width, sx), scaled(v.height, sy)}; };
        r.outer = shrink(r.outer);
        r.focus = shrink(r.focus);
        r.target = shrink(r.target);
    }
    // QVFR: two outer and two focus views. Two-loop: two frames of two outer-sized eyes.
    r.pixels = s.mode == RenderMode::Qvfr ? 2.0 * (area(r.outer) + area(r.focus)) : 4.0 * area(r.outer);
    r.runtime = {runtime.width, runtime.height};
    r.runtimePixels = 2.0 * area(r.runtime);
    r.saved = r.runtimePixels > 0 ? 1.0 - r.pixels / r.runtimePixels : 0;
    r.centerPpd = runtime.centerPpd * r.focusDensity * sx;  // a runtime-maximum clamp lowers the density too
    return r;
}

}  // namespace aceqv::launcher

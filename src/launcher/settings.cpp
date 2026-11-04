#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
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
    s.offset = read_focus_offset(ini.c_str());
    s.gazeDeadzone = read_gaze_deadzone(ini.c_str());
    s.shape = read_focus_shape(ini.c_str());
    s.outline = read_int(ini, L"quad", L"debug_border", 0) != 0;
    s.cpuProfile = read_int(ini, L"experiment", L"cpu_profile", 0) != 0;
    s.baselineNext = read_int(ini, L"launcher", L"baseline_next", 0) != 0;
    const std::wstring color = read_string(ini, L"quad", L"debug_border_color");
    s.color = _wcsicmp(color.c_str(), L"magenta") == 0 ? OutlineColor::Magenta
              : _wcsicmp(color.c_str(), L"green") == 0 ? OutlineColor::Green
                                                       : OutlineColor::Teal;
    s.menuYaw = clamp_to(read_signed(ini, L"menu", L"yaw", s.menuYaw), kMenuYawRange);
    s.menuPitch = clamp_to(read_signed(ini, L"menu", L"pitch", s.menuPitch), kMenuPitchRange);
    s.menuWidth = clamp_to(read_signed(ini, L"menu", L"width", s.menuWidth), kMenuWidthRange);
    s.cheeky = read_cheeky_settings(ini.c_str());
    s.osd = read_osd_placement(ini.c_str());
    s.crop = read_int(ini, L"crop", L"enabled", 0) != 0;
    s.cropTop = clamp_to(read_int(ini, L"crop", L"top", 0), kCropRange);
    s.cropBottom = clamp_to(read_int(ini, L"crop", L"bottom", 0), kCropRange);
    s.cropOuter = clamp_to(read_int(ini, L"crop", L"outer", 0), kCropRange);
    s.cropInner = clamp_to(read_int(ini, L"crop", L"inner", 0), kCropRange);
    s.helmet = read_helmet_settings(ini.c_str());
    s.hotkeys = read_hotkeys(ini.c_str());
    return s;
}

double Settings::crop_width() const {
    return mode == RenderMode::Qvfr && crop ? 1 - (clamp_to(cropOuter, kCropRange) + clamp_to(cropInner, kCropRange)) / 100.0 : 1;
}

double Settings::crop_height() const {
    return mode == RenderMode::Qvfr && crop ? 1 - (clamp_to(cropTop, kCropRange) + clamp_to(cropBottom, kCropRange)) / 100.0 : 1;
}

std::wstring crop_key(const Settings& s) {
    if (s.mode != RenderMode::Qvfr || !s.crop)
        return L"0,0,0,0";
    const auto n = [](int v) { return std::to_wstring(clamp_to(v, kCropRange)); };
    return n(s.cropTop) + L"," + n(s.cropBottom) + L"," + n(s.cropOuter) + L"," + n(s.cropInner);
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
    ok &= write(ini, L"experiment", L"cpu_profile", s.cpuProfile ? L"1" : L"0");
    ok &= write(ini, L"launcher", L"baseline_next", s.baselineNext ? L"1" : L"0");
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
        ok &= write(ini, L"crop", L"enabled", s.crop ? L"1" : L"0");  // and the FOV crop
        ok &= write(ini, L"crop", L"top", n(clamp_to(s.cropTop, kCropRange)));
        ok &= write(ini, L"crop", L"bottom", n(clamp_to(s.cropBottom, kCropRange)));
        ok &= write(ini, L"crop", L"outer", n(clamp_to(s.cropOuter, kCropRange)));
        ok &= write(ini, L"crop", L"inner", n(clamp_to(s.cropInner, kCropRange)));
        ok &= write_helmet_settings(ini.c_str(), s.helmet);
    }
    ok &= write(ini, L"experiment", L"focus_percent", n(s.width));
    ok &= write(ini, L"experiment", L"focus_height_percent", n(s.height));
    ok &= write(ini, L"quad", L"feather_percent", n(s.feather));
    ok &= write(ini, L"quad", L"corner_radius_percent", n(s.corner));
    ok &= write(ini, L"quad", L"gaze", s.eyeTracking ? L"1" : L"0");
    ok &= write_focus_offset(ini.c_str(), s.offset);
    ok &= write_gaze_deadzone(ini.c_str(), s.gazeDeadzone);
    ok &= write_focus_shape(ini.c_str(), s.shape);
    ok &= write(ini, L"quad", L"debug_border",s.outline ? L"1" : L"0");
    ok &= write(ini, L"quad", L"debug_border_color", color_name(s.color));
    ok &= write(ini, L"menu", L"yaw", n(clamp_to(s.menuYaw, kMenuYawRange)));
    ok &= write(ini, L"menu", L"pitch", n(clamp_to(s.menuPitch, kMenuPitchRange)));
    ok &= write(ini, L"menu", L"width", n(clamp_to(s.menuWidth, kMenuWidthRange)));
    ok &= write_cheeky_settings(ini.c_str(), s.cheeky, false);  // the layer applies them to Cheeky live
    ok &= write_osd_placement(ini.c_str(), s.osd);
    ok &= write_hotkeys(ini.c_str(), s.hotkeys);
    return ok;
}

Profile settings_profile(const Settings& settings, const std::wstring& name) {
    // Written by save_settings into a scratch profiles folder, then read back as a profile of it.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path folder =
        std::filesystem::path(temp) / (L"aceqv-profile-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    save_settings((folder / L"scratch.ini").wstring(), settings);
    Profile written;
    load_profile(folder.wstring(), L"scratch", written, true);
    std::filesystem::remove_all(folder, ec);
    Profile p;
    p.name = name;
    for (const ProfileKey& k : written.keys) {
        const bool skip = k.section == L"menu" || k.section == L"osd" || k.section == L"hotkeys" || k.section == L"launcher" ||
                          k.section == L"profiles" || (k.section == L"experiment" && k.key == L"cpu_profile") ||
                          (k.section == L"cheeky" && (k.key == L"runtime" || k.key == L"config_dir"));
        if (!skip)
            p.keys.push_back(k);
    }
    return p;
}

std::string share_profile(const Profile& profile) {
    const Profile defaults = settings_profile(Settings{}, profile.name);
    Profile diff;
    diff.name = profile.name;
    for (const ProfileKey& k : profile.keys)
        if (std::find(defaults.keys.begin(), defaults.keys.end(), k) == defaults.keys.end())
            diff.keys.push_back(k);
    if (diff.keys.empty())  // the defaults themselves: one key, so the text is a profile
        diff.keys.push_back(defaults.keys.front());
    return export_profile(diff);
}

bool shared_profile(const std::string& text, Profile& profile) {
    Profile in;
    if (!import_profile(text, in))
        return false;
    Profile p = settings_profile(Settings{}, in.name);
    for (const ProfileKey& k : in.keys) {
        if (k.section == L"experiment" && k.key == L"mode" && k.value != L"views4" && k.value != L"quad")
            continue;
        for (ProfileKey& mine : p.keys)
            if (_wcsicmp(mine.section.c_str(), k.section.c_str()) == 0 && _wcsicmp(mine.key.c_str(), k.key.c_str()) == 0)
                mine.value = k.value;
    }
    profile = std::move(p);
    return true;
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
    l.crop = read_string(runtimeInfo, L"runtime", L"launch_crop");
    if (l.crop.empty())
        l.crop = L"0,0,0,0";  // a layer from before the crop
    return l;
}

bool fits_launch(const Settings& s, const LaunchSizes& launch) {
    if (!launch.known || s.mode != launch.mode)
        return false;
    if (s.mode == RenderMode::TwoLoop)
        return s.outer == launch.outer;
    // The focus views' size against the native eye, as the layer computes it (percent, rounded there).
    const double focusWidth = s.inner * s.width / 100.0, focusHeight = s.inner * s.height / 100.0;
    // The outer views (periphery density over the crop's share) no larger than at launch.
    int top = 0, bottom = 0, outer = 0, inner = 0;
    swscanf_s(launch.crop.c_str(), L"%d,%d,%d,%d", &top, &bottom, &outer, &inner);
    const double launchWidth = launch.outer * (100 - outer - inner) / 100.0, launchHeight = launch.outer * (100 - top - bottom) / 100.0;
    return s.outer * s.crop_width() <= launchWidth + 0.5 && s.outer * s.crop_height() <= launchHeight + 0.5 &&
           focusWidth <= launch.focusWidth + 0.5 && focusHeight <= launch.focusHeight + 0.5;
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
    r.vramUsedMb = read_int(path, L"runtime", L"vram_used_mb", 0);
    r.vramBudgetMb = read_int(path, L"runtime", L"vram_budget_mb", 0);
    r.vramScaledMb = read_int(path, L"runtime", L"vram_scaled_mb", 0);
    swscanf_s(read_string(path, L"runtime", L"vram_target").c_str(), L"%dx%d", &r.vramTarget.width, &r.vramTarget.height);
    // The last session's measurements (empty keys: not measured).
    SessionInfo& s = r.session;
    const auto number = [&](const wchar_t* key) {
        const std::wstring v = read_string(path, L"runtime", key);
        return v.empty() ? -1.0 : _wtof(v.c_str());
    };
    s.time = read_string(path, L"runtime", L"session_time");
    s.mode = read_string(path, L"runtime", L"session_mode");
    s.limited = read_string(path, L"runtime", L"session_limited");
    s.layersBelow = read_string(path, L"runtime", L"session_layers_below");
    s.log = read_string(path, L"runtime", L"session_log");
    s.cpuProfile = read_string(path, L"runtime", L"session_cpu_profile");
    s.fps = number(L"session_fps");
    s.frameP50 = number(L"session_frame_p50_ms");
    s.frameP90 = number(L"session_frame_p90_ms");
    s.latePercent = number(L"session_late_percent");
    s.refreshHz = number(L"session_refresh_hz");
    s.cpuMs = number(L"session_cpu_ms");
    s.gpuMs = number(L"session_gpu_ms");
    s.gpuBusy = number(L"session_gpu_busy");
    s.cores = number(L"session_cpu_cores");
    s.gazeMs = number(L"session_gaze_ms");
    s.layerMs = number(L"session_layer_ms");
    s.frameGeneration = read_int(path, L"runtime", L"session_frame_generation", 0) != 0;
    // "GameThread=69|Physics=95|(aSeeVRClient.dll)=97": name, share of one core.
    const std::wstring list = read_string(path, L"runtime", L"session_threads");
    for (size_t at = 0; at < list.size();) {
        const size_t bar = list.find(L'|', at), end = bar == std::wstring::npos ? list.size() : bar;
        const std::wstring item = list.substr(at, end - at);
        const size_t eq = item.rfind(L'=');
        if (eq != std::wstring::npos && eq > 0)
            s.threads.push_back({item.substr(0, eq), _wtof(item.c_str() + eq + 1)});
        at = end + 1;
    }
    return r;
}

std::wstring session_summary_text(const RuntimeInfo& r) {
    const SessionInfo& s = r.session;
    if (!s.known())
        return L"ace-quadview: no session measured yet.";
    const auto f = [](const wchar_t* format, double v) {  // at whatever length it comes to
        const int n = _scwprintf(format, v);
        if (n <= 0)
            return std::wstring();
        std::wstring text(static_cast<size_t>(n), L'\0');
        swprintf_s(text.data(), static_cast<size_t>(n) + 1, format, v);
        return text;
    };
    std::wstring t = L"ace-quadview session " + s.time + L" · " + s.mode;
    if (s.refreshHz > 0)
        t += f(L" · %.0f Hz", s.refreshHz);
    t += s.frameGeneration ? L" · frame generation on\n" : L" · frame generation off\n";
    t += f(L"Frame rate: %.1f fps", s.fps);
    if (s.frameP50 > 0)
        t += f(L"; frame time %.2f ms median", s.frameP50) + f(L", %.2f ms 90th percentile", s.frameP90);
    if (s.latePercent >= 0)
        t += f(L"; %.1f%% of frames late", s.latePercent);
    t += L"\n";
    if (s.cpuMs >= 0)
        t += f(L"CPU frame time (render thread): %.2f ms\n", s.cpuMs);
    if (s.gpuMs >= 0)
        t += f(L"GPU frame time (whole GPU): %.2f ms", s.gpuMs) + f(L", %.0f%% busy\n", s.gpuBusy);
    if (!s.limited.empty())
        t += L"Limited by: " + s.limited + L"\n";
    if (s.cores >= 0)
        t += f(L"CPU in use: %.1f cores in all\n", s.cores);
    if (!s.threads.empty()) {
        t += L"Busiest threads (share of one core):";
        for (size_t i = 0; i < s.threads.size(); ++i)
            t += (i ? L"," : L"") + std::wstring(L" ") + s.threads[i].first + f(L" %.0f%%", s.threads[i].second);
        t += L"\n";
    }
    if (s.gazeMs >= 0)
        t += f(L"Eye tracking lookup: %.3f ms (own thread)\n", s.gazeMs);
    if (s.layerMs >= 0)
        t += f(L"Layer's own render-thread time: %.2f ms per frame\n", s.layerMs);
    if (r.vramUsedMb > 0)
        t += f(L"VRAM: %.1f GB", r.vramUsedMb / 1024) + f(L" of %.1f GB\n", r.vramBudgetMb / 1024);
    t += L"Layers below: " + (s.layersBelow.empty() ? std::wstring(L"none") : s.layersBelow) + L"\n";
    if (!s.log.empty())
        t += L"Log: " + s.log + L"\n";
    if (!s.cpuProfile.empty())
        t += L"CPU capture: " + s.cpuProfile + L"\n";
    return t;
}

Readouts readouts(const Settings& s, const RuntimeInfo& runtime) {
    Readouts r;
    const auto scaled = [](int base, double fraction) { return static_cast<int>(std::lround(base * fraction)); };
    const auto area = [](Size v) { return static_cast<double>(v.width) * v.height; };
    // QVFR's FOV crop: the outer views keep their density over the kept share of the eye.
    r.outer = {scaled(runtime.width, s.outer / 100.0 * s.crop_width()), scaled(runtime.height, s.outer / 100.0 * s.crop_height())};
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

VramEstimate vram_estimate(const Settings& s, const Readouts& r, const RuntimeInfo& runtime) {
    VramEstimate e;
    const double launchArea = static_cast<double>(runtime.vramTarget.width) * runtime.vramTarget.height;
    if (s.mode != RenderMode::Qvfr || runtime.vramUsedMb <= 0 || launchArea <= 0)
        return e;
    const double area = static_cast<double>(r.target.width) * r.target.height;
    const double scaled = std::min(runtime.vramScaledMb, runtime.vramUsedMb);
    e.mb = runtime.vramUsedMb - scaled + scaled * area / launchArea;
    e.budgetMb = runtime.vramBudgetMb;
    return e;
}

}  // namespace aceqv::launcher

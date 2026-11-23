#include "settings.h"

#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>

#include <windows.h>

#include "../games/game.h"
#include "settings_ini.h"

// Cheeky's runtime and layer, the DLSS dll path, the running game's live ini, launch sizes and runtime info.
namespace simqvfr::launcher {

using settings_ini::read_int;
using settings_ini::read_string;
using settings_ini::write;

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

std::wstring dlss_dll_path(const std::wstring& folder, const std::wstring& layerFolder) {
    const std::wstring& in = folder.empty() ? layerFolder : folder;
    return in.empty() ? std::wstring() : (std::filesystem::path(in) / L"nvngx_dlss.dll").wstring();
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
    const std::wstring ace = L"\\" + std::wstring(games::launcher_game().exeName);
    if (!running || exe.size() < ace.size() || _wcsicmp(exe.c_str() + exe.size() - ace.size(), ace.c_str()) != 0)
        return {};
    return ini;
}

LaunchSizes load_launch_sizes(const std::wstring& runtimeInfo) {
    LaunchSizes l;
    const std::wstring mode = read_string(runtimeInfo, L"runtime", L"launch_mode");
    l.known = !mode.empty();
    l.mode = parse_render_mode(mode.c_str()) == IniRenderMode::TwoLoop ? RenderMode::TwoLoop : RenderMode::Qvfr;
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
    const std::wstring focus = read_string(path, L"runtime", L"session_focus");  // empty: not known (GetPrivateProfileInt reads 0)
    r.sessionFocus = focus == L"1" ? 1 : focus == L"0" ? 0 : -1;
    r.cheekyStatus = read_string(path, L"runtime", L"cheeky_status");
    r.cheekyVersion = read_string(path, L"runtime", L"cheeky_version");
    r.quadViews = read_string(path, L"runtime", L"quad_views");
    r.quadViewsUsed = read_int(path, L"runtime", L"quad_views_used", 0) != 0;
    swscanf_s(read_string(path, L"runtime", L"quad_runtime_focus").c_str(), L"%dx%d", &r.quadFocusWidth, &r.quadFocusHeight);
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
    s.stint = read_int(path, L"runtime", L"session_stint", 0);
    s.seconds = number(L"session_stint_seconds");
    s.fps = number(L"session_fps");
    s.frameP50 = number(L"session_frame_p50_ms");
    s.frameP90 = number(L"session_frame_p90_ms");
    s.latePercent = number(L"session_late_percent");
    s.refreshHz = number(L"session_refresh_hz");
    s.halfRatePercent = number(L"session_half_rate_percent");
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

}  // namespace simqvfr::launcher

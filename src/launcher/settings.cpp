#include "settings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include <windows.h>

#include "../games/game.h"
#include "../ini.h"
#include "game_tabs.h"
#include "settings_ini.h"

// Load, save and defaults, and profiles of the settings. Per-game copies: settings_per_game.cpp; Cheeky, launch
// sizes and runtime info: settings_runtime.cpp; readouts and slider maths: settings_readouts.cpp.
namespace simqvfr::launcher {

using settings_ini::clamp_to;
using settings_ini::read_int;
using settings_ini::read_signed;
using settings_ini::read_string;
using settings_ini::write;

namespace {

// The views4 call sites of the tested QVFR configuration (recon "Darkness fixed").
constexpr wchar_t kQvfrCallers[] = L"init,draw,tiles,slots,cameras,fog,consts,final,oitpass,dlss";

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
    const wchar_t* const f = ini.c_str();
    s.mode = parse_render_mode(read_string(ini, L"experiment", L"mode").c_str()) == IniRenderMode::TwoLoop ? RenderMode::TwoLoop
                                                                                                          : RenderMode::Qvfr;
    s.width = ini_int(f, kFocusPercent);
    s.height = ini_int(f, kFocusHeightPercent, s.width);
    const int eyeScale = ini_raw(f, kEyeScale, kEyeScale.def);
    const int periphery = ini_raw(f, kPeripheryScale, 0);
    s.outer = kPeripheryScale.clamp(s.mode == RenderMode::Qvfr && periphery ? periphery : eyeScale);
    // Unset, the layer derives the density from eye_scale (equal quarters); show that value.
    const int density = ini_raw(f, kFocusDensity, 0);
    s.inner = kFocusDensity.clamp(density ? density : static_cast<int>(std::lround(100.0 * kEyeScale.clamp(eyeScale) / s.width)));
    s.objectDetail = ini_int(f, kObjectDetail);
    s.sharpen = ini_int(f, kSharpenPercent);
    s.grade = read_color_grade(f);
    s.carLight = read_car_lighting(f);
    s.eyeOff = ini_int(f, kEyeOff);
    s.nativeQuad = std::clamp(read_int(ini, L"experiment", L"native_quad_views", 1), 0, 2);
    if (read_int(ini, L"launcher", L"native_quad_reset", 0) == 0)  // its card is gone: an off it saved goes back to auto
        s.nativeQuad = 1;
    s.feather = ini_int(f, kFeatherPercent);
    s.corner = ini_int(f, kCornerRadiusPercent);
    s.focusMask = ini_flag(f, kFocusMask);
    s.focusMaskMargin = ini_int(f, kFocusMaskMargin);
    s.eyeTracking = ini_flag(f, kGaze);
    s.offset = read_focus_offset(f);
    s.gazeDeadzone = read_gaze_deadzone(f);
    s.shape = read_focus_shape(f);
    s.outline = ini_flag(f, kDebugBorder);
    s.cpuProfile = read_int(ini, L"experiment", L"cpu_profile", 0) != 0;
    s.frameCapture = read_int(ini, L"experiment", L"d3d11_capture_hotkey", 0) != 0;
    s.baselineNext = read_int(ini, L"launcher", L"baseline_next", 0) != 0;
    s.debugLogs = std::clamp(read_int(ini, L"launcher", L"debug_logs", 0), 0, kDebugLogLaunches);
    s.antiCheat = read_int(ini, L"launcher", L"anticheat", 0) != 0;
    s.updateCheck = read_int(ini, L"launcher", L"update_check", 0) != 0;
    s.updateAsked = read_int(ini, L"launcher", L"update_asked", 0) != 0;
    s.updateSkip = read_string(ini, L"launcher", L"update_skip");
    s.hiddenGames = read_string(ini, L"launcher", L"hidden_games");
    s.shownGames = read_string(ini, L"launcher", L"shown_games");
    {
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::wstring id = games::wide_id(*all[i]);
            uint32_t rgb = 0;
            if (parse_color(read_string(ini, L"launcher", (L"color_" + id).c_str()), rgb))
                s.gameColors[id] = rgb;
        }
    }
    const BorderColor color = read_border_color(f);
    s.color = color == BorderColor::Magenta ? OutlineColor::Magenta : color == BorderColor::Teal ? OutlineColor::Teal : OutlineColor::Green;
    s.menuYaw = clamp_to(read_signed(ini, L"menu", L"yaw", s.menuYaw), kMenuYawRange);
    s.menuPitch = clamp_to(read_signed(ini, L"menu", L"pitch", s.menuPitch), kMenuPitchRange);
    s.menuWidth = clamp_to(read_signed(ini, L"menu", L"width", s.menuWidth), kMenuWidthRange);
    s.hotkeyFooter = ini_flag(f, kHotkeyFooter);
    s.cpuIsolateMain = ini_flag(f, kCpuIsolateMain);
    s.cheeky = read_cheeky_settings(ini.c_str());
    s.dlss = read_dlss_settings(ini.c_str());
    s.nr = read_nr_settings(ini.c_str());
    s.osd = read_osd_placement(ini.c_str());
    s.clock = read_clock_placement(ini.c_str());
    s.crop = ini_flag(f, kCropEnabled);
    s.cropTop = ini_int(f, kCropTop);
    s.cropBottom = ini_int(f, kCropBottom);
    s.cropOuter = ini_int(f, kCropOuter);
    s.cropInner = ini_int(f, kCropInner);
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

Settings restored_defaults(const Settings& current) {
    Settings s;
    s.hotkeys = current.hotkeys;  // the Hotkeys pane's own Reset hotkeys
    s.updateCheck = current.updateCheck;
    s.updateAsked = current.updateAsked;
    s.updateSkip = current.updateSkip;
    s.hiddenGames = current.hiddenGames;
    s.shownGames = current.shownGames;
    s.gameColors = current.gameColors;
    s.nr.folder = current.nr.folder;  // where the player's nvngx_dlssnr.dll is: not a setting to lose (Sim-QVFR has none)
    return s;  // [dlss] folder too: back to the nvngx_dlss.dll Sim-QVFR includes
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
        ok &= write(ini, kPeripheryScale, n(s.outer));
        ok &= write(ini, kFocusDensity, n(s.inner));
    } else {
        ok &= write(ini, L"experiment", L"mode", L"quad");
    }
    ok &= write(ini, kEyeScale, n(s.outer));  // Two-loop's resolution; QVFR's fallback
    ok &= write(ini, L"experiment", L"native_quad_views", n(s.nativeQuad));    // LMU's, at the next launch
    ok &= write(ini, L"launcher", L"native_quad_reset", L"1");  // the card's old off reset once (load_settings)
    ok &= write(ini, L"experiment", L"cpu_profile", s.cpuProfile ? L"1" : L"0");
    ok &= write(ini, L"experiment", L"d3d11_capture_hotkey", s.frameCapture ? L"1" : L"0");
    ok &= write(ini, L"launcher", L"baseline_next", s.baselineNext ? L"1" : L"0");
    ok &= write(ini, L"launcher", L"debug_logs", std::to_wstring(s.debugLogs));
    ok &= write(ini, L"launcher", L"anticheat", s.antiCheat ? L"1" : L"0");  // at the next launch
    ok &= write(ini, L"launcher", L"update_check", s.updateCheck ? L"1" : L"0");
    ok &= write(ini, L"launcher", L"update_asked", s.updateAsked ? L"1" : L"0");
    ok &= write(ini, L"launcher", L"update_skip", s.updateSkip);
    ok &= write(ini, L"launcher", L"hidden_games", s.hiddenGames);
    ok &= write(ini, L"launcher", L"shown_games", s.shownGames);
    {
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::wstring id = games::wide_id(*all[i]), key = L"color_" + id;
            const auto it = s.gameColors.find(id);
            ok &= WritePrivateProfileStringW(L"launcher", key.c_str(), it == s.gameColors.end() ? nullptr : color_text(it->second).c_str(),
                                             ini.c_str()) != 0 || it == s.gameColors.end();  // no colour: the key goes
        }
    }
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
        ok &= write(ini, kPeripheryScale, n(clamp_to(s.outer, kOuterRange)));
        ok &= write(ini, kFocusDensity, n(clamp_to(s.inner, kInnerRange)));
        ok &= write(ini, kCropEnabled, s.crop ? L"1" : L"0");  // and the FOV crop
        ok &= write(ini, kCropTop, n(clamp_to(s.cropTop, kCropRange)));
        ok &= write(ini, kCropBottom, n(clamp_to(s.cropBottom, kCropRange)));
        ok &= write(ini, kCropOuter, n(clamp_to(s.cropOuter, kCropRange)));
        ok &= write(ini, kCropInner, n(clamp_to(s.cropInner, kCropRange)));
        ok &= write_helmet_settings(ini.c_str(), s.helmet);
        ok &= write(ini, kObjectDetail, n(clamp_to(s.objectDetail, kObjectDetailRange)));  // X-Plane's
        ok &= write(ini, kSharpenPercent, n(clamp_to(s.sharpen, kSharpenRange)));
        ok &= write_color_grade(ini.c_str(), s.grade);
        ok &= write_car_lighting(ini.c_str(), s.carLight);
    }
    ok &= write(ini, kFocusPercent, n(s.width));
    ok &= write(ini, kFocusHeightPercent, n(s.height));
    ok &= write(ini, kFeatherPercent, n(s.feather));
    ok &= write(ini, kCornerRadiusPercent, n(s.corner));
    ok &= write(ini, kGaze, s.eyeTracking ? L"1" : L"0");
    ok &= write(ini, kEyeOff, n(kEyeOff.clamp(s.eyeOff)));  // QVFR or not (the layer's black eye)
    ok &= write_focus_offset(ini.c_str(), s.offset);
    ok &= write_gaze_deadzone(ini.c_str(), s.gazeDeadzone);
    ok &= write_focus_shape(ini.c_str(), s.shape);
    ok &= write(ini, kDebugBorder, s.outline ? L"1" : L"0");
    ok &= write(ini, kBorderColorSection, kBorderColorKey, color_name(s.color));
    ok &= write(ini, L"menu", L"yaw", n(clamp_to(s.menuYaw, kMenuYawRange)));
    ok &= write(ini, L"menu", L"pitch", n(clamp_to(s.menuPitch, kMenuPitchRange)));
    ok &= write(ini, L"menu", L"width", n(clamp_to(s.menuWidth, kMenuWidthRange)));
    ok &= write(ini, kHotkeyFooter, s.hotkeyFooter ? L"1" : L"0");
    ok &= write(ini, kCpuIsolateMain, s.cpuIsolateMain ? L"1" : L"0");
    ok &= write_cheeky_settings(ini.c_str(), s.cheeky, false);  // the layer applies them to Cheeky live
    ok &= write_dlss_settings(ini.c_str(), s.dlss);  // and Sim-QVFR's own DLSS to the game (but the folder: next launch)
    ok &= write_nr_settings(ini.c_str(), s.nr);      // and DLSS-NR on its views (the folder likewise)
    ok &= write_osd_placement(ini.c_str(), s.osd);
    ok &= write_clock_placement(ini.c_str(), s.clock);
    ok &= write_hotkeys(ini.c_str(), s.hotkeys);
    return ok;
}

Profile settings_profile(const Settings& settings, const std::wstring& name) {
    // Written by save_settings into a scratch profiles folder, then read back as a profile of it.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path folder =
        std::filesystem::path(temp) / (L"simqvfr-profile-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    save_settings((folder / L"scratch.ini").wstring(), settings);
    Profile written;
    load_profile(folder.wstring(), L"scratch", written, true);
    std::filesystem::remove_all(folder, ec);
    Profile p;
    p.name = name;
    for (const ProfileKey& k : written.keys)
        if (per_game_key(k.section, k.key))
            p.keys.push_back(k);
    return p;
}

bool per_game_key(const std::wstring& section, const std::wstring& key) {
    return !(section == L"menu" || section == L"osd" || section == L"clock" || section == L"hotkeys" || section == L"launcher" ||
             section == L"profiles" || (section == L"experiment" && (key == L"cpu_profile" || key == L"d3d11_capture_hotkey")) ||
             (section == L"cheeky" && (key == L"runtime" || key == L"config_dir")) ||
             (section == L"dlss" && key == L"folder") || (section == L"dlss_nr" && key == L"folder"));
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
        if (k.section == L"experiment" && k.key == L"mode" && k.value != L"views4")  // Two-loop (quad) is legacy: not shared
            continue;
        for (ProfileKey& mine : p.keys)
            if (_wcsicmp(mine.section.c_str(), k.section.c_str()) == 0 && _wcsicmp(mine.key.c_str(), k.key.c_str()) == 0)
                mine.value = k.value;
    }
    profile = std::move(p);
    return true;
}

}  // namespace simqvfr::launcher

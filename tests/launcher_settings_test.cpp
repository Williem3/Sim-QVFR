// Offline check of the launcher's settings (settings.h, the window's model): the shipped QVFR ini, saves and
// reloads, the mode switch, other keys left alone, the runtime report, the kit's readouts, the live settings, and
// every setting the launcher shows surviving a load and save (the UI overhaul's round trip).
#include "launcher_test_kit.h"
#include "quad/settings_menu.h"

#include "quad/focus_mask.h"

namespace {

// Every key of an ini file, "section/key" (lower case) -> value.
std::map<std::wstring, std::wstring> ini_keys(const std::wstring& ini) {
    std::map<std::wstring, std::wstring> out;
    std::vector<wchar_t> sections(64 * 1024), entries(64 * 1024);
    GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), ini.c_str());
    for (const wchar_t* s = sections.data(); *s; s += wcslen(s) + 1) {
        GetPrivateProfileSectionW(s, entries.data(), static_cast<DWORD>(entries.size()), ini.c_str());
        for (const wchar_t* e = entries.data(); *e; e += wcslen(e) + 1) {
            const std::wstring entry = e;
            const size_t eq = entry.find(L'=');
            std::wstring key = std::wstring(s) + L"/" + entry.substr(0, eq);
            std::transform(key.begin(), key.end(), key.begin(), towlower);
            out[key] = eq == std::wstring::npos ? L"" : entry.substr(eq + 1);
        }
    }
    return out;
}

// The UI overhaul's guard (docs/ui/OVERHAUL_PLAN.md): every setting the launcher shows, at a value other than its
// default, survives load and save unchanged (the window's every change is a load_settings, an edit, a save_settings),
// a profile of them round trips, and the window's own [launcher] ui_theme is in no profile and left alone by a save.
void settings_round_trip_checks() {
    const fs::path dir = fs::temp_directory_path() / L"sim-qvfr-round-trip-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    Settings s;
    s.eyeTracking = false;
    s.outer = 37;
    s.inner = 155;
    s.objectDetail = 61;
    s.sharpen = 23;
    s.nativeQuad = 0;
    s.width = 44;
    s.height = 36;
    s.outline = true;
    s.color = OutlineColor::Magenta;
    s.feather = 17;
    s.corner = 12;
    s.shape = simqvfr::focus_shape_of({{-45, 30}, {0, 50}, {45, 30}, {40, -50}, {-40, -50}});
    s.offset.fixed = 3;
    s.offset.gaze = -2;
    s.offset.fixedX = -4;
    s.offset.gazeX = 5;
    s.gazeDeadzone = 22;
    s.menuYaw = -9;
    s.menuPitch = 5;
    s.menuWidth = 41;
    s.hotkeyFooter = false;
    s.osd = {true, 12, -8, 30};
    s.crop = true;
    s.cropTop = 5;
    s.cropBottom = 7;
    s.cropOuter = 9;
    s.cropInner = 3;
    s.helmet.on = true;
    s.helmet.rim = 8;
    s.helmet.corners = 50;
    s.helmet.light = 60;
    s.helmet.size = 120;
    s.helmet.height = -4;
    s.helmet.depth = 20;
    s.helmet.visor = VisorPosition::Half;
    s.helmet.detail = true;
    s.cheeky = {true, false, false, 60, 12, 11, 130, true, true, 2, 2, true, 3, 40, 75, 25, true, 3};
    s.dlss.on = true;
    s.dlss.folder = L"C:\\Games\\dlss";
    s.dlss.focusScale = 67;
    s.dlss.focusPreset = 13;
    s.dlss.eyePreset = 6;
    s.dlss.textureSharpening = 2;
    s.dlss.carVectors = false;
    s.nr.on = true;
    s.nr.folder = L"C:\\Games\\nr";
    s.nr.preset = 1;
    s.nr.style = 2;
    s.nr.intensity = 70;
    s.nr.localTone = 120;
    s.nr.localStructure = 80;
    s.nr.autoMask = true;
    s.nr.periphery = true;
    s.nr.focusScale = 60;
    s.nr.peripheryScale = 40;
    s.nr.workingLimit = 15;
    s.nr.peripheryCoverage = 70;
    s.nr.edgeBlend = 12;
    s.nr.beforeDlss = true;
    s.nr.exposure = 120;
    s.nr.inputGamma = false;
    s.nr.autoExposure = false;
    s.nr.maxGain = 200;
    s.cpuProfile = true;
    s.frameCapture = true;
    s.hotkeys[static_cast<int>(HotkeyAction::Overlay)].key = KeyCombo{'O', true, false, true};
    s.baselineNext = true;
    s.antiCheat = true;
    s.updateCheck = true;
    s.updateAsked = true;
    s.updateSkip = L"v1.2.0-beta";
    s.hiddenGames = L"xplane";
    s.shownGames = L"lmu";
    s.gameColors[L"ace"] = 0x123456;
    for (int i = 0; i < kDevSwitchCount; ++i)  // the Developer tab's switches, each on (an A/B at its own frames)
        s.dev[i] = std::wstring(kDevSwitches[i].key) == L"lmu_car_vectors_ab" ? 300 : kDevSwitches[i].on;
    const std::wstring a = (dir / L"a.ini").wstring(), b = (dir / L"b.ini").wstring();
    check(save_settings(a, s), "round trip: a user's settings, none at its default, save");
    const auto keysA = ini_keys(a);
    check(save_settings(b, load_settings(a)) && ini_keys(b) == keysA,
          "round trip: loaded and saved again, every key comes back with the same value");
    const std::string before = read_text(a);
    save_settings(a, load_settings(a));
    check(read_text(a) == before, "round trip: saving what was loaded leaves the file byte for byte as it was");
    bool spotted = keysA.at(L"quad/gaze") == L"0" && keysA.at(L"experiment/focus_density") == L"155" &&
                   keysA.at(L"helmet/visor") == L"half" && keysA.at(L"cheeky/fg_release") == L"3" &&
                   keysA.at(L"dlss_nr/max_gain") == L"200" && keysA.at(L"launcher/color_ace") == L"123456" &&
                   keysA.at(L"experiment/lmu_car_vectors_ab") == L"300" && keysA.at(L"experiment/views4_menu_scale") == L"100";
    check(spotted, "round trip: the values are the ones set (not defaults)");

    // A profile of them: saved, read back and applied to another ini, it holds the same keys.
    const Profile p = settings_profile(load_settings(a), L"Mine");
    const std::wstring profiles = (dir / L"profiles").wstring();
    Profile q;
    check(save_profile(profiles, p) && load_profile(profiles, L"Mine", q, false) && q.keys == p.keys,
          "round trip: a profile saves and loads with identical keys");
    const std::wstring c = (dir / L"c.ini").wstring();
    save_settings(c, Settings{});
    check(apply_profile(c.c_str(), q) && settings_profile(load_settings(c), L"Mine").keys == p.keys,
          "round trip: applied to fresh settings, a profile gives back the same picture");

    // The Developer tab's switches are in no profile, and [launcher] developer is the user's: a save never writes it.
    check(std::none_of(p.keys.begin(), p.keys.end(),
                       [](const ProfileKey& k) {
                           return std::any_of(std::begin(kDevSwitches), std::end(kDevSwitches),
                                              [&](const DevSwitch& d) { return k.key == d.key; });
                       }) &&
              !keysA.count(L"launcher/developer"),
          "round trip: the Developer tab's switches stay out of profiles; [launcher] developer isn't written");
    WritePrivateProfileStringW(L"launcher", L"developer", L"1", a.c_str());
    check(load_settings(a).developer && restored_defaults(load_settings(a)).developer &&
              restored_defaults(load_settings(a)).dev == DevValues{},
          "round trip: [launcher] developer=1 shows the tab, kept by Restore Defaults, which turns its switches off");
    // The theme button's key: one for every game, kept by a save.
    WritePrivateProfileStringW(L"launcher", L"ui_theme", L"dark", a.c_str());
    save_settings(a, load_settings(a));
    wchar_t theme[16] = {};
    GetPrivateProfileStringW(L"launcher", L"ui_theme", L"", theme, 16, a.c_str());
    check(std::wstring(theme) == L"dark" && !per_game_key(L"launcher", L"ui_theme"),
          "round trip: [launcher] ui_theme stays through a save and is in no profile or game copy");
    fs::remove_all(dir, ec);
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-launcher-settings-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    // Settings (the window's model): the shipped QVFR ini, a save and reload, the mode switch, other
    // keys left alone, the runtime report and the kit's readouts.
    {
        const std::wstring ini = (folder / L"settings.ini").wstring();
        write_file(ini, "; comment kept\r\n[experiment]\r\nmode=views4\r\ndred=1\r\nfocus_percent=31\r\neye_scale=40\r\n"
                        "[quad]\r\nfeather_percent=20\r\ndebug_border=0\r\ndebug_border_color=magenta\r\ngaze=1\r\n");
        const Settings shipped = load_settings(ini);
        check(shipped.outer == 40 && shipped.width == 31 && shipped.height == 31 &&
                  shipped.inner == 129 && shipped.feather == 20 && shipped.eyeTracking && !shipped.outline &&
                  shipped.color == OutlineColor::Magenta,
              "settings: the shipped ini reads as QVFR outer 40%, 31% x 31%, inner 129% (from eye_scale)");

        Settings s = shipped;
        s.outer = 30;
        s.inner = 110;
        s.width = 35;
        s.height = 25;
        s.feather = 12;
        s.corner = 40;
        s.eyeTracking = false;
        s.outline = true;
        s.color = OutlineColor::Teal;
        s.offset = {-6, 3};
        const bool saved = save_settings(ini, s);
        const Settings reread = load_settings(ini);
        std::string iniText;
        read_file(ini, iniText);
        check(saved && reread.outer == 30 && reread.inner == 110 && reread.width == 35 && reread.height == 25 &&
                  reread.feather == 12 && reread.corner == 40 && !reread.eyeTracking && reread.outline && reread.color == OutlineColor::Teal &&
                  iniText.find("periphery_scale=30") != std::string::npos && iniText.find("focus_density=110") != std::string::npos &&
                  iniText.find("focus_height_percent=25") != std::string::npos &&
                  iniText.find("corner_radius_percent=40") != std::string::npos &&
                  iniText.find("views4_compose=1") != std::string::npos && iniText.find("debug_border_color=teal") != std::string::npos,
              "settings: QVFR save writes periphery_scale, focus_density, both focus axes and the views4 keys, and reads back");
        check(iniText.find("; comment kept") != std::string::npos && iniText.find("dred=1") != std::string::npos,
              "settings: comments and other keys are left alone");
        check(reread.offset.fixed == -6 && reread.offset.gaze == 3 && iniText.find("fixed_offset_y=-6") != std::string::npos &&
                  iniText.find("gaze_offset_y=3") != std::string::npos,
              "settings: both vertical offsets are written (negative too) and read back");
        {
            Settings across = reread;
            across.offset.fixedX = -7;
            across.offset.gazeX = 4;
            save_live_settings(ini, across);
            std::string acrossText;
            read_file(ini, acrossText);
            const Settings back = load_settings(ini);
            check(back.offset.fixedX == -7 && back.offset.gazeX == 4 && back.offset.fixed == -6 &&
                      acrossText.find("fixed_offset_x=-7") != std::string::npos &&
                      acrossText.find("gaze_offset_x=4") != std::string::npos,
                  "settings: both horizontal offsets are written live and read back");
            save_settings(ini, reread);
        }
        {
            // The focus shape: a box by default (the shipped ini has no shape keys); a custom outline round-trips.
            Settings f = reread;
            check(!f.shape.custom && f.shape == simqvfr::FocusShape{}, "settings: the focus shape is a box by default");
            f.shape = simqvfr::focus_shape_of({{-25, 50}, {25, 50}, {50, -50}, {-50, -50}, {-60, 0}});  // clamps to -50
            save_live_settings(ini, f);
            std::string shapeText;
            read_file(ini, shapeText);
            const Settings shape = load_settings(ini);
            check(shape.shape.custom && shape.shape.count == 5 && shape.shape.points[0] == simqvfr::FocusPoint{-25, 50} &&
                      shape.shape.points[4] == simqvfr::FocusPoint{-50, 0} &&
                      shapeText.find("focus_shape=points") != std::string::npos &&
                      shapeText.find("shape_points=-25,50 25,50 50,-50 -50,-50 -50,0") != std::string::npos,
                  "settings: the custom shape is written live and read back, its points clamped to the box");
            // An ini from before the outline (focus_shape=corners and the corners' insets) reads as their four points.
            save_settings(ini, reread);
            WritePrivateProfileStringW(L"quad", L"focus_shape", L"corners", ini.c_str());
            WritePrivateProfileStringW(L"quad", L"shape_points", nullptr, ini.c_str());
            WritePrivateProfileStringW(L"quad", L"shape_top_left_in", L"30", ini.c_str());
            WritePrivateProfileStringW(L"quad", L"shape_bottom_right_up", L"10", ini.c_str());
            const Settings legacy = load_settings(ini);
            check(legacy.shape.custom && legacy.shape.count == 4 && legacy.shape.points[0] == simqvfr::FocusPoint{-20, 50} &&
                      legacy.shape.points[1] == simqvfr::FocusPoint{50, 50} &&
                      legacy.shape.points[2] == simqvfr::FocusPoint{50, -40},
                  "settings: the old corner shape reads as its four points");
            WritePrivateProfileStringW(L"quad", L"shape_top_left_in", nullptr, ini.c_str());
            WritePrivateProfileStringW(L"quad", L"shape_bottom_right_up", nullptr, ini.c_str());
            save_settings(ini, reread);
        }
        {
            // The Diagnostics pane's switches: the CPU capture and the one launch without QVFR.
            Settings d = reread;
            d.cpuProfile = true;
            d.baselineNext = true;
            save_settings(ini, d);
            std::string diagText;
            read_file(ini, diagText);
            const Settings diag = load_settings(ini);
            check(diag.cpuProfile && diag.baselineNext && diagText.find("cpu_profile=1") != std::string::npos &&
                      diagText.find("baseline_next=1") != std::string::npos && diagText.find("baseline=") == std::string::npos,
                  "settings: CPU capture and next launch without QVFR are saved and read back ([experiment] baseline untouched)");
            d.cpuProfile = d.baselineNext = false;
            save_settings(ini, d);
            const Settings off = load_settings(ini);
            check(!off.cpuProfile && !off.baselineNext, "settings: and both turn off again");
        }

        // Two-Loop is gone: an ini still set to it reads as QVFR, and is saved back as views4.
        WritePrivateProfileStringW(L"experiment", L"mode", L"quad", ini.c_str());
        save_settings(ini, load_settings(ini));
        read_file(ini, iniText);
        check(iniText.find("mode=views4") != std::string::npos && iniText.find("mode=quad") == std::string::npos,
              "settings: an ini set to Two-loop (removed) reads as QVFR, saved as views4");

        Settings big;
        big.outer = 200;
        big.width = 1;
        save_settings(ini, big);
        const Settings clamped = load_settings(ini);
        check(clamped.outer == kOuterRange.max && clamped.width == kSizeRange.min, "settings: values are clamped to the kit's ranges");

        {
            // Dragging Inner resolution: 5% steps from 50 to 400, and 100% under some whole pixel at any track width
            // (a plain 1% drag over ~230 px went 99 -> 101).
            bool hundred = true, onGrid = true;
            for (int travel = 120; travel <= 900; ++travel) {
                bool found = false;
                for (int px = 0; px <= travel; ++px) {
                    const int at = slider_value_at(kInnerRange, static_cast<double>(px) / travel, kInnerDragStep);
                    found |= at == 100;
                    onGrid &= at >= kInnerRange.min && at <= kInnerRange.max && (at - kInnerRange.min) % kInnerDragStep == 0;
                }
                hundred &= found;
            }
            check(hundred && onGrid && slider_value_at(kInnerRange, 0.0, kInnerDragStep) == kInnerRange.min &&
                      slider_value_at(kInnerRange, 1.0, kInnerDragStep) == kInnerRange.max &&
                      slider_value_at(kInnerRange, 2.0, kInnerDragStep) == kInnerRange.max &&
                      slider_value_at(kInnerRange, -1.0, kInnerDragStep) == kInnerRange.min,
                  "slider: Inner resolution drags in 5% steps, 100% reachable at any width, ends at 50% and 400%");
            check(slider_value_at(kOuterRange, 0.5) == 55 && slider_value_at(kOuterRange, 0.0) == 10 &&
                      slider_value_at(kOuterRange, 1.0) == 100 && slider_value_at({0, 50}, 0.31) == 16,
                  "slider: other sliders keep 1% steps");
        }

        Settings placed;
        placed.menuYaw = -20;
        placed.menuPitch = -12;
        placed.menuWidth = 45;
        save_settings(ini, placed);
        const Settings menu = load_settings(ini);
        read_file(ini, iniText);
        check(menu.menuYaw == -20 && menu.menuPitch == -12 && menu.menuWidth == 45 && iniText.find("[menu]") != std::string::npos &&
                  iniText.find("yaw=-20") != std::string::npos,
              "settings: the headset menu's place round-trips through [menu], negative degrees included");
        placed.menuYaw = 90;
        placed.menuWidth = 5;
        save_settings(ini, placed);
        const Settings outside = load_settings(ini);
        check(outside.menuYaw == kMenuYawRange.max && outside.menuWidth == kMenuWidthRange.min,
              "settings: the headset menu's place is clamped to its ranges");

        // Cheeky Foveated DLSS: [cheeky] round-trips; the live save leaves `enabled` (next launch) alone.
        Settings dlss = placed;
        dlss.cheeky = {true, false, true, 60, 12, 10};
        dlss.cheeky.supersampling = 150;
        dlss.cheeky.motionFix = dlss.cheeky.nr = true;
        dlss.cheeky.nrPreset = 1;
        dlss.cheeky.nrPasses = 2;
        dlss.cheeky.fg = true;
        dlss.cheeky.fgMultiplier = 3;
        dlss.cheeky.fgSize = 40;
        dlss.cheeky.fgScale = 75;
        dlss.cheeky.fgPeriphery = 25;
        save_settings(ini, dlss);
        check(load_settings(ini).cheeky == dlss.cheeky, "settings: [cheeky] round-trips (load, processing, outer, presets, supersampling, motion fix, DLSS-NR, frame generation)");
        Settings dlssLive = dlss;
        dlssLive.cheeky.load = false;
        dlssLive.cheeky.outerScale = 40;
        save_live_settings(ini, dlssLive);
        const simqvfr::CheekySettings afterLive = load_settings(ini).cheeky;
        check(afterLive.load && afterLive.outerScale == 40, "settings: the live save changes Cheeky's settings, not enabled");
        dlssLive.osd = {true, -15, 22, 30};
        save_live_settings(ini, dlssLive);
        check(load_settings(ini).osd == dlssLive.osd, "settings: [osd] (the performance overlay) round-trips, live");
        dlssLive.clock = {true, -18, 14, 12};
        save_live_settings(ini, dlssLive);
        check(load_settings(ini).clock == dlssLive.clock && load_settings(ini).osd == dlssLive.osd,
              "settings: [clock] round-trips, live, beside [osd]");
        check(!per_game_key(L"clock", L"yaw"), "settings: the clock's place is every game's, as the overlay's");
        const simqvfr::MenuPlacement layerMenu;
        check(Settings{}.menuYaw == layerMenu.yaw && Settings{}.menuPitch == layerMenu.pitch &&
                  Settings{}.menuWidth == layerMenu.width,
              "settings: the launcher's default menu place is the layer's");
        Settings cropSave = load_settings(ini);
        cropSave.crop = true;
        cropSave.cropTop = 30;
        cropSave.cropBottom = 55;  // above kCropRange: clamped on its own
        cropSave.cropOuter = 12;
        cropSave.cropInner = 0;
        save_settings(ini, cropSave);
        const Settings cropBack = load_settings(ini);
        check(cropBack.crop && cropBack.cropTop == 30 && cropBack.cropBottom == kCropRange.max && cropBack.cropOuter == 12 &&
                  cropBack.cropInner == 0,
              "settings: [crop] round-trips, each side clamped on its own");
        {
            Settings detail = cropBack;
            check(detail.objectDetail == 100, "settings: object detail defaults to 100% (the focus views' LOD)");
            detail.objectDetail = 40;
            save_live_settings(ini, detail);
            check(load_settings(ini).objectDetail == 40, "settings: object detail round-trips, live");
            detail.objectDetail = 250;  // above kObjectDetailRange: clamped
            save_settings(ini, detail);
            check(load_settings(ini).objectDetail == kObjectDetailRange.max, "settings: object detail is clamped to 0-100%");
            check(detail.sharpen == 0, "settings: sharpening defaults to off");
            detail.sharpen = 45;
            save_live_settings(ini, detail);
            check(load_settings(ini).sharpen == 45 && ini_int(ini, L"quad", L"sharpen_percent") == 45,
                  "settings: sharpening round-trips, live ([quad] sharpen_percent)");
            detail.sharpen = 150;  // above kSharpenRange: clamped
            save_settings(ini, detail);
            check(load_settings(ini).sharpen == kSharpenRange.max, "settings: sharpening is clamped to 0-100%");
            check(per_game_key(L"quad", L"sharpen_percent"), "settings: sharpen_percent is a per-game key");
            {
                Settings sharp;
                sharp.sharpen = 60;
                bool inProfile = false;
                for (const ProfileKey& k : settings_profile(sharp, L"sharp").keys)
                    inProfile |= k.section == L"quad" && k.key == L"sharpen_percent" && k.value == L"60";
                check(inProfile, "settings: a profile holds the sharpening");
                check(restored_defaults(sharp).sharpen == 0, "settings: Restore Defaults turns sharpening off");
            }
            detail.sharpen = 0;
            save_settings(ini, detail);
            check(detail.textureMatch == 100, "settings: periphery texture match defaults to 100%");
            detail.textureMatch = 30;
            save_live_settings(ini, detail);
            check(load_settings(ini).textureMatch == 30 && ini_int(ini, L"quad", L"periphery_texture_match") == 30,
                  "settings: periphery texture match round-trips, live ([quad] periphery_texture_match)");
            check(per_game_key(L"quad", L"periphery_texture_match"), "settings: periphery_texture_match is a per-game key");
            detail.textureMatch = 100;
            save_settings(ini, detail);
            check(!detail.grade.on, "settings: colour correction defaults to off");
            detail.grade = {true, -5, 10, -20, 40};
            save_live_settings(ini, detail);
            check(load_settings(ini).grade == detail.grade && ini_int(ini, L"quad", L"color_correction") == 1,
                  "settings: colour correction round-trips, live, negative values included");
            {
                Settings graded;
                graded.grade = {true, 0, 0, 0, -100};
                bool inProfile = false;
                for (const ProfileKey& k : settings_profile(graded, L"graded").keys)
                    inProfile |= k.section == L"quad" && k.key == L"color_saturation" && k.value == L"-100";
                check(inProfile, "settings: a profile holds the colour correction");
                check(!restored_defaults(graded).grade.on, "settings: Restore Defaults turns colour correction off");
            }
            detail.grade = {};
            save_settings(ini, detail);
        }
        LaunchSizes launched;
        launched.known = true;
        launched.outer = cropBack.outer;
        launched.focusWidth = launched.focusHeight = 1000;
        launched.crop = L"30,40,12,0";
        const bool sameCrop = fits_launch(cropBack, launched);
        launched.crop = L"0,0,0,0";
        const bool moreCrop = fits_launch(cropBack, launched);
        Settings lessCrop = cropBack;
        lessCrop.crop = false;
        launched.crop = L"30,40,12,0";
        Settings helmetSave = cropBack;
        helmetSave.helmet.on = true;
        helmetSave.helmet.rim = 9;
        helmetSave.helmet.light = 30;
        save_settings(ini, helmetSave);
        check(load_settings(ini).helmet == helmetSave.helmet, "settings: [helmet] round-trips");
        check(sameCrop && moreCrop && !fits_launch(lessCrop, launched),
              "live: a crop applies live, up to the launch's outer views (less crop than at launch: next launch)");
        check(cheeky_runtime_path(ini, L"C:\\layer") == L"C:\\layer\\CheekyFoveatedDLSS\\CheekyFoveatedDLSSRuntime.dll",
              "Cheeky runtime: by default in CheekyFoveatedDLSS next to the layer");
        save_cheeky_runtime(ini, L"D:\\cheeky\\CheekyFoveatedDLSSRuntime.dll");
        check(cheeky_runtime_path(ini, L"C:\\layer") == L"D:\\cheeky\\CheekyFoveatedDLSSRuntime.dll",
              "Cheeky runtime: a chosen path is used as it is");

        // The runtime report, and the kit's example: recommended 2584 x 2160, defaults (outer 50,
        // inner 100, 35% x 35%) give 1292 x 1080 and 904 x 756, a 4392 x 1080 target, 4.16 M pixels.
        const std::wstring runtimeIni = (folder / L"runtime.ini").wstring();
        check(!load_runtime_info(runtimeIni).known(), "runtime report: unknown before the first launch");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nmax_width=8192\r\n"
                               "max_height=8192\r\neye_tracking=0\r\n");
        const RuntimeInfo rt = load_runtime_info(runtimeIni);
        check(rt.known() && rt.width == 2584 && rt.height == 2160 && rt.maxWidth == 8192 && rt.eyeTracker == EyeTracker::Missing,
              "runtime report: recommended size, maximum and a missing eye tracker");
        check(rt.sessionFocus == -1, "runtime report: input focus not known without session_focus");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nsession_focus=\r\n");
        check(load_runtime_info(runtimeIni).sessionFocus == -1, "runtime report: an empty session_focus is not known (not 0)");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nsession_focus=0\r\n");
        check(load_runtime_info(runtimeIni).sessionFocus == 0, "runtime report: a session that never got input focus");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nsession_focus=1\r\n");
        check(load_runtime_info(runtimeIni).sessionFocus == 1, "runtime report: a session with input focus");
        const Readouts r = readouts(Settings{}, rt);
        check(r.centerPpd == 0, "readouts: no Center PPD before the layer reports it");
        RuntimeInfo withPpd = rt;
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nnative_center_ppd=45.32\r\n");
        withPpd.centerPpd = load_runtime_info(runtimeIni).centerPpd;
        Settings sharp;
        sharp.inner = 120;
        check(std::abs(withPpd.centerPpd - 45.32) < 1e-9 && std::lround(readouts(sharp, withPpd).centerPpd) == 54,
              "readouts: Center PPD = the reported native PPD x the focus density (45.32 x 120% = 54)");
        std::printf("     kit example: outer %dx%d, focus %dx%d, target %dx%d, %.2f M\n", r.outer.width, r.outer.height,
                    r.focus.width, r.focus.height, r.target.width, r.target.height, r.pixels / 1e6);
        check(r.outer.width == 1292 && r.outer.height == 1080 && r.focus.width == 904 && r.focus.height == 756 &&
                  r.target.width == 4392 && r.target.height == 1080 && std::abs(r.pixels / 1e6 - 4.16) < 0.005 &&
                  !r.overRuntimeLimit,
              "readouts: the kit's example numbers (QVFR.viewPixels)");
        // VRAM: none before a QVFR launch measures it; then the part that follows ACE's target scales with
        // the target's area (here the same target, then one with half the area) and the rest stays.
        check(vram_estimate(Settings{}, r, rt).mb == 0, "vram estimate: none before a launch reports it");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nvram_used_mb=9000\r\n"
                               "vram_budget_mb=15000\r\nvram_scaled_mb=2000\r\nvram_target=4392x1080\r\n");
        const RuntimeInfo withVram = load_runtime_info(runtimeIni);
        check(withVram.vramUsedMb == 9000 && withVram.vramBudgetMb == 15000 && withVram.vramScaledMb == 2000 &&
                  withVram.vramTarget.width == 4392 && withVram.vramTarget.height == 1080,
              "runtime report: VRAM used, budget, the part that follows the target and its size");
        const VramEstimate same = vram_estimate(Settings{}, r, withVram);
        check(std::abs(same.mb - 9000) < 1e-6 && same.budgetMb == 15000, "vram estimate: at the launch's target, what it measured");
        Readouts half = r;
        half.target = {2196, 1080};
        check(std::abs(vram_estimate(Settings{}, half, withVram).mb - 8000) < 1e-6,
              "vram estimate: half the target's area, half of the 2000 MB that follows it (9000 -> 8000)");
        // The last session (the layer's session_* keys): read back, and the pasteable summary.
        check(!withVram.session.known() && session_summary_text(withVram).find(L"no session") != std::wstring::npos,
              "session: none before a launch reports one");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nvram_used_mb=12519\r\n"
                               "vram_budget_mb=13008\r\nsession_time=2026-11-03 09:07\r\nsession_mode=QVFR\r\n"
                               "session_fps=90.0\r\nsession_frame_p50_ms=11.11\r\nsession_frame_p90_ms=11.66\r\n"
                               "session_late_percent=0.1\r\nsession_refresh_hz=90\r\nsession_cpu_ms=7.70\r\n"
                               "session_gpu_ms=8.60\r\nsession_gpu_busy=78\r\nsession_cpu_cores=4.8\r\n"
                               "session_limited=Neither (headroom)\r\n"
                               "session_threads=(aSeeVRClient.dll)=96|Physics=95|GameThread=69|Render Worker 0=26\r\n"
                               "session_frame_generation=0\r\nsession_gaze_ms=0.020\r\nsession_layer_ms=0.03\r\n"
                               "session_layers_below=XR_APILAYER_MBUCCHIA_toolkit\r\nsession_cpu_profile=\r\n");
        const RuntimeInfo withSession = load_runtime_info(runtimeIni);
        const SessionInfo& si = withSession.session;
        check(si.known() && std::abs(si.fps - 90) < 1e-9 && std::abs(si.cpuMs - 7.7) < 1e-9 && std::abs(si.gpuMs - 8.6) < 1e-9 &&
                  si.limited == L"Neither (headroom)" && !si.frameGeneration && si.cpuProfile.empty(),
              "session: frame rate, CPU and GPU frame time, what limits it read back");
        check(si.threads.size() == 4 && si.threads[0].first == L"(aSeeVRClient.dll)" && std::abs(si.threads[0].second - 96) < 1e-9 &&
                  si.threads[3].first == L"Render Worker 0",
              "session: the busiest threads in order, names with spaces and parentheses kept");
        const std::wstring summaryText = session_summary_text(withSession);
        std::printf("     summary:\n%ls", summaryText.c_str());
        check(summaryText.find(L"CPU frame time (render thread): 7.70 ms") != std::wstring::npos &&
                  summaryText.find(L"GPU frame time (whole GPU): 8.60 ms, 78% busy") != std::wstring::npos &&
                  summaryText.find(L"GameThread 69%") != std::wstring::npos &&
                  summaryText.find(L"VRAM: 12.2 GB of 12.7 GB") != std::wstring::npos &&
                  summaryText.find(L"frame generation off") != std::wstring::npos,
              "session: the summary has the frame times, threads, VRAM and frame generation");
        Settings wide;
        wide.outer = 100;
        wide.inner = 200;
        wide.width = 60;
        check(r.runtime.width == 2584 && r.runtime.height == 2160 && std::abs(r.runtimePixels - 2.0 * 2584 * 2160) < 1 &&
                  std::abs(r.saved - (1 - r.pixels / r.runtimePixels)) < 1e-12 && std::lround(r.saved * 100) == 63,
              "readouts: pixels saved against both eyes at the runtime's recommended size");
        const Readouts w = readouts(wide, rt);
        check(w.overRuntimeLimit, "readouts: a target wider than the runtime allows is flagged");
        // The layer clamps the offered eye to half the maximum width and the views keep their shares.
        std::printf("     clamped: outer %dx%d, focus %dx%d, target %dx%d\n", w.outer.width, w.outer.height, w.focus.width,
                    w.focus.height, w.target.width, w.target.height);
        check(std::abs(w.target.width / 2 - 4096) <= 2 && w.outer.height == 2160 &&
                  std::abs(w.pixels - 2.0 * (static_cast<double>(w.outer.width) * w.outer.height +
                                             static_cast<double>(w.focus.width) * w.focus.height)) < 1 &&
                  w.saved < 0,
              "readouts: views shrink with the clamped eye; more pixels than the runtime shows as negative saved");
        // A Varjo XR-4 (2026-11-19 report): the maximum is the native width, 4148. ACE's double-wide target halves it, so
        // a 30% periphery and a 20% box at 100% fill it exactly and anything more is too large; LMU offers each view as an
        // image of its own (the focus view beside its eye), so the same settings leave room up to about a 50% periphery.
        {
            RuntimeInfo varjo = rt;
            varjo.width = 4148;
            varjo.height = 3556;
            varjo.maxWidth = varjo.maxHeight = 4148;
            Settings at;
            at.outer = 30;
            at.inner = 100;
            at.width = at.height = 20;
            at.crop = false;
            Settings more = at;
            more.outer = 40;
            Settings nearHalf = at;
            nearHalf.outer = 49;
            Settings over = at;
            over.outer = 60;
            using games::QvfrImage;
            check(!readouts(at, varjo, false).overRuntimeLimit && readouts(more, varjo, false).overRuntimeLimit,
                  "readouts: Varjo, ACE's double-wide target: 30% + 20% fits, 40% is too large");
            check(!readouts(at, varjo, true, QvfrImage::SeparateViews, true).overRuntimeLimit &&
                      !readouts(more, varjo, true, QvfrImage::SeparateViews, true).overRuntimeLimit &&
                      !readouts(nearHalf, varjo, true, QvfrImage::SeparateViews, true).overRuntimeLimit &&
                      readouts(over, varjo, true, QvfrImage::SeparateViews, true).overRuntimeLimit,
                  "readouts: Varjo, LMU's separate views with the focus beside the eye: 30-49% fit, 60% is too large");
            Settings tall = at;
            tall.outer = 100;
            check(readouts(tall, varjo, false, QvfrImage::EyeColumns).overRuntimeLimit &&
                      !readouts(at, varjo, false, QvfrImage::EyeColumns).overRuntimeLimit,
                  "readouts: ACC's eye columns: the wider view against half the maximum");
        }
        // FOV crop: the outer views keep their density over the kept share; the focus views are unchanged.
        Settings cropped;
        cropped.crop = true;
        cropped.cropTop = 15;
        cropped.cropBottom = 5;
        cropped.cropOuter = 10;
        const Readouts cr = readouts(cropped, rt);
        check(cr.outer.width == 1163 && cr.outer.height == 864 && cr.focus.width == r.focus.width && cr.pixels < r.pixels,
              "readouts: a crop of 15/5 top/bottom and 10 outer gives 1163 x 864 outer views, the same focus views");
        Settings cropOff = cropped;
        cropOff.crop = false;
        check(readouts(cropOff, rt).outer.width == 1292 && crop_key(cropOff) == L"0,0,0,0" && crop_key(cropped) == L"15,5,10,0",
              "readouts: a crop that is off changes nothing");

        // The focus mask: in a game that masks its focus views (GameFeatures::focusMask), the pixels it skips outside
        // the focus shape leave the count; nothing for the plain box, the mask off or another game.
        {
            Settings trap;
            trap.shape = simqvfr::focus_shape_of({{-20, 50}, {20, 50}, {50, -50}, {-50, -50}});  // the top corners 30% in
            const Readouts other = readouts(trap, rt, false), lmu = readouts(trap, rt, true);
            const double share = focus_mask_share(focus_area_of(trap.shape, 0), static_cast<unsigned>(lmu.focus.width),
                                                  static_cast<unsigned>(lmu.focus.height), 16);
            const double outerArea = static_cast<double>(lmu.outer.width) * lmu.outer.height;
            const double focusArea = static_cast<double>(lmu.focus.width) * lmu.focus.height;
            std::printf("     trapezoid: %.1f%% of the focus views masked, saved %.1f%% (%.1f%% without the mask)\n",
                        lmu.focusMasked * 100, lmu.saved * 100, other.saved * 100);
            check(other.focusMasked == 0 && other.pixels == r.pixels && lmu.focusMasked == share && share > 0.25 &&
                      share < 0.30 && std::abs(lmu.pixels - 2.0 * (outerArea + focusArea * (1 - share))) < 1 &&
                      lmu.saved > other.saved,
                  "readouts: a trapezoid in a game that masks: its cut corners left out of the pixels, more saved");
            Settings maskOff = trap;
            maskOff.focusMask = false;
            Settings rounded;
            rounded.corner = 100;
            const Readouts round = readouts(rounded, rt, true);
            check(readouts(Settings{}, rt, true).focusMasked == 0 && readouts(maskOff, rt, true).focusMasked == 0 &&
                      round.focusMasked > 0.1 && round.focusMasked < share && round.pixels < r.pixels,
                  "readouts: the plain box or focus_mask=0 mask nothing; full rounding masks less than the trapezoid");
        }

        // Live settings: only the reloadable keys go to the running game's ini, and only while that
        // process is ACE.
        const std::wstring liveIni = (folder / L"live.ini").wstring();
        write_file(liveIni, "[experiment]\r\nmode=views4\r\nperiphery_scale=50\r\nfocus_percent=39\r\n");
        Settings changedLive = Settings{};
        changedLive.outer = 70;
        changedLive.width = 45;
        changedLive.feather = 25;
        changedLive.outline = true;
        check(save_live_settings(liveIni, changedLive), "live: the live keys are written");
        read_file(liveIni, iniText);
        check(iniText.find("focus_percent=45") != std::string::npos && iniText.find("feather_percent=25") != std::string::npos &&
                  iniText.find("debug_border=1") != std::string::npos && iniText.find("periphery_scale=70") != std::string::npos &&
                  iniText.find("focus_density=100") != std::string::npos && iniText.find("eye_scale") == std::string::npos &&
                  iniText.find("mode=views4") != std::string::npos,
              "live: QVFR sends foveation and resolution, never the method or eye_scale");

        // Live resolution fits the launch: up to its launch sizes.
        write_file(runtimeIni, "[runtime]\r\nlaunch_mode=views4\r\nlaunch_outer=50\r\nlaunch_focus_width=39\r\n"
                               "launch_focus_height=39\r\n");
        const LaunchSizes qvfrLaunch = load_launch_sizes(runtimeIni);
        Settings lower = Settings{};  // outer 50, inner 100 x 35%: inside the launch's 39%
        check(qvfrLaunch.known && fits_launch(lower, qvfrLaunch),
              "live fit: the launch settings fit");
        lower.outer = 30;
        lower.inner = 80;
        lower.width = 45;  // 80% x 45% = 36% of the native eye, under 39%
        check(fits_launch(lower, qvfrLaunch), "live fit: lower periphery and a wider box at lower density fit");
        Settings higher = Settings{};
        higher.inner = 120;  // 120% x 35% = 42%, over 39%
        Settings wider = Settings{};
        wider.width = 50;  // the same density over a larger box needs more pixels
        Settings sharper = Settings{};
        sharper.outer = 60;
        check(!fits_launch(higher, qvfrLaunch) && !fits_launch(wider, qvfrLaunch) && !fits_launch(sharper, qvfrLaunch),
              "live fit: more density, a larger box at the same density or more periphery do not");
        check(!load_launch_sizes(liveIni).known && !fits_launch(Settings{}, load_launch_sizes(liveIni)),
              "live fit: no launch report, no fit");
        write_file(runtimeIni, "[runtime]\r\nlive_ini=" + narrow(liveIni) + "\r\nlive_pid=" +
                                   std::to_string(GetCurrentProcessId()) + "\r\n");
        check(running_live_ini(runtimeIni).empty(), "live: a live_pid that is not ACE is ignored");
        write_file(runtimeIni, "[runtime]\r\nlive_ini=" + narrow(liveIni) + "\r\nlive_pid=0\r\n");
        check(running_live_ini(runtimeIni).empty(), "live: no running game, no live ini");
    }

    fs::remove_all(folder, ec);
    settings_round_trip_checks();
    return test_result();
}

// Offline check of the launcher's launch plan (src/launcher/launch_plan.h) against fake implicit
// layer registrations in a temporary folder: which layers go below the ACE layer and in what order,
// which are silenced or left alone, the explicit manifests written, the variables set, the
// environment block, Steam's library list, and the manifest JSON round trip.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include <windows.h>

#include "launcher/launch_plan.h"
#include "launcher/settings.h"

using namespace aceqv::launcher;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

fs::path folder;

// A layer manifest (and, unless `missing`, its library) in its own subfolder; returns the manifest.
std::wstring layer(const char* sub, const char* name, const char* disable, const char* enable = nullptr,
                   bool missing = false) {
    const fs::path dir = folder / sub;
    fs::create_directories(dir);
    std::string text = std::string("{\n  \"file_format_version\": \"1.0.0\",\n  \"api_layer\": {\n    \"name\": \"") + name +
                       "\",\n    \"library_path\": \".\\\\" + name + ".dll\",\n    \"api_version\": \"1.0\"";
    if (disable)
        text += std::string(",\n    \"disable_environment\": \"") + disable + "\"";
    if (enable)
        text += std::string(",\n    \"enable_environment\": \"") + enable + "\"";
    text += "\n  }\n}\n";
    write_file((dir / (std::string(name) + ".json")).wstring(), text);
    if (!missing)
        write_file((dir / (std::string(name) + ".dll")).wstring(), "MZ");
    return (dir / (std::string(name) + ".json")).wstring();
}

std::vector<std::string> names(const LayerPlan& plan) {
    std::vector<std::string> out;
    for (const MovedLayer& m : plan.below)
        out.push_back(m.name);
    return out;
}

std::wstring value_of(const std::vector<std::pair<std::wstring, std::wstring>>& vars, const std::wstring& name) {
    for (const auto& [n, v] : vars)
        if (n == name)
            return v;
    return L"<unset>";
}

}  // namespace

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    folder = fs::path(temp) / L"aceqv-launcher-test";
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    const std::vector<Registration> registrations = {
        {layer("ace", kLayerName, "DISABLE_XR_APILAYER_ACE_quadview"), true},  // this layer, installed (dev machine)
        {layer("toolkit", "XR_APILAYER_MBUCCHIA_toolkit", "DISABLE_XR_APILAYER_MBUCCHIA_toolkit"), true},
        {layer("qvf", "XR_APILAYER_MBUCCHIA_quadviews", "DISABLE_QUADVIEWS"), false},  // registered, switched off
        {layer("vxr1", kFrameGenerationLayer, "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR"), true},
        {layer("vxr2", kFrameGenerationLayer, "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR"), true},  // registered twice
        {layer("stuck", "XR_APILAYER_NO_SWITCH", nullptr), true},
        {layer("gated", "XR_APILAYER_GATED", "DISABLE_GATED", "ENABLE_GATED"), true},
        {layer("gone", "XR_APILAYER_GONE", "DISABLE_GONE", nullptr, true), true},
        {(folder / L"absent.json").wstring(), true},
    };

    // Default: every movable layer below the ACE layer, in registry order.
    const LayerPlan plan = plan_layers(registrations, Options{});
    const std::vector<std::string> expected = {"XR_APILAYER_MBUCCHIA_toolkit", kFrameGenerationLayer};
    check(names(plan) == expected, "Toolkit then VectorXR (once) below the ACE layer, in registry order");
    bool absolute = !plan.below.empty();
    for (const MovedLayer& m : plan.below) {
        const auto* l = m.manifest.find("api_layer");
        absolute &= fs::path(m.library).is_absolute() && fs::exists(m.library) && l &&
                    l->string_at("library_path") == narrow(m.library) && !l->find("disable_environment") &&
                    !l->find("enable_environment");
    }
    check(absolute, "explicit copies point at the absolute library and drop the enable/disable switches");
    check(plan.below.size() == 2 && plan.below[1].library.find(L"vxr1") != std::wstring::npos,
          "the first registration of a layer registered twice is the one loaded");
    const auto noted = [&](const char* what) {
        return std::any_of(plan.notes.begin(), plan.notes.end(), [&](const std::string& n) { return n.find(what) != std::string::npos; });
    };
    check(noted("XR_APILAYER_NO_SWITCH: has no disable_environment, so it stays ABOVE"),
          "a layer without a disable variable is reported as staying above");
    check(noted("XR_APILAYER_GONE: library") && noted("absent.json: unreadable manifest"),
          "missing library and unreadable manifest are reported");
    check(!noted("quadviews") && !noted("GATED") && plan.off.empty(),
          "switched-off and inactive (enable variable unset) layers are ignored");

    // An enable_environment layer whose variable is set counts as active.
    Options gated;
    gated.setVariables = {L"enable_gated"};
    check(names(plan_layers(registrations, gated)).back() == "XR_APILAYER_GATED", "a gated layer with its variable set is moved");

    // mode=quad: frame generation off; off_layers: Toolkit off.
    Options quad;
    quad.frameGenerationOff = true;
    quad.offLayers = {"XR_APILAYER_MBUCCHIA_toolkit"};
    const LayerPlan quadPlan = plan_layers(registrations, quad);
    check(quadPlan.below.empty() && quadPlan.off.size() == 2 &&
              quadPlan.off[0].second == L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit" &&
              quadPlan.off[1].second == L"DISABLE_XR_APILAYER_DIENERTECH_VECTORXR",
          "mode=quad turns VectorXR off and off_layers turns Toolkit off");

    // Variables.
    const auto vars = launch_variables(plan, L"C:\\run", L"C:\\run\\ace-quadview.ini", L"C:\\logs\\ace.log", L"C:\\runtime.ini");
    check(value_of(vars, L"XR_ENABLE_API_LAYERS") ==
                  L"XR_APILAYER_ACE_quadview;XR_APILAYER_MBUCCHIA_toolkit;XR_APILAYER_DIENERTECH_VECTORXR" &&
              value_of(vars, L"XR_API_LAYER_PATH") == L"C:\\run" &&
              value_of(vars, L"DISABLE_XR_APILAYER_ACE_quadview") == L"1" &&
              value_of(vars, L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit") == L"1" &&
              value_of(vars, L"DISABLE_XR_APILAYER_DIENERTECH_VECTORXR") == L"1" &&
              value_of(vars, L"ACE_QV_INI") == L"C:\\run\\ace-quadview.ini" && value_of(vars, L"SteamAppId") == L"3058630" &&
              value_of(vars, L"ACE_QV_RUNTIME_INFO") == L"C:\\runtime.ini",
          "variables: layer order, silenced implicit copies, settings, Steam app id, runtime report");
    const auto quadVars = launch_variables(quadPlan, L"C:\\run", L"i", L"l", L"r");
    check(value_of(quadVars, L"XR_ENABLE_API_LAYERS") == L"XR_APILAYER_ACE_quadview" &&
              value_of(quadVars, L"DISABLE_XR_APILAYER_DIENERTECH_VECTORXR") == L"1",
          "mode=quad: only the ACE layer, VectorXR silenced");

    // Run folder.
    const fs::path run = folder / L"run";
    std::wstring error;
    const fs::path ourLibrary = folder / L"ace" / (std::string(kLayerName) + ".dll");
    const bool written = write_run_folder(run.wstring(), registrations[0].manifest, ourLibrary.wstring(), plan, error);
    std::string text;
    aceqv::json::Value ours, toolkit;
    const bool read = read_file((run / (std::string(kLayerName) + ".json")).wstring(), text) && aceqv::json::parse(text, ours) &&
                      read_file((run / "XR_APILAYER_MBUCCHIA_toolkit.json").wstring(), text) && aceqv::json::parse(text, toolkit);
    const auto* ourLayer = read ? ours.find("api_layer") : nullptr;
    check(written && read && ourLayer && ourLayer->string_at("library_path") == narrow(ourLibrary.wstring()) &&
              !ourLayer->find("disable_environment") && ours.string_at("file_format_version") == "1.0.0" &&
              fs::exists(run / (std::string(kFrameGenerationLayer) + ".json")),
          "run folder: this layer's manifest (absolute library, no disable switch) and one per moved layer");
    check(!write_run_folder((folder / L"run2").wstring(), registrations[1].manifest, L"x", plan, error),
          "a manifest that is not the ACE layer's is refused");

    // Environment block.
    const std::wstring block = environment_block({L"=C:=C:\\x", L"Path=C:\\Windows", L"ACE_QV_CAPTURE=1", L"zeta=1"},
                                                 {{L"PATH", L"C:\\bin"}, {L"ACE_QV_INI", L"i"}, {L"alpha", L"2"}});
    std::vector<std::wstring> entries;
    for (const wchar_t* p = block.c_str(); *p; p += wcslen(p) + 1)
        entries.emplace_back(p);
    const std::vector<std::wstring> wantEnv = {L"=C:=C:\\x", L"ACE_QV_INI=i", L"alpha=2", L"Path=C:\\bin", L"zeta=1"};
    check(entries == wantEnv && block.size() >= 2 && block[block.size() - 1] == L'\0' && block[block.size() - 2] == L'\0',
          "environment block: inherited ACE_QV_* dropped, names case-insensitive, sorted, double NUL");

    // Steam libraries.
    const std::string vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
                            "\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"K:\\\\SteamLibrary\"\n\t}\n}\n";
    const auto libraries = steam_libraries(vdf);
    check(libraries.size() == 2 && libraries[0] == L"C:\\Program Files (x86)\\Steam" && libraries[1] == L"K:\\SteamLibrary",
          "Steam library paths read from libraryfolders.vdf");

    // JSON round trip, escapes included.
    aceqv::json::Value v, back;
    const std::string doc = "\xEF\xBB\xBF{\"a\": \"caf\\u00e9 \\\"q\\\" \\ud83d\\ude00\\n\", \"n\": -1.5e3, \"l\": [true, null, {}], \"e\": []}";
    const bool parsed = aceqv::json::parse(doc, v) && aceqv::json::parse(aceqv::json::dump(v), back);
    check(parsed && back.string_at("a") == "caf\xC3\xA9 \"q\" \xF0\x9F\x98\x80\n" && back.find("n")->text == "-1.5e3" &&
              back.find("l")->items.size() == 3 && back.find("l")->items[0].boolean && back.find("e")->items.empty(),
          "JSON: escapes, surrogate pairs, numbers, nesting survive a round trip");
    check(!aceqv::json::parse("{\"a\": 1,}", v) && !aceqv::json::parse("{\"a\": 1} x", v), "JSON: syntax errors rejected");

    // Settings (the window's model): the shipped QVFR ini, a save and reload, the mode switch, other
    // keys left alone, the runtime report and the kit's readouts.
    {
        const std::wstring ini = (folder / L"settings.ini").wstring();
        write_file(ini, "; comment kept\r\n[experiment]\r\nmode=views4\r\ndred=1\r\nfocus_percent=31\r\neye_scale=40\r\n"
                        "[quad]\r\nfeather_percent=20\r\ndebug_border=0\r\ndebug_border_color=magenta\r\ngaze=1\r\n");
        const Settings shipped = load_settings(ini);
        check(shipped.mode == RenderMode::Qvfr && shipped.outer == 40 && shipped.width == 31 && shipped.height == 31 &&
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

        s.mode = RenderMode::TwoLoop;
        save_settings(ini, s);
        const Settings loop = load_settings(ini);
        read_file(ini, iniText);
        check(loop.mode == RenderMode::TwoLoop && loop.outer == 30 && iniText.find("mode=quad") != std::string::npos &&
                  iniText.find("eye_scale=30") != std::string::npos,
              "settings: Two-loop writes mode=quad with the outer resolution as eye_scale");

        Settings big;
        big.outer = 200;
        big.width = 1;
        save_settings(ini, big);
        const Settings clamped = load_settings(ini);
        check(clamped.outer == kOuterRange.max && clamped.width == kSizeRange.min, "settings: values are clamped to the kit's ranges");

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
        const aceqv::CheekySettings afterLive = load_settings(ini).cheeky;
        check(afterLive.load && afterLive.outerScale == 40, "settings: the live save changes Cheeky's settings, not enabled");
        dlssLive.osd = {true, -15, 22, 30};
        save_live_settings(ini, dlssLive);
        check(load_settings(ini).osd == dlssLive.osd, "settings: [osd] (the performance overlay) round-trips, live");
        Settings cropSave = load_settings(ini);
        cropSave.mode = RenderMode::Qvfr;
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
        // inner 100, 39% x 39%) give 1292 x 1080 and 1008 x 842, a 4600 x 1080 target, 4.49 M pixels.
        const std::wstring runtimeIni = (folder / L"runtime.ini").wstring();
        check(!load_runtime_info(runtimeIni).known(), "runtime report: unknown before the first launch");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nmax_width=8192\r\n"
                               "max_height=8192\r\neye_tracking=0\r\n");
        const RuntimeInfo rt = load_runtime_info(runtimeIni);
        check(rt.known() && rt.width == 2584 && rt.height == 2160 && rt.maxWidth == 8192 && rt.eyeTracker == EyeTracker::Missing,
              "runtime report: recommended size, maximum and a missing eye tracker");
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
        check(r.outer.width == 1292 && r.outer.height == 1080 && r.focus.width == 1008 && r.focus.height == 842 &&
                  r.target.width == 4600 && r.target.height == 1080 && std::abs(r.pixels / 1e6 - 4.49) < 0.005 &&
                  !r.overRuntimeLimit,
              "readouts: the kit's example numbers (QVFR.viewPixels)");
        Settings wide;
        wide.outer = 100;
        wide.inner = 200;
        wide.width = 60;
        check(r.runtime.width == 2584 && r.runtime.height == 2160 && std::abs(r.runtimePixels - 2.0 * 2584 * 2160) < 1 &&
                  std::abs(r.saved - (1 - r.pixels / r.runtimePixels)) < 1e-12 && std::lround(r.saved * 100) == 60,
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
        Settings loopDefaults;
        loopDefaults.mode = RenderMode::TwoLoop;
        const Readouts l = readouts(loopDefaults, rt);
        check(l.focus.width == 1292 && l.target.width == 2584 && std::abs(l.pixels - 4.0 * 1292 * 1080) < 1 &&
                  std::abs(l.focusDensity - 50.0 / 39) < 1e-9,
              "readouts: Two-loop renders two outer-size frames; focus density = outer / width");

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
        write_file(liveIni, "[experiment]\r\nmode=quad\r\neye_scale=50\r\n");
        changedLive.mode = RenderMode::TwoLoop;
        save_live_settings(liveIni, changedLive);
        read_file(liveIni, iniText);
        check(iniText.find("periphery_scale") == std::string::npos && iniText.find("eye_scale=50") != std::string::npos,
              "live: Two-loop sends no resolution");

        // Live resolution fits the launch: QVFR up to its launch sizes, Two-loop only unchanged.
        write_file(runtimeIni, "[runtime]\r\nlaunch_mode=views4\r\nlaunch_outer=50\r\nlaunch_focus_width=39\r\n"
                               "launch_focus_height=39\r\n");
        const LaunchSizes qvfrLaunch = load_launch_sizes(runtimeIni);
        Settings lower = Settings{};  // outer 50, inner 100 x 39%: exactly the launch
        check(qvfrLaunch.known && qvfrLaunch.mode == RenderMode::Qvfr && fits_launch(lower, qvfrLaunch),
              "live fit: the launch settings fit");
        lower.outer = 30;
        lower.inner = 80;
        lower.width = 45;  // 80% x 45% = 36% of the native eye, under 39%
        check(fits_launch(lower, qvfrLaunch), "live fit: lower periphery and a wider box at lower density fit");
        Settings higher = Settings{};
        higher.inner = 120;  // 120% x 39% = 47%, over 39%
        Settings wider = Settings{};
        wider.width = 50;  // the same density over a larger box needs more pixels
        Settings sharper = Settings{};
        sharper.outer = 60;
        Settings loopMode = Settings{};
        loopMode.mode = RenderMode::TwoLoop;
        check(!fits_launch(higher, qvfrLaunch) && !fits_launch(wider, qvfrLaunch) && !fits_launch(sharper, qvfrLaunch) &&
                  !fits_launch(loopMode, qvfrLaunch),
              "live fit: more density, a larger box at the same density, more periphery or another method do not");
        write_file(runtimeIni, "[runtime]\r\nlaunch_mode=quad\r\nlaunch_outer=50\r\n");
        const LaunchSizes loopLaunch = load_launch_sizes(runtimeIni);
        Settings loopLower = loopMode;
        loopLower.outer = 40;
        check(fits_launch(loopMode, loopLaunch) && !fits_launch(loopLower, loopLaunch) && !load_launch_sizes(liveIni).known,
              "live fit: Two-loop resolution is fixed; no launch report, no fit");
        write_file(runtimeIni, "[runtime]\r\nlive_ini=" + narrow(liveIni) + "\r\nlive_pid=" +
                                   std::to_string(GetCurrentProcessId()) + "\r\n");
        check(running_live_ini(runtimeIni).empty(), "live: a live_pid that is not ACE is ignored");
        write_file(runtimeIni, "[runtime]\r\nlive_ini=" + narrow(liveIni) + "\r\nlive_pid=0\r\n");
        check(running_live_ini(runtimeIni).empty(), "live: no running game, no live ini");
    }

    fs::remove_all(folder, ec);
    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}

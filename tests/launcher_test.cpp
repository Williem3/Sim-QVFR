// Offline check of the launcher's launch plan (src/launcher/launch_plan.h) against fake implicit
// layer registrations in a temporary folder: which layers go below the ACE layer and in what order,
// which are silenced or left alone, the explicit manifests written, the variables set, the
// environment block, Steam's library list, and the manifest JSON round trip.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include <windows.h>

#include "launcher/launch_plan.h"
#include "launcher/layer_order.h"
#include "launcher/report.h"
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

    // The launcher's layer profile (layer_order.h): order, on/off either way, and what a launch locks.
    {
        std::vector<Registration> regs = registrations;
        regs.push_back({layer("cheeky", kCheekyLayer, "CHEEKY_OPENXR_LAYER_DISABLE"), false});  // off in Windows
        check(plan.above == std::vector<std::string>{"XR_APILAYER_NO_SWITCH"}, "profile: a layer that cannot be silenced stays above");
        LauncherLayers profile;
        Options o = launch_options(profile, false, false, false);
        LayerPlan p = plan_layers(regs, o);
        check(p.layers.size() == 4 && p.layers[0].name == "XR_APILAYER_MBUCCHIA_toolkit" && p.layers[0].on &&
                  p.layers[1].name == "XR_APILAYER_MBUCCHIA_quadviews" && !p.layers[1].on && !p.layers[1].systemOn &&
                  p.layers[3].name == kCheekyLayer && !p.layers[3].on && names(p).size() == 2,
              "profile: by default every movable layer is listed in registry order, as Windows has it on or off");
        check(move_launcher_layer(profile, p, kFrameGenerationLayer, -1) && profile.order.size() == 4 &&
                  profile.order[1] == kFrameGenerationLayer,
              "profile: a layer moves up; the whole order is the profile's from then on");
        p = plan_layers(regs, launch_options(profile, false, false, false));
        check(p.layers[1].name == kFrameGenerationLayer && !move_launcher_layer(profile, p, "XR_APILAYER_MBUCCHIA_toolkit", -1) &&
                  move_launcher_layer(profile, p, "XR_APILAYER_MBUCCHIA_toolkit", 1),
              "profile: the top layer cannot move further up, and moves down");
        p = plan_layers(regs, launch_options(profile, false, false, false));
        check(names(p) == std::vector<std::string>{kFrameGenerationLayer, "XR_APILAYER_MBUCCHIA_toolkit"},
              "profile: the launch loads the profile's order below the ACE layer");
        set_launcher_layer(profile, "XR_APILAYER_MBUCCHIA_quadviews", true, false);
        set_launcher_layer(profile, "XR_APILAYER_MBUCCHIA_toolkit", false, true);
        p = plan_layers(regs, launch_options(profile, false, false, false));
        const auto pv = launch_variables(p, L"C:\\run", L"i", L"l", L"r");
        check(names(p) == std::vector<std::string>{kFrameGenerationLayer, "XR_APILAYER_MBUCCHIA_quadviews"} &&
                  value_of(pv, L"DISABLE_QUADVIEWS") == L"<unset>" &&
                  value_of(pv, L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit") == L"1" && profile.on.size() == 1 && profile.off.size() == 1,
              "profile: a layer off in Windows loads for ACE alone (nothing to silence); one on in Windows is silenced");
        set_launcher_layer(profile, "XR_APILAYER_MBUCCHIA_toolkit", true, true);
        check(profile.off.empty(), "profile: back to its Windows state, the layer is no longer listed");
        // Locks: Cheeky loaded keeps its layer on; its frame generation, or Two-loop, keeps VectorXR off.
        LauncherLayers against = profile;
        set_launcher_layer(against, kCheekyLayer, false, false);
        p = plan_layers(regs, launch_options(against, false, true, true));
        const auto locked = [&](const char* n) {
            return *std::find_if(p.layers.begin(), p.layers.end(), [&](const PlannedLayer& l) { return l.name == n; });
        };
        check(locked(kCheekyLayer).on && !locked(kCheekyLayer).lock.empty() && !locked(kFrameGenerationLayer).on &&
                  !locked(kFrameGenerationLayer).lock.empty() &&
                  std::find(names(p).begin(), names(p).end(), std::string(kCheekyLayer)) != names(p).end(),
              "locks: Cheeky on keeps its layer on, its frame generation keeps VectorXR off");
        p = plan_layers(regs, launch_options(LauncherLayers{}, true, false, false));
        check(!locked(kFrameGenerationLayer).on && locked(kFrameGenerationLayer).lock.find("Two-loop") != std::string::npos,
              "locks: Two-loop keeps VectorXR off");
        // The ini: written and read back; old off_layers files keep working.
        const std::wstring profileIni = (folder / L"profile.ini").wstring();
        write_file(profileIni, "[launcher]\r\noff_layers=A, B;A\r\n");
        check(read_launcher_layers(profileIni).off == std::vector<std::string>{"A", "B"}, "profile: off_layers as before (deduplicated)");
        check(write_launcher_layers(profileIni, profile) && read_launcher_layers(profileIni) == profile,
              "profile: layer_order, off_layers and on_layers round trip");
    }

    // Windows' list (layer_order.h), on a scratch key: the ACE layer locked first, moves within a hive,
    // switches, the rewrite in order with a backup, and a stale list refused.
    {
        HKEY base = nullptr, machine = nullptr, user = nullptr, key = nullptr;
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\aceqv-launcher-test");
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\aceqv-launcher-test", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &base, nullptr);
        RegCreateKeyExW(base, L"machine", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &machine, nullptr);
        RegCreateKeyExW(base, L"user", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr);
        const std::wstring toolkit = layer("toolkit", "XR_APILAYER_MBUCCHIA_toolkit", "DISABLE_XR_APILAYER_MBUCCHIA_toolkit"),
                           ace = layer("ace", kLayerName, "DISABLE_XR_APILAYER_ACE_quadview"),
                           vxr = layer("vxr1", kFrameGenerationLayer, "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR"),
                           qvf = layer("qvf", "XR_APILAYER_MBUCCHIA_quadviews", "DISABLE_QUADVIEWS");
        const auto put = [](HKEY parent, const std::wstring& name, DWORD v) {
            HKEY k = nullptr;
            RegCreateKeyExW(parent, L"Implicit", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
            RegSetValueExW(k, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
            RegCloseKey(k);
        };
        put(machine, toolkit, 0);
        put(machine, ace, 0);
        put(machine, vxr, 1);
        put(user, qvf, 0);
        std::vector<SystemLayer> sys = read_system_layers(machine, user, L"Implicit");
        check(sys.size() == 4 && sys[0].name == kLayerName && !sys[0].lock.empty() && sys[1].name == "XR_APILAYER_MBUCCHIA_toolkit" &&
                  !sys[2].on && sys[3].name == "XR_APILAYER_MBUCCHIA_quadviews" && !sys[3].machine,
              "system: both hives read, named, switched; the ACE layer locked first in its hive");
        check(!move_system_layer(sys, 1, -1) && !move_system_layer(sys, 0, 1) && !move_system_layer(sys, 2, 1) &&
                  move_system_layer(sys, 2, -1) && sys[1].name == kFrameGenerationLayer,
              "system: nothing moves above the ACE layer or across hives; others swap");
        const std::vector<SystemLayer> before = read_system_layers(machine, user, L"Implicit");
        sys[1].on = true;
        check(system_needs_admin(before, sys) && !system_needs_admin(before, before), "system: an HKLM change needs administrator rights");
        std::vector<SystemLayer> roundTrip;
        check(parse_system_request(system_request(sys), roundTrip) && roundTrip == sys, "system: the elevated request round trips");
        std::wstring error;
        const std::wstring backup = (folder / L"backup.reg").wstring();
        check(write_system_hive(machine, L"Implicit", true, sys, backup, error) &&
                  write_system_hive(user, L"Implicit", false, sys, L"", error),
              "system: both hives written");
        const std::vector<SystemLayer> after = read_system_layers(machine, user, L"Implicit");
        check(after == sys, "system: the registry now holds that order and those switches");
        std::string reg;
        check(read_file(backup, reg) && reg.size() > 2 && reg[0] == '\xff' && reg[1] == '\xfe', "system: a .reg backup of the key as it was");
        sys.pop_back();
        sys.erase(sys.begin() + 1);
        check(!write_system_hive(machine, L"Implicit", true, sys, L"", error) && read_system_layers(machine, user, L"Implicit") == after,
              "system: a list that no longer matches the key is refused, the key untouched");
        RegCloseKey(machine);
        RegCloseKey(user);
        RegCloseKey(base);
        (void)key;
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\aceqv-launcher-test");
    }

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
            // The focus shape: a box by default (the shipped ini has no shape keys); corners round-trip.
            Settings f = reread;
            check(!f.shape.corners && f.shape == aceqv::FocusShape{}, "settings: the focus shape is a box by default");
            f.shape.corners = true;
            f.shape.in[static_cast<int>(aceqv::FocusCorner::TopLeft)] = 25;
            f.shape.in[static_cast<int>(aceqv::FocusCorner::TopRight)] = 25;
            f.shape.vertical[static_cast<int>(aceqv::FocusCorner::BottomLeft)] = 60;  // clamps to kFocusCornerMax
            save_live_settings(ini, f);
            std::string shapeText;
            read_file(ini, shapeText);
            const Settings shape = load_settings(ini);
            check(shape.shape.corners && shape.shape.in[0] == 25 && shape.shape.in[1] == 25 &&
                      shape.shape.vertical[static_cast<int>(aceqv::FocusCorner::BottomLeft)] == aceqv::kFocusCornerMax &&
                      shapeText.find("focus_shape=corners") != std::string::npos &&
                      shapeText.find("shape_top_left_in=25") != std::string::npos,
                  "settings: the corner shape is written live and read back, its insets clamped");
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
        // VRAM: none before a QVFR launch measures it; then the part that follows ACE's target scales with
        // the target's area (here the same target, then one with half the area) and the rest stays.
        check(vram_estimate(Settings{}, r, rt).mb == 0, "vram estimate: none before a launch reports it");
        write_file(runtimeIni, "[runtime]\r\nrecommended_width=2584\r\nrecommended_height=2160\r\nvram_used_mb=9000\r\n"
                               "vram_budget_mb=15000\r\nvram_scaled_mb=2000\r\nvram_target=4600x1080\r\n");
        const RuntimeInfo withVram = load_runtime_info(runtimeIni);
        check(withVram.vramUsedMb == 9000 && withVram.vramBudgetMb == 15000 && withVram.vramScaledMb == 2000 &&
                  withVram.vramTarget.width == 4600 && withVram.vramTarget.height == 1080,
              "runtime report: VRAM used, budget, the part that follows the target and its size");
        const VramEstimate same = vram_estimate(Settings{}, r, withVram);
        check(std::abs(same.mb - 9000) < 1e-6 && same.budgetMb == 15000, "vram estimate: at the launch's target, what it measured");
        Readouts half = r;
        half.target = {2300, 1080};
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
        Settings twoLoop;
        twoLoop.mode = RenderMode::TwoLoop;
        check(vram_estimate(twoLoop, r, withVram).mb == 0, "vram estimate: QVFR only");
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

    // Report an issue (report.h): the files zipped, the zip, the issue's body and link.
    {
        const fs::path logs = folder / L"report" / L"logs";
        fs::create_directories(logs);
        const auto now = fs::file_time_type::clock::now();
        const auto day = std::chrono::hours(24);
        const auto make = [&](const std::wstring& name, fs::file_time_type::duration age, size_t size = 10) {
            write_file((logs / name).wstring(), std::string(size, 'x'));
            fs::last_write_time(logs / name, now - age);
        };
        make(L"ace-20261101-100000.log", 30 * day);
        make(L"ace-20261103-100000.log", std::chrono::hours(4));
        make(L"ace-20261103-110000.log", std::chrono::hours(3));
        make(L"ace-20261103-120000.log", std::chrono::hours(2));
        make(L"ace-20261103-130000.log", std::chrono::hours(1));
        make(L"ace-quadview.log", std::chrono::minutes(1));  // the old single log: not a session log
        for (int i = 0; i < 4; ++i)
            make(L"cpu-profile-2026110" + std::to_wstring(i) + L".txt", std::chrono::hours(5 - i));
        make(L"cpu-profile-old.txt", 9 * day);
        make(L"cheeky-snapshot.json", std::chrono::hours(1));
        make(L"hang-101010.dmp", std::chrono::hours(1));
        make(L"hang-090909.dmp", 9 * day);
        make(L"hang-111111.dmp", std::chrono::minutes(5), static_cast<size_t>(kReportMaxDumpBytes) + 1);
        make(L"other.txt", std::chrono::hours(1));
        const std::wstring settingsIni = (folder / L"report" / L"ace-quadview.ini").wstring();
        const std::wstring reportRuntime = (folder / L"report" / L"runtime.ini").wstring();
        write_file(settingsIni, "[experiment]\r\nmode=views4\r\n");
        write_file(reportRuntime, "[runtime]\r\nrecommended_width=2584\r\n");
        const uint64_t nowTicks = static_cast<uint64_t>(now.time_since_epoch().count());
        const std::vector<ReportFile> files = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        std::vector<std::wstring> zipped;
        for (const ReportFile& f : files)
            zipped.push_back(f.name);
        const std::vector<std::wstring> want = {L"ace-quadview.ini",         L"runtime.ini",
                                                L"ace-20261103-130000.log",  L"ace-20261103-120000.log",
                                                L"ace-20261103-110000.log",  L"cpu-profile-20261103.txt",
                                                L"cpu-profile-20261102.txt", L"cpu-profile-20261101.txt",
                                                L"cheeky-snapshot.json",     L"hang-101010.dmp"};
        check(zipped == want, "report: the inis, the newest three session logs and CPU captures, Cheeky's snapshot, "
                              "a recent dump under the cap; no old, oversized or other files");
        for (const wchar_t* name : {L"ace-20261103-100000.log", L"ace-20261103-110000.log", L"ace-20261103-120000.log",
                                    L"ace-20261103-130000.log"})
            fs::remove(logs / name);
        const std::vector<ReportFile> old = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        check(std::count_if(old.begin(), old.end(), [](const ReportFile& f) { return f.name == L"ace-20261101-100000.log"; }) == 1,
              "report: only an old session log: the newest one still goes in");

        check(crc32("123456789", 9) == 0xcbf43926u, "report: CRC-32 check value");
        const std::vector<ReportFile> zipEntries = {{L"runtime.ini", reportRuntime}, {L"issue.md", L"", "# Problem \xc3\xa9"}};
        const std::wstring zip = (folder / L"report" / L"report.zip").wstring();
        const std::wstring zipError = write_zip(zip, zipEntries);
        std::string bytes;
        if (FILE* f = nullptr; _wfopen_s(&f, zip.c_str(), L"rb") == 0 && f) {
            char buffer[4096];
            for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                bytes.append(buffer, n);
            std::fclose(f);
        }
        const auto u16 = [&](size_t at) {
            return static_cast<unsigned char>(bytes[at]) | static_cast<unsigned char>(bytes[at + 1]) << 8;
        };
        const size_t eocd = bytes.size() >= 22 ? bytes.size() - 22 : 0;
        check(zipError.empty() && bytes.compare(0, 4, "PK\x03\x04") == 0 && bytes.compare(eocd, 4, "PK\x05\x06") == 0 &&
                  u16(eocd + 10) == 2 && bytes.find("# Problem \xc3\xa9") != std::string::npos &&
                  bytes.find("recommended_width=2584") != std::string::npos,
              "report: a stored zip of both entries, its end record counting them");
        check(!write_zip((folder / L"missing" / L"x.zip").wstring(), zipEntries).empty(), "report: an unwritable zip says why");

        check(windows_name(26100, L"24H2", 2314) == L"Windows 11 24H2 (build 26100.2314)" &&
                  windows_name(19045, L"22H2", 0) == L"Windows 10 22H2 (build 19045)",
              "report: Windows 11 from the build number");
        const uint64_t umd = (32ull << 48) | (15ull << 16) | 6094;
        check(gpu_driver_text(0x10de, umd) == L"560.94 (32.0.15.6094)" && gpu_driver_text(0x1002, umd) == L"32.0.15.6094",
              "report: NVIDIA's driver number from the Windows version");
        check(openxr_runtime_name(R"({"runtime": {"name": "SteamVR", "library_path": "x.dll"}})", L"C:\\a\\steamxr.json") ==
                      L"SteamVR" &&
                  openxr_runtime_name(R"({"runtime": {"library_path": "x.dll"}})", L"C:\\a\\pimax.json") == L"pimax.json",
              "report: the OpenXR runtime's name, else its manifest's file");
        check(url_encode(L"a b&\u00e9\n~") == "a%20b%26%C3%A9%0A~", "report: URL encoding of UTF-8");

        IssueReport report;
        report.version = L"1.2.3";
        report.zipName = L"ace-quadview-report-20261103-120000.zip";
        report.system = {L"AMD Ryzen 7 9800X3D, 16 threads", L"NVIDIA GeForce RTX 5090, 32 GB", L"581.57 (32.0.15.8157)",
                         L"Windows 11 24H2 (build 26100.2314)", L"Pimax OpenXR", 64};
        report.runtime.width = 2584;
        report.runtime.height = 2160;
        report.runtime.eyeTracker = EyeTracker::Detected;
        SessionInfo& session = report.runtime.session;
        session.fps = 89.9;
        session.frameP50 = 11.1;
        session.frameP90 = 12.4;
        session.limited = L"GPU";
        session.log = L"C:\\Users\\someone\\ace\\logs\\ace-20261103-120000.log";
        session.threads = {{L"GameThread", 71}, {L"RHIThread", 40}};
        const std::wstring details = issue_details(report);
        const auto has = [&](const wchar_t* text) { return details.find(text) != std::wstring::npos; };
        check(has(L"RTX 5090") && has(L"581.57") && has(L"Pimax OpenXR") && has(L"64 GB") && has(L"2584×2160") &&
                  has(L"eye tracked") && has(L"89.9 fps") && has(L"GameThread 71%") && has(L"Limited by: GPU") &&
                  has(L"detected") && has(L"ace-quadview 1.2.3"),
              "report: the details list version, hardware, headset, settings and the last session");
        check(!has(L"someone") && has(L"Log: ace-20261103-120000.log"),
              "report: the session log by name, not by a path with the user's name");

        // The likely causes: none for a healthy session, each known one when its sign shows.
        Settings healthy;
        RuntimeInfo fine = report.runtime;
        fine.session.limited = L"Neither (headroom)";
        check(likely_causes(healthy, fine).empty() && likely_causes(healthy, RuntimeInfo{}).empty(),
              "causes: none for a session with headroom, or before any session");
        const auto titles = [](const std::vector<Cause>& causes) {
            std::wstring all;
            for (const Cause& c : causes)
                all += c.title + L"|";
            return all;
        };
        RuntimeInfo troubled = fine;
        troubled.eyeTracker = EyeTracker::Missing;
        troubled.vramUsedMb = 11000;
        troubled.vramBudgetMb = 11600;
        troubled.cheekyStatus = L"missing";
        troubled.session.layersBelow = L"XR_APILAYER_MBUCCHIA_toolkit";
        troubled.session.limited = L"Pacing (frames late, neither saturated)";
        troubled.session.latePercent = 25;
        Settings dlss;
        dlss.cheeky.load = true;
        const std::wstring found = titles(likely_causes(dlss, troubled));
        check(found.find(L"tracker sent nothing") != std::wstring::npos && found.find(L"Cheeky missing") != std::wstring::npos &&
                  found.find(L"VRAM was nearly full (95%") != std::wstring::npos &&
                  found.find(L"OpenXR Toolkit") != std::wstring::npos && found.find(L"25% of frames were late") != std::wstring::npos,
              "causes: no eye tracker samples, Cheeky missing, VRAM near full, OpenXR Toolkit, late frames");
        Settings fixed;
        fixed.eyeTracking = false;
        check(titles(likely_causes(fixed, troubled)).find(L"tracker") == std::wstring::npos &&
                  titles(likely_causes(healthy, troubled)).find(L"Cheeky") == std::wstring::npos,
              "causes: no tracker warning in Fixed, no Cheeky one with Foveated DLSS off");
        RuntimeInfo gpu = fine, cpu = fine;
        gpu.session.limited = L"GPU";
        gpu.session.gpuBusy = 98;
        cpu.session.threads = {{L"GameThread", 97}};
        check(titles(likely_causes(healthy, gpu)).find(L"GPU was busy all the time (98%)") != std::wstring::npos &&
                  titles(likely_causes(healthy, cpu)).find(L"CPU work limited") != std::wstring::npos,
              "causes: a busy GPU, and ACE's game thread near a whole core");

        // The title: the kind, then the description's first line, cut at a word.
        report.kind = 2;  // Performance
        report.description = L"  Stutters every few seconds on Monza  \nSecond line";
        check(issue_title(report) == L"[Performance] Stutters every few seconds on Monza", "title: kind and first line");
        report.description = L"The focus area jumps around whenever I look at the left mirror in the hairpin at Monza";
        check(issue_title(report) == L"[Performance] The focus area jumps around whenever I look at the left mirror in the…",
              "title: a long line cut at a word");
        report.kind = 0;
        report.description.clear();
        check(issue_title(report) == L"[Problem] ace-quadview 1.2.3: ", "title: Not sure and no description: Problem, the version");

        report.description = L"The focus area jumps & flickers.";
        report.causes = likely_causes(dlss, troubled);
        const std::wstring body = issue_body(report);
        const auto at = [&](const wchar_t* text) { return body.find(text); };
        check(body.starts_with(L"### What happened?\n\nThe focus area jumps & flickers.") &&
                  at(L"### Steps to reproduce") < at(L"### When does it happen?") &&
                  at(L"### When does it happen?") < at(L"### Screenshots and logs") &&
                  at(report.zipName.c_str()) < at(L"### Launcher checks") &&
                  at(L"### Launcher checks") < at(L"OpenXR Toolkit") && at(L"OpenXR Toolkit") < at(L"### Collected by the launcher") &&
                  at(L"### Collected by the launcher") < at(L"RTX 5090") && at(L"RTX 5090") != std::wstring::npos,
              "body: the description, the form's prompts, the zip, the checks, then the collected details");
        IssueReport bare = report;
        bare.description.clear();
        bare.causes.clear();
        const std::wstring bareBody = issue_body(bare);
        check(bareBody.starts_with(L"### What happened?\n\n<!--") && bareBody.find(L"None found.") != std::wstring::npos,
              "body: a prompt without a description, and no checks found");

        const std::vector<IssueField> fields = issue_fields(report);
        check(fields.size() == 4 && fields[0].id == "what" && fields[0].value == report.description && fields[1].id == "files" &&
                  fields[1].value.find(report.zipName) != std::wstring::npos && fields[2].id == "checks" &&
                  fields[2].value.find(L"OpenXR Toolkit") != std::wstring::npos && fields[3].id == "details" &&
                  fields[3].value == issue_details(report),
              "fields: what, files (the zip), checks and details, by the forms' ids");
        check(std::string(kIssueTemplates[0]) == "bug.yml" && std::string(kIssueTemplates[2]) == "performance.yml" &&
                  std::wstring(kIssueKinds[2]) == L"Performance",
              "forms: Not sure is bug.yml, each kind its own form");
        // Every form in .github/ISSUE_TEMPLATE exists and has the fields the launcher fills.
        const fs::path forms = fs::path(__FILE__).parent_path().parent_path() / L".github" / L"ISSUE_TEMPLATE";
        bool formsOk = true;
        for (const char* form : kIssueTemplates) {
            std::string yml;
            if (FILE* f = nullptr; _wfopen_s(&f, (forms / form).c_str(), L"rb") == 0 && f) {
                char buffer[4096];
                for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                    yml.append(buffer, n);
                std::fclose(f);
            }
            for (const IssueField& field : fields) {  // "id: what" ending its line (LF or CRLF)
                const size_t idAt = yml.find("id: " + field.id);
                const size_t end = idAt == std::string::npos ? yml.size() : idAt + 4 + field.id.size();
                formsOk = formsOk && end < yml.size() && (yml[end] == '\n' || yml[end] == '\r');
            }
        }
        check(formsOk, "forms: each kind's form exists with what, files, checks and details");

        const std::string url = issue_url(kIssueRepo, kIssueTemplates[0], L"[Problem] ace-quadview 1.2.3: ", fields);
        check(url.starts_with("https://github.com/Williem3/Sim-QVFR/issues/new?template=bug.yml&title=%5BProblem%5D%20ace-quadview%201.2.3%3A%20&what=The%20focus%20area%20jumps%20%26%20flickers.&files=") &&
                  url.find("&checks=") != std::string::npos && url.find("&details=") != std::string::npos &&
                  url.size() <= kIssueUrlMax && url.find("Trimmed") == std::string::npos,
              "report: the form's link with each field, whole when it fits");
        std::vector<IssueField> empty = fields;
        empty[0].value.clear();
        check(issue_url(L"someone/test", "crash.yml", L"t", empty).starts_with("https://github.com/someone/test/issues/new?template=crash.yml&") &&
                  issue_url(kIssueRepo, "bug.yml", L"t", empty).find("&what=") == std::string::npos,
              "report: another repository and form; empty fields left out");
        std::vector<IssueField> longFields = fields;
        for (int i = 0; i < 400; ++i)  // inside the session's ``` block, before its closing fence
            longFields[3].value.insert(longFields[3].value.size() - 4, L"Busiest thread line " + std::to_wstring(i) + L"\n");
        const std::string trimmed = issue_url(kIssueRepo, "bug.yml", L"t", longFields, 4000);
        size_t fences = 0;
        for (size_t f = trimmed.find("%60%60%60"); f != std::string::npos; f = trimmed.find("%60%60%60", f + 9))
            ++fences;
        check(trimmed.size() <= 4000 && trimmed.find("&what=The%20focus") != std::string::npos &&
                  trimmed.find("OpenXR%20Toolkit") != std::string::npos && trimmed.find("Trimmed") != std::string::npos &&
                  fences % 2 == 0,
              "report: long details are cut from their end to fit, their code block closed, with a note");
        const std::string tiny = issue_url(kIssueRepo, "bug.yml", L"t", longFields, 1500);
        check(tiny.size() <= 1500 && tiny.find("&what=The%20focus") != std::string::npos && tiny.find("&details=") == std::string::npos,
              "report: with less room the details go, then the checks, the description last");
    }

    fs::remove_all(folder, ec);
    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}

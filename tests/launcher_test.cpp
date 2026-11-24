// Offline check of the launcher's launch plan (src/launcher/launch_plan.h) against fake implicit
// layer registrations in a temporary folder: which layers go below the Sim-QVFR layer and in what order,
// which are silenced or left alone, the explicit manifests written, the variables set, the
// environment block, Steam's library list, and the manifest JSON round trip.
#include "launcher_test_kit.h"

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-launcher-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    const std::vector<Registration> registrations = {
        {layer("ours", kLayerName, "DISABLE_XR_APILAYER_SIM_qvfr"), true},  // this layer, installed (dev machine)
        {layer("toolkit", "XR_APILAYER_MBUCCHIA_toolkit", "DISABLE_XR_APILAYER_MBUCCHIA_toolkit"), true},
        {layer("qvf", "XR_APILAYER_MBUCCHIA_quadviews", "DISABLE_QUADVIEWS"), false},  // registered, switched off
        {layer("vxr1", kFrameGenerationLayer, "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR"), true},
        {layer("vxr2", kFrameGenerationLayer, "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR"), true},  // registered twice
        {layer("stuck", "XR_APILAYER_NO_SWITCH", nullptr), true},
        {layer("gated", "XR_APILAYER_GATED", "DISABLE_GATED", "ENABLE_GATED"), true},
        {layer("gone", "XR_APILAYER_GONE", "DISABLE_GONE", nullptr, true), true},
        {(folder / L"absent.json").wstring(), true},
    };

    // Default: every movable layer below the Sim-QVFR layer, in registry order.
    const LayerPlan plan = plan_layers(registrations, Options{});
    const std::vector<std::string> expected = {"XR_APILAYER_MBUCCHIA_toolkit", kFrameGenerationLayer};
    check(names(plan) == expected, "Toolkit then VectorXR (once) below the Sim-QVFR layer, in registry order");
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

    // A manifest the OpenXR loader refuses is left out: named in XR_ENABLE_API_LAYERS it would fail
    // xrCreateInstance, and ACC (OpenComposite) would start flat.
    {
        const std::wstring bad = layer("bad", "XR_APILAYER_BAD_VERSION", "DISABLE_BAD");
        std::string text;
        read_file(bad, text);
        text.replace(text.find("\"implementation_version\": \"1\""), 29, "\"implementation_version\": \"1.2.0\"");
        write_file(bad, text);
        std::vector<Registration> regs = registrations;
        regs.push_back({bad, true});
        const LayerPlan p = plan_layers(regs, Options{});
        check(names(p) == expected && std::any_of(p.notes.begin(), p.notes.end(), [](const std::string& n) {
                  return n.find("XR_APILAYER_BAD_VERSION: the OpenXR loader rejects its manifest (implementation_version") == 0;
              }),
              "a layer whose manifest the loader rejects is left out, with a note");
        json::Value m;
        check(json::parse("{\"file_format_version\":\"1.0.0\",\"api_layer\":{\"name\":\"X\",\"library_path\":\"x.dll\","
                          "\"api_version\":\"1.1\",\"implementation_version\":\"7\"}}", m) && loader_rejects(m).empty(),
              "a valid manifest (api_version 1.1) passes the loader's checks");
        check(json::parse("{\"file_format_version\":\"1.0.0\",\"api_layer\":{\"name\":\"X\",\"library_path\":\"x.dll\","
                          "\"api_version\":\"1.0\",\"implementation_version\":1}}", m) && !loader_rejects(m).empty(),
              "a numeric implementation_version is refused, as the loader does");
    }

    // A layer name becomes a file in the run folder (an elevated launch) and an XR_ENABLE_API_LAYERS entry: only
    // plain names (A-Z, 0-9, _ . -) are taken, the rest left alone with a note.
    for (const char* odd : {"..\\\\..\\\\escaped", "../escaped", "C:x", "A;B", "A,B", "..", "A..B", "A B"}) {
        const std::wstring m = layer("odd", "XR_APILAYER_ODD", "DISABLE_ODD");
        std::string text;
        read_file(m, text);
        text.replace(text.find("\"name\": \"XR_APILAYER_ODD\""), 25, std::string("\"name\": \"") + odd + "\"");
        write_file(m, text);
        std::vector<Registration> regs = registrations;
        regs.push_back({m, true});
        const LayerPlan p = plan_layers(regs, Options{});
        check(names(p) == expected && std::any_of(p.notes.begin(), p.notes.end(), [](const std::string& n) {
                  return n.find("is not a plain name") != std::string::npos;
              }),
              (std::string("a layer named ") + odd + " is left out, with a note").c_str());
    }
    check(names(plan_layers(std::vector<Registration>{{layer("plain", "XR_APILAYER_Plain-1.2", "DISABLE_PLAIN"), true}}, Options{})) ==
              std::vector<std::string>{"XR_APILAYER_Plain-1.2"},
          "a plain layer name with dots and dashes is taken");

    // off_layers: Toolkit off, VectorXR left as Windows has it.
    Options toolkitOff;
    toolkitOff.offLayers = {"XR_APILAYER_MBUCCHIA_toolkit"};
    const LayerPlan toolkitPlan = plan_layers(registrations, toolkitOff);
    check(toolkitPlan.off.size() == 1 && toolkitPlan.off[0].second == L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit",
          "off_layers turns Toolkit off, and only Toolkit");

    // The launcher's layer profile (layer_order.h): order, on/off either way, and what a launch locks.
    {
        std::vector<Registration> regs = registrations;
        regs.push_back({layer("cheeky", kCheekyLayer, "CHEEKY_OPENXR_LAYER_DISABLE"), false});  // off in Windows
        check(plan.above == std::vector<std::string>{"XR_APILAYER_NO_SWITCH"}, "profile: a layer that cannot be silenced stays above");
        LauncherLayers profile;
        Options o = launch_options(profile, false, false);
        LayerPlan p = plan_layers(regs, o);
        check(p.layers.size() == 4 && p.layers[0].name == "XR_APILAYER_MBUCCHIA_toolkit" && p.layers[0].on &&
                  p.layers[1].name == "XR_APILAYER_MBUCCHIA_quadviews" && !p.layers[1].on && !p.layers[1].systemOn &&
                  p.layers[3].name == kCheekyLayer && !p.layers[3].on && names(p).size() == 2,
              "profile: by default every movable layer is listed in registry order, as Windows has it on or off");
        check(move_launcher_layer(profile, p, kFrameGenerationLayer, -1) && profile.order.size() == 4 &&
                  profile.order[1] == kFrameGenerationLayer,
              "profile: a layer moves up; the whole order is the profile's from then on");
        p = plan_layers(regs, launch_options(profile, false, false));
        check(p.layers[1].name == kFrameGenerationLayer && !move_launcher_layer(profile, p, "XR_APILAYER_MBUCCHIA_toolkit", -1) &&
                  move_launcher_layer(profile, p, "XR_APILAYER_MBUCCHIA_toolkit", 1),
              "profile: the top layer cannot move further up, and moves down");
        p = plan_layers(regs, launch_options(profile, false, false));
        check(names(p) == std::vector<std::string>{kFrameGenerationLayer, "XR_APILAYER_MBUCCHIA_toolkit"},
              "profile: the launch loads the profile's order below the Sim-QVFR layer");
        set_launcher_layer(profile, "XR_APILAYER_MBUCCHIA_quadviews", true, false);
        set_launcher_layer(profile, "XR_APILAYER_MBUCCHIA_toolkit", false, true);
        p = plan_layers(regs, launch_options(profile, false, false));
        const auto pv = launch_variables(p, L"C:\\run", L"i", L"l", L"r");
        check(names(p) == std::vector<std::string>{kFrameGenerationLayer, "XR_APILAYER_MBUCCHIA_quadviews"} &&
                  value_of(pv, L"DISABLE_QUADVIEWS") == L"<unset>" &&
                  value_of(pv, L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit") == L"1" && profile.on.size() == 1 && profile.off.size() == 1,
              "profile: a layer off in Windows loads for ACE alone (nothing to silence); one on in Windows is silenced");
        set_launcher_layer(profile, "XR_APILAYER_MBUCCHIA_toolkit", true, true);
        check(profile.off.empty(), "profile: back to its Windows state, the layer is no longer listed");
        // Locks: Cheeky loaded keeps its layer on; its frame generation keeps VectorXR off.
        LauncherLayers against = profile;
        set_launcher_layer(against, kCheekyLayer, false, false);
        p = plan_layers(regs, launch_options(against, true, true));
        const auto locked = [&](const char* n) {
            return *std::find_if(p.layers.begin(), p.layers.end(), [&](const PlannedLayer& l) { return l.name == n; });
        };
        check(locked(kCheekyLayer).on && !locked(kCheekyLayer).lock.empty() && !locked(kFrameGenerationLayer).on &&
                  !locked(kFrameGenerationLayer).lock.empty() &&
                  std::find(names(p).begin(), names(p).end(), std::string(kCheekyLayer)) != names(p).end(),
              "locks: Cheeky on keeps its layer on, its frame generation keeps VectorXR off");
        // Run with anti-cheat: Cheeky's layer off, over [cheeky] load and on_layers alike.
        LauncherLayers wantsCheeky;
        set_launcher_layer(wantsCheeky, kCheekyLayer, true, false);
        p = plan_layers(regs, launch_options(wantsCheeky, true, true, {}, true));
        const std::vector<std::string> loaded = names(p);
        check(!locked(kCheekyLayer).on && locked(kCheekyLayer).lock.find("anti-cheat") != std::string::npos &&
                  std::find(loaded.begin(), loaded.end(), std::string(kCheekyLayer)) == loaded.end(),
              "locks: Run with anti-cheat keeps Cheeky's layer off, whatever [cheeky] and the profile say");
        std::vector<Registration> cheekyOn = registrations;
        cheekyOn.push_back({layer("cheeky-on", kCheekyLayer, "CHEEKY_OPENXR_LAYER_DISABLE"), true});  // on in Windows
        p = plan_layers(cheekyOn, launch_options(LauncherLayers{}, false, false, {}, true));
        const auto pc = launch_variables(p, L"C:\run", L"i", L"l", L"r");
        check(!locked(kCheekyLayer).on && value_of(pc, L"CHEEKY_OPENXR_LAYER_DISABLE") == L"1",
              "locks: Run with anti-cheat silences Cheeky's layer switched on in Windows");
        // The ini: written and read back; old off_layers files keep working.
        const std::wstring profileIni = (folder / L"profile.ini").wstring();
        write_file(profileIni, "[launcher]\r\noff_layers=A, B;A\r\n");
        check(read_launcher_layers(profileIni).off == std::vector<std::string>{"A", "B"}, "profile: off_layers as before (deduplicated)");
        check(write_launcher_layers(profileIni, profile) && read_launcher_layers(profileIni) == profile,
              "profile: layer_order, off_layers and on_layers round trip");
    }

    // Windows' list (layer_order.h), on a scratch key: the Sim-QVFR layer locked first, moves within a hive,
    // a new order refused, switches written in place with a backup, and a stale list refused.
    {
        HKEY base = nullptr, machine = nullptr, user = nullptr, key = nullptr;
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\simqvfr-launcher-test");
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\simqvfr-launcher-test", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &base, nullptr);
        RegCreateKeyExW(base, L"machine", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &machine, nullptr);
        RegCreateKeyExW(base, L"user", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr);
        const std::wstring toolkit = layer("toolkit", "XR_APILAYER_MBUCCHIA_toolkit", "DISABLE_XR_APILAYER_MBUCCHIA_toolkit"),
                           ours = layer("ours", kLayerName, "DISABLE_XR_APILAYER_SIM_qvfr"),
                           vxr = layer("vxr1", kFrameGenerationLayer, "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR"),
                           qvf = layer("qvf", "XR_APILAYER_MBUCCHIA_quadviews", "DISABLE_QUADVIEWS");
        const auto put = [](HKEY parent, const std::wstring& name, DWORD v) {
            HKEY k = nullptr;
            RegCreateKeyExW(parent, L"Implicit", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
            RegSetValueExW(k, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
            RegCloseKey(k);
        };
        put(machine, toolkit, 0);
        put(machine, ours, 0);
        put(machine, vxr, 1);
        put(user, qvf, 0);
        std::vector<SystemLayer> sys = read_system_layers(machine, user, L"Implicit");
        check(sys.size() == 4 && sys[0].name == kLayerName && !sys[0].lock.empty() && sys[1].name == "XR_APILAYER_MBUCCHIA_toolkit" &&
                  !sys[2].on && sys[3].name == "XR_APILAYER_MBUCCHIA_quadviews" && !sys[3].machine,
              "system: both hives read, named, switched; the Sim-QVFR layer locked first in its hive");
        check(!move_system_layer(sys, 1, -1) && !move_system_layer(sys, 0, 1) && !move_system_layer(sys, 2, 1) &&
                  move_system_layer(sys, 2, -1) && sys[1].name == kFrameGenerationLayer,
              "system: nothing moves above the Sim-QVFR layer or across hives; others swap");
        const std::vector<SystemLayer> before = read_system_layers(machine, user, L"Implicit");
        std::wstring error;
        // Reordering Windows' list would rewrite other vendors' values: refused, nothing written.
        check(!write_system_hive(machine, L"Implicit", true, sys, L"", error) && error.find(L"order") != std::wstring::npos &&
                  read_system_layers(machine, user, L"Implicit") == before,
              "system: a new order is refused with a reason, the key untouched");
        sys = before;    // Windows' order, the Sim-QVFR layer shown first although the key has it second
        sys[2].on = true;  // VectorXR
        check(system_needs_admin(before, sys) && !system_needs_admin(before, before), "system: an HKLM change needs administrator rights");
        std::vector<SystemLayer> roundTrip;
        check(parse_system_request(system_request(sys), roundTrip) && roundTrip == sys, "system: the elevated request round trips");
        {
            // The elevated copy acts only on the bytes the window hashed; a strict parse besides.
            const std::string request = system_request(sys);
            std::string swapped = request;
            swapped[2] = swapped[2] == '1' ? '0' : '1';
            const std::string hash = system_request_hash(request);
            check(hash.size() == 64 && hash == system_request_hash(request) && hash != system_request_hash(swapped) &&
                      system_request_hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                  "system: the request's SHA-256 (a changed request has another)");
            std::vector<SystemLayer> parsed;
            check(!parse_system_request("M\t1\tC:\\a.json\x01\n", parsed) && !parse_system_request("M\t1\tC:\\a\x1b.json\n", parsed) &&
                      !parse_system_request("M\t1\t\xff\xfe.json\n", parsed) && !parse_system_request("X\t1\tC:\\a.json\n", parsed) &&
                      !parse_system_request("", parsed) && parse_system_request("U\t0\tC:\\a.json\r\n", parsed) &&
                      parsed.size() == 1 && !parsed[0].machine && !parsed[0].on,
                  "system: a request with control characters, bad UTF-8 or a bad line is refused");
        }
        const std::wstring backup = (folder / L"backup.reg").wstring();
        check(write_system_hive(machine, L"Implicit", true, sys, backup, error) &&
                  write_system_hive(user, L"Implicit", false, sys, L"", error),
              "system: both hives' switches written (the lock's place is no reorder)");
        const std::vector<SystemLayer> after = read_system_layers(machine, user, L"Implicit");
        check(after == sys, "system: the registry now holds those switches");
        std::vector<std::wstring> stored;
        if (RegOpenKeyExW(machine, L"Implicit", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            wchar_t valueName[2048];
            for (DWORD i = 0;; ++i) {
                DWORD length = static_cast<DWORD>(std::size(valueName));
                if (RegEnumValueW(key, i, valueName, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;
                stored.emplace_back(valueName, length);
            }
            RegCloseKey(key);
            key = nullptr;
        }
        check(stored == std::vector<std::wstring>{toolkit, ours, vxr}, "system: the key keeps its values in their order, in place");
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
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\simqvfr-launcher-test");
    }

    // Variables.
    const auto vars = launch_variables(plan, L"C:\\run", L"C:\\run\\sim-qvfr.ini", L"C:\\logs\\ace.log", L"C:\\runtime.ini");
    check(value_of(vars, L"XR_ENABLE_API_LAYERS") ==
                  L"XR_APILAYER_SIM_qvfr;XR_APILAYER_MBUCCHIA_toolkit;XR_APILAYER_DIENERTECH_VECTORXR" &&
              value_of(vars, L"XR_API_LAYER_PATH") == L"C:\\run" &&
              value_of(vars, L"DISABLE_XR_APILAYER_SIM_qvfr") == L"1" &&
              value_of(vars, L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit") == L"1" &&
              value_of(vars, L"DISABLE_XR_APILAYER_DIENERTECH_VECTORXR") == L"1" &&
              value_of(vars, L"SIM_QVFR_INI") == L"C:\\run\\sim-qvfr.ini" && value_of(vars, L"SteamAppId") == L"3058630" &&
              value_of(vars, L"SIM_QVFR_RUNTIME_INFO") == L"C:\\runtime.ini",
          "variables: layer order, silenced implicit copies, settings, Steam app id, runtime report");
    const auto toolkitVars = launch_variables(toolkitPlan, L"C:\\run", L"i", L"l", L"r");
    check(value_of(toolkitVars, L"XR_ENABLE_API_LAYERS") == L"XR_APILAYER_SIM_qvfr;XR_APILAYER_DIENERTECH_VECTORXR" &&
              value_of(toolkitVars, L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit") == L"1",
          "off_layers: Toolkit left out of the list and silenced, VectorXR loaded below Sim-QVFR");
    // An OpenComposite game (ACC): its loader keeps only the first and last of a list, so the loader gets the
    // Sim-QVFR layer alone and the layer chains the rest (SIM_QVFR_CHAIN), still silenced as implicit copies.
    const auto chainVars = launch_variables(plan, L"C:\\run", L"i", L"l", L"r", {}, true);
    check(value_of(chainVars, L"XR_ENABLE_API_LAYERS") == L"XR_APILAYER_SIM_qvfr" &&
              value_of(chainVars, L"SIM_QVFR_CHAIN") == L"XR_APILAYER_MBUCCHIA_toolkit;XR_APILAYER_DIENERTECH_VECTORXR" &&
              value_of(chainVars, L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit") == L"1",
          "chained: the loader gets the Sim-QVFR layer alone; SIM_QVFR_CHAIN lists the rest in order");
    Options allOff;  // nothing left below the Sim-QVFR layer
    allOff.offLayers = {"XR_APILAYER_MBUCCHIA_toolkit", "XR_APILAYER_DIENERTECH_VECTORXR"};
    const LayerPlan nothingBelow = plan_layers(registrations, allOff);
    check(value_of(launch_variables(nothingBelow, L"C:\\run", L"i", L"l", L"r", {}, true), L"SIM_QVFR_CHAIN") == L"<unset>" &&
              value_of(vars, L"SIM_QVFR_CHAIN") == L"<unset>",
          "chained: nothing to chain, or a game whose loader takes the list: no SIM_QVFR_CHAIN");

    // Run folder.
    const fs::path run = folder / L"run";
    std::wstring error;
    const fs::path ourLibrary = folder / L"layer" / (std::string(kLayerName) + ".dll");
    const bool written = write_run_folder(run.wstring(), registrations[0].manifest, ourLibrary.wstring(), plan, error);
    std::string text;
    simqvfr::json::Value ours, toolkit;
    const bool read = read_file((run / (std::string(kLayerName) + ".json")).wstring(), text) && simqvfr::json::parse(text, ours) &&
                      read_file((run / "XR_APILAYER_MBUCCHIA_toolkit.json").wstring(), text) && simqvfr::json::parse(text, toolkit);
    const auto* ourLayer = read ? ours.find("api_layer") : nullptr;
    check(written && read && ourLayer && ourLayer->string_at("library_path") == narrow(ourLibrary.wstring()) &&
              !ourLayer->find("disable_environment") && ours.string_at("file_format_version") == "1.0.0" &&
              fs::exists(run / (std::string(kFrameGenerationLayer) + ".json")),
          "run folder: this layer's manifest (absolute library, no disable switch) and one per moved layer");
    check(!write_run_folder((folder / L"run2").wstring(), registrations[1].manifest, L"x", plan, error),
          "a manifest that is not the Sim-QVFR layer's is refused");
    LayerPlan escaping = plan;
    escaping.below[0].name = "..\\escaped";
    check(!write_run_folder((folder / L"run3").wstring(), registrations[0].manifest, ourLibrary.wstring(), escaping, error) &&
              !fs::exists(folder / L"escaped.json"),
          "run folder: a layer name that leaves the folder is refused, nothing written outside it");

    // Environment block.
    const std::wstring block = environment_block({L"=C:=C:\\x", L"Path=C:\\Windows", L"SIM_QVFR_CAPTURE=1", L"zeta=1"},
                                                 {{L"PATH", L"C:\\bin"}, {L"SIM_QVFR_INI", L"i"}, {L"alpha", L"2"}});
    std::vector<std::wstring> entries;
    for (const wchar_t* p = block.c_str(); *p; p += wcslen(p) + 1)
        entries.emplace_back(p);
    const std::vector<std::wstring> wantEnv = {L"=C:=C:\\x", L"alpha=2", L"Path=C:\\bin", L"SIM_QVFR_INI=i", L"zeta=1"};
    check(entries == wantEnv && block.size() >= 2 && block[block.size() - 1] == L'\0' && block[block.size() - 2] == L'\0',
          "environment block: inherited SIM_QVFR_* dropped, names case-insensitive, sorted, double NUL");

    // Steam libraries.
    const std::string vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
                            "\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"K:\\\\SteamLibrary\"\n\t}\n}\n";
    const auto libraries = steam_libraries(vdf);
    check(libraries.size() == 2 && libraries[0] == L"C:\\Program Files (x86)\\Steam" && libraries[1] == L"K:\\SteamLibrary",
          "Steam library paths read from libraryfolders.vdf");

    // JSON round trip, escapes included.
    simqvfr::json::Value v, back;
    const std::string doc = "\xEF\xBB\xBF{\"a\": \"caf\\u00e9 \\\"q\\\" \\ud83d\\ude00\\n\", \"n\": -1.5e3, \"l\": [true, null, {}], \"e\": []}";
    const bool parsed = simqvfr::json::parse(doc, v) && simqvfr::json::parse(simqvfr::json::dump(v), back);
    check(parsed && back.string_at("a") == "caf\xC3\xA9 \"q\" \xF0\x9F\x98\x80\n" && back.find("n")->text == "-1.5e3" &&
              back.find("l")->items.size() == 3 && back.find("l")->items[0].boolean && back.find("e")->items.empty(),
          "JSON: escapes, surrogate pairs, numbers, nesting survive a round trip");
    check(!simqvfr::json::parse("{\"a\": 1,}", v) && !simqvfr::json::parse("{\"a\": 1} x", v), "JSON: syntax errors rejected");

    fs::remove_all(folder, ec);
    return test_result();
}

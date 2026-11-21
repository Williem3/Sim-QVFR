// Offline check of the launcher's launch checks (preflight.h): the game build, Steam and the game, Cheeky's loader,
// runtime and layer, OpenXR Toolkit's place and Turbo mode, eye tracking, and the files' hashes.
#include "launcher_test_kit.h"

int SIM_QVFR_TEST_MAIN() {
    folder = temp_path(L"simqvfr-launcher-preflight-test");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);

    // Launch checks (preflight.h): the game build, Steam and ACE, Cheeky's loader, Cheeky's runtime and layer,
    // OpenXR Toolkit's place and Turbo mode, eye tracking.
    {
        const auto find = [](const std::vector<Check>& checks, const char* id) -> const Check* {
            for (const Check& c : checks)
                if (c.id == id)
                    return &c;
            return nullptr;
        };
        PreflightInput in;
        in.gameExe = L"K:\\Steam\\steamapps\\common\\Assetto Corsa EVO\\AssettoCorsaEVO.exe";
        in.build = BuildState::Supported;
        std::vector<Check> checks = preflight_checks(in);
        check(worst_tone(checks) == CheckTone::Ok && find(checks, "build") && find(checks, "steam") &&
                  find(checks, "cheeky_loader") && !find(checks, "game_running") && !find(checks, "cheeky"),
              "preflight: all clear: build, Steam and the game folder pass; nothing about Cheeky while it is off");

        // Administrator rights: the game would inherit them and its OpenXR loader ignore the layer folder.
        check(find(checks, "elevated") && find(checks, "elevated")->tone == CheckTone::Ok,
              "preflight: a launcher with normal rights passes the administrator check");
        {
            PreflightInput e = in;
            e.elevated = true;
            e.parent = L"SimHub.exe";
            std::vector<Check> elevatedChecks = preflight_checks(e);
            const Check* elevated = find(elevatedChecks, "elevated");
            check(elevated && elevated->tone == CheckTone::Warn && !elevated->confirm &&
                      elevated->detail.find(L"machine-wide") != std::wstring::npos &&
                      elevated->detail.find(L"SimHub.exe") != std::wstring::npos,
                  "preflight: elevated, the launch registers its layers: a note, not held back, naming who started it");
            e.elevatedRegistration = false;
            elevatedChecks = preflight_checks(e);
            elevated = find(elevatedChecks, "elevated");
            check(elevated && elevated->tone == CheckTone::Bad && elevated->confirm &&
                      elevated->detail.find(L"SimHub.exe") != std::wstring::npos &&
                      elevated->detail.find(L"elevated_registration=0") != std::wstring::npos,
                  "preflight: elevated with registration off holds the launch back and names the program that started it");
            e.parent = L"explorer.exe";
            elevatedChecks = preflight_checks(e);
            elevated = find(elevatedChecks, "elevated");
            check(elevated && elevated->detail.find(L"Compatibility tab") != std::wstring::npos,
                  "preflight: elevated from Explorer: the answer is a normal double-click or the Compatibility tab");
            e.mode = LaunchMode::Off;
            elevatedChecks = preflight_checks(e);
            check(find(elevatedChecks, "elevated")->tone == CheckTone::Warn && !find(elevatedChecks, "elevated")->confirm,
                  "preflight: elevated with mode=off is only a warning");
        }
        std::printf("info preflight: this test runs %s (started by %ls)\n",
                    launcher_elevated() ? "as administrator" : "with normal rights", parent_process_name().c_str());

        in.build = BuildState::Unsupported;
        checks = preflight_checks(in);
        const Check* build = find(checks, "build");
        check(build && build->tone == CheckTone::Bad && build->confirm && build->fix == CheckFix::OpenReleases &&
                  worst_tone(checks) == CheckTone::Bad && first_with(checks, CheckTone::Bad) == build,
              "preflight: an updated game holds the launch back and points at the releases");
        in.mode = LaunchMode::Off;
        checks = preflight_checks(in);
        build = find(checks, "build");
        check(build && build->tone == CheckTone::Warn && !build->confirm, "preflight: mode=off patches nothing: only a warning");
        in.mode = LaunchMode::Qvfr;
        {  // A game whose QVFR runs on any build (LMU): an update or a beta branch is a note, not a hold.
            simqvfr::games::set_launcher_game(lmu::kGame);
            checks = preflight_checks(in);
            build = find(checks, "build");
            check(lmu::kGame.anyBuild && !ace::kGame.anyBuild && build && build->tone == CheckTone::Warn &&
                      !build->confirm && build->fix == CheckFix::OpenReleases &&
                      build->detail.find(L"Foveated rendering, DLSS and FSR still run") != std::wstring::npos,
                  "preflight: LMU on a build Sim-QVFR wasn't made for: a warning, the launch not held back");
            simqvfr::games::set_launcher_game(ace::kGame);
        }
        in.build = BuildState::Hashing;
        checks = preflight_checks(in);
        check(find(checks, "build")->tone == CheckTone::Pending && worst_tone(checks) == CheckTone::Ok,
              "preflight: still hashing holds nothing back");
        check(build_state("", true) == BuildState::Hashing && build_state("", false) == BuildState::Unknown &&
                  build_state("00", false) == BuildState::Unsupported &&
                  build_state(ace::kBuilds[0], false) == BuildState::Supported,
              "preflight: build_state from the exe's hash");
        // The games: found by exe name in any case; the launcher starts ACE; ACC's build isn't ACE's.
        check(simqvfr::games::game_for_exe(L"AssettoCorsaEVO.exe") == &ace::kGame &&
                  simqvfr::games::game_for_exe(L"ac2-win64-shipping.EXE") == &acc::kGame &&
                  simqvfr::games::game_for_exe(L"x-plane.exe") == &xplane::kGame &&
                  simqvfr::games::game_for_exe(L"acc.exe") == nullptr && simqvfr::games::game_for_exe(nullptr) == nullptr &&
                  &simqvfr::games::launcher_game() == &ace::kGame &&
                  simqvfr::games::supported_build(acc::kGame, acc::kBuilds[0]) &&
                  !simqvfr::games::supported_build(ace::kGame, acc::kBuilds[0]) &&
                  simqvfr::games::supported_build(xplane::kGame, xplane::kBuilds[0]),
              "games: lookup by exe, the launcher's game, builds per game");
        in.build = BuildState::Supported;

        // The launcher's game ([launcher] game): by id in any case; ACC's launch and what it has so far.
        std::size_t count = 0;
        const simqvfr::games::GameInfo* const* all = simqvfr::games::all_games(count);
        check(count == 7 && all[5] == &ets2::kGame && all[6] == simqvfr::games::game_by_id("rf2") && all[0] == &ace::kGame && all[1] == &acc::kGame && all[2] == &xplane::kGame &&
                  all[3] == &lmu::kGame && all[4] == &ats::kGame && simqvfr::games::game_by_id("ATS") == &ats::kGame && simqvfr::games::game_by_id("ACC") == &acc::kGame &&
                  simqvfr::games::game_by_id("XPlane") == &xplane::kGame && simqvfr::games::game_by_id("LMU") == &lmu::kGame &&
                  simqvfr::games::game_by_id("nope") == nullptr,
              "games: the launcher's list, found by id in any case");
        // Le Mans Ultimate: started as its own exe (not EasyAntiCheat's bootstrapper) in its own OpenXR, identity only;
        // its anti-cheat processes listed, none for the other games.
        {
            std::wstring found;
            const bool lmuAntiCheatListed =
                lmu::kGame.antiCheat && std::wstring(lmu::kGame.antiCheat[0]) == L"start_protected_game.exe" &&
                !ace::kGame.antiCheat && !acc::kGame.antiCheat && !xplane::kGame.antiCheat &&
                !simqvfr::games::anticheat_running(ace::kGame, found) && found.empty();
            check(std::wstring(lmu::kGame.exeName) == L"Le Mans Ultimate.exe" && std::wstring(lmu::kGame.launchArgs) == L"+XR" &&
                      lmu::kGame.openComposite == nullptr && lmu::kGame.features.fovCrop && lmu::kGame.features.helmet &&
                      lmu::kGame.features.liveResolution && !lmu::kGame.features.foveatedDlss &&
                      simqvfr::games::game_for_exe(L"le mans ultimate.exe") == &lmu::kGame &&
                      simqvfr::games::supported_build(lmu::kGame, lmu::kBuilds[0]) && lmuAntiCheatListed,
                  "games: Le Mans Ultimate's identity (own OpenXR, +XR, FOV crop, helmet and live resolution, "
                  "anti-cheat listed)");
        }
        check(acc::kGame.openComposite && !ace::kGame.openComposite && std::wstring(acc::kGame.exeFolder) ==
                  L"AC2\\Binaries\\Win64" && std::wstring(ace::kGame.exeFolder).empty() &&
                  acc::kGame.features.foveatedDlss && acc::kGame.features.helmet &&
                  acc::kGame.features.liveResolution && acc::kGame.features.fovCrop && !ace::kGame.features.foveatedDlss &&
                  ace::kGame.features.helmet && ace::kGame.features.liveResolution && ace::kGame.features.fovCrop,
              "games: ACC through OpenComposite, its exe in a subfolder, with every feature ACE has and Foveated DLSS "
              "(not ACE's any more)");
        check(ats::kGame.features.layerMultiView && ets2::kGame.features.layerMultiView && !ats::kGame.features.multiView &&
                  !ets2::kGame.features.multiView && !lmu::kGame.features.layerMultiView &&
                  !acc::kGame.features.layerMultiView && !ace::kGame.features.layerMultiView,
              "games: ATS and ETS2 draw their focus view by multi-view through the D3D11 layer (the launcher's wording), "
              "not by LMU's multi-view layout");
        check(!find(checks, "opencomposite"), "preflight: ACE needs no OpenComposite");
        simqvfr::games::set_launcher_game(acc::kGame);
        in.openComposite = OpenComposite::Valve;
        checks = preflight_checks(in);
        const Check* oc = find(checks, "opencomposite");
        check(oc && oc->tone == CheckTone::Bad && oc->confirm && oc->fix == CheckFix::OpenGuide &&
                  oc->fixTarget == kOpenCompositeGuideUrl,
              "preflight: ACC without OpenComposite holds the launch back and points at the guide");
        in.openComposite = OpenComposite::Installed;
        checks = preflight_checks(in);
        check(find(checks, "opencomposite") && find(checks, "opencomposite")->tone == CheckTone::Ok &&
                  worst_tone(checks) == CheckTone::Ok,
              "preflight: ACC with OpenComposite: all clear");
        simqvfr::games::set_launcher_game(ace::kGame);
        in.openComposite = OpenComposite::Unknown;
        write_file((folder / "valve_openvr.dll").wstring(), std::string("MZ...OpenVR Loader DLL...Valve"));
        write_file((folder / "oc_openvr.dll").wstring(), std::string("MZ...OpenComposite...runtime"));
        check(open_composite((folder / "valve_openvr.dll").wstring()) == OpenComposite::Valve &&
                  open_composite((folder / "oc_openvr.dll").wstring()) == OpenComposite::Installed &&
                  open_composite((folder / "missing_openvr.dll").wstring()) == OpenComposite::Missing,
              "open_composite: OpenComposite's names itself, Valve's doesn't, a missing one is missing");

        // The game's DLSS with Foveated DLSS on: the 310 series or a warning that shows the file (ACC ships 2.2.15).
        {
            PreflightInput d = in;
            simqvfr::games::set_launcher_game(acc::kGame);
            d.openComposite = OpenComposite::Installed;
            d.cheekyLoad = d.cheekyRuntimeFound = true;
            d.cheekyLayer = CheekyLayer::Enabled;
            d.dlssPath = L"K:\\ACC\\nvngx_dlss.dll";
            d.dlssVersion = L"2.2.15.0";
            std::vector<Check> dc = preflight_checks(d);
            const Check* old = find(dc, "dlss_version");
            check(old && old->tone == CheckTone::Warn && !old->confirm && old->fix == CheckFix::ShowFile &&
                      old->fixTarget == d.dlssPath && old->detail.find(L"2.2.15.0") != std::wstring::npos,
                  "preflight: Foveated DLSS with ACC's own DLSS 2.2.15 warns, names it and shows the file");
            d.dlssVersion = L"310.9.1.0";
            dc = preflight_checks(d);
            check(find(dc, "dlss_version") && find(dc, "dlss_version")->tone == CheckTone::Ok,
                  "preflight: DLSS 310 is new enough");
            d.dlssVersion.clear();
            check(!find(preflight_checks(d), "dlss_version"), "preflight: an unknown DLSS version says nothing");
            d.dlssVersion = L"2.2.15.0";
            d.cheekyLoad = false;
            check(!find(preflight_checks(d), "dlss_version"), "preflight: without Foveated DLSS the DLSS version doesn't matter");

            // Sim-QVFR's own DLSS (LMU): multi-view rendering, then its nvngx_dlss.dll (the included one or the player's)
            // and its version.
            simqvfr::games::set_launcher_game(lmu::kGame);
            PreflightInput o = in;
            o.ownDlss = true;
            o.ownDlssIncluded = true;
            o.ownDlssFound = true;
            o.ownDlssPath = L"C:\\Sim-QVFR\\layer\\nvngx_dlss.dll";
            o.ownDlssVersion = L"310.9.1.0";
            std::vector<Check> ownChecks = preflight_checks(o);
            const Check* ready = find(ownChecks, "own_dlss");
            check(ready && ready->tone == CheckTone::Ok && ready->title.find(L"310.9.1.0") != std::wstring::npos &&
                      ready->detail.find(L"Sim-QVFR includes") != std::wstring::npos,
                  "preflight: the included DLSS 310 DLL is ready, its version named");
            o.ownDlssFound = false;
            o.ownDlssVersion.clear();
            ownChecks = preflight_checks(o);
            const Check* gone = find(ownChecks, "own_dlss");
            check(gone && gone->tone == CheckTone::Warn && gone->fix == CheckFix::OpenDlss &&
                      gone->detail.find(L"C:\\Sim-QVFR\\layer\\nvngx_dlss.dll") != std::wstring::npos &&
                      gone->detail.find(L"install Sim-QVFR again") != std::wstring::npos &&
                      gone->detail.find(L"without DLSS") != std::wstring::npos,
                  "preflight: the included DLL missing warns where it looked, to install again, and that DLSS won't run");
            o.ownDlssIncluded = false;
            o.ownDlssFound = true;
            o.ownDlssPath = L"K:\\ACE\\nvngx_dlss.dll";
            o.ownDlssVersion = L"310.9.16.0";
            ownChecks = preflight_checks(o);
            const Check* yours = find(ownChecks, "own_dlss");
            check(yours && yours->tone == CheckTone::Ok && yours->detail.find(L"Your nvngx_dlss.dll") != std::wstring::npos,
                  "preflight: a DLL the player chose is ready, named as theirs");
            o.ownDlssVersion = L"2.5.1.0";
            ownChecks = preflight_checks(o);
            const Check* older = find(ownChecks, "own_dlss");
            check(older && older->tone == CheckTone::Warn && older->fix == CheckFix::OpenDlss && !older->confirm,
                  "preflight: an older DLSS warns and opens the DLSS pane, without stopping the launch");
            o.ownDlssFound = false;
            o.ownDlssVersion.clear();
            ownChecks = preflight_checks(o);
            const Check* missing = find(ownChecks, "own_dlss");
            check(missing && missing->tone == CheckTone::Warn && missing->fix == CheckFix::OpenDlss &&
                      missing->detail.find(L"K:\\ACE\\nvngx_dlss.dll") != std::wstring::npos &&
                      missing->detail.find(L"the one Sim-QVFR includes") != std::wstring::npos,
                  "preflight: a chosen nvngx_dlss.dll gone warns where it looked, and that the included one is taken");
            check(dlss_dll_path(L"", L"C:\\Sim-QVFR\\layer") == L"C:\\Sim-QVFR\\layer\\nvngx_dlss.dll" &&
                      dlss_dll_path(L"K:\\ACE", L"C:\\Sim-QVFR\\layer") == L"K:\\ACE\\nvngx_dlss.dll" &&
                      dlss_dll_path(L"", L"").empty(),
                  "dlss_dll_path: the chosen folder's, else the included one beside the layer");
            o.ownDlss = false;
            check(!find(preflight_checks(o), "own_dlss"), "preflight: Sim-QVFR's DLSS off: no check");
            simqvfr::games::set_launcher_game(ace::kGame);
            check(version_major(L"310.9.1.0") == 310 && version_major(L"2.2.15.0") == 2 && version_major(L"") == -1 &&
                      version_major(L"x1") == -1,
                  "version_major: a file version's first number");
            check(!file_version(L"C:\\Windows\\System32\\kernel32.dll").empty() &&
                      file_version((folder / "missing.dll").wstring()).empty(),
                  "file_version: a system DLL has one, a missing file none");
            check(std::wstring(acc::kGame.dlssDll).find(L"nvngx_dlss.dll") != std::wstring::npos &&
                      std::wstring(ace::kGame.dlssDll) == L"nvngx_dlss.dll",
                  "games: each names its DLSS");
        }

        in.steamRunning = false;
        in.gameRunning = true;
        checks = preflight_checks(in);
        check(find(checks, "steam")->tone == CheckTone::Bad && find(checks, "steam")->fix == CheckFix::StartSteam &&
                  find(checks, "game_running") && find(checks, "game_running")->tone == CheckTone::Bad,
              "preflight: Steam not running and ACE already running");
        in.gameHasLayer = true;
        checks = preflight_checks(in);
        check(find(checks, "game_running") && find(checks, "game_running")->tone == CheckTone::Ok && !find(checks, "game_running")->confirm,
              "preflight: ACE running with Sim-QVFR (launched from here) is no problem");
        in.gameHasLayer = false;
        in.gameStarting = true;
        checks = preflight_checks(in);
        check(find(checks, "game_running") && find(checks, "game_running")->tone == CheckTone::Ok &&
                  find(checks, "game_running")->title.find(L"is starting") != std::wstring::npos,
              "preflight: ACE launched from here, Sim-QVFR not loaded yet: starting, not running without it");
        in.gameStarting = false;
        in.steamRunning = true;
        in.gameRunning = false;
        in.gameExe.clear();
        checks = preflight_checks(in);
        check(find(checks, "game") && find(checks, "game")->tone == CheckTone::Bad && !find(checks, "build") &&
                  !find(checks, "cheeky_loader"),
              "preflight: no game: no build or game folder check");
        check(find(checks, "game")->fix == CheckFix::ChooseGameFolder && find(checks, "game")->fixLabel == L"Choose folder…" &&
                  find(checks, "game")->detail.find(L"game_dir") == std::wstring::npos,
              "preflight: no game: a Choose folder button, no ini key to edit by hand");
        in.gameExe = L"K:\\Steam\\steamapps\\common\\Assetto Corsa EVO\\AssettoCorsaEVO.exe";

        // Cheeky's loader: found by its name in the DLL, a different dxgi.dll left alone.
        const fs::path game = folder / "game";
        fs::create_directories(game);
        write_file((game / "dxgi.dll").wstring(), std::string("MZ\0\0CheekyFoveatedDLSS loader", 30));
        write_file((game / "version.dll").wstring(), "MZ some other proxy");
        in.cheekyLoaders = cheeky_loaders(game.wstring());
        checks = preflight_checks(in);
        const Check* loader = find(checks, "cheeky_loader");
        check(in.cheekyLoaders.size() == 1 && in.cheekyLoaders[0].ends_with(L"dxgi.dll") && loader &&
                  loader->tone == CheckTone::Bad && loader->fix == CheckFix::ShowFile && loader->fixTarget == in.cheekyLoaders[0],
              "preflight: Cheeky's dxgi.dll in the game folder; another version.dll left alone");
        check(cheeky_loaders((folder / "nowhere").wstring()).empty(), "preflight: no game folder files, no loaders");
        in.cheekyLoaders.clear();

        // Cheeky on: its runtime and layer, then Toolkit's place against its layer and Turbo mode.
        in.cheekyLoad = true;
        checks = preflight_checks(in);
        check(find(checks, "cheeky_runtime") && find(checks, "cheeky_layer") && !find(checks, "cheeky"),
              "preflight: Cheeky on without its runtime or layer");
        in.cheekyRuntimeFound = true;
        in.cheekyLayer = CheekyLayer::Enabled;
        const auto moved = [](const char* name) {
            MovedLayer m;
            m.name = name;
            return m;
        };
        in.layers.below = {moved(kCheekyLayer), moved(kToolkitLayer)};
        checks = preflight_checks(in);
        check(find(checks, "cheeky") && find(checks, "toolkit_order") && find(checks, "toolkit_order")->fix == CheckFix::OpenLayers,
              "preflight: Toolkit below Cheeky's layer");
        in.layers.below = {moved(kToolkitLayer), moved(kCheekyLayer)};
        check(!find(preflight_checks(in), "toolkit_order"), "preflight: Toolkit above Cheeky's layer");
        in.layers.below = {moved(kCheekyLayer)};
        in.layers.above = {kToolkitLayer};
        check(!find(preflight_checks(in), "toolkit_order"), "preflight: Toolkit above the Sim-QVFR layer is above Cheeky's");
        in.layers.above.clear();
        in.toolkitTurbo = 1;
        in.cheekyFg = true;
        check(!find(preflight_checks(in), "toolkit_turbo"), "preflight: Turbo mode does not matter while Toolkit is not loaded");
        in.layers.below = {moved(kToolkitLayer), moved(kCheekyLayer)};
        checks = preflight_checks(in);
        const Check* turbo = find(checks, "toolkit_turbo");
        check(turbo && turbo->tone == CheckTone::Bad && turbo->fix == CheckFix::TurboOff, "preflight: Turbo mode with Cheeky's frame generation");
        in.cheekyFg = false;
        check(!find(preflight_checks(in), "toolkit_turbo"), "preflight: Turbo mode without frame generation");
        in.cheekyLoad = false;

        in.eyeTracker = EyeTracker::Missing;
        checks = preflight_checks(in);
        const Check* eye = find(checks, "eye_tracking");
        check(eye && eye->tone == CheckTone::Warn && worst_tone(checks) == CheckTone::Warn, "preflight: no eye tracking last time");
        in.eyeTracked = false;
        check(!find(preflight_checks(in), "eye_tracking"), "preflight: fixed foveation needs no eye tracker");

        const fs::path hashed = folder / "hash.txt";
        write_file(hashed.wstring(), "abc");
        check(simqvfr::sha256_file(hashed.c_str()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
                  simqvfr::sha256_file((folder / "missing").c_str()).empty(),
              "sha256_file: a known digest; empty for a missing file");
    }

    fs::remove_all(folder, ec);
    return test_result();
}

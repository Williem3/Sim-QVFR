// Offline check of the launcher's state: what the action bar says after Launch (launch_watch.h), the logs folder's
// retention, the session history, the game tabs, per-game settings, older versions' leftovers, and an elevated
// launch's explicit registrations (explicit_layers.h) on a scratch registry key.
#include "launcher_test_kit.h"

namespace {

// The game tabs (game_tabs.h; the Games pane): the hidden list, the colours as text, HSV, readable accents, and the
// settings keeping both launcher-wide.
// After Launch (launch_watch.h): what the action bar says became of the game, from what the watch saw; the watch
// itself on a real process; Restore Defaults' keeps; Choose folder…'s folder.
void launch_status_checks() {
    const std::wstring ace = L"Assetto Corsa EVO";
    LaunchSeen s;
    LaunchOutcome o = launch_outcome(s, ace);
    check(o.state == LaunchState::Starting && o.tone == 3 && o.text == L"Starting Assetto Corsa EVO…" && o.link == LaunchLink::None,
          "launch status: just launched, starting");
    s.layerLoaded = true;
    o = launch_outcome(s, ace);
    check(o.state == LaunchState::Running && o.tone == 0 && o.text == L"Assetto Corsa EVO is running with Sim-QVFR" &&
              o.link == LaunchLink::Diagnostics,
          "launch status: Sim-QVFR loaded: running with it, leading to Diagnostics");
    s.loaderError = L"something later";
    check(launch_outcome(s, ace).state == LaunchState::Running, "launch status: a loader error after Sim-QVFR loaded doesn't count");
    s = {};
    s.loaderError = L"xrCreateInstance failed";
    o = launch_outcome(s, ace);
    check(o.state == LaunchState::NoLayer && o.tone == 2 && o.link == LaunchLink::Report &&
              o.text == L"Assetto Corsa EVO started without Sim-QVFR · OpenXR: xrCreateInstance failed",
          "launch status: the OpenXR loader's error and no layer: started without Sim-QVFR, with the loader's reason");
    s = {};
    s.seconds = kLayerLoadSeconds - 1;
    check(launch_outcome(s, ace).state == LaunchState::Starting, "launch status: still starting before the wait is up");
    s.seconds = kLayerLoadSeconds;
    o = launch_outcome(s, ace);
    check(o.state == LaunchState::NotLoaded && o.tone == 1 && o.link == LaunchLink::Report && !o.finished(),
          "launch status: no layer after the wait: a warning, not yet a verdict");

    s = {};
    s.exited = true;
    s.layerLoaded = true;
    s.ranSeconds = 23 * 60 + 5;
    o = launch_outcome(s, ace);
    check(o.state == LaunchState::Closed && o.tone == 0 && o.finished() && o.text == L"Assetto Corsa EVO closed after 23 min" &&
              o.link == LaunchLink::Diagnostics,
          "launch status: closed, how long it ran");
    s.fps = 88.24;
    check(launch_outcome(s, ace).text == L"Assetto Corsa EVO closed after 23 min · 88.2 fps",
          "launch status: closed, with this launch's frame rate");
    s.exitCode = 0xC0000005;
    o = launch_outcome(s, ace);
    check(o.state == LaunchState::Crashed && o.tone == 2 && o.link == LaunchLink::Report &&
              o.text == L"Assetto Corsa EVO crashed after 23 min · Report an issue",
          "launch status: an access violation is a crash");
    s.exitCode = 0xC000013A;
    check(launch_outcome(s, ace).state == LaunchState::Closed, "launch status: a closed console isn't a crash");
    s.exitCode = 0;
    s.hang = true;
    check(launch_outcome(s, ace).text == L"Assetto Corsa EVO stopped responding after 23 min · Report an issue",
          "launch status: a hang dump: stopped responding");
    s.hang = false;
    s.layerLoaded = false;
    s.crashReport = true;
    o = launch_outcome(s, ace);
    check(o.state == LaunchState::Crashed && o.text == L"Assetto Corsa EVO crashed while starting · Report an issue",
          "launch status: the game's crash report before Sim-QVFR loaded: crashed while starting");
    s.crashReport = false;
    o = launch_outcome(s, ace);
    check(o.state == LaunchState::ClosedNoLayer && o.tone == 1 && o.link == LaunchLink::Report &&
              o.text == L"Assetto Corsa EVO closed without loading Sim-QVFR · Report an issue",
          "launch status: closed without Sim-QVFR ever loading");
    s.loaderError = L"failed to find layer";
    check(launch_outcome(s, ace).text == L"Assetto Corsa EVO closed without loading Sim-QVFR · OpenXR: failed to find layer",
          "launch status: closed without it, with the loader's reason");

    check(duration_text(45) == L"45 s" && duration_text(23 * 60 + 5) == L"23 min" && duration_text(65 * 60) == L"1 h 05 min" &&
              duration_text(-3) == L"0 s",
          "launch status: durations");
    check(crash_exit_code(0xC0000005) && crash_exit_code(0xC0000409) && !crash_exit_code(0) && !crash_exit_code(1) &&
              !crash_exit_code(0xC000013A),
          "launch status: crash exit codes are NTSTATUS errors");

    // The loader's own error lines only; the game's output is mixed in.
    check(loader_error("Fossilize ERROR: Failed to parse ApplicationInfoFilter\n"
                       "Warning [GENERAL |  | OpenXR-Loader] : a warning\n"
                       "Error: not the loader\n"
                       "Error [GENERAL | xrCreateInstance | OpenXR-Loader] : xrCreateInstance failed\r\n"
                       "Error [GENERAL | xrCreateInstance | OpenXR-Loader] : the second\n") == L"xrCreateInstance failed",
          "loader error: the loader's first Error line, not the game's ERROR or the loader's warnings");
    check(loader_error("Fossilize ERROR: x\nWarning [GENERAL | | OpenXR-Loader] : y\n").empty() && loader_error("").empty(),
          "loader error: none in the game's own output");
    check(loader_error("Error [GENERAL | xrCreateInstance]") == L"Error [GENERAL | xrCreateInstance]",
          "loader error: a line without a message is kept whole");
    check(loader_error("Error [GENERAL | platform_utils | OpenXR-Loader] : !!! WARNING !!! Environment variable "
                       "XR_API_LAYER_PATH is being ignored due to running from an elevated context. The value 'x' will NOT "
                       "be used.\n"
                       "Error [GENERAL | xrCreateInstance | OpenXR-Loader] : the real one\n") == L"the real one",
          "loader error: the elevated XR_API_LAYER_PATH warning isn't a reason (the launch registers in HKLM)");

    // The watch on real processes: a fake run folder, runtime.ini and logs beside it.
    const fs::path dir = fs::temp_directory_path() / ("sim-qvfr-launch-watch-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path run = dir / "logs" / "run-1";
    fs::create_directories(run);
    const std::wstring runtimeIni = (dir / "runtime.ini").wstring(), log = (dir / "logs" / "session-1.log").wstring();
    const auto start = [&](const wchar_t* commandLine, LaunchWatch& watch) {
        std::wstring command = commandLine;
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return DWORD(0);
        CloseHandle(pi.hThread);
        watch.start({pi.hProcess, pi.dwProcessId, run.wstring(), log}, ace::kGame, runtimeIni, (dir / "crashes").wstring());
        return pi.dwProcessId;
    };
    const auto settle = [](LaunchWatch& watch) {
        for (int i = 0; i < 200 && !watch.outcome().finished(); ++i) {
            watch.poll();
            Sleep(25);
        }
        watch.poll();
    };
    {
        // Flat: the loader's error in openxr-loader.log, then a crash exit code.
        write_file((run / kLoaderOutputName).wstring(),
                   "game output\nError [GENERAL | xrCreateInstance | OpenXR-Loader] : xrCreateInstance failed\n");
        LaunchWatch watch;
        const DWORD pid = start(L"cmd.exe /c exit 3221225477", watch);  // 0xC0000005
        settle(watch);
        const LaunchOutcome w = watch.outcome();
        check(pid != 0 && w.state == LaunchState::Crashed && w.text.find(L"crashed while starting") != std::wstring::npos,
              "launch watch: a process that exits with an access violation before Sim-QVFR loads crashed while starting");
    }
    {
        // Loaded and closed: runtime.ini names its process and this launch's session.
        write_file((run / kLoaderOutputName).wstring(), "game output only\n");
        LaunchWatch watch;
        const DWORD pid = start(L"cmd.exe /c ping -n 2 127.0.0.1 >nul", watch);
        WritePrivateProfileStringW(L"runtime", L"live_pid", std::to_wstring(pid).c_str(), runtimeIni.c_str());
        WritePrivateProfileStringW(L"runtime", L"session_log", log.c_str(), runtimeIni.c_str());
        WritePrivateProfileStringW(L"runtime", L"session_fps", L"90.0", runtimeIni.c_str());
        watch.poll();
        const LaunchOutcome running = watch.outcome();
        settle(watch);
        const LaunchOutcome w = watch.outcome();
        check(pid != 0 && running.state == LaunchState::Running && w.state == LaunchState::Closed &&
                  w.text.find(L"90.0 fps") != std::wstring::npos,
              "launch watch: live_pid is the launched process: running, then closed with the session's frame rate");
        // Another launch's session (another log) doesn't lend it its frame rate.
        WritePrivateProfileStringW(L"runtime", L"session_log", L"C:\\elsewhere\\session-0.log", runtimeIni.c_str());
        LaunchWatch other;
        const DWORD pid2 = start(L"cmd.exe /c exit 0", other);
        WritePrivateProfileStringW(L"runtime", L"live_pid", std::to_wstring(pid2).c_str(), runtimeIni.c_str());
        settle(other);
        check(other.outcome().state == LaunchState::Closed && other.outcome().text.find(L"fps") == std::wstring::npos,
              "launch watch: the frame rate only from this launch's own session");
    }
    {
        // A crash report folder written since the launch.
        write_file((run / kLoaderOutputName).wstring(), "");
        WritePrivateProfileStringW(L"runtime", L"live_pid", L"0", runtimeIni.c_str());
        LaunchWatch watch;
        const DWORD pid = start(L"cmd.exe /c exit 0", watch);
        fs::create_directories(dir / "crashes" / "UE4CC-Windows-1");
        settle(watch);
        check(pid != 0 && watch.outcome().state == LaunchState::Crashed, "launch watch: a new crash report folder means it crashed");
        watch.stop();
        check(!watch.active(), "launch watch: stopped, it follows nothing");
    }
    fs::remove_all(dir, ec);

    // Restore Defaults keeps the hotkeys, the update check and the game tabs; everything else goes back.
    {
        Settings mine;
        mine.outer = 30;
        mine.menuYaw = 12;
        mine.cheeky.load = true;
        mine.crop = true;
        mine.hotkeys[0].key = KeyCombo{'K', true, false, false};
        mine.updateCheck = true;
        mine.hiddenGames = L"acc";
        mine.gameColors[L"ace"] = 0x123456;
        const Settings back = restored_defaults(mine);
        const Settings d;
        check(back.outer == d.outer && back.menuYaw == d.menuYaw && back.cheeky.load == d.cheeky.load && back.crop == d.crop,
              "restore defaults: the game's settings go back to the defaults");
        check(back.hotkeys == mine.hotkeys && back.updateCheck && back.hiddenGames == L"acc" && back.gameColors.size() == 1 &&
                  back.gameColors.at(L"ace") == 0x123456,
              "restore defaults: the hotkeys, the update check, the hidden tabs and the colours stay");
    }

    // Choose folder…: the game's folder from the folder the user picks.
    {
        const fs::path lib = fs::temp_directory_path() / ("sim-qvfr-game-folder-" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(lib, ec);
        const fs::path game = lib / "steamapps" / "common" / acc::kGame.steamFolder;
        const fs::path exeDir = game / acc::kGame.exeFolder;
        fs::create_directories(exeDir);
        write_file((exeDir / acc::kGame.exeName).wstring(), "MZ");
        const std::wstring want = game.lexically_normal().wstring();
        check(game_folder_from(game.wstring(), acc::kGame) == want && game_folder_from(game.wstring() + L"\\", acc::kGame) == want,
              "game folder: the game's own folder, with or without a trailing backslash");
        check(game_folder_from(exeDir.wstring(), acc::kGame) == want, "game folder: the exe's folder is taken up to the game's");
        check(game_folder_from((lib / "steamapps" / "common").wstring(), acc::kGame) == want &&
                  game_folder_from(lib.wstring(), acc::kGame) == want,
              "game folder: steamapps\\common, or the Steam library above it");
        check(game_folder_from((lib / "steamapps").wstring(), acc::kGame).empty() && game_folder_from(L"", acc::kGame).empty() &&
                  game_folder_from((exeDir / "missing").wstring(), acc::kGame).empty() &&
                  game_folder_from(game.wstring(), ace::kGame).empty(),
              "game folder: a folder without the game's exe, or another game's folder, is none");
        fs::remove_all(lib, ec);
    }
}

// prune_logs never touches what it can't be sure is Sim-QVFR's own: other names, other content, links, a run folder's
// unknown contents, a linked logs folder. 40 of each kind, so every kind is over its limit.
void log_retention_strict_checks() {
    const fs::path root = temp_path(L"simqvfr-log-retention-strict"), logs = root / L"logs";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(logs);
    const auto put = [](const fs::path& p, const std::string& data) { write_file(p.wstring(), data); return p; };
    wchar_t stamp[32];
    for (int i = 0; i < 40; ++i) {  // ours, 40 of each: the oldest go
        swprintf_s(stamp, L"20261101-%06d", i + 100);
        put(logs / (std::wstring(L"session-") + stamp + L".log"), "log");
        fs::create_directories(logs / (std::wstring(L"run-") + stamp));
        put(logs / (std::wstring(L"run-") + stamp) / L"launch.txt", "run");
        put(logs / (std::wstring(L"sim-qvfr-report-") + stamp + L".zip"), "PKzip");
    }
    // Not ours, by name: a letter in the stamp, a longer stamp, another extension, another case.
    const fs::path badStamp = put(logs / L"session-2026110a-000000.log", "x");
    const fs::path longStamp = put(logs / L"session-20261101-0000000.log", "x");
    const fs::path otherExt = put(logs / L"session-20261101-000000.log.txt", "x");
    const fs::path upper = put(logs / L"SESSION-20261101-000001.LOG", "x");
    // Not ours, by content: a .dmp that isn't a minidump, a .zip that isn't a zip.
    const fs::path notDump = put(logs / L"hang-010101.dmp", "not a minidump");
    for (int i = 0; i < 8; ++i)
        put(logs / (L"hang-02020" + std::to_wstring(i) + L".dmp"), "MDMP");
    const fs::path notZip = put(logs / L"sim-qvfr-report-20261101-000000.zip", "not a zip");
    // The oldest run folders with what a launch doesn't write: a subfolder, a file of the user's.
    const fs::path withSub = logs / L"run-20261101-000100", withFile = logs / L"run-20261101-000101";
    fs::create_directories(withSub / L"mine");
    put(withSub / L"mine" / L"keep.txt", "keep");
    const fs::path userFile = put(withFile / L"my-notes.txt", "keep");
    // A link named like a session log and a run folder, each to something elsewhere.
    const fs::path outside = root / L"outside";
    fs::create_directories(outside);
    const fs::path outsideFile = put(outside / L"precious.txt", "keep"), outsideInner = put(outside / L"launch.txt", "keep");
    std::error_code l1, l2;
    fs::create_symlink(outsideFile, logs / L"session-20261101-000000.log", l1);
    fs::create_directory_symlink(outside, logs / L"run-20261101-000000", l2);

    const PruneResult r = prune_logs(logs.wstring());
    int sessions = 0, runs = 0;
    for (const auto& e : fs::directory_iterator(logs)) {
        const std::wstring n = e.path().filename().wstring();
        sessions += n.rfind(L"session-20261101-0001", 0) == 0 ? 1 : 0;
        runs += n.rfind(L"run-20261101-0001", 0) == 0 ? 1 : 0;
    }
    check(r.removed > 0 && sessions == kKeepSessions, "log retention strict: ours pruned to the newest");
    check(fs::exists(badStamp) && fs::exists(longStamp) && fs::exists(otherExt) && fs::exists(upper),
          "log retention strict: names not exactly ours untouched");
    check(fs::exists(notDump) && fs::exists(notZip), "log retention strict: a .dmp or .zip without its signature untouched");
    check(fs::exists(withSub / L"mine" / L"keep.txt") && fs::exists(userFile) && !fs::exists(withFile / L"launch.txt") &&
              runs == kKeepSessions + 2,
          "log retention strict: a run folder with anything else keeps it, only the launch's files go");
    if (!l1 && !l2)
        check(fs::exists(outsideFile) && fs::exists(outsideInner) && fs::is_symlink(logs / L"session-20261101-000000.log") &&
                  fs::is_symlink(logs / L"run-20261101-000000"),
              "log retention strict: links named like ours, and what they point at, untouched");
    else
        std::printf("note log retention strict: links can't be made here; their checks skipped\n");
    // A captures folder that is a link: never offered to Clear captures.
    std::error_code l4;
    fs::create_directory_symlink(outside, logs / L"captures", l4);
    if (!l4)
        check(diagnostics_paths(logs.wstring()).empty(), "log retention strict: a linked captures folder isn't offered to clear");
    // A logs folder that is itself a link: not looked into.
    std::error_code l3;
    const fs::path linkedLogs = root / L"linked-logs";
    fs::create_directory_symlink(logs, linkedLogs, l3);
    if (!l3) {
        for (int i = 0; i < 40; ++i)
            put(logs / (L"cpu-profile-20261102-0000" + std::to_wstring(10 + i) + L".txt"), "p");
        check(prune_logs(linkedLogs.wstring()).removed == 0 && diagnostics_paths(linkedLogs.wstring()).empty(),
              "log retention strict: a linked logs folder isn't looked into");
    }
    fs::remove_all(root, ec);
}

// The logs folder's retention (launcher/log_retention.h): the newest of each kind kept, the rest removed, the
// registration ledgers and the diagnostics' output never touched.
void log_retention_checks() {
    const fs::path logs = temp_path(L"simqvfr-log-retention-test");
    std::error_code ec;
    fs::remove_all(logs, ec);
    fs::create_directories(logs);
    const auto file = [&](const std::wstring& name, size_t bytes = 10) {
        std::string data(bytes, 'x');  // a minidump's and a zip's own signatures, as prune_logs checks them
        if (name.size() > 4 && name.compare(name.size() - 4, 4, L".dmp") == 0)
            data.replace(0, 4, "MDMP");
        if (name.size() > 4 && name.compare(name.size() - 4, 4, L".zip") == 0)
            data.replace(0, 2, "PK");
        write_file((logs / name).wstring(), data);
        return logs / name;
    };
    wchar_t stamp[32];
    for (int i = 0; i < kKeepSessions + 5; ++i) {  // 20261101-000000 .. : sortable stamps, the oldest first
        swprintf_s(stamp, L"20261101-%06d", i);
        file(std::wstring(L"session-") + stamp + L".log", 100);
        fs::create_directories(logs / (std::wstring(L"run-") + stamp));
        write_file((logs / (std::wstring(L"run-") + stamp) / L"launch.txt").wstring(), "run");
    }
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < kKeepHangDumps + 2; ++i)  // names without a date: their file times decide, hang-5 the newest
        fs::last_write_time(file(L"hang-1" + std::to_wstring(9 - i) + L"0000.dmp"), now - std::chrono::hours(10 - i));
    for (int i = 0; i < kKeepRegistryBackups + 4; ++i) {
        swprintf_s(stamp, L"20261101-%06d", i);
        file(std::wstring(L"openxr-layers-machine-") + stamp + L".reg");
    }
    file(L"openxr-layers-user-20261101-000000.reg");
    for (int i = 0; i < kKeepReports + 2; ++i) {
        swprintf_s(stamp, L"20261101-%06d", i);
        file(std::wstring(L"sim-qvfr-report-") + stamp + L".zip");
    }
    fs::create_directories(logs / L"openxr-registrations");
    write_file((logs / L"openxr-registrations" / L"run-20261101-000000.txt").wstring(), "ledger");
    fs::create_directories(logs / L"captures");
    write_file((logs / L"captures" / L"cap01.png").wstring(), std::string(5000, 'c'));
    {  // an older version's copy of a game's program (leftovers.h): a Windows program by content
        std::string program(3000, '\0');
        program[0] = 'M';
        program[1] = 'Z';
        program[0x3c] = static_cast<char>(0x80);
        program.replace(0x80, 4, std::string("PE\0\0", 4));
        write_file((logs / L"Le Mans Ultimate-0123456789ab-image.dump").wstring(), program);
    }
    file(L"something-else.txt");

    const PruneResult r = prune_logs(logs.wstring());
    const auto count = [&](const wchar_t* prefix) {
        int n = 0;
        for (const auto& e : fs::directory_iterator(logs))
            n += e.path().filename().wstring().rfind(prefix, 0) == 0 ? 1 : 0;
        return n;
    };
    check(count(L"session-") == kKeepSessions && count(L"run-") == kKeepSessions &&
              !fs::exists(logs / L"session-20261101-000004.log") && fs::exists(logs / L"session-20261101-000005.log") &&
              !fs::exists(logs / L"run-20261101-000000") && fs::exists(logs / L"run-20261101-000034"),
          "log retention: the newest session logs and run folders kept, by their stamps");
    check(count(L"hang-") == kKeepHangDumps && fs::exists(logs / L"hang-130000.dmp") && !fs::exists(logs / L"hang-190000.dmp"),
          "log retention: the newest hang dumps kept, by their file times");
    check(count(L"openxr-layers-machine-") == kKeepRegistryBackups + 1 &&
              fs::exists(logs / L"openxr-layers-machine-20261101-000000.reg") &&
              !fs::exists(logs / L"openxr-layers-machine-20261101-000001.reg") &&
              fs::exists(logs / L"openxr-layers-machine-20261101-000013.reg") && count(L"openxr-layers-user-") == 1,
          "log retention: a hive's oldest registry backup and its newest kept");
    check(count(L"sim-qvfr-report-") == kKeepReports, "log retention: the newest report zips kept");
    check(fs::exists(logs / L"openxr-registrations" / L"run-20261101-000000.txt") && fs::exists(logs / L"captures" / L"cap01.png") &&
              fs::exists(logs / L"Le Mans Ultimate-0123456789ab-image.dump") && fs::exists(logs / L"something-else.txt"),
          "log retention: ledgers, captures, image dumps and other files untouched");
    check(r.removed == 5 + 5 + 2 + 3 + 2 && r.bytes >= 5 * 100, "log retention: what was removed, counted");
    check(prune_logs(logs.wstring()).removed == 0, "log retention: a second pass removes nothing");

    const std::vector<std::wstring> paths = diagnostics_paths(logs.wstring());
    check(paths.size() == 2 && paths[0].find(L"Le Mans Ultimate") != std::wstring::npos &&
              paths[1].find(L"captures") != std::wstring::npos,
          "log retention: the captures folder and the image dump as the diagnostics' output");
    const LogsUse use = logs_use(logs.wstring());
    check(use.diagnosticsBytes == 8000 && use.bytes > use.diagnosticsBytes, "log retention: the logs folder's size, the diagnostics' part");
    check(size_text(512) == L"1 KB" && size_text(5ull << 20) == L"5.0 MB" && size_text(3ull << 29) == L"1.5 GB",
          "log retention: sizes as text");
    fs::remove_all(logs, ec);
}

// The session history (launcher/session_history.h): records kept per session, pruned, compared.
void session_history_checks() {
    const fs::path dir = temp_path(L"simqvfr-session-history-test");
    std::error_code ec;
    fs::remove_all(dir, ec);
    check(session_id(L"C:\\q\\logs\\session-20261107-101500.log") == L"session-20261107-101500" && session_id(L"C:\\q\\ace.log").empty() &&
              session_id(L"").empty(),
          "sessions: a session's id is its log's name");
    check(session_run_ini(L"C:\\q\\logs\\session-20261107-101500.log") == L"C:\\q\\logs\\run-20261107-101500\\sim-qvfr.ini",
          "sessions: the run folder's ini next to the log");
    check(baseline_mode(L"Off (the game's own two views)") && !baseline_mode(L"QVFR"), "sessions: a baseline by its mode");

    Settings a;  // the defaults: outer 50, inner 100
    Settings b = a;
    b.outer = 40;
    const std::vector<std::wstring> one = setting_changes(a, b);
    check(one.size() == 1 && one[0] == L"Periphery 50% \u2192 40%", "sessions: one change, as Periphery 50% → 40%");
    check(setting_changes(a, a).empty(), "sessions: no change between the same settings");
    Settings c = b;
    c.crop = true;
    c.inner = 120;
    c.cheeky.load = true;  // on: its details only once it was on before too
    const std::vector<std::wstring> three = setting_changes(a, c);
    check(three.size() == 4 && three[1] == L"Focus density 100% \u2192 120%" && three[2] == L"Crop on" && three[3] == L"Cheeky Foveated DLSS on",
          "sessions: changes in the panes' order, a switch as on or off");
    check(changes_text(three, 2) == L"Periphery 50% \u2192 40%, Focus density 100% \u2192 120% and 2 more" &&
              changes_text({L"A", L"B"}) == L"A and B" && changes_text({L"A"}) == L"A",
          "sessions: changes as one phrase, the rest counted");
    Settings hidden = a;
    hidden.cropTop = 10;  // the crop is off: its sides don't count
    check(setting_changes(a, hidden).empty(), "sessions: a crop side with the crop off is not a change");
    {  // Sim-QVFR's own DLSS, DLSS-NR and the CPU core: a comparison and Undo see them (the tuner review's F3)
        Settings d = a, e = a;
        d.dlss.on = e.dlss.on = true;
        e.dlss.focusScale = 67;
        e.dlss.textureSharpening = 1;
        const std::vector<std::wstring> dlss = setting_changes(d, e);
        check(dlss.size() == 2 && dlss[0] == L"Focus views DLAA → Quality" &&
                  dlss[1] == L"Texture sharpening Off → Focus only",
              "sessions: DLSS's options by the DLSS card's names");
        check(setting_changes(a, d) == std::vector<std::wstring>{L"DLSS on"}, "sessions: DLSS on");
        Settings f = d;
        f.dlss.upscaler = 1;
        check(setting_changes(d, f) == std::vector<std::wstring>{L"Upscaler DLSS → FSR"}, "sessions: DLSS to FSR");
        Settings g = a, h = a;
        g.nr.on = h.nr.on = true;
        h.nr.intensity = 50;
        h.cpuIsolateMain = !a.cpuIsolateMain;
        const std::vector<std::wstring> rest = setting_changes(g, h);
        check(rest.size() == 2 && rest[0] == L"DLSS-NR options" && rest[1].rfind(L"Main thread", 0) == 0,
              "sessions: DLSS-NR's options as one line, the main thread's core");
    }

    const auto session = [](const wchar_t* stamp, double fps, double p90, const wchar_t* mode = L"QVFR") {
        SessionInfo s;
        s.log = std::wstring(L"C:\\q\\logs\\session-") + stamp + L".log";
        s.time = L"2026-11-07 10:00";
        s.mode = mode;
        s.fps = fps;
        s.frameP50 = p90 - 2;
        s.frameP90 = p90;
        return s;
    };
    const std::wstring history = dir.wstring();
    check(store_session(history, session_record(session(L"20261107-100000", 80, 14), L"ace", a), L"") &&
              store_session(history, session_record(session(L"20261107-101000", 70, 15, L"Off (the game's own two views)"), L"ace", a), L"") &&
              store_session(history, session_record(session(L"20261107-102000", 60, 16), L"acc", a), L"") &&
              store_session(history, session_record(session(L"20261107-103000", 86, 12.8), L"ace", b), L""),
          "sessions: records written");
    // The same session's newer numbers replace its record.
    check(store_session(history, session_record(session(L"20261107-103000", 86.5, 12.8), L"ace", b), L""), "sessions: a record updated");
    std::vector<SessionRecord> records = load_sessions(history);
    check(records.size() == 4 && records[0].id == L"session-20261107-103000" && std::fabs(records[0].fps - 86.5) < 1e-6 &&
              records[0].settings.outer == 40 && records[1].game == L"acc" && records[2].baseline,
          "sessions: newest first, with their settings and numbers");
    check(records[0].same_numbers(session(L"20261107-103000", 86.5, 12.8)) && !records[0].same_numbers(session(L"20261107-103000", 87, 12.8)),
          "sessions: the same numbers aren't stored again");
    check(previous_session(records, 0) == 3 && other_kind_session(records, 0) == 2 && previous_session(records, 1) == -1 &&
              other_kind_session(records, 2) == 3,
          "sessions: the previous of the same game and kind, and of the other kind");
    check(comparison_text(L"vs previous", records[0], records[3]) ==
              L"vs previous: +6.5 fps, 90th percentile \u22121.20 ms, after Periphery 50% \u2192 40%",
          "sessions: the comparison line");
    check(comparison_text(L"vs without QVFR", records[0], records[2]) == L"vs without QVFR: +16.5 fps, 90th percentile \u22122.20 ms",
          "sessions: against a run without QVFR, without settings");
    // Settings copied from a file: the game's own as the launcher holds them.
    const std::wstring source = (dir / L"source.ini").wstring();
    Settings fromFile;
    fromFile.inner = 150;
    save_settings(source, fromFile);
    check(store_session(history, session_record(session(L"20261107-104000", 90, 12), L"ace", Settings{}), source) &&
              load_sessions(history)[0].settings.inner == 150,
          "sessions: a record's settings copied from the game's file");
    for (int i = 0; i < kSessionHistory + 3; ++i)
        store_session(history, session_record(session((L"20261108-1" + std::to_wstring(10000 + i)).c_str(), 90, 12), L"ace", a), L"");
    records = load_sessions(history);
    check(static_cast<int>(records.size()) == kSessionHistory && records[0].id == L"session-20261108-1" + std::to_wstring(10000 + kSessionHistory + 2),
          "sessions: only the newest are kept");
    fs::remove_all(dir, ec);
}

void game_tabs_checks() {
    check(!game_hidden(L"", L"ace") && game_hidden(L"acc, xplane", L"xplane") && game_hidden(L"ACC", L"acc") &&
              !game_hidden(L"acc", L"ace"),
          "game tabs: the hidden list (commas, spaces, any case)");
    check(with_game_hidden(L"", L"acc", true) == L"acc" && with_game_hidden(L"acc", L"xplane", true) == L"acc,xplane" &&
              with_game_hidden(L"acc,xplane", L"acc", false) == L"xplane" && with_game_hidden(L"acc", L"acc", true) == L"acc",
          "game tabs: hiding and showing, no duplicates");
    uint32_t rgb = 0;
    check(parse_color(L"#1A2b3C", rgb) && rgb == 0x1a2b3c && parse_color(L"00ff00", rgb) && rgb == 0x00ff00 &&
              !parse_color(L"12345", rgb) && !parse_color(L"zzzzzz", rgb) && color_text(0x0a0b0c) == L"0A0B0C",
          "game tabs: colours as RRGGBB text");
    check(hsv_rgb(0, 1, 1) == 0xff0000 && hsv_rgb(120, 1, 1) == 0x00ff00 && hsv_rgb(240, 1, 1) == 0x0000ff &&
              hsv_rgb(0, 0, 1) == 0xffffff && hsv_rgb(77, 0.4f, 0) == 0,
          "game tabs: HSV to RGB");
    bool round = true;
    for (const uint32_t c : {0xd2691eu, 0x3dd6c6u, 0x7f00ffu, 0x808080u}) {
        float h = 0, s = 0, v = 0;
        rgb_hsv(c, h, s, v);
        const uint32_t back = hsv_rgb(h, s, v);
        for (int shift = 0; shift <= 16; shift += 8)
            round &= std::abs(static_cast<int>((back >> shift) & 0xff) - static_cast<int>((c >> shift) & 0xff)) <= 1;
    }
    check(round, "game tabs: RGB to HSV and back");
    bool readable = true, fillKept = true, textOnFill = true;
    for (const uint32_t c : {0xffff00u, 0xffffffu, 0x000000u, 0x00ffffu, 0x1010a0u, 0xff5f57u, 0x808080u})
        for (const bool dark : {false, true}) {
            const AccentColors a = accent_from(c, dark);
            readable &= contrast(a.accent, dark ? 0x1c1c1e : 0xf2f2f5) >= 3.0 && a.ring == a.accent;
            fillKept &= a.fill == c;
            textOnFill &= contrast(a.onAccent, a.fill) >= contrast(a.onAccent ^ 0xffffffu, a.fill);
        }
    check(readable, "game tabs: an accent from any colour reads against the window (3:1), light and dark");
    check(fillKept && textOnFill, "game tabs: the fill is the colour picked, with the better of black and white on it");

    const fs::path dir = fs::temp_directory_path() / L"sim-qvfr-game-tabs-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::wstring ini = (dir / L"sim-qvfr.ini").wstring();
    Settings s;
    s.hiddenGames = L"acc";
    s.gameColors[L"xplane"] = 0x7f00ff;
    save_settings(ini, s);
    Settings back = load_settings(ini);
    check(back.hiddenGames == L"acc" && back.gameColors.size() == 1 && back.gameColors[L"xplane"] == 0x7f00ff,
          "game tabs: hidden games and colours round-trip");
    back.gameColors.clear();
    save_settings(ini, back);
    wchar_t value[16] = {};
    GetPrivateProfileStringW(L"launcher", L"color_xplane", L"gone", value, 16, ini.c_str());
    check(std::wstring(value) == L"gone" && load_settings(ini).gameColors.empty(), "game tabs: the default colour back removes the key");
    back.gameColors[L"ace"] = 0x123456;
    save_settings(ini, back);
    const Profile p = settings_profile(load_settings(ini), L"x");
    check(std::none_of(p.keys.begin(), p.keys.end(), [](const ProfileKey& k) { return k.section == L"launcher"; }),
          "game tabs: launcher-wide, never in a profile or a game's own settings");
    WritePrivateProfileStringW(L"launcher", L"game", L"ace", ini.c_str());
    switch_game_settings(ini, L"ace", L"xplane");
    const Settings after = load_settings(ini);
    check(after.hiddenGames == L"acc" && after.gameColors.count(L"ace") == 1, "game tabs: switching games keeps them");
    // Runtime composition (native_quad_views): auto (1) by default, read so from an ini without the key (one from
    // before the setting). No control any more: an off the old card saved reads as auto once, a hand-set value after
    // that is kept.
    {
        const std::wstring old = (dir / L"before-native.ini").wstring();
        WritePrivateProfileStringW(L"experiment", L"periphery_scale", L"40", old.c_str());
        check(Settings{}.nativeQuad == 1 && load_settings(old).nativeQuad == 1,
              "runtime composition: auto by default, also for an ini from before the setting");
        WritePrivateProfileStringW(L"experiment", L"native_quad_views", L"0", old.c_str());  // the old card's off
        check(load_settings(old).nativeQuad == 1, "runtime composition: the old card's off reads as auto");
        save_settings(old, load_settings(old));
        check(GetPrivateProfileIntW(L"experiment", L"native_quad_views", -1, old.c_str()) == 1 &&
                  GetPrivateProfileIntW(L"launcher", L"native_quad_reset", -1, old.c_str()) == 1,
              "runtime composition: saved as auto, the reset marked done");
        WritePrivateProfileStringW(L"experiment", L"native_quad_views", L"0", old.c_str());  // set by hand afterwards
        Settings kept = load_settings(old);
        check(kept.nativeQuad == 0, "runtime composition: a hand-set off after the reset is kept");
        save_settings(old, kept);
        check(load_settings(old).nativeQuad == 0, "runtime composition: and saved back as it was");
    }
    fs::remove_all(dir, ec);
}

// Per-game settings (settings.h store/restore/seed/switch_game_settings, profiles.h game_settings_target): an update
// starts every game from the settings in use, then each game's changes stay its own; the shared keys stay shared.
void per_game_settings_checks() {
    const fs::path dir = fs::temp_directory_path() / L"sim-qvfr-per-game-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::wstring ini = (dir / L"sim-qvfr.ini").wstring();
    // A user's ini before the update: one set of settings, ACE selected, no games folder.
    Settings before;
    before.mode = RenderMode::Qvfr;
    before.inner = 160;
    before.outer = 45;
    save_settings(ini, before);
    WritePrivateProfileStringW(L"launcher", L"game", L"ace", ini.c_str());
    WritePrivateProfileStringW(L"menu", L"yaw", L"7", ini.c_str());
    set_current_profile(ini.c_str(), L"Balanced");
    const std::string untouched = read_text(ini);

    std::size_t count = 0;
    games::all_games(count);
    check(seed_game_settings(ini) == static_cast<int>(count), "per-game: the first start after the update makes a copy for every game");
    const std::string afterSeed = read_text(ini);
    check(afterSeed == untouched, "per-game: seeding leaves the ini as it was (nothing changes for the user)");
    bool replicated = true;
    for (const wchar_t* id : {L"ace", L"acc", L"xplane"}) {
        const std::wstring copy = game_settings_file(ini, id);
        replicated &= ini_int(copy, L"experiment", L"focus_density") == 160 && ini_int(copy, L"experiment", L"periphery_scale") == 45 &&
                      current_profile(copy.c_str()) == L"Balanced";
    }
    check(replicated, "per-game: every game's copy holds the settings in use, and the profile in use");
    check(ini_int(game_settings_file(ini, L"xplane"), L"menu", L"yaw") == -1 &&
              ini_int(game_settings_file(ini, L"xplane"), L"launcher", L"game") == -1,
          "per-game: a copy holds no shared keys ([menu], [launcher])");
    check(seed_game_settings(ini) == 0, "per-game: seeding again makes nothing (existing copies are kept)");

    // ACE -> X-Plane: X-Plane starts from the replicated values; a change there stays X-Plane's.
    check(switch_game_settings(ini, L"ace", L"xplane"), "per-game: switching ACE -> X-Plane works");
    WritePrivateProfileStringW(L"launcher", L"game", L"xplane", ini.c_str());
    check(load_settings(ini).inner == 160, "per-game: X-Plane's first values are the replicated ones");
    Settings x = load_settings(ini);
    x.inner = 100;
    x.objectDetail = 50;
    save_settings(ini, x);
    set_current_profile(ini.c_str(), L"Performance");
    check(switch_game_settings(ini, L"xplane", L"ace"), "per-game: switching X-Plane -> ACE works");
    WritePrivateProfileStringW(L"launcher", L"game", L"ace", ini.c_str());
    const Settings ace = load_settings(ini);
    check(ace.inner == 160 && ace.outer == 45, "per-game: back on ACE, its 160% inner is as it was (X-Plane's 100% stayed X-Plane's)");
    check(current_profile(ini.c_str()) == L"Balanced", "per-game: ACE's profile in use comes back with it");
    check(ini_int(ini, L"menu", L"yaw") == 7, "per-game: shared keys ([menu]) aren't swapped");
    check(switch_game_settings(ini, L"ace", L"xplane"), "per-game: and back to X-Plane");
    const Settings xBack = load_settings(ini);
    check(xBack.inner == 100 && xBack.objectDetail == 50 && current_profile(ini.c_str()) == L"Performance",
          "per-game: X-Plane's own 100% inner, 50% object detail and profile come back");
    WritePrivateProfileStringW(L"launcher", L"game", L"xplane", ini.c_str());

    // A game without a copy (a new game in an update): it starts from the values in use.
    DeleteFileW(game_settings_file(ini, L"acc").c_str());
    check(switch_game_settings(ini, L"xplane", L"acc") && load_settings(ini).inner == 100 &&
              ini_int(game_settings_file(ini, L"acc"), L"experiment", L"focus_density") == 100,
          "per-game: a game without a copy starts from the settings in use, and gets its copy");
    WritePrivateProfileStringW(L"launcher", L"game", L"acc", ini.c_str());

    // A copy older than a setting (ACE's, saved before [quad] sharpen_percent): switching to it gives that setting its
    // default, not the value the last game left in the ini; the last game keeps its own.
    WritePrivateProfileStringW(L"quad", L"sharpen_percent", nullptr, game_settings_file(ini, L"ace").c_str());
    Settings sharp = load_settings(ini);
    sharp.sharpen = 100;
    save_settings(ini, sharp);
    check(switch_game_settings(ini, L"acc", L"ace") && load_settings(ini).sharpen == 0 && load_settings(ini).inner == 160,
          "per-game: a setting newer than the game's copy comes back at its default (ACC's 100% sharpening stays ACC's)");
    check(switch_game_settings(ini, L"ace", L"acc") && load_settings(ini).sharpen == 100,
          "per-game: and ACC's own 100% sharpening comes back with it");

    // The headset menu's writes (game_settings_target): the launcher's ini only for the game it shows.
    check(game_settings_target(ini, L"acc") == ini, "per-game: the game the launcher shows writes to the launcher's ini");
    check(game_settings_target(ini, L"ace") == game_settings_file(ini, L"ace"),
          "per-game: another game (running while the launcher shows this one) writes to its own copy");
    const std::wstring fresh = (dir / L"fresh.ini").wstring();
    WritePrivateProfileStringW(L"experiment", L"mode", L"views4", fresh.c_str());
    check(game_settings_target(fresh, L"ace") == fresh && game_settings_target(fresh, L"xplane") == game_settings_file(fresh, L"xplane"),
          "per-game: an ini without [launcher] game means ACE, as the launcher reads it");
    fs::remove_all(dir, ec);
}

// An elevated launch's registrations (explicit_layers.h), on a scratch key holding someone else's values: only ours
// are added and taken out, nothing else changes, the key stays, and only what the ledger lists comes out.
// What older versions left in their log folders (launcher/leftovers.h): only the copies of a game's program, by name,
// content, place and kind; nothing else, whatever it is called.
void leftovers_checks() {
    const fs::path root = temp_path(L"simqvfr-leftovers-test"), logs = root / L"logs", layerLogs = root / L"layer" / L"logs";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(logs / L"captures");
    fs::create_directories(layerLogs);
    // A Windows program as the old copy was: "MZ", the PE header's offset at 0x3c, "PE\0\0" there.
    std::string program(0x200, '\0');
    program[0] = 'M';
    program[1] = 'Z';
    program[0x3c] = static_cast<char>(0x80);
    program.replace(0x80, 4, std::string("PE\0\0", 4));
    const auto put = [](const fs::path& p, const std::string& data) { write_file(p.wstring(), data); return p; };
    const fs::path lmu = put(logs / L"Le Mans Ultimate-66942337813b-image.dump", program);
    const fs::path ace = put(layerLogs / L"AssettoCorsaEVO-0123456789ab-image.dump", program);
    const fs::path lowerName = put(logs / L"le mans ultimate-aaaaaaaaaaaa-image.dump", program);  // the file's own case
    const fs::path otherGame = put(logs / L"Some Other Game-66942337813b-image.dump", program);
    // Windows names ignore case: the case variants get hashes of their own, or they would be the copy itself.
    const fs::path upperHash = put(logs / L"Le Mans Ultimate-ABCDEF123456-image.dump", program);
    const fs::path shortHash = put(logs / L"Le Mans Ultimate-66942337813-image.dump", program);
    const fs::path noHash = put(logs / L"Le Mans Ultimate-image.dump", program);
    const fs::path upperSuffix = put(logs / L"Le Mans Ultimate-888888888888-IMAGE.DUMP", program);
    const fs::path extra = put(logs / L"Le Mans Ultimate-66942337813b-image.dump.bak", program);
    const fs::path notProgram = put(logs / L"Le Mans Ultimate-111111111111-image.dump", std::string(4096, 'x'));
    std::string mzOnly = program;
    mzOnly.replace(0x80, 4, "XXXX");
    const fs::path noPe = put(logs / L"Le Mans Ultimate-222222222222-image.dump", mzOnly);
    const fs::path tiny = put(logs / L"Le Mans Ultimate-333333333333-image.dump", "MZ");
    const fs::path nested = put(logs / L"captures" / L"Le Mans Ultimate-444444444444-image.dump", program);
    const fs::path session = put(logs / L"session-20261113-090000.log", "log");
    const fs::path hang = put(logs / L"hang-120000.dmp", program);
    fs::create_directories(logs / L"X-Plane-555555555555-image.dump");  // a folder by that name
    // A link by that name to a program elsewhere: the link isn't removed, nor what it points at (if links can be made).
    const fs::path target = put(root / L"target.exe", program);
    std::error_code linkError;
    fs::create_symlink(target, logs / L"Le Mans Ultimate-666666666666-image.dump", linkError);

    check(is_leftover(lmu) && is_leftover(ace) && is_leftover(lowerName), "leftovers: the old copies' names and content match");
    check(!is_leftover(otherGame) && !is_leftover(upperHash) && !is_leftover(shortHash) && !is_leftover(noHash) &&
              !is_leftover(upperSuffix) && !is_leftover(extra),
          "leftovers: any other name doesn't (another game, the hash's case or length, the suffix)");
    check(!is_leftover(notProgram) && !is_leftover(noPe) && !is_leftover(tiny),
          "leftovers: the right name with other content doesn't");

    const Leftovers r = remove_leftovers({logs.wstring(), layerLogs.wstring(), (root / L"missing").wstring()});
    check(r.removed == 3 && r.bytes == 3 * program.size() && r.files.size() == 3 && !fs::exists(lmu) && !fs::exists(ace) &&
              !fs::exists(lowerName),
          "leftovers: the copies in both log folders removed, each named, sizes counted");
    bool kept = true;
    for (const fs::path& p : {otherGame, upperHash, shortHash, noHash, upperSuffix, extra, notProgram, noPe, tiny, nested,
                              session, hang, target})
        kept = kept && fs::exists(p);
    check(kept && fs::is_directory(logs / L"X-Plane-555555555555-image.dump"),
          "leftovers: every other file, subfolders and folders by that name untouched");
    if (!linkError) {
        check(fs::is_symlink(fs::symlink_status(logs / L"Le Mans Ultimate-666666666666-image.dump")) && fs::exists(target),
              "leftovers: a link by that name and its target untouched");
        // A log folder that is itself a link: not looked into.
        const fs::path linked = root / L"linked-logs";
        std::error_code dirLinkError;
        fs::create_directory_symlink(layerLogs, linked, dirLinkError);
        const fs::path behind = put(layerLogs / L"Le Mans Ultimate-777777777777-image.dump", program);
        if (!dirLinkError)
            check(remove_leftovers({linked.wstring()}).removed == 0 && fs::exists(behind),
                  "leftovers: a log folder that is a link isn't looked into");
    } else {
        std::printf("note leftovers: links can't be made here (no symlink right); their checks skipped\n");
    }
    check(remove_leftovers({logs.wstring(), layerLogs.wstring()}).removed == (linkError ? 0 : 1),
          "leftovers: a second pass finds only what was added since");
    fs::remove_all(root, ec);
}

void explicit_layers_checks() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / L"simqvfr-explicit-test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / L"ledgers");
    const wchar_t* scratch = L"Software\\simqvfr-launcher-test-explicit";
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    HKEY base = nullptr, key = nullptr, sub = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, scratch, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &base, nullptr);
    RegCreateKeyExW(base, L"Explicit", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr);
    const auto put = [&](const std::wstring& name, DWORD v) {
        RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    };
    const auto value = [&](const std::wstring& name, DWORD& v) {
        DWORD type = 0, size = sizeof(v);
        return RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS &&
               type == REG_DWORD;
    };
    // Every value of the key as stored (name, type, bytes), and its subkeys.
    const auto snapshot = [&]() {
        std::vector<std::wstring> out;
        for (DWORD i = 0;; ++i) {
            wchar_t name[1024];
            BYTE data[1024];
            DWORD nameLength = 1024, type = 0, size = sizeof(data);
            if (RegEnumValueW(key, i, name, &nameLength, nullptr, &type, data, &size) != ERROR_SUCCESS)
                break;
            std::wstring line = std::wstring(name, nameLength) + L"|" + std::to_wstring(type) + L"|";
            for (DWORD b = 0; b < size; ++b)
                line += std::to_wstring(data[b]) + L",";
            out.push_back(line);
        }
        for (DWORD i = 0;; ++i) {
            wchar_t name[256];
            DWORD length = 256;
            if (RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            out.push_back(L"[" + std::wstring(name, length) + L"]");
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto key_exists = [&]() {
        HKEY k = nullptr;
        const bool there = RegOpenKeyExW(base, L"Explicit", 0, KEY_READ, &k) == ERROR_SUCCESS;
        if (there)
            RegCloseKey(k);
        return there;
    };

    // Someone else's: an enabled and a disabled layer, a string value and a subkey.
    const std::wstring theirs = L"C:\\Program Files\\Other\\layer.json", theirsOff = L"C:\\Other\\off.json";
    put(theirs, 0);
    put(theirsOff, 1);
    const std::wstring text = L"not a layer";
    RegSetValueExW(key, L"Note", 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                   static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCreateKeyExW(key, L"Sub", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &sub, nullptr);
    RegCloseKey(sub);
    const std::vector<std::wstring> before = snapshot();

    const std::wstring ours = (dir / L"run-1" / L"XR_APILAYER_SIM_qvfr.json").wstring(),
                       below = (dir / L"run-1" / L"XR_APILAYER_MBUCCHIA_toolkit.json").wstring();
    const std::wstring ledger = (dir / L"ledgers" / L"run-1.txt").wstring(), backup = (dir / L"before.reg").wstring();
    std::vector<std::wstring> notes;
    std::wstring error;
    const bool registered = register_layers(base, L"Explicit", false, {ours, below, theirs}, ledger, backup, notes, error);
    DWORD v = 1;
    check(registered && value(ours, v) && v == 0 && value(below, v) && v == 0, "explicit: the launch's manifests added, enabled");
    std::string ledgerText;
    Ledger listed;
    check(read_file(ledger, ledgerText) && parse_ledger(ledgerText, listed) && listed.values == std::vector<std::wstring>{ours, below} &&
              listed.launcher == process_stamp(GetCurrentProcess()) && listed.game.pid == 0,
          "explicit: the ledger lists ours and this launcher; a value already there isn't ours, so it isn't listed");
    check(value(theirs, v) && v == 0 &&
              std::any_of(notes.begin(), notes.end(), [&](const std::wstring& n) { return n.find(L"not ours") != std::wstring::npos; }),
          "explicit: a manifest already registered is left as it is, and said");
    std::vector<std::wstring> now = snapshot();
    std::vector<std::wstring> others;
    std::copy_if(now.begin(), now.end(), std::back_inserter(others), [&](const std::wstring& line) {
        return line.rfind(ours + L"|", 0) != 0 && line.rfind(below + L"|", 0) != 0;
    });
    check(others == before, "explicit: every other value and the subkey unchanged, byte for byte");
    std::string reg;
    check(read_file(backup, reg) && reg.size() > 2 && reg[0] == '\xff' && reg[1] == '\xfe', "explicit: a .reg backup of the key as it was");

    const ProcessStamp game{4242, 99};
    check(ledger_game_started(ledger, game) && read_file(ledger, ledgerText) && parse_ledger(ledgerText, listed) && listed.game == game &&
              listed.values.size() == 2,
          "explicit: the game written into the ledger, the values kept");

    // The user switched one off meanwhile: that one is theirs now and stays.
    put(below, 1);
    notes.clear();
    check(unregister_layers(base, L"Explicit", ledger, notes) && !value(ours, v) && value(below, v) && v == 1 && !fs::exists(ledger),
          "explicit: ours taken out; one changed since is left as it is; the ledger goes");
    RegDeleteValueW(key, below.c_str());
    check(snapshot() == before && key_exists(), "explicit: after the launch the key holds exactly what it held, and stays");
    notes.clear();
    check(unregister_layers(base, L"Explicit", ledger, notes) && snapshot() == before, "explicit: no ledger, nothing to do");

    // A value that looks like ours but no ledger lists: never taken out.
    const std::wstring lookalike = (dir / L"run-0" / L"XR_APILAYER_SIM_qvfr.json").wstring();
    put(lookalike, 0);
    const std::wstring ledger2 = (dir / L"ledgers" / L"run-2.txt").wstring(), ledger3 = (dir / L"ledgers" / L"run-3.txt").wstring();
    const std::wstring ours2 = (dir / L"run-2" / L"XR_APILAYER_SIM_qvfr.json").wstring(),
                       ours3 = (dir / L"run-3" / L"XR_APILAYER_SIM_qvfr.json").wstring();
    notes.clear();
    check(register_layers(base, L"Explicit", false, {ours2}, ledger2, L"", notes, error) &&
              register_layers(base, L"Explicit", false, {ours3}, ledger3, L"", notes, error) &&
              ledger_game_started(ledger2, {1, 1}) && ledger_game_started(ledger3, {2, 2}),
          "explicit: two launches registered");
    write_file((dir / L"ledgers" / L"notes.txt").wstring(), "something else\r\nvalue " + narrow(lookalike) + "\r\n");
    // The sweep: launch 2's game still runs, launch 3's is gone.
    notes.clear();
    sweep_ledgers(base, L"Explicit", (dir / L"ledgers").wstring(), notes, [](const ProcessStamp& p) { return p.pid == 1; });
    check(value(ours2, v) && fs::exists(ledger2) && !value(ours3, v) && !fs::exists(ledger3),
          "explicit: the sweep takes out a finished launch's values only, and leaves a running one's");
    check(value(lookalike, v) && fs::exists(dir / L"ledgers" / L"notes.txt"),
          "explicit: a lookalike value no ledger lists stays, and a file that isn't a ledger is ignored");
    notes.clear();
    sweep_ledgers(base, L"Explicit", (dir / L"ledgers").wstring(), notes, [](const ProcessStamp&) { return false; });
    check(!value(ours2, v) && !fs::exists(ledger2) && value(lookalike, v), "explicit: once its game is gone, the other goes too");
    RegDeleteValueW(key, lookalike.c_str());
    check(snapshot() == before, "explicit: the key back to exactly what it held");

    // A ledger is a user-writable file read as administrator: only a .json straight in one of our run folders comes
    // out, whatever else it lists at DWORD 0.
    {
        const std::wstring climbs = (dir / L"run-5" / L".." / L"climbs.json").wstring(),
                           elsewhere = (dir / L"other" / L"layer.json").wstring(), notJson = (dir / L"run-5" / L"layer.dll").wstring(),
                           relative = L"run-5\\relative.json", real = (dir / L"run-5" / L"XR_APILAYER_SIM_qvfr.json").wstring();
        for (const std::wstring& forged : {climbs, elsewhere, notJson, relative, real})
            put(forged, 0);
        const std::wstring forgedLedger = (dir / L"ledgers" / L"run-5.txt").wstring();
        write_file(forgedLedger, ledger_text(Ledger{{}, {}, {theirs, climbs, elsewhere, notJson, relative, real}}));
        notes.clear();
        check(unregister_layers(base, L"Explicit", forgedLedger, notes) && !value(real, v) && value(theirs, v) && v == 0 &&
                  value(climbs, v) && value(elsewhere, v) && value(notJson, v) && value(relative, v) && !fs::exists(forgedLedger) &&
                  std::count_if(notes.begin(), notes.end(), [](const std::wstring& n) { return n.find(L"ignored") != std::wstring::npos; }) == 5,
              "explicit: a ledger's values outside our run folders are ignored, said, and left as they are");
        for (const std::wstring& forged : {climbs, elsewhere, notJson, relative})
            RegDeleteValueW(key, forged.c_str());
        check(snapshot() == before, "explicit: the key back to exactly what it held after a forged ledger");
    }

    // No key yet: created (never replaced), and it stays after, empty.
    RegCloseKey(key);
    RegDeleteTreeW(base, L"Explicit");
    notes.clear();
    const std::wstring ledger4 = (dir / L"ledgers" / L"run-4.txt").wstring();
    check(register_layers(base, L"Explicit", false, {ours}, ledger4, backup + L"2", notes, error) && key_exists() &&
              !fs::exists(backup + L"2") &&
              std::any_of(notes.begin(), notes.end(), [](const std::wstring& n) { return n.find(L"created") == 0; }),
          "explicit: a missing key is created (nothing to back up), and said");
    notes.clear();
    RegOpenKeyExW(base, L"Explicit", 0, KEY_ALL_ACCESS, &key);
    check(unregister_layers(base, L"Explicit", ledger4, notes) && key_exists() && snapshot().empty(),
          "explicit: the key it created stays, empty");
    RegCloseKey(key);

    // Ledger text and process stamps.
    Ledger round{{7, 8}, {9, 10}, {L"C:\\a b\\x.json", L"D:\\y.json"}}, back;
    check(parse_ledger(ledger_text(round), back) && back == round, "explicit: the ledger round trips");
    check(!parse_ledger("value C:\\x.json\r\n", back) && !parse_ledger(narrow(L"Sim-QVFR OpenXR registrations\r\nlauncher x\r\n"), back),
          "explicit: a file without the ledger's header, or a broken line, isn't a ledger");
    const ProcessStamp self = process_stamp(GetCurrentProcess());
    check(stamp_running(self) && !stamp_running({self.pid, self.created + 1}) && !stamp_running({}),
          "explicit: a process is known by its id and start time");

    RegCloseKey(base);
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    fs::remove_all(dir, ec);
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    per_game_settings_checks();
    game_tabs_checks();
    launch_status_checks();
    session_history_checks();
    log_retention_checks();
    log_retention_strict_checks();
    explicit_layers_checks();
    leftovers_checks();
    return test_result();
}

// Offline check of the launcher's launch plan (src/launcher/launch_plan.h) against fake implicit
// layer registrations in a temporary folder: which layers go below the Sim-QVFR layer and in what order,
// which are silenced or left alone, the explicit manifests written, the variables set, the
// environment block, Steam's library list, and the manifest JSON round trip.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <windows.h>

#include "games/ace/game.h"
#include "games/acc/game.h"
#include "games/lmu/game.h"
#include "games/xplane/game.h"
#include "input/hotkeys.h"
#include "launcher/launch_plan.h"
#include "launcher/launch_watch.h"
#include "launcher/explicit_layers.h"
#include "launcher/game_tabs.h"
#include "launcher/install.h"
#include "launcher/layer_order.h"
#include "launcher/preflight.h"
#include "launcher/report.h"
#include "launcher/session_history.h"
#include "launcher/settings.h"
#include "launcher/update_check.h"
#include "profile/profiles.h"
#include "sha256_file.h"

using namespace simqvfr;
using namespace simqvfr::launcher;
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
                       "\",\n    \"library_path\": \".\\\\" + name + ".dll\",\n    \"api_version\": \"1.0\",\n    \"implementation_version\": \"1\"";
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

int ini_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, -1, ini.c_str()));
}

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
        mine.multiView = true;
        mine.hotkeys[0].key = KeyCombo{'K', true, false, false};
        mine.updateCheck = true;
        mine.hiddenGames = L"acc";
        mine.gameColors[L"ace"] = 0x123456;
        const Settings back = restored_defaults(mine);
        const Settings d;
        check(back.outer == d.outer && back.menuYaw == d.menuYaw && back.cheeky.load == d.cheeky.load && back.crop == d.crop &&
                  !back.multiView,
              "restore defaults: the game's settings go back to the defaults (multi-view rendering off)");
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

// The session history (launcher/session_history.h): records kept per session, pruned, compared.
void session_history_checks() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path dir = fs::path(temp) / L"simqvfr-session-history-test";
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
    check(one.size() == 1 && one[0] == L"Outer 50% \u2192 40%", "sessions: one change, as Outer 50% → 40%");
    check(setting_changes(a, a).empty(), "sessions: no change between the same settings");
    Settings c = b;
    c.crop = true;
    c.inner = 120;
    c.cheeky.load = true;  // on: its details only once it was on before too
    const std::vector<std::wstring> three = setting_changes(a, c);
    check(three.size() == 4 && three[1] == L"Inner 100% \u2192 120%" && three[2] == L"Crop on" && three[3] == L"Cheeky Foveated DLSS on",
          "sessions: changes in the panes' order, a switch as on or off");
    check(changes_text(three, 2) == L"Outer 50% \u2192 40%, Inner 100% \u2192 120% and 2 more" &&
              changes_text({L"A", L"B"}) == L"A and B" && changes_text({L"A"}) == L"A",
          "sessions: changes as one phrase, the rest counted");
    Settings hidden = a;
    hidden.cropTop = 10;  // the crop is off: its sides don't count
    check(setting_changes(a, hidden).empty(), "sessions: a crop side with the crop off is not a change");

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
              L"vs previous: +6.5 fps, 90th percentile \u22121.20 ms, after Outer 50% \u2192 40%",
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
    const std::string untouched = [&] {
        std::ifstream in(ini, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();

    std::size_t count = 0;
    games::all_games(count);
    check(seed_game_settings(ini) == static_cast<int>(count), "per-game: the first start after the update makes a copy for every game");
    const std::string afterSeed = [&] {
        std::ifstream in(ini, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
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

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    folder = fs::path(temp) / L"simqvfr-launcher-test";
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
              "profile: the launch loads the profile's order below the Sim-QVFR layer");
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
        // Run with anti-cheat: Cheeky's layer off, over [cheeky] load and on_layers alike.
        LauncherLayers wantsCheeky;
        set_launcher_layer(wantsCheeky, kCheekyLayer, true, false);
        p = plan_layers(regs, launch_options(wantsCheeky, false, true, true, {}, true));
        const std::vector<std::string> loaded = names(p);
        check(!locked(kCheekyLayer).on && locked(kCheekyLayer).lock.find("anti-cheat") != std::string::npos &&
                  std::find(loaded.begin(), loaded.end(), std::string(kCheekyLayer)) == loaded.end(),
              "locks: Run with anti-cheat keeps Cheeky's layer off, whatever [cheeky] and the profile say");
        std::vector<Registration> cheekyOn = registrations;
        cheekyOn.push_back({layer("cheeky-on", kCheekyLayer, "CHEEKY_OPENXR_LAYER_DISABLE"), true});  // on in Windows
        p = plan_layers(cheekyOn, launch_options(LauncherLayers{}, false, false, false, {}, true));
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
    // switches, the rewrite in order with a backup, and a stale list refused.
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
    const auto quadVars = launch_variables(quadPlan, L"C:\\run", L"i", L"l", L"r");
    check(value_of(quadVars, L"XR_ENABLE_API_LAYERS") == L"XR_APILAYER_SIM_qvfr" &&
              value_of(quadVars, L"DISABLE_XR_APILAYER_DIENERTECH_VECTORXR") == L"1",
          "mode=quad: only the Sim-QVFR layer, VectorXR silenced");
    // An OpenComposite game (ACC): its loader keeps only the first and last of a list, so the loader gets the
    // Sim-QVFR layer alone and the layer chains the rest (SIM_QVFR_CHAIN), still silenced as implicit copies.
    const auto chainVars = launch_variables(plan, L"C:\\run", L"i", L"l", L"r", {}, true);
    check(value_of(chainVars, L"XR_ENABLE_API_LAYERS") == L"XR_APILAYER_SIM_qvfr" &&
              value_of(chainVars, L"SIM_QVFR_CHAIN") == L"XR_APILAYER_MBUCCHIA_toolkit;XR_APILAYER_DIENERTECH_VECTORXR" &&
              value_of(chainVars, L"DISABLE_XR_APILAYER_MBUCCHIA_toolkit") == L"1",
          "chained: the loader gets the Sim-QVFR layer alone; SIM_QVFR_CHAIN lists the rest in order");
    check(value_of(launch_variables(quadPlan, L"C:\\run", L"i", L"l", L"r", {}, true), L"SIM_QVFR_CHAIN") == L"<unset>" &&
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
            check(!f.shape.corners && f.shape == simqvfr::FocusShape{}, "settings: the focus shape is a box by default");
            f.shape.corners = true;
            f.shape.in[static_cast<int>(simqvfr::FocusCorner::TopLeft)] = 25;
            f.shape.in[static_cast<int>(simqvfr::FocusCorner::TopRight)] = 25;
            f.shape.vertical[static_cast<int>(simqvfr::FocusCorner::BottomLeft)] = 60;  // clamps to kFocusCornerMax
            save_live_settings(ini, f);
            std::string shapeText;
            read_file(ini, shapeText);
            const Settings shape = load_settings(ini);
            check(shape.shape.corners && shape.shape.in[0] == 25 && shape.shape.in[1] == 25 &&
                      shape.shape.vertical[static_cast<int>(simqvfr::FocusCorner::BottomLeft)] == simqvfr::kFocusCornerMax &&
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
        {
            Settings detail = cropBack;
            check(detail.objectDetail == 100, "settings: object detail defaults to 100% (the focus views' LOD)");
            detail.objectDetail = 40;
            save_live_settings(ini, detail);
            check(load_settings(ini).objectDetail == 40, "settings: object detail round-trips, live");
            detail.objectDetail = 250;  // above kObjectDetailRange: clamped
            save_settings(ini, detail);
            check(load_settings(ini).objectDetail == kObjectDetailRange.max, "settings: object detail is clamped to 0-100%");
            check(!detail.multiView, "settings: multi-view rendering defaults to off");
            detail.multiView = true;
            save_settings(ini, detail);
            check(load_settings(ini).multiView && ini_int(ini, L"experiment", L"multi_view") == 1,
                  "settings: multi-view rendering round-trips ([experiment] multi_view=1)");
            check(per_game_key(L"experiment", L"multi_view"), "settings: multi_view is a per-game key");
            detail.multiView = false;
            save_settings(ini, detail);
            check(ini_int(ini, L"experiment", L"multi_view") == 0, "settings: and off again (multi_view=0)");
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
        make(L"session-20261101-100000.log", 30 * day);
        make(L"session-20261103-100000.log", std::chrono::hours(4));
        make(L"session-20261103-110000.log", std::chrono::hours(3));
        make(L"session-20261103-120000.log", std::chrono::hours(2));
        make(L"session-20261103-130000.log", std::chrono::hours(1));
        make(L"sim-qvfr.log", std::chrono::minutes(1));  // the old single log: not a session log
        for (int i = 0; i < 4; ++i)
            make(L"cpu-profile-2026110" + std::to_wstring(i) + L".txt", std::chrono::hours(5 - i));
        make(L"cpu-profile-old.txt", 9 * day);
        make(L"cheeky-snapshot.json", std::chrono::hours(1));
        make(L"hang-101010.dmp", std::chrono::hours(1));
        make(L"hang-090909.dmp", 9 * day);
        make(L"hang-111111.dmp", std::chrono::minutes(5), static_cast<size_t>(kReportMaxDumpBytes) + 1);
        make(L"other.txt", std::chrono::hours(1));
        const std::wstring settingsIni = (folder / L"report" / L"sim-qvfr.ini").wstring();
        const std::wstring reportRuntime = (folder / L"report" / L"runtime.ini").wstring();
        write_file(settingsIni, "[experiment]\r\nmode=views4\r\n");
        write_file(reportRuntime, "[runtime]\r\nrecommended_width=2584\r\n");
        const uint64_t nowTicks = static_cast<uint64_t>(now.time_since_epoch().count());
        const std::vector<ReportFile> files = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        std::vector<std::wstring> zipped;
        for (const ReportFile& f : files)
            zipped.push_back(f.name);
        const std::vector<std::wstring> want = {L"sim-qvfr.ini",         L"runtime.ini",
                                                L"session-20261103-130000.log",  L"session-20261103-120000.log",
                                                L"session-20261103-110000.log",  L"cpu-profile-20261103.txt",
                                                L"cpu-profile-20261102.txt", L"cpu-profile-20261101.txt",
                                                L"cheeky-snapshot.json",     L"hang-101010.dmp"};
        check(zipped == want, "report: the inis, the newest three session logs and CPU captures, Cheeky's snapshot, "
                              "a recent dump under the cap; no old, oversized or other files");
        for (const wchar_t* name : {L"session-20261103-100000.log", L"session-20261103-110000.log", L"session-20261103-120000.log",
                                    L"session-20261103-130000.log"})
            fs::remove(logs / name);
        const std::vector<ReportFile> old = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        check(std::count_if(old.begin(), old.end(), [](const ReportFile& f) { return f.name == L"session-20261101-100000.log"; }) == 1,
              "report: only an old session log: the newest one still goes in");

        // A launch the loader failed has no session log: the newest run's plan and loader output go in instead,
        // only the end of a long output.
        fs::create_directories(logs / L"run-20261103-120000");
        fs::create_directories(logs / L"run-20261103-130000");
        write_file((logs / L"run-20261103-120000" / L"launch.txt").wstring(), "older launch");
        write_file((logs / L"run-20261103-130000" / L"launch.txt").wstring(), "newest launch");
        write_file((logs / L"run-20261103-130000" / kLoaderOutputName).wstring(),
                   std::string(static_cast<size_t>(kReportLoaderOutputBytes), 'x') + "Error [GENERAL | xrCreateInstance]");
        const std::vector<ReportFile> runs = report_files({logs.wstring(), reportRuntime, settingsIni}, nowTicks);
        const auto entry = [&](const wchar_t* name) {
            const auto it = std::find_if(runs.begin(), runs.end(), [&](const ReportFile& f) { return f.name == name; });
            return it == runs.end() ? nullptr : &*it;
        };
        const ReportFile* runPlan = entry(L"launch.txt");
        const ReportFile* loader = entry(kLoaderOutputName);
        check(runPlan && runPlan->path.find(L"run-20261103-130000") != std::wstring::npos,
              "report: the newest run's launch.txt goes in");
        check(loader && loader->path.empty() && loader->data.size() == kReportLoaderOutputBytes &&
                  loader->data.ends_with("xrCreateInstance]"),
              "report: a long loader output goes in as its last part");

        // The game's crash reports (GameInfo::crashReports): only those during a zipped session, the newest
        // kReportGameCrashes, each as game-crash-<local time>/ with its CrashContext and log redacted.
        {
            const fs::path base = folder / L"report-crashes", crashLogs = base / L"logs", crashes = base / L"Crashes";
            fs::create_directories(crashLogs);
            fs::create_directories(crashes);
            const auto stamp = [](fs::file_time_type t) {
                const uint64_t ticks = static_cast<uint64_t>(t.time_since_epoch().count());
                const FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
                SYSTEMTIME utc{}, local{};
                FileTimeToSystemTime(&ft, &utc);
                SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
                wchar_t text[32];
                swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", local.wYear, local.wMonth, local.wDay, local.wHour,
                           local.wMinute, local.wSecond);
                return std::wstring(text);
            };
            using std::chrono::hours, std::chrono::minutes;
            const auto session = [&](fs::file_time_type::duration startAge, fs::file_time_type::duration endAge) {
                const fs::path p = crashLogs / (L"session-" + stamp(now - startAge) + L".log");
                write_file(p.wstring(), "log");
                fs::last_write_time(p, now - endAge);
            };
            session(hours(5), hours(4));
            session(hours(3), hours(2));
            session(minutes(50), minutes(40));
            const std::string gameLog =
                "LogInit: Computer: GAMING-PC\r\nLogInit: User: al\r\nFile [C:/Users/al/AppData/Local/AC2/Saved]\r\n"
                "LogKsGamePlatform: UGamePlatformServices::Init() loaded local account: S76561198000000001 Alice Example "
                "ally#0\r\n"
                "LogOnline: Warning: STEAM: Failed to obtain steam user stats, user: Ally Cat [0x21873D004C0] has no "
                "stats entries\r\n"
                "Check ok with S76561198000000001, Alice Example on GAMING-PC; build 97656119000000001234; also al stays\r\n";
            // The dump holds the context's ids as Unreal copies them: ASCII in another case, and UTF-16.
            const auto utf16 = [](const std::string& s) {
                std::string out;
                for (char ch : s)
                    out += std::string(1, ch) + '\0';
                return out;
            };
            const std::string dump = std::string("MDMP\0\0", 6) + "head EBB98300AA11 mid " + utf16("3b18b8ffee22") + " tail";
            const auto crash = [&](const wchar_t* name, fs::file_time_type::duration age) {
                const fs::path d = crashes / name;
                fs::create_directories(d);
                write_file((d / L"UE4Minidump.dmp").wstring(), dump);
                write_file((d / kCrashContextName).wstring(),
                           "<LoginId>ebb98300aa11</LoginId><MachineId>EBB98300AA11</MachineId><ErrorMessage>boom"
                           "</ErrorMessage><EpicAccountId>3b18b8ffee22</EpicAccountId><UserName></UserName>");
                write_file((d / L"AC2.log").wstring(), gameLog);
                write_file((d / L"CrashReportClient.ini").wstring(), "[x]");
                for (const fs::directory_entry& f : fs::directory_iterator(d))
                    fs::last_write_time(f.path(), now - age);
            };
            crash(L"UE4CC-A", hours(4) - minutes(1));   // just after session 1's last line: in, but the oldest
            crash(L"UE4CC-B", hours(2) + minutes(30));  // during session 2
            crash(L"UE4CC-C", minutes(39));             // just after session 3's last line
            crash(L"UE4CC-D", minutes(20));             // 20 minutes after session 3: the game without Sim-QVFR
            crash(L"UE4CC-E", hours(3) + minutes(30));  // between sessions 1 and 2
            const std::vector<ReportFile> got =
                report_files({crashLogs.wstring(), reportRuntime, settingsIni, crashes.wstring()}, nowTicks);
            std::vector<std::wstring> chosen;  // the crashes' folders in the zip (by their time), in zip order
            int crashEntries = 0;
            const ReportFile* context = nullptr;
            const ReportFile* gameLogEntry = nullptr;
            const ReportFile* dumpEntry = nullptr;
            for (const ReportFile& f : got) {
                if (!f.name.starts_with(L"game-crash-"))
                    continue;
                ++crashEntries;
                if (f.name.ends_with(L"/UE4Minidump.dmp")) {
                    chosen.push_back(f.name.substr(0, f.name.find(L'/')));
                    dumpEntry = dumpEntry ? dumpEntry : &f;
                }
                if (f.name.ends_with(kCrashContextName))
                    context = &f;
                if (f.name.ends_with(L"/AC2.log"))
                    gameLogEntry = &f;
            }
            const std::vector<std::wstring> wantCrashes = {L"game-crash-" + stamp(now - minutes(39)),
                                                           L"game-crash-" + stamp(now - hours(2) - minutes(30))};
            check(chosen == wantCrashes && crashEntries == 6,
                  "report: the game's two newest crashes during zipped sessions (dump, context, log each), named by "
                  "their local time; none between or after sessions, no crash reporter ini");
            const auto has = [](const ReportFile* f, const std::string& text) {
                return f && f->data.find(text) != std::string::npos;
            };
            check(has(context, "<LoginId>redacted</LoginId>") && has(context, "<MachineId>redacted</MachineId>") &&
                      has(context, "<EpicAccountId>redacted</EpicAccountId>") && has(context, "<UserName></UserName>") &&
                      has(context, "<ErrorMessage>boom</ErrorMessage>") && !has(context, "ebb983") &&
                      !has(context, "EBB983") && !has(context, "3b18"),
                  "report: CrashContext without the login, machine and Epic account ids; the error kept");
            check(dumpEntry && dumpEntry->path.empty() && dumpEntry->data.size() == dump.size() &&
                      dumpEntry->data.starts_with(std::string("MDMP\0\0", 6)) && dumpEntry->data.ends_with(" tail") &&
                      has(dumpEntry, "head 000000000000 mid ") && !has(dumpEntry, "EBB98300AA11") &&
                      !has(dumpEntry, utf16("3b18b8ffee22")) && has(dumpEntry, utf16("000000000000")),
                  "report: the dump with the context's ids zeroed, ASCII in any case and UTF-16, its size kept");
            check(crash_context_ids("<LoginId></LoginId><MachineId>short</MachineId>").empty(),
                  "report: no ids from empty or short values");
            check(gameLogEntry && !has(gameLogEntry, "GAMING-PC") && !has(gameLogEntry, "Users/al/") &&
                      has(gameLogEntry, "C:/Users/<user>/AppData") && has(gameLogEntry, "LogInit: User: <user>") &&
                      !has(gameLogEntry, "Alice Example") && !has(gameLogEntry, "ally#0") &&
                      !has(gameLogEntry, "Ally Cat [") && has(gameLogEntry, "user: <steam name> [0x21873D004C0]") &&
                      !has(gameLogEntry, "76561198000000001") && has(gameLogEntry, "with S<steam id>, <account> on <computer>") &&
                      has(gameLogEntry, "build 97656119000000001234") && has(gameLogEntry, "also al stays"),
                  "report: the game's log without the computer, user, account and Steam names or SteamID64s, "
                  "wherever they appear; longer numbers and a short user name outside paths kept");
            check(redact_game_log("no names here\r\n") == "no names here\r\n", "report: a log with no names is unchanged");
        }

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
        report.zipName = L"sim-qvfr-report-20261103-120000.zip";
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
        session.log = L"C:\\Users\\someone\\Sim-QVFR\\logs\\session-20261103-120000.log";
        session.threads = {{L"GameThread", 71}, {L"RHIThread", 40}};
        const std::wstring details = issue_details(report);
        const auto has = [&](const wchar_t* text) { return details.find(text) != std::wstring::npos; };
        check(has(L"RTX 5090") && has(L"581.57") && has(L"Pimax OpenXR") && has(L"64 GB") && has(L"2584×2160") &&
                  has(L"eye tracked") && has(L"89.9 fps") && has(L"GameThread 71%") && has(L"Limited by: GPU") &&
                  has(L"detected") && has(L"Sim-QVFR 1.2.3"),
              "report: the details list version, hardware, headset, settings and the last session");
        check(!has(L"someone") && has(L"Log: session-20261103-120000.log"),
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
        check(issue_title(report) == L"[Problem] Sim-QVFR 1.2.3: ", "title: Not sure and no description: Problem, the version");

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

        const std::string url = issue_url(kIssueRepo, kIssueTemplates[0], L"[Problem] Sim-QVFR 1.2.3: ", fields);
        check(url.starts_with("https://github.com/Williem3/Sim-QVFR/issues/new?template=bug.yml&title=%5BProblem%5D%20Sim-QVFR%201.2.3%3A%20&what=The%20focus%20area%20jumps%20%26%20flickers.&files=") &&
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
        check(count == 4 && all[0] == &ace::kGame && all[1] == &acc::kGame && all[2] == &xplane::kGame &&
                  all[3] == &lmu::kGame && simqvfr::games::game_by_id("ACC") == &acc::kGame &&
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
                      !lmu::kGame.features.liveResolution && !lmu::kGame.features.foveatedDlss &&
                      simqvfr::games::game_for_exe(L"le mans ultimate.exe") == &lmu::kGame &&
                      simqvfr::games::supported_build(lmu::kGame, lmu::kBuilds[0]) && lmuAntiCheatListed,
                  "games: Le Mans Ultimate's identity (own OpenXR, +XR, FOV crop and helmet, no live resolution flag yet, "
                  "anti-cheat listed)");
        }
        check(acc::kGame.openComposite && !ace::kGame.openComposite && std::wstring(acc::kGame.exeFolder) ==
                  L"AC2\\Binaries\\Win64" && std::wstring(ace::kGame.exeFolder).empty() &&
                  acc::kGame.features.foveatedDlss && acc::kGame.features.helmet &&
                  acc::kGame.features.liveResolution && acc::kGame.features.fovCrop && ace::kGame.features.foveatedDlss &&
                  ace::kGame.features.helmet && ace::kGame.features.liveResolution && ace::kGame.features.fovCrop,
              "games: ACC through OpenComposite, its exe in a subfolder, with every feature ACE has");
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

    // Hotkeys (input/hotkeys.h): key and wheel control text both ways, hats, the ini, clashes, the launcher's settings.
    {
        KeyCombo k;
        check(parse_key(L"Ctrl+Alt+P", k) && k == KeyCombo{'P', true, true, false} && key_text(k) == L"Ctrl+Alt+P",
              "hotkeys: Ctrl+Alt+P both ways");
        check(parse_key(L"shift+f5", k) && k == KeyCombo{VK_F5, false, false, true} && key_text(k) == L"Shift+F5",
              "hotkeys: names and modifiers in any case");
        check(parse_key(L"Ctrl+Num +", k) && k.vk == VK_ADD && k.ctrl && key_text(k) == L"Ctrl+Num +" &&
                  parse_key(L"Alt+Page Down", k) && k.vk == VK_NEXT && parse_key(L"Num 7", k) && k.vk == VK_NUMPAD7,
              "hotkeys: a key named with a plus, a space, the number pad");
        check(parse_key(key_text(KeyCombo{0xE2, false, true, false}), k) && k.vk == 0xE2,
              "hotkeys: a key without a name round-trips by its number");
        check(!parse_key(L"", k) && !parse_key(L"Ctrl+", k) && !parse_key(L"Ctrl+Alt", k) && !parse_key(L"Hyper+P", k) &&
                  !parse_key(L"F25", k),
              "hotkeys: not keys");
        check(key_text(KeyCombo{}).empty(), "hotkeys: no key, no text");

        PadControl pad;
        check(parse_pad(L"0EB7_0020:button13:Fanatec Wheel", pad) && pad.device == L"0EB7_0020" && pad.control == 12 &&
                  pad.name == L"Fanatec Wheel" && pad_value(pad) == L"0EB7_0020:button13:Fanatec Wheel" &&
                  control_text(pad.control) == L"Button 13",
              "hotkeys: a wheel button both ways, numbered from 1 as Windows shows it");
        check(parse_pad(L"046D_C24F:hat1left:G29", pad) && pad.control == kPadHatBase + kHatLeft && control_text(pad.control) == L"Hat left" &&
                  parse_pad(L"046D_C24F:hat2up:", pad) && pad.control == kPadHatBase + 4 + kHatUp &&
                  control_text(pad.control) == L"Hat 2 up" && pad_value(pad) == L"046D_C24F:hat2up:",
              "hotkeys: hat directions");
        check(!parse_pad(L"", pad) && !parse_pad(L"046D_C24F:button0:", pad) && !parse_pad(L"046D_C24F:button129:", pad) &&
                  !parse_pad(L":button1:", pad) && !parse_pad(L"046D_C24F:hat5up:", pad) && !parse_pad(L"046D_C24F:hat1in:", pad),
              "hotkeys: not wheel controls");
        check(pad_value(PadControl{L"1234_5678", 0, L"Box; v2\r\n"}) == L"1234_5678:button1:Box  v2  ",
              "hotkeys: a name keeps no ini comment or line break");
        check(hat_directions(0xFFFFFFFF) == 0 && hat_directions(0) == (1u << kHatUp) && hat_directions(9000) == (1u << kHatRight) &&
                  hat_directions(4500) == ((1u << kHatUp) | (1u << kHatRight)) && hat_directions(27000) == (1u << kHatLeft) &&
                  hat_directions(31500) == ((1u << kHatLeft) | (1u << kHatUp)),
              "hotkeys: a hat's angle, diagonals holding two directions");

        const std::wstring hotkeysIni = (folder / "hotkeys.ini").wstring();
        const Hotkeys defaults = default_hotkeys();
        check(read_hotkeys(hotkeysIni.c_str()) == defaults &&
                  key_text(defaults[static_cast<int>(HotkeyAction::Overlay)].key) == L"Ctrl+Alt+P" &&
                  key_text(defaults[static_cast<int>(HotkeyAction::FrameCapture)].key) == L"Ctrl+Alt+F" &&
                  !defaults[static_cast<int>(HotkeyAction::MenuToggle)].key.set(),
              "hotkeys: unset, the defaults (no open/close key)");
        Hotkeys changedKeys = defaults;
        changedKeys[static_cast<int>(HotkeyAction::Visor)].key = {};
        changedKeys[static_cast<int>(HotkeyAction::MenuToggle)].pad = {L"0EB7_0020", 4, L"Wheel"};
        changedKeys[static_cast<int>(HotkeyAction::MenuUp)].pad = {L"0EB7_0020", kPadHatBase + kHatUp, L"Wheel"};
        check(write_hotkeys(hotkeysIni.c_str(), changedKeys) && read_hotkeys(hotkeysIni.c_str()) == changedKeys,
              "hotkeys: written and read back, a cleared key staying cleared");
        changedKeys[static_cast<int>(HotkeyAction::Overlay)].key = changedKeys[static_cast<int>(HotkeyAction::CpuCapture)].key;
        changedKeys[static_cast<int>(HotkeyAction::MenuDown)].pad = changedKeys[static_cast<int>(HotkeyAction::MenuToggle)].pad;
        check(hotkey_clash(changedKeys, HotkeyAction::Overlay, false) == static_cast<int>(HotkeyAction::CpuCapture) &&
                  hotkey_clash(changedKeys, HotkeyAction::MenuToggle, true) == static_cast<int>(HotkeyAction::MenuDown) &&
                  hotkey_clash(changedKeys, HotkeyAction::Visor, false) == -1 && hotkey_clash(changedKeys, HotkeyAction::Visor, true) == -1,
              "hotkeys: clashes, but none between two unset ones");

        Settings withKeys;
        withKeys.hotkeys[static_cast<int>(HotkeyAction::Overlay)].key = {VK_F9, false, false, false};
        withKeys.frameCapture = true;
        check(save_settings(hotkeysIni, withKeys) &&
                  load_settings(hotkeysIni).hotkeys[static_cast<int>(HotkeyAction::Overlay)].key.vk == VK_F9 &&
                  load_settings(hotkeysIni).frameCapture,
              "hotkeys: saved and loaded with the launcher's settings (the live keys)");
    }

    // Profiles (profile/profiles.h; settings.h's settings_profile, share_profile, shared_profile).
    {
        const fs::path profileFolder = folder / "profiles";
        const std::vector<Profile> presets = builtin_profiles(true), fixedPresets = builtin_profiles(false);
        check(presets.size() == 3 && presets[0].name == L"Performance" && presets[2].name == L"Quality" &&
                  fixedPresets.size() == 3 && fixedPresets[1].name == L"Balanced" &&
                  preset_summary(presets[1]) == L"Outer 25% \u00b7 inner 160% \u00b7 focus 28 \u00d7 28%" &&
                  preset_summary(fixedPresets[1]) == L"Outer 25% \u00b7 inner 140% \u00b7 focus 40 \u00d7 35%",
              "profiles: three presets for each foveated mode, by the same names, each summed up");
        // Every preset fits the 8192-pixel limit, keeps the outer views at most 25% and the centre denser than native;
        // the fixed set's area is larger than the eye tracked one's, and less dense.
        bool fit = true, centred = true, fixedLarger = true;
        const auto value = [](const Profile& p, const wchar_t* key) {
            for (const ProfileKey& k : p.keys)
                if (k.key == key)
                    return _wtoi(k.value.c_str());
            return 0;
        };
        for (size_t i = 0; i < 3; ++i)
            for (const Profile* p : {&presets[i], &fixedPresets[i]}) {
                fit &= value(*p, L"periphery_scale") * 100 + value(*p, L"focus_density") * value(*p, L"focus_percent") < 100 * 100;
                centred &= value(*p, L"periphery_scale") <= 25 && value(*p, L"focus_density") > 100 &&
                           value(*p, L"eye_scale") == value(*p, L"periphery_scale");
                fixedLarger &= p == &presets[i] ||
                               (value(*p, L"focus_percent") > value(presets[i], L"focus_percent") &&
                                value(*p, L"focus_height_percent") > value(presets[i], L"focus_height_percent") &&
                                value(*p, L"focus_density") < value(presets[i], L"focus_density"));
            }
        check(fit, "profiles: each preset keeps periphery + density x focus under 1 (the 8192-pixel limit)");
        check(centred && value(presets[2], L"focus_density") == 180, "profiles: presets weigh the centre: outer at most 25%, centre up to 180%");
        check(fixedLarger, "profiles: fixed presets: a larger focus area, less dense, than eye tracked ones");

        check(valid_profile_name(L"Wet night 90 Hz") && !valid_profile_name(L"") && !valid_profile_name(L"a/b") &&
                  !valid_profile_name(L"balanced") && !valid_profile_name(L" padded") && !valid_profile_name(L"dot.") &&
                  !valid_profile_name(std::wstring(41, L'x')),
              "profiles: names a file can hold, none a preset's");
        check(profile_names(profileFolder.wstring()) == std::vector<std::wstring>{L"Performance", L"Balanced", L"Quality"},
              "profiles: without a folder, the presets");

        Settings mine;
        mine.outer = 42;
        mine.width = 33;
        mine.helmet.on = true;
        mine.osd.enabled = true;  // a panel's place: not in a profile
        mine.hotkeys[static_cast<int>(HotkeyAction::Overlay)].key = {VK_F9, false, false, false};
        const Profile saved = settings_profile(mine, L"Night");
        bool panels = false, hotkeysIn = false, launcherKeys = false, outerIn = false;
        for (const ProfileKey& k : saved.keys) {
            panels |= k.section == L"osd" || k.section == L"menu";
            hotkeysIn |= k.section == L"hotkeys";
            launcherKeys |= k.section == L"launcher" || k.key == L"cpu_profile";
            outerIn |= k.key == L"periphery_scale" && k.value == L"42";
        }
        check(!saved.keys.empty() && outerIn && !panels && !hotkeysIn && !launcherKeys,
              "profiles: a profile of the settings holds the picture's keys, not the panels, hotkeys or launcher's");
        check(save_profile(profileFolder.wstring(), saved) &&
                  profile_names(profileFolder.wstring()) == std::vector<std::wstring>{L"Performance", L"Balanced", L"Quality", L"Night"},
              "profiles: saved, listed after the presets");
        Profile loaded;
        check(load_profile(profileFolder.wstring(), L"Night", loaded, true) && loaded.keys == saved.keys && !loaded.builtIn,
              "profiles: loaded back as saved");
        Profile preset, fixedPreset;
        check(load_profile(profileFolder.wstring(), L"balanced", preset, true) && preset.builtIn && preset.keys == presets[1].keys &&
                  load_profile(profileFolder.wstring(), L"Balanced", fixedPreset, false) && fixedPreset.keys == fixedPresets[1].keys,
              "profiles: a preset by name, any case, the one for the foveated mode");
        Profile badName = saved;
        badName.name = L"Quality";
        check(!save_profile(profileFolder.wstring(), badName), "profiles: no saved profile takes a preset's name");

        const std::wstring profileIni = (folder / "profile-settings.ini").wstring();
        Settings other;
        other.outer = 70;
        save_settings(profileIni, other);
        check(!profile_matches(profileIni.c_str(), saved) && apply_profile(profileIni.c_str(), saved) &&
                  profile_matches(profileIni.c_str(), saved) && load_settings(profileIni).outer == 42 &&
                  load_settings(profileIni).helmet.on,
              "profiles: used, the ini matches it and the launcher reads its settings");
        check(apply_profile(profileIni.c_str(), presets[0]) && load_settings(profileIni).outer == 20 &&
                  load_settings(profileIni).inner == 140 &&
                  load_settings(profileIni).helmet.on && !profile_matches(profileIni.c_str(), saved) &&
                  profile_matches(profileIni.c_str(), presets[0]),
              "profiles: a preset changes the resolution only");
        // The headset menu changes periphery_scale and leaves eye_scale (QVFR's fallback alone): not a change in QVFR.
        apply_profile(profileIni.c_str(), saved);
        WritePrivateProfileStringW(L"experiment", L"eye_scale", L"10", profileIni.c_str());
        const bool qvfrMatch = profile_matches(profileIni.c_str(), saved);
        WritePrivateProfileStringW(L"experiment", L"mode", L"quad", profileIni.c_str());
        check(qvfrMatch && !profile_matches(profileIni.c_str(), saved),
              "profiles: eye_scale doesn't count in QVFR; in Two-loop (its resolution) it does");
        check(set_current_profile(profileIni.c_str(), L"Night") && current_profile(profileIni.c_str()) == L"Night",
              "profiles: the current one named");

        const std::string sharedText = share_profile(saved);
        Profile roundTrip;
        check(sharedText.rfind("QVFR1:", 0) == 0 && sharedText.size() < export_profile(saved).size() &&
                  shared_profile(sharedText, roundTrip) && roundTrip.name == L"Night" && roundTrip.keys == saved.keys,
              "profiles: shared as only what differs, and back whole");
        check(shared_profile("  " + sharedText + "\r\n", roundTrip), "profiles: pasted with spaces around");
        Profile sneaky;
        sneaky.name = L"Sneaky";
        sneaky.keys = {{L"cheeky", L"runtime", L"C:\\evil.dll"}, {L"launcher", L"game_dir", L"C:\\x"},
                       {L"experiment", L"mode", L"double_render"}, {L"experiment", L"periphery_scale", L"20"}};
        Profile safe;
        bool leaked = false, outer20 = false, modeKept = false;
        check(shared_profile(export_profile(sneaky), safe), "profiles: a shared text with other keys still reads");
        for (const ProfileKey& k : safe.keys) {
            leaked |= k.key == L"runtime" || k.key == L"game_dir" || k.value == L"double_render";
            outer20 |= k.key == L"periphery_scale" && k.value == L"20";
            modeKept |= k.key == L"mode" && k.value == L"views4";
        }
        check(!leaked && outer20 && modeKept, "profiles: a shared text sets only a profile's keys, and the method only to a method");
        Profile legacy, fromLegacy;
        legacy.name = L"Legacy";
        legacy.keys = {{L"experiment", L"mode", L"quad"}};
        bool legacyKept = true;
        check(shared_profile(export_profile(legacy), fromLegacy), "profiles: a Two-loop shared text still reads");
        for (const ProfileKey& k : fromLegacy.keys)
            legacyKept &= !(k.key == L"mode" && k.value != L"views4");
        check(legacyKept, "profiles: a shared text can't switch to Two-loop (legacy)");
        Profile junk;
        check(!shared_profile("hello", junk) && !shared_profile("QVFR1:!!!", junk) && !import_profile("QVFR1:", junk) &&
                  !import_profile(export_profile(Profile{L"", {{L"quad", L"gaze", L"1"}}}), junk) &&
                  !import_profile(export_profile(Profile{L"Empty", {}}), junk),
              "profiles: not shared profiles");
        check(delete_profile(profileFolder.wstring(), L"Night") &&
                  profile_names(profileFolder.wstring()).size() == 3 && !delete_profile(profileFolder.wstring(), L"Balanced"),
              "profiles: deleted; a preset can't be");
    }

    // The update check (update_check.h): versions compared by their numbers, the newest release in GitHub's list.
    {
        check(compare_versions(L"v2026.11.3.3-alpha", L"2026.11.3.3-alpha (1a2b3c4)") == 0 &&
                  compare_versions(L"v2026.11.4.1-alpha", L"2026.11.3.3-alpha (1a2b3c4)") > 0 &&
                  compare_versions(L"v2026.11.3.10-alpha", L"v2026.11.3.9-alpha") > 0 &&
                  compare_versions(L"v2026.11.3", L"2026.11.3.1") < 0 && compare_versions(L"v2027.1.1.1", L"dev") == 0,
              "update: versions by their numbers, a suffix or commit ignored, 10 after 9");
        check(known_version(L"2026.11.3.3-alpha (abc)") && !known_version(L"dev") && !known_version(L""),
              "update: a development build has no version to compare");
        const auto zip = [](const char* name) {
            return std::string(R"("assets": [{"name": ")") + name +
                   R"(", "size": 3, "digest": "sha256:00", "browser_download_url": "u"}])";
        };
        const std::string releases = std::string("[") +
            R"({"tag_name": "v1.1.0-sim-qvfr-beta", "html_url": "draft", "draft": true, )" + zip("sim-qvfr-1.1.0.zip") + "}," +
            R"({"tag_name": "v1.0.0-sim-qvfr-beta", "html_url": "a", "draft": false, "prerelease": true, )" +
            zip("sim-qvfr-1.0.0.zip") + "}," +
            R"({"tag_name": "v1.0.1-sim-qvfr-beta", "html_url": "https://github.com/Williem3/Sim-QVFR/releases/tag/v1.0.1-sim-qvfr-beta",
                "draft": false, "prerelease": true, )" + zip("sim-qvfr-1.0.1.zip") + "}," +
            R"({"tag_name": "v2026.11.4.1-alpha", "html_url": "old", "draft": false, "prerelease": true, )" +
            zip("ace-quadview-2026.11.4.1-alpha.zip") + "}," +
            R"({"tag_name": "v1.2.0", "html_url": "notes only", "draft": false})" + "]";
        Release release;
        check(newest_release(releases, release) && release.tag == L"v1.0.1-sim-qvfr-beta" &&
                  release.url == L"https://github.com/Williem3/Sim-QVFR/releases/tag/v1.0.1-sim-qvfr-beta",
              "update: the highest Sim-QVFR release that isn't a draft, pre-releases counted; the old ace-quadview "
              "alphas and releases without a package ignored");
        check(!newest_release("[]", release) && !newest_release("{\"message\": \"rate limited\"}", release) &&
                  !newest_release("not json", release) && !newest_release(R"([{"tag_name": "v1", "draft": true}])", release),
              "update: no release in an empty list, an error answer, garbage or drafts only");
    }

    // Install (install.h): the hash lists, what to replace and keep, the downloads trusted, a staged release
    // installed into a folder (the user's settings and changed files kept, a backup), and put back on a failure.
    {
        const std::string hexA(64, 'a'), hexB(64, 'b'), hexC(64, 'c');
        const auto list = parse_sha256_list(std::string(64, 'A') + "  layer/sim-qvfr.dll\r\n" + hexB + "  README.txt\n" +
                                            hexC + "  ../evil.dll\n" + hexC + "  C:/evil.dll\n" + "nothex  x\n" + hexC +
                                            "  /root.dll\n");
        check(list.size() == 2 && list.at(L"layer\\sim-qvfr.dll") == hexA && list.at(L"README.txt") == hexB,
              "install: SHA256.txt read, lowercase, backslashes; no path out of the folder");

        const std::map<std::wstring, std::string> release = {
            {L"sim-qvfr-launcher.exe", hexA}, {L"sim-qvfr.ini", hexA}, {L"layer\\helmet\\top.png", hexB},
            {L"layer\\helmet\\visor.png", hexB},     {L"README.txt", hexC},       {L"NEW.txt", hexA}};
        const std::map<std::wstring, std::string> oldRelease = {
            {L"sim-qvfr-launcher.exe", hexC}, {L"layer\\helmet\\top.png", hexC}, {L"layer\\helmet\\visor.png", hexC}};
        const std::map<std::wstring, std::string> installed = {
            {L"sim-qvfr-launcher.exe", hexC}, {L"sim-qvfr.ini", hexB},  // the user's settings
            {L"layer\\helmet\\top.png", hexC},                                   // as the old release had it
            {L"layer\\helmet\\visor.png", hexA},                                 // the user's own image
            {L"README.txt", hexC}};                                            // already the new one
        const auto steps = plan_install(release, oldRelease, [&](const std::wstring& p) {
            const auto it = installed.find(p);
            return it == installed.end() ? std::string() : it->second;
        });
        const auto action = [&](const wchar_t* p) {
            for (const InstallStep& s : steps)
                if (s.path == p)
                    return s.action;
            return InstallAction::Same;
        };
        check(action(L"sim-qvfr-launcher.exe") == InstallAction::Replace && action(L"sim-qvfr.ini") == InstallAction::KeepSettings &&
                  action(L"layer\\helmet\\top.png") == InstallAction::Replace && action(L"layer\\helmet\\visor.png") == InstallAction::KeepChanged &&
                  action(L"README.txt") == InstallAction::Same && action(L"NEW.txt") == InstallAction::Add,
              "install: replace the old release's files, keep the settings and the user's own, add new ones");
        const auto noList = plan_install(release, {}, [&](const std::wstring& p) {
            const auto it = installed.find(p);
            return it == installed.end() ? std::string() : it->second;
        });
        bool helmetKept = false, exeReplaced = false;
        for (const InstallStep& s : noList) {
            helmetKept |= s.path == L"layer\\helmet\\top.png" && s.action == InstallAction::KeepChanged;
            exeReplaced |= s.path == L"sim-qvfr-launcher.exe" && s.action == InstallAction::Replace;
        }
        check(helmetKept && exeReplaced, "install: without the old release's list, helmet images stay and program files go");

        check(trusted_download("https://github.com/Williem3/Sim-QVFR/releases/download/v2026.11.4.1-alpha/sim-qvfr-2026.11.4.1-alpha.zip") &&
                  !trusted_download("http://github.com/Williem3/Sim-QVFR/releases/download/v1/a.zip") &&
                  !trusted_download("https://github.com/someone/else/releases/download/v1/a.zip") &&
                  !trusted_download("https://github.com/Williem3/Sim-QVFR/releases/download/../../evil.zip") &&
                  !trusted_download("https://github.com/Williem3/Sim-QVFR/releases/download/"),
              "install: only the repository's release downloads, over HTTPS");

        const std::string withAsset = R"([{"tag_name": "v2026.11.4.1-alpha", "html_url": "https://github.com/x", "draft": false,
            "assets": [{"name": "notes.txt", "size": 5, "digest": "sha256:00"},
                       {"name": "sim-qvfr-2026.11.4.1-alpha.zip", "size": 12066130,
                        "digest": "sha256:6306eb9b219a7d78685f96fc82da6180c6bb53f88be44fea5b7400f1e482c11e",
                        "browser_download_url": "https://github.com/Williem3/Sim-QVFR/releases/download/v2026.11.4.1-alpha/sim-qvfr-2026.11.4.1-alpha.zip"}]}])";
        Release r;
        check(newest_release(withAsset, r) && r.zipSize == 12066130 &&
                  r.zipSha256 == "6306eb9b219a7d78685f96fc82da6180c6bb53f88be44fea5b7400f1e482c11e" && trusted_download(r.zipUrl),
              "install: the release's zip, its size and SHA-256 from GitHub's list");
        Release noDigest;
        check(!newest_release(R"([{"tag_name": "v1", "assets": [{"name": "sim-qvfr-1.zip", "size": 3, "browser_download_url": "u"}]}])",
                              noDigest),
              "install: a zip without a digest isn't offered to install");

        // A staged release over an installed one, on disk.
        const fs::path root = folder / "install", app = root / "app", staged = root / "staged";
        const auto put = [](const fs::path& p, const std::string& text) {
            fs::create_directories(p.parent_path());
            write_file(p.wstring(), text);
        };
        const auto hash_of = [](const fs::path& p) { return simqvfr::sha256_file(p.c_str()); };
        const auto list_of = [&](const fs::path& dir, std::vector<const char*> files) {
            std::string text;
            for (const char* f : files)
                text += hash_of(dir / f) + "  " + std::string(f) + "\r\n";
            write_file((dir / "SHA256.txt").wstring(), text);
        };
        put(app / "sim-qvfr-launcher.exe", "old launcher");
        put(app / "layer/sim-qvfr.dll", "old layer");
        put(app / "layer/helmet/top.png", "old top");
        put(app / "layer/helmet/visor.png", "old visor");
        put(app / "sim-qvfr.ini", "[shipped]");
        put(app / "VERSION.txt", "Sim-QVFR 1");
        list_of(app, {"sim-qvfr-launcher.exe", "layer/sim-qvfr.dll", "layer/helmet/top.png", "layer/helmet/visor.png",
                      "sim-qvfr.ini", "VERSION.txt"});
        put(app / "sim-qvfr.ini", "[mine]");          // the user's settings since
        put(app / "layer/helmet/visor.png", "my visor");    // and their own image
        put(app / "profiles/Night.ini", "[profile]");      // not in any release
        put(staged / "sim-qvfr-launcher.exe", "new launcher");
        put(staged / "layer/sim-qvfr.dll", "new layer");
        put(staged / "layer/helmet/top.png", "new top");
        put(staged / "layer/helmet/visor.png", "new visor");
        put(staged / "sim-qvfr.ini", "[shipped 2]");
        put(staged / "VERSION.txt", "Sim-QVFR 2");
        put(staged / "README.txt", "new readme");
        list_of(staged, {"sim-qvfr-launcher.exe", "layer/sim-qvfr.dll", "layer/helmet/top.png", "layer/helmet/visor.png",
                         "sim-qvfr.ini", "VERSION.txt", "README.txt"});
        check(packaged_install(app.wstring()) && !packaged_install(staged.parent_path().wstring()),
              "install: a packaged install has VERSION.txt and SHA256.txt");

        // A file in use (opened without sharing) can't be moved aside: nothing changes.
        std::string readBack;
        HANDLE locked = CreateFileW((app / "layer/sim-qvfr.dll").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        const InstallResult failed = install_staged(app.wstring(), staged.wstring(), L"1 (abc)", [](const InstallProgress&) {});
        CloseHandle(locked);
        check(!failed.ok && failed.error.find(L"sim-qvfr.dll") != std::wstring::npos &&
                  read_file((app / "sim-qvfr-launcher.exe").wstring(), readBack) && readBack == "old launcher" &&
                  read_file((app / "layer/sim-qvfr.dll").wstring(), readBack) && readBack == "old layer" &&
                  !fs::exists(app / "README.txt"),
              "install: a file in use stops it, and the files already replaced go back");

        int steps2 = 0;
        const InstallResult done = install_staged(app.wstring(), staged.wstring(), L"1 (abc)", [&](const InstallProgress&) { ++steps2; });
        std::string launcher, layer, visor, ini, top, backupLauncher, readme;
        read_file((app / "sim-qvfr-launcher.exe").wstring(), launcher);
        read_file((app / "layer/sim-qvfr.dll").wstring(), layer);
        read_file((app / "layer/helmet/visor.png").wstring(), visor);
        read_file((app / "layer/helmet/top.png").wstring(), top);
        read_file((app / "sim-qvfr.ini").wstring(), ini);
        read_file((app / "README.txt").wstring(), readme);
        read_file((app / "update/backup-1/sim-qvfr-launcher.exe").wstring(), backupLauncher);
        check(done.ok && launcher == "new launcher" && layer == "new layer" && top == "new top" && readme == "new readme" &&
                  steps2 > 0,
              "install: the new release's program files in place, new ones added");
        check(ini == "[mine]" && visor == "my visor" && fs::exists(app / "profiles/Night.ini") && done.kept == 2,
              "install: the user's settings, own helmet image and profiles kept");
        check(backupLauncher == "old launcher" && done.backup == (app / "update" / "backup-1").wstring() &&
                  parse_sha256_list((read_file((app / "SHA256.txt").wstring(), readBack), readBack)).count(L"README.txt"),
              "install: the old files in update\\backup-<version>, the new SHA256.txt in place");

        fs::create_directories(app / "update/staged");
        write_file((app / "update/release.zip").wstring(), "zip");
        fs::create_directories(app / "update/backup-0");
        Sleep(20);
        fs::last_write_time(app / "update/backup-1", fs::file_time_type::clock::now());
        clean_update_folder(app.wstring());
        check(!fs::exists(app / "update/staged") && !fs::exists(app / "update/release.zip") && fs::exists(app / "update/backup-1") &&
                  !fs::exists(app / "update/backup-0"),
              "install: the next start clears update\\ but the newest backup");

        put(staged / "layer/sim-qvfr.dll", "tampered");
        const InstallResult tampered = install_staged(app.wstring(), staged.wstring(), L"2", [](const InstallProgress&) {});
        check(!tampered.ok && tampered.error.find(L"SHA256") != std::wstring::npos, "install: a file that doesn't match its list stops it");
    }

    fs::remove_all(folder, ec);
    per_game_settings_checks();
    game_tabs_checks();
    launch_status_checks();
    session_history_checks();
    explicit_layers_checks();
    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}

// ace-quadview-launcher: starts the chosen game (src/games) in VR with the ace-quadview layer for this launch
// only, nearest the game and above every implicit layer it can move (launch_plan.h). Settings come
// from ace-quadview.ini next to the launcher; [launcher] holds the launcher's own:
//   game=       the game to launch: ace (default) or acc (games/<id>/game.h)
//   game_dir_<id>= that game's folder in Steam's libraries (default: found through Steam; ACE also reads the older
//               game_dir)
//   off_layers= comma-separated OpenXR layer names to turn off for ACE (e.g. XR_APILAYER_MBUCCHIA_toolkit)
//   on_layers=  layers switched off in Windows to load for ACE all the same
//   layer_order= the layers' order below the ACE layer (the OpenXR layers pane; layer_order.h)
// Usage: ace-quadview-launcher               the settings window (window.cpp)
//        ace-quadview-launcher --launch      launch now, report on the console it was started from
//        ace-quadview-launcher --dry-run     write the run folder and the plan, start nothing
//        ace-quadview-launcher --apply-system-layers <request>   (elevated, from the window) writes
//                                            HKLM's part of Windows' layer list (layer_order.h)
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "../cheeky/cheeky_settings.h"
#include "../games/game.h"
#include "launch_plan.h"
#include "launcher.h"
#include "launcher_version.h"
#include "preflight.h"
#include "settings.h"

using namespace aceqv::launcher;
namespace fs = std::filesystem;
namespace games = aceqv::games;

namespace {

std::wstring report;           // everything said, also saved as launch.txt in the run folder
HANDLE cliOut = nullptr;       // command-line use: the console or pipe it was started from

void say(const std::wstring& line) {
    report += line + L"\n";
    if (!cliOut)
        return;
    const std::wstring text = line + L"\r\n";
    DWORD written = 0;
    if (GetFileType(cliOut) == FILE_TYPE_CHAR) {
        WriteConsoleW(cliOut, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        const std::string utf8 = narrow(text);
        WriteFile(cliOut, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

std::wstring exe_folder() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path().wstring();
}

std::wstring ini_string(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[2048] = {};
    GetPrivateProfileStringW(section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)), ini.c_str());
    return buffer;
}

std::vector<Registration> implicit_registrations() {
    std::vector<Registration> out;
    for (const HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(hive, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ | KEY_WOW64_64KEY,
                          &key) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[2048];
            DWORD nameLength = static_cast<DWORD>(std::size(name)), type = 0, value = 1, size = sizeof(value);
            const LSTATUS s = RegEnumValueW(key, i, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
            if (s == ERROR_NO_MORE_ITEMS)
                break;
            if (s == ERROR_SUCCESS || s == ERROR_MORE_DATA)
                out.push_back({name, s == ERROR_SUCCESS && type == REG_DWORD && value == 0});
        }
        RegCloseKey(key);
    }
    return out;
}

bool process_running(const wchar_t* exe) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        found = _wcsicmp(entry.szExeFile, exe) == 0;
    CloseHandle(snapshot);
    return found;
}

std::wstring find_game_folder() {
    HKEY key = nullptr;
    wchar_t steam[MAX_PATH] = {};
    DWORD size = sizeof(steam);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    const bool ok = RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, reinterpret_cast<BYTE*>(steam), &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok)
        return {};
    std::vector<std::wstring> libraries = {fs::path(steam).make_preferred().wstring()};
    std::string vdf;
    if (read_file((fs::path(steam) / L"steamapps" / L"libraryfolders.vdf").wstring(), vdf))
        for (const std::wstring& library : steam_libraries(vdf))
            libraries.push_back(library);
    const games::GameInfo& game = games::launcher_game();
    for (const std::wstring& library : libraries) {
        const fs::path folder = fs::path(library) / L"steamapps" / L"common" / game.steamFolder;
        std::error_code ec;
        if (fs::exists(folder / game.exeFolder / game.exeName, ec))
            return folder.wstring();
    }
    return {};
}

// The chosen game's folder: [launcher] game_dir_<id> (ACE: also game_dir), else its folder in Steam's libraries.
std::wstring game_folder(const std::wstring& ini) {
    const games::GameInfo& game = games::launcher_game();
    std::wstring set = ini_string(ini, L"launcher", (L"game_dir_" + widen(game.id)).c_str());
    if (set.empty() && game.id == std::string_view("ace"))
        set = ini_string(ini, L"launcher", L"game_dir");
    return set.empty() ? find_game_folder() : set;
}

// The chosen game's exe under `folder`.
fs::path game_exe(const std::wstring& folder) {
    const games::GameInfo& game = games::launcher_game();
    return fs::path(folder) / game.exeFolder / game.exeName;
}

std::vector<std::wstring> current_environment() {
    std::vector<std::wstring> out;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; p && *p; p += wcslen(p) + 1)
        out.emplace_back(p);
    FreeEnvironmentStringsW(block);
    return out;
}

std::vector<std::wstring> variable_names(const std::vector<std::wstring>& environment) {
    std::vector<std::wstring> names;
    for (const std::wstring& e : environment)
        if (const size_t eq = e.find(L'=', 1); eq != std::wstring::npos)
            names.push_back(e.substr(0, eq));
    return names;
}

// The launch's layer options from the ini: the launcher's profile and what the method and Cheeky lock.
Options ini_layer_options(const std::wstring& ini, const std::vector<std::wstring>& environment) {
    const std::wstring mode = ini_string(ini, L"experiment", L"mode");
    const aceqv::CheekySettings cheeky = aceqv::read_cheeky_settings(ini.c_str());
    const bool dlss = games::launcher_game().features.foveatedDlss;  // Cheeky only in a game that has it
    return launch_options(read_launcher_layers(ini),
                          _wcsicmp(mode.c_str(), L"quad") == 0 || _wcsicmp(mode.c_str(), L"two_loops") == 0,
                          dlss && cheeky.load, dlss && cheeky.fg, variable_names(environment));
}

std::wstring stamp_now() {
    wchar_t stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::wcsftime(stamp, std::size(stamp), L"%Y%m%d-%H%M%S", &local);
    return stamp;
}

// Writes one hive of `want`, backed up into logs first.
bool write_hive(bool machine, const std::vector<SystemLayer>& want, std::wstring& error) {
    const fs::path logs = fs::path(exe_folder()) / L"logs";
    std::error_code ec;
    fs::create_directories(logs, ec);
    const std::wstring backup =
        (logs / (L"openxr-layers-" + std::wstring(machine ? L"machine-" : L"user-") + stamp_now() + L".reg")).wstring();
    return write_system_hive(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kImplicitLayersKey, machine, want, backup,
                             error);
}

// --apply-system-layers: the elevated copy writes HKLM's part of the request (the window wrote HKCU's).
int apply_request(const std::wstring& path) {
    std::string text;
    std::vector<SystemLayer> want;
    if (!read_file(path, text) || !parse_system_request(text, want))
        return 2;
    std::wstring error;
    if (write_hive(true, want, error))
        return 0;
    write_file(path + L".error", narrow(error));
    return 1;
}

int fail(const std::wstring& why) {
    say(L"ERROR: " + why);
    return 1;
}

int run(bool dryRun) {
    const fs::path here = exe_folder();
    const fs::path layerFolder = layer_folder();
    std::error_code ec;
    const fs::path library = layerFolder / L"ace-quadview.dll", manifest = layerFolder / L"XR_APILAYER_ACE_quadview.json";
    if (!fs::exists(library, ec) || !fs::exists(manifest, ec))
        return fail(L"ace-quadview.dll and XR_APILAYER_ACE_quadview.json not found next to the launcher or in its ace folder");
    const fs::path ini = here / L"ace-quadview.ini";
    if (!fs::exists(ini, ec))
        return fail(L"ace-quadview.ini not found next to the launcher (" + ini.wstring() + L")");

    const std::wstring game = game_folder(ini.wstring());
    const games::GameInfo& info = games::launcher_game();
    const fs::path exe = game_exe(game);
    if (game.empty() || !fs::exists(exe, ec))
        return fail(std::wstring(info.name) + L" not found through Steam; set game_dir_" + widen(info.id) +
                    L" in [launcher] of ace-quadview.ini");

    const std::wstring mode = ini_string(ini.wstring(), L"experiment", L"mode");
    const std::vector<std::wstring> environment = current_environment();
    const LayerPlan plan = plan_layers(implicit_registrations(), ini_layer_options(ini.wstring(), environment));

    const std::wstring stamp = stamp_now();
    const fs::path logs = here / L"logs", runFolder = logs / (std::wstring(L"run-") + stamp);
    const fs::path log = logs / (std::wstring(L"ace-") + stamp + L".log"), iniCopy = runFolder / L"ace-quadview.ini";
    std::wstring error;
    if (!write_run_folder(runFolder.wstring(), manifest.wstring(), library.wstring(), plan, error))
        return fail(error);
    if (!CopyFileW(ini.c_str(), iniCopy.c_str(), FALSE))
        return fail(L"cannot copy ace-quadview.ini into " + runFolder.wstring());
    // The Diagnostics pane's "Next launch without QVFR" ([launcher] baseline_next): baseline=1 in this launch's
    // copy only (ACE's own two views, measured); turned off once ACE has started, so the next launch has QVFR.
    const bool baseline = ini_string(ini.wstring(), L"launcher", L"baseline_next") == L"1";
    if (baseline)
        WritePrivateProfileStringW(L"experiment", L"baseline", L"1", iniCopy.c_str());
    const auto variables =
        launch_variables(plan, runFolder.wstring(), iniCopy.wstring(), log.wstring(), runtime_info_path(), ini.wstring());

    say(L"ace-quadview launcher " + widen(ACE_QV_VERSION) + std::wstring(dryRun ? L" (dry run)" : L""));
    say(L"  game:      " + exe.wstring());
    say(L"  layer:     " + library.wstring());
    say(L"  settings:  " + ini.wstring() + L" (mode=" + (mode.empty() ? L"off" : mode) + L"), snapshot in the run folder");
    if (baseline)
        say(L"  baseline:  this launch without QVFR, measuring (the Diagnostics pane's Next launch without QVFR)");
    say(L"  run:       " + runFolder.wstring());
    say(L"  layer log: " + log.wstring());
    std::wstring order = std::wstring(info.shortName) + L" -> " + widen(kLayerName);
    for (const MovedLayer& m : plan.below)
        order += L" -> " + widen(m.name);
    say(L"  OpenXR:    " + order + L" -> runtime");
    for (const PlannedLayer& p : plan.layers)
        if (!p.on)
            say(L"  off:       " + widen(p.name) +
                (!p.lock.empty() ? L" (" + widen(p.lock) + L")" : p.systemOn ? L" (off_layers)" : L" (off in Windows)"));
        else if (!p.lock.empty() || !p.systemOn)
            say(L"  on:        " + widen(p.name) + L" (" + (p.lock.empty() ? L"on_layers" : widen(p.lock)) + L")");
    for (const std::string& note : plan.notes)
        say(L"  note:      " + widen(note));
    for (const auto& [name, value] : variables)
        say(L"  set " + name + L"=" + value);

    const bool gameRunning = process_running(info.exeName), steamRunning = process_running(L"steam.exe");
    if (dryRun) {
        if (gameRunning)
            say(L"  (" + std::wstring(info.shortName) + L" is running: a real launch would refuse)");
        if (!steamRunning)
            say(L"  (Steam is not running: a real launch would refuse)");
        write_file((runFolder / L"launch.txt").wstring(), narrow(report));
        return 0;
    }
    if (gameRunning)
        return fail(std::wstring(info.name) + L" is already running; close it first");
    if (!steamRunning)
        return fail(L"Steam is not running; start it first (" + std::wstring(info.shortName) + L" needs it)");

    std::wstring block = environment_block(environment, variables);
    std::wstring command = L"\"" + exe.wstring() + L"\" " + info.launchArgs;  // the game's OpenXR start
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    const std::wstring workingFolder = exe.parent_path().wstring();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, block.data(),
                        workingFolder.c_str(), &si, &pi))
        return fail(L"starting " + std::wstring(info.name) + L" failed (error " + std::to_wstring(GetLastError()) + L")");
    say(L"Started " + std::wstring(info.name) + L" (pid " + std::to_wstring(pi.dwProcessId) + L").");
    if (baseline)  // one launch only
        WritePrivateProfileStringW(L"launcher", L"baseline_next", L"0", ini.c_str());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    write_file((runFolder / L"launch.txt").wstring(), narrow(report));
    return 0;
}

}  // namespace

namespace aceqv::launcher {

std::wstring settings_path() { return (fs::path(exe_folder()) / L"ace-quadview.ini").wstring(); }

std::wstring runtime_info_path() { return (fs::path(exe_folder()) / L"runtime.ini").wstring(); }

std::wstring layer_folder() {
    // The layer: next to the launcher (development build) or in ace\ (release package).
    const fs::path here = exe_folder();
    std::error_code ec;
    return (fs::exists(here / L"ace-quadview.dll", ec) ? here : here / L"ace").wstring();
}

LayerPlan launcher_layer_plan() {
    return plan_layers(implicit_registrations(), ini_layer_options(settings_path(), current_environment()));
}

std::wstring apply_system_layers(HWND owner, const std::vector<SystemLayer>& want) {
    const std::vector<SystemLayer> current = read_system_layers();
    std::wstring error;
    if (!write_hive(false, want, error))
        return error;
    if (!system_needs_admin(current, want))
        return {};
    // HKLM: the same request, written by an elevated copy of this launcher (one UAC prompt).
    const fs::path logs = fs::path(exe_folder()) / L"logs";
    std::error_code ec;
    fs::create_directories(logs, ec);
    const std::wstring request = (logs / (L"openxr-layers-request-" + stamp_now() + L".txt")).wstring();
    if (!write_file(request, system_request(want)))
        return L"cannot write " + request;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring parameters = L"--apply-system-layers \"" + request + L"\"";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess)
        return GetLastError() == ERROR_CANCELLED ? L"administrator rights were declined; the machine-wide layers are unchanged"
                                                 : L"the elevated launcher did not start";
    WaitForSingleObject(info.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    std::string why;
    read_file(request + L".error", why);
    DeleteFileW(request.c_str());
    DeleteFileW((request + L".error").c_str());
    return code == 0 ? std::wstring() : why.empty() ? L"writing the machine-wide layers failed" : widen(why);
}

std::wstring launcher_game_exe() {
    const std::wstring game = game_folder(settings_path());
    const fs::path exe = game_exe(game);
    std::error_code ec;
    return !game.empty() && fs::exists(exe, ec) ? exe.wstring() : std::wstring();
}

void choose_launcher_game() {
    wchar_t id[32] = {};
    GetPrivateProfileStringW(L"launcher", L"game", L"ace", id, static_cast<DWORD>(std::size(id)), settings_path().c_str());
    if (const games::GameInfo* game = games::game_by_id(narrow(id).c_str()))
        games::set_launcher_game(*game);
}

PreflightInput launcher_preflight(const std::wstring& exe, BuildState build) {
    const std::wstring ini = settings_path();
    PreflightInput in;
    in.gameExe = exe;
    in.build = build;
    const std::wstring mode = ini_string(ini, L"experiment", L"mode");
    in.mode = mode.empty() || _wcsicmp(mode.c_str(), L"off") == 0                              ? LaunchMode::Off
              : _wcsicmp(mode.c_str(), L"quad") == 0 || _wcsicmp(mode.c_str(), L"two_loops") == 0 ? LaunchMode::TwoLoop
                                                                                                 : LaunchMode::Qvfr;
    in.steamRunning = process_running(L"steam.exe");
    in.gameRunning = process_running(games::launcher_game().exeName);
    if (const wchar_t* openvr = games::launcher_game().openComposite) {
        const std::wstring folder = game_folder(ini);
        in.openComposite = folder.empty() ? OpenComposite::Unknown : open_composite((fs::path(folder) / openvr).wstring());
    }
    if (!exe.empty() && games::launcher_game().features.foveatedDlss)
        in.cheekyLoaders = cheeky_loaders(fs::path(exe).parent_path().wstring());
    const aceqv::CheekySettings cheeky = aceqv::read_cheeky_settings(ini.c_str());
    const bool dlss = games::launcher_game().features.foveatedDlss;
    in.cheekyLoad = dlss && cheeky.load;
    if (const wchar_t* dlssDll = games::launcher_game().dlssDll; in.cheekyLoad && dlssDll) {
        const std::wstring folder = game_folder(ini);
        if (!folder.empty()) {
            in.dlssPath = (fs::path(folder) / dlssDll).wstring();
            in.dlssVersion = file_version(in.dlssPath);
        }
    }
    in.cheekyFg = dlss && cheeky.fg && games::launcher_game().features.frameGeneration;
    in.cheekyRuntimeFound = GetFileAttributesW(cheeky_runtime_path(ini, layer_folder()).c_str()) != INVALID_FILE_ATTRIBUTES;
    in.cheekyLayer = cheeky_layer_state();
    in.layers = launcher_layer_plan();
    in.toolkitTurbo = exe.empty() ? -1 : toolkit_turbo(exe);
    in.eyeTracked = load_settings(ini).eyeTracking;
    in.eyeTracker = load_runtime_info(runtime_info_path()).eyeTracker;
    return in;
}

int launch_ace(bool dryRun, std::wstring& out) {
    report.clear();
    const int result = run(dryRun);
    out = report;
    return result;
}

}  // namespace aceqv::launcher

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool dryRun = false, launchNow = false;
    std::wstring request;
    for (int i = 1; i < argc; ++i) {
        dryRun |= _wcsicmp(argv[i], L"--dry-run") == 0;
        launchNow |= _wcsicmp(argv[i], L"--launch") == 0;
        if (_wcsicmp(argv[i], L"--apply-system-layers") == 0 && i + 1 < argc)
            request = argv[++i];
    }
    LocalFree(argv);
    if (!request.empty())
        return apply_request(request);
    choose_launcher_game();
    if (!dryRun && !launchNow)
        return run_window(instance, show);
    // Command line: report to the console (or pipe) the launcher was started from, if any.
    cliOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!cliOut || cliOut == INVALID_HANDLE_VALUE) {
        cliOut = nullptr;
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            cliOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (cliOut == INVALID_HANDLE_VALUE)
            cliOut = nullptr;
    }
    return run(dryRun);
}

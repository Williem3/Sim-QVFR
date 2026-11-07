// sim-qvfr-launcher: starts the chosen game (src/games) in VR with the Sim-QVFR layer for this launch
// only, nearest the game and above every implicit layer it can move (launch_plan.h). Settings come
// from sim-qvfr.ini next to the launcher; [launcher] holds the launcher's own:
//   game=       the game to launch: ace (default) or acc (games/<id>/game.h)
//   game_dir_<id>= that game's folder in Steam's libraries (default: found through Steam; ACE also reads the older
//               game_dir)
//   off_layers= comma-separated OpenXR layer names to turn off for ACE (e.g. XR_APILAYER_MBUCCHIA_toolkit)
//   on_layers=  layers switched off in Windows to load for ACE all the same
//   layer_order= the layers' order below the Sim-QVFR layer (the OpenXR layers pane; layer_order.h)
// Usage: sim-qvfr-launcher               the settings window (window.cpp)
//        sim-qvfr-launcher --launch      launch now, report on the console it was started from
//        sim-qvfr-launcher --dry-run     write the run folder and the plan, start nothing
//        sim-qvfr-launcher --apply-system-layers <request>   (elevated, from the window) writes
//                                            HKLM's part of Windows' layer list (layer_order.h)
//        sim-qvfr-launcher --hang-dump <pid> <log>   (from the layer's hang watch) stands by while the game
//                                            runs; if it hangs, logs its modules and writes its minidump
//                                            (probe/hang_dump.h)
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
#include "../probe/hang_dump.h"
#include "launch_plan.h"
#include "launcher.h"
#include "launcher_version.h"
#include "preflight.h"
#include "report.h"
#include "settings.h"

using namespace simqvfr::launcher;
namespace fs = std::filesystem;
namespace games = simqvfr::games;

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

// `game`'s folder in Steam's libraries; empty if Steam doesn't have it.
std::wstring find_game_folder(const games::GameInfo& game) {
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
    for (const std::wstring& library : libraries) {
        const fs::path folder = fs::path(library) / L"steamapps" / L"common" / game.steamFolder;
        std::error_code ec;
        if (fs::exists(folder / game.exeFolder / game.exeName, ec))
            return folder.wstring();
    }
    return {};
}

// A game's folder: [launcher] game_dir_<id> (ACE: also game_dir), else its folder in Steam's libraries.
std::wstring game_folder(const std::wstring& ini, const games::GameInfo& game = games::launcher_game()) {
    std::wstring set = ini_string(ini, L"launcher", (L"game_dir_" + widen(game.id)).c_str());
    if (set.empty() && game.id == std::string_view("ace"))
        set = ini_string(ini, L"launcher", L"game_dir");
    return set.empty() ? find_game_folder(game) : set;
}

// A game's exe under `folder` (the chosen game's by default).
fs::path game_exe(const std::wstring& folder, const games::GameInfo& game = games::launcher_game()) {
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
    const simqvfr::CheekySettings cheeky = simqvfr::read_cheeky_settings(ini.c_str());
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

// The game's stdout and stderr, into one file: the only handles it inherits (a handle list), so nothing else of
// the launcher's leaks into the game. Without the file the game starts as before, with no standard handles.
struct LoaderOutput {
    STARTUPINFOEXW startup{};
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<char> attributes;

    explicit LoaderOutput(const std::wstring& path) {
        startup.StartupInfo.cb = sizeof(startup);
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes.resize(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size) ||
            !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &file, sizeof(file), nullptr, nullptr)) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            attributes.clear();
            return;
        }
        startup.lpAttributeList = list;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nullptr;
        startup.StartupInfo.hStdOutput = file;
        startup.StartupInfo.hStdError = file;
    }
    BOOL inherits() const { return file != INVALID_HANDLE_VALUE; }
    ~LoaderOutput() {
        if (startup.lpAttributeList)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    LoaderOutput(const LoaderOutput&) = delete;
    LoaderOutput& operator=(const LoaderOutput&) = delete;
};

int fail(const std::wstring& why) {
    say(L"ERROR: " + why);
    return 1;
}

// Loads `library` into the suspended process (a remote thread running LoadLibraryW; kernel32 sits at the same address
// in every process of a boot). The thread is the process's first to run, so it also does its loader start-up.
bool load_into(HANDLE process, const std::wstring& library, std::wstring& error) {
    const SIZE_T bytes = (library.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        error = L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    bool ok = false;
    if (WriteProcessMemory(process, remote, library.c_str(), bytes, nullptr)) {
        const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
        if (HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr)) {
            const DWORD wait = WaitForSingleObject(thread, 30000);
            DWORD loaded = 0;
            GetExitCodeThread(thread, &loaded);  // the module's address, cut to 32 bits: only nonzero matters
            ok = wait == WAIT_OBJECT_0 && loaded != 0;
            if (!ok)
                error = wait == WAIT_OBJECT_0 ? L"LoadLibraryW failed in the game" : L"loading the layer timed out";
            CloseHandle(thread);
        } else {
            error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L")";
        }
    } else {
        error = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L")";
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

// `launched`: the started game, its process handle kept open for the caller (the window follows it).
int run(bool dryRun, LaunchedGame* launched = nullptr) {
    const fs::path here = exe_folder();
    const fs::path layerFolder = layer_folder();
    std::error_code ec;
    const fs::path library = layerFolder / L"sim-qvfr.dll", manifest = layerFolder / L"XR_APILAYER_SIM_qvfr.json";
    if (!fs::exists(library, ec) || !fs::exists(manifest, ec))
        return fail(L"sim-qvfr.dll and XR_APILAYER_SIM_qvfr.json not found next to the launcher or in its layer folder");
    const fs::path ini = here / L"sim-qvfr.ini";
    if (!fs::exists(ini, ec))
        return fail(L"sim-qvfr.ini not found next to the launcher (" + ini.wstring() + L")");

    const std::wstring game = game_folder(ini.wstring());
    const games::GameInfo& info = games::launcher_game();
    const fs::path exe = game_exe(game);
    if (game.empty() || !fs::exists(exe, ec))
        return fail(std::wstring(info.name) + L" not found through Steam; set game_dir_" + widen(info.id) +
                    L" in [launcher] of sim-qvfr.ini");

    const std::wstring mode = ini_string(ini.wstring(), L"experiment", L"mode");
    const std::vector<std::wstring> environment = current_environment();
    const LayerPlan plan = plan_layers(implicit_registrations(), ini_layer_options(ini.wstring(), environment));

    const std::wstring stamp = stamp_now();
    const fs::path logs = here / L"logs", runFolder = logs / (std::wstring(L"run-") + stamp);
    const fs::path log = logs / (std::wstring(L"session-") + stamp + L".log"), iniCopy = runFolder / L"sim-qvfr.ini";
    std::wstring error;
    if (!write_run_folder(runFolder.wstring(), manifest.wstring(), library.wstring(), plan, error))
        return fail(error);
    if (!CopyFileW(ini.c_str(), iniCopy.c_str(), FALSE))
        return fail(L"cannot copy sim-qvfr.ini into " + runFolder.wstring());
    // The Diagnostics pane's "Next launch without QVFR" ([launcher] baseline_next): baseline=1 in this launch's
    // copy only (ACE's own two views, measured); turned off once ACE has started, so the next launch has QVFR.
    const bool baseline = ini_string(ini.wstring(), L"launcher", L"baseline_next") == L"1";
    if (baseline)
        WritePrivateProfileStringW(L"experiment", L"baseline", L"1", iniCopy.c_str());
    auto variables =
        launch_variables(plan, runFolder.wstring(), iniCopy.wstring(), log.wstring(), runtime_info_path(), ini.wstring(),
                         info.openComposite != nullptr);  // OpenComposite's loader mangles a list of layers
    // The OpenXR loader prints why it refuses a runtime or a layer (errors on stderr, XR_LOADER_DEBUG's levels on
    // stdout), before any layer runs: a game it fails starts flat with no layer log at all. The game's
    // stdout and stderr go to the run folder (loader_output) for the report. A user's own level is kept.
    if (std::none_of(environment.begin(), environment.end(),
                     [](const std::wstring& e) { return _wcsnicmp(e.c_str(), L"XR_LOADER_DEBUG=", 16) == 0; }))
        variables.emplace_back(L"XR_LOADER_DEBUG", L"warn");

    say(L"Sim-QVFR launcher " + widen(SIM_QVFR_VERSION) + std::wstring(dryRun ? L" (dry run)" : L""));
    say(L"  game:      " + exe.wstring());
    say(L"  args:      " + std::wstring(info.launchArgs));
    say(L"  layer:     " + library.wstring());
    // An elevated game's OpenXR loader ignores XR_API_LAYER_PATH (the Launch checks' "elevated" check).
    say(std::wstring(L"  rights:    ") +
        (launcher_elevated() ? L"ADMINISTRATOR: the game inherits it and OpenXR won't find the layers (started by "
                             : L"normal (started by ") +
        (parent_process_name().empty() ? L"unknown" : parent_process_name()) + L")");
    say(L"  settings:  " + ini.wstring() + L" (mode=" + (mode.empty() ? L"off" : mode) + L"), snapshot in the run folder");
    if (baseline)
        say(L"  baseline:  this launch without QVFR, measuring (the Diagnostics pane's Next launch without QVFR)");
    say(L"  run:       " + runFolder.wstring());
    say(L"  layer log: " + log.wstring());
    std::wstring order = std::wstring(info.shortName) + L" -> " + widen(kLayerName);
    for (const MovedLayer& m : plan.below)
        order += L" -> " + widen(m.name);
    say(L"  OpenXR:    " + order + L" -> runtime" +
        (info.openComposite && !plan.below.empty() ? L" (the layers below Sim-QVFR chained by it: SIM_QVFR_CHAIN)" : L""));
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

    auto launchVariables = variables;
    if (info.earlyLoad)  // the layer sets the game up when loaded into it, before it runs (early_load.cpp)
        launchVariables.push_back({L"SIM_QVFR_EARLY", L"1"});
    std::wstring block = environment_block(environment, launchVariables);
    std::wstring command = L"\"" + exe.wstring() + L"\" " + info.launchArgs;  // the game's OpenXR start
    PROCESS_INFORMATION pi{};
    const std::wstring workingFolder = exe.parent_path().wstring();
    LoaderOutput output((runFolder / kLoaderOutputName).wstring());
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | (info.earlyLoad ? CREATE_SUSPENDED : 0);
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, output.inherits(), flags, block.data(),
                        workingFolder.c_str(), &output.startup.StartupInfo, &pi))
        return fail(L"starting " + std::wstring(info.name) + L" failed (error " + std::to_wstring(GetLastError()) + L")");
    if (info.earlyLoad) {
        std::wstring loadError;
        if (load_into(pi.hProcess, library.wstring(), loadError))
            say(L"  layer loaded into " + std::wstring(info.shortName) + L" before it runs");
        else
            say(L"  layer NOT loaded early (" + loadError + L"); it loads at OpenXR start as usual");
        ResumeThread(pi.hThread);
    }
    say(L"Started " + std::wstring(info.name) + L" (pid " + std::to_wstring(pi.dwProcessId) + L").");
    if (baseline)  // one launch only
        WritePrivateProfileStringW(L"launcher", L"baseline_next", L"0", ini.c_str());
    CloseHandle(pi.hThread);
    if (launched)
        *launched = {pi.hProcess, pi.dwProcessId, runFolder.wstring(), log.wstring()};
    else
        CloseHandle(pi.hProcess);
    write_file((runFolder / L"launch.txt").wstring(), narrow(report));
    return 0;
}

}  // namespace

namespace simqvfr::launcher {

std::wstring settings_path() { return (fs::path(exe_folder()) / L"sim-qvfr.ini").wstring(); }

std::wstring runtime_info_path() { return (fs::path(exe_folder()) / L"runtime.ini").wstring(); }

std::wstring layer_folder() {
    // The layer: next to the launcher (development build) or in layer\ (release package).
    const fs::path here = exe_folder();
    std::error_code ec;
    return (fs::exists(here / L"sim-qvfr.dll", ec) ? here : here / L"layer").wstring();
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

bool game_installed(const games::GameInfo& game) {
    const std::wstring folder = game_folder(settings_path(), game);
    std::error_code ec;
    return !folder.empty() && fs::exists(game_exe(folder, game), ec);
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
    in.gameHasLayer = in.gameRunning && !running_live_ini(runtime_info_path()).empty();
    in.elevated = launcher_elevated();
    if (in.elevated)
        in.parent = parent_process_name();
    if (const wchar_t* openvr = games::launcher_game().openComposite) {
        const std::wstring folder = game_folder(ini);
        in.openComposite = folder.empty() ? OpenComposite::Unknown : open_composite((fs::path(folder) / openvr).wstring());
    }
    if (!exe.empty() && games::launcher_game().features.foveatedDlss)
        in.cheekyLoaders = cheeky_loaders(fs::path(exe).parent_path().wstring());
    const simqvfr::CheekySettings cheeky = simqvfr::read_cheeky_settings(ini.c_str());
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

int launch_game(bool dryRun, std::wstring& out, LaunchedGame* launched) {
    report.clear();
    const int result = run(dryRun, launched);
    out = report;
    return result;
}

}  // namespace simqvfr::launcher

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int dumpResult = 0;  // the hang dump helper does nothing else (no settings, no window)
    if (argv && simqvfr::hang_dump_command(argc, argv, dumpResult)) {
        LocalFree(argv);
        return dumpResult;
    }
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

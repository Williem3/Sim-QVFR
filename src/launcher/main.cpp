// sim-qvfr-launcher: starts the chosen game (src/games) in VR with the Sim-QVFR layer for this launch
// only, nearest the game and above every implicit layer it can move (launch_plan.h). Settings come
// from sim-qvfr.ini next to the launcher; [launcher] holds the launcher's own:
//   game=       the game to launch: ace (default) or acc (games/<id>/game.h)
//   game_dir_<id>= that game's folder in Steam's libraries (default: found through Steam; ACE also reads the older
//               game_dir)
//   off_layers= comma-separated OpenXR layer names to turn off for ACE (e.g. XR_APILAYER_MBUCCHIA_toolkit)
//   on_layers=  layers switched off in Windows to load for ACE all the same
//   layer_order= the layers' order below the Sim-QVFR layer (the OpenXR layers pane; layer_order.h)
//   elevated_registration= 0: running as administrator, don't register the launch's layers in HKLM (explicit_layers.h)
// Usage: sim-qvfr-launcher               the settings window (window.cpp)
//        sim-qvfr-launcher --launch      launch now, report on the console it was started from
//        sim-qvfr-launcher --dry-run     write the run folder and the plan, start nothing
//        sim-qvfr-launcher --apply-system-layers <request>   (elevated, from the window) writes
//                                            HKLM's part of Windows' layer list (layer_order.h)
//        sim-qvfr-launcher --hang-dump <pid> <log>   (from the layer's hang watch) stands by while the game
//                                            runs; if it hangs, logs its modules and writes its minidump
//                                            (probe/hang_dump.h)
//        sim-qvfr-launcher --unregister-after <handle> <ledger>   (from an elevated launch) waits for the game,
//                                            then takes out the layers registered for it (explicit_layers.h)
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
#include "../engine/nvapi_gpus.h"
#include "../games/game.h"
#include "../probe/hang_dump.h"
#include "explicit_layers.h"
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

// The id of a running `exe`, 0 if none.
DWORD process_id(const wchar_t* exe) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD pid = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !pid; more = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, exe) == 0)
            pid = entry.th32ProcessID;
    CloseHandle(snapshot);
    return pid;
}

// The game the anti-cheat's bootstrapper (`bootstrapper`, started) goes on to start: waited for up to `seconds`, while
// the bootstrapper shows its splash. The game wasn't running before the launch (run checks), so the first one is it.
// Its handle may be refused (the anti-cheat guards it); then only its id.
PROCESS_INFORMATION started_by_bootstrapper(HANDLE bootstrapper, const wchar_t* exe, int seconds) {
    PROCESS_INFORMATION game{};
    for (int i = 0; i < seconds * 4; ++i) {
        if ((game.dwProcessId = process_id(exe)) != 0)
            break;
        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(bootstrapper, &code) && code != STILL_ACTIVE && code != 0)
            return {};  // the bootstrapper failed (EAC not installed, a launch it refused)
        Sleep(250);
    }
    if (game.dwProcessId)
        game.hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, game.dwProcessId);
    return game;
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
    const games::GameInfo& game = games::launcher_game();
    const bool dlss = game.features.foveatedDlss;  // Cheeky only in a game that has it
    // Through the game's anti-cheat ([launcher] anticheat=1): Cheeky's layer off (games/game.h antiCheatStart).
    const bool antiCheat = game.antiCheatStart && games::anticheat_chosen(ini.c_str());
    return launch_options(read_launcher_layers(ini),
                          _wcsicmp(mode.c_str(), L"quad") == 0 || _wcsicmp(mode.c_str(), L"two_loops") == 0,
                          dlss && cheeky.load, dlss && cheeky.fg, variable_names(environment), antiCheat);
}

std::wstring stamp_now() {
    wchar_t stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::wcsftime(stamp, std::size(stamp), L"%Y%m%d-%H%M%S", &local);
    return stamp;
}

// An elevated launch's registrations (explicit_layers.h): one ledger per launch in logs\openxr-registrations, and
// every value added or taken out, with its time, in logs\openxr-registrations.log.
fs::path registrations_folder() { return fs::path(exe_folder()) / L"logs" / L"openxr-registrations"; }

void log_registrations(const std::vector<std::wstring>& notes) {
    if (notes.empty())
        return;
    const fs::path path = fs::path(exe_folder()) / L"logs" / L"openxr-registrations.log";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    std::string text;
    for (const std::wstring& note : notes)
        text += narrow(stamp_now() + L"  " + note) + "\r\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

// Starts a copy of this launcher that waits for `game` to exit, then takes out what `ledger` lists
// (--unregister-after): the launcher's window may close first, and --launch returns at once.
bool start_unregister_helper(HANDLE game, const std::wstring& ledger) {
    HANDLE inherited = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), game, GetCurrentProcess(), &inherited, SYNCHRONIZE, TRUE, 0))
        return false;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool ok = false;
    if (InitializeProcThreadAttributeList(list, 1, 0, &size)) {
        // The game's handle is the only one it inherits.
        if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited, sizeof(inherited), nullptr, nullptr)) {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring command = L"\"" + std::wstring(exe) + L"\" --unregister-after " +
                                   std::to_wstring(reinterpret_cast<uintptr_t>(inherited)) + L" \"" + ledger + L"\"";
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.lpAttributeList = list;
            PROCESS_INFORMATION pi{};
            ok = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                                nullptr, nullptr, &startup.StartupInfo, &pi) != FALSE;
            if (ok) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        DeleteProcThreadAttributeList(list);
    }
    CloseHandle(inherited);
    return ok;
}

// --unregister-after <handle> <ledger>: waits for the game (its handle inherited), then takes out its registrations.
// Only a ledger in this launcher's own logs\openxr-registrations; a failed wait leaves them for the next start's sweep.
int unregister_after(const wchar_t* handle, const std::wstring& ledger) {
    const HANDLE game = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(handle, nullptr, 10)));
    const fs::path folder = fs::path(ledger).parent_path().lexically_normal();
    if (_wcsicmp(folder.c_str(), registrations_folder().lexically_normal().c_str()) != 0)
        return 2;
    const DWORD waited = WaitForSingleObject(game, INFINITE);
    CloseHandle(game);
    if (waited != WAIT_OBJECT_0)
        return 3;
    std::vector<std::wstring> notes;
    const bool ok = unregister_layers(HKEY_LOCAL_MACHINE, kExplicitLayersKey, ledger, notes);
    log_registrations(notes);
    return ok ? 0 : 1;
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

// `value` put in front of the semicolon list `name` already holds in `environment` (or alone): the player's own
// Vulkan layers and layer folders stay.
std::wstring list_first(const std::vector<std::wstring>& environment, const std::wstring& name, const std::wstring& value) {
    for (const std::wstring& entry : environment)
        if (entry.size() > name.size() && entry[name.size()] == L'=' && _wcsnicmp(entry.c_str(), name.c_str(), name.size()) == 0)
            return entry.size() > name.size() + 1 ? value + L";" + entry.substr(name.size() + 1) : value;
    return value;
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
    migrate_settings(ini.wstring());  // a launch from the command line too (the window's start did it already)

    const std::wstring game = game_folder(ini.wstring());
    const games::GameInfo& info = games::launcher_game();
    const fs::path exe = game_exe(game);
    if (game.empty() || !fs::exists(exe, ec))
        return fail(std::wstring(info.name) + L" not found through Steam; set game_dir_" + widen(info.id) +
                    L" in [launcher] of sim-qvfr.ini");
    // [launcher] anticheat=1 (the Games pane), for a game with an anti-cheat bootstrapper: the game starts protected,
    // through the bootstrapper, and the layer runs in it as usual (games/game.h antiCheatStart). The user's risk.
    const bool viaAntiCheat = info.antiCheatStart && games::anticheat_chosen(ini.c_str());
    const fs::path bootstrapper = viaAntiCheat ? exe.parent_path() / info.antiCheatStart : fs::path();
    if (viaAntiCheat && !fs::exists(bootstrapper, ec))
        return fail(std::wstring(info.antiCheatStart) + L" not found next to " + exe.wstring() +
                    L" (Run with anti-cheat is on): verify the game's files in Steam");

    const std::wstring mode = ini_string(ini.wstring(), L"experiment", L"mode");
    const std::vector<std::wstring> environment = current_environment();
    const LayerPlan plan = plan_layers(implicit_registrations(), ini_layer_options(ini.wstring(), environment));
    // Nothing of Cheeky's runs in a game its anti-cheat protects: its layer is off (ini_layer_options), so the launch
    // refuses where it can't be kept out (a registration without a disable variable, its loader in the game folder).
    if (viaAntiCheat) {
        if (std::find(plan.above.begin(), plan.above.end(), std::string(kCheekyLayer)) != plan.above.end())
            return fail(L"Cheeky's OpenXR layer is on in Windows and can't be turned off for one launch, and Run with "
                        L"anti-cheat is on: turn Cheeky's layer off in the OpenXR layers pane (Without the launcher)");
        const std::vector<std::wstring> loaders = cheeky_loaders(exe.parent_path().wstring());
        if (!loaders.empty())
            return fail(L"Cheeky's loader " + loaders.front() + L" is in the game folder, and Run with anti-cheat is on: "
                        L"move it out of the game folder");
    }

    const std::wstring stamp = stamp_now();
    const fs::path logs = here / L"logs", runFolder = logs / (std::wstring(L"run-") + stamp);
    const fs::path log = logs / (std::wstring(L"session-") + stamp + L".log"), iniCopy = runFolder / L"sim-qvfr.ini";
    std::wstring error;
    if (!write_run_folder(runFolder.wstring(), manifest.wstring(), library.wstring(), plan, error))
        return fail(error);
    if (!CopyFileW(ini.c_str(), iniCopy.c_str(), FALSE))
        return fail(L"cannot copy sim-qvfr.ini into " + runFolder.wstring());
    // Multi-view rendering on any GPU: NVIDIA's on an RTX 20 series or newer (engine/nvapi_gpus.h), Sim-QVFR's own
    // (instancing) on others (the layer checks the game's device). LMU's MVR gates (lmu_mvr) are NVIDIA's alone: off
    // in this launch's copy on any other GPU, whatever the ini says.
    const bool multiViewGpu = simqvfr::nvapi::two_views_gpu() != nullptr;
    if (!multiViewGpu)
        WritePrivateProfileStringW(L"experiment", L"lmu_mvr", L"0", iniCopy.c_str());
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
    // An elevated game's OpenXR loader ignores XR_API_LAYER_PATH and reads explicit layers from HKLM alone, so the
    // manifests XR_ENABLE_API_LAYERS names are registered there for this launch (explicit_layers.h), unless
    // [launcher] elevated_registration=0.
    const bool elevated = launcher_elevated();
    const bool registerLayers = elevated && ini_string(ini.wstring(), L"launcher", L"elevated_registration") != L"0";
    std::vector<std::wstring> named;
    for (const auto& [name, value] : variables)
        if (name == L"XR_ENABLE_API_LAYERS")
            for (const std::string& layer : split_layer_names(value))
                named.push_back((runFolder / (widen(layer) + L".json")).wstring());

    say(L"Sim-QVFR launcher " + widen(SIM_QVFR_VERSION) + std::wstring(dryRun ? L" (dry run)" : L""));
    say(L"  game:      " + exe.wstring());
    say(L"  args:      " + std::wstring(info.launchArgs));
    if (viaAntiCheat)
        say(L"  anti-cheat: ON, started through " + bootstrapper.filename().wstring() +
            L" ([launcher] anticheat=1): Sim-QVFR runs with the anti-cheat, at the user's risk");
    say(L"  layer:     " + library.wstring());
    const std::wstring parent = parent_process_name();
    say(std::wstring(L"  rights:    ") +
        (!elevated        ? L"normal"
         : registerLayers ? L"ADMINISTRATOR: the game inherits it, so its OpenXR loader reads layers from HKLM alone; "
                            L"this launch's are registered there until the game exits"
                          : L"ADMINISTRATOR: the game inherits it and OpenXR won't find the layers (elevated_registration=0)") +
        L" (started by " + (parent.empty() ? L"unknown" : parent) + L")");
    say(L"  settings:  " + ini.wstring() + L" (mode=" + (mode.empty() ? L"off" : mode) + L"), snapshot in the run folder");
    if (baseline)
        say(L"  baseline:  this launch without QVFR, measuring (the Diagnostics pane's Next launch without QVFR)");
    if (info.features.multiView && GetPrivateProfileIntW(L"experiment", L"multi_view", 1, iniCopy.c_str()) != 0)
        say(std::wstring(L"  multi-view: on, ") +
            (multiViewGpu && ini_string(iniCopy.wstring(), L"experiment", L"multi_view_software") != L"1"
                 ? L"NVIDIA's multi-view rendering"
                 : L"Sim-QVFR's own (instancing)") +
            L" (the game's device decides at launch)");
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
        if (registerLayers)
            for (const std::wstring& m : named)
                say(L"  (a real launch would register " + m + L" in HKLM\\" + kExplicitLayersKey + L" until the game exits)");
        write_file((runFolder / L"launch.txt").wstring(), narrow(report));
        return 0;
    }
    if (gameRunning)
        return fail(std::wstring(info.name) + L" is already running; close it first");
    if (!steamRunning)
        return fail(L"Steam is not running; start it first (" + std::wstring(info.shortName) + L" needs it)");

    auto launchVariables = variables;
    const bool early = info.earlyLoad && !viaAntiCheat;  // the bootstrapper starts the game: nothing to load into early
    if (early) {
        // The Vulkan loader loads the DLL as a layer at the game's first vkCreateInstance, before it makes any shader
        // module, and the DLL sets the game up then (vk_early_layer.cpp, early_load.cpp). For this launch only: no
        // registry. A Vulkan loader older than 1.3.234 skips VK_ADD_LAYER_PATH, and the layer then loads at OpenXR
        // start as usual (the X-Plane adapter logs which).
        launchVariables.push_back({L"SIM_QVFR_EARLY", L"1"});
        launchVariables.push_back({L"VK_ADD_LAYER_PATH", list_first(environment, L"VK_ADD_LAYER_PATH", (layerFolder / L"vulkan").wstring())});
        launchVariables.push_back(
            {L"VK_INSTANCE_LAYERS", list_first(environment, L"VK_INSTANCE_LAYERS", L"VK_LAYER_SIM_qvfr_early")});
    }
    std::wstring block = environment_block(environment, launchVariables);
    // The game's OpenXR start; through the anti-cheat, the bootstrapper passes the arguments on.
    const fs::path startExe = viaAntiCheat ? bootstrapper : exe;
    std::wstring command = L"\"" + startExe.wstring() + L"\" " + info.launchArgs;
    PROCESS_INFORMATION pi{};
    const std::wstring workingFolder = exe.parent_path().wstring();
    LoaderOutput output((runFolder / kLoaderOutputName).wstring());
    // Registered just before the start; a failed start takes them out again.
    const std::wstring ledger = (registrations_folder() / (L"run-" + stamp + L".txt")).wstring();
    bool registered = false;
    if (registerLayers) {
        std::vector<std::wstring> notes;
        std::wstring registryError;
        registered = register_layers(HKEY_LOCAL_MACHINE, kExplicitLayersKey, true, named, ledger,
                                     (runFolder / L"openxr-explicit-before.reg").wstring(), notes, registryError);
        for (const std::wstring& note : notes)
            say(L"  registry:  " + note);
        if (!registered)
            say(L"  registry:  NOT registered (" + registryError + L"): the game will likely start flat");
        log_registrations(notes);
    }
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    if (!CreateProcessW(startExe.c_str(), command.data(), nullptr, nullptr, output.inherits(), flags, block.data(),
                        workingFolder.c_str(), &output.startup.StartupInfo, &pi)) {
        const DWORD startError = GetLastError();
        if (registered) {
            std::vector<std::wstring> notes;
            unregister_layers(HKEY_LOCAL_MACHINE, kExplicitLayersKey, ledger, notes);
            for (const std::wstring& note : notes)
                say(L"  registry:  " + note);
            log_registrations(notes);
        }
        return fail(L"starting " + std::wstring(info.name) + L" failed (error " + std::to_wstring(startError) + L")");
    }
    if (viaAntiCheat) {
        // From here on the game itself, not the bootstrapper (which exits once the game runs).
        const PROCESS_INFORMATION protectedGame = started_by_bootstrapper(pi.hProcess, info.exeName, 90);
        if (!protectedGame.dwProcessId) {
            say(L"  anti-cheat: " + std::wstring(info.name) + L" did not start within 90 s (the bootstrapper's window says why)");
        } else {
            say(L"  anti-cheat: " + std::wstring(info.exeName) + L" started (pid " + std::to_wstring(protectedGame.dwProcessId) + L")" +
                (protectedGame.hProcess ? L"" : L"; its handle refused, so its exit won't be seen here"));
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            pi = {};
            pi.hProcess = protectedGame.hProcess;
            pi.dwProcessId = protectedGame.dwProcessId;
        }
    }
    if (registered && pi.hProcess) {
        ledger_game_started(ledger, process_stamp(pi.hProcess));
        say(start_unregister_helper(pi.hProcess, ledger)
                ? L"  registry:  taken out when the game exits (a copy of the launcher waits for it)"
                : L"  registry:  the copy that takes them out did not start; the next start as administrator does");
    }
    if (early)
        say(L"  early load: the layer loads with " + std::wstring(info.shortName) + L"'s Vulkan start (VK_LAYER_SIM_qvfr_early)");
    say(L"Started " + std::wstring(info.name) + L" (pid " + std::to_wstring(pi.dwProcessId) + L").");
    if (baseline)  // one launch only
        WritePrivateProfileStringW(L"launcher", L"baseline_next", L"0", ini.c_str());
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (launched)
        *launched = {pi.hProcess, pi.dwProcessId, runFolder.wstring(), log.wstring()};
    else if (pi.hProcess)
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
    in.elevatedRegistration = ini_string(ini, L"launcher", L"elevated_registration") != L"0";
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
    // Sim-QVFR's own DLSS: on for a QVFR launch of a game with it; its DLL where the layer will look.
    const simqvfr::DlssSettings own = simqvfr::read_dlss_settings(ini.c_str());
    in.ownDlss = games::launcher_game().features.sideLoadedDlss && own.on && in.mode == LaunchMode::Qvfr;
    if (in.ownDlss) {
        in.ownDlssMultiView = GetPrivateProfileIntW(L"experiment", L"multi_view", 1, ini.c_str()) != 0;
        in.ownDlssIncluded = own.folder.empty();
        in.ownDlssPath = dlss_dll_path(own.folder, layer_folder());
        in.ownDlssFound = !in.ownDlssPath.empty() && GetFileAttributesW(in.ownDlssPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (in.ownDlssFound)
            in.ownDlssVersion = file_version(in.ownDlssPath);
    }
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
    if (argv && argc == 4 && _wcsicmp(argv[1], L"--unregister-after") == 0) {  // nothing else either
        const std::wstring handle = argv[2], ledger = argv[3];
        LocalFree(argv);
        return unregister_after(handle.c_str(), ledger);
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
    // As administrator: an earlier elevated launch whose registrations were never taken out (the PC went off while
    // its game ran) is cleared from its ledger.
    if (launcher_elevated()) {
        std::vector<std::wstring> notes;
        sweep_ledgers(HKEY_LOCAL_MACHINE, kExplicitLayersKey, registrations_folder().wstring(), notes);
        log_registrations(notes);
    }
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

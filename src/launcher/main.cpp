// ace-quadview-launcher: starts Assetto Corsa EVO in VR with the ace-quadview layer for this launch
// only, nearest the game and above every implicit layer it can move (launch_plan.h). Settings come
// from ace-quadview.ini next to the launcher; [launcher] holds the launcher's own:
//   game_dir=   the folder holding AssettoCorsaEVO.exe (default: found through Steam)
//   off_layers= comma-separated OpenXR layer names to turn off for ACE (e.g. XR_APILAYER_MBUCCHIA_toolkit)
// Usage: ace-quadview-launcher               the settings window (window.cpp)
//        ace-quadview-launcher --launch      launch now, report on the console it was started from
//        ace-quadview-launcher --dry-run     write the run folder and the plan, start nothing
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "launch_plan.h"
#include "launcher.h"
#include "launcher_version.h"

using namespace aceqv::launcher;
namespace fs = std::filesystem;

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

std::vector<std::string> split_names(const std::wstring& list) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : narrow(list) + ",") {
        if (c == ',' || c == ';') {
            if (!current.empty())
                out.push_back(current);
            current.clear();
        } else if (c != ' ' && c != '\t') {
            current += c;
        }
    }
    return out;
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
    for (const std::wstring& library : libraries) {
        const fs::path folder = fs::path(library) / L"steamapps" / L"common" / L"Assetto Corsa EVO";
        std::error_code ec;
        if (fs::exists(folder / L"AssettoCorsaEVO.exe", ec))
            return folder.wstring();
    }
    return {};
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

    std::wstring game = ini_string(ini.wstring(), L"launcher", L"game_dir");
    if (game.empty())
        game = find_game_folder();
    const fs::path exe = fs::path(game) / L"AssettoCorsaEVO.exe";
    if (game.empty() || !fs::exists(exe, ec))
        return fail(L"Assetto Corsa EVO not found through Steam; set game_dir in [launcher] of ace-quadview.ini");

    const std::wstring mode = ini_string(ini.wstring(), L"experiment", L"mode");
    const std::vector<std::wstring> environment = current_environment();
    Options options;
    options.offLayers = split_names(ini_string(ini.wstring(), L"launcher", L"off_layers"));
    options.frameGenerationOff = _wcsicmp(mode.c_str(), L"quad") == 0 || _wcsicmp(mode.c_str(), L"two_loops") == 0;
    options.setVariables = variable_names(environment);
    const LayerPlan plan = plan_layers(implicit_registrations(), options);

    wchar_t stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::wcsftime(stamp, std::size(stamp), L"%Y%m%d-%H%M%S", &local);
    const fs::path logs = here / L"logs", runFolder = logs / (std::wstring(L"run-") + stamp);
    const fs::path log = logs / (std::wstring(L"ace-") + stamp + L".log"), iniCopy = runFolder / L"ace-quadview.ini";
    std::wstring error;
    if (!write_run_folder(runFolder.wstring(), manifest.wstring(), library.wstring(), plan, error))
        return fail(error);
    if (!CopyFileW(ini.c_str(), iniCopy.c_str(), FALSE))
        return fail(L"cannot copy ace-quadview.ini into " + runFolder.wstring());
    const auto variables =
        launch_variables(plan, runFolder.wstring(), iniCopy.wstring(), log.wstring(), runtime_info_path(), ini.wstring());

    say(L"ace-quadview launcher " + widen(ACE_QV_VERSION) + std::wstring(dryRun ? L" (dry run)" : L""));
    say(L"  game:      " + exe.wstring());
    say(L"  layer:     " + library.wstring());
    say(L"  settings:  " + ini.wstring() + L" (mode=" + (mode.empty() ? L"off" : mode) + L"), snapshot in the run folder");
    say(L"  run:       " + runFolder.wstring());
    say(L"  layer log: " + log.wstring());
    std::wstring order = L"ACE -> " + widen(kLayerName);
    for (const MovedLayer& m : plan.below)
        order += L" -> " + widen(m.name);
    say(L"  OpenXR:    " + order + L" -> runtime");
    for (const auto& [name, variable] : plan.off)
        say(L"  off:       " + widen(name) + (name == kFrameGenerationLayer && options.frameGenerationOff
                                                  ? L" (frame generation breaks mode=quad)"
                                                  : L" (off_layers)"));
    for (const std::string& note : plan.notes)
        say(L"  note:      " + widen(note));
    for (const auto& [name, value] : variables)
        say(L"  set " + name + L"=" + value);

    const bool aceRunning = process_running(L"AssettoCorsaEVO.exe"), steamRunning = process_running(L"steam.exe");
    if (dryRun) {
        if (aceRunning)
            say(L"  (ACE is running: a real launch would refuse)");
        if (!steamRunning)
            say(L"  (Steam is not running: a real launch would refuse)");
        write_file((runFolder / L"launch.txt").wstring(), narrow(report));
        return 0;
    }
    if (aceRunning)
        return fail(L"Assetto Corsa EVO is already running; close it first");
    if (!steamRunning)
        return fail(L"Steam is not running; start it first (ACE needs it)");

    std::wstring block = environment_block(environment, variables);
    std::wstring command = L"\"" + exe.wstring() + L"\" -vr";  // -vr: ACE's OpenXR start
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, block.data(),
                        game.c_str(), &si, &pi))
        return fail(L"starting Assetto Corsa EVO failed (error " + std::to_wstring(GetLastError()) + L")");
    say(L"Started Assetto Corsa EVO (pid " + std::to_wstring(pi.dwProcessId) + L").");
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
    for (int i = 1; i < argc; ++i) {
        dryRun |= _wcsicmp(argv[i], L"--dry-run") == 0;
        launchNow |= _wcsicmp(argv[i], L"--launch") == 0;
    }
    LocalFree(argv);
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

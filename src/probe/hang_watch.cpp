#include "probe/hang_watch.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>

#include "log.h"
#include "probe/hang_dump.h"
#include "xr.h"

#include <psapi.h>

namespace simqvfr {
namespace {

constexpr wchar_t kHelperName[] = L"sim-qvfr-launcher.exe";
constexpr DWORD kHelperWaitMs = 120000;

std::atomic<bool> armed{false}, started{false}, framed{false};
std::atomic<unsigned> limit{0};          // seconds without a frame
std::atomic<uint64_t> lastFrame{0};      // GetTickCount64 at xrCreateSession or the last xrEndFrame
// Resolved at the first arm, before the watch thread starts: finding them takes the loader lock
// (GetModuleHandleExW, GetModuleFileNameW), which a stuck game may hold.
std::wstring helper, helperOverride, logFile;
HANDLE hangEvent = nullptr;  // the helper's signal (hang_dump_event)

bool is_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The launcher: next to this module (a development build), or one folder up (the package keeps the layer
// in layer\ and the launcher at its root).
std::wstring find_helper() {
    if (!helperOverride.empty())
        return is_file(helperOverride) ? helperOverride : std::wstring();
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&find_helper), &self);
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(self, module, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(module).parent_path();
    for (const auto& candidate : {dir / kHelperName, dir.parent_path() / kHelperName})
        if (is_file(candidate))
            return candidate.wstring();
    return {};
}

// The helper stands by from the watch's start: started when the watch fires, CreateProcessW would wait for a
// thread that holds the loader lock (a DLL load in progress, or the hang itself). Null: no helper.
HANDLE start_helper() {
    if (helper.empty())
        return nullptr;
    wchar_t eventName[64];
    hang_dump_event(GetCurrentProcessId(), eventName);
    hangEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (!hangEvent) {
        logf("hang watch: cannot create %ls (error %lu); no minidump", eventName, GetLastError());
        return nullptr;
    }
    std::wstring command =
        L"\"" + helper + L"\" --hang-dump " + std::to_wstring(GetCurrentProcessId()) + L" \"" + logFile + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        logf("hang watch: cannot start %ls (error %lu); no minidump", helper.c_str(), GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    logf("hang watch: dump helper standing by (pid %lu)", pi.dwProcessId);
    return pi.hProcess;
}

// The helper writes the modules and the dump from its own process; this thread only signals and waits.
void dump(HANDLE process) {
    if (!process) {
        logf("hang watch: no dump helper; modules from here, no minidump");
        log_modules();
        return;
    }
    const ULONGLONG start = GetTickCount64();
    SetEvent(hangEvent);
    if (WaitForSingleObject(process, kHelperWaitMs) != WAIT_OBJECT_0) {
        logf("hang watch: the dump helper is still running after %lu s; not waiting for it", kHelperWaitMs / 1000);
        return;
    }
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    logf("hang watch: minidump %s (helper exit %lu, %.1f s)", hang_dump_result_text(code), code,
         (GetTickCount64() - start) / 1000.0);
}

DWORD WINAPI watch(void*) {
    const HANDLE process = start_helper();
    for (;;) {
        Sleep(200);
        const unsigned seconds = limit.load();
        if (!armed || GetTickCount64() - lastFrame.load() < seconds * 1000ull)
            continue;
        if (framed)
            logf("hang watch: no xrEndFrame for %u s mid-session; the game looks stuck", seconds);
        else
            logf("hang watch: no xrEndFrame %u s after xrCreateSession; the game looks stuck", seconds);
        dump(process);
        if (process)
            CloseHandle(process);
        return 0;  // once per process
    }
}

}  // namespace

void log_modules() {
    // psapi's Ex functions read the loader list through ReadProcessMemory, as for another process; plain
    // GetModuleFileNameW takes the loader lock and would wait out a DLL load in progress.
    const HANDLE self = GetCurrentProcess();
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!EnumProcessModulesEx(self, modules, sizeof(modules), &bytes, LIST_MODULES_ALL))
        return;
    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), 1024);
    logf("modules: %lu loaded", count);
    for (DWORD i = 0; i < count; ++i) {
        MODULEINFO info{};
        wchar_t path[MAX_PATH] = {};
        GetModuleInformation(self, modules[i], &info, sizeof(info));
        GetModuleFileNameExW(self, modules[i], path, MAX_PATH);
        logf("  module %016llx size %08lx %ls", reinterpret_cast<unsigned long long>(info.lpBaseOfDll),
             info.SizeOfImage, path);
    }
}

void hang_watch_set_helper(const wchar_t* path) { helperOverride = path ? path : L""; }

void hang_watch_arm(unsigned seconds) {
    if (!seconds)
        return;
    limit = seconds;
    framed = false;
    lastFrame = GetTickCount64();
    armed = true;
    if (!started.exchange(true)) {
        helper = find_helper();
        logFile = log_file();
        if (helper.empty())
            logf("hang watch: %u s; no %ls found, so a hang is logged without a minidump", seconds, kHelperName);
        else
            logf("hang watch: %u s; a hang is dumped by %ls", seconds, helper.c_str());
        if (HANDLE thread = CreateThread(nullptr, 0, watch, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
}

void hang_watch_frame() {
    lastFrame = GetTickCount64();
    framed = true;
}

void hang_watch_disarm() { armed = false; }

}  // namespace simqvfr

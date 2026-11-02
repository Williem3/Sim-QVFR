#include "probe/hang_watch.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>

#include "log.h"
#include "xr.h"

#include <dbghelp.h>
#include <psapi.h>

namespace aceqv {
namespace {

std::atomic<bool> armed{false}, fired{false};
std::atomic<bool> framed{false};

void write_dump() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"hang-%02u%02u%02u.dmp", t.wHour, t.wMinute, t.wSecond);
    const std::filesystem::path path = std::filesystem::path(log_directory()) / name;
    // dbghelp from System32 only, loaded when needed.
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto write = dbghelp ? reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                               : nullptr;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!write || file == INVALID_HANDLE_VALUE) {
        logf("hang watch: cannot write %ls", path.c_str());
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        return;
    }
    // Stacks, thread state and modules; no heap (small, and enough to see where each thread waits).
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                 MiniDumpWithHandleData);
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
    CloseHandle(file);
    logf("hang watch: minidump %ls %s", path.c_str(), ok ? "written" : "failed");
}

DWORD WINAPI watch(void* parameter) {
    const unsigned seconds = static_cast<unsigned>(reinterpret_cast<uintptr_t>(parameter));
    for (unsigned waited = 0; waited < seconds * 10; ++waited) {
        if (framed)
            return 0;
        Sleep(100);
    }
    logf("hang watch: no xrEndFrame %u s after xrCreateSession; ACE looks stuck", seconds);
    log_modules();
    write_dump();
    return 0;
}

}  // namespace

void log_modules() {
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), 1024);
    logf("modules: %lu loaded", count);
    for (DWORD i = 0; i < count; ++i) {
        MODULEINFO info{};
        wchar_t path[MAX_PATH] = {};
        GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info));
        GetModuleFileNameW(modules[i], path, MAX_PATH);
        logf("  module %016llx size %08lx %ls", reinterpret_cast<unsigned long long>(info.lpBaseOfDll),
             info.SizeOfImage, path);
    }
}

void hang_watch_arm(unsigned seconds) {
    if (!seconds || fired.exchange(true))
        return;
    armed = true;
    framed = false;
    if (HANDLE thread = CreateThread(nullptr, 0, watch, reinterpret_cast<void*>(static_cast<uintptr_t>(seconds)), 0,
                                     nullptr))
        CloseHandle(thread);
}

void hang_watch_frame() {
    if (armed && !framed)
        framed = true;
}

}  // namespace aceqv

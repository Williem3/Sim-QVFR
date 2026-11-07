#include "probe/hang_dump.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

namespace simqvfr {
namespace {

// Appends lines to the game's log as log.cpp writes them ("HH:MM:SS.mmm [tid] message"). The layer keeps the
// log open for appending and shares it for writing, so both append whole lines.
class Log {
public:
    explicit Log(const wchar_t* path)
        : file_(CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~Log() {
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
    }
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void operator()(const char* format, ...) {
        if (file_ == INVALID_HANDLE_VALUE)
            return;
        char message[1536];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char line[2048];
        int n = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%lu] %s\r\n", t.wHour, t.wMinute, t.wSecond,
                              t.wMilliseconds, GetCurrentThreadId(), message);
        if (n <= 0)
            return;
        if (n >= static_cast<int>(sizeof(line)))
            n = static_cast<int>(sizeof(line)) - 1;
        DWORD written = 0;
        WriteFile(file_, line, static_cast<DWORD>(n), &written, nullptr);
    }

private:
    HANDLE file_;
};

std::string utf8(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// The game's modules, read from its loader list through ReadProcessMemory. The list can change while the game
// loads, so a failed or grown read is tried again.
void log_modules(HANDLE process, Log& log) {
    std::vector<HMODULE> modules(1024);
    DWORD bytes = 0;
    bool listed = false;
    for (int attempt = 0; attempt < 5 && !listed; ++attempt) {
        if (!EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                  &bytes, LIST_MODULES_ALL)) {
            Sleep(20);
            continue;
        }
        if (bytes > modules.size() * sizeof(HMODULE))
            modules.resize(bytes / sizeof(HMODULE) + 64);
        else
            listed = true;
    }
    if (!listed) {
        log("hang dump: the game's modules cannot be listed (error %lu)", GetLastError());
        return;
    }
    const DWORD count = bytes / sizeof(HMODULE);
    log("modules: %lu loaded", count);
    for (DWORD i = 0; i < count; ++i) {
        MODULEINFO info{};
        wchar_t path[MAX_PATH] = {};
        GetModuleInformation(process, modules[i], &info, sizeof(info));
        GetModuleFileNameExW(process, modules[i], path, MAX_PATH);
        log("  module %016llx size %08lx %s", reinterpret_cast<unsigned long long>(info.lpBaseOfDll),
            info.SizeOfImage, utf8(path).c_str());
    }
}

}  // namespace

int hang_dump(unsigned long pid, const wchar_t* logPath) {
    // Opened while standing by, so a hang needs nothing more from the game. PROCESS_DUP_HANDLE as well:
    // MiniDumpWithHandleData reads the game's handles through it; SYNCHRONIZE to see it exit.
    HANDLE process =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
    wchar_t eventName[64];
    hang_dump_event(pid, eventName);
    HANDLE hang = OpenEventW(SYNCHRONIZE, FALSE, eventName);
    if (!process || !hang) {
        Log log(logPath);
        log("hang dump: cannot open process %lu or its hang event (error %lu); no minidump", pid, GetLastError());
        if (process)
            CloseHandle(process);
        if (hang)
            CloseHandle(hang);
        return kHangDumpNoProcess;
    }
    const HANDLE waits[] = {hang, process};
    const DWORD woken = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    CloseHandle(hang);
    if (woken != WAIT_OBJECT_0) {  // the game exited (or the wait failed): nothing to dump
        CloseHandle(process);
        return kHangDumpGameExited;
    }
    Log log(logPath);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"hang-%02u%02u%02u.dmp", t.wHour, t.wMinute, t.wSecond);
    std::wstring dump = logPath;
    const size_t slash = dump.find_last_of(L"\\/");
    dump = (slash == std::wstring::npos ? std::wstring() : dump.substr(0, slash + 1)) + name;
    const wchar_t* dumpPath = dump.c_str();
    const std::string dumpName = utf8(dumpPath);
    log_modules(process, log);
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto write = dbghelp ? reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                               : nullptr;
    if (!write) {
        log("hang dump: no dbghelp.dll in System32; no minidump");
        CloseHandle(process);
        return kHangDumpNoDbghelp;
    }
    HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log("hang dump: cannot create %s (error %lu)", dumpName.c_str(), GetLastError());
        CloseHandle(process);
        return kHangDumpNoFile;
    }
    // Stacks, thread state and modules; no heap (small, and enough to see where each thread waits).
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                 MiniDumpWithHandleData);
    const ULONGLONG start = GetTickCount64();
    const BOOL ok = write(process, pid, file, type, nullptr, nullptr, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    CloseHandle(process);
    const double seconds = (GetTickCount64() - start) / 1000.0;
    if (!ok) {
        DeleteFileW(dumpPath);
        log("hang dump: MiniDumpWriteDump failed (error 0x%08lx) after %.1f s", error, seconds);
        return kHangDumpFailed;
    }
    log("hang dump: %s written from outside the game (%lld KB, %.1f s)", dumpName.c_str(), size.QuadPart / 1024,
        seconds);
    return kHangDumpWritten;
}

bool hang_dump_command(int argc, wchar_t** argv, int& exitCode) {
    if (argc < 2 || _wcsicmp(argv[1], L"--hang-dump") != 0)
        return false;
    wchar_t* end = nullptr;
    const unsigned long pid = argc == 4 ? std::wcstoul(argv[2], &end, 10) : 0;
    exitCode = pid && end && !*end ? hang_dump(pid, argv[3]) : kHangDumpUsage;
    return true;
}

}  // namespace simqvfr

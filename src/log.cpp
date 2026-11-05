#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include "xr.h"

namespace simqvfr {
namespace {

std::mutex logMutex;
HANDLE logFile = INVALID_HANDLE_VALUE;
bool logOpened = false;

std::filesystem::path logPath() {
    wchar_t env[32768];
    DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_LOG", env, static_cast<DWORD>(std::size(env)));
    if (n > 0 && n < std::size(env)) {
        std::filesystem::path p(env);
        if (p.has_root_name() && p.has_root_directory())
            return p;
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&log_line), &self);
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(self, module, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(module).parent_path();
    // The package layout keeps the DLL in <package>/build and logs in <package>/logs.
    if (dir.filename() == L"build")
        dir = dir.parent_path();
    return dir / L"logs" / L"sim-qvfr.log";
}

void open() {
    logOpened = true;
    std::filesystem::path path = logPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

void log_line(const char* message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%lu] %s\r\n", t.wHour, t.wMinute, t.wSecond,
                          t.wMilliseconds, GetCurrentThreadId(), message);
    if (n <= 0)
        return;
    if (n >= static_cast<int>(sizeof(line)))
        n = static_cast<int>(sizeof(line)) - 1;
    std::lock_guard lock(logMutex);
    if (!logOpened)
        open();
    if (logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(logFile, line, static_cast<DWORD>(n), &written, nullptr);
    }
}

std::wstring log_directory() { return logPath().parent_path().wstring(); }

void logf(const char* format, ...) {
    char message[1536];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    log_line(message);
}

}  // namespace simqvfr

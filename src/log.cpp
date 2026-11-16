#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include "log_prefix.h"
#include "module_path.h"
#include "xr.h"

namespace simqvfr {
namespace {

std::mutex logMutex;
HANDLE logFile = INVALID_HANDLE_VALUE;
bool logOpened = false;
uint64_t logWritten = 0;  // this process's bytes
bool logFull = false;

std::filesystem::path logPath() {
    wchar_t env[32768];
    DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_LOG", env, static_cast<DWORD>(std::size(env)));
    if (n > 0 && n < std::size(env)) {
        std::filesystem::path p(env);
        if (p.has_root_name() && p.has_root_directory())
            return p;
    }
    std::filesystem::path dir = module_folder(reinterpret_cast<const void*>(&log_line));
    // The package layout keeps the DLL in <package>/build and logs in <package>/logs.
    if (dir.filename() == L"build")
        dir = dir.parent_path();
    return dir / L"logs" / L"sim-qvfr.log";
}

bool debugLevel() {
    static const bool on = [] {
        wchar_t level[16] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_LOG_LEVEL", level, static_cast<DWORD>(std::size(level)));
        return n > 0 && n < std::size(level) && _wcsicmp(level, L"debug") == 0;
    }();
    return on;
}

uint64_t limit() { return debugLevel() ? kLogDebugLimitBytes : kLogLimitBytes; }

void write(const char* text, int n) {
    DWORD written = 0;
    WriteFile(logFile, text, static_cast<DWORD>(n), &written, nullptr);
    logWritten += written;
}

void open() {
    logOpened = true;
    std::filesystem::path path = logPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // A log reused across launches (the default path) is moved aside once it is large: at most two of them.
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (!ec && size > kLogRotateBytes) {
        std::filesystem::path old = path;
        old.replace_extension(L".old" + path.extension().wstring());
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void line(const char* message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const DWORD tid = GetCurrentThreadId();
    char text[2048];
    int n = log_prefix(text, sizeof(text), t, tid);
    const int m = n > 0 ? std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), "%s\r\n", message) : -1;
    if (m < 0)
        return;
    n += m;
    if (n >= static_cast<int>(sizeof(text)))
        n = static_cast<int>(sizeof(text)) - 1;
    std::lock_guard lock(logMutex);
    if (!logOpened) {
        open();
        if (logFile != INVALID_HANDLE_VALUE) {
            char head[192];
            const int p = log_prefix(head, sizeof(head), t, tid);
            const int h = p + std::snprintf(head + p, sizeof(head) - static_cast<size_t>(p), "log: %s level, at most %llu MB from this process%s\r\n",
                                            debugLevel() ? "debug" : "info", static_cast<unsigned long long>(limit() >> 20),
                                            debugLevel() ? "" : " (the launcher's Diagnostics pane: Detailed logs for more)");
            if (h > 0 && h < static_cast<int>(sizeof(head)))
                write(head, h);
        }
    }
    if (logFile == INVALID_HANDLE_VALUE || logFull)
        return;
    if (logWritten + static_cast<uint64_t>(n) > limit()) {
        logFull = true;
        char last[160];
        const int p = log_prefix(last, sizeof(last), t, tid);
        const int k = p + std::snprintf(last + p, sizeof(last) - static_cast<size_t>(p), "log: %llu MB written; nothing more is logged from here on\r\n",
                                        static_cast<unsigned long long>(limit() >> 20));
        if (k > 0 && k < static_cast<int>(sizeof(last)))
            write(last, k);
        return;
    }
    write(text, n);
}

}  // namespace

void log_line(const char* message) { line(message); }

bool log_debug() { return debugLevel(); }

std::wstring log_directory() { return logPath().parent_path().wstring(); }

std::wstring captures_directory() {
    const std::filesystem::path dir = std::filesystem::path(log_directory()) / L"captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

std::wstring log_file() { return logPath().wstring(); }

void logf(const char* format, ...) {
    char message[1536];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    line(message);
}

void logd(const char* format, ...) {
    if (!debugLevel())
        return;
    char message[1536];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    line(message);
}

}  // namespace simqvfr

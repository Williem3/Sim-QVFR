#include "log.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "log_prefix.h"
#include "module_path.h"
#include "xr.h"

namespace simqvfr {
namespace {

std::mutex logMutex;
std::mutex fileMutex;  // WriteFile's: the writer thread's batches and the lines written at once
HANDLE logFile = INVALID_HANDLE_VALUE;
bool logOpened = false;
uint64_t logWritten = 0;  // this process's bytes (counted as they are queued or written)
bool logFull = false;

// log_async: while on, the lines go to `pending` (under logMutex) and a thread of ours writes them, so the thread that
// logged (the game's render thread, every 900 frames) doesn't wait on the disk. Off: each line written at once.
struct Writer {
    std::thread thread;
    std::condition_variable wake;
    std::string pending;
    bool on = false, stop = false;
    ~Writer();
};
Writer writer;

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

void write_now(const char* text, size_t n) {
    std::lock_guard file(fileMutex);
    DWORD written = 0;
    WriteFile(logFile, text, static_cast<DWORD>(n), &written, nullptr);
}

// A line (under logMutex): queued for the writer while it runs, else written at once.
void write(const char* text, int n) {
    logWritten += static_cast<uint64_t>(n);
    if (writer.on) {
        writer.pending.append(text, static_cast<size_t>(n));
        writer.wake.notify_one();
        return;
    }
    write_now(text, static_cast<size_t>(n));
}

void run_writer() {
    std::string batch;
    std::unique_lock lock(logMutex);
    for (;;) {
        writer.wake.wait(lock, [] { return writer.stop || !writer.pending.empty(); });
        if (writer.pending.empty())
            break;  // stopping, everything written
        batch.swap(writer.pending);
        lock.unlock();
        write_now(batch.data(), batch.size());
        batch.clear();
        lock.lock();
    }
}

// Lines keep going to the queue until the writer has written it all and ended (`on` cleared only after the join), so a
// line logged meanwhile is never written ahead of older queued ones.
void stop_writer() {
    std::thread thread;
    {
        std::lock_guard lock(logMutex);
        writer.stop = true;
        thread.swap(writer.thread);
    }
    writer.wake.notify_all();
    if (thread.joinable())
        thread.join();
    // What was queued after the writer's last look, written before the lock is given back: a line logged meanwhile
    // (written at once from now on) can't overtake it.
    std::lock_guard lock(logMutex);
    writer.on = false;
    writer.stop = false;
    if (!writer.pending.empty() && logFile != INVALID_HANDLE_VALUE)
        write_now(writer.pending.data(), writer.pending.size());
    writer.pending.clear();
}

std::atomic<bool> processEnding{false};  // log_process_ending

// At a process's end (DllMain's DLL_PROCESS_DETACH with the process ending, log_process_ending) the other threads are
// gone, perhaps holding a mutex: nothing waited for. The writer's thread detached, what it left written only if the
// mutexes are free.
Writer::~Writer() {
    if (!processEnding.load()) {
        stop_writer();
        return;
    }
    if (thread.joinable())
        thread.detach();
    on = false;
    std::unique_lock lock(logMutex, std::try_to_lock);
    std::unique_lock file(fileMutex, std::try_to_lock);
    if (lock.owns_lock() && file.owns_lock() && !pending.empty() && logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(logFile, pending.data(), static_cast<DWORD>(pending.size()), &written, nullptr);
    }
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

void log_process_ending() { processEnding.store(true); }

void log_async(bool on) {
    if (!on) {
        stop_writer();
        return;
    }
    std::lock_guard lock(logMutex);
    if (writer.on || writer.thread.joinable())
        return;
    try {
        writer.thread = std::thread(run_writer);
        writer.on = true;
    } catch (...) {
        writer.on = false;  // no thread: lines written at once, as before
    }
}

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

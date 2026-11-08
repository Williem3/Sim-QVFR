#include "launch_watch.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>

#include "../games/game.h"
#include "launch_plan.h"
#include "report.h"
#include "settings.h"

namespace fs = std::filesystem;

namespace simqvfr::launcher {

namespace {

uint64_t ticks(const FILETIME& t) { return static_cast<uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime; }

uint64_t now_ticks() {
    FILETIME t{};
    GetSystemTimeAsFileTime(&t);
    return ticks(t);
}

double seconds_between(uint64_t from, uint64_t to) { return to > from ? (to - from) / 1e7 : 0; }

// A file or folder in `folder` matching `pattern` made since `since` (FILETIME units).
bool made_since(const std::wstring& folder, const wchar_t* pattern, bool folders, uint64_t since) {
    if (folder.empty())
        return false;
    WIN32_FIND_DATAW f{};
    HANDLE find = FindFirstFileExW((fs::path(folder) / pattern).c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    do {
        const bool isFolder = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isFolder != folders || wcscmp(f.cFileName, L".") == 0 || wcscmp(f.cFileName, L"..") == 0)
            continue;
        found = ticks(f.ftCreationTime) >= since || ticks(f.ftLastWriteTime) >= since;
    } while (!found && FindNextFileW(find, &f));
    FindClose(find);
    return found;
}

std::wstring fps_text(double fps) {
    wchar_t text[32];
    swprintf_s(text, L"%.1f fps", fps);
    return text;
}

}  // namespace

std::wstring duration_text(double seconds) {
    const long long s = std::max(0LL, std::llround(seconds));
    if (s < 60)
        return std::to_wstring(s) + L" s";
    const long long minutes = s / 60;
    if (minutes < 60)
        return std::to_wstring(minutes) + L" min";
    wchar_t text[32];
    swprintf_s(text, L"%lld h %02lld min", minutes / 60, minutes % 60);
    return text;
}

bool crash_exit_code(uint32_t code) {
    constexpr uint32_t kControlC = 0xC000013A;  // STATUS_CONTROL_C_EXIT: its console closed, not a crash
    return (code & 0xC0000000u) == 0xC0000000u && code != kControlC;
}

std::wstring loader_error(const std::string& output) {
    size_t at = 0;
    while (at < output.size()) {
        size_t end = output.find('\n', at);
        if (end == std::string::npos)
            end = output.size();
        std::string line = output.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // The loader's own: "Error [<type> | <command> | <message id>] : <message>".
        if (line.rfind("Error [", 0) != 0)
            continue;
        // Printed as an error, but harmless: an elevated game ignores XR_API_LAYER_PATH, and the launcher registers
        // the launch's layers machine-wide instead (it was shown as the reason when LMU crashed below the layer).
        if (line.find("is being ignored due to running from an elevated context") != std::string::npos)
            continue;
        const size_t close = line.find(']');
        if (close == std::string::npos || line.find(" | ") > close)
            continue;
        size_t from = close + 1;
        while (from < line.size() && (line[from] == ' ' || line[from] == ':'))
            ++from;
        std::string message = line.substr(from);
        while (!message.empty() && (message.back() == ' ' || message.back() == '\t'))
            message.pop_back();
        return widen(message.empty() ? line : message);
    }
    return {};
}

LaunchOutcome launch_outcome(const LaunchSeen& seen, const std::wstring& name) {
    const std::wstring openxr = seen.loaderError.empty() ? L"" : L" · OpenXR: " + seen.loaderError;
    if (!seen.exited) {
        if (seen.layerLoaded)
            return {LaunchState::Running, 0, name + L" is running with Sim-QVFR", LaunchLink::Diagnostics};
        if (!seen.loaderError.empty())
            return {LaunchState::NoLayer, 2, name + L" started without Sim-QVFR" + openxr, LaunchLink::Report};
        if (seen.seconds >= kLayerLoadSeconds)
            return {LaunchState::NotLoaded, 1, L"Sim-QVFR hasn’t loaded in " + name + L" yet · Report an issue", LaunchLink::Report};
        return {LaunchState::Starting, 3, L"Starting " + name + L"…", LaunchLink::None};
    }
    if (seen.hang)
        return {LaunchState::Crashed, 2, name + L" stopped responding after " + duration_text(seen.ranSeconds) + L" · Report an issue",
                LaunchLink::Report};
    if (seen.crashReport || crash_exit_code(seen.exitCode))
        return {LaunchState::Crashed, 2,
                name + (seen.layerLoaded ? L" crashed after " + duration_text(seen.ranSeconds) : L" crashed while starting") +
                    L" · Report an issue",
                LaunchLink::Report};
    if (!seen.layerLoaded)
        return {LaunchState::ClosedNoLayer, 1,
                name + L" closed without loading Sim-QVFR" + (openxr.empty() ? L" · Report an issue" : openxr), LaunchLink::Report};
    return {LaunchState::Closed, 0,
            name + L" closed after " + duration_text(seen.ranSeconds) + (seen.fps > 0 ? L" · " + fps_text(seen.fps) : L""),
            LaunchLink::Diagnostics};
}

void LaunchWatch::start(const LaunchedGame& launched, const games::GameInfo& game, const std::wstring& runtimeIni,
                        const std::wstring& crashFolder) {
    stop();
    game_ = &game;
    process_ = launched.process;
    pid_ = launched.pid;
    runFolder_ = launched.runFolder;
    log_ = launched.log;
    runtimeIni_ = runtimeIni;
    crashFolder_ = crashFolder;
    started_ = now_ticks();
    exitedAt_ = 0;
    loaderRead_ = 0;
    loaderTail_.clear();
    seen_ = {};
}

void LaunchWatch::stop() {
    if (process_)
        CloseHandle(process_);
    process_ = nullptr;
    game_ = nullptr;
    pid_ = 0;
}

LaunchOutcome LaunchWatch::outcome() const { return launch_outcome(seen_, game_ ? game_->name : L""); }

bool LaunchWatch::poll() {
    if (!game_)
        return false;
    const LaunchSeen before = seen_;
    const uint64_t now = now_ticks();
    if (seen_.exited && seconds_between(exitedAt_, now) > kCrashReportSeconds)
        return false;  // settled: nothing more to learn about this launch
    seen_.seconds = seconds_between(started_, now);
    if (!seen_.exited && process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        seen_.exited = true;
        seen_.exitCode = code;
        FILETIME created{}, exited{}, kernel{}, user{};
        seen_.ranSeconds = GetProcessTimes(process_, &created, &exited, &kernel, &user) ? seconds_between(ticks(created), ticks(exited))
                                                                                        : seen_.seconds;
        exitedAt_ = now;
    }
    // The layer writes its process id at xrCreateInstance; it stays after the game exits, so a game that loaded
    // Sim-QVFR and died between two looks still counts.
    if (!seen_.layerLoaded && pid_ &&
        GetPrivateProfileIntW(L"runtime", L"live_pid", 0, runtimeIni_.c_str()) == static_cast<UINT>(pid_))
        seen_.layerLoaded = true;
    // The OpenXR loader's error, read as the game writes it (only what's new each time).
    if (!seen_.layerLoaded && seen_.loaderError.empty() && !runFolder_.empty()) {
        HANDLE file = CreateFileW((fs::path(runFolder_) / kLoaderOutputName).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) > loaderRead_) {
                const uint64_t want = std::min<uint64_t>(static_cast<uint64_t>(size.QuadPart) - loaderRead_, 1u << 20);
                std::string chunk(static_cast<size_t>(want), '\0');
                LARGE_INTEGER at{};
                at.QuadPart = static_cast<LONGLONG>(loaderRead_);
                DWORD read = 0;
                if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN) &&
                    ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
                    loaderRead_ += read;
                    loaderTail_ += chunk.substr(0, read);
                    // Whole lines only, but the last line too once the game has exited.
                    const size_t lineEnd = seen_.exited ? loaderTail_.size() : loaderTail_.rfind('\n');
                    if (lineEnd != std::string::npos) {
                        seen_.loaderError = loader_error(loaderTail_.substr(0, lineEnd));
                        loaderTail_.erase(0, lineEnd);
                    }
                }
            }
            CloseHandle(file);
        }
    }
    // After the exit: the crash reporter's report, a hang dump from the layer's hang watch, and the session's
    // frame rate when the layer measured this launch.
    if (seen_.exited && seconds_between(exitedAt_, now) <= kCrashReportSeconds) {
        if (!seen_.crashReport)
            seen_.crashReport = made_since(crashFolder_, L"*", true, started_);
        if (!seen_.hang && !runFolder_.empty())
            seen_.hang = made_since(fs::path(runFolder_).parent_path().wstring(), L"hang-*.dmp", false, started_);
        if (seen_.layerLoaded && seen_.fps <= 0) {
            const SessionInfo session = load_runtime_info(runtimeIni_).session;
            if (!log_.empty() && _wcsicmp(session.log.c_str(), log_.c_str()) == 0 && session.fps > 0)
                seen_.fps = session.fps;
        }
    }
    return launch_outcome(seen_, game_->name).text != launch_outcome(before, game_->name).text;
}

}  // namespace simqvfr::launcher

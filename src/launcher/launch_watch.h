// After Launch: what became of the game the launcher started, for the action bar. The window follows the launched
// process on its timer (LaunchWatch::poll); launch_outcome turns what it saw into the status, pure, for the test.
//
// - Running: Sim-QVFR loaded in it (the layer writes its process id as runtime.ini's live_pid at xrCreateInstance).
// - Started without Sim-QVFR: the OpenXR loader printed an error (the run folder's openxr-loader.log) and the layer
//   never loaded: the game runs flat. Still not loaded after kLayerLoadSeconds, the status says so, softer.
// - Closed: its time, and the session's frame rate when the layer measured this launch (runtime.ini's session_log is
//   this launch's log).
// - Crashed: a crash exit code, or a new crash report from the game's crash reporter (GameInfo::crashReports); it
//   stopped responding: the layer's hang watch dumped it.
// - Closed without loading Sim-QVFR.
#pragma once

#include <cstdint>
#include <string>

#include <windows.h>

namespace simqvfr::games {
struct GameInfo;
}

namespace simqvfr::launcher {

// How long a game may run before the status says Sim-QVFR hasn't loaded (a game can load a while before OpenXR).
inline constexpr double kLayerLoadSeconds = 180;
// After the game exits, how long its crash reporter has to write its report.
inline constexpr double kCrashReportSeconds = 15;

// What the watch saw of the launched game.
struct LaunchSeen {
    double seconds = 0;         // since the launch
    bool exited = false;
    uint32_t exitCode = 0;
    double ranSeconds = 0;      // exited: how long it ran
    bool layerLoaded = false;   // Sim-QVFR loaded in it
    std::wstring loaderError;   // the OpenXR loader's first error (loader_error); empty: none
    bool crashReport = false;   // the game wrote a crash report since the launch
    bool hang = false;          // the layer's hang watch dumped it (logs\hang-*.dmp) since the launch
    double fps = -1;            // this launch's measured frame rate, -1: not measured
    bool operator==(const LaunchSeen&) const = default;
};

enum class LaunchState { Starting, Running, NotLoaded, NoLayer, Closed, ClosedNoLayer, Crashed };
// Where the status leads when clicked.
enum class LaunchLink { None, Diagnostics, Report };

struct LaunchOutcome {
    LaunchState state = LaunchState::Starting;
    int tone = 3;  // the badge's: 0 good, 1 warning, 2 problem, 3 in progress
    std::wstring text;
    LaunchLink link = LaunchLink::None;
    bool finished() const { return state == LaunchState::Closed || state == LaunchState::ClosedNoLayer || state == LaunchState::Crashed; }
};

// The status for what was seen of `gameName` ("Assetto Corsa EVO").
LaunchOutcome launch_outcome(const LaunchSeen& seen, const std::wstring& gameName);

// The OpenXR loader's first error in `output` (the game's stdout and stderr): its message, from a line like
// "Error [GENERAL | xrCreateInstance | OpenXR-Loader] : <message>"; empty if none. The game's own lines, mixed in
// ("Fossilize ERROR: ..."), don't count.
std::wstring loader_error(const std::string& output);

// "45 s", "23 min", "1 h 05 min".
std::wstring duration_text(double seconds);

// A crash exit code: an NTSTATUS error (0xC0000005, an access violation; 0xC0000409, a fast fail).
bool crash_exit_code(uint32_t code);

// The game launch_game started, handed to the window (which closes the handle).
struct LaunchedGame {
    HANDLE process = nullptr;
    DWORD pid = 0;
    std::wstring runFolder;  // logs\run-<time>: openxr-loader.log
    std::wstring log;        // the layer's log for this launch (SIM_QVFR_LOG)
};

// Following one launch (the window's timer calls poll).
class LaunchWatch {
public:
    LaunchWatch() = default;
    LaunchWatch(const LaunchWatch&) = delete;
    LaunchWatch& operator=(const LaunchWatch&) = delete;
    ~LaunchWatch() { stop(); }

    // Follows `launched` (taking its handle) of `game`; runtime.ini is `runtimeIni`, the game's crash reports
    // are in `crashFolder` (empty: none).
    void start(const LaunchedGame& launched, const games::GameInfo& game, const std::wstring& runtimeIni,
               const std::wstring& crashFolder);
    // Looks again; true if what it saw changed.
    bool poll();
    // Stops following (the status goes).
    void stop();

    bool active() const { return game_ != nullptr; }
    const games::GameInfo* game() const { return game_; }
    LaunchOutcome outcome() const;

private:
    const games::GameInfo* game_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    std::wstring runFolder_, log_, runtimeIni_, crashFolder_;
    uint64_t started_ = 0, exitedAt_ = 0;  // FILETIME units
    uint64_t loaderRead_ = 0;              // openxr-loader.log's bytes read so far
    std::string loaderTail_;               // its last line, until it ends
    LaunchSeen seen_;
};

}  // namespace simqvfr::launcher

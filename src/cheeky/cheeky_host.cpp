#include "cheeky/cheeky_host.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

#include "cheeky/cheeky_views.h"
#include "log.h"
#include "xr.h"

namespace simqvfr {
namespace {

// Cheeky's runtime ABI v1 (its shared/runtime_host_api.hpp). The runtime rejects a Start whose size
// or abi differ, so a Cheeky build with another ABI refuses to start rather than misreading this.
constexpr uint32_t kRuntimeAbi = 1;
constexpr uint32_t kRendererD3D11 = 0, kRendererD3D12 = 1;
// Cheeky's host ids are uevr 0, standalone 1, optiscaler 2, and it accepts no others yet: the layer
// presents as standalone (Cheeky then names its log CheekyFoveatedDLSS-Standalone.log).
constexpr uint32_t kHostStandalone = 1;

struct RuntimeStart {
    uint32_t size = sizeof(RuntimeStart);
    uint32_t abi = kRuntimeAbi;
    const wchar_t* configDirectory = nullptr;
    uint32_t renderer = kRendererD3D12;
    void* device = nullptr;
    void* queue = nullptr;  // required for D3D12 processing
    uint64_t* attachment = nullptr;
    uint32_t host = kHostStandalone;
};
static_assert(sizeof(RuntimeStart) == 56 && offsetof(RuntimeStart, configDirectory) == 8 &&
                  offsetof(RuntimeStart, renderer) == 16 && offsetof(RuntimeStart, device) == 24 &&
                  offsetof(RuntimeStart, attachment) == 40 && offsetof(RuntimeStart, host) == 48,
              "Cheeky's CheekyRuntimeStart layout");

using StartFn = bool (*)(const RuntimeStart*);
using DetachFn = void (*)(uint64_t);
using TickFn = void (*)(uint64_t, uint32_t, void*, void*);
using CommandFn = bool (*)(uint64_t, const char*);
using SnapshotFn = bool (*)(char*, uint32_t);

struct Host {
    std::mutex mutex;
    std::atomic<bool> running{false};
    uint64_t attachment = 0;
    TickFn tick = nullptr;
    CommandFn command = nullptr;
    SnapshotFn snapshot = nullptr;
    DetachFn detach = nullptr;
    void* device = nullptr;  // the session's: ID3D12Device (renderer 1) or ID3D11Device (renderer 0)
    void* queue = nullptr;   // D3D12 only
    uint32_t renderer = kRendererD3D12;
    uint64_t requests = 0;  // Command request ids
    // The view report.
    unsigned reportSeconds = 10;
    uint64_t frames = 0, framesAtReport = 0, nextReport = 0;
    size_t viewsLogged = 0;
    std::vector<CheekyView> views;
    std::filesystem::path snapshotFile;
    CheekyStatus status = CheekyStatus::Off;
    std::string version;  // Cheeky's, from its snapshot ("0.6.3-standalone")
    std::string applied;  // the last settings applied (cheeky_apply)
} host;

std::wstring absolute(const std::wstring& path, const std::wstring& base) {
    std::filesystem::path p(path);
    return (p.is_absolute() ? p : std::filesystem::path(base) / p).lexically_normal().wstring();
}

std::string snapshot_locked() {
    if (!host.snapshot)
        return {};
    // Bounded JSON; false also means it did not fit, so grow up to 8 MB.
    for (uint32_t capacity = 256 * 1024; capacity <= 8u * 1024 * 1024; capacity *= 2) {
        std::vector<char> buffer(capacity);
        if (host.snapshot(buffer.data(), capacity))
            return std::string(buffer.data());
    }
    return {};
}

void report_locked() {
    const std::string json = snapshot_locked();
    if (json.empty()) {
        log_line("cheeky: snapshot failed");
        return;
    }
    if (!host.snapshotFile.empty()) {
        HANDLE file = CreateFileW(host.snapshotFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    std::vector<CheekyView> views = cheeky_views(json);
    const uint64_t frames = host.frames - host.framesAtReport;
    logf("cheeky: %zu DLSS view(s) over the last %llu xrEndFrames%s", views.size(),
         static_cast<unsigned long long>(frames), views.size() != host.viewsLogged ? " (count changed)" : "");
    for (const std::string& line : cheeky_view_report(host.views, views, frames))
        log_line(line.c_str());
    host.viewsLogged = views.size();
    host.views = std::move(views);
    host.framesAtReport = host.frames;
}

}  // namespace

CheekyConfig cheeky_config(const wchar_t* ini, const std::wstring& layerDir) {
    CheekyConfig c;
    c.enabled = GetPrivateProfileIntW(L"cheeky", L"enabled", 0, ini) != 0;
    wchar_t value[MAX_PATH] = {};
    GetPrivateProfileStringW(L"cheeky", L"config_dir", L"CheekyFoveatedDLSS", value, MAX_PATH, ini);
    c.configDir = absolute(value, layerDir);
    GetPrivateProfileStringW(L"cheeky", L"runtime", L"", value, MAX_PATH, ini);
    c.runtime = value[0] ? absolute(value, layerDir)
                         : (std::filesystem::path(c.configDir) / L"CheekyFoveatedDLSSRuntime.dll").wstring();
    c.reportSeconds = GetPrivateProfileIntW(L"cheeky", L"report_s", 10, ini);
    return c;
}

bool cheeky_start(const CheekyConfig& config) {
    if (!config.enabled)
        return false;
    std::lock_guard lock(host.mutex);
    if (host.running)
        return true;
    // Its own folder first for its dependencies; never the game's folder (Cheeky's loader may sit there).
    HMODULE runtime = LoadLibraryExW(config.runtime.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!runtime) {
        logf("cheeky: cannot load %ls (error %lu); Cheeky off", config.runtime.c_str(), GetLastError());
        host.status = CheekyStatus::Missing;
        return false;
    }
    const auto start = reinterpret_cast<StartFn>(GetProcAddress(runtime, "CheekyRuntime_Start"));
    host.tick = reinterpret_cast<TickFn>(GetProcAddress(runtime, "CheekyRuntime_Tick"));
    host.command = reinterpret_cast<CommandFn>(GetProcAddress(runtime, "CheekyRuntime_Command"));
    host.snapshot = reinterpret_cast<SnapshotFn>(GetProcAddress(runtime, "CheekyRuntime_Snapshot"));
    host.detach = reinterpret_cast<DetachFn>(GetProcAddress(runtime, "CheekyRuntime_Detach"));
    if (!start || !host.tick || !host.command || !host.snapshot || !host.detach) {
        logf("cheeky: %ls lacks the runtime ABI exports; Cheeky off", config.runtime.c_str());
        host.tick = nullptr;
        host.status = CheekyStatus::Refused;
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(config.configDir, ec);
    RuntimeStart input;
    input.configDirectory = config.configDir.c_str();
    input.attachment = &host.attachment;
    // The runtime stays resident until the game exits (it pins itself), so the module is never freed.
    if (!start(&input) || !host.attachment) {
        logf("cheeky: runtime refused to start (another Cheeky loaded, bad CheekyFoveatedDLSS.ini or an ABI "
             "mismatch); see the Cheeky log in %ls",
             config.configDir.c_str());
        host.tick = nullptr;
        host.status = CheekyStatus::Refused;
        return false;
    }
    host.reportSeconds = config.reportSeconds;
    host.snapshotFile = std::filesystem::path(log_directory()) / L"cheeky-snapshot.json";
    host.running = true;
    host.status = CheekyStatus::Running;
    const std::string json = snapshot_locked();
    const size_t at = json.find("\"version\":\"");
    if (at != std::string::npos) {
        const size_t begin = at + 11, end = json.find('"', begin);
        host.version = json.substr(begin, end == std::string::npos ? 0 : end - begin);
    }
    logf("cheeky: runtime %ls started (attachment %llu, settings in %ls); waiting for the session's device",
         config.runtime.c_str(), static_cast<unsigned long long>(host.attachment), config.configDir.c_str());
    return true;
}

bool cheeky_running() { return host.running; }

CheekyStatus cheeky_status() { return host.status; }

std::wstring cheeky_version() { return std::wstring(host.version.begin(), host.version.end()); }

bool cheeky_apply(const std::string& lines) {
    if (!host.running || lines == host.applied)
        return true;
    const bool ok = cheeky_set(lines);
    if (ok)
        host.applied = lines;
    std::string shown = lines;
    std::replace(shown.begin(), shown.end(), '\n', ' ');
    logf("cheeky: settings %s: %s", ok ? "applied" : "REFUSED", shown.c_str());
    return ok;
}

namespace {

void session_locked(uint32_t renderer, void* device, void* queue) {
    if (!device && host.reportSeconds)
        report_locked();  // the session's last numbers
    host.renderer = renderer;
    host.device = device;
    host.queue = queue;
    // A null device pauses processing until a later tick brings graphics again.
    host.tick(host.attachment, renderer, device, queue);
    logf("cheeky: %s", !device                        ? "session ended; processing paused"
                       : renderer == kRendererD3D11 ? "D3D11 device handed over"
                                                    : "D3D12 device and queue handed over");
}

}  // namespace

void cheeky_session(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!host.running)
        return;
    std::lock_guard lock(host.mutex);
    session_locked(kRendererD3D12, device, queue);
}

void cheeky_session_d3d11(ID3D11Device* device) {
    if (!host.running)
        return;
    std::lock_guard lock(host.mutex);
    session_locked(kRendererD3D11, device, nullptr);
}

void cheeky_frame() {
    if (!host.running)
        return;
    std::lock_guard lock(host.mutex);
    if (!host.device)
        return;
    host.tick(host.attachment, host.renderer, host.device, host.queue);
    ++host.frames;
    if (!host.reportSeconds)
        return;
    const uint64_t now = GetTickCount64();
    if (!host.nextReport) {
        host.nextReport = now + host.reportSeconds * 1000ull;
        host.framesAtReport = host.frames;
    } else if (now >= host.nextReport) {
        host.nextReport = now + host.reportSeconds * 1000ull;
        report_locked();
    }
}

bool cheeky_set(const std::string& lines) {
    if (!host.running)
        return false;
    std::lock_guard lock(host.mutex);
    const std::string command = "1\n" + std::to_string(++host.requests) + "\nset\n" + lines;
    return host.command(host.attachment, command.c_str());
}

std::string cheeky_snapshot() {
    if (!host.running)
        return {};
    std::lock_guard lock(host.mutex);
    return snapshot_locked();
}

}  // namespace simqvfr

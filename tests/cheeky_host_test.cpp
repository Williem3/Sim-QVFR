// Offline check of the Cheeky runtime host (cheeky/cheeky_host.h) and its snapshot reading
// (cheeky/cheeky_views.h): the [cheeky] keys and their defaults, a disabled or missing runtime staying
// off, and against tests/cheeky_fake_runtime.cpp the Start the runtime accepts, ticks with and without
// the session's device, the "set" command, and the views and per-frame evaluations read from a
// snapshot. With ACE_QV_CHEEKY_RUNTIME naming a real CheekyFoveatedDLSSRuntime.dll, a second process
// (this exe with --real) starts that one with no graphics and reads its snapshot.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "cheeky/cheeky_host.h"
#include "cheeky/cheeky_views.h"
#include "xr.h"

using namespace aceqv;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    failures += !ok;
}

struct FakeState {
    uint32_t starts, ticks, nullTicks, commands, lastRenderer, host;
    void *lastDevice, *lastQueue;
    char lastCommand[256];
};

std::filesystem::path exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

// A real runtime, started in its own process: it hooks NGX and stays resident.
int real_runtime(const std::wstring& runtime, const std::filesystem::path& scratch) {
    CheekyConfig config;
    config.enabled = true;
    config.runtime = runtime;
    config.configDir = (scratch / L"real").wstring();
    expect(cheeky_start(config), "a real runtime accepts the layer's Start");
    const std::string snapshot = cheeky_snapshot();
    expect(snapshot.find("\"settings\"") != std::string::npos, "a real runtime's snapshot has its settings");
    std::printf("real snapshot: %zu bytes, %zu view(s)\n", snapshot.size(), cheeky_views(snapshot).size());
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path scratch = exe_dir() / L"cheeky_host_test";
    std::error_code ec;
    std::filesystem::create_directories(scratch, ec);
    SetEnvironmentVariableW(L"ACE_QV_LOG", (scratch / L"test.log").c_str());
    wchar_t real[MAX_PATH] = {};
    const DWORD realLength = GetEnvironmentVariableW(L"ACE_QV_CHEEKY_RUNTIME", real, MAX_PATH);
    if (argc > 1 && std::strcmp(argv[1], "--real") == 0)
        return realLength ? real_runtime(real, scratch) : 1;

    // [cheeky] keys.
    const std::wstring ini = (scratch / L"test.ini").wstring();
    DeleteFileW(ini.c_str());
    CheekyConfig c = cheeky_config(ini.c_str(), L"C:\\layer");
    expect(!c.enabled, "off unless enabled=1");
    expect(c.configDir == L"C:\\layer\\CheekyFoveatedDLSS", "default config_dir is next to the layer");
    expect(c.runtime == L"C:\\layer\\CheekyFoveatedDLSS\\CheekyFoveatedDLSSRuntime.dll",
           "default runtime is in config_dir");
    expect(c.reportSeconds == 10, "default report every 10 s");
    WritePrivateProfileStringW(L"cheeky", L"enabled", L"1", ini.c_str());
    WritePrivateProfileStringW(L"cheeky", L"runtime", L"..\\cheeky\\rt.dll", ini.c_str());
    WritePrivateProfileStringW(L"cheeky", L"config_dir", L"D:\\cfg", ini.c_str());
    WritePrivateProfileStringW(L"cheeky", L"report_s", L"0", ini.c_str());
    c = cheeky_config(ini.c_str(), L"C:\\layer");
    expect(c.enabled && c.runtime == L"C:\\cheeky\\rt.dll", "a relative runtime is against the layer");
    expect(c.configDir == L"D:\\cfg" && c.reportSeconds == 0, "absolute config_dir and report_s=0 as given");

    // Off or missing: nothing starts, and the frame calls do nothing.
    CheekyConfig off;
    off.runtime = (exe_dir() / L"cheeky_fake_runtime.dll").wstring();
    off.configDir = (scratch / L"cfg").wstring();
    expect(!cheeky_start(off) && !cheeky_running(), "disabled: not started");
    CheekyConfig missing = off;
    missing.enabled = true;
    missing.runtime = (scratch / L"absent.dll").wstring();
    expect(!cheeky_start(missing) && !cheeky_running(), "missing runtime: not started");
    cheeky_frame();
    expect(!cheeky_set("Enabled=1"), "no command without a runtime");

    // The fake runtime.
    CheekyConfig on = off;
    on.enabled = true;
    on.reportSeconds = 0;
    expect(cheeky_start(on) && cheeky_running(), "the fake runtime accepts the layer's Start");
    HMODULE fake = GetModuleHandleW(L"cheeky_fake_runtime.dll");
    const auto state = fake ? reinterpret_cast<const FakeState* (*)()>(GetProcAddress(fake, "Fake_State"))() : nullptr;
    expect(state && state->starts == 1 && state->host == 1, "started once, as the standalone host");
    expect(std::filesystem::is_directory(on.configDir), "config_dir created");
    expect(cheeky_start(on) && state->starts == 1, "a second start reuses the running runtime");
    cheeky_frame();
    expect(state->ticks == 0, "no ticks before the session's device");
    auto* device = reinterpret_cast<ID3D12Device*>(0x1000);
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(0x2000);
    cheeky_session(device, queue);
    for (int i = 0; i < 5; ++i)
        cheeky_frame();
    expect(state->ticks == 6 && state->lastRenderer == 1 && state->lastDevice == device && state->lastQueue == queue,
           "the session hand-over and each frame tick with ACE's D3D12 device and queue");
    expect(cheeky_set("Enabled=1\nCenterPreset=11") &&
               std::strcmp(state->lastCommand, "1\n1\nset\nEnabled=1\nCenterPreset=11") == 0,
           "set: Cheeky's command format, versioned with a request id");
    cheeky_session(nullptr, nullptr);
    cheeky_frame();
    expect(state->nullTicks == 1 && state->ticks == 7, "session end pauses with a null device; no frames after");

    // The snapshot's views and the per-frame report.
    const std::vector<CheekyView> before = cheeky_views(cheeky_snapshot());
    expect(before.size() == 2 && before[0].id == "0xa" && before[0].calls == 6 && before[1].calls == 12,
           "views and evaluation counts read from the snapshot");
    expect(before[1].region == "Center" && before[1].mapping == "Default quad order" && before[1].inWidth == 800 &&
               before[1].outHeight == 1400,
           "a view's region, mapping and sizes");
    cheeky_session(device, queue);
    for (int i = 0; i < 4; ++i)
        cheeky_frame();
    const std::vector<std::string> lines = cheeky_view_report(before, cheeky_views(cheeky_snapshot()), 5);
    expect(lines.size() == 2 && lines[0].find("5 evaluations, 1.00 per frame") != std::string::npos &&
               lines[1].find("10 evaluations, 2.00 per frame") != std::string::npos,
           "evaluations per frame since the last report");
    expect(cheeky_view_report({}, before, 5)[1].find("12 evaluations") != std::string::npos,
           "a view new since the last report counts from zero");
    expect(cheeky_views("{\"settings\":{}}").empty(), "a snapshot without view_details has no views");

    // A D3D11 session (ACC): renderer 0, its device, no queue; each frame ticks the same.
    auto* device11 = reinterpret_cast<ID3D11Device*>(0x3000);
    const uint32_t ticksBefore = state->ticks;
    cheeky_session_d3d11(device11);
    cheeky_frame();
    expect(state->ticks == ticksBefore + 2 && state->lastRenderer == 0 && state->lastDevice == device11 &&
               state->lastQueue == nullptr,
           "a D3D11 session hands over its device as renderer 0, no queue, and each frame ticks it");
    cheeky_session(nullptr, nullptr);
    expect(state->lastDevice == nullptr, "and its end pauses as a D3D12 one does");

    if (realLength) {  // its own process: a real runtime hooks NGX and cannot share this one with the fake
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --real";
        DWORD code = 1;
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 60000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        expect(code == 0, "the real runtime check (--real) passed");
    } else {
        std::printf("ACE_QV_CHEEKY_RUNTIME not set: real runtime not checked\n");
    }

    std::printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}

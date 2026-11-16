// Offline check of the layer's log (src/log.h): info and debug levels, the size limit, the rotation of a large log.
// The level is read once per process, so each case runs in a child of this program (`log_level_test <case> <log>`)
// with its own SIM_QVFR_LOG and SIM_QVFR_LOG_LEVEL.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "log.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// The child: writes what its case asks into SIM_QVFR_LOG.
int child(const char* name) {
    if (std::strcmp(name, "lines") == 0) {
        simqvfr::logf("info line %d", 1);
        simqvfr::logd("debug line %d", 2);
        return 0;
    }
    if (std::strcmp(name, "async") == 0) {  // a session's lines through the writer thread, then at once again
        simqvfr::logf("before %d", 0);
        simqvfr::log_async(true);
        for (int i = 1; i <= 2000; ++i)
            simqvfr::logf("queued %d", i);
        simqvfr::log_async(false);
        simqvfr::logf("after %d", 2001);
        simqvfr::log_async(true);
        simqvfr::logf("left queued at exit %d", 2002);  // written as the process ends (the writer's own end)
        return 0;
    }
    if (std::strcmp(name, "stopping") == 0) {  // another thread logs while the writer stops: its lines stay in order
        simqvfr::log_async(true);
        std::thread other([] {
            for (int i = 1; i <= 3000; ++i)
                simqvfr::logf("other %d", i);
        });
        for (int i = 1; i <= 200; ++i)
            simqvfr::logf("main %d", i);
        simqvfr::log_async(false);
        other.join();
        return 0;
    }
    if (std::strcmp(name, "ending") == 0) {  // the process ends (DllMain's flag) with lines queued: no wait, no hang
        simqvfr::log_async(true);
        for (int i = 1; i <= 2000; ++i)
            simqvfr::logf("queued %d", i);
        // In a game the writer was killed by now; here it is alive: let it write its queue and wait idle first, so the
        // exit's detach doesn't race it touching the writer's state.
        Sleep(300);
        simqvfr::log_process_ending();
        return 0;
    }
    if (std::strcmp(name, "flood") == 0) {
        std::string big(1000, 'x');
        for (int i = 0; i < 40000; ++i)  // ~40 MB asked, over the 32 MB limit
            simqvfr::logf("flood %d %s", i, big.c_str());
        return 0;
    }
    return 3;
}

// Runs this program as a child for `name`, logging into `log` at `level` ("" for none).
int run_child(const char* name, const fs::path& log, const wchar_t* level) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", log.c_str());
    SetEnvironmentVariableW(L"SIM_QVFR_LOG_LEVEL", *level ? level : nullptr);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" child ";
    for (const char* c = name; *c; ++c)
        command += static_cast<wchar_t>(*c);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "child") == 0)
        return child(argv[2]);
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path dir = fs::path(temp) / L"simqvfr-log-level-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const fs::path info = dir / L"info.log", debug = dir / L"debug.log";
    check(run_child("lines", info, L"") == 0 && run_child("lines", debug, L"debug") == 0, "log: the children ran");
    const std::string i = read(info), d = read(debug);
    check(i.find("log: info level") != std::string::npos && i.find("info line 1") != std::string::npos &&
              i.find("debug line 2") == std::string::npos,
          "log: info level by default, its first line says so, debug lines left out");
    check(d.find("log: debug level") != std::string::npos && d.find("info line 1") != std::string::npos &&
              d.find("debug line 2") != std::string::npos,
          "log: SIM_QVFR_LOG_LEVEL=debug writes the debug lines too");

    // log_async: every line written, in order, the writer's queue too, also a line still queued at the process's exit.
    const fs::path async = dir / L"async.log";
    check(run_child("async", async, L"") == 0, "log: the async child ran");
    {
        const std::string a = read(async);
        size_t at = a.find("before 0");
        bool ordered = at != std::string::npos;
        for (int k = 1; k <= 2000 && ordered; ++k) {
            const size_t next = a.find("queued " + std::to_string(k) + "\r\n", at);
            ordered = next != std::string::npos && next > at;
            at = ordered ? next : at;
        }
        const size_t after = a.find("after 2001", at), last = a.find("left queued at exit 2002");
        check(ordered && after != std::string::npos && last != std::string::npos && last > after,
              "log: lines through the writer thread all written, in order; a line queued at the exit too");
    }

    const fs::path stopping = dir / L"stopping.log";
    check(run_child("stopping", stopping, L"") == 0, "log: the stopping child ran");
    {
        const std::string s = read(stopping);
        size_t at = 0;
        bool ordered = true;
        for (int k = 1; k <= 3000 && ordered; ++k) {
            const size_t next = s.find("other " + std::to_string(k) + "\r\n", at);
            ordered = next != std::string::npos;
            at = ordered ? next : at;
        }
        check(ordered, "log: a thread's lines all written in order while the writer stops");
    }
    const fs::path ending = dir / L"ending.log";
    check(run_child("ending", ending, L"") == 0, "log: a process ending with lines queued exits (nothing waited for)");

    const fs::path flood = dir / L"flood.log";
    check(run_child("flood", flood, L"") == 0, "log: the flooding child ran");
    const uintmax_t size = fs::file_size(flood, ec);
    const std::string f = read(flood);
    check(!ec && size <= simqvfr::kLogLimitBytes + 256 && size > simqvfr::kLogLimitBytes - 4096,
          "log: one process writes no more than its limit");
    check(f.size() > 200 && f.find("nothing more is logged from here on", f.size() - 200) != std::string::npos,
          "log: the last line says the limit was reached");

    // A log over the rotation size when opened (the default path, reused by launches): moved to <name>.old.log.
    const fs::path reused = dir / L"sim-qvfr.log", old = dir / L"sim-qvfr.old.log";
    {
        std::ofstream out(reused, std::ios::binary);
        const std::string block(1 << 20, 'o');
        for (uint64_t n = 0; n <= simqvfr::kLogRotateBytes; n += block.size())
            out << block;
    }
    check(run_child("lines", reused, L"") == 0, "log: the child on a large log ran");
    check(fs::file_size(old, ec) > simqvfr::kLogRotateBytes && fs::file_size(reused, ec) < 4096 &&
              read(reused).find("info line 1") != std::string::npos,
          "log: a large log is moved to .old.log and a new one started");
    check(run_child("lines", reused, L"") == 0 && read(reused).find("info line 1") != read(reused).rfind("info line 1"),
          "log: a small one is appended to");

    fs::remove_all(dir, ec);
    std::printf("RESULT: %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}

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

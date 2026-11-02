// Offline check of the start-up hang watch (probe/hang_watch.h): armed with no frame, it logs the
// modules and writes a minidump; a second arm in the same process does nothing; a frame before the
// timeout (checked in a child process, since the watch fires once per process) writes no dump.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "probe/hang_watch.h"
#include "xr.h"

using namespace aceqv;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    failures += !ok;
}

size_t dumps(const std::filesystem::path& dir) {
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        n += e.path().extension() == L".dmp";
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (argc > 1 && std::strcmp(argv[1], "--framed") == 0) {  // child: a frame arrives in time
        hang_watch_arm(1);
        hang_watch_frame();
        Sleep(1600);
        return 0;
    }
    const std::filesystem::path dir = std::filesystem::path(self).parent_path() / L"hang_watch_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::filesystem::path log = dir / L"test.log";
    SetEnvironmentVariableW(L"ACE_QV_LOG", log.c_str());

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + std::wstring(self) + L"\" --framed";
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    expect(dumps(dir) == 0, "a frame before the timeout: no dump");

    hang_watch_arm(1);
    hang_watch_arm(1);  // once per process
    Sleep(3000);
    std::ifstream in(log);
    std::stringstream text;
    text << in.rdbuf();
    expect(dumps(dir) == 1, "no frame: one minidump");
    expect(text.str().find("ACE looks stuck") != std::string::npos, "the hang is logged");
    expect(text.str().find("hang_watch_test.exe") != std::string::npos && text.str().find("ntdll.dll") != std::string::npos,
           "the loaded modules are logged");

    std::printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}

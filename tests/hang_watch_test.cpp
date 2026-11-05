// Offline check of the hang watch (probe/hang_watch.h), each case in a child process (the watch fires once per
// process): frames keep coming, or the session ends, so no dump; frames stop mid-session, so a dump; and in this
// process, armed twice with no frame at all, one dump, with the modules logged.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "probe/hang_watch.h"
#include "xr.h"

using namespace simqvfr;

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

std::string read(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void child(const wchar_t* self, const wchar_t* mode) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + std::wstring(self) + L"\" " + mode;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

}  // namespace

int main(int argc, char** argv) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (argc > 1) {  // children
        hang_watch_arm(1);
        hang_watch_frame();
        if (std::strcmp(argv[1], "--framing") == 0) {  // frames keep coming until the process ends
            for (int i = 0; i < 16; ++i) {
                Sleep(100);
                hang_watch_frame();
            }
            return 0;
        }
        if (std::strcmp(argv[1], "--ended") == 0)  // the session ends after its frame
            hang_watch_disarm();
        Sleep(1600);  // --stall: no frame after the first
        return 0;
    }
    const std::filesystem::path dir = std::filesystem::path(self).parent_path() / L"hang_watch_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::filesystem::path log = dir / L"test.log";
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", log.c_str());

    child(self, L"--framing");
    child(self, L"--ended");
    expect(dumps(dir) == 0, "frames keep coming, or the session ended: no dump");
    child(self, L"--stall");
    expect(dumps(dir) == 1 && read(log).find("mid-session") != std::string::npos,
           "frames stop mid-session: a dump, logged as mid-session");

    hang_watch_arm(1);
    hang_watch_arm(1);  // still one watch thread
    Sleep(3000);
    const std::string text = read(log);
    expect(dumps(dir) == 2, "no frame at all: one minidump");
    expect(text.find("after xrCreateSession; the game looks stuck") != std::string::npos, "the start-up hang is logged");
    expect(text.find("hang_watch_test.exe") != std::string::npos && text.find("ntdll.dll") != std::string::npos,
           "the loaded modules are logged");

    std::printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}

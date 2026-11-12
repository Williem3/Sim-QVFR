// Offline check of the hang watch (probe/hang_watch.h) and its dump helper (probe/hang_dump.h: the launcher,
// built next to this test, standing by from the first arm), each case in a child process (the watch fires once per
// process) with a folder of its own, all at once, each lasting until its watch has answered (not a set time): frames
// keep coming, or the session ends, so no dump; frames stop mid-session, so the helper writes a dump; no helper, or
// one that can't start, so no dump (never one from inside); frames stop while another thread holds the loader lock
// (X-Plane loading a plugin, 2026-11-06), so a dump all the same, and log_modules doesn't wait for the lock; and in
// this process, armed twice with no frame at all, one dump, the modules logged, and dbghelp never loaded here.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "log.h"
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

// Every dump is a whole minidump ("MDMP") with what the in-game one had: threads (3), modules (4), handles (12),
// unloaded modules (14), thread info (17) and thread names (24), the stuck thread's among them.
bool dumps_valid(const std::filesystem::path& dir) {
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != L".dmp")
            continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        uint32_t header[4] = {};
        if (d.size() < sizeof(header) || std::memcmp(d.data(), "MDMP", 4) != 0)
            return false;
        std::memcpy(header, d.data(), sizeof(header));
        bool streams[32] = {};
        for (uint32_t i = 0; i < header[2] && header[3] + 12ull * (i + 1) <= d.size(); ++i) {
            uint32_t type = 0;
            std::memcpy(&type, d.data() + header[3] + 12ull * i, 4);
            if (type < 32)
                streams[type] = true;
        }
        const std::wstring name = L"stuck main thread";
        const bool named =
            d.find(std::string(reinterpret_cast<const char*>(name.data()), name.size() * 2)) != std::string::npos;
        if (!streams[3] || !streams[4] || !streams[12] || !streams[14] || !streams[17] || !streams[24] || !named) {
            std::printf("  %ls: streams 3 %d, 4 %d, 12 %d, 14 %d, 17 %d, 24 %d; thread name %d\n", e.path().filename().c_str(),
                        streams[3], streams[4], streams[12], streams[14], streams[17], streams[24], named);
            return false;
        }
    }
    return true;
}

std::string read(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

size_t count(const std::string& text, const char* what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
        ++n;
    return n;
}

// The longest a case waits for the watch. Its loop starts only once the helper stands by, and starting the helper
// took up to 4.6 s with the whole suite running: Windows Defender checks a freshly linked launcher on its first
// start, every case's helper waiting on that one check (2026-11-11). A set time per case (3 s) failed there.
constexpr DWORD kWaitMs = 20000;

// Waits up to kWaitMs for `log` to have `what` (this process's own log: the watch writes it). True if it came.
bool wait_logged(const std::filesystem::path& log, const char* what) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (read(log).find(what) != std::string::npos)
            return true;
        if (GetTickCount64() - start >= kWaitMs)
            return false;
        Sleep(20);
    }
}

// When the first line with `what` was logged ("HH:MM:SS.mmm [tid] ..."), in seconds of the day; -1 if none.
double logged_at(const std::string& text, const char* what) {
    const size_t at = text.find(what);
    if (at == std::string::npos)
        return -1;
    const size_t line = text.rfind('\n', at);
    unsigned h = 0, m = 0, s = 0, ms = 0;
    if (std::sscanf(text.c_str() + (line == std::string::npos ? 0 : line + 1), "%u:%u:%u.%u", &h, &m, &s, &ms) != 4)
        return -1;
    return h * 3600.0 + m * 60.0 + s + ms / 1000.0;
}

// A case's watch, step by step after its arm (-1: not logged): where the time goes on a busy machine.
void print_timeline(const char* name, const std::string& text) {
    const double arm = logged_at(text, "hang watch: 1 s;");
    const auto after = [&](const char* what) {
        const double at = logged_at(text, what);
        return at < 0 || arm < 0 ? -1.0 : at - arm + (at < arm ? 86400 : 0);
    };
    std::printf("  (%s, after the arm: helper standing by %.2f s, watch fired %.2f s, dump written %.2f s, "
                "its answer logged %.2f s)\n",
                name, after("dump helper standing by"), after("the game looks stuck"),
                after("written from outside the game"), after("hang watch: minidump"));
}

// A child case started in a folder of its own, for its log (SIM_QVFR_LOG, inherited) and any dump, so the cases
// run side by side.
HANDLE start_child(const wchar_t* self, const wchar_t* mode, const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", (dir / L"test.log").c_str());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + std::wstring(self) + L"\" " + mode;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// The child's exit code; -1 if it didn't start or finish in time.
int finish_child(HANDLE process) {
    if (!process)
        return -1;
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(process, 30000) == WAIT_OBJECT_0)
        GetExitCodeProcess(process, &code);
    else
        TerminateProcess(process, static_cast<UINT>(-1));
    CloseHandle(process);
    return static_cast<int>(code);
}

// ntdll's loader lock, as a thread loading a DLL holds it.
using LockLoader = LONG(NTAPI*)(ULONG flags, ULONG* state, ULONG_PTR* cookie);
using UnlockLoader = LONG(NTAPI*)(ULONG flags, ULONG_PTR cookie);

struct Probe {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    void (*work)() = nullptr;
};

DWORD WINAPI probe_thread(void* p) {
    auto* probe = static_cast<Probe*>(p);
    SetEvent(probe->ready);  // past its thread start-up, which needs the loader lock
    WaitForSingleObject(probe->go, INFINITE);
    probe->work();
    SetEvent(probe->done);
    return 0;
}

HANDLE start_probe(Probe& probe, void (*work)()) {
    probe.work = work;
    HANDLE thread = CreateThread(nullptr, 0, probe_thread, &probe, 0, nullptr);
    WaitForSingleObject(probe.ready, INFINITE);
    return thread;
}

// The hang watch fires while this thread holds the loader lock. 0: the helper wrote a dump meanwhile and
// log_modules ran; 1: no dump while the lock was held; 2: log_modules waited for the lock; 3: no LdrLockLoaderLock;
// 4: the helper never stood by.
int stall_holding_loader_lock(const std::filesystem::path& dir) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto lock = reinterpret_cast<LockLoader>(GetProcAddress(ntdll, "LdrLockLoaderLock"));
    const auto unlock = reinterpret_cast<UnlockLoader>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"));
    if (!lock || !unlock)
        return 3;
    const size_t before = dumps(dir);
    const std::filesystem::path log(log_file());
    const size_t standing = count(read(log), "dump helper standing by");
    hang_watch_arm(1);
    // In a game the helper stands by long before the hang, so the lock is taken only once it does: the watch
    // thread's CreateProcessW, the first in this process, loads sechost.dll under the loader lock (2026-11-06).
    // Frames keep coming meanwhile, so the watch fires only with the lock held.
    bool ready = false;
    for (DWORD i = 0; i < kWaitMs / 10 && !ready; ++i) {
        hang_watch_frame();
        ready = count(read(log), "dump helper standing by") > standing;
        if (!ready)
            Sleep(10);
    }
    if (!ready) {
        std::printf("  (the dump helper never stood by)\n");
        return 4;
    }
    Probe modules, name;
    HANDLE modulesThread = start_probe(modules, [] { log_modules(); });
    HANDLE nameThread = start_probe(name, [] {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
    });
    hang_watch_frame();
    ULONG_PTR cookie = 0;
    lock(0, nullptr, &cookie);
    // The lock held until the watch has the helper's answer (logged once the helper exits, its file closed): the
    // watch fires after 1 s, and the dump took up to 0.6 s with the whole suite running (2026-11-11).
    wait_logged(log, "hang watch: minidump");
    const bool dumped = dumps(dir) > before;
    SetEvent(modules.go);
    SetEvent(name.go);
    const bool modulesFree = WaitForSingleObject(modules.done, 3000) == WAIT_OBJECT_0;
    const bool nameWaits = WaitForSingleObject(name.done, 500) == WAIT_TIMEOUT;
    unlock(0, cookie);
    WaitForSingleObject(modulesThread, INFINITE);
    WaitForSingleObject(nameThread, INFINITE);
    CloseHandle(modulesThread);
    CloseHandle(nameThread);
    std::printf("  (with the loader lock held, GetModuleFileNameW %s)\n", nameWaits ? "waits for it" : "doesn't wait");
    return !dumped ? 1 : !modulesFree ? 2 : 0;
}

}  // namespace

int SIM_QVFR_TEST_MAIN(int argc, char** argv) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SetThreadDescription(GetCurrentThread(), L"stuck main thread");  // in every dump's thread names
    if (HMODULE gone = LoadLibraryExW(L"msimg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        FreeLibrary(gone);  // an unloaded module, for the dump's list
    if (argc > 1) {  // children
        if (std::strcmp(argv[1], "--loader-lock") == 0)
            return stall_holding_loader_lock(std::filesystem::path(log_directory()));
        if (std::strcmp(argv[1], "--no-helper") == 0)
            hang_watch_set_helper(L"C:\\no-such-folder\\sim-qvfr-launcher.exe");
        if (std::strcmp(argv[1], "--bad-helper") == 0) {  // there, but not a program
            log_line("hang watch test: this log as the helper");  // the case's own folder starts empty
            hang_watch_set_helper(log_file().c_str());
        }
        const std::filesystem::path log(log_file());
        hang_watch_arm(1);
        hang_watch_frame();
        // Each case lasts as long as the watch needs, not a set time: the watch counts only once its helper
        // stands by (kWaitMs).
        if (std::strcmp(argv[1], "--framing") == 0) {  // frames keep coming until the process ends
            for (DWORD i = 0; i < kWaitMs / 100 && read(log).find("dump helper standing by") == std::string::npos;
                 ++i) {
                Sleep(100);
                hang_watch_frame();
            }
            for (int i = 0; i < 16; ++i) {  // past the limit, the watch counting
                Sleep(100);
                hang_watch_frame();
            }
            return 0;
        }
        if (std::strcmp(argv[1], "--ended") == 0) {  // the session ends after its frame
            hang_watch_disarm();
            wait_logged(log, "dump helper standing by");
            Sleep(2000);  // twice the limit, the watch counting
            return 0;
        }
        // --stall, --no-helper, --bad-helper: no frame after the first, until the watch has answered: the helper's
        // exit, or the modules from here.
        wait_logged(log, std::strcmp(argv[1], "--stall") == 0 ? "hang watch: minidump" : "modules from here");
        return 0;
    }
    const std::filesystem::path dir = std::filesystem::path(self).parent_path() / L"hang_watch_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // Every case at once, each in its own folder: the children, then this process's own case.
    const wchar_t* const modes[] = {L"framing", L"ended", L"stall", L"no-helper", L"bad-helper", L"loader-lock"};
    HANDLE children[std::size(modes)] = {};
    for (size_t i = 0; i < std::size(modes); ++i)
        children[i] = start_child(self, (L"--" + std::wstring(modes[i])).c_str(), dir / modes[i]);
    const std::filesystem::path own = dir / L"main";
    std::filesystem::create_directories(own);
    SetEnvironmentVariableW(L"SIM_QVFR_LOG", (own / L"test.log").c_str());  // before this process's first log line
    hang_watch_arm(1);
    hang_watch_arm(1);  // still one watch thread
    wait_logged(own / L"test.log", "hang watch: minidump");  // the watch's answer, the helper gone
    int exits[std::size(modes)] = {};
    for (size_t i = 0; i < std::size(modes); ++i)
        exits[i] = finish_child(children[i]);
    const auto folder = [&](const wchar_t* mode) { return dir / mode; };
    const auto log_of = [&](const wchar_t* mode) { return read(folder(mode) / L"test.log"); };
    print_timeline("stall", log_of(L"stall"));
    print_timeline("loader lock", log_of(L"loader-lock"));
    print_timeline("no frame at all", read(own / L"test.log"));

    expect(dumps(folder(L"framing")) == 0 && dumps(folder(L"ended")) == 0 &&
               count(log_of(L"framing"), "dump helper standing by") == 1 &&
               count(log_of(L"ended"), "dump helper standing by") == 1,
           "frames keep coming, or the session ended, the watch counting: no dump");
    std::string text = log_of(L"stall");
    expect(dumps(folder(L"stall")) == 1 && text.find("mid-session") != std::string::npos,
           "frames stop mid-session: a dump, logged as mid-session");
    expect(count(text, "written (helper exit 0") == 1 && count(text, "written from outside the game") == 1,
           "the helper wrote it and logged it");
    expect(text.find(std::filesystem::path(self).filename().string()) != std::string::npos &&
               text.find("ntdll.dll") != std::string::npos,
           "the helper logged the stuck process's modules");

    expect(dumps(folder(L"no-helper")) == 0 && dumps(folder(L"bad-helper")) == 0,
           "no helper, or one that can't start: no dump");
    expect(log_of(L"no-helper").find("modules from here, no minidump") != std::string::npos,
           "no helper: logged, modules from here");
    text = log_of(L"bad-helper");
    expect(text.find("cannot start") != std::string::npos && text.find("; no minidump") != std::string::npos,
           "a helper that can't start: logged");

    expect(exits[5] == 0, "frames stop while a thread holds the loader lock: the helper dumps, log_modules doesn't wait");
    expect(dumps(folder(L"loader-lock")) == 1, "  and that dump is there");

    text = read(own / L"test.log");
    expect(dumps(own) == 1, "no frame at all: one minidump");
    expect(text.find("after xrCreateSession; the game looks stuck") != std::string::npos, "the start-up hang is logged");
    bool valid = dumps_valid(own);
    for (const wchar_t* mode : modes)
        valid = dumps_valid(folder(mode)) && valid;
    expect(valid, "every dump is a whole minidump");
    expect(!GetModuleHandleW(L"dbghelp.dll"), "dbghelp never loaded in the stuck process");

    std::printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}

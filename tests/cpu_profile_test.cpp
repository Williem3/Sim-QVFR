// Offline check of the CPU sampling profiler (probe/cpu_profile.h): a thread spinning in a known function is
// profiled for a second; the report must put that function on top by self time, its caller in the total
// time, and the thread must keep running (never left paused).
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <windows.h>

#include "probe/cpu_profile.h"

using namespace simqvfr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

std::atomic<bool> stop{false};
std::atomic<uint64_t> spins{0};

__declspec(noinline) uint64_t busy_leaf(uint64_t x) {
    for (int i = 0; i < 100000; ++i)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    return x;
}

__declspec(noinline) void busy_outer() {
    uint64_t x = 1;
    while (!stop) {
        x = busy_leaf(x);
        spins.fetch_add(1, std::memory_order_relaxed);
    }
    std::printf("     (%llu)\n", static_cast<unsigned long long>(x & 1));
}

// Works, then sleeps, over and over: samples land in the sleep (blocked) and in the work.
__declspec(noinline) void work_then_sleep() {
    uint64_t x = 3;
    while (!stop) {
        for (int i = 0; i < 20; ++i)
            x = busy_leaf(x);
        Sleep(1);
    }
    std::printf("     (%llu)\n", static_cast<unsigned long long>(x & 1));
}

// A game loop: a frame every ~5 ms, every 8th frame 30 ms longer in a function of its own (the spike), marked
// at each frame's end as the layer does at xrEndFrame.
__declspec(noinline) uint64_t spike_work(uint64_t x) {
    const ULONGLONG until = GetTickCount64() + 30;
    while (GetTickCount64() < until)
        x = busy_leaf(x);
    return x;
}

__declspec(noinline) void frame_loop() {
    uint64_t x = 5;
    for (int frame = 1; !stop; ++frame) {
        x = busy_leaf(x);
        if (frame % 8 == 0)
            x = spike_work(x);
        Sleep(4);
        cpu_profile_mark_frame();
    }
    std::printf("     (%llu)\n", static_cast<unsigned long long>(x & 1));
}

// How the report names a function (at its start, `address`) without symbols: module+offset.
std::string start_offset(void* address) {
    char text[64];
    std::snprintf(text, sizeof(text), "+0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<uint64_t>(address) -
                                                  reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))));
    return text;
}

}  // namespace

int main() {
    DWORD id = 0;
    std::thread worker([&id] {
        id = GetCurrentThreadId();
        busy_outer();
    });
    DWORD sleeperId = 0;
    std::thread sleeper([&sleeperId] {
        sleeperId = GetCurrentThreadId();
        work_then_sleep();
    });
    DWORD framesId = 0;
    std::thread frames([&framesId] {
        framesId = GetCurrentThreadId();
        frame_loop();
    });
    while (!id || !sleeperId || !framesId)
        Sleep(1);
    cpu_profile_mark_frame();  // no capture running: nothing kept
    const std::wstring path = (std::filesystem::temp_directory_path() / L"sim-qvfr-cpu-profile-test.txt").wstring();
    DeleteFileW(path.c_str());
    check(cpu_profile_start({{id, GetCurrentThreadId(), sleeperId, framesId}, 2, path}), "a capture starts (the calling thread profiled too)");
    check(!cpu_profile_start({{id}, 1, path}), "only one capture at a time");
    cpu_profile_wait();
    const uint64_t before = spins.load();
    Sleep(50);
    check(spins.load() > before, "the profiled thread runs on after the capture (never left paused)");
    stop = true;
    worker.join();
    sleeper.join();
    frames.join();

    std::wifstream in(path);
    std::wstringstream text;
    text << in.rdbuf();
    const std::wstring report = text.str();
    std::string utf8;  // ASCII is all that is searched for
    for (const wchar_t c : report)
        utf8 += static_cast<char>(c < 128 ? c : '?');
    check(cpu_profile_last_report() == path && !report.empty(), "the report is written where asked");
    const std::string leaf = start_offset(reinterpret_cast<void*>(&busy_leaf)),
                      outer = start_offset(reinterpret_cast<void*>(&busy_outer));
    const size_t self = utf8.find("Self time:"), total = utf8.find("Total time:");
    const std::string selfPart = self == std::string::npos ? "" : utf8.substr(self, total - self);
    const std::string totalPart = total == std::string::npos ? "" : utf8.substr(total);
    const auto first_line = [](const std::string& part) {
        const size_t a = part.find('\n');
        return a == std::string::npos ? std::string() : part.substr(a + 1, part.find('\n', a + 1) - a - 1);
    };
    std::printf("     top self: %s\n", first_line(selfPart).c_str());
    check(first_line(selfPart).find("busy_leaf") != std::string::npos || first_line(selfPart).find(leaf) != std::string::npos,
          "the spinning function is on top by self time");
    check(totalPart.find("busy_outer") != std::string::npos || totalPart.find(outer) != std::string::npos,
          "its caller is in the total time (the stack was walked)");
    check(utf8.find("% of a core") != std::string::npos, "the thread's load is reported");
    check(utf8.find("Thread " + std::to_string(GetCurrentThreadId())) != std::string::npos,
          "the thread that started the capture is in it (the layer starts captures from ACE's render thread)");
    {
        // The sleeper: samples in its sleep count as blocked, and its work still comes out on top.
        const size_t at = utf8.find("Thread " + std::to_string(sleeperId));
        const std::string part = at == std::string::npos ? "" : utf8.substr(at, utf8.find("\nThread ", at + 1) - at);
        unsigned long long running = 0, blocked = 0;
        const size_t counts = part.find("samples: ");
        if (counts != std::string::npos)
            sscanf_s(part.c_str() + counts, "samples: %llu running, %llu blocked", &running, &blocked);
        const size_t selfAt = part.find("Self time:");
        const std::string top = selfAt == std::string::npos ? "" : first_line(part.substr(selfAt));
        std::printf("     sleeper: %llu running, %llu blocked; top self: %s\n", running, blocked, top.c_str());
        check(blocked > 0 && running > 0, "work then sleep: the sleeps are counted apart as blocked");
        check(top.find("busy_leaf") != std::string::npos || top.find(leaf) != std::string::npos,
              "work then sleep: its work, not the sleep, is on top by self time");
    }
    check(utf8.find("ntdll.dll!") != std::string::npos || utf8.find("KERNEL32.DLL!") != std::string::npos ||
              utf8.find("KERNELBASE.dll!") != std::string::npos,
          "system functions are named from their exports (the waiting main thread is in one)");
    {
        // The slow frames: found, and the spike's function ranked first among the frame thread's extra time.
        const size_t at = utf8.find("\nSlow frames:");
        const std::string part = at == std::string::npos ? "" : utf8.substr(at);
        std::printf("     %s\n", part.substr(1, part.find('\n', 1) - 1).c_str());
        check(at != std::string::npos && part.find("Slow: longer than") != std::string::npos, "slow frames are found from the marks");
        const size_t thread = part.find("Thread " + std::to_string(framesId));
        const std::string section = thread == std::string::npos ? "" : part.substr(thread, part.find("\n  Thread ", thread + 1) - thread);
        const size_t running = section.find("Running, total:");
        const std::string topExtra = running == std::string::npos ? "" : first_line(section.substr(running));
        std::printf("     frame thread's top extra: %s\n", topExtra.c_str());
        const std::string spike = start_offset(reinterpret_cast<void*>(&spike_work));
        check(section.find("spike_work") != std::string::npos || section.find(spike) != std::string::npos,
              "the spike's function is in the slow frames' extra time");
        const size_t slowSelf = section.find("Running, self:");
        const std::string slowSelfPart = slowSelf == std::string::npos ? "" : section.substr(slowSelf);
        check(first_line(slowSelfPart).find("busy_leaf") != std::string::npos || first_line(slowSelfPart).find(leaf) != std::string::npos,
              "the slow frames' extra self time is the spike's leaf");
    }
    DeleteFileW(path.c_str());
    std::puts(failures ? "RESULT: FAIL" : "RESULT: pass");
    return failures ? 1 : 0;
}

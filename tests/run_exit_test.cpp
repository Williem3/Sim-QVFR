// Offline check of request_game_exit (probe/run_exit.h): this process's visible top-level window gets WM_CLOSE, as from
// its close button; a hidden one and an owned one don't.
#include <windows.h>

#include <cstdio>

#include "probe/run_exit.h"

namespace {

int closes[3] = {};
HWND windows[3] = {};

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        for (int i = 0; i < 3; ++i)
            if (hwnd == windows[i])
                ++closes[i];
        return 0;  // a game would quit here; the test only counts
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int SIM_QVFR_TEST_MAIN() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SimQvfrRunExitTest";
    RegisterClassW(&wc);
    const auto make = [&](DWORD style, HWND owner) {
        return CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"run exit test", style, -32000, -32000, 64, 64, owner,
                               nullptr, wc.hInstance, nullptr);
    };
    windows[0] = make(WS_POPUP | WS_VISIBLE, nullptr);     // the game's window
    windows[1] = make(WS_POPUP, nullptr);                  // hidden
    windows[2] = make(WS_POPUP | WS_VISIBLE, windows[0]);  // owned (a dialog of the game's)
    const int asked = simqvfr::request_game_exit("run exit test");
    MSG msg;
    for (int i = 0; i < 50 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i)
        DispatchMessageW(&msg);
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };
    check(asked == 1, "one window asked");
    check(closes[0] == 1, "the visible top-level window got WM_CLOSE");
    check(closes[1] == 0, "the hidden window didn't");
    check(closes[2] == 0, "the owned window didn't");
    for (HWND h : windows)
        DestroyWindow(h);
    std::printf(failures ? "run_exit_test: %d failure(s)\n" : "run_exit_test: ok\n", failures);
    return failures ? 1 : 0;
}

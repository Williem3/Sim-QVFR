#include "run_exit.h"

#include <windows.h>

#include <vector>

#include "../log.h"

namespace simqvfr {

int request_game_exit(const char* why) {
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER))
                reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    for (HWND hwnd : windows)
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    logf("run exit: %s: asked the game to close (%zu window(s), as their close button would)", why, windows.size());
    return static_cast<int>(windows.size());
}

}  // namespace simqvfr

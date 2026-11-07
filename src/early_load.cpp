// The layer DLL's entry. Normally the OpenXR loader loads the layer at xrCreateInstance and nothing happens here. For
// a game the launcher loads it into before the game runs (GameInfo::earlyLoad: X-Plane, which creates its shaders
// before it starts OpenXR), SIM_QVFR_EARLY=1 is set for the launch and the game is set up now, while its main thread
// is still suspended (games::early_load). The OpenXR loader later gets this same, already loaded module.
#include <windows.h>

#include "games/adapter.h"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t early[4] = {};
        if (GetEnvironmentVariableW(L"SIM_QVFR_EARLY", early, 4) == 1 && early[0] == L'1') {
            SetEnvironmentVariableW(L"SIM_QVFR_EARLY", nullptr);  // once: not for child processes
            simqvfr::games::early_load();
        }
    }
    return TRUE;
}

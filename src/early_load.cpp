// The layer DLL's entry. Normally the OpenXR loader loads the layer at xrCreateInstance and nothing happens here. For
// a game the layer must set up before it starts OpenXR (GameInfo::earlyLoad: X-Plane, which creates its shaders
// first), the launcher sets SIM_QVFR_EARLY=1 and enables the DLL as a Vulkan layer (vk_early_layer.cpp): the Vulkan
// loader loads it at the game's first vkCreateInstance, before any shader module exists, and the game is set up now
// (games::early_load). The OpenXR loader later gets this same, already loaded module.
#include <windows.h>

#include "games/adapter.h"

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t early[4] = {};
        if (GetEnvironmentVariableW(L"SIM_QVFR_EARLY", early, 4) == 1 && early[0] == L'1') {
            SetEnvironmentVariableW(L"SIM_QVFR_EARLY", nullptr);  // once: not for child processes
            // The game's patches point into this module, and the Vulkan loader frees its layers at vkDestroyInstance
            // (a game may make and destroy an instance before the one it keeps): pinned, it stays for the process.
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(self), &pinned);
            simqvfr::games::early_load();
        }
    }
    return TRUE;
}

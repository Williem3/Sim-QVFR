// The layer DLL's entry. Normally the OpenXR loader loads the layer at xrCreateInstance and nothing happens here. For
// a game the layer must set up before it starts OpenXR (GameInfo::earlyLoad: X-Plane, which creates its shaders
// first), the launcher sets SIM_QVFR_EARLY=1 and enables the DLL as a Vulkan layer (vk_early_layer.cpp): the Vulkan
// loader loads it at the game's first vkCreateInstance, before any shader module exists. DllMain only notes it (and
// pins the module): the game is set up (games::early_load) from the layer's vkCreateInstance, which the loader calls
// right after, outside the loader lock, so the game's mod DLL can be loaded then (LoadLibrary under the loader lock
// can deadlock). The OpenXR loader later gets this same, already loaded module.
#include <windows.h>

#include <atomic>

#include "games/adapter.h"
#include "log.h"

namespace simqvfr::layer {
extern std::atomic<bool> processTerminating;  // layer/state.h
}

namespace {
std::atomic<bool> earlyPending{false};  // SIM_QVFR_EARLY=1 at load: the early start still to run
}

// The early start, once, if this load was an early one (vk_early_layer.cpp's vkCreateInstance).
void sim_qvfr_run_early_load() {
    if (earlyPending.exchange(false))
        simqvfr::games::early_load();
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    // Process exit: the CRT destroys this DLL's globals right after this returns, with the other threads already gone.
    if (reason == DLL_PROCESS_DETACH && reserved) {
        simqvfr::layer::processTerminating.store(true);
        simqvfr::log_process_ending();
    }
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t early[4] = {};
        if (GetEnvironmentVariableW(L"SIM_QVFR_EARLY", early, 4) == 1 && early[0] == L'1') {
            SetEnvironmentVariableW(L"SIM_QVFR_EARLY", nullptr);  // once: not for child processes
            // The game's patches point into this module, and the Vulkan loader frees its layers at vkDestroyInstance
            // (a game may make and destroy an instance before the one it keeps): pinned, it stays for the process.
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(self), &pinned);
            earlyPending.store(true);
        }
    }
    return TRUE;
}

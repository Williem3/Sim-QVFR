#include "host_services.h"

#include <windows.h>

#include <MinHook.h>

#include <string>

#include "../dev_build.h"
#include "../engine/code_patch.h"
#include "../log.h"
#include "../probe/run_exit.h"

namespace simqvfr::mod_api {

namespace {

int32_t b(bool v) { return v ? 1 : 0; }

void host_log(int32_t level, const char* message) {
    if (!message)
        return;
    if (level == SQVFR_LOG_DEBUG) {
        if (log_debug())
            log_line(message);
        return;
    }
    log_line(message);
}

// The launch's ini: the launcher's run copy (SIM_QVFR_INI), read once.
const wchar_t* host_ini_path() {
    static const std::wstring ini = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"SIM_QVFR_INI", path, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(path) : std::wstring();
    }();
    return ini.c_str();
}

const wchar_t* host_captures_dir() {
    static const std::wstring dir = captures_directory();
    return dir.c_str();
}

int32_t host_hook(void* target, void* detour, void** original) {
    if (!target || !detour || !original || !minhook_ready("mod hook"))
        return 0;
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
        return 0;
    const MH_STATUS enabled = MH_EnableHook(target);
    return b(enabled == MH_OK || enabled == MH_ERROR_ENABLED);
}

int32_t host_unhook(void* target) {
    if (!target)
        return 0;
    const MH_STATUS s = MH_DisableHook(target);
    return b(s == MH_OK || s == MH_ERROR_DISABLED);
}

uintptr_t host_module_base() { return module_base(); }

int32_t host_code_matches(uintptr_t rva, const uint8_t* anchor, size_t size) {
    return b(anchor && code_matches(rva, anchor, size));
}

int32_t host_code_write(uintptr_t rva, const void* bytes, size_t size) { return b(bytes && code_write(rva, bytes, size)); }

int32_t host_code_redirect(uintptr_t rva, const uint8_t* anchor, size_t size, const uint8_t* stub, size_t stubSize) {
    return b(anchor && stub && code_redirect(rva, anchor, size, stub, stubSize));
}

uintptr_t host_vtable_slot(uintptr_t slotRva) { return vtable_slot(slotRva); }

int32_t host_vtable_slot_replace(uintptr_t slotRva, uintptr_t expectedRva, const void* replacement) {
    return b(replacement && vtable_slot_replace(slotRva, expectedRva, replacement));
}

int32_t host_readable(const void* address, size_t size) { return b(address && readable(address, size)); }

int32_t host_request_game_exit(const char* why) { return request_game_exit(why ? why : "a mod asked"); }

}  // namespace

const SqvfrHost& host_table() {
    static const SqvfrHost host = [] {
        SqvfrHost h = {};
        h.size = sizeof(SqvfrHost);
        h.abi = SQVFR_ABI_VERSION;
        h.flags = kDevBuild ? SQVFR_HOST_DEV_BUILD : 0u;
        h.gameId = "";  // PluginAdapter::load names the game in its own copy
        h.log = host_log;
        h.ini_path = host_ini_path;
        h.captures_dir = host_captures_dir;
        h.hook = host_hook;
        h.unhook = host_unhook;
        h.module_base = host_module_base;
        h.code_matches = host_code_matches;
        h.code_write = host_code_write;
        h.code_redirect = host_code_redirect;
        h.vtable_slot = host_vtable_slot;
        h.vtable_slot_replace = host_vtable_slot_replace;
        h.readable = host_readable;
        h.request_game_exit = host_request_game_exit;
        return h;
    }();
    return host;
}

}  // namespace simqvfr::mod_api

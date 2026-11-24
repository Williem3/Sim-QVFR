#include "mod_loader.h"

#include <windows.h>

#include <filesystem>

#include "../module_path.h"

namespace simqvfr::mod_api {

std::wstring mods_folder() {
    const std::filesystem::path core = module_folder(reinterpret_cast<const void*>(&mods_folder));
    const bool inLayer = _wcsicmp(core.filename().c_str(), L"layer") == 0;
    return ((inLayer ? core.parent_path() : core) / L"mods").wstring();
}

std::wstring mod_dll_path(const wchar_t* folder, const wchar_t* dll) {
    return (std::filesystem::path(mods_folder()) / folder / dll).wstring();
}

namespace {

std::string narrow(const std::wstring& w) {
    std::string out;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

std::unique_ptr<PluginAdapter> load_mod_dll(const std::wstring& path, const games::GameInfo* game, std::string& why,
                                           const std::string& modId) {
    if (!std::filesystem::path(path).is_absolute()) {
        why = "not a full path: " + narrow(path);
        return nullptr;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        why = "no mod at " + narrow(path);
        return nullptr;
    }
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        why = "the mod won't load (error " + std::to_string(GetLastError()) + "): " + narrow(path);
        return nullptr;
    }
    const auto entry = reinterpret_cast<SqvfrModEntry>(GetProcAddress(module, SQVFR_MOD_ENTRY_NAME));
    if (!entry) {
        why = std::string("the mod has no ") + SQVFR_MOD_ENTRY_NAME + " export: " + narrow(path);
        return nullptr;
    }
    return PluginAdapter::load(game, entry, why, modId);
}

std::unique_ptr<PluginAdapter> load_checked_mod(const std::wstring& path, const std::wstring& id, const std::wstring& ini,
                                                const games::GameInfo* game, bool firstPartyPath, bool onlineLaunch,
                                                TrustVerdict& verdict, std::string& why) {
    verdict = {};
    if (!std::filesystem::path(path).is_absolute()) {
        why = "not a full path: " + narrow(path);
        verdict.why = why;
        return nullptr;
    }
    // Held open, others only reading, from the check until LoadLibraryExW has mapped it: the file checked is the file
    // loaded (no write, rename or delete in between).
    HANDLE hold = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (hold == INVALID_HANDLE_VALUE) {
        why = (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? "no mod at "
                                                                                                 : "the mod can't be opened: ") +
              narrow(path);
        verdict.why = why;
        return nullptr;
    }
    static const bool coreOfficial = module_officially_signed(reinterpret_cast<const void*>(&mods_folder));
    TrustInput in;
    in.mod = read_mod_facts(path);
    in.approval = read_mod_approval(ini, id);
    in.coreOfficial = coreOfficial;
    // A first-party id with a file the package didn't ship (a zip that claims "scs") is judged as third-party.
    in.firstPartyPath = firstPartyPath && shipped_by_package(path, in.mod.sha256);
    in.onlineLaunch = onlineLaunch;
    verdict = decide_mod_trust(in);
    std::unique_ptr<PluginAdapter> mod;
    if (verdict.trust == ModTrust::Refused)
        why = verdict.why;
    else
        mod = load_mod_dll(path, game, why, narrow(id));
    CloseHandle(hold);
    return mod;
}

}  // namespace simqvfr::mod_api

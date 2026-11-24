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

std::unique_ptr<PluginAdapter> load_mod_dll(const std::wstring& path, const games::GameInfo* game, std::string& why) {
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
    return PluginAdapter::load(game, entry, why);
}

}  // namespace simqvfr::mod_api

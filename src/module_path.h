// The folder of the module (DLL or exe) that holds an address: the layer's DLL folder from inside the layer.
// Header-only and windows.h alone.
#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

namespace simqvfr {

// The folder of the module `addressInModule` is in (a function or static of that module). MAX_PATH, as before.
inline std::wstring module_folder(const void* addressInModule) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addressInModule), &module);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().wstring();
}

}  // namespace simqvfr

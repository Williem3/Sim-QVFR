#include "leftovers.h"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <fstream>

#include "../games/game.h"

namespace fs = std::filesystem;

namespace simqvfr::launcher {
namespace {

// A link, junction or other reparse point (never followed or removed).
bool reparse_point(const fs::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// "<game exe without .exe>-<12 lowercase hex>-image.dump", the game one of Sim-QVFR's.
bool leftover_name(const std::wstring& name) {
    static constexpr wchar_t kSuffix[] = L"-image.dump";
    const size_t suffix = std::wcslen(kSuffix), hash = 12;
    if (name.size() < 1 + 1 + hash + suffix || name.compare(name.size() - suffix, suffix, kSuffix) != 0)
        return false;
    const size_t hashAt = name.size() - suffix - hash;
    if (name[hashAt - 1] != L'-')
        return false;
    for (size_t i = hashAt; i < hashAt + hash; ++i)
        if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f')))
            return false;
    const std::wstring stem = name.substr(0, hashAt - 1);
    std::size_t count = 0;
    const games::GameInfo* const* list = games::all_games(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::wstring exe = list[i]->exeName;  // "Le Mans Ultimate.exe"; the old name took the file's own case
        if (exe.size() > 4 && _wcsicmp(exe.c_str() + exe.size() - 4, L".exe") == 0 &&
            _wcsicmp(exe.substr(0, exe.size() - 4).c_str(), stem.c_str()) == 0)
            return true;
    }
    return false;
}

// A Windows program: "MZ", and "PE\0\0" at the offset the DOS header gives.
bool windows_program(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    char dos[64] = {};
    if (!in.read(dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z')
        return false;
    uint32_t peAt = 0;
    std::memcpy(&peAt, dos + 0x3c, sizeof(peAt));
    if (peAt < sizeof(dos) || peAt > (1u << 20))
        return false;
    char pe[4] = {};
    in.seekg(peAt);
    return in.read(pe, sizeof(pe)) && pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0;
}

}  // namespace

bool is_leftover(const fs::path& file) {
    std::error_code ec;
    const fs::file_status s = fs::symlink_status(file, ec);
    return !ec && fs::is_regular_file(s) && !reparse_point(file) && leftover_name(file.filename().wstring()) &&
           windows_program(file);
}

Leftovers remove_leftovers(const std::vector<std::wstring>& folders) {
    Leftovers r;
    for (const std::wstring& folder : folders) {
        std::error_code ec;
        if (!fs::is_directory(fs::symlink_status(folder, ec)) || ec || reparse_point(folder))
            continue;
        std::vector<fs::path> found;  // decided first, removed after: the listing isn't changed while it is read
        for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (is_leftover(it->path()))
                found.push_back(it->path());
        for (const fs::path& file : found) {
            std::error_code e;
            const uintmax_t size = fs::file_size(file, e);
            if (!e && fs::remove(file, e) && !e) {
                ++r.removed;
                r.bytes += size;
                r.files.push_back(file.wstring());
            }
        }
    }
    return r;
}

}  // namespace simqvfr::launcher

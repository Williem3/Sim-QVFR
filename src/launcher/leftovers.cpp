#include "leftovers.h"

#include <cwchar>
#include <filesystem>

namespace fs = std::filesystem;

namespace simqvfr::launcher {

Leftovers remove_leftovers(const std::wstring& logsFolder) {
    Leftovers r;
    static constexpr wchar_t kSuffix[] = L"-image.dump";
    const size_t n = std::wcslen(kSuffix);
    std::error_code ec;
    for (auto it = fs::directory_iterator(logsFolder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e;
        const std::wstring name = it->path().filename().wstring();
        if (!it->is_regular_file(e) || name.size() <= n || _wcsicmp(name.c_str() + name.size() - n, kSuffix) != 0)
            continue;
        const uintmax_t size = it->file_size(e);
        if (fs::remove(it->path(), e) && !e) {
            ++r.removed;
            r.bytes += size;
        }
    }
    return r;
}

}  // namespace simqvfr::launcher

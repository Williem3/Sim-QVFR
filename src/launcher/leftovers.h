// Files older Sim-QVFR versions left behind that players never asked for: from 1.0.2-beta to 1.1.0-beta, the layer wrote
// a copy of the game's program into its log folder (~30 MB for Le Mans Ultimate, again after each game update). A
// launcher built without the local analysis tools (probe/local_analysis.h) removes them at each launch.
//
// Nothing else is ever removed. A file goes only if ALL of these hold (is_leftover):
//   - it sits directly in one of the folders the old layer wrote to: <launcher folder>\logs (launches from the
//     launcher) or <launcher folder>\layer\logs (the layer started without it); never a subfolder, and not in a folder
//     that is a link or junction;
//   - it is a plain file, not a link or junction;
//   - its name is exactly <exe name of one of Sim-QVFR's games without .exe>-<12 lowercase hex digits>-image.dump, as
//     the old layer named it ("Le Mans Ultimate-66942337813b-image.dump");
//   - its content is a Windows program: "MZ" at the start and "PE\0\0" where the header says, as the old copy was.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace simqvfr::launcher {

struct Leftovers {
    int removed = 0;
    uint64_t bytes = 0;
    std::vector<std::wstring> files;  // the full paths removed
};

// Whether `file` is one of those copies (every rule above but the folder's).
bool is_leftover(const std::filesystem::path& file);
// Removes the copies directly in each of `folders` (each skipped when missing or a link or junction).
Leftovers remove_leftovers(const std::vector<std::wstring>& folders);

}  // namespace simqvfr::launcher

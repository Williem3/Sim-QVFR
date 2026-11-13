// Files older Sim-QVFR versions left in the logs folder that players never asked for: up to 1.1.0-beta, the layer wrote
// a copy of the game's program there (<exe>-<hash>-image.dump, ~30 MB for Le Mans Ultimate, again after each game
// update). A launcher built without the local analysis tools (probe/local_analysis.h) removes them at each launch.
#pragma once

#include <cstdint>
#include <string>

namespace simqvfr::launcher {

struct Leftovers {
    int removed = 0;
    uint64_t bytes = 0;
};
// Removes `logsFolder`'s *-image.dump files (that folder's own files only).
Leftovers remove_leftovers(const std::wstring& logsFolder);

}  // namespace simqvfr::launcher

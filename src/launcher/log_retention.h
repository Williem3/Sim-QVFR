// What the launcher's logs folder keeps. Every launch adds a session log (session-<stamp>.log) and a run folder
// (run-<stamp>\), an elevated layer change a registry backup, a hang a minidump; prune_logs, at each launch before the
// launch writes its own, keeps the newest of each kind:
//   session-<stamp>.log, run-<stamp>\                 the newest kKeepSessions of each (more than kSessionHistory, so
//                                                       every session the Diagnostics pane compares keeps its files)
//   hang-<time>.dmp                                    the newest kKeepHangDumps (by file time: the name has no date)
//   openxr-layers-machine|user-<stamp>.reg             per hive, the oldest (Windows' layers before Sim-QVFR's first
//                                                       change) and the newest kKeepRegistryBackups
//   sim-qvfr-report-<stamp>.zip, sim-qvfr-logs-*.zip   the newest kKeepReports of each
//   cpu-profile-*.txt                                  the newest kKeepCpuProfiles
// Never touched: openxr-registrations\ (an elevated launch's ledgers, the unregister sweep needs them) and its log,
// and the diagnostics' own output (diagnostics_paths: captures*, pix, shaders, image dumps), which the user asked for:
// the Diagnostics pane shows its size and clears it to the Recycle Bin.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace simqvfr::launcher {

inline constexpr int kKeepSessions = 30;
inline constexpr int kKeepHangDumps = 5;
inline constexpr int kKeepRegistryBackups = 10;
inline constexpr int kKeepReports = 5;
inline constexpr int kKeepCpuProfiles = 10;

struct PruneResult {
    int removed = 0;     // files and folders
    uint64_t bytes = 0;  // their size
};
PruneResult prune_logs(const std::wstring& logsFolder);

// What the diagnostics wrote into `logsFolder` that nothing prunes: the folders captures, captures-*, pix and shaders,
// and image dumps (a dev build's, local analysis tools; or an older version's, by leftovers.h's is_leftover), existing
// ones only.
std::vector<std::wstring> diagnostics_paths(const std::wstring& logsFolder);

struct LogsUse {
    uint64_t bytes = 0;             // everything in the logs folder
    uint64_t diagnosticsBytes = 0;  // of it, diagnostics_paths'
};
LogsUse logs_use(const std::wstring& logsFolder);

// "820 KB", "12.4 MB", "1.3 GB".
std::wstring size_text(uint64_t bytes);

}  // namespace simqvfr::launcher

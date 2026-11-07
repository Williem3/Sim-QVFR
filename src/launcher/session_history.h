// Session history: the last few measured sessions, each with the settings it ran with, so the Diagnostics pane can
// compare a session with the one before it ("vs previous: +6.0 fps, 90th percentile −1.20 ms, after Outer 50% → 40%")
// and with a run without QVFR (the pane's Next launch without QVFR).
//
// A session is runtime.ini's session_* keys (the layer's write_session_summary, every ~10 s while the game runs), named
// by its log (session_log: logs\session-<stamp>.log, one per launch). The launcher keeps each one as
// <its folder>\sessions\session-<stamp>.ini: the game's settings as the launcher held them at the session's latest
// numbers (the launcher's ini, or the game's own copy: where the headset menu writes too), with the numbers in
// [session]. A record is written only when the session's numbers change, so a change made after the game closed
// belongs to the next session. Only the newest kSessionHistory stay.
#pragma once

#include <string>
#include <vector>

#include "settings.h"

namespace simqvfr::launcher {

inline constexpr int kSessionHistory = 12;  // every game's together; the pane lists a game's newest kSessionRows
inline constexpr int kSessionRows = 5;

struct SessionRecord {
    std::wstring id;    // its log's name without .log ("session-20261107-101500")
    std::wstring game;  // the game's id ("ace")
    std::wstring time, mode;
    bool baseline = false;  // without QVFR: the game's own two views (Next launch without QVFR)
    double fps = -1, frameP50 = -1, frameP90 = -1, latePercent = -1, cpuMs = -1, gpuMs = -1;
    std::wstring limited;
    Settings settings;
    // The same numbers as `session` (nothing new to store).
    bool same_numbers(const SessionInfo& session) const;
};

// <the launcher's ini folder>\sessions.
std::wstring sessions_folder(const std::wstring& settingsIni);
// A session's id from runtime.ini's session_log (the log's file name without .log); empty without one.
std::wstring session_id(const std::wstring& log);
// The run folder's copy of the ini for a session's log (logs\run-<stamp>\sim-qvfr.ini), where the launch wrote the
// game's id; empty if `log` isn't a session log.
std::wstring session_run_ini(const std::wstring& log);
// A session measured without QVFR (its mode, as the layer names it).
bool baseline_mode(const std::wstring& mode);

// A record of `session` for `game` with `settings`.
SessionRecord session_record(const SessionInfo& session, const std::wstring& game, const Settings& settings);
// Writes `record` into `folder` (new, or the same session's newer numbers) with its settings copied from
// `settingsFile`, and keeps the newest kSessionHistory. True if written.
bool store_session(const std::wstring& folder, const SessionRecord& record, const std::wstring& settingsFile);
// The records, newest first.
std::vector<SessionRecord> load_sessions(const std::wstring& folder);

// What changed from `before` to `after` that a session can measure, in the panes' order: "Outer 50% → 40%",
// "Crop on". Empty: nothing.
std::vector<std::wstring> setting_changes(const Settings& before, const Settings& after);
// The first `max` changes as one phrase: "Outer 50% → 40%, Inner 100% → 120% and 2 more".
std::wstring changes_text(const std::vector<std::wstring>& changes, size_t max = 3);

// The record before records[index] (newest first) of the same game and kind (with QVFR or without); -1: none.
int previous_session(const std::vector<SessionRecord>& records, size_t index);
// The newest record of the same game older than records[index] of the other kind (without QVFR for a session with
// it, and the other way round); -1: none.
int other_kind_session(const std::vector<SessionRecord>& records, size_t index);
// How `now` differs from `before`: "+6.0 fps, 90th percentile −1.20 ms" (the minus sign U+2212).
std::wstring difference_text(const SessionRecord& now, const SessionRecord& before);
// The comparison as a line: "vs previous: +6.0 fps, 90th percentile −1.20 ms, after Outer 50% → 40%". `label`: "vs
// previous", "vs without QVFR".
std::wstring comparison_text(const std::wstring& label, const SessionRecord& now, const SessionRecord& before);

}  // namespace simqvfr::launcher

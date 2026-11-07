// Hang watch: ACE sometimes stops between xrCreateSession and its first frame (inside the runtime's
// xrSuggestInteractionProfileBindings, with no crash), and a game can freeze mid-session (ACC loading a track
// with four views). While a session runs, if no xrEndFrame arrives for `seconds` (from xrCreateSession, or from
// the last frame), a background thread signals the dump helper (probe/hang_dump.h: sim-qvfr-launcher.exe next
// to the layer, or one folder up, standing by from the first arm), which logs every loaded module (name, base,
// size, so addresses in other logs, such as Cheeky's crash log, can be named) and writes a minidump with every
// thread's stack to logs\hang-<time>.dmp. Once per process.
// Nothing is dumped from inside the game: that suspends every other thread and deadlocked X-Plane when one of
// them held the loader lock (2026-11-06). Without the helper the modules are logged here and no dump is written.
// Once the helper stands by, the watch thread needs no loader lock (paths are resolved at the first arm, and it
// only signals an event), so a thread stuck holding it (a DLL load) neither stops the dump nor is stopped by it.
#pragma once

namespace simqvfr {

// At xrCreateSession. 0 seconds: off.
void hang_watch_arm(unsigned seconds);
// At every xrEndFrame.
void hang_watch_frame();
// At xrDestroySession: no frames are due until the next session.
void hang_watch_disarm();
// Tests: the helper to start (instead of the launcher next to this module); before the first arm.
void hang_watch_set_helper(const wchar_t* path);
// Logs the loaded modules now. It reads the loader list as another process would (psapi through
// ReadProcessMemory), so it doesn't wait for a thread that holds the loader lock.
void log_modules();

}  // namespace simqvfr

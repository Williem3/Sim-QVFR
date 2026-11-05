// Hang watch: ACE sometimes stops between xrCreateSession and its first frame (inside the runtime's
// xrSuggestInteractionProfileBindings, with no crash), and a game can freeze mid-session (ACC loading a track
// with four views). While a session runs, if no xrEndFrame arrives for `seconds` (from xrCreateSession, or from
// the last frame), a background thread logs every loaded module (name, base, size, so addresses in other logs,
// such as Cheeky's crash log, can be named) and writes a minidump with every thread's stack to
// logs\hang-<time>.dmp. Once per process.
#pragma once

namespace simqvfr {

// At xrCreateSession. 0 seconds: off.
void hang_watch_arm(unsigned seconds);
// At every xrEndFrame.
void hang_watch_frame();
// At xrDestroySession: no frames are due until the next session.
void hang_watch_disarm();
// Logs the loaded modules now.
void log_modules();

}  // namespace simqvfr

// Start-up hang watch: ACE sometimes stops between xrCreateSession and its first frame (inside the
// runtime's xrSuggestInteractionProfileBindings, with no crash). If no xrEndFrame arrives within
// `seconds` of xrCreateSession, a background thread logs every loaded module (name, base, size, so
// addresses in other logs, such as Cheeky's crash log, can be named) and writes a minidump with every
// thread's stack to logs\hang-<time>.dmp. Once per process.
#pragma once

namespace aceqv {

// At xrCreateSession. 0 seconds: off.
void hang_watch_arm(unsigned seconds);
// At every xrEndFrame: the first one disarms the watch.
void hang_watch_frame();
// Logs the loaded modules now.
void log_modules();

}  // namespace aceqv

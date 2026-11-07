// The hang watch's minidump (probe/hang_watch.h), written from outside the game. When the watch arms, the layer
// starts `sim-qvfr-launcher --hang-dump <pid> <log>`, which stands by until the game signals its event
// (hang_dump_event) or exits. Signalled, it appends the game's loaded modules to the game's log and writes
// hang-HHMMSS.dmp next to it. In the game itself MiniDumpWriteDump suspends every other thread and then needs the
// loader lock: when a suspended thread held it (X-Plane loading the SR22's xlua plugin, 2026-11-06) the game
// never woke. From outside, nothing in the game has to run while its threads are suspended. The helper is
// started ahead because CreateProcessW in the game waits for the loader lock too (the first one in a process
// loads sechost.dll under it); signalling an event doesn't. The launcher builds hang_dump.cpp; the layer only
// includes this header (no dbghelp in the game).
#pragma once

#include <cwchar>

namespace simqvfr {

// The helper's exit codes.
enum HangDumpResult : int {
    kHangDumpWritten = 0,
    kHangDumpNoProcess = 2,  // the game can't be opened (gone, or more privileged than the helper)
    kHangDumpNoDbghelp = 3,  // no dbghelp.dll in System32
    kHangDumpNoFile = 4,     // the dump file can't be created
    kHangDumpFailed = 5,     // MiniDumpWriteDump failed (the partial file is deleted)
    kHangDumpUsage = 6,      // the command line isn't --hang-dump <pid> <log>
    kHangDumpGameExited = 7, // the game ended without a hang
};

// The event the game signals for a dump: "Local\sim-qvfr-hang-<pid>".
inline void hang_dump_event(unsigned long pid, wchar_t (&name)[64]) {
    swprintf_s(name, L"Local\\sim-qvfr-hang-%lu", pid);
}

inline const char* hang_dump_result_text(unsigned long code) {
    switch (code) {
        case kHangDumpWritten: return "written";
        case kHangDumpNoProcess: return "not written: the helper cannot open the game";
        case kHangDumpNoDbghelp: return "not written: no dbghelp.dll";
        case kHangDumpNoFile: return "not written: the file cannot be created";
        case kHangDumpFailed: return "not written: MiniDumpWriteDump failed";
        case kHangDumpUsage: return "not written: bad helper command line";
        case kHangDumpGameExited: return "not written: the helper saw the game exit";
        default: return "not written: the helper failed";
    }
}

// In the helper. `argv` as CommandLineToArgvW gives it. True when it asks for a hang dump (--hang-dump first),
// which is then stood by for and done; `exitCode` is what the helper returns.
bool hang_dump_command(int argc, wchar_t** argv, int& exitCode);
// Waits for process `pid` to signal its event or exit. Signalled: appends its modules to `logPath` (the log's own
// line format) and writes the minidump next to it: every thread's stack and state, the unloaded modules and the
// handles; no heap.
int hang_dump(unsigned long pid, const wchar_t* logPath);

}  // namespace simqvfr

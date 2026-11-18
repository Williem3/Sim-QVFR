// The Diagnostics card: the last session's verdict, compared with the ones before it, and its helpers.
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "../launcher.h"
#include "../log_retention.h"
#include "../settings.h"
#include "../session_history.h"
#include "../../games/game.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

namespace {

// What a thread is, for the names users will see most (others: none).
std::wstring thread_note(const std::wstring& name) {
    if (name == L"GameThread")
        return tr(L"The game's main thread, which here also submits each frame. Near 100% it limits the frame rate.");
    if (name == L"Physics")
        return tr(L"Waits for each physics step by spinning on the clock, so it always looks busy.");
    if (name.find(L"aSeeVRClient") != std::wstring::npos)
        return tr(L"The Pimax eye tracker's client: it polls the tracker continuously, so it always looks busy.");
    if (name.find(L"virtualdesktop-openxr") != std::wstring::npos)
        return tr(L"Virtual Desktop's frame pacing: it waits for each frame by spinning on the clock, so it always "
                  L"looks busy.");
    if (name.find(L"Cheeky") != std::wstring::npos)
        return tr(L"Cheeky Foveated DLSS's frame generation.");
    return L"";
}

// The layers below Sim-QVFR as people know them: "XR_APILAYER_MBUCCHIA_toolkit, ..." -> "OpenXR Toolkit, ...".
std::wstring friendly_layers(const std::wstring& names) {
    std::wstring out;
    for (size_t at = 0; at < names.size();) {
        const size_t comma = names.find(L',', at), end = comma == std::wstring::npos ? names.size() : comma;
        std::wstring name = names.substr(at, end - at);
        name.erase(0, name.find_first_not_of(L' '));
        if (!name.empty()) {
            std::string ascii;  // layer names are ASCII
            for (const wchar_t c : name)
                ascii += static_cast<char>(c);
            out += (out.empty() ? L"" : L", ") + friendly_layer(ascii);
        }
        at = end + 1;
    }
    return out;
}

// The logs folder's size, walked at most every 10 s (the card is laid out on every paint).
struct LogsUseCache {
    ULONGLONG at = 0;
    LogsUse use;
} logsUse;

const LogsUse& logs_use_now() {
    const ULONGLONG now = GetTickCount64();
    if (!logsUse.at || now - logsUse.at > 10000) {
        logsUse.use = logs_use(logs_folder());
        logsUse.at = now;
    }
    return logsUse.use;
}

}  // namespace

std::wstring logs_folder() {
    const std::wstring runtime = runtime_info_path();  // the launcher's folder\runtime.ini
    const size_t slash = runtime.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring(L".") : runtime.substr(0, slash)) + L"\\logs";
}

void clear_captures() {
    std::wstring list;  // double-null terminated
    for (const std::wstring& path : diagnostics_paths(logs_folder()))
        list += path + L'\0';
    if (list.empty())
        return;
    list += L'\0';
    SHFILEOPSTRUCTW op{};
    op.hwnd = app.hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    // To the Recycle Bin, without asking; a drive without one (FOF_WANTNUKEWARNING): Windows asks before deleting.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
    SHFileOperationW(&op);
    logsUse.at = 0;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

bool copy_text(const std::wstring& text) {
    if (!OpenClipboard(app.hwnd))
        return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(memory), text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) {
            GlobalFree(memory);
            memory = nullptr;
        }
        CloseClipboard();
        return memory != nullptr;
    }
    CloseClipboard();
    return false;
}

namespace {

// The last session's record in the history (runtime.ini's session), -1: not kept (no ini, or not measured).
int last_session_index() {
    const std::wstring id = session_record_id(app.runtime.session);
    for (size_t i = 0; !id.empty() && i < app.sessions.size(); ++i)
        if (app.sessions[i].id == id)
            return static_cast<int>(i);
    return -1;
}

// The last session against the one before it, and against the newest of the other kind (with or without QVFR), as
// lines for the summary: "vs previous: +6.0 fps, 90th percentile −1.20 ms, after Outer 50% → 40%".
std::wstring comparison_lines() {
    const int now = last_session_index();
    if (now < 0)
        return L"";
    std::wstring text;
    const SessionRecord& r = app.sessions[now];
    if (const int before = previous_session(app.sessions, now); before >= 0)
        text += comparison_text(L"vs previous (" + app.sessions[before].time + L")", r, app.sessions[before]) + L"\n";
    if (const int other = other_kind_session(app.sessions, now); other >= 0)
        text += comparison_text(r.baseline ? L"vs with QVFR (" + app.sessions[other].time + L")"
                                           : L"vs without QVFR (" + app.sessions[other].time + L")",
                                r, app.sessions[other]) +
                L"\n";
    return text;
}

}  // namespace

void copy_summary() { copy_text(session_summary_text(app.runtime) + comparison_lines()); }

std::wstring clipboard_text() {
    std::wstring text;
    if (!OpenClipboard(app.hwnd))
        return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = p;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

// Whether the description box's field is wholly inside the scrolling area: only then is the native edit
// shown (a child window cannot be clipped like the canvas); otherwise the canvas draws its text.
bool report_edit_fits() {
    const Rect& r = app.reportEditRect;
    return r.w > 0 && r.y >= app.panelBody.y && r.bottom() <= app.panelBody.bottom();
}

// The Diagnostics card: the verdict first (frame rate against the headset's, slow frames against the frame's budget, what
// limits the frame), then the sessions before it, the details behind a More, and the capture switches.
Card diagnostics_card() {
    Card c{Tab::Game, tr(L"Diagnostics")};
    const SessionInfo& last = app.runtime.session;
    if (last.known() && last.refreshHz > 0 && last.fps < last.refreshHz * 0.97)
        c.chip = trf(L"Below {fps} fps", {{L"fps", formatted(L"%.0f", last.refreshHz)}});
    c.body = [](Body& b) {
        const SessionInfo& s = app.runtime.session;
        const std::wstring dash = L"—";
        if (!s.known()) {
            b.note(tr(L"No session measured yet. Launch the game from here: this fills in while it runs, and keeps the "
                      L"last session’s numbers after it closes."));
        } else {
            std::wstring head = s.time + L"  ·  " + s.mode;
            if (s.refreshHz > 0)
                head += formatted(L"  ·  %.0f Hz", s.refreshHz);
            head += L"  ·  ";
            head += s.frameGeneration ? tr(L"frame generation on") : tr(L"frame generation off");
            b.note(head);
            const double budget = s.refreshHz > 0 ? 1000 / s.refreshHz : 0;
            b.row(tr(L"Frame rate"),
                  s.refreshHz > 0
                      ? trf(L"Target {fps} fps, the headset’s refresh.", {{L"fps", formatted(L"%.0f", s.refreshHz)}})
                      : L"",
                  formatted(L"%.1f fps", s.fps));
            b.row(tr(L"Slow frames"),
                  budget > 0
                      ? trf(L"90th percentile; the frame’s budget is {ms} ms.", {{L"ms", formatted(L"%.1f", budget)}})
                      : std::wstring(tr(L"90th percentile.")),
                  s.frameP90 > 0 ? formatted(L"%.2f ms", s.frameP90) : dash);
            b.row(tr(L"Limited by"), L"", s.limited.empty() ? dash : s.limited);
            // Against the session before it (what changed in between) and a run of the other kind (with or without QVFR).
            const int now = last_session_index();
            if (now >= 0) {
                const SessionRecord& r = app.sessions[now];
                if (const int before = previous_session(app.sessions, now); before >= 0) {
                    const SessionRecord& p = app.sessions[before];
                    std::wstring desc = p.time;
                    if (!r.baseline) {
                        const std::vector<std::wstring> changes = setting_changes(p.settings, r.settings);
                        desc += L"  ·  ";
                        desc += changes.empty() ? std::wstring(tr(L"same settings"))
                                                : trf(L"after {changes}", {{L"changes", changes_text(changes)}});
                    }
                    b.row(tr(L"Previous session"), desc, difference_text(r, p));
                }
                if (const int other = other_kind_session(app.sessions, now); other >= 0) {
                    const SessionRecord& o = app.sessions[other];
                    b.row(r.baseline ? tr(L"With QVFR") : tr(L"Without QVFR"),
                          r.baseline ? o.time : o.time + L"  ·  " + tr(L"the game’s own two views"),
                          difference_text(r, o));
                }
            }
            b.more(tr(L"details"),
                   {tr(L"Late frames"), tr(L"Frame time"), tr(L"CPU frame time"), tr(L"GPU frame time"),
                    tr(L"CPU in use"), tr(L"Busiest threads"), tr(L"Eye tracking lookup"),
                    tr(L"Layer on the render thread"), tr(L"Layers below"), L"VRAM"},
                   [](Body& m) {
                       const SessionInfo& si = app.runtime.session;
                       const std::wstring none = L"—";
                       const double ms = si.refreshHz > 0 ? 1000 / si.refreshHz : 0;
                       m.row(tr(L"Frame time"), tr(L"Median and 90th percentile."),
                             si.frameP50 > 0 ? formatted(L"%.2f ms", si.frameP50) + formatted(L"  ·  %.2f ms", si.frameP90) : none);
                       m.row(tr(L"Late frames"), tr(L"Frames that missed the headset’s refresh."),
                             si.latePercent >= 0 ? formatted(L"%.1f%%", si.latePercent) : none);
                       m.row(tr(L"CPU frame time"),
                             ms > 0 ? trf(L"The render thread’s work; near {ms} ms the CPU limits the frame.",
                                          {{L"ms", formatted(L"%.1f", ms)}})
                                    : L"",
                             si.cpuMs >= 0 ? formatted(L"%.2f ms", si.cpuMs) : none);
                       m.row(tr(L"GPU frame time"), tr(L"The whole GPU per game frame."),
                             si.gpuMs >= 0 ? formatted(L"%.2f ms", si.gpuMs) + L"  ·  " +
                                                 trf(L"{percent}% busy", {{L"percent", formatted(L"%.0f", si.gpuBusy)}})
                                           : none);
                       m.row(tr(L"CPU in use"), tr(L"All of the game’s threads together."),
                             si.cores >= 0 ? trf(L"{count} cores", {{L"count", formatted(L"%.1f", si.cores)}}) : none);
                       m.group(tr(L"Busiest threads"));
                       if (si.threads.empty())
                           m.row(tr(L"Not measured"), L"", none);
                       for (const auto& [name, percent] : si.threads)
                           m.row(name, thread_note(name),
                                 trf(L"{percent}% of a core", {{L"percent", formatted(L"%.0f", percent)}}));
                       m.group(tr(L"Layer and memory"));
                       if (si.gazeMs >= 0)
                           m.row(tr(L"Eye tracking lookup"), tr(L"On the layer’s own thread."),
                                 formatted(L"%.3f ms", si.gazeMs));
                       m.row(tr(L"Layer on the render thread"), tr(L"Sim-QVFR’s own time in each frame."),
                             si.layerMs >= 0 ? formatted(L"%.2f ms", si.layerMs) : none);
                       const bool toolkit = si.layersBelow.find(L"MBUCCHIA_toolkit") != std::wstring::npos;
                       m.row(tr(L"Layers below"),
                             toolkit ? tr(L"OpenXR Toolkit adds about 0.8 ms to every frame: switch it off in OpenXR "
                                          L"layers if unused.")
                                     : L"",
                             si.layersBelow.empty() ? std::wstring(tr(L"None")) : friendly_layers(si.layersBelow));
                       if (app.runtime.vramUsedMb > 0)
                           m.row(L"VRAM", tr(L"The game’s use against what Windows allows it."),
                                 trf(L"{used} GB of {budget} GB",
                                     {{L"used", formatted(L"%.1f", app.runtime.vramUsedMb / 1024)},
                                      {L"budget", formatted(L"%.1f", app.runtime.vramBudgetMb / 1024)}}));
                   });
        }
        // The game's last few sessions, each with what changed since the one before it.
        {
            const int now = last_session_index();
            const std::wstring game = now >= 0 ? app.sessions[now].game : games::wide_id(games::launcher_game());
            std::vector<size_t> recent;
            for (size_t i = 0; i < app.sessions.size() && recent.size() < kSessionRows; ++i)
                if (app.sessions[i].game == game)
                    recent.push_back(i);
            if (recent.size() > 1) {
                b.group(tr(L"Recent sessions"));
                for (size_t i : recent) {
                    const SessionRecord& r = app.sessions[i];
                    const int before = previous_session(app.sessions, i);
                    std::wstring desc;
                    if (r.baseline)
                        desc = tr(L"Without QVFR: the game’s own two views.");
                    else if (before < 0)
                        desc = tr(L"The oldest kept.");
                    else if (const std::vector<std::wstring> changes = setting_changes(app.sessions[before].settings, r.settings);
                             changes.empty())
                        desc = tr(L"Same settings as the one before.");
                    else
                        desc = changes_text(changes, 4) + L".";
                    std::wstring value = formatted(L"%.1f fps", r.fps);
                    if (r.frameP90 > 0)
                        value += formatted(L"  ·  %.2f ms", r.frameP90);
                    const std::wstring stint = stint_text(r);
                    std::wstring when = r.time;
                    if (!stint.empty())
                        when += L"  ·  " + stint;
                    if (static_cast<int>(i) == now)
                        when += L"  ·  " + std::wstring(tr(L"last"));
                    b.row(when, desc, value);
                }
            }
        }
        // Tools for comparing and for bug reports, each at the next launch: behind a More, unless one is on (then they show,
        // so it isn't forgotten).
        const auto tools = [](Body& m) {
            const std::wstring cpuKey = key_text(app.s.hotkeys[static_cast<int>(HotkeyAction::CpuCapture)].key);
            const std::wstring frameKey = key_text(app.s.hotkeys[static_cast<int>(HotkeyAction::FrameCapture)].key);
            m.toggle(kDiagCapture, tr(L"CPU capture"), app.s.cpuProfile,
                     cpuKey.empty() ? std::wstring(tr(L"Its hotkey while driving records 20 s of the busiest threads."))
                                    : trf(L"{key} while driving records 20 s of the busiest threads.",
                                          {{L"key", cpuKey}}),
                     false, Applies::NextLaunch);
            m.toggle(kDiagFrameCapture, tr(L"Frame capture"), app.s.frameCapture,
                     frameKey.empty() ? std::wstring(tr(L"Its hotkey saves the frame the headset gets, as PNG files."))
                                      : trf(L"{key} saves the frame the headset gets, as PNG files.",
                                            {{L"key", frameKey}}),
                     false, Applies::NextLaunch);
            m.toggle(kDiagBaseline, tr(L"Next launch without QVFR"), app.s.baselineNext,
                     tr(L"Once, with QVFR off and still measured, to compare against."), false, Applies::NextLaunch);
            const std::wstring launches = std::to_wstring(app.s.debugLogs > 0 ? app.s.debugLogs : kDebugLogLaunches);
            m.toggle(kDiagDebugLogs, tr(L"Detailed logs"), app.s.debugLogs > 0,
                     app.s.debugLogs == 1
                         ? std::wstring(
                               tr(L"The next launch logs everything, for a bug report; then normal logs again."))
                     : app.s.debugLogs > 1
                         ? trf(L"The next {count} launches log everything, for a bug report; then normal logs again.",
                               {{L"count", launches}})
                         : trf(L"For a bug report: {count} launches that log everything, then normal logs again.",
                               {{L"count", launches}}),
                     false, Applies::NextLaunch);
            const LogsUse& use = logs_use_now();
            const std::wstring kept =
                trf(L"The newest {count} sessions are kept.", {{L"count", std::to_wstring(kKeepSessions)}});
            if (use.diagnosticsBytes > 0)
                m.action(tr(L"Logs folder"),
                         kept + L" " +
                             trf(L"Captures stay until cleared: {size} of {total}.",
                                 {{L"size", size_text(use.diagnosticsBytes)}, {L"total", size_text(use.bytes)}}),
                         kDiagClearCaptures, tr(L"Clear captures"));
            else
                m.row(tr(L"Logs folder"), kept, size_text(use.bytes));
        };
        b.divider();
        if (app.s.cpuProfile || app.s.frameCapture || app.s.baselineNext || app.s.debugLogs > 0) {
            b.group(tr(L"Tools for comparing and bug reports"));
            tools(b);
        } else {
            b.more(tr(L"tools for comparing and bug reports"),
                   {tr(L"CPU capture"), tr(L"Frame capture"), tr(L"Next launch without QVFR"), tr(L"Detailed logs"),
                    tr(L"Logs folder")},
                   tools);
        }
    };
    return c;
}

}  // namespace simqvfr::launcher::ui::win

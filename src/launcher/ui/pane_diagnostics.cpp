// The Diagnostics card: the last session's verdict, compared with the ones before it, and its helpers.
#include <string>
#include <vector>

#include <windows.h>

#include "../launcher.h"
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
        return L"The game's main thread, which here also submits each frame. Near 100% it limits the frame rate.";
    if (name == L"Physics")
        return L"Waits for each physics step by spinning on the clock, so it always looks busy.";
    if (name.find(L"aSeeVRClient") != std::wstring::npos)
        return L"The Pimax eye tracker's client: it polls the tracker continuously, so it always looks busy.";
    if (name.find(L"virtualdesktop-openxr") != std::wstring::npos)
        return L"Virtual Desktop's frame pacing: it waits for each frame by spinning on the clock, so it always looks busy.";
    if (name.find(L"Cheeky") != std::wstring::npos)
        return L"Cheeky Foveated DLSS's frame generation.";
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

}  // namespace

std::wstring logs_folder() {
    const std::wstring runtime = runtime_info_path();  // the launcher's folder\runtime.ini
    const size_t slash = runtime.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring(L".") : runtime.substr(0, slash)) + L"\\logs";
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
    const std::wstring id = session_id(app.runtime.session.log);
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
    Card c{Tab::Game, L"Diagnostics"};
    const SessionInfo& last = app.runtime.session;
    if (last.known() && last.refreshHz > 0 && last.fps < last.refreshHz * 0.97)
        c.chip = formatted(L"Below %.0f fps", last.refreshHz);
    c.body = [](Body& b) {
        const SessionInfo& s = app.runtime.session;
        const std::wstring dash = L"—";
        if (!s.known()) {
            b.note(L"No session measured yet. Launch the game from here: this fills in while it runs, and keeps the last "
                   L"session’s numbers after it closes.");
        } else {
            std::wstring head = s.time + L"  ·  " + s.mode;
            if (s.refreshHz > 0)
                head += formatted(L"  ·  %.0f Hz", s.refreshHz);
            head += s.frameGeneration ? L"  ·  frame generation on" : L"  ·  frame generation off";
            b.note(head);
            const double budget = s.refreshHz > 0 ? 1000 / s.refreshHz : 0;
            b.row(L"Frame rate", s.refreshHz > 0 ? formatted(L"Target %.0f fps, the headset’s refresh.", s.refreshHz) : L"",
                  formatted(L"%.1f fps", s.fps));
            b.row(L"Slow frames", budget > 0 ? formatted(L"90th percentile; the frame’s budget is %.1f ms.", budget) : L"90th percentile.",
                  s.frameP90 > 0 ? formatted(L"%.2f ms", s.frameP90) : dash);
            b.row(L"Limited by", L"", s.limited.empty() ? dash : s.limited);
            // Against the session before it (what changed in between) and a run of the other kind (with or without QVFR).
            const int now = last_session_index();
            if (now >= 0) {
                const SessionRecord& r = app.sessions[now];
                if (const int before = previous_session(app.sessions, now); before >= 0) {
                    const SessionRecord& p = app.sessions[before];
                    std::wstring desc = p.time;
                    if (!r.baseline) {
                        const std::vector<std::wstring> changes = setting_changes(p.settings, r.settings);
                        desc += changes.empty() ? L"  ·  same settings" : L"  ·  after " + changes_text(changes);
                    }
                    b.row(L"Previous session", desc, difference_text(r, p));
                }
                if (const int other = other_kind_session(app.sessions, now); other >= 0) {
                    const SessionRecord& o = app.sessions[other];
                    b.row(r.baseline ? L"With QVFR" : L"Without QVFR", o.time + (r.baseline ? L"" : L"  ·  the game’s own two views"),
                          difference_text(r, o));
                }
            }
            b.more(L"details",
                   {L"Late frames", L"Frame time", L"CPU frame time", L"GPU frame time", L"CPU in use", L"Busiest threads",
                    L"Eye tracking lookup", L"Layer on the render thread", L"Layers below", L"VRAM"},
                   [](Body& m) {
                       const SessionInfo& si = app.runtime.session;
                       const std::wstring none = L"—";
                       const double ms = si.refreshHz > 0 ? 1000 / si.refreshHz : 0;
                       m.row(L"Frame time", L"Median and 90th percentile.",
                             si.frameP50 > 0 ? formatted(L"%.2f ms", si.frameP50) + formatted(L"  ·  %.2f ms", si.frameP90) : none);
                       m.row(L"Late frames", L"Frames that missed the headset’s refresh.",
                             si.latePercent >= 0 ? formatted(L"%.1f%%", si.latePercent) : none);
                       m.row(L"CPU frame time",
                             ms > 0 ? formatted(L"The render thread’s work; near %.1f ms the CPU limits the frame.", ms) : L"",
                             si.cpuMs >= 0 ? formatted(L"%.2f ms", si.cpuMs) : none);
                       m.row(L"GPU frame time", L"The whole GPU per game frame.",
                             si.gpuMs >= 0 ? formatted(L"%.2f ms", si.gpuMs) + formatted(L"  ·  %.0f%% busy", si.gpuBusy) : none);
                       m.row(L"CPU in use", L"All of the game’s threads together.",
                             si.cores >= 0 ? formatted(L"%.1f cores", si.cores) : none);
                       m.group(L"Busiest threads");
                       if (si.threads.empty())
                           m.row(L"Not measured", L"", none);
                       for (const auto& [name, percent] : si.threads)
                           m.row(name, thread_note(name), formatted(L"%.0f%% of a core", percent));
                       m.group(L"Layer and memory");
                       if (si.gazeMs >= 0)
                           m.row(L"Eye tracking lookup", L"On the layer’s own thread.", formatted(L"%.3f ms", si.gazeMs));
                       m.row(L"Layer on the render thread", L"Sim-QVFR’s own time in each frame.",
                             si.layerMs >= 0 ? formatted(L"%.2f ms", si.layerMs) : none);
                       const bool toolkit = si.layersBelow.find(L"MBUCCHIA_toolkit") != std::wstring::npos;
                       m.row(L"Layers below",
                             toolkit ? L"OpenXR Toolkit adds about 0.8 ms to every frame: switch it off in OpenXR layers if unused." : L"",
                             si.layersBelow.empty() ? L"None" : friendly_layers(si.layersBelow));
                       if (app.runtime.vramUsedMb > 0)
                           m.row(L"VRAM", L"The game’s use against what Windows allows it.",
                                 formatted(L"%.1f GB", app.runtime.vramUsedMb / 1024) + formatted(L" of %.1f GB", app.runtime.vramBudgetMb / 1024));
                   });
        }
        // The game's last few sessions, each with what changed since the one before it.
        {
            const int now = last_session_index();
            const std::wstring game = now >= 0 ? app.sessions[now].game : game_id(games::launcher_game());
            std::vector<size_t> recent;
            for (size_t i = 0; i < app.sessions.size() && recent.size() < kSessionRows; ++i)
                if (app.sessions[i].game == game)
                    recent.push_back(i);
            if (recent.size() > 1) {
                b.group(L"Recent sessions");
                for (size_t i : recent) {
                    const SessionRecord& r = app.sessions[i];
                    const int before = previous_session(app.sessions, i);
                    std::wstring desc;
                    if (r.baseline)
                        desc = L"Without QVFR: the game’s own two views.";
                    else if (before < 0)
                        desc = L"The oldest kept.";
                    else if (const std::vector<std::wstring> changes = setting_changes(app.sessions[before].settings, r.settings);
                             changes.empty())
                        desc = L"Same settings as the one before.";
                    else
                        desc = changes_text(changes, 4) + L".";
                    std::wstring value = formatted(L"%.1f fps", r.fps);
                    if (r.frameP90 > 0)
                        value += formatted(L"  ·  %.2f ms", r.frameP90);
                    b.row(r.time + (static_cast<int>(i) == now ? L"  ·  last" : L""), desc, value);
                }
            }
        }
        // Tools for comparing and for bug reports, each at the next launch: behind a More, unless one is on (then they show,
        // so it isn't forgotten).
        const auto tools = [](Body& m) {
            const std::wstring cpuKey = key_text(app.s.hotkeys[static_cast<int>(HotkeyAction::CpuCapture)].key);
            const std::wstring frameKey = key_text(app.s.hotkeys[static_cast<int>(HotkeyAction::FrameCapture)].key);
            m.toggle(kDiagCapture, L"CPU capture", app.s.cpuProfile,
                     (cpuKey.empty() ? std::wstring(L"Its hotkey") : cpuKey) + L" while driving records 20 s of the busiest threads.",
                     false, Applies::NextLaunch);
            m.toggle(kDiagFrameCapture, L"Frame capture", app.s.frameCapture,
                     (frameKey.empty() ? std::wstring(L"Its hotkey") : frameKey) + L" saves the frame the headset gets, as PNG files.",
                     false, Applies::NextLaunch);
            m.toggle(kDiagBaseline, L"Next launch without QVFR", app.s.baselineNext,
                     L"Once, with QVFR off and still measured, to compare against.", false, Applies::NextLaunch);
        };
        b.divider();
        if (app.s.cpuProfile || app.s.frameCapture || app.s.baselineNext) {
            b.group(L"Tools for comparing and bug reports");
            tools(b);
        } else {
            b.more(L"tools for comparing and bug reports", {L"CPU capture", L"Frame capture", L"Next launch without QVFR"}, tools);
        }
    };
    return c;
}

}  // namespace simqvfr::launcher::ui::win

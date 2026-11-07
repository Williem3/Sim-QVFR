// The Diagnostics pane: the last session's summary, compared with the one before it, and its helpers.
#include <string>
#include <vector>

#include <windows.h>

#include "../launcher.h"
#include "../settings.h"
#include "../session_history.h"
#include "../../games/game.h"
#include "window_state.h"
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
    return r.w > 0 && r.y >= app.scrollArea.y && r.bottom() <= app.scrollArea.bottom();
}

float diagnostics_pane(float x, float y, float w, bool draw) {
    const SessionInfo& s = app.runtime.session;
    const std::wstring dash = L"—";
    float h = 0;
    if (!s.known()) {
        const std::wstring none = L"No session measured yet. Start the game with this launcher: this page fills in "
                                  L"while it runs, and keeps the last session's numbers after it closes.";
        const float nh = app.canvas.measure_height(none, t::kBody, w);
        if (draw)
            app.canvas.text(none, t::kBody, {x, y + h, w, nh + 2}, col(app.c->label_secondary), Align::Leading, false, true);
        h += nh + t::kSpace6;
    } else {
        std::wstring head = s.time + L"  ·  " + s.mode;
        if (s.refreshHz > 0)
            head += formatted(L"  ·  %.0f Hz", s.refreshHz);
        head += s.frameGeneration ? L"  ·  Frame generation on" : L"  ·  Frame generation off";
        if (draw)
            app.canvas.text(head, t::kSubheadline, {x + t::kSpace3, y + h, w - t::kSpace3, 18}, col(app.c->label_secondary));
        h += 18 + t::kSpace4;
        const double budget = s.refreshHz > 0 ? 1000 / s.refreshHz : 0;
        std::vector<Row> rows = {
            value_row(L"Frame rate", L"", formatted(L"%.1f fps", s.fps)),
            value_row(L"Frame time", L"Median and 90th percentile: the slower frames, which you feel as stutter.",
                      s.frameP50 > 0 ? formatted(L"%.2f ms", s.frameP50) + formatted(L"  ·  %.2f ms", s.frameP90) : dash),
            value_row(L"Late frames", L"Frames that missed the headset's refresh.",
                      s.latePercent >= 0 ? formatted(L"%.1f%%", s.latePercent) : dash),
            value_row(L"CPU frame time",
                      budget > 0 ? formatted(L"The game's render thread's work per frame. Close to %.1f ms, the CPU limits the frame.",
                                             budget)
                                 : L"The game's render thread's work per frame.",
                      s.cpuMs >= 0 ? formatted(L"%.2f ms", s.cpuMs) : dash),
            value_row(L"GPU frame time", L"The whole GPU per game frame, the compositor and frame generation included.",
                      s.gpuMs >= 0 ? formatted(L"%.2f ms", s.gpuMs) + formatted(L"  ·  %.0f%% busy", s.gpuBusy) : dash),
            value_row(L"Limited by", L"", s.limited.empty() ? dash : s.limited),
            value_row(L"CPU in use", L"All of the game's threads together.", s.cores >= 0 ? formatted(L"%.1f cores", s.cores) : dash)};
        h += section(x, y + h, w, L"Last session", rows, L"", draw);
        h += t::kSpace6;
        // Against the session before it (what changed in between) and a run of the other kind (with or without QVFR).
        const int now = last_session_index();
        if (now >= 0) {
            const SessionRecord& r = app.sessions[now];
            std::vector<Row> compared;
            if (const int before = previous_session(app.sessions, now); before >= 0) {
                const SessionRecord& p = app.sessions[before];
                std::wstring desc = p.time;
                if (!r.baseline) {
                    const std::vector<std::wstring> changes = setting_changes(p.settings, r.settings);
                    desc += changes.empty() ? L"  ·  same settings" : L"  ·  after " + changes_text(changes);
                }
                compared.push_back(value_row(L"Previous session", desc, difference_text(r, p)));
            }
            if (const int other = other_kind_session(app.sessions, now); other >= 0) {
                const SessionRecord& o = app.sessions[other];
                compared.push_back(value_row(r.baseline ? L"With QVFR" : L"Without QVFR",
                                             o.time + (r.baseline ? L"" : L"  ·  the game’s own two views"), difference_text(r, o)));
            }
            if (!compared.empty()) {
                h += section(x, y + h, w, L"Compared with", compared,
                             L"This session’s frame rate and 90th percentile against each, for the same game.", draw);
                h += t::kSpace6;
            }
        }
        std::vector<Row> threads;
        for (const auto& [name, percent] : s.threads)
            threads.push_back(value_row(name, thread_note(name), formatted(L"%.0f%% of a core", percent)));
        if (threads.empty())
            threads.push_back(value_row(L"Not measured", L"", dash));
        h += section(x, y + h, w, L"Busiest threads", threads,
                     L"Each is a share of one core: near 100%, a thread can hold the frame rate back.",
                     draw);
        h += t::kSpace6;
        const bool toolkit = s.layersBelow.find(L"MBUCCHIA_toolkit") != std::wstring::npos;
        std::vector<Row> layer;
        if (s.gazeMs >= 0)
            layer.push_back(value_row(L"Eye tracking lookup", L"On the layer's own thread, not the game's.",
                                      formatted(L"%.3f ms", s.gazeMs)));
        layer.push_back(value_row(L"Layer on the render thread", L"Sim-QVFR's own time in each frame (composition apart).",
                                  s.layerMs >= 0 ? formatted(L"%.2f ms", s.layerMs) : dash));
        layer.push_back(value_row(L"Layers below", toolkit ? L"OpenXR Toolkit adds time to every frame's submission (about "
                                                             L"0.8 ms measured): switch it off for the game in OpenXR layers if you "
                                                             L"don't use it."
                                                           : L"",
                                  s.layersBelow.empty() ? L"None" : friendly_layers(s.layersBelow)));
        if (app.runtime.vramUsedMb > 0)
            layer.push_back(value_row(L"VRAM", L"The game's use against what Windows allows it.",
                                      formatted(L"%.1f GB", app.runtime.vramUsedMb / 1024) +
                                          formatted(L" of %.1f GB", app.runtime.vramBudgetMb / 1024)));
        h += section(x, y + h, w, L"Layer and memory", layer, L"", draw);
        h += t::kSpace6;
    }
    // The game's last few sessions, each with what changed since the one before it (the last session's game, else the
    // game shown).
    {
        const int now = last_session_index();
        const std::wstring game = now >= 0 ? app.sessions[now].game : game_id(games::launcher_game());
        std::vector<Row> recent;
        for (size_t i = 0; i < app.sessions.size() && recent.size() < kSessionRows; ++i) {
            const SessionRecord& r = app.sessions[i];
            if (r.game != game)
                continue;
            const int before = previous_session(app.sessions, i);
            std::wstring desc;
            if (r.baseline)
                desc = L"Without QVFR: the game’s own two views.";
            else if (before < 0)
                desc = L"The oldest kept.";
            else if (const std::vector<std::wstring> changes = setting_changes(app.sessions[before].settings, r.settings); changes.empty())
                desc = L"Same settings as the one before.";
            else
                desc = changes_text(changes, 4) + L".";
            std::wstring value = formatted(L"%.1f fps", r.fps);
            if (r.frameP90 > 0)
                value += formatted(L"  ·  %.2f ms", r.frameP90);
            recent.push_back(value_row(r.time + (static_cast<int>(i) == now ? L"  ·  last" : L""), desc, value));
        }
        if (recent.size() > 1) {
            h += section(x, y + h, w, L"Recent sessions", recent,
                         L"Frame rate and 90th percentile, with what changed since the session before.", draw);
            h += t::kSpace6;
        }
    }
    // Asking for help: the capture switch, copying the numbers above, the logs folder.
    Row capture;
    const std::wstring captureKey = key_text(app.s.hotkeys[static_cast<int>(HotkeyAction::CpuCapture)].key);
    const std::wstring captureDesc = L"At the next launch, " + (captureKey.empty() ? std::wstring(L"the CPU capture hotkey") : captureKey) +
                                     L" while driving records 20 s of where the busiest threads spend their time.";
    capture.fullHeight = [captureDesc](float width) { return checkbox_height(captureDesc, width); };
    capture.full = [captureDesc](const Rect& r) { checkbox(kDiagCapture, r, app.s.cpuProfile, L"CPU capture", captureDesc); };
    // Frame capture (probe/d3d11_capture.h): the frame the headset gets, saved at a key press.
    Row frame;
    const std::wstring frameKey = key_text(app.s.hotkeys[static_cast<int>(HotkeyAction::FrameCapture)].key);
    const std::wstring frameDesc = L"At the next launch, " + (frameKey.empty() ? std::wstring(L"the Frame capture hotkey") : frameKey) +
                                   L" saves the frame the headset gets, as PNG files in the logs folder.";
    frame.fullHeight = [frameDesc](float width) { return checkbox_height(frameDesc, width); };
    frame.full = [frameDesc](const Rect& r) {
        checkbox(kDiagFrameCapture, r, app.s.frameCapture, L"Frame capture", frameDesc);
    };
    Row baseline;
    const std::wstring baselineDesc = L"Starts the game once with QVFR off, still measuring, to compare against. The launch after "
                                      L"has QVFR again.";
    baseline.fullHeight = [baselineDesc](float width) { return checkbox_height(baselineDesc, width); };
    baseline.full = [baselineDesc](const Rect& r) {
        checkbox(kDiagBaseline, r, app.s.baselineNext, L"Next launch without QVFR", baselineDesc);
    };
    Row actions;
    actions.fullHeight = [](float) { return t::kControlHeight; };
    actions.full = [](const Rect& r) {
        const float copyW = button_width(L"Copy summary");
        const float logsW = button_width(L"Open logs folder");
        button(kDiagCopy, {r.x, r.y, copyW, t::kControlHeight}, L"Copy summary", false);
        button(kDiagLogs, {r.x + copyW + t::kSpace3, r.y, logsW, t::kControlHeight}, L"Open logs folder", false);
    };
    const std::wstring footer = L"To report a problem, use Report an issue: it zips these logs and fills in the issue.";
    h += section(x, y + h, w, L"Capture and share", {capture, frame, baseline, actions}, footer, draw, Applies::NextLaunch);
    return h;
}

}  // namespace simqvfr::launcher::ui::win

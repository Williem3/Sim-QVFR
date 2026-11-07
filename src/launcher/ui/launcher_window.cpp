// The launcher window, built natively from the QVFR launcher kit (docs/qvfr-launcher-kit): a tab per game
// across the top, a sidebar of panes in groups, cards of the kit's controls, a live preview, and an action bar
// with the status, Restore Defaults and Launch. This file has the window procedure and run_window; the state
// is in window_state.h, the panes in pane_*.cpp, the window around them in chrome.cpp, input in pointer.cpp
// and input.cpp. Settings save as they change (settings.h); Launch runs launch_game (main.cpp).
//
// Immediate mode: each paint lays out and draws the current pane and records hit rectangles; input
// acts on the last frame's rectangles and the settings, then repaints.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "../preflight.h"
#include "../report.h"
#include "../game_tabs.h"
#include "../settings.h"
#include "../session_history.h"
#include "../install.h"
#include "../update_check.h"
#include "../../games/game.h"
#include "../../input/pad_input.h"
#include "launcher_version.h"
#include "window_state.h"
#include "controls.h"
#include "form.h"

#pragma comment(lib, "comdlg32.lib")

namespace simqvfr::launcher::ui::win {

namespace {

// ---- OpenXR layers -----------------------------------------------------------------------------

// Reads both profiles again; Windows' draft is kept while it has changes (`keepDraft`).
void refresh_layers(bool keepDraft = true) {
    app.layerPlan = launcher_layer_plan();
    const bool dirty = app.layersDraft != app.layersWindows || app.layersLockMoved;
    app.layersWindows = read_system_layers(HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, kImplicitLayersKey, &app.layersLockMoved);
    if (!keepDraft || !dirty || app.layersDraft.size() != app.layersWindows.size())
        app.layersDraft = app.layersWindows;
}

int layer_rows() {
    return static_cast<int>(app.layerProfile == 0 ? app.layerPlan.layers.size() : app.layersDraft.size());
}

// A layer's name as people know it; the registered name for the rest.
std::wstring friendly_layer(const std::string& name, const std::wstring& manifest = {}) {
    static const std::pair<const char*, const wchar_t*> kKnown[] = {
        {"XR_APILAYER_MBUCCHIA_toolkit", L"OpenXR Toolkit"},
        {"XR_APILAYER_DIENERTECH_VECTORXR", L"VectorXR"},
        {"XR_APILAYER_CHEEKY_foveated_dlss", L"Cheeky Foveated DLSS"},
        {"XR_APILAYER_MBUCCHIA_quad_views_foveated", L"Quad-Views-Foveated"},
        {"XR_APILAYER_SIM_qvfr", L"Sim-QVFR"},
        {"XR_APILAYER_NOVENDOR_motion_compensation", L"OpenXR Motion Compensation"},
        {"XR_APILAYER_FREDEMMOTT_OpenKneeboard", L"OpenKneeboard"},
        {"XR_APILAYER_FREDEMMOTT_HandTrackedCockpitClicking", L"Hand Tracked Cockpit Clicking"},
    };
    for (const auto& [n, label] : kKnown)
        if (_stricmp(n, name.c_str()) == 0)
            return label;
    if (name.empty())
        return std::filesystem::path(manifest).filename().wstring();
    return widen(name);
}

// The launcher's profile changes in the ini, then the plan is read again.
void edit_launcher_layers(const std::function<void(LauncherLayers&)>& change) {
    if (!app.iniFound)
        return;
    LauncherLayers profile = read_launcher_layers(app.ini);
    change(profile);
    write_launcher_layers(app.ini, profile);
    app.iniText = ini_text();
    refresh_layers();
}

void toggle_layer(int i) {
    if (app.layerProfile == 0) {
        if (i < 0 || i >= static_cast<int>(app.layerPlan.layers.size()) || !app.layerPlan.layers[i].lock.empty())
            return;
        const PlannedLayer l = app.layerPlan.layers[i];
        edit_launcher_layers([&](LauncherLayers& p) { set_launcher_layer(p, l.name, !l.on, l.systemOn); });
    } else if (i >= 0 && i < static_cast<int>(app.layersDraft.size())) {
        app.layersDraft[i].on = !app.layersDraft[i].on;
    }
    app.layerMessage.clear();
}

void move_layer(int i, int step) {
    if (app.layerProfile == 0) {
        if (i < 0 || i >= static_cast<int>(app.layerPlan.layers.size()))
            return;
        const std::string name = app.layerPlan.layers[i].name;
        bool moved = false;
        edit_launcher_layers([&](LauncherLayers& p) { moved = move_launcher_layer(p, app.layerPlan, name, step); });
        if (moved)
            app.layerRow = i + step;
    } else if (move_system_layer(app.layersDraft, static_cast<size_t>(i), step)) {
        app.layerRow = i + step;
    }
    app.layerMessage.clear();
}

void apply_layers() {
    const std::wstring error = apply_system_layers(app.hwnd, app.layersDraft);
    refresh_layers(!error.empty());
    app.layerMessage = error.empty() ? L"Windows’ OpenXR layers updated. A backup of each list is in the logs folder." : error;
    app.layerTone = error.empty() ? 0 : 2;
}

void mini_check(int option, const Rect& box, bool on, bool disabled) {
    switch_track(box, on, disabled);
    if (!disabled)
        app.hits.push_back({kLayerOn, {box.x - 4, box.y - 4, box.w + 8, box.h + 8}, option});
}

void arrow_button(Ctl id, int option, const Rect& r, bool disabled) {
    app.canvas.fill(r, col(!disabled && pointer_in(r) ? app.c->control_hover : app.c->control_bg, disabled ? 0.5f : 1),
                    t::kRadiusControl);
    app.canvas.path(id == kLayerUp ? "M3 7.5L6 4.5l3 3" : "M3 4.5L6 7.5l3-3", {r.x + (r.w - 12) / 2, r.y + (r.h - 12) / 2, 12, 12},
                    12, col(disabled ? app.c->label_tertiary : app.c->label), 1.5f);
    if (!disabled)
        app.hits.push_back({id, r, option});
}

constexpr float kLayerControlsW = 40 + 14 + 32 + 4 + 32;

// One movable layer: its switch (disabled when locked) and its moves.
Row layer_row(const std::wstring& label, const std::wstring& desc, int index, bool on, bool switchLocked, bool canUp,
              bool canDown) {
    Row row;
    row.label = label;
    row.desc = desc;
    row.controlW = kLayerControlsW;
    row.controlH = 32;
    row.control = [=](const Rect& r) {
        mini_check(index, {r.x, r.y + 5, 40, 22}, on, switchLocked);
        arrow_button(kLayerUp, index, {r.x + 54, r.y, 32, 32}, !canUp);
        arrow_button(kLayerDown, index, {r.x + 90, r.y, 32, 32}, !canDown);
        if (focused(kLayerList) && app.layerRow == index)
            focus_ring({r.x - 2, r.y - 2, r.w + 4, r.h + 4}, t::kRadiusControl);
    };
    return row;
}

float layers_pane(float x, float y, float w, bool draw) {
    float h = 0;
    {
        const std::wstring names[2] = {kLayerProfileNames[0], kLayerProfileNames[1]};
        Row profile = choice_row(kLayerProfile, L"Profile", names, 2, app.layerProfile, false);
        profile.desc = app.layerProfile == 0
                           ? L"What " + short_name() + L" loads when you launch it from here. Windows’ own list stays as it is."
                           : L"What every other app loads, and " + short_name() + L" started from Steam. Changes Windows’ registry.";
        h += section(x, y, w, L"", {profile}, L"", draw) + t::kSpace6;
    }
    if (app.layerProfile == 0) {
        std::vector<Row> rows;
        for (const std::string& name : app.layerPlan.above)
            rows.push_back(locked_row(friendly_layer(name), L"Has no switch to turn it off per launch, so it stays above the Sim-QVFR layer.",
                                      L"Above"));
        rows.push_back(locked_row(friendly_layer(kLayerName), L"Always nearest the game.", L"Locked"));
        const int n = static_cast<int>(app.layerPlan.layers.size());
        for (int i = 0; i < n; ++i) {
            const PlannedLayer& l = app.layerPlan.layers[i];
            std::wstring desc = !l.lock.empty() ? widen(l.lock) : l.systemOn ? L"" : L"Off in Windows";
            if (friendly_layer(l.name) != widen(l.name))
                desc += (desc.empty() ? L"" : L" · ") + widen(l.name);
            rows.push_back(layer_row(friendly_layer(l.name), desc, i, l.on, !l.lock.empty(), i > 0, i + 1 < n));
        }
        h += section(x, y + h, w, L"Load order", rows,
                     L"Nearest the game first: each launch loads them below the Sim-QVFR layer in this order.", draw,
                     Applies::NextLaunch);
        Row reset;
        reset.label = L"Windows’ order";
        reset.desc = L"Forget this profile: launches follow Windows’ order and switches again.";
        reset.controlW = button_width(L"Reset");
        reset.controlH = t::kControlHeight;
        reset.control = [](const Rect& r) { button(kLayerReset, r, L"Reset", false); };
        h += t::kSpace6 + section(x, y + h + t::kSpace6, w, L"", {reset}, L"", draw);
    } else {
        for (const bool machine : {true, false}) {
            std::vector<Row> rows;
            const int n = static_cast<int>(app.layersDraft.size());
            for (int i = 0; i < n; ++i) {
                const SystemLayer& l = app.layersDraft[i];
                if (l.machine != machine)
                    continue;
                const bool up = i > 0 && app.layersDraft[i - 1].machine == machine && app.layersDraft[i - 1].lock.empty() && l.lock.empty();
                const bool down = i + 1 < n && app.layersDraft[i + 1].machine == machine && l.lock.empty();
                std::wstring desc = l.lock.empty() ? std::wstring() : widen(l.lock) + L" · ";
                desc += l.name.empty() ? L"Unreadable manifest: " + l.manifest : l.manifest;
                rows.push_back(layer_row(friendly_layer(l.name, l.manifest), desc, i, l.on, false, up, down));
            }
            if (rows.empty())
                continue;
            h += section(x, y + h, w, machine ? L"All users (HKLM)" : L"This user (HKCU)", rows,
                         machine ? L"Changing these needs administrator rights: Windows asks once when you apply."
                                 : L"Windows loads these after the machine-wide ones.",
                         draw, Applies::NextLaunch) +
                 t::kSpace6;
        }
        if (app.layersDraft.empty()) {
            if (draw)
                badge(x, y + h, 3, L"No OpenXR layers are registered in Windows.", w);
            h += 20 + t::kSpace6;
        }
        Row apply;
        apply.label = layers_dirty() ? L"Changes not applied" : L"Windows’ list";
        apply.desc = app.layersLockMoved ? L"The Sim-QVFR layer is not first in its list: applying moves it there."
                                         : L"Nearest the game first. The current list is backed up into logs before it changes.";
        const float applyW = button_width(L"Apply"),
                    revertW = button_width(L"Revert");
        apply.controlW = applyW + t::kSpace2 + revertW;
        apply.controlH = t::kControlHeight;
        const bool dirty = layers_dirty();
        apply.control = [=](const Rect& r) {
            if (dirty) {
                button(kLayerRevert, {r.x, r.y, revertW, r.h}, L"Revert", false);
                button(kLayerApply, {r.x + revertW + t::kSpace2, r.y, applyW, r.h}, L"Apply", true);
            }
        };
        h += section(x, y + h, w, L"", {apply}, L"", draw);
    }
    if (!app.layerMessage.empty()) {
        if (draw)
            badge(x, y + h + t::kSpace4, app.layerTone, app.layerMessage, w);
        h += t::kSpace4 + 20;
    }
    return h;
}

// The preview: the chain a frame goes through, the game at the top, the runtime at the bottom.
float layers_preview(float x, float y, float w, bool draw) {
    std::vector<std::pair<std::wstring, int>> chain;  // tone: 0 end, 1 locked, 2 layer
    chain.push_back({game_name(), 0});
    if (app.layerProfile == 0) {
        for (const std::string& name : app.layerPlan.above)
            chain.push_back({friendly_layer(name), 2});
        chain.push_back({friendly_layer(kLayerName), 1});
        for (const PlannedLayer& l : app.layerPlan.layers)
            if (l.on)
                chain.push_back({friendly_layer(l.name), l.lock.empty() ? 2 : 1});
    } else {
        for (const SystemLayer& l : app.layersDraft)
            if (l.on)
                chain.push_back({friendly_layer(l.name, l.manifest), l.lock.empty() ? 2 : 1});
    }
    chain.push_back({L"OpenXR runtime", 0});
    const float pill = 30, gap = 14;
    if (draw)
        for (size_t i = 0; i < chain.size(); ++i) {
            const Rect r{x, y + i * (pill + gap), w, pill};
            const auto& [text, tone] = chain[i];
            app.canvas.fill(r, col(tone == 0 ? app.c->segment_track : tone == 1 ? app.c->accent_fill : app.c->group_bg), pill / 2);
            if (tone == 2)
                app.canvas.stroke(r, col(app.c->separator), 1, pill / 2);
            app.canvas.text(text, t::kBody, {r.x + 14, r.y, r.w - 28, r.h}, col(tone == 1 ? app.c->on_accent : app.c->label),
                            Align::Center);
            if (i + 1 < chain.size()) {
                const float cx = x + w / 2, top = r.bottom() + 2, bottom = r.bottom() + gap - 2;
                app.canvas.line(cx, top, cx, bottom, col(app.c->label_secondary), 1.5f);
                app.canvas.path("M2 3l3 3 3-3", {cx - 5, bottom - 6, 10, 8}, 10, col(app.c->label_secondary), 1.5f);
            }
        }
    return chain.size() * (pill + gap) - gap;
}

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

void copy_summary() { copy_text(session_summary_text(app.runtime) + comparison_lines()); }

}  // namespace

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

namespace {

// Whether the description box's field is wholly inside the scrolling area: only then is the native edit
// shown (a child window cannot be clipped like the canvas); otherwise the canvas draws its text.
bool report_edit_fits() {
    const Rect& r = app.reportEditRect;
    return r.w > 0 && r.y >= app.scrollArea.y && r.bottom() <= app.scrollArea.bottom();
}

// "<prefix>-<local time>.zip".
std::wstring zip_name(const wchar_t* prefix) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[64];
    swprintf_s(name, L"%s-%04u%02u%02u-%02u%02u%02u.zip", prefix, now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);
    return name;
}

// The files a report zips (report_files): the session logs, the ini files, the CPU captures and hang dumps,
// the game's crash reports and the OpenXR loader's output.
std::vector<ReportFile> logs_to_zip() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const std::wstring gameCrashes = game_crashes_folder(games::launcher_game().crashReports);
    return report_files({logs_folder(), runtime_info_path(), app.ini, gameCrashes},
                        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// Explorer, `path` selected in its folder.
void show_in_explorer(const std::wstring& path) {
    const std::wstring select = L"/select,\"" + path + L"\"";
    ShellExecuteW(app.hwnd, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
}

// Export logs: the files a report zips, in logs\sim-qvfr-logs-<time>.zip with details.md (version, PC,
// headset, settings, the last session), and Explorer showing it. No issue: for logs asked for directly.
void export_logs() {
    const std::wstring logs = logs_folder();
    CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring name = zip_name(L"sim-qvfr-logs");
    reload_runtime();
    IssueReport report{widen(SIM_QVFR_VERSION), app.s, app.runtime, system_info(), name};
    report.causes = likely_causes(app.s, app.runtime);
    std::vector<ReportFile> files = logs_to_zip();
    files.push_back({L"details.md", L"", utf8(issue_details(report))});
    const std::wstring zip = logs + L"\\" + name;
    const std::wstring error = write_zip(zip, files);
    if (error.empty()) {
        app.exportZip = zip;
        show_in_explorer(zip);
    } else {
        app.status = Status::Error;
        app.error = L"Could not write the logs' zip: " + error;
        app.exportZip.clear();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// Report an issue: the logs zipped into logs\sim-qvfr-report-<time>.zip (with the issue's text as
// issue.md), Explorer showing it, and a new issue on the kind's form, its fields prefilled (report.h).
void report_issue() {
    const std::wstring logs = logs_folder();
    CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring name = zip_name(L"sim-qvfr-report");
    reload_runtime();
    IssueReport report{widen(SIM_QVFR_VERSION), app.s, app.runtime, system_info(), name, app.reportKind};
    const int length = GetWindowTextLengthW(app.reportEdit);
    report.description.resize(static_cast<size_t>(std::max(length, 0)));
    GetWindowTextW(app.reportEdit, report.description.data(), length + 1);
    for (size_t at; (at = report.description.find(L"\r\n")) != std::wstring::npos;)  // the edit's line ends
        report.description.erase(at, 1);
    report.causes = likely_causes(app.s, app.runtime);
    std::vector<ReportFile> files = logs_to_zip();
    files.push_back({L"issue.md", L"", utf8(issue_body(report))});
    const std::wstring zip = logs + L"\\" + name;
    const std::wstring error = write_zip(zip, files);
    if (error.empty()) {
        app.reportZip = zip;
        InvalidateRect(app.hwnd, nullptr, FALSE);
        show_in_explorer(zip);
    } else {
        app.status = Status::Error;
        app.error = L"Could not write the report's zip: " + error;
        report.zipName.clear();
        app.reportZip.clear();
        InvalidateRect(app.hwnd, nullptr, FALSE);
    }
    // [launcher] issue_repo: another repository with the forms, to try them before they reach this one's.
    wchar_t repo[128] = {};
    GetPrivateProfileStringW(L"launcher", L"issue_repo", kIssueRepo, repo, static_cast<DWORD>(std::size(repo)), app.ini.c_str());
    const std::string url = issue_url(repo, kIssueTemplates[std::clamp(report.kind, 0, kIssueKindCount - 1)],
                                      issue_title(report), issue_fields(report));
    ShellExecuteA(app.hwnd, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The report's zip in Explorer, selected.
void show_report_zip() { show_in_explorer(app.reportZip); }

// The zip tile held down: dragged, the zip itself (Explorer's data object for it, so a browser's drop takes it
// as a file, the issue's upload); only clicked, Explorer shows it. GitHub has no API to attach it for the user.
void drag_report_zip(int px, int py) {
    if (GetFileAttributesW(app.reportZip.c_str()) == INVALID_FILE_ATTRIBUTES) {  // deleted meanwhile
        app.reportZip.clear();
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    POINT pt{px, py};
    ClientToScreen(app.hwnd, &pt);
    if (!DragDetect(app.hwnd, pt)) {
        show_report_zip();
        return;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(app.reportZip.c_str(), nullptr, &pidl, 0, nullptr)))
        return;
    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IDataObject* data = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&folder), &child))) {
        folder->GetUIObjectOf(app.hwnd, 1, &child, IID_IDataObject, nullptr, reinterpret_cast<void**>(&data));
        folder->Release();
    }
    if (data) {
        DWORD effect = 0;
        SHDoDragDrop(app.hwnd, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        data->Release();
    }
    CoTaskMemFree(pidl);
}

float report_zip_section(float x, float y, float w, bool draw);

// The Report an issue pane: what the last session points to, then the kind of problem, what happened and the
// button (report_issue).
float report_pane(float x, float y, float w, bool draw) {
    float h = 0;
    // Known causes the last session shows come first: many problems end there.
    const std::vector<Cause> causes = likely_causes(app.s, app.runtime);
    if (!causes.empty()) {
        std::vector<Row> rows;
        for (const Cause& c : causes) {
            Row row;
            row.label = c.title;
            row.desc = c.fix;
            row.control = [](const Rect&) {};  // text only
            rows.push_back(row);
        }
        h += section(x, y + h, w, L"Before you report", rows,
                     L"What the launcher noticed about the last session, and what to try. The report includes these.", draw);
        h += t::kSpace6;
    }
    Row kind;
    kind.label = L"Kind of problem";
    const std::vector<std::wstring> kinds(std::begin(kIssueKinds), std::end(kIssueKinds));
    kind.controlW = popup_width(kinds.data(), kIssueKindCount, 160);
    kind.controlH = t::kControlHeight;
    kind.control = [](const Rect& r) { popup_button(kDiagKind, r, kIssueKinds[app.reportKind], false); };
    Row description;
    constexpr float kEditH = 96;
    description.fullHeight = [](float) { return t::kBody.lineHeight + t::kSpace2 + kEditH; };
    description.full = [](const Rect& r) {
        app.canvas.text(L"What happened?", t::kBody, {r.x, r.y, r.w, t::kBody.lineHeight}, col(app.c->label));
        const Rect field{r.x, r.y + t::kBody.lineHeight + t::kSpace2, r.w, kEditH};
        app.canvas.fill(field, col(app.c->field_bg), t::kRadiusControl);
        app.canvas.stroke(field, col(app.c->control_border), 1, t::kRadiusControl);
        if (focused(kDiagDescription) || GetFocus() == app.reportEdit)
            focus_ring(field, t::kRadiusControl);
        app.reportEditRect = {field.x + 6, field.y + 5, field.w - 12, field.h - 10};
        app.hits.push_back({kDiagDescription, field});
        if (!IsWindowVisible(app.reportEdit) || !report_edit_fits()) {  // the edit is hidden: its text, clipped here
            std::wstring text(static_cast<size_t>(GetWindowTextLengthW(app.reportEdit)), L'\0');
            GetWindowTextW(app.reportEdit, text.data(), static_cast<int>(text.size()) + 1);
            app.canvas.push_clip(field);
            app.canvas.text(text.empty() ? L"What you saw, and what you expected instead." : text, t::kBody,
                            app.reportEditRect, col(text.empty() ? app.c->label_secondary : app.c->label), Align::Leading,
                            false, true);
            app.canvas.pop_clip();
        }
    };
    Row report;
    report.fullHeight = [](float) { return t::kControlHeight; };
    report.full = [](const Rect& r) {
        const float reportW = button_width(L"Report an issue");
        button(kDiagReport, {r.x, r.y, reportW, t::kControlHeight}, L"Report an issue", true);
    };
    h += section(x, y + h, w, L"Describe the problem", {kind, description, report},
                 L"Opens a GitHub issue with the details filled in, and zips the logs for you to drag into it.",
                 draw);
    if (!app.reportZip.empty()) {
        h += t::kSpace6;
        h += report_zip_section(x, y + h, w, draw);
    }
    // The logs alone, when they're asked for without an issue.
    Row exportRow;
    exportRow.label = L"Export logs";
    if (app.exportZip.empty()) {
        exportRow.desc = L"Zips Sim-QVFR’s and the game’s logs, and shows the zip in Explorer.";
    } else {
        const size_t slash = app.exportZip.find_last_of(L'\\');
        exportRow.desc = L"Saved " + (slash == std::wstring::npos ? app.exportZip : app.exportZip.substr(slash + 1));
    }
    exportRow.controlW = button_width(L"Export logs");
    exportRow.controlH = t::kControlHeight;
    exportRow.control = [](const Rect& r) { button(kDiagExport, r, L"Export logs", false); };
    h += t::kSpace6;
    h += section(x, y + h, w, L"Send logs only", {exportRow}, L"When you’ve been asked for logs: no GitHub issue.", draw);
    return h;
}

// The report's zip, to drag from here onto the issue: the step reports most often miss.
float report_zip_section(float x, float y, float w, bool draw) {
    Row zip;
    constexpr float kTileH = 56;
    zip.fullHeight = [](float) { return kTileH; };
    zip.full = [](const Rect& r) {
        const Rect tile{r.x, r.y, r.w, kTileH};
        const bool over = pointer_in(tile);
        app.canvas.fill(tile, col(app.c->accent_fill, over ? 0.16f : 0.1f), t::kRadiusControl);
        app.canvas.stroke(tile, col(app.c->accent, 0.6f), 1, t::kRadiusControl);
        if (focused(kDiagZip))
            focus_ring(tile, t::kRadiusControl);
        // A file with a zipper.
        constexpr const char* kIconZip = "M4 1.75h5.5l3 3v9.5H4zM9.5 1.75v3h3M7 3h1M7 5h1M7 7h1M6.75 9h1.5v2h-1.5z";
        const float iw = 28;
        app.canvas.path(kIconZip, {tile.x + t::kSpace4, tile.y + (kTileH - iw) / 2, iw, iw}, 16, col(app.c->accent), 1.4f);
        const float tx = tile.x + t::kSpace4 + iw + t::kSpace3, tw = tile.right() - t::kSpace4 - tx;
        const size_t slash = app.reportZip.find_last_of(L'\\');
        const std::wstring name = slash == std::wstring::npos ? app.reportZip : app.reportZip.substr(slash + 1);
        app.canvas.text(L"Drag this onto the GitHub issue", t::kHeadline, {tx, tile.y + 9, tw, t::kHeadline.lineHeight},
                        col(app.c->label));
        app.canvas.text(name + L"  ·  click to show it in Explorer", t::kCallout,
                        {tx, tile.y + 9 + t::kHeadline.lineHeight + 2, tw, t::kCallout.lineHeight}, col(app.c->label_secondary));
        app.hits.push_back({kDiagZip, tile});
    };
    return section(x, y, w, L"Attach the logs", {zip},
                   L"GitHub can’t attach it for you: drop it into the issue’s “Screenshots and logs” box.", draw);
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

// The wheels and controllers DirectInput finds (opened when the Hotkeys pane shows), for the pane's note.
void open_pads() {
    if (!app.padsOpen)
        app.padsOpen = app.pads.open(app.hwnd);
    else
        app.pads.rescan();  // a wheel plugged in since
}

std::wstring pads_found() {
    if (!app.padsOpen)
        return L"DirectInput isn’t available: wheel and controller buttons can’t be read.";
    std::wstring names;
    for (const auto& [id, name] : app.pads.devices())
        names += (names.empty() ? L"" : L", ") + name;
    return names.empty() ? L"No wheel or controller found: plug it in, then come back to this pane."
                         : L"Found: " + names + L".";
}

// Hotkeys ([hotkeys], input/hotkeys.h): each action's key and wheel button. Click one, then press.
float hotkeys_pane(float x, float y, float w, bool draw) {
    static const wchar_t* const kDescriptions[kHotkeyActions] = {
        L"Opens the headset menu, or closes it.",
        L"Selects the line above.",
        L"Selects the line below.",
        L"Lowers the selected value.",
        L"Raises the selected value.",
        L"Turns the performance overlay on or off.",
        L"Steps the helmet’s visor: closed, half, open.",
        L"Records a CPU capture, with CPU capture on in the Diagnostics pane.",
        L"Saves the frame the headset gets, with Frame capture on in the Diagnostics pane."};
    const float slotW = 136;
    const auto row = [slotW](HotkeyAction action) {
        const int a = static_cast<int>(action);
        const Hotkey& h = app.s.hotkeys[a];
        Row r;
        r.label = kHotkeyLabels[a];
        r.desc = kDescriptions[a];
        if (h.pad.set())
            r.desc += L" Wheel button on " + (h.pad.name.empty() ? h.pad.device : h.pad.name) + L".";
        if (const int other = hotkey_clash(app.s.hotkeys, action, false); other >= 0)
            r.desc += L" " + key_text(h.key) + L" is also " + kHotkeyLabels[other] + L".";
        if (const int other = hotkey_clash(app.s.hotkeys, action, true); other >= 0)
            r.desc += L" Its wheel button is also " + std::wstring(kHotkeyLabels[other]) + L".";
        r.controlW = 2 * slotW + t::kSpace2;
        r.controlH = t::kControlHeight;
        r.control = [a, slotW](const Rect& rect) {
            const Hotkey& k = app.s.hotkeys[a];
            const bool keyCapture = app.captureAction == a && !app.capturePad, padCapture = app.captureAction == a && app.capturePad;
            const std::wstring keyLabel = keyCapture ? L"Press keys…" : k.key.set() ? key_text(k.key) : L"No key";
            const std::wstring padLabel = padCapture ? L"Press a button…" : k.pad.set() ? control_text(k.pad.control) : L"No wheel button";
            button(kHotkeyKey, {rect.x, rect.y, slotW, rect.h}, keyLabel, keyCapture);
            app.hits.back().option = a;
            button(kHotkeyPad, {rect.x + slotW + t::kSpace2, rect.y, slotW, rect.h}, padLabel, padCapture);
            app.hits.back().option = a;
        };
        return r;
    };
    float h = 0;
    h += section(x, y + h, w, L"Headset menu",
                 {row(HotkeyAction::MenuToggle), row(HotkeyAction::MenuUp), row(HotkeyAction::MenuDown),
                  row(HotkeyAction::MenuLeft), row(HotkeyAction::MenuRight)},
                 pads_found(),
                 draw, Applies::Live) +
         t::kSpace6;
    std::vector<Row> inGame = {row(HotkeyAction::Overlay)};
    if (game_has(&games::GameFeatures::helmet))  // the visor steps the helmet overlay
        inGame.push_back(row(HotkeyAction::Visor));
    inGame.insert(inGame.end(), {row(HotkeyAction::CpuCapture), row(HotkeyAction::FrameCapture)});
    h += section(x, y + h, w, L"In the game", inGame,
                 L"Click a button, then press the keys or a wheel button: Esc cancels, Delete clears it.",
                 draw, Applies::Live) +
         t::kSpace6;
    Row reset;
    reset.fullHeight = [](float) { return t::kControlHeight; };
    reset.full = [](const Rect& r) {
        const float resetW = button_width(L"Reset hotkeys");
        button(kHotkeysReset, {r.x, r.y, resetW, t::kControlHeight}, L"Reset hotkeys", false);
    };
    h += section(x, y + h, w, L"", {reset}, L"Back to the Ctrl+Alt keys, without wheel buttons. Restore Defaults leaves them as they are.",
                 draw);
    return h;
}

// ---- Games ------------------------------------------------------------------------------------

// Which games are installed (launcher.h game_installed), by id: looked up when the window opens, comes back to the front
// or a game's folder is chosen.
void refresh_installed() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    for (std::size_t i = 0; i < count; ++i)
        app.installed[game_id(*all[i])] = game_installed(*all[i]);
}

}  // namespace

bool installed(const games::GameInfo& game) {
    const auto it = app.installed.find(game_id(game));
    return it == app.installed.end() || it->second;
}

namespace {

// A game has a tab when it is installed and not hidden, or when the user gave it one (one Steam doesn't find, installed
// elsewhere: its folder is chosen in Launch checks).
bool game_shown(const games::GameInfo& game) {
    return installed(game) ? !game_hidden(app.s.hiddenGames, game_id(game)) : game_hidden(app.s.shownGames, game_id(game));
}

int shown_game_count() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    int shown = 0;
    for (std::size_t i = 0; i < count; ++i)
        shown += game_shown(*all[i]) ? 1 : 0;
    return shown;
}

// The game whose colour the wheel edits: the one picked by its swatch, else the selected game.
int color_game_index() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    if (app.colorGame >= 0 && static_cast<std::size_t>(app.colorGame) < count)
        return app.colorGame;
    for (std::size_t i = 0; i < count; ++i)
        if (all[i] == &games::launcher_game())
            return static_cast<int>(i);
    return 0;
}

const games::GameInfo& color_game() {
    std::size_t count = 0;
    return *games::all_games(count)[color_game_index()];
}

// The wheel's hue, saturation and brightness from the edited game's colour (its own, else its palette's fill).
void load_color_edit() {
    const int index = color_game_index();
    if (app.colorLoaded == index)
        return;
    app.colorLoaded = index;
    const auto it = app.s.gameColors.find(game_id(color_game()));
    const uint32_t rgb = it != app.s.gameColors.end() ? it->second : builtin_accent(color_game()).accent_fill;
    float value = 1;
    rgb_hsv(rgb, app.colorHue, app.colorSat, value);
    app.colorValue = static_cast<int>(std::lround(value * 100));
}

void apply_color_edit() {
    app.s.gameColors[game_id(color_game())] = hsv_rgb(app.colorHue, app.colorSat, app.colorValue / 100.0f);
    apply_theme();  // the selected game's accent may be the one changed
}

// A point on the wheel: its angle the hue, its distance from the centre the saturation (the rim: full).
void set_color_from(float x, float y) {
    const Rect& r = app.colorWheel;
    if (r.w <= 0)
        return;
    const float radius = r.w / 2, dx = (x - (r.x + radius)) / radius, dy = (y - (r.y + radius)) / radius;
    app.colorSat = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
    float hue = std::atan2(-dy, dx) * 180.0f / 3.14159265f;
    app.colorHue = hue < 0 ? hue + 360 : hue;
    apply_color_edit();
    changed(false);  // saved once the pointer lets go (WM_LBUTTONUP), as a slider is
}

// The colour wheel at `r` (square): hue round it, saturation outward, at the brightness set; a ring marks the colour.
void color_wheel(const Rect& r) {
    static std::vector<uint32_t> pixels;
    static int madeSize = -1, madeValue = -1;
    const int n = std::max(16, static_cast<int>(std::lround(r.w * app.canvas.dpi_scale())));
    if (n != madeSize || app.colorValue != madeValue) {
        madeSize = n;
        madeValue = app.colorValue;
        pixels.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0);
        const float half = n / 2.0f, value = app.colorValue / 100.0f;
        for (int py = 0; py < n; ++py)
            for (int px = 0; px < n; ++px) {
                const float dx = (px + 0.5f - half) / half, dy = (py + 0.5f - half) / half;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float alpha = std::clamp((1.0f - d) * half + 0.5f, 0.0f, 1.0f);  // a smooth rim
                if (alpha <= 0)
                    continue;
                float hue = std::atan2(-dy, dx) * 180.0f / 3.14159265f;
                const uint32_t rgb = hsv_rgb(hue < 0 ? hue + 360 : hue, std::min(d, 1.0f), value);
                const auto pre = [alpha](uint32_t c) { return static_cast<uint32_t>(std::lround(c * alpha)); };
                pixels[static_cast<std::size_t>(py) * n + px] = static_cast<uint32_t>(std::lround(alpha * 255)) << 24 |
                                                                pre((rgb >> 16) & 0xff) << 16 | pre((rgb >> 8) & 0xff) << 8 |
                                                                pre(rgb & 0xff);
            }
    }
    app.canvas.image(r, pixels, n, n, static_cast<uint64_t>(n) * 1000 + static_cast<uint64_t>(app.colorValue) + 1);
    const float radius = r.w / 2, angle = app.colorHue * 3.14159265f / 180.0f;
    const float mx = r.x + radius + std::cos(angle) * app.colorSat * radius, my = r.y + radius - std::sin(angle) * app.colorSat * radius;
    app.canvas.circle(mx, my, 7, col(hsv_rgb(app.colorHue, app.colorSat, app.colorValue / 100.0f)), true);
    app.canvas.circle(mx, my, 7.5f, col(0xffffff), false, 2);
    app.canvas.circle(mx, my, 9, col(0x000000, 0.35f), false, 1);
    app.colorWheel = r;
    app.hits.push_back({kColorWheel, r});
}

float games_pane(float x, float y, float w, bool draw) {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    load_color_edit();
    const int editing = color_game_index();
    const bool onlyOne = shown_game_count() <= 1;
    std::vector<Row> rows;
    for (std::size_t i = 0; i < count; ++i) {
        const games::GameInfo* game = all[i];
        const bool shown = game_shown(*game), locked = shown && onlyOne;  // the last tab stays
        const bool found = installed(*game);
        const std::wstring desc = locked  ? L"The last tab: show another game to hide this one."
                                  : !found ? shown ? L"Has a tab: choose its folder in Launch checks."
                                                   : L"Steam doesn’t list it. Turn it on if it’s installed elsewhere."
                                  : shown  ? L"Has a tab at the top of the window."
                                           : L"No tab. Its settings are kept for when you show it again.";
        Row row;
        row.fullHeight = [desc](float width) { return checkbox_height(desc, width - 36); };
        row.full = [=](const Rect& r) {
            const t::Accent a = game_accent(*game);
            const Rect swatch{r.x, r.y + (r.h - 22) / 2, 22, 22};
            app.canvas.circle(swatch.x + 11, swatch.y + 11, 9, col(a.accent_fill), true);
            if (static_cast<int>(i) == editing)  // the game the wheel below edits
                app.canvas.circle(swatch.x + 11, swatch.y + 11, 11.5f, col(app.c->label), false, 1.5f);
            if (focused(kColorGame) && app.hoverOption == static_cast<int>(i))
                focus_ring(swatch, 11);
            app.hits.push_back({kColorGame, swatch.inset(-4, -4), static_cast<int>(i)});
            const Rect rest{r.x + 36, r.y, r.w - 36, r.h};
            const float tw = rest.w - kSwitchW - t::kSpace5;
            const float descH = app.canvas.measure_height(desc, t::kCallout, tw);
            const float ty = rest.y + (rest.h - t::kBody.lineHeight - 2 - descH) / 2;
            app.canvas.text(game->name, t::kBody, {rest.x, ty, tw, t::kBody.lineHeight}, col(app.c->label));
            if (!found) {  // tagged: Steam doesn't have it
                const float nx = rest.x + std::ceil(app.canvas.measure(game->name, t::kBody)) + t::kSpace2;
                badge(nx, ty, 3, L"Not found", std::max(0.0f, rest.x + tw - nx));
            }
            app.canvas.text(desc, t::kCallout, {rest.x, ty + t::kBody.lineHeight + 2, tw, descH + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
            switch_track({rest.right() - kSwitchW, rest.y + (rest.h - kSwitchH) / 2, kSwitchW, kSwitchH}, shown, locked);
            if (!locked)
                app.hits.push_back({kGameShown, rest, static_cast<int>(i)});
        };
        rows.push_back(row);
    }
    float h = section(x, y, w, L"Game tabs", rows,
                      L"Installed games get a tab; turn one off to hide it. Click a colour dot to change it below.", draw);
    h += t::kSpace6;
    const games::GameInfo& game = color_game();
    const bool custom = app.s.gameColors.count(game_id(game)) > 0;
    Row wheel;
    wheel.fullHeight = [](float) { return 184.0f; };
    wheel.full = [&game](const Rect& r) {
        color_wheel({r.x, r.y, r.h, r.h});
        // What it looks like: the game's tab and a button in its colours.
        const t::Accent a = game_accent(game);
        const float px = r.x + r.h + t::kSpace6, pw = std::min(260.0f, r.right() - px);
        if (pw < 120)
            return;
        const Rect tab{px, r.y + 40, pw, 34};
        app.canvas.fill(tab, col(a.accent_fill, app.dark ? 0.22f : 0.12f), t::kRadiusControl);
        app.canvas.circle(tab.x + 18, tab.y + tab.h / 2, 4, col(a.accent), true);
        app.canvas.text(game.name, t::kBody, {tab.x + 30, tab.y, tab.w - 34, tab.h}, col(app.c->label));
        app.canvas.fill({tab.x + 10, tab.bottom() - 3, tab.w - 20, 3}, col(a.accent), 1.5f);
        const Rect button{px, tab.bottom() + 16, std::min(pw, 140.0f), t::kControlHeight};
        app.canvas.fill(button, col(a.accent_fill), t::kRadiusControl);
        app.canvas.text(L"Launch", kButtonLabel, button, col(a.on_accent), Align::Center);
        app.canvas.text(L"Preview", t::kSubheadline, {px, r.y + 14, pw, 18}, col(app.c->label_secondary));
    };
    Row reset;
    reset.label = L"Built-in colour";
    reset.desc = custom ? L"Go back to " + std::wstring(game.shortName) + L"’s own colour." : L"In use.";
    reset.controlW = button_width(L"Use default");
    reset.controlH = t::kControlHeight;
    if (custom)
        reset.control = [](const Rect& r) { button(kColorReset, r, L"Use default", Btn::Secondary); };
    h += section(x, y + h, w, L"Colour for " + std::wstring(game.name),
                 {wheel, slider_row(kColorValue, L"Brightness", L"", app.colorValue, L""), reset},
                 L"Drag in the wheel to pick a colour; it applies at once.", draw);
    return h;
}

float pane_content(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool qvfr = app.s.mode == RenderMode::Qvfr;
    float h = 0;
    switch (app.pane) {
    case Pane::Checks: h += checks_pane(x, y, w, draw); break;
    case Pane::Hotkeys: h += hotkeys_pane(x, y, w, draw); break;
    case Pane::Profiles: h += profiles_pane(x, y, w, draw); break;
    case Pane::Resolution: {
        const bool known = app.runtime.known();
        const std::wstring footer =
            known ? L"Percent of the headset’s recommended resolution (" + std::to_wstring(app.runtime.width) + L" × " +
                        std::to_wstring(app.runtime.height) + L" px per eye). Outer and inner are set independently."
                  : L"Percent of the headset’s recommended resolution. Pixel sizes appear after the first launch. Outer and inner are "
                    L"set independently.";
        std::vector<Row> rows = {
            slider_row(kOuter, L"Outer resolution", L"The full field of view, rendered at lower density.", app.s.outer,
                       known ? px(ro.outer) : L""),
            slider_row(kInner, L"Inner resolution",
                       qvfr ? L"Pixel density inside the focus region."
                            : L"Set by outer resolution and foveated size in Two-loop Foveated Interlaced.",
                       qvfr ? app.s.inner : static_cast<int>(std::lround(ro.focusDensity * 100)),
                       known && qvfr ? px(ro.focus) : L"", !qvfr),
        };
        h += section(x, y, w, L"Per-eye resolution", rows, footer, draw, qvfr ? Applies::LiveLowered : Applies::NextLaunch);
        // Object detail (X-Plane): the LOD the focus views need, or less of it for main-thread time.
        if (game_has(&games::GameFeatures::objectDetail)) {
            h += t::kSpace6;
            h += section(x, y + h, w, L"Scenery",
                         {slider_row(kObjectDetail, L"Object detail",
                                     qvfr ? L"Tree and object detail where you look, against CPU time."
                                          : L"Available in Quad View Foveated Rendering.",
                                     app.s.objectDetail, L"", !qvfr)},
                         L"100% keeps trees and objects as detailed in the focus region as at its density; 0% is the "
                         L"game’s own detail for the outer resolution.",
                         draw, Applies::Live);
        }
        // The FOV crop (each side on its own, QVFR only) and the helmet overlay: left out for a game without them.
        if (game_has(&games::GameFeatures::fovCrop)) {
            h += t::kSpace6;
            Row cropOn;
            const std::wstring cropDesc = qvfr ? L"Skips the edges of each eye that a helmet covers, so they are never rendered."
                                               : L"Available in Quad View Foveated Rendering.";
            cropOn.fullHeight = [cropDesc](float width) { return checkbox_height(cropDesc, width); };
            cropOn.full = [cropDesc](const Rect& r) { checkbox(kCropOn, r, app.s.crop, L"Crop the view", cropDesc, false); };
            const bool cropOff = !qvfr || !app.s.crop;
            h += section(x, y + h, w, L"Field of view crop",
                         {cropOn, slider_row(kCropTop, L"Top", L"", app.s.cropTop, L"", cropOff),
                          slider_row(kCropBottom, L"Bottom", L"", app.s.cropBottom, L"", cropOff),
                          slider_row(kCropOuter, L"Outer side", L"Toward the temple, mirrored for each eye.", app.s.cropOuter, L"",
                                     cropOff),
                          slider_row(kCropInner, L"Inner side", L"Toward the nose.", app.s.cropInner, L"", cropOff)},
                         L"Share of each eye’s view removed at that side; the headset shows black there.",
                         draw, Applies::Live);
        }
        if (game_has(&games::GameFeatures::helmet)) {
            h += t::kSpace6;
            Row helmetOn;
            const std::wstring helmetDesc = qvfr ? L"Draws a helmet’s visor opening around the view, lit by the scene."
                                                 : L"Available in Quad View Foveated Rendering.";
            helmetOn.fullHeight = [helmetDesc](float width) { return checkbox_height(helmetDesc, width); };
            helmetOn.full = [helmetDesc](const Rect& r) {
                checkbox(kHelmetOn, r, app.s.helmet.on, L"Show a helmet", helmetDesc, false);
            };
            const bool helmetOff = !qvfr || !app.s.helmet.on;
            h += section(x, y + h, w, L"Helmet overlay",
                         {helmetOn,
                          slider_row(kHelmetRim, L"Rim", L"How far the padding reaches into the view.", app.s.helmet.rim, L"",
                                     helmetOff),
                          slider_row(kHelmetCorners, L"Corners", L"Rounds the visor opening.", app.s.helmet.corners, L"",
                                     helmetOff),
                          slider_row(kHelmetLight, L"Lighting", L"How much the scene lights the helmet.", app.s.helmet.light, L"",
                                     helmetOff),
                          [&] {
                              const std::wstring names[3] = {kVisorPositionNames[0], kVisorPositionNames[1], kVisorPositionNames[2]};
                              Row visor = choice_row(kVisor, L"Visor", names, 3, static_cast<int>(app.s.helmet.visor), helmetOff);
                              visor.desc = L"Your images only. Ctrl+Alt+V in the game steps it.";
                              return visor;
                          }(),
                          slider_row(kHelmetSize, L"Size", L"Your images only: how wide they are, in degrees of view.",
                                     app.s.helmet.size, L"", helmetOff, L"°"),
                          slider_row(kHelmetHeight, L"Height", L"Your images only: degrees above straight ahead.",
                                     app.s.helmet.height, L"", helmetOff, L"°"),
                          slider_row(kHelmetDepth, L"Depth",
                                     L"Your images only: how far in front of your eyes, in cm. 0 is far away, with no depth.",
                                     app.s.helmet.depth, L"", helmetOff, L" cm"),
                          [&] {
                              const std::wstring names[2] = {L"Performance (2048)", L"Detail (4096)"};
                              Row detail = choice_row(kHelmetDetail, L"Detail", names, 2, app.s.helmet.detail ? 1 : 0, helmetOff);
                              detail.desc = L"Your images only: Detail is sharper, Performance costs less GPU and memory.";
                              return detail;
                          }()},
                         L"Your own helmet: top.png, bottom.png and visor.png in the helmet folder next to the layer.",
                         draw, Applies::Live);
        }
        break;
    }
    case Pane::Foveation: {
        if (eye_warning()) {
            if (draw)
                badge(x, y, 1, L"Eye tracker not detected. The focus region stays centered until one connects.", w);
            h += 20 + t::kSpace6;
        }
        Row method, mode;
        method.label = L"Rendering method";
        {
            const std::wstring names[2] = {kMethodNames[0], kMethodNames[1]};
            method.controlW = popup_width(names, 2, 180);
        }
        method.controlH = t::kControlHeight;
        method.control = [](const Rect& r) { popup_button(kMethod, r, kMethodNames[app.s.mode == RenderMode::Qvfr ? 0 : 1], false); };
        mode.label = L"Foveated mode";
        mode.desc = app.s.eyeTracking ? L"The focus region follows your gaze." : L"The focus region stays at the center of each lens.";
        {
            const std::wstring names[2] = {kModeNames[0], kModeNames[1]};
            mode.controlW = popup_width(names, 2, 140);
        }
        mode.controlH = t::kControlHeight;
        mode.control = [](const Rect& r) { popup_button(kMode, r, kModeNames[app.s.eyeTracking ? 0 : 1], false); };
        h += section(x, y + h, w, L"Mode", method_shown() ? std::vector<Row>{method, mode} : std::vector<Row>{mode},
                     qvfr ? L"Eye tracked or fixed: whether the sharp focus region follows your eyes."
                          : L"Two-loop Foveated Interlaced is legacy: it renders each headset frame twice. Switch to Quad View "
                            L"Foveated Rendering; Two-loop won't be offered again.",
                     draw, method_shown() ? Applies::NextLaunch : Applies::Live);
        h += t::kSpace6;
        h += section(x, y + h, w, L"Focus region",
                     {slider_row(kWidth, L"Foveated width", L"", app.s.width, L""),
                      slider_row(kHeight, L"Foveated height", L"", app.s.height, L""),
                      slider_row(kCorner, L"Corner radius",
                                 app.s.shape.corners ? L"Box shape only." : L"Rounds the corners of the focus region.",
                                 app.s.corner, L"", app.s.shape.corners),
                      slider_row(kFeather, L"Feather", L"Softens the edge of the focus region.", app.s.feather, L""),
                      app.s.eyeTracking
                          ? slider_row(kFocusOffset, L"Gaze offset",
                                       L"Moves the focus region above (+) or below (−) where you look, if it sits off.",
                                       app.s.offset.gaze, L"", false, L"°")
                          : slider_row(kFocusOffset, L"Vertical position",
                                       L"Raises (+) or lowers (−) the fixed focus region, e.g. toward the road or the dash.",
                                       app.s.offset.fixed, L"", false, L"°"),
                      slider_row(kGazeDeadzone, L"Gaze dead-zone",
                                 L"How far your gaze moves before the focus region follows. Higher holds it steadier "
                                 L"against eye-tracker jitter (shimmer with MSAA); 0 follows every sample.",
                                 app.s.gazeDeadzone, L"", !app.s.eyeTracking, L"°", true)},
                     L"Share of the outer view that the focus view covers. Fixed and eye-tracked modes keep their own "
                     L"vertical offset.",
                     draw, Applies::Live);
        h += t::kSpace6;
        {
            const std::wstring names[2] = {kShapeNames[0], kShapeNames[1]};
            Row shape = choice_row(kShape, L"Shape", names, 2, app.s.shape.corners ? 1 : 0, false);
            shape.desc = L"Box: a rectangle, with the corner radius above. Corners: move each corner in on its own.";
            const bool off = !app.s.shape.corners;
            const auto corner = [&](Ctl id, const wchar_t* label) {
                return slider_row(id, label, L"", *slider_value(id), L"", off);
            };
            h += section(x, y + h, w, L"Focus shape",
                         {shape, corner(kShapeTopLeftIn, L"Top left in"), corner(kShapeTopLeftDown, L"Top left down"),
                          corner(kShapeTopRightIn, L"Top right in"), corner(kShapeTopRightDown, L"Top right down"),
                          corner(kShapeBottomLeftIn, L"Bottom left in"), corner(kShapeBottomLeftUp, L"Bottom left up"),
                          corner(kShapeBottomRightIn, L"Bottom right in"),
                          corner(kShapeBottomRightUp, L"Bottom right up")},
                         L"Pull the top corners in for a trapezoid: more of the dash sharp, at the same cost.",
                         draw, Applies::Live);
        }
        break;
    }
    case Pane::Overlay: {
        Row show;
        const std::wstring desc = L"Draws a colored border around the focus region.";
        show.fullHeight = [desc](float width) { return checkbox_height(desc, width); };
        show.full = [desc](const Rect& r) { checkbox(kOutline, r, app.s.outline, L"Show focus outline", desc); };
        Row color;
        color.label = L"Outline color";
        color.disabled = !app.s.outline;
        color.controlW = segmented_width();
        color.controlH = t::kControlHeight;
        color.control = [](const Rect& r) { segmented(kColor, r, static_cast<int>(app.s.color), !app.s.outline); };
        h += section(x, y, w, L"Focus overlay",
                     {show, color},
                     L"The outline is drawn into the headset image, to show where the focus region is.", draw, Applies::Live);
        break;
    }
    case Pane::Dlss: {
        if (!game_has(&games::GameFeatures::foveatedDlss)) {
            Row note;
            note.label = L"Cheeky Foveated DLSS";
            note.desc = not_yet() + L" The launch leaves it off for this game.";
            note.disabled = true;
            h += section(x, y, w, L"Foveated DLSS", {note}, L"", draw);
            break;
        }
        // Cheeky's author asked for this credit, with the version, wherever the UI controls Cheeky.
        const std::wstring credit = cheeky_version_label(app.runtime.cheekyVersion) +
                                    (app.runtime.cheekyVersion.empty() ? L"  ·  its version shows after a launch with it" : L"");
        if (draw)
            app.canvas.text(credit, t::kSubheadline, {x + t::kSpace3, y, w - t::kSpace3, 16}, col(app.c->label_secondary));
        h += 16 + t::kSpace4;
        const std::wstring runtime = cheeky_runtime_path(app.ini, layer_folder());
        const bool found = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        const CheekyLayer layer = cheeky_layer_state();
        const auto badge_row = [](const std::wstring& label, const std::wstring& desc, int tone, const std::wstring& text) {
            Row row;
            row.label = label;
            row.desc = desc;
            row.controlW = badge_width(text);
            row.controlH = 20;
            row.control = [tone, text](const Rect& r) { badge(r.x, r.y, tone, text); };
            return row;
        };
        Row use;
        const std::wstring useDesc = L"Loads Cheeky’s DLSS runtime into " + game_name() + L", so the outer views can use a "
                                     L"cheaper DLSS pass.";
        use.fullHeight = [useDesc](float width) { return checkbox_height(useDesc, width); };
        use.full = [useDesc](const Rect& r) { checkbox(kCheekyUse, r, app.s.cheeky.load, L"Use Cheeky Foveated DLSS", useDesc); };
        Row rt;
        rt.label = L"Runtime";
        rt.desc = (found ? L"" : L"Not found: ") + runtime;
        rt.controlW = button_width(L"Choose…");
        rt.controlH = t::kControlHeight;
        rt.control = [](const Rect& r) { button(kCheekyRuntime, r, L"Choose…", false); };
        const Row xr = layer == CheekyLayer::Enabled
                           ? badge_row(L"OpenXR layer", L"Cheeky’s layer tells its runtime the frame timing it needs.", 0, L"Installed")
                       : layer == CheekyLayer::Disabled
                           ? badge_row(L"OpenXR layer", L"Registered but turned off: run CheekyOpenXRSetup.exe again.", 1, L"Turned off")
                           : badge_row(L"OpenXR layer", L"Install it with CheekyOpenXRSetup.exe from Cheeky’s release.", 2, L"Not installed");
        const std::wstring& status = app.runtime.cheekyStatus;
        const std::wstring last = status == L"running"   ? L"Cheeky ran at the last launch."
                                  : status == L"missing" ? L"At the last launch, Cheeky’s runtime was not found."
                                  : status == L"refused" ? L"At the last launch, Cheeky did not start: see its log next to its runtime."
                                                         : L"";
        h += section(x, y + h, w, L"Cheeky Foveated DLSS", {use, rt, xr},
                     L"Put the CheekyFoveatedDLSS folder from Cheeky’s Standalone ZIP next to sim-qvfr.dll, or choose "
                     L"its runtime. " + last,
                     draw, Applies::NextLaunch) + t::kSpace6;
        const bool outer = app.s.cheeky.outerDlaa;
        Row on, cheap;
        const std::wstring onDesc = L"Off leaves ACE’s own DLSS on every view.";
        on.fullHeight = [onDesc](float width) { return checkbox_height(onDesc, width); };
        on.full = [onDesc](const Rect& r) { checkbox(kCheekyOn, r, app.s.cheeky.processing, L"Foveated DLSS", onDesc); };
        const std::wstring cheapDesc = L"The outer views get Cheeky’s own DLAA pass at a lower resolution instead of ACE’s DLSS.";
        cheap.fullHeight = [cheapDesc](float width) { return checkbox_height(cheapDesc, width); };
        cheap.full = [cheapDesc](const Rect& r) { checkbox(kCheekyOuter, r, app.s.cheeky.outerDlaa, L"Cheaper outer views", cheapDesc); };
        std::wstring presetNames[std::size(kCheekyPresets)];
        for (size_t i = 0; i < std::size(kCheekyPresets); ++i)
            presetNames[i] = kCheekyPresets[i].name;
        const float presetW = popup_width(presetNames, static_cast<int>(std::size(presetNames)), 140);
        const auto preset_row = [presetW](Ctl id, const std::wstring& label, const std::wstring& desc, int value, bool disabled) {
            Row row;
            row.label = label;
            row.desc = desc;
            row.disabled = disabled;
            row.controlW = presetW;
            row.controlH = t::kControlHeight;
            row.control = [id, value, disabled](const Rect& r) { popup_button(id, r, cheeky_preset_name(value), disabled); };
            return row;
        };
        h += section(x, y + h, w, L"Processing",
                     {on, cheap,
                      slider_row(kCheekyScale, L"Outer DLAA scale", L"That pass’s resolution, a share of ACE’s render size.",
                                 app.s.cheeky.outerScale, L"", !outer),
                      preset_row(kCheekyOuterPreset, L"Outer preset", L"", app.s.cheeky.outerPreset, !outer),
                      preset_row(kCheekyFocusPreset, L"Focus preset", L"DLSS preset for the sharp focus views.",
                                 app.s.cheeky.focusPreset, false)},
                     L"In Quad View Foveated Rendering, Sim-QVFR tells Cheeky which "
                     L"views are outer and which are focus.",
                     draw, Applies::Live);
        h += t::kSpace6;
        Row fix, nr;
        const std::wstring fixDesc = L"Cheeky’s workaround for smearing with some presets. Adds GPU work.";
        fix.fullHeight = [fixDesc](float width) { return checkbox_height(fixDesc, width); };
        fix.full = [fixDesc](const Rect& r) { checkbox(kCheekyMotionFix, r, app.s.cheeky.motionFix, L"Fix motion-vector blur", fixDesc); };
        const std::wstring nrDesc = L"NVIDIA’s DLSS-NR (DLSS 5) on the focus views. Needs nvngx_dlssnr.dll next to the game.";
        nr.fullHeight = [nrDesc](float width) { return checkbox_height(nrDesc, width); };
        nr.full = [nrDesc](const Rect& r) { checkbox(kCheekyNr, r, app.s.cheeky.nr, L"DLSS-NR", nrDesc); };
        const std::wstring nrPresets[] = {kCheekyNrPresets[0], kCheekyNrPresets[1], kCheekyNrPresets[2]};
        const std::wstring nrPasses[] = {L"1 pass", L"2 passes", L"3 passes"};
        std::vector<Row> experimental = {
            slider_row(kCheekySupersampling, L"Focus supersampling",
                       L"Runs the focus views’ DLSS above their size for extra sharpness, at a GPU cost.",
                       app.s.cheeky.supersampling, L""),
            fix, nr,
            choice_row(kCheekyNrPreset, L"DLSS-NR preset", nrPresets, 3, std::clamp(app.s.cheeky.nrPreset, 0, 2), !app.s.cheeky.nr),
            choice_row(kCheekyNrPasses, L"DLSS-NR passes", nrPasses, 3, std::clamp(app.s.cheeky.nrPasses, 1, 3) - 1,
                       !app.s.cheeky.nr)};
        h += section(x, y + h, w, L"Experimental: focus views", experimental,
                     L"Cheeky applies these to the focus views only. Each extra DLSS-NR pass costs about "
                     L"one more NR evaluation.",
                     draw, Applies::Live);
        h += t::kSpace6;
        Row fgOn;
        const bool fgHere = game_has(&games::GameFeatures::frameGeneration);
        const std::wstring fgDesc = !fgHere ? not_yet()
                                    : L"Cheeky’s DLSS-G adds generated frames between ACE’s, guided by its depth and motion. "
                                      L"Turn OpenXR Toolkit’s Turbo mode and other frame generation (VectorXR’s) off.";
        fgOn.fullHeight = [fgDesc](float width) { return checkbox_height(fgDesc, width); };
        fgOn.full = [fgDesc, fgHere](const Rect& r) {
            checkbox(kFgOn, r, app.s.cheeky.fg && fgHere, L"Frame generation", fgDesc, !fgHere);
        };
        const bool fgOff = !app.s.cheeky.fg || !fgHere;
        Row fgBorder;
        const std::wstring borderDesc = L"Outlines the region in the headset, to check that it follows your eyes.";
        fgBorder.fullHeight = [borderDesc](float width) { return checkbox_height(borderDesc, width); };
        fgBorder.full = [borderDesc](const Rect& r) {
            checkbox(kFgBorder, r, app.s.cheeky.fgBorder, L"Show the center region", borderDesc);
        };
        const std::wstring multipliers[] = {kFgMultipliers[0], kFgMultipliers[1], kFgMultipliers[2]};
        const std::wstring quarters[] = {kFgQuarters[0], kFgQuarters[1], kFgQuarters[2], kFgQuarters[3]};
        h += section(x, y + h, w, L"Frame generation (experimental)",
                     {fgOn,
                      choice_row(kFgMultiplier, L"Multiplier", multipliers, 3, std::clamp(app.s.cheeky.fgMultiplier - 2, 0, 2), fgOff),
                      slider_row(kFgSize, L"Center region", L"Where frames are generated at full quality, a share of each eye.",
                                 app.s.cheeky.fgSize, L"", fgOff),
                      choice_row(kFgScale, L"Center FG scale", quarters, 4, std::clamp(app.s.cheeky.fgScale / 25 - 1, 0, 3), fgOff),
                      choice_row(kFgPeriphery, L"Periphery FG scale", quarters, 4,
                                 std::clamp(app.s.cheeky.fgPeriphery / 25 - 1, 0, 3), fgOff),
                      [&] {
                          const std::wstring names[3] = {kCheekyFgReleaseNames[0], kCheekyFgReleaseNames[1],
                                                         kCheekyFgReleaseNames[2]};
                          Row release = choice_row(kFgRelease, L"Release game", names, 3,
                                                   cheeky_fg_release_index(app.s.cheeky.fgRelease), fgOff);
                          release.desc = L"When the game starts its next frame. Never: CPU and GPU overlap (most frames). "
                                         L"Capture: after the GPU finishes. Generated: after the generated frame.";
                          return release;
                      }(),
                      fgBorder},
                     L"Lower scales use less GPU time and memory.",
                     draw, Applies::Live);
        const std::wstring license = L"Cheeky Foveated DLSS is made by ClarkCheekyKent and is free software under the "
                                     L"GNU GPL v3 (github.com/ClarkCheekyKent/CheekyFoveatedDLSS).";
        const float lh = app.canvas.measure_height(license, t::kCallout, w);
        if (draw)
            app.canvas.text(license, t::kCallout, {x, y + h + t::kSpace6, w, lh + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
        h += t::kSpace6 + lh;
        break;
    }
    case Pane::Menu: {
        h += section(x, y, w, L"Placement",
                     {slider_row(kMenuYaw, L"Horizontal", L"Degrees right of straight ahead. Negative is left.", app.s.menuYaw,
                                 L"", false, L"°"),
                      slider_row(kMenuPitch, L"Vertical", L"Degrees above eye level. Negative is below.", app.s.menuPitch, L"",
                                 false, L"°"),
                      slider_row(kMenuWidth, L"Size", L"Width of the panel, in degrees of your view.", app.s.menuWidth, L"",
                                 false, L"°")},
                     L"Drag it in the preview, or move it from the menu itself in the headset.",
                     draw, Applies::Live);
        const std::wstring open = L"Ctrl+Alt+arrows while the game has focus (or the keys and wheel buttons set in the Hotkeys "
                                  L"pane), or click the left controller stick. The right stick selects and changes values; "
                                  L"the menu hides after a few seconds.";
        const float oh = app.canvas.measure_height(open, t::kCallout, w - 2 * t::kSpace3);
        if (draw) {
            app.canvas.text(L"Opening the menu", t::kHeadline, {x + t::kSpace3, y + h + t::kSpace6, w - t::kSpace3, 16},
                            col(app.c->label));
            app.canvas.text(open, t::kCallout, {x + t::kSpace3, y + h + t::kSpace6 + 16 + 6, w - 2 * t::kSpace3, oh + 2},
                            col(app.c->label_secondary), Align::Leading, false, true);
        }
        h += t::kSpace6 + 16 + 6 + oh;
        h += t::kSpace6;
        Row footer;
        const std::wstring footerDesc = L"A line under the menu with the in-game hotkeys (overlay, visor, CPU capture) and their "
                                        L"wheel buttons, as set in the Hotkeys pane. Also on the menu’s Overlay and menu page.";
        footer.fullHeight = [footerDesc](float width) { return checkbox_height(footerDesc, width); };
        footer.full = [footerDesc](const Rect& r) {
            checkbox(kHotkeyFooter, r, app.s.hotkeyFooter, L"Show the hotkeys under the menu", footerDesc);
        };
        h += section(x, y + h, w, L"", {footer}, L"", draw) + t::kSpace6;
        Row osdOn;
        const std::wstring osdDesc = L"Frame rate and pacing, CPU and GPU time with their headroom and which one limits the "
                                     L"frame, VRAM, frame generation and eye tracking, floating in your view.";
        osdOn.fullHeight = [osdDesc](float width) { return checkbox_height(osdDesc, width); };
        osdOn.full = [osdDesc](const Rect& r) { checkbox(kOsdOn, r, app.s.osd.enabled, L"Show the overlay", osdDesc); };
        const bool osdOff = !app.s.osd.enabled;
        h += section(x, y + h, w, L"Performance overlay",
                     {osdOn,
                      slider_row(kOsdYaw, L"Horizontal", L"Degrees right of straight ahead. Negative is left.", app.s.osd.yaw,
                                 L"", osdOff, L"°"),
                      slider_row(kOsdPitch, L"Vertical", L"Degrees above eye level. Negative is below.", app.s.osd.pitch, L"",
                                 osdOff, L"°"),
                      slider_row(kOsdSize, L"Size", L"Width of the overlay, in degrees of your view.", app.s.osd.width, L"",
                                 osdOff, L"°")},
                     L"Drag it in the preview, or move it from the headset menu’s Overlay and menu page. Ctrl+Alt+P "
                     L"while the game has focus turns it on and off.",
                     draw, Applies::Live);
        break;
    }
    case Pane::Games: h += games_pane(x, y, w, draw); break;
    case Pane::Layers: h += layers_pane(x, y, w, draw); break;
    case Pane::Diagnostics: h += diagnostics_pane(x, y, w, draw); break;
    case Pane::Report: h += report_pane(x, y, w, draw); break;
    case Pane::About: {
        const float lh = t::kBody.lineHeight;
        if (draw) {
            app.canvas.text(L"Sim-QVFR Launcher", t::kTitle1, {x, y, w, t::kTitle1.lineHeight}, col(app.c->label));
            app.canvas.text(L"Starts " + game_name() + L" with foveated rendering enabled.", t::kBody,
                            {x, y + t::kTitle1.lineHeight + 8, w, lh}, col(app.c->label_secondary));
            // A packaged build has its release's version; a development build ("dev") names the release it is headed for.
            const std::wstring version = known_version(widen(SIM_QVFR_VERSION))
                                             ? widen(SIM_QVFR_VERSION)
                                             : widen(SIM_QVFR_RELEASE) + L" · development build";
            app.canvas.text(L"Version " + version, t::kBody, {x, y + t::kTitle1.lineHeight + 8 + lh + 4, w, lh},
                            col(app.c->label_secondary));
        }
        h += t::kTitle1.lineHeight + 8 + lh + 4 + lh;
        // Support, before Updates so a short window still shows it: a line and a button that opens the Ko-fi page.
        const std::wstring support = L"If Sim-QVFR improves your races, you can support its development on Ko-fi.";
        const float sh = app.canvas.measure_height(support, t::kCallout, w);
        if (draw)
            app.canvas.text(support, t::kCallout, {x, y + h + t::kSpace6, w, sh + 2}, col(app.c->label_secondary),
                            Align::Leading, false, true);
        h += t::kSpace6 + sh + t::kSpace2;
        const float bw = button_width(L"Support on Ko-fi") + 24;
        if (draw)
            button(kSupport, {x, y + h, bw, t::kControlHeight}, L"Support on Ko-fi", Btn::Support, kIconHeart);
        h += t::kControlHeight + t::kSpace6;
        // Updates (update_check.h): the start-up switch, then where the last check stands with Check now and Download.
        Row autoCheck;
        const std::wstring autoDesc = L"Asks GitHub for the newest Sim-QVFR release when the launcher starts. Nothing else "
                                      L"is sent, and nothing downloads by itself.";
        autoCheck.fullHeight = [autoDesc](float width) { return checkbox_height(autoDesc, width); };
        autoCheck.full = [autoDesc](const Rect& r) {
            checkbox(kUpdateCheck, r, app.s.updateCheck, L"Check for updates when the launcher starts", autoDesc);
        };
        Row status;
        const std::wstring mine = widen(SIM_QVFR_VERSION);
        switch (app.update) {
        case Update::Off: status.label = L"Not checked"; break;
        case Update::Checking: status.label = L"Checking…"; break;
        case Update::UpToDate: status.label = L"Up to date"; status.desc = L"The newest release is " + app.release.tag + L"."; break;
        case Update::Available:
            status.label = app.release.tag + L" is available";
            status.desc = L"You have " + mine + L".";
            if (app.installing)
                status.desc = app.installProgress.step + L"… " + std::to_wstring(static_cast<int>(app.installProgress.share * 100)) + L"%";
            else if (!app.installError.empty())
                status.desc = L"The install stopped: " + app.installError + L". Nothing was changed.";
            else if (!installable())
                status.desc += L" Download it by hand: this copy can’t install it itself (a development build, or the release "
                               L"has no checked zip).";
            break;
        case Update::Dev:
            status.label = L"Newest release: " + app.release.tag;
            status.desc = L"This is a development build (" + mine + L").";
            break;
        case Update::Failed: status.label = L"Couldn’t check"; status.desc = app.updateError + L"."; break;
        }
        // The buttons in a row of their own under it (the status's text keeps the row's width): Install (primary) where
        // this copy can install the release, Download (its page; the primary without Install), Check now. None while
        // installing.
        std::vector<Row> updateRows = {autoCheck, status};
        if (!app.installing) {
            const bool install = installable();
            const bool download = (app.update == Update::Available || app.update == Update::Dev) && !app.release.url.empty();
            const std::wstring installLabel = app.installError.empty() ? L"Install " + app.release.tag : L"Try again";
            Row actions;
            actions.fullHeight = [](float) { return t::kControlHeight; };
            actions.full = [install, download, installLabel](const Rect& r) {
                float bx = r.x;
                const auto add = [&bx, &r](Ctl id, const std::wstring& label, bool primary) {
                    const float bw = button_width(label);
                    button(id, {bx, r.y, bw, t::kControlHeight}, label, primary);
                    bx += bw + t::kSpace2;
                };
                if (install)
                    add(kUpdateInstall, installLabel, true);
                if (download)
                    add(kUpdateDownload, L"Download", !install);
                add(kUpdateNow, L"Check now", false);
            };
            updateRows.push_back(actions);
        }
        h += section(x, y + h, w, L"Updates", updateRows,
                     L"Install keeps your settings, profiles and changed files; Download opens the release’s page.",
                     draw);
        // The same notice as the README's: unofficial, at the user's own risk.
        const std::wstring notice =
            L"Sim-QVFR is not made, approved or sanctioned by Kunos Simulazioni or the developers of the games it "
            L"supports. It hooks the game's code and changes how it renders, so a game update can break it, and "
            L"it can crash the game. Use it in offline sessions only, and please don't report problems you see "
            L"while using it to the game's developers.";
        const float nh = app.canvas.measure_height(notice, t::kCallout, w);
        if (draw) {
            app.canvas.text(L"Experimental and unofficial. Use at your own risk.", t::kHeadline,
                            {x, y + h + t::kSpace6, w, 16}, col(app.c->label));
            app.canvas.text(notice, t::kCallout, {x, y + h + t::kSpace6 + 16 + t::kSpace2, w, nh + 2},
                            col(app.c->label_secondary), Align::Leading, false, true);
        }
        h += t::kSpace6 + 16 + t::kSpace2 + nh;
        break;
    }
    }
    return h;
}

// FoveationPreview (one eye, a schematic) at (x, y), width w. Returns its height.
float preview(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool known = app.runtime.known();
    // The whole eye (the FOV crop is drawn over it); without a recommended size, the kit's example
    // aspect (2584 x 2160) shapes the drawing.
    const Size outer = known ? ro.runtime : Size{2584, 2160};
    const float W = 320, H = std::round(W * outer.height / std::max(1, outer.width)), k = w / W;
    const float frameH = H * k;
    if (draw) {
        const Rect frame{x, y, w, frameH};
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        const auto R = [&](float rx, float ry, float rw, float rh) { return Rect{x + rx * k, y + ry * k, rw * k, rh * k}; };
        const bool tfi = app.s.mode == RenderMode::TwoLoop;
        const bool eye = app.s.eyeTracking && !eye_warning();
        const float fw = W * app.s.width / 100, fh = H * app.s.height / 100;
        // Centred on the lens in both modes (eye tracked: looking straight ahead), moved by the vertical
        // offset, roughly: about 100 degrees of view top to bottom.
        const float gx = 0.5f, gy = 0.5f - app.s.offset.for_mode(app.s.eyeTracking) / 100.0f;
        // The kept view (the left eye: its outer side is on the left); the focus region stays inside it.
        const bool cropHere = game_has(&games::GameFeatures::fovCrop);  // none for a game without the crop
        const float cl = W * static_cast<float>(cropHere && app.s.crop_width() < 1 ? app.s.cropOuter : 0) / 100,
                    cr = W * static_cast<float>(cropHere && app.s.crop_width() < 1 ? app.s.cropInner : 0) / 100,
                    ct = H * static_cast<float>(cropHere && app.s.crop_height() < 1 ? app.s.cropTop : 0) / 100,
                    cb = H * static_cast<float>(cropHere && app.s.crop_height() < 1 ? app.s.cropBottom : 0) / 100;
        const float fx = std::clamp(gx * W - fw / 2, cl, std::max(cl, W - cr - fw)),
                    fy = std::clamp(gy * H - fh / 2, ct, std::max(ct, H - cb - fh));
        // Grid cell size shows pixel density: 8 in the periphery, 8 / ratio in the focus region.
        const double ratio = tfi ? ro.focusDensity * 100 / app.s.outer : ro.focusDensity * 100 / app.s.outer;
        const float pc = 8, fc = std::clamp(static_cast<float>(pc / std::max(0.01, ratio)), 2.0f, 8.0f);
        app.canvas.fill(R(0, 0, W, H), col(app.c->preview_periphery));
        for (float gxl = 0; gxl <= W; gxl += pc)
            app.canvas.line(x + gxl * k, y, x + gxl * k, y + frameH, col(app.c->preview_canvas), 0.6f * k);
        for (float gyl = 0; gyl <= H; gyl += pc)
            app.canvas.line(x, y + gyl * k, x + w, y + gyl * k, col(app.c->preview_canvas), 0.6f * k);
        if (tfi)
            for (float iy = 0; iy < H; iy += 4)
                app.canvas.fill(R(0, iy, W, 2), col(app.c->preview_canvas, 0.55f));
        // Feather: the focus region fades in over feather % of its size, as the compositor blends it;
        // its corners rounded by corner % of half the shorter side, or with the corner shape, each corner
        // pulled in (focus_shape.h; its feather drawn as the quadrilateral shrunk toward its middle).
        const float band = std::min(fw, fh) * app.s.feather / 100.0f;
        const float corner = app.s.shape.corners ? 0.0f : std::min(fw, fh) / 2 * app.s.corner / 100.0f;
        const auto quad = [&](float in) {  // the corner shape, `in` from its edges across and down
            ComPtr<ID2D1PathGeometry> path;
            app.canvas.factory()->CreatePathGeometry(&path);
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            const float sx = (fw - 2 * in) / fw, sy = (fh - 2 * in) / fh;
            for (int i = 0; i < kFocusCorners; ++i) {
                float u, v;
                focus_corner_point(app.s.shape, static_cast<FocusCorner>(i), u, v);
                const D2D1_POINT_2F p{x + (fx + fw / 2 + (u - 0.5f) * fw * sx) * k, y + (fy + fh / 2 + (v - 0.5f) * fh * sy) * k};
                if (i == 0)
                    sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
                else
                    sink->AddLine(p);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            return path;
        };
        const int steps = band > 0.5f ? 6 : 1;
        for (int i = 0; i < steps; ++i) {
            const float in = band * i / steps, alpha = steps == 1 ? 1.0f : static_cast<float>(i + 1) / steps;
            if (app.s.shape.corners)
                app.canvas.fill(quad(in).Get(), col(app.c->preview_focus, alpha));
            else
                app.canvas.fill(R(fx + in, fy + in, fw - 2 * in, fh - 2 * in), col(app.c->preview_focus, alpha),
                                std::max(0.0f, corner - in) * k);
        }
        // The focus region's finer grid (the kit's focus pattern), on its own grid origin, inside its shape.
        ComPtr<ID2D1Geometry> shape;
        if (app.s.shape.corners) {
            shape = quad(0);
        } else {
            ComPtr<ID2D1RoundedRectangleGeometry> box;
            app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(R(fx, fy, fw, fh).d2d(), corner * k, corner * k), &box);
            shape = box;
        }
        ComPtr<ID2D1Layer> focusLayer;
        rt->CreateLayer(&focusLayer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), focusLayer.Get());
        for (float lx = fx; lx <= fx + fw; lx += fc)
            app.canvas.line(x + lx * k, y + fy * k, x + lx * k, y + (fy + fh) * k, col(app.c->preview_periphery), 0.4f * k);
        for (float ly = fy; ly <= fy + fh; ly += fc)
            app.canvas.line(x + fx * k, y + ly * k, x + (fx + fw) * k, y + ly * k, col(app.c->preview_periphery), 0.4f * k);
        rt->PopLayer();
        if (app.s.outline && app.s.shape.corners)
            app.canvas.stroke(quad(1).Get(), col(overlay(static_cast<int>(app.s.color))), 2 * k);
        else if (app.s.outline)
            app.canvas.stroke(R(fx, fy, fw, fh), col(overlay(static_cast<int>(app.s.color))), 2 * k, corner * k);
        // The cropped edges, as the headset shows them; with the helmet, its rim reaches into the view.
        const bool helmet = app.s.mode == RenderMode::Qvfr && app.s.helmet.on && game_has(&games::GameFeatures::helmet);
        const float rim = helmet ? H * app.s.helmet.rim / 100.0f : 0;
        const Color cropped = col(app.c->preview_canvas, helmet ? 1.0f : 0.9f);
        app.canvas.fill(R(0, 0, W, ct + rim), cropped);
        app.canvas.fill(R(0, H - cb - rim, W, cb + rim), cropped);
        app.canvas.fill(R(0, ct, cl + rim, H - ct - cb), cropped);
        app.canvas.fill(R(W - cr - rim, ct, cr + rim, H - ct - cb), cropped);
        if (eye) {
            const float cx = x + gx * W * k, cy = y + gy * H * k;
            const Color label = col(app.c->preview_label);
            app.canvas.circle(cx, cy, 5 * k, label, false, 1.2f * k);
            app.canvas.line(cx - 9 * k, cy, cx - 5 * k, cy, label, 1.2f * k);
            app.canvas.line(cx + 5 * k, cy, cx + 9 * k, cy, label, 1.2f * k);
            app.canvas.line(cx, cy - 9 * k, cx, cy - 5 * k, label, 1.2f * k);
            app.canvas.line(cx, cy + 5 * k, cx, cy + 9 * k, label, 1.2f * k);
        }
        const t::TypeStyle tiny{9.5f * k, 12 * k, 400, false};
        if (known) {
            app.canvas.text(L"Focus " + dims(ro.focus), tiny, R(fx + 6, fy + 4, W - fx - 6, 12), col(app.c->preview_label), Align::Leading, false);
            app.canvas.text(L"Outer " + dims(ro.outer), tiny, R(cl + 8, H - cb - 17, W - cl - cr - 16, 12), col(app.c->preview_label),
                            Align::Leading, false);
        }
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
    }
    return frameH;
}

// What the foveation preview's two marks mean. Returns its height.
float preview_legend(float x, float y, float w, bool draw) {
    if (draw) {
        const Rect focusKey{x, y + 3, 10, 10};
        app.canvas.fill(focusKey, col(app.c->preview_focus), 2);
        app.canvas.stroke(focusKey, col(app.c->accent), 1, 2);
        const std::wstring focusText = app.s.eyeTracking && !eye_warning() ? L"Focus, follows your eyes" : L"Focus, fixed";
        const float fw = std::ceil(app.canvas.measure(focusText, t::kSubheadline));
        app.canvas.text(focusText, t::kSubheadline, {x + 16, y, fw + 2, 16}, col(app.c->label_secondary));
        const float cx = x + 16 + fw + t::kSpace4;
        app.canvas.fill({cx, y + 3, 10, 10}, col(app.c->preview_canvas), 2);
        app.canvas.stroke({cx, y + 3, 10, 10}, col(app.c->label_muted), 1, 2);
        app.canvas.text(L"Not rendered", t::kSubheadline, {cx + 16, y, std::max(0.0f, x + w - cx - 16), 16}, col(app.c->label_secondary));
    }
    return 16;
}

// A label and its value on one line, the value trailing. Returns the line's height.
float readout(float x, float y, float w, const std::wstring& name, const std::wstring& value, bool draw,
              const std::wstring& of = L"") {
    if (draw) {
        app.canvas.text(name, t::kBody, {x, y, w / 2, 20}, col(app.c->label_secondary));
        const float ow = of.empty() ? 0 : std::ceil(app.canvas.measure(of, t::kBody)) + 2;
        app.canvas.text(value, t::kBody, {x + w / 2, y, w / 2 - ow - (of.empty() ? 0 : 4), 20}, col(app.c->label), Align::Trailing);
        if (!of.empty())
            app.canvas.text(of, t::kBody, {x + w - ow, y, ow, 20}, col(app.c->label_muted), Align::Trailing);
    }
    return 20;
}

// A share as a bar under a readout. Returns its height.
float meter(float x, float y, float w, double share, uint32_t color, bool draw) {
    if (draw) {
        app.canvas.fill({x, y, w, 6}, col(app.c->segment_track), 3);
        const float f = static_cast<float>(std::clamp(share, 0.0, 1.0));
        if (f > 0)
            app.canvas.fill({x, y, std::max(6.0f, w * f), 6}, col(color), 3);
    }
    return 6;
}

// An upper-case group label (the sidebar's, a card's). Returns its height.
float eyebrow(float x, float y, float w, const std::wstring& text, bool draw) {
    if (draw) {
        std::wstring upper = text;
        for (wchar_t& ch : upper)
            ch = static_cast<wchar_t>(towupper(ch));
        app.canvas.text(upper, t::kFootnote, {x, y, w, t::kFootnote.lineHeight}, col(app.c->label_muted));
    }
    return t::kFootnote.lineHeight;
}

// The foveation preview's numbers: each view's size, then the pixels and VRAM against what the headset would draw.
float readouts_card(float x, float y, float w, bool draw) {
    const Readouts ro = readouts(app.s, app.runtime);
    const bool known = app.runtime.known();
    const std::wstring dash = L"—";
    float h = eyebrow(x, y, w, L"Per eye", draw) + t::kSpace2;
    h += readout(x, y + h, w, L"Headset", known ? dims(ro.runtime) : dash, draw) + 6;
    h += readout(x, y + h, w, L"Outer view", known ? dims(ro.outer) : dash, draw) + 6;
    h += readout(x, y + h, w, L"Focus view", known ? dims(ro.focus) : dash, draw) + 6;
    h += readout(x, y + h, w, L"Center sharpness", ro.centerPpd > 0 ? std::to_wstring(std::lround(ro.centerPpd)) + L" PPD" : dash,
                 draw);
    h += t::kSpace3;
    if (draw)
        app.canvas.line(x, y + h, x + w, y + h, col(app.c->separator), 1);
    h += t::kSpace3;
    char mp[24], of[24];
    std::snprintf(mp, sizeof(mp), "%.2f M", ro.pixels / 1e6);
    std::snprintf(of, sizeof(of), "of %.2f M", ro.runtimePixels / 1e6);
    h += readout(x, y + h, w, L"Pixels per frame", known ? widen(mp) : dash, draw, known ? widen(of) : L"") + 6;
    h += meter(x, y + h, w, known && ro.runtimePixels > 0 ? ro.pixels / ro.runtimePixels : 0, app.c->accent_fill, draw) + 6;
    if (known) {
        const int savedPercent = static_cast<int>(std::lround(ro.saved * 100));
        const std::wstring saved = savedPercent >= 0 ? std::to_wstring(savedPercent) + L"% fewer pixels than full resolution"
                                                     : std::to_wstring(-savedPercent) + L"% more pixels than full resolution";
        if (draw)
            app.canvas.text(saved, t::kCallout, {x, y + h, w, 17}, col(savedPercent >= 0 ? app.c->success : app.c->warning));
        h += 17;
    }
    // VRAM, scaled from the last QVFR launch's measurement (settings.h's vram_estimate).
    const VramEstimate vram = vram_estimate(app.s, ro, app.runtime);
    h += t::kSpace4;
    char gb[24], budget[24] = "";
    std::snprintf(gb, sizeof(gb), "%.1f GB", vram.mb / 1024);
    if (vram.budgetMb > 0)
        std::snprintf(budget, sizeof(budget), "of %.1f GB", vram.budgetMb / 1024);
    const bool vramKnown = known && vram.mb > 0;
    h += readout(x, y + h, w, L"VRAM, estimated", vramKnown ? widen(gb) : dash, draw, vramKnown ? widen(budget) : L"") + 6;
    const double share = vram.budgetMb > 0 ? vram.mb / vram.budgetMb : 0;
    h += meter(x, y + h, w, vramKnown ? share : 0,
               share > 0.9 ? app.c->danger : share > 0.75 ? app.c->warning : app.c->accent_fill, draw);
    return h;
}

// Angular size of a panel: its height in degrees for a width in degrees, at any distance.
float panel_height_deg(int widthDeg, float aspect) {
    constexpr float kRad = 3.14159265f / 180;
    return 2 * std::atan(std::tan(widthDeg * kRad / 2) * aspect) / kRad;
}

float menu_height_deg(int widthDeg) { return panel_height_deg(widthDeg, kMenuAspect); }

float osd_height_deg(int widthDeg) { return panel_height_deg(widthDeg, static_cast<float>(kOsdHeight) / kOsdWidth); }

std::wstring degrees(int v, const wchar_t* plus, const wchar_t* minus) {
    return v == 0 ? L"0°" : std::to_wstring(std::abs(v)) + L"° " + (v > 0 ? plus : minus);
}

// The settings menu's place (Headset menu pane) at (x, y), width w: a kRefFovX x kRefFovY view in
// degrees with the panel, drawn in its own colours, where it floats. Returns its height.
float placement_preview(float x, float y, float w, bool draw) {
    const float k = w / kRefFovX, frameH = std::round(kRefFovY * k);
    const Rect frame{x, y, w, frameH};
    const float pw = app.s.menuWidth * k, ph = menu_height_deg(app.s.menuWidth) * k;
    const float pcx = x + (kRefFovX / 2 + app.s.menuYaw) * k, pcy = y + (kRefFovY / 2 - app.s.menuPitch) * k;
    const Rect panel{pcx - pw / 2, pcy - ph / 2, pw, ph};
    if (draw) {
        app.placeFrame = frame;
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        app.canvas.factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame.d2d(), t::kRadiusGroup, t::kRadiusGroup), &clip);
        ComPtr<ID2D1Layer> layer;
        ID2D1RenderTarget* rt = app.canvas.target();
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        app.canvas.fill(frame, col(app.c->preview_periphery));
        // A 10° grid, with the horizon and straight ahead stronger.
        for (float d = 10; d < kRefFovX; d += 10)
            app.canvas.line(x + d * k, y, x + d * k, y + frameH, col(app.c->preview_canvas), 0.6f);
        for (float d = 10; d < kRefFovY; d += 10)
            app.canvas.line(x, y + d * k, x + w, y + d * k, col(app.c->preview_canvas), 0.6f);
        const float cx = x + w / 2, cy = y + frameH / 2;
        const Color axis = col(app.c->preview_label, 0.35f);
        app.canvas.line(cx, y, cx, y + frameH, axis, 1);
        app.canvas.line(x, cy, x + w, cy, axis, 1);
        app.canvas.circle(cx, cy, 5, col(app.c->preview_label, 0.8f), false, 1.2f);
        // The panel as the headset shows it: title, resolution line, the settings rows (the first
        // selected) and the hint, in its own colours (in panel pixels, 1024 x 972).
        const bool active = app.dragging == kMenuPanel;
        app.canvas.fill({panel.x, panel.y + 2, panel.w, panel.h}, {0, 0.35f}, 4);
        app.canvas.fill(panel, col(kMenuBackground, 0.92f), 4);
        const float m = pw * 36 / 1024, u = ph / 972;
        const auto bar = [&](float bx, float by, float bw, float bh, Color c) {
            app.canvas.fill({panel.x + bx, panel.y + by * u, bw, std::max(1.5f, bh * u)}, c, 1);
        };
        bar(m, 30, pw * 0.42f, 34, col(kMenuLabel));
        bar(m, 100, pw * 0.36f, 22, col(kMenuDim));
        bar(pw / 2, 100, pw * 0.3f, 22, col(kMenuLabel, 0.8f));
        for (int i = 0; i < 11; ++i) {
            const float ry = 200 + 50.0f * i;
            if (i == 0)
                app.canvas.fill({panel.x + m * 0.55f, panel.y + (ry - 8) * u, pw - 1.1f * m, 50 * u}, col(kMenuHighlight), 1);
            bar(m, ry + 8, pw * (0.22f + 0.04f * (i % 3)), 18, col(kMenuLabel, 0.85f));
            const float vw = pw * 0.12f;
            bar(pw - m - vw, ry + 8, vw, 18, col(kMenuLabel, 0.85f));
        }
        bar(m, 880, pw * 0.6f, 14, col(kMenuDim, 0.7f));
        app.canvas.stroke(panel, col(active ? app.c->accent : app.c->preview_label, active ? 1 : 0.45f), active ? 1.5f : 0.75f, 4);
        if (focused(kMenuPanel))
            focus_ring(panel, 4);
        // The performance overlay, when it is on: four number tiles, the frame-time strip and its cells.
        Rect card{};
        if (app.s.osd.enabled) {
            const float cw = app.s.osd.width * k, ch = osd_height_deg(app.s.osd.width) * k;
            const float ccx = x + (kRefFovX / 2 + app.s.osd.yaw) * k, ccy = y + (kRefFovY / 2 - app.s.osd.pitch) * k;
            card = {ccx - cw / 2, ccy - ch / 2, cw, ch};
            const bool held = app.dragging == kOsdPanel;
            const float r = std::max(2.0f, cw * 32 / kOsdWidth), cu = cw / kOsdWidth, cv = ch / kOsdHeight;
            app.canvas.fill({card.x, card.y + 2, card.w, card.h}, {0, 0.35f}, r);
            app.canvas.fill(card, col(kOsdCard, 0.9f), r);
            const auto block = [&](float bx, float by, float bw, float bh, Color c) {
                app.canvas.fill({card.x + bx * cu, card.y + by * cv, std::max(1.0f, bw * cu), std::max(1.0f, bh * cv)}, c, 1);
            };
            for (int i = 0; i < 4; ++i) {
                const float tx = 36 + i * 238.0f + (i ? 24 : 0);
                block(tx, 30, 90, 14, col(kMenuDim, 0.8f));
                block(tx, 58, 110.0f + 20 * (i % 2), 40, col(kMenuLabel));
                block(tx, 126, 120, 14, col(i == 3 ? kOsdYellow : kOsdGreen, 0.9f));
            }
            block(36, 166, kOsdWidth - 72, 60, col(kOsdTrack));
            block(44, 184, kOsdWidth - 88, 36, col(kOsdGreen, 0.75f));
            for (int i = 0; i < 6; ++i) {
                const float cx2 = 36 + (i % 3) * 317.0f, cy2 = 244 + (i / 3) * 76.0f;
                block(cx2, cy2, 100, 12, col(kMenuDim, 0.8f));
                block(cx2, cy2 + 30, 150, 22, i % 3 == 1 || i == 5 ? col(kOsdGreen, 0.55f) : col(kMenuLabel, 0.85f));
            }
            app.canvas.stroke(card, col(held ? app.c->accent : app.c->preview_label, held ? 1 : 0.45f), held ? 1.5f : 0.75f, r);
            if (focused(kOsdPanel))
                focus_ring(card, r);
        }
        rt->PopLayer();
        app.canvas.stroke(frame, col(app.c->separator), 0.5f, t::kRadiusGroup);
        app.hits.push_back({kMenuPanel, frame, 0});  // empty space: the panel's centre jumps there
        app.hits.push_back({kMenuPanel, panel, 1});  // the panel: dragged from where it is held
        if (app.s.osd.enabled)
            app.hits.push_back({kOsdPanel, card, 1});  // the overlay, over the menu
    }
    return frameH;
}

// How to use the placement preview. Returns its height.
float placement_caption(float x, float y, float w, bool draw) {
    const std::wstring caption = app.s.osd.enabled ? L"Drag the menu or the overlay to place it. Headsets’ fields of view vary."
                                                   : L"Drag the panel to place it. Headsets’ fields of view vary.";
    const float ch = app.canvas.measure_height(caption, t::kSubheadline, w);
    if (draw)
        app.canvas.text(caption, t::kSubheadline, {x, y, w, ch + 2}, col(app.c->label_secondary), Align::Leading, false, true);
    return ch;
}

// The settings menu's place and size, in numbers.
float placement_readouts(float x, float y, float w, bool draw) {
    char size[48], metres[48], distance[16];
    std::snprintf(size, sizeof(size), "%d° × %d°", app.s.menuWidth, static_cast<int>(std::lround(menu_height_deg(app.s.menuWidth))));
    std::snprintf(metres, sizeof(metres), "%.2f m", 2 * kMenuDistance * std::tan(app.s.menuWidth * 3.14159265 / 360));
    std::snprintf(distance, sizeof(distance), "%.1f m", kMenuDistance);
    float h = eyebrow(x, y, w, L"Settings menu", draw) + t::kSpace2;
    h += readout(x, y + h, w, L"Horizontal", degrees(app.s.menuYaw, L"right", L"left"), draw) + 6;
    h += readout(x, y + h, w, L"Vertical", degrees(app.s.menuPitch, L"up", L"down"), draw) + 6;
    h += readout(x, y + h, w, L"Panel size", widen(size), draw) + 6;
    h += readout(x, y + h, w, L"Panel width", widen(metres), draw) + 6;
    h += readout(x, y + h, w, L"Distance", widen(distance), draw);
    return h;
}

using Content = std::function<float(float, float, float, bool)>;

// A card in the preview column: a title (and a pill at its end) over `content`. Returns its height.
float preview_card(float x, float y, float w, const std::wstring& title, const std::wstring& pill, const Content& content,
                   bool draw) {
    const float pad = t::kSpace4, head = title.empty() ? 0 : 20 + t::kSpace3, radius = t::kRadiusGroup + 2;
    const float ch = content(x + pad, y + pad + head, w - 2 * pad, false);
    const float h = pad + head + ch + pad;
    if (draw) {
        app.canvas.fill({x, y, w, h}, col(app.c->group_bg), radius);
        app.canvas.stroke({x, y, w, h}, col(app.c->card_border), 1, radius);
        if (!title.empty()) {
            app.canvas.text(title, t::kHeadline, {x + pad, y + pad, w - 2 * pad, 20}, col(app.c->label));
            if (!pill.empty()) {
                const float pw = std::ceil(app.canvas.measure(pill, t::kSubheadline)) + 18;
                const Rect pr{x + w - pad - pw, y + pad, pw, 20};
                app.canvas.fill(pr, col(app.dark ? 0xffffff : 0, 0.07f), 10);
                app.canvas.text(pill, t::kSubheadline, pr, col(app.c->label_secondary), Align::Center);
            }
        }
        content(x + pad, y + pad + head, w - 2 * pad, true);
    }
    return h;
}

// `frame` drawn in the preview's dark well, then `after` under it.
Content in_well(Content frame, Content after = nullptr) {
    return [frame, after](float x, float y, float w, bool draw) {
        const float pad = t::kSpace3, fh = frame(x + pad, y + pad, w - 2 * pad, false);
        if (draw) {
            app.canvas.fill({x, y, w, fh + 2 * pad}, col(app.c->preview_canvas), 10);
            frame(x + pad, y + pad, w - 2 * pad, true);
        }
        float h = fh + 2 * pad;
        if (after)
            h += t::kSpace3 + after(x, y + h + t::kSpace3, w, draw);
        return h;
    };
}

// The preview column: the placement preview on the Headset menu pane, the load order on OpenXR layers, the foveation
// preview elsewhere; each a card, with a second card of numbers under the previews.
float side_preview(float x, float y, float w, bool draw) {
    if (app.pane == Pane::Layers)
        return preview_card(x, y, w, L"Load order", game_name() + L" to headset", layers_preview, draw);
    float h = 0;
    if (app.pane == Pane::Menu) {
        h += preview_card(x, y, w, L"Placement", L"100° × 80° view", in_well(placement_preview, placement_caption), draw);
        h += t::kSpace4;
        return h + preview_card(x, y + h, w, L"", L"", placement_readouts, draw);
    }
    h += preview_card(x, y, w, L"Preview", L"Left eye", in_well(preview, preview_legend), draw);
    h += t::kSpace4;
    return h + preview_card(x, y + h, w, L"", L"", readouts_card, draw);
}

// ---- Window ------------------------------------------------------------------------------------

struct Item {
    const wchar_t* label;
    const char* icon[3];
};

// The kit's sidebar glyphs (16 x 16, 1.5 stroke); circles and rects are drawn as paths.
const Item kItems[] = {
    {L"Launch checks", {"M2 4.25l1.5 1.5 2.75-3", "M2 10.75l1.5 1.5 2.75-3", "M8.75 4.5H14M8.75 11H14"}},
    {L"Profiles", {"M3 3.25H10.5V12.75H3Z", "M10.5 5.25H13V14.75H5.5V12.75", "M5 6.25H8.5M5 8.75H8.5"}},
    {L"Resolution", {"M3.25 2.75H12.75C13.6 2.75 14.25 3.4 14.25 4.25V9.75C14.25 10.6 13.6 11.25 12.75 11.25H3.25C2.4 11.25 1.75 10.6 1.75 9.75V4.25C1.75 3.4 2.4 2.75 3.25 2.75Z", "M5.5 14h5M8 11.25V14", nullptr}},
    {L"Foveation", {"M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z", nullptr, nullptr}},
    {L"Focus overlay", {"M5 5H11V11H5Z", nullptr, nullptr}},
    {L"Foveated DLSS", {"M8 1.75L9.6 6.4 14.25 8 9.6 9.6 8 14.25 6.4 9.6 1.75 8 6.4 6.4Z", nullptr, nullptr}},
    {L"Headset menu", {"M3.25 3.25H12.75C13.6 3.25 14.25 3.9 14.25 4.75V11.25C14.25 12.1 13.6 12.75 12.75 12.75H3.25C2.4 12.75 1.75 12.1 1.75 11.25V4.75C1.75 3.9 2.4 3.25 3.25 3.25Z", "M4.5 6.5H8.5", "M4.5 9.5H11.5"}},
    {L"Hotkeys", {"M2.25 4.25H13.75V11.75H2.25Z", "M4.75 6.75H5.25M7.75 6.75H8.25M10.75 6.75H11.25", "M5.25 9.25H10.75"}},
    {L"Games", {"M2.5 2.5H7V7H2.5Z", "M9 2.5H13.5V7H9Z", "M2.5 9H7V13.5H2.5ZM9 9H13.5V13.5H9Z"}},
    {L"OpenXR layers", {"M2.5 4.5H10.5", "M2.5 8H10.5", "M2.5 11.5H10.5"}},
    {L"Diagnostics", {"M1.5 8.5H4.5L6.25 4.5L9.75 11.5L11.5 8.5H14.5", nullptr, nullptr}},
    {L"Report an issue", {"M3.5 14.25V2", "M3.5 2.75H12.5L10.5 5.5L12.5 8.25H3.5", nullptr}},
    {L"About", {"M8 7.25V11", nullptr, nullptr}},
};

static_assert(std::size(kItems) == kPanes, "a sidebar item for every pane");

}  // namespace

// A pane the sidebar lists for this game: Foveated DLSS only where the game has it.
bool pane_shown(int pane) {
    return pane != static_cast<int>(Pane::Dlss) || game_has(&games::GameFeatures::foveatedDlss);
}

namespace {

// The shown pane nearest `pane` in `step`'s direction (-1 or 1), or `pane` itself if shown; `from` if none.
int shown_pane(int pane, int step, int from) {
    for (int p = pane; p >= 0 && p < kPanes; p += step)
        if (pane_shown(p))
            return p;
    return from;
}

// The sidebar's groups, each a run of panes in Pane's order; a pane's eyebrow is its group.
struct Group {
    const wchar_t* label;
    int first, end;
};

constexpr Group kGroups[] = {{L"Setup", 0, 2}, {L"Picture", 2, 6}, {L"In the headset", 6, 8}, {L"Advanced", 8, 11}, {L"Help", 11, 13}};
static_assert(kGroups[std::size(kGroups) - 1].end == kPanes, "every pane in a group");

const wchar_t* group_of(int pane) {
    for (const Group& g : kGroups)
        if (pane >= g.first && pane < g.end)
            return g.label;
    return L"";
}

// What a pane is for, under its title.
std::wstring pane_description(Pane pane) {
    switch (pane) {
    case Pane::Checks: return L"What would stop Sim-QVFR at the next launch, each with its fix.";
    case Pane::Profiles: return L"A profile saves the whole picture: resolution, foveation, crop, overlay and DLSS.";
    case Pane::Resolution: {
        const bool crop = game_has(&games::GameFeatures::fovCrop), helmet = game_has(&games::GameFeatures::helmet);
        return std::wstring(L"How many pixels each view draws") + (crop && helmet ? L", the edges it skips and the helmet around it."
                                                                   : crop       ? L" and the edges it skips."
                                                                   : helmet     ? L" and the helmet around it."
                                                                                : L".");
    }
    case Pane::Foveation: return L"The size, shape and movement of the sharp focus region.";
    case Pane::Overlay: return L"An outline in the headset that shows where the focus region is.";
    case Pane::Dlss: return L"Cheaper outer views, a sharper centre and frame generation, with Cheeky Foveated DLSS.";
    case Pane::Menu: return L"Where the settings menu and the performance overlay float in your view.";
    case Pane::Hotkeys: return L"The keys and wheel buttons for the headset menu and the in-game toggles.";
    case Pane::Games: return L"Which games have a tab, and each game’s colour.";
    case Pane::Layers: return L"The OpenXR layers " + game_name() + L" loads, and their order.";
    case Pane::Diagnostics: return L"The last session’s numbers against the ones before it, and what to share when you ask for help.";
    case Pane::Report: return L"Zips the logs and opens a GitHub issue with the details filled in.";
    case Pane::About: return L"The version, updates and support.";
    }
    return L"";
}

void sidebar_icon(int i, const Rect& r, Color c) {
    for (const char* p : kItems[i].icon)
        if (p)
            app.canvas.path(p, r, 16, c, 1.5f);
    if (i == static_cast<int>(Pane::Foveation))
        app.canvas.circle(r.x + 8, r.y + 8, 2, c, false, 1.5f);
    if (i == static_cast<int>(Pane::Overlay))  // the outer square, dashed 2 / 2 along each edge
        for (float d = 1.75f; d < 14.25f; d += 4) {
            const float e = std::min(d + 2, 14.25f);
            app.canvas.line(r.x + d, r.y + 1.75f, r.x + e, r.y + 1.75f, c, 1.5f);
            app.canvas.line(r.x + d, r.y + 14.25f, r.x + e, r.y + 14.25f, c, 1.5f);
            app.canvas.line(r.x + 1.75f, r.y + d, r.x + 1.75f, r.y + e, c, 1.5f);
            app.canvas.line(r.x + 14.25f, r.y + d, r.x + 14.25f, r.y + e, c, 1.5f);
        }
    if (i == static_cast<int>(Pane::Layers)) {  // the list's order: an up and a down arrow
        app.canvas.path("M12.5 6.5V2.5M11 4l1.5-1.5L14 4", r, 16, c, 1.5f);
        app.canvas.path("M12.5 9.5V13.5M11 12l1.5 1.5L14 12", r, 16, c, 1.5f);
    }
    if (i == static_cast<int>(Pane::About)) {
        app.canvas.circle(r.x + 8, r.y + 8, 6.25f, c, false, 1.5f);
        app.canvas.circle(r.x + 8, r.y + 5, 0.9f, c, true);
    }
}

// The open pop-up menu: under its button, as wide as the button or its longest item (check mark
// column included), right-aligned with the button when it would leave the window.
constexpr float kMenuItemH = 32;

Rect popup_menu_rect() {
    const Popup& p = app.popup;
    float w = p.anchor.w;
    for (const auto& item : p.items)
        w = std::max(w, std::ceil(app.canvas.measure(item, t::kBody)) + 28 + 6 + 12 + 8);
    Rect menu{p.anchor.x, p.anchor.y + p.anchor.h + 4, w, kMenuItemH * p.items.size() + 12};
    if (menu.right() > app.canvas.width() - 8)
        menu.x = p.anchor.right() - w;
    if (menu.bottom() > app.canvas.height())
        menu.y = p.anchor.y - menu.h - 4;
    return menu;
}

}  // namespace

// The description box (a native edit, kept in step with the canvas after each paint): over the field the
// Diagnostics pane drew; hidden when it is not wholly in view, or a popup is open.
// The native edits' font (Segoe UI 13 at the window's DPI), made again when the DPI changes: the report's description
// box and the sheet's name field.
void edit_font() {
    const UINT dpi = static_cast<UINT>(std::lround(96 * app.canvas.dpi_scale()));
    if (app.reportFont && app.reportFontDpi == dpi)
        return;
    HFONT font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND edit : {app.reportEdit, app.sheetEdit})
        if (edit)
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (app.reportFont)
        DeleteObject(app.reportFont);
    app.reportFont = font;
    app.reportFontDpi = dpi;
}

namespace {

void place_report_edit() {
    const Rect& r = app.reportEditRect;
    if (!app.reportEdit)
        return;
    if (!report_edit_fits() || app.popup.open || app.sheet.open) {
        if (GetFocus() == app.reportEdit)
            SetFocus(app.hwnd);
        ShowWindow(app.reportEdit, SW_HIDE);
        return;
    }
    const float k = app.canvas.dpi_scale();
    edit_font();
    const int x = static_cast<int>(std::lround(r.x * k)), y = static_cast<int>(std::lround(r.y * k));
    const int w = static_cast<int>(std::lround(r.w * k)), h = static_cast<int>(std::lround(r.h * k));
    SetWindowPos(app.reportEdit, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (app.reportBrushRgb != app.c->field_bg)  // the theme changed: its colours (WM_CTLCOLOREDIT)
        InvalidateRect(app.reportEdit, nullptr, TRUE);
}

void on_key(WPARAM key);

// The description box's keys: Tab and Shift+Tab move on through the window's focus order; Escape leaves it.
LRESULT CALLBACK report_edit_proc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam) {
    static const WNDPROC base = reinterpret_cast<WNDPROC>(GetPropW(edit, L"simqvfr.base"));
    if (message == WM_KEYDOWN && (wParam == VK_TAB || wParam == VK_ESCAPE)) {
        app.focus = kDiagDescription;
        SetFocus(app.hwnd);
        if (wParam == VK_TAB)
            on_key(VK_TAB);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return 0;
    }
    if (message == WM_CHAR && (wParam == L'\t' || wParam == 27))  // already handled as keys
        return 0;
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
        InvalidateRect(app.hwnd, nullptr, FALSE);  // the field's focus ring
    return CallWindowProcW(base, edit, message, wParam, lParam);
}

void create_report_edit() {
    app.reportEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                     0, 0, 0, 0, app.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!app.reportEdit)
        return;
    SendMessageW(app.reportEdit, EM_SETLIMITTEXT, 4000, 0);  // the link has room for about that much
    SetPropW(app.reportEdit, L"simqvfr.base", reinterpret_cast<HANDLE>(GetWindowLongPtrW(app.reportEdit, GWLP_WNDPROC)));
    SetWindowLongPtrW(app.reportEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(report_edit_proc));
}

// The action bar's badge for the launch checks' first problem (and how many more); it opens the pane. Returns its width.
constexpr float kBarX = t::kSpace5;

float checks_badge(float y, int tone, const Check& first, float maxWidth) {
    size_t more = 0;
    for (const Check& c : app.checks)
        more += c.tone == CheckTone::Bad || c.tone == CheckTone::Warn ? 1 : 0;
    const std::wstring text = first.title + (more > 1 ? L"  ·  " + std::to_wstring(more - 1) + L" more" : L"");
    const float w = badge(kBarX, y, tone, text, maxWidth);
    app.hits.push_back({kChecksBadge, {kBarX, y, w, 20}});
    return w;
}

constexpr t::TypeStyle kBodyMedium{t::kBody.size, t::kBody.lineHeight, 500, false};

constexpr t::TypeStyle kBodyStrong{t::kBody.size, t::kBody.lineHeight, 600, false};

// The app's mark: the game's accent with an eye.
void brand_mark(float x, float y, float size) {
    app.canvas.fill({x, y, size, size}, col(app.c->accent_fill), size * 0.28f);
    const float g = size * 0.66f, gx = x + (size - g) / 2, gy = y + (size - g) / 2;
    app.canvas.path("M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6S2 12 2 12z", {gx, gy, g, g}, 24, col(app.c->on_accent), 2.2f);
    app.canvas.circle(gx + g / 2, gy + g / 2, g * 0.11f, col(app.c->on_accent), false, 2.2f * g / 24);
}

// The tab strip: the app's name, then a tab per game, each marked in its own accent; the chosen one's palette is the
// window's (apply_theme).
void game_tabs(float W) {
    Canvas& cv = app.canvas;
    const float H = t::kTabbarHeight;
    cv.fill({0, 0, W, H}, col(app.c->titlebar_bg));
    cv.line(0, H - 0.5f, W, H - 0.5f, col(app.c->card_border), 1);
    // The app: its mark, its name and BETA.
    const float bx = t::kSpace4;
    brand_mark(bx, (H - 24) / 2, 24);
    const float nameW = std::ceil(cv.measure(L"Sim-QVFR", kBodyStrong));
    cv.text(L"Sim-QVFR", kBodyStrong, {bx + 24 + 10, 0, nameW + 2, H}, col(app.c->label));
    const t::TypeStyle beta{10.5f, 14, 600, false};
    const float betaW = std::ceil(cv.measure(L"BETA", beta)) + 14, betaX = bx + 24 + 10 + nameW + 8;
    cv.fill({betaX, (H - 18) / 2, betaW, 18}, col(app.c->warning_bg), 9);
    cv.text(L"BETA", beta, {betaX, (H - 18) / 2, betaW, 18}, col(app.c->warning), Align::Center);
    // The games, from the content column's edge; short names when the full ones don't fit.
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    const float x0 = std::max(t::kSidebarWidth, betaX + betaW + t::kSpace5);
    const auto tab_width = [&](const std::wstring& label) { return 14 + 8 + 8 + std::ceil(cv.measure(label, kBodyStrong)) + 16; };
    float full = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (game_shown(*all[i]) || all[i] == &games::launcher_game())
            full += tab_width(all[i]->name) + 4;
    const bool shortNames = x0 + full > W - t::kSpace4;
    float x = x0;
    for (std::size_t i = 0; i < count; ++i) {
        const games::GameInfo& game = *all[i];
        const bool selected = &game == &games::launcher_game();
        if (!selected && !game_shown(game))
            continue;  // hidden in the Games pane
        const std::wstring label = shortNames ? game.shortName : game.name;
        const t::Accent accent = game_accent(game);
        const Rect tab{x, 8, tab_width(label), H - 16};
        if (selected)
            cv.fill(tab, col(accent.accent_fill, app.dark ? 0.22f : 0.12f), t::kRadiusControl);
        else if (pointer_in(tab))
            cv.fill(tab, col(app.dark ? 0xffffff : 0, 0.06f), t::kRadiusControl);
        cv.circle(tab.x + 14 + 4, tab.y + tab.h / 2, 4, col(accent.accent), true);
        cv.text(label, selected ? kBodyStrong : kBodyMedium, {tab.x + 14 + 8 + 8, tab.y, tab.w - 30, tab.h},
                col(selected ? app.c->label : app.c->label_secondary));
        if (selected)  // the chosen tab's edge, in its accent, on the strip's bottom line
            cv.fill({tab.x + 10, H - 3, tab.w - 20, 3}, col(accent.accent), 1.5f);
        if (selected && focused(kGameTab))
            focus_ring(tab, t::kRadiusControl);
        app.hits.push_back({kGameTab, tab, static_cast<int>(i)});
        x += tab.w + 4;
    }
}

// The sidebar: the panes in their groups, each a row with its icon. When the groups don't fit, their labels go.
void sidebar(float top, float bottom) {
    Canvas& cv = app.canvas;
    const float W = t::kSidebarWidth;
    cv.fill({0, top, W, bottom - top}, col(app.c->sidebar_bg));
    cv.line(W - 0.5f, top, W - 0.5f, bottom, col(app.c->card_border), 1);
    const float labelH = t::kFootnote.lineHeight + 6, groupGap = 10, rowGap = 2;
    float fullH = 2 * t::kSpace3;
    for (const Group& g : kGroups) {
        fullH += labelH;
        for (int i = g.first; i < g.end; ++i)
            fullH += pane_shown(i) ? t::kSidebarRowHeight + rowGap : 0;
    }
    fullH += groupGap * (std::size(kGroups) - 1);
    const bool compact = fullH > bottom - top;
    const float rowH = compact ? 28.0f : t::kSidebarRowHeight;
    const CheckTone worst = worst_tone(app.checks);
    float sy = top + t::kSpace3;
    for (const Group& g : kGroups) {
        if (!compact) {
            eyebrow(12 + 10, sy, W - 44, g.label, true);
            sy += labelH;
        }
        for (int i = g.first; i < g.end; ++i) {
            if (!pane_shown(i))
                continue;
            const Rect row{12, sy, W - 24, rowH};
            const bool selected = static_cast<int>(app.pane) == i;
            if (selected)
                cv.fill(row, col(app.c->sidebar_selection, app.dark ? 0.2f : 0.12f), t::kRadiusControl);
            else if (pointer_in(row))
                cv.fill(row, col(app.dark ? 0xffffff : 0, 0.05f), t::kRadiusControl);
            sidebar_icon(i, {row.x + 10, row.y + (rowH - 16) / 2, 16, 16}, col(selected ? app.c->accent : app.c->label_muted));
            cv.text(kItems[i].label, selected ? kBodyStrong : kBodyMedium, {row.x + 10 + 16 + 10, row.y, row.w - 64, rowH},
                    col(selected ? app.c->label : app.c->heading));
            // A mark on Foveation without an eye tracker, and on Launch checks with a problem (red) or warning.
            const bool checksMark = i == static_cast<int>(Pane::Checks) && worst != CheckTone::Ok;
            if ((i == static_cast<int>(Pane::Foveation) && eye_warning()) || checksMark) {
                const bool danger = checksMark && worst == CheckTone::Bad;
                const float bx = row.right() - 10 - 16, by = row.y + (rowH - 16) / 2;
                cv.circle(bx + 8, by + 8, 8, col(danger ? app.c->danger_bg : app.c->warning_bg), true);
                const Color mark = col(danger ? app.c->danger : app.c->warning);
                cv.line(bx + 8, by + 4.5f, bx + 8, by + 8.5f, mark, 1.7f);
                cv.circle(bx + 8, by + 11, 1, mark, true);
            }
            if (selected && focused(kSidebar))
                focus_ring(row, t::kRadiusControl);
            app.hits.push_back({kSidebar, row, i});
            sy += rowH + rowGap;
        }
        sy += compact ? 6 : groupGap;
    }
}

// The pane's header: its group, its title and what it is for. Returns its height.
float pane_header(float x, float y, float w, bool draw) {
    const std::wstring desc = pane_description(app.pane);
    const float dh = app.canvas.measure_height(desc, t::kBody, w);
    if (draw) {
        app.canvas.text(group_of(static_cast<int>(app.pane)), {12, 16, 600, false}, {x, y, w, 16}, col(app.c->label_muted));
        app.canvas.text(kItems[static_cast<int>(app.pane)].label, t::kLargeTitle, {x, y + 20, w, t::kLargeTitle.lineHeight},
                        col(app.c->label));
        app.canvas.text(desc, t::kBody, {x, y + 20 + t::kLargeTitle.lineHeight + 4, w, dh + 2}, col(app.c->label_secondary),
                        Align::Leading, false, true);
    }
    return 20 + t::kLargeTitle.lineHeight + 4 + dh;
}

void paint() {
    if (!app.canvas.begin())
        return;
    app.hits.clear();
    app.reportEditRect = {};
    app.sheetEditRect = {};
    Canvas& cv = app.canvas;
    const float W = cv.width(), Hh = cv.height();
    const float tabH = t::kTabbarHeight, actionH = t::kActionbarHeight;
    const float bodyTop = tabH, bodyH = Hh - tabH - actionH;
    cv.fill({0, 0, W, Hh}, col(app.c->window_bg));

    game_tabs(W);
    sidebar(bodyTop, bodyTop + bodyH);

    // Main and preview columns; below 1000 wide the preview drops under the form.
    const bool narrow = W < 1000;
    const float pad = t::kSpace7;
    const float mainX = t::kSidebarWidth, mainW = narrow ? W - mainX : W - mainX - t::kPreviewWidth;
    const float contentX = mainX + pad, contentW = std::min(mainW - 2 * pad, 760.0f);
    app.scrollArea = {mainX, bodyTop, W - mainX, bodyH};
    cv.push_clip(app.scrollArea);
    const float y0 = bodyTop + pad - app.scroll;
    // About has no header: its own title, the app's name, says what the page is.
    float contentH = app.pane == Pane::About ? 0 : pane_header(contentX, y0, contentW, true) + t::kSpace7;
    contentH += pane_content(contentX, y0 + contentH, contentW, true);
    float total = contentH + pad;
    if (narrow) {
        const float pw = std::min(contentW, 460.0f);
        total += side_preview(contentX, y0 + total, pw, true) + pad;
    }
    float previewOver = 0;
    if (!narrow) {  // the preview stays in view, scrolling only as far as its own bottom needs
        const float ax = W - t::kPreviewWidth, aw = t::kPreviewWidth - pad;
        const float ph = side_preview(ax, 0, aw, false) + 2 * pad;
        previewOver = std::max(0.0f, ph - bodyH);
        side_preview(ax, bodyTop + pad - std::min(app.scroll, previewOver), aw, true);
    }
    cv.pop_clip();
    app.scrollMax = std::max({0.0f, total - bodyH, previewOver});

    // Action bar: the status, Restore Defaults, Launch.
    const float barY = bodyTop + bodyH;
    cv.fill({0, barY, W, actionH}, col(app.c->sidebar_bg));
    cv.line(0, barY + 0.5f, W, barY + 0.5f, col(app.c->card_border), 1);
    std::wstring launchLabel = L"Launch " + game_name();
    float launchW = std::ceil(cv.measure(launchLabel, kPrimaryLabel)) + 16 + 10 + 2 * 22;
    const float restoreW = button_width(L"Restore Defaults") + 24;
    if (W - launchW - restoreW - 3 * t::kSpace5 < 200) {  // narrow: the game's short name
        launchLabel = L"Launch " + std::wstring(games::launcher_game().shortName);
        launchW = std::ceil(cv.measure(launchLabel, kPrimaryLabel)) + 16 + 10 + 2 * 22;
    }
    const Rect launchR{W - t::kSpace5 - launchW, barY + (actionH - t::kControlHeightLarge) / 2, launchW, t::kControlHeightLarge};
    const Rect restoreR{launchR.x - t::kSpace2 - restoreW, barY + (actionH - t::kControlHeight) / 2, restoreW, t::kControlHeight};
    const float badgeMax = restoreR.x - t::kSpace4 - kBarX;
    const float by = barY + (actionH - 20) / 2;
    const Readouts ro = readouts(app.s, app.runtime);
    // The game launched from here (for the game whose tab is chosen): starting, and what went wrong, come before the
    // launch checks; running and closed after them, and after what a change did to the running game.
    const bool watching = app.watch.game() == &games::launcher_game();
    const LaunchOutcome launchNow = watching ? app.watch.outcome() : LaunchOutcome{};
    const bool launchFirst = watching && (launchNow.state == LaunchState::Starting || launchNow.tone == 1 || launchNow.tone == 2);
    const auto launch_badge = [&] {
        const float w = badge(kBarX, by, launchNow.tone, launchNow.text, badgeMax);
        if (launchNow.link != LaunchLink::None)
            app.hits.push_back({kLaunchBadge, {kBarX, by, w, 20}});
        return w;
    };
    float bw = 0;
    if (app.status == Status::Starting)
        bw = badge(kBarX, by, 3, L"Starting " + game_name() + L"…", badgeMax);
    else if (app.status == Status::Error)
        bw = badge(kBarX, by, 2, app.error, badgeMax);
    else if (!app.iniFound)
        bw = badge(kBarX, by, 2, L"sim-qvfr.ini not found next to the launcher.", badgeMax);
    else if (app.undoRestore) {
        bw = badge(kBarX, by, 3, L"Defaults restored for " + short_name() + L" · Undo", badgeMax);
        app.hits.push_back({kUndoBadge, {kBarX, by, bw, 20}});
    } else if (launchFirst)
        bw = launch_badge();
    else if (const Check* bad = first_with(app.checks, CheckTone::Bad))
        bw = checks_badge(by, 2, *bad, badgeMax);
    else if (eye_warning())
        bw = badge(kBarX, by, 1, L"Eye tracker not detected", badgeMax);
    else if (app.runtime.known() && ro.overRuntimeLimit)
        bw = badge(kBarX, by, 1, L"Larger than the runtime’s maximum image size", badgeMax);
    else if (const Check* warn = first_with(app.checks, CheckTone::Warn))
        bw = checks_badge(by, 1, *warn, badgeMax);
    else if (app.live == Live::NextLaunch)
        bw = badge(kBarX, by, 1, app.s.mode == RenderMode::Qvfr
                                     ? L"Applied live up to the launch resolution · the rest at the next launch"
                                     : L"Foveation applied live · resolution and method at the next launch",
                   badgeMax);
    else if (app.live == Live::Applied)
        bw = badge(kBarX, by, 0, L"Applied to the running game", badgeMax);
    else if (watching)
        bw = launch_badge();
    else if (app.update == Update::Available) {
        bw = badge(kBarX, by, 3,
                   app.installing ? L"Installing Sim-QVFR " + app.release.tag + L"…"
                                  : L"Sim-QVFR " + app.release.tag + L" is available" + (installable() ? L" · Install" : L""),
                   badgeMax);
        app.hits.push_back({kUpdateBadge, {kBarX, by, bw, 20}});
    } else
        bw = badge(kBarX, by, 0, L"Ready", badgeMax);
    // What the launch will use, after the status when there is room: the profile, the mode, the focus density.
    {
        const std::wstring profile = !app.profileCurrent.empty() && app.profileMatches ? app.profileCurrent : L"Custom";
        const std::wstring rest = L"  ·  " + std::wstring(app.s.eyeTracking ? L"Eye tracked" : L"Fixed") + L"  ·  Focus " +
                                  std::to_wstring(static_cast<int>(std::lround(ro.focusDensity * 100))) + L"%";
        const float sx = kBarX + bw + t::kSpace4, room = badgeMax + kBarX - sx;
        const float w1 = std::ceil(cv.measure(L"Profile ", t::kBody)), w2 = std::ceil(cv.measure(profile, kBodyMedium)),
                    w3 = std::ceil(cv.measure(rest, t::kBody));
        if (room > 1 + t::kSpace4 + w1 + w2 + w3) {
            cv.line(sx, by + 2, sx, by + 18, col(app.c->card_border), 1);
            const float tx = sx + t::kSpace4;
            cv.text(L"Profile ", t::kBody, {tx, by - 2, w1 + 2, 24}, col(app.c->label_secondary));
            cv.text(profile, kBodyMedium, {tx + w1, by - 2, w2 + 2, 24}, col(app.c->label));
            cv.text(rest, t::kBody, {tx + w1 + w2, by - 2, w3 + 2, 24}, col(app.c->label_secondary));
        }
    }
    button(kRestore, restoreR, L"Restore Defaults", Btn::Ghost, kIconReset);
    if (game_running())  // a second launch would start nothing: the game's state is in Launch checks
        button(kLaunch, launchR, short_name() + L" is running", Btn::Secondary);
    else
        button(kLaunch, launchR, launchLabel, Btn::Primary, kIconPlay);

    if (app.sheet.open)
        sheet_card();  // over everything: nothing under it answers
    else
        help_tip();

    // An open pop-up menu, over everything.
    if (app.popup.open) {
        const Popup& p = app.popup;
        const float itemH = kMenuItemH;
        const Rect menu = popup_menu_rect();
        cv.fill({menu.x, menu.y + 4, menu.w, menu.h}, {0, app.dark ? 0.5f : 0.16f}, t::kRadiusGroup);
        cv.fill(menu, col(app.dark ? app.c->control_bg : app.c->group_bg), t::kRadiusGroup);
        cv.stroke(menu, col(app.dark ? app.c->control_hover : app.c->card_border), 1, t::kRadiusGroup);
        for (size_t i = 0; i < p.items.size(); ++i) {
            const Rect item{menu.x + 6, menu.y + 6 + itemH * i, menu.w - 12, itemH};
            const bool hot = static_cast<int>(i) == p.hover;
            if (hot)
                cv.fill(item, col(app.c->accent_fill), 6);
            if (static_cast<int>(i) == p.selected)
                cv.path("M3.5 8.2l2.6 2.6L12.5 4.5", {item.x + 8, item.y + (itemH - 14) / 2, 14, 14}, 16,
                        col(hot ? app.c->on_accent : app.c->accent), 1.8f);
            cv.text(p.items[i], t::kBody, {item.x + 28, item.y, item.w - 34, itemH}, col(hot ? app.c->on_accent : app.c->label));
        }
    }
    cv.end();
    place_report_edit();
    place_sheet_edit();
}

// ---- Input -------------------------------------------------------------------------------------

const Hit* hit_at(float x, float y) {
    for (auto it = app.hits.rbegin(); it != app.hits.rend(); ++it)
        if (it->r.contains(x, y))
            return &*it;
    return nullptr;
}

void set_slider_from_x(float x) {
    int* v = slider_value(app.dragging);
    if (!v)
        return;
    const float travel = app.dragTrack.w - 18;
    const float f = std::clamp((x - app.dragTrack.x - 9) / travel, 0.0f, 1.0f);
    const int before = *v;
    // Inner resolution in its steps (100% reachable); the arrow keys still move every slider by 1.
    *v = slider_value_at(app.dragRange, f, app.dragging == kInner ? kInnerDragStep : 1);
    if (app.dragging == kColorValue)
        apply_color_edit();  // the brightness: the edited game's colour follows
    changed(false);
    if (*v != before && app.iniFound)
        push_live();  // the running game follows the knob; the settings save once it is let go
}

// The place a preview drag or the arrows change: the settings menu's or the performance overlay's.
std::pair<int*, int*> placement_of(Ctl id) {
    return id == kOsdPanel ? std::pair{&app.s.osd.yaw, &app.s.osd.pitch} : std::pair{&app.s.menuYaw, &app.s.menuPitch};
}

// Dragging a panel in the placement preview: its centre follows the pointer (less where it was held).
void set_panel_from(float x, float y) {
    const float k = app.placeFrame.w / kRefFovX;
    if (k <= 0)
        return;
    const auto [yawOf, pitchOf] = placement_of(app.dragging);
    const int yaw = std::clamp(static_cast<int>(std::lround((x - app.placeFrame.x) / k - kRefFovX / 2 + app.grabYaw)),
                               kMenuYawRange.min, kMenuYawRange.max);
    const int pitch = std::clamp(static_cast<int>(std::lround(kRefFovY / 2 - (y - app.placeFrame.y) / k + app.grabPitch)),
                                 kMenuPitchRange.min, kMenuPitchRange.max);
    const bool moved = yaw != *yawOf || pitch != *pitchOf;
    *yawOf = yaw;
    *pitchOf = pitch;
    changed(false);
    if (moved && app.iniFound)
        push_live();  // the running game moves it as it is dragged; the settings save once it is let go
}

void open_popup(Ctl owner) {
    for (const Hit& h : app.hits)
        if (h.id == owner) {
            app.popup.anchor = h.r;
            break;
        }
    app.popup.owner = owner;
    if (owner == kCheekyOuterPreset || owner == kCheekyFocusPreset) {  // Cheeky's presets (the outer views have no game default)
        const bool focus = owner == kCheekyFocusPreset;
        const int value = focus ? app.s.cheeky.focusPreset : app.s.cheeky.outerPreset;
        app.popup.items.clear();
        app.popup.selected = 0;
        for (const CheekyPreset& p : kCheekyPresets)
            if (focus || p.value != 0) {
                if (p.value == value)
                    app.popup.selected = static_cast<int>(app.popup.items.size());
                app.popup.items.push_back(p.name);
            }
    } else if (owner == kCheekyNrPreset) {
        app.popup.items.assign(std::begin(kCheekyNrPresets), std::end(kCheekyNrPresets));
        app.popup.selected = app.s.cheeky.nrPreset;
    } else if (owner == kFgMultiplier) {
        app.popup.items.assign(std::begin(kFgMultipliers), std::end(kFgMultipliers));
        app.popup.selected = std::clamp(app.s.cheeky.fgMultiplier - 2, 0, 2);
    } else if (owner == kFgRelease) {
        app.popup.items.assign(std::begin(kCheekyFgReleaseNames), std::end(kCheekyFgReleaseNames));
        app.popup.selected = cheeky_fg_release_index(app.s.cheeky.fgRelease);
    } else if (owner == kFgScale || owner == kFgPeriphery) {
        app.popup.items.assign(std::begin(kFgQuarters), std::end(kFgQuarters));
        app.popup.selected = std::clamp((owner == kFgScale ? app.s.cheeky.fgScale : app.s.cheeky.fgPeriphery) / 25 - 1, 0, 3);
    } else if (owner == kVisor) {
        app.popup.items.assign(std::begin(kVisorPositionNames), std::end(kVisorPositionNames));
        app.popup.selected = static_cast<int>(app.s.helmet.visor);
    } else if (owner == kShape) {
        app.popup.items.assign(std::begin(kShapeNames), std::end(kShapeNames));
        app.popup.selected = app.s.shape.corners ? 1 : 0;
    } else if (owner == kDiagKind) {
        app.popup.items.assign(std::begin(kIssueKinds), std::end(kIssueKinds));
        app.popup.selected = app.reportKind;
    } else if (owner == kHelmetDetail) {
        app.popup.items = {L"Performance (2048)", L"Detail (4096)"};
        app.popup.selected = app.s.helmet.detail ? 1 : 0;
    } else if (owner == kLayerProfile) {
        app.popup.items = {kLayerProfileNames[0], kLayerProfileNames[1]};
        app.popup.selected = app.layerProfile;
    } else if (owner == kCheekyNrPasses) {
        app.popup.items = {L"1 pass", L"2 passes", L"3 passes"};
        app.popup.selected = app.s.cheeky.nrPasses - 1;
    } else {
        app.popup.items = owner == kMethod ? std::vector<std::wstring>{kMethodNames[0], kMethodNames[1]}
                                           : std::vector<std::wstring>{kModeNames[0], kModeNames[1]};
        app.popup.selected = owner == kMethod ? (app.s.mode == RenderMode::Qvfr ? 0 : 1) : (app.s.eyeTracking ? 0 : 1);
    }
    app.popup.hover = app.popup.selected;
    app.popup.open = true;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void choose(Ctl owner, int option) {
    if (owner == kDiagKind) {  // not a setting
        app.reportKind = option;
        return;
    }
    if (owner == kMethod)
        app.s.mode = option == 0 ? RenderMode::Qvfr : RenderMode::TwoLoop;
    else if (owner == kMode)
        app.s.eyeTracking = option == 0;
    else if (owner == kCheekyFocusPreset)
        app.s.cheeky.focusPreset = kCheekyPresets[option].value;
    else if (owner == kCheekyOuterPreset)
        app.s.cheeky.outerPreset = kCheekyPresets[option + 1].value;  // the list without the game default
    else if (owner == kCheekyNrPreset)
        app.s.cheeky.nrPreset = option;
    else if (owner == kCheekyNrPasses)
        app.s.cheeky.nrPasses = option + 1;
    else if (owner == kFgMultiplier)
        app.s.cheeky.fgMultiplier = option + 2;
    else if (owner == kFgScale)
        app.s.cheeky.fgScale = (option + 1) * 25;
    else if (owner == kFgPeriphery)
        app.s.cheeky.fgPeriphery = (option + 1) * 25;
    else if (owner == kFgRelease)
        app.s.cheeky.fgRelease = kCheekyFgReleasePoints[std::clamp(option, 0, 2)];
    else if (owner == kVisor)
        app.s.helmet.visor = static_cast<VisorPosition>(std::clamp(option, 0, 2));
    else if (owner == kHelmetDetail)
        app.s.helmet.detail = option == 1;
    else if (owner == kShape)
        app.s.shape.corners = option == 1;
    else if (owner == kLayerProfile) {
        app.layerProfile = std::clamp(option, 0, 1);
        app.layerRow = 0;
        app.layerMessage.clear();
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    changed();
}

// A game tab: [launcher] game, then everything that follows the game (its checks, its features, its palette).
void select_game(int option) {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    if (option < 0 || static_cast<std::size_t>(option) >= count || all[option] == &games::launcher_game())
        return;
    if (app.iniFound) {
        // Each game keeps its own settings: this one's go to its copy, the other's come back into the ini.
        switch_game_settings(app.ini, widen(games::launcher_game().id), widen(all[option]->id));
        WritePrivateProfileStringW(L"launcher", L"game", widen(all[option]->id).c_str(), app.ini.c_str());
        choose_launcher_game();
        app.s = load_settings(app.ini);
        app.iniText = ini_text();
        app.undoRestore.reset();  // it held the other game's settings
        refresh_profiles();  // the profile in use is the game's
    } else {
        games::set_launcher_game(*all[option]);  // for this window only: there is no ini to keep it in
    }
    if (!pane_shown(static_cast<int>(app.pane)))
        app.pane = static_cast<Pane>(shown_pane(static_cast<int>(app.pane), -1, 0));
    apply_theme();
    refresh_checks();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int game_index() {
    std::size_t count = 0;
    const games::GameInfo* const* all = games::all_games(count);
    for (std::size_t i = 0; i < count; ++i)
        if (all[i] == &games::launcher_game())
            return static_cast<int>(i);
    return 0;
}

void close_popup(bool accept) {
    if (accept && app.popup.hover >= 0)
        choose(app.popup.owner, app.popup.hover);
    app.popup.open = false;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

int popup_item_at(float x, float y) {
    const Popup& p = app.popup;
    const float itemH = kMenuItemH;
    const Rect menu = popup_menu_rect();
    for (size_t i = 0; i < p.items.size(); ++i)
        if (Rect{menu.x + 6, menu.y + 6 + itemH * i, menu.w - 12, itemH}.contains(x, y))
            return static_cast<int>(i);
    return -1;
}

// Picks Cheeky's runtime DLL (from Cheeky's Standalone ZIP) and saves its path to [cheeky] runtime.
void choose_cheeky_runtime() {
    wchar_t path[MAX_PATH] = {};
    const std::wstring current = cheeky_runtime_path(app.ini, layer_folder());
    wcsncpy_s(path, current.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFilter = L"Cheeky runtime (CheekyFoveatedDLSSRuntime.dll)\0CheekyFoveatedDLSSRuntime.dll\0DLL files\0*.dll\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose Cheeky Foveated DLSS's runtime";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) && app.iniFound) {
        save_cheeky_runtime(app.ini, path);
        app.iniText = ini_text();
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

}  // namespace

// Launch checks' Choose folder… (a game Steam doesn't list): the game's folder, saved as [launcher] game_dir_<id>.
void choose_game_folder() {
    const games::GameInfo& game = games::launcher_game();
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const std::wstring title = L"Choose " + game_name() + L"’s folder";
    dialog->SetTitle(title.c_str());
    std::wstring picked;
    if (SUCCEEDED(dialog->Show(app.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (picked.empty())
        return;
    const std::wstring folder = game_folder_from(picked, game);
    if (folder.empty()) {
        notify(L"That folder doesn’t hold " + game_name(),
               picked + L" has no " + game.exeName + L". Choose the folder Steam’s Browse local files opens for " + game_name() + L".");
        return;
    }
    if (!app.iniFound || !WritePrivateProfileStringW(L"launcher", (L"game_dir_" + widen(game.id)).c_str(), folder.c_str(),
                                                     app.ini.c_str())) {
        notify(L"The folder couldn’t be saved", L"sim-qvfr.ini wasn’t found next to the launcher.");
        return;
    }
    app.iniText = ini_text();
    refresh_installed();  // found now
    if (app.status == Status::Error)
        app.status = Status::Idle;  // "not found: choose its folder" is answered
}

namespace {

// Restore Defaults' Undo: the settings from before it.
void undo_restore() {
    if (!app.undoRestore)
        return;
    app.s = *app.undoRestore;
    changed();  // also ends the Undo
}

void end_capture() {
    if (app.captureAction < 0)
        return;
    app.captureAction = -1;
    KillTimer(app.hwnd, kCaptureTimer);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// The Hotkeys pane: `action`'s key or (pad) wheel button takes the next press.
void start_capture(int action, bool pad) {
    if (pad) {
        open_pads();
        if (!app.padsOpen) {
            notify(L"Wheel buttons can’t be read", L"DirectInput isn’t available, so wheel and controller buttons can’t be read.");
            return;
        }
        app.captureHeld = app.pads.poll();
        SetTimer(app.hwnd, kCaptureTimer, kCaptureMs, nullptr);
    }
    app.captureAction = action;
    app.capturePad = pad;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void set_hotkey(const KeyCombo* key, const PadControl* pad) {
    Hotkey& h = app.s.hotkeys[app.captureAction];
    if (key)
        h.key = *key;
    if (pad)
        h.pad = *pad;
    end_capture();
    changed();
}

// A key while catching one: Esc cancels, Delete or Backspace alone clears, modifiers wait for their key.
bool capture_key(WPARAM vk) {
    if (app.captureAction < 0)
        return false;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (vk == VK_ESCAPE) {
        end_capture();
    } else if ((vk == VK_DELETE || vk == VK_BACK) && !ctrl && !alt && !shift) {
        const KeyCombo none;
        const PadControl noPad;
        set_hotkey(app.capturePad ? nullptr : &none, app.capturePad ? &noPad : nullptr);
    } else if (!app.capturePad && vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LWIN && vk != VK_RWIN) {
        const KeyCombo key{static_cast<int>(vk), ctrl, alt, shift};
        set_hotkey(&key, nullptr);
    }
    return true;
}

// The capture timer: the first control pressed that wasn't held when the capture started.
void capture_poll() {
    if (app.captureAction < 0 || !app.capturePad)
        return;
    const std::vector<PadHeld> held = app.pads.poll();
    const auto in = [](const std::vector<PadHeld>& list, const PadHeld& p) {
        for (const PadHeld& q : list)
            if (q.control == p.control && q.device == p.device)
                return true;
        return false;
    };
    for (const PadHeld& p : held)
        if (!in(app.captureHeld, p)) {
            const PadControl pad{p.device, p.control, p.name};
            set_hotkey(nullptr, &pad);
            return;
        }
    // A control let go since counts when pressed again.
    std::vector<PadHeld> still;
    for (const PadHeld& p : app.captureHeld)
        if (in(held, p))
            still.push_back(p);
    app.captureHeld = std::move(still);
}

}  // namespace

void activate(Ctl id, int option) {
    end_capture();  // any click ends catching a hotkey (one on a hotkey's button starts again)
    switch (id) {
    case kSidebar:
        app.pane = static_cast<Pane>(option);
        app.scroll = 0;
        if (app.pane == Pane::Layers)
            refresh_layers();
        if (app.pane == Pane::Hotkeys)
            open_pads();
        if (app.pane == Pane::Profiles) {
            app.profileMessage.clear();
            refresh_profiles();
        }
        if (app.pane == Pane::Diagnostics || app.pane == Pane::Report)
            reload_runtime();
        break;
    case kSheetButton: close_sheet(option); return;
    case kChecksBadge: activate(kSidebar, static_cast<int>(Pane::Checks)); return;
    case kUndoBadge: undo_restore(); return;
    case kLaunchBadge: {
        const LaunchOutcome o = app.watch.outcome();
        if (o.link == LaunchLink::Diagnostics) {
            activate(kSidebar, static_cast<int>(Pane::Diagnostics));
        } else if (o.link == LaunchLink::Report) {
            if (o.state == LaunchState::Crashed)
                app.reportKind = 1;  // kIssueKinds: Crash or freeze
            activate(kSidebar, static_cast<int>(Pane::Report));
            if (o.finished())
                app.watch.stop();  // seen: Report an issue has its logs
        }
        return;
    }
    case kHotkeyKey:
    case kHotkeyPad:
        if (option >= 0 && option < kHotkeyActions)
            start_capture(option, id == kHotkeyPad);
        return;
    case kHotkeysReset:
        app.s.hotkeys = default_hotkeys();
        changed();
        return;
    case kProfileUse:
    case kProfileRevert:
    case kProfileUpdate:
    case kProfileShare:
    case kProfileDelete: {
        if (option < 0 || option >= static_cast<int>(app.profileNames.size()))
            return;
        const std::wstring name = app.profileNames[option];
        Profile p;
        if (id == kProfileUse || id == kProfileRevert) {
            use_profile(name);
            if (id == kProfileRevert && app.profileMatches)
                app.profileMessage = L"Back to " + name + L".";
        } else if (id == kProfileUpdate) {  // the settings now, into the profile in use: no question, it's its own button
            if (save_profile(profiles_folder(app.ini), settings_profile(app.s, name))) {
                app.profileMessage = L"Updated " + name + L" with the settings you have now.";
            } else {
                app.profileMessage = L"Couldn’t update " + name + L".";
            }
            refresh_profiles();
        } else if (id == kProfileShare && load_profile(profiles_folder(app.ini), name, p, app.s.eyeTracking)) {
            app.profileMessage = copy_text(widen(share_profile(p)))
                                     ? L"Copied " + name + L" to the clipboard: paste it in a chat or a post."
                                     : L"Couldn’t copy to the clipboard.";
        } else if (id == kProfileDelete) {
            Sheet s;
            s.title = L"Delete " + name + L"?";
            s.text = L"It goes from Your profiles. The settings you have now stay as they are.";
            s.buttons = {L"Cancel", L"Delete"};
            s.focus = 0;
            s.danger = true;
            s.done = [name](int button, const std::wstring&) {
                if (button != 1)
                    return;
                delete_profile(profiles_folder(app.ini), name);
                if (_wcsicmp(name.c_str(), app.profileCurrent.c_str()) == 0)
                    set_current_profile(app.ini.c_str(), L"");
                app.profileMessage = L"Deleted " + name + L".";
                refresh_profiles();
            };
            open_sheet(std::move(s));
        }
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    case kProfileSave: save_current_profile(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kProfilePaste: paste_profile(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kCheckFix:
        if (option >= 0 && option < static_cast<int>(app.checks.size()))
            fix_check(app.checks[option]);
        return;
    case kDiagCapture: app.s.cpuProfile = !app.s.cpuProfile; changed(); return;
    case kDiagFrameCapture: app.s.frameCapture = !app.s.frameCapture; changed(); return;
    case kDiagBaseline: app.s.baselineNext = !app.s.baselineNext; changed(); return;
    case kDiagKind: open_popup(id); return;
    case kDiagDescription:
        if (!report_edit_fits()) {  // scroll the field into view first: the edit shows only then
            const Rect& r = app.reportEditRect;
            const float by = r.bottom() > app.scrollArea.bottom() ? r.bottom() + 5 - app.scrollArea.bottom() : r.y - 5 - app.scrollArea.y;
            app.scroll = std::clamp(app.scroll + by + (by > 0 ? t::kSpace4 : -t::kSpace4), 0.0f, app.scrollMax);
            RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
        SetFocus(app.reportEdit);
        return;
    case kDiagReport: report_issue(); return;
    case kDiagExport: export_logs(); return;
    case kDiagZip: show_report_zip(); return;  // the keyboard's; the mouse drags it (drag_report_zip)
    case kDiagCopy: copy_summary(); return;
    case kDiagLogs: ShellExecuteW(app.hwnd, L"explore", logs_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return;
    case kLayerProfile: open_popup(id); return;
    case kLayerOn: app.layerRow = option; toggle_layer(option); break;
    case kLayerUp: move_layer(option, -1); break;
    case kLayerDown: move_layer(option, 1); break;
    case kLayerApply: apply_layers(); break;
    case kLayerRevert:
        app.layersDraft = app.layersWindows;
        app.layersLockMoved = false;
        app.layerMessage.clear();
        refresh_layers(false);
        break;
    case kLayerReset:
        edit_launcher_layers([](LauncherLayers& p) { p = {}; });
        app.layerMessage.clear();
        break;
    case kGameTab: select_game(option); return;
    case kMethod:
    case kMode:
    case kCheekyOuterPreset:
    case kCheekyFocusPreset:
    case kCheekyNrPreset:
    case kCheekyNrPasses:
    case kFgMultiplier:
    case kFgScale:
    case kFgPeriphery:
    case kFgRelease:
    case kVisor:
    case kShape:
    case kHelmetDetail: open_popup(id); return;
    case kFgOn:
        if (game_has(&games::GameFeatures::frameGeneration)) {  // greyed out for a game without it
            app.s.cheeky.fg = !app.s.cheeky.fg;
            changed();
        }
        return;
    case kOsdOn: app.s.osd.enabled = !app.s.osd.enabled; changed(); return;
    case kHotkeyFooter: app.s.hotkeyFooter = !app.s.hotkeyFooter; changed(); return;
    case kUpdateCheck:
        app.s.updateCheck = !app.s.updateCheck;
        app.s.updateAsked = true;  // a choice: the first start's question is answered
        changed();
        if (app.s.updateCheck && app.update == Update::Off)
            start_update_check();
        return;
    case kUpdateNow: start_update_check(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kUpdateDownload:
        if (!app.release.url.empty() && app.release.url.rfind(L"https://github.com/", 0) == 0)
            ShellExecuteW(app.hwnd, L"open", app.release.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kUpdateBadge: activate(kSidebar, static_cast<int>(Pane::About)); return;
    case kUpdateInstall: start_install(); InvalidateRect(app.hwnd, nullptr, FALSE); return;
    case kGameShown: {
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        if (option < 0 || static_cast<std::size_t>(option) >= count)
            return;
        const bool hide = game_shown(*all[option]);
        if (hide && shown_game_count() <= 1)
            return;  // the last tab stays
        // An installed game is hidden by its listing in hidden_games; one Steam doesn't find is shown by shown_games.
        if (installed(*all[option]))
            app.s.hiddenGames = with_game_hidden(app.s.hiddenGames, game_id(*all[option]), hide);
        else
            app.s.shownGames = with_game_hidden(app.s.shownGames, game_id(*all[option]), !hide);
        changed();
        if (hide && all[option] == &games::launcher_game())  // the game in use went: the first shown takes over
            for (std::size_t i = 0; i < count; ++i)
                if (game_shown(*all[i])) {
                    select_game(static_cast<int>(i));
                    break;
                }
        return;
    }
    case kColorGame:
        app.colorGame = option;
        app.colorLoaded = -2;
        load_color_edit();
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    case kColorReset:
        app.s.gameColors.erase(game_id(color_game()));
        app.colorLoaded = -2;
        load_color_edit();
        apply_theme();
        changed();
        return;
    case kCropOn:
        if (app.s.mode == RenderMode::Qvfr && game_has(&games::GameFeatures::fovCrop)) {
            app.s.crop = !app.s.crop;
            changed();
        }
        return;
    case kHelmetOn:
        if (app.s.mode == RenderMode::Qvfr && game_has(&games::GameFeatures::helmet)) {
            app.s.helmet.on = !app.s.helmet.on;
            changed();
        }
        return;
    case kFgBorder: app.s.cheeky.fgBorder = !app.s.cheeky.fgBorder; changed(); return;
    case kCheekyMotionFix: app.s.cheeky.motionFix = !app.s.cheeky.motionFix; changed(); return;
    case kCheekyNr: app.s.cheeky.nr = !app.s.cheeky.nr; changed(); return;
    case kOutline: app.s.outline = !app.s.outline; changed(); return;
    case kCheekyUse: app.s.cheeky.load = !app.s.cheeky.load; changed(); return;
    case kCheekyOn: app.s.cheeky.processing = !app.s.cheeky.processing; changed(); return;
    case kCheekyOuter: app.s.cheeky.outerDlaa = !app.s.cheeky.outerDlaa; changed(); return;
    case kCheekyRuntime: choose_cheeky_runtime(); return;
    case kColor: app.s.color = static_cast<OutlineColor>(option); changed(); return;
    case kRestore: {
        const Settings before = app.s;
        app.s = restored_defaults(app.s);
        changed();
        app.undoRestore = before;  // the action bar offers Undo until the next change
        return;
    }
    case kLaunch: launch(); return;
    case kSupport: ShellExecuteW(app.hwnd, L"open", kSupportUrl, nullptr, nullptr, SW_SHOWNORMAL); return;
    default: break;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

namespace {

float dip(LPARAM lp, bool y) {
    return (y ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp)) / app.canvas.dpi_scale();
}

// A push button: Return presses it while it has the focus (as on Windows); anywhere else Return presses Launch, the
// window's default button. Space presses these too.
bool push_button(Ctl id) {
    switch (id) {
    case kCheekyRuntime:
    case kUpdateNow:
    case kUpdateDownload:
    case kUpdateInstall:
    case kUpdateBadge:
    case kLayerApply:
    case kLayerRevert:
    case kLayerReset:
    case kLayerUp:
    case kLayerDown:
    case kSupport:
    case kRestore:
    case kLaunch:
    case kHotkeysReset:
    case kHotkeyKey:
    case kHotkeyPad:
    case kProfileUse:
    case kProfileShare:
    case kProfileDelete:
    case kProfileUpdate:
    case kProfileRevert:
    case kProfileSave:
    case kProfilePaste:
    case kCheckFix:
    case kChecksBadge:
    case kUndoBadge:
    case kLaunchBadge:
    case kDiagReport:
    case kDiagZip:
    case kDiagExport:
    case kDiagCopy:
    case kDiagLogs:
    case kColorReset: return true;
    default: return false;
    }
}

// A switch or a pop-up button: Space toggles or opens it; Return, as on Windows, presses Launch.
bool space_control(Ctl id) {
    switch (id) {
    case kMethod:
    case kMode:
    case kOutline:
    case kCheekyUse:
    case kCheekyOn:
    case kCheekyOuter:
    case kCheekyOuterPreset:
    case kCheekyFocusPreset:
    case kCheekyMotionFix:
    case kCheekyNr:
    case kCheekyNrPreset:
    case kCheekyNrPasses:
    case kFgOn:
    case kFgBorder:
    case kFgMultiplier:
    case kFgScale:
    case kFgPeriphery:
    case kFgRelease:
    case kOsdOn:
    case kHotkeyFooter:
    case kUpdateCheck:
    case kCropOn:
    case kHelmetOn:
    case kVisor:
    case kShape:
    case kHelmetDetail:
    case kLayerProfile:
    case kLayerOn:
    case kDiagCapture:
    case kDiagFrameCapture:
    case kDiagBaseline:
    case kDiagKind:
    case kGameShown:
    case kColorGame: return true;
    default: return false;
    }
}

void on_key(WPARAM key) {
    if (app.sheet.open) {
        sheet_key(key);
        return;
    }
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (app.popup.open) {
        const int n = static_cast<int>(app.popup.items.size());
        if (key == VK_UP || key == VK_DOWN)
            app.popup.hover = (app.popup.hover + (key == VK_DOWN ? 1 : n - 1)) % n;
        else if (key == VK_RETURN || key == VK_SPACE)
            close_popup(true);
        else if (key == VK_ESCAPE)
            close_popup(false);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == 'Z' && GetKeyState(VK_CONTROL) < 0 && app.undoRestore) {  // Ctrl+Z: Restore Defaults' Undo
        undo_restore();
        return;
    }
    if (key == VK_TAB && GetKeyState(VK_CONTROL) < 0) {  // Ctrl+Tab, Ctrl+Shift+Tab: the next or previous game with a tab
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        const int n = static_cast<int>(count);
        for (int step = 1; step < n; ++step) {
            const int i = (game_index() + (shift ? n - step : step)) % n;
            if (game_shown(*all[i])) {
                select_game(i);
                break;
            }
        }
        return;
    }
    if (key == VK_TAB) {
        const auto order = focus_order();
        auto it = std::find(order.begin(), order.end(), app.focus);
        int i = it == order.end() ? (shift ? 0 : -1) : static_cast<int>(it - order.begin());
        i = (i + (shift ? static_cast<int>(order.size()) - 1 : 1)) % static_cast<int>(order.size());
        app.focus = order[i];
        app.focusOption = -1;
        app.keyboard = true;
        if (app.focus == kDiagDescription)  // typing goes to the description box
            activate(kDiagDescription, -1);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_RETURN) {  // the focused button, else Launch (the window's default button)
        if (push_button(app.focus))
            activate(app.focus, app.focusOption);
        else
            launch();
        return;
    }
    app.keyboard = true;
    if (int* v = slider_value(app.focus)) {
        const Range r = slider_range(app.focus);
        int nv = *v;
        if (key == VK_LEFT || key == VK_DOWN)
            nv -= 1;
        else if (key == VK_RIGHT || key == VK_UP)
            nv += 1;
        else if (key == VK_PRIOR)
            nv += 10;
        else if (key == VK_NEXT)
            nv -= 10;
        else if (key == VK_HOME)
            nv = r.min;
        else if (key == VK_END)
            nv = r.max;
        else
            return;
        *v = std::clamp(nv, r.min, r.max);
        changed();
        return;
    }
    switch (app.focus) {
    case kLayerList: {  // Up/Down pick a row, Space switches it, Ctrl+Up/Down moves it
        const int n = layer_rows();
        if (n == 0)
            break;
        app.layerRow = std::clamp(app.layerRow, 0, n - 1);
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if ((key == VK_UP || key == VK_DOWN) && ctrl)
            move_layer(app.layerRow, key == VK_UP ? -1 : 1);
        else if (key == VK_UP || key == VK_DOWN)
            app.layerRow = std::clamp(app.layerRow + (key == VK_UP ? -1 : 1), 0, n - 1);
        else if (key == VK_SPACE)
            toggle_layer(app.layerRow);
        InvalidateRect(app.hwnd, nullptr, FALSE);
        break;
    }
    case kGameTab: {  // Left and Right pick the game, Home and End the first and last (shown games only)
        std::size_t count = 0;
        const games::GameInfo* const* all = games::all_games(count);
        const auto next_shown = [&](int from, int step) {
            for (int i = from; i >= 0 && i < static_cast<int>(count); i += step)
                if (game_shown(*all[i]))
                    return i;
            return -1;
        };
        int to = -1;
        if (key == VK_LEFT || key == VK_RIGHT)
            to = next_shown(game_index() + (key == VK_RIGHT ? 1 : -1), key == VK_RIGHT ? 1 : -1);
        else if (key == VK_HOME || key == VK_END)
            to = key == VK_HOME ? next_shown(0, 1) : next_shown(static_cast<int>(count) - 1, -1);
        if (to >= 0)
            select_game(to);
        break;
    }
    case kSidebar:
        if (key == VK_UP || key == VK_DOWN) {
            const int step = key == VK_DOWN ? 1 : -1, here = static_cast<int>(app.pane);
            activate(kSidebar, shown_pane(std::clamp(here + step, 0, kPanes - 1), step, here));
        }
        break;
    case kMenuPanel:
    case kOsdPanel: {  // arrows move the panel a degree, Page Up / Page Down ten
        const int step = key == VK_PRIOR || key == VK_NEXT ? 10 : 1;
        const int dx = key == VK_RIGHT ? step : key == VK_LEFT ? -step : 0;
        const int dy = key == VK_UP || key == VK_PRIOR ? step : key == VK_DOWN || key == VK_NEXT ? -step : 0;
        if (dx || dy) {
            const auto [yawOf, pitchOf] = placement_of(app.focus);
            *yawOf = std::clamp(*yawOf + dx, kMenuYawRange.min, kMenuYawRange.max);
            *pitchOf = std::clamp(*pitchOf + dy, kMenuPitchRange.min, kMenuPitchRange.max);
            changed();
        }
        break;
    }
    case kColor:
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN) {
            const int d = key == VK_RIGHT || key == VK_DOWN ? 1 : 2;
            activate(kColor, (static_cast<int>(app.s.color) + d) % 3);
        }
        break;
    default:
        if (key == VK_SPACE && (push_button(app.focus) || space_control(app.focus)))
            activate(app.focus, app.focusOption);
        break;
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        app.hwnd = hwnd;
        apply_theme();
        if (!app.canvas.initialize(hwnd))
            return -1;
        app.iniText = ini_text();
        SetTimer(hwnd, kFollowTimer, 500, nullptr);
        create_report_edit();
        create_sheet_edit();
        refresh_installed();
        if (!game_shown(games::launcher_game())) {  // e.g. ACE, the first default, where only another game is installed
            std::size_t count = 0;
            const games::GameInfo* const* all = games::all_games(count);
            for (std::size_t i = 0; i < count; ++i)
                if (game_shown(*all[i])) {
                    select_game(static_cast<int>(i));
                    break;
                }
        }
        refresh_checks();
        if (app.iniFound)
            refresh_profiles();
        // The window opens on Launch checks: what the game, Steam and the layers look like now (often a new day: a game
        // update, Steam closed). The launcher stays open while the game runs, so a tuning session keeps its pane.
        activate(kSidebar, static_cast<int>(Pane::Checks));
        if (app.s.updateCheck)
            start_update_check();
        else if (app.iniFound && !app.s.updateAsked)
            ask_update_check();  // the first start, once
        return 0;
    case kUpdateMessage:
        update_done(reinterpret_cast<UpdateResult*>(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kInstallProgressMessage: {
        const std::unique_ptr<InstallProgress> p(reinterpret_cast<InstallProgress*>(lParam));
        app.installProgress = *p;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case kInstallDoneMessage:
        install_done(reinterpret_cast<InstallResult*>(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kHashedMessage: {
        const std::unique_ptr<std::string> sha(reinterpret_cast<std::string*>(lParam));
        app.exeSha = *sha;
        app.hashing = false;
        refresh_checks();  // also hashes again if the exe changed meanwhile
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CTLCOLOREDIT:  // the description box in the theme's field colours
        if (reinterpret_cast<HWND>(lParam) == app.reportEdit || reinterpret_cast<HWND>(lParam) == app.sheetEdit) {
            if (!app.reportBrush || app.reportBrushRgb != app.c->field_bg) {
                if (app.reportBrush)
                    DeleteObject(app.reportBrush);
                app.reportBrush = CreateSolidBrush(colorref(app.c->field_bg));
                app.reportBrushRgb = app.c->field_bg;
            }
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, colorref(app.c->label));
            SetBkColor(dc, colorref(app.c->field_bg));
            return reinterpret_cast<LRESULT>(app.reportBrush);
        }
        break;
    case WM_TIMER:
        if (wParam == kCaptureTimer)
            capture_poll();
        if (wParam == kFollowTimer) {
            follow_ini();
            if (app.watch.poll()) {  // the launched game's status changed: the checks follow it (running, starting)
                refresh_checks();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // Launch checks: Steam, ACE, the layers and the game folder change outside the launcher.
            static int ticks = 0;
            if (++ticks % 4 == 0) {
                refresh_checks();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            // A running session: the layer rewrites runtime.ini every ~10 s. Diagnostics and Report an issue follow it,
            // and the session history keeps its numbers whichever pane shows.
            static FILETIME seen{};
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW(runtime_info_path().c_str(), GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &seen) != 0) {
                seen = a.ftLastWriteTime;
                reload_runtime();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        app.canvas.resize();
        app.scroll = std::min(app.scroll, app.scrollMax);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        app.canvas.set_dpi(HIWORD(wParam));
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(hwnd);
        RECT r{0, 0, MulDiv(static_cast<int>(kMinWidth), dpi, 96), MulDiv(static_cast<int>(kMinHeight - kTitlebar), dpi, 96)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            apply_theme();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {  // a session may have reported the headset meanwhile
            reload_runtime();
            refresh_installed();  // a game installed or removed meanwhile
            if (app.pane == Pane::Layers)  // a layer may have been installed or switched meanwhile
                refresh_layers();
            refresh_checks();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN: {
        const float x = dip(lParam, false), y = dip(lParam, true);
        app.keyboard = false;
        if (GetFocus() != hwnd)  // a click outside the description box leaves it
            SetFocus(hwnd);
        if (app.popup.open) {
            app.popup.hover = popup_item_at(x, y);
            close_popup(app.popup.hover >= 0);
            return 0;
        }
        const Hit* h = hit_at(x, y);
        if (app.sheet.open) {  // only its buttons (pressed until the button is let go) and its field
            if (h && h->id == kSheetButton) {
                app.pressed = kSheetButton;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (h && h->id == kSheetField) {
                SetFocus(app.sheetEdit);
            }
            return 0;
        }
        if (!h)
            return 0;
        app.focus = h->id;  // as Windows does: Return now presses it, if it's a button
        app.focusOption = h->option;
        if (slider_value(h->id)) {
            app.dragging = h->id;
            app.dragTrack = h->r;
            app.dragRange = slider_range(h->id);
            SetCapture(hwnd);
            set_slider_from_x(x);
        } else if (h->id == kColorWheel) {
            app.dragging = kColorWheel;
            SetCapture(hwnd);
            set_color_from(x, y);
        } else if (h->id == kMenuPanel || h->id == kOsdPanel) {
            app.dragging = h->id;
            const float k = app.placeFrame.w / kRefFovX;
            const auto [yawOf, pitchOf] = placement_of(h->id);
            // Held on the panel: that point stays under the pointer; on empty space, the centre jumps there.
            app.grabYaw = h->option == 1 ? *yawOf - ((x - app.placeFrame.x) / k - kRefFovX / 2) : 0;
            app.grabPitch = h->option == 1 ? *pitchOf - (kRefFovY / 2 - (y - app.placeFrame.y) / k) : 0;
            SetCapture(hwnd);
            set_panel_from(x, y);
        } else if (h->id == kDiagZip) {
            drag_report_zip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        } else if (h->id == kRestore || h->id == kLaunch || h->id == kSupport || h->id == kLayerApply ||
                   h->id == kLayerRevert || h->id == kLayerReset) {
            app.pressed = h->id;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            activate(h->id, h->option);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = dip(lParam, false), y = dip(lParam, true);
        app.mouseX = x;
        app.mouseY = y;
        if (app.dragging == kMenuPanel || app.dragging == kOsdPanel)
            set_panel_from(x, y);
        else if (app.dragging == kColorWheel)
            set_color_from(x, y);
        else if (app.dragging != kNone)
            set_slider_from_x(x);
        else if (app.popup.open) {
            const int hover = popup_item_at(x, y);
            if (hover != app.popup.hover) {
                app.popup.hover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else {
            const Hit* h = hit_at(x, y);
            const int tip = h && h->id == kHelpIcon ? h->option : -1;
            const Ctl hover = h ? h->id : kNone;
            const int hoverOption = h ? h->option : -1;
            if (tip != app.tip || hover != app.hover || hoverOption != app.hoverOption) {  // hover states follow
                app.tip = tip;
                app.hover = hover;
                app.hoverOption = hoverOption;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (hover != kNone) {  // a WM_MOUSELEAVE when the pointer leaves the window, to clear them
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        app.mouseX = app.mouseY = -1;
        if (app.tip >= 0 || app.hover != kNone) {
            app.tip = -1;
            app.hover = kNone;
            app.hoverOption = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:  // a drag that lost the mouse (no button-up): end it, or follow_ini stays off
        if (app.dragging != kNone) {
            app.dragging = kNone;
            changed();
        }
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (app.dragging != kNone) {
            app.dragging = kNone;
            changed();  // save once the knob is let go
        }
        if (app.pressed != kNone) {
            const Ctl pressed = app.pressed;
            app.pressed = kNone;
            const Hit* h = hit_at(dip(lParam, false), dip(lParam, true));
            if (h && h->id == pressed)
                activate(pressed, h->option);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (app.sheet.open)
            return 0;
        app.tip = -1;  // its ⓘ moves away
        if (app.scrollMax > 0) {
            app.scroll = std::clamp(app.scroll - GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 40, 0.0f, app.scrollMax);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:  // Alt held: the Hotkeys pane catches Alt combinations
        if (capture_key(wParam))
            return 0;
        if (message == WM_SYSKEYDOWN)
            break;
        on_key(wParam);
        return 0;
    case WM_SYSCHAR:
    case WM_SYSKEYUP:
        if (app.captureAction >= 0)
            return 0;  // no beep, and no system menu when Alt is let go, while catching a key
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

}  // namespace simqvfr::launcher::ui::win

namespace simqvfr::launcher {

int run_window(HINSTANCE instance, int show) {
    using namespace ui::win;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);  // dragging the report's zip out (SHDoDragDrop)
    app.ini = settings_path();
    app.iniFound = GetFileAttributesW(app.ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    clean_update_folder(std::filesystem::path(app.ini).parent_path().wstring());  // what an install left but its backup
    if (app.iniFound) {
        // Per-game settings: a game without its own copy yet (the first start after the update, or a new game)
        // starts from the settings in use, so updating changes nothing until a game's settings are changed.
        seed_game_settings(app.ini);
        app.s = load_settings(app.ini);
        app.sessions = load_sessions(sessions_folder(app.ini));
    }
    reload_runtime();
    app.focus = kLaunch;  // Return launches

    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"QvfrLauncher";
    // The app icon (launcher.rc): the large one for Alt+Tab and the taskbar, the small one for the title bar.
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&cls);
    // The kit's 960 x 600 window includes its 38px title bar; the native caption replaces it.
    const UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(static_cast<int>(kDefaultWidth), dpi, 96), MulDiv(static_cast<int>(kDefaultHeight - kTitlebar), dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"Sim-QVFR Launcher", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace simqvfr::launcher

// Report an issue (its panel), the report's zip (and dragging it into the issue), exporting the logs, the Get help card,
// and the description box (a native edit over the canvas).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "../launcher.h"
#include "../launch_plan.h"
#include "../report.h"
#include "../../games/game.h"
#include "launcher_version.h"
#include "window_state.h"
#include "card.h"
#include "controls.h"
#include "form.h"

namespace simqvfr::launcher::ui::win {

namespace {

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

}  // namespace

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

namespace {

float report_zip_section(float x, float y, float w, bool draw);

}  // namespace

// The Report an issue panel's content: what the last session points to, then the kind of problem, what happened and
// the button (report_issue), then the zip to drag into the issue.
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
    return h;
}

// The Get help card: what the last session points to, then reporting an issue (its panel), exporting the logs alone,
// copying the summary, the logs folder.
Card help_card() {
    Card c{Tab::Game, L"Get help"};
    c.body = [](Body& b) {
        const std::vector<Cause> causes = likely_causes(app.s, app.runtime);
        if (causes.empty())
            b.note(L"Nothing stood out in the last session.");
        for (size_t i = 0; i < causes.size() && i < 2; ++i)
            b.row(causes[i].title, causes[i].fix, L"");
        if (causes.size() > 2) {
            std::vector<std::wstring> labels;
            for (size_t i = 2; i < causes.size(); ++i)
                labels.push_back(causes[i].title);
            b.more(std::to_wstring(causes.size() - 2) + L" more", labels, [causes](Body& m) {
                for (size_t i = 2; i < causes.size(); ++i)
                    m.row(causes[i].title, causes[i].fix, L"");
            });
        }
        b.actions({{kReportOpen, L"Report an issue…", Btn::Primary},
                   {kDiagExport, L"Export logs"},
                   {kDiagCopy, L"Copy summary"},
                   {kDiagLogs, L"Open logs folder"}});
        if (!app.exportZip.empty()) {
            const size_t slash = app.exportZip.find_last_of(L'\\');
            b.note(L"Saved " + (slash == std::wstring::npos ? app.exportZip : app.exportZip.substr(slash + 1)) + L" and shown in Explorer.");
        }
        b.note(L"Report an issue opens a GitHub issue with the details filled in and zips the logs for it. Export logs is for "
               L"when you’re asked for logs alone.");
    };
    return c;
}

namespace {

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

}  // namespace

void place_report_edit() {
    const Rect& r = app.reportEditRect;
    if (!app.reportEdit)
        return;
    if (app.panel != PanelKind::Report || !report_edit_fits() || app.popup.open || app.sheet.open) {
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

namespace {

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

}  // namespace

void create_report_edit() {
    app.reportEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                     0, 0, 0, 0, app.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!app.reportEdit)
        return;
    SendMessageW(app.reportEdit, EM_SETLIMITTEXT, 4000, 0);  // the link has room for about that much
    SetPropW(app.reportEdit, L"simqvfr.base", reinterpret_cast<HANDLE>(GetWindowLongPtrW(app.reportEdit, GWLP_WNDPROC)));
    SetWindowLongPtrW(app.reportEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(report_edit_proc));
}

}  // namespace simqvfr::launcher::ui::win

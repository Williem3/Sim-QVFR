// "Report an issue" (the Diagnostics pane): the logs a bug report needs, zipped into one file, and a new
// GitHub issue on the issue form for the kind of problem (.github/ISSUE_TEMPLATE), its fields prefilled: the
// user's description, the known causes the last session shows, and what the launcher can tell on its own
// (settings, the last session, headset, hardware). The zip is stored (not compressed): a small writer here
// rather than a dependency, and GitHub takes attachments up to 25 MB, which kReportMaxBytes stays under.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "settings.h"

namespace simqvfr::launcher {

// Where issues go; [launcher] issue_repo in sim-qvfr.ini overrides it (a test repository with the forms).
inline constexpr wchar_t kIssueRepo[] = L"Williem3/Sim-QVFR";
// GitHub answers 414 above about 8 KB of URL; the fields are trimmed to fit (the zip has all of it).
inline constexpr size_t kIssueUrlMax = 8000;
// The zip's limits: newest first, files older than kReportMaxAgeDays left out (the ini files and the
// newest session log excepted), anything that would take the zip past kReportMaxBytes left out.
inline constexpr int kReportLogs = 3, kReportCpuProfiles = 3, kReportDumps = 2, kReportMaxAgeDays = 7;
inline constexpr uint64_t kReportMaxBytes = 24ull << 20, kReportMaxDumpBytes = 16ull << 20;

// One entry of the zip: a file on disk (`path`), or `data` when `path` is empty.
struct ReportFile {
    std::wstring name;  // its name in the zip
    std::wstring path;
    std::string data;
    uint64_t size = 0;
};

struct ReportSources {
    std::wstring logs;         // the logs folder: ace-*.log, cpu-profile-*.txt, cheeky-snapshot.json, hang-*.dmp
    std::wstring runtimeIni;   // runtime.ini
    std::wstring settingsIni;  // the sim-qvfr.ini in use
};

// The files to zip from `sources`, as of `now` (FILETIME units): the newest kReportLogs session logs, the
// two ini files, the newest kReportCpuProfiles CPU captures and kReportDumps hang dumps (each under
// kReportMaxDumpBytes) of the last kReportMaxAgeDays, Cheeky's snapshot; kReportMaxBytes in all.
std::vector<ReportFile> report_files(const ReportSources& sources, uint64_t now);
// Writes `files` into a new zip at `zip` (stored entries, CRC-32). Empty on success, else why.
std::wstring write_zip(const std::wstring& zip, const std::vector<ReportFile>& files);
uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);

// The machine, for the issue (system_info() asks Windows; the rest only format, for the test).
struct SystemInfo {
    std::wstring cpu, gpu, gpuDriver, windows, openxrRuntime;
    double ramGb = 0;
};
SystemInfo system_info();
// "Windows 11 24H2 (build 26100.2314)": 22000 and up are Windows 11 (the registry's ProductName says 10).
std::wstring windows_name(int build, const std::wstring& displayVersion, int ubr);
// The driver's Windows version (DXGI's UMD version, "32.0.15.6094"), with NVIDIA's own ("560.94") for NVIDIA.
std::wstring gpu_driver_text(uint32_t vendorId, uint64_t umdVersion);
// The active OpenXR runtime's name: its manifest's runtime.name, else the manifest's file name.
std::wstring openxr_runtime_name(const std::string& manifestJson, const std::wstring& manifestPath);

// What the user says in the launcher before the issue opens: the kind of problem (kIssueKinds, 0: not
// sure) and what happened, in their words.
inline constexpr const wchar_t* kIssueKinds[] = {L"Not sure",      L"Crash or freeze", L"Performance",
                                                 L"Visual glitch", L"Eye tracking",    L"Foveated DLSS or frame generation",
                                                 L"Launcher"};
inline constexpr int kIssueKindCount = static_cast<int>(sizeof(kIssueKinds) / sizeof(kIssueKinds[0]));
// Each kind's issue form in .github/ISSUE_TEMPLATE (bug.yml for "Not sure"); its title prefix is the kind's
// name ("Problem" for bug.yml).
inline constexpr const char* kIssueTemplates[] = {"bug.yml",          "crash.yml", "performance.yml", "visual.yml",
                                                  "eye-tracking.yml", "dlss.yml",  "launcher.yml"};
static_assert(sizeof(kIssueTemplates) / sizeof(kIssueTemplates[0]) == kIssueKindCount);

// A known cause the last session shows, with what to try: the Diagnostics pane lists them before the report
// (many problems end there), and the issue carries them as "Launcher checks".
struct Cause {
    std::wstring title, fix;
};
std::vector<Cause> likely_causes(const Settings& settings, const RuntimeInfo& runtime);

struct IssueReport {
    std::wstring version;  // the launcher's (the layer ships with it)
    Settings settings;
    RuntimeInfo runtime;
    SystemInfo system;
    std::wstring zipName;  // empty: no zip was made
    int kind = 0;               // kIssueKinds
    std::wstring description;   // what happened, as typed in the launcher (may be empty)
    std::vector<Cause> causes;  // likely_causes
};
// "[Performance] <the description's first line, up to ~70 characters>", or "[Performance] Sim-QVFR
// <version>: " when there is no description ("[Problem]" for "Not sure", as bug.yml's title).
std::wstring issue_title(const IssueReport& report);
// The collected part: version, PC, headset, settings and the last session (Markdown).
std::wstring issue_details(const IssueReport& report);
// The whole report as Markdown, for the zip's issue.md: What happened (the description, else a prompt),
// Steps to reproduce, When does it happen, Screenshots and logs (the zip), then the launcher checks (the
// causes) and the details. The issue itself gets them as the form's fields (the link may leave some out).
std::wstring issue_body(const IssueReport& report);

// One prefilled field of the issue form, by its id (every form in .github/ISSUE_TEMPLATE has these).
struct IssueField {
    std::string id;
    std::wstring value;
};
// what (the description), files (the zip to drag in), checks (the causes) and details, in that order.
std::vector<IssueField> issue_fields(const IssueReport& report);
std::string utf8(const std::wstring& text);
// UTF-8, percent-encoded but for RFC 3986's unreserved characters.
std::string url_encode(const std::wstring& text);
// https://github.com/<repo>/issues/new?template=<form>&title=...&<id>=..., at most `max` characters: lines
// are dropped from the end of the last fields first (details, then checks) until it fits, with a note that
// the zip's issue.md has everything. GitHub reads the forms from the repository's default branch, and opens
// an empty issue, every field dropped, when it has no form of that name.
std::string issue_url(const std::wstring& repo, const char* form, const std::wstring& title,
                      const std::vector<IssueField>& fields, size_t max = kIssueUrlMax);

}  // namespace simqvfr::launcher

// "Report an issue" (the Diagnostics pane): the logs a bug report needs, zipped into one file, and a new
// GitHub issue prefilled with a "Describe your problem" section over what the launcher can tell on its own
// (settings, the last session, headset, hardware). The zip is stored (not compressed): a small writer here
// rather than a dependency, and GitHub takes attachments up to 25 MB, which kReportMaxBytes stays under.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "settings.h"

namespace aceqv::launcher {

inline constexpr wchar_t kIssueRepo[] = L"Williem3/Sim-QVFR";
// GitHub answers 414 above about 8 KB of URL; the body is trimmed to fit (the zip has all of it).
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
    std::wstring settingsIni;  // the ace-quadview.ini in use
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

struct IssueReport {
    std::wstring version;  // the launcher's (the layer ships with it)
    Settings settings;
    RuntimeInfo runtime;
    SystemInfo system;
    std::wstring zipName;  // empty: no zip was made
};
// The issue's Markdown body: "Describe your problem" (prompts, then asking for screenshots and the zip) over
// the collected information.
std::wstring issue_body(const IssueReport& report);
std::string utf8(const std::wstring& text);
// UTF-8, percent-encoded but for RFC 3986's unreserved characters.
std::string url_encode(const std::wstring& text);
// https://github.com/<repo>/issues/new?title=...&body=..., at most `max` characters: lines are dropped from
// the body's end (the collected part) until it fits, with a note that the zip has everything.
std::string issue_url(const std::wstring& repo, const std::wstring& title, const std::wstring& body,
                      size_t max = kIssueUrlMax);

}  // namespace aceqv::launcher

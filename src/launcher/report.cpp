#include "report.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <windows.h>
#include <dxgi.h>

#include "json.h"

#pragma comment(lib, "dxgi.lib")

namespace aceqv::launcher {

namespace fs = std::filesystem;

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// `format` with one number, at whatever length it comes to.
std::wstring formatted(const wchar_t* format, double v) {
    const int n = _scwprintf(format, v);
    if (n <= 0)
        return {};
    std::wstring text(static_cast<size_t>(n), L'\0');
    swprintf_s(text.data(), static_cast<size_t>(n) + 1, format, v);
    return text;
}

std::wstring number(int v) { return std::to_wstring(v); }

bool starts_with(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }
bool ends_with(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct Found {
    fs::path path;
    uint64_t time = 0, size = 0;  // last write (FILETIME units), bytes
};

// The files in `folder` named `prefix`*`suffix`, newest first. MSVC's file clock counts FILETIME's
// 100 ns ticks from 1601.
std::vector<Found> find(const std::wstring& folder, const wchar_t* prefix, const wchar_t* suffix,
                        bool (*accept)(const std::wstring& name) = nullptr) {
    std::vector<Found> out;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const std::wstring name = it->path().filename().wstring();
        if (!starts_with(name, prefix) || !ends_with(name, suffix) || (accept && !accept(name)))
            continue;
        Found f{it->path(), static_cast<uint64_t>(it->last_write_time(ec).time_since_epoch().count()), it->file_size(ec)};
        if (!ec)
            out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.time > b.time; });
    return out;
}

// The session logs, ace-<date>-<time>.log (not the older ace-quadview*.log ones).
bool session_log(const std::wstring& name) { return name.size() > 4 && iswdigit(name[4]); }

std::wstring windows_error(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out = text ? text : L"error " + std::to_wstring(code);
    LocalFree(text);
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    return out;
}

// A whole file, shared with whoever has it open (a running game still writes its log).
bool read_file(const std::wstring& path, std::string& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart < (1ll << 31);
    out.assign(ok ? static_cast<size_t>(size.QuadPart) : 0, '\0');
    for (size_t at = 0; ok && at < out.size();) {
        DWORD got = 0;
        ok = ReadFile(file, out.data() + at, static_cast<DWORD>(std::min<size_t>(out.size() - at, 1u << 20)), &got,
                      nullptr) != 0;
        if (ok && got == 0) {  // shorter than it was a moment ago
            out.resize(at);
            break;
        }
        at += got;
    }
    CloseHandle(file);
    return ok;
}

void put16(std::string& out, uint32_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}
void put32(std::string& out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

std::wstring registry_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[512] = {};
    DWORD bytes = sizeof(buffer);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS)
        return {};
    std::wstring s = buffer;
    s.erase(0, s.find_first_not_of(L' '));
    s.erase(s.find_last_not_of(L' ') + 1);
    return s;
}

DWORD registry_dword(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD v = 0, bytes = sizeof(v);
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &bytes) == ERROR_SUCCESS ? v : 0;
}

// The session summary's "Log:" and "CPU capture:" lines name files by their full path, which holds the
// user's name: the issue gets the file name only (the zip has the file).
std::wstring without_folders(const std::wstring& summary) {
    std::wstring out;
    for (size_t at = 0; at < summary.size();) {
        const size_t nl = summary.find(L'\n', at), end = nl == std::wstring::npos ? summary.size() : nl;
        std::wstring line = summary.substr(at, end - at);
        for (const wchar_t* key : {L"Log: ", L"CPU capture: "})
            if (starts_with(line, key)) {
                const size_t slash = line.find_last_of(L"\\/");
                if (slash != std::wstring::npos)
                    line = key + line.substr(slash + 1);
            }
        out += line + L"\n";
        at = end + 1;
    }
    return out;
}

}  // namespace

std::vector<ReportFile> report_files(const ReportSources& sources, uint64_t now) {
    constexpr uint64_t kDay = 24ull * 3600 * 10'000'000;
    const uint64_t since = now > kReportMaxAgeDays * kDay ? now - kReportMaxAgeDays * kDay : 0;
    std::vector<ReportFile> out;
    uint64_t total = 0;
    const auto add = [&](const fs::path& path, const std::wstring& name, uint64_t size) {
        if (total + size > kReportMaxBytes)
            return;
        total += size;
        out.push_back({name, path.wstring(), {}, size});
    };
    const auto add_file = [&](const std::wstring& path, const wchar_t* name) {
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        if (!path.empty() && !ec)
            add(path, name, size);
    };
    // In order of use, so the size cap leaves out what matters least.
    add_file(sources.settingsIni, L"ace-quadview.ini");
    add_file(sources.runtimeIni, L"runtime.ini");
    const std::vector<Found> logs = find(sources.logs, L"ace-", L".log", session_log);
    for (size_t i = 0; i < logs.size() && i < kReportLogs; ++i)
        if (i == 0 || logs[i].time >= since)
            add(logs[i].path, logs[i].path.filename().wstring(), logs[i].size);
    const std::vector<Found> profiles = find(sources.logs, L"cpu-profile-", L".txt");
    for (size_t i = 0; i < profiles.size() && i < kReportCpuProfiles; ++i)
        if (profiles[i].time >= since)
            add(profiles[i].path, profiles[i].path.filename().wstring(), profiles[i].size);
    for (const Found& f : find(sources.logs, L"cheeky-snapshot", L".json"))
        if (f.path.filename() == L"cheeky-snapshot.json")
            add(f.path, L"cheeky-snapshot.json", f.size);
    int dumps = 0;
    for (const Found& f : find(sources.logs, L"hang-", L".dmp"))
        if (dumps < kReportDumps && f.time >= since && f.size <= kReportMaxDumpBytes) {
            add(f.path, f.path.filename().wstring(), f.size);
            ++dumps;
        }
    return out;
}

uint32_t crc32(const void* data, size_t size, uint32_t crc) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::wstring write_zip(const std::wstring& zip, const std::vector<ReportFile>& files) {
    // Stored entries (method 0), names in UTF-8 (flag bit 11), all stamped with the time now (DOS time).
    SYSTEMTIME t;
    GetLocalTime(&t);
    const uint32_t dosTime = (t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2);
    const uint32_t dosDate = ((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay;
    std::string out, directory;
    uint32_t entries = 0;
    for (const ReportFile& f : files) {
        std::string data;
        if (f.path.empty())
            data = f.data;
        else if (!read_file(f.path, data))
            continue;  // gone or locked meanwhile: the rest still helps
        if (out.size() + data.size() > 0xffffffffull - (1u << 20))
            return L"too large for a zip";
        const std::string name = utf8(f.name);
        const uint32_t crc = crc32(data.data(), data.size()), offset = static_cast<uint32_t>(out.size());
        const uint32_t size = static_cast<uint32_t>(data.size());
        std::string common;  // version needed .. extra length, shared by both headers
        put16(common, 10);
        put16(common, 1u << 11);
        put16(common, 0);
        put16(common, dosTime);
        put16(common, dosDate);
        put32(common, crc);
        put32(common, size);
        put32(common, size);
        put16(common, static_cast<uint32_t>(name.size()));
        put16(common, 0);
        put32(out, 0x04034b50);
        out += common + name + data;
        put32(directory, 0x02014b50);
        put16(directory, 10);  // made by: MS-DOS, 1.0
        directory += common;
        put16(directory, 0);  // comment length
        put16(directory, 0);  // disk
        put16(directory, 0);  // internal attributes
        put32(directory, 0);  // external attributes
        put32(directory, offset);
        directory += name;
        ++entries;
    }
    const uint32_t directoryAt = static_cast<uint32_t>(out.size());
    out += directory;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, entries);
    put16(out, entries);
    put32(out, static_cast<uint32_t>(directory.size()));
    put32(out, directoryAt);
    put16(out, 0);
    const HANDLE file = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return windows_error(GetLastError());
    DWORD written = 0;
    const bool ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) && written == out.size();
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(zip.c_str());
        return windows_error(error);
    }
    return {};
}

std::wstring windows_name(int build, const std::wstring& displayVersion, int ubr) {
    std::wstring s = build >= 22000 ? L"Windows 11" : L"Windows 10";
    if (!displayVersion.empty())
        s += L" " + displayVersion;
    s += L" (build " + number(build);
    if (ubr > 0)
        s += L"." + number(ubr);
    return s + L")";
}

std::wstring gpu_driver_text(uint32_t vendorId, uint64_t umd) {
    const unsigned a = static_cast<unsigned>(umd >> 48), b = static_cast<unsigned>((umd >> 32) & 0xffff),
                   c = static_cast<unsigned>((umd >> 16) & 0xffff), d = static_cast<unsigned>(umd & 0xffff);
    std::wstring s = number(a) + L"." + number(b) + L"." + number(c) + L"." + number(d);
    if (vendorId == 0x10de) {  // NVIDIA's number: the last five digits of the last two parts, "560.94"
        const unsigned n = (c % 10) * 10000 + d;
        wchar_t nv[16];
        swprintf_s(nv, L"%u.%02u", n / 100, n % 100);
        s = nv + std::wstring(L" (") + s + L")";
    }
    return s;
}

std::wstring openxr_runtime_name(const std::string& manifestJson, const std::wstring& manifestPath) {
    json::Value root;
    if (json::parse(manifestJson, root))
        if (const json::Value* runtime = root.find("runtime")) {
            const std::string name = runtime->string_at("name");
            if (!name.empty())
                return widen(name);
        }
    return fs::path(manifestPath).filename().wstring();
}

SystemInfo system_info() {
    SystemInfo info;
    info.cpu = registry_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                               L"ProcessorNameString");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    info.cpu += (info.cpu.empty() ? L"" : L", ") + number(static_cast<int>(si.dwNumberOfProcessors)) + L" threads";
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb))
        info.ramGb = kb / (1024.0 * 1024.0);
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    info.windows = windows_name(_wtoi(registry_string(HKEY_LOCAL_MACHINE, nt, L"CurrentBuildNumber").c_str()),
                                registry_string(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion"),
                                static_cast<int>(registry_dword(HKEY_LOCAL_MACHINE, nt, L"UBR")));
    // The GPU: the hardware adapter with the most dedicated memory (a laptop's discrete one, not the iGPU).
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        SIZE_T best = 0;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (info.gpu.empty() || desc.DedicatedVideoMemory > best)) {
                best = desc.DedicatedVideoMemory;
                info.gpu = desc.Description + formatted(L", %.0f GB", desc.DedicatedVideoMemory / (1024.0 * 1024 * 1024));
                info.gpuDriver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))
                                     ? gpu_driver_text(desc.VendorId, static_cast<uint64_t>(umd.QuadPart))
                                     : L"";
            }
            adapter->Release();
        }
        factory->Release();
    }
    const std::wstring manifest = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    std::string text;
    if (!manifest.empty())
        info.openxrRuntime = read_file(manifest, text) ? openxr_runtime_name(text, manifest)
                                                       : fs::path(manifest).filename().wstring() + L" (missing)";
    return info;
}

std::string utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring issue_body(const IssueReport& r) {
    const Settings& s = r.settings;
    const RuntimeInfo& rt = r.runtime;
    const bool qvfr = s.mode == RenderMode::Qvfr;
    const std::wstring unknown = L"not known";
    std::wstring b;
    b += L"## Describe your problem\n\n";
    b += L"**What happened?**\n<!-- What did you see, and what did you expect instead? -->\n\n\n";
    b += L"**Steps to reproduce**\n<!-- From starting the launcher: settings changed, track and car, what you did. -->\n1. \n2. \n\n";
    b += L"**When does it happen?**\n<!-- Every launch, after some minutes, on one track or car, since an update... -->\n\n\n";
    b += L"**Screenshots and logs**\n";
    b += L"Please drag in screenshots (in the headset, and the launcher's Diagnostics page)";
    if (!r.zipName.empty())
        b += L" and **`" + r.zipName + L"`**, the logs the launcher just zipped (the Explorer window it opened shows it)";
    b += L".\n\n";
    b += L"---\n### Collected by the launcher\n\n";
    b += L"- **Version:** ace-quadview " + (r.version.empty() ? unknown : r.version) + L" (launcher and layer)\n";
    const SystemInfo& sys = r.system;
    b += L"- **CPU:** " + (sys.cpu.empty() ? unknown : sys.cpu) + L"\n";
    b += L"- **GPU:** " + (sys.gpu.empty() ? unknown : sys.gpu);
    if (!sys.gpuDriver.empty())
        b += L", driver " + sys.gpuDriver;
    b += L"\n";
    b += L"- **RAM:** " + (sys.ramGb > 0 ? formatted(L"%.0f GB", sys.ramGb) : unknown) + L"\n";
    b += L"- **Windows:** " + (sys.windows.empty() ? unknown : sys.windows) + L"\n";
    b += L"- **OpenXR runtime:** " + (sys.openxrRuntime.empty() ? unknown : sys.openxrRuntime) + L"\n";
    std::wstring headset = rt.known() ? number(rt.width) + L"×" + number(rt.height) + L" per eye recommended" : unknown;
    if (rt.maxWidth > 0)
        headset += L" (max " + number(rt.maxWidth) + L"×" + number(rt.maxHeight) + L")";
    if (rt.centerPpd > 0)
        headset += formatted(L", %.1f px/° at the centre", rt.centerPpd);
    b += L"- **Headset:** " + headset + L"\n";
    b += L"- **Eye tracker:** " +
         std::wstring(rt.eyeTracker == EyeTracker::Detected  ? L"detected"
                      : rt.eyeTracker == EyeTracker::Missing ? L"no samples"
                                                             : L"not known") +
         L"\n";
    b += L"- **Method:** " + std::wstring(qvfr ? L"Quad View Foveated Rendering (QVFR)" : L"Two-loop Foveated Interlaced") +
         L", " + (s.eyeTracking ? L"eye tracked" : L"fixed") + L"\n";
    std::wstring res = qvfr ? L"outer " + number(s.outer) + L"%, focus density " + number(s.inner) + L"%"
                            : L"eye scale " + number(s.outer) + L"%";
    res += L", focus area " + number(s.width) + L"×" + number(s.height) + L"%";
    if (rt.known()) {
        const Readouts ro = readouts(s, rt);
        res += L" (outer " + number(ro.outer.width) + L"×" + number(ro.outer.height);
        if (qvfr)
            res += L", focus " + number(ro.focus.width) + L"×" + number(ro.focus.height);
        res += L" px)";
    }
    b += L"- **Resolution:** " + res + L"\n";
    b += L"- **Focus area:** feather " + number(s.feather) + L"%, " +
         (s.shape.corners ? std::wstring(L"corners") : L"corner radius " + number(s.corner) + L"%") +
         formatted(L", gaze dead-zone %.1f°", s.gazeDeadzone / 10.0) + L", offset " +
         number(s.offset.for_mode(s.eyeTracking)) + L"°\n";
    if (qvfr)
        b += L"- **FOV crop:** " + (s.crop ? crop_key(s) + L" % (top, bottom, outer, inner)" : std::wstring(L"off")) + L"\n";
    std::wstring dlss = s.cheeky.load ? L"on" : L"off";
    if (!rt.cheekyStatus.empty())
        dlss += L" (last launch: " + rt.cheekyStatus + (rt.cheekyVersion.empty() ? L"" : L" " + rt.cheekyVersion) + L")";
    if (s.cheeky.load)
        dlss += s.cheeky.fg ? L", frame generation " + number(s.cheeky.fgMultiplier) + L"x" : L", frame generation off";
    b += L"- **Foveated DLSS:** " + dlss + L"\n";
    b += L"\n**Last session**\n```text\n" + without_folders(session_summary_text(rt)) + L"```\n";
    return b;
}

std::string url_encode(const std::wstring& text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : utf8(text)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string issue_url(const std::wstring& repo, const std::wstring& title, const std::wstring& body, size_t max) {
    const std::string head = "https://github.com/" + utf8(repo) + "/issues/new?title=" + url_encode(title) + "&body=";
    std::string url = head + url_encode(body);
    if (url.size() <= max)
        return url;
    // Drop whole lines from the end (the collected part; the zip's issue.md has it all), keeping a closed fence.
    const std::wstring note = L"\n_(Trimmed to fit the link: the zip's issue.md has all of it.)_\n";
    std::wstring kept = body;
    while (!kept.empty()) {
        const size_t nl = kept.find_last_of(L'\n', kept.size() >= 2 ? kept.size() - 2 : 0);
        kept.resize(nl == std::wstring::npos ? 0 : nl + 1);
        size_t fences = 0;  // an odd count: the cut is inside a ``` block
        for (size_t at = kept.find(L"```"); at != std::wstring::npos; at = kept.find(L"```", at + 3))
            ++fences;
        const bool open = fences % 2 == 1;
        const std::wstring candidate = kept + (open ? L"```\n" : L"") + note;
        url = head + url_encode(candidate);
        if (url.size() <= max)
            return url;
    }
    return head.substr(0, std::min(head.size(), max));
}

}  // namespace aceqv::launcher

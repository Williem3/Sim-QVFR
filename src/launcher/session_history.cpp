#include "session_history.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iterator>

#include <windows.h>

#include "../quad/gaze_deadzone.h"

namespace simqvfr::launcher {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kSection[] = L"session";

std::wstring read(const std::wstring& file, const wchar_t* key) {
    wchar_t buffer[512] = {};
    GetPrivateProfileStringW(kSection, key, L"", buffer, static_cast<DWORD>(std::size(buffer)), file.c_str());
    return buffer;
}

double read_number(const std::wstring& file, const wchar_t* key) {
    const std::wstring v = read(file, key);
    return v.empty() ? -1.0 : _wtof(v.c_str());
}

std::wstring number_text(double v) {
    if (v < 0)
        return L"";
    wchar_t text[32];
    swprintf_s(text, L"%.3f", v);
    return text;
}

bool same(double a, double b) { return std::fabs(a - b) < 0.0005; }

// The session records in `folder`, newest first (their names hold the session's start: session-<yyyymmdd>-<hhmmss>).
std::vector<fs::path> record_files(const std::wstring& folder) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (it->is_regular_file(ec) && name.rfind(L"session-", 0) == 0 && it->path().extension() == L".ini")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
    return files;
}

// "+6.0 fps", "−1.20 ms", "±0.0 fps".
std::wstring signed_number(double v, int digits, const wchar_t* unit) {
    wchar_t text[48];
    swprintf_s(text, L"%.*f", digits, std::fabs(v));
    const bool zero = std::wcstod(text, nullptr) == 0;
    return std::wstring(zero ? L"±" : v > 0 ? L"+" : L"−") + text + unit;
}

std::wstring percent(int v) { return std::to_wstring(v) + L"%"; }
std::wstring degrees(int v) { return std::to_wstring(v) + L"°"; }

}  // namespace

bool SessionRecord::same_numbers(const SessionInfo& s) const {
    return time == s.time && same(fps, s.fps) && same(frameP50, s.frameP50) && same(frameP90, s.frameP90) &&
           same(latePercent, s.latePercent) && same(cpuMs, s.cpuMs) && same(gpuMs, s.gpuMs);
}

std::wstring sessions_folder(const std::wstring& settingsIni) {
    return (fs::path(settingsIni).parent_path() / L"sessions").wstring();
}

std::wstring session_id(const std::wstring& log) {
    if (log.empty())
        return L"";
    const std::wstring stem = fs::path(log).stem().wstring();
    return stem.rfind(L"session-", 0) == 0 && stem.size() > 8 ? stem : L"";
}

std::wstring session_run_ini(const std::wstring& log) {
    const std::wstring id = session_id(log);
    if (id.empty())
        return L"";
    return (fs::path(log).parent_path() / (L"run-" + id.substr(8)) / L"sim-qvfr.ini").wstring();
}

bool baseline_mode(const std::wstring& mode) { return mode.rfind(L"Off", 0) == 0; }

SessionRecord session_record(const SessionInfo& s, const std::wstring& game, const Settings& settings) {
    SessionRecord r;
    r.id = session_id(s.log);
    r.game = game;
    r.time = s.time;
    r.mode = s.mode;
    r.baseline = baseline_mode(s.mode);
    r.fps = s.fps;
    r.frameP50 = s.frameP50;
    r.frameP90 = s.frameP90;
    r.latePercent = s.latePercent;
    r.cpuMs = s.cpuMs;
    r.gpuMs = s.gpuMs;
    r.limited = s.limited;
    r.settings = settings;
    return r;
}

bool store_session(const std::wstring& folder, const SessionRecord& r, const std::wstring& settingsFile) {
    if (r.id.empty() || r.id.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return false;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path file = fs::path(folder) / (r.id + L".ini");
    const std::wstring temp = file.wstring() + L".tmp";
    // The settings: a copy of the game's file (fast), else written from the record.
    DeleteFileW(temp.c_str());
    if (settingsFile.empty() || !CopyFileW(settingsFile.c_str(), temp.c_str(), FALSE))
        if (!save_settings(temp, r.settings)) {
            DeleteFileW(temp.c_str());
            return false;
        }
    SetFileAttributesW(temp.c_str(), FILE_ATTRIBUTE_NORMAL);  // a read-only ini copies as read-only
    const auto put = [&](const wchar_t* key, const std::wstring& value) {
        return WritePrivateProfileStringW(kSection, key, value.c_str(), temp.c_str()) != 0;
    };
    bool ok = put(L"game", r.game) && put(L"time", r.time) && put(L"mode", r.mode) && put(L"baseline", r.baseline ? L"1" : L"0") &&
              put(L"fps", number_text(r.fps)) && put(L"frame_p50_ms", number_text(r.frameP50)) &&
              put(L"frame_p90_ms", number_text(r.frameP90)) && put(L"late_percent", number_text(r.latePercent)) &&
              put(L"cpu_ms", number_text(r.cpuMs)) && put(L"gpu_ms", number_text(r.gpuMs)) && put(L"limited", r.limited);
    ok = ok && MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    const std::vector<fs::path> files = record_files(folder);
    for (size_t i = kSessionHistory; i < files.size(); ++i)
        fs::remove(files[i], ec);
    return true;
}

std::vector<SessionRecord> load_sessions(const std::wstring& folder) {
    std::vector<SessionRecord> out;
    for (const fs::path& path : record_files(folder)) {
        const std::wstring file = path.wstring();
        SessionRecord r;
        r.id = path.stem().wstring();
        r.game = read(file, L"game");
        r.time = read(file, L"time");
        r.mode = read(file, L"mode");
        r.baseline = read(file, L"baseline") == L"1";
        r.fps = read_number(file, L"fps");
        r.frameP50 = read_number(file, L"frame_p50_ms");
        r.frameP90 = read_number(file, L"frame_p90_ms");
        r.latePercent = read_number(file, L"late_percent");
        r.cpuMs = read_number(file, L"cpu_ms");
        r.gpuMs = read_number(file, L"gpu_ms");
        r.limited = read(file, L"limited");
        r.settings = load_settings(file);
        if (r.fps > 0)
            out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::wstring> setting_changes(const Settings& a, const Settings& b) {
    std::vector<std::wstring> out;
    const auto values = [&](const std::wstring& label, const std::wstring& x, const std::wstring& y) {
        if (x != y)
            out.push_back(label + L" " + x + L" → " + y);
    };
    const auto on_off = [&](const std::wstring& label, bool x, bool y) {
        if (x != y)
            out.push_back(label + (y ? L" on" : L" off"));
    };
    const bool qvfr = b.mode == RenderMode::Qvfr;
    if (a.mode != b.mode)
        values(L"Method", a.mode == RenderMode::Qvfr ? L"QVFR" : L"Two-loop", qvfr ? L"QVFR" : L"Two-loop");
    if (a.eyeTracking != b.eyeTracking)
        values(L"Foveated mode", a.eyeTracking ? L"Eye tracked" : L"Fixed", b.eyeTracking ? L"Eye tracked" : L"Fixed");
    values(L"Periphery", percent(a.outer), percent(b.outer));
    if (qvfr && a.mode == RenderMode::Qvfr)  // Two-loop's follows from the outer resolution and the foveated size
        values(L"Focus density", percent(a.inner), percent(b.inner));
    values(L"Object detail", percent(a.objectDetail), percent(b.objectDetail));
    values(L"Sharpening", percent(a.sharpen), percent(b.sharpen));
    on_off(L"Runtime composition", a.nativeQuad != 0, b.nativeQuad != 0);
    on_off(L"Crop", a.crop, b.crop);
    if (a.crop && b.crop) {
        values(L"Crop top", percent(a.cropTop), percent(b.cropTop));
        values(L"Crop bottom", percent(a.cropBottom), percent(b.cropBottom));
        values(L"Crop outer side", percent(a.cropOuter), percent(b.cropOuter));
        values(L"Crop inner side", percent(a.cropInner), percent(b.cropInner));
    }
    on_off(L"Helmet", a.helmet.on, b.helmet.on);
    if (a.helmet.on && b.helmet.on) {
        if (a.helmet.detail != b.helmet.detail)
            values(L"Helmet detail", a.helmet.detail ? L"Detail" : L"Performance", b.helmet.detail ? L"Detail" : L"Performance");
        HelmetSettings x = a.helmet, y = b.helmet;
        x.detail = y.detail;
        if (!(x == y))
            out.push_back(L"Helmet look");
    }
    values(L"Focus width", percent(a.width), percent(b.width));
    values(L"Focus height", percent(a.height), percent(b.height));
    if (!a.shape.corners && !b.shape.corners)
        values(L"Corner radius", percent(a.corner), percent(b.corner));
    values(L"Feather", percent(a.feather), percent(b.feather));
    if (a.eyeTracking && b.eyeTracking) {
        values(L"Gaze offset", degrees(a.offset.gaze), degrees(b.offset.gaze));
        values(L"Gaze dead-zone", gaze_deadzone_text(a.gazeDeadzone) + L"°", gaze_deadzone_text(b.gazeDeadzone) + L"°");
    } else if (!a.eyeTracking && !b.eyeTracking) {
        values(L"Vertical position", degrees(a.offset.fixed), degrees(b.offset.fixed));
    }
    if (a.shape.corners != b.shape.corners)
        values(L"Focus shape", a.shape.corners ? L"Corners" : L"Box", b.shape.corners ? L"Corners" : L"Box");
    else if (a.shape.corners && !(a.shape == b.shape))
        out.push_back(L"Focus shape corners");
    on_off(L"Focus outline", a.outline, b.outline);
    const CheekySettings &c = a.cheeky, &d = b.cheeky;
    on_off(L"Cheeky Foveated DLSS", c.load, d.load);
    if (c.load && d.load) {
        on_off(L"Foveated DLSS", c.processing, d.processing);
        on_off(L"Cheaper outer views", c.outerDlaa, d.outerDlaa);
        if (c.outerDlaa && d.outerDlaa)
            values(L"Outer DLAA scale", percent(c.outerScale), percent(d.outerScale));
        values(L"Focus supersampling", percent(c.supersampling), percent(d.supersampling));
        on_off(L"DLSS-NR", c.nr, d.nr);
        on_off(L"Frame generation", c.fg, d.fg);
        if (c.fg && d.fg)
            values(L"Frame generation", std::to_wstring(c.fgMultiplier) + L"x", std::to_wstring(d.fgMultiplier) + L"x");
        // The rest (presets, the FG regions' scales, the release point, the motion fix): one line for them all.
        CheekySettings x = c;
        x.processing = d.processing;
        x.outerDlaa = d.outerDlaa;
        x.outerScale = d.outerScale;
        x.supersampling = d.supersampling;
        x.nr = d.nr;
        x.fg = d.fg;
        x.fgMultiplier = d.fgMultiplier;
        x.fgBorder = d.fgBorder;  // an outline for checking, not a cost
        if (!(x == d))
            out.push_back(L"Foveated DLSS options");
    }
    return out;
}

std::wstring changes_text(const std::vector<std::wstring>& changes, size_t max) {
    std::wstring text;
    const size_t shown = std::min(changes.size(), std::max<size_t>(max, 1));
    for (size_t i = 0; i < shown; ++i) {
        const bool last = i + 1 == shown && shown == changes.size();
        text += (i == 0 ? L"" : last ? L" and " : L", ") + changes[i];
    }
    if (shown < changes.size())
        text += L" and " + std::to_wstring(changes.size() - shown) + L" more";
    return text;
}

int previous_session(const std::vector<SessionRecord>& records, size_t index) {
    for (size_t i = index + 1; i < records.size(); ++i)
        if (records[i].game == records[index].game && records[i].baseline == records[index].baseline)
            return static_cast<int>(i);
    return -1;
}

int other_kind_session(const std::vector<SessionRecord>& records, size_t index) {
    for (size_t i = index + 1; i < records.size(); ++i)
        if (records[i].game == records[index].game && records[i].baseline != records[index].baseline)
            return static_cast<int>(i);
    return -1;
}

std::wstring difference_text(const SessionRecord& now, const SessionRecord& before) {
    std::wstring text;
    if (now.fps > 0 && before.fps > 0)
        text = signed_number(now.fps - before.fps, 1, L" fps");
    if (now.frameP90 > 0 && before.frameP90 > 0)
        text += (text.empty() ? L"" : L", ") + std::wstring(L"90th percentile ") + signed_number(now.frameP90 - before.frameP90, 2, L" ms");
    return text;
}

std::wstring comparison_text(const std::wstring& label, const SessionRecord& now, const SessionRecord& before) {
    std::wstring text = label + L": " + difference_text(now, before);
    // Settings matter between two sessions with QVFR; without it the game ran its own two views.
    if (!now.baseline && !before.baseline && now.baseline == before.baseline) {
        const std::vector<std::wstring> changes = setting_changes(before.settings, now.settings);
        text += changes.empty() ? L", same settings" : L", after " + changes_text(changes);
    }
    return text;
}

}  // namespace simqvfr::launcher

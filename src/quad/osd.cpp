#include "osd.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>

#include <windows.h>

#include "../ini_put.h"

namespace simqvfr {

OsdPlacement read_osd_placement(const wchar_t* ini) {
    // As strings: GetPrivateProfileInt reads negative values as 0.
    const auto read = [ini](const wchar_t* key, int fallback) {
        wchar_t text[16] = {};
        GetPrivateProfileStringW(L"osd", key, L"", text, 16, ini);
        return text[0] ? _wtoi(text) : fallback;
    };
    OsdPlacement p;
    p.enabled = read(L"enabled", 0) != 0;
    p.yaw = std::clamp(read(L"yaw", p.yaw), -kOsdYawMax, kOsdYawMax);
    p.pitch = std::clamp(read(L"pitch", p.pitch), -kOsdPitchMax, kOsdPitchMax);
    p.width = std::clamp(read(L"width", p.width), kOsdWidthMin, kOsdWidthMax);
    return p;
}

bool write_osd_placement(const wchar_t* ini, const OsdPlacement& p) {
    const auto put = [ini](const wchar_t* key, int value) {
        return ini_put(ini, L"osd", key, value);
    };
    return put(L"enabled", p.enabled ? 1 : 0) && put(L"yaw", p.yaw) && put(L"pitch", p.pitch) && put(L"width", p.width);
}

namespace {

// The value after the first "key": at or after `from` (npos: none). Cheeky's snapshot is flat
// enough, and its keys ordered, for a scan: each section's key comes before the fields read from it.
size_t value_at(const std::string& json, const char* key, size_t from) {
    if (from == std::string::npos)
        return from;
    const std::string quoted = std::string("\"") + key + "\":";
    const size_t at = json.find(quoted, from);
    if (at == std::string::npos)
        return at;
    size_t i = at + quoted.size();
    while (i < json.size() && json[i] == ' ')
        ++i;
    return i;
}

double number_at(const std::string& json, size_t at, double fallback) {
    if (at == std::string::npos || at >= json.size())
        return fallback;
    char* end = nullptr;
    const double v = std::strtod(json.c_str() + at, &end);
    return end != json.c_str() + at ? v : fallback;
}

double headroom(double ms, double budget) { return budget > 0 && ms >= 0 ? (budget - ms) / budget * 100 : 0; }

OsdTone headroom_tone(double percent) {
    return percent >= 15 ? OsdTone::Good : percent >= 0 ? OsdTone::Warn : OsdTone::Bad;
}

std::wstring format(const wchar_t* pattern, double a, double b = 0) {
    wchar_t s[96];
    swprintf_s(s, pattern, a, b);
    return s;
}

}  // namespace

OsdCheeky osd_cheeky(const std::string& json) {
    OsdCheeky c;
    const size_t fg = value_at(json, "fg", 0);
    if (fg == std::string::npos || fg >= json.size() || json[fg] != '{')
        return c;
    c.valid = true;
    const size_t active = value_at(json, "active", fg);
    c.active = active != std::string::npos && json.compare(active, 4, "true") == 0;
    if (const size_t state = value_at(json, "state", fg); state != std::string::npos && json[state] == '"') {
        const size_t end = json.find('"', state + 1);
        if (end != std::string::npos)
            for (size_t i = state + 1; i < end; ++i)  // Cheeky's states are ASCII
                c.state += static_cast<wchar_t>(static_cast<unsigned char>(json[i]));
    }
    c.outputFps = number_at(json, value_at(json, "unique_fps", value_at(json, "cadence", fg)), 0);
    c.refreshHz = number_at(json, value_at(json, "refresh_hz", value_at(json, "cadence", fg)), 0);
    c.fgMs = number_at(json, value_at(json, "total_ms", value_at(json, "performance", fg)), -1);
    const size_t load = value_at(json, "system_load", 0);
    const size_t gpuValid = value_at(json, "gpu_valid", load);
    if (gpuValid != std::string::npos && json.compare(gpuValid, 4, "true") == 0)
        c.gpuBusyPercent = number_at(json, value_at(json, "gpu_busy_percent", load), -1);
    return c;
}

OsdSummary osd_summary(const OsdInput& in) {
    OsdSummary s;
    double total = 0, cpuTotal = 0, workTotal = 0, busyTotal = 0, waitTotal = 0;
    size_t cpuFrames = 0, workFrames = 0, waitFrames = 0;
    for (const OsdFrame& f : in.frames) {
        total += f.intervalMs;
        if (f.cpuMs >= 0) {
            cpuTotal += f.cpuMs;
            ++cpuFrames;
        }
        if (f.workMs >= 0 && f.cpuMs >= 0) {
            workTotal += f.workMs;
            busyTotal += f.cpuMs;
            ++workFrames;
        }
        if (f.waitMs >= 0) {
            waitTotal += f.waitMs;
            ++waitFrames;
        }
    }
    const size_t n = in.frames.size();
    if (n && total > 0) {
        s.frameMs = total / static_cast<double>(n);
        s.fps = 1000.0 / s.frameMs;
        double squares = 0;
        for (const OsdFrame& f : in.frames)
            squares += (f.intervalMs - s.frameMs) * (f.intervalMs - s.frameMs);
        s.jitterMs = std::sqrt(squares / static_cast<double>(n));
        std::vector<double> sorted;
        for (const OsdFrame& f : in.frames)
            sorted.push_back(f.intervalMs);
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&](double p) { return sorted[std::min(n - 1, static_cast<size_t>(p * static_cast<double>(n)))]; };
        s.frameP90Ms = at(0.9);
        s.low1Fps = at(0.99) > 0 ? 1000.0 / at(0.99) : 0;
    }
    // With frame generation running, ACE only has to deliver every multiplier-th displayed frame.
    // The budget: the headset's refresh period times the frame generation multiplier. Cheeky's layer below
    // gives ACE the multiplied period already, so with the refresh known the period is not multiplied again;
    // without it, the period is taken as the refresh's.
    const int multiplier = in.fg == OsdFg::On ? std::max(1, in.fgMultiplier) : 1;
    const double refreshMs = in.refreshMs > 0 ? in.refreshMs : in.periodMs;
    s.budgetMs = in.refreshMs > 0 ? std::max(in.periodMs, in.refreshMs * multiplier)
                 : in.periodMs > 0 ? in.periodMs * multiplier
                                   : 0;
    if (s.budgetMs > 0 && n) {
        size_t late = 0;
        for (const OsdFrame& f : in.frames)
            late += f.intervalMs > 1.05 * s.budgetMs;
        s.latePercent = 100.0 * static_cast<double>(late) / static_cast<double>(n);
    }
    s.refreshHz = refreshMs > 0 ? 1000.0 / refreshMs : 0;
    s.shownFps = in.fg == OsdFg::On && in.fgOutputFps > 0 ? in.fgOutputFps : s.fps;
    // CPU: the render thread's work per frame when known (else its CPU time), and the busiest thread's.
    if (workFrames) {
        s.cpuMs = workTotal / static_cast<double>(workFrames);
        s.cpuBusyMs = busyTotal / static_cast<double>(workFrames);
        s.cpuBlockedMs = std::max(0.0, s.cpuMs - s.cpuBusyMs);
    } else {
        s.cpuMs = cpuFrames ? cpuTotal / static_cast<double>(cpuFrames) : -1;
    }
    s.waitMs = waitFrames ? waitTotal / static_cast<double>(waitFrames) : -1;
    s.cpuThreadMs = in.cpuBusiestPercent >= 0 && s.frameMs > 0 ? in.cpuBusiestPercent / 100 * s.frameMs : -1;
    s.cpuHeadroom = headroom(std::max(s.cpuMs, s.cpuThreadMs), s.budgetMs);
    // GPU: the whole GPU's time per frame when its busy share is known; ACE's queue time otherwise.
    s.gpuGameMs = in.gpuMs;
    s.gpuLayerMs = in.composeMs;
    s.gpuFgMs = in.fg == OsdFg::On && in.fgMs >= 0 ? in.fgMs * std::max(0, in.fgMultiplier - 1) : -1;
    if (in.gpuBusyPercent >= 0 && s.frameMs > 0) {
        s.gpuWhole = true;
        s.gpuMs = in.gpuBusyPercent / 100 * s.frameMs;
        s.gpuRestMs = std::max(0.0, s.gpuMs - std::max(0.0, s.gpuGameMs) - std::max(0.0, s.gpuFgMs) -
                                        std::max(0.0, s.gpuLayerMs));
    } else {
        s.gpuMs = in.gpuMs;
    }
    s.gpuHeadroom = headroom(s.gpuMs, s.budgetMs);
    if (s.budgetMs <= 0 || (s.cpuMs < 0 && s.gpuMs < 0 && in.gpuBusyPercent < 0))
        return s;
    // Direct signals, when the layer measures them: the whole GPU busy (95% or more: nothing left), the busiest
    // thread at a full core, or frames over budget with ACE barely held in xrWaitFrame (it was late itself).
    // Over budget with neither: the runtime or frame generation paces ACE.
    const bool over = s.frameMs > 1.05 * s.budgetMs;
    if (in.gpuBusyPercent >= 0 && (s.waitMs >= 0 || in.cpuBusiestPercent >= 0)) {
        const bool gpuFull = in.gpuBusyPercent >= 95;
        const bool cpuFull = in.cpuBusiestPercent >= 95 || (over && s.waitMs >= 0 && s.waitMs < 1.0 && !gpuFull);
        // Held in xrWaitFrame and still past the budget, even by a little: something below ACE (the runtime,
        // frame generation) sets its pace, not its own work.
        const bool held = s.waitMs >= 1.0 && s.frameMs > 1.01 * s.budgetMs;
        s.bound = gpuFull ? OsdBound::Gpu : cpuFull ? OsdBound::Cpu : over || held ? OsdBound::Pacing : OsdBound::None;
        return s;
    }
    // The GPU time is an upper bound: it also counts the GPU waiting for ACE to submit (gpu_frame_timer.h).
    // So a GPU time close to the CPU time means the CPU held the GPU up. The GPU's busy share, when
    // known, says it directly.
    const double limit = 0.85 * s.budgetMs;
    const bool cpuHeld = s.cpuMs >= 0 && s.gpuMs >= 0 && s.cpuMs >= 0.9 * s.gpuMs;
    const bool gpu = in.gpuBusyPercent >= 0 ? in.gpuBusyPercent >= 90 : s.gpuMs >= limit && !cpuHeld;
    // A missed budget the GPU does not explain: ACE's CPU side (perhaps a thread other than the
    // render thread, whose time the CPU row shows).
    const bool missed = s.frameMs > 1.05 * s.budgetMs && s.gpuMs >= 0 && in.gpuBusyPercent < 90;
    const bool cpu = s.cpuMs >= limit || (s.gpuMs >= limit && cpuHeld) || missed;
    s.bound = gpu ? OsdBound::Gpu : cpu ? OsdBound::Cpu : OsdBound::None;
    return s;
}

std::vector<OsdTile> osd_tiles(const OsdInput& in, const OsdSummary& s) {
    std::vector<OsdTile> tiles;
    {
        OsdTile t{L"Frame rate", s.shownFps > 0 ? format(L"%.0f", s.shownFps) : L"—", L"fps"};
        if (in.fg == OsdFg::On && s.fps > 0)
            t.detail = format(L"game %.0f  ·  %.0f Hz", s.fps, s.refreshHz);
        else if (s.refreshHz > 0)
            t.detail = format(L"of %.0f Hz", s.refreshHz);
        tiles.push_back(t);
    }
    {
        OsdTile t{L"Frame time", s.frameMs > 0 ? format(L"%.1f", s.frameMs) : L"—", L"ms"};
        if (s.frameMs > 0 && s.budgetMs > 0) {
            t.detail = format(L"target %.1f ms", s.budgetMs);
            t.tone = s.frameMs <= 1.05 * s.budgetMs ? OsdTone::Good : s.frameMs <= 1.25 * s.budgetMs ? OsdTone::Warn : OsdTone::Bad;
        }
        tiles.push_back(t);
    }
    const auto processor = [&](const wchar_t* label, double ms, double headroomPercent) {
        OsdTile t{label, ms >= 0 ? format(L"%.1f", ms) : L"—", ms >= 0 ? L"ms" : L""};
        if (ms >= 0 && s.budgetMs > 0) {
            t.detail = headroomPercent >= 0 ? format(L"%.0f%% headroom", headroomPercent) : format(L"%.0f%% over", -headroomPercent);
            t.tone = headroom_tone(headroomPercent);
        }
        return t;
    };
    // The CPU's time per frame: the render thread's work or the busiest thread's, whichever is longer (the
    // headroom's measure; the cells below show both).
    tiles.push_back(processor(L"CPU", std::max(s.cpuMs, s.cpuThreadMs), s.cpuHeadroom));
    OsdTile gpu = processor(s.gpuWhole ? L"GPU (all work)" : L"GPU", s.gpuMs, s.gpuHeadroom);
    if (s.gpuMs < 0 && in.gpuBusyPercent >= 0)  // no timestamps (not QVFR): the GPU's busy share
        gpu.detail = format(L"%.0f%% busy", in.gpuBusyPercent);
    tiles.push_back(gpu);
    return tiles;
}

std::vector<OsdRow> osd_rows(const OsdInput& in, const OsdSummary& s) {
    std::vector<OsdRow> rows;
    {
        OsdRow r{L"Pacing"};
        if (s.frameMs <= 0 || s.budgetMs <= 0) {
            r.value = L"—";
        } else {
            r.value = (s.latePercent < 1 ? std::wstring(L"Smooth") : format(L"%.0f%% late", s.latePercent)) +
                      format(L"  ±%.1f ms", s.jitterMs);
            r.tone = s.latePercent < 1 ? OsdTone::Good : s.latePercent < 5 ? OsdTone::Warn : OsdTone::Bad;
        }
        rows.push_back(r);
    }
    {
        OsdRow r{L"Limited by"};
        r.pill = true;
        switch (s.bound) {
        case OsdBound::Gpu: r.value = L"GPU"; r.tone = OsdTone::Warn; break;
        case OsdBound::Cpu: r.value = L"CPU"; r.tone = OsdTone::Warn; break;
        case OsdBound::Pacing: r.value = L"Pacing"; r.tone = OsdTone::Warn; break;
        case OsdBound::None: r.value = L"Neither"; r.tone = OsdTone::Good; break;
        case OsdBound::Unknown: r.value = L"—"; r.pill = false; break;
        }
        rows.push_back(r);
    }
    {
        OsdRow r{L"Frame p90 · 1% low"};
        r.value = s.frameP90Ms > 0 ? format(L"%.1f ms · %.0f fps", s.frameP90Ms, s.low1Fps) : L"—";
        if (s.frameP90Ms > 0 && s.budgetMs > 0)
            r.tone = s.frameP90Ms <= 1.05 * s.budgetMs ? OsdTone::Good : s.frameP90Ms <= 1.25 * s.budgetMs ? OsdTone::Warn : OsdTone::Bad;
        rows.push_back(r);
    }
    {
        // The GPU's time per ACE frame, by who used it (ms): ACE's queue, frame generation, the layer, the rest.
        const auto part = [](double ms) { return ms >= 0 ? format(L"%.1f", ms) : std::wstring(L"—"); };
        OsdRow r{L"GPU ms: game/FG/layer/rest"};
        r.value = s.gpuWhole ? part(s.gpuGameMs) + L" / " + part(s.gpuFgMs) + L" / " + part(s.gpuLayerMs) + L" / " +
                                   part(s.gpuRestMs)
                  : s.gpuGameMs >= 0 ? part(s.gpuGameMs) + L" / " + part(s.gpuFgMs) + L" / " + part(s.gpuLayerMs) + L" / —"
                                    : L"—";
        rows.push_back(r);
    }
    {
        OsdRow r{L"Busiest game thread"};
        if (in.cpuBusiestPercent < 0) {
            r.value = L"—";
        } else {
            std::wstring who = in.cpuBusiestIsRender ? L"render" : in.cpuBusiestName.substr(0, 12);
            r.value = format(L"%.0f%%", in.cpuBusiestPercent) + (who.empty() ? L"" : L" · " + who);
            r.tone = in.cpuBusiestPercent >= 95 ? OsdTone::Bad : in.cpuBusiestPercent >= 85 ? OsdTone::Warn : OsdTone::Good;
        }
        rows.push_back(r);
    }
    {
        OsdRow r{L"Render thread"};
        r.value = s.cpuBusyMs >= 0 ? format(L"%.1f busy · %.1f blocked", s.cpuBusyMs, s.cpuBlockedMs) : L"—";
        rows.push_back(r);
    }
    {
        OsdRow r{L"VRAM"};
        if (in.vramBudget == 0) {
            r.value = L"—";
        } else {
            constexpr double kGb = 1024.0 * 1024 * 1024;
            const double used = static_cast<double>(in.vramUsed) / static_cast<double>(in.vramBudget);
            r.value = format(L"%.1f / %.1f GB", static_cast<double>(in.vramUsed) / kGb, static_cast<double>(in.vramBudget) / kGb);
            r.meter = std::clamp(used, 0.0, 1.0);
            r.tone = used < 0.85 ? OsdTone::Good : used < 0.95 ? OsdTone::Warn : OsdTone::Bad;
        }
        rows.push_back(r);
    }
    {
        OsdRow r{L"Frame generation"};
        r.pill = true;
        if (in.fg == OsdFg::On) {
            r.value = format(L"On · %.0fx", static_cast<double>(std::max(1, in.fgMultiplier)));
            if (in.fgOutputFps > 0)
                r.value += format(L" · %.0f fps", in.fgOutputFps);
            r.tone = OsdTone::Good;
        } else if (in.fg == OsdFg::Starting) {
            r.value = in.fgState.empty() ? L"Starting" : in.fgState;
            r.tone = OsdTone::Warn;
        } else {
            r.value = L"Off";
        }
        rows.push_back(r);
    }
    {
        OsdRow r{L"Eye tracking"};
        r.pill = true;
        switch (in.gaze) {
        case OsdGaze::Off: r.value = L"Off"; break;
        case OsdGaze::Unavailable: r.value = L"Not available"; r.tone = OsdTone::Bad; break;
        case OsdGaze::Tracking: r.value = L"Tracking"; r.tone = OsdTone::Good; break;
        case OsdGaze::Partial: r.value = L"Partial"; r.tone = OsdTone::Warn; break;
        case OsdGaze::Lost: r.value = L"Lost"; r.tone = OsdTone::Bad; break;
        }
        rows.push_back(r);
    }
    return rows;
}

}  // namespace simqvfr

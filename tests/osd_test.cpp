// Offline check of the performance overlay (quad/osd.h): frame rate, pacing and headroom from frame
// times, which processor limits the frame, the rows' states, the [osd] round trip, and that the card
// draws with rounded, see-through corners.
#include <cstdio>
#include <filesystem>
#include <string>

#include <windows.h>

#include "quad/osd.h"

using namespace aceqv;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool close_to(double a, double b, double tolerance = 0.05) { return a > b - tolerance && a < b + tolerance; }

OsdInput steady(double intervalMs, double cpuMs, double gpuMs, double periodMs = 11.111) {
    OsdInput in;
    in.periodMs = periodMs;
    in.gpuMs = gpuMs;
    for (int i = 0; i < 45; ++i)
        in.frames.push_back({intervalMs, cpuMs});
    return in;
}

const OsdRow* row(const std::vector<OsdRow>& rows, const wchar_t* label) {
    for (const OsdRow& r : rows)
        if (r.label == label)
            return &r;
    return nullptr;
}

}  // namespace

int main() {
    {
        const OsdSummary s = osd_summary(steady(11.111, 5, 7));
        check(close_to(s.fps, 90, 0.1) && close_to(s.budgetMs, 11.111) && s.latePercent == 0 && close_to(s.jitterMs, 0),
              "a steady 90 Hz frame: 90 fps, on budget, smooth");
        check(close_to(s.cpuHeadroom, 55, 0.1) && close_to(s.gpuHeadroom, 37, 0.1) && s.bound == OsdBound::None,
              "headroom is the budget left; neither processor limits");
    }
    {
        const OsdSummary s = osd_summary(steady(11.111, 4, 10.5));
        check(s.bound == OsdBound::Gpu, "GPU time near the budget with the CPU well under: GPU bound");
    }
    {
        const OsdSummary s = osd_summary(steady(13, 10.5, 11));
        check(s.bound == OsdBound::Cpu, "GPU time close to the CPU time (it waited for ACE): CPU bound");
        check(s.cpuHeadroom > 0 && s.cpuHeadroom < 10, "CPU headroom is small");
    }
    {
        OsdInput in = steady(11.111, 4, 10.5);
        in.gpuBusyPercent = 60;
        check(osd_summary(in).bound == OsdBound::None, "the GPU's busy share, when known, overrules the timestamps");
    }
    {
        OsdInput in = steady(22.222, 6, 15);
        in.fg = OsdFg::On;
        in.fgMultiplier = 2;
        const OsdSummary s = osd_summary(in);
        check(close_to(s.budgetMs, 22.222) && s.latePercent == 0 && s.bound == OsdBound::None,
              "frame generation 2x: ACE's budget is two refresh periods");
    }
    {
        OsdInput in = steady(11.111, 5, 7);
        for (int i = 0; i < 5; ++i)
            in.frames[i * 9].intervalMs = 22.222;
        const OsdSummary s = osd_summary(in);
        const std::vector<OsdRow> rows = osd_rows(in, s);
        const OsdRow* pacing = row(rows, L"Pacing");
        check(close_to(s.latePercent, 100.0 * 5 / 45, 0.1) && pacing && pacing->tone == OsdTone::Bad &&
                  pacing->value.find(L"late") != std::wstring::npos,
              "skipped frames show as late in the pacing row");
    }
    {
        const OsdSummary s = osd_summary(steady(14, 3, 6));
        check(s.bound == OsdBound::Cpu, "frames over budget with an idle GPU: limited by the CPU");
    }
    {
        const std::string json =
            R"({"version":"0.6.3","system_load": {"valid": true, "gpu_valid": true, "cpu_total_percent": 24.1, )"
            R"("gpu_busy_percent": 78.0024}, "fg": {"layer_present": true, "active": true, "state": "Generating frames", )"
            R"("automatic": {"cpu_ms": 0, "gpu_ms": 0}, "performance": {"center_ms": 1.07, "total_ms": 1.72236}, )"
            R"("cadence": {"valid": true, "game_fps": 86.1, "unique_fps": 90.1143, "refresh_hz": 90.0}}})";
        const OsdCheeky c = osd_cheeky(json);
        check(c.valid && c.active && c.state == L"Generating frames" && close_to(c.outputFps, 90.1143, 0.001) &&
                  close_to(c.fgMs, 1.72236, 0.0001) && close_to(c.gpuBusyPercent, 78.0024, 0.0001),
              "Cheeky's snapshot: FG active, its state, output fps, FG time and GPU busy");
        check(!osd_cheeky("{}").valid && !osd_cheeky("").valid, "no snapshot: nothing from Cheeky");
    }
    {
        const OsdSummary s = osd_summary(steady(11.111, -1, -1));
        check(s.cpuMs < 0 && s.bound == OsdBound::Unknown, "no CPU or GPU time: the bound is unknown");
    }
    {
        OsdInput in = steady(11.111, 5, 7);
        in.vramUsed = 11ull << 30;
        in.vramBudget = 12ull << 30;
        in.gaze = OsdGaze::Tracking;
        in.fg = OsdFg::Starting;
        in.fgState = L"Waiting for guides";
        in.composeMs = 0.4;
        const std::vector<OsdRow> rows = osd_rows(in, osd_summary(in));
        const OsdRow* vram = row(rows, L"VRAM");
        const OsdRow* fg = row(rows, L"Frame generation");
        const OsdRow* gaze = row(rows, L"Eye tracking");
        const OsdRow* overhead = row(rows, L"GPU overhead");
        check(rows.size() == 6, "six cells");
        const std::vector<OsdTile> tiles = osd_tiles(in, osd_summary(in));
        check(tiles.size() == 4 && tiles[0].value == L"90" && tiles[0].unit == L"fps" && tiles[0].detail == L"of 90 Hz" &&
                  tiles[1].value == L"11.1" && tiles[1].tone == OsdTone::Good && tiles[2].label == L"CPU" &&
                  tiles[2].detail == L"55% headroom" && tiles[3].value == L"7.0" && tiles[3].detail == L"37% headroom",
              "four tiles: frame rate, frame time, CPU and GPU with their headroom");
        check(vram && vram->value == L"11.0 / 12.0 GB" && vram->tone == OsdTone::Warn && close_to(vram->meter, 11.0 / 12),
              "VRAM: used of the budget, with a meter, amber above 85%");
        check(fg && fg->pill && fg->value == L"Waiting for guides" && fg->tone == OsdTone::Warn,
              "frame generation not yet generating shows Cheeky's state");
        check(gaze && gaze->pill && gaze->value == L"Tracking" && gaze->tone == OsdTone::Good, "eye tracking badge");
        check(overhead && overhead->value == L"QVFR 0.40 ms", "the layer's own GPU time");
        in.fg = OsdFg::On;
        in.fgMultiplier = 2;
        in.fgOutputFps = 180;
        const std::vector<OsdTile> fgTiles = osd_tiles(in, osd_summary(in));
        check(fgTiles[0].value == L"180" && fgTiles[0].detail.find(L"ACE 90") == 0,
              "with frame generation the frame rate tile shows what the headset shows, and ACE's own");
    }
    {
        const std::wstring ini = (std::filesystem::temp_directory_path() / L"aceqv_osd_test.ini").wstring();
        DeleteFileW(ini.c_str());
        check(read_osd_placement(ini.c_str()) == OsdPlacement{}, "no [osd]: off, above the centre, 26 degrees");
        const OsdPlacement p{true, -20, -8, 22};
        check(write_osd_placement(ini.c_str(), p) && read_osd_placement(ini.c_str()) == p,
              "[osd] round trip, negative degrees included");
        WritePrivateProfileStringW(L"osd", L"width", L"99", ini.c_str());
        check(read_osd_placement(ini.c_str()).width == kOsdWidthMax, "the width is clamped");
        WritePrivateProfileStringW(L"osd", L"yaw", L"-90", ini.c_str());
        check(read_osd_placement(ini.c_str()).yaw == -kOsdYawMax, "and so is the yaw");
        DeleteFileW(ini.c_str());
    }
    {
        OsdInput in = steady(11.111, 5, 7);
        const OsdSummary s = osd_summary(in);
        std::vector<double> history(120, 11.1);
        history[60] = 30;
        const std::vector<uint32_t> pixels = draw_osd_panel(s, history, osd_tiles(in, s), osd_rows(in, s));
        const auto alpha = [&](int x, int y) { return pixels[static_cast<size_t>(y) * kOsdWidth + x] >> 24; };
        bool text = false;
        for (int y = 30; y < 100 && !text; ++y)
            for (int x = 36; x < 200; ++x)
                text |= (pixels[static_cast<size_t>(y) * kOsdWidth + x] & 0xff) > 0xc0;  // the white frame rate
        check(pixels.size() == static_cast<size_t>(kOsdWidth) * kOsdHeight && alpha(0, 0) == 0 &&
                  alpha(kOsdWidth - 1, kOsdHeight - 1) == 0 && alpha(kOsdWidth / 2, kOsdHeight / 2) > 0xc0 && text,
              "the card: rounded transparent corners, translucent body, the frame rate drawn");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
